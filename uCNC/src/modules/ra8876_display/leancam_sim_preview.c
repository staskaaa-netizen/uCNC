#include "leancam_sim_preview.h"
#include "ra8876_ll.h"
#include "../../cnc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define SIM_AREA_X0          500
#define SIM_AREA_Y0          180
#define SIM_AREA_X1         1023
#define SIM_AREA_Y1          529
#define SIM_AREA_W          (SIM_AREA_X1 - SIM_AREA_X0 + 1)
#define SIM_AREA_H          (SIM_AREA_Y1 - SIM_AREA_Y0 + 1)

#define SIM_LIVE_X0            0
#define SIM_LIVE_Y0            0
#define SIM_LIVE_X1         1023
#define SIM_LIVE_Y1          599
#define SIM_LIVE_W          (SIM_LIVE_X1 - SIM_LIVE_X0 + 1)
#define SIM_LIVE_H          (SIM_LIVE_Y1 - SIM_LIVE_Y0 + 1)
#define SIM_LIVE_STOCK_TOP    92
#define SIM_LIVE_RIGHT_MARGIN 76
#define SIM_LIVE_BOTTOM_MARGIN 86
#define SIM_LIVE_LEFT_MARGIN  50
#define SIM_LIVE_ADDR        RA_PAGE_BASE(2)
#define SIM_LIVE_TOOL_MIN      2
#define SIM_LIVE_TOOL_MAX     36
#define SIM_LIVE_REMOVE_MAX  180
#define SIM_LIVE_REMOVE_BIAS   2
#define SIM_LIVE_TEXT_X        18
#define SIM_LIVE_STATUS_Y1      8
#define SIM_LIVE_STATUS_Y2     42
#define SIM_LIVE_TEXT_H        82
#define SIM_LIVE_RT_X_RADIUS   1
#define SIM_LIVE_DOC_DIAM_FACTOR 1.0f
#define SIM_LIVE_DOC_Z_FACTOR  1.0f
#define SIM_TOOL_ADDR          RA_PAGE_BASE(3)

#ifndef RA8876_LCAM_LIVE_DEPTH_GRADIENT
#define RA8876_LCAM_LIVE_DEPTH_GRADIENT 0
#endif

#define SIM_STOCK_TOP        200
#define SIM_RIGHT_MARGIN      70
#define SIM_BOTTOM_MARGIN     80
#define SIM_REDRAW_MS        250
#define SIM_AXIS_EXT          30

#define SIM_COL_BG       RA_BLACK
#define SIM_COL_STOCK    RA_GRAY
#define SIM_COL_STOCK_2  0x528A
#define SIM_COL_FRAME    RA_GRAY
#define SIM_COL_AXIS     RA_WHITE
#define SIM_COL_DIM      RA_CYAN
#define SIM_COL_HI       RA_YELLOW
#define SIM_COL_CHUCK    RA_BLUE
#define SIM_COL_CHUCK_FILL 0xC618
#define SIM_COL_REMOVED  SIM_COL_BG
#define SIM_COL_TOOL     RA_RED
#define SIM_COL_WARN     RA_YELLOW

#define SIM_LC_MODE_PROGRAM  2u
#define SIM_LC_MODE_DRAFT    3u

#define SIM_TILE_H           12
#define SIM_CHUCK_H          30

#define SIM_LABEL_Z1_DX     -48
#define SIM_LABEL_Z1_DY      -30
#define SIM_LABEL_D1_DX     -48
#define SIM_LABEL_D1_DY       -2
#define SIM_LABEL_Z2_DX     -48
#define SIM_LABEL_Z2_DY      -30
#define SIM_LABEL_D2_DX     -48
#define SIM_LABEL_D2_DY       -2
#define SIM_LABEL_EXTRA_DX     6
#define SIM_LABEL_EXTRA_DY    16

typedef enum
{
    SIM_CORNER_NONE = 0,
    SIM_CORNER_RND,
    SIM_CORNER_CHMF
} sim_corner_kind_t;

typedef struct
{
    float d1;
    float dt;
    float d2;
    float z1;
    float z2;
    float z_profile_end;
    float d_profile_end;
    float d_corner_end;
    float arc_i;
    float arc_k;
    float amount;
    sim_corner_kind_t corner;
} sim_turn_shape_t;

typedef struct
{
    float length;
    float od;
    float id;
    float clamp;
    float extra;
} sim_setup_t;

static int sim_clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float sim_absf(float v)
{
    return (v < 0.0f) ? -v : v;
}

/*
 * LeanCam stores/programs X values as diameters.
 * The lathe preview/live view is a top half-section, so vertical drawing uses
 * radius height. Keep this conversion centralized; callers should pass
 * diameter values to sim_d_to_py()/sim_live_d_to_py_raw().
 */
static float sim_diam_to_draw_radius(float d)
{
    return d * 0.5f;
}

static int sim_diam_len_to_px(const lcam_sim_view_t *view, float d)
{
    if (!view)
        return 0;
    return (int)(sim_diam_to_draw_radius(d) * view->d_scale + 0.5f);
}

static int sim_radius_len_to_px(const lcam_sim_view_t *view, float r)
{
    if (!view)
        return 0;
    return (int)(r * view->d_scale + 0.5f);
}

static bool sim_get_field_text(const char *line, const char *name, char *out, unsigned out_len)
{
    const char *p;
    unsigned name_len;

    if (!line || !name || !out || out_len == 0)
        return false;

    out[0] = 0;
    name_len = (unsigned)strlen(name);
    p = line;
    if (p[0] == '>' && p[1] == ' ')
        p += 2;

    while ((p = strstr(p, name)) != NULL)
    {
        const char *open;
        const char *close;
        unsigned n;

        if ((p == line || p[-1] == '|' || (p >= line + 2 && p[-1] == ' ' && p[-2] == '>')) &&
            p[name_len] == '{')
        {
            open = p + name_len;
            close = strchr(open + 1, '}');
            if (!close)
                return false;

            n = (unsigned)(close - open - 1);
            if (n >= out_len)
                n = out_len - 1;

            memcpy(out, open + 1, n);
            out[n] = 0;
            return true;
        }

        p += name_len;
    }

    return false;
}

static bool sim_field_float(const char *line, const char *name, float *out)
{
    char buf[32];
    char *s;
    char *endp;

    if (!sim_get_field_text(line, name, buf, sizeof(buf)))
        return false;

    s = buf;
    while (*s == ' ') s++;
    if (*s == '(') s++;
    if (*s == '*' || *s == 0)
        return false;

    *out = (float)strtod(s, &endp);
    return endp != s;
}

static bool sim_field_float2(const char *line, const char *a, const char *b, float *out)
{
    return sim_field_float(line, a, out) || sim_field_float(line, b, out);
}

static bool sim_field_float3(const char *line, const char *a, const char *b, const char *c, float *out)
{
    return sim_field_float(line, a, out) || sim_field_float(line, b, out) || sim_field_float(line, c, out);
}

static bool sim_field_uint(const char *line, const char *name, unsigned *out)
{
    char buf[16];
    char *s;
    char *endp;
    unsigned long v;

    if (!out || !sim_get_field_text(line, name, buf, sizeof(buf)))
        return false;

    s = buf;
    while (*s == ' ') s++;
    if (*s == '(') s++;
    if (*s == '*' || *s == 0)
        return false;

    v = strtoul(s, &endp, 10);
    if (endp == s)
        return false;

    *out = (unsigned)v;
    return true;
}

static bool sim_field_tool_diameter(const char *line, float *out)
{
    return sim_field_float3(line, "TD", "TOOL_DIAMETER", "TOOL_DIA", out) ||
           sim_field_float3(line, "DIA", "DIAMETER", "D", out);
}

static void sim_read_setup(const ui_snapshot_frame_t *frame, sim_setup_t *setup)
{
    const char *line = frame ? frame->leancam_setup_line : NULL;

    setup->length = 100.0f;
    setup->od = 50.0f;
    setup->id = 0.0f;
    setup->clamp = 0.0f;
    setup->extra = 0.0f;

    if (!line || !line[0])
        return;

    (void)sim_field_float2(line, "L", "LENGTH", &setup->length);
    (void)sim_field_float2(line, "OD", "OUTER_DIAMETER", &setup->od);
    (void)sim_field_float2(line, "ID", "INNER_DIAMETER", &setup->id);
    (void)sim_field_float2(line, "CLAMP", "CLAMP_LENGTH", &setup->clamp);
    (void)sim_field_float2(line, "EXTRA", "EXTRA_LENGTH", &setup->extra);

    if (setup->length != setup->length || setup->length <= 0.0f) setup->length = 100.0f;
    if (setup->od != setup->od || setup->od <= 0.0f) setup->od = 50.0f;
    if (setup->id != setup->id || setup->id < 0.0f) setup->id = 0.0f;
    if (setup->clamp != setup->clamp || setup->clamp < 0.0f) setup->clamp = 0.0f;
    if (setup->extra != setup->extra || setup->extra < 0.0f) setup->extra = 0.0f;
}

static void sim_build_view(const sim_setup_t *setup, lcam_sim_view_t *view)
{
    float visible_len;
    float z_scale;
    float d_scale;
    float fit_scale;
    int stock_h;
    int stock_w;
    int usable_w;
    int usable_h;

    visible_len = setup->length + setup->extra;
    if (visible_len <= 0.0f)
        visible_len = 100.0f;

    usable_w = (SIM_AREA_X1 - SIM_RIGHT_MARGIN) - SIM_AREA_X0;
    usable_h = (SIM_AREA_Y1 - SIM_BOTTOM_MARGIN) - SIM_STOCK_TOP;
    if (usable_w < 1) usable_w = 1;
    if (usable_h < 1) usable_h = 1;

    z_scale = (float)usable_w / visible_len;
    d_scale = (float)usable_h / sim_diam_to_draw_radius(setup->od);
    if (z_scale <= 0.0f) z_scale = 1.0f;
    if (d_scale <= 0.0f) d_scale = 1.0f;
    fit_scale = (z_scale < d_scale) ? z_scale : d_scale;
    if (fit_scale != fit_scale || fit_scale <= 0.0f)
        fit_scale = 1.0f;

    stock_w = (int)(visible_len * fit_scale + 0.5f);
    stock_h = (int)(sim_diam_to_draw_radius(setup->od) * fit_scale + 0.5f);
    if (stock_w < 1) stock_w = 1;
    if (stock_h < 1) stock_h = 1;
    if (stock_w > usable_w) stock_w = usable_w;
    if (stock_h > usable_h) stock_h = usable_h;

    view->x0 = SIM_AREA_X0;
    view->y0 = SIM_AREA_Y0;
    view->x1 = SIM_AREA_X1;
    view->y1 = SIM_AREA_Y1;
    view->stock_right = SIM_AREA_X1 - SIM_RIGHT_MARGIN;
    view->stock_left = view->stock_right - stock_w;
    if (view->stock_left < SIM_AREA_X0)
        view->stock_left = SIM_AREA_X0;
    view->stock_top = SIM_STOCK_TOP;
    view->stock_bottom = SIM_STOCK_TOP + stock_h;
    if (view->stock_bottom > SIM_AREA_Y1 - SIM_BOTTOM_MARGIN)
        view->stock_bottom = SIM_AREA_Y1 - SIM_BOTTOM_MARGIN;
    view->z_zero_x = sim_clampi(view->stock_left + (int)(setup->length * fit_scale + 0.5f),
                                view->stock_left,
                                view->stock_right);

    view->z_scale = fit_scale;
    view->d_scale = fit_scale;
    view->stock_len = visible_len;
    view->stock_od = setup->od;
}

static void sim_build_live_view(const sim_setup_t *setup, lcam_sim_view_t *view)
{
    float visible_len;
    float z_scale;
    float d_scale;
    float fit_scale;
    int stock_h;
    int stock_w;
    int usable_w;
    int usable_h;

    visible_len = setup->length + setup->extra;
    if (visible_len <= 0.0f)
        visible_len = 100.0f;

    usable_w = (SIM_LIVE_X1 - SIM_LIVE_RIGHT_MARGIN) - (SIM_LIVE_X0 + SIM_LIVE_LEFT_MARGIN);
    usable_h = (SIM_LIVE_Y1 - SIM_LIVE_BOTTOM_MARGIN) - SIM_LIVE_STOCK_TOP;
    if (usable_w < 1) usable_w = 1;
    if (usable_h < 1) usable_h = 1;

    z_scale = (float)usable_w / visible_len;
    d_scale = (float)usable_h / sim_diam_to_draw_radius(setup->od);
    if (z_scale <= 0.0f) z_scale = 1.0f;
    if (d_scale <= 0.0f) d_scale = 1.0f;
    fit_scale = (z_scale < d_scale) ? z_scale : d_scale;
    if (fit_scale != fit_scale || fit_scale <= 0.0f)
        fit_scale = 1.0f;

    stock_w = (int)(visible_len * fit_scale + 0.5f);
    stock_h = (int)(sim_diam_to_draw_radius(setup->od) * fit_scale + 0.5f);
    if (stock_w < 1) stock_w = 1;
    if (stock_h < 1) stock_h = 1;
    if (stock_w > usable_w) stock_w = usable_w;
    if (stock_h > usable_h) stock_h = usable_h;

    view->x0 = SIM_LIVE_X0;
    view->y0 = SIM_LIVE_Y0;
    view->x1 = SIM_LIVE_X1;
    view->y1 = SIM_LIVE_Y1;
    view->stock_left = SIM_LIVE_X0 + SIM_LIVE_LEFT_MARGIN;
    view->stock_right = view->stock_left + stock_w;
    if (view->stock_right > SIM_LIVE_X1 - SIM_LIVE_RIGHT_MARGIN)
        view->stock_right = SIM_LIVE_X1 - SIM_LIVE_RIGHT_MARGIN;
    view->stock_top = SIM_LIVE_STOCK_TOP;
    view->stock_bottom = SIM_LIVE_STOCK_TOP + stock_h;
    if (view->stock_bottom > SIM_LIVE_Y1 - SIM_LIVE_BOTTOM_MARGIN)
        view->stock_bottom = SIM_LIVE_Y1 - SIM_LIVE_BOTTOM_MARGIN;
    view->z_zero_x = sim_clampi(view->stock_left + (int)(setup->length * fit_scale + 0.5f),
                                view->stock_left,
                                view->stock_right);

    view->z_scale = fit_scale;
    view->d_scale = fit_scale;
    view->stock_len = visible_len;
    view->stock_od = setup->od;
}

int sim_z_to_px(const lcam_sim_view_t *view, float z)
{
    int x;

    if (!view)
        return 0;

    x = view->z_zero_x + (int)(z * view->z_scale + ((z >= 0.0f) ? 0.5f : -0.5f));
    return sim_clampi(x, view->stock_left, view->stock_right);
}

int sim_d_to_py(const lcam_sim_view_t *view, float d)
{
    int y;

    if (!view)
        return 0;

    y = view->stock_top + sim_diam_len_to_px(view, d);
    return sim_clampi(y, view->stock_top, view->stock_bottom);
}

static int sim_live_z_to_px_raw(const lcam_sim_view_t *view, float z)
{
    if (!view)
        return 0;

    return view->z_zero_x + (int)(z * view->z_scale + ((z >= 0.0f) ? 0.5f : -0.5f));
}

static int sim_live_d_to_py_raw(const lcam_sim_view_t *view, float d)
{
    if (!view)
        return 0;

    return view->stock_top + sim_diam_len_to_px(view, d);
}

static void sim_textf(int x, int y, uint16_t fg, uint16_t bg, const char *fmt, ...)
{
    char buf[64];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ra_text((uint16_t)x, (uint16_t)y, fg, bg, buf);
}

static void sim_draw_stock(const lcam_sim_view_t *view, const sim_setup_t *setup)
{
    int z0_x;
    int y;
    int bottom_label_y;

    ra_fill_rect((uint16_t)view->x0, (uint16_t)view->y0, (uint16_t)view->x1, (uint16_t)view->y1, SIM_COL_BG);

    for (y = view->stock_top; y <= view->stock_bottom; y += SIM_TILE_H)
    {
        int y2 = y + SIM_TILE_H - 1;
        if (y2 > view->stock_bottom)
            y2 = view->stock_bottom;

        ra_fill_rect((uint16_t)view->stock_left,
                     (uint16_t)y,
                     (uint16_t)view->stock_right,
                     (uint16_t)y2,
                     SIM_COL_STOCK);
    }

    z0_x = view->z_zero_x;

    ra_draw_line(view->stock_left - SIM_AXIS_EXT,
                 view->stock_top,
                 view->stock_right + SIM_AXIS_EXT,
                 view->stock_top,
                 SIM_COL_AXIS);
    ra_draw_line(z0_x,
                 view->stock_top - SIM_AXIS_EXT,
                 z0_x,
                 view->stock_bottom + SIM_AXIS_EXT,
                 SIM_COL_AXIS);

    sim_textf(z0_x - 18,
              sim_clampi(view->stock_top - 30, view->y0 + 2, view->y1 - 18),
              SIM_COL_DIM,
              SIM_COL_BG,
              "Z0");
    sim_textf(view->stock_left + 6, view->stock_top - 30, SIM_COL_DIM, SIM_COL_BG, "Z-%.0f", (double)view->stock_len);
    sim_textf(view->stock_right + 6, view->stock_top - 6, SIM_COL_DIM, SIM_COL_BG, "X0");
    if (setup->extra > 0.0f)
        sim_textf(z0_x + 5, view->stock_top - 60, SIM_COL_DIM, SIM_COL_BG, "EX %.1f", (double)setup->extra);

    bottom_label_y = view->stock_bottom + SIM_CHUCK_H + 10;
    bottom_label_y = sim_clampi(bottom_label_y, view->y0 + 2, view->y1 - 18);
    sim_textf(view->stock_left + 6, bottom_label_y, SIM_COL_DIM, SIM_COL_BG, "L %.1f", (double)setup->length);
    sim_textf(view->stock_left + 86, bottom_label_y, SIM_COL_DIM, SIM_COL_BG, "OD %.1f", (double)setup->od);
}

static void sim_draw_chuck(const lcam_sim_view_t *view, const sim_setup_t *setup)
{
    int chuck_w;
    int jaw_h;
    int x1;
    int y1;
    int y2;

    if (setup->clamp <= 0.0f)
        return;

    chuck_w = (int)(setup->clamp * view->z_scale + 0.5f);
    //chuck_w = sim_clampi(chuck_w, 10, 70);
    jaw_h = SIM_CHUCK_H;
    x1 = view->stock_left;

    y1 = view->stock_bottom + 1;
    y2 = view->stock_bottom + jaw_h;
    y2 = sim_clampi(y2, view->y0 + 1, view->y1 - 1);

    ra_fill_rect((uint16_t)x1, (uint16_t)y1, (uint16_t)(x1 + chuck_w), (uint16_t)y2, SIM_COL_CHUCK_FILL);
    sim_textf(x1 + 3, sim_clampi(y1 + 9, view->y0 + 2, view->y1 - 18), RA_BLACK, SIM_COL_CHUCK_FILL, "CL %.1f", (double)setup->clamp);
}

static void sim_draw_hatch_rect(int x1, int y1, int x2, int y2, int spacing, bool vertical)
{
    int p;

    if (spacing < 2) spacing = 2;
    if (spacing > 12) spacing = 12;

    ra_fill_rect((uint16_t)x1, (uint16_t)y1, (uint16_t)x2, (uint16_t)y2, SIM_COL_HI);
    if (vertical)
    {
        for (p = x1 + spacing; p < x2; p += spacing)
            ra_draw_line(p, y1, p, y2, RA_BLACK);
    }
    else
    {
        for (p = y1 + spacing; p < y2; p += spacing)
            ra_draw_line(x1, p, x2, p, RA_BLACK);
    }
}

static void sim_draw_hatch_vline(int x, int y1, int y2, int spacing)
{
    int tmp;

    (void)spacing;

    if (y2 < y1) { tmp = y1; y1 = y2; y2 = tmp; }
    if (y2 <= y1)
        return;

    ra_draw_line(x, y1, x, y2, SIM_COL_HI);
}

static bool sim_is_cycle(const char *line, const char *name)
{
    size_t n;

    if (!line || !name)
        return false;
    if (line[0] == '>' && line[1] == ' ')
        line += 2;

    n = strlen(name);
    return strncmp(line, name, n) == 0 && line[n] == '|';
}

static bool sim_should_draw_for_mode(uint8_t mode)
{
    return mode == SIM_LC_MODE_PROGRAM || mode == SIM_LC_MODE_DRAFT;
}

static void sim_begin_offscreen(uint32_t *old_draw_base)
{
    if (old_draw_base)
        *old_draw_base = ra_get_draw_base();

    ra_set_draw_base(RA8876_BACKBUF_ADDR);
}

static void sim_present_offscreen(uint32_t old_draw_base)
{
    ra_set_draw_base(old_draw_base);
    ra_blit(RA8876_BACKBUF_ADDR,
            SIM_AREA_X0,
            SIM_AREA_Y0,
            old_draw_base,
            SIM_AREA_X0,
            SIM_AREA_Y0,
            SIM_AREA_W,
            SIM_AREA_H);
}

static void sim_draw_status_message(const ui_snapshot_frame_t *frame)
{
    const char *msg = (frame && frame->leancam_message[0]) ? frame->leancam_message : "LC: ready";

    ra_fill_rect(SIM_AREA_X0, SIM_AREA_Y0, SIM_AREA_X1, SIM_AREA_Y1, SIM_COL_BG);
    sim_textf(SIM_AREA_X0 + 20, SIM_AREA_Y0 + 30, SIM_COL_DIM, SIM_COL_BG, "G-code generator");
    sim_textf(SIM_AREA_X0 + 20, SIM_AREA_Y0 + 60, SIM_COL_AXIS, SIM_COL_BG, "%-48.48s", msg);
}

static bool sim_preview_sources_changed(const ui_snapshot_frame_t *frame,
                                        char *last_setup,
                                        char *last_line,
                                        char *last_tool,
                                        char *last_active_field)
{
    if (!frame || !last_setup || !last_line || !last_tool || !last_active_field)
        return false;

    if (strcmp(last_setup, frame->leancam_setup_line) == 0 &&
        strcmp(last_line, frame->leancam_preview_line) == 0 &&
        strcmp(last_tool, frame->leancam_tool_line) == 0 &&
        strcmp(last_active_field, frame->leancam_active_field) == 0)
        return false;

    return true;
}

static void sim_store_preview_sources(const ui_snapshot_frame_t *frame,
                                      char *last_setup,
                                      char *last_line,
                                      char *last_tool,
                                      char *last_active_field)
{
    if (!frame || !last_setup || !last_line || !last_tool || !last_active_field)
        return;

    ui_snapshot_strcpy(last_setup, frame->leancam_setup_line, UI_LC_LINE_LEN);
    ui_snapshot_strcpy(last_line, frame->leancam_preview_line, UI_LC_LINE_LEN);
    ui_snapshot_strcpy(last_tool, frame->leancam_tool_line, UI_LC_LINE_LEN);
    ui_snapshot_strcpy(last_active_field, frame->leancam_active_field, 12);
}

static void sim_draw_corner_label(const lcam_sim_view_t *view, int x, int y, const char *label, int dx, int dy)
{
    int label_w = label ? ((int)strlen(label) * 8) : 0;
    int tx = sim_clampi(x + dx, view->x0 + 2, view->x1 - label_w - 2);
    int ty = sim_clampi(y + dy, view->y0 + 2, view->y1 - 18);

    sim_textf(tx, ty, SIM_COL_DIM, SIM_COL_BG, "%s", label ? label : "");
}

static bool sim_active_field_is(const ui_snapshot_frame_t *frame, const char *name)
{
    if (!frame || !name || !frame->leancam_active_field[0])
        return false;

    if (strcmp(frame->leancam_active_field, name) == 0)
        return true;

    if ((strcmp(name, "OD") == 0 || strcmp(name, "ID") == 0) &&
        (strcmp(frame->leancam_active_field, "M") == 0 ||
         strcmp(frame->leancam_active_field, "D") == 0 ||
         strcmp(frame->leancam_active_field, "D1") == 0))
        return true;

    return false;
}

static void sim_draw_corner_value_label(const lcam_sim_view_t *view,
                                        const ui_snapshot_frame_t *frame,
                                        int x,
                                        int y,
                                        const char *name,
                                        float value,
                                        int dx,
                                        int dy)
{
    char value_text[16];
    int name_w;
    int value_w;
    int tx;
    int ty;

    if (!view || !name)
        return;

    snprintf(value_text, sizeof(value_text), "%.2f", (double)value);
    name_w = ((int)strlen(name) + 1) * 8;
    value_w = (int)strlen(value_text) * 8;
    tx = sim_clampi(x + dx, view->x0 + 2, view->x1 - name_w - value_w - 2);
    ty = sim_clampi(y + dy, view->y0 + 2, view->y1 - 18);

    if (sim_active_field_is(frame, name))
    {
        sim_textf(tx + name_w, ty, RA_BLACK, RA_WHITE, "%s", value_text);
        sim_textf(tx, ty, SIM_COL_DIM, SIM_COL_BG, "%s ", name);
    }
    else
    {
        sim_textf(tx, ty, SIM_COL_DIM, SIM_COL_BG, "%s ", name);
        sim_textf(tx + name_w, ty, SIM_COL_DIM, SIM_COL_BG, "%s", value_text);
    }
}

static void sim_draw_value_label_at(const lcam_sim_view_t *view,
                                    const ui_snapshot_frame_t *frame,
                                    int x,
                                    int y,
                                    const char *name,
                                    float value)
{
    char value_text[16];
    int name_w;
    int value_w;
    int tx;
    int ty;

    if (!view || !name)
        return;

    snprintf(value_text, sizeof(value_text), "%.2f", (double)value);
    name_w = ((int)strlen(name) + 1) * 8;
    value_w = (int)strlen(value_text) * 8;
    tx = sim_clampi(x, view->x0 + 2, view->x1 - name_w - value_w - 2);
    ty = sim_clampi(y, view->y0 + 2, view->y1 - 18);

    if (sim_active_field_is(frame, name))
    {
        sim_textf(tx + name_w, ty, RA_BLACK, RA_WHITE, "%s", value_text);
        sim_textf(tx, ty, SIM_COL_DIM, SIM_COL_BG, "%s ", name);
    }
    else
    {
        sim_textf(tx, ty, SIM_COL_DIM, SIM_COL_BG, "%s ", name);
        sim_textf(tx + name_w, ty, SIM_COL_DIM, SIM_COL_BG, "%s", value_text);
    }
}

static void sim_draw_corner_amount_label_at(const lcam_sim_view_t *view,
                                            const ui_snapshot_frame_t *frame,
                                            int x,
                                            int y,
                                            const char *name,
                                            float value)
{
    if (!view || !name || !name[0])
        return;

    sim_draw_value_label_at(view, frame, x, y, name, value);
}

static void sim_draw_turn_start_group(const lcam_sim_view_t *view,
                                      const ui_snapshot_frame_t *frame,
                                      int anchor_x,
                                      int anchor_y,
                                      float z1,
                                      float d1)
{
    int x = anchor_x - 72;
    int y = anchor_y - 32;

    sim_draw_value_label_at(view, frame, x, y, "Z1", z1);
    sim_draw_value_label_at(view, frame, x, y + 16, "D1", d1);
}

static void sim_draw_turn_dt_label(const lcam_sim_view_t *view,
                                   const ui_snapshot_frame_t *frame,
                                   int anchor_x,
                                   int anchor_y,
                                   float dt)
{
    sim_draw_value_label_at(view, frame, anchor_x + 10, anchor_y - 8, "DT", dt);
}

static void sim_draw_turn_end_group(const lcam_sim_view_t *view,
                                    const ui_snapshot_frame_t *frame,
                                    int anchor_x,
                                    int anchor_y,
                                    float z2,
                                    float d2,
                                    const char *corner_name,
                                    float corner_amount,
                                    bool is_od)
{
    char z2_text[16];
    char d2_text[16];
    char corner_text[16];
    int has_corner = corner_name && corner_name[0] && corner_amount > 0.0f;
    int lines = has_corner ? 3 : 2;
    int corner_w;
    int z2_w;
    int d2_w;
    int max_w;
    int x;
    int y = anchor_y - ((lines * 16) + 10);

    (void)is_od;
    snprintf(z2_text, sizeof(z2_text), "%.2f", (double)z2);
    snprintf(d2_text, sizeof(d2_text), "%.2f", (double)d2);
    snprintf(corner_text, sizeof(corner_text), "%.2f", (double)corner_amount);
    corner_w = has_corner ? (((int)strlen(corner_name) + 1 + (int)strlen(corner_text)) * 8) : 0;
    z2_w = (3 + (int)strlen(z2_text)) * 8;
    d2_w = (3 + (int)strlen(d2_text)) * 8;
    max_w = MAX(corner_w, MAX(z2_w, d2_w));
    x = anchor_x - max_w - 10;

    if (y < view->y0 + 2)
        y = view->y0 + 2;

    if (has_corner)
    {
        sim_draw_corner_amount_label_at(view, frame, x, y, corner_name, corner_amount);
        y += 16;
    }
    sim_draw_value_label_at(view, frame, x, y, "Z2", z2);
    sim_draw_value_label_at(view, frame, x, y + 16, "D2", d2);
}

static void sim_draw_thread_start_group(const lcam_sim_view_t *view,
                                        const ui_snapshot_frame_t *frame,
                                        int anchor_x,
                                        int anchor_y,
                                        float z1,
                                        float d,
                                        bool is_od)
{
    int x = anchor_x - 72;
    int y = anchor_y - 32;

    sim_draw_value_label_at(view, frame, x, y, "Z1", z1);
    sim_draw_value_label_at(view, frame, x, y + 16, is_od ? "OD" : "ID", d);
}

static void sim_draw_thread_end_group(const lcam_sim_view_t *view,
                                      const ui_snapshot_frame_t *frame,
                                      int anchor_x,
                                      int anchor_y,
                                      float z2,
                                      float pitch)
{
    char z2_text[16];
    char p_text[16];
    int z2_w;
    int p_w;
    int x;
    int y = anchor_y - 42;

    snprintf(z2_text, sizeof(z2_text), "%.2f", (double)z2);
    snprintf(p_text, sizeof(p_text), "%.2f", (double)pitch);
    z2_w = (3 + (int)strlen(z2_text)) * 8;
    p_w = (2 + (int)strlen(p_text)) * 8;
    x = anchor_x - MAX(z2_w, p_w) - 10;

    if (y < view->y0 + 2)
        y = view->y0 + 2;

    sim_draw_value_label_at(view, frame, x, y, "Z2", z2);
    sim_draw_value_label_at(view, frame, x, y + 16, "P", pitch);
}

static void sim_format_corner_label(char *dst, unsigned dst_len, const char *name, float v)
{
    if (!dst || !dst_len)
        return;

    snprintf(dst, dst_len, "%s %.2f", name ? name : "", (double)v);
    dst[dst_len - 1] = 0;
}

static void sim_draw_dz_corner_labels(const lcam_sim_view_t *view,
                                      int start_x,
                                      int start_y,
                                      const char *z1_label,
                                      const char *d1_label,
                                      int end_x,
                                      int end_y,
                                      const char *z2_label,
                                      const char *d2_label)
{
    sim_draw_corner_label(view, start_x, start_y, z1_label, SIM_LABEL_Z1_DX, SIM_LABEL_Z1_DY);
    sim_draw_corner_label(view, start_x, start_y, d1_label, SIM_LABEL_D1_DX, SIM_LABEL_D1_DY);
    sim_draw_corner_label(view, end_x, end_y, z2_label, SIM_LABEL_Z2_DX, SIM_LABEL_Z2_DY);
    sim_draw_corner_label(view, end_x, end_y, d2_label, SIM_LABEL_D2_DX, SIM_LABEL_D2_DY);
}

static void sim_build_turn_corner_geometry(bool is_od, sim_turn_shape_t *shape)
{
    const float eps = 0.000001f;
    float r_start;
    float r_end;
    float dz;
    float dr;
    float len;
    float az;
    float ax;
    float bx;
    float dot;
    float trim;

    if (!shape)
        return;

    shape->z_profile_end = shape->z2;
    shape->d_profile_end = shape->d2;
    shape->d_corner_end = shape->d2;
    shape->arc_i = 0.0f;
    shape->arc_k = 0.0f;

    if (shape->corner == SIM_CORNER_NONE || shape->amount <= 0.0f)
        return;

    r_start = shape->dt * 0.5f;
    r_end = shape->d2 * 0.5f;
    dz = shape->z2 - shape->z1;
    dr = r_end - r_start;
    len = sqrtf((dz * dz) + (dr * dr));
    if (len <= eps)
    {
        shape->corner = SIM_CORNER_NONE;
        shape->amount = 0.0f;
        return;
    }

    az = -dz / len;
    ax = -dr / len;
    bx = is_od ? 1.0f : -1.0f;

    dot = ax * bx;
    if (dot > 0.999f) dot = 0.999f;
    if (dot < -0.999f) dot = -0.999f;

    if (shape->corner == SIM_CORNER_CHMF)
    {
        trim = shape->amount;
    }
    else
    {
        trim = shape->amount * sqrtf((1.0f + dot) / (1.0f - dot));
    }

    shape->z_profile_end = shape->z2 + (az * trim);
    shape->d_profile_end = 2.0f * (r_end + (ax * trim));
    shape->d_corner_end = 2.0f * (r_end + (bx * trim));

    if (shape->corner == SIM_CORNER_RND)
    {
        float bis_z = az;
        float bis_x = ax + bx;
        float bis_len = sqrtf((bis_z * bis_z) + (bis_x * bis_x));
        float inv_sin_half;
        float center_z;
        float center_r;

        if (bis_len <= eps)
        {
            shape->corner = SIM_CORNER_NONE;
            shape->amount = 0.0f;
            shape->z_profile_end = shape->z2;
            shape->d_profile_end = shape->d2;
            shape->d_corner_end = shape->d2;
            return;
        }

        inv_sin_half = sqrtf(2.0f / (1.0f - dot));
        center_z = shape->z2 + (bis_z / bis_len) * shape->amount * inv_sin_half;
        center_r = r_end + (bis_x / bis_len) * shape->amount * inv_sin_half;
        shape->arc_k = center_z - shape->z_profile_end;
        shape->arc_i = center_r - (shape->d_profile_end * 0.5f);
    }
}

static void sim_read_turn_shape(const char *line, bool is_od, sim_turn_shape_t *shape)
{
    float rnd = 0.0f;
    float chmf = 0.0f;

    if (!shape)
        return;

    memset(shape, 0, sizeof(*shape));
    if (!sim_field_float2(line, "D1", "DIAMETER_1", &shape->d1)) return;
    if (!sim_field_float2(line, "D2", "DIAMETER_2", &shape->d2)) return;
    if (!sim_field_float2(line, "Z1", "Z_1", &shape->z1)) return;
    if (!sim_field_float2(line, "Z2", "Z_2", &shape->z2)) return;

    shape->dt = shape->d2;
    (void)sim_field_float3(line, "DT", "D_TAPER", "TAPER_DIAMETER", &shape->dt);

    (void)sim_field_float3(line, "RND", "ROUND", "RADIUS", &rnd);
    (void)sim_field_float3(line, "CHMF", "CHAMFER", "C", &chmf);
    if (rnd > 0.0f && chmf <= 0.0f)
    {
        shape->corner = SIM_CORNER_RND;
        shape->amount = rnd;
    }
    else if (chmf > 0.0f && rnd <= 0.0f)
    {
        shape->corner = SIM_CORNER_CHMF;
        shape->amount = chmf;
    }

    if (is_od && shape->dt > shape->d1)
        shape->dt = shape->d1;
    if (!is_od && shape->dt < shape->d1)
        shape->dt = shape->d1;

    if (shape->amount >= sim_absf(shape->z1 - shape->z2))
        shape->amount = 0.0f;
    if (shape->amount <= 0.0f)
        shape->corner = SIM_CORNER_NONE;

    sim_build_turn_corner_geometry(is_od, shape);
}

static float sim_lerpf(float a, float b, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return a + ((b - a) * t);
}

static float sim_profile_d_at_z(const sim_turn_shape_t *shape, bool is_od, float z)
{
    float z_span;
    float t;
    float d_profile;
    float corner_len;
    bool in_corner;

    (void)is_od;

    if (!shape)
        return 0.0f;

    z_span = shape->z_profile_end - shape->z1;
    if (z_span == 0.0f)
        d_profile = shape->d_profile_end;
    else
    {
        t = (z - shape->z1) / z_span;
        d_profile = sim_lerpf(shape->dt, shape->d_profile_end, t);
    }

    if (shape->corner == SIM_CORNER_NONE || shape->amount <= 0.0f)
        return d_profile;

    in_corner = shape->z_profile_end < shape->z2
        ? (z >= shape->z_profile_end && z <= shape->z2)
        : (z <= shape->z_profile_end && z >= shape->z2);
    if (!in_corner)
        return d_profile;

    corner_len = sim_absf(shape->z2 - shape->z_profile_end);
    if (corner_len <= 0.0f)
        return shape->d_corner_end;

    if (shape->corner == SIM_CORNER_RND)
    {
        float center_z = shape->z_profile_end + shape->arc_k;
        float center_r = (shape->d_profile_end * 0.5f) + shape->arc_i;
        float dz = z - center_z;
        float root_arg;
        float rd;
        float d_a;
        float d_b;
        float d_ref;

        root_arg = (shape->amount * shape->amount) - (dz * dz);
        if (root_arg < 0.0f)
            root_arg = 0.0f;
        rd = sqrtf(root_arg);

        d_a = 2.0f * (center_r + rd);
        d_b = 2.0f * (center_r - rd);
        t = (z - shape->z_profile_end) / (shape->z2 - shape->z_profile_end);
        d_ref = sim_lerpf(shape->d_profile_end, shape->d_corner_end, t);
        return sim_absf(d_a - d_ref) < sim_absf(d_b - d_ref) ? d_a : d_b;
    }

    t = (z - shape->z_profile_end) / (shape->z2 - shape->z_profile_end);
    return sim_lerpf(shape->d_profile_end, shape->d_corner_end, t);
}

static void sim_draw_turn_shape_preview(const lcam_sim_view_t *view,
                                        const sim_turn_shape_t *shape,
                                        bool is_od,
                                        int spacing)
{
    int x1;
    int x2;
    int x;
    int tmp;
    int hatch_step;
    int min_y;
    int max_y;
    int y;
    float z;
    int base_y;
    int profile_y;
    int prev_x = 0;
    int prev_y = 0;
    bool have_prev = false;

    if (!view || !shape)
        return;

    x1 = sim_z_to_px(view, shape->z1);
    x2 = sim_z_to_px(view, shape->z2);
    if (x2 < x1) { tmp = x1; x1 = x2; x2 = tmp; }
    if (x2 <= x1) x2 = x1 + 1;

    base_y = sim_d_to_py(view, shape->d1);
    hatch_step = spacing > 0 ? spacing : 4;
    min_y = base_y;
    max_y = base_y;

    for (x = x1; x <= x2; ++x)
    {
        z = ((float)x - (float)view->z_zero_x) / view->z_scale;
        profile_y = sim_d_to_py(view, sim_profile_d_at_z(shape, is_od, z));

        if (is_od)
            sim_draw_hatch_vline(x, profile_y, base_y, hatch_step);
        else
            sim_draw_hatch_vline(x, base_y, profile_y, hatch_step);

        if (profile_y < min_y) min_y = profile_y;
        if (profile_y > max_y) max_y = profile_y;
    }

    for (y = min_y + hatch_step; y < max_y; y += hatch_step)
    {
        int seg_start = -1;

        for (x = x1; x <= x2; ++x)
        {
            int ya;
            int yb;
            bool inside;

            z = ((float)x - (float)view->z_zero_x) / view->z_scale;
            profile_y = sim_d_to_py(view, sim_profile_d_at_z(shape, is_od, z));
            ya = profile_y < base_y ? profile_y : base_y;
            yb = profile_y > base_y ? profile_y : base_y;
            inside = y >= ya && y <= yb;

            if (inside && seg_start < 0)
                seg_start = x;
            else if (!inside && seg_start >= 0)
            {
                if (x - 1 > seg_start)
                    ra_draw_line(seg_start, y, x - 1, y, RA_BLACK);
                seg_start = -1;
            }
        }

        if (seg_start >= 0 && x2 > seg_start)
            ra_draw_line(seg_start, y, x2, y, RA_BLACK);
    }

    for (x = x1; x <= x2; ++x)
    {
        z = ((float)x - (float)view->z_zero_x) / view->z_scale;
        profile_y = sim_d_to_py(view, sim_profile_d_at_z(shape, is_od, z));

        if (have_prev)
            ra_draw_line(prev_x, prev_y, x, profile_y, RA_BLACK);
        prev_x = x;
        prev_y = profile_y;
        have_prev = true;
    }
}

static void sim_draw_od_preview(const lcam_sim_view_t *view, const char *line, const ui_snapshot_frame_t *frame)
{
    float d1;
    float d2;
    float dt;
    float z1;
    float z2;
    float doc;
    int label_x1;
    int label_end_x;
    int label_y1;
    int label_corner_y;
    int label_dt_y;
    int spacing = 0;
    const char *corner_name = "";
    sim_turn_shape_t shape;

    if (!sim_field_float2(line, "D1", "DIAMETER_1", &d1)) return;
    if (!sim_field_float2(line, "D2", "DIAMETER_2", &d2)) return;
    if (!sim_field_float2(line, "Z1", "Z_1", &z1)) return;
    if (!sim_field_float2(line, "Z2", "Z_2", &z2)) return;
    sim_read_turn_shape(line, true, &shape);
    dt = shape.dt;

    label_x1 = sim_z_to_px(view, z1);
    label_end_x = sim_z_to_px(view, z2);
    label_y1 = sim_d_to_py(view, d1);
    label_corner_y = sim_d_to_py(view, sim_profile_d_at_z(&shape, true, z2));
    label_dt_y = sim_d_to_py(view, dt);

    if (sim_field_float3(line, "DOC", "R_DOC", "ROUGH_DOC", &doc) ||
        (frame && sim_field_float3(frame->leancam_tool_line, "R_DOC", "ROUGH_DOC", "ROUGH_DEPTH_OF_CUT", &doc)))
        spacing = sim_diam_len_to_px(view, sim_absf(doc));
    if (spacing <= 0) spacing = 4;

    sim_draw_turn_shape_preview(view, &shape, true, spacing);
    if (shape.corner != SIM_CORNER_NONE)
        corner_name = shape.corner == SIM_CORNER_RND ? "RND" : "CHMF";
    sim_draw_turn_start_group(view, frame, label_x1, label_y1, z1, d1);
    sim_draw_turn_dt_label(view, frame, label_x1, label_dt_y, dt);
    sim_draw_turn_end_group(view, frame, label_end_x, label_corner_y, z2, d2, corner_name, shape.amount, true);
}

static void sim_draw_id_preview(const lcam_sim_view_t *view, const char *line, const ui_snapshot_frame_t *frame)
{
    float d1;
    float d2;
    float dt;
    float z1;
    float z2;
    float doc;
    int label_x1;
    int label_end_x;
    int label_y1;
    int label_corner_y;
    int label_dt_y;
    int spacing = 0;
    const char *corner_name = "";
    sim_turn_shape_t shape;

    if (!sim_field_float2(line, "D1", "DIAMETER_1", &d1)) return;
    if (!sim_field_float2(line, "D2", "DIAMETER_2", &d2)) return;
    if (!sim_field_float2(line, "Z1", "Z_1", &z1)) return;
    if (!sim_field_float2(line, "Z2", "Z_2", &z2)) return;
    sim_read_turn_shape(line, false, &shape);
    dt = shape.dt;

    label_x1 = sim_z_to_px(view, z1);
    label_end_x = sim_z_to_px(view, z2);
    label_y1 = sim_d_to_py(view, d1);
    label_corner_y = sim_d_to_py(view, sim_profile_d_at_z(&shape, false, z2));
    label_dt_y = sim_d_to_py(view, dt);

    if (sim_field_float3(line, "DOC", "R_DOC", "ROUGH_DOC", &doc) ||
        (frame && sim_field_float3(frame->leancam_tool_line, "R_DOC", "ROUGH_DOC", "ROUGH_DEPTH_OF_CUT", &doc)))
        spacing = sim_diam_len_to_px(view, sim_absf(doc));
    if (spacing <= 0) spacing = 4;

    sim_draw_turn_shape_preview(view, &shape, false, spacing);
    if (shape.corner != SIM_CORNER_NONE)
        corner_name = shape.corner == SIM_CORNER_RND ? "RND" : "CHMF";
    sim_draw_turn_start_group(view, frame, label_x1, label_y1, z1, d1);
    sim_draw_turn_dt_label(view, frame, label_x1, label_dt_y, dt);
    sim_draw_turn_end_group(view, frame, label_end_x, label_corner_y, z2, d2, corner_name, shape.amount, false);
}

static void sim_draw_face_preview(const lcam_sim_view_t *view, const char *line, const ui_snapshot_frame_t *frame)
{
    float d1 = 0.0f;
    float d;
    float z1 = 0.0f;
    float z;
    int x1;
    int x2;
    int label_x1;
    int label_x2;
    int label_y1;
    int label_y2;
    int y2;
    int tmp;
    float doc;
    int spacing = 0;
    char z1_label[24];
    char z_label[24];
    char d_label[24];

    if (!sim_field_float3(line, "D", "OD", "OUTER_DIAMETER", &d)) d = view->stock_od;
    (void)sim_field_float2(line, "Z1", "Z_1", &z1);
    if (!sim_field_float2(line, "Z", "Z_2", &z)) return;

    label_x1 = sim_z_to_px(view, z1);
    label_x2 = sim_z_to_px(view, z);
    label_y1 = sim_d_to_py(view, d1);
    label_y2 = sim_d_to_py(view, d);
    x1 = label_x1;
    x2 = label_x2;
    if (x2 < x1) { tmp = x1; x1 = x2; x2 = tmp; }
    y2 = label_y2;
    if (x2 <= x1) x2 = x1 + 1;

    if (sim_field_float3(line, "DOC", "R_DOC", "ROUGH_DOC", &doc) ||
        (frame && sim_field_float3(frame->leancam_tool_line, "R_DOC", "ROUGH_DOC", "ROUGH_DEPTH_OF_CUT", &doc)))
        spacing = sim_diam_len_to_px(view, sim_absf(doc));
    if (spacing <= 0) spacing = 4;

    sim_format_corner_label(z1_label, sizeof(z1_label), "Z1", z1);
    sim_format_corner_label(z_label, sizeof(z_label), "Z", z);
    sim_format_corner_label(d_label, sizeof(d_label), "D", d);
    sim_draw_hatch_rect(x1, view->stock_top, x2, y2, spacing, true);
    sim_draw_dz_corner_labels(view, label_x1, label_y2, z1_label, d_label, label_x2, label_y1, z_label, "");
}

static void sim_draw_groove_preview(const lcam_sim_view_t *view, const char *line)
{
    float d1;
    float d2;
    float z1;
    float z2;
    int x1;
    int x2;
    int y1;
    int y2;
    int tmp;
    char d1_label[24];
    char d2_label[24];

    if (!sim_field_float(line, "D1", &d1)) return;
    if (!sim_field_float(line, "D2", &d2)) return;
    if (!sim_field_float(line, "Z1", &z1)) return;
    if (!sim_field_float(line, "Z2", &z2)) return;

    x1 = sim_z_to_px(view, z1);
    x2 = sim_z_to_px(view, z2);
    y1 = sim_d_to_py(view, d1);
    y2 = sim_d_to_py(view, d2);
    if (x2 < x1) { tmp = x1; x1 = x2; x2 = tmp; }
    if (y2 < y1) { tmp = y1; y1 = y2; y2 = tmp; }
    if (x2 <= x1) x2 = x1 + 1;
    if (y2 <= y1) y2 = y1 + 1;

    sim_format_corner_label(d1_label, sizeof(d1_label), "D1", d1);
    sim_format_corner_label(d2_label, sizeof(d2_label), "D2", d2);
    sim_draw_hatch_rect(x1, y1, x2, y2, 4, true);
    sim_draw_corner_label(view, x1, y1, d1_label, -54, -18);
    sim_draw_corner_label(view, x2, y2, d2_label, 6, 4);
}

static void sim_draw_thread_lane_hint(const lcam_sim_view_t *view,
                                      const ui_snapshot_frame_t *frame,
                                      float z1,
                                      float z2,
                                      int y)
{
    float dir;
    float start_z0;
    float stop_z1;
    int sx0;
    int sx1;
    int ex0;
    int ex1;
    char label[40];

    if (!view || !frame || !frame->leancam_thread_lane_valid)
        return;

    y = sim_clampi(y, view->y0 + 18, view->y1 - 34);

    dir = (z2 >= z1) ? -1.0f : 1.0f;
    start_z0 = z1 + (dir * frame->leancam_thread_start_lane);
    stop_z1 = z2 - (dir * frame->leancam_thread_stop_lane);

    sx0 = sim_z_to_px(view, start_z0);
    sx1 = sim_z_to_px(view, z1);
    ex0 = sim_z_to_px(view, z2);
    ex1 = sim_z_to_px(view, stop_z1);

    if (sx1 < sx0) { int t = sx0; sx0 = sx1; sx1 = t; }
    if (ex1 < ex0) { int t = ex0; ex0 = ex1; ex1 = t; }

    sx0 = sim_clampi(sx0, view->x0 + 1, view->x1 - 1);
    sx1 = sim_clampi(sx1, view->x0 + 1, view->x1 - 1);
    ex0 = sim_clampi(ex0, view->x0 + 1, view->x1 - 1);
    ex1 = sim_clampi(ex1, view->x0 + 1, view->x1 - 1);

    if (sx1 > sx0)
    {
        ra_draw_line(sx0, y, sx1, y, SIM_COL_WARN);
        ra_draw_line(sx0, y - 5, sx0, y + 5, SIM_COL_WARN);
        ra_draw_line(sx1, y - 5, sx1, y + 5, SIM_COL_WARN);
    }

    if (ex1 > ex0)
    {
        ra_draw_line(ex0, y + 12, ex1, y + 12, SIM_COL_WARN);
        ra_draw_line(ex0, y + 7, ex0, y + 17, SIM_COL_WARN);
        ra_draw_line(ex1, y + 7, ex1, y + 17, SIM_COL_WARN);
    }

    snprintf(label, sizeof(label), "START LANE %.2f", frame->leancam_thread_start_lane);
    sim_draw_corner_label(view, sim_z_to_px(view, z1), y, label, -86, -26);
    snprintf(label, sizeof(label), "STOP LANE %.2f", frame->leancam_thread_stop_lane);
    sim_draw_corner_label(view, sim_z_to_px(view, z2), y + 12, label, 8, 10);
}

static void sim_draw_thread_preview(const lcam_sim_view_t *view, const char *line, const ui_snapshot_frame_t *frame, bool is_od)
{
    float d_start = 0.0f;
    float d_end = 0.0f;
    float depth = 0.0f;
    float z1;
    float z2;
    float pitch = 0.0f;
    float nominal = 0.0f;
    float taper = 0.0f;
    bool has_nominal;
    int x1;
    int x2;
    int y_start;
    int y_end;
    int tmp;
    int p;
    int spacing;

    has_nominal = sim_field_float2(line, "M", "NOM", &nominal);
    (void)sim_field_float3(line, "P", "PITCH", "K", &pitch);

    if (is_od)
    {
        if (!sim_field_float3(line, "D", "MAJOR", "OD", &d_start) &&
            !sim_field_float2(line, "D1", "OUTSIDE_DIAMETER", &d_start) &&
            has_nominal)
            d_start = nominal;
        (void)sim_field_float3(line, "D2", "MINOR", "ID", &d_end);
        if (d_end <= 0.0f && has_nominal && pitch > 0.0f)
            d_end = nominal - (1.22687f * pitch);
    }
    else
    {
        if (!sim_field_float3(line, "D", "MINOR", "ID", &d_start) &&
            !sim_field_float2(line, "D1", "INSIDE_DIAMETER", &d_start) &&
            has_nominal && pitch > 0.0f)
            d_start = nominal - (1.08253f * pitch);
        (void)sim_field_float3(line, "D2", "MAJOR", "OD", &d_end);
        if (d_end <= 0.0f && has_nominal)
            d_end = nominal;
    }

    if (!sim_field_float2(line, "Z1", "Z_START", &z1)) return;
    if (!sim_field_float2(line, "Z2", "Z_END", &z2)) return;
    (void)sim_field_float3(line, "THR_DEPTH", "THREAD_DEPTH", "DEPTH", &depth);
    (void)sim_field_float3(line, "TAPER", "D_TAPER", "TAPER_DIAMETER", &taper);
    if (d_end <= 0.0f && depth > 0.0f)
        d_end = is_od ? (d_start - depth) : (d_start + depth);
    if (d_end <= 0.0f)
        return;

    x1 = sim_z_to_px(view, z1);
    x2 = sim_z_to_px(view, z2);
    y_start = sim_d_to_py(view, d_start);
    y_end = sim_d_to_py(view, d_end + taper);

    if (x2 < x1) { tmp = x1; x1 = x2; x2 = tmp; }
    if (x2 <= x1) x2 = x1 + 1;

    sim_draw_hatch_rect(x1,
                        is_od ? y_end : y_start,
                        x2,
                        is_od ? y_start : y_end,
                        4,
                        false);

    spacing = pitch > 0.0f ? (int)(pitch * view->z_scale + 0.5f) : 8;
    spacing = sim_clampi(spacing, 4, 18);
    for (p = x1; p <= x2; p += spacing)
    {
        int p2 = p + spacing;
        if (p2 > x2) p2 = x2;
        ra_draw_line(p, is_od ? y_start : y_end, p2, is_od ? y_end : y_start, RA_BLACK);
    }

    sim_draw_thread_lane_hint(view, frame, z1, z2, is_od ? (y_start - 34) : (y_end - 34));
    sim_draw_thread_start_group(view, frame, sim_z_to_px(view, z1), y_start, z1, d_start, is_od);
    sim_draw_thread_end_group(view, frame, sim_z_to_px(view, z2), y_end, z2, pitch);
}

static void sim_draw_drill_preview(const lcam_sim_view_t *view, const char *line, const ui_snapshot_frame_t *frame)
{
    float z1;
    float depth;
    float target;
    float tool_d = 0.0f;
    int x1;
    int x2;
    int y1;
    int y2;
    int tmp;
    char z1_label[24];
    char z2_label[24];
    char d_label[24];

    if (!sim_field_float2(line, "Z1", "Z_START", &z1)) return;
    if (!sim_field_float(line, "DEPTH", &depth)) return;

    target = (depth <= 0.0f) ? depth : (z1 - depth);
    if (target >= z1)
        return;

    if (!sim_field_tool_diameter(line, &tool_d) &&
        frame && !sim_field_tool_diameter(frame->leancam_tool_line, &tool_d))
        tool_d = view->stock_od * 0.12f;

    if (tool_d != tool_d || tool_d <= 0.0f)
        tool_d = view->stock_od * 0.12f;
    if (tool_d > view->stock_od)
        tool_d = view->stock_od;
    x1 = sim_z_to_px(view, z1);
    x2 = sim_z_to_px(view, target);
    y1 = view->stock_top;
    y2 = sim_d_to_py(view, tool_d);

    if (x2 < x1) { tmp = x1; x1 = x2; x2 = tmp; }
    if (x2 <= x1) x2 = x1 + 1;
    if (y2 <= y1) y2 = y1 + 1;

    ra_fill_rect((uint16_t)x1, (uint16_t)y1, (uint16_t)x2, (uint16_t)y2, SIM_COL_HI);
    ra_draw_rect((uint16_t)x1, (uint16_t)y1, (uint16_t)x2, (uint16_t)y2, RA_BLACK);

    sim_format_corner_label(z1_label, sizeof(z1_label), "Z1", z1);
    sim_format_corner_label(z2_label, sizeof(z2_label), "Z", target);
    sim_format_corner_label(d_label, sizeof(d_label), "TD", tool_d);
    sim_draw_corner_label(view, sim_z_to_px(view, z1), y2, z1_label, 6, 4);
    sim_draw_corner_label(view, sim_z_to_px(view, target), y2, z2_label, -54, 4);
    sim_draw_corner_label(view, x1, y2, d_label, 6, 20);
}

typedef struct
{
    bool initialized;
    bool have_prev;
    bool thread_phase_set;
    char setup_line[UI_LC_LINE_LEN];
    sim_setup_t setup;
    lcam_sim_view_t view;
    int doc_x_px;
    int doc_z_px;
    int sprite_x_px;
    int sprite_z_px;
    uint32_t thread_phase_origin;
    int32_t thread_sync_ec;
    int32_t thread_next_cut_ec;
    bool thread_z_set;
    float thread_start_z;
    float thread_pitch;
    float thread_next_cut_z;
    int thread_z_dir;
    float prev_x;
    float prev_z;
} sim_live_state_t;

static sim_live_state_t g_live_sim;

static bool sim_live_signature_changed(const ui_snapshot_frame_t *frame)
{
    if (!frame)
        return false;

    return strcmp(g_live_sim.setup_line, frame->leancam_setup_line) != 0 ||
           frame->leancam_setup_line[0] == 0;
}

static void sim_live_store_signature(const ui_snapshot_frame_t *frame)
{
    if (!frame)
        return;

    ui_snapshot_strcpy(g_live_sim.setup_line, frame->leancam_setup_line, sizeof(g_live_sim.setup_line));
}

static float sim_live_runtime_x_to_diam(float runtime_x)
{
#if SIM_LIVE_RT_X_RADIUS
    return runtime_x * 2.0f;
#else
    return runtime_x;
#endif
}

static float sim_live_doc_from_frame(const ui_snapshot_frame_t *frame)
{
    float doc = 0.0f;

    if (!frame)
        return 0.5f;

    if (!sim_field_float3(frame->leancam_preview_line, "DOC", "R_DOC", "ROUGH_DOC", &doc) &&
        !sim_field_float3(frame->leancam_tool_line, "R_DOC", "ROUGH_DOC", "ROUGH_DEPTH_OF_CUT", &doc))
        doc = 0.5f;

    return sim_absf(doc);
}

static bool sim_live_line_is_thread(const char *line)
{
    if (!line || !line[0])
        return false;

    return sim_is_cycle(line, "THR_OD") ||
           sim_is_cycle(line, "THR_ID") ||
           sim_is_cycle(line, "THREAD") ||
           strstr(line, "G33") != NULL ||
           strstr(line, "g33") != NULL;
}

static bool sim_live_is_threading(const ui_snapshot_frame_t *frame)
{
    if (!frame)
        return false;

    return frame->g33_active ||
           sim_live_line_is_thread(frame->leancam_preview_line);
}

static bool sim_live_is_id_cycle(const ui_snapshot_frame_t *frame)
{
    const char *line;

    if (!frame)
        return false;

    line = frame->leancam_preview_line;
    return sim_is_cycle(line, "ID") || sim_is_cycle(line, "THR_ID");
}

bool leancam_sim_live_should_hold(const ui_snapshot_frame_t *frame)
{
    return frame && frame->leancam_active && frame->g33_active;
}

#if RA8876_LCAM_LIVE_DEPTH_GRADIENT
static uint16_t sim_live_depth_color(float x_diam);
#endif

static void sim_live_draw_depth_gradient_line(const lcam_sim_view_t *view, int px, int py)
{
#if RA8876_LCAM_LIVE_DEPTH_GRADIENT
    int y;

    if (!view)
        return;

    px = sim_clampi(px, view->stock_left, view->stock_right);
    py = sim_clampi(py, view->stock_top, view->stock_bottom);

    for (y = view->stock_top; y <= py; y++)
    {
        float d = ((float)(y - view->stock_top) / view->d_scale) * 2.0f;
        ra_draw_line(px, y, px, y, sim_live_depth_color(d));
    }
#endif
}

#if RA8876_LCAM_LIVE_DEPTH_GRADIENT
static uint16_t sim_gray565(unsigned g)
{
    if (g > 255U)
        g = 255U;

    return (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
}

static uint16_t sim_live_depth_color(float x_diam)
{
    float norm;
    unsigned gray;

    if (x_diam != x_diam || g_live_sim.view.stock_od <= 0.0f)
        return sim_gray565(80U);

    norm = x_diam / g_live_sim.view.stock_od;
    if (norm < 0.0f)
        norm = 0.0f;
    if (norm > 1.0f)
        norm = 1.0f;

    gray = (unsigned)(34.0f + norm * 116.0f);
    return sim_gray565(gray);
}
#endif

static void sim_live_fill_rhombus(int cx, int cy, int w, int h, uint16_t col)
{
    int half_w;
    int half_h;
    int y;

    if (w < 2)
        w = 2;
    if (h < 2)
        h = 2;

    half_w = w / 2;
    half_h = h / 2;
    if (half_w < 1)
        half_w = 1;
    if (half_h < 1)
        half_h = 1;

    for (y = -half_h; y <= half_h; y++)
    {
        int yy = cy + y;
        int span = half_w - (int)(((int32_t)sim_absf((float)y) * half_w) / half_h);
        if (yy < SIM_LIVE_Y0 || yy > SIM_LIVE_Y1)
            continue;
        ra_draw_line(sim_clampi(cx - span, SIM_LIVE_X0, SIM_LIVE_X1),
                     yy,
                     sim_clampi(cx + span, SIM_LIVE_X0, SIM_LIVE_X1),
                     yy,
                     col);
    }
}

static bool sim_live_thread_z_params(const ui_snapshot_frame_t *frame, float *start_z, float *pitch, int *dir)
{
    float z1 = 0.0f;
    float z2 = 0.0f;
    float p = 0.0f;
    bool have_z1;
    bool have_z2;

    if (!frame)
        return false;

    if (!sim_field_float3(frame->leancam_preview_line, "P", "PITCH", "K", &p))
        return false;
    p = sim_absf(p);
    if (p <= 0.0f)
        return false;

    have_z1 = sim_field_float2(frame->leancam_preview_line, "Z1", "Z_START", &z1);
    have_z2 = sim_field_float2(frame->leancam_preview_line, "Z2", "Z_END", &z2);

    if (!have_z1)
        z1 = g_live_sim.prev_z;

    if (start_z)
        *start_z = z1;
    if (pitch)
        *pitch = p;
    if (dir)
    {
        if (have_z2 && z2 < z1)
            *dir = -1;
        else if (have_z2 && z2 > z1)
            *dir = 1;
        else
            *dir = (g_live_sim.prev_z > z1) ? -1 : 1;
    }

    return true;
}

static bool sim_live_thread_phase_allows_cut(const ui_snapshot_frame_t *frame, float z_pos, float *cut_z)
{
    float start_z = 0.0f;
    float pitch = 0.0f;
    int dir = 1;

    if (cut_z)
        *cut_z = z_pos;

    if (!sim_live_is_threading(frame))
        return true;

    if (!sim_live_thread_z_params(frame, &start_z, &pitch, &dir))
        return false;

    if (!g_live_sim.thread_z_set ||
        sim_absf(g_live_sim.thread_start_z - start_z) > 0.0001f ||
        sim_absf(g_live_sim.thread_pitch - pitch) > 0.0001f ||
        g_live_sim.thread_z_dir != dir)
    {
        g_live_sim.thread_start_z = start_z;
        g_live_sim.thread_pitch = pitch;
        g_live_sim.thread_z_dir = dir;
        g_live_sim.thread_next_cut_z = start_z;
        g_live_sim.thread_z_set = true;
    }
    else if ((dir < 0 && z_pos > start_z) ||
             (dir > 0 && z_pos < start_z))
    {
        g_live_sim.thread_next_cut_z = start_z;
        return false;
    }

    if (dir < 0)
    {
        if (z_pos > g_live_sim.thread_next_cut_z)
            return false;
        if (cut_z)
            *cut_z = g_live_sim.thread_next_cut_z;
        while (z_pos <= g_live_sim.thread_next_cut_z)
            g_live_sim.thread_next_cut_z -= pitch;
    }
    else
    {
        if (z_pos < g_live_sim.thread_next_cut_z)
            return false;
        if (cut_z)
            *cut_z = g_live_sim.thread_next_cut_z;
        while (z_pos >= g_live_sim.thread_next_cut_z)
            g_live_sim.thread_next_cut_z += pitch;
    }

    return true;
}

static void sim_live_update_doc_px(const ui_snapshot_frame_t *frame)
{
    float doc = sim_live_doc_from_frame(frame);
    int x_px = (int)(sim_radius_len_to_px(&g_live_sim.view, doc) * SIM_LIVE_DOC_DIAM_FACTOR + 0.999f);
    int z_px = (int)(doc * g_live_sim.view.z_scale * SIM_LIVE_DOC_Z_FACTOR + 0.999f);

    g_live_sim.doc_x_px = sim_clampi(x_px, SIM_LIVE_TOOL_MIN, SIM_LIVE_REMOVE_MAX);
    g_live_sim.doc_z_px = sim_clampi(z_px, SIM_LIVE_TOOL_MIN, SIM_LIVE_REMOVE_MAX);
}

static int sim_live_marker_px(int doc_px)
{
    return sim_clampi((doc_px + 1) / 2, SIM_LIVE_TOOL_MIN, SIM_LIVE_REMOVE_MAX);
}

static void sim_live_build_tool_sprite(void)
{
    uint32_t old_draw_base;
    int w = sim_live_marker_px(g_live_sim.doc_z_px);
    int h = sim_live_marker_px(g_live_sim.doc_x_px);

    if (g_live_sim.sprite_z_px == w && g_live_sim.sprite_x_px == h)
        return;

    old_draw_base = ra_get_draw_base();
    ra_set_draw_base(SIM_TOOL_ADDR);
    ra_fill_rect(0, 0, (uint16_t)(w - 1), (uint16_t)(h - 1), SIM_COL_TOOL);
    ra_draw_line(0, 0, w - 1, 0, SIM_COL_WARN);
    ra_draw_line(0, 0, 0, h - 1, SIM_COL_WARN);
    ra_set_draw_base(old_draw_base);

    g_live_sim.sprite_z_px = w;
    g_live_sim.sprite_x_px = h;
}

static void sim_live_init_canvas(const ui_snapshot_frame_t *frame)
{
    uint32_t old_draw_base;

    sim_read_setup(frame, &g_live_sim.setup);
    sim_build_live_view(&g_live_sim.setup, &g_live_sim.view);
    sim_live_store_signature(frame);
    sim_live_update_doc_px(frame);
    g_live_sim.sprite_x_px = 0;
    g_live_sim.sprite_z_px = 0;
    g_live_sim.have_prev = false;
    g_live_sim.thread_phase_set = false;
    g_live_sim.thread_z_set = false;
    sim_live_build_tool_sprite();

    old_draw_base = ra_get_draw_base();
    ra_set_draw_base(SIM_LIVE_ADDR);
    sim_draw_stock(&g_live_sim.view, &g_live_sim.setup);
    sim_draw_chuck(&g_live_sim.view, &g_live_sim.setup);
    ra_set_draw_base(old_draw_base);

    g_live_sim.initialized = true;
}

static unsigned sim_live_tool_from_frame(const ui_snapshot_frame_t *frame)
{
    unsigned tool = 0;

    if (!frame)
        return 0;

    (void)sim_field_uint(frame->leancam_tool_line, "T", &tool);
    return tool;
}

static float sim_live_surface_speed(float x_diam, unsigned spindle)
{
    if (x_diam != x_diam || x_diam <= 0.0f || spindle == 0)
        return 0.0f;

    return (float)(3.14159265f * x_diam * (float)spindle * 0.001f);
}

static void sim_live_draw_text(float x_diam,
                               float z_pos,
                               float feed,
                               unsigned spindle,
                               float doc,
                               unsigned tool)
{
    float surface = sim_live_surface_speed(x_diam, spindle);

    ra_fill_rect(SIM_LIVE_X0,
                 SIM_LIVE_Y0,
                 SIM_LIVE_X1,
                 SIM_LIVE_TEXT_H,
                 SIM_COL_BG);
    sim_textf(SIM_LIVE_TEXT_X,
              SIM_LIVE_STATUS_Y1,
              SIM_COL_AXIS,
              SIM_COL_BG,
              "X %.3f  Z %.3f  T%u  DOC %.3f",
              (double)x_diam,
              (double)z_pos,
              tool,
              (double)doc);
    sim_textf(SIM_LIVE_TEXT_X,
              SIM_LIVE_STATUS_Y2,
              SIM_COL_DIM,
              SIM_COL_BG,
              "F %.1f  S %u  V %.1f m/min",
              (double)feed,
              spindle,
              (double)surface);
}

static bool sim_live_in_stock_z(const lcam_sim_view_t *view, float z)
{
    int px;

    if (!view)
        return false;

    px = sim_live_z_to_px_raw(view, z);
    return px >= view->stock_left && px <= view->stock_right;
}

static void sim_live_draw_removal(const ui_snapshot_frame_t *frame, float x_diam, float z_pos)
{
    const lcam_sim_view_t *view = &g_live_sim.view;
    bool is_thread;
    bool is_id;
    float cut_z;
    int px;
    int py;
    int ppx;
    int ppy;
    int x1;
    int x2;
    int y1;
    int y2;
    int pad;
    int tmp;

    if (x_diam != x_diam || z_pos != z_pos)
        return;

    if (x_diam < 0.0f)
        x_diam = 0.0f;

    is_thread = sim_live_is_threading(frame);
    is_id = sim_live_is_id_cycle(frame);
    cut_z = z_pos;

    px = sim_live_z_to_px_raw(view, z_pos);
    py = sim_live_d_to_py_raw(view, x_diam);

    if (!g_live_sim.have_prev)
    {
        g_live_sim.prev_x = x_diam;
        g_live_sim.prev_z = z_pos;
        g_live_sim.have_prev = true;
        return;
    }

    if (!sim_live_thread_phase_allows_cut(frame, z_pos, &cut_z))
    {
        g_live_sim.prev_x = x_diam;
        g_live_sim.prev_z = z_pos;
        return;
    }

    px = sim_live_z_to_px_raw(view, cut_z);
    if (is_thread)
    {
        ppx = sim_z_to_px(view, cut_z);
        ppy = py;
    }
    else
    {
        ppx = sim_z_to_px(view, g_live_sim.prev_z);
        ppy = sim_d_to_py(view, g_live_sim.prev_x);
    }

    if (x_diam <= view->stock_od && g_live_sim.prev_x <= view->stock_od &&
        (sim_live_in_stock_z(view, cut_z) || (!is_thread && sim_live_in_stock_z(view, g_live_sim.prev_z))))
    {
        x1 = ppx;
        x2 = px;
        if (is_id)
        {
            y1 = view->stock_top;
            y2 = ppy > py ? ppy : py;
        }
        else
        {
            y1 = ppy < py ? ppy : py;
            y2 = view->stock_bottom;
        }
        if (x2 < x1) { tmp = x1; x1 = x2; x2 = tmp; }

        pad = g_live_sim.doc_z_px + SIM_LIVE_REMOVE_BIAS - 1;
        if (pad < 1)
            pad = 1;

        x1 = sim_clampi(x1, view->stock_left, view->stock_right);
        x2 = sim_clampi(x2 + pad, view->stock_left, view->stock_right);
        y1 = sim_clampi(y1, view->stock_top, view->stock_bottom);
        y2 = sim_clampi(y2, view->stock_top, view->stock_bottom);

        if (x2 >= x1 && y2 >= y1)
        {
            if (is_thread)
            {
                if (is_id)
                    ra_fill_rect((uint16_t)x1, (uint16_t)y1, (uint16_t)x2, (uint16_t)y2, SIM_COL_REMOVED);
                sim_live_fill_rhombus(px + (g_live_sim.doc_z_px / 2),
                                      py + (g_live_sim.doc_x_px / 2),
                                      g_live_sim.doc_z_px,
                                      g_live_sim.doc_x_px,
                                      SIM_COL_REMOVED);
            }
            else
            {
                ra_fill_rect((uint16_t)x1, (uint16_t)y1, (uint16_t)x2, (uint16_t)y2, SIM_COL_REMOVED);
            }
            if (!is_thread)
                sim_live_draw_depth_gradient_line(view, px, py);
        }
    }

    g_live_sim.prev_x = x_diam;
    g_live_sim.prev_z = z_pos;
}

static bool sim_live_tool_in_chuck(float z_pos)
{
    const lcam_sim_view_t *view = &g_live_sim.view;
    int chuck_right;
    int px;

    if (g_live_sim.setup.clamp <= 0.0f)
        return false;

    chuck_right = view->stock_left + (int)(g_live_sim.setup.clamp * view->z_scale + 0.5f);
    px = sim_live_z_to_px_raw(view, z_pos);
    return px <= chuck_right;
}

static void sim_live_draw_thread_marker(uint32_t dst_base, int px, int py, int w, int h)
{
    uint32_t old_draw_base = ra_get_draw_base();
    int cx;
    int cy;
    int half_w;
    int half_h;
    int y;

    if (w < SIM_LIVE_TOOL_MIN)
        w = SIM_LIVE_TOOL_MIN;
    if (h < SIM_LIVE_TOOL_MIN)
        h = SIM_LIVE_TOOL_MIN;

    px = sim_clampi(px, SIM_LIVE_X0, SIM_LIVE_X1);
    py = sim_clampi(py, SIM_LIVE_Y0, SIM_LIVE_Y1);
    half_w = w / 2;
    half_h = h / 2;
    cx = px + half_w;
    cy = py + half_h;

    ra_set_draw_base(dst_base);
    for (y = -half_h; y <= half_h; y++)
    {
        int yy = cy + y;
        int span = half_w - (int)(((int32_t)sim_absf((float)y) * half_w) / half_h);
        if (yy < SIM_LIVE_Y0 || yy > SIM_LIVE_Y1)
            continue;
        ra_draw_line(sim_clampi(cx - span, SIM_LIVE_X0, SIM_LIVE_X1),
                     yy,
                     sim_clampi(cx + span, SIM_LIVE_X0, SIM_LIVE_X1),
                     yy,
                     SIM_COL_TOOL);
    }
    ra_draw_line(cx - half_w, cy, cx, cy - half_h, SIM_COL_WARN);
    ra_draw_line(cx, cy - half_h, cx + half_w, cy, SIM_COL_WARN);
    ra_draw_line(cx + half_w, cy, cx, cy + half_h, SIM_COL_WARN);
    ra_draw_line(cx, cy + half_h, cx - half_w, cy, SIM_COL_WARN);
    ra_set_draw_base(old_draw_base);
}

static void sim_live_draw_marker(uint32_t dst_base, const ui_snapshot_frame_t *frame, float x_diam, float z_pos)
{
    const lcam_sim_view_t *view = &g_live_sim.view;
    int px = sim_live_z_to_px_raw(view, z_pos);
    int py = sim_live_d_to_py_raw(view, x_diam);
    int w = sim_live_marker_px(g_live_sim.doc_z_px);
    int h = sim_live_marker_px(g_live_sim.doc_x_px);
    bool is_id = sim_live_is_id_cycle(frame);
    bool chuck_warn = sim_live_tool_in_chuck(z_pos);

    if (chuck_warn && g_live_sim.setup.clamp > 0.0f)
    {
        int chuck_w = (int)(g_live_sim.setup.clamp * view->z_scale + 0.5f);
        int c = ((mcu_millis() / 250U) & 1U) ? SIM_COL_WARN : RA_RED;
        //chuck_w = sim_clampi(chuck_w, 10, 90);
        uint32_t old_draw_base = ra_get_draw_base();
        ra_set_draw_base(dst_base);
        ra_fill_rect((uint16_t)view->stock_left,
                     (uint16_t)(view->stock_bottom + 1),
                     (uint16_t)(view->stock_left + chuck_w),
                     (uint16_t)(view->stock_bottom + SIM_CHUCK_H),
                     (uint16_t)c);
        sim_textf(view->stock_left + 3, view->stock_bottom + 10, RA_BLACK, (uint16_t)c, "CL %.1f", (double)g_live_sim.setup.clamp);
        ra_set_draw_base(old_draw_base);
    }

    if (sim_live_is_threading(frame))
    {
        if (is_id)
            py -= h - 1;
        sim_live_draw_thread_marker(dst_base, px, py, w, h);
    }
    else
    {
        if (is_id)
            py -= h - 1;
        ra_blit(SIM_TOOL_ADDR,
                0,
                0,
                dst_base,
                (uint16_t)sim_clampi(px, view->x0, view->x1),
                (uint16_t)sim_clampi(py, view->y0, view->y1),
                (uint16_t)w,
                (uint16_t)h);
    }
}

void leancam_sim_live_draw(const ui_snapshot_frame_t *frame)
{
    uint32_t old_draw_base;
    float x_diam = 0.0f;
    float z_pos = 0.0f;
    float feed = 0.0f;
    float doc = 0.0f;
    unsigned spindle = 0;
    unsigned tool = 0;

    if (!frame || !frame->leancam_active)
        return;

    if (!g_live_sim.initialized || sim_live_signature_changed(frame))
        sim_live_init_canvas(frame);
    else
    {
        sim_live_update_doc_px(frame);
        sim_live_build_tool_sprite();
    }

    if (frame->axes_valid)
    {
        x_diam = sim_live_runtime_x_to_diam(frame->axis[0]);
        z_pos = frame->axis[2];
    }
    if (frame->feed_valid)
        feed = frame->feed;
    if (frame->spindle_valid)
        spindle = (unsigned)frame->spindle;
    doc = sim_live_doc_from_frame(frame);
    tool = sim_live_tool_from_frame(frame);

    old_draw_base = ra_get_draw_base();
    ra_set_draw_base(SIM_LIVE_ADDR);
    sim_live_draw_removal(frame, x_diam, z_pos);
    sim_live_draw_text(x_diam, z_pos, feed, spindle, doc, tool);
    ra_set_draw_base(old_draw_base);

    ra_blit(SIM_LIVE_ADDR,
            SIM_LIVE_X0,
            SIM_LIVE_Y0,
            RA8876_BACKBUF_ADDR,
            SIM_LIVE_X0,
            SIM_LIVE_Y0,
            SIM_LIVE_W,
            SIM_LIVE_H);

    sim_live_draw_marker(RA8876_BACKBUF_ADDR, frame, x_diam, z_pos);

    ra_blit(RA8876_BACKBUF_ADDR,
            SIM_LIVE_X0,
            SIM_LIVE_Y0,
            old_draw_base,
            SIM_LIVE_X0,
            SIM_LIVE_Y0,
            SIM_LIVE_W,
            SIM_LIVE_H);
}

void leancam_sim_preview_draw(const ui_snapshot_frame_t *frame)
{
    static uint32_t last_draw_ms = 0;
    static uint8_t last_mode = 0xff;
    static bool was_visible = false;
    static bool was_msg_visible = false;
    static char last_setup[UI_LC_LINE_LEN];
    static char last_line[UI_LC_LINE_LEN];
    static char last_tool[UI_LC_LINE_LEN];
    static char last_active_field[12];
    static char last_msg[UI_SNAPSHOT_POPUP_LEN];
    sim_setup_t setup;
    lcam_sim_view_t view;
    const char *line;
    uint32_t now;
    uint32_t old_draw_base = RA8876_VISIBLE_ADDR;
    bool mode_changed;
    bool source_changed;

    if (!frame || !frame->leancam_active)
        return;

    mode_changed = (last_mode != frame->leancam_mode);

    if (!sim_should_draw_for_mode(frame->leancam_mode))
    {
        if (frame->leancam_message[0])
        {
            if (was_visible || !was_msg_visible || mode_changed ||
                strcmp(last_msg, frame->leancam_message) != 0)
            {
                sim_begin_offscreen(&old_draw_base);
                sim_draw_status_message(frame);
                sim_present_offscreen(old_draw_base);
                ui_snapshot_strcpy(last_msg, frame->leancam_message, sizeof(last_msg));
            }

            was_msg_visible = true;
        }
        else
        {
            if (was_visible || was_msg_visible || mode_changed)
            {
                sim_begin_offscreen(&old_draw_base);
                ra_fill_rect(SIM_AREA_X0, SIM_AREA_Y0, SIM_AREA_X1, SIM_AREA_Y1, SIM_COL_BG);
                sim_present_offscreen(old_draw_base);
            }

            was_msg_visible = false;
            last_msg[0] = 0;
        }

        was_visible = false;
        last_mode = frame->leancam_mode;
        last_setup[0] = 0;
        last_line[0] = 0;
        last_tool[0] = 0;
        last_active_field[0] = 0;
        return;
    }

    source_changed = sim_preview_sources_changed(frame, last_setup, last_line, last_tool, last_active_field);
    if (!mode_changed && !source_changed)
        return;

    now = mcu_millis();
    if (!mode_changed && !source_changed &&
        last_draw_ms != 0 &&
        (uint32_t)(now - last_draw_ms) < SIM_REDRAW_MS)
        return;

    last_draw_ms = now;
    last_mode = frame->leancam_mode;
    was_visible = true;
    was_msg_visible = false;
    last_msg[0] = 0;
    sim_store_preview_sources(frame, last_setup, last_line, last_tool, last_active_field);

    sim_begin_offscreen(&old_draw_base);
    sim_read_setup(frame, &setup);
    sim_build_view(&setup, &view);
    sim_draw_stock(&view, &setup);
    sim_draw_chuck(&view, &setup);

    line = frame->leancam_preview_line;
    if (!line || !line[0] || sim_is_cycle(line, "SETUP"))
    {
        sim_present_offscreen(old_draw_base);
        return;
    }

    if (sim_is_cycle(line, "OD"))
        sim_draw_od_preview(&view, line, frame);
    else if (sim_is_cycle(line, "ID"))
        sim_draw_id_preview(&view, line, frame);
    else if (sim_is_cycle(line, "FACE"))
        sim_draw_face_preview(&view, line, frame);
    else if (sim_is_cycle(line, "DRILL"))
        sim_draw_drill_preview(&view, line, frame);
    else if (sim_is_cycle(line, "GROOVE"))
        sim_draw_groove_preview(&view, line);
    else if (sim_is_cycle(line, "THR_OD"))
        sim_draw_thread_preview(&view, line, frame, true);
    else if (sim_is_cycle(line, "THR_ID"))
        sim_draw_thread_preview(&view, line, frame, false);

    sim_present_offscreen(old_draw_base);
}
