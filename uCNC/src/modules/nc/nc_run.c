#include "nc_run.h"

#include "../../interface/grbl_stream.h"

#include <stdio.h>

static bool g_nc_run_active;
static bool g_nc_run_hold;
static bool g_nc_run_done;
static size_t g_nc_run_line;
static nc_emit_stream_t g_nc_run_emit;
static char g_nc_run_stream_line[NC_MAX_LINE_LEN + 2];
static size_t g_nc_run_stream_pos;
static size_t g_nc_run_stream_len;
static const nc_document_t *g_nc_run_stream_doc;
static bool g_nc_run_stream_active;

static bool nc_run_stream_load_line(void);

static uint8_t nc_run_stream_available(void)
{
    if (g_nc_run_stream_pos < g_nc_run_stream_len) {
        return 1u;
    }
    return nc_run_stream_load_line() ? 1u : 0u;
}

static uint8_t nc_run_stream_getc(void)
{
    if (g_nc_run_stream_pos >= g_nc_run_stream_len &&
        !nc_run_stream_load_line()) {
        return 0;
    }
    return (uint8_t)g_nc_run_stream_line[g_nc_run_stream_pos++];
}

static void nc_run_stream_clear(void)
{
    g_nc_run_stream_pos = 0;
    g_nc_run_stream_len = 0;
    g_nc_run_stream_line[0] = '\0';
    g_nc_run_stream_doc = NULL;
    g_nc_run_stream_active = false;
    grbl_stream_change(NULL);
}

static bool nc_run_line_sendable(const char *line)
{
    if (!line) {
        return false;
    }
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    return *line && *line != '(';
}

void nc_run_init(void)
{
    nc_run_reset();
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
    nc_emit_stream_begin(&g_nc_run_emit, doc, line);
    g_nc_run_active = true;
    g_nc_run_hold = false;
    g_nc_run_done = false;
    return true;
}

void nc_run_reset(void)
{
    g_nc_run_active = false;
    g_nc_run_hold = false;
    g_nc_run_done = false;
    g_nc_run_line = 0;
    nc_emit_stream_begin(&g_nc_run_emit, NULL, 0);
}

void nc_run_stop(void)
{
    g_nc_run_active = false;
    g_nc_run_hold = false;
    g_nc_run_done = false;
    nc_emit_stream_begin(&g_nc_run_emit, NULL, 0);
}

bool nc_run_toggle_hold(void)
{
    if (!g_nc_run_active) {
        return false;
    }
    g_nc_run_hold = !g_nc_run_hold;
    return true;
}

bool nc_run_active(void)
{
    return g_nc_run_active;
}

bool nc_run_hold(void)
{
    return g_nc_run_hold;
}

bool nc_run_done(void)
{
    return g_nc_run_done;
}

size_t nc_run_line(void)
{
    return g_nc_run_line;
}

void nc_run_set_line(const nc_document_t *doc, size_t line)
{
    if (doc && doc->line_count > 0 && line >= doc->line_count) {
        line = doc->line_count - 1;
    }
    g_nc_run_line = line;
    nc_emit_stream_begin(&g_nc_run_emit, doc, line);
}

void nc_run_send_line(const char *line)
{
    int n;

    if (!nc_run_line_sendable(line)) {
        return;
    }
    n = snprintf(g_nc_run_stream_line, sizeof(g_nc_run_stream_line), "%s\n", line);
    if (n <= 0) {
        return;
    }
    if (n >= (int)sizeof(g_nc_run_stream_line)) {
        n = (int)sizeof(g_nc_run_stream_line) - 1;
        g_nc_run_stream_line[n - 1] = '\n';
        g_nc_run_stream_line[n] = '\0';
    }
    g_nc_run_stream_pos = 0;
    g_nc_run_stream_len = (size_t)n;
    grbl_stream_printf("[MSG:NC SEND %.96s]\r\n", line);
    grbl_stream_readonly(nc_run_stream_getc,
                         nc_run_stream_available,
                         nc_run_stream_clear);
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
    (void)emitted_line;
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
    nc_emit_result_t emit_result;

    if (!doc || doc->line_count == 0) {
        return NC_RUN_STEP_NEEDS_PROGRAM;
    }
    if (g_nc_run_hold) {
        return NC_RUN_STEP_HOLD;
    }
    if (!g_nc_run_active || g_nc_run_done) {
        (void)nc_run_arm(doc, doc->cursor_line);
    }

    if (emitted_line) {
        *emitted_line = nc_emit_stream_line(&g_nc_run_emit);
    }
    emit_result = nc_emit_stream_next(&g_nc_run_emit, out, out_sz, emitted_line);
    g_nc_run_line = nc_emit_stream_line(&g_nc_run_emit);

    if (emit_result == NC_EMIT_LINE) {
        return NC_RUN_STEP_EMITTED;
    }
    if (g_nc_run_line < doc->line_count) {
        return NC_RUN_STEP_SKIPPED;
    }

    g_nc_run_done = true;
    g_nc_run_active = false;
    return NC_RUN_STEP_COMPLETE;
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
