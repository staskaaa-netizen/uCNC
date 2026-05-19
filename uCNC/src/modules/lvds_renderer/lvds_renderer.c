#include "lvds_renderer.h"

#include "../../cnc.h"
#include "../ui_snapshot/ui_snapshot.h"
#include "lvds_hstx.h"
#include "lvds_leancam_table.h"
#include "lvds_renderer_state.h"
#include "lvds_palette.h"
#include "lvds_psram.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifndef LVDS_RENDERER_MS
#define LVDS_RENDERER_MS 20
#endif

#define LC_RENDER_MODE_FILES 0
#define LC_RENDER_MODE_NC_VIEW 4

#define LC_COL_BG      lvds_palette_element(LC_ELEM_BACKGROUND)
#define LC_COL_TOP     lvds_palette_element(LC_ELEM_HEADER)
#define LC_COL_PANEL   lvds_palette_element(LC_ELEM_PANEL)
#define LC_COL_LINE    lvds_palette_element(LC_ELEM_BORDER)
#define LC_COL_TEXT    lvds_palette_element(LC_ELEM_TEXT)
#define LC_COL_DIM     lvds_palette_element(LC_ELEM_SECONDARY_TEXT)
#define LC_COL_VALUE   lvds_palette_element(LC_ELEM_VALUE_TEXT)
#define LC_COL_HI      lvds_palette_element(LC_ELEM_SELECTED_TEXT)
#define LC_COL_BAD     lvds_palette_element(LC_ELEM_ERROR)
#define LC_COL_OK      lvds_palette_element(LC_ELEM_OK)
#define LC_COL_STOCK   lvds_palette_element(LC_ELEM_STOCK)
#define LC_COL_SELECT  lvds_palette_element(LC_ELEM_SELECTED_ROW)
#define LC_COL_FOOTER_BG    lvds_palette_element(LC_ELEM_FOOTER_BG)
#define LC_COL_FOOTER_TEXT  lvds_palette_element(LC_ELEM_FOOTER_TEXT)
#define LC_COL_FOOTER_VALUE lvds_palette_element(LC_ELEM_FOOTER_VALUE)
#define LC_COL_CUT     lvds_palette_element(LC_ELEM_CUT)
#define LC_COL_HATCH   lvds_palette_element(LC_ELEM_HATCH)
#define LC_COL_DARK    lvds_palette_element(LC_ELEM_TOOL)

#define LC_PREVIEW_BG               lvds_palette_element(LC_ELEM_PREVIEW_BG)
#define LC_PREVIEW_PANEL_BG         lvds_palette_element(LC_ELEM_PREVIEW_BG)
#define LC_PREVIEW_FRAME_FG         lvds_palette_element(LC_ELEM_PREVIEW_FRAME)
#define LC_PREVIEW_TITLE_FG         lvds_palette_element(LC_ELEM_PREVIEW_TEXT)
#define LC_PREVIEW_LABEL_FG         lvds_palette_element(LC_ELEM_PREVIEW_DIM_TEXT)
#define LC_PREVIEW_VALUE_FG         lvds_palette_element(LC_ELEM_PREVIEW_DIM_TEXT)
#define LC_PREVIEW_ACTIVE_VALUE_FG  lvds_palette_element(LC_ELEM_PREVIEW_ACTIVE_TEXT)
#define LC_PREVIEW_ACTIVE_VALUE_BG  lvds_palette_element(LC_ELEM_PREVIEW_ACTIVE_BG)
#define LC_PREVIEW_MESSAGE_FG       lvds_palette_element(LC_ELEM_PREVIEW_VALUE_TEXT)
#define LC_PREVIEW_STOCK_FG         lvds_palette_element(LC_ELEM_PREVIEW_STOCK)
#define LC_PREVIEW_AXIS_FG          lvds_palette_element(LC_ELEM_PREVIEW_TEXT)
#define LC_PREVIEW_CHUCK_FG         lvds_palette_element(LC_ELEM_PREVIEW_CHUCK)
#define LC_PREVIEW_CHUCK_TEXT_FG    lvds_palette_element(LC_ELEM_PREVIEW_CHUCK_TEXT)
#define LC_PREVIEW_CUT_FG           lvds_palette_element(LC_ELEM_PREVIEW_CUT)
#define LC_PREVIEW_HATCH_FG         lvds_palette_element(LC_ELEM_PREVIEW_HATCH)
#define LC_PREVIEW_PROFILE_FG       lvds_palette_element(LC_ELEM_PREVIEW_PROFILE)
#define LC_PREVIEW_TOOL_FG          lvds_palette_element(LC_ELEM_PREVIEW_TOOL)
#define LC_PREVIEW_TOOL_MARK        lvds_palette_element(LC_ELEM_PREVIEW_TOOL_MARK)
#define LC_PREVIEW_TOOL_OUTLINE     lvds_palette_element(LC_ELEM_PREVIEW_TOOL_OUTLINE)
#define LC_PREVIEW_ERROR_FG         lvds_palette_element(LC_ELEM_ERROR)

#define LC_LIVE_BG                  lvds_palette_element(LC_ELEM_LIVE_BG)
#define LC_LIVE_PANEL_BG            lvds_palette_element(LC_ELEM_LIVE_BG)
#define LC_LIVE_FRAME_FG            lvds_palette_element(LC_ELEM_LIVE_FRAME)
#define LC_LIVE_TITLE_FG            lvds_palette_element(LC_ELEM_LIVE_TEXT)
#define LC_LIVE_LABEL_FG            lvds_palette_element(LC_ELEM_LIVE_TEXT)
#define LC_LIVE_VALUE_FG            lvds_palette_element(LC_ELEM_LIVE_VALUE_TEXT)
#define LC_LIVE_DEBUG_FG            lvds_palette_element(LC_ELEM_LIVE_DEBUG_TEXT)
#define LC_LIVE_STOCK_FG            lvds_palette_element(LC_ELEM_LIVE_STOCK)
#define LC_LIVE_AXIS_FG             lvds_palette_element(LC_ELEM_LIVE_AXIS)
#define LC_LIVE_TOOL_FG             lvds_palette_element(LC_ELEM_LIVE_TOOL)
#define LC_LIVE_TOOL_MARK           lvds_palette_element(LC_ELEM_LIVE_TOOL_MARK)
#define LC_LIVE_TOOL_OUTLINE        lvds_palette_element(LC_ELEM_LIVE_TOOL_OUTLINE)
#define LC_LIVE_CHUCK_FG            lvds_palette_element(LC_ELEM_LIVE_CHUCK)
#define LC_LIVE_CHUCK_OUTLINE       lvds_palette_element(LC_ELEM_LIVE_CHUCK_OUTLINE)
#define LC_LIVE_COLLISION           lvds_palette_element(LC_ELEM_LIVE_COLLISION)

#define LC_LIVE_SIM_W 680
#define LC_LIVE_SIM_H 380
#define LC_LIVE_SIM_PSRAM_OFFSET (512u * 1024u)
#define LC_PREVIEW_LABEL_FONT LVDS_FONT_NORMAL
#define LC_PREVIEW_LABEL_H 18
#define LC_LEFT_PANE_X 10
#define LC_LEFT_PANE_W 392
#define LC_RIGHT_PANE_X 412
#define LC_RIGHT_PANE_W 372
#define LC_TEXT_X (LC_RIGHT_PANE_X + 14)
#define LC_PREVIEW_X (LC_LEFT_PANE_X + 14)
#define LC_RENDER_MODE_DRAFT 3

#ifndef lvds_renderer_LIVE_RT_X_RADIUS
#define lvds_renderer_LIVE_RT_X_RADIUS 1
#endif

static uint32_t g_last_seq;
static uint8_t *g_live_sim_mask;
static bool g_live_sim_ready;
static bool g_live_sim_was_running;
static char g_live_sim_line[UI_LC_LINE_LEN];
static char g_live_sim_setup_line[UI_LC_LINE_LEN];
static float g_live_sim_last_x;
static float g_live_sim_last_z;
static bool g_live_sim_has_last;
static bool g_live_sim_static_drawn;
static bool g_live_tool_rect_valid;
static int g_live_tool_rect_x;
static int g_live_tool_rect_y;
static int g_live_tool_rect_w;
static int g_live_tool_rect_h;
static uint32_t g_render_last_us;
static uint32_t g_render_period_us;
static uint32_t g_live_prof_clear_us;
static uint32_t g_live_prof_material_us;
static uint32_t g_live_prof_overlay_us;
static uint32_t g_live_prof_footer_us;
static uint32_t g_live_prof_present_us;
static uint32_t g_live_prof_material_rects;
static uint32_t g_prof_header_us;
static uint32_t g_prof_clear_us;
static uint32_t g_prof_rows_us;
static uint32_t g_prof_preview_us;
static uint32_t g_prof_footer_us;
static uint16_t g_render_fps_x10;
static bool g_live_chuck_collision_last;
static bool g_live_chuck_collision_valid;
static ui_snapshot_frame_t g_normal_body_frame;
static bool g_normal_body_valid;

static void draw_perf_meter(int x, int y, lvds_color_t fg, lvds_color_t bg);
static void draw_block_meter(int x, int y, lvds_color_t fg, lvds_color_t bg);

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

typedef enum {
    LC_SIM_CORNER_NONE = 0,
    LC_SIM_CORNER_RND,
    LC_SIM_CORNER_CHMF
} lc_sim_corner_kind_t;

typedef struct {
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
    lc_sim_corner_kind_t corner;
} lc_sim_turn_shape_t;

static lc_sim_setup_t g_live_sim_setup;
static lc_sim_view_t g_live_sim_view;


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
                         lvds_color_t fg, lvds_color_t bg, int font)
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
    lvds_hstx_text(x, y, buf, fg, bg, font);
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

static void lc_sim_build_view(const lc_sim_setup_t *setup, lc_sim_view_t *view, bool full_screen)
{
    float visible_len = setup->length + setup->extra;
    float usable_w = full_screen ? 680.0f : 294.0f;
    float usable_h = full_screen ? 380.0f : 245.0f;
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

    view->x0 = full_screen ? 24 : LC_PREVIEW_X;
    view->y0 = full_screen ? 92 : 104;
    view->x1 = full_screen ? 776 : (LC_LEFT_PANE_X + LC_LEFT_PANE_W - 14);
    view->y1 = full_screen ? 548 : 520;
    if (full_screen) {
        view->stock_left = 20;
        view->stock_right = view->stock_left + stock_w;
        if (view->stock_right > view->x1 - 2) {
            view->stock_right = view->x1 - 2;
            view->stock_left = view->stock_right - stock_w;
        }
    } else {
        view->stock_left = LC_LEFT_PANE_X + 20;
        view->stock_right = view->stock_left + stock_w;
        if (view->stock_right > view->x1 - 2) {
            view->stock_right = view->x1 - 2;
            view->stock_left = view->stock_right - stock_w;
        }
    }
    view->stock_top = full_screen ? 126 : 170;
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

static int lc_sim_zx_view(const lc_sim_view_t *view, float z)
{
    return lc_clampi(view->z0_x + (int)(z * view->scale + (z >= 0.0f ? 0.5f : -0.5f)),
                    view->x0 + 2, view->x1 - 2);
}

static int lc_sim_dy(const lc_sim_view_t *view, float d)
{
    int y = view->stock_top + (int)((d * 0.5f) * view->scale + 0.5f);
    return lc_clampi(y, view->stock_top, view->stock_bottom);
}

static int lc_sim_dy_view(const lc_sim_view_t *view, float d)
{
    int y = view->stock_top + (int)((d * 0.5f) * view->scale + 0.5f);
    return lc_clampi(y, view->y0 + 2, view->y1 - 2);
}

static int lc_sim_diam_len_px(const lc_sim_view_t *view, float d)
{
    int px;

    if (!view) {
        return 0;
    }
    if (d < 0.0f) {
        d = -d;
    }
    px = (int)((d * 0.5f) * view->scale + 0.5f);
    return px < 1 ? 1 : px;
}

static float lc_lerpf(float a, float b, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return a + ((b - a) * t);
}

static float lc_absf(float v)
{
    return v < 0.0f ? -v : v;
}

static bool lc_sim_active_field_is(const ui_snapshot_frame_t *frame, const char *name)
{
    if (!frame || !name || !frame->leancam_active_field[0]) {
        return false;
    }
    if (strcmp(frame->leancam_active_field, name) == 0) {
        return true;
    }
    if ((strcmp(name, "OD") == 0 || strcmp(name, "ID") == 0) &&
        (strcmp(frame->leancam_active_field, "M") == 0 ||
         strcmp(frame->leancam_active_field, "D") == 0 ||
         strcmp(frame->leancam_active_field, "D1") == 0)) {
        return true;
    }
    if (strcmp(name, "Z2") == 0 && strcmp(frame->leancam_active_field, "Z") == 0) {
        return true;
    }
    if (strcmp(name, "D1") == 0 &&
        (strcmp(frame->leancam_active_field, "D") == 0 ||
         strcmp(frame->leancam_active_field, "OD") == 0)) {
        return true;
    }
    return false;
}

static void lc_sim_value_label_at(const lc_sim_view_t *view,
                                  const ui_snapshot_frame_t *frame,
                                  int x,
                                  int y,
                                  const char *name,
                                  float value)
{
    char value_text[18];
    int name_w;
    int value_w;
    int tx;
    int ty;
    uint16_t value_fg;
    uint16_t value_bg;

    if (!view || !name) {
        return;
    }

    snprintf(value_text, sizeof(value_text), "%.2f", (double)value);
    name_w = lvds_hstx_text_width(name, LC_PREVIEW_LABEL_FONT) + lvds_hstx_text_width(" ", LC_PREVIEW_LABEL_FONT);
    value_w = lvds_hstx_text_width(value_text, LC_PREVIEW_LABEL_FONT);
    tx = lc_clampi(x, view->x0 + 2, view->x1 - name_w - value_w - 2);
    ty = lc_clampi(y, view->y0 + 2, view->y1 - LC_PREVIEW_LABEL_H);

    lvds_hstx_text(tx, ty, name, LC_PREVIEW_LABEL_FG, LC_PREVIEW_PANEL_BG, LC_PREVIEW_LABEL_FONT);
    value_fg = lc_sim_active_field_is(frame, name) ? LC_PREVIEW_ACTIVE_VALUE_FG : LC_PREVIEW_VALUE_FG;
    value_bg = lc_sim_active_field_is(frame, name) ? LC_PREVIEW_ACTIVE_VALUE_BG : LC_PREVIEW_PANEL_BG;
    lvds_hstx_text(tx + name_w, ty, value_text, value_fg, value_bg, LC_PREVIEW_LABEL_FONT);
}

static void lc_sim_draw_turn_start_group(const lc_sim_view_t *view,
                                         const ui_snapshot_frame_t *frame,
                                         int anchor_x,
                                         int anchor_y,
                                         float z1,
                                         float d1)
{
    int x = anchor_x - 62;
    int y = anchor_y - 36;

    lc_sim_value_label_at(view, frame, x, y, "Z1", z1);
    lc_sim_value_label_at(view, frame, x, y + LC_PREVIEW_LABEL_H, "D1", d1);
}

static void lc_sim_draw_turn_dt_label(const lc_sim_view_t *view,
                                      const ui_snapshot_frame_t *frame,
                                      int anchor_x,
                                      int anchor_y,
                                      float dt)
{
    lc_sim_value_label_at(view, frame, anchor_x - 62, anchor_y - 15, "DT", dt);
}

static void lc_sim_draw_turn_end_group(const lc_sim_view_t *view,
                                       const ui_snapshot_frame_t *frame,
                                       int anchor_x,
                                       int anchor_y,
                                       float z2,
                                       float d2,
                                       const char *corner_name,
                                       float corner_amount)
{
    char z2_text[18];
    char d2_text[18];
    char corner_text[18];
    int has_corner = corner_name && corner_name[0] && corner_amount > 0.0f;
    int lines = has_corner ? 3 : 2;
    int max_w;
    int x;
    int y = anchor_y - ((lines * LC_PREVIEW_LABEL_H) + 8);

    snprintf(z2_text, sizeof(z2_text), "%.2f", (double)z2);
    snprintf(d2_text, sizeof(d2_text), "%.2f", (double)d2);
    snprintf(corner_text, sizeof(corner_text), "%.2f", (double)corner_amount);
    max_w = lvds_hstx_text_width("Z2 ", LC_PREVIEW_LABEL_FONT) + lvds_hstx_text_width(z2_text, LC_PREVIEW_LABEL_FONT);
    max_w = lvds_hstx_text_width("D2 ", LC_PREVIEW_LABEL_FONT) + lvds_hstx_text_width(d2_text, LC_PREVIEW_LABEL_FONT) > max_w ?
            lvds_hstx_text_width("D2 ", LC_PREVIEW_LABEL_FONT) + lvds_hstx_text_width(d2_text, LC_PREVIEW_LABEL_FONT) : max_w;
    if (has_corner) {
        int cw = lvds_hstx_text_width(corner_name, LC_PREVIEW_LABEL_FONT) +
                 lvds_hstx_text_width(" ", LC_PREVIEW_LABEL_FONT) +
                 lvds_hstx_text_width(corner_text, LC_PREVIEW_LABEL_FONT);
        if (cw > max_w) max_w = cw;
    }
    x = anchor_x - max_w - 8;
    if (y < view->y0 + 2) y = view->y0 + 2;

    if (has_corner) {
        lc_sim_value_label_at(view, frame, x, y, corner_name, corner_amount);
        y += LC_PREVIEW_LABEL_H;
    }
    lc_sim_value_label_at(view, frame, x, y, "Z2", z2);
    lc_sim_value_label_at(view, frame, x, y + LC_PREVIEW_LABEL_H, "D2", d2);
}

static int lc_live_sim_mx(const lc_sim_view_t *view, float z)
{
    int px = lc_sim_zx_view(view, z) - view->stock_left;
    return lc_clampi(px, 0, LC_LIVE_SIM_W - 1);
}

static int lc_live_sim_my(const lc_sim_view_t *view, float d)
{
    int py = lc_sim_dy_view(view, d) - view->stock_top;
    return lc_clampi(py, 0, LC_LIVE_SIM_H - 1);
}

static bool lc_live_sim_running(const ui_snapshot_frame_t *frame)
{
    //todo add here real screen switch selection matrix. ie this false does allow to see tool movenet on sim preview without removing stock. nice feature. 
    //return false ; 
    return frame && (frame->motion_active || (frame->state & (EXEC_RUN | EXEC_HOLD)));
}

static float lc_live_runtime_x_to_diam(float runtime_x)
{
    float x = runtime_x < 0.0f ? -runtime_x : runtime_x;
#if lvds_renderer_LIVE_RT_X_RADIUS
    return x * 2.0f;
#else
    return x;
#endif
}

static bool lc_live_sim_is_id_cycle(const ui_snapshot_frame_t *frame)
{
    const char *line = frame ? frame->leancam_preview_line : NULL;
    return lc_is_cycle(line, "ID") || lc_is_cycle(line, "THR_ID");
}

static void lc_sim_draw_face_start_group(const lc_sim_view_t *view,
                                         const ui_snapshot_frame_t *frame,
                                         int anchor_x,
                                         int anchor_y,
                                         float z1,
                                         float d)
{
    int x = anchor_x - 62;
    int y = anchor_y - 36;

    lc_sim_value_label_at(view, frame, x, y, "Z1", z1);
    lc_sim_value_label_at(view, frame, x, y + LC_PREVIEW_LABEL_H, "D1", d);
}

static void lc_sim_draw_face_end_group(const lc_sim_view_t *view,
                                       const ui_snapshot_frame_t *frame,
                                       int anchor_x,
                                       int anchor_y,
                                       float z,
                                       float d)
{
    (void)d;
    lc_sim_value_label_at(view, frame, anchor_x - 62, view->stock_top - 24, "Z2", z);
}

static bool lc_live_sim_is_face_cycle(const ui_snapshot_frame_t *frame)
{
    const char *line = frame ? frame->leancam_preview_line : NULL;
    return lc_is_cycle(line, "FACE");
}

static int lc_live_doc_px(const ui_snapshot_frame_t *frame, const lc_sim_view_t *view, float scale_factor)
{
    float doc = 1.0f;
    const char *line = frame ? frame->leancam_preview_line : NULL;

    if (!lc_field_float3(line, "DOC", "R_DOC", "ROUGH_DOC", &doc) &&
        frame && !lc_field_float3(frame->leancam_tool_line, "R_DOC", "ROUGH_DOC", "ROUGH_DEPTH_OF_CUT", &doc)) {
        doc = 1.0f;
    }
    if (doc < 0.2f) {
        doc = 0.2f;
    }
    return lc_clampi((int)((doc * view->scale * scale_factor) + 1.5f), 4, 48);
}

static void lc_live_sim_reset(const ui_snapshot_frame_t *frame)
{
    if (!g_live_sim_mask || !frame) {
        g_live_sim_ready = false;
        return;
    }

    lc_sim_read_setup(frame, &g_live_sim_setup);
    lc_sim_build_view(&g_live_sim_setup, &g_live_sim_view, true);
    memset(g_live_sim_mask, 0, LC_LIVE_SIM_W * LC_LIVE_SIM_H);

    int material_top = g_live_sim_setup.id > 0.0f ? lc_live_sim_my(&g_live_sim_view, g_live_sim_setup.id) : 0;
    for (int y = material_top; y <= g_live_sim_view.stock_bottom - g_live_sim_view.stock_top && y < LC_LIVE_SIM_H; ++y) {
        memset(g_live_sim_mask + (y * LC_LIVE_SIM_W), 1,
               (size_t)(g_live_sim_view.stock_right - g_live_sim_view.stock_left + 1));
    }

    ui_snapshot_strcpy(g_live_sim_line, frame->leancam_preview_line, sizeof(g_live_sim_line));
    ui_snapshot_strcpy(g_live_sim_setup_line, frame->leancam_setup_line, sizeof(g_live_sim_setup_line));
    g_live_sim_has_last = false;
    g_live_sim_static_drawn = false;
    g_live_tool_rect_valid = false;
    g_live_chuck_collision_valid = false;
    g_live_sim_ready = true;
}

static void lc_live_sim_force_redraw(void)
{
    g_live_sim_has_last = false;
    g_live_sim_static_drawn = false;
    g_live_tool_rect_valid = false;
    g_live_chuck_collision_valid = false;
}

static void lc_live_sim_remove_rect(int x1, int y1, int x2, int y2)
{
    int tmp;

    if (!g_live_sim_mask || !g_live_sim_ready) {
        return;
    }

    if (x2 < x1) { tmp = x1; x1 = x2; x2 = tmp; }
    if (y2 < y1) { tmp = y1; y1 = y2; y2 = tmp; }

    x1 = lc_clampi(x1, 0, LC_LIVE_SIM_W - 1);
    x2 = lc_clampi(x2, 0, LC_LIVE_SIM_W - 1);
    y1 = lc_clampi(y1, 0, LC_LIVE_SIM_H - 1);
    y2 = lc_clampi(y2, 0, LC_LIVE_SIM_H - 1);

    for (int y = y1; y <= y2; ++y) {
        memset(g_live_sim_mask + (y * LC_LIVE_SIM_W) + x1, 0, (size_t)(x2 - x1 + 1));
    }
}

static void lc_live_sim_cut_swept_rect(const ui_snapshot_frame_t *frame, float x0, float z0, float x1, float z1)
{
    float d0 = lc_live_runtime_x_to_diam(x0);
    float d1 = lc_live_runtime_x_to_diam(x1);
    int mx0 = lc_live_sim_mx(&g_live_sim_view, z0);
    int mx1 = lc_live_sim_mx(&g_live_sim_view, z1);
    int my0 = lc_live_sim_my(&g_live_sim_view, d0);
    int my1 = lc_live_sim_my(&g_live_sim_view, d1);
    int top;
    int bottom;
    int pad = 1;

    if (d0 > g_live_sim_view.stock_od && d1 > g_live_sim_view.stock_od) {
        return;
    }

    if (lc_live_sim_is_face_cycle(frame)) {
        int doc_px = lc_live_doc_px(frame, &g_live_sim_view, 1.0f);
        int z_left = mx0 < mx1 ? mx0 : mx1;
        int z_right = z_left + doc_px;
        top = 0;
        bottom = my0 > my1 ? my0 : my1;
        lc_live_sim_remove_rect(z_left, top, z_right, bottom);
        return;
    }

    if (lc_live_sim_is_id_cycle(frame)) {
        top = 0;
        bottom = g_live_sim_view.stock_bottom - g_live_sim_view.stock_top;
    } else {
        top = my0 < my1 ? my0 : my1;
        bottom = g_live_sim_view.stock_bottom - g_live_sim_view.stock_top;
    }

    lc_live_sim_remove_rect(mx0, top, mx1 + pad, bottom);
}

static bool lc_live_sim_context_changed(const ui_snapshot_frame_t *frame)
{
    if (!frame) {
        return true;
    }
    return strcmp(g_live_sim_line, frame->leancam_preview_line) != 0 ||
           strcmp(g_live_sim_setup_line, frame->leancam_setup_line) != 0;
}

static void lc_live_sim_update_cut(const ui_snapshot_frame_t *frame)
{
    float x;
    float z;

    if (!g_live_sim_ready || !frame || !frame->axes_valid) {
        return;
    }

    x = frame->axis[0];
    z = frame->axis[2];
    if (g_live_sim_has_last) {
        lc_live_sim_cut_swept_rect(frame, g_live_sim_last_x, g_live_sim_last_z, x, z);
    } else {
        lc_live_sim_cut_swept_rect(frame, x, z, x, z);
    }

    g_live_sim_last_x = x;
    g_live_sim_last_z = z;
    g_live_sim_has_last = true;
}

static void lc_live_sim_draw_material(void)
{
    int stock_w;
    int stock_h;
    int active_start = -1;
    int active_end = -1;
    int active_y = 0;

    if (!g_live_sim_mask || !g_live_sim_ready) {
        return;
    }

    stock_w = g_live_sim_view.stock_right - g_live_sim_view.stock_left + 1;
    stock_h = g_live_sim_view.stock_bottom - g_live_sim_view.stock_top + 1;
    if (stock_w > LC_LIVE_SIM_W) stock_w = LC_LIVE_SIM_W;
    if (stock_h > LC_LIVE_SIM_H) stock_h = LC_LIVE_SIM_H;

    g_live_prof_material_rects = 0;
    for (int y = 0; y < stock_h; ++y) {
        int run_start = -1;
        int row_start = -1;
        int row_end = -1;
        const uint8_t *row = g_live_sim_mask + (y * LC_LIVE_SIM_W);

        for (int x = 0; x <= stock_w; ++x) {
            bool material = (x < stock_w) && row[x];
            if (material && run_start < 0) {
                run_start = x;
            } else if (!material && run_start >= 0) {
                row_start = run_start;
                row_end = x;
                run_start = -1;
                break;
            }
        }

        if (row_start == active_start && row_end == active_end && row_start >= 0) {
            continue;
        }

        if (active_start >= 0) {
            lvds_hstx_fill_rect(g_live_sim_view.stock_left + active_start,
                                 g_live_sim_view.stock_top + active_y,
                                 active_end - active_start,
                                 y - active_y,
                                 LC_LIVE_STOCK_FG);
            g_live_prof_material_rects++;
        }

        active_start = row_start;
        active_end = row_end;
        active_y = y;
    }

    if (active_start >= 0) {
        lvds_hstx_fill_rect(g_live_sim_view.stock_left + active_start,
                             g_live_sim_view.stock_top + active_y,
                             active_end - active_start,
                             stock_h - active_y,
                             LC_LIVE_STOCK_FG);
        g_live_prof_material_rects++;
    }
}

static void lc_sim_label(const lc_sim_view_t *view, int x, int y, const char *name, float v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %.1f", name, (double)v);
    lvds_hstx_text(lc_clampi(x, view->x0 + 4, view->x1 - 70),
                    lc_clampi(y, view->y0 + 8, view->y1 - 18),
                    buf, LC_PREVIEW_LABEL_FG, LC_PREVIEW_PANEL_BG, LVDS_FONT_NORMAL);
}

static void lc_sim_label_large(const lc_sim_view_t *view, int x, int y, const char *name, float v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %.1f", name, (double)v);
    lvds_hstx_text(lc_clampi(x, view->x0 + 4, view->x1 - 110),
                    lc_clampi(y, view->y0 + 8, view->y1 - 18),
                    buf, LC_LIVE_LABEL_FG, LC_LIVE_PANEL_BG, LVDS_FONT_NORMAL);
}

static void lc_sim_hatch_rect(int x, int y, int w, int h, bool vertical)
{
    int p;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    lvds_hstx_fill_rect(x, y, w, h, LC_PREVIEW_CUT_FG);
    lvds_hstx_rect(x, y, w, h, LC_PREVIEW_HATCH_FG);
    if (vertical) {
        for (p = x + 6; p < x + w; p += 6) {
            lvds_hstx_line(p, y, p, y + h - 1, LC_PREVIEW_HATCH_FG);
        }
    } else {
        for (p = y + 6; p < y + h; p += 6) {
            lvds_hstx_line(x, p, x + w - 1, p, LC_PREVIEW_HATCH_FG);
        }
    }
}

static void lc_sim_hatch_vline(int x, int y1, int y2)
{
    int tmp;

    if (y2 < y1) { tmp = y1; y1 = y2; y2 = tmp; }
    if (y2 <= y1) {
        return;
    }
    lvds_hstx_line(x, y1, x, y2, LC_PREVIEW_CUT_FG);
}

static void lc_sim_build_turn_corner_geometry(bool is_od, lc_sim_turn_shape_t *shape)
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

    if (!shape) {
        return;
    }

    shape->z_profile_end = shape->z2;
    shape->d_profile_end = shape->d2;
    shape->d_corner_end = shape->d2;
    shape->arc_i = 0.0f;
    shape->arc_k = 0.0f;

    if (shape->corner == LC_SIM_CORNER_NONE || shape->amount <= 0.0f) {
        return;
    }

    r_start = shape->dt * 0.5f;
    r_end = shape->d2 * 0.5f;
    dz = shape->z2 - shape->z1;
    dr = r_end - r_start;
    len = sqrtf((dz * dz) + (dr * dr));
    if (len <= eps) {
        shape->corner = LC_SIM_CORNER_NONE;
        shape->amount = 0.0f;
        return;
    }

    az = -dz / len;
    ax = -dr / len;
    bx = is_od ? 1.0f : -1.0f;
    dot = ax * bx;
    if (dot > 0.999f) dot = 0.999f;
    if (dot < -0.999f) dot = -0.999f;

    trim = shape->corner == LC_SIM_CORNER_CHMF ?
           shape->amount :
           shape->amount * sqrtf((1.0f + dot) / (1.0f - dot));

    shape->z_profile_end = shape->z2 + (az * trim);
    shape->d_profile_end = 2.0f * (r_end + (ax * trim));
    shape->d_corner_end = 2.0f * (r_end + (bx * trim));

    if (shape->corner == LC_SIM_CORNER_RND) {
        float bis_z = az;
        float bis_x = ax + bx;
        float bis_len = sqrtf((bis_z * bis_z) + (bis_x * bis_x));
        float inv_sin_half;
        float center_z;
        float center_r;

        if (bis_len <= eps) {
            shape->corner = LC_SIM_CORNER_NONE;
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

static void lc_sim_read_turn_shape(const char *line, bool is_od, lc_sim_turn_shape_t *shape)
{
    float rnd = 0.0f;
    float chmf = 0.0f;

    if (!shape) {
        return;
    }

    memset(shape, 0, sizeof(*shape));
    if (!lc_field_float2(line, "D1", "DIAMETER_1", &shape->d1)) return;
    if (!lc_field_float2(line, "D2", "DIAMETER_2", &shape->d2)) return;
    if (!lc_field_float2(line, "Z1", "Z_1", &shape->z1)) return;
    if (!lc_field_float2(line, "Z2", "Z_2", &shape->z2)) return;

    shape->dt = shape->d2;
    (void)lc_field_float3(line, "DT", "D_TAPER", "TAPER_DIAMETER", &shape->dt);
    (void)lc_field_float3(line, "RND", "ROUND", "RADIUS", &rnd);
    (void)lc_field_float3(line, "CHMF", "CHAMFER", "C", &chmf);

    if (rnd > 0.0f && chmf <= 0.0f) {
        shape->corner = LC_SIM_CORNER_RND;
        shape->amount = rnd;
    } else if (chmf > 0.0f && rnd <= 0.0f) {
        shape->corner = LC_SIM_CORNER_CHMF;
        shape->amount = chmf;
    }

    if (is_od && shape->dt > shape->d1) shape->dt = shape->d1;
    if (!is_od && shape->dt < shape->d1) shape->dt = shape->d1;
    if (shape->amount >= lc_absf(shape->z1 - shape->z2)) shape->amount = 0.0f;
    if (shape->amount <= 0.0f) shape->corner = LC_SIM_CORNER_NONE;

    lc_sim_build_turn_corner_geometry(is_od, shape);
}

static float lc_sim_profile_d_at_z(const lc_sim_turn_shape_t *shape, bool is_od, float z)
{
    float z_span;
    float d_profile;
    bool in_corner;

    (void)is_od;
    if (!shape) {
        return 0.0f;
    }

    z_span = shape->z_profile_end - shape->z1;
    d_profile = z_span == 0.0f ?
                shape->d_profile_end :
                lc_lerpf(shape->dt, shape->d_profile_end, (z - shape->z1) / z_span);

    if (shape->corner == LC_SIM_CORNER_NONE || shape->amount <= 0.0f) {
        return d_profile;
    }

    in_corner = shape->z_profile_end < shape->z2 ?
                (z >= shape->z_profile_end && z <= shape->z2) :
                (z <= shape->z_profile_end && z >= shape->z2);
    if (!in_corner) {
        return d_profile;
    }

    if (shape->corner == LC_SIM_CORNER_RND) {
        float center_z = shape->z_profile_end + shape->arc_k;
        float center_r = (shape->d_profile_end * 0.5f) + shape->arc_i;
        float dz = z - center_z;
        float root_arg = (shape->amount * shape->amount) - (dz * dz);
        float rd;
        float d_a;
        float d_b;
        float d_ref;

        if (root_arg < 0.0f) root_arg = 0.0f;
        rd = sqrtf(root_arg);
        d_a = 2.0f * (center_r + rd);
        d_b = 2.0f * (center_r - rd);
        d_ref = lc_lerpf(shape->d_profile_end, shape->d_corner_end,
                         (z - shape->z_profile_end) / (shape->z2 - shape->z_profile_end));
        return lc_absf(d_a - d_ref) < lc_absf(d_b - d_ref) ? d_a : d_b;
    }

    return lc_lerpf(shape->d_profile_end, shape->d_corner_end,
                   (z - shape->z_profile_end) / (shape->z2 - shape->z_profile_end));
}

static void lc_sim_draw_chuck(const lc_sim_view_t *view, const lc_sim_setup_t *setup);

static void lc_sim_draw_stock(const lc_sim_view_t *view, const lc_sim_setup_t *setup)
{
    lvds_hstx_text(view->x0, view->y0, "Cycle preview", LC_PREVIEW_TITLE_FG, LC_PREVIEW_PANEL_BG, LVDS_FONT_NORMAL);
    lvds_hstx_line(view->stock_left - 20, view->stock_top, view->stock_right + 20, view->stock_top, LC_PREVIEW_AXIS_FG);
    lvds_hstx_line(view->z0_x, view->stock_top - 24, view->z0_x, view->stock_bottom + 28, LC_PREVIEW_AXIS_FG);
    lvds_hstx_fill_rect(view->stock_left, view->stock_top,
                         view->stock_right - view->stock_left + 1,
                         view->stock_bottom - view->stock_top + 1,
                         LC_PREVIEW_STOCK_FG);
    if (setup->id > 0.0f) {
        int id_y = lc_sim_dy(view, setup->id);
        if (id_y > view->stock_top) {
            lvds_hstx_fill_rect(view->stock_left, view->stock_top + 1,
                                 view->stock_right - view->stock_left + 1,
                                 id_y - view->stock_top,
                                 LC_PREVIEW_PANEL_BG);
            lvds_hstx_line(view->stock_left, id_y, view->stock_right, id_y, LC_PREVIEW_AXIS_FG);
        }
    }
    lvds_hstx_rect(view->stock_left, view->stock_top,
                    view->stock_right - view->stock_left + 1,
                    view->stock_bottom - view->stock_top + 1,
                    LC_PREVIEW_AXIS_FG);

    if (setup->clamp > 0.0f) {
        lc_sim_draw_chuck(view, setup);
    }

    lvds_hstx_text(view->z0_x - 12, view->stock_top - 24, "Z0", LC_PREVIEW_LABEL_FG, LC_PREVIEW_PANEL_BG, LVDS_FONT_NORMAL);
    lc_sim_label(view, view->stock_left + 4, view->stock_top - 42, "L", setup->length);
    lc_sim_label(view, view->stock_left + 92, view->stock_top - 42, "OD", setup->od);
    if (setup->id > 0.0f) {
        lc_sim_label(view, view->stock_left + 190, view->stock_top - 42, "ID", setup->id);
    }
}

static void lc_sim_draw_chuck(const lc_sim_view_t *view, const lc_sim_setup_t *setup)
{
    char buf[24];
    int clamp_w;
    int y1;
    int y2;
    int jaw_x;

    if (!view || !setup || setup->clamp <= 0.0f) {
        return;
    }

    clamp_w = (int)(setup->clamp * view->scale + 0.5f);
    if (clamp_w < 28) clamp_w = 28;
    if (clamp_w > view->stock_right - view->stock_left + 1) {
        clamp_w = view->stock_right - view->stock_left + 1;
    }

    y1 = view->stock_bottom + 1;
    y2 = y1 + 43;
    if (y2 > view->y1 - 4) {
        y2 = view->y1 - 4;
        y1 = y2 - 43;
    }
    if (y1 <= view->stock_bottom) {
        y1 = view->stock_bottom + 1;
    }
    if (y2 <= y1) {
        return;
    }

    jaw_x = 0;
    lvds_hstx_fill_rect(jaw_x, view->stock_bottom - 18,
                         view->stock_left - jaw_x + 1, 62, LC_PREVIEW_CHUCK_FG);
    lvds_hstx_fill_rect(view->stock_left, y1, clamp_w, y2 - y1 + 1, LC_PREVIEW_CHUCK_FG);

    snprintf(buf, sizeof(buf), "CL %.1f", (double)setup->clamp);
    lvds_hstx_text(10, y1 + 13, buf,
                    LC_PREVIEW_CHUCK_TEXT_FG, LC_PREVIEW_CHUCK_FG, LVDS_FONT_NORMAL);
}

static bool lc_live_tool_hits_chuck(const ui_snapshot_frame_t *frame,
                                    const lc_sim_view_t *view,
                                    const lc_sim_setup_t *setup)
{
    float z;
    float chuck_z_min;

    if (!frame || !view || !setup || !frame->axes_valid || setup->clamp <= 0.0f) {
        return false;
    }
    if (lc_live_sim_is_id_cycle(frame)) {
        return false;
    }

    z = frame->axis[2];
    chuck_z_min = -setup->length;

    return z <= chuck_z_min + setup->clamp;
}

static void lc_sim_draw_live_chuck(const lc_sim_view_t *view,
                                   const lc_sim_setup_t *setup,
                                   bool collision)
{
    int clamp_w;
    int y1;
    int y2;
    int jaw_x;
    lvds_color_t fill = collision ? LC_LIVE_COLLISION : LC_LIVE_CHUCK_FG;
    char buf[24];

    if (!view || !setup || setup->clamp <= 0.0f) {
        return;
    }

    clamp_w = (int)(setup->clamp * view->scale + 0.5f);
    if (clamp_w < 28) clamp_w = 28;
    if (clamp_w > view->stock_right - view->stock_left + 1) {
        clamp_w = view->stock_right - view->stock_left + 1;
    }

    y1 = view->stock_bottom + 1;
    y2 = y1 + 43;
    if (y2 > view->y1 - 4) {
        y2 = view->y1 - 4;
        y1 = y2 - 43;
    }
    if (y1 <= view->stock_bottom) {
        y1 = view->stock_bottom + 1;
    }
    if (y2 <= y1) {
        return;
    }

    jaw_x = 0;

    lvds_hstx_fill_rect(jaw_x, view->stock_bottom - 18,
                         view->stock_left - jaw_x + 1, 62, fill);
    lvds_hstx_fill_rect(view->stock_left, y1, clamp_w, y2 - y1 + 1, fill);
    snprintf(buf, sizeof(buf), collision ? "HIT" : "CL %.1f", (double)setup->clamp);
    lvds_hstx_text(10, y1 + 13, buf, LC_LIVE_BG, fill, LVDS_FONT_NORMAL);
}

static void lc_sim_draw_turn_shape_preview(const lc_sim_view_t *view,
                                           const lc_sim_turn_shape_t *shape,
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
    int base_y;
    int profile_y;
    int prev_x = 0;
    int prev_y = 0;
    bool have_prev = false;

    if (!view || !shape) {
        return;
    }

    x1 = lc_sim_zx(view, shape->z1);
    x2 = lc_sim_zx(view, shape->z2);
    if (x2 < x1) { tmp = x1; x1 = x2; x2 = tmp; }
    if (x2 <= x1) x2 = x1 + 1;

    base_y = lc_sim_dy(view, shape->d1);
    hatch_step = lc_clampi(spacing > 0 ? spacing : 4, 2, 12);
    min_y = base_y;
    max_y = base_y;

    for (x = x1; x <= x2; ++x) {
        float z = ((float)x - (float)view->z0_x) / view->scale;
        profile_y = lc_sim_dy(view, lc_sim_profile_d_at_z(shape, is_od, z));

        if (is_od) {
            lc_sim_hatch_vline(x, profile_y, base_y);
        } else {
            lc_sim_hatch_vline(x, base_y, profile_y);
        }
        if (profile_y < min_y) min_y = profile_y;
        if (profile_y > max_y) max_y = profile_y;
    }

    for (y = min_y + hatch_step; y < max_y; y += hatch_step) {
        int seg_start = -1;

        for (x = x1; x <= x2; ++x) {
            float z = ((float)x - (float)view->z0_x) / view->scale;
            int ya;
            int yb;
            bool inside;

            profile_y = lc_sim_dy(view, lc_sim_profile_d_at_z(shape, is_od, z));
            ya = profile_y < base_y ? profile_y : base_y;
            yb = profile_y > base_y ? profile_y : base_y;
            inside = y >= ya && y <= yb;

            if (inside && seg_start < 0) {
                seg_start = x;
            } else if (!inside && seg_start >= 0) {
                if (x - 1 > seg_start) {
                    lvds_hstx_line(seg_start, y, x - 1, y, LC_PREVIEW_HATCH_FG);
                }
                seg_start = -1;
            }
        }
        if (seg_start >= 0 && x2 > seg_start) {
            lvds_hstx_line(seg_start, y, x2, y, LC_PREVIEW_HATCH_FG);
        }
    }

    for (x = x1; x <= x2; ++x) {
        float z = ((float)x - (float)view->z0_x) / view->scale;
        profile_y = lc_sim_dy(view, lc_sim_profile_d_at_z(shape, is_od, z));

        if (have_prev) {
            lvds_hstx_line(prev_x, prev_y, x, profile_y, LC_PREVIEW_PROFILE_FG);
        }
        prev_x = x;
        prev_y = profile_y;
        have_prev = true;
    }
}

static void lc_sim_draw_turn(const lc_sim_view_t *view,
                             const char *line,
                             const ui_snapshot_frame_t *frame,
                             bool is_od)
{
    lc_sim_turn_shape_t shape;
    float doc;
    int spacing = 0;
    int label_x1;
    int label_end_x;
    int label_y1;
    int label_corner_y;
    int label_dt_y;
    const char *corner_name = "";

    lc_sim_read_turn_shape(line, is_od, &shape);
    if (shape.z1 == 0.0f && shape.z2 == 0.0f && shape.d1 == 0.0f && shape.d2 == 0.0f) {
        return;
    }

    label_x1 = lc_sim_zx(view, shape.z1);
    label_end_x = lc_sim_zx(view, shape.z2);
    label_y1 = lc_sim_dy(view, shape.d1);
    label_corner_y = lc_sim_dy(view, lc_sim_profile_d_at_z(&shape, is_od, shape.z2));
    label_dt_y = lc_sim_dy(view, shape.dt);

    if (lc_field_float3(line, "DOC", "R_DOC", "ROUGH_DOC", &doc) ||
        (frame && lc_field_float3(frame->leancam_tool_line, "R_DOC", "ROUGH_DOC", "ROUGH_DEPTH_OF_CUT", &doc))) {
        spacing = lc_sim_diam_len_px(view, doc);
    }
    if (spacing <= 0) spacing = 4;

    lc_sim_draw_turn_shape_preview(view, &shape, is_od, spacing);
    if (shape.corner != LC_SIM_CORNER_NONE) {
        corner_name = shape.corner == LC_SIM_CORNER_RND ? "RND" : "CHMF";
    }
    lc_sim_draw_turn_start_group(view, frame, label_x1, label_y1, shape.z1, shape.d1);
    lc_sim_draw_turn_dt_label(view, frame, label_x1, label_dt_y, shape.dt);
    lc_sim_draw_turn_end_group(view, frame, label_end_x, label_corner_y,
                               shape.z2, shape.d2, corner_name, shape.amount);
}

static void lc_sim_draw_live_tool(const ui_snapshot_frame_t *frame, const lc_sim_view_t *view)
{
    float z;
    float x;
    float d;
    int zx;
    int dy;

    if (!frame || !frame->axes_valid) {
        return;
    }

    x = frame->axis[0];
    z = frame->axis[2];
    d = lc_live_runtime_x_to_diam(x);
    zx = lc_sim_zx_view(view, z);
    dy = lc_sim_dy_view(view, d);

    lvds_hstx_line(zx, view->y0 + 8, zx, view->y1 - 8, LC_PREVIEW_TOOL_FG);
    lvds_hstx_line(view->x0 + 8, dy, view->x1 - 8, dy, LC_PREVIEW_TOOL_FG);
    lvds_hstx_fill_rect(zx - 5, dy - 5, 11, 11, LC_PREVIEW_TOOL_MARK);
    lvds_hstx_rect(zx - 7, dy - 7, 15, 15, LC_PREVIEW_TOOL_OUTLINE);
}

static void lc_live_draw_tool(const ui_snapshot_frame_t *frame, const lc_sim_view_t *view)
{
    float z;
    float x;
    float d;
    int size;
    int zx;
    int dy;
    int x0;
    int y0;

    if (!frame || !view || !frame->axes_valid) {
        return;
    }

    x = frame->axis[0];
    z = frame->axis[2];
    d = lc_live_runtime_x_to_diam(x);
    size = lc_live_doc_px(frame, view, lc_live_sim_is_face_cycle(frame) ? 1.0f : 0.5f);
    zx = lc_sim_zx_view(view, z);
    dy = lc_sim_dy_view(view, d);
    x0 = lc_clampi(zx, view->x0 + 2, view->x1 - size - 2);
    y0 = lc_live_sim_is_id_cycle(frame) ? (dy - size) : dy;
    y0 = lc_clampi(y0, view->y0 + 2, view->y1 - size - 2);

    if (g_live_tool_rect_valid) {
        lvds_hstx_fill_rect(g_live_tool_rect_x,
                            g_live_tool_rect_y,
                            g_live_tool_rect_w,
                            g_live_tool_rect_h,
                            LC_LIVE_BG);
    }

    lvds_hstx_fill_rect(x0, y0, size, size, LC_LIVE_TOOL_MARK);
    g_live_tool_rect_x = x0;
    g_live_tool_rect_y = y0;
    g_live_tool_rect_w = size;
    g_live_tool_rect_h = size;
    g_live_tool_rect_valid = true;
}

static const char *lc_nc_row_gcode(const char *row)
{
    const char *p = row;

    if (!p) {
        return "";
    }
    while (*p >= '0' && *p <= '9') {
        p++;
    }
    while (*p == ' ') {
        p++;
    }
    return p;
}

static bool lc_gcode_has_word(const char *line, char letter, int code);

static bool lc_gcode_has_motion(const char *line)
{
    return lc_gcode_has_word(line, 'G', 0) ||
           lc_gcode_has_word(line, 'G', 1) ||
           lc_gcode_has_word(line, 'G', 2) ||
           lc_gcode_has_word(line, 'G', 3);
}

static bool lc_gcode_has_word(const char *line, char letter, int code)
{
    const char *p = line;
    char lower = (letter >= 'A' && letter <= 'Z') ? (char)(letter + ('a' - 'A')) : letter;

    while (p && *p) {
        if (*p == letter || *p == lower) {
            char *endp;
            long v = strtol(p + 1, &endp, 10);
            if (endp != p + 1 && v == code &&
                (*endp == 0 || *endp == ' ' || *endp == '\t')) {
                return true;
            }
        }
        p++;
    }
    return false;
}

static bool lc_gcode_axis_value(const char *line, char axis, float *out)
{
    const char *p = line;
    char lower = (axis >= 'A' && axis <= 'Z') ? (char)(axis + ('a' - 'A')) : axis;

    if (!line || !out) {
        return false;
    }

    while (*p) {
        if (*p == axis || *p == lower) {
            char *endp;
            float v = (float)strtod(p + 1, &endp);
            if (endp != p + 1) {
                *out = v;
                return true;
            }
        }
        p++;
    }
    return false;
}

static float lc_angle_norm(float a)
{
    const float two_pi = 6.28318530718f;

    while (a < 0.0f) {
        a += two_pi;
    }
    while (a >= two_pi) {
        a -= two_pi;
    }
    return a;
}

static void lc_sim_draw_nc_arc(const lc_sim_view_t *view,
                               float z0,
                               float d0,
                               float z1,
                               float d1,
                               float i,
                               float k,
                               bool cw,
                               bool selected)
{
    const float two_pi = 6.28318530718f;
    float center_z = z0 + k;
    float r0 = d0 * 0.5f;
    float r1 = d1 * 0.5f;
    float center_r = r0 + i;
    float rz = z0 - center_z;
    float rr = r0 - center_r;
    float radius = sqrtf((rz * rz) + (rr * rr));
    float a0;
    float a1;
    float sweep;
    int steps;
    int prev_x;
    int prev_y;
    int s;

    if (!view || radius <= 0.0001f) {
        return;
    }

    a0 = atan2f(r0 - center_r, z0 - center_z);
    a1 = atan2f(r1 - center_r, z1 - center_z);
    a0 = lc_angle_norm(a0);
    a1 = lc_angle_norm(a1);

    if (cw) {
        sweep = a1 - a0;
        if (sweep >= 0.0f) {
            sweep -= two_pi;
        }
    } else {
        sweep = a1 - a0;
        if (sweep <= 0.0f) {
            sweep += two_pi;
        }
    }

    steps = (int)(lc_absf(sweep) * radius * view->scale / 6.0f) + 4;
    steps = lc_clampi(steps, 6, 48);
    prev_x = lc_sim_zx_view(view, z0);
    prev_y = lc_sim_dy_view(view, d0);

    for (s = 1; s <= steps; ++s) {
        float t = (float)s / (float)steps;
        float a = a0 + (sweep * t);
        float z = center_z + cosf(a) * radius;
        float d = (center_r + sinf(a) * radius) * 2.0f;
        int x = lc_sim_zx_view(view, z);
        int y = lc_sim_dy_view(view, d);

        lvds_hstx_line_w(prev_x, prev_y, x, y, LC_PREVIEW_PROFILE_FG, selected ? 3 : 1);
        prev_x = x;
        prev_y = y;
    }
}

static void lc_sim_draw_nc_overlay(const ui_snapshot_frame_t *frame,
                                   const lc_sim_view_t *view)
{
    float z = 0.0f;
    float d = view ? view->stock_od : 0.0f;
    bool have_pos = false;
    bool diameter_mode = true;
    int i;

    if (!frame || !view || frame->leancam_mode != LC_RENDER_MODE_NC_VIEW) {
        return;
    }

    for (i = 0; i < frame->leancam_line_count && i < UI_LC_MAX_LINES; ++i) {
        const char *g = lc_nc_row_gcode(frame->leancam_lines[i]);
        float next_z = z;
        float next_d = d;
        bool has_z;
        bool has_x;
        bool motion;
        bool is_arc;
        bool is_cw;

        if (!g || g[0] == '(' || g[0] == 0) {
            continue;
        }

        if (lc_gcode_has_word(g, 'G', 7)) {
            diameter_mode = true;
        } else if (lc_gcode_has_word(g, 'G', 8)) {
            diameter_mode = false;
        }

        has_z = lc_gcode_axis_value(g, 'Z', &next_z);
        has_x = lc_gcode_axis_value(g, 'X', &next_d);
        if (has_x) {
            next_d = lc_absf(next_d) * (diameter_mode ? 1.0f : 2.0f);
        }
        motion = lc_gcode_has_motion(g) && (has_z || has_x);
        is_cw = lc_gcode_has_word(g, 'G', 2);
        is_arc = is_cw || lc_gcode_has_word(g, 'G', 3);

        if (motion && have_pos) {
            float arc_i = 0.0f;
            float arc_k = 0.0f;
            bool has_i = lc_gcode_axis_value(g, 'I', &arc_i);
            bool has_k = lc_gcode_axis_value(g, 'K', &arc_k);
            if (is_arc && has_i && has_k) {
                lc_sim_draw_nc_arc(view, z, d, next_z, next_d, arc_i, arc_k, is_cw,
                                   frame->leancam_line_selected[i]);
            } else {
                int x1 = lc_sim_zx_view(view, z);
                int y1 = lc_sim_dy_view(view, d);
                int x2 = lc_sim_zx_view(view, next_z);
                int y2 = lc_sim_dy_view(view, next_d);
                lvds_hstx_line_w(x1, y1, x2, y2, LC_PREVIEW_PROFILE_FG,
                                  frame->leancam_line_selected[i] ? 3 : 1);
            }
        }

        if (has_z || has_x) {
            z = next_z;
            d = next_d;
            have_pos = true;
        }

        if (frame->leancam_line_selected[i]) {
            break;
        }
    }
}

static void lc_sim_draw_preview_ex(const ui_snapshot_frame_t *frame, bool full_screen)
{
    lc_sim_setup_t setup;
    lc_sim_view_t view;
    const char *line;

    lc_sim_read_setup(frame, &setup);
    lc_sim_build_view(&setup, &view, full_screen);
    lc_sim_draw_stock(&view, &setup);

    line = frame->leancam_preview_line;
    if (!line || !line[0] || lc_is_cycle(line, "SETUP")) {
        lc_sim_draw_nc_overlay(frame, &view);
        lc_sim_draw_live_tool(frame, &view);
        return;
    }

    if (lc_is_cycle(line, "OD")) {
        lc_sim_draw_turn(&view, line, frame, true);
    } else if (lc_is_cycle(line, "ID")) {
        lc_sim_draw_turn(&view, line, frame, false);
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
        lc_sim_draw_face_start_group(&view, frame, lc_sim_zx(&view, z1), y2, z1, d);
        lc_sim_draw_face_end_group(&view, frame, lc_sim_zx(&view, z), y2, z, d);
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
        lc_sim_label(&view, x2 - 34, y2 + 8, "Z", target);
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
        lvds_hstx_line_w(x1, y, x2, y, LC_PREVIEW_CUT_FG, 3);
        for (p = x1; p < x2; p += lc_clampi((int)(pitch * view.scale + 0.5f), 5, 18)) {
            lvds_hstx_line(p, y - 8, p + 8, y + 8, LC_PREVIEW_PROFILE_FG);
        }
        lc_sim_label(&view, x1, y + 16, "P", pitch);
    }

    lc_sim_draw_nc_overlay(frame, &view);
    lc_sim_draw_live_tool(frame, &view);
}

static void lc_sim_draw_preview(const ui_snapshot_frame_t *frame)
{
    lc_sim_draw_preview_ex(frame, false);
}

static bool lc_should_draw_fullscreen_preview(const ui_snapshot_frame_t *frame)
{
    if (!frame) {
        return false;
    }
    return false;
}

static void draw_fullscreen_preview(const ui_snapshot_frame_t *frame)
{
    char buf[96];

    lvds_hstx_fill_rect(0, 42, LVDS_HSTX_WIDTH, 558, LC_PREVIEW_BG);
    lvds_hstx_rect(12, 56, 776, 500, LC_PREVIEW_FRAME_FG);
    lc_text_clip(26, 62, frame->leancam_title[0] ? frame->leancam_title : "LeanCam live preview",
                 36, LC_PREVIEW_TITLE_FG, LC_PREVIEW_PANEL_BG, LVDS_FONT_NORMAL);
    lc_text_clip(330, 62, frame->leancam_message, 38, LC_PREVIEW_MESSAGE_FG, LC_PREVIEW_PANEL_BG, LVDS_FONT_NORMAL);
    lc_sim_draw_preview_ex(frame, true);
    snprintf(buf, sizeof(buf), "Live X %.3f  Z %.3f",
             frame->axes_valid ? (double)frame->axis[0] : 0.0,
             frame->axes_valid ? (double)frame->axis[2] : 0.0);
    lc_text_clip(26, 566, buf, 32, LC_PREVIEW_LABEL_FG, LC_PREVIEW_PANEL_BG, LVDS_FONT_SMALL);
    lc_text_clip(330, 566, frame->leancam_preview_line, 54, LC_PREVIEW_LABEL_FG, LC_PREVIEW_PANEL_BG, LVDS_FONT_SMALL);
}

static void draw_live_run_preview(const ui_snapshot_frame_t *frame)
{
    uint32_t t0;
    uint32_t t1;
    bool chuck_collision = false;

    if (!g_live_sim_mask) {
        g_live_sim_mask = lvds_psram_available() ? (uint8_t *)lvds_psram_ptr(LC_LIVE_SIM_PSRAM_OFFSET) : NULL;
    }

    if (!g_live_sim_ready ||
        (g_live_sim_was_running && lc_live_sim_context_changed(frame))) {
        lc_live_sim_reset(frame);
    } else if (!g_live_sim_was_running) {
        lc_live_sim_force_redraw();
    }

    t0 = mcu_micros();
    if (!g_live_sim_static_drawn) {
        lvds_hstx_fill_rect(0, 42, LVDS_HSTX_WIDTH, 558, LC_LIVE_BG);
        lc_text_clip(26, 62, "Live", 18, LC_LIVE_TITLE_FG, LC_LIVE_PANEL_BG, LVDS_FONT_LARGE);
    }
    t1 = mcu_micros();
    g_live_prof_clear_us = t1 - t0;

    if (g_live_sim_ready) {
        chuck_collision = lc_live_tool_hits_chuck(frame, &g_live_sim_view, &g_live_sim_setup);
        if (!g_live_sim_static_drawn) {
            t0 = mcu_micros();
            lc_live_sim_draw_material();
            t1 = mcu_micros();
            g_live_prof_material_us = t1 - t0;
            t0 = mcu_micros();
            lvds_hstx_text(g_live_sim_view.z0_x - 12, g_live_sim_view.stock_top - 24, "Z0", LC_LIVE_LABEL_FG, LC_LIVE_PANEL_BG, LVDS_FONT_NORMAL);
            lc_sim_draw_live_chuck(&g_live_sim_view, &g_live_sim_setup, chuck_collision);
            g_live_chuck_collision_last = chuck_collision;
            g_live_chuck_collision_valid = true;
            lc_sim_label_large(&g_live_sim_view, g_live_sim_view.stock_left + 4, g_live_sim_view.stock_top - 46, "L", g_live_sim_setup.length);
            lc_sim_label_large(&g_live_sim_view, g_live_sim_view.stock_left + 128, g_live_sim_view.stock_top - 46, "OD", g_live_sim_setup.od);
            t1 = mcu_micros();
            g_live_prof_overlay_us = t1 - t0;
            g_live_sim_static_drawn = true;
        } else {
            g_live_prof_material_us = 0;
            g_live_prof_material_rects = 0;
            g_live_prof_overlay_us = 0;
        }
        lc_live_sim_update_cut(frame);
        t0 = mcu_micros();
        if (!g_live_chuck_collision_valid || chuck_collision != g_live_chuck_collision_last) {
            lc_sim_draw_live_chuck(&g_live_sim_view, &g_live_sim_setup, chuck_collision);
            g_live_chuck_collision_last = chuck_collision;
            g_live_chuck_collision_valid = true;
        }
        lc_live_draw_tool(frame, &g_live_sim_view);
        t1 = mcu_micros();
        g_live_prof_overlay_us += t1 - t0;
    } else {
        t0 = mcu_micros();
        lc_text_clip(26, 128, "Live sim needs PSRAM", 32, LC_LIVE_COLLISION, LC_LIVE_PANEL_BG, LVDS_FONT_NORMAL);
        lc_sim_draw_preview_ex(frame, true);
        t1 = mcu_micros();
        g_live_prof_material_us = 0;
        g_live_prof_overlay_us = t1 - t0;
    }

    t0 = mcu_micros();
    draw_perf_meter(610, 566, LC_LIVE_DEBUG_FG, LC_LIVE_PANEL_BG);
    t1 = mcu_micros();
    g_live_prof_footer_us = t1 - t0;
}

static const char *lc_split_preview_title(const ui_snapshot_frame_t *frame)
{
    if (frame && (frame->leancam_mode == LC_RENDER_MODE_FILES || frame->leancam_mode == 1)) {
        return "File manager";
    }
    if (frame && frame->leancam_mode == LC_RENDER_MODE_NC_VIEW) {
        return "G code edit";
    }
    return "Programming";
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

static void draw_perf_meter(int x, int y, lvds_color_t fg, lvds_color_t bg)
{
    char perf[56];
    snprintf(perf, sizeof(perf), "%u.%u fps  %lu us",
             (unsigned)(g_render_fps_x10 / 10u),
             (unsigned)(g_render_fps_x10 % 10u),
             (unsigned long)g_render_last_us);
    lc_text_clip(x, y, perf, 22, fg, bg, LVDS_FONT_SMALL);
}

static void draw_block_meter(int x, int y, lvds_color_t fg, lvds_color_t bg)
{
    char perf[80];
    snprintf(perf, sizeof(perf), "H%lu C%lu R%lu P%lu F%lu Pr%lu",
             (unsigned long)g_prof_header_us,
             (unsigned long)g_prof_clear_us,
             (unsigned long)g_prof_rows_us,
             (unsigned long)g_prof_preview_us,
             (unsigned long)g_prof_footer_us,
             (unsigned long)g_live_prof_present_us);
    lc_text_clip(x, y, perf, 40, fg, bg, LVDS_FONT_SMALL);
}

static void draw_normal_meters_only(void)
{
    uint32_t t0 = mcu_micros();
    g_prof_clear_us = 0;
    g_prof_rows_us = 0;
    g_prof_preview_us = 0;
    draw_block_meter(350, 552, LC_COL_FOOTER_TEXT, LC_COL_FOOTER_BG);
    draw_perf_meter(610, 574, LC_COL_FOOTER_TEXT, LC_COL_FOOTER_BG);
    g_prof_footer_us = mcu_micros() - t0;
}

static void draw_bar(const ui_snapshot_frame_t *frame)
{
    static bool header_bg_ready;
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

    if (!header_bg_ready) {
        lvds_hstx_fill_rect(0, 0, LVDS_HSTX_WIDTH, 42, LC_COL_TOP);
        header_bg_ready = true;
    }
    snprintf(buf, sizeof(buf), "%-5s X:%7.3f Z:%7.3f F:%5.1f S:%u",
             frame ? exec_state_text(frame->state) : "BOOT",
             (double)x, (double)z, (double)feed, spindle);
    lvds_hstx_text(12, 10, buf, LC_COL_TEXT, LC_COL_TOP, LVDS_FONT_LARGE);
}

static void draw_value_with_highlight(int x, int y, const char *value,
                                      bool has_hi, uint8_t hi_start, uint8_t hi_end,
                                      int clear_cols, lvds_color_t bg)
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
        lvds_hstx_text(x, y, line, LC_COL_VALUE, bg, LVDS_FONT_NORMAL);
        return;
    }

    if (hi_end > (uint8_t)len) {
        hi_end = (uint8_t)len;
    }

    snprintf(left, sizeof(left), "%.*s", (int)hi_start, value);
    snprintf(mid, sizeof(mid), "%.*s", (int)(hi_end - hi_start), value + hi_start);
    snprintf(right, sizeof(right), "%-*.*s",
             clear_cols - (int)hi_end, clear_cols - (int)hi_end, value + hi_end);

    lvds_hstx_text(x, y, left, LC_COL_VALUE, bg, LVDS_FONT_NORMAL);
    lvds_hstx_text(x + ((int)hi_start * 8), y, mid, LC_COL_BG, LC_COL_HI, LVDS_FONT_NORMAL);
    lvds_hstx_text(x + ((int)hi_end * 8), y, right, LC_COL_VALUE, bg, LVDS_FONT_NORMAL);
}

static void lc_scroll_pair_to_highlight(char *header,
                                        size_t header_sz,
                                        char *value,
                                        size_t value_sz,
                                        uint8_t *hi_start,
                                        uint8_t *hi_end,
                                        uint8_t visible_cols)
{
    size_t len;
    uint8_t scroll;

    if (!header || !value || !hi_start || !hi_end || visible_cols == 0) {
        return;
    }

    len = strlen(value);
    if (len <= visible_cols || *hi_end <= visible_cols || *hi_start >= len) {
        return;
    }

    scroll = (*hi_start > 12u) ? (uint8_t)(*hi_start - 12u) : *hi_start;
    if (scroll == 0u || scroll >= len) {
        return;
    }

    if (scroll < strlen(header)) {
        memmove(header, header + scroll, strlen(header) - scroll + 1u);
    } else if (header_sz > 0) {
        header[0] = 0;
    }

    if (scroll < strlen(value)) {
        memmove(value, value + scroll, strlen(value) - scroll + 1u);
    } else if (value_sz > 0) {
        value[0] = 0;
    }

    *hi_start = (uint8_t)(*hi_start - scroll);
    *hi_end = (*hi_end > scroll) ? (uint8_t)(*hi_end - scroll) : 0u;
}

static void draw_leancam_rows(const ui_snapshot_frame_t *frame)
{
    uint32_t t0;
    int i;
    int y = 86;
    int max_blocks = 7;
    bool compact_rows = frame->leancam_mode == LC_RENDER_MODE_FILES ||
                        frame->leancam_mode == LC_RENDER_MODE_NC_VIEW;

    g_prof_clear_us = 0;
    g_prof_rows_us = 0;
    g_prof_preview_us = 0;
    g_prof_footer_us = 0;

    t0 = mcu_micros();
    lvds_hstx_fill_rect(0, 42, LVDS_HSTX_WIDTH, 558, LC_COL_BG);

    lvds_hstx_fill_rect((LVDS_HSTX_WIDTH / 2) - 1, 58, 2, 474, LC_COL_LINE);
    lc_text_clip(LC_PREVIEW_X, 64, lc_split_preview_title(frame), 14, LC_COL_TEXT, LC_COL_BG, LVDS_FONT_LARGE);
    g_prof_clear_us = mcu_micros() - t0;

    t0 = mcu_micros();
    if (compact_rows) {
        int max_rows = 23;
        for (i = 0; i < frame->leancam_line_count && i < UI_LC_MAX_LINES && max_rows > 0; ++i) {
            lvds_color_t fg = frame->leancam_line_selected[i] ? LC_COL_HI : LC_COL_TEXT;
            lvds_color_t bg = frame->leancam_line_selected[i] ? LC_COL_SELECT : LC_COL_BG;

            if (frame->leancam_line_selected[i]) {
                lvds_hstx_fill_rect(LC_RIGHT_PANE_X + 8, y - 2, LC_RIGHT_PANE_W - 16, 19, bg);
                lvds_hstx_rect(LC_RIGHT_PANE_X + 8, y - 2, LC_RIGHT_PANE_W - 16, 19, LC_COL_HI);
            }
            lc_text_clip(LC_TEXT_X, y, frame->leancam_lines[i], 43, fg, bg, LVDS_FONT_NORMAL);
            y += 20;
            max_rows--;
        }
        g_prof_rows_us = mcu_micros() - t0;

        t0 = mcu_micros();
        lc_sim_draw_preview(frame);
        lc_text_clip(LC_PREVIEW_X, 522, frame->leancam_active_field,
                     34, LC_COL_VALUE, LC_COL_BG, LVDS_FONT_SMALL);
        g_prof_preview_us = mcu_micros() - t0;

        t0 = mcu_micros();
        lvds_hstx_fill_rect(0, 548, LVDS_HSTX_WIDTH, 52, LC_COL_FOOTER_BG);
        lc_text_clip(12, 552, frame->leancam_message, 78, LC_COL_FOOTER_VALUE, LC_COL_FOOTER_BG, LVDS_FONT_NORMAL);
        lc_text_clip(12, 574, frame->leancam_helper, 78, LC_COL_FOOTER_TEXT, LC_COL_FOOTER_BG, LVDS_FONT_NORMAL);
        draw_block_meter(350, 552, LC_COL_FOOTER_TEXT, LC_COL_FOOTER_BG);
        draw_perf_meter(610, 574, LC_COL_FOOTER_TEXT, LC_COL_FOOTER_BG);
        g_prof_footer_us = mcu_micros() - t0;
        return;
    }

    for (i = 0; i < frame->leancam_line_count && i < UI_LC_MAX_LINES && max_blocks > 0; ++i) {
        lvds_lc_table_line_t tl;
        lvds_color_t fg = frame->leancam_line_selected[i] ? LC_COL_HI : LC_COL_TEXT;
        char title[64];

        lvds_lc_build_table_line(frame, i, &tl);
        if (tl.table_like) {
            lvds_color_t row_bg = frame->leancam_line_selected[i] ? LC_COL_SELECT : LC_COL_BG;
            char header[UI_LC_LINE_LEN];
            char value[UI_LC_LINE_LEN];
            uint8_t hi_start;
            uint8_t hi_end;
            lc_block_title(frame->leancam_lines[i], title, sizeof(title));
            ui_snapshot_strcpy(header, lc_table_without_block_name(tl.header), sizeof(header));
            ui_snapshot_strcpy(value, lc_table_without_block_name(tl.value), sizeof(value));
            hi_start = tl.hi_start > 11 ? (uint8_t)(tl.hi_start - 11) : 0;
            hi_end = tl.hi_end > 11 ? (uint8_t)(tl.hi_end - 11) : 0;
            if (tl.has_hi) {
                lc_scroll_pair_to_highlight(header, sizeof(header), value, sizeof(value),
                                            &hi_start, &hi_end, 45);
            }
            if (frame->leancam_line_selected[i]) {
                lvds_hstx_fill_rect(LC_RIGHT_PANE_X + 8, y - 3, LC_RIGHT_PANE_W - 16, 64, row_bg);
                lvds_hstx_rect(LC_RIGHT_PANE_X + 8, y - 3, LC_RIGHT_PANE_W - 16, 64, LC_COL_HI);
            }
            lc_text_clip(LC_TEXT_X, y, title, 43, fg, row_bg, LVDS_FONT_NORMAL);
            y += 18;
            lc_text_clip(LC_TEXT_X, y, header, 43, fg, row_bg, LVDS_FONT_NORMAL);
            y += 20;
            draw_value_with_highlight(LC_TEXT_X, y, value,
                                      tl.has_hi,
                                      hi_start,
                                      hi_end,
                                      43,
                                      row_bg);
            y += 26;
            //lvds_hstx_line(24, y - 10, 388, y - 10, LC_COL_PANEL);
            max_blocks--;
        } else {
            bool has_hi = frame->leancam_field_hi_end[i] > frame->leancam_field_hi_start[i];
            if (has_hi) {
                draw_value_with_highlight(LC_TEXT_X, y, frame->leancam_lines[i], true,
                                          frame->leancam_field_hi_start[i],
                                          frame->leancam_field_hi_end[i],
                                          43,
                                          LC_COL_BG);
            } else {
                lc_text_clip(LC_TEXT_X, y, frame->leancam_lines[i], 43, fg, LC_COL_BG, LVDS_FONT_NORMAL);
            }
            y += 22;
            max_blocks--;
        }
    }
    g_prof_rows_us = mcu_micros() - t0;

    t0 = mcu_micros();
    lc_sim_draw_preview(frame);
    lc_text_clip(LC_PREVIEW_X, 522, frame->leancam_active_field,
                 34, LC_COL_VALUE, LC_COL_BG, LVDS_FONT_SMALL);
    g_prof_preview_us = mcu_micros() - t0;

    t0 = mcu_micros();
    lvds_hstx_fill_rect(0, 548, LVDS_HSTX_WIDTH, 52, LC_COL_FOOTER_BG);
    lc_text_clip(12, 552, frame->leancam_message, 78, LC_COL_FOOTER_VALUE, LC_COL_FOOTER_BG, LVDS_FONT_NORMAL);
    lc_text_clip(12, 574, frame->leancam_helper, 78, LC_COL_FOOTER_TEXT, LC_COL_FOOTER_BG, LVDS_FONT_NORMAL);
    draw_block_meter(350, 552, LC_COL_FOOTER_TEXT, LC_COL_FOOTER_BG);
    draw_perf_meter(610, 574, LC_COL_FOOTER_TEXT, LC_COL_FOOTER_BG);
    g_prof_footer_us = mcu_micros() - t0;
}

static bool normal_body_changed(const ui_snapshot_frame_t *frame)
{
    const ui_snapshot_frame_t *old = &g_normal_body_frame;

    if (!g_normal_body_valid || !frame) {
        return true;
    }
    if (frame->leancam_active != old->leancam_active ||
        frame->leancam_show_menu != old->leancam_show_menu ||
        frame->leancam_mode != old->leancam_mode ||
        frame->leancam_line_count != old->leancam_line_count ||
        frame->leancam_thread_lane_valid != old->leancam_thread_lane_valid ||
        frame->leancam_thread_start_lane != old->leancam_thread_start_lane ||
        frame->leancam_thread_stop_lane != old->leancam_thread_stop_lane ||
        frame->leancam_thread_ramp_lane != old->leancam_thread_ramp_lane ||
        frame->leancam_thread_lock_lane != old->leancam_thread_lock_lane ||
        frame->leancam_thread_z_speed != old->leancam_thread_z_speed) {
        return true;
    }
    if (strcmp(frame->leancam_title, old->leancam_title) ||
        strcmp(frame->leancam_message, old->leancam_message) ||
        strcmp(frame->leancam_helper, old->leancam_helper) ||
        strcmp(frame->leancam_setup_line, old->leancam_setup_line) ||
        strcmp(frame->leancam_preview_line, old->leancam_preview_line) ||
        strcmp(frame->leancam_tool_line, old->leancam_tool_line) ||
        strcmp(frame->leancam_active_field, old->leancam_active_field)) {
        return true;
    }
    if (memcmp(frame->leancam_lines, old->leancam_lines, sizeof(frame->leancam_lines)) ||
        memcmp(frame->leancam_line_selected, old->leancam_line_selected, sizeof(frame->leancam_line_selected)) ||
        memcmp(frame->leancam_field_hi_start, old->leancam_field_hi_start, sizeof(frame->leancam_field_hi_start)) ||
        memcmp(frame->leancam_field_hi_end, old->leancam_field_hi_end, sizeof(frame->leancam_field_hi_end))) {
        return true;
    }
    return false;
}

static void remember_normal_body(const ui_snapshot_frame_t *frame)
{
    if (!frame) {
        g_normal_body_valid = false;
        return;
    }
    memcpy(&g_normal_body_frame, frame, sizeof(g_normal_body_frame));
    g_normal_body_valid = true;
}

void lvds_renderer_draw_init(void)
{
    g_last_seq = UINT32_MAX;
    g_normal_body_valid = false;
    lvds_hstx_clear(LC_COL_BG);
    draw_bar(NULL);
    lvds_hstx_text(24, 82, "Waiting for LeanCam snapshot", LC_COL_TEXT, LC_COL_BG, LVDS_FONT_NORMAL);
    lvds_hstx_present();
}

void lvds_renderer_draw_poll(void)
{
    static uint32_t last_ms;
    static uint32_t last_present_us;
    const ui_snapshot_frame_t *frame;
    uint32_t seq;
    uint32_t now = mcu_millis();
    uint32_t start_us;
    uint32_t draw_us;
    uint32_t end_us;

    if ((uint32_t)(now - last_ms) < LVDS_RENDERER_MS) {
        return;
    }
    last_ms = now;

    frame = lvds_renderer_state_frame();
    seq = frame ? frame->seq : 0;
    if (!frame || !ui_snapshot_has_newer_seq(seq, g_last_seq)) {
        return;
    }

    if (lc_live_sim_running(frame)) {
        g_normal_body_valid = false;
        lvds_hstx_direct_scanout(false);
        start_us = mcu_micros();
        draw_bar(frame);
        g_prof_header_us = mcu_micros() - start_us;
        draw_live_run_preview(frame);
    } else if (lc_should_draw_fullscreen_preview(frame)) {
        g_normal_body_valid = false;
        lvds_hstx_direct_scanout(false);
        start_us = mcu_micros();
        draw_bar(frame);
        g_prof_header_us = mcu_micros() - start_us;
        draw_fullscreen_preview(frame);
    } else {
        bool body_changed = normal_body_changed(frame);
        lvds_hstx_direct_scanout(false);
        start_us = mcu_micros();
        draw_bar(frame);
        g_prof_header_us = mcu_micros() - start_us;
        if (body_changed) {
            draw_leancam_rows(frame);
            remember_normal_body(frame);
        } else {
            draw_normal_meters_only();
        }
    }
    g_live_sim_was_running = lc_live_sim_running(frame);
    draw_us = mcu_micros() - start_us;
    start_us = mcu_micros();
    lvds_hstx_present();
    end_us = mcu_micros();
    g_live_prof_present_us = end_us - start_us;
    g_render_last_us = draw_us + g_live_prof_present_us;
    if (last_present_us != 0) {
        g_render_period_us = end_us - last_present_us;
        if (g_render_period_us > 0) {
            g_render_fps_x10 = (uint16_t)(10000000u / g_render_period_us);
        }
    }
    last_present_us = end_us;
    g_last_seq = seq;
}


