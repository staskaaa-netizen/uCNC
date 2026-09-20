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
static char g_nc_run_stream_line[NC_MAX_LINE_LEN + 2];
static size_t g_nc_run_stream_pos;
static size_t g_nc_run_stream_len;
static const nc_document_t *g_nc_run_stream_doc;
static bool g_nc_run_stream_active;
static bool g_nc_run_single_pending;
static size_t g_nc_run_stream_end_line;

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

static bool nc_run_stream_load_line(void);
static void nc_run_stream_clear(void);
static void nc_run_send_pop(void);
static bool nc_run_failed(void *args);
static bool nc_run_parser_reset(void *args);
CREATE_EVENT_LISTENER(cnc_parse_cmd_error, nc_run_failed);
CREATE_EVENT_LISTENER(parser_reset, nc_run_parser_reset);

static uint8_t nc_run_stream_available(void)
{
    /* Panel blocks first: they are one block each and are already complete. */
    while (g_nc_run_send_count > 0u) {
        uint8_t slot = g_nc_run_send_head;

        if (g_nc_run_send_pos[slot] < g_nc_run_send_len[slot]) {
            return 1u;
        }
        nc_run_send_pop();
    }
    if (g_nc_run_stream_pos < g_nc_run_stream_len) {
        return 1u;
    }
    if (g_nc_run_single_pending) {
        nc_run_stream_clear();
        return 0u;
    }
    return nc_run_stream_load_line() ? 1u : 0u;
}

static uint8_t nc_run_stream_getc(void)
{
    while (g_nc_run_send_count > 0u) {
        uint8_t slot = g_nc_run_send_head;

        if (g_nc_run_send_pos[slot] < g_nc_run_send_len[slot]) {
            return (uint8_t)g_nc_run_send_line[slot][g_nc_run_send_pos[slot]++];
        }
        nc_run_send_pop();
    }
    if (g_nc_run_stream_pos >= g_nc_run_stream_len &&
        !nc_run_stream_load_line()) {
        return 0;
    }
    return (uint8_t)g_nc_run_stream_line[g_nc_run_stream_pos++];
}

static void nc_run_send_clear(void)
{
    g_nc_run_send_head = 0u;
    g_nc_run_send_count = 0u;
    memset(g_nc_run_send_pos, 0, sizeof(g_nc_run_send_pos));
    memset(g_nc_run_send_len, 0, sizeof(g_nc_run_send_len));
}

/* Retire the finished block. Once the reader has nothing of ours left it goes
   back to the console, otherwise the panel would leave the serial input
   pointed at a drained buffer after the first jog. */
static void nc_run_send_pop(void)
{
    if (g_nc_run_send_count == 0u) {
        return;
    }
    g_nc_run_send_head = (uint8_t)((g_nc_run_send_head + 1u) % NC_RUN_SEND_SLOTS);
    g_nc_run_send_count--;
    if (g_nc_run_send_count == 0u && !g_nc_run_stream_active &&
        !g_nc_run_single_pending) {
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

static void nc_run_stream_clear(void)
{
    bool was_active = g_nc_run_stream_active || g_nc_run_single_pending;
    g_nc_run_single_pending = false;

    nc_run_send_clear();
    g_nc_run_stream_pos = 0;
    g_nc_run_stream_len = 0;
    g_nc_run_stream_line[0] = '\0';
    g_nc_run_stream_doc = NULL;
    g_nc_run_stream_active = false;
    g_nc_run_stream_end_line = (size_t)-1;
    if (was_active) {
        g_nc_run_active = false;
        g_nc_run_done = !g7x_parser_busy();
        if (!g_nc_run_done) {
            g_nc_run_error = STATUS_INVALID_STATEMENT;
            g_nc_run_error_line = g_nc_run_last_sent_line;
            g7x_parser_cancel();
            grbl_stream_printf("[MSG:NC stopped: incomplete G7x cycle]\r\n");
        }
    }
    grbl_stream_change(NULL);
}

static bool nc_run_line_starts_gcode(const char *line, unsigned code)
{
    char *end;
    unsigned long value;

    if (!line) {
        return false;
    }
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (toupper((unsigned char)*line++) != 'G') {
        return false;
    }
    value = strtoul(line, &end, 10);
    if (end == line || value != code) {
        return false;
    }
    return *end == '\0' || *end == ' ' || *end == '\t';
}

static bool nc_run_line_is_g7x_header(const char *line)
{
    return nc_run_line_starts_gcode(line, 71u) ||
           nc_run_line_starts_gcode(line, 72u);
}

static bool nc_run_find_g7x_end(const nc_document_t *doc, size_t line, size_t *end_line)
{
    /* Shared with the preview: a numbered range ends at N(Q), otherwise at the
       G80 line. */
    return nc_g7x_block_end(doc, line, end_line);
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
        registered = true;
    }
    nc_run_reset();
}

static bool nc_run_failed(void *args)
{
    if (g_nc_run_stream_active || g_nc_run_active) {
        g_nc_run_error = *(uint8_t *)args;
        g_nc_run_error_line = g_nc_run_last_sent_line;
        grbl_stream_printf("[MSG:NC stopped on error %u]\r\n", (unsigned)*(uint8_t *)args);
        if (*(uint8_t *)args == STATUS_SYSTEM_GC_LOCK)
            grbl_stream_printf("[MSG:NC lock state=%u alarm=%u; check ?]\r\n",
                               cnc_get_exec_state(EXEC_ALLACTIVE), (unsigned)cnc_has_alarm());
        nc_run_stream_clear();
        g_nc_run_active = false;
        g_nc_run_done = false;
        g_nc_run_hold = false;
    }
    return EVENT_CONTINUE;
}

static bool nc_run_parser_reset(void *args)
{
    (void)args;
    if (g_nc_run_stream_active || g_nc_run_single_pending)
        nc_run_stream_clear();
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
    return true;
}

void nc_run_reset(void)
{
    g_nc_run_error = STATUS_OK;
    g_nc_run_active = false;
    g_nc_run_hold = false;
    g_nc_run_done = false;
    g_nc_run_line = 0;
}

void nc_run_stop(void)
{
    if (nc_run_active() || g_nc_run_stream_active) {
        cnc_set_exec_state(EXEC_CANCELING);
        g7x_parser_cancel();
    }
    if (g_nc_run_stream_active || g_nc_run_single_pending)
        nc_run_stream_clear();
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

uint8_t nc_run_error(void) { return g_nc_run_error; }
size_t nc_run_error_line(void) { return g_nc_run_error_line; }

void nc_run_set_line(const nc_document_t *doc, size_t line)
{
    if (doc && doc->line_count > 0 && line >= doc->line_count) {
        line = doc->line_count - 1;
    }
    g_nc_run_line = line;
}

/* True while the RUN reader is holding program blocks. A panel block must not
   cut into a program: the modal state it leaves behind (G90 after a jog) would
   land in the middle of the cycle. */
bool nc_run_streaming(void)
{
    return g_nc_run_stream_active;
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
    grbl_stream_readonly(nc_run_stream_getc,
                         nc_run_stream_available,
                         nc_run_stream_clear);
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

    /* A G7x block is sent as a whole: the header (or both header lines) plus
       its numbered range or G80 terminator. */
    start_line = nc_g7x_block_start(doc, line);
    if (nc_run_line_is_g7x_header(doc->lines[start_line].text) &&
        nc_run_find_g7x_end(doc, start_line, &end_line)) {
        if (!nc_run_start_stream(doc, start_line)) {
            return false;
        }
        g_nc_run_stream_end_line = end_line;
        return true;
    }

    if (!nc_run_send_line(text)) {
        return false;
    }
    g_nc_run_single_pending = true;
    g_nc_run_error = STATUS_OK;
    g_nc_run_last_sent_line = line;
    g_nc_run_active = true;
    g_nc_run_line = line + 1u;
    g_nc_run_done = true;
    return true;
}

static bool nc_run_stream_load_line(void)
{
    char emit[NC_MAX_LINE_LEN];
    size_t emitted_line = 0;
    nc_run_step_result_t result;
    int n;

    if (!g_nc_run_stream_active || !g_nc_run_stream_doc) {
        return false;
    }
    if (g_nc_run_hold)
        return false;
    if (g_nc_run_line > g_nc_run_stream_end_line) {
        grbl_stream_printf("[MSG:NC STREAM DONE %d]\r\n", (int)NC_RUN_STEP_COMPLETE);
        nc_run_stream_clear();
        return false;
    }

    result = nc_run_step_sendable(g_nc_run_stream_doc,
                                  emit,
                                  sizeof(emit),
                                  &emitted_line);
    if (result != NC_RUN_STEP_EMITTED || !nc_run_line_sendable(emit)) {
        grbl_stream_printf("[MSG:NC STREAM DONE %d]\r\n", (int)result);
        nc_run_stream_clear();
        return false;
    }

    n = snprintf(g_nc_run_stream_line, sizeof(g_nc_run_stream_line), "%s\n", emit);
    if (n <= 0) {
        nc_run_stream_clear();
        return false;
    }
    if (n >= (int)sizeof(g_nc_run_stream_line)) {
        n = (int)sizeof(g_nc_run_stream_line) - 1;
        g_nc_run_stream_line[n - 1] = '\n';
        g_nc_run_stream_line[n] = '\0';
    }
    g_nc_run_stream_pos = 0;
    g_nc_run_stream_len = (size_t)n;
    grbl_stream_printf("[MSG:NC SEND %.96s]\r\n", emit);
    g_nc_run_last_sent_line = emitted_line;
    return true;
}

bool nc_run_start_stream(const nc_document_t *doc, size_t line)
{
    if (!nc_run_arm(doc, line)) {
        return false;
    }

    g_nc_run_stream_doc = doc;
    g_nc_run_stream_active = true;
    g_nc_run_stream_pos = 0;
    g_nc_run_stream_len = 0;
    g_nc_run_stream_end_line = (size_t)-1;
    g_nc_run_stream_line[0] = '\0';
    grbl_stream_printf("[MSG:NC STREAM START %lu]\r\n", (unsigned long)(line + 1u));
    grbl_stream_readonly(nc_run_stream_getc,
                         nc_run_stream_available,
                         nc_run_stream_clear);
    return true;
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
