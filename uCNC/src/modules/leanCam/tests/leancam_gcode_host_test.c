/* LeanCam test contract:
 * Purpose: host-side checks for G-code generation geometry and command expansion.
 * Called by: developer test runs, not firmware runtime.
 * Calls into: LeanCam generator/text/schema code under host stubs.
 * Owns: test fixtures only.
 */

#include "../leancam_gcode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LC_TEST_MAX_CAPTURE 256

const char *lc_tool_catalog_find_in_program_or_catalog(const program_t *prog, int before_or_at, int t)
{
    (void)prog;
    (void)before_or_at;
    (void)t;
    return NULL;
}

typedef struct
{
    int lines;
    int fail_after;
    char first[128];
    char last[128];
    char emitted[LC_TEST_MAX_CAPTURE][96];
    int emitted_count;
    int saw_g71_rough;
    int saw_g72_rough;
    int saw_g71_old_overrough;
} lc_test_sink_t;

static int lc_test_send(const char *line, void *user)
{
    lc_test_sink_t *sink = (lc_test_sink_t *)user;

    if (!sink || !line)
        return 0;

    if (sink->lines == 0)
    {
        strncpy(sink->first, line, sizeof(sink->first) - 1u);
        sink->first[sizeof(sink->first) - 1u] = 0;
    }

    strncpy(sink->last, line, sizeof(sink->last) - 1u);
    sink->last[sizeof(sink->last) - 1u] = 0;
    if (sink->emitted_count < LC_TEST_MAX_CAPTURE)
    {
        strncpy(sink->emitted[sink->emitted_count], line, sizeof(sink->emitted[sink->emitted_count]) - 1u);
        sink->emitted[sink->emitted_count][sizeof(sink->emitted[sink->emitted_count]) - 1u] = 0;
        sink->emitted_count++;
    }
    if (strstr(line, "(G71 rough "))
        sink->saw_g71_rough = 1;
    if (strstr(line, "(G72 rough "))
        sink->saw_g72_rough = 1;
    if (strcmp(line, "G1 Z-24.500") == 0)
        sink->saw_g71_old_overrough = 1;

    sink->lines++;
    if (sink->fail_after > 0 && sink->lines > sink->fail_after)
        return 0;

    return 1;
}

static int lc_count_emitted_contains(const lc_test_sink_t *sink, const char *needle)
{
    int i;
    int count = 0;

    if (!sink || !needle)
        return 0;
    for (i = 0; i < sink->emitted_count; ++i)
        if (strstr(sink->emitted[i], needle))
            count++;
    return count;
}

static int lc_find_emitted_exact_from(const lc_test_sink_t *sink, int start, const char *line)
{
    int i;

    if (!sink || !line)
        return -1;
    if (start < 0)
        start = 0;
    for (i = start; i < sink->emitted_count; ++i)
        if (strcmp(sink->emitted[i], line) == 0)
            return i;
    return -1;
}

static int lc_find_emitted_exact_before(const lc_test_sink_t *sink, int end, const char *line)
{
    int i;

    if (!sink || !line)
        return -1;
    if (end < 0 || end > sink->emitted_count)
        end = sink->emitted_count;
    for (i = 0; i < end; ++i)
        if (strcmp(sink->emitted[i], line) == 0)
            return i;
    return -1;
}

static int lc_expect_finish_sequence(const lc_test_sink_t *sink,
                                     const char *name,
                                     const char **finish,
                                     int finish_count)
{
    int pos;
    int i;

    pos = lc_find_emitted_exact_from(sink, 0, "(G7x finish continuous contour)");
    if (pos < 0)
    {
        printf("FAIL %s missing finish marker\n", name);
        return 1;
    }

    for (i = 0; i < finish_count; ++i)
    {
        pos = lc_find_emitted_exact_from(sink, pos + 1, finish[i]);
        if (pos < 0)
        {
            printf("FAIL %s missing finish line %s\n", name, finish[i]);
            return 1;
        }
    }

    return 0;
}

static int lc_expect_rough_lines_before_finish(const lc_test_sink_t *sink,
                                               const char *name,
                                               const char **rough,
                                               int rough_count)
{
    int finish_pos;
    int i;

    finish_pos = lc_find_emitted_exact_from(sink, 0, "(G7x finish continuous contour)");
    if (finish_pos < 0)
    {
        printf("FAIL %s missing finish marker for rough checks\n", name);
        return 1;
    }

    for (i = 0; i < rough_count; ++i)
    {
        if (lc_find_emitted_exact_before(sink, finish_pos, rough[i]) < 0)
        {
            printf("FAIL %s missing rough line %s\n", name, rough[i]);
            return 1;
        }
    }

    return 0;
}

static int lc_expect_basic_cycle_classes(const lc_test_sink_t *sink,
                                         const char *name,
                                         const char *region_marker,
                                         const char *rough_marker)
{
    if (!sink)
        return 1;
    if (lc_count_emitted_contains(sink, region_marker) != 1)
    {
        printf("FAIL %s missing region marker\n", name);
        return 1;
    }
    if (lc_count_emitted_contains(sink, rough_marker) < 1)
    {
        printf("FAIL %s missing rough marker\n", name);
        return 1;
    }
    if (lc_count_emitted_contains(sink, "(G7x finish continuous contour)") != 1)
    {
        printf("FAIL %s missing finish marker\n", name);
        return 1;
    }
    if (lc_count_emitted_contains(sink, "G0 ") < 1)
    {
        printf("FAIL %s missing rapid moves\n", name);
        return 1;
    }
    if (lc_count_emitted_contains(sink, " F") < 1)
    {
        printf("FAIL %s missing feed moves\n", name);
        return 1;
    }
    return 0;
}

static float lc_test_absf(float v)
{
    return v < 0.0f ? -v : v;
}

static int lc_test_word_float(const char *line, char word, float *out)
{
    const char *p;
    char *endp;

    if (!line || !out)
        return 0;
    for (p = line; *p; ++p)
    {
        if (*p != word)
            continue;
        if (p != line && p[-1] != ' ')
            continue;
        *out = strtof(p + 1, &endp);
        return endp && endp != p + 1;
    }
    return 0;
}

static int lc_expect_no_diagonal_g71_rough(const lc_test_sink_t *sink, const char *name)
{
    int i;
    int rough = 0;
    float cur_x = 0.0f;
    float cur_z = 0.0f;
    int have_x = 0;
    int have_z = 0;

    if (!sink)
        return 1;

    for (i = 0; i < sink->emitted_count; ++i)
    {
        const char *line = sink->emitted[i];
        float next_x = cur_x;
        float next_z = cur_z;
        float v;
        int has_x;
        int has_z;

        if (strstr(line, "(G7x finish continuous contour)"))
            rough = 0;
        if (strstr(line, "(G71 rough "))
            rough = 1;
        if (strncmp(line, "G0", 2) != 0 && strncmp(line, "G1", 2) != 0)
            continue;

        has_x = lc_test_word_float(line, 'X', &v);
        if (has_x)
        {
            next_x = v;
            have_x = 1;
        }
        has_z = lc_test_word_float(line, 'Z', &v);
        if (has_z)
        {
            next_z = v;
            have_z = 1;
        }

        if (rough && strncmp(line, "G1", 2) == 0 && have_x && have_z)
        {
            int x_changed = lc_test_absf(next_x - cur_x) > 0.001f;
            int z_changed = lc_test_absf(next_z - cur_z) > 0.001f;
            if (x_changed && z_changed)
            {
                printf("FAIL %s diagonal rough %s from X%.3f Z%.3f\n", name, line, cur_x, cur_z);
                return 1;
            }
        }

        cur_x = next_x;
        cur_z = next_z;
    }

    return 0;
}

static int lc_expect_g71_rough_z_near(const lc_test_sink_t *sink,
                                      const char *name,
                                      float cut_x,
                                      float expected_z,
                                      float tolerance)
{
    int i;
    int rough = 0;
    float cur_x = 0.0f;
    float cur_z = 0.0f;

    if (!sink)
        return 1;

    for (i = 0; i < sink->emitted_count; ++i)
    {
        const char *line = sink->emitted[i];
        float v;
        int has_x;
        int has_z;

        if (strstr(line, "(G7x finish continuous contour)"))
            rough = 0;
        if (strstr(line, "(G71 rough "))
            rough = 1;
        if (strncmp(line, "G0", 2) != 0 && strncmp(line, "G1", 2) != 0)
            continue;

        has_x = lc_test_word_float(line, 'X', &v);
        if (has_x)
            cur_x = v;
        has_z = lc_test_word_float(line, 'Z', &v);
        if (has_z)
            cur_z = v;

        if (rough && strncmp(line, "G1", 2) == 0 && has_z && lc_test_absf(cur_x - cut_x) <= 0.001f)
        {
            if (lc_test_absf(cur_z - expected_z) > tolerance)
            {
                printf("FAIL %s rough X%.3f Z %.3f expected %.3f\n", name, cut_x, cur_z, expected_z);
                return 1;
            }
            return 0;
        }
    }

    printf("FAIL %s missing rough Z at X%.3f\n", name, cut_x);
    return 1;
}

static int lc_run_g71_example(const char *name,
                              const char **contour,
                              int contour_count,
                              const char **finish,
                              int finish_count,
                              const char **rough,
                              int rough_count,
                              const char *forbidden_rough)
{
    static const char *setup = "SETUP L50 OD50 ID0 CLAMP0 EXTRA0 CLR1";
    static const char *tool = "TOOL T1 R0.8 ORIENT3 R_FEED120 FIN_FEED60 DOC2.0 FIN_DOC0.5 RPM800 XOFF0 ZOFF0";
    lc_test_sink_t sink;
    lc_gcode_result_t got = LC_GCODE_OK;
    char err[96];
    int i;

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;

    if (!leancam_gcode_emit_program_header(lc_test_send, &sink))
    {
        printf("FAIL %s header\n", name);
        return 1;
    }

    got = leancam_gcode_run_program_line_ex("G71 U2 R1 X0.5 Z0.5 F120",
                                            setup, tool, lc_test_send, &sink, err, sizeof(err));
    for (i = 0; got == LC_GCODE_OK && i < contour_count; ++i)
        got = leancam_gcode_run_program_line_ex(contour[i],
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G80",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK && !leancam_gcode_emit_program_footer_ex(lc_test_send, &sink, err, sizeof(err)))
        got = LC_GCODE_STREAM_REJECT;

    if (got != LC_GCODE_OK)
    {
        printf("FAIL %s result=%d err=%s\n", name, (int)got, err);
        return 1;
    }
    if (lc_count_emitted_contains(&sink, "(LC G71 region elements ") != 1)
    {
        printf("FAIL %s region count\n", name);
        return 1;
    }
    if (!sink.saw_g71_rough)
    {
        printf("FAIL %s no roughing\n", name);
        return 1;
    }
    if (forbidden_rough && lc_find_emitted_exact_from(&sink, 0, forbidden_rough) >= 0)
    {
        printf("FAIL %s unsafe rough line %s\n", name, forbidden_rough);
        return 1;
    }
    if (lc_expect_rough_lines_before_finish(&sink, name, rough, rough_count))
        return 1;
    if (lc_expect_no_diagonal_g71_rough(&sink, name))
        return 1;
    if (lc_expect_finish_sequence(&sink, name, finish, finish_count))
        return 1;

    return 0;
}

static int lc_run_g71_examples(const char *setup, const char *tool)
{
    const char *a_contour[] = {
        "G1 X50 Z0",
        "G1 X40 Z-10",
        "G1 X30 Z-25",
        "G1 X50 Z-25"
    };
    const char *a_finish[] = {
        "G1 X50.000 Z0.000 F60.000",
        "G1 X40.000 Z-10.000",
        "G1 X30.000 Z-25.000",
        "G1 X50.000 Z-25.000"
    };
    const char *a_rough[] = {
        "(G71 rough X48.000)",
        "(G71 rough X30.500)"
    };
    const char *b_contour[] = {
        "G1 X50 Z0",
        "G1 X42 Z-8",
        "G1 X35 Z-18",
        "G1 X25 Z-30",
        "G1 X50 Z-30"
    };
    const char *b_finish[] = {
        "G1 X50.000 Z0.000 F60.000",
        "G1 X42.000 Z-8.000",
        "G1 X35.000 Z-18.000",
        "G1 X25.000 Z-30.000",
        "G1 X50.000 Z-30.000"
    };
    const char *b_rough[] = {
        "(G71 rough X48.000)",
        "(G71 rough X25.500)"
    };
    const char *c_contour[] = {
        "G1 X50 Z0",
        "G1 X45 Z-6",
        "G1 X38 Z-14",
        "G1 X32 Z-24",
        "G1 X25 Z-35",
        "G1 X50 Z-35"
    };
    const char *c_finish[] = {
        "G1 X50.000 Z0.000 F60.000",
        "G1 X45.000 Z-6.000",
        "G1 X38.000 Z-14.000",
        "G1 X32.000 Z-24.000",
        "G1 X25.000 Z-35.000",
        "G1 X50.000 Z-35.000"
    };
    const char *c_rough[] = {
        "(G71 rough X48.000)",
        "(G71 rough X25.500)"
    };
    const char *edited_start_contour[] = {
        "G1 X25 Z2",
        "G1 X25 Z-10 C0 R0",
        "G1 X35 Z-10 C0 R0",
        "G1 X35 Z-15 C0 R0",
        "G1 X51 Z-15"
    };
    const char *edited_start_finish[] = {
        "G1 X25.000 Z2.000 F60.000",
        "G1 X25.000 Z-10.000",
        "G1 X35.000 Z-10.000",
        "G1 X35.000 Z-15.000",
        "G1 X51.000 Z-15.000"
    };
    const char *edited_start_rough[] = {
        "(G71 rough X49.000)",
        "(G71 rough X25.500)"
    };
    const char *scanline_contour[] = {
        "G1 X25 Z0",
        "G1 X25 Z-15 C0 R0",
        "G1 X30 Z-20 C0 R0",
        "G1 X50 Z-25"
    };
    const char *scanline_finish[] = {
        "G1 X25.000 Z0.000 F60.000",
        "G1 X25.000 Z-15.000",
        "G1 X30.000 Z-20.000",
        "G1 X50.000 Z-25.000"
    };
    const char *scanline_rough[] = {
        "(G71 rough X48.000)",
        "(G71 rough X25.500)"
    };
    const char *arc_contour[] = {
        "G1 X50 Z0",
        "G3 X40 Z-10 R10",
        "G1 X30 Z-25",
        "G1 X50 Z-25"
    };
    const char *arc_finish[] = {
        "G1 X50.000 Z0.000 F60.000",
        "G3 X40.000 Z-10.000 R10.000",
        "G1 X30.000 Z-25.000",
        "G1 X50.000 Z-25.000"
    };
    const char *arc_rough[] = {
        "(G71 rough X48.000)",
        "(G71 rough X30.500)"
    };
    const char *arc_cw_contour[] = {
        "G1 X40 Z0",
        "G2 X50 Z-10 R10",
        "G1 X50 Z-25",
        "G1 X40 Z-25"
    };
    const char *arc_cw_finish[] = {
        "G1 X40.000 Z0.000 F60.000",
        "G2 X50.000 Z-10.000 R10.000",
        "G1 X50.000 Z-25.000",
        "G1 X40.000 Z-25.000"
    };
    const char *arc_cw_rough[] = {
        "(G71 rough X48.000)",
        "(G71 rough X40.500)"
    };
    int fails = 0;

    (void)setup;
    (void)tool;
    fails += lc_run_g71_example("G71 example A", a_contour, 4, a_finish, 4, a_rough, 2, "G1 Z-24.500");
    fails += lc_run_g71_example("G71 example B", b_contour, 5, b_finish, 5, b_rough, 2, "G1 Z-29.500");
    fails += lc_run_g71_example("G71 example C", c_contour, 6, c_finish, 6, c_rough, 2, "G1 Z-34.500");
    fails += lc_run_g71_example("G71 edited start/close", edited_start_contour, 5, edited_start_finish, 5, edited_start_rough, 2, NULL);
    fails += lc_run_g71_example("G71 scanline only", scanline_contour, 4, scanline_finish, 4, scanline_rough, 2, NULL);
    fails += lc_run_g71_example("G71 simple arc", arc_contour, 4, arc_finish, 4, arc_rough, 2, NULL);
    fails += lc_run_g71_example("G71 simple CW arc", arc_cw_contour, 4, arc_cw_finish, 4, arc_cw_rough, 2, NULL);
    if (!fails)
        printf("PASS G71 examples\n");
    return fails;
}

static int lc_run_raw_g7x_rough_checks(const char *setup, const char *tool)
{
    lc_test_sink_t sink;
    lc_gcode_result_t got;
    char err[96];

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;

    got = leancam_gcode_run_program_line_ex("G71 U2 R1 X0.5 Z0.5 F120",
                                            setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X50 Z0",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X25 Z-25 C0 R0",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X50 Z-25",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G80",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK && !leancam_gcode_emit_program_footer_ex(lc_test_send, &sink, err, sizeof(err)))
        got = LC_GCODE_STREAM_REJECT;
    if (got != LC_GCODE_OK || !sink.saw_g71_rough || sink.saw_g71_old_overrough)
    {
        printf("FAIL raw G71 rough result=%d err=%s rough=%d old=%d\n",
               (int)got, err, sink.saw_g71_rough, sink.saw_g71_old_overrough);
        return 1;
    }

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    if (!leancam_gcode_emit_program_header(lc_test_send, &sink))
        return 1;
    memset(&sink, 0, sizeof(sink));

    got = leancam_gcode_run_program_line_ex("G72 W2 R1 X0.5 Z0.5 F120",
                                            setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X10 Z-25",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X40 Z-25 C0 R0",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X40 Z0",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G80",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK && !leancam_gcode_emit_program_footer_ex(lc_test_send, &sink, err, sizeof(err)))
        got = LC_GCODE_STREAM_REJECT;
    if (got != LC_GCODE_OK || !sink.saw_g72_rough)
    {
        printf("FAIL raw G72 rough result=%d err=%s rough=%d\n", (int)got, err, sink.saw_g72_rough);
        return 1;
    }
    {
        const char *g72_finish[] = {
            "G1 X10.000 Z-25.000 F45.000",
            "G1 X40.000 Z-25.000",
            "G1 X40.000 Z0.000"
        };
        if (lc_expect_basic_cycle_classes(&sink, "raw G72 rough", "(LC G72 region elements ", "(G72 rough ") ||
            lc_expect_finish_sequence(&sink, "raw G72 rough", g72_finish, 3))
            return 1;
    }

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    got = leancam_gcode_run_program_line_ex("G72 W1 R1 X0.5 Z0.5 F120",
                                            setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X50 Z0",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X50 Z-5 C0 R0",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X5 Z-5 C0 R0",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X5 Z2",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G80",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got != LC_GCODE_OK || lc_find_emitted_exact_from(&sink, 0, "G1 X5.500 F120.000") < 0)
    {
        printf("FAIL raw G72 rectangle result=%d err=%s\n", (int)got, err);
        return 1;
    }

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    if (!leancam_gcode_emit_program_header(lc_test_send, &sink))
        return 1;
    got = leancam_gcode_run_program_line_ex("G72 W2 R1 X0.5 Z0.5 F120",
                                            setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X10 Z-25",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G2 X40 Z-25 R20",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X40 Z0",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G80",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got != LC_GCODE_BAD_FIELD || strstr(err, "arc roughing unsupported") == NULL)
    {
        printf("FAIL raw G72 arc unsupported result=%d err=%s\n", (int)got, err);
        return 1;
    }

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    if (!leancam_gcode_emit_program_header(lc_test_send, &sink))
        return 1;
    got = leancam_gcode_run_program_line_ex("G71 U2 R1 X0.5 Z0.5 F120",
                                            setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X50 Z0",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X25 Z-25",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X40 Z-10",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G80",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK && leancam_gcode_emit_program_footer_ex(lc_test_send, &sink, err, sizeof(err)))
    {
        printf("FAIL raw G71 nonmonotonic accepted\n");
        return 1;
    }
    if (strstr(err, "non-monotonic in Z") == NULL)
    {
        printf("FAIL raw G71 nonmonotonic err=%s\n", err);
        return 1;
    }

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    if (!leancam_gcode_emit_program_header(lc_test_send, &sink))
        return 1;
    got = leancam_gcode_run_program_line_ex("G72 W2 R1 X0.5 Z0.5 F120",
                                            setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X10 Z-25",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X40 Z-25",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X20 Z0",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G80",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK && leancam_gcode_emit_program_footer_ex(lc_test_send, &sink, err, sizeof(err)))
    {
        printf("FAIL raw G72 nonmonotonic accepted\n");
        return 1;
    }
    if (strstr(err, "non-monotonic in X") == NULL)
    {
        printf("FAIL raw G72 nonmonotonic err=%s\n", err);
        return 1;
    }

    printf("PASS raw G7x roughing\n");
    return 0;
}

static int lc_run_g1_corner_rule_checks(const char *setup, const char *tool)
{
    lc_test_sink_t sink;
    lc_gcode_result_t got;
    char err[96];
    const char *chamfer_finish[] = {
        "G1 X50.000 Z0.000 F45.000",
        "G1 X50.000 Z-9.000",
        "G1 X48.000 Z-10.000",
        "G1 X40.000 Z-10.000",
        "G1 X40.000 Z-20.000"
    };
    const char *radius_finish[] = {
        "G1 X50.000 Z0.000 F45.000",
        "G1 X50.000 Z-9.000",
        "G3 X48.000 Z-10.000 I-1.000 K0.000",
        "G1 X40.000 Z-10.000",
        "G1 X40.000 Z-20.000"
    };

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    if (!leancam_gcode_emit_program_header(lc_test_send, &sink))
        return 1;
    got = leancam_gcode_run_program_line_ex("G71 U2 R1 X0.5 Z0.5 F120",
                                            setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X50 Z0",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X40 Z-10 C1 R1",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got != LC_GCODE_BAD_FIELD || strstr(err, "C and R exclusive") == NULL)
    {
        printf("FAIL G1 C/R exclusive result=%d err=%s\n", (int)got, err);
        return 1;
    }

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    if (!leancam_gcode_emit_program_header(lc_test_send, &sink))
        return 1;
    got = leancam_gcode_run_program_line_ex("G71 U2 R1 X0.5 Z0.5 F120",
                                            setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X50 Z0",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X50 Z-10 C1 R0",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X40 Z-10",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X40 Z-20",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G80",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK && !leancam_gcode_emit_program_footer_ex(lc_test_send, &sink, err, sizeof(err)))
        got = LC_GCODE_STREAM_REJECT;
    if (got != LC_GCODE_OK)
    {
        printf("FAIL G1 C chamfer result=%d err=%s\n", (int)got, err);
        return 1;
    }
    if (lc_expect_finish_sequence(&sink, "G1 C chamfer", chamfer_finish, 5))
        return 1;

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    if (!leancam_gcode_emit_program_header(lc_test_send, &sink))
        return 1;
    got = leancam_gcode_run_program_line_ex("G71 U2 R1 X0.5 Z0.5 F120",
                                            setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X50 Z0",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X50 Z-10 R1",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X40 Z-10",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X40 Z-20",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G80",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK && !leancam_gcode_emit_program_footer_ex(lc_test_send, &sink, err, sizeof(err)))
        got = LC_GCODE_STREAM_REJECT;
    if (got != LC_GCODE_OK)
    {
        printf("FAIL G1 R radius result=%d err=%s\n", (int)got, err);
        return 1;
    }
    if (lc_expect_finish_sequence(&sink, "G1 R radius", radius_finish, 5))
        return 1;

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    if (!leancam_gcode_emit_program_header(lc_test_send, &sink))
        return 1;
    got = leancam_gcode_run_program_line_ex("G71 U2 R1 X0.5 Z0.5 F120",
                                            setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X25 Z-12",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X25 Z-15 R5",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X50 Z-15",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G80",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK && leancam_gcode_emit_program_footer_ex(lc_test_send, &sink, err, sizeof(err)))
    {
        printf("FAIL G1 R too large accepted\n");
        return 1;
    }
    if (strstr(err, "bad corner geometry") == NULL)
    {
        printf("FAIL G1 R too large err=%s\n", err);
        return 1;
    }

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    if (!leancam_gcode_emit_program_header(lc_test_send, &sink))
        return 1;
    got = leancam_gcode_run_program_line_ex("G71 U2 R1 X0.5 Z0.5 F120",
                                            setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X20 Z-15",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X25 Z-15 R5",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X50 Z-15",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G80",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK && leancam_gcode_emit_program_footer_ex(lc_test_send, &sink, err, sizeof(err)))
    {
        printf("FAIL G1 R straight accepted\n");
        return 1;
    }
    if (strstr(err, "bad corner geometry") == NULL)
    {
        printf("FAIL G1 R straight err=%s\n", err);
        return 1;
    }

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    if (!leancam_gcode_emit_program_header(lc_test_send, &sink))
        return 1;
    got = leancam_gcode_run_program_line_ex("G71 U2 R1 X0.5 Z0.5 F120",
                                            setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X25 Z5",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X25 Z-5",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X35 Z-5 R5",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X35 Z-12 R5",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X50 Z-12",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G80",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK && leancam_gcode_emit_program_footer_ex(lc_test_send, &sink, err, sizeof(err)))
    {
        printf("FAIL adjacent G1 R overlap accepted\n");
        return 1;
    }
    if (strstr(err, "bad corner geometry") == NULL)
    {
        printf("FAIL adjacent G1 R overlap err=%s\n", err);
        return 1;
    }

    printf("PASS G1 C/R rules\n");
    return 0;
}

static int lc_run_g71_radius_boundary_check(const char *setup, const char *tool)
{
    lc_test_sink_t sink;
    lc_gcode_result_t got;
    char err[96];

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    if (!leancam_gcode_emit_program_header(lc_test_send, &sink))
        return 1;
    got = leancam_gcode_run_program_line_ex("G71 U2 R1 X0.5 Z0.5 F120",
                                            setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X25 Z5 C0 R0",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X25 Z-15 C0 R5",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G1 X50 Z-15",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK)
        got = leancam_gcode_run_program_line_ex("G80",
                                                setup, tool, lc_test_send, &sink, err, sizeof(err));
    if (got == LC_GCODE_OK && !leancam_gcode_emit_program_footer_ex(lc_test_send, &sink, err, sizeof(err)))
        got = LC_GCODE_STREAM_REJECT;

    if (got != LC_GCODE_OK)
    {
        printf("FAIL G71 radius boundary result=%d err=%s\n", (int)got, err);
        return 1;
    }
    if (lc_expect_no_diagonal_g71_rough(&sink, "G71 radius boundary"))
        return 1;
    if (lc_expect_g71_rough_z_near(&sink, "G71 radius boundary", 32.0f, -14.184f, 0.03f))
        return 1;
    if (lc_expect_g71_rough_z_near(&sink, "G71 radius boundary", 30.0f, -13.676f, 0.03f))
        return 1;
    if (lc_expect_g71_rough_z_near(&sink, "G71 radius boundary", 28.0f, -12.807f, 0.03f))
        return 1;
    if (lc_expect_g71_rough_z_near(&sink, "G71 radius boundary", 26.0f, -11.061f, 0.03f))
        return 1;

    printf("PASS G71 radius boundary\n");
    return 0;
}

int main(void)
{
    static const char *setup = "SETUP L80 OD40 ID10 CLAMP5 EXTRA2 CLR1";
    static const char *tool = "TOOL T3 R0.8 ORIENT3 R_FEED90 FIN_FEED45 DOC1.0 FIN_DOC0.2 RPM800 XOFF0 ZOFF0";
    int fails = 0;

    fails += lc_run_raw_g7x_rough_checks(setup, tool);
    fails += lc_run_g1_corner_rule_checks(setup, tool);
    fails += lc_run_g71_radius_boundary_check(setup, tool);
    fails += lc_run_g71_examples(setup, tool);

    if (fails)
    {
        printf("FAILURES %d\n", fails);
        return 1;
    }

    printf("ALL PASS\n");
    return 0;
}

