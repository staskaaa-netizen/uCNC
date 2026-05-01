#include "../leancam_gcode.h"

#include <stdio.h>
#include <string.h>

typedef struct
{
    int lines;
    int fail_after;
    char first[128];
    char last[128];
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

    if (strcmp(sink.first, "(LeanCam generated)") != 0 || strcmp(sink.last, "G8") != 0)
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

    fails += lc_run_program_mode_checks(setup, tool);

    if (fails)
    {
        printf("FAILURES %d\n", fails);
        return 1;
    }

    printf("ALL PASS\n");
    return 0;
}
