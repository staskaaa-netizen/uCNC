/* LeanCam visual contract:
 * Purpose: draw a renderer-neutral LeanCam snapshot into LVDS primitives/pixels.
 * Called by: execution_controller render step.
 * Calls into: LVDS primitive drawing and visual submodules only.
 * Owns: visual-only cache/state; it must not poll keys, generate G-code, or perform file I/O.
 */
#include "leancam_visual.h"

#include "../../../cnc.h"
#include "../../../interface/grbl_stream.h"
#include "../execution_controller.h"
#include "../leancam_bridge.h"
#include "../leancam_resource.h"
#include "leancam_visual_state.h"
#include "../leancam_snapshot_frame.h"
#include "../../lvds_renderer/lvds_draw_api.h"
#include "../../lvds_renderer/lvds_hstx.h"
#include "../../lvds_renderer/lvds_palette.h"
#include "../../lvds_renderer/lvds_psram.h"
#include "lvds_ui_layout.h"
#include "lvds_ui_footer.h"
#include "lvds_ui_header.h"
#include "lvds_ui_meters.h"
#include "lvds_ui_live.h"
#include "lvds_ui_program.h"
#include "lvds_ui_text.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifndef LVDS_RENDERER_AUTO_LIVE_SIM
#define LVDS_RENDERER_AUTO_LIVE_SIM 1
#endif

#ifndef LVDS_RENDERER_AUTO_LIVE_IN_NC_VIEW
#define LVDS_RENDERER_AUTO_LIVE_IN_NC_VIEW 0
#endif

#ifndef LVDS_RENDERER_LIVE_SPLIT_PANEL
#define LVDS_RENDERER_LIVE_SPLIT_PANEL 0
#endif

#ifndef LVDS_RENDERER_FULLSCREEN_SIM_PREVIEW
#define LVDS_RENDERER_FULLSCREEN_SIM_PREVIEW 0
#endif

#ifndef LVDS_RENDERER_SHOW_PERF_METERS
#define LVDS_RENDERER_SHOW_PERF_METERS 0
#endif

#ifndef LVDS_RENDERER_G33_LIVE_HOLD_MS
#define LVDS_RENDERER_G33_LIVE_HOLD_MS 1200u
#endif

#ifndef LEANCAM_DEBUG
#define LEANCAM_DEBUG 0
#endif

#ifndef LEANCAM_DEBUG_PREVIEW
#define LEANCAM_DEBUG_PREVIEW 0
#endif

#ifndef LEANCAM_DEBUG_LVDS
#define LEANCAM_DEBUG_LVDS 0
#endif

#ifndef LVDS_RENDERER_PREVIEW_SERIAL_DEBUG
#define LVDS_RENDERER_PREVIEW_SERIAL_DEBUG LEANCAM_DEBUG_PREVIEW
#endif

#define LC_RENDER_MODE_FILES 0
#define LC_RENDER_MODE_PROGRAM 2
#define LC_RENDER_MODE_DRAFT 3
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
#define LC_THREAD_FLANK_TAN30_NUM 577
#define LC_THREAD_FLANK_TAN30_DEN 1000
#define LC_THREAD_INSERT_TAN60_NUM 1732
#define LC_THREAD_INSERT_TAN60_DEN 1000
#define LC_PREVIEW_HATCH_MAX_POINTS 96
#define LC_FULLSCREEN_PREVIEW_STEPS 4u
#define LC_PREVIEW_ARC_MAX_STEPS 24
#define LC_PREVIEW_LABEL_FONT LVDS_FONT_NORMAL
#define LC_PREVIEW_LABEL_H 18
#define LC_LEFT_PANE_X 10
#define LC_LEFT_PANE_W 392
#define LC_RIGHT_PANE_X 412
#define LC_RIGHT_PANE_W 372
#define LC_RIGHT_CELL_X (LC_RIGHT_PANE_X - 2)
#define LC_RIGHT_CELL_W (LC_RIGHT_PANE_W + 6)
#define LC_TEXT_X (LC_RIGHT_PANE_X + 4)
#define LC_PREVIEW_X (LC_LEFT_PANE_X + 14)
#define LC_LEFT_LIVE_CLEAR_X (LC_LEFT_PANE_X + 2)
#define LC_LEFT_LIVE_CLEAR_Y 86
#define LC_LEFT_LIVE_CLEAR_W (LC_LEFT_PANE_W - 15)
#define LC_LEFT_LIVE_CLEAR_H 458
#define LC_RIGHT_TEXT_COLS 45
#define LC_FOOTER_TEXT_COLS 80

#define LC_TOOL_EDITOR_LEFT_X 24
#define LC_TOOL_EDITOR_LEFT_W 300
#define LC_TOOL_EDITOR_RIGHT_X 340
#define LC_TOOL_EDITOR_RIGHT_COLS 54

#ifndef lvds_renderer_LIVE_RT_X_RADIUS
#define lvds_renderer_LIVE_RT_X_RADIUS 1
#endif

#ifndef LEANCAM_NC_LIVE_GUIDE_WIDTH
#define LEANCAM_NC_LIVE_GUIDE_WIDTH 1
#endif

#ifndef LC_LVDS_SERIAL_DEBUG
#define LC_LVDS_SERIAL_DEBUG LEANCAM_DEBUG_LVDS
#endif

#if LC_LVDS_SERIAL_DEBUG
#define LC_LVDS_DBG(fmt, ...) grbl_stream_printf(__romstr__("[MSG:LVDS " fmt "]\r\n"), ##__VA_ARGS__)
#else
#define LC_LVDS_DBG(fmt, ...)
#endif

#if LVDS_RENDERER_PREVIEW_SERIAL_DEBUG
#define LC_PREVIEW_DBG(fmt, ...) grbl_stream_printf(__romstr__("[MSG:LC preview " fmt "]\r\n"), ##__VA_ARGS__)
#else
#define LC_PREVIEW_DBG(fmt, ...)
#endif

static bool lc_lvds_debug_line_changed(uint32_t seq, uint8_t mode, const char *line)
{
    static uint8_t last_mode = 255u;
    static char last_line[UI_LC_LINE_LEN];

    (void)seq;
    if (!line) {
        line = "";
    }
    if (mode != last_mode || strcmp(line, last_line) != 0) {
        last_mode = mode;
        ui_snapshot_strcpy(last_line, line, sizeof(last_line));
        return true;
    }
    return false;
}

static bool lc_lvds_debug_branch_changed(const char *branch)
{
    static char last_branch[24];

    if (!branch) {
        branch = "";
    }
    if (strcmp(branch, last_branch) != 0) {
        ui_snapshot_strcpy(last_branch, branch, sizeof(last_branch));
        return true;
    }
    return false;
}

static bool lc_lvds_debug_preview_changed(const char *stage, const char *line)
{
    static char last_line[UI_LC_LINE_LEN];

    (void)stage;
    if (!stage) {
        stage = "";
    }
    if (!line) {
        line = "";
    }
    if (strcmp(line, last_line) != 0) {
        ui_snapshot_strcpy(last_line, line, sizeof(last_line));
        return true;
    }
    return false;
}

static uint32_t g_last_seq;
static uint8_t *g_live_sim_mask;
static bool g_live_sim_ready;
static bool g_live_sim_was_running;
static uint32_t g_live_sim_thread_hold_until_ms;
static char g_live_sim_title[UI_LC_LINE_LEN];
static char g_live_sim_line[UI_LC_LINE_LEN];
static char g_live_sim_setup_line[UI_LC_LINE_LEN];
static char g_live_sim_tool_line[UI_LC_LINE_LEN];
static float g_live_sim_last_x;
static float g_live_sim_last_z;
static bool g_live_sim_has_last;
static bool g_live_sim_static_drawn;
static bool g_live_sim_full_screen;
static bool g_live_thread_z_set;
static float g_live_thread_start_z;
static float g_live_thread_pitch;
static float g_live_thread_next_cut_z;
static int g_live_thread_z_dir;
static bool g_live_tool_rect_valid;
static bool g_live_tool_rect_thread;
static int g_live_tool_rect_x;
static int g_live_tool_rect_y;
static int g_live_tool_rect_w;
static int g_live_tool_rect_h;
static uint32_t g_render_last_us;
static uint32_t g_render_period_us;
static volatile bool g_render_in_poll;
static uint32_t g_render_reentry_count;
static uint32_t g_render_max_draw_us;
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
static uint32_t g_fullscreen_preview_hash;
static uint8_t g_fullscreen_preview_step = LC_FULLSCREEN_PREVIEW_STEPS;
static bool g_fullscreen_preview_was_active;
static float g_fullscreen_gcode_draw_z;
static float g_fullscreen_gcode_draw_d;
static bool g_fullscreen_gcode_draw_have_pos;
static bool g_fullscreen_gcode_draw_diameter_mode = true;
static uint8_t g_fullscreen_gcode_draw_feed_class;
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
    bool full_screen;
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

static void lc_text_clip(int x, int y, const char *text, int cols,
                         lvds_color_t fg, lvds_color_t bg, int font)
{
    lvds_draw_text_clip(x, y, text, cols, fg, bg, font);
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
    while (*s == ' ' || *s == '\t')
        s++;
    n = strlen(name);
    return strncmp(s, name, n) == 0 &&
           (s[n] == 0 || s[n] == ' ' || s[n] == '\t');
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

    while (*p) {
        const char *tok;
        const char *end;
        const char *value;
        const char *value_end;
        size_t n;

        while (*p == ' ' || *p == '\t')
            p++;
        tok = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        end = p;

        if ((size_t)(end - tok) > name_len &&
            strncmp(tok, name, name_len) == 0 &&
            tok[name_len] != '_') {
            value = tok + name_len;
            value_end = end;

            if (*value == '=')
                value++;
            if (*value == '{') {
                const char *close = memchr(value + 1, '}', (size_t)(end - value - 1));
                if (!close)
                    return false;
                value++;
                value_end = close;
            }

            n = (size_t)(value_end - value);
            if (n >= out_sz)
                n = out_sz - 1;
            if (n)
                memcpy(out, value, n);
            out[n] = 0;
            return true;
        }
    }
    return false;
}

static bool lc_get_display_field_text(const char *line, const char *name, char *out, size_t out_sz)
{
    const char *p;
    size_t name_len;

    if (!line || !name || !out || out_sz == 0)
        return false;
    out[0] = 0;
    line = lc_skip_line_number(line);
    name_len = strlen(name);
    p = line;

    while ((p = strstr(p, name)) != NULL) {
        const char *value;
        size_t n = 0;

        if ((p == line || p[-1] == ' ') &&
            p[name_len] != 0 &&
            p[name_len] != ' ' &&
            p[name_len] != '_' &&
            p[name_len] != '|') {
            value = p + name_len;
            while (value[n] && value[n] != ' ' && n + 1 < out_sz)
                n++;
            memcpy(out, value, n);
            out[n] = 0;
            return n > 0;
        }
        p += name_len;
    }
    return false;
}

static bool lc_get_tool_field_text(const char *line, const char *name, char *out, size_t out_sz)
{
    return lc_get_field_text(line, name, out, out_sz) ||
           lc_get_display_field_text(line, name, out, out_sz);
}

static bool lc_tool_field_float(const char *line, const char *name, float *out)
{
    char buf[32];
    char *endp;

    if (!out || !lc_get_tool_field_text(line, name, buf, sizeof(buf)))
        return false;
    *out = (float)strtod(buf, &endp);
    return endp != buf;
}

static bool lc_is_tool_line(const char *line)
{
    line = lc_skip_line_number(line);
    return strncmp(line, "TOOL ", 5) == 0 || strncmp(line, "TOOL|", 5) == 0;
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
    view->full_screen = full_screen;
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

static float lc_sim_view_z_from_x(const lc_sim_view_t *view, int x)
{
    if (!view || view->scale <= 0.0001f)
        return 0.0f;
    return ((float)x - (float)view->z0_x) / view->scale;
}

static float lc_sim_view_d_from_y(const lc_sim_view_t *view, int y)
{
    if (!view || view->scale <= 0.0001f)
        return 0.0f;
    return (((float)y - (float)view->stock_top) / view->scale) * 2.0f;
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
    name_w = lvds_draw_text_width(name, LC_PREVIEW_LABEL_FONT) + lvds_draw_text_width(" ", LC_PREVIEW_LABEL_FONT);
    value_w = lvds_draw_text_width(value_text, LC_PREVIEW_LABEL_FONT);
    tx = lc_clampi(x, view->x0 + 2, view->x1 - name_w - value_w - 2);
    ty = lc_clampi(y, view->y0 + 2, view->y1 - LC_PREVIEW_LABEL_H);

    lvds_draw_text(tx, ty, name, LC_PREVIEW_LABEL_FG, LC_PREVIEW_PANEL_BG, LC_PREVIEW_LABEL_FONT);
    value_fg = lc_sim_active_field_is(frame, name) ? LC_PREVIEW_ACTIVE_VALUE_FG : LC_PREVIEW_VALUE_FG;
    value_bg = lc_sim_active_field_is(frame, name) ? LC_PREVIEW_ACTIVE_VALUE_BG : LC_PREVIEW_PANEL_BG;
    lvds_draw_text(tx + name_w, ty, value_text, value_fg, value_bg, LC_PREVIEW_LABEL_FONT);
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
    max_w = lvds_draw_text_width("Z2 ", LC_PREVIEW_LABEL_FONT) + lvds_draw_text_width(z2_text, LC_PREVIEW_LABEL_FONT);
    max_w = lvds_draw_text_width("D2 ", LC_PREVIEW_LABEL_FONT) + lvds_draw_text_width(d2_text, LC_PREVIEW_LABEL_FONT) > max_w ?
            lvds_draw_text_width("D2 ", LC_PREVIEW_LABEL_FONT) + lvds_draw_text_width(d2_text, LC_PREVIEW_LABEL_FONT) : max_w;
    if (has_corner) {
        int cw = lvds_draw_text_width(corner_name, LC_PREVIEW_LABEL_FONT) +
                 lvds_draw_text_width(" ", LC_PREVIEW_LABEL_FONT) +
                 lvds_draw_text_width(corner_text, LC_PREVIEW_LABEL_FONT);
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

static bool lc_live_line_is_thread(const char *line);

static bool lc_live_sim_running(const ui_snapshot_frame_t *frame)
{
    uint32_t now;

    if (!frame) {
        return false;
    }
#if !LVDS_RENDERER_AUTO_LIVE_IN_NC_VIEW
    if (frame->leancam_mode == LC_RENDER_MODE_NC_VIEW) {
        return false;
    }
#endif
    now = mcu_millis();
    if (frame->g33_active || frame->g33_sync_valid) {
        g_live_sim_thread_hold_until_ms = now + LVDS_RENDERER_G33_LIVE_HOLD_MS;
        return true;
    }
#if LVDS_RENDERER_AUTO_LIVE_SIM
    if (lc_live_line_is_thread(frame->leancam_preview_line) &&
        (frame->motion_active || (frame->state & (EXEC_RUN | EXEC_HOLD)))) {
        g_live_sim_thread_hold_until_ms = now + LVDS_RENDERER_G33_LIVE_HOLD_MS;
        return true;
    }
    if ((int32_t)(g_live_sim_thread_hold_until_ms - now) > 0) {
        return true;
    }
    return frame->motion_active || (frame->state & (EXEC_RUN | EXEC_HOLD));
#else
    (void)frame;
    return false;
#endif
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

static bool lc_live_line_is_thread(const char *line)
{
    if (!line || !line[0]) {
        return false;
    }
    return lc_is_cycle(line, "THR_OD") ||
           lc_is_cycle(line, "THR_ID") ||
           lc_is_cycle(line, "THREAD") ||
           lc_is_cycle(line, "G76") ||
           strstr(line, "G33") != NULL ||
           strstr(line, "g33") != NULL;
}

static bool lc_live_sim_is_threading(const ui_snapshot_frame_t *frame)
{
    if (!frame) {
        return false;
    }
    return frame->g33_active || lc_live_line_is_thread(frame->leancam_preview_line);
}

static bool lc_live_sim_thread_hold_active(const ui_snapshot_frame_t *frame)
{
    uint32_t now;

    if (!frame || frame->g33_active || frame->g33_sync_valid) {
        return false;
    }
    now = mcu_millis();
    if ((int32_t)(g_live_sim_thread_hold_until_ms - now) <= 0) {
        return false;
    }
    return !frame->motion_active && !(frame->state & (EXEC_RUN | EXEC_HOLD));
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
        frame && !lc_field_float3(frame->leancam_tool_line, "DOC", "R_DOC", "ROUGH_DOC", &doc)) {
        doc = 1.0f;
    }
    if (doc < 0.2f) {
        doc = 0.2f;
    }
    return lc_clampi((int)((doc * view->scale * scale_factor) + 1.5f), 4, 48);
}

static void lc_live_sim_reset(const ui_snapshot_frame_t *frame, bool full_screen)
{
    if (!g_live_sim_mask || !frame) {
        g_live_sim_ready = false;
        return;
    }

    lc_sim_read_setup(frame, &g_live_sim_setup);
    lc_sim_build_view(&g_live_sim_setup, &g_live_sim_view, full_screen);
    memset(g_live_sim_mask, 0, LC_LIVE_SIM_W * LC_LIVE_SIM_H);

    int material_top = g_live_sim_setup.id > 0.0f ? lc_live_sim_my(&g_live_sim_view, g_live_sim_setup.id) : 0;
    for (int y = material_top; y <= g_live_sim_view.stock_bottom - g_live_sim_view.stock_top && y < LC_LIVE_SIM_H; ++y) {
        memset(g_live_sim_mask + (y * LC_LIVE_SIM_W), 1,
               (size_t)(g_live_sim_view.stock_right - g_live_sim_view.stock_left + 1));
    }

    ui_snapshot_strcpy(g_live_sim_title, frame->leancam_title, sizeof(g_live_sim_title));
    ui_snapshot_strcpy(g_live_sim_line, frame->leancam_preview_line, sizeof(g_live_sim_line));
    ui_snapshot_strcpy(g_live_sim_setup_line, frame->leancam_setup_line, sizeof(g_live_sim_setup_line));
    ui_snapshot_strcpy(g_live_sim_tool_line, frame->leancam_tool_line, sizeof(g_live_sim_tool_line));
    g_live_sim_has_last = false;
    g_live_sim_static_drawn = false;
    g_live_thread_z_set = false;
    g_live_tool_rect_valid = false;
    g_live_tool_rect_thread = false;
    g_live_chuck_collision_valid = false;
    g_live_sim_full_screen = full_screen;
    g_live_sim_ready = true;
}

static void lc_live_sim_force_redraw(void)
{
    g_live_sim_has_last = false;
    g_live_sim_static_drawn = false;
    g_live_thread_z_set = false;
    g_live_tool_rect_valid = false;
    g_live_tool_rect_thread = false;
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

static void lc_live_sim_remove_thread_front(int lane_x,
                                            int tool_x,
                                            int tip_y,
                                            int h,
                                            int dir)
{
    int half_h;
    int front_x;

    if (!g_live_sim_mask || !g_live_sim_ready) {
        return;
    }

    if (h < 2) h = 2;
    half_h = h / 2;
    if (half_h < 1) half_h = 1;
    front_x = dir < 0 ? tool_x : tool_x + 1;

    for (int y = 0; y <= half_h; ++y) {
        int yy = tip_y + y;
        int span;
        if (yy < 0 || yy >= LC_LIVE_SIM_H) {
            continue;
        }
        span = (int)(((int32_t)y * LC_THREAD_FLANK_TAN30_NUM) / LC_THREAD_FLANK_TAN30_DEN);
        if (dir < 0) {
            lc_live_sim_remove_rect(lane_x - span, yy, front_x, yy);
        } else {
            lc_live_sim_remove_rect(front_x, yy, lane_x + span, yy);
        }
    }
}

static bool lc_live_thread_z_params(const ui_snapshot_frame_t *frame, float *start_z, float *pitch, int *dir)
{
    float z1 = 0.0f;
    float z2 = 0.0f;
    float p = 0.0f;
    bool have_z1;
    bool have_z2;

    if (!frame) {
        return false;
    }

    if (!lc_field_float3(frame->leancam_preview_line, "P", "PITCH", "K", &p)) {
        return false;
    }
    p = lc_absf(p);
    if (p <= 0.0f) {
        return false;
    }

    have_z1 = lc_field_float2(frame->leancam_preview_line, "Z1", "Z_START", &z1);
    have_z2 = lc_field_float2(frame->leancam_preview_line, "Z2", "Z_END", &z2);
    if (!have_z1) {
        z1 = g_live_sim_last_z;
    }

    if (start_z) *start_z = z1;
    if (pitch) *pitch = p;
    if (dir) {
        if (have_z2 && z2 < z1) {
            *dir = -1;
        } else if (have_z2 && z2 > z1) {
            *dir = 1;
        } else {
            *dir = (g_live_sim_last_z > z1) ? -1 : 1;
        }
    }
    return true;
}

static bool lc_live_thread_phase_allows_cut(const ui_snapshot_frame_t *frame, float z_pos, float *cut_z)
{
    float start_z = 0.0f;
    float pitch = 0.0f;
    int dir = 1;

    if (cut_z) {
        *cut_z = z_pos;
    }
    if (!lc_live_thread_z_params(frame, &start_z, &pitch, &dir)) {
        return false;
    }

    if (!g_live_thread_z_set ||
        lc_absf(g_live_thread_start_z - start_z) > 0.0001f ||
        lc_absf(g_live_thread_pitch - pitch) > 0.0001f ||
        g_live_thread_z_dir != dir) {
        g_live_thread_start_z = start_z;
        g_live_thread_pitch = pitch;
        g_live_thread_z_dir = dir;
        g_live_thread_next_cut_z = start_z;
        g_live_thread_z_set = true;
    } else if ((dir < 0 && z_pos > start_z) ||
               (dir > 0 && z_pos < start_z)) {
        g_live_thread_next_cut_z = start_z;
        return false;
    }

    if (dir < 0) {
        if (z_pos > g_live_thread_next_cut_z) {
            return false;
        }
        if (cut_z) {
            *cut_z = g_live_thread_next_cut_z;
        }
        for (int guard = 0; guard < 256 && z_pos <= g_live_thread_next_cut_z; ++guard) {
            g_live_thread_next_cut_z -= pitch;
        }
    } else {
        if (z_pos < g_live_thread_next_cut_z) {
            return false;
        }
        if (cut_z) {
            *cut_z = g_live_thread_next_cut_z;
        }
        for (int guard = 0; guard < 256 && z_pos >= g_live_thread_next_cut_z; ++guard) {
            g_live_thread_next_cut_z += pitch;
        }
    }

    return true;
}

static bool lc_live_sim_cut_thread_lanes(const ui_snapshot_frame_t *frame,
                                         int is_id,
                                         float z0,
                                         float z1,
                                         int mx0,
                                         int mx1,
                                         int my1,
                                         int doc_z_px,
                                         int doc_x_px)
{
    float cut_z = z1;
    int dir = 1;
    int cut_mx;
    int tool_front_x;
    int flank_tip_y;

    (void)z0;
    (void)mx0;

    if (!lc_live_thread_phase_allows_cut(frame, z1, &cut_z)) {
        return false;
    }
    (void)lc_live_thread_z_params(frame, NULL, NULL, &dir);

    cut_mx = lc_live_sim_mx(&g_live_sim_view, cut_z);
    tool_front_x = dir < 0 ? (mx1 - doc_z_px) : (mx1 + doc_z_px);
    flank_tip_y = is_id ? my1 : my1 + doc_x_px;
    lc_live_sim_remove_thread_front(cut_mx, tool_front_x, flank_tip_y, doc_x_px, dir);

    return true;
}

static void lc_live_sim_cut_swept_rect(const ui_snapshot_frame_t *frame, float x0, float z0, float x1, float z1)
{
    float d0 = lc_live_runtime_x_to_diam(x0);
    float d1 = lc_live_runtime_x_to_diam(x1);
    bool is_thread = lc_live_sim_is_threading(frame);
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

    if (is_thread) {
        int doc_z_px = lc_live_doc_px(frame, &g_live_sim_view, 1.0f);
        int doc_x_px = lc_live_doc_px(frame, &g_live_sim_view, 0.7f);
        (void)lc_live_sim_cut_thread_lanes(frame,
                                           lc_live_sim_is_id_cycle(frame),
                                           z0,
                                           z1,
                                           mx0,
                                           mx1,
                                           my1,
                                           doc_z_px,
                                           doc_x_px);
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
    return strcmp(g_live_sim_title, frame->leancam_title) != 0 ||
           strcmp(g_live_sim_line, frame->leancam_preview_line) != 0 ||
           strcmp(g_live_sim_setup_line, frame->leancam_setup_line) != 0 ||
           strcmp(g_live_sim_tool_line, frame->leancam_tool_line) != 0;
}

static bool lc_live_sim_needs_reset(const ui_snapshot_frame_t *frame, bool full_screen)
{
    return !g_live_sim_ready ||
           g_live_sim_full_screen != full_screen ||
           (!lc_live_sim_thread_hold_active(frame) &&
            lc_live_sim_context_changed(frame));
}

static void lc_live_sim_update_cut(const ui_snapshot_frame_t *frame)
{
    float x;
    float z;
    bool should_cut = true;

    if (!g_live_sim_ready || !frame || !frame->axes_valid) {
        return;
    }

    x = frame->axis[0];
    z = frame->axis[2];
    if (lc_live_line_is_thread(frame->leancam_preview_line) &&
        !frame->g33_active &&
        !frame->g33_sync_valid) {
        should_cut = false;
        g_live_thread_z_set = false;
    }

    if (should_cut) {
        if (g_live_sim_has_last) {
            lc_live_sim_cut_swept_rect(frame, g_live_sim_last_x, g_live_sim_last_z, x, z);
        } else {
            lc_live_sim_cut_swept_rect(frame, x, z, x, z);
        }
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
            lvds_draw_fill_rect(g_live_sim_view.stock_left + active_start,
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
        lvds_draw_fill_rect(g_live_sim_view.stock_left + active_start,
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
    lvds_draw_text(lc_clampi(x, view->x0 + 4, view->x1 - 70),
                    lc_clampi(y, view->y0 + 8, view->y1 - 18),
                    buf, LC_PREVIEW_LABEL_FG, LC_PREVIEW_PANEL_BG, LVDS_FONT_NORMAL);
}

static void lc_sim_label_large(const lc_sim_view_t *view, int x, int y, const char *name, float v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %.1f", name, (double)v);
    lvds_draw_text(lc_clampi(x, view->x0 + 4, view->x1 - 110),
                    lc_clampi(y, view->y0 + 8, view->y1 - 18),
                    buf, LC_LIVE_LABEL_FG, LC_LIVE_PANEL_BG, LVDS_FONT_NORMAL);
}

static void lc_sim_hatch_rect(int x, int y, int w, int h, bool vertical)
{
    int p;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    lvds_draw_fill_rect(x, y, w, h, LC_PREVIEW_CUT_FG);
    lvds_draw_rect(x, y, w, h, LC_PREVIEW_HATCH_FG);
    if (vertical) {
        for (p = x + 6; p < x + w; p += 6) {
            lvds_draw_line(p, y, p, y + h - 1, LC_PREVIEW_HATCH_FG);
        }
    } else {
        for (p = y + 6; p < y + h; p += 6) {
            lvds_draw_line(x, p, x + w - 1, p, LC_PREVIEW_HATCH_FG);
        }
    }
}

static void lc_sim_hatch_vline(int x, int y1, int y2)
{
    int tmp;

    if (x < LC_LEFT_PANE_X || x >= LVDS_HSTX_WIDTH) {
        return;
    }
    if (y2 < y1) { tmp = y1; y1 = y2; y2 = tmp; }
    y1 = lc_clampi(y1, 42, 547);
    y2 = lc_clampi(y2, 42, 547);
    if (y2 <= y1) {
        return;
    }
    lvds_draw_line(x, y1, x, y2, LC_PREVIEW_CUT_FG);
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
    if (!view->full_screen) {
        lvds_draw_text(view->x0, view->y0, "Cycle preview", LC_PREVIEW_TITLE_FG, LC_PREVIEW_PANEL_BG, LVDS_FONT_NORMAL);
    }
    lvds_draw_line(view->stock_left - 20, view->stock_top, view->stock_right + 20, view->stock_top, LC_PREVIEW_AXIS_FG);
    lvds_draw_line(view->z0_x, view->stock_top - 24, view->z0_x, view->stock_bottom + 28, LC_PREVIEW_AXIS_FG);
    lvds_draw_fill_rect(view->stock_left, view->stock_top,
                         view->stock_right - view->stock_left + 1,
                         view->stock_bottom - view->stock_top + 1,
                         LC_PREVIEW_STOCK_FG);
    if (setup->id > 0.0f) {
        int id_y = lc_sim_dy(view, setup->id);
        if (id_y > view->stock_top) {
            lvds_draw_fill_rect(view->stock_left, view->stock_top + 1,
                                 view->stock_right - view->stock_left + 1,
                                 id_y - view->stock_top,
                                 LC_PREVIEW_PANEL_BG);
            lvds_draw_line(view->stock_left, id_y, view->stock_right, id_y, LC_PREVIEW_AXIS_FG);
        }
    }
    lvds_draw_rect(view->stock_left, view->stock_top,
                    view->stock_right - view->stock_left + 1,
                    view->stock_bottom - view->stock_top + 1,
                    LC_PREVIEW_AXIS_FG);

    if (setup->clamp > 0.0f) {
        lc_sim_draw_chuck(view, setup);
    }

    lvds_draw_text(view->z0_x - 12, view->stock_top - 24, "Z0", LC_PREVIEW_LABEL_FG, LC_PREVIEW_PANEL_BG, LVDS_FONT_NORMAL);
    lc_sim_label(view,
                 view->stock_left + (view->full_screen ? 4 : 4),
                 view->stock_top - (view->full_screen ? 26 : 42),
                 "L",
                 setup->length);
    lc_sim_label(view,
                 view->stock_left + (view->full_screen ? 92 : 92),
                 view->stock_top - (view->full_screen ? 26 : 42),
                 "OD",
                 setup->od);
    if (setup->id > 0.0f) {
        lc_sim_label(view,
                     view->stock_left + 190,
                     view->stock_top - (view->full_screen ? 26 : 42),
                     "ID",
                     setup->id);
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
    lvds_draw_fill_rect(jaw_x, view->stock_bottom - 18,
                         view->stock_left - jaw_x + 1, 62, LC_PREVIEW_CHUCK_FG);
    lvds_draw_fill_rect(view->stock_left, y1, clamp_w, y2 - y1 + 1, LC_PREVIEW_CHUCK_FG);

    snprintf(buf, sizeof(buf), "CL %.1f", (double)setup->clamp);
    lvds_draw_text(10, y1 + 13, buf,
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

    lvds_draw_fill_rect(jaw_x, view->stock_bottom - 18,
                         view->stock_left - jaw_x + 1, 62, fill);
    lvds_draw_fill_rect(view->stock_left, y1, clamp_w, y2 - y1 + 1, fill);
    snprintf(buf, sizeof(buf), collision ? "HIT" : "CL %.1f", (double)setup->clamp);
    lvds_draw_text(10, y1 + 13, buf, LC_LIVE_BG, fill, LVDS_FONT_NORMAL);
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
    x1 = lc_clampi(x1, view->x0 + 2, view->x1 - 2);
    x2 = lc_clampi(x2, view->x0 + 2, view->x1 - 2);
    if (x2 <= x1) x2 = x1 + 1;
    if (x2 > view->x1 - 2) x2 = view->x1 - 2;
    if (x2 <= x1) return;

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
                    lvds_draw_line(seg_start, y, x - 1, y, LC_PREVIEW_HATCH_FG);
                }
                seg_start = -1;
            }
        }
        if (seg_start >= 0 && x2 > seg_start) {
            lvds_draw_line(seg_start, y, x2, y, LC_PREVIEW_HATCH_FG);
        }
    }

    for (x = x1; x <= x2; ++x) {
        float z = ((float)x - (float)view->z0_x) / view->scale;
        profile_y = lc_sim_dy(view, lc_sim_profile_d_at_z(shape, is_od, z));

        if (have_prev) {
            lvds_draw_line(prev_x, prev_y, x, profile_y, LC_PREVIEW_PROFILE_FG);
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
        (frame && lc_field_float3(frame->leancam_tool_line, "DOC", "R_DOC", "ROUGH_DOC", &doc))) {
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

static bool lc_sim_build_radius_turn_line(const char *line,
                                          const lc_sim_setup_t *setup,
                                          bool is_od,
                                          char *out,
                                          size_t out_sz)
{
    float d;
    float z1;
    float z2;
    float r;
    float d1;
    float q = is_od ? 0.0f : 2.0f;

    if (!line || !setup || !out || out_sz == 0) {
        return false;
    }
    if (!lc_field_float2(line, "D", "DIAMETER", &d)) {
        return false;
    }
    if (!lc_field_float2(line, "Z1", "Z_1", &z1)) {
        return false;
    }
    if (!lc_field_float2(line, "Z2", "Z_2", &z2)) {
        return false;
    }
    if (!lc_field_float2(line, "R", "RADIUS", &r)) {
        return false;
    }
    d1 = is_od ? setup->od : setup->id;
    if (d1 <= 0.0f) {
        d1 = d;
    }
    if (r <= 0.0f || d <= 0.0f) {
        return false;
    }
    if (is_od && d > d1) {
        return false;
    }
    if (!is_od && d < d1) {
        return false;
    }
    (void)lc_field_float(line, "Q", &q);

    snprintf(out,
             out_sz,
             "%s|D1{%.3f}|Z1{%.3f}|Z2{%.3f}|D2{%.3f}|RND{%.3f}|Q{%.0f}",
             is_od ? "OD" : "ID",
             (double)d1,
             (double)z1,
             (double)z2,
             (double)d,
             (double)r,
             (double)q);
    return true;
}

static bool lc_sim_build_chamfer_turn_line(const char *line,
                                           const lc_sim_setup_t *setup,
                                           bool is_od,
                                           char *out,
                                           size_t out_sz)
{
    float d;
    float z;
    float size;
    float d1;
    float q = is_od ? 0.0f : 2.0f;

    if (!line || !setup || !out || out_sz == 0) {
        return false;
    }
    if (!lc_field_float2(line, "D", "DIAMETER", &d)) {
        return false;
    }
    if (!lc_field_float(line, "Z", &z)) {
        return false;
    }
    if (!lc_field_float2(line, "SIZE", "CHMF", &size)) {
        return false;
    }
    d1 = is_od ? setup->od : setup->id;
    if (d1 <= 0.0f) {
        if (is_od) {
            d1 = d + (2.0f * size);
        } else {
            d1 = d - (2.0f * size);
            if (d1 < 0.0f) {
                d1 = 0.0f;
            }
        }
    }
    (void)lc_field_float(line, "Q", &q);

    snprintf(out,
             out_sz,
             "%s|D1{%.3f}|Z1{%.3f}|Z2{%.3f}|D2{%.3f}|CHMF{%.3f}|Q{%.0f}",
             is_od ? "OD" : "ID",
             (double)d1,
             (double)(z + size),
             (double)z,
             (double)d,
             (double)size,
             (double)q);
    return true;
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

    lvds_draw_line(zx, view->y0 + 8, zx, view->y1 - 8, LC_PREVIEW_TOOL_FG);
    lvds_draw_line(view->x0 + 8, dy, view->x1 - 8, dy, LC_PREVIEW_TOOL_FG);
    lvds_draw_fill_rect(zx - 5, dy - 5, 11, 11, LC_PREVIEW_TOOL_MARK);
    lvds_draw_rect(zx - 7, dy - 7, 15, 15, LC_PREVIEW_TOOL_OUTLINE);
}

static void lc_tool_active_edges(int orient, bool *left, bool *top, bool *right, bool *bottom)
{
    if (left) *left = (orient == 1 || orient == 4 || orient == 7 || orient == 2 || orient == 5 || orient == 8);
    if (top) *top = (orient == 7 || orient == 8 || orient == 9 || orient == 4 || orient == 5 || orient == 6);
    if (right) *right = (orient == 3 || orient == 6 || orient == 9 || orient == 2 || orient == 5 || orient == 8);
    if (bottom) *bottom = (orient == 1 || orient == 2 || orient == 3 || orient == 4 || orient == 5 || orient == 6);
}

static int lc_tool_tip_corner(int orient)
{
    switch (orient) {
        case 4:
        case 7:
        case 8:
            return 7;
        case 6:
        case 9:
            return 9;
        case 3:
            return 3;
        case 1:
        case 2:
        default:
            return 1;
    }
}

static bool lc_tool_orient_code_valid(int orient)
{
    int digits = 0;

    if (orient == 0) {
        return true;
    }

    if (orient < 0 || orient > 9999) {
        return false;
    }

    while (orient > 0) {
        int d = orient % 10;
        if (d < 1 || d > 9) {
            return false;
        }
        orient /= 10;
        digits++;
    }

    return digits >= 1 && digits <= 4;
}

static int lc_tool_orient_digits(int orient, int *digits, int max_digits)
{
    int tmp[4];
    int n = 0;

    while (orient > 0 && n < (int)(sizeof(tmp) / sizeof(tmp[0]))) {
        tmp[n++] = orient % 10;
        orient /= 10;
    }
    if (orient > 0 || n <= 0 || n > max_digits) {
        return 0;
    }
    for (int i = 0; i < n; ++i) {
        digits[i] = tmp[n - 1 - i];
    }
    return n;
}

static bool lc_tool_keypad_point(int digit, int origin_x, int origin_y, int step, int *x, int *y)
{
    static const int kx[10] = {0, -1, 0, 1, -1, 0, 1, -1, 0, 1};
    static const int ky[10] = {0,  1, 1, 1,  0, 0, 0, -1,-1,-1};

    if (digit < 1 || digit > 9) {
        return false;
    }
    if (x) *x = origin_x + (kx[digit] * step);
    if (y) *y = origin_y + (ky[digit] * step);
    return true;
}

static int lc_tool_shape_tip_digit(int orient)
{
    int digits[4];
    int n = lc_tool_orient_digits(orient, digits, 4);

    if (n <= 1) {
        return lc_tool_tip_corner(orient);
    }

    if (n == 3) {
        return digits[1];
    }

    if (n == 4) {
        return digits[1];
    }
    return digits[0];
}

static void lc_draw_tool_doc_dot(int tip_x, int tip_y, int size)
{
    int r = lc_clampi(size / 2, 4, 28);
    lvds_draw_fill_ellipse(tip_x, tip_y, r, r, lvds_palette_color(red_bright));
    lvds_draw_ellipse(tip_x, tip_y, r, r, LC_PREVIEW_TOOL_OUTLINE);
}

static int lc_tool_orient_from_frame(const ui_snapshot_frame_t *frame)
{
    float orient;

    if (frame && lc_field_float(frame->leancam_tool_line, "ORIENT", &orient)) {
        int oi = (int)(orient + (orient >= 0.0f ? 0.5f : -0.5f));
        if (lc_tool_orient_code_valid(oi)) {
            return oi;
        }
    }
    return 0;
}

static int lc_live_tool_edge_origin(int tip, int size, bool near_edge, bool far_edge)
{
    if (near_edge && far_edge) {
        return tip - (size / 2);
    }
    if (near_edge) {
        return tip;
    }
    if (far_edge) {
        return tip - size + 1;
    }
    return tip - (size / 2);
}

static void lc_tool_marker_line(int x1, int y1, int x2, int y2, lvds_color_t color, int thick)
{
    x1 = lc_clampi(x1, 0, LVDS_HSTX_WIDTH - 1);
    y1 = lc_clampi(y1, 0, LVDS_HSTX_HEIGHT - 1);
    x2 = lc_clampi(x2, 0, LVDS_HSTX_WIDTH - 1);
    y2 = lc_clampi(y2, 0, LVDS_HSTX_HEIGHT - 1);

    if (thick > 1) {
        lvds_draw_line_w(x1, y1, x2, y2, color, thick);
    } else {
        lvds_draw_line(x1, y1, x2, y2, color);
    }
}

static void lc_fill_triangle(int x1, int y1,
                             int x2, int y2,
                             int x3, int y3,
                             lvds_color_t color)
{
    int min_y = y1;
    int max_y = y1;

    if (y2 < min_y) min_y = y2;
    if (y3 < min_y) min_y = y3;
    if (y2 > max_y) max_y = y2;
    if (y3 > max_y) max_y = y3;

    for (int y = min_y; y <= max_y; ++y) {
        int xs[3];
        int n = 0;

        if ((y1 <= y && y < y2) || (y2 <= y && y < y1)) {
            xs[n++] = x1 + ((x2 - x1) * (y - y1)) / (y2 - y1);
        }
        if ((y2 <= y && y < y3) || (y3 <= y && y < y2)) {
            xs[n++] = x2 + ((x3 - x2) * (y - y2)) / (y3 - y2);
        }
        if ((y3 <= y && y < y1) || (y1 <= y && y < y3)) {
            xs[n++] = x3 + ((x1 - x3) * (y - y3)) / (y1 - y3);
        }

        if (n >= 2) {
            int xa = xs[0];
            int xb = xs[1];
            if (xa > xb) {
                int t = xa;
                xa = xb;
                xb = t;
            }
            if (y < 0 || y >= LVDS_HSTX_HEIGHT) {
                continue;
            }
            xa = lc_clampi(xa, 0, LVDS_HSTX_WIDTH - 1);
            xb = lc_clampi(xb, 0, LVDS_HSTX_WIDTH - 1);
            if (xb >= xa) {
                lvds_draw_fill_rect(xa, y, xb - xa + 1, 1, color);
            }
        }
    }
}

static void lc_draw_tool_drill_marker(int tip_x, int tip_y, int size, int thick)
{
    int len = MAX(8, size / 3);
    int half = (len * 1732) / 1000; /* 120 degree included drill point. */
    int base_x = tip_x + len;
    lvds_color_t fill = lvds_palette_color(yellow);
    lvds_color_t edge = lvds_palette_color(red_bright);

    lc_fill_triangle(tip_x, tip_y, base_x, tip_y - half, base_x, tip_y, fill);
    lc_fill_triangle(tip_x, tip_y, base_x, tip_y, base_x, tip_y + half, fill);
    lc_tool_marker_line(tip_x, tip_y, base_x, tip_y - half, edge, thick);
    lc_tool_marker_line(tip_x, tip_y, base_x, tip_y + half, edge, thick);
    lc_tool_marker_line(base_x, tip_y - half, base_x, tip_y + half, edge, thick);
    lc_tool_marker_line(tip_x, tip_y, base_x, tip_y, edge, 1);
}

static void lc_tool_marker_radius(int cx, int cy, int rr, int sx, int sy, lvds_color_t color, int thick)
{
    int last_x = cx;
    int last_y = cy - (sy * rr);

    if (rr <= 0) {
        return;
    }

    for (int i = 1; i <= rr; ++i) {
        int yy = (int)(sqrtf((float)(rr * rr - i * i)) + 0.5f);
        int px = cx - (sx * i);
        int py = cy - (sy * yy);
        lc_tool_marker_line(last_x, last_y, px, py, color, thick);
        last_x = px;
        last_y = py;
    }
}

static void lc_tool_marker_clear_radius_outside(int x, int y, int rr, int sx, int sy, lvds_color_t relief)
{
    int cx = x + (sx > 0 ? rr : 0);
    int cy = y + (sy > 0 ? rr : 0);

    if (rr <= 0) {
        return;
    }

    for (int py = y; py <= y + rr; ++py) {
        for (int px = x; px <= x + rr; ++px) {
            int dx = px - cx;
            int dy = py - cy;
            if ((dx * dx) + (dy * dy) > (rr * rr)) {
                lvds_draw_fill_rect(px, py, 1, 1, relief);
            }
        }
    }
}

static int lc_round_to_int(float v)
{
    return (int)(v + (v >= 0.0f ? 0.5f : -0.5f));
}

static void lc_tool_marker_rounded_corner(int cx,
                                          int cy,
                                          int ax,
                                          int ay,
                                          int bx,
                                          int by,
                                          int rr,
                                          lvds_color_t color,
                                          int thick)
{
    float avx = (float)(ax - cx);
    float avy = (float)(ay - cy);
    float bvx = (float)(bx - cx);
    float bvy = (float)(by - cy);
    float al = sqrtf((avx * avx) + (avy * avy));
    float bl = sqrtf((bvx * bvx) + (bvy * bvy));
    float r;
    float apx;
    float apy;
    float bpx;
    float bpy;
    int last_x;
    int last_y;

    if (rr <= 0 || al < 1.0f || bl < 1.0f) {
        lc_tool_marker_line(cx, cy, ax, ay, color, thick);
        lc_tool_marker_line(cx, cy, bx, by, color, thick);
        return;
    }

    r = (float)rr;
    if (r > al * 0.45f) r = al * 0.45f;
    if (r > bl * 0.45f) r = bl * 0.45f;

    apx = (float)cx + (avx / al) * r;
    apy = (float)cy + (avy / al) * r;
    bpx = (float)cx + (bvx / bl) * r;
    bpy = (float)cy + (bvy / bl) * r;

    lc_tool_marker_line(lc_round_to_int(apx), lc_round_to_int(apy), ax, ay, color, thick);
    lc_tool_marker_line(lc_round_to_int(bpx), lc_round_to_int(bpy), bx, by, color, thick);

    last_x = lc_round_to_int(apx);
    last_y = lc_round_to_int(apy);
    for (int i = 1; i <= 8; ++i) {
        float t = (float)i / 8.0f;
        float it = 1.0f - t;
        float qx = (it * it * apx) + (2.0f * it * t * (float)cx) + (t * t * bpx);
        float qy = (it * it * apy) + (2.0f * it * t * (float)cy) + (t * t * bpy);
        int x = lc_round_to_int(qx);
        int y = lc_round_to_int(qy);
        lc_tool_marker_line(last_x, last_y, x, y, color, thick);
        last_x = x;
        last_y = y;
    }
}

static void lc_draw_tool_marker_shape(int x,
                                      int y,
                                      int size,
                                      int rr,
                                      bool left,
                                      bool top,
                                      bool right,
                                      bool bottom,
                                      int thick,
                                      lvds_color_t relief)
{
    int x2 = x + size - 1;
    int y2 = y + size - 1;
    int left_top = (left && top) ? rr : 0;
    int top_right = (top && right) ? rr : 0;
    int right_bottom = (right && bottom) ? rr : 0;
    int bottom_left = (bottom && left) ? rr : 0;
    lvds_color_t fill = lvds_palette_color(yellow);
    lvds_color_t edge = lvds_palette_color(red_bright);

    if (size < 2) {
        return;
    }
    if (rr < 0) rr = 0;
    if (rr > size / 2) rr = size / 2;

    lvds_draw_fill_rect(x, y, size, size, fill);

    if (rr > 0) {
        if (left && top) lc_tool_marker_clear_radius_outside(x, y, rr, 1, 1, relief);
        if (top && right) lc_tool_marker_clear_radius_outside(x2 - rr, y, rr, -1, 1, relief);
        if (right && bottom) lc_tool_marker_clear_radius_outside(x2 - rr, y2 - rr, rr, -1, -1, relief);
        if (bottom && left) lc_tool_marker_clear_radius_outside(x, y2 - rr, rr, 1, -1, relief);
    }

    if (left && y + left_top <= y2 - bottom_left) {
        lc_tool_marker_line(x, y + left_top, x, y2 - bottom_left, edge, thick);
    }
    if (top && x + left_top <= x2 - top_right) {
        lc_tool_marker_line(x + left_top, y, x2 - top_right, y, edge, thick);
    }
    if (right && y + top_right <= y2 - right_bottom) {
        lc_tool_marker_line(x2, y + top_right, x2, y2 - right_bottom, edge, thick);
    }
    if (bottom && x + bottom_left <= x2 - right_bottom) {
        lc_tool_marker_line(x + bottom_left, y2, x2 - right_bottom, y2, edge, thick);
    }

    if (rr > 0) {
        if (left && top) lc_tool_marker_radius(x + rr, y + rr, rr, 1, 1, edge, thick);
        if (top && right) lc_tool_marker_radius(x2 - rr, y + rr, rr, -1, 1, edge, thick);
        if (right && bottom) lc_tool_marker_radius(x2 - rr, y2 - rr, rr, -1, -1, edge, thick);
        if (bottom && left) lc_tool_marker_radius(x + rr, y2 - rr, rr, 1, -1, edge, thick);
    }
}

static void lc_draw_tool_polygon_marker(int tip_x, int tip_y, int orient, int size, int rr, int thick)
{
    int digits[4];
    int px[4];
    int py[4];
    int tip_grid_x;
    int tip_grid_y;
    int step = lc_clampi(size / 2, 8, 56);
    int n = lc_tool_orient_digits(orient, digits, 4);
    lvds_color_t fill = lvds_palette_color(yellow);
    lvds_color_t edge = lvds_palette_color(red_bright);
    lvds_color_t mount = LC_COL_DIM;

    if (n < 3) {
        return;
    }

    if (n == 4) {
        int cut_x;
        int cut_y;
        int z_x;
        int z_y;
        if (!lc_tool_keypad_point(digits[1], 0, 0, step, &cut_x, &cut_y) ||
            !lc_tool_keypad_point(digits[2], 0, 0, step, &z_x, &z_y)) {
            return;
        }
        tip_grid_x = cut_x;  /* first middle point supplies the Z-screen anchor. */
        tip_grid_y = z_y;    /* second middle point supplies the X-screen anchor. */
    } else {
        int tip_digit = lc_tool_shape_tip_digit(orient);
        if (!lc_tool_keypad_point(tip_digit, 0, 0, step, &tip_grid_x, &tip_grid_y)) {
            return;
        }
    }

    for (int i = 0; i < n; ++i) {
        int gx;
        int gy;
        if (!lc_tool_keypad_point(digits[i], 0, 0, step, &gx, &gy)) {
            return;
        }
        px[i] = tip_x + gx - tip_grid_x;
        py[i] = tip_y + gy - tip_grid_y;
    }

    for (int i = 1; i + 1 < n; ++i) {
        lc_fill_triangle(px[0], py[0], px[i], py[i], px[i + 1], py[i + 1], fill);
    }

    if (n == 3) {
        lc_tool_marker_rounded_corner(px[1], py[1], px[0], py[0], px[2], py[2], rr, edge, thick);
    } else if (n == 4) {
        lc_tool_marker_line(px[1], py[1], px[2], py[2], edge, MAX(thick, 2));
        lc_tool_marker_line(px[3], py[3], px[0], py[0], mount, 1);
    }
}

static void lc_draw_tool_marker_panel(const ui_snapshot_frame_t *frame, int x, int y)
{
    char title[48];
    float orient_f;
    float r = 0.0f;
    int orient = 3;
    int size = 34;
    int sx = x + 26;
    int sy = y + 20;
    int rr = 0;
    bool left;
    bool top;
    bool right;
    bool bottom;

    if (!frame || !lc_is_tool_line(frame->leancam_tool_line)) {
        return;
    }
    if (lc_field_float(frame->leancam_tool_line, "ORIENT", &orient_f)) {
        orient = (int)(orient_f + (orient_f >= 0.0f ? 0.5f : -0.5f));
    }
    if (!lc_tool_orient_code_valid(orient)) orient = 0;
    (void)lc_field_float(frame->leancam_tool_line, "R", &r);
    if (r > 0.0f) rr = lc_clampi((int)(r * 7.0f + 0.5f), 1, size / 2);
    lc_tool_active_edges(orient > 9 ? lc_tool_shape_tip_digit(orient) : orient, &left, &top, &right, &bottom);

    lvds_draw_rect(x, y, 86, 58, LC_COL_LINE);
    snprintf(title, sizeof(title), "O%d R%.1f", orient, (double)r);
    lc_text_clip(x + 4, y + 4, title, 10, LC_COL_DIM, LC_COL_BG, LVDS_FONT_SMALL);
    if (orient == 0) {
        lc_draw_tool_doc_dot(sx + (size / 2), sy + (size / 2), size);
    } else if (orient > 9) {
        lc_draw_tool_polygon_marker(sx + (size / 2), sy + (size / 2), orient, size, rr, 1);
    } else {
        lc_draw_tool_marker_shape(sx, sy, size, rr, left, top, right, bottom, 1, LC_COL_BG);
    }
}

static void lc_draw_tool_tip_glyph_centered(const char *line,
                                            int x,
                                            int y,
                                            int box_size,
                                            int marker_size,
                                            bool selected,
                                            lvds_color_t relief)
{
    float orient_f = 3.0f;
    float r = 0.0f;
    int orient;
    int rr = 0;
    int sx = x + ((box_size - marker_size) / 2);
    int sy = y + ((box_size - marker_size) / 2);
    int tip_x = x + (box_size / 2);
    int tip_y = y + (box_size / 2);
    int tip_corner;
    bool left;
    bool top;
    bool right;
    bool bottom;

    if (!line || !lc_is_tool_line(line)) {
        return;
    }

    (void)lc_tool_field_float(line, "ORIENT", &orient_f);
    (void)lc_tool_field_float(line, "R", &r);
    orient = (int)(orient_f + (orient_f >= 0.0f ? 0.5f : -0.5f));
    if (!lc_tool_orient_code_valid(orient)) orient = 0;
    if (r > 0.0f) rr = lc_clampi((int)(r * 4.0f + 0.5f), 1, marker_size / 2);

    tip_corner = lc_tool_shape_tip_digit(orient);
    switch (tip_corner) {
        case 7:
            tip_x = sx;
            tip_y = sy;
            break;
        case 9:
            tip_x = sx + marker_size - 1;
            tip_y = sy;
            break;
        case 3:
            tip_x = sx + marker_size - 1;
            tip_y = sy + marker_size - 1;
            break;
        case 1:
        default:
            tip_x = sx;
            tip_y = sy + marker_size - 1;
            break;
    }

    lc_tool_active_edges(orient > 9 ? tip_corner : orient, &left, &top, &right, &bottom);
    if (orient == 0) {
        lc_draw_tool_doc_dot(x + (box_size / 2), y + (box_size / 2), marker_size);
    } else if (orient > 9) {
        lc_draw_tool_polygon_marker(x + (box_size / 2), y + (box_size / 2),
                                    orient, marker_size, rr, selected ? 2 : 1);
    } else if (orient == 5) {
        lc_draw_tool_drill_marker(x + (box_size / 2), y + (box_size / 2),
                                  marker_size, selected ? 2 : 1);
    } else {
        lc_draw_tool_marker_shape(sx, sy, marker_size, rr,
                                  left, top, right, bottom,
                                  selected ? 2 : 1, relief);
        lvds_draw_fill_ellipse(tip_x, tip_y, 1, 1, lvds_palette_color(red_bright));
    }
}

static void lc_draw_tool_tip_glyph_at_tip(const char *line,
                                          int tip_x,
                                          int tip_y,
                                          int size,
                                          bool selected,
                                          lvds_color_t relief)
{
    float orient_f = 3.0f;
    float r = 0.0f;
    int orient;
    int rr = 0;
    int sx = tip_x;
    int sy = tip_y;
    int tip_corner;
    bool left;
    bool top;
    bool right;
    bool bottom;

    if (!line || !lc_is_tool_line(line)) {
        return;
    }

    (void)lc_tool_field_float(line, "ORIENT", &orient_f);
    (void)lc_tool_field_float(line, "R", &r);
    orient = (int)(orient_f + (orient_f >= 0.0f ? 0.5f : -0.5f));
    if (!lc_tool_orient_code_valid(orient)) orient = 0;
    if (r > 0.0f) rr = lc_clampi((int)(r * 4.0f + 0.5f), 1, size / 2);

    tip_corner = lc_tool_shape_tip_digit(orient);
    switch (tip_corner) {
        case 7:
            sx = tip_x;
            sy = tip_y;
            break;
        case 9:
            sx = tip_x - size + 1;
            sy = tip_y;
            break;
        case 3:
            sx = tip_x - size + 1;
            sy = tip_y - size + 1;
            break;
        case 1:
        default:
            sx = tip_x;
            sy = tip_y - size + 1;
            break;
    }

    lc_tool_active_edges(orient > 9 ? tip_corner : orient, &left, &top, &right, &bottom);
    if (orient == 0) {
        lc_draw_tool_doc_dot(tip_x, tip_y, size);
    } else if (orient > 9) {
        lc_draw_tool_polygon_marker(tip_x, tip_y, orient, size, rr, selected ? 2 : 1);
    } else if (orient == 5) {
        lc_draw_tool_drill_marker(tip_x, tip_y, size, selected ? 2 : 1);
    } else {
        lc_draw_tool_marker_shape(sx, sy, size, rr,
                                  left, top, right, bottom,
                                  selected ? 2 : 1, relief);
    }
    lvds_draw_fill_ellipse(tip_x, tip_y, 2, 2, lvds_palette_color(red_bright));
}

static void lc_draw_tool_row_glyph(const char *line, int x, int y, bool selected)
{
    lc_draw_tool_tip_glyph_centered(line,
                                    x,
                                    y,
                                    18,
                                    selected ? 13 : 11,
                                    selected,
                                    selected ? LC_COL_SELECT : LC_COL_BG);
}

static void lc_draw_tool_param_value(const char *line,
                                     const char *key,
                                     const char *label,
                                     int x,
                                     int y,
                                     int value_cols)
{
    char value[32];
    char buf[48];

    if (!label)
        label = key;
    snprintf(buf, sizeof(buf), "%-8s", label);
    lc_text_clip(x, y, buf, 8, LC_COL_DIM, LC_COL_BG, LVDS_FONT_NORMAL);
    if (!lc_get_tool_field_text(line, key, value, sizeof(value)))
        value[0] = 0;
    lc_text_clip(x + 76, y, value[0] ? value : "-", value_cols, LC_COL_VALUE, LC_COL_BG, LVDS_FONT_NORMAL);
}

static void lc_draw_tool_table_detail(const ui_snapshot_frame_t *frame, int y)
{
    const char *line = NULL;
    int i;
    int panel_x = 18;
    int panel_w = LVDS_HSTX_WIDTH - 36;
    int glyph_x = panel_x + 18;
    int glyph_y = y + 28;
    int axis_x = glyph_x + 32;
    int axis_y = glyph_y + 36;
    int left_x = panel_x + 112;
    int mid_x = panel_x + 345;

    if (!frame)
        return;

    for (i = 0; i < frame->leancam_line_count && i < UI_LC_MAX_LINES; ++i) {
        if (frame->leancam_line_selected[i] &&
            lc_is_tool_line(frame->leancam_lines[i])) {
            line = frame->leancam_lines[i];
            break;
        }
    }
    if (!line)
        return;

    lvds_draw_line(panel_x, y, panel_x + panel_w, y, LC_COL_LINE);
    lc_text_clip(glyph_x, y + 8, "Tool tip", 10, LC_COL_TEXT, LC_COL_BG, LVDS_FONT_NORMAL);
    lvds_draw_line(glyph_x, axis_y, glyph_x + 76, axis_y, LC_PREVIEW_AXIS_FG);
    lvds_draw_line(axis_x, glyph_y + 4, axis_x, glyph_y + 68, LC_PREVIEW_AXIS_FG);
    lc_text_clip(axis_x + 4, glyph_y + 4, "X0", 4, LC_PREVIEW_AXIS_FG, LC_COL_BG, LVDS_FONT_SMALL);
    lc_text_clip(glyph_x + 54, axis_y + 4, "Z0", 4, LC_PREVIEW_AXIS_FG, LC_COL_BG, LVDS_FONT_SMALL);
    lc_draw_tool_tip_glyph_at_tip(line, axis_x, axis_y, 42, true, LC_COL_BG);

    lc_draw_tool_param_value(line, "T", "T", left_x, y + 34, 9);
    lc_draw_tool_param_value(line, "ORIENT", "ORIENT", left_x, y + 50, 9);
    lc_draw_tool_param_value(line, "R", "RADIUS", left_x, y + 66, 9);
    lc_draw_tool_param_value(line, "DOC", "DOC", left_x, y + 82, 9);

    lc_draw_tool_param_value(line, "FIN_DOC", "FIN_DOC", mid_x, y + 34, 9);
    lc_draw_tool_param_value(line, "R_FEED", "R_FEED", mid_x, y + 50, 9);
    lc_draw_tool_param_value(line, "FIN_FEED", "FIN_FEED", mid_x, y + 66, 9);
    lc_draw_tool_param_value(line, "RPM", "RPM", mid_x, y + 82, 9);
    lc_draw_tool_param_value(line, "XOFF", "XOFF", mid_x + 230, y + 34, 8);
    lc_draw_tool_param_value(line, "ZOFF", "ZOFF", mid_x + 230, y + 50, 8);
}

static bool lc_frame_is_tool_asset(const ui_snapshot_frame_t *frame)
{
    if (!frame) {
        return false;
    }

    return strncmp(frame->leancam_title, "Tool Catalog", 12) == 0 ||
           strncmp(frame->leancam_title, "Tool Glyph", 10) == 0;
}

static bool lc_frame_is_catalog_asset(const ui_snapshot_frame_t *frame)
{
    return lc_frame_is_tool_asset(frame);
}

static void lc_draw_tool_asset_preview(const ui_snapshot_frame_t *frame)
{
    const char *line;
    char buf[64];
    float v = 0.0f;
    float r = 0.0f;
    float doc = 2.0f;
    float orient_f = 3.0f;
    int orient = 3;
    int size;
    int sx;
    int sy;
    int tip_x;
    int tip_y;
    int tip_corner;
    int block_x = LC_TOOL_EDITOR_LEFT_X + 16;
    int block_y = 112;
    int block_w = LC_TOOL_EDITOR_LEFT_W - 32;
    int block_h = 188;
    int rr = 0;
    bool left;
    bool top;
    bool right;
    bool bottom;

    if (!frame) {
        return;
    }

    line = frame->leancam_tool_line[0] ? frame->leancam_tool_line : frame->leancam_preview_line;
    if (!lc_is_tool_line(line)) {
        lvds_draw_fill_rect(LC_TOOL_EDITOR_LEFT_X, 86, LC_TOOL_EDITOR_LEFT_W, 458, LC_COL_BG);
        lc_text_clip(LC_TOOL_EDITOR_LEFT_X + 16, 128, "Select or create a TOOL asset", 34,
                     LC_COL_DIM, LC_COL_BG, LVDS_FONT_NORMAL);
        return;
    }
    (void)lc_tool_field_float(line, "R", &r);
    (void)lc_tool_field_float(line, "DOC", &doc);
    (void)lc_tool_field_float(line, "ORIENT", &orient_f);
    orient = (int)(orient_f + (orient_f >= 0.0f ? 0.5f : -0.5f));
    if (!lc_tool_orient_code_valid(orient)) orient = 0;
    if (doc <= 0.0f) doc = 2.0f;
    if (r < 0.0f) r = 0.0f;
    size = lc_clampi((int)(doc * 28.0f + 0.5f), 34, 112);
    if (r > 0.0f) rr = lc_clampi((int)((r / doc) * (float)size + 0.5f), 1, size / 2);
    tip_x = block_x + (block_w / 2);
    tip_y = block_y + (block_h / 2);
    tip_corner = lc_tool_shape_tip_digit(orient);
    switch (tip_corner) {
        case 7:
            sx = tip_x;
            sy = tip_y;
            break;
        case 9:
            sx = tip_x - size + 1;
            sy = tip_y;
            break;
        case 3:
            sx = tip_x - size + 1;
            sy = tip_y - size + 1;
            break;
        case 1:
        default:
            sx = tip_x;
            sy = tip_y - size + 1;
            break;
    }
    lc_tool_active_edges(orient > 9 ? tip_corner : orient, &left, &top, &right, &bottom);

    lvds_draw_fill_rect(LC_TOOL_EDITOR_LEFT_X, 86, LC_TOOL_EDITOR_LEFT_W, 458, LC_COL_BG);
    lvds_draw_fill_rect(block_x, block_y, block_w, block_h, LC_PREVIEW_STOCK_FG);
    lvds_draw_line(block_x + 6, tip_y, block_x + block_w - 7, tip_y, LC_PREVIEW_AXIS_FG);
    lvds_draw_line(tip_x, block_y + 6, tip_x, block_y + block_h - 7, LC_PREVIEW_AXIS_FG);
    lc_text_clip(tip_x + 4, block_y + 8, "X0", 4, LC_PREVIEW_AXIS_FG, LC_PREVIEW_STOCK_FG, LVDS_FONT_SMALL);
    lc_text_clip(block_x + block_w - 28, tip_y + 4, "Z0", 4, LC_PREVIEW_AXIS_FG, LC_PREVIEW_STOCK_FG, LVDS_FONT_SMALL);
    if (orient == 0) {
        lc_draw_tool_doc_dot(tip_x, tip_y, size);
    } else if (orient > 9) {
        lc_draw_tool_polygon_marker(tip_x, tip_y, orient, size, rr, 2);
    } else if (orient == 5) {
        lc_draw_tool_drill_marker(tip_x, tip_y, size, 2);
    } else {
        lc_draw_tool_marker_shape(sx, sy, size, rr, left, top, right, bottom, 2, LC_PREVIEW_STOCK_FG);
    }
    lvds_draw_fill_ellipse(tip_x, tip_y, 2, 2, lvds_palette_color(red_bright));

    snprintf(buf, sizeof(buf), "ORIENT %d", orient);
    lc_text_clip(LC_TOOL_EDITOR_LEFT_X + 16, 326, buf, 32, LC_COL_TEXT, LC_COL_BG, LVDS_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "DOC %5.2f     R %5.2f", (double)doc, (double)r);
    lc_text_clip(LC_TOOL_EDITOR_LEFT_X + 16, 350, buf, 32, LC_COL_TEXT, LC_COL_BG, LVDS_FONT_NORMAL);
    if (lc_tool_field_float(line, "T", &v)) {
        snprintf(buf, sizeof(buf), "T %.0f", (double)v);
        lc_text_clip(LC_TOOL_EDITOR_LEFT_X + 16, 374, buf, 32, LC_COL_DIM, LC_COL_BG, LVDS_FONT_NORMAL);
    }

    if (frame->leancam_active_field[0]) {
        snprintf(buf, sizeof(buf), "EDIT %s", frame->leancam_active_field);
        lc_text_clip(LC_TOOL_EDITOR_LEFT_X + 16, 502, buf, 32, LC_COL_VALUE, LC_COL_BG, LVDS_FONT_NORMAL);
    }
}

static void lc_draw_catalog_asset_preview(const ui_snapshot_frame_t *frame)
{
    if (lc_frame_is_tool_asset(frame)) {
        lc_draw_tool_asset_preview(frame);
    }
}

static int lc_live_tool_radius_px(const ui_snapshot_frame_t *frame, const lc_sim_view_t *view, int size)
{
    float r = 0.0f;
    int px;

    if (!frame || !view || !lc_field_float(frame->leancam_tool_line, "R", &r) || r <= 0.0f) {
        return 0;
    }
    px = (int)(r * view->scale + 0.5f);
    return lc_clampi(px, 1, size / 2);
}

static void lc_live_tool_active_edges(int orient, bool *left, bool *top, bool *right, bool *bottom)
{
    if (left) *left = (orient == 1 || orient == 4 || orient == 7 || orient == 2 || orient == 5 || orient == 8);
    if (top) *top = (orient == 7 || orient == 8 || orient == 9 || orient == 4 || orient == 5 || orient == 6);
    if (right) *right = (orient == 3 || orient == 6 || orient == 9 || orient == 2 || orient == 5 || orient == 8);
    if (bottom) *bottom = (orient == 1 || orient == 2 || orient == 3 || orient == 4 || orient == 5 || orient == 6);
}

static void lc_live_draw_tool_square(const ui_snapshot_frame_t *frame,
                                     const lc_sim_view_t *view,
                                     int x,
                                     int y,
                                     int size,
                                     int orient)
{
    bool left;
    bool top;
    bool right;
    bool bottom;
    int rr = lc_live_tool_radius_px(frame, view, size);

    lc_live_tool_active_edges(orient, &left, &top, &right, &bottom);
    lc_draw_tool_marker_shape(x, y, size, rr, left, top, right, bottom, 1, LC_LIVE_STOCK_FG);
}

static void lc_live_clear_tool_rect(void)
{
    if (!g_live_tool_rect_valid) {
        return;
    }

    lvds_draw_fill_rect(g_live_tool_rect_x,
                        g_live_tool_rect_y,
                        g_live_tool_rect_w,
                        g_live_tool_rect_h,
                        LC_LIVE_BG);
    g_live_tool_rect_valid = false;
    g_live_tool_rect_thread = false;
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
    int orient;
    bool left;
    bool top;
    bool right;
    bool bottom;

    if (!frame || !view || !frame->axes_valid || !frame->leancam_tool_line[0]) {
        return;
    }

    x = frame->axis[0];
    z = frame->axis[2];
    d = lc_live_runtime_x_to_diam(x);
    size = lc_live_doc_px(frame, view, lc_live_sim_is_face_cycle(frame) ? 1.0f : 0.5f);
    zx = lc_sim_zx_view(view, z);
    dy = lc_sim_dy_view(view, d);
    orient = lc_tool_orient_from_frame(frame);
    if (orient == 0) {
        int r = lc_clampi(size / 2, 3, 10);
        int rx = lc_clampi(zx - r - 2, view->x0 + 2, view->x1 - 4);
        int ry = lc_clampi(dy - r - 2, view->y0 + 2, view->y1 - 4);
        int rw = (r * 2) + 5;
        int rh = (r * 2) + 5;
        if (rx + rw > view->x1 - 2) rw = view->x1 - 2 - rx;
        if (ry + rh > view->y1 - 2) rh = view->y1 - 2 - ry;

        lc_draw_tool_doc_dot(zx, dy, size);
        g_live_tool_rect_thread = false;
        g_live_tool_rect_x = rx;
        g_live_tool_rect_y = ry;
        g_live_tool_rect_w = rw;
        g_live_tool_rect_h = rh;
        g_live_tool_rect_valid = true;
        return;
    }
    if (orient == 5) {
        int rx = lc_clampi(zx - size - 2, view->x0 + 2, view->x1 - 4);
        int ry = lc_clampi(dy - size - 2, view->y0 + 2, view->y1 - 4);
        int rw = (size * 2) + 5;
        int rh = (size * 2) + 5;
        if (rx + rw > view->x1 - 2) rw = view->x1 - 2 - rx;
        if (ry + rh > view->y1 - 2) rh = view->y1 - 2 - ry;

        lc_draw_tool_drill_marker(zx, dy, size, 2);
        g_live_tool_rect_thread = false;
        g_live_tool_rect_x = rx;
        g_live_tool_rect_y = ry;
        g_live_tool_rect_w = rw;
        g_live_tool_rect_h = rh;
        g_live_tool_rect_valid = true;
        return;
    }
    if (orient > 9) {
        int rr = lc_live_tool_radius_px(frame, view, size);
        int rx = lc_clampi(zx - size - 3, view->x0 + 2, view->x1 - 4);
        int ry = lc_clampi(dy - size - 3, view->y0 + 2, view->y1 - 4);
        int rw = (size * 2) + 7;
        int rh = (size * 2) + 7;
        if (rx + rw > view->x1 - 2) rw = view->x1 - 2 - rx;
        if (ry + rh > view->y1 - 2) rh = view->y1 - 2 - ry;

        lc_draw_tool_polygon_marker(zx, dy, orient, size, rr, 2);
        g_live_tool_rect_thread = false;
        g_live_tool_rect_x = rx;
        g_live_tool_rect_y = ry;
        g_live_tool_rect_w = rw;
        g_live_tool_rect_h = rh;
        g_live_tool_rect_valid = true;
        return;
    }
    {
        lc_live_tool_active_edges(orient, &left, &top, &right, &bottom);
        x0 = lc_live_tool_edge_origin(zx, size, left, right);
        y0 = lc_live_tool_edge_origin(dy, size, top, bottom);
    }
    x0 = lc_clampi(x0, view->x0 + 2, view->x1 - size - 2);
    y0 = lc_clampi(y0, view->y0 + 2, view->y1 - size - 2);

    lc_live_draw_tool_square(frame, view, x0, y0, size, orient);
    g_live_tool_rect_thread = false;
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
           lc_gcode_has_word(line, 'G', 3) ||
           lc_gcode_has_word(line, 'G', 33);
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

    steps = (int)(lc_absf(sweep) * radius * view->scale / 10.0f) + 4;
    steps = lc_clampi(steps, 6, LC_PREVIEW_ARC_MAX_STEPS);
    prev_x = lc_sim_zx_view(view, z0);
    prev_y = lc_sim_dy_view(view, d0);

    for (s = 1; s <= steps; ++s) {
        float t = (float)s / (float)steps;
        float a = a0 + (sweep * t);
        float z = center_z + cosf(a) * radius;
        float d = (center_r + sinf(a) * radius) * 2.0f;
        int x = lc_sim_zx_view(view, z);
        int y = lc_sim_dy_view(view, d);

        lvds_draw_line_w(prev_x, prev_y, x, y, LC_PREVIEW_PROFILE_FG, selected ? 3 : 1);
        prev_x = x;
        prev_y = y;
    }
}

static bool lc_raw_preview_is_region_header(const char *line)
{
    line = lc_skip_line_number(line);
    return lc_is_cycle(line, "G71") || lc_is_cycle(line, "G72");
}

static bool lc_raw_preview_is_contour(const char *line)
{
    line = lc_skip_line_number(line);
    return lc_is_cycle(line, "G1") || lc_is_cycle(line, "G2") || lc_is_cycle(line, "G3");
}

static bool lc_raw_preview_auto_state(const char *line, char *out, size_t out_sz)
{
    if (!lc_get_field_text(line, "AUTO", out, out_sz))
        return false;
    if (strcmp(out, "START") == 0 || strcmp(out, "CLOSE") == 0)
        return true;
    if (out && out_sz > 0)
        out[0] = 0;
    return false;
}

static void lc_raw_preview_draw_line_segment(const lc_sim_view_t *view,
                                             float z0,
                                             float d0,
                                             float z1,
                                             float d1,
                                             lvds_color_t color,
                                             int width);
static void lc_raw_preview_draw_dashed_segment(const lc_sim_view_t *view,
                                               float z0,
                                               float d0,
                                               float z1,
                                               float d1,
                                               lvds_color_t color);
static bool lc_raw_preview_draw_r_arc_display(const lc_sim_view_t *view,
                                              float z0,
                                              float d0,
                                              float z1,
                                              float d1,
                                              float r,
                                              bool cw,
                                              lvds_color_t color,
                                              int width);

typedef struct {
    float x;
    float z;
} lc_preview_v2_t;

typedef struct {
    lc_preview_v2_t t1;
    lc_preview_v2_t t2;
    lc_preview_v2_t c;
    float r;
    bool cw;
} lc_preview_corner_arc_t;

typedef enum
{
    LC_PREVIEW_SEG_RAPID_G0 = 0,
    LC_PREVIEW_SEG_ROUGH_FEED,
    LC_PREVIEW_SEG_FINISH_FEED,
    LC_PREVIEW_SEG_FEED
} lc_preview_segment_class_t;

static uint32_t lc_preview_hash_bytes(uint32_t h, const char *s)
{
    if (!s)
        s = "";
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    h ^= 0xffu;
    h *= 16777619u;
    return h;
}

static uint32_t lc_fullscreen_preview_hash(const ui_snapshot_frame_t *frame)
{
    uint32_t h = 2166136261u;

    if (!frame)
        return 0;

    h = lc_preview_hash_bytes(h, frame->leancam_preview_line);
    h = lc_preview_hash_bytes(h, frame->leancam_setup_line);
    h = lc_preview_hash_bytes(h, frame->leancam_tool_line);
    h ^= frame->leancam_preview_region_count;
    h *= 16777619u;
    return h ? h : 1u;
}

static uint32_t lc_raw_preview_region_hash(const ui_snapshot_frame_t *frame)
{
    uint32_t h = 2166136261u;
    uint8_t i;

    if (!frame)
        return 0;

    h = lc_preview_hash_bytes(h, frame->leancam_setup_line);
    h = lc_preview_hash_bytes(h, frame->leancam_tool_line);
    for (i = 0; i < frame->leancam_preview_region_count && i < UI_LC_PREVIEW_REGION_MAX; ++i)
        h = lc_preview_hash_bytes(h, frame->leancam_preview_region[i]);
    h ^= frame->leancam_preview_region_count;
    h *= 16777619u;
    return h ? h : 1u;
}

static const char *lc_raw_preview_region_name(const ui_snapshot_frame_t *frame)
{
    uint8_t i;

    if (!frame)
        return "";
    for (i = 0; i < frame->leancam_preview_region_count && i < UI_LC_PREVIEW_REGION_MAX; ++i) {
        const char *line = lc_skip_line_number(frame->leancam_preview_region[i]);
        if (lc_is_cycle(line, "G71")) return "G71";
        if (lc_is_cycle(line, "G72")) return "G72";
    }
    return "";
}

static void lc_raw_preview_label_text(char *out,
                                      size_t out_sz,
                                      const char *region_name,
                                      const char *auto_state,
                                      int contour_no,
                                      bool selected,
                                      bool crowded)
{
    if (!out || out_sz == 0)
        return;
    out[0] = 0;

    if (auto_state && auto_state[0] && !crowded) {
        if (selected && region_name && region_name[0])
            snprintf(out, out_sz, "%s %s", region_name, auto_state);
        else
            snprintf(out, out_sz, "%s", auto_state);
        return;
    }

    if (selected && auto_state && auto_state[0] && region_name && region_name[0] && !crowded) {
        snprintf(out, out_sz, "%s %s", region_name, auto_state);
        return;
    }

    snprintf(out, out_sz, "C%d", contour_no);
}

static void lc_raw_preview_draw_text_label(const lc_sim_view_t *view,
                                           int x,
                                           int y,
                                           const char *text,
                                           bool selected)
{
    int w;
    int tx;
    int ty;

    if (!view || !text || !text[0])
        return;
    w = lvds_draw_text_width(text, LVDS_FONT_SMALL);
    tx = lc_clampi(x + 5, view->x0 + 2, view->x1 - w - 2);
    ty = lc_clampi(y - 8, view->y0 + 4, view->y1 - 14);
    if (selected) {
        lvds_draw_fill_rect(tx - 2, ty - 1, w + 4, 13, LC_PREVIEW_ACTIVE_VALUE_BG);
    }
    lvds_draw_text(tx, ty, text,
                   selected ? LC_PREVIEW_ACTIVE_VALUE_FG : LC_PREVIEW_LABEL_FG,
                   selected ? LC_PREVIEW_ACTIVE_VALUE_BG : LC_PREVIEW_PANEL_BG,
                   LVDS_FONT_SMALL);
}

static void lc_raw_preview_draw_line_segment(const lc_sim_view_t *view,
                                             float z0,
                                             float d0,
                                             float z1,
                                             float d1,
                                             lvds_color_t color,
                                             int width)
{
    if (!view ||
        !isfinite(z0) || !isfinite(d0) ||
        !isfinite(z1) || !isfinite(d1))
        return;
    lvds_draw_line_w(lc_sim_zx_view(view, z0),
                     lc_sim_dy_view(view, d0),
                     lc_sim_zx_view(view, z1),
                     lc_sim_dy_view(view, d1),
                     color,
                     width);
}

static void lc_raw_preview_draw_dashed_segment(const lc_sim_view_t *view,
                                               float z0,
                                               float d0,
                                               float z1,
                                               float d1,
                                               lvds_color_t color)
{
    float dz = z1 - z0;
    float dd = d1 - d0;
    float px0 = (float)lc_sim_zx_view(view, z0);
    float py0 = (float)lc_sim_dy_view(view, d0);
    float px1 = (float)lc_sim_zx_view(view, z1);
    float py1 = (float)lc_sim_dy_view(view, d1);
    float plen = sqrtf((px1 - px0) * (px1 - px0) + (py1 - py0) * (py1 - py0));
    int pieces = lc_clampi((int)(plen / 8.0f), 1, 80);
    int p;

    if (!view)
        return;
    for (p = 0; p < pieces; p += 2) {
        float a = (float)p / (float)pieces;
        float b = (float)(p + 1) / (float)pieces;
        if (b > 1.0f) b = 1.0f;
        lc_raw_preview_draw_line_segment(view,
                                         z0 + dz * a,
                                         d0 + dd * a,
                                         z0 + dz * b,
                                         d0 + dd * b,
                                         color,
                                         1);
    }
}

static lc_preview_v2_t lc_preview_v2_add(lc_preview_v2_t a, lc_preview_v2_t b)
{
    lc_preview_v2_t r = { a.x + b.x, a.z + b.z };
    return r;
}

static lc_preview_v2_t lc_preview_v2_sub(lc_preview_v2_t a, lc_preview_v2_t b)
{
    lc_preview_v2_t r = { a.x - b.x, a.z - b.z };
    return r;
}

static lc_preview_v2_t lc_preview_v2_mul(lc_preview_v2_t a, float s)
{
    lc_preview_v2_t r = { a.x * s, a.z * s };
    return r;
}

static float lc_preview_v2_dot(lc_preview_v2_t a, lc_preview_v2_t b)
{
    return (a.x * b.x) + (a.z * b.z);
}

static float lc_preview_v2_cross(lc_preview_v2_t a, lc_preview_v2_t b)
{
    return (a.x * b.z) - (a.z * b.x);
}

static float lc_preview_v2_len(lc_preview_v2_t a)
{
    return sqrtf(lc_preview_v2_dot(a, a));
}

static bool lc_preview_v2_norm(lc_preview_v2_t a, lc_preview_v2_t *out)
{
    float l = lc_preview_v2_len(a);

    if (!out || l < 0.0001f)
        return false;
    out->x = a.x / l;
    out->z = a.z / l;
    return true;
}

static bool lc_preview_build_r_corner(lc_preview_v2_t p0,
                                      lc_preview_v2_t p1,
                                      lc_preview_v2_t p2,
                                      float r,
                                      lc_preview_corner_arc_t *out)
{
    lc_preview_v2_t p0r = { p0.x * 0.5f, p0.z };
    lc_preview_v2_t p1r = { p1.x * 0.5f, p1.z };
    lc_preview_v2_t p2r = { p2.x * 0.5f, p2.z };
    lc_preview_v2_t a;
    lc_preview_v2_t b;
    lc_preview_v2_t bis;
    float len_a = lc_preview_v2_len(lc_preview_v2_sub(p0r, p1r));
    float len_b = lc_preview_v2_len(lc_preview_v2_sub(p2r, p1r));
    float dot;
    float theta;
    float half;
    float tan_half;
    float sin_half;
    float t;

    if (!out || r <= 0.0f || len_a < 0.0001f || len_b < 0.0001f)
        return false;
    if (!isfinite(r) ||
        !isfinite(p0.x) || !isfinite(p0.z) ||
        !isfinite(p1.x) || !isfinite(p1.z) ||
        !isfinite(p2.x) || !isfinite(p2.z))
        return false;
    if (!lc_preview_v2_norm(lc_preview_v2_sub(p0r, p1r), &a) ||
        !lc_preview_v2_norm(lc_preview_v2_sub(p2r, p1r), &b))
        return false;
    dot = fmaxf(-1.0f, fminf(1.0f, lc_preview_v2_dot(a, b)));
    if (lc_absf(dot) > 0.999f)
        return false;
    theta = acosf(dot);
    half = theta * 0.5f;
    tan_half = tanf(half);
    sin_half = sinf(half);
    if (lc_absf(tan_half) < 0.0001f || lc_absf(sin_half) < 0.0001f)
        return false;
    t = r / tan_half;
    if (!isfinite(t))
        return false;
    if (t > len_a + 0.0001f || t > len_b + 0.0001f)
        return false;
    if (!lc_preview_v2_norm(lc_preview_v2_add(a, b), &bis))
        return false;

    out->t1 = lc_preview_v2_add(p1r, lc_preview_v2_mul(a, t));
    out->t2 = lc_preview_v2_add(p1r, lc_preview_v2_mul(b, t));
    out->c = lc_preview_v2_add(p1r, lc_preview_v2_mul(bis, r / sin_half));
    if (!isfinite(out->t1.x) || !isfinite(out->t1.z) ||
        !isfinite(out->t2.x) || !isfinite(out->t2.z) ||
        !isfinite(out->c.x) || !isfinite(out->c.z))
        return false;
    out->r = r;
    if (lc_absf(lc_preview_v2_len(lc_preview_v2_sub(out->t1, out->c)) - r) > 0.01f ||
        lc_absf(lc_preview_v2_len(lc_preview_v2_sub(out->t2, out->c)) - r) > 0.01f ||
        lc_absf(lc_preview_v2_dot(lc_preview_v2_sub(out->t1, out->c), lc_preview_v2_sub(p1r, p0r))) > 0.01f ||
        lc_absf(lc_preview_v2_dot(lc_preview_v2_sub(out->t2, out->c), lc_preview_v2_sub(p2r, p1r))) > 0.01f)
        return false;
    out->cw = lc_preview_v2_cross(lc_preview_v2_sub(out->t1, out->c),
                                  lc_preview_v2_sub(out->t2, out->c)) < 0.0f;
    out->t1.x *= 2.0f;
    out->t2.x *= 2.0f;
    out->c.x *= 2.0f;
    return true;
}

static float lc_preview_arc_sweep(float a0, float a1, bool cw)
{
    float two_pi = 6.2831853f;
    float sweep = a1 - a0;

    if (cw) {
        while (sweep >= 0.0f)
            sweep -= two_pi;
    } else {
        while (sweep <= 0.0f)
            sweep += two_pi;
    }
    return sweep;
}

static float lc_preview_short_arc_sweep(float a0, float a1)
{
    float two_pi = 6.2831853f;
    float sweep = a1 - a0;

    while (sweep > 3.14159265f)
        sweep -= two_pi;
    while (sweep < -3.14159265f)
        sweep += two_pi;
    return sweep;
}

static void lc_raw_preview_draw_corner_arc(const lc_sim_view_t *view,
                                           const lc_preview_corner_arc_t *arc,
                                           bool selected)
{
    float a0;
    float sweep;
    int steps;
    int last_x;
    int last_y;
    int i;

    if (!view || !arc || arc->r <= 0.0f)
        return;
    a0 = atan2f((arc->t1.x * 0.5f) - (arc->c.x * 0.5f), arc->t1.z - arc->c.z);
    /*
     * G1 R is a tangent corner fillet. The construction already gives the two
     * tangent points, so preview the minor arc between them. Using the stored
     * CW flag here can select the exterior 270 degree sweep for a 90 degree
     * lathe corner because display X/Z coordinates use a different handedness
     * than explicit G2/G3 arc commands.
     */
    sweep = lc_preview_short_arc_sweep(a0,
                                       atan2f((arc->t2.x * 0.5f) - (arc->c.x * 0.5f),
                                              arc->t2.z - arc->c.z));
    steps = lc_clampi((int)(lc_absf(sweep) * arc->r * view->scale / 10.0f) + 4, 4, LC_PREVIEW_ARC_MAX_STEPS);
    last_x = lc_sim_zx_view(view, arc->t1.z);
    last_y = lc_sim_dy_view(view, arc->t1.x);
    for (i = 1; i <= steps; ++i) {
        float a = a0 + sweep * ((float)i / (float)steps);
        float z = arc->c.z + cosf(a) * arc->r;
        float d = ((arc->c.x * 0.5f) + sinf(a) * arc->r) * 2.0f;
        int x = lc_sim_zx_view(view, z);
        int y = lc_sim_dy_view(view, d);
        lvds_draw_line_w(last_x, last_y, x, y, LC_PREVIEW_PROFILE_FG, selected ? 3 : 2);
        last_x = x;
        last_y = y;
    }
}

static bool lc_preview_build_chamfer_corner(lc_preview_v2_t p0,
                                            lc_preview_v2_t p1,
                                            lc_preview_v2_t p2,
                                            float amount,
                                            lc_preview_v2_t *t1_out,
                                            lc_preview_v2_t *t2_out)
{
    const float eps = 0.0001f;
    lc_preview_v2_t p0r = { p0.x * 0.5f, p0.z };
    lc_preview_v2_t p1r = { p1.x * 0.5f, p1.z };
    lc_preview_v2_t p2r = { p2.x * 0.5f, p2.z };
    lc_preview_v2_t a;
    lc_preview_v2_t b;
    float len_a = lc_preview_v2_len(lc_preview_v2_sub(p1r, p0r));
    float len_b = lc_preview_v2_len(lc_preview_v2_sub(p2r, p1r));
    float dot;
    float turn;
    float trim;

    if (!t1_out || !t2_out || amount <= 0.0f || len_a <= eps || len_b <= eps)
        return false;
    if (!isfinite(amount) ||
        !isfinite(p0.x) || !isfinite(p0.z) ||
        !isfinite(p1.x) || !isfinite(p1.z) ||
        !isfinite(p2.x) || !isfinite(p2.z))
        return false;
    if (!lc_preview_v2_norm(lc_preview_v2_sub(p1r, p0r), &a) ||
        !lc_preview_v2_norm(lc_preview_v2_sub(p2r, p1r), &b))
        return false;
    dot = ((-a.x) * b.x) + ((-a.z) * b.z);
    dot = fmaxf(-1.0f, fminf(1.0f, dot));
    turn = acosf(dot);
    if (turn <= 0.0001f || lc_absf(3.14159265f - turn) <= 0.0001f)
        return false;
    trim = amount / tanf(turn * 0.5f);
    if (!isfinite(trim))
        return false;
    if (trim >= len_a || trim >= len_b)
        return false;

    *t1_out = lc_preview_v2_sub(p1r, lc_preview_v2_mul(a, trim));
    *t2_out = lc_preview_v2_add(p1r, lc_preview_v2_mul(b, trim));
    t1_out->x *= 2.0f;
    t2_out->x *= 2.0f;
    return true;
}

static bool lc_raw_preview_r_arc_center_display(float z0,
                                                float d0,
                                                float z1,
                                                float d1,
                                                float r,
                                                bool cw,
                                                lc_preview_v2_t *center_out)
{
    float r0 = d0 * 0.5f;
    float r1 = d1 * 0.5f;
    float dz = z1 - z0;
    float dr = r1 - r0;
    float chord = sqrtf((dz * dz) + (dr * dr));
    float mid_z = (z0 + z1) * 0.5f;
    float mid_r = (r0 + r1) * 0.5f;
    float h;
    float nz;
    float nr;
    lc_preview_v2_t c0;
    lc_preview_v2_t c1;
    float a0;
    float s0;
    float s1;

    if (!center_out || r <= 0.0f || chord <= 0.0001f || chord > (2.0f * r + 0.0001f))
        return false;

    h = sqrtf(fmaxf(0.0f, (r * r) - ((chord * 0.5f) * (chord * 0.5f))));
    nz = -dr / chord;
    nr = dz / chord;
    c0.z = mid_z + (nz * h);
    c0.x = mid_r + (nr * h);
    c1.z = mid_z - (nz * h);
    c1.x = mid_r - (nr * h);

    a0 = atan2f(r0 - c0.x, z0 - c0.z);
    s0 = lc_absf(lc_preview_arc_sweep(a0, atan2f(r1 - c0.x, z1 - c0.z), cw));
    a0 = atan2f(r0 - c1.x, z0 - c1.z);
    s1 = lc_absf(lc_preview_arc_sweep(a0, atan2f(r1 - c1.x, z1 - c1.z), cw));
    if (s0 <= 3.14159265f + 0.0001f || s1 <= 3.14159265f + 0.0001f)
        *center_out = s0 <= s1 ? c0 : c1;
    else
        *center_out = c0;
    return true;
}

static bool lc_raw_preview_draw_r_arc_display(const lc_sim_view_t *view,
                                              float z0,
                                              float d0,
                                              float z1,
                                              float d1,
                                              float r,
                                              bool cw,
                                              lvds_color_t color,
                                              int width)
{
    lc_preview_v2_t c;
    float a0;
    float sweep;
    int steps;
    int last_x;
    int last_y;
    int i;

    if (!view || !lc_raw_preview_r_arc_center_display(z0, d0, z1, d1, r, cw, &c))
        return false;

    a0 = atan2f((d0 * 0.5f) - c.x, z0 - c.z);
    sweep = lc_preview_arc_sweep(a0, atan2f((d1 * 0.5f) - c.x, z1 - c.z), cw);
    if (lc_absf(sweep) > 3.14159265f)
        sweep += cw ? 6.2831853f : -6.2831853f;
    steps = lc_clampi((int)(lc_absf(sweep) * r * view->scale / 10.0f) + 4, 4, LC_PREVIEW_ARC_MAX_STEPS);
    last_x = lc_sim_zx_view(view, z0);
    last_y = lc_sim_dy_view(view, d0);
    for (i = 1; i <= steps; ++i) {
        float a = a0 + sweep * ((float)i / (float)steps);
        float z = c.z + cosf(a) * r;
        float d = (c.x + sinf(a) * r) * 2.0f;
        int x = lc_sim_zx_view(view, z);
        int y = lc_sim_dy_view(view, d);
        lvds_draw_line_w(last_x, last_y, x, y, color, width);
        last_x = x;
        last_y = y;
    }
    return true;
}

static bool lc_raw_preview_draw_trimmed_corner(const lc_sim_view_t *view,
                                               float z0,
                                               float d0,
                                               float z1,
                                               float d1,
                                               float z2,
                                               float d2,
                                               float amount,
                                               bool radius,
                                               bool selected,
                                               bool helper,
                                               float *out_z,
                                               float *out_d)
{
    lvds_color_t color = helper ? LC_PREVIEW_LABEL_FG : LC_PREVIEW_PROFILE_FG;
    int width = selected ? 3 : (helper ? 1 : 2);
    lc_preview_v2_t p0 = { d0, z0 };
    lc_preview_v2_t p1 = { d1, z1 };
    lc_preview_v2_t p2 = { d2, z2 };
    lc_preview_v2_t t1;
    lc_preview_v2_t t2;

    if (!view || amount <= 0.0f)
        return false;

    if (radius) {
        lc_preview_corner_arc_t arc;

        if (!lc_preview_build_r_corner(p0, p1, p2, amount, &arc))
            return false;
        t1 = arc.t1;
        t2 = arc.t2;
        lc_raw_preview_draw_line_segment(view, z0, d0, t1.z, t1.x, color, width);
        lc_raw_preview_draw_corner_arc(view, &arc, selected);
    } else {
        if (!lc_preview_build_chamfer_corner(p0, p1, p2, amount, &t1, &t2))
            return false;
        lc_raw_preview_draw_line_segment(view, z0, d0, t1.z, t1.x, color, width);
        lc_raw_preview_draw_line_segment(view, t1.z, t1.x, t2.z, t2.x, color, width);
    }
    if (out_z) *out_z = t2.z;
    if (out_d) *out_d = t2.x;
    return true;
}

static bool lc_preview_polyline_x_at_z(const float *contour_z,
                                       const float *contour_d,
                                       uint8_t count,
                                       float z,
                                       int x_dir,
                                       float *d_out)
{
    bool found = false;
    float best = 0.0f;

    if (!contour_z || !contour_d || !d_out || count < 2 || !isfinite(z))
        return false;
    for (uint8_t i = 1; i < count; ++i) {
        float z0 = contour_z[i - 1];
        float z1 = contour_z[i];
        float d0 = contour_d[i - 1];
        float d1 = contour_d[i];
        float dz = z1 - z0;
        float d;
        float t;

        if ((z < z0 && z < z1) || (z > z0 && z > z1))
            continue;
        if (fabsf(dz) <= 0.0001f) {
            if (fabsf(z - z0) > 0.0001f)
                continue;
            d = x_dir < 0 ? fminf(d0, d1) : fmaxf(d0, d1);
        } else {
            t = (z - z0) / dz;
            if (t < -0.0001f || t > 1.0001f)
                continue;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            d = d0 + ((d1 - d0) * t);
        }
        if (!found ||
            (x_dir < 0 && d < best) ||
            (x_dir > 0 && d > best)) {
            best = d;
            found = true;
        }
    }
    if (!found)
        return false;
    *d_out = best;
    return true;
}

static bool lc_preview_polyline_z_at_d(const float *contour_z,
                                       const float *contour_d,
                                       uint8_t count,
                                       float d,
                                       int z_dir,
                                       float *z_out)
{
    bool found = false;
    float best = 0.0f;

    if (!contour_z || !contour_d || !z_out || count < 2 || !isfinite(d))
        return false;
    for (uint8_t i = 1; i < count; ++i) {
        float z0 = contour_z[i - 1];
        float z1 = contour_z[i];
        float d0 = contour_d[i - 1];
        float d1 = contour_d[i];
        float dd = d1 - d0;
        float z;
        float t;

        if ((d < d0 && d < d1) || (d > d0 && d > d1))
            continue;
        if (fabsf(dd) <= 0.0001f) {
            if (fabsf(d - d0) > 0.0001f)
                continue;
            z = z_dir < 0 ? fminf(z0, z1) : fmaxf(z0, z1);
        } else {
            t = (d - d0) / dd;
            if (t < -0.0001f || t > 1.0001f)
                continue;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            z = z0 + ((z1 - z0) * t);
        }
        if (!found ||
            (z_dir < 0 && z < best) ||
            (z_dir > 0 && z > best)) {
            best = z;
            found = true;
        }
    }
    if (!found)
        return false;
    *z_out = best;
    return true;
}

static void lc_preview_build_effective_polyline(const float *contour_z,
                                                const float *contour_d,
                                                const float *contour_c,
                                                const float *contour_r,
                                                uint8_t count,
                                                float *out_z,
                                                float *out_d,
                                                uint8_t out_max,
                                                uint8_t *out_count)
{
    uint8_t n = 0;

    if (!out_count)
        return;
    *out_count = 0;
    if (!contour_z || !contour_d || !out_z || !out_d || out_max == 0 || count == 0)
        return;

    out_z[n] = contour_z[0];
    out_d[n] = contour_d[0];
    n++;

    for (uint8_t i = 1; i < count && n < out_max; ++i) {
        if (i + 1 < count && contour_r && contour_r[i] > 0.0f) {
            lc_preview_corner_arc_t arc;
            lc_preview_v2_t p0 = { contour_d[i - 1], contour_z[i - 1] };
            lc_preview_v2_t p1 = { contour_d[i], contour_z[i] };
            lc_preview_v2_t p2 = { contour_d[i + 1], contour_z[i + 1] };

            if (lc_preview_build_r_corner(p0, p1, p2, contour_r[i], &arc)) {
                int steps = lc_clampi((int)(arc.r / 1.0f) + 4, 4, 20);
                out_z[n] = arc.t1.z;
                out_d[n] = arc.t1.x;
                n++;
                for (int s = 1; s < steps && n < out_max; ++s) {
                    float a0 = atan2f((arc.t1.x * 0.5f) - (arc.c.x * 0.5f), arc.t1.z - arc.c.z);
                    float sweep = lc_preview_short_arc_sweep(a0,
                                                             atan2f((arc.t2.x * 0.5f) - (arc.c.x * 0.5f),
                                                                    arc.t2.z - arc.c.z));
                    float a = a0 + sweep * ((float)s / (float)steps);
                    out_z[n] = arc.c.z + cosf(a) * arc.r;
                    out_d[n] = ((arc.c.x * 0.5f) + sinf(a) * arc.r) * 2.0f;
                    n++;
                }
                if (n < out_max) {
                    out_z[n] = arc.t2.z;
                    out_d[n] = arc.t2.x;
                    n++;
                }
                continue;
            }
        } else if (i + 1 < count && contour_c && contour_c[i] > 0.0f) {
            lc_preview_v2_t t1;
            lc_preview_v2_t t2;
            lc_preview_v2_t p0 = { contour_d[i - 1], contour_z[i - 1] };
            lc_preview_v2_t p1 = { contour_d[i], contour_z[i] };
            lc_preview_v2_t p2 = { contour_d[i + 1], contour_z[i + 1] };

            if (lc_preview_build_chamfer_corner(p0, p1, p2, contour_c[i], &t1, &t2)) {
                out_z[n] = t1.z;
                out_d[n] = t1.x;
                n++;
                if (n < out_max) {
                    out_z[n] = t2.z;
                    out_d[n] = t2.x;
                    n++;
                }
                continue;
            }
        }

        out_z[n] = contour_z[i];
        out_d[n] = contour_d[i];
        n++;
    }

    *out_count = n;
}

static void lc_preview_draw_graphic_rough_area(const lc_sim_view_t *view,
                                               bool is_g72,
                                               const float *hatch_z,
                                               const float *hatch_d,
                                               uint8_t hatch_count,
                                               float rough_doc,
                                               float x_allow,
                                               float z_allow,
                                               float min_z,
                                               float max_z,
                                               float min_d,
                                               float max_d,
                                               float start_z,
                                               float start_d)
{
    lvds_color_t yellow = LC_PREVIEW_HATCH_FG;

    if (!view || !hatch_z || !hatch_d || hatch_count < 2)
        return;
    if (rough_doc < 0.0f)
        rough_doc = -rough_doc;
    if (rough_doc <= 0.0001f)
        rough_doc = 2.0f;

    if (is_g72) {
        int x0 = lc_sim_zx_view(view, max_z);
        int x1 = lc_sim_zx_view(view, min_z + z_allow);
        int xa = x0 < x1 ? x0 : x1;
        int xb = x0 > x1 ? x0 : x1;
        int stock_y = lc_sim_dy_view(view, start_d > 0.0f ? start_d : max_d);
        int x_dir = (min_d < start_d) ? -1 : 1;

        for (int x = xa; x <= xb; ++x) {
            float z = lc_sim_view_z_from_x(view, x);
            float boundary_d;
            int by;
            int y0;
            int y1;

            if (!lc_preview_polyline_x_at_z(hatch_z, hatch_d, hatch_count, z, x_dir, &boundary_d))
                continue;
            boundary_d -= (float)x_dir * x_allow;
            by = lc_sim_dy_view(view, boundary_d);
            y0 = stock_y < by ? stock_y : by;
            y1 = stock_y > by ? stock_y : by;
            if (y1 > y0)
                lvds_draw_line(x, y0, x, y1, yellow);
        }
    } else {
        int y0 = lc_sim_dy_view(view, min_d + x_allow);
        int y1 = lc_sim_dy_view(view, max_d);
        int ya = y0 < y1 ? y0 : y1;
        int yb = y0 > y1 ? y0 : y1;
        int start_x = lc_sim_zx_view(view, start_z);
        int z_dir = (min_z < start_z) ? -1 : 1;

        for (int y = ya; y <= yb; ++y) {
            float d = lc_sim_view_d_from_y(view, y) - x_allow;
            float boundary_z;
            int bx;
            int x0;
            int x1;

            if (!lc_preview_polyline_z_at_d(hatch_z, hatch_d, hatch_count, d, z_dir, &boundary_z))
                continue;
            boundary_z -= (float)z_dir * z_allow;
            bx = lc_sim_zx_view(view, boundary_z);
            x0 = start_x < bx ? start_x : bx;
            x1 = start_x > bx ? start_x : bx;
            if (x1 > x0)
                lvds_draw_line(x0, y, x1, y, yellow);
        }
    }
}

static bool lc_sim_draw_raw_grouped_preview(const ui_snapshot_frame_t *frame,
                                            const lc_sim_view_t *view,
                                            bool allow_generated)
{
    const char *region_name;
    const char *header = NULL;
    float prev_z = 0.0f;
    float prev_d = view ? view->stock_od : 0.0f;
    bool have_prev = false;
    int contour_no = 0;
    int last_label_x = -10000;
    int last_label_y = -10000;
    bool any = false;
    bool is_g72 = false;
    float rough_doc = 0.0f;
    float retract = 1.0f;
    float x_allow = 0.0f;
    float z_allow = 0.0f;
    float min_z = 0.0f;
    float max_z = 0.0f;
    float min_d = 0.0f;
    float max_d = 0.0f;
    float start_z = 0.0f;
    bool have_bounds = false;
    float contour_z[UI_LC_PREVIEW_REGION_MAX];
    float contour_d[UI_LC_PREVIEW_REGION_MAX];
    float contour_c[UI_LC_PREVIEW_REGION_MAX];
    float contour_r[UI_LC_PREVIEW_REGION_MAX];
    float hatch_z[LC_PREVIEW_HATCH_MAX_POINTS];
    float hatch_d[LC_PREVIEW_HATCH_MAX_POINTS];
    uint8_t contour_count = 0;
    uint8_t finish_count = 0;
    uint8_t hatch_count = 0;
    uint8_t i;

    (void)allow_generated;
    if (!frame || !view || frame->leancam_preview_region_count == 0)
        return false;
    memset(contour_z, 0, sizeof(contour_z));
    memset(contour_d, 0, sizeof(contour_d));
    memset(contour_c, 0, sizeof(contour_c));
    memset(contour_r, 0, sizeof(contour_r));
    memset(hatch_z, 0, sizeof(hatch_z));
    memset(hatch_d, 0, sizeof(hatch_d));

    region_name = lc_raw_preview_region_name(frame);
    lc_text_clip(view->x0 + 4,
                 view->y0 + 18,
                 region_name[0] ? region_name : "G7x",
                 8,
                 LC_PREVIEW_ACTIVE_VALUE_FG,
                 LC_PREVIEW_PANEL_BG,
                 LVDS_FONT_SMALL);
    LC_PREVIEW_DBG("bounds scan begin");
    for (i = 0; i < frame->leancam_preview_region_count && i < UI_LC_PREVIEW_REGION_MAX; ++i) {
        const char *line = frame->leancam_preview_region[i];
        float z;
        float d;
        float c = 0.0f;
        float r = 0.0f;

        if (lc_raw_preview_is_region_header(line)) {
            header = line;
            is_g72 = lc_is_cycle(line, "G72");
            continue;
        }
        if (!lc_raw_preview_is_contour(line))
            continue;
        if (!lc_field_float(line, "X", &d) || !lc_field_float(line, "Z", &z))
            continue;
        (void)lc_field_float(line, "C", &c);
        (void)lc_field_float(line, "R", &r);
        if (contour_count < UI_LC_PREVIEW_REGION_MAX) {
            contour_z[contour_count] = z;
            contour_d[contour_count] = d;
            contour_c[contour_count] = c;
            contour_r[contour_count] = r;
            contour_count++;
        }
        if (!have_bounds) {
            min_z = max_z = start_z = z;
            min_d = max_d = d;
            have_bounds = true;
        } else {
            if (z < min_z) min_z = z;
            if (z > max_z) max_z = z;
            if (d < min_d) min_d = d;
            if (d > max_d) max_d = d;
        }
    }
    LC_PREVIEW_DBG("bounds scan end contours=%u finish=%u have=%u", (unsigned)contour_count, (unsigned)finish_count, (unsigned)have_bounds);
    finish_count = contour_count;
    if (finish_count >= 3 &&
        lc_absf(contour_z[finish_count - 1] - contour_z[finish_count - 2]) <= 0.0001f &&
        contour_d[finish_count - 1] >= contour_d[0] - 0.0001f) {
        finish_count--;
    }
    LC_PREVIEW_DBG("finish count=%u", (unsigned)finish_count);
    lc_preview_build_effective_polyline(contour_z,
                                        contour_d,
                                        contour_c,
                                        contour_r,
                                        contour_count,
                                        hatch_z,
                                        hatch_d,
                                        LC_PREVIEW_HATCH_MAX_POINTS,
                                        &hatch_count);

    if (header && have_bounds) {
        (void)lc_field_float(header, is_g72 ? "W" : "U", &rough_doc);
        (void)lc_field_float(header, "R", &retract);
        (void)lc_field_float(header, "X", &x_allow);
        (void)lc_field_float(header, "Z", &z_allow);
        if (rough_doc < 0.0f) rough_doc = -rough_doc;
        if (rough_doc <= 0.0f) rough_doc = 2.0f;
        if (retract <= 0.0f) retract = 1.0f;

        lc_preview_draw_graphic_rough_area(view,
                                           is_g72,
                                           hatch_z,
                                           hatch_d,
                                           hatch_count,
                                           rough_doc,
                                           x_allow,
                                           z_allow,
                                           min_z,
                                           max_z,
                                           min_d,
                                           max_d,
                                           contour_z[0],
                                           contour_d[0]);
    }

    LC_PREVIEW_DBG("raw overlay begin");
    for (i = 0; i < frame->leancam_preview_region_count && i < UI_LC_PREVIEW_REGION_MAX; ++i) {
        const char *line = frame->leancam_preview_region[i];
        float z;
        float d;
        float r = 0.0f;
        float c = 0.0f;
        char auto_state[16] = "";
        bool helper;
        bool selected = frame->leancam_preview_region_selected[i] != 0;
        bool is_arc;
        bool cw;
        int x;
        int y;
        char label[24];
        bool crowded;
        float draw_end_z;
        float draw_end_d;

        if (lc_raw_preview_is_region_header(line)) {
            have_prev = false;
            contour_no = 0;
            continue;
        }
        if (!lc_raw_preview_is_contour(line))
            continue;
        if (!lc_field_float(line, "X", &d) || !lc_field_float(line, "Z", &z))
            continue;

        LC_PREVIEW_DBG("raw row i=%u sel=%u line=%.48s", (unsigned)i, (unsigned)selected, line);
        any = true;
        contour_no++;
        helper = lc_raw_preview_auto_state(line, auto_state, sizeof(auto_state));
        is_arc = lc_is_cycle(line, "G2") || lc_is_cycle(line, "G3");
        cw = lc_is_cycle(line, "G2");
        x = lc_sim_zx_view(view, z);
        y = lc_sim_dy_view(view, d);
        draw_end_z = z;
        draw_end_d = d;

        if (!have_prev) {
            lvds_draw_rect(x - 3, y - 3, 7, 7, helper ? LC_PREVIEW_LABEL_FG : LC_PREVIEW_PROFILE_FG);
            if (selected) {
                lvds_draw_line_w(x - 7, y, x + 7, y, LC_PREVIEW_ACTIVE_VALUE_FG, 2);
                lvds_draw_line_w(x, y - 7, x, y + 7, LC_PREVIEW_ACTIVE_VALUE_FG, 2);
            }
        } else if (is_arc) {
            float i_off = 0.0f;
            float k_off = 0.0f;
            if (lc_field_float(line, "R", &r) &&
                lc_raw_preview_draw_r_arc_display(view,
                                                  prev_z,
                                                  prev_d,
                                                  z,
                                                  d,
                                                  r,
                                                  cw,
                                                  LC_PREVIEW_PROFILE_FG,
                                                  selected ? 3 : 2)) {
                /* drawn */
            } else if (lc_field_float(line, "I", &i_off) && lc_field_float(line, "K", &k_off)) {
                lc_sim_draw_nc_arc(view, prev_z, prev_d, z, d, i_off, k_off, cw, selected);
            } else {
                lc_raw_preview_draw_line_segment(view, prev_z, prev_d, z, d, LC_PREVIEW_ERROR_FG, selected ? 3 : 2);
            }
        } else {
            lvds_color_t color = helper ? LC_PREVIEW_LABEL_FG : LC_PREVIEW_ACTIVE_VALUE_FG;
            int width = selected ? 3 : (helper ? 1 : 2);
            bool used_corner = false;
            float corner_out_z = z;
            float corner_out_d = d;

            (void)lc_field_float(line, "C", &c);
            (void)lc_field_float(line, "R", &r);
            if ((c > 0.0f || r > 0.0f) &&
                (i + 1) < frame->leancam_preview_region_count) {
                float next_z;
                float next_d;
                const char *next = frame->leancam_preview_region[i + 1];
                if (lc_raw_preview_is_contour(next) &&
                    lc_field_float(next, "X", &next_d) &&
                    lc_field_float(next, "Z", &next_z)) {
                    used_corner = lc_raw_preview_draw_trimmed_corner(view,
                                                                      prev_z,
                                                                      prev_d,
                                                                      z,
                                                                      d,
                                                                      next_z,
                                                                      next_d,
                                                                      c > 0.0f ? c : r,
                                                                      r > 0.0f,
                                                                      selected,
                                                                      helper,
                                                                      &corner_out_z,
                                                                      &corner_out_d);
                }
                if (!used_corner) {
                    color = LC_PREVIEW_ERROR_FG;
                    width = selected ? 3 : 2;
                }
            }
            if (!used_corner) {
                if (helper && !selected)
                    lc_raw_preview_draw_dashed_segment(view, prev_z, prev_d, z, d, color);
                else
                    lc_raw_preview_draw_line_segment(view, prev_z, prev_d, z, d, color, width);
            } else {
                draw_end_z = corner_out_z;
                draw_end_d = corner_out_d;
            }
        }

        crowded = (lc_absf((float)(x - last_label_x)) < 46.0f &&
                   lc_absf((float)(y - last_label_y)) < 18.0f);
        if (helper || selected) {
            lc_raw_preview_label_text(label, sizeof(label), region_name, auto_state, contour_no, selected, crowded);
            lc_raw_preview_draw_text_label(view, x, y, label, selected);
            last_label_x = x;
            last_label_y = y;
        } else if (contour_no <= 3) {
            snprintf(label, sizeof(label), "C%d", contour_no);
            lc_raw_preview_draw_text_label(view, x, y, label, false);
            last_label_x = x;
            last_label_y = y;
        }
        if (!helper) {
            lvds_draw_rect(x - 2, y - 2, 5, 5, selected ? LC_PREVIEW_ACTIVE_VALUE_FG : LC_PREVIEW_PROFILE_FG);
        }

        prev_z = draw_end_z;
        prev_d = draw_end_d;
        have_prev = true;
    }

    LC_PREVIEW_DBG("raw overlay end any=%u", (unsigned)any);
    return any;
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
                (void)diameter_mode;
                lc_sim_draw_nc_arc(view, z, d, next_z, next_d, arc_i, arc_k, is_cw,
                                   frame->leancam_line_selected[i]);
            } else {
                int x1 = lc_sim_zx_view(view, z);
                int y1 = lc_sim_dy_view(view, d);
                int x2 = lc_sim_zx_view(view, next_z);
                int y2 = lc_sim_dy_view(view, next_d);
                lvds_draw_line_w(x1, y1, x2, y2, LC_PREVIEW_PROFILE_FG,
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

static bool lc_sim_draw_region_contour_only(const ui_snapshot_frame_t *frame,
                                            const lc_sim_view_t *view,
                                            bool show_selected)
{
    float prev_z = 0.0f;
    float prev_d = view ? view->stock_od : 0.0f;
    bool have_prev = false;
    bool any = false;
    int i;

    if (!frame || !view || frame->leancam_preview_region_count == 0)
        return false;

    for (i = 0; i < frame->leancam_preview_region_count && i < UI_LC_PREVIEW_REGION_MAX; ++i) {
        const char *line = frame->leancam_preview_region[i];
        float z;
        float d;
        float r = 0.0f;
        float c = 0.0f;
        bool selected = show_selected && frame->leancam_preview_region_selected[i] != 0;
        bool is_arc;
        bool cw;

        if (lc_raw_preview_is_region_header(line)) {
            have_prev = false;
            continue;
        }
        if (!lc_raw_preview_is_contour(line))
            continue;
        if (!lc_field_float(line, "X", &d) || !lc_field_float(line, "Z", &z))
            continue;

        is_arc = lc_is_cycle(line, "G2") || lc_is_cycle(line, "G3");
        cw = lc_is_cycle(line, "G2");

        if (!have_prev) {
            int marker = show_selected ? 7 : 5;
            int half = marker / 2;
            lvds_draw_rect(lc_sim_zx_view(view, z) - half,
                           lc_sim_dy_view(view, d) - half,
                           marker,
                           marker,
                           selected ? LC_PREVIEW_ACTIVE_VALUE_FG : LC_PREVIEW_PROFILE_FG);
        } else if (is_arc) {
            float i_off = 0.0f;
            float k_off = 0.0f;
            if (lc_field_float(line, "R", &r) &&
                lc_raw_preview_draw_r_arc_display(view,
                                                  prev_z,
                                                  prev_d,
                                                  z,
                                                  d,
                                                  r,
                                                  cw,
                                                  LC_PREVIEW_PROFILE_FG,
                                                  selected ? 3 : 2)) {
                /* drawn */
            } else if (lc_field_float(line, "I", &i_off) && lc_field_float(line, "K", &k_off)) {
                lc_sim_draw_nc_arc(view, prev_z, prev_d, z, d, i_off, k_off, cw, selected);
            } else {
                lc_raw_preview_draw_line_segment(view, prev_z, prev_d, z, d, LC_PREVIEW_PROFILE_FG, selected ? 3 : 2);
            }
        } else {
            bool used_corner = false;
            float corner_out_z = z;
            float corner_out_d = d;

            (void)lc_field_float(line, "C", &c);
            (void)lc_field_float(line, "R", &r);
            if ((c > 0.0f || r > 0.0f) &&
                (i + 1) < frame->leancam_preview_region_count) {
                float next_z;
                float next_d;
                const char *next = frame->leancam_preview_region[i + 1];
                if (lc_raw_preview_is_contour(next) &&
                    lc_field_float(next, "X", &next_d) &&
                    lc_field_float(next, "Z", &next_z)) {
                    used_corner = lc_raw_preview_draw_trimmed_corner(view,
                                                                      prev_z,
                                                                      prev_d,
                                                                      z,
                                                                      d,
                                                                      next_z,
                                                                      next_d,
                                                                      c > 0.0f ? c : r,
                                                                      r > 0.0f,
                                                                      selected,
                                                                      false,
                                                                      &corner_out_z,
                                                                      &corner_out_d);
                }
            }
            if (used_corner) {
                z = corner_out_z;
                d = corner_out_d;
            } else {
                lc_raw_preview_draw_line_segment(view,
                                                 prev_z,
                                                 prev_d,
                                                 z,
                                                 d,
                                                 LC_PREVIEW_ACTIVE_VALUE_FG,
                                                 selected ? 3 : 2);
            }
        }

        prev_z = z;
        prev_d = d;
        have_prev = true;
        any = true;
    }

    return any;
}

static void lc_sim_draw_preview_ex_step(const ui_snapshot_frame_t *frame,
                                        bool full_screen,
                                        uint8_t step,
                                        uint8_t step_count);

static void lc_sim_draw_preview_ex(const ui_snapshot_frame_t *frame, bool full_screen)
{
    lc_sim_draw_preview_ex_step(frame, full_screen, 0u, 1u);
}

static void lc_sim_draw_preview_ex_step(const ui_snapshot_frame_t *frame,
                                        bool full_screen,
                                        uint8_t step,
                                        uint8_t step_count)
{
    lc_sim_setup_t setup;
    lc_sim_view_t view;
    const char *line;

    if (!frame) {
        return;
    }

    lc_sim_read_setup(frame, &setup);
    lc_sim_build_view(&setup, &view, full_screen);
    if (lc_lvds_debug_preview_changed("begin", frame ? frame->leancam_preview_line : "")) {
        LC_LVDS_DBG("preview begin mode=%u fs=%u line=%.48s",
                    frame ? (unsigned)frame->leancam_mode : 255u,
                    full_screen ? 1u : 0u,
                    frame ? frame->leancam_preview_line : "");
    }
    if (step_count > 1u && step == 0u) {
        lc_sim_draw_stock(&view, &setup);
        return;
    }
    if (step_count <= 1u || step == 0u)
        lc_sim_draw_stock(&view, &setup);

    if (step_count > 1u && step == 1u)
        return;

    if (lc_sim_draw_raw_grouped_preview(frame, &view, full_screen && step_count <= 1u)) {
        if (step_count <= 1u || step + 1u >= step_count) {
            lc_sim_draw_nc_overlay(frame, &view);
            lc_sim_draw_live_tool(frame, &view);
        }
        if (lc_lvds_debug_preview_changed("raw grouped", frame->leancam_preview_line)) {
            LC_LVDS_DBG("preview raw grouped done");
        }
        return;
    }

    line = frame->leancam_preview_line;
    if (!line || !line[0] || lc_is_cycle(line, "SETUP")) {
        lc_sim_draw_nc_overlay(frame, &view);
        lc_sim_draw_live_tool(frame, &view);
        if (lc_lvds_debug_preview_changed("empty", line)) {
            LC_LVDS_DBG("preview setup/empty done");
        }
        return;
    }

    if (lc_is_cycle(line, "TOOLCALL")) {
        /* Tool call uses only the small resolved-tool glyph in the footer area. */
    } else if (lc_is_cycle(line, "OD")) {
        if (lc_lvds_debug_preview_changed("OD start", line)) {
            LC_LVDS_DBG("preview OD start");
        }
        lc_sim_draw_turn(&view, line, frame, true);
        if (lc_lvds_debug_preview_changed("OD done", line)) {
            LC_LVDS_DBG("preview OD done");
        }
    } else if (lc_is_cycle(line, "ID")) {
        if (lc_lvds_debug_preview_changed("ID start", line)) {
            LC_LVDS_DBG("preview ID start");
        }
        lc_sim_draw_turn(&view, line, frame, false);
        if (lc_lvds_debug_preview_changed("ID done", line)) {
            LC_LVDS_DBG("preview ID done");
        }
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
    } else if (lc_is_cycle(line, "DRILL") || lc_is_cycle(line, "TAP")) {
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
        if (lc_is_cycle(line, "TAP")) {
            float pitch = 0.0f;
            int p;
            (void)lc_field_float2(line, "PITCH", "P", &pitch);
            if (pitch > 0.0f) {
                int step = lc_clampi((int)(pitch * view.scale + 0.5f), 5, 18);
                for (p = x1; p < x2; p += step) {
                    lvds_draw_line(p, view.stock_top + 2, lc_clampi(p + 8, x1, x2), y2 - 2, LC_PREVIEW_PROFILE_FG);
                }
                lc_sim_label(&view, x1, y2 + 24, "P", pitch);
            }
        }
    } else if (lc_is_cycle(line, "CHAMFER") || lc_is_cycle(line, "CHAMFER_OD") ||
               lc_is_cycle(line, "CHMF_ID") || lc_is_cycle(line, "CHAMFER_ID")) {
        char turn_line[128];
        bool is_od = !lc_is_cycle(line, "CHMF_ID") && !lc_is_cycle(line, "CHAMFER_ID");
        if (lc_sim_build_chamfer_turn_line(line, &setup, is_od, turn_line, sizeof(turn_line))) {
            lc_sim_draw_turn(&view, turn_line, frame, is_od);
        }
    } else if (lc_is_cycle(line, "R_OD") || lc_is_cycle(line, "R_ID") ||
               lc_is_cycle(line, "RADIUS_OD") || lc_is_cycle(line, "RADIUS_ID")) {
        char turn_line[128];
        bool is_od = lc_is_cycle(line, "R_OD") || lc_is_cycle(line, "RADIUS_OD");
        if (lc_sim_build_radius_turn_line(line, &setup, is_od, turn_line, sizeof(turn_line))) {
            lc_sim_draw_turn(&view, turn_line, frame, is_od);
        }
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
    } else if (lc_is_cycle(line, "THR_OD") || lc_is_cycle(line, "THR_ID") || lc_is_cycle(line, "G76")) {
        float z1, z2, pitch = 1.5f, d = setup.od;
        float depth = 0.0f, final_d = 0.0f, angle = 0.0f;
        int x1, x2, y, y2, p, tmp;
        if (!lc_field_float2(line, "Z1", "Z_START", &z1)) z1 = 0.0f;
        if (!lc_field_float3(line, "Z2", "Z_END", "Z", &z2)) return;
        if (lc_is_cycle(line, "G76")) {
            (void)lc_field_float2(line, "P", "PITCH", &pitch);
            (void)lc_field_float2(line, "START_X", "X_START", &d);
            if (!lc_field_float2(line, "X", "X_END", &final_d)) {
                (void)lc_field_float2(line, "K", "DEPTH", &depth);
                final_d = d - lc_absf(depth);
            }
            (void)lc_field_float2(line, "Q", "ANGLE", &angle);
        } else {
            (void)lc_field_float3(line, "P", "PITCH", "K", &pitch);
            (void)lc_field_float3(line, "M", "D", "OD", &d);
            final_d = d;
        }
        x1 = lc_sim_zx(&view, z1);
        x2 = lc_sim_zx(&view, z2);
        if (x2 < x1) { tmp = x1; x1 = x2; x2 = tmp; }
        y = lc_sim_dy(&view, d);
        y2 = lc_sim_dy(&view, final_d > 0.0f ? final_d : d);
        lvds_draw_line_w(x1, y2, x2, y2, LC_PREVIEW_CUT_FG, 3);
        if (y2 != y) {
            int yh = y < y2 ? y : y2;
            int hh = y < y2 ? (y2 - y + 1) : (y - y2 + 1);
            lc_sim_hatch_rect(x1, yh, x2 - x1 + 1, hh, true);
        }
        for (p = x1; p < x2; p += lc_clampi((int)(pitch * view.scale + 0.5f), 5, 18)) {
            lvds_draw_line(p, y2 - 8, p + 8, y2 + 8, LC_PREVIEW_PROFILE_FG);
        }
        lc_sim_label(&view, x1, y2 + 16, "P", pitch);
        if (angle > 0.0f)
            lc_sim_label(&view, x1 + 72, y2 + 16, "Q", angle);
    }

    lc_sim_draw_nc_overlay(frame, &view);
    lc_sim_draw_live_tool(frame, &view);
    if (lc_lvds_debug_preview_changed("end", line)) {
        LC_LVDS_DBG("preview end");
    }
}

static void lc_paced_draw_live_tool_for_frame(const ui_snapshot_frame_t *frame, bool full_screen);

static void lc_snapshot_paced_draw_reset(const lc_sim_view_t *view)
{
    g_fullscreen_gcode_draw_z = 0.0f;
    g_fullscreen_gcode_draw_d = view ? view->stock_od : 0.0f;
    g_fullscreen_gcode_draw_have_pos = false;
    g_fullscreen_gcode_draw_diameter_mode = true;
    g_fullscreen_gcode_draw_feed_class = (uint8_t)LC_PREVIEW_SEG_FEED;
}

static bool lc_snapshot_paced_draw_line(const lc_sim_view_t *view, const char *g)
{
    float next_z;
    float next_d;
    bool has_z;
    bool has_x;
    bool motion;
    bool is_g0;
    bool is_arc;
    bool is_cw;

    if (!view || !g || !g[0])
        return false;
    if (g[0] == '(') {
        if (strstr(g, "rough"))
            g_fullscreen_gcode_draw_feed_class = (uint8_t)LC_PREVIEW_SEG_ROUGH_FEED;
        else if (strstr(g, "finish continuous contour"))
            g_fullscreen_gcode_draw_feed_class = (uint8_t)LC_PREVIEW_SEG_FINISH_FEED;
        return false;
    }

    next_z = g_fullscreen_gcode_draw_z;
    next_d = g_fullscreen_gcode_draw_d;
    if (lc_gcode_has_word(g, 'G', 7))
        g_fullscreen_gcode_draw_diameter_mode = true;
    else if (lc_gcode_has_word(g, 'G', 8))
        g_fullscreen_gcode_draw_diameter_mode = false;

    has_z = lc_gcode_axis_value(g, 'Z', &next_z);
    has_x = lc_gcode_axis_value(g, 'X', &next_d);
    if (has_x)
        next_d = lc_absf(next_d) * (g_fullscreen_gcode_draw_diameter_mode ? 1.0f : 2.0f);

    is_g0 = lc_gcode_has_word(g, 'G', 0);
    motion = lc_gcode_has_motion(g) && (has_z || has_x);
    is_cw = lc_gcode_has_word(g, 'G', 2);
    is_arc = is_cw || lc_gcode_has_word(g, 'G', 3);

    if (motion && g_fullscreen_gcode_draw_have_pos) {
        lc_preview_segment_class_t segment_class = is_g0 ?
                                                   LC_PREVIEW_SEG_RAPID_G0 :
                                                   (lc_preview_segment_class_t)g_fullscreen_gcode_draw_feed_class;
        lvds_color_t color = LC_PREVIEW_PROFILE_FG;
        int width = 2;

        if (segment_class == LC_PREVIEW_SEG_ROUGH_FEED) {
            color = LC_PREVIEW_HATCH_FG;
            width = 1;
        } else if (segment_class == LC_PREVIEW_SEG_FINISH_FEED) {
            color = LC_PREVIEW_ACTIVE_VALUE_FG;
            width = 3;
        }

        if (is_g0) {
            lc_raw_preview_draw_dashed_segment(view,
                                               g_fullscreen_gcode_draw_z,
                                               g_fullscreen_gcode_draw_d,
                                               next_z,
                                               next_d,
                                               LC_PREVIEW_AXIS_FG);
        } else if (is_arc) {
            float arc_i = 0.0f;
            float arc_k = 0.0f;
            float arc_r = 0.0f;
            bool has_i = lc_gcode_axis_value(g, 'I', &arc_i);
            bool has_k = lc_gcode_axis_value(g, 'K', &arc_k);

            if (!(has_i && has_k) && lc_gcode_axis_value(g, 'R', &arc_r)) {
                if (!lc_raw_preview_draw_r_arc_display(view,
                                                        g_fullscreen_gcode_draw_z,
                                                        g_fullscreen_gcode_draw_d,
                                                        next_z,
                                                        next_d,
                                                        arc_r,
                                                        is_cw,
                                                        color,
                                                        width)) {
                    lc_raw_preview_draw_line_segment(view,
                                                     g_fullscreen_gcode_draw_z,
                                                     g_fullscreen_gcode_draw_d,
                                                     next_z,
                                                     next_d,
                                                     LC_PREVIEW_ERROR_FG,
                                                     width);
                }
            } else if (has_i && has_k) {
                lc_sim_draw_nc_arc(view,
                                   g_fullscreen_gcode_draw_z,
                                   g_fullscreen_gcode_draw_d,
                                   next_z,
                                   next_d,
                                   arc_i,
                                   arc_k,
                                   is_cw,
                                   segment_class == LC_PREVIEW_SEG_FINISH_FEED);
            } else {
                lc_raw_preview_draw_line_segment(view,
                                                 g_fullscreen_gcode_draw_z,
                                                 g_fullscreen_gcode_draw_d,
                                                 next_z,
                                                 next_d,
                                                 color,
                                                 width);
            }
        } else {
            lc_raw_preview_draw_line_segment(view,
                                             g_fullscreen_gcode_draw_z,
                                             g_fullscreen_gcode_draw_d,
                                             next_z,
                                             next_d,
                                             color,
                                             width);
        }
    }

    if (has_z || has_x) {
        g_fullscreen_gcode_draw_z = next_z;
        g_fullscreen_gcode_draw_d = next_d;
        g_fullscreen_gcode_draw_have_pos = true;
    }

    return motion;
}

static void lc_sim_draw_preview(const ui_snapshot_frame_t *frame)
{
    static uint32_t paced_pane_hash;
    static uint16_t paced_pane_seq;
    lc_sim_setup_t setup;
    lc_sim_view_t view;
    uint32_t hash;

    if (frame && frame->leancam_fullscreen_sim) {
        lc_sim_read_setup(frame, &setup);
        lc_sim_build_view(&setup, &view, false);
        hash = lc_raw_preview_region_hash(frame);
        if (paced_pane_hash != hash) {
            paced_pane_hash = hash;
            paced_pane_seq = 0;
            lc_snapshot_paced_draw_reset(&view);
            lc_sim_draw_stock(&view, &setup);
        }
        if (frame->leancam_sim_preview_seq != paced_pane_seq) {
            paced_pane_seq = frame->leancam_sim_preview_seq;
            (void)lc_snapshot_paced_draw_line(&view, frame->leancam_sim_preview_line);
            leancam_bridge_preview_ack(paced_pane_seq);
        }
        lc_paced_draw_live_tool_for_frame(frame, false);
        return;
    }
    paced_pane_hash = 0;
    lc_sim_draw_preview_ex(frame, false);
}

static void draw_block_meter(int x, int y, lvds_color_t fg, lvds_color_t bg);
static void draw_leancam_footer(const ui_snapshot_frame_t *frame,
                                const char *helper,
                                const char *helper_fallback);
static const char *lc_split_preview_title(const ui_snapshot_frame_t *frame);
static void lc_sim_draw_preview_ex_step(const ui_snapshot_frame_t *frame,
                                        bool full_screen,
                                        uint8_t step,
                                        uint8_t step_count);
static int lc_wrapped_text_height(const char *text, int cols);
static void lc_draw_wrapped_text(int x,
                                 int y,
                                 const char *text,
                                 int cols,
                                 lvds_color_t fg,
                                 lvds_color_t bg,
                                 bool has_hi,
                                 uint8_t hi_start,
                                 uint8_t hi_end);

static bool lc_should_draw_fullscreen_preview(const ui_snapshot_frame_t *frame)
{
#if !LVDS_RENDERER_FULLSCREEN_SIM_PREVIEW
    (void)frame;
    return false;
#else
    if (!frame || !frame->leancam_fullscreen_sim)
        return false;
    if (frame->leancam_mode != LC_RENDER_MODE_PROGRAM)
        return false;
    if (!frame->leancam_preview_line[0] ||
        strchr(frame->leancam_preview_line, '{') ||
        strchr(frame->leancam_preview_line, '}'))
        return false;
    return true;
#endif
}

static void lc_paced_draw_live_tool_for_frame(const ui_snapshot_frame_t *frame, bool full_screen)
{
    lc_sim_setup_t setup;
    lc_sim_view_t view;

    if (!frame)
        return;
    lc_sim_read_setup(frame, &setup);
    lc_sim_build_view(&setup, &view, full_screen);
    lc_sim_draw_live_tool(frame, &view);
}

static void draw_fullscreen_preview(const ui_snapshot_frame_t *frame)
{
    char buf[96];
    uint32_t hash;
    lc_sim_setup_t setup;
    lc_sim_view_t view;
    static uint16_t fullscreen_paced_seq;
    uint8_t step;

    if (!frame)
        return;
    hash = lc_fullscreen_preview_hash(frame);
    if (g_fullscreen_preview_hash != hash) {
        g_fullscreen_preview_hash = hash;
        g_fullscreen_preview_step = 0u;
        lc_sim_read_setup(frame, &setup);
        lc_sim_build_view(&setup, &view, true);
        lc_snapshot_paced_draw_reset(&view);
        fullscreen_paced_seq = 0;
    }
    step = g_fullscreen_preview_step;
    if (g_fullscreen_preview_step < LC_FULLSCREEN_PREVIEW_STEPS)
        g_fullscreen_preview_step++;

    if (step == 0u) {
        lvds_draw_fill_rect(0, 42, LVDS_HSTX_WIDTH, 558, LC_PREVIEW_BG);
        lvds_draw_text_clip(26,
                            62,
                            "Preview",
                            12,
                            LC_PREVIEW_TITLE_FG,
                            LC_PREVIEW_PANEL_BG,
                            LVDS_FONT_LARGE);
        lvds_ui_live_draw_source_rows(frame,
                                      142,
                                      62,
                                      LC_PREVIEW_LABEL_FG,
                                      LC_PREVIEW_TITLE_FG,
                                      LC_PREVIEW_PANEL_BG,
                                      LVDS_FONT_NORMAL);
        lc_sim_read_setup(frame, &setup);
        lc_sim_build_view(&setup, &view, true);
        lc_sim_draw_stock(&view, &setup);
    }
    lc_sim_read_setup(frame, &setup);
    lc_sim_build_view(&setup, &view, true);
    if (frame->leancam_sim_preview_seq != fullscreen_paced_seq) {
        fullscreen_paced_seq = frame->leancam_sim_preview_seq;
        (void)lc_snapshot_paced_draw_line(&view, frame->leancam_sim_preview_line);
        leancam_bridge_preview_ack(fullscreen_paced_seq);
    }
    lc_paced_draw_live_tool_for_frame(frame, true);
    snprintf(buf, sizeof(buf), "Sim X %.3f  Z %.3f",
             frame->axes_valid ? (double)frame->axis[0] : 0.0,
             frame->axes_valid ? (double)frame->axis[2] : 0.0);
    lc_text_clip(26, 566, buf, 32, LC_PREVIEW_LABEL_FG, LC_PREVIEW_PANEL_BG, LVDS_FONT_SMALL);
    draw_leancam_footer(frame, "A Back|# Run", "A Back | # Run");
}

static void draw_split_live_preview(const ui_snapshot_frame_t *frame)
{
    uint32_t t0;
    uint32_t t1;
    bool chuck_collision = false;

    if (!g_live_sim_mask) {
        g_live_sim_mask = (uint8_t *)lc_resource_psram_region(LC_PSRAM_REGION_LIVE_SIM, LC_LIVE_SIM_W * LC_LIVE_SIM_H);
    }

    if (lc_live_sim_needs_reset(frame, false)) {
        lc_live_sim_reset(frame, false);
    } else if (!g_live_sim_was_running) {
        lc_live_sim_force_redraw();
    }

    t0 = mcu_micros();
    lvds_draw_fill_rect(LC_LEFT_LIVE_CLEAR_X,
                        LC_LEFT_LIVE_CLEAR_Y,
                        LC_LEFT_LIVE_CLEAR_W,
                        LC_LEFT_LIVE_CLEAR_H,
                        LC_LIVE_BG);
    g_prof_clear_us += mcu_micros() - t0;

    if (g_live_sim_ready) {
        chuck_collision = lc_live_tool_hits_chuck(frame, &g_live_sim_view, &g_live_sim_setup);
        lc_live_sim_update_cut(frame);

        t0 = mcu_micros();
        lc_live_clear_tool_rect();
        lc_live_sim_draw_material();
        t1 = mcu_micros();
        g_live_prof_material_us = t1 - t0;

        t0 = mcu_micros();
        lvds_draw_text(g_live_sim_view.z0_x - 12, g_live_sim_view.stock_top - 24, "Z0", LC_LIVE_LABEL_FG, LC_LIVE_PANEL_BG, LVDS_FONT_NORMAL);
        lc_sim_draw_live_chuck(&g_live_sim_view, &g_live_sim_setup, chuck_collision);
        lc_live_draw_tool(frame, &g_live_sim_view);
        t1 = mcu_micros();
        g_live_prof_overlay_us = t1 - t0;
    } else {
        t0 = mcu_micros();
        lc_text_clip(LC_PREVIEW_X, 128, "Live sim needs PSRAM", 28, LC_LIVE_COLLISION, LC_LIVE_PANEL_BG, LVDS_FONT_NORMAL);
        lc_sim_draw_preview_ex(frame, false);
        t1 = mcu_micros();
        g_live_prof_material_us = 0;
        g_live_prof_overlay_us = t1 - t0;
    }
}

static bool lc_nc_live_preview_running(const ui_snapshot_frame_t *frame)
{
    return frame &&
           frame->leancam_mode == LC_RENDER_MODE_NC_VIEW &&
           (frame->motion_active || (frame->state & (EXEC_RUN | EXEC_HOLD)));
}

static void lc_nc_live_draw_position_guides(const ui_snapshot_frame_t *frame,
                                            const lc_sim_view_t *view)
{
    int zx;
    int dy;
    int width = LEANCAM_NC_LIVE_GUIDE_WIDTH;
    lvds_color_t guide = LC_LIVE_TOOL_FG;

    if (!frame || !view || !frame->axes_valid)
        return;
    if (width <= 0)
        return;

    zx = lc_clampi(lc_sim_zx_view(view, frame->axis[2]), view->x0, view->x1);
    dy = lc_clampi(lc_sim_dy_view(view, lc_live_runtime_x_to_diam(frame->axis[0])), view->y0, view->y1);
    lvds_draw_line_w(zx, view->y0, zx, view->y1, guide, width);
    lvds_draw_line_w(view->x0, dy, view->x1, dy, guide, width);
    lvds_draw_line_w(zx - 8, dy, zx + 8, dy, guide, width);
    lvds_draw_line_w(zx, dy - 8, zx, dy + 8, guide, width);
}

static void draw_nc_live_preview(const ui_snapshot_frame_t *frame)
{
    uint32_t t0;
    uint32_t t1;
    bool chuck_collision = false;

    if (!g_live_sim_mask) {
        g_live_sim_mask = (uint8_t *)lc_resource_psram_region(LC_PSRAM_REGION_LIVE_SIM, LC_LIVE_SIM_W * LC_LIVE_SIM_H);
    }

    if (lc_live_sim_needs_reset(frame, false)) {
        lc_live_sim_reset(frame, false);
    } else if (!g_live_sim_was_running) {
        lc_live_sim_force_redraw();
    }

    t0 = mcu_micros();
    lvds_draw_fill_rect(LC_LEFT_LIVE_CLEAR_X,
                        LC_LEFT_LIVE_CLEAR_Y,
                        LC_LEFT_LIVE_CLEAR_W,
                        LC_LEFT_LIVE_CLEAR_H,
                        LC_LIVE_BG);
    g_prof_clear_us += mcu_micros() - t0;

    if (g_live_sim_ready) {
        chuck_collision = lc_live_tool_hits_chuck(frame, &g_live_sim_view, &g_live_sim_setup);
        lc_live_sim_update_cut(frame);

        t0 = mcu_micros();
        lc_live_clear_tool_rect();
        lc_live_sim_draw_material();
        t1 = mcu_micros();
        g_live_prof_material_us = t1 - t0;

        t0 = mcu_micros();
        (void)lc_sim_draw_region_contour_only(frame, &g_live_sim_view, false);
        lc_nc_live_draw_position_guides(frame, &g_live_sim_view);
        lvds_draw_text(g_live_sim_view.z0_x - 12, g_live_sim_view.stock_top - 24, "Z0", LC_LIVE_LABEL_FG, LC_LIVE_PANEL_BG, LVDS_FONT_NORMAL);
        lc_sim_draw_live_chuck(&g_live_sim_view, &g_live_sim_setup, chuck_collision);
        lc_live_draw_tool(frame, &g_live_sim_view);
        t1 = mcu_micros();
        g_live_prof_overlay_us = t1 - t0;
    } else {
        t0 = mcu_micros();
        lc_text_clip(LC_PREVIEW_X, 128, "Live sim needs PSRAM", 28, LC_LIVE_COLLISION, LC_LIVE_PANEL_BG, LVDS_FONT_NORMAL);
        lc_sim_draw_preview_ex(frame, false);
        t1 = mcu_micros();
        g_live_prof_material_us = 0;
        g_live_prof_overlay_us = t1 - t0;
    }
}

static void draw_live_run_preview(const ui_snapshot_frame_t *frame)
{
    uint32_t t0;
    uint32_t t1;
    bool chuck_collision = false;

    if (!g_live_sim_mask) {
        g_live_sim_mask = (uint8_t *)lc_resource_psram_region(LC_PSRAM_REGION_LIVE_SIM, LC_LIVE_SIM_W * LC_LIVE_SIM_H);
    }

    if (lc_live_sim_needs_reset(frame, true)) {
        lc_live_sim_reset(frame, true);
    } else if (!g_live_sim_was_running) {
        lc_live_sim_force_redraw();
    }

    t0 = mcu_micros();
    if (!g_live_sim_static_drawn) {
        lvds_draw_fill_rect(0, 42, LVDS_HSTX_WIDTH, 558, LC_LIVE_BG);
        lvds_ui_live_draw_header(frame,
                                 26,
                                 62,
                                 "Live",
                                 18,
                                 132,
                                 62,
                                 650,
                                 62,
                                 17,
                                 LC_LIVE_TITLE_FG,
                                 LC_LIVE_LABEL_FG,
                                 LC_LIVE_VALUE_FG,
                                 LC_LIVE_VALUE_FG,
                                 LC_LIVE_PANEL_BG,
                                 LVDS_FONT_LARGE,
                                 LVDS_FONT_NORMAL);
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
            lvds_draw_text(g_live_sim_view.z0_x - 12, g_live_sim_view.stock_top - 24, "Z0", LC_LIVE_LABEL_FG, LC_LIVE_PANEL_BG, LVDS_FONT_NORMAL);
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
        lc_live_clear_tool_rect();
        lc_live_sim_draw_material();
        t1 = mcu_micros();
        g_live_prof_material_us = t1 - t0;

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
    /* draw_perf_meter(626, 574, LC_LIVE_DEBUG_FG, LC_LIVE_PANEL_BG); */
    t1 = mcu_micros();
    g_live_prof_footer_us = t1 - t0;
}

static const char *lc_split_preview_title(const ui_snapshot_frame_t *frame)
{
    if (frame && (frame->leancam_mode == LC_RENDER_MODE_FILES || frame->leancam_mode == 1)) {
        return "File manager";
    }
    if (lc_frame_is_tool_asset(frame)) {
        return "Tool editor";
    }
    if (frame && frame->leancam_mode == LC_RENDER_MODE_NC_VIEW) {
        return "NC run";
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

static const char *frame_state_text(const ui_snapshot_frame_t *frame)
{
    if (!frame) {
        return "BOOT";
    }
    if (frame->g33_active || frame->g33_sync_valid) {
        return "G33";
    }
    return exec_state_text(frame->state);
}

static void draw_perf_meter(int x, int y, lvds_color_t fg, lvds_color_t bg)
{
    lvds_ui_draw_perf_meter(x,
                            y,
                            fg,
                            bg,
                            LVDS_RENDERER_SHOW_PERF_METERS,
                            g_render_fps_x10,
                            g_render_last_us);
}

static void draw_leancam_footer(const ui_snapshot_frame_t *frame,
                                const char *helper,
                                const char *helper_fallback)
{
    char message[UI_SNAPSHOT_POPUP_LEN];

    if (frame) {
        snprintf(message,
                 sizeof(message),
                 "%.26s B%lu U%lu R%lu",
                 frame->leancam_message,
                 (unsigned long)frame->diag_build_count,
                 (unsigned long)frame->diag_uptime_s,
                 (unsigned long)(mcu_millis() / 1000u));
    } else {
        message[0] = 0;
    }

    lvds_ui_footer_draw_status(LVDS_HSTX_WIDTH,
                               548,
                               52,
                               12,
                               552,
                               12,
                               574,
                               LC_FOOTER_TEXT_COLS,
                               message,
                               helper,
                               helper_fallback,
                               LC_COL_FOOTER_BG,
                               LC_COL_FOOTER_VALUE,
                               LC_COL_FOOTER_TEXT,
                               lvds_palette_color(white_warm),
                               lvds_palette_color(black),
                               lvds_palette_color(gray_96),
                               LC_COL_FOOTER_TEXT,
                               LVDS_FONT_NORMAL);
    draw_block_meter(350, 552, LC_COL_FOOTER_TEXT, LC_COL_FOOTER_BG);
    draw_perf_meter(626, 574, LC_COL_FOOTER_TEXT, LC_COL_FOOTER_BG);
}

static void draw_block_meter(int x, int y, lvds_color_t fg, lvds_color_t bg)
{
    lvds_ui_draw_block_meter(x,
                             y,
                             fg,
                             bg,
                             LVDS_RENDERER_SHOW_PERF_METERS,
                             g_prof_header_us,
                             g_prof_clear_us,
                             g_prof_rows_us,
                             g_prof_preview_us,
                             g_prof_footer_us,
                             g_live_prof_present_us);
}

static void draw_normal_meters_only(const ui_snapshot_frame_t *frame)
{
    uint32_t t0 = mcu_micros();
    g_prof_clear_us = 0;
    g_prof_rows_us = 0;
    g_prof_preview_us = 0;
    draw_leancam_footer(frame,
                        frame ? frame->leancam_helper : "",
                        frame ? frame->leancam_helper : "");
    draw_block_meter(350, 552, LC_COL_FOOTER_TEXT, LC_COL_FOOTER_BG);
    draw_perf_meter(626, 574, LC_COL_FOOTER_TEXT, LC_COL_FOOTER_BG);
    g_prof_footer_us = mcu_micros() - t0;
}

static void draw_tool_editor_rows(const ui_snapshot_frame_t *frame)
{
    uint32_t t0;
    int i;
    int y;
    lvds_tool_editor_layout_t layout;
    bool tool_asset = lc_frame_is_tool_asset(frame);

    lvds_ui_tool_editor_layout(LVDS_HSTX_WIDTH,
                               LC_TOOL_EDITOR_LEFT_X,
                               LC_TOOL_EDITOR_LEFT_W,
                               LC_TOOL_EDITOR_RIGHT_X,
                               LC_TOOL_EDITOR_RIGHT_COLS,
                               tool_asset ? 1u : 0u,
                               &layout);
    y = layout.row_y;

    g_prof_clear_us = 0;
    g_prof_rows_us = 0;
    g_prof_preview_us = 0;
    g_prof_footer_us = 0;

    t0 = mcu_micros();
    lvds_draw_fill_rect(0, 42, LVDS_HSTX_WIDTH, 558, LC_COL_BG);
    lc_text_clip(layout.title_x, layout.title_y, "Tool editor", 18, LC_COL_TEXT, LC_COL_BG, LVDS_FONT_LARGE);
    g_prof_clear_us = mcu_micros() - t0;

    t0 = mcu_micros();
    for (i = 0; i < frame->leancam_line_count && i < UI_LC_MAX_LINES && layout.max_rows > 0; ++i) {
        lvds_color_t fg = frame->leancam_line_selected[i] ? LC_COL_HI : LC_COL_TEXT;
        lvds_color_t row_bg = frame->leancam_line_selected[i] ? LC_COL_SELECT : LC_COL_BG;
        bool has_hi = frame->leancam_field_hi_end[i] > frame->leancam_field_hi_start[i];
        int row_h = lc_wrapped_text_height(frame->leancam_lines[i], layout.text_cols) + 4;

        if (frame->leancam_line_selected[i]) {
            lvds_draw_fill_rect(layout.row_cell_x, y - 3, layout.row_cell_w, row_h, row_bg);
            lvds_draw_rect(layout.row_cell_x, y - 3, layout.row_cell_w, row_h, LC_COL_HI);
        }
        if (tool_asset) {
            lc_draw_tool_row_glyph(frame->leancam_lines[i],
                                   layout.glyph_x + 2,
                                   y + 1,
                                   frame->leancam_line_selected[i] != 0);
        }
        lc_draw_wrapped_text(layout.text_x,
                             y,
                             frame->leancam_lines[i],
                             layout.text_cols,
                             fg,
                             row_bg,
                             has_hi,
                             frame->leancam_field_hi_start[i],
                             frame->leancam_field_hi_end[i]);
        y += row_h;
        layout.max_rows--;
    }
    g_prof_rows_us = mcu_micros() - t0;

    t0 = mcu_micros();
    if (tool_asset)
        lc_draw_tool_table_detail(frame, layout.detail_y);
    lc_text_clip(layout.active_field_x, layout.active_field_y, frame->leancam_active_field,
                 34, LC_COL_VALUE, LC_COL_BG, LVDS_FONT_SMALL);
    g_prof_preview_us = mcu_micros() - t0;

    t0 = mcu_micros();
    draw_leancam_footer(frame, frame->leancam_helper, frame->leancam_helper);
    g_prof_footer_us = mcu_micros() - t0;
}

static void draw_normal_preview_only(const ui_snapshot_frame_t *frame)
{
    uint32_t t0;
    bool split_live = lc_live_sim_running(frame) && LVDS_RENDERER_LIVE_SPLIT_PANEL;
    bool nc_live = lc_nc_live_preview_running(frame);

    g_prof_rows_us = 0;
    g_prof_footer_us = 0;

    t0 = mcu_micros();
    if (!split_live && !nc_live && !(frame && frame->leancam_fullscreen_sim)) {
        lvds_draw_fill_rect(LC_LEFT_PANE_X + 2, 86, LC_LEFT_PANE_W - 16, 458, LC_COL_BG);
    }
    g_prof_clear_us = mcu_micros() - t0;

    t0 = mcu_micros();
    if (nc_live) {
        draw_nc_live_preview(frame);
    } else if (split_live) {
        draw_split_live_preview(frame);
    } else if (lc_frame_is_catalog_asset(frame)) {
        lc_draw_catalog_asset_preview(frame);
    } else {
        lc_sim_draw_preview(frame);
    }
    lc_text_clip(LC_PREVIEW_X, 522, frame->leancam_active_field,
                 34, LC_COL_VALUE, LC_COL_BG, LVDS_FONT_SMALL);
    g_prof_preview_us = mcu_micros() - t0;

    t0 = mcu_micros();
    draw_block_meter(350, 552, LC_COL_FOOTER_TEXT, LC_COL_FOOTER_BG);
    draw_perf_meter(626, 574, LC_COL_FOOTER_TEXT, LC_COL_FOOTER_BG);
    g_prof_footer_us = mcu_micros() - t0;
}

static void draw_bar(const ui_snapshot_frame_t *frame)
{
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

    lvds_ui_header_draw(LVDS_HSTX_WIDTH,
                        frame_state_text(frame),
                        x,
                        z,
                        feed,
                        spindle,
                        LC_COL_TOP,
                        LC_COL_TEXT,
                        LVDS_FONT_LARGE);
}

static int lc_wrapped_text_height(const char *text, int cols)
{
    return lvds_ui_text_wrapped_height(text, cols);
}

static void lc_draw_wrapped_text(int x,
                                 int y,
                                 const char *text,
                                 int cols,
                                 lvds_color_t fg,
                                 lvds_color_t bg,
                                 bool has_hi,
                                 uint8_t hi_start,
                                 uint8_t hi_end)
{
    lvds_ui_text_draw_wrapped(x,
                              y,
                              text,
                              cols,
                              fg,
                              bg,
                              LC_COL_VALUE,
                              LC_COL_BG,
                              LC_COL_HI,
                              LVDS_FONT_NORMAL,
                              has_hi,
                              hi_start,
                              hi_end);
}

static void draw_leancam_rows(const ui_snapshot_frame_t *frame)
{
    uint32_t t0;
    int i;
    int y;
    lvds_program_layout_t layout;
    bool split_live = lc_live_sim_running(frame) && LVDS_RENDERER_LIVE_SPLIT_PANEL;
    bool nc_live = lc_nc_live_preview_running(frame);
    bool compact_rows = frame->leancam_mode == LC_RENDER_MODE_FILES ||
                        frame->leancam_mode == LC_RENDER_MODE_NC_VIEW;

    if (lc_frame_is_catalog_asset(frame)) {
        draw_tool_editor_rows(frame);
        return;
    }
    lvds_ui_program_layout(LVDS_HSTX_WIDTH,
                           LC_LEFT_PANE_X,
                           LC_LEFT_PANE_W,
                           LC_RIGHT_PANE_X,
                           LC_RIGHT_PANE_W,
                           LC_RIGHT_TEXT_COLS,
                           &layout);
    y = layout.row_y;

    g_prof_clear_us = 0;
    g_prof_rows_us = 0;
    g_prof_preview_us = 0;
    g_prof_footer_us = 0;

    t0 = mcu_micros();
    lvds_ui_program_draw_shell(LVDS_HSTX_WIDTH,
                               &layout,
                               lc_split_preview_title(frame),
                               frame->leancam_title,
                               frame->leancam_title[0] &&
                                   frame->leancam_mode != LC_RENDER_MODE_FILES &&
                                   frame->leancam_mode != LC_RENDER_MODE_NC_VIEW,
                               LC_COL_BG,
                               LC_COL_LINE,
                               LC_COL_TEXT,
                               LC_COL_DIM,
                               LVDS_FONT_LARGE,
                               LVDS_FONT_NORMAL);
    g_prof_clear_us = mcu_micros() - t0;

    t0 = mcu_micros();
    if (compact_rows) {
        int max_rows = layout.compact_max_rows;
        for (i = 0; i < frame->leancam_line_count && i < UI_LC_MAX_LINES && max_rows > 0; ++i) {
            lvds_color_t fg = frame->leancam_line_selected[i] ? LC_COL_HI : LC_COL_TEXT;
            lvds_color_t bg = frame->leancam_line_selected[i] ? LC_COL_SELECT : LC_COL_BG;
            int row_h = lc_wrapped_text_height(frame->leancam_lines[i], layout.text_cols);

            if (frame->leancam_line_selected[i]) {
                lvds_draw_fill_rect(layout.row_cell_x, y - 2, layout.row_cell_w, row_h, bg);
                lvds_draw_rect(layout.row_cell_x, y - 2, layout.row_cell_w, row_h, LC_COL_HI);
            }
            lc_draw_wrapped_text(layout.text_x, y, frame->leancam_lines[i], layout.text_cols, fg, bg, false, 0, 0);
            y += row_h;
            max_rows--;
        }
        g_prof_rows_us = mcu_micros() - t0;

        t0 = mcu_micros();
        if (nc_live) {
            draw_nc_live_preview(frame);
        } else if (split_live) {
            draw_split_live_preview(frame);
        } else if (lc_frame_is_catalog_asset(frame)) {
            lc_draw_catalog_asset_preview(frame);
        } else {
            lc_sim_draw_preview(frame);
        }
        if (!lc_frame_is_catalog_asset(frame)) {
            lc_draw_tool_marker_panel(frame, layout.tool_panel_x, layout.tool_panel_y);
        }
        lc_text_clip(layout.preview_active_x, layout.preview_active_y, frame->leancam_active_field,
                     34, LC_COL_VALUE, LC_COL_BG, LVDS_FONT_SMALL);
        g_prof_preview_us = mcu_micros() - t0;

        t0 = mcu_micros();
        draw_leancam_footer(frame, frame->leancam_helper, frame->leancam_helper);
        g_prof_footer_us = mcu_micros() - t0;
        return;
    }

    for (i = 0; i < frame->leancam_line_count && i < UI_LC_MAX_LINES; ++i) {
        lvds_color_t fg = frame->leancam_line_selected[i] ? LC_COL_HI : LC_COL_TEXT;
        lvds_color_t row_bg = frame->leancam_line_selected[i] ? LC_COL_SELECT : LC_COL_BG;
        bool has_hi = frame->leancam_field_hi_end[i] > frame->leancam_field_hi_start[i];
        int row_h = lc_wrapped_text_height(frame->leancam_lines[i], layout.text_cols);

        if (y + row_h > layout.rows_bottom)
            break;

        if (frame->leancam_line_selected[i]) {
            lvds_draw_fill_rect(layout.row_cell_x, y - 3, layout.row_cell_w, row_h, row_bg);
            lvds_draw_rect(layout.row_cell_x, y - 3, layout.row_cell_w, row_h, LC_COL_HI);
        }
        lc_draw_wrapped_text(layout.text_x,
                             y,
                             frame->leancam_lines[i],
                             layout.text_cols,
                             fg,
                             row_bg,
                             has_hi,
                             frame->leancam_field_hi_start[i],
                             frame->leancam_field_hi_end[i]);
        y += row_h;
    }
    g_prof_rows_us = mcu_micros() - t0;

    t0 = mcu_micros();
    if (nc_live) {
        draw_nc_live_preview(frame);
    } else if (split_live) {
        draw_split_live_preview(frame);
    } else if (lc_frame_is_catalog_asset(frame)) {
        lc_draw_catalog_asset_preview(frame);
    } else {
        lc_sim_draw_preview(frame);
    }
    if (!lc_frame_is_catalog_asset(frame)) {
        lc_draw_tool_marker_panel(frame, layout.tool_panel_x, layout.tool_panel_y);
    }
    lc_text_clip(layout.preview_active_x, layout.preview_active_y, frame->leancam_active_field,
                 34, LC_COL_VALUE, LC_COL_BG, LVDS_FONT_SMALL);
    g_prof_preview_us = mcu_micros() - t0;

    t0 = mcu_micros();
    draw_leancam_footer(frame, frame->leancam_helper, frame->leancam_helper);
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
    if (frame->leancam_preview_region_count != old->leancam_preview_region_count ||
        memcmp(frame->leancam_preview_region, old->leancam_preview_region, sizeof(frame->leancam_preview_region)) ||
        memcmp(frame->leancam_preview_region_selected, old->leancam_preview_region_selected, sizeof(frame->leancam_preview_region_selected))) {
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

static bool normal_preview_cursor_changed(const ui_snapshot_frame_t *frame)
{
    const ui_snapshot_frame_t *old = &g_normal_body_frame;

    if (!g_normal_body_valid || !frame) {
        return false;
    }
    if (frame->axes_valid != old->axes_valid) {
        return true;
    }
    if (!frame->axes_valid) {
        return false;
    }
    return frame->axis[0] != old->axis[0] || frame->axis[2] != old->axis[2];
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

void leancam_visual_init(void)
{
    g_last_seq = UINT32_MAX;
    g_normal_body_valid = false;
    lvds_hstx_clear(LC_COL_BG);
    draw_bar(NULL);
    lvds_draw_text(24, 82, "Waiting for LeanCam snapshot", LC_COL_TEXT, LC_COL_BG, LVDS_FONT_NORMAL);
#if EXECUTION_CONTROLLER_CHUNKED_PRESENT
    lvds_hstx_present_chunked_request(EXECUTION_CONTROLLER_PRESENT_CHUNKS);
#else
    lvds_hstx_present();
#endif
}

void leancam_visual_prepare(void)
{
}

uint32_t leancam_visual_reentry_count(void)
{
    return g_render_reentry_count;
}

uint32_t leancam_visual_max_draw_us(void)
{
    return g_render_max_draw_us;
}

void leancam_visual_draw(void)
{
    static uint32_t last_diag_ms;
    static uint32_t last_present_us;
    const ui_snapshot_frame_t *frame;
    uint32_t seq;
    uint32_t now = mcu_millis();
    uint32_t start_us;
    uint32_t draw_us;
    uint32_t end_us;
    bool seq_changed;
    bool diag_tick;
    bool sim_preview_tick;
    bool fullscreen_preview_now;
    bool nc_live_now;

    if (g_render_in_poll) {
        g_render_reentry_count++;
        return;
    }

    if (cnc_is_file_io_critical()) {
        return;
    }

    g_render_in_poll = true;

    frame = leancam_visual_state_frame();
    seq = frame ? frame->seq : 0;
    if (!frame) {
        g_render_in_poll = false;
        return;
    }
    seq_changed = ui_snapshot_has_newer_seq(seq, g_last_seq);
    diag_tick = (uint32_t)(now - last_diag_ms) >= 1000u;
    sim_preview_tick = frame->leancam_fullscreen_sim;
    nc_live_now = lc_nc_live_preview_running(frame);
    fullscreen_preview_now = lc_should_draw_fullscreen_preview(frame);
    if (fullscreen_preview_now && !g_fullscreen_preview_was_active) {
        g_fullscreen_preview_hash = 0;
        g_fullscreen_preview_step = 0u;
        g_normal_body_valid = false;
    }
    g_fullscreen_preview_was_active = fullscreen_preview_now;
    if (!seq_changed && !diag_tick && !sim_preview_tick && !nc_live_now) {
        g_render_in_poll = false;
        return;
    }
    if (diag_tick)
        last_diag_ms = now;
    if (seq_changed && lc_lvds_debug_line_changed(seq, frame->leancam_mode, frame->leancam_preview_line)) {
        LC_LVDS_DBG("poll seq=%lu mode=%u body=%u preview=%.48s tool=%.48s",
                    (unsigned long)seq,
                    (unsigned)frame->leancam_mode,
                    (unsigned)frame->leancam_line_count,
                    frame->leancam_preview_line,
                    frame->leancam_tool_line);
    }

    if (!seq_changed && !sim_preview_tick) {
        start_us = mcu_micros();
        draw_normal_meters_only(frame);
    } else if (!nc_live_now && lc_live_sim_running(frame) && !LVDS_RENDERER_LIVE_SPLIT_PANEL) {
        g_normal_body_valid = false;
        start_us = mcu_micros();
        draw_bar(frame);
        g_prof_header_us = mcu_micros() - start_us;
        if (lc_lvds_debug_branch_changed("live")) LC_LVDS_DBG("branch live");
        draw_live_run_preview(frame);
    } else if (fullscreen_preview_now) {
        g_normal_body_valid = false;
        start_us = mcu_micros();
        draw_bar(frame);
        g_prof_header_us = mcu_micros() - start_us;
        if (lc_lvds_debug_branch_changed("fullscreen")) LC_LVDS_DBG("branch fullscreen");
        draw_fullscreen_preview(frame);
    } else {
        bool body_changed = normal_body_changed(frame);
        bool cursor_changed = normal_preview_cursor_changed(frame) || nc_live_now;
        start_us = mcu_micros();
        draw_bar(frame);
        g_prof_header_us = mcu_micros() - start_us;
        if (frame->leancam_fullscreen_sim) {
            if (lc_lvds_debug_branch_changed("paced pane")) LC_LVDS_DBG("branch paced pane");
            draw_normal_preview_only(frame);
            remember_normal_body(frame);
        } else if (body_changed) {
            if (lc_lvds_debug_branch_changed("rows body")) LC_LVDS_DBG("branch rows body");
            draw_leancam_rows(frame);
            remember_normal_body(frame);
        } else if (cursor_changed) {
            if (lc_lvds_debug_branch_changed("preview cursor")) LC_LVDS_DBG("branch preview cursor");
            draw_normal_preview_only(frame);
            remember_normal_body(frame);
        } else {
            draw_normal_meters_only(frame);
        }
    }
    g_live_sim_was_running = lc_live_sim_running(frame) || nc_live_now;
    draw_us = mcu_micros() - start_us;
    if (draw_us > g_render_max_draw_us)
        g_render_max_draw_us = draw_us;
    start_us = mcu_micros();
#if EXECUTION_CONTROLLER_CHUNKED_PRESENT
    lvds_hstx_present_chunked_request(EXECUTION_CONTROLLER_PRESENT_CHUNKS);
#else
    lvds_hstx_present();
#endif
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
    if (seq_changed)
        g_last_seq = seq;
    g_render_in_poll = false;
}


