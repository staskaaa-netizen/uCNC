#include "leancam_hdmi_renderer.h"

#include "../../cnc.h"
#include "../ui_snapshot/ui_snapshot.h"
#include "../ra8876_display/ra_leancam_table.h"
#include "leancam_display.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef LC_HDMI_RENDER_MS
#define LC_HDMI_RENDER_MS 50
#endif

#define LC_COL_BG      lc_display_rgb(0, 0, 0)
#define LC_COL_TOP     lc_display_rgb(18, 42, 54)
#define LC_COL_PANEL   lc_display_rgb(8, 18, 22)
#define LC_COL_LINE    lc_display_rgb(68, 130, 146)
#define LC_COL_TEXT    lc_display_rgb(230, 238, 228)
#define LC_COL_DIM     lc_display_rgb(122, 150, 150)
#define LC_COL_VALUE   lc_display_rgb(246, 210, 92)
#define LC_COL_HI      lc_display_rgb(255, 245, 130)
#define LC_COL_BAD     lc_display_rgb(230, 80, 70)
#define LC_COL_OK      lc_display_rgb(80, 210, 130)
#define LC_COL_STOCK   lc_display_rgb(138, 142, 140)
#define LC_COL_SELECT  lc_display_rgb(36, 54, 58)
#define LC_COL_CUT     lc_display_rgb(238, 204, 78)
#define LC_COL_HATCH   lc_display_rgb(58, 50, 22)
#define LC_COL_DARK    lc_display_rgb(4, 8, 10)

static uint32_t g_last_seq;

typedef struct {
    float length;
    float od;
    float id;
    float clamp;
    float extra;
} lc_sim_setup_t;

typedef struct {
    int x0;
    int y0;
    int x1;
    int y1;
    int stock_left;
    int stock_right;
    int stock_top;
    int stock_bottom;
    int z0_x;
    float scale;
    float stock_len;
    float stock_od;
} lc_sim_view_t;

static const char *lc_skip_line_number(const char *s)
{
    if (!s) {
        return "";
    }
    if (s[0] >= '0' && s[0] <= '9' &&
        s[1] >= '0' && s[1] <= '9' &&
        s[2] == ' ') {
        return s + 3;
    }
    return s;
}

static void lc_prefix_name(const char *line, char *out, size_t out_sz)
{
    const char *s;
    const char *bar;
    size_t n;

    if (!out || out_sz == 0) {
        return;
    }
    out[0] = '\0';
    s = lc_skip_line_number(line);
    bar = strchr(s, '|');
    if (!bar) {
        snprintf(out, out_sz, "%s", s);
        return;
    }
    n = (size_t)(bar - s);
    if (n >= out_sz) {
        n = out_sz - 1;
    }
    memcpy(out, s, n);
    out[n] = '\0';
}

static const char *lc_cycle_full_name(const char *name)
{
    if (!name || !name[0]) return "LeanCam block";
    if (!strcmp(name, "SETUP")) return "Setup";
    if (!strcmp(name, "TOOL")) return "Tool";
    if (!strcmp(name, "OD")) return "Outside diameter turning";
    if (!strcmp(name, "ID")) return "Inside diameter turning";
    if (!strcmp(name, "FACE")) return "Facing";
    if (!strcmp(name, "DRILL")) return "Drilling";
    if (!strcmp(name, "CUT")) return "Cut / slot";
    if (!strcmp(name, "CHAMFER")) return "Chamfer";
    if (!strcmp(name, "THR_OD")) return "Outside threading";
    if (!strcmp(name, "THR_ID")) return "Inside threading";
    if (!strcmp(name, "RADIUS_OD")) return "Outside radius";
    if (!strcmp(name, "RADIUS_ID")) return "Inside radius";
    if (!strcmp(name, "GROOVE")) return "Grooving";
    if (!strcmp(name, "PART")) return "Parting";
    return name;
}

static void lc_block_title(const char *line, char *out, size_t out_sz)
{
    char name[32];
    const char *s;

    if (!out || out_sz == 0) {
        return;
    }
    s = line ? line : "";
    lc_prefix_name(s, name, sizeof(name));
    if (s[0] >= '0' && s[0] <= '9' &&
        s[1] >= '0' && s[1] <= '9' &&
        s[2] == ' ') {
        snprintf(out, out_sz, "%c%c - %s", s[0], s[1], lc_cycle_full_name(name));
    } else {
        snprintf(out, out_sz, "%s", lc_cycle_full_name(name));
    }
}

static const char *lc_table_without_block_name(const char *s)
{
    const char *p;

    if (!s) {
        return "";
    }
    p = s + 10;
    while (*p == ' ') {
        p++;
    }
    return p;
}

static void lc_text_clip(int x, int y, const char *text, int cols,
                         lc_color_t fg, lc_color_t bg, int font)
{
    char buf[UI_LC_HELPER_LEN];
    int len;

    if (!text) {
        text = "";
    }
    if (cols < 1) {
        return;
    }
    len = (int)strlen(text);
    if (len > cols) {
        len = cols;
    }
    snprintf(buf, sizeof(buf), "%-*.*s", cols, len, text);
    if ((int)strlen(text) > cols && cols > 3) {
        buf[cols - 3] = '.';
        buf[cols - 2] = '.';
        buf[cols - 1] = '.';
        buf[cols] = '\0';
    }
    lc_display_text(x, y, buf, fg, bg, font);
}

static int lc_clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool lc_is_cycle(const char *line, const char *name)
{
    const char *s = lc_skip_line_number(line);
    size_t n;

    if (!s || !name) return false;
    n = strlen(name);
    return strncmp(s, name, n) == 0 && (s[n] == '|' || s[n] == 0);
}

static bool lc_get_field_text(const char *line, const char *name, char *out, size_t out_sz)
{
    const char *p;
    size_t name_len;

    if (!line || !name || !out || out_sz == 0) return false;
    out[0] = 0;
    line = lc_skip_line_number(line);
    p = line;
    name_len = strlen(name);

    while ((p = strstr(p, name)) != NULL) {
        const char *open;
        const char *close;
        size_t n;

        if ((p == line || p[-1] == '|') && p[name_len] == '{') {
            open = p + name_len + 1;
            close = strchr(open, '}');
            if (!close) return false;
            n = (size_t)(close - open);
            if (n >= out_sz) n = out_sz - 1;
            memcpy(out, open, n);
            out[n] = 0;
            return true;
        }
        p += name_len;
    }
    return false;
}

static bool lc_field_float(const char *line, const char *name, float *out)
{
    char buf[32];
    char *s;
    char *endp;

    if (!out || !lc_get_field_text(line, name, buf, sizeof(buf))) return false;
    s = buf;
    while (*s == ' ') s++;
    if (*s == '(') s++;
    if (*s == '*' || *s == 0) return false;
    *out = (float)strtod(s, &endp);
    return endp != s;
}

static bool lc_field_float2(const char *line, const char *a, const char *b, float *out)
{
    return lc_field_float(line, a, out) || lc_field_float(line, b, out);
}

static bool lc_field_float3(const char *line, const char *a, const char *b, const char *c, float *out)
{
    return lc_field_float(line, a, out) || lc_field_float(line, b, out) || lc_field_float(line, c, out);
}

static void lc_sim_read_setup(const ui_snapshot_frame_t *frame, lc_sim_setup_t *setup)
{
    const char *line = frame ? frame->leancam_setup_line : NULL;

    setup->length = 75.0f;
    setup->od = 50.0f;
    setup->id = 0.0f;
    setup->clamp = 8.0f;
    setup->extra = 3.0f;

    if (!line || !line[0]) return;
    (void)lc_field_float2(line, "L", "LENGTH", &setup->length);
    (void)lc_field_float2(line, "OD", "OUTER_DIAMETER", &setup->od);
    (void)lc_field_float2(line, "ID", "INNER_DIAMETER", &setup->id);
    (void)lc_field_float2(line, "CLAMP", "CLAMP_LENGTH", &setup->clamp);
    (void)lc_field_float2(line, "EXTRA", "EXTRA_LENGTH", &setup->extra);

    if (setup->length <= 0.0f) setup->length = 75.0f;
    if (setup->od <= 0.0f) setup->od = 50.0f;
    if (setup->id < 0.0f) setup->id = 0.0f;
    if (setup->clamp < 0.0f) setup->clamp = 0.0f;
    if (setup->extra < 0.0f) setup->extra = 0.0f;
}

static void lc_sim_build_view(const lc_sim_setup_t *setup, lc_sim_view_t *view)
{
    float visible_len = setup->length + setup->extra;
    float usable_w = 294.0f;
    float usable_h = 245.0f;
    float z_scale;
    float d_scale;
    int stock_w;
    int stock_h;

    if (visible_len <= 0.0f) visible_len = 75.0f;
    z_scale = usable_w / visible_len;
    d_scale = usable_h / (setup->od * 0.5f);
    view->scale = z_scale < d_scale ? z_scale : d_scale;
    if (view->scale <= 0.0f) view->scale = 1.0f;

    stock_w = (int)(visible_len * view->scale + 0.5f);
    stock_h = (int)((setup->od * 0.5f) * view->scale + 0.5f);
    if (stock_w < 1) stock_w = 1;
    if (stock_h < 1) stock_h = 1;
    if (stock_w > (int)usable_w) stock_w = (int)usable_w;
    if (stock_h > (int)usable_h) stock_h = (int)usable_h;

    view->x0 = 432;
    view->y0 = 104;
    view->x1 = 776;
    view->y1 = 520;
    view->stock_right = 744;
    view->stock_left = view->stock_right - stock_w;
    view->stock_top = 170;
    view->stock_bottom = view->stock_top + stock_h;
    view->z0_x = lc_clampi(view->stock_left + (int)(setup->length * view->scale + 0.5f),
                           view->stock_left, view->stock_right);
    view->stock_len = visible_len;
    view->stock_od = setup->od;
}

static int lc_sim_zx(const lc_sim_view_t *view, float z)
{
    return lc_clampi(view->z0_x + (int)(z * view->scale + (z >= 0.0f ? 0.5f : -0.5f)),
                    view->stock_left, view->stock_right);
}

static int lc_sim_dy(const lc_sim_view_t *view, float d)
{
    int y = view->stock_top + (int)((d * 0.5f) * view->scale + 0.5f);
    return lc_clampi(y, view->stock_top, view->stock_bottom);
}

static void lc_sim_label(int x, int y, const char *name, float v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %.1f", name, (double)v);
    lc_display_text(lc_clampi(x, 432, 730), lc_clampi(y, 112, 504),
                    buf, LC_COL_DIM, LC_COL_BG, LC_FONT_SMALL);
}

static void lc_sim_hatch_rect(int x, int y, int w, int h, bool vertical)
{
    int p;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    lc_display_fill_rect(x, y, w, h, LC_COL_CUT);
    lc_display_rect(x, y, w, h, LC_COL_HATCH);
    if (vertical) {
        for (p = x + 6; p < x + w; p += 6) {
            lc_display_line(p, y, p, y + h - 1, LC_COL_HATCH);
        }
    } else {
        for (p = y + 6; p < y + h; p += 6) {
            lc_display_line(x, p, x + w - 1, p, LC_COL_HATCH);
        }
    }
}

static void lc_sim_draw_stock(const lc_sim_view_t *view, const lc_sim_setup_t *setup)
{
    int clamp_w;

    lc_display_text(432, 104, "Cycle preview", LC_COL_DIM, LC_COL_BG, LC_FONT_NORMAL);
    lc_display_line(view->stock_left - 20, view->stock_top, view->stock_right + 20, view->stock_top, LC_COL_TEXT);
    lc_display_line(view->z0_x, view->stock_top - 24, view->z0_x, view->stock_bottom + 28, LC_COL_TEXT);
    lc_display_fill_rect(view->stock_left, view->stock_top,
                         view->stock_right - view->stock_left + 1,
                         view->stock_bottom - view->stock_top + 1,
                         LC_COL_STOCK);
    lc_display_rect(view->stock_left, view->stock_top,
                    view->stock_right - view->stock_left + 1,
                    view->stock_bottom - view->stock_top + 1,
                    LC_COL_TEXT);

    if (setup->clamp > 0.0f) {
        clamp_w = (int)(setup->clamp * view->scale + 0.5f);
        if (clamp_w < 6) clamp_w = 6;
        lc_display_fill_rect(view->stock_left, view->stock_bottom + 2, clamp_w, 22, LC_COL_LINE);
        lc_display_text(view->stock_left + 3, view->stock_bottom + 8, "CL", LC_COL_BG, LC_COL_LINE, LC_FONT_SMALL);
    }

    lc_display_text(view->z0_x - 8, view->stock_top - 20, "Z0", LC_COL_DIM, LC_COL_BG, LC_FONT_SMALL);
    lc_sim_label(view->stock_left, view->stock_bottom + 34, "L", setup->length);
    lc_sim_label(view->stock_left + 74, view->stock_bottom + 34, "OD", setup->od);
}

static void lc_sim_draw_turn(const lc_sim_view_t *view, const char *line, bool is_od)
{
    float d1, d2, dt, z1, z2;
    int x1, x2, y1, y2, yt, tmp;

    if (!lc_field_float2(line, "D1", "DIAMETER_1", &d1)) return;
    if (!lc_field_float2(line, "D2", "DIAMETER_2", &d2)) return;
    if (!lc_field_float2(line, "Z1", "Z_1", &z1)) return;
    if (!lc_field_float2(line, "Z2", "Z_2", &z2)) return;
    if (!lc_field_float2(line, "DT", "D_TARGET", &dt)) dt = d2;

    x1 = lc_sim_zx(view, z1);
    x2 = lc_sim_zx(view, z2);
    y1 = lc_sim_dy(view, d1);
    y2 = lc_sim_dy(view, d2);
    yt = lc_sim_dy(view, dt);
    if (x2 < x1) { tmp = x1; x1 = x2; x2 = tmp; }
    if (x2 <= x1) x2 = x1 + 1;

    if (is_od) {
        lc_sim_hatch_rect(x1, y2, x2 - x1 + 1, y1 - y2 + 1, true);
    } else {
        lc_sim_hatch_rect(x1, y1, x2 - x1 + 1, y2 - y1 + 1, true);
    }
    lc_display_line_w(lc_sim_zx(view, z1), lc_sim_dy(view, d1),
                      lc_sim_zx(view, z2), lc_sim_dy(view, d2), LC_COL_BAD, 2);
    lc_display_line(lc_sim_zx(view, z1), yt, lc_sim_zx(view, z2), yt, LC_COL_OK);
    lc_sim_label(x1 - 16, y1 + 8, "D1", d1);
    lc_sim_label(x2 - 42, y2 + 8, "D2", d2);
}

static void lc_sim_draw_preview(const ui_snapshot_frame_t *frame)
{
    lc_sim_setup_t setup;
    lc_sim_view_t view;
    const char *line;

    lc_sim_read_setup(frame, &setup);
    lc_sim_build_view(&setup, &view);
    lc_sim_draw_stock(&view, &setup);

    line = frame->leancam_preview_line;
    if (!line || !line[0] || lc_is_cycle(line, "SETUP")) return;

    if (lc_is_cycle(line, "OD")) {
        lc_sim_draw_turn(&view, line, true);
    } else if (lc_is_cycle(line, "ID")) {
        lc_sim_draw_turn(&view, line, false);
    } else if (lc_is_cycle(line, "FACE")) {
        float d, z1 = 0.0f, z;
        int x1, x2, y2, tmp;
        if (!lc_field_float3(line, "D", "OD", "OUTER_DIAMETER", &d)) d = setup.od;
        (void)lc_field_float2(line, "Z1", "Z_1", &z1);
        if (!lc_field_float2(line, "Z", "Z_2", &z)) return;
        x1 = lc_sim_zx(&view, z1);
        x2 = lc_sim_zx(&view, z);
        if (x2 < x1) { tmp = x1; x1 = x2; x2 = tmp; }
        y2 = lc_sim_dy(&view, d);
        lc_sim_hatch_rect(x1, view.stock_top, x2 - x1 + 1, y2 - view.stock_top + 1, true);
        lc_sim_label(x2 + 4, view.stock_top + 8, "Z", z);
    } else if (lc_is_cycle(line, "DRILL")) {
        float z1, depth, target, td = setup.od * 0.12f;
        int x1, x2, y2, tmp;
        if (!lc_field_float2(line, "Z1", "Z_START", &z1)) return;
        if (!lc_field_float(line, "DEPTH", &depth)) return;
        (void)lc_field_float3(frame->leancam_tool_line, "D", "TD", "TOOL_DIA", &td);
        target = depth <= 0.0f ? depth : z1 - depth;
        x1 = lc_sim_zx(&view, z1);
        x2 = lc_sim_zx(&view, target);
        if (x2 < x1) { tmp = x1; x1 = x2; x2 = tmp; }
        y2 = lc_sim_dy(&view, td);
        lc_sim_hatch_rect(x1, view.stock_top, x2 - x1 + 1, y2 - view.stock_top + 1, false);
        lc_sim_label(x2 - 34, y2 + 8, "Z", target);
    } else if (lc_is_cycle(line, "GROOVE") || lc_is_cycle(line, "PART") || lc_is_cycle(line, "CUT")) {
        float d1 = setup.od, d2 = 0.0f, z1, z2, width;
        int x1, x2, y1, y2, tmp;
        (void)lc_field_float(line, "D1", &d1);
        (void)lc_field_float(line, "D2", &d2);
        if (!lc_field_float2(line, "Z1", "Z", &z1)) return;
        if (!lc_field_float(line, "Z2", &z2)) {
            if (!lc_field_float(line, "WIDTH", &width)) width = 3.0f;
            z2 = z1 - width;
        }
        x1 = lc_sim_zx(&view, z1);
        x2 = lc_sim_zx(&view, z2);
        y1 = lc_sim_dy(&view, d1);
        y2 = lc_sim_dy(&view, d2);
        if (x2 < x1) { tmp = x1; x1 = x2; x2 = tmp; }
        if (y2 < y1) { tmp = y1; y1 = y2; y2 = tmp; }
        lc_sim_hatch_rect(x1, y1, x2 - x1 + 1, y2 - y1 + 1, true);
    } else if (lc_is_cycle(line, "THR_OD") || lc_is_cycle(line, "THR_ID")) {
        float z1, z2, pitch = 1.5f, d = setup.od;
        int x1, x2, y, p, tmp;
        if (!lc_field_float2(line, "Z1", "Z_START", &z1)) return;
        if (!lc_field_float2(line, "Z2", "Z_END", &z2)) return;
        (void)lc_field_float3(line, "P", "PITCH", "K", &pitch);
        (void)lc_field_float3(line, "M", "D", "OD", &d);
        x1 = lc_sim_zx(&view, z1);
        x2 = lc_sim_zx(&view, z2);
        if (x2 < x1) { tmp = x1; x1 = x2; x2 = tmp; }
        y = lc_sim_dy(&view, d);
        lc_display_line_w(x1, y, x2, y, LC_COL_CUT, 3);
        for (p = x1; p < x2; p += lc_clampi((int)(pitch * view.scale + 0.5f), 5, 18)) {
            lc_display_line(p, y - 8, p + 8, y + 8, LC_COL_DARK);
        }
        lc_sim_label(x1, y + 16, "P", pitch);
    }
}

static const char *exec_state_text(uint8_t state)
{
    switch (state) {
        case EXEC_HOLD: return "HOLD";
        case EXEC_HOMING: return "HOME";
        case EXEC_JOG: return "JOG";
        case EXEC_RUN: return "RUN";
        case EXEC_LIMITS:
        case EXEC_POSITION_MAYBE_LOST: return "ALARM";
        default: return "IDLE";
    }
}

static void draw_bar(const ui_snapshot_frame_t *frame)
{
    char buf[96];
    float x = 0.0f;
    float z = 0.0f;
    float feed = 0.0f;
    unsigned spindle = 0;

    if (frame && frame->axes_valid) {
        x = frame->axis[0];
        z = frame->axis[2];
    }
    if (frame && frame->feed_valid) {
        feed = frame->feed;
    }
    if (frame && frame->spindle_valid) {
        spindle = (unsigned)frame->spindle;
    }

    lc_display_fill_rect(0, 0, LC_DISPLAY_WIDTH, 42, LC_COL_TOP);
    lc_display_text(12, 10, "LeanCam", LC_COL_TEXT, LC_COL_TOP, LC_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "%-5s X:%7.3f Z:%7.3f F:%5.1f S:%u",
             frame ? exec_state_text(frame->state) : "BOOT",
             (double)x, (double)z, (double)feed, spindle);
    lc_display_text(142, 10, buf, LC_COL_TEXT, LC_COL_TOP, LC_FONT_NORMAL);
}

static void draw_value_with_highlight(int x, int y, const char *value,
                                      bool has_hi, uint8_t hi_start, uint8_t hi_end,
                                      int clear_cols, lc_color_t bg)
{
    char left[UI_LC_LINE_LEN];
    char mid[UI_LC_LINE_LEN];
    char right[UI_LC_LINE_LEN];
    int len;

    if (!value) {
        value = "";
    }
    len = (int)strlen(value);
    if (len > clear_cols) {
        len = clear_cols;
    }

    if (!has_hi || hi_end <= hi_start || hi_start >= (uint8_t)len) {
        char line[UI_LC_LINE_LEN];
        snprintf(line, sizeof(line), "%-*.*s", clear_cols, clear_cols, value);
        lc_display_text(x, y, line, LC_COL_VALUE, bg, LC_FONT_NORMAL);
        return;
    }

    if (hi_end > (uint8_t)len) {
        hi_end = (uint8_t)len;
    }

    snprintf(left, sizeof(left), "%.*s", (int)hi_start, value);
    snprintf(mid, sizeof(mid), "%.*s", (int)(hi_end - hi_start), value + hi_start);
    snprintf(right, sizeof(right), "%-*.*s",
             clear_cols - (int)hi_end, clear_cols - (int)hi_end, value + hi_end);

    lc_display_text(x, y, left, LC_COL_VALUE, bg, LC_FONT_NORMAL);
    lc_display_text(x + ((int)hi_start * 8), y, mid, LC_COL_BG, LC_COL_HI, LC_FONT_NORMAL);
    lc_display_text(x + ((int)hi_end * 8), y, right, LC_COL_VALUE, bg, LC_FONT_NORMAL);
}

static void draw_leancam_rows(const ui_snapshot_frame_t *frame)
{
    int i;
    int y = 90;
    int max_blocks = 6;

    lc_display_fill_rect(0, 42, LC_DISPLAY_WIDTH, 558, LC_COL_BG);

    lc_display_rect(10, 58, 392, 490, LC_COL_LINE);
    lc_display_rect(418, 58, 372, 490, LC_COL_LINE);
    lc_text_clip(24, 64, frame->leancam_title[0] ? frame->leancam_title : "LeanCam",
                 28, LC_COL_TEXT, LC_COL_BG, LC_FONT_NORMAL);
    lc_text_clip(432, 64, "Preview", 20, LC_COL_TEXT, LC_COL_BG, LC_FONT_NORMAL);
    lc_text_clip(184, 64, frame->leancam_message, 25, LC_COL_VALUE, LC_COL_BG, LC_FONT_NORMAL);

    for (i = 0; i < frame->leancam_line_count && i < UI_LC_MAX_LINES && max_blocks > 0; ++i) {
        ra_lc_table_line_t tl;
        lc_color_t fg = frame->leancam_line_selected[i] ? LC_COL_HI : LC_COL_TEXT;
        char title[64];

        ra_lc_build_table_line(frame, i, &tl);
        if (tl.table_like) {
            lc_color_t row_bg = frame->leancam_line_selected[i] ? LC_COL_SELECT : LC_COL_BG;
            lc_block_title(frame->leancam_lines[i], title, sizeof(title));
            if (frame->leancam_line_selected[i]) {
                lc_display_fill_rect(18, y - 3, 376, 82, row_bg);
                lc_display_rect(18, y - 3, 376, 82, LC_COL_HI);
            }
            lc_text_clip(24, y, title, 45, fg, row_bg, LC_FONT_NORMAL);
            y += 22;
            lc_text_clip(24, y, lc_table_without_block_name(tl.header), 45, fg, row_bg, LC_FONT_NORMAL);
            y += 24;
            draw_value_with_highlight(24, y, lc_table_without_block_name(tl.value),
                                      tl.has_hi,
                                      tl.hi_start > 11 ? (uint8_t)(tl.hi_start - 11) : 0,
                                      tl.hi_end > 11 ? (uint8_t)(tl.hi_end - 11) : 0,
                                      45,
                                      row_bg);
            y += 34;
            lc_display_line(24, y - 10, 388, y - 10, LC_COL_PANEL);
            max_blocks--;
        } else {
            lc_text_clip(24, y, frame->leancam_lines[i], 45, fg, LC_COL_BG, LC_FONT_NORMAL);
            y += 30;
            max_blocks--;
        }
    }

    lc_sim_draw_preview(frame);
    lc_text_clip(432, 522, frame->leancam_active_field,
                 34, LC_COL_VALUE, LC_COL_BG, LC_FONT_SMALL);
    lc_display_text(12, 566, frame->leancam_helper, LC_COL_DIM, LC_COL_BG, LC_FONT_SMALL);
}

void leancam_hdmi_renderer_init(void)
{
    g_last_seq = 0;
    lc_display_clear(LC_COL_BG);
    draw_bar(NULL);
    lc_display_text(24, 82, "Waiting for LeanCam snapshot", LC_COL_TEXT, LC_COL_BG, LC_FONT_NORMAL);
    lc_display_present();
}

void leancam_hdmi_renderer_poll(void)
{
    static ui_snapshot_frame_t frame;
    static uint32_t last_ms;
    uint32_t seq = 0;
    uint32_t now = mcu_millis();

    if ((uint32_t)(now - last_ms) < LC_HDMI_RENDER_MS) {
        return;
    }
    last_ms = now;

    if (!ui_snapshot_copy_latest(&g_ui_snapshot, &frame, &seq)) {
        return;
    }

    if (!ui_snapshot_has_newer_seq(seq, g_last_seq)) {
        return;
    }

    draw_bar(&frame);
    draw_leancam_rows(&frame);
    lc_display_present();
    g_last_seq = seq;
}

void leancam_hdmi_renderer_set_keyboard_connected(bool connected)
{
    (void)connected;
}
