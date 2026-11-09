#include "nc_sender.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *g_plain[] = {
    "(comment)",
    "G970 X10 Z2",
    "G0 X40 Z0",
    "G1 U-2 W-5 F100",
    "M2"
};

static const char *g_compensated[] = {
    "G0 X50 Z2",
    "T2",
    "G42",
    "G71 U1 R0.5 X0 Z0 F100",
    "G1 X40 Z0",
    "G1 X40 Z-8",
    "G80",
    "M2"
};

static unsigned g_radius_calls;
static float g_last_radius;

typedef struct {
    const char *const *lines;
    size_t count;
} nc_sender_test_source_t;

static const char *program_line(void *user, size_t index)
{
    const nc_sender_test_source_t *source = user;

    return index < source->count ? source->lines[index] : "";
}

static bool tool_radius(void *user, int tool, float *radius)
{
    (void)user;
    g_radius_calls++;
    if (tool != 2 || !radius) {
        return false;
    }
    *radius = 1.5f;
    g_last_radius = *radius;
    return true;
}

void grbl_stream_printf(const char *fmt, ...)
{
    (void)fmt;
}

static g7x_doc_t document(size_t count, const void *source)
{
    g7x_doc_t doc;

    doc.line = program_line;
    doc.user = (void *)source;
    doc.count = count;
    return doc;
}

static int check_point(void)
{
    char out[96];
    float x = 40.0f;
    float z = 0.0f;
    uint8_t words = 0u;

    if (!nc_sender_emit_line_point("G1 X40 Z2 U-1.5 W-2", &x, &z,
                                   out, sizeof(out), &words)) {
        return 1;
    }
    if (x != 38.5f || z != 0.0f ||
        words != (NC_SENDER_EMIT_WORD_X_ABS | NC_SENDER_EMIT_WORD_Z_ABS |
                  NC_SENDER_EMIT_WORD_U_INC | NC_SENDER_EMIT_WORD_W_INC) ||
        !strstr(out, "X38.500") || !strstr(out, "Z0.000")) {
        printf("nc_sender_host: FAIL U/W resolution: \"%s\" X%.3f Z%.3f\n",
               out, (double)x, (double)z);
        return 1;
    }
    if (nc_sender_emit_line_is_direct("G70 P10 Q20") ||
        !nc_sender_emit_line_is_direct("G1 X20 Z0") ||
        !nc_sender_emit_line_is_direct("M3 S800")) {
        puts("nc_sender_host: FAIL direct-line classification");
        return 1;
    }
    return 0;
}

static int check_source_filter(void)
{
    static const nc_sender_test_source_t source = {
        g_plain, sizeof(g_plain) / sizeof(g_plain[0])
    };
    g7x_doc_t doc = document(source.count, &source);
    char out[96];

    if (nc_sender_emit_source_line(&doc, 0u, out, sizeof(out)) !=
            NC_SENDER_EMIT_SKIP ||
        nc_sender_emit_source_line(&doc, 1u, out, sizeof(out)) !=
            NC_SENDER_EMIT_SKIP ||
        nc_sender_emit_source_line(&doc, 2u, out, sizeof(out)) !=
            NC_SENDER_EMIT_LINE ||
        strcmp(out, "G0 X40 Z0") != 0) {
        puts("nc_sender_host: FAIL source-line filter");
        return 1;
    }
    return 0;
}

static int check_stream(bool compensated)
{
    static const nc_sender_test_source_t plain = {
        g_plain, sizeof(g_plain) / sizeof(g_plain[0])
    };
    static const nc_sender_test_source_t comp = {
        g_compensated, sizeof(g_compensated) / sizeof(g_compensated[0])
    };
    const nc_sender_test_source_t *source = compensated ? &comp : &plain;
    g7x_doc_t doc = document(source->count, source);
    nc_sender_emit_stream_t stream;
    char out[96];
    size_t line = 0u;
    unsigned guard = 0u;
    bool error = false;

    g_radius_calls = 0u;
    g_last_radius = 0.0f;
    nc_sender_emit_stream_begin(&stream, &doc, 0u,
                                compensated ? tool_radius : 0, 0);
    nc_sender_emit_stream_set_log(&stream, false);
    while (stream.active && guard++ < 200u) {
        nc_sender_emit_result_t result =
            nc_sender_emit_stream_next(&stream, out, sizeof(out), &line);

        if (result == NC_SENDER_EMIT_ERROR) {
            error = true;
            break;
        }
    }
    if (guard >= 200u) {
        puts("nc_sender_host: FAIL stream did not terminate");
        return 1;
    }
    if (compensated) {
        if (error || !g_radius_calls || g_last_radius != 1.5f) {
            puts("nc_sender_host: FAIL compensated cycle did not resolve T2");
            return 1;
        }
    } else if (error) {
        puts("nc_sender_host: FAIL plain program stopped on a generator error");
        return 1;
    }
    return 0;
}

int main(void)
{
    if (check_point() || check_source_filter() ||
        check_stream(false) || check_stream(true)) {
        return 1;
    }
    puts("nc_sender_host: PASS U/W, filtering, paging and compensated-cycle lookup");
    return 0;
}
