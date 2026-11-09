#include "nc_sender.h"

#include "../g7x/g7x_blocks.h"

#include "../../cnc.h"
#include "../../interface/grbl_stream.h"
#include "../../core/interpolator.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* The program run is paced, not flushed: the pacer hands the machine one unit
   and waits until it has run before handing over the next. The old stream gave
   the reader the whole program at once, so the sender's line ran ahead of the
   tool by the depth of the controller's look-ahead.

   A *unit* is what runs as one thing: the motion one source line stands for. A
   G7x cycle expands into a path, and a path is one unit - its lines follow each
   other without a stop between them, because they are one cut - while a plain
   line is a unit of its own. The machine says when a unit is over: the planner
   and the interpolator are empty. */
static bool g_nc_sender_run_active;
static bool g_nc_sender_run_hold;
static size_t g_nc_sender_run_line;
static uint8_t g_nc_sender_run_error;
static size_t g_nc_sender_run_error_line;

static const g7x_doc_t *g_nc_sender_run_doc;
static bool g_nc_sender_run_program_active;
static size_t g_nc_sender_run_end_line;
static size_t g_nc_sender_run_running_line;
static bool g_nc_sender_run_running_valid;
static bool g_nc_sender_run_step_in_flight;
/* The unit the mark is on: a whole G7x block, or a single line outside every
   block. The mark is the unit's *first* line and stays there while the unit's
   lines go out, so a cycle is marked as the block it is and not one row at a
   time as its expansion walks (`nc`'s own rule: "the unit is also what the pane
   marks"). */
static size_t g_nc_sender_run_unit_first;
static size_t g_nc_sender_run_unit_last;
static nc_sender_emit_stream_t g_nc_sender_run_stream;
static nc_sender_status_fn g_nc_sender_run_status;
static void *g_nc_sender_run_status_user;
static nc_sender_tool_radius_fn g_nc_sender_run_tool_radius;
static void *g_nc_sender_run_tool_radius_user;

/* The console log's own code for a normal end (`nc_run.c`'s STEP_COMPLETE). */
#define NC_SENDER_RUN_WHY_COMPLETE 4

/* One-shot blocks the panel sends on its own (a jog, a zero, a spindle start).
   They travel on the same reader as the run, so they cannot share its single
   line buffer: a jog is two blocks - the move and the G90 that puts the machine
   back - and writing the second over the first would leave the parser with only
   the G90. */
#define NC_SENDER_RUN_SEND_SLOTS 4
/* A sent line is the program's line with the sender's own `N` in front of it
   (`N12 G1 X30.000 F500.000`), so the block needs room for the number as well
   as the text. */
#define NC_SENDER_RUN_NUMBER_LEN 12
static char g_nc_sender_run_send_line[NC_SENDER_RUN_SEND_SLOTS][NC_SENDER_MAX_LINE_LEN + NC_SENDER_RUN_NUMBER_LEN];
static size_t g_nc_sender_run_send_pos[NC_SENDER_RUN_SEND_SLOTS];
static size_t g_nc_sender_run_send_len[NC_SENDER_RUN_SEND_SLOTS];
static uint8_t g_nc_sender_run_send_head;
static uint8_t g_nc_sender_run_send_count;

/* Which program line each number the sender handed over came from. The
   controller keeps the `N` of the line it is *executing* (`Ln:` in its status,
   `itp_get_rt_line_number()`) - one number per line it was given, which is a
   different thing from the line of the program, because a cycle is one line of
   the program and many lines of the stream. This is the map between them, and
   it is a ring because the machine may be a few lines behind the sender inside
   a block. */
#define NC_SENDER_RUN_SENT_MAX 128
static struct {
    uint32_t seq;
    size_t line;
} g_nc_sender_run_sent[NC_SENDER_RUN_SENT_MAX];
static unsigned g_nc_sender_run_sent_next;
static uint32_t g_nc_sender_run_seq;
/* The block the machine was reading when it refused. A refusal names the line
   of the *program* it belongs to; this is the text the panel actually handed
   over, so the log can say which block it was - the bench's "error 2: Invalid
   number" on a cycle that expands to nothing but valid motion is exactly the
   question this answers. */
static char g_nc_sender_run_refused[NC_SENDER_MAX_LINE_LEN + 2];
static bool nc_sender_run_failed(void *args);
static bool nc_sender_run_parser_reset(void *args);
static void nc_sender_run_send_pop(void);
static void nc_sender_run_send_retire(void);
/* The machine's own state words: see "the tool and the compensation" below. */
static void nc_sender_run_state_forget(void);
static void nc_sender_run_state_refresh(void);
CREATE_EVENT_LISTENER(cnc_parse_cmd_error, nc_sender_run_failed);
CREATE_EVENT_LISTENER(parser_reset, nc_sender_run_parser_reset);

static bool nc_sender_run_pace_listener(void *args)
{
    (void)args;
    nc_sender_run_pace();
    return EVENT_CONTINUE;
}
CREATE_EVENT_LISTENER(cnc_dotasks, nc_sender_run_pace_listener);

/* The panel's own blocks travel through these two callbacks. The reader belongs
   to the panel only while a block is going out: the moment the last one has
   been read it goes back to the console, which is also what happens between the
   units of a paced program. */
static uint8_t nc_sender_run_send_available(void)
{
    nc_sender_run_send_retire();
    return g_nc_sender_run_send_count > 0u ? 1u : 0u;
}

static uint8_t nc_sender_run_send_getc(void)
{
    uint8_t slot;

    while (g_nc_sender_run_send_count > 0u) {
        slot = g_nc_sender_run_send_head;
        if (g_nc_sender_run_send_pos[slot] < g_nc_sender_run_send_len[slot]) {
            return (uint8_t)g_nc_sender_run_send_line[slot][g_nc_sender_run_send_pos[slot]++];
        }
        nc_sender_run_send_pop();
    }
    return 0u;
}

static void nc_sender_run_send_clear(void)
{
    g_nc_sender_run_send_head = 0u;
    g_nc_sender_run_send_count = 0u;
    memset(g_nc_sender_run_send_pos, 0, sizeof(g_nc_sender_run_send_pos));
    memset(g_nc_sender_run_send_len, 0, sizeof(g_nc_sender_run_send_len));
    memset(g_nc_sender_run_sent, 0, sizeof(g_nc_sender_run_sent));
    g_nc_sender_run_sent_next = 0u;
    g_nc_sender_run_seq = 0u;
}

/* Retire the blocks the reader has taken. The reader retires lazily, and the
   pacer must not wait for that pass: once the parser has the whole block the
   machine owns it, and the next line may be handed over. */
static void nc_sender_run_send_retire(void)
{
    while (g_nc_sender_run_send_count > 0u) {
        uint8_t slot = g_nc_sender_run_send_head;

        if (g_nc_sender_run_send_pos[slot] < g_nc_sender_run_send_len[slot]) {
            break;
        }
        nc_sender_run_send_pop();
    }
}

/* Hand the reader back to the console once nothing of ours is left, otherwise
   the panel would leave the serial input pointed at a drained buffer. */
static void nc_sender_run_send_pop(void)
{
    if (g_nc_sender_run_send_count == 0u) {
        return;
    }
    g_nc_sender_run_send_head = (uint8_t)((g_nc_sender_run_send_head + 1u) % NC_SENDER_RUN_SEND_SLOTS);
    g_nc_sender_run_send_count--;
    if (g_nc_sender_run_send_count == 0u) {
        grbl_stream_change(0);
    }
}

/* The block the parser is on when it refuses. A line is read a character at a
   time, so the refusal can land while the head of the queue is being read, or
   once that one has been drained and the line was already in the parser - in
   which case the head is the *next* block and the refused one is the slot
   behind it. */
static void nc_sender_run_send_note_refused(void)
{
    uint8_t slot = g_nc_sender_run_send_head;
    const char *text;
    size_t len;

    if (g_nc_sender_run_send_count == 0u || g_nc_sender_run_send_pos[slot] == 0u) {
        slot = (uint8_t)((slot + NC_SENDER_RUN_SEND_SLOTS - 1u) % NC_SENDER_RUN_SEND_SLOTS);
    }
    text = g_nc_sender_run_send_line[slot];
    len = strlen(text);
    while (len > 0u && (text[len - 1u] == '\n' || text[len - 1u] == '\r')) {
        len--;
    }
    /* The number in front is the sender's own (it is what the machine answers
       with); the operator reads the block the panel was given. */
    if (text[0] == 'N') {
        size_t after = 1u;

        while (text[after] >= '0' && text[after] <= '9') {
            after++;
        }
        if (after > 1u && text[after] == ' ') {
            text += after + 1u;
            len -= after + 1u;
        }
    }
    if (len >= sizeof(g_nc_sender_run_refused)) {
        len = sizeof(g_nc_sender_run_refused) - 1u;
    }
    memcpy(g_nc_sender_run_refused, text, len);
    g_nc_sender_run_refused[len] = '\0';
}

static bool nc_sender_run_send_push(const char *line)
{
    uint8_t slot;
    int n;

    if (g_nc_sender_run_send_count >= NC_SENDER_RUN_SEND_SLOTS) {
        return false;
    }
    slot = (uint8_t)((g_nc_sender_run_send_head + g_nc_sender_run_send_count) % NC_SENDER_RUN_SEND_SLOTS);
    n = snprintf(g_nc_sender_run_send_line[slot], sizeof(g_nc_sender_run_send_line[slot]),
                 "%s\n", line);
    if (n <= 0) {
        return false;
    }
    if (n >= (int)sizeof(g_nc_sender_run_send_line[slot])) {
        n = (int)sizeof(g_nc_sender_run_send_line[slot]) - 1;
        g_nc_sender_run_send_line[slot][n - 1] = '\n';
        g_nc_sender_run_send_line[slot][n] = '\0';
    }
    g_nc_sender_run_send_pos[slot] = 0;
    g_nc_sender_run_send_len[slot] = (size_t)n;
    g_nc_sender_run_send_count++;
    return true;
}

/* The run is over - it reached its end, or something stopped it. `why` is what
   the console log reports; a stop that has already said why does not repeat
   itself. */
/* Why the run stopped, in the operator's words: the line the generator gave up
   on, the module's own reason - which names the check and the value to look at
   - and, only if the module had none, the bare result name. A stop that says
   "unsupported" and nothing else is not a diagnosis; the strip and the console
   say the same sentence here. */
static void nc_sender_run_say_stopped(void)
{
    const char *why = g7x_refusal_text();
    char text[NC_SENDER_STATUS_MAX];

    if (why[0]) {
        snprintf(text, sizeof(text), "Line %lu: %.44s",
                 (unsigned long)(g_nc_sender_run_stream.block_line + 1u), why);
    } else {
        snprintf(text, sizeof(text), "Line %lu: %s",
                 (unsigned long)(g_nc_sender_run_stream.block_line + 1u),
                 g7x_result_text(g_nc_sender_run_stream.error));
    }
    grbl_stream_printf("[MSG:NCC STREAM STOPPED %s at line %lu: %s]\r\n",
                       g7x_result_text(g_nc_sender_run_stream.error),
                       (unsigned long)(g_nc_sender_run_stream.block_line + 1u),
                       why[0] ? why : "no reason given");
    if (g_nc_sender_run_status) {
        g_nc_sender_run_status(g_nc_sender_run_status_user, text);
    }
}

static void nc_sender_run_program_finish(int why)
{
    /* The line the run stopped on is the state the parked screen wears: the
       document goes below, and the state machine keeps this answer until the
       next run re-reads a program. */
    nc_sender_run_state_refresh();
    nc_sender_run_send_clear();
    g_nc_sender_run_program_active = false;
    g_nc_sender_run_doc = 0;
    g_nc_sender_run_end_line = (size_t)-1;
    g_nc_sender_run_step_in_flight = false;
    g_nc_sender_run_active = false;
    /* A normal end is what makes the DRO stop saying "run" once the queue
       drains; a stop that has already spoken for itself is not one. */
    if (why >= 0) {
        grbl_stream_printf("[MSG:NCC STREAM DONE %d]\r\n", why);
    }
    grbl_stream_change(0);
}

/* Nothing queued and nothing left to run: the machine has finished what it was
   given. This is the pacer's gate. */
static bool nc_sender_run_machine_idle(void)
{
    return planner_buffer_is_empty() && itp_is_empty();
}

/* True while the expansion of the block in play still has lines to give: a
   contour is one cut, so its lines go out back to back rather than one stop per
   row. A new block - or a plain line - waits for the machine instead. */
static bool nc_sender_run_mid_block(void)
{
    return g_nc_sender_run_stream.g7x.active || g_nc_sender_run_stream.g7x_collecting;
}

static bool nc_sender_run_line_sendable(const char *line)
{
    if (!line) {
        return false;
    }
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (!*line || *line == '(') {
        return false;
    }
    /* The setup rows a G7x program carries are the panel's, not the controller's
       (`nc_run.c` asks the same question). Everything else the panel hands over
       goes: a jog is a `$J=` block and a zero is a `G10`, and neither is a line
       of a program. */
    if (toupper((unsigned char)line[0]) == 'G' &&
        line[1] == '9' && line[2] == '7' &&
        line[3] >= '0' && line[3] <= '3' &&
        (line[4] == '\0' || line[4] == ' ' || line[4] == '\t')) {
        return false;
    }
    return true;
}

void nc_sender_run_init(nc_sender_status_fn status, void *status_user,
                        nc_sender_tool_radius_fn tool_radius,
                        void *tool_radius_user)
{
    static bool registered;

    g_nc_sender_run_status = status;
    g_nc_sender_run_status_user = status_user;
    g_nc_sender_run_tool_radius = tool_radius;
    g_nc_sender_run_tool_radius_user = tool_radius_user;
    if (!registered) {
        ADD_EVENT_LISTENER(cnc_parse_cmd_error, nc_sender_run_failed);
        ADD_EVENT_LISTENER(parser_reset, nc_sender_run_parser_reset);
        ADD_EVENT_LISTENER(cnc_dotasks, nc_sender_run_pace_listener);
        registered = true;
    }
    nc_sender_run_reset();
}

static bool nc_sender_run_failed(void *args)
{
    if (g_nc_sender_run_program_active || g_nc_sender_run_active || g_nc_sender_run_step_in_flight) {
        g_nc_sender_run_error = *(uint8_t *)args;
        nc_sender_run_send_note_refused();
        /* The block, not the last line handed over: a cycle is expanded over
           many lines and the controller answers for the move, so the line to
           fix is the block the move came out of (`nc`'s own rule - a cycle
           stays marked as the block it is). The line the sender last handed
           over cannot answer it: the generator's own motions - the clearance,
           the retracts, the changes of pass - are not rows of the program. */
        g_nc_sender_run_error_line = g_nc_sender_run_stream.block_line;
        grbl_stream_printf("[MSG:NCC stopped on error %u]\r\n",
                           (unsigned)*(uint8_t *)args);
        nc_sender_run_program_finish(-1);
        /* The run stops *on* the line that failed, so the pane marks what has to
           be fixed and the next `1 SINGLE` acts on that line. */
        g_nc_sender_run_line = g_nc_sender_run_error_line;
        g_nc_sender_run_running_valid = false;
        g_nc_sender_run_step_in_flight = false;
        g_nc_sender_run_active = false;
        g_nc_sender_run_hold = false;
    }
    return EVENT_CONTINUE;
}

static bool nc_sender_run_parser_reset(void *args)
{
    /* The controller answers a refusal with a parser reset, and that reset must
       not carry the pane back to the top of the program: the fault and the line
       it stopped on stay put, so the operator keeps looking at the line that has
       to be fixed (bench: "on error - it still jumps to first line. no i should
       stay here"). */
    uint8_t error = g_nc_sender_run_error;
    size_t error_line = g_nc_sender_run_error_line;

    (void)args;
    if (g_nc_sender_run_program_active) {
        nc_sender_run_program_finish(-1);
    }
    nc_sender_run_reset();
    if (error != STATUS_OK) {
        g_nc_sender_run_error = error;
        g_nc_sender_run_error_line = error_line;
        g_nc_sender_run_line = error_line;
    }
    return EVENT_CONTINUE;
}

static void nc_sender_run_unit(const g7x_doc_t *doc, size_t line,
                         size_t *first, size_t *last);

/* Arm the pacer: the machine is given the program from `line` to `end_line`. */
static bool nc_sender_run_arm(const g7x_doc_t *doc, size_t line, size_t end_line)
{
    if (!doc || doc->count == 0u) {
        return false;
    }
    if (end_line >= doc->count) {
        end_line = doc->count - 1u;
    }
    if (line >= doc->count) {
        line = doc->count - 1u;
    }
    nc_sender_emit_stream_begin(&g_nc_sender_run_stream, doc, line,
                                g_nc_sender_run_tool_radius,
                                g_nc_sender_run_tool_radius_user);
    if (!g_nc_sender_run_stream.active) {
        return false;
    }
    g_nc_sender_run_doc = doc;
    g_nc_sender_run_program_active = true;
    g_nc_sender_run_end_line = end_line;
    g_nc_sender_run_line = line;
    g_nc_sender_run_error = STATUS_OK;
    g_nc_sender_run_active = true;
    g_nc_sender_run_hold = cnc_get_exec_state(EXEC_HOLD | EXEC_DOOR) != 0;
    g_nc_sender_run_running_valid = false;
    g_nc_sender_run_step_in_flight = false;
    /* The unit the armed line is in: the mark holds this block until a line
       outside it starts the next one. */
    nc_sender_run_unit(doc, line, &g_nc_sender_run_unit_first, &g_nc_sender_run_unit_last);
    return true;
}

bool nc_sender_run_start(const g7x_doc_t *doc, size_t line)
{
    /* A **new run** starts from the program's own rows: the state words are
       re-read for it, so a program that never asks for `G41`/`G42` is a `G40`
       program and a run started *inside* a compensated portion arms with that
       portion's own words (`nc_sender_emit_stream_begin()`'s prime pass) - neither
       the last run's offset nor a global that was just cleared (bench,
       2026-10-04: *"g40 run is still red alert"*, and then *"now g42 still
       appears while g41 portion was not finished"*). */
    nc_sender_run_state_forget();
    if (!nc_sender_run_arm(doc, line, doc ? doc->count - 1u : 0u)) {
        return false;
    }
    g_nc_sender_run_running_line = line;
    g_nc_sender_run_running_valid = true;
    grbl_stream_printf("[MSG:NCC STREAM START %lu]\r\n", (unsigned long)(line + 1u));
    return true;
}

/* The unit that owns `line`: the block it sits in, or the line alone. `nc`'s
   own answer (`g7x_doc_block_containing`, not `g7x_doc_line_path`) - and the
   difference is the whole of a mark bug. `line_path` also answers for a `G70`
   with the range it re-cuts, and that range is *above* the finish cut: the pane
   took the mark back onto a row of a block the run had already finished, and a
   `1 SINGLE` on the finish cut would have sent those rows instead of the cut
   (bench, 2026-09-27: "on errors - again 1 line is selected and this one will
   not run"). */
static void nc_sender_run_unit(const g7x_doc_t *doc, size_t line,
                               size_t *first, size_t *last)
{
    *first = line;
    *last = line;
    (void)g7x_doc_block_containing(doc, line, first, last);
}

bool nc_sender_run_send_unit(const g7x_doc_t *doc, size_t line)
{
    size_t first = line;
    size_t last = line;

    if (!doc || line >= doc->count) {
        return false;
    }
    nc_sender_run_unit(doc, line, &first, &last);
    if (!nc_sender_run_arm(doc, first, last)) {
        return false;
    }
    /* The whole block goes out, but the mark stays on the line the operator
       stepped from: it is the line in play, and the pane draws the block pale
       around it. */
    g_nc_sender_run_running_line = line;
    g_nc_sender_run_running_valid = true;
    g_nc_sender_run_step_in_flight = true;
    return true;
}

void nc_sender_run_reset(void)
{
    g_nc_sender_run_error = STATUS_OK;
    g_nc_sender_run_active = false;
    g_nc_sender_run_hold = false;
    g_nc_sender_run_line = 0u;
    g_nc_sender_run_program_active = false;
    g_nc_sender_run_doc = 0;
    g_nc_sender_run_end_line = (size_t)-1;
    g_nc_sender_run_running_valid = false;
    g_nc_sender_run_step_in_flight = false;
    g_nc_sender_run_unit_first = 0u;
    g_nc_sender_run_unit_last = 0u;
    /* The state words go with the run: the next one re-reads the program it is
       about to run. */
    nc_sender_run_state_forget();
    /* The refusal this run may have left behind goes with the run: a fresh
       reset has nothing refused, and the sentence is built from this before the
       controller's own parser reset arrives. */
    g_nc_sender_run_refused[0] = '\0';
}

void nc_sender_run_stop(void)
{
    if (nc_sender_run_active() || g_nc_sender_run_program_active) {
        cnc_set_exec_state(EXEC_CANCELING);
    }
    if (g_nc_sender_run_program_active) {
        nc_sender_run_program_finish(-1);
    }
    g_nc_sender_run_active = false;
    g_nc_sender_run_hold = false;
}

bool nc_sender_run_toggle_hold(void)
{
    if (!nc_sender_run_active()) {
        return false;
    }
    g_nc_sender_run_hold = !g_nc_sender_run_hold;
    cnc_call_rt_command(g_nc_sender_run_hold ? CMD_CODE_FEED_HOLD : CMD_CODE_CYCLE_START);
    return true;
}

bool nc_sender_run_active(void)
{
    /* A finished run is still a run while the *machine* is executing what it
       was given - and the machine says so itself, in its own status word
       (`cnc_get_status()`: the same answer its `<...>` report carries). Bits
       the panel picked for itself are not it, and neither are the machine's
       queue depths: asked either way the strip kept saying RUN with the machine
       reporting `<Idle>` and nothing moving (bench, 2026-09-28). */
    return g_nc_sender_run_active || cnc_get_status() == EXEC_STATUS_RUNNING;
}

bool nc_sender_run_hold(void)
{
    return g_nc_sender_run_hold;
}

size_t nc_sender_run_line(void)
{
    return g_nc_sender_run_line;
}

size_t nc_sender_run_display_line(void)
{
    size_t line = 0u;

    /* The machine's own answer first: the line it is *executing* is the line
       the tool is on. What the sender has handed over is a step ahead of that
       inside a block, which is how the mark came to sit on the next line while
       the tool was still on this one. */
    if (nc_sender_run_executing_line(&line)) {
        return line;
    }
    return g_nc_sender_run_running_valid ? g_nc_sender_run_running_line : g_nc_sender_run_line;
}

uint8_t nc_sender_run_error(void) { return g_nc_sender_run_error; }
size_t nc_sender_run_error_line(void) { return g_nc_sender_run_error_line; }
const char *nc_sender_run_refused_text(void) { return g_nc_sender_run_refused; }

void nc_sender_run_set_line(const g7x_doc_t *doc, size_t line)
{
    if (doc && doc->count > 0u && line >= doc->count) {
        line = doc->count - 1u;
    }
    g_nc_sender_run_line = line;
    /* The operator has taken the cursor: the pane follows the key, and the mark
       goes back to the running unit as soon as the pacer hands the next one
       over. */
    g_nc_sender_run_running_valid = false;
}

bool nc_sender_run_streaming(void)
{
    return g_nc_sender_run_program_active;
}

/* --- the machine's own state words: the tool and the compensation ---------

   The tool and the compensation a screen wears - the strip's `T`/`G40`-`G42`,
   the tip the drawing rides the cut with, the brush's own shape - are **G7x's
   state machine** (`g7x_state_at()`), asked at the line the **machine** is
   executing (`nc_sender_run_executing_line()`): the last `Tn` and the last
   `G40`/`G41`/`G42` at or above it, and nothing else advances it.

   `ncc` asks; it does not read the words itself. The sender walks ahead of the
   machine by design (it must, to expand a cycle), so an answer taken from the
   sender's own walk showed `G42` while the `G41` part of the program was still
   being cut, and a run *started inside* a compensated portion - `2 FROM` on a
   later cycle - armed from a global that had just been cleared and cut the block
   with `G40` (bench, 2026-10-04: *"now g42 still appears while g41 portion was
   not finished. so make it very state machine in g7x for tool and for offset"*).

   What is cached here is only the answer for the line it was asked about: the
   frame loop asks twice a frame (the strip and the drawing) and G7x's scan reads
   the rows again. A run that is over keeps the last answer it had - the parked
   picture stays the picture of the part. */
static struct {
    const g7x_doc_t *doc;
    size_t line;
    int tool;
    g7x_comp_side_t comp;
    bool valid;
} g_nc_sender_run_state;

static void nc_sender_run_state_at(const g7x_doc_t *doc, size_t line)
{
    g7x_state_t state;

    if (g_nc_sender_run_state.valid && g_nc_sender_run_state.doc == doc &&
        g_nc_sender_run_state.line == line) {
        return;
    }
    state = g7x_state_at(doc, line);
    g_nc_sender_run_state.doc = doc;
    g_nc_sender_run_state.line = line;
    g_nc_sender_run_state.tool = state.tool;
    g_nc_sender_run_state.comp = state.comp;
    g_nc_sender_run_state.valid = true;
}

/* Bring the state to the line the machine is on. A run that is over has no
   document to read any more (`nc_sender_run_program_finish()` drops it), and the
   state it stopped with is the one the parked screen wears. */
static void nc_sender_run_state_refresh(void)
{
    if (!g_nc_sender_run_doc) {
        return;
    }
    nc_sender_run_state_at(g_nc_sender_run_doc, nc_sender_run_display_line());
}

static void nc_sender_run_state_forget(void)
{
    memset(&g_nc_sender_run_state, 0, sizeof(g_nc_sender_run_state));
    /* No tool has been called yet - the machine's own "nothing in the spindle"
       answer, which a screen reads as "ask the program instead" (`ncc`'s `T-1`,
       `nc`'s own). */
    g_nc_sender_run_state.tool = -1;
}

/* The tool the machine is cutting with - a screen that wants "the tool in the
   spindle" asks this, not the editor's place in the text. */
int nc_sender_run_tool(void)
{
    nc_sender_run_state_refresh();
    return g_nc_sender_run_state.tool;
}

bool nc_sender_run_expanding(void)
{
    return g_nc_sender_run_stream.g7x.active || g_nc_sender_run_stream.g7x_collecting;
}

g7x_comp_side_t nc_sender_run_comp_mode(void)
{
    nc_sender_run_state_refresh();
    return g_nc_sender_run_state.comp;
}

bool nc_sender_run_send_line(const char *line)
{
    if (!nc_sender_run_line_sendable(line)) {
        return false;
    }
    if (!nc_sender_run_send_push(line)) {
        grbl_stream_printf("[MSG:NCC SEND FULL %.96s]\r\n", line);
        return false;
    }
    grbl_stream_printf("[MSG:NCC SEND %.96s]\r\n", line);
    grbl_stream_readonly(nc_sender_run_send_getc, nc_sender_run_send_available,
                         nc_sender_run_send_clear);
    return true;
}

/* Hand a *program* line over with the sender's own number in front of it, and
   remember the line of the program that number came from. The controller keeps
   the `N` of the line it is executing and answers with it (`Ln:` in its status,
   `itp_get_rt_line_number()`), so this pair is the whole of the map between what
   the machine is cutting and the line of the program the operator reads - a
   cycle being one line of the program and many blocks of the stream. */
static bool nc_sender_run_send_numbered(const char *line, size_t program_line)
{
    char numbered[NC_SENDER_MAX_LINE_LEN + NC_SENDER_RUN_NUMBER_LEN];
    int n = snprintf(numbered, sizeof(numbered), "N%lu %s",
                     (unsigned long)(g_nc_sender_run_seq + 1u), line);

    if (n <= 0 || n >= (int)sizeof(numbered)) {
        return false;
    }
    if (!nc_sender_run_send_line(numbered)) {
        return false;
    }
    g_nc_sender_run_seq++;
    g_nc_sender_run_sent[g_nc_sender_run_sent_next].seq = g_nc_sender_run_seq;
    g_nc_sender_run_sent[g_nc_sender_run_sent_next].line = program_line;
    g_nc_sender_run_sent_next = (g_nc_sender_run_sent_next + 1u) % NC_SENDER_RUN_SENT_MAX;
    return true;
}

/* The program line behind a number the sender handed over, or false when that
   number is not ours (a one-shot the panel sent, or a run before this one). */
static bool nc_sender_run_sent_line(uint32_t seq, size_t *line)
{
    unsigned i;

    if (seq == 0u) {
        return false;
    }
    for (i = 0u; i < NC_SENDER_RUN_SENT_MAX; i++) {
        if (g_nc_sender_run_sent[i].seq == seq) {
            if (line) {
                *line = g_nc_sender_run_sent[i].line;
            }
            return true;
        }
    }
    return false;
}

bool nc_sender_run_executing_line(size_t *line)
{
    size_t at = 0u;
    size_t first = 0u;
    size_t last = 0u;

    /* Nothing of the panel's is in the machine's hands: the mark is the
       panel's own again (a fault, a step, a run that has not started). */
    if (!g_nc_sender_run_program_active && !g_nc_sender_run_step_in_flight) {
        return false;
    }
    if (!nc_sender_run_sent_line(itp_get_rt_line_number(), &at)) {
        return false;
    }
    /* The line in play is the unit's own first line: a cycle is one line of the
       program, and the bench asked for it to be marked as the block it is
       rather than one row at a time. */
    first = at;
    last = at;
    nc_sender_run_unit(g_nc_sender_run_doc, at, &first, &last);
    if (line) {
        *line = first;
    }
    return true;
}

/* The pacer, from the main loop: hand the machine one line and wait for it. The
   order of the gates matters: a line already on the wire is finished first; a
   run that has nothing left ends; a block still being expanded keeps feeding;
   and anything else waits for the machine to be idle, so the sender never gets
   more than one unit ahead of the tool. */
void nc_sender_run_pace(void)
{
    char emit[NC_SENDER_MAX_LINE_LEN];
    size_t emitted_line = 0u;
    nc_sender_emit_result_t result;

    nc_sender_run_send_retire();
    if (!g_nc_sender_run_program_active && g_nc_sender_run_send_count == 0u &&
        nc_sender_run_machine_idle()) {
        g_nc_sender_run_step_in_flight = false;
    }

    if (!g_nc_sender_run_program_active || !g_nc_sender_run_doc) {
        return;
    }
    if (g_nc_sender_run_hold) {
        return;
    }
    /* A controller in an alarm has stopped for a reason: it must not be fed the
       rest of the program. `# RELOAD` is what ends the run. */
    if (cnc_has_alarm()) {
        return;
    }
    if (g_nc_sender_run_send_count > 0u) {
        return;                       /* the last line is still going out */
    }
    if (!nc_sender_run_mid_block() && !nc_sender_run_machine_idle()) {
        return;                       /* the machine is still running the last */
    }

    for (;;) {
        if (!g_nc_sender_run_stream.active) {
            /* End of the program, or the generator giving up on it: the two are
               not the same thing, and the bench saw the second dressed as the
               first - a run whose range never closed still said `STREAM DONE`
               and the operator walked the rest of the file by hand. */
            if (nc_sender_emit_stream_failed(&g_nc_sender_run_stream)) {
                /* The same as below: the pane keeps the line the generator gave
                   up on. */
                g_nc_sender_run_error_line = g_nc_sender_run_stream.block_line;
                g_nc_sender_run_line = g_nc_sender_run_error_line;
                g_nc_sender_run_running_valid = false;
                nc_sender_run_say_stopped();
            }
            nc_sender_run_program_finish(nc_sender_emit_stream_failed(&g_nc_sender_run_stream)
                                       ? -1
                                       : NC_SENDER_RUN_WHY_COMPLETE);
            return;
        }
        if (g_nc_sender_run_stream.source_line > g_nc_sender_run_end_line) {
            nc_sender_run_program_finish(NC_SENDER_RUN_WHY_COMPLETE);
            return;
        }
        result = nc_sender_emit_stream_next(&g_nc_sender_run_stream, emit, sizeof(emit),
                                      &emitted_line);
        if (result == NC_SENDER_EMIT_ERROR) {
            /* The *panel* refused this line - its generator, not the
               controller. The pane has to keep marking the line the generator
               stopped on, or `1 SINGLE` retries the line after it and the one
               that failed never runs (`g_nc_sender_run_line` is the line the pacer
               would have sent *next*, so it is one past the trouble): bench,
               2026-09-27 - "on errors - again 1 line is selected and this one
               will not run". `block_line` is the line the stream refused: the
               cycle's header, which is what the operator has to fix. */
            g_nc_sender_run_error_line = g_nc_sender_run_stream.block_line;
            g_nc_sender_run_line = g_nc_sender_run_error_line;
            g_nc_sender_run_running_valid = false;
            nc_sender_run_say_stopped();
            nc_sender_run_program_finish(-1);
            return;
        }
        if (result == NC_SENDER_EMIT_LINE) {
            /* The generator's own notes - `(G71 rough X23.000)` and the like -
               are a line of the stream but not a line of the program: the
               controller is never given them. */
            if (nc_sender_run_line_sendable(emit)) {
                break;
            }
            grbl_stream_printf("[MSG:NCC SKIP SEND %.96s]\r\n", emit);
        }
        /* Nothing sendable in what was given, and nothing left after it: the
           program is done. The last line of a file is returned *with* the
           stream already finished, so this is checked after the answer, not
           before it. */
        if (!g_nc_sender_run_stream.active) {
            nc_sender_run_program_finish(NC_SENDER_RUN_WHY_COMPLETE);
            return;
        }
    }

    /* The mark follows the *unit* that is starting, and only that: a line that
       belongs to the block already in play leaves the mark where it is, so a
       cycle is marked as the block it is while its expansion walks through it.
       A line outside the block opens the next one (or the operator has taken
       the cursor: `nc_run_set_line` clears the flag and the pane goes back to
       the machine's answer at the next block). `nc`'s own rule. */
    if (!g_nc_sender_run_running_valid ||
        !(emitted_line >= g_nc_sender_run_unit_first &&
          emitted_line <= g_nc_sender_run_unit_last)) {
        size_t first = emitted_line;
        size_t last = emitted_line;

        nc_sender_run_unit(g_nc_sender_run_doc, emitted_line, &first, &last);
        /* A unit the run has already passed does not take the mark back: a `G70`
           re-feeds the profile above it row by row, and those rows belong to
           the block the run has finished. The mark stays on the finish cut's own
           line (bench: "it still wants to jump to 1 line").

           But the line the sender has just reached is where the operator is,
           and it takes the mark even when the block it sits in reaches back
           over them: a program's rows after a cycle are its own, and the last
           one used to be left unmarked because the block's own first line was
           behind the mark - the run ended on `M5` with the mark still on the row
           before it (bench: "it seems not to mark last m5 line"). `nc`'s own
           words: the unit is what the machine has to be given whole, but the
           operator's line is where they are. */
        if (!g_nc_sender_run_running_valid || first >= g_nc_sender_run_running_line) {
            g_nc_sender_run_unit_first = first;
            g_nc_sender_run_unit_last = last;
            g_nc_sender_run_running_line = first;
            g_nc_sender_run_running_valid = true;
        }
    }
    /* The block, not the single output: a cycle's own motions and the rows it
       re-cuts all come from the cycle's line (`block_line`), so the number the
       machine answers with maps back to the line of the program that is being
       cut - one line per cycle, which is what the pane marks. */
    if (!nc_sender_run_send_numbered(emit, nc_sender_emit_stream_block_line(&g_nc_sender_run_stream))) {
        nc_sender_run_program_finish(-1);
        return;
    }
    g_nc_sender_run_step_in_flight = true;
    /* The sender's line only ever goes forward. A block's own motions answer
       for the block (see `block_line`), so the raw answer walks back to it
       after a row of the block has gone out - and the pane reads this line as
       "handed over up to", which is a line the run has passed. */
    if (emitted_line + 1u > g_nc_sender_run_line) {
        g_nc_sender_run_line = emitted_line + 1u;
    }
}
