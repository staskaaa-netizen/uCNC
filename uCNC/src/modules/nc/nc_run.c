#include "nc_run.h"
#include "../../cnc.h"

#include "../../interface/grbl_stream.h"
#include "../g7x/g7x.h"
#include "nc_g7x.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool g_nc_run_active;
static bool g_nc_run_hold;
static bool g_nc_run_done;
static size_t g_nc_run_line;
static uint8_t g_nc_run_error;
static size_t g_nc_run_error_line;
static size_t g_nc_run_last_sent_line;

/* The program run is paced, not flushed: the pacer hands the machine one block,
   waits until nothing is queued or stepping, and only then hands over the next
   one. The old stream gave the reader the whole program at once, so the
   sender's line ran ahead of the tool by the depth of the controller's
   look-ahead and the pane marked a line the machine had not reached - the bench
   read it as "it is marking next line while running previous one".

   The pause between blocks is deliberate and accepted: an exact mark is worth
   more than continuous velocity on this machine, and no block is ever handed
   over that the machine cannot start.

   A *unit* is what runs as one thing: a whole G7x block (header, contour rows
   and end mark), or a single line outside every block. The pacer's wait is
   between them, and it is the machine that says when one is over: a cycle still
   *collecting* its rows keeps taking them (`g7x_parser_collecting()`, or the
   contour would never be completed), and everything else waits until the planner
   and the interpolator are empty. The unit is also what the pane marks. */
static const nc_document_t *g_nc_run_doc;
static bool g_nc_run_program_active;
static size_t g_nc_run_end_line;
static size_t g_nc_run_unit_first;
static size_t g_nc_run_unit_last;
static size_t g_nc_run_running_line;
static bool g_nc_run_running_valid;

/* A block the panel handed the machine as a step - a one-shot `1 SINGLE` or a
   unit of a run - that has not been taken and run yet. An error while one is in
   flight is the panel's to report ("the line it sent was refused"), which is what
   puts it in RUN's message area; a line the console sent is the console's
   business at every other time. */
static bool g_nc_run_step_in_flight;

/* One-shot blocks the panel sends on its own (a jog, a zero, a touch-off, a
   spindle start). They travel on the same reader as the RUN stream, so they
   cannot share its single line buffer: a jog is two blocks - the incremental
   move and the G90 that puts the machine back - and writing the second one
   over the first would leave the parser with only the G90. */
#define NC_RUN_SEND_SLOTS 4
static char g_nc_run_send_line[NC_RUN_SEND_SLOTS][NC_MAX_LINE_LEN + 2];
static size_t g_nc_run_send_pos[NC_RUN_SEND_SLOTS];
static size_t g_nc_run_send_len[NC_RUN_SEND_SLOTS];
static uint8_t g_nc_run_send_head;
static uint8_t g_nc_run_send_count;

static bool nc_run_failed(void *args);
static bool nc_run_parser_reset(void *args);
static void nc_run_send_pop(void);
static void nc_run_send_retire(void);
CREATE_EVENT_LISTENER(cnc_parse_cmd_error, nc_run_failed);
CREATE_EVENT_LISTENER(parser_reset, nc_run_parser_reset);

/* The pacer runs from the main loop, so a program keeps moving while the panel
   is on another screen: the DRO's state, the console log and the mark all
   belong to the same run wherever the operator is looking. */
static bool nc_run_pace_listener(void *args)
{
    (void)args;
    nc_run_pace();
    return EVENT_CONTINUE;
}
CREATE_EVENT_LISTENER(cnc_dotasks, nc_run_pace_listener);

/* The panel's own blocks - a jog, a zero, a touch-off, the blocks the pacer
   hands over - travel through these two callbacks. The reader belongs to the
   panel only while a block is going out: the moment the last one has been read
   it is handed back to the console, which is also what happens between the
   blocks of a paced program. */
static uint8_t nc_run_send_available(void)
{
    nc_run_send_retire();
    return g_nc_run_send_count > 0u ? 1u : 0u;
}

static uint8_t nc_run_send_getc(void)
{
    uint8_t slot;

    while (g_nc_run_send_count > 0u) {
        slot = g_nc_run_send_head;
        if (g_nc_run_send_pos[slot] < g_nc_run_send_len[slot]) {
            return (uint8_t)g_nc_run_send_line[slot][g_nc_run_send_pos[slot]++];
        }
        nc_run_send_pop();
    }
    return 0u;
}

static void nc_run_send_clear(void)
{
    g_nc_run_send_head = 0u;
    g_nc_run_send_count = 0u;
    memset(g_nc_run_send_pos, 0, sizeof(g_nc_run_send_pos));
    memset(g_nc_run_send_len, 0, sizeof(g_nc_run_send_len));
}

/* Retire the blocks that have been read. The reader retires lazily - the pop
   happens on the `available()`/`getc()` after the last character - and the pacer
   must not wait for that lazy pass: once the parser has taken the whole block
   the machine owns it, and the next one may be handed over. */
static void nc_run_send_retire(void)
{
    while (g_nc_run_send_count > 0u) {
        uint8_t slot = g_nc_run_send_head;

        if (g_nc_run_send_pos[slot] < g_nc_run_send_len[slot]) {
            break;
        }
        nc_run_send_pop();
    }
}

/* Retire the finished block. Once the reader has nothing of ours left it goes
   back to the console, otherwise the panel would leave the serial input
   pointed at a drained buffer after the first jog. A paced program relies on
   the same hand-back: while the machine is running the block it was given, the
   reader is the console's, and the pacer takes it again for the next block. */
static void nc_run_send_pop(void)
{
    if (g_nc_run_send_count == 0u) {
        return;
    }
    g_nc_run_send_head = (uint8_t)((g_nc_run_send_head + 1u) % NC_RUN_SEND_SLOTS);
    g_nc_run_send_count--;
    if (g_nc_run_send_count == 0u) {
        grbl_stream_change(NULL);
    }
}

static bool nc_run_send_push(const char *line)
{
    uint8_t slot;
    int n;

    if (g_nc_run_send_count >= NC_RUN_SEND_SLOTS) {
        return false;
    }
    slot = (uint8_t)((g_nc_run_send_head + g_nc_run_send_count) % NC_RUN_SEND_SLOTS);
    n = snprintf(g_nc_run_send_line[slot], sizeof(g_nc_run_send_line[slot]),
                 "%s\n", line);
    if (n <= 0) {
        return false;
    }
    if (n >= (int)sizeof(g_nc_run_send_line[slot])) {
        n = (int)sizeof(g_nc_run_send_line[slot]) - 1;
        g_nc_run_send_line[slot][n - 1] = '\n';
        g_nc_run_send_line[slot][n] = '\0';
    }
    g_nc_run_send_pos[slot] = 0;
    g_nc_run_send_len[slot] = (size_t)n;
    g_nc_run_send_count++;
    return true;
}

/* The program run is over - it reached its end, or something stopped it. `why`
   is what the console log reports; the machine's own G7x state is asked whether
   a cycle was left half-collected, and that is an error rather than a normal
   end. */
static void nc_run_program_finish(int why)
{
    bool was_running = g_nc_run_program_active;

    nc_run_send_clear();
    g_nc_run_program_active = false;
    g_nc_run_doc = NULL;
    g_nc_run_end_line = (size_t)-1;
    g_nc_run_unit_first = 0u;
    g_nc_run_unit_last = 0u;
    g_nc_run_step_in_flight = false;
    g_nc_run_active = false;
    /* A normal end says how it ended; a stop that has already said why (an
       error, a cancel, a parser reset) does not repeat itself. */
    if (why >= 0) {
        grbl_stream_printf("[MSG:NC STREAM DONE %d]\r\n", why);
    }

    if (!was_running) {
        return;
    }
    /* A cycle left half-*collected* is an incomplete program: the panel handed
       over rows and the run ended before the end mark arrived, so the module is
       still waiting for them. A cycle that is already built and running is not -
       the program was handed over whole, and the machine owns what it has (the
       runner is the machine's own business). Cancelling that one would be the
       panel reaching into a cycle the machine is executing, which is exactly
       the fault this distinction exists to avoid. */
    g_nc_run_done = !g7x_parser_collecting();
    if (!g_nc_run_done) {
        g_nc_run_error = STATUS_INVALID_STATEMENT;
        g_nc_run_error_line = g_nc_run_last_sent_line;
        g7x_parser_cancel();
        grbl_stream_printf("[MSG:NC stopped: incomplete G7x cycle]\r\n");
    }
    grbl_stream_change(NULL);
}

/* Nothing queued and nothing left to step: the machine has finished what it was
   given. This is the pacer's gate, and `g7x_parser_busy()` is read beside it
   because a cycle that is still being collected or emitted has not finished
   even when the planner happens to be empty for a moment - the module is the
   one that knows when its cycle ends. */
static bool nc_run_machine_idle(void)
{
    return !g7x_parser_busy() &&
           planner_buffer_is_empty() &&
           itp_is_empty();
}

static bool nc_run_line_sendable(const char *line)
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
    if (toupper((unsigned char)line[0]) == 'G' &&
        line[1] == '9' && line[2] == '7' &&
        line[3] >= '0' && line[3] <= '3' &&
        (line[4] == '\0' || line[4] == ' ' || line[4] == '\t')) {
        return false;
    }
    return true;
}

void nc_run_init(void)
{
    static bool registered;
    if (!registered) {
        ADD_EVENT_LISTENER(cnc_parse_cmd_error, nc_run_failed);
        ADD_EVENT_LISTENER(parser_reset, nc_run_parser_reset);
        ADD_EVENT_LISTENER(cnc_dotasks, nc_run_pace_listener);
        registered = true;
    }
    nc_run_reset();
}

static bool nc_run_failed(void *args)
{
    if (g_nc_run_program_active || g_nc_run_active || g_nc_run_step_in_flight) {
        g_nc_run_error = *(uint8_t *)args;
        g_nc_run_error_line = g_nc_run_last_sent_line;
        grbl_stream_printf("[MSG:NC stopped on error %u]\r\n", (unsigned)*(uint8_t *)args);
        if (*(uint8_t *)args == STATUS_SYSTEM_GC_LOCK)
            grbl_stream_printf("[MSG:NC lock state=%u alarm=%u; check ?]\r\n",
                               cnc_get_exec_state(EXEC_ALLACTIVE), (unsigned)cnc_has_alarm());
        nc_run_program_finish(-1);
        /* The run stops *on* the line that failed: the sender's position goes
           back to it, so the pane marks what has to be fixed and the next
           `1 SINGLE` acts on that line rather than on the one after it (bench:
           "after error it still goes to next line"). The mark is the sender's
           again at once - there is no unit to keep showing - and the machine's
           queue, if it has one, finishes on its own. */
        g_nc_run_line = g_nc_run_error_line;
        g_nc_run_running_valid = false;
        g_nc_run_step_in_flight = false;
        g_nc_run_active = false;
        g_nc_run_done = false;
        g_nc_run_hold = false;
    }
    return EVENT_CONTINUE;
}

static bool nc_run_parser_reset(void *args)
{
    (void)args;
    if (g_nc_run_program_active)
        nc_run_program_finish(-1);
    nc_run_reset();
    return EVENT_CONTINUE;
}

bool nc_run_arm(const nc_document_t *doc, size_t line)
{
    if (!doc || doc->line_count == 0) {
        return false;
    }
    if (line >= doc->line_count) {
        line = doc->line_count - 1;
    }

    g_nc_run_line = line;
    g_nc_run_error = STATUS_OK;
    g_nc_run_active = true;
    g_nc_run_hold = cnc_get_exec_state(EXEC_HOLD | EXEC_DOOR) != 0;
    g_nc_run_done = false;
    /* A fresh arm owns the mark again: the cursor the operator was walking is
       the line this run starts from. */
    g_nc_run_running_valid = false;
    return true;
}

void nc_run_reset(void)
{
    g_nc_run_error = STATUS_OK;
    g_nc_run_active = false;
    g_nc_run_hold = false;
    g_nc_run_done = false;
    g_nc_run_line = 0;
    g_nc_run_program_active = false;
    g_nc_run_doc = NULL;
    g_nc_run_end_line = (size_t)-1;
    g_nc_run_running_valid = false;
    g_nc_run_step_in_flight = false;
}

void nc_run_stop(void)
{
    if (nc_run_active() || g_nc_run_program_active) {
        cnc_set_exec_state(EXEC_CANCELING);
        g7x_parser_cancel();
    }
    if (g_nc_run_program_active)
        nc_run_program_finish(-1);
    g_nc_run_active = false;
    g_nc_run_hold = false;
    g_nc_run_done = false;
}

bool nc_run_toggle_hold(void)
{
    if (!nc_run_active()) {
        return false;
    }
    g_nc_run_hold = !g_nc_run_hold;
    cnc_call_rt_command(g_nc_run_hold ? CMD_CODE_FEED_HOLD : CMD_CODE_CYCLE_START);
    return true;
}

bool nc_run_active(void)
{
    return g_nc_run_active || g7x_parser_busy() ||
           (g_nc_run_done && (!planner_buffer_is_empty() || !itp_is_empty() ||
                             cnc_get_exec_state(EXEC_RUNNING | EXEC_HOLD)));
}

bool nc_run_hold(void)
{
    return g_nc_run_hold;
}

bool nc_run_done(void)
{
    return g_nc_run_done && !nc_run_active();
}

size_t nc_run_line(void)
{
    return g_nc_run_line;
}

bool nc_run_running(size_t *line)
{
    if (!g_nc_run_running_valid) {
        return false;
    }
    if (line) {
        *line = g_nc_run_running_line;
    }
    return true;
}

size_t nc_run_display_line(void)
{
    return g_nc_run_running_valid ? g_nc_run_running_line : g_nc_run_line;
}

uint8_t nc_run_error(void) { return g_nc_run_error; }
size_t nc_run_error_line(void) { return g_nc_run_error_line; }

void nc_run_set_line(const nc_document_t *doc, size_t line)
{
    if (doc && doc->line_count > 0 && line >= doc->line_count) {
        line = doc->line_count - 1;
    }
    g_nc_run_line = line;
    /* The operator has taken the cursor: the pane follows the key, and the mark
       goes back to the running unit as soon as the pacer hands the next one
       over. */
    g_nc_run_running_valid = false;
}

/* True while a program run is armed. A panel block must not cut into a program:
   the modal state it leaves behind (G90 after a jog) would land in the middle of
   the cycle. It is true between blocks as well - the pacer holds the program
   there, and that is exactly where a stray jog would slip in. */
bool nc_run_streaming(void)
{
    return g_nc_run_program_active;
}

bool nc_run_send_line(const char *line)
{
    if (!nc_run_line_sendable(line)) {
        return false;
    }
    if (!nc_run_send_push(line)) {
        grbl_stream_printf("[MSG:NC SEND FULL %.96s]\r\n", line);
        return false;
    }
    grbl_stream_printf("[MSG:NC SEND %.96s]\r\n", line);
    grbl_stream_readonly(nc_run_send_getc,
                         nc_run_send_available,
                         nc_run_send_clear);
    return true;
}

bool nc_run_send_document_line(const nc_document_t *doc, size_t line)
{
    const char *text;
    size_t start_line;
    size_t end_line;

    if (!doc || line >= doc->line_count) {
        return false;
    }
    text = doc->lines[line].text;
    if (!nc_run_line_sendable(text)) {
        return false;
    }

    /* A G7x block is sent as a whole: the header (or both header lines) plus its
       numbered range or G80 terminator. That holds *wherever* the cursor sits in
       the block: in RUN the pane marks the line the run is on, which while a
       cycle runs is one of its contour rows, and a contour row sent on its own
       would execute as plain motion outside the cycle it belongs to. A line
       outside every block is its own step. */
    if (nc_g7x_block_containing(doc, line, &start_line, &end_line)) {
        if (!nc_run_start_stream(doc, start_line)) {
            return false;
        }
        g_nc_run_end_line = end_line;
        /* The whole block is what goes out, but the mark stays on the line the
           operator stepped from: it is the line in play - the one the keys and
           the digits are on - and jumping it up to the block's header is what
           the bench saw as "it still marks next row with g71, not the one
           starting with N50". It is inside the unit, so the pacer leaves it
           alone while the block's rows go out, and the pane draws the block
           pale around it. */
        g_nc_run_running_line = line;
        g_nc_run_running_valid = true;
        return true;
    }

    if (!nc_run_send_line(text)) {
        return false;
    }
    g_nc_run_error = STATUS_OK;
    g_nc_run_last_sent_line = line;
    /* A one-shot step is not a program run: the run state is the machine's own -
       `nc_run_active()`'s "there is motion left" - so the DRO and the strip stop
       saying "running" the moment the step is done, instead of staying lit until
       the next reset. */
    g_nc_run_active = false;
    g_nc_run_line = line + 1u;
    g_nc_run_done = true;
    /* A one-shot step is a unit of its own: its mark is this line until the
       machine has run it. */
    g_nc_run_running_line = line;
    g_nc_run_running_valid = true;
    g_nc_run_step_in_flight = true;
    return true;
}

bool nc_run_start_stream(const nc_document_t *doc, size_t line)
{
    size_t unit_first = line;
    size_t unit_last = line;

    if (!nc_run_arm(doc, line)) {
        return false;
    }

    g_nc_run_doc = doc;
    g_nc_run_program_active = true;
    g_nc_run_end_line = (size_t)-1;
    (void)nc_g7x_block_containing(doc, line, &unit_first, &unit_last);
    g_nc_run_unit_first = unit_first;
    g_nc_run_unit_last = unit_last;
    /* The run starts on the line it was armed with and the mark is that line:
       the unit is what the machine has to be given whole, but the operator's
       line is where they are, and it is the line `1 SINGLE` acts on. */
    g_nc_run_running_line = line;
    g_nc_run_running_valid = true;
    grbl_stream_printf("[MSG:NC STREAM START %lu]\r\n", (unsigned long)(line + 1u));
    return true;
}

/* The pacer, called from the main loop: hand the machine one block and wait
   until it has run. Nothing here blocks - it returns and is called again - so
   the machine, the keypad and the panel keep running while a block is in
   flight, and the operator can still read the screen and hold or stop the run.

   The order of the gates matters: a block already on the wire is finished
   first; a run that has nothing left to hand over ends (which is how a document
   that stops inside a cycle is closed and reported); a cycle still being
   collected keeps taking its rows; and anything else waits for the machine to be
   idle, so the sender never gets more than one running block ahead of the tool. */
void nc_run_pace(void)
{
    char emit[NC_MAX_LINE_LEN];
    size_t emitted_line = 0;
    nc_run_step_result_t result;

    /* Retire what the reader has already taken, and notice a step that is over:
       once it is off the wire and nothing is moving, a later error belongs to
       whoever sends next. This runs on every pass, so it holds for a one-shot
       step too, where no program is armed. */
    nc_run_send_retire();
    if (!g_nc_run_program_active && g_nc_run_send_count == 0u &&
        nc_run_machine_idle()) {
        g_nc_run_step_in_flight = false;
        /* Nothing more to do here. The mark is *not* moved on when the unit
           finishes: it stays on the line in play until the operator takes the
           cursor - with a line key, a new step or a reload. Moving it on by
           itself put it on whatever came next, and in a program whose
           next line is a cycle header that never closes that meant the pane
           marked the *next* `G71` with nothing behind it (bench: "now it runs
           but it marks also next g71. which it should not mark"), and the line
           the operator had just cut was no longer marked at all. */
    }

    if (!g_nc_run_program_active || !g_nc_run_doc) {
        return;
    }
    if (g_nc_run_hold) {
        return;
    }
    /* A controller in an alarm has stopped for a reason: it must not be fed the
       rest of the program. The run is still armed - the DRO says ALARM and the
       strip says why - and the operator's own reset (`# RELOAD`) is what ends
       it. */
    if (cnc_has_alarm()) {
        return;
    }
    if (g_nc_run_send_count > 0u) {
        return;                       /* the last block is still going out */
    }

    /* Nothing left to hand over: the program ends here. This is checked before
       the wait, because a document that ends inside a cycle has to *end* the run
       - the module is asked whether a cycle was left half-collected - and a
       machine still cutting the last block is the tail `nc_run_active()` already
       accounts for. */
    if (g_nc_run_line > g_nc_run_end_line ||
        g_nc_run_line >= g_nc_run_doc->line_count) {
        nc_run_program_finish((int)NC_RUN_STEP_COMPLETE);
        return;
    }

    /* The next line to hand over. A cycle's rows have to keep coming - while the
       machine is collecting a contour, the line that follows still belongs to it
       - and everything else waits for the machine to finish what it was given.
       The module answers the first half (`g7x_parser_collecting()`), the planner
       and the interpolator answer the second. */
    if (!g7x_parser_collecting() && !nc_run_machine_idle()) {
        return;
    }

    result = nc_run_step_sendable(g_nc_run_doc, emit, sizeof(emit), &emitted_line);
    if (result == NC_RUN_STEP_HOLD) {
        return;
    }
    if (result != NC_RUN_STEP_EMITTED || !nc_run_line_sendable(emit)) {
        nc_run_program_finish((int)result);
        return;
    }

    /* The mark follows the unit that is starting: either this line opens a new
       one, or the operator has taken the cursor and the pane goes back to the
       machine's answer at the next block. */
    if (!g_nc_run_running_valid ||
        !(emitted_line >= g_nc_run_unit_first && emitted_line <= g_nc_run_unit_last)) {
        size_t first = emitted_line;
        size_t last = emitted_line;

        (void)nc_g7x_block_containing(g_nc_run_doc, emitted_line, &first, &last);
        g_nc_run_unit_first = first;
        g_nc_run_unit_last = last;
        g_nc_run_running_line = first;
        g_nc_run_running_valid = true;
    }

    if (!nc_run_send_line(emit)) {
        nc_run_program_finish(-1);
        return;
    }
    g_nc_run_step_in_flight = true;
    g_nc_run_last_sent_line = emitted_line;
}

nc_run_step_result_t nc_run_step(const nc_document_t *doc,
                                 char *out,
                                 size_t out_sz,
                                 size_t *emitted_line)
{
    const char *line;

    if (!out || out_sz == 0 || !doc || doc->line_count == 0) {
        return NC_RUN_STEP_NEEDS_PROGRAM;
    }
    out[0] = '\0';
    if (g_nc_run_hold) {
        return NC_RUN_STEP_HOLD;
    }
    if (!g_nc_run_active || g_nc_run_done) {
        (void)nc_run_arm(doc, doc->cursor_line);
    }

    if (g_nc_run_line >= doc->line_count) {
        g_nc_run_done = true;
        g_nc_run_active = false;
        return NC_RUN_STEP_COMPLETE;
    }

    if (emitted_line) {
        *emitted_line = g_nc_run_line;
    }
    line = doc->lines[g_nc_run_line].text;
    g_nc_run_line++;
    if (!nc_run_line_sendable(line)) {
        return NC_RUN_STEP_SKIPPED;
    }

    snprintf(out, out_sz, "%s", line);
    return NC_RUN_STEP_EMITTED;
}

nc_run_step_result_t nc_run_step_sendable(const nc_document_t *doc,
                                          char *out,
                                          size_t out_sz,
                                          size_t *emitted_line)
{
    nc_run_step_result_t result;
    uint8_t guard = 0;

    do {
        result = nc_run_step(doc, out, out_sz, emitted_line);
        if (result == NC_RUN_STEP_EMITTED && !nc_run_line_sendable(out)) {
            grbl_stream_printf("[MSG:NC SKIP SEND %.96s]\r\n", out ? out : "");
        }
    } while ((result == NC_RUN_STEP_SKIPPED ||
              (result == NC_RUN_STEP_EMITTED && !nc_run_line_sendable(out))) &&
             ++guard < 32u);

    return result;
}
