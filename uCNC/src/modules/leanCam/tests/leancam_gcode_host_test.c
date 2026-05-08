#include "../leancam_gcode.h"

#include <stdio.h>
#include <string.h>

typedef struct
{
    int lines;
    int fail_after;
    char first[128];
    char last[128];
    int saw_taper;
    int saw_radius;
    int saw_chamfer;
    int saw_thread;
    int saw_id_retract;
    int face_inner_seen;
    int saw_face_z_before_x;
    int saw_od_taper_safe;
    int saw_od_taper_oversafe;
    int saw_od_chamfer_clear;
    int saw_negative_doc_finish_stock;
    int id_radius_seen;
    int saw_id_corner_z_first;
    int saw_id_corner_x_before_z;
} lc_test_sink_t;

typedef struct
{
    const char *name;
    const char *line;
    const char *setup;
    const char *tool;
    lc_gcode_result_t want;
    const char *err_substr;
} lc_case_t;

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
    if (strstr(line, "X38.000") || strstr(line, "X14.000"))
        sink->saw_taper = 1;
    if (strstr(line, "G2 ") || strstr(line, "G3 "))
    {
        sink->saw_radius = 1;
        if (strstr(line, "G3 "))
            sink->id_radius_seen = 1;
    }
    if (strstr(line, "X36.000 Z-30.000") || strstr(line, "X16.000 Z-25.000"))
        sink->saw_chamfer = 1;
    if (strstr(line, "G33 "))
        sink->saw_thread = 1;
    if (strcmp(line, "G0 X41.000") == 0)
        sink->saw_od_taper_safe = 1;
    if (strcmp(line, "G0 X47.000") == 0)
        sink->saw_od_taper_oversafe = 1;
    if (strcmp(line, "G0 X46.000") == 0)
        sink->saw_od_chamfer_clear = 1;
    if (strstr(line, "(OD rough finish-stock 0.200)"))
        sink->saw_negative_doc_finish_stock = 1;
    if (sink->id_radius_seen && strcmp(line, "G0 Z1.000") == 0)
    {
        sink->saw_id_corner_z_first = 1;
        sink->id_radius_seen = 0;
    }
    else if (sink->id_radius_seen && strncmp(line, "G0 X", 4) == 0)
    {
        sink->saw_id_corner_x_before_z = 1;
        sink->id_radius_seen = 0;
    }
    if (strcmp(line, "G1 X17.000") == 0)
        sink->saw_id_retract = 1;
    if (strcmp(line, "G1 X10.000") == 0)
        sink->face_inner_seen = 1;
    else if (sink->face_inner_seen && strcmp(line, "G0 Z1.000") == 0)
        sink->saw_face_z_before_x = 1;
    else if (sink->face_inner_seen && strncmp(line, "G0 X", 4) == 0 && !sink->saw_face_z_before_x)
        sink->face_inner_seen = 0;

    sink->lines++;
    if (sink->fail_after > 0 && sink->lines > sink->fail_after)
        return 0;

    return 1;
}

static int lc_run_case(const lc_case_t *c)
{
    lc_test_sink_t sink;
    lc_gcode_result_t got;
    char err[96];

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;

    got = leancam_gcode_run_line_ex(c->line, c->setup, c->tool, lc_test_send, &sink, err, sizeof(err));
    if (got != c->want)
    {
        printf("FAIL %-28s result got %d want %d err=%s\n", c->name, (int)got, (int)c->want, err);
        return 1;
    }

    if (c->err_substr && !strstr(err, c->err_substr))
    {
        printf("FAIL %-28s err got '%s' want contains '%s'\n", c->name, err, c->err_substr);
        return 1;
    }

    if (got == LC_GCODE_OK && sink.lines <= 0)
    {
        printf("FAIL %-28s emitted no lines\n", c->name);
        return 1;
    }

    printf("PASS %-28s lines=%d first=%s\n", c->name, sink.lines, sink.first);
    return 0;
}

static int lc_run_shape_checks(const char *setup, const char *tool)
{
    lc_test_sink_t sink;
    lc_gcode_result_t got;
    char err[96];

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    got = leancam_gcode_run_line_ex("OD|D1{40}|DT{38}|Z1{0}|Z2{-30}|D2{32}|RND{2}|CLR{1}",
                                    setup,
                                    tool,
                                    lc_test_send,
                                    &sink,
                                    err,
                                    sizeof(err));
    if (got != LC_GCODE_OK || !sink.saw_taper || !sink.saw_radius)
    {
        printf("FAIL od taper radius result=%d err=%s taper=%d radius=%d\n",
               (int)got, err, sink.saw_taper, sink.saw_radius);
        return 1;
    }

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    got = leancam_gcode_run_line_ex("ID|D1{10}|DT{14}|Z1{0}|Z2{-25}|D2{20}|CHMF{2}|CLR{1}",
                                    setup,
                                    tool,
                                    lc_test_send,
                                    &sink,
                                    err,
                                    sizeof(err));
    if (got != LC_GCODE_OK || !sink.saw_taper || !sink.saw_chamfer)
    {
        printf("FAIL id taper chamfer result=%d err=%s taper=%d chamfer=%d\n",
               (int)got, err, sink.saw_taper, sink.saw_chamfer);
        return 1;
    }

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    got = leancam_gcode_run_line_ex("OD|D1{40}|DT{38}|Z1{0}|Z2{-30}|D2{32}|RND{2}|CHMF{1}|CLR{1}",
                                    setup,
                                    tool,
                                    lc_test_send,
                                    &sink,
                                    err,
                                    sizeof(err));
    if (got != LC_GCODE_BAD_FIELD || !strstr(err, "exclusive"))
    {
        printf("FAIL corner exclusive result=%d err=%s\n", (int)got, err);
        return 1;
    }

    printf("PASS taper/corner checks\n");
    return 0;
}

static int lc_run_id_return_checks(const char *setup, const char *tool)
{
    lc_test_sink_t sink;
    lc_gcode_result_t got;
    char err[96];

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    got = leancam_gcode_run_line_ex("ID|D1{10}|Z1{0}|Z2{-25}|D2{18}|CLR{1}",
                                    setup,
                                    tool,
                                    lc_test_send,
                                    &sink,
                                    err,
                                    sizeof(err));
    if (got != LC_GCODE_OK || !sink.saw_id_retract)
    {
        printf("FAIL id return retract result=%d err=%s retract=%d\n",
               (int)got, err, sink.saw_id_retract);
        return 1;
    }

    printf("PASS id return retract\n");
    return 0;
}

static int lc_run_face_return_checks(const char *setup, const char *tool)
{
    lc_test_sink_t sink;
    lc_gcode_result_t got;
    char err[96];

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    got = leancam_gcode_run_line_ex("FACE|D{40}|Z1{1}|Z{0}|DOC{0.5}|CLR{1}",
                                    setup,
                                    tool,
                                    lc_test_send,
                                    &sink,
                                    err,
                                    sizeof(err));
    if (got != LC_GCODE_OK || !sink.saw_face_z_before_x)
    {
        printf("FAIL face return retract result=%d err=%s saw=%d\n",
               (int)got, err, sink.saw_face_z_before_x);
        return 1;
    }

    printf("PASS face return retract\n");
    return 0;
}

static int lc_run_od_safe_x_checks(const char *setup, const char *tool)
{
    lc_test_sink_t sink;
    lc_gcode_result_t got;
    char err[96];

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    got = leancam_gcode_run_line_ex("OD|D1{40}|DT{38}|Z1{0}|Z2{-30}|D2{32}|CLR{1}",
                                    setup,
                                    tool,
                                    lc_test_send,
                                    &sink,
                                    err,
                                    sizeof(err));
    if (got != LC_GCODE_OK || !sink.saw_od_taper_safe || sink.saw_od_taper_oversafe)
    {
        printf("FAIL od safe x result=%d err=%s safe=%d oversafe=%d\n",
               (int)got, err, sink.saw_od_taper_safe, sink.saw_od_taper_oversafe);
        return 1;
    }

    printf("PASS od safe x\n");
    return 0;
}

static int lc_run_chamfer_retract_checks(const char *setup, const char *tool)
{
    lc_test_sink_t sink;
    lc_gcode_result_t got;
    char err[96];

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    got = leancam_gcode_run_line_ex("OD|D1{40}|DT{38}|Z1{0}|Z2{-30}|D2{32}|CHMF{2}|CLR{1}",
                                    setup,
                                    tool,
                                    lc_test_send,
                                    &sink,
                                    err,
                                    sizeof(err));
    if (got != LC_GCODE_OK || !sink.saw_od_chamfer_clear)
    {
        printf("FAIL od chamfer retract result=%d err=%s clear=%d\n",
               (int)got, err, sink.saw_od_chamfer_clear);
        return 1;
    }

    printf("PASS od chamfer retract\n");
    return 0;
}

static int lc_run_doc_sign_checks(const char *setup)
{
    static const char *tool_neg_doc = "TOOL|T{3}|D{6}|S{800}|R_FEED{90}|FIN_FEED{45}|R_DOC{-1.0}|FIN_DOC{-0.2}";
    lc_test_sink_t sink;
    lc_gcode_result_t got;
    char err[96];

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    got = leancam_gcode_run_line_ex("OD|D1{40}|Z1{0}|Z2{-30}|D2{32}|CLR{1}",
                                    setup,
                                    tool_neg_doc,
                                    lc_test_send,
                                    &sink,
                                    err,
                                    sizeof(err));
    if (got != LC_GCODE_OK || !sink.saw_negative_doc_finish_stock)
    {
        printf("FAIL doc sign result=%d err=%s finish=%d\n",
               (int)got, err, sink.saw_negative_doc_finish_stock);
        return 1;
    }

    printf("PASS doc sign\n");
    return 0;
}

static int lc_run_id_corner_retract_checks(const char *setup, const char *tool)
{
    lc_test_sink_t sink;
    lc_gcode_result_t got;
    char err[96];

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    got = leancam_gcode_run_line_ex("ID|D1{10}|DT{14}|Z1{0}|Z2{-25}|D2{20}|RND{2}|CLR{1}",
                                    setup,
                                    tool,
                                    lc_test_send,
                                    &sink,
                                    err,
                                    sizeof(err));
    if (got != LC_GCODE_OK || !sink.saw_id_corner_z_first || sink.saw_id_corner_x_before_z)
    {
        printf("FAIL id corner retract result=%d err=%s zfirst=%d xfirst=%d\n",
               (int)got, err, sink.saw_id_corner_z_first, sink.saw_id_corner_x_before_z);
        return 1;
    }

    printf("PASS id corner retract\n");
    return 0;
}

static int lc_run_program_mode_checks(const char *setup, const char *tool)
{
    lc_test_sink_t sink;
    lc_gcode_result_t got;
    char err[96];

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;

    if (!leancam_gcode_emit_program_header(lc_test_send, &sink))
    {
        printf("FAIL program header rejected\n");
        return 1;
    }

    if (strcmp(sink.first, "(LeanCam generated)") != 0 || strcmp(sink.last, "G7") != 0)
    {
        printf("FAIL program header first=%s last=%s\n", sink.first, sink.last);
        return 1;
    }

    memset(&sink, 0, sizeof(sink));
    got = leancam_gcode_run_program_line_ex("DRILL|Z1{0}|DEPTH{-10}|PECK{5}|FEED{90}|S{800}",
                                            setup,
                                            tool,
                                            lc_test_send,
                                            &sink,
                                            err,
                                            sizeof(err));
    if (got != LC_GCODE_OK)
    {
        printf("FAIL program drill result=%d err=%s\n", (int)got, err);
        return 1;
    }

    if (strcmp(sink.first, "G21") == 0 || strcmp(sink.last, "M5") == 0)
    {
        printf("FAIL program drill kept per-cycle wrapper first=%s last=%s\n", sink.first, sink.last);
        return 1;
    }

    memset(&sink, 0, sizeof(sink));
    if (!leancam_gcode_emit_program_footer(lc_test_send, &sink) ||
        strcmp(sink.first, "M5") != 0 ||
        strcmp(sink.last, "M30") != 0)
    {
        printf("FAIL program footer first=%s last=%s\n", sink.first, sink.last);
        return 1;
    }

    printf("PASS program mode wrappers\n");
    return 0;
}

static int lc_run_thread_checks(const char *setup, const char *tool)
{
    lc_test_sink_t sink;
    lc_gcode_result_t got;
    char err[96];

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    got = leancam_gcode_run_line_ex("THR_OD|M{20}|P{1.5}|Z1{0}|Z2{-12}|DOC{0.3}|N{4}|ST{1}|CLR{1}",
                                    setup,
                                    tool,
                                    lc_test_send,
                                    &sink,
                                    err,
                                    sizeof(err));
    if (got != LC_GCODE_OK || !sink.saw_thread)
    {
        printf("FAIL thread od result=%d err=%s saw=%d\n", (int)got, err, sink.saw_thread);
        return 1;
    }

    memset(&sink, 0, sizeof(sink));
    err[0] = 0;
    got = leancam_gcode_run_line_ex("THR_ID|M{12}|P{1.5}|Z1{0}|Z2{-12}|DOC{0.3}|N{4}|ST{0}|CLR{1}",
                                    setup,
                                    tool,
                                    lc_test_send,
                                    &sink,
                                    err,
                                    sizeof(err));
    if (got != LC_GCODE_OK || !sink.saw_thread)
    {
        printf("FAIL thread id result=%d err=%s saw=%d\n", (int)got, err, sink.saw_thread);
        return 1;
    }

    printf("PASS thread checks\n");
    return 0;
}

int main(void)
{
    static const char *setup = "SETUP|L{80}|OD{40}|ID{10}|CLAMP{5}|EXTRA{2}|CLR{1}|MAT{ST45}|WOFF{G54}";
    static const char *tool = "TOOL|T{3}|D{6}|S{800}|R_FEED{90}|FIN_FEED{45}|R_DOC{1.0}|FIN_DOC{0.2}";
    const lc_case_t cases[] = {
        {"od ok", "OD|D1{40}|Z1{0}|Z2{-30}|D2{32}|CLR{1}", setup, tool, LC_GCODE_OK, NULL},
        {"id ok", "ID|D1{10}|Z1{0}|Z2{-25}|D2{18}|CLR{1}", setup, tool, LC_GCODE_OK, NULL},
        {"face ok", "FACE|D{40}|Z1{1}|Z{0}|DOC{0.5}|CLR{1}", setup, tool, LC_GCODE_OK, NULL},
        {"drill ok", "DRILL|Z1{0}|DEPTH{-30}|PECK{5}|FEED{90}|S{800}", setup, tool, LC_GCODE_OK, NULL},
        {"cut ok", "CUT|D{0}|Z{-40}|WIDTH{3}|CLR{1}", setup, tool, LC_GCODE_OK, NULL},
        {"part ok", "PART|D{0}|Z{-40}|WIDTH{3}|CLR{1}", setup, tool, LC_GCODE_OK, NULL},
        {"groove ok", "GROOVE|D1{40}|D2{30}|Z1{-10}|Z2{-20}|WIDTH{3}|CLR{1}", setup, tool, LC_GCODE_OK, NULL},
        {"od no setup", "OD|D1{40}|Z1{0}|Z2{-30}|D2{32}|CLR{1}", NULL, tool, LC_GCODE_NO_SETUP, "no SETUP"},
        {"od bad number junk", "OD|D1{40abc}|Z1{0}|Z2{-30}|D2{32}|CLR{1}", setup, tool, LC_GCODE_BAD_FIELD, "D1"},
        {"od reversed z", "OD|D1{40}|Z1{-30}|Z2{0}|D2{32}|CLR{1}", setup, tool, LC_GCODE_BAD_FIELD, "Z2"},
        {"id reversed diameter", "ID|D1{20}|Z1{0}|Z2{-20}|D2{10}|CLR{1}", setup, tool, LC_GCODE_BAD_FIELD, "D2"},
        {"face bad doc", "FACE|D{40}|Z1{1}|Z{0}|DOC{0}|CLR{1}", setup, tool, LC_GCODE_BAD_FIELD, "DOC"},
        {"drill wrong direction", "DRILL|Z1{0}|DEPTH{0}|PECK{5}|FEED{90}", setup, tool, LC_GCODE_BAD_FIELD, "target"},
        {"drill too many pecks", "DRILL|Z1{0}|DEPTH{-30}|PECK{0.001}|FEED{90}", setup, tool, LC_GCODE_BAD_FIELD, "too many"},
        {"cut stock violation", "CUT|D{41}|Z{-40}|WIDTH{3}|CLR{1}", setup, tool, LC_GCODE_BAD_FIELD, "SETUP.OD"},
        {"groove reversed diameter", "GROOVE|D1{30}|D2{40}|Z1{-10}|Z2{-20}|WIDTH{3}|CLR{1}", setup, tool, LC_GCODE_BAD_FIELD, "D2"},
        {"unsupported", "TAP|Z1{0}|DEPTH{-20}|PITCH{1}|RPM{300}", setup, tool, LC_GCODE_UNSUPPORTED, "unsupported"},
        {"huge value rejected", "DRILL|Z1{0}|DEPTH{-10000000}|PECK{5}|FEED{90}", setup, tool, LC_GCODE_BAD_FIELD, "DEPTH"},
    };
    int fails = 0;
    unsigned i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        fails += lc_run_case(&cases[i]);

    fails += lc_run_shape_checks(setup, tool);
    fails += lc_run_id_return_checks(setup, tool);
    fails += lc_run_face_return_checks(setup, tool);
    fails += lc_run_od_safe_x_checks(setup, tool);
    fails += lc_run_chamfer_retract_checks(setup, tool);
    fails += lc_run_doc_sign_checks(setup);
    fails += lc_run_id_corner_retract_checks(setup, tool);
    fails += lc_run_program_mode_checks(setup, tool);
    fails += lc_run_thread_checks(setup, tool);

    if (fails)
    {
        printf("FAILURES %d\n", fails);
        return 1;
    }

    printf("ALL PASS\n");
    return 0;
}
