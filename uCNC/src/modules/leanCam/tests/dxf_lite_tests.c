#include "../dxf_lite.h"
#include "../dxf_lite_gcode.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *text;
    size_t pos;
} str_reader_t;

typedef struct {
    int lines;
    int arcs;
    int circles;
    int warnings;
    double x1;
    double y1;
    double x2;
    double y2;
    double cx;
    double cy;
    int ccw;
} prim_sink_t;

typedef struct {
    char lines[64][96];
    int count;
} gcode_sink_t;

static int read_line(void *user, char *buf, size_t len)
{
    str_reader_t *r = (str_reader_t *)user;
    size_t n = 0;

    if (!r || !buf || len == 0 || !r->text[r->pos])
        return 0;
    while (r->text[r->pos] && r->text[r->pos] != '\n' && n + 1u < len)
        buf[n++] = r->text[r->pos++];
    while (r->text[r->pos] && r->text[r->pos] != '\n')
        r->pos++;
    if (r->text[r->pos] == '\n')
        r->pos++;
    buf[n] = 0;
    return 1;
}

static void cb_line(void *user, double x1, double y1, double x2, double y2)
{
    prim_sink_t *s = (prim_sink_t *)user;
    s->lines++;
    s->x1 = x1;
    s->y1 = y1;
    s->x2 = x2;
    s->y2 = y2;
}

static void cb_arc(void *user, double x1, double y1, double x2, double y2, double cx, double cy, int ccw)
{
    prim_sink_t *s = (prim_sink_t *)user;
    s->arcs++;
    s->x1 = x1;
    s->y1 = y1;
    s->x2 = x2;
    s->y2 = y2;
    s->cx = cx;
    s->cy = cy;
    s->ccw = ccw;
}

static void cb_circle(void *user, double cx, double cy, double r)
{
    prim_sink_t *s = (prim_sink_t *)user;
    s->circles++;
    s->cx = cx;
    s->cy = cy;
    s->x1 = r;
}

static void cb_warning(void *user, const char *msg)
{
    prim_sink_t *s = (prim_sink_t *)user;
    (void)msg;
    s->warnings++;
}

static int write_gcode(void *user, const char *line)
{
    gcode_sink_t *s = (gcode_sink_t *)user;
    if (s->count < 64) {
        strncpy(s->lines[s->count], line, sizeof(s->lines[s->count]) - 1u);
        s->lines[s->count][sizeof(s->lines[s->count]) - 1u] = 0;
        s->count++;
    }
    return 1;
}

static int near_d(double a, double b)
{
    return fabs(a - b) < 0.001;
}

static int parse_prims(const char *text, prim_sink_t *sink)
{
    dxf_lite_callbacks_t cb;
    dxf_lite_reader_t rd;
    str_reader_t sr;

    memset(sink, 0, sizeof(*sink));
    memset(&cb, 0, sizeof(cb));
    cb.line = cb_line;
    cb.arc = cb_arc;
    cb.circle = cb_circle;
    cb.warning = cb_warning;
    sr.text = text;
    sr.pos = 0;
    rd.read_line = read_line;
    rd.user = &sr;
    return dxf_lite_parse(&rd, &cb, sink);
}

static int parse_gcode(const char *text, gcode_sink_t *sink)
{
    dxf_lite_gcode_emitter_t emit;
    dxf_lite_reader_t rd;
    str_reader_t sr;

    memset(sink, 0, sizeof(*sink));
    dxf_lite_gcode_init(&emit, write_gcode, sink);
    sr.text = text;
    sr.pos = 0;
    rd.read_line = read_line;
    rd.user = &sr;
    return dxf_lite_parse(&rd, dxf_lite_gcode_callbacks(), &emit);
}

static int has_line(const gcode_sink_t *sink, const char *line)
{
    int i;
    for (i = 0; i < sink->count; ++i)
        if (strcmp(sink->lines[i], line) == 0)
            return 1;
    return 0;
}

static int test_line(void)
{
    const char *dxf =
        "0\nSECTION\n2\nENTITIES\n0\nLINE\n8\n0\n10\n0\n20\n0\n11\n10\n21\n5\n0\nENDSEC\n0\nEOF\n";
    prim_sink_t p;
    gcode_sink_t g;
    return parse_prims(dxf, &p) == DXF_LITE_OK &&
           p.lines == 1 && near_d(p.x1, 0) && near_d(p.y1, 0) &&
           near_d(p.x2, 10) && near_d(p.y2, 5) &&
           parse_gcode(dxf, &g) == DXF_LITE_OK &&
           has_line(&g, "G0 X0 Y0") &&
           has_line(&g, "G1 X10 Y5");
}

static int test_arc(void)
{
    const char *dxf =
        "0\nSECTION\n2\nENTITIES\n0\nARC\n10\n0\n20\n0\n40\n10\n50\n0\n51\n90\n0\nENDSEC\n0\nEOF\n";
    prim_sink_t p;
    gcode_sink_t g;
    return parse_prims(dxf, &p) == DXF_LITE_OK &&
           p.arcs == 1 && p.ccw && near_d(p.x1, 10) && near_d(p.y1, 0) &&
           near_d(p.x2, 0) && near_d(p.y2, 10) &&
           parse_gcode(dxf, &g) == DXF_LITE_OK &&
           has_line(&g, "G3 X6.12323e-16 Y10 I-10 J0");
}

static int test_circle(void)
{
    const char *dxf =
        "0\nSECTION\n2\nENTITIES\n0\nCIRCLE\n10\n5\n20\n5\n40\n2\n0\nENDSEC\n0\nEOF\n";
    prim_sink_t p;
    gcode_sink_t g;
    return parse_prims(dxf, &p) == DXF_LITE_OK &&
           p.circles == 1 && near_d(p.cx, 5) && near_d(p.cy, 5) && near_d(p.x1, 2) &&
           parse_gcode(dxf, &g) == DXF_LITE_OK &&
           has_line(&g, "G0 X7 Y5") &&
           has_line(&g, "G3 X3 Y5 I-2 J0") &&
           has_line(&g, "G3 X7 Y5 I2 J0");
}

static int test_lwpoly_open_closed(void)
{
    const char *open =
        "0\nSECTION\n2\nENTITIES\n0\nLWPOLYLINE\n90\n4\n70\n0\n10\n0\n20\n0\n10\n10\n20\n0\n10\n10\n20\n5\n10\n0\n20\n5\n0\nENDSEC\n0\nEOF\n";
    const char *closed =
        "0\nSECTION\n2\nENTITIES\n0\nLWPOLYLINE\n90\n4\n70\n1\n10\n0\n20\n0\n10\n10\n20\n0\n10\n10\n20\n5\n10\n0\n20\n5\n0\nENDSEC\n0\nEOF\n";
    prim_sink_t p;
    return parse_prims(open, &p) == DXF_LITE_OK && p.lines == 3 &&
           parse_prims(closed, &p) == DXF_LITE_OK && p.lines == 4;
}

static int test_bulge(void)
{
    const char *dxf =
        "0\nSECTION\n2\nENTITIES\n0\nLWPOLYLINE\n90\n2\n70\n0\n10\n0\n20\n0\n42\n1\n10\n10\n20\n0\n0\nENDSEC\n0\nEOF\n";
    prim_sink_t p;
    return parse_prims(dxf, &p) == DXF_LITE_OK &&
           p.arcs == 1 && p.lines == 0 && p.ccw &&
           near_d(p.x1, 0) && near_d(p.y1, 0) &&
           near_d(p.x2, 10) && near_d(p.y2, 0) &&
           near_d(p.cx, 5) && near_d(p.cy, 0);
}

static int test_old_polyline(void)
{
    const char *dxf =
        "0\nSECTION\n2\nENTITIES\n0\nPOLYLINE\n70\n1\n0\nVERTEX\n10\n0\n20\n0\n0\nVERTEX\n10\n10\n20\n0\n0\nVERTEX\n10\n10\n20\n5\n0\nVERTEX\n10\n0\n20\n5\n0\nSEQEND\n0\nENDSEC\n0\nEOF\n";
    prim_sink_t p;
    return parse_prims(dxf, &p) == DXF_LITE_OK && p.lines == 4;
}

static int test_ignore_and_malformed(void)
{
    const char *ok =
        "0\nSECTION\n2\nENTITIES\n0\nTEXT\n1\nhello\n0\nINSERT\n2\nx\n0\nSPLINE\n0\nLINE\n10\n1\n20\n2\n11\n3\n21\n4\n0\nENDSEC\n0\nEOF\n";
    const char *bad =
        "0\nSECTION\n2\nENTITIES\n0\nCIRCLE\n10\n0\n20\n0\n0\nENDSEC\n0\nEOF\n";
    prim_sink_t p;
    return parse_prims(ok, &p) == DXF_LITE_OK && p.lines == 1 && p.warnings >= 3 &&
           parse_prims(bad, &p) == DXF_LITE_ERR_MALFORMED;
}

int main(void)
{
    int fails = 0;
    fails += !test_line();
    fails += !test_arc();
    fails += !test_circle();
    fails += !test_lwpoly_open_closed();
    fails += !test_bulge();
    fails += !test_old_polyline();
    fails += !test_ignore_and_malformed();
    if (fails) {
        printf("dxf_lite_tests failed: %d\n", fails);
        return 1;
    }
    printf("dxf_lite_tests passed\n");
    return 0;
}
