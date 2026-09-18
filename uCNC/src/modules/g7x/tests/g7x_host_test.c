#include "../g7x_contour.h"
#include "../g7x_source.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

typedef struct {
    char lines[256][96];
    int count;
} sink_t;

static int collect_g7x(g7x_stream_t *stream, sink_t *sink)
{
    char out[96];
    g7x_step_result_t step;

    memset(sink, 0, sizeof(*sink));
    while ((step = g7x_stream_next(stream, out, sizeof(out))) == G7X_STEP_LINE) {
        if (sink->count >= 256) {
            printf("FAIL too many G7x lines\n");
            return 1;
        }
        strncpy(sink->lines[sink->count], out, sizeof(sink->lines[sink->count]) - 1u);
        sink->count++;
    }
    if (step != G7X_STEP_DONE) {
        printf("FAIL G7x stream ended with %d\n", (int)step);
        return 1;
    }
    return 0;
}

static int collect_g76(g7x_thread_stream_t *stream, sink_t *sink)
{
    char out[96];
    g7x_step_result_t step;

    memset(sink, 0, sizeof(*sink));
    while ((step = g7x_thread_next(stream, out, sizeof(out))) == G7X_STEP_LINE) {
        if (sink->count >= 256) {
            printf("FAIL too many G76 lines\n");
            return 1;
        }
        strncpy(sink->lines[sink->count], out, sizeof(sink->lines[sink->count]) - 1u);
        sink->count++;
    }
    if (step != G7X_STEP_DONE) {
        printf("FAIL G76 stream ended with %d\n", (int)step);
        return 1;
    }
    return 0;
}

static int contains(const sink_t *sink, const char *needle)
{
    int i;

    for (i = 0; i < sink->count; i++)
        if (strstr(sink->lines[i], needle))
            return 1;
    return 0;
}

static int has_exact(const sink_t *sink, const char *line)
{
    int i;

    for (i = 0; i < sink->count; i++)
        if (strcmp(sink->lines[i], line) == 0)
            return 1;
    return 0;
}

static int add_line(g7x_stream_t *stream, const char *line)
{
    bool done = false;
    g7x_result_t result = g7x_stream_add_line(stream, line, &done);

    if (result != G7X_OK) {
        printf("FAIL add %s result=%s\n", line, g7x_result_text(result));
        return 1;
    }
    return 0;
}

static int test_allowance_approach(void)
{
    /* Diameter source coordinates: X allowance 0.5 and radial R1 produce
       X52.5 clearance. Z allowance 0.5 plus R1 produces +/-1.5 clearance. */
    for (int cycle = 71; cycle <= 72; cycle++) {
        for (int direction = -1; direction <= 1; direction += 2) {
            g7x_stream_t stream;
            sink_t sink;
            const char *header = cycle == 71 ? "G71 U2 R1 X0.5 Z0.5 F120" :
                                                "G72 W2 R1 X0.5 Z0.5 F120";
            if (g7x_stream_begin(&stream, header) != G7X_OK ||
                add_line(&stream, "G1 X50 Z0") ||
                add_line(&stream, direction < 0 ? "G1 X30 Z-10" : "G1 X30 Z10") ||
                add_line(&stream, "G80") || collect_g7x(&stream, &sink)) return 1;
            const char *zclear = direction < 0 ? "G0 Z1.500" : "G0 Z-1.500";
            if (sink.count < 5 || strcmp(sink.lines[2], "G0 X52.500") ||
                strcmp(sink.lines[3], zclear)) {
                printf("FAIL G%d approach direction %d\n", cycle, direction); return 1;
            }
            for (int i = 0; i < sink.count; i++) {
                if (!strncmp(sink.lines[i], "G0 ", 3) &&
                    strchr(sink.lines[i], 'X') && strchr(sink.lines[i], 'Z')) {
                    puts("FAIL diagonal rapid in cycle"); return 1;
                }
                if (!strcmp(sink.lines[i], "(G7x finish contour)")) {
                    if (i + 2 >= sink.count || strcmp(sink.lines[i+1], "G0 X52.500") ||
                        strcmp(sink.lines[i+2], zclear)) {
                        puts("FAIL finish approach clearance"); return 1;
                    }
                }
            }
            if (strcmp(sink.lines[sink.count-2], "G0 X52.500") ||
                strcmp(sink.lines[sink.count-1], zclear)) {
                puts("FAIL final allowance clearance"); return 1;
            }
        }
    }
    return 0;
}

static int test_g71_basic(void)
{
    g7x_stream_t stream;
    sink_t sink;

    if (g7x_stream_begin(&stream, "G71 U2 R1 X0.5 Z0.5 F120") != G7X_OK)
        return 1;
    if (add_line(&stream, "G1 X50 Z0") ||
        add_line(&stream, "G1 X40 Z-10") ||
        add_line(&stream, "G1 X30 Z-25") ||
        add_line(&stream, "G80"))
        return 1;
    if (collect_g7x(&stream, &sink))
        return 1;
    if (!contains(&sink, "(G71 rough X23.000)") ||
        !contains(&sink, "(G7x finish contour)") ||
        !has_exact(&sink, "G1 X50.000 Z0.000 F120.000") ||
        !has_exact(&sink, "G1 X30.000 Z-25.000")) {
        printf("FAIL G71 basic output\n");
        return 1;
    }
    return 0;
}

static int test_g72_basic(void)
{
    g7x_stream_t stream;
    sink_t sink;

    if (g7x_stream_begin(&stream, "G72 W2 R1 X0.5 Z0.5 F120") != G7X_OK)
        return 1;
    if (add_line(&stream, "G1 X10 Z-25") ||
        add_line(&stream, "G1 X40 Z-25 C0 R0") ||
        add_line(&stream, "G1 X40 Z0") ||
        add_line(&stream, "G80"))
        return 1;
    if (collect_g7x(&stream, &sink))
        return 1;
    if (!contains(&sink, "(G72 rough Z") ||
        !contains(&sink, "(G7x finish contour)") ||
        !has_exact(&sink, "G1 X10.000 Z-25.000 F120.000") ||
        !has_exact(&sink, "G1 X40.000 Z0.000")) {
        printf("FAIL G72 basic output\n");
        return 1;
    }
    return 0;
}

static int test_g71_corner_radius(void)
{
    g7x_stream_t stream;
    sink_t sink;

    if (g7x_stream_begin(&stream, "G71 U2 R1 X0.5 Z0.5 F120") != G7X_OK)
        return 1;
    if (add_line(&stream, "G1 X50 Z0") ||
        add_line(&stream, "G1 X50 Z-10 R1") ||
        add_line(&stream, "G1 X40 Z-10") ||
        add_line(&stream, "G1 X40 Z-20") ||
        add_line(&stream, "G80"))
        return 1;
    if (collect_g7x(&stream, &sink))
        return 1;
    if (!has_exact(&sink, "G1 X50.000 Z-9.000") ||
        !contains(&sink, "G3 X48.000 Z-10.000 I-1.000 K0.000")) {
        printf("FAIL G71 radius corner output\n");
        return 1;
    }
    return 0;
}

static int test_bad_contour_rejected(void)
{
    g7x_stream_t stream;
    bool done = false;

    if (g7x_stream_begin(&stream, "G71 U2 R1 X0.5 Z0.5 F120") != G7X_OK)
        return 1;
    if (add_line(&stream, "G1 X50 Z0") ||
        add_line(&stream, "G1 X25 Z-25") ||
        add_line(&stream, "G1 X40 Z-10"))
        return 1;
    if (g7x_stream_add_line(&stream, "G80", &done) != G7X_UNSUPPORTED) {
        printf("FAIL non-monotonic contour accepted\n");
        return 1;
    }
    return 0;
}

static int test_g76_basic(void)
{
    g7x_thread_stream_t stream;
    sink_t sink;

    if (g7x_thread_begin(&stream,
                         "G76 START_X40 X36 Z-20 P2 Q1 F1.5 H1 MIN_Q1",
                         0.0f,
                         2.0f) != G7X_OK) {
        printf("FAIL G76 begin\n");
        return 1;
    }
    if (collect_g76(&stream, &sink))
        return 1;
    if (!contains(&sink, "(G76 FANUC D 40.000 X 36.000 F 1.500 P 2.000 Q 1.000 R 0.000") ||
        !has_exact(&sink, "G0 X42.000 Z1.500") ||
        !contains(&sink, "(THREAD pass 2 X36.000") ||
        !has_exact(&sink, "G33 X36.000 Z-20.000 K1.500") ||
        !contains(&sink, "(THREAD spring X36.000") ||
        !has_exact(&sink, "G0 Z1.500")) {
        printf("FAIL G76 basic output\n");
        return 1;
    }
    return 0;
}

static int test_g76_taper_id(void)
{
    g7x_thread_stream_t stream;
    sink_t sink;

    if (g7x_thread_begin(&stream,
                         "G76 START_X20 X24 Z10 Z1=0 P2 Q1 F2 D1 MIN_Q1",
                         0.0f,
                         1.0f) != G7X_OK) {
        printf("FAIL G76 ID/taper begin\n");
        return 1;
    }
    if (collect_g76(&stream, &sink))
        return 1;
    if (!has_exact(&sink, "G0 X19.000 Z-2.000") ||
        !contains(&sink, "(THREAD pass 2 X24.000 Z0.000)") ||
        !has_exact(&sink, "G33 X25.000 Z10.000 K2.000")) {
        printf("FAIL G76 ID/taper output\n");
        return 1;
    }
    return 0;
}

static int test_g76_letters(void)
{
    g7x_thread_stream_t stream;
    sink_t sink;

    if (g7x_thread_begin(&stream,
                         "G76 START_X20 X18 Z-20 P1.0 Q0.3 MIN_Q0.05 FINISH_R0.02 F1.5 H2",
                         0.0f,
                         1.0f) != G7X_OK) {
        printf("FAIL G76 semantic begin\n");
        return 1;
    }
    if (collect_g76(&stream, &sink))
        return 1;
    if (!contains(&sink, "(G76 FANUC D 20.000 X 18.000 F 1.500 P 1.000 Q 0.050 R 0.020") ||
        !has_exact(&sink, "G0 X21.000 Z1.500") ||
        !contains(&sink, "(THREAD finish X18.000") ||
        !contains(&sink, "(THREAD spring X18.000") ||
        !has_exact(&sink, "G33 X18.000 Z-20.000 K1.500")) {
        printf("FAIL G76 semantic output\n");
        return 1;
    }
    return 0;
}

static int test_g76_semantic_invalid_pitch_depth(void)
{
    g7x_thread_stream_t stream;

    if (g7x_thread_begin(&stream,
                         "G76 START_X20 X18 Z-20 P0 Q0.3 F0",
                         0.0f,
                         1.0f) != G7X_BAD_FIELD) {
        printf("FAIL G76 invalid pitch/depth accepted\n");
        return 1;
    }
    return 0;
}

static int test_g76_decreasing_schedule(void)
{
    g7x_thread_stream_t stream;
    sink_t sink;
    if (g7x_thread_begin_semantic(&stream, 40, 36, 0, -20, 1.5f,
                                 2, 1, .25f, .1f, 1, 0, 1, 0, 0) != G7X_OK ||
        collect_g76(&stream, &sink)) return 1;
    unsigned cuts = 0;
    for (int i = 0; i < sink.count; i++)
        if (strncmp(sink.lines[i], "G33 ", 4) == 0) cuts++;
    if (cuts != 5 || !contains(&sink, "(THREAD pass 1 X38.000") ||
        !contains(&sink, "(THREAD pass 2 X36.500") ||
        !contains(&sink, "(THREAD pass 3 X36.200") ||
        !contains(&sink, "(THREAD finish X36.000") ||
        !contains(&sink, "(THREAD spring X36.000")) {
        puts("FAIL first/decreasing/minimum/finish/spring schedule"); return 1;
    }
    return 0;
}

static int test_g76_invalid_contract(void)
{
    g7x_thread_stream_t stream;
    const char *bad[] = {
        "G76 START_X40 X36 Z-20 P1 Q1 F1.5", /* inconsistent height */
        "G76 START_X40 X36 Z-20 P2 Q1 Fnan",
        "G76 START_X40 X36 Z-20 P2 Q1 F1.5 MIN_Q2",
        "G76 START_X40 X36 Z-20 P2 Q1 F1.5 L1.5",
        "G76 START_X40 X36 Z-20 P2 Q1 F1.5 L501",
        "G76 START_X40 X36 Z-20 P2 Q1 F1.5 I-30"
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (g7x_thread_begin(&stream, bad[i], 0, 1) != G7X_BAD_FIELD) {
            printf("FAIL accepted %s\n", bad[i]); return 1;
        }
    }
    if (g7x_thread_begin_semantic(&stream, 40, 36, 0, -20, INFINITY,
                                2, 1, .25f, 0, 1, 0, 0, 0, 0) != G7X_BAD_FIELD)
        return 1;
    return 0;
}

/* Fanuc two-line header: depth and retract on the first block, range and finish
   allowances in U/W on the second. It must generate exactly the same cycle as
   the project's one-line spelling of the same geometry. */
static int test_two_line_header(void)
{
    static const char *first[2] = { "G71 U2 R1", "G72 W2 R1" };
    static const char *second[2] = { "G71 P100 Q200 U0.5 W0.25 F120",
                                     "G72 P100 Q200 U0.5 W0.25 F120" };
    static const char *single[2] = { "G71 U2 R1 X0.5 Z0.25 F120",
                                     "G72 W2 R1 X0.5 Z0.25 F120" };
    g7x_stream_t linked;
    g7x_stream_t plain;
    sink_t a;
    sink_t b;
    int cycle;
    int i;

    for (cycle = 0; cycle < 2; cycle++) {
        g7x_stream_reset(&linked);
        g7x_stream_reset(&plain);
        if (g7x_stream_begin_linked(&linked, first[cycle], second[cycle]) != G7X_OK ||
            g7x_stream_begin(&plain, single[cycle]) != G7X_OK)
            return 1;
        for (i = 0; i < 2; i++) {
            g7x_stream_t *stream = i == 0 ? &linked : &plain;
            if (add_line(stream, "N100 G1 X50 Z0") ||
                add_line(stream, "G1 X50 Z-10") ||
                add_line(stream, "N200 G1 X40 Z-10") ||
                add_line(stream, "G80"))
                return 1;
        }
        if (collect_g7x(&linked, &a) || collect_g7x(&plain, &b))
            return 1;
        if (a.count != b.count) {
            printf("FAIL two-line stream count %d vs %d\n", a.count, b.count);
            return 1;
        }
        for (i = 0; i < a.count; i++) {
            if (strcmp(a.lines[i], b.lines[i])) {
                printf("FAIL two-line [%s] vs [%s]\n", a.lines[i], b.lines[i]);
                return 1;
            }
        }
        /* X 50 + U0.5 + R1 is a diameter of 52.5; Z 0 + W0.25 + R1 clears 1.25. */
        if (!has_exact(&a, "G0 X52.500") || !has_exact(&a, "G0 Z1.250")) {
            printf("FAIL two-line clearance cycle %d\n", cycle);
            return 1;
        }
    }
    return 0;
}

typedef struct {
    uint32_t numbers[4];
    const char *texts[4];
    unsigned count;
} fake_source_t;

static g7x_source_status_t fake_source_next(void *user,
                                            const g7x_source_pos_t *from,
                                            g7x_source_pos_t *out,
                                            char *text,
                                            size_t text_sz)
{
    const fake_source_t *source = user;
    unsigned i;

    for (i = (unsigned)from->line_index; i < source->count; i++) {
        if (source->numbers[i] < from->number)
            continue;
        snprintf(text, text_sz, "%s", source->texts[i]);
        out->number = source->numbers[i];
        out->line_index = (size_t)i + 1u;
        return G7X_SOURCE_OK;
    }
    return G7X_SOURCE_MISSING;
}

typedef struct {
    uint32_t seen[8];
    unsigned count;
} range_log_t;

static void log_range_block(void *user, uint32_t number, const char *text)
{
    range_log_t *log = user;

    (void)text;
    if (log->count < 8)
        log->seen[log->count] = number;
    log->count++;
}

/* Bounded serial retention: lookup by number, ordered range walk, and explicit
   failures for missing, ambiguous and evicted blocks. */
static int test_numbered_history(void)
{
    g7x_history_t history;
    range_log_t log;
    char long_text[G7X_RETAINED_TEXT_LEN + 8];
    unsigned visited = 0;
    unsigned i;

    g7x_history_reset(&history);
    if (g7x_history_find(&history, 100) ||
        g7x_history_visit_range(&history, 100, 200, log_range_block, &log,
                                &visited) != G7X_RANGE_MISSING ||
        g7x_history_add(NULL, 1, "x") != G7X_BAD_FIELD ||
        g7x_history_add(&history, 1, "") != G7X_BAD_FIELD)
        return 1;

    memset(long_text, 'G', sizeof(long_text) - 1u);
    long_text[sizeof(long_text) - 1u] = '\0';
    if (g7x_history_add(&history, 99, long_text) != G7X_WRITE_FAILED)
        return 1;

    if (g7x_history_add(&history, 100, "N100 G0 X50 Z0") != G7X_OK ||
        g7x_history_add(&history, 150, "N150 G1 Z-10") != G7X_OK ||
        g7x_history_add(&history, 200, "N200 G1 X40") != G7X_OK)
        return 1;
    if (!g7x_history_find(&history, 100) ||
        !g7x_history_find(&history, 200) || g7x_history_find(&history, 300) ||
        strcmp(g7x_history_find(&history, 150), "N150 G1 Z-10"))
        return 1;

    memset(&log, 0, sizeof(log));
    if (g7x_history_visit_range(&history, 100, 200, log_range_block, &log,
                                &visited) != G7X_OK ||
        visited != 3 || log.count != 3 || log.seen[0] != 100 ||
        log.seen[1] != 150 || log.seen[2] != 200) {
        printf("FAIL history range walk visited=%u\n", visited);
        return 1;
    }
    if (g7x_history_visit_range(&history, 100, 300, log_range_block, &log,
                                &visited) != G7X_RANGE_MISSING ||
        g7x_history_visit_range(&history, 200, 100, log_range_block, &log,
                                &visited) != G7X_RANGE_AMBIGUOUS ||
        g7x_history_visit_range(&history, 100, 200, NULL, &log,
                                &visited) != G7X_BAD_FIELD)
        return 1;

    /* A repeated number inside the range is ambiguous, not "the last one". */
    g7x_history_reset(&history);
    if (g7x_history_add(&history, 100, "N100 G0 X50 Z0") != G7X_OK ||
        g7x_history_add(&history, 150, "N150 G1 Z-10") != G7X_OK ||
        g7x_history_add(&history, 175, "N175 G1 X45") != G7X_OK ||
        g7x_history_add(&history, 150, "N150 G1 Z-12") != G7X_OK ||
        g7x_history_add(&history, 200, "N200 G1 X40") != G7X_OK ||
        g7x_history_visit_range(&history, 100, 200, log_range_block, &log,
                                &visited) != G7X_RANGE_AMBIGUOUS)
        return 1;
    /* A repeated number after the Q block is outside the range and allowed. */
    g7x_history_reset(&history);
    if (g7x_history_add(&history, 100, "N100 G0 X50 Z0") != G7X_OK ||
        g7x_history_add(&history, 150, "N150 G1 Z-10") != G7X_OK ||
        g7x_history_add(&history, 200, "N200 G1 X40") != G7X_OK ||
        g7x_history_add(&history, 150, "N150 G1 Z-12") != G7X_OK ||
        g7x_history_visit_range(&history, 100, 200, log_range_block, &log,
                                &visited) != G7X_OK || visited != 3)
        return 1;

    /* Eviction is bounded and must not resolve dropped blocks by accident. */
    g7x_history_reset(&history);
    for (i = 0; i < G7X_MAX_RETAINED_BLOCKS + 2u; i++) {
        char text[G7X_RETAINED_TEXT_LEN];
        snprintf(text, sizeof(text), "N%u G1 X1", 1000u + i);
        if (g7x_history_add(&history, 1000u + i, text) != G7X_OK)
            return 1;
    }
    if (!g7x_history_evicted(&history) ||
        g7x_history_find(&history, 1000) ||
        !g7x_history_find(&history, 1000u + G7X_MAX_RETAINED_BLOCKS + 1u) ||
        g7x_history_visit_range(&history, 1000, 1000u + G7X_MAX_RETAINED_BLOCKS + 1u,
                                log_range_block, &log, &visited) != G7X_RANGE_MISSING)
        return 1;
    return 0;
}

/* The caller-supplied cursor is the only random-access path into program text;
   G7x itself must report a missing source instead of guessing. */
static int test_numbered_source(void)
{
    fake_source_t source = {
        { 100u, 150u, 200u },
        { "N100 G0 X50 Z0", "N150 G1 Z-10", "N200 G1 X40" },
        3u
    };
    g7x_source_t cursor = { fake_source_next, &source };
    g7x_source_pos_t pos = { 0u, 0u };
    g7x_source_pos_t found = { 0u, 0u };
    char text[48];
    const uint32_t want[3] = { 100u, 150u, 200u };
    unsigned i;

    if (g7x_source_next(NULL, &pos, &found, text, sizeof(text)) != G7X_SOURCE_ERROR)
        return 1;
    for (i = 0; i < 3u; i++) {
        if (g7x_source_next(&cursor, &pos, &found, text, sizeof(text)) != G7X_SOURCE_OK ||
            found.number != want[i] || strcmp(text, source.texts[i]))
            return 1;
        pos = found;
    }
    if (g7x_source_next(&cursor, &pos, &found, text, sizeof(text)) != G7X_SOURCE_MISSING)
        return 1;

    /* Starting mid-document resumes at the requested line index. */
    pos.number = 0u;
    pos.line_index = 1u;
    if (g7x_source_next(&cursor, &pos, &found, text, sizeof(text)) != G7X_SOURCE_OK ||
        found.number != 150u)
        return 1;
    return 0;
}

int main(void)
{
    int fails = 0;

    fails += test_g71_basic();
    fails += test_allowance_approach();
    fails += test_g72_basic();
    fails += test_g71_corner_radius();
    fails += test_bad_contour_rejected();
    fails += test_g76_basic();
    fails += test_g76_taper_id();
    fails += test_g76_letters();
    fails += test_g76_semantic_invalid_pitch_depth();
    fails += test_g76_decreasing_schedule();
    fails += test_g76_invalid_contract();
    fails += test_numbered_history();
    fails += test_numbered_source();
    fails += test_two_line_header();

    if (fails) {
        printf("FAILURES %d\n", fails);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
