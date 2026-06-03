#include "dxf_lite_gcode.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int dxf_gc_emit(dxf_lite_gcode_emitter_t *e, const char *fmt, ...)
{
    char line[96];
    va_list ap;

    if (!e || !e->write)
        return 0;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    return e->write(e->user, line);
}

static void dxf_gc_header(dxf_lite_gcode_emitter_t *e)
{
    if (!e || e->header_emitted)
        return;
    (void)dxf_gc_emit(e, "G21");
    (void)dxf_gc_emit(e, "G90");
    (void)dxf_gc_emit(e, "G17");
    e->header_emitted = 1;
}

static double dxf_gc_x(dxf_lite_gcode_emitter_t *e, double x)
{
    return (x * e->x_scale) + e->x_offset;
}

static double dxf_gc_y(dxf_lite_gcode_emitter_t *e, double y)
{
    return (y * e->y_scale) + e->y_offset;
}

static void dxf_gc_rapid_to(dxf_lite_gcode_emitter_t *e, double x, double y)
{
    double mx = dxf_gc_x(e, x);
    double my = dxf_gc_y(e, y);

    dxf_gc_header(e);
    if (!e->have_pos || fabs(e->x - mx) > 1e-6 || fabs(e->y - my) > 1e-6) {
        (void)dxf_gc_emit(e, "G0 X%.6g Y%.6g", mx, my);
        e->x = mx;
        e->y = my;
        e->have_pos = 1;
    }
}

static void dxf_gc_cb_rapid(void *user, double x, double y)
{
    dxf_gc_rapid_to((dxf_lite_gcode_emitter_t *)user, x, y);
}

static void dxf_gc_cb_line(void *user, double x1, double y1, double x2, double y2)
{
    dxf_lite_gcode_emitter_t *e = (dxf_lite_gcode_emitter_t *)user;
    double mx2 = dxf_gc_x(e, x2);
    double my2 = dxf_gc_y(e, y2);

    dxf_gc_rapid_to(e, x1, y1);
    (void)dxf_gc_emit(e, "G1 X%.6g Y%.6g", mx2, my2);
    e->x = mx2;
    e->y = my2;
}

static void dxf_gc_cb_arc(void *user,
                          double x1, double y1,
                          double x2, double y2,
                          double cx, double cy,
                          int ccw)
{
    dxf_lite_gcode_emitter_t *e = (dxf_lite_gcode_emitter_t *)user;
    double mx1 = dxf_gc_x(e, x1);
    double my1 = dxf_gc_y(e, y1);
    double mx2 = dxf_gc_x(e, x2);
    double my2 = dxf_gc_y(e, y2);
    double mcx = dxf_gc_x(e, cx);
    double mcy = dxf_gc_y(e, cy);

    dxf_gc_rapid_to(e, x1, y1);
    (void)dxf_gc_emit(e, "%s X%.6g Y%.6g I%.6g J%.6g",
                      ccw ? "G3" : "G2",
                      mx2,
                      my2,
                      mcx - mx1,
                      mcy - my1);
    e->x = mx2;
    e->y = my2;
}

static void dxf_gc_cb_circle(void *user, double cx, double cy, double r)
{
    dxf_lite_gcode_emitter_t *e = (dxf_lite_gcode_emitter_t *)user;
    double x0 = cx + r;
    double y0 = cy;
    double x1 = cx - r;
    double y1 = cy;

    dxf_gc_rapid_to(e, x0, y0);
    dxf_gc_cb_arc(e, x0, y0, x1, y1, cx, cy, 1);
    dxf_gc_cb_arc(e, x1, y1, x0, y0, cx, cy, 1);
}

static void dxf_gc_cb_warning(void *user, const char *msg)
{
    (void)user;
    (void)msg;
}

static const dxf_lite_callbacks_t g_dxf_lite_gcode_callbacks = {
    dxf_gc_cb_rapid,
    dxf_gc_cb_line,
    dxf_gc_cb_arc,
    dxf_gc_cb_circle,
    dxf_gc_cb_warning
};

void dxf_lite_gcode_init(dxf_lite_gcode_emitter_t *emitter,
                         dxf_lite_gcode_write_fn write,
                         void *user)
{
    if (!emitter)
        return;
    memset(emitter, 0, sizeof(*emitter));
    emitter->write = write;
    emitter->user = user;
    emitter->x_scale = 1.0;
    emitter->y_scale = 1.0;
}

const dxf_lite_callbacks_t *dxf_lite_gcode_callbacks(void)
{
    return &g_dxf_lite_gcode_callbacks;
}

int dxf_lite_gcode_finish(dxf_lite_gcode_emitter_t *emitter)
{
    return emitter && emitter->write;
}
