#include "../g7x_contour.h"

#include <stdio.h>
#include <string.h>

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

static int test_g71_basic(void)
{
    g7x_stream_t stream;
    sink_t sink;

    if (g7x_stream_begin(&stream, "G71 U2 R1 X0.5 Z0.5 F120") != G7X_OK)
        return 1;
    if (add_line(&stream, "G1 X50 Z0") ||
        add_line(&stream, "G1 X40 Z-10") ||
        add_line(&stream, "G1 X30 Z-25") ||
        add_line(&stream, "G1 X50 Z-25") ||
        add_line(&stream, "G80"))
        return 1;
    if (collect_g7x(&stream, &sink))
        return 1;
    if (!contains(&sink, "(G71 rough X48.000)") ||
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
                         "G76 START_X20 X24 Z10 Z1=0 P2 Q2 F2 D1 MIN_Q2",
                         0.0f,
                         1.0f) != G7X_OK) {
        printf("FAIL G76 ID/taper begin\n");
        return 1;
    }
    if (collect_g76(&stream, &sink))
        return 1;
    if (!has_exact(&sink, "G0 X17.000 Z-2.000") ||
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

int main(void)
{
    int fails = 0;

    fails += test_g71_basic();
    fails += test_g72_basic();
    fails += test_g71_corner_radius();
    fails += test_bad_contour_rejected();
    fails += test_g76_basic();
    fails += test_g76_taper_id();
    fails += test_g76_letters();
    fails += test_g76_semantic_invalid_pitch_depth();

    if (fails) {
        printf("FAILURES %d\n", fails);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
