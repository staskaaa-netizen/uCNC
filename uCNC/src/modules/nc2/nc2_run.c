#include "nc2_run.h"

#include "nc2_emit.h"
#include "nc2_tools.h"

#include "../../cnc.h"
#include "../../interface/grbl_stream.h"

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
static bool g_nc2_run_active;
static bool g_nc2_run_hold;
static bool g_nc2_run_done;
static size_t g_nc2_run_line;
static uint8_t g_nc2_run_error;
static size_t g_nc2_run_error_line;
static size_t g_nc2_run_last_sent_line;

static const nc2_document_t *g_nc2_run_doc;
static bool g_nc2_run_program_active;
static size_t g_nc2_run_end_line;
static size_t g_nc2_run_running_line;
static bool g_nc2_run_running_valid;
static bool g_nc2_run_step_in_flight;
/* The unit the mark is on: a whole G7x block, or a single line outside every
   block. The mark is the unit's *first* line and stays there while the unit's
   lines go out, so a cycle is marked as the block it is and not one row at a
   time as its expansion walks (`nc`'s own rule: "the unit is also what the pane
   marks"). */
static size_t g_nc2_run_unit_first;
static size_t g_nc2_run_unit_last;
/* The tool the machine last saw: the last `T` the pacer handed over. It stays
   across runs (a tool is in the spindle until another one is), and it is what a
   screen asks when it wants the machine's tool rather than the editor's place
   in the text. */
static int g_nc2_run_tool = -1;
static nc2_emit_stream_t g_nc2_run_stream;

/* The console log's own code for a normal end (`nc_run.c`'s STEP_COMPLETE). */
#define NC2_RUN_WHY_COMPLETE 4

/* One-shot blocks the panel sends on its own (a jog, a zero, a spindle start).
   They travel on the same reader as the run, so they cannot share its single
   line buffer: a jog is two blocks - the move and the G90 that puts the machine
   back - and writing the second over the first would leave the parser with only
   the G90. */
#define NC2_RUN_SEND_SLOTS 4
static char g_nc2_run_send_line[NC2_RUN_SEND_SLOTS][NC2_MAX_LINE_LEN + 2];
static size_t g_nc2_run_send_pos[NC2_RUN_SEND_SLOTS];
static size_t g_nc2_run_send_len[NC2_RUN_SEND_SLOTS];
static uint8_t g_nc2_run_send_head;
static uint8_t g_nc2_run_send_count;

static bool nc2_run_failed(void *args);
static bool nc2_run_parser_reset(void *args);
static void nc2_run_send_pop(void);
static void nc2_run_send_retire(void);
CREATE_EVENT_LISTENER(cnc_parse_cmd_error, nc2_run_failed);
CREATE_EVENT_LISTENER(parser_reset, nc2_run_parser_reset);

static bool nc2_run_pace_listener(void *args)
{
    (void)args;
    nc2_run_pace();
    return EVENT_CONTINUE;
}
CREATE_EVENT_LISTENER(cnc_dotasks, nc2_run_pace_listener);

/* The panel's own blocks travel through these two callbacks. The reader belongs
   to the panel only while a block is going out: the moment the last one has
   been read it goes back to the console, which is also what happens between the
   units of a paced program. */
static uint8_t nc2_run_send_available(void)
{
    nc2_run_send_retire();
    return g_nc2_run_send_count > 0u ? 1u : 0u;
}

static uint8_t nc2_run_send_getc(void)
{
    uint8_t slot;

    while (g_nc2_run_send_count > 0u) {
        slot = g_nc2_run_send_head;
        if (g_nc2_run_send_pos[slot] < g_nc2_run_send_len[slot]) {
            return (uint8_t)g_nc2_run_send_line[slot][g_nc2_run_send_pos[slot]++];
        }
        nc2_run_send_pop();
    }
    return 0u;
}

static void nc2_run_send_clear(void)
{
    g_nc2_run_send_head = 0u;
    g_nc2_run_send_count = 0u;
    memset(g_nc2_run_send_pos, 0, sizeof(g_nc2_run_send_pos));
    memset(g_nc2_run_send_len, 0, sizeof(g_nc2_run_send_len));
}

/* Retire the blocks the reader has taken. The reader retires lazily, and the
   pacer must not wait for that pass: once the parser has the whole block the
   machine owns it, and the next line may be handed over. */
static void nc2_run_send_retire(void)
{
    while (g_nc2_run_send_count > 0u) {
        uint8_t slot = g_nc2_run_send_head;

        if (g_nc2_run_send_pos[slot] < g_nc2_run_send_len[slot]) {
            break;
        }
        nc2_run_send_pop();
    }
}

/* Hand the reader back to the console once nothing of ours is left, otherwise
   the panel would leave the serial input pointed at a drained buffer. */
static void nc2_run_send_pop(void)
{
    if (g_nc2_run_send_count == 0u) {
        return;
    }
    g_nc2_run_send_head = (uint8_t)((g_nc2_run_send_head + 1u) % NC2_RUN_SEND_SLOTS);
    g_nc2_run_send_count--;
    if (g_nc2_run_send_count == 0u) {
        grbl_stream_change(0);
    }
}

static bool nc2_run_send_push(const char *line)
{
    uint8_t slot;
    int n;

    if (g_nc2_run_send_count >= NC2_RUN_SEND_SLOTS) {
        return false;
    }
    slot = (uint8_t)((g_nc2_run_send_head + g_nc2_run_send_count) % NC2_RUN_SEND_SLOTS);
    n = snprintf(g_nc2_run_send_line[slot], sizeof(g_nc2_run_send_line[slot]),
                 "%s\n", line);
    if (n <= 0) {
        return false;
    }
    if (n >= (int)sizeof(g_nc2_run_send_line[slot])) {
        n = (int)sizeof(g_nc2_run_send_line[slot]) - 1;
        g_nc2_run_send_line[slot][n - 1] = '\n';
        g_nc2_run_send_line[slot][n] = '\0';
    }
    g_nc2_run_send_pos[slot] = 0;
    g_nc2_run_send_len[slot] = (size_t)n;
    g_nc2_run_send_count++;
    return true;
}

/* The run is over - it reached its end, or something stopped it. `why` is what
   the console log reports; a stop that has already said why does not repeat
   itself. */
static void nc2_run_program_finish(int why)
{
    nc2_run_send_clear();
    g_nc2_run_program_active = false;
    g_nc2_run_doc = 0;
    g_nc2_run_end_line = (size_t)-1;
    g_nc2_run_step_in_flight = false;
    g_nc2_run_active = false;
    /* A normal end is what makes the DRO stop saying "run" once the queue
       drains; a stop that has already spoken for itself is not one. */
    g_nc2_run_done = why >= 0;
    if (why >= 0) {
        grbl_stream_printf("[MSG:NC2 STREAM DONE %d]\r\n", why);
    }
    grbl_stream_change(0);
}

/* Nothing queued and nothing left to run: the machine has finished what it was
   given. This is the pacer's gate. */
static bool nc2_run_machine_idle(void)
{
    return planner_buffer_is_empty() && itp_is_empty();
}

/* True while the expansion of the block in play still has lines to give: a
   contour is one cut, so its lines go out back to back rather than one stop per
   row. A new block - or a plain line - waits for the machine instead. */
static bool nc2_run_mid_block(void)
{
    return g_nc2_run_stream.g7x.active || g_nc2_run_stream.g7x_collecting;
}

static bool nc2_run_line_sendable(const char *line)
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

void nc2_run_init(void)
{
    static bool registered;

    if (!registered) {
        ADD_EVENT_LISTENER(cnc_parse_cmd_error, nc2_run_failed);
        ADD_EVENT_LISTENER(parser_reset, nc2_run_parser_reset);
        ADD_EVENT_LISTENER(cnc_dotasks, nc2_run_pace_listener);
        registered = true;
    }
    nc2_run_reset();
}

static bool nc2_run_failed(void *args)
{
    if (g_nc2_run_program_active || g_nc2_run_active || g_nc2_run_step_in_flight) {
        g_nc2_run_error = *(uint8_t *)args;
        g_nc2_run_error_line = g_nc2_run_last_sent_line;
        grbl_stream_printf("[MSG:NC2 stopped on error %u]\r\n",
                           (unsigned)*(uint8_t *)args);
        nc2_run_program_finish(-1);
        /* The run stops *on* the line that failed, so the pane marks what has to
           be fixed and the next `1 SINGLE` acts on that line. */
        g_nc2_run_line = g_nc2_run_error_line;
        g_nc2_run_running_valid = false;
        g_nc2_run_step_in_flight = false;
        g_nc2_run_active = false;
        g_nc2_run_done = false;
        g_nc2_run_hold = false;
    }
    return EVENT_CONTINUE;
}

static bool nc2_run_parser_reset(void *args)
{
    (void)args;
    if (g_nc2_run_program_active) {
        nc2_run_program_finish(-1);
    }
    nc2_run_reset();
    return EVENT_CONTINUE;
}

static void nc2_run_unit(const nc2_document_t *doc, size_t line,
                         size_t *first, size_t *last);

/* Arm the pacer: the machine is given the program from `line` to `end_line`. */
static bool nc2_run_arm(const nc2_document_t *doc, size_t line, size_t end_line)
{
    if (!doc || doc->line_count == 0u) {
        return false;
    }
    if (end_line >= doc->line_count) {
        end_line = doc->line_count - 1u;
    }
    if (line >= doc->line_count) {
        line = doc->line_count - 1u;
    }
    nc2_emit_stream_begin(&g_nc2_run_stream, doc, line);
    if (!g_nc2_run_stream.active) {
        return false;
    }
    g_nc2_run_doc = doc;
    g_nc2_run_program_active = true;
    g_nc2_run_end_line = end_line;
    g_nc2_run_line = line;
    g_nc2_run_error = STATUS_OK;
    g_nc2_run_active = true;
    g_nc2_run_hold = cnc_get_exec_state(EXEC_HOLD | EXEC_DOOR) != 0;
    g_nc2_run_done = false;
    g_nc2_run_running_valid = false;
    g_nc2_run_step_in_flight = false;
    /* The unit the armed line is in: the mark holds this block until a line
       outside it starts the next one. */
    nc2_run_unit(doc, line, &g_nc2_run_unit_first, &g_nc2_run_unit_last);
    return true;
}

bool nc2_run_start(const nc2_document_t *doc, size_t line)
{
    if (!nc2_run_arm(doc, line, doc ? doc->line_count - 1u : 0u)) {
        return false;
    }
    g_nc2_run_running_line = line;
    g_nc2_run_running_valid = true;
    grbl_stream_printf("[MSG:NC2 STREAM START %lu]\r\n", (unsigned long)(line + 1u));
    return true;
}

/* The unit that owns `line`: a G7x block (its whole path), or the line alone. */
static void nc2_run_unit(const nc2_document_t *doc, size_t line,
                         size_t *first, size_t *last)
{
    g7x_doc_t view = nc2_document_g7x(doc);

    if (!g7x_doc_line_path(&view, line, first, last)) {
        *first = line;
        *last = line;
    }
}

bool nc2_run_send_unit(const nc2_document_t *doc, size_t line)
{
    size_t first = line;
    size_t last = line;

    if (!doc || line >= doc->line_count) {
        return false;
    }
    nc2_run_unit(doc, line, &first, &last);
    if (!nc2_run_arm(doc, first, last)) {
        return false;
    }
    /* The whole block goes out, but the mark stays on the line the operator
       stepped from: it is the line in play, and the pane draws the block pale
       around it. */
    g_nc2_run_running_line = line;
    g_nc2_run_running_valid = true;
    g_nc2_run_step_in_flight = true;
    return true;
}

void nc2_run_reset(void)
{
    g_nc2_run_error = STATUS_OK;
    g_nc2_run_active = false;
    g_nc2_run_hold = false;
    g_nc2_run_done = false;
    g_nc2_run_line = 0u;
    g_nc2_run_program_active = false;
    g_nc2_run_doc = 0;
    g_nc2_run_end_line = (size_t)-1;
    g_nc2_run_running_valid = false;
    g_nc2_run_step_in_flight = false;
    g_nc2_run_unit_first = 0u;
    g_nc2_run_unit_last = 0u;
}

void nc2_run_stop(void)
{
    if (nc2_run_active() || g_nc2_run_program_active) {
        cnc_set_exec_state(EXEC_CANCELING);
    }
    if (g_nc2_run_program_active) {
        nc2_run_program_finish(-1);
    }
    g_nc2_run_active = false;
    g_nc2_run_hold = false;
    g_nc2_run_done = false;
}

bool nc2_run_toggle_hold(void)
{
    if (!nc2_run_active()) {
        return false;
    }
    g_nc2_run_hold = !g_nc2_run_hold;
    cnc_call_rt_command(g_nc2_run_hold ? CMD_CODE_FEED_HOLD : CMD_CODE_CYCLE_START);
    return true;
}

bool nc2_run_active(void)
{
    return g_nc2_run_active ||
           (g_nc2_run_done && (!planner_buffer_is_empty() || !itp_is_empty() ||
                               cnc_get_exec_state(EXEC_RUNNING | EXEC_HOLD)));
}

bool nc2_run_hold(void)
{
    return g_nc2_run_hold;
}

bool nc2_run_done(void)
{
    return g_nc2_run_done && !nc2_run_active();
}

size_t nc2_run_line(void)
{
    return g_nc2_run_line;
}

bool nc2_run_running(size_t *line)
{
    if (!g_nc2_run_running_valid) {
        return false;
    }
    if (line) {
        *line = g_nc2_run_running_line;
    }
    return true;
}

size_t nc2_run_display_line(void)
{
    return g_nc2_run_running_valid ? g_nc2_run_running_line : g_nc2_run_line;
}

uint8_t nc2_run_error(void) { return g_nc2_run_error; }
size_t nc2_run_error_line(void) { return g_nc2_run_error_line; }

void nc2_run_set_line(const nc2_document_t *doc, size_t line)
{
    if (doc && doc->line_count > 0u && line >= doc->line_count) {
        line = doc->line_count - 1u;
    }
    g_nc2_run_line = line;
    /* The operator has taken the cursor: the pane follows the key, and the mark
       goes back to the running unit as soon as the pacer hands the next one
       over. */
    g_nc2_run_running_valid = false;
}

bool nc2_run_streaming(void)
{
    return g_nc2_run_program_active;
}

/* The tool the machine last saw. A screen that wants "the tool in the spindle"
   asks this before it reads any file: the machine's answer, not the editor's
   place in the text. */
int nc2_run_tool(void)
{
    return g_nc2_run_tool;
}

bool nc2_run_expanding(void)
{
    return g_nc2_run_stream.g7x.active || g_nc2_run_stream.g7x_collecting;
}

bool nc2_run_send_line(const char *line)
{
    if (!nc2_run_line_sendable(line)) {
        return false;
    }
    if (!nc2_run_send_push(line)) {
        grbl_stream_printf("[MSG:NC2 SEND FULL %.96s]\r\n", line);
        return false;
    }
    grbl_stream_printf("[MSG:NC2 SEND %.96s]\r\n", line);
    grbl_stream_readonly(nc2_run_send_getc, nc2_run_send_available,
                         nc2_run_send_clear);
    return true;
}

/* The pacer, from the main loop: hand the machine one line and wait for it. The
   order of the gates matters: a line already on the wire is finished first; a
   run that has nothing left ends; a block still being expanded keeps feeding;
   and anything else waits for the machine to be idle, so the sender never gets
   more than one unit ahead of the tool. */
void nc2_run_pace(void)
{
    char emit[NC2_MAX_LINE_LEN];
    size_t emitted_line = 0u;
    nc2_emit_result_t result;

    nc2_run_send_retire();
    if (!g_nc2_run_program_active && g_nc2_run_send_count == 0u &&
        nc2_run_machine_idle()) {
        g_nc2_run_step_in_flight = false;
    }

    if (!g_nc2_run_program_active || !g_nc2_run_doc) {
        return;
    }
    if (g_nc2_run_hold) {
        return;
    }
    /* A controller in an alarm has stopped for a reason: it must not be fed the
       rest of the program. `# RELOAD` is what ends the run. */
    if (cnc_has_alarm()) {
        return;
    }
    if (g_nc2_run_send_count > 0u) {
        return;                       /* the last line is still going out */
    }
    if (!nc2_run_mid_block() && !nc2_run_machine_idle()) {
        return;                       /* the machine is still running the last */
    }

    for (;;) {
        if (!g_nc2_run_stream.active) {
            /* End of the program, or the generator giving up on it: the two are
               not the same thing, and the bench saw the second dressed as the
               first - a run whose range never closed still said `STREAM DONE`
               and the operator walked the rest of the file by hand. */
            if (nc2_emit_stream_failed(&g_nc2_run_stream)) {
                grbl_stream_printf("[MSG:NC2 STREAM STOPPED %s]\r\n",
                                   g7x_result_text(g_nc2_run_stream.error));
            }
            nc2_run_program_finish(nc2_emit_stream_failed(&g_nc2_run_stream)
                                       ? -1
                                       : NC2_RUN_WHY_COMPLETE);
            return;
        }
        if (g_nc2_run_stream.source_line > g_nc2_run_end_line) {
            nc2_run_program_finish(NC2_RUN_WHY_COMPLETE);
            return;
        }
        result = nc2_emit_stream_next(&g_nc2_run_stream, emit, sizeof(emit),
                                      &emitted_line);
        if (result == NC2_EMIT_ERROR) {
            grbl_stream_printf("[MSG:NC2 STREAM STOPPED %s]\r\n",
                               g7x_result_text(g_nc2_run_stream.error));
            nc2_run_program_finish(-1);
            return;
        }
        if (result == NC2_EMIT_LINE) {
            /* The generator's own notes - `(G71 rough X23.000)` and the like -
               are a line of the stream but not a line of the program: the
               controller is never given them. */
            if (nc2_run_line_sendable(emit)) {
                break;
            }
            grbl_stream_printf("[MSG:NC2 SKIP SEND %.96s]\r\n", emit);
        }
        /* Nothing sendable in what was given, and nothing left after it: the
           program is done. The last line of a file is returned *with* the
           stream already finished, so this is checked after the answer, not
           before it. */
        if (!g_nc2_run_stream.active) {
            nc2_run_program_finish(NC2_RUN_WHY_COMPLETE);
            return;
        }
    }

    /* The mark follows the *unit* that is starting, and only that: a line that
       belongs to the block already in play leaves the mark where it is, so a
       cycle is marked as the block it is while its expansion walks through it.
       A line outside the block opens the next one (or the operator has taken
       the cursor: `nc_run_set_line` clears the flag and the pane goes back to
       the machine's answer at the next block). `nc`'s own rule. */
    if (!g_nc2_run_running_valid ||
        !(emitted_line >= g_nc2_run_unit_first &&
          emitted_line <= g_nc2_run_unit_last)) {
        size_t first = emitted_line;
        size_t last = emitted_line;

        nc2_run_unit(g_nc2_run_doc, emitted_line, &first, &last);
        /* A unit the run has already passed does not take the mark back: a `G70`
           re-cuts the profile above it (the range's own rows), and the pane has
           to stay on the block the run is on (bench: "it still wants to jump to
           1 line"). */
        if (!g_nc2_run_running_valid || first >= g_nc2_run_unit_first) {
            g_nc2_run_unit_first = first;
            g_nc2_run_unit_last = last;
            g_nc2_run_running_line = first;
            g_nc2_run_running_valid = true;
        }
    }
    {
        int tool;

        if (nc2_tools_line_tool_number(emit, &tool)) {
            g_nc2_run_tool = tool;      /* what the machine is cutting with */
        }
    }
    if (!nc2_run_send_line(emit)) {
        nc2_run_program_finish(-1);
        return;
    }
    g_nc2_run_step_in_flight = true;
    g_nc2_run_last_sent_line = emitted_line;
    g_nc2_run_line = emitted_line + 1u;
}
