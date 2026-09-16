#include "nc_visual.h"

#include "../../cnc.h"
#include "../../interface/grbl_stream.h"
#include "nc.h"
#include "nc_emit.h"
#include "nc_files.h"
#include "nc_menu.h"
#include "nc_palette.h"
#include "nc_presets.h"
#include "nc_run.h"
#include "nc_sim.h"
#include "nc_state.h"
#include "nc_text.h"
#include "nc_tools.h"
#include "../g7x/g7x_contour.h"
#include "../lvds_renderer/lvds_draw_api.h"
#include "../lvds_renderer/lvds_hstx.h"
#if __has_include("../lvds_renderer/lvds_psram.h")
#include "../lvds_renderer/lvds_psram.h"
#define NC_VISUAL_HAVE_PSRAM 1
#else
#define NC_VISUAL_HAVE_PSRAM 0
#endif

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#define NC_PREVIEW_ARC_MAX_STEPS 64
#define NC_PREVIEW_CONTOUR_MAX 48
#ifndef NC_PREVIEW_DIN_STYLE
#define NC_PREVIEW_DIN_STYLE 1
#endif
#ifndef NC_PREVIEW_DIN_POINT_MARKERS
#define NC_PREVIEW_DIN_POINT_MARKERS 1
#endif
#define NC_LIVE_STOCK_MAX_W 680
#define NC_LIVE_STOCK_MAX_H 380
#define NC_LIVE_STOCK_PSRAM_OFFSET (512u * 1024u)

#define NC_VISUAL_CHAR_W   8
#define NC_VISUAL_ROW_H    24
#define NC_LEFT_PANE_X     10
#define NC_LEFT_PANE_W     350
#define NC_SPLIT_X         366
#define NC_RIGHT_PANE_X    374
#define NC_RIGHT_PANE_W    410
#define NC_LINE_NO_X_PAD   4
#define NC_LINE_TEXT_X_PAD 32
#define NC_FOOTER_Y        546
#define NC_FOOTER_H        54
static bool g_nc_visual_in_draw;
static nc_document_t g_nc_visual_doc;
static char g_nc_visual_status[64];
static bool g_nc_visual_dirty;
static bool g_nc_visual_sim_stock = true;
static bool g_nc_visual_sim_path = true;
static bool g_nc_visual_sim_rough = true;
static bool g_nc_visual_show_dims = true;
static uint16_t g_nc_visual_fps;
static uint16_t g_nc_visual_fps_frames;
static uint32_t g_nc_visual_fps_last_ms;
static uint16_t g_nc_visual_stat_snapshot_ms;
static uint16_t g_nc_visual_stat_draw_ms;
static uint16_t g_nc_visual_stat_present_ms;
static uint16_t g_nc_visual_stat_total_ms;
static uint16_t g_nc_visual_stat_header_ms;
static uint16_t g_nc_visual_stat_preview_ms;
static uint16_t g_nc_visual_stat_body_ms;
static uint16_t g_nc_visual_stat_footer_ms;
static uint16_t g_nc_visual_stat_preview_collect_ms;
static uint16_t g_nc_visual_stat_preview_clear_ms;
static uint16_t g_nc_visual_stat_preview_stock_ms;
static uint16_t g_nc_visual_stat_preview_geom_ms;
static uint16_t g_nc_visual_stat_preview_tool_ms;
static uint32_t g_nc_visual_acc_snapshot_us;
static uint32_t g_nc_visual_acc_draw_us;
static uint32_t g_nc_visual_acc_present_us;
static uint32_t g_nc_visual_acc_total_us;
static uint32_t g_nc_visual_acc_header_us;
static uint32_t g_nc_visual_acc_preview_us;
static uint32_t g_nc_visual_acc_body_us;
static uint32_t g_nc_visual_acc_footer_us;
static uint32_t g_nc_visual_acc_preview_collect_us;
static uint32_t g_nc_visual_acc_preview_clear_us;
static uint32_t g_nc_visual_acc_preview_stock_us;
static uint32_t g_nc_visual_acc_preview_geom_us;
static uint32_t g_nc_visual_acc_preview_tool_us;
static uint32_t g_nc_visual_frame_header_us;
static uint32_t g_nc_visual_frame_preview_us;
static uint32_t g_nc_visual_frame_body_us;
static uint32_t g_nc_visual_frame_footer_us;
static uint32_t g_nc_visual_frame_preview_collect_us;
static uint32_t g_nc_visual_frame_preview_clear_us;
static uint32_t g_nc_visual_frame_preview_stock_us;
static uint32_t g_nc_visual_frame_preview_geom_us;
static uint32_t g_nc_visual_frame_preview_tool_us;
static size_t g_nc_visual_last_draw_run_line = (size_t)-1;
static bool g_nc_visual_last_runtime_busy;
static nc_text_edit_t g_nc_visual_edit;
static bool g_nc_visual_new_file_active;
static char g_nc_visual_new_file_name[NC_FILE_NAME_MAX];
static uint8_t *g_nc_live_stock_mask;
static bool g_nc_live_stock_ready;
static bool g_nc_live_stock_was_active;
static bool g_nc_live_stock_has_last;
static int g_nc_live_stock_w;
static int g_nc_live_stock_h;
static float g_nc_live_stock_setup_x;
static float g_nc_live_stock_setup_z;
static float g_nc_live_stock_setup_i;
static float g_nc_live_stock_last_x;
static float g_nc_live_stock_last_z;
static bool g_nc_live_tool_rect_valid;
static int g_nc_live_tool_rect_x;
static int g_nc_live_tool_rect_y;
static int g_nc_live_tool_rect_w;
static int g_nc_live_tool_rect_h;
static bool g_nc_live_preview_cache_valid;
static nc_preview_info_t g_nc_live_preview_cache;
static nc_tool_t g_nc_live_tool_cache;
static bool g_nc_live_have_tool_cache;
static int g_nc_live_stock_w_cache;
static int g_nc_live_stock_h_cache;
static int g_nc_live_stock_left_cache;
static int g_nc_live_stock_top_cache;
static int g_nc_live_z0_x_cache;

typedef enum {
    NC_PREVIEW_SEG_FEED = 0,
    NC_PREVIEW_SEG_ROUGH,
    NC_PREVIEW_SEG_FINISH
} nc_preview_segment_t;

static void nc_visual_draw_text_clip(int x,
                                     int y,
                                     const char *text,
                                     int cols,
                                     lvds_color_t fg,
                                     lvds_color_t bg,
                                     int font);
static void nc_visual_serial_selected_line(void);
static void nc_visual_serial_selected_file(void);
static size_t nc_visual_code_line(void);
static bool nc_visual_can_edit_code(void);
static const char *nc_visual_file_basename(const char *path);
static void nc_visual_fps_tick(uint32_t snapshot_us,
                               uint32_t draw_us,
                               uint32_t present_us,
                               uint32_t total_us);
static void nc_visual_draw_tool_glyph(int tip_x,
                                      int tip_y,
                                      int size,
                                      const nc_tool_t *tool,
                                      lvds_color_t bg,
                                      bool selected);

static nc_mode_t g_nc_visual_mode = NC_MODE_MANUAL;
static uint8_t g_nc_visual_selected_action = NC_FOOTER_ACTION_NONE;

static const char *nc_visual_tool_path(void)
{
    const char *path = nc_state_path(NC_MODE_TOOLS);

    return (path && path[0] && nc_state_tool_path_supported(path)) ? path : NC_TOOL_PATH;
}

static void nc_visual_set_mode(nc_mode_t mode)
{
    if (mode < 0 || mode >= NC_MODE_COUNT) {
        return;
    }
    g_nc_visual_mode = mode;
    nc_state_set_mode(mode);
    nc_state_save();
}

static void nc_visual_save_current_if_file(void)
{
    if (g_nc_visual_doc.dirty && nc_path_supported(g_nc_visual_doc.path)) {
        (void)nc_save_file(&g_nc_visual_doc, g_nc_visual_doc.path);
    }
    if (nc_path_supported(g_nc_visual_doc.path)) {
        nc_state_remember_path(g_nc_visual_mode, g_nc_visual_doc.path);
        nc_state_save();
    }
}

static void nc_visual_seed_demo(void)
{
    nc_document_init(&g_nc_visual_doc);
    nc_insert_line(&g_nc_visual_doc, 0, "G970 X-10 U120 Z-150 W30");
    nc_insert_line(&g_nc_visual_doc, 1, "G971 X80 Z125 E0");
    nc_insert_line(&g_nc_visual_doc, 2, "G972 C15");
    nc_insert_line(&g_nc_visual_doc, 3, "G973 P7");
    nc_insert_line(&g_nc_visual_doc, 4, "G71 U2 R1 X0.5 Z0.5 F120");
    nc_insert_line(&g_nc_visual_doc, 5, "\tG1 X50 Z0");
    nc_insert_line(&g_nc_visual_doc, 6, "\tG1 X25 Z-25");
    nc_insert_line(&g_nc_visual_doc, 7, "G80");
    g_nc_visual_doc.cursor_line = 4;
    nc_select_next_word(&g_nc_visual_doc);
    nc_select_next_word(&g_nc_visual_doc);
    strncpy(g_nc_visual_doc.path, "NC module bring-up", sizeof(g_nc_visual_doc.path) - 1);
    g_nc_visual_doc.dirty = false;
    strncpy(g_nc_visual_status, "EDIT: B/C line, D/* word", sizeof(g_nc_visual_status) - 1);
    g_nc_visual_dirty = true;
}

typedef struct {
    float x;
    float z;
} nc_preview_v2_t;

static float nc_visual_absf(float v)
{
    return v < 0.0f ? -v : v;
}

static int nc_visual_clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float nc_visual_directed_arc_sweep(float a0, float a1, bool cw)
{
    float sweep = a1 - a0;

    if (cw) {
        while (sweep >= 0.0f) {
            sweep -= 6.2831853f;
        }
    } else {
        while (sweep <= 0.0f) {
            sweep += 6.2831853f;
        }
    }
    return sweep;
}

static bool nc_visual_r_arc_center(float start_z,
                                   float start_x,
                                   float end_z,
                                   float end_x,
                                   float r,
                                   bool cw,
                                   nc_preview_v2_t *center)
{
    float sx = start_x * 0.5f;
    float ex = end_x * 0.5f;
    float dz = end_z - start_z;
    float dx = ex - sx;
    float chord = sqrtf(dz * dz + dx * dx);
    float abs_r = nc_visual_absf(r);
    float mid_z = (start_z + end_z) * 0.5f;
    float mid_x = (sx + ex) * 0.5f;
    float h;
    float nz;
    float nx;
    nc_preview_v2_t c1;
    nc_preview_v2_t c2;
    float s1;
    float s2;

    if (!center || chord < 0.0001f || abs_r < chord * 0.5f) {
        return false;
    }

    h = sqrtf((abs_r * abs_r) - ((chord * 0.5f) * (chord * 0.5f)));
    nz = -dx / chord;
    nx = dz / chord;
    c1.z = mid_z + nz * h;
    c1.x = (mid_x + nx * h) * 2.0f;
    c2.z = mid_z - nz * h;
    c2.x = (mid_x - nx * h) * 2.0f;

    s1 = nc_visual_directed_arc_sweep(atan2f(sx - c1.x * 0.5f, start_z - c1.z),
                                      atan2f(ex - c1.x * 0.5f, end_z - c1.z),
                                      cw);
    s2 = nc_visual_directed_arc_sweep(atan2f(sx - c2.x * 0.5f, start_z - c2.z),
                                      atan2f(ex - c2.x * 0.5f, end_z - c2.z),
                                      cw);
    if (r >= 0.0f) {
        *center = nc_visual_absf(s1) <= nc_visual_absf(s2) ? c1 : c2;
    } else {
        *center = nc_visual_absf(s1) > nc_visual_absf(s2) ? c1 : c2;
    }
    return true;
}

static int nc_visual_preview_z(const nc_preview_info_t *p, int z0_x, int stock_w, float z)
{
    if (!p || p->stock_visible_z <= 0.0f) {
        return z0_x;
    }
    return z0_x + (int)((z / p->stock_visible_z) * (float)stock_w);
}

static int nc_visual_preview_x(const nc_preview_info_t *p, int stock_top, int stock_h, float x)
{
    if (!p || p->stock_x <= 0.0f) {
        return stock_top;
    }
    return stock_top + (int)(((x * 0.5f) / (p->stock_x * 0.5f)) * (float)stock_h);
}

static void nc_visual_draw_preview_tool_panel(int x,
                                              int y,
                                              int w,
                                              int h,
                                              const nc_tool_t *tool)
{
    int panel_w = 112;
    int panel_h = 48;
    int panel_x = x + w - panel_w - 10;
    int panel_y = y + 8;
    char buf[32];

    if (!tool || !tool->valid) {
        return;
    }

    lvds_draw_fill_rect(panel_x, panel_y, panel_w, panel_h, NC_VISUAL_PREVIEW_BG);
    lvds_draw_line(panel_x + 8, panel_y + 26, panel_x + 48, panel_y + 26, NC_VISUAL_DIM);
    lvds_draw_line(panel_x + 28, panel_y + 8, panel_x + 28, panel_y + 42, NC_VISUAL_DIM);
    nc_visual_draw_tool_glyph(panel_x + 28, panel_y + 26, 20, tool, NC_VISUAL_PREVIEW_BG, false);
    snprintf(buf, sizeof(buf), "T%d O%d", tool->t, tool->orient);
    nc_visual_draw_text_clip(panel_x + 54, panel_y + 10, buf, 8, NC_VISUAL_TEXT, NC_VISUAL_PREVIEW_BG, LVDS_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "R %.2g", (double)tool->r);
    nc_visual_draw_text_clip(panel_x + 54, panel_y + 28, buf, 8, NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_NORMAL);
}

static void nc_visual_draw_sim_code_pane(const nc_document_t *doc, int x, int y, int w)
{
    size_t line = nc_visual_code_line();
    size_t first;
    int row;

    if (!doc || doc->line_count == 0) {
        nc_visual_draw_text_clip(x, y, "No SIM file", w / NC_VISUAL_CHAR_W, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
        return;
    }
    if (line >= doc->line_count) {
        line = 0;
    }
    first = line > 1u ? line - 1u : 0u;
    if (first + 3u > doc->line_count && doc->line_count > 3u) {
        first = doc->line_count - 3u;
    }

    for (row = 0; row < 3 && first + (size_t)row < doc->line_count; row++) {
        size_t idx = first + (size_t)row;
        bool selected = idx == line;
        char buf[96];

        if (selected) {
            lvds_draw_fill_rect(x - 4, y + row * 24 - 4, w + 8, 22, NC_VISUAL_SELECT);
        }
        snprintf(buf, sizeof(buf), "%3lu  %.44s", (unsigned long)(idx + 1u), doc->lines[idx].text);
        nc_visual_draw_text_clip(x,
                                 y + row * 24,
                                 buf,
                                 w / NC_VISUAL_CHAR_W,
                                 selected ? NC_VISUAL_TEXT : NC_VISUAL_DIM,
                                 selected ? NC_VISUAL_SELECT : NC_VISUAL_BG,
                                 LVDS_FONT_NORMAL);
    }
}

static int nc_visual_tool_tip_digit(int orient)
{
    int digits[4];
    int n = 0;
    int tmp = orient;

    if (orient >= 1 && orient <= 9 && orient != 5) {
        return orient;
    }
    if (orient > 9) {
        while (tmp > 0 && n < (int)(sizeof(digits) / sizeof(digits[0]))) {
            digits[n++] = tmp % 10;
            tmp /= 10;
        }
        if (tmp > 0 || n <= 0) {
            return 3;
        }
        if (n == 3 || n == 4) {
            return digits[n - 2];
        }
        return digits[n - 1];
    }
    return 3;
}

static int nc_visual_tool_orient_digits(int orient, int *digits, int max_digits)
{
    int tmp[4];
    int n = 0;
    int i;

    while (orient > 0 && n < (int)(sizeof(tmp) / sizeof(tmp[0]))) {
        tmp[n++] = orient % 10;
        orient /= 10;
    }
    if (orient > 0 || n <= 0 || n > max_digits) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        digits[i] = tmp[n - 1 - i];
    }
    return n;
}

static bool nc_visual_tool_keypad_point(int digit, int ox, int oy, int step, int *x, int *y)
{
    static const int kx[10] = {0, -1, 0, 1, -1, 0, 1, -1, 0, 1};
    static const int ky[10] = {0,  1, 1, 1,  0, 0, 0, -1,-1,-1};

    if (digit < 1 || digit > 9) {
        return false;
    }
    if (x) *x = ox + (kx[digit] * step);
    if (y) *y = oy + (ky[digit] * step);
    return true;
}

static void nc_visual_tool_edges(int orient, bool *left, bool *top, bool *right, bool *bottom)
{
    int o = nc_visual_tool_tip_digit(orient);

    if (left) *left = (o == 1 || o == 4 || o == 7 || o == 2 || o == 5 || o == 8);
    if (top) *top = (o == 7 || o == 8 || o == 9 || o == 4 || o == 5 || o == 6);
    if (right) *right = (o == 3 || o == 6 || o == 9 || o == 2 || o == 5 || o == 8);
    if (bottom) *bottom = (o == 1 || o == 2 || o == 3 || o == 4 || o == 5 || o == 6);
}

static void nc_visual_fill_triangle(int x1, int y1,
                                    int x2, int y2,
                                    int x3, int y3,
                                    lvds_color_t color)
{
    int min_y = y1;
    int max_y = y1;
    int y;

    if (y2 < min_y) min_y = y2;
    if (y3 < min_y) min_y = y3;
    if (y2 > max_y) max_y = y2;
    if (y3 > max_y) max_y = y3;

    if (min_y < 0) min_y = 0;
    if (max_y >= LVDS_HSTX_HEIGHT) max_y = LVDS_HSTX_HEIGHT - 1;

    for (y = min_y; y <= max_y; y++) {
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
            if (xa < 0) xa = 0;
            if (xb >= LVDS_HSTX_WIDTH) xb = LVDS_HSTX_WIDTH - 1;
            if (xb >= xa) {
                lvds_draw_fill_rect(xa, y, xb - xa + 1, 1, color);
            }
        }
    }
}

static void nc_visual_tool_marker_line(int x1,
                                       int y1,
                                       int x2,
                                       int y2,
                                       lvds_color_t color,
                                       int thick)
{
    x1 = nc_visual_clampi(x1, 0, LVDS_HSTX_WIDTH - 1);
    y1 = nc_visual_clampi(y1, 0, LVDS_HSTX_HEIGHT - 1);
    x2 = nc_visual_clampi(x2, 0, LVDS_HSTX_WIDTH - 1);
    y2 = nc_visual_clampi(y2, 0, LVDS_HSTX_HEIGHT - 1);

    if (thick > 1) {
        lvds_draw_line_w(x1, y1, x2, y2, color, thick);
    } else {
        lvds_draw_line(x1, y1, x2, y2, color);
    }
}

static int nc_visual_tool_polygon_points(int tip_x,
                                         int tip_y,
                                         int orient,
                                         int size,
                                         int *px,
                                         int *py)
{
    int digits[4];
    int tip_grid_x;
    int tip_grid_y;
    int step = nc_visual_clampi(size / 2, 6, 56);
    int n = nc_visual_tool_orient_digits(orient, digits, 4);
    int i;

    if (n < 3) {
        return 0;
    }
    if (n == 4) {
        int cut_x;
        int cut_y;
        int z_x;
        int z_y;
        if (!nc_visual_tool_keypad_point(digits[1], 0, 0, step, &cut_x, &cut_y) ||
            !nc_visual_tool_keypad_point(digits[2], 0, 0, step, &z_x, &z_y)) {
            return 0;
        }
        tip_grid_x = cut_x;
        tip_grid_y = z_y;
    } else {
        int tip_digit = nc_visual_tool_tip_digit(orient);
        if (!nc_visual_tool_keypad_point(tip_digit, 0, 0, step, &tip_grid_x, &tip_grid_y)) {
            return 0;
        }
    }

    for (i = 0; i < n; i++) {
        int gx;
        int gy;
        if (!nc_visual_tool_keypad_point(digits[i], 0, 0, step, &gx, &gy)) {
            return 0;
        }
        px[i] = tip_x + gx - tip_grid_x;
        py[i] = tip_y + gy - tip_grid_y;
    }
    return n;
}

static void nc_visual_draw_tool_polygon(int tip_x,
                                        int tip_y,
                                        int orient,
                                        int size,
                                        int thick,
                                        lvds_color_t fill,
                                        lvds_color_t edge,
                                        lvds_color_t mount)
{
    int px[4];
    int py[4];
    int n = nc_visual_tool_polygon_points(tip_x, tip_y, orient, size, px, py);
    int i;

    if (n < 3) {
        return;
    }
    for (i = 1; i + 1 < n; i++) {
        nc_visual_fill_triangle(px[0], py[0], px[i], py[i], px[i + 1], py[i + 1], fill);
    }
    if (n == 3) {
        nc_visual_tool_marker_line(px[1], py[1], px[0], py[0], edge, thick);
        nc_visual_tool_marker_line(px[1], py[1], px[2], py[2], edge, thick);
        nc_visual_tool_marker_line(px[0], py[0], px[2], py[2], mount, 1);
    } else {
        nc_visual_tool_marker_line(px[1], py[1], px[2], py[2], edge, thick > 1 ? thick : 2);
        nc_visual_tool_marker_line(px[3], py[3], px[0], py[0], mount, 1);
    }
}

static void nc_visual_draw_tool_glyph(int tip_x,
                                      int tip_y,
                                      int size,
                                      const nc_tool_t *tool,
                                      lvds_color_t bg,
                                      bool selected)
{
    int rr;
    int corner;
    int sx;
    int sy;
    bool left;
    bool top;
    bool right;
    bool bottom;
    lvds_color_t edge = selected ? NC_VISUAL_LINE_NO_SELECTED : NC_VISUAL_TOOL_MARK;
    lvds_color_t fill = NC_VISUAL_TOOL_FILL;

    if (!tool || !tool->valid) {
        return;
    }

    rr = tool->r > 0.0f ? (int)(tool->r * 8.0f) : 2;
    rr = nc_visual_clampi(rr, 1, size / 3);
    corner = nc_visual_tool_tip_digit(tool->orient);
    switch (corner) {
    case 7:
        sx = tip_x;
        sy = tip_y;
        break;
    case 9:
        sx = tip_x - size;
        sy = tip_y;
        break;
    case 1:
        sx = tip_x;
        sy = tip_y - size;
        break;
    case 3:
    default:
        sx = tip_x - size;
        sy = tip_y - size;
        break;
    }

    if (tool->orient == 0) {
        lvds_draw_fill_ellipse(tip_x, tip_y, 4, 4, fill);
        lvds_draw_ellipse(tip_x, tip_y, 4, 4, edge);
    } else if (tool->orient == 5) {
        lvds_draw_fill_rect(tip_x - size / 2, tip_y - size / 2, size, size, fill);
        lvds_draw_rect(tip_x - size / 2, tip_y - size / 2, size, size, edge);
        lvds_draw_line(tip_x - size / 2, tip_y, tip_x + size / 2, tip_y, edge);
        lvds_draw_line(tip_x, tip_y - size / 2, tip_x, tip_y + size / 2, edge);
    } else if (tool->orient > 9) {
        nc_visual_draw_tool_polygon(tip_x,
                                    tip_y,
                                    tool->orient,
                                    size,
                                    selected ? 2 : 1,
                                    fill,
                                    edge,
                                    NC_VISUAL_DIM);
    } else {
        nc_visual_tool_edges(tool->orient, &left, &top, &right, &bottom);
        lvds_draw_fill_rect(sx + 1, sy + 1, size - 1, size - 1, fill);
        if (left) lvds_draw_line(sx, sy, sx, sy + size, edge);
        if (top) lvds_draw_line(sx, sy, sx + size, sy, edge);
        if (right) lvds_draw_line(sx + size, sy, sx + size, sy + size, edge);
        if (bottom) lvds_draw_line(sx, sy + size, sx + size, sy + size, edge);
        lvds_draw_fill_ellipse(tip_x, tip_y, rr, rr, edge);
        lvds_draw_rect(tip_x - rr, tip_y - rr, rr * 2, rr * 2, bg);
    }
    lvds_draw_fill_ellipse(tip_x, tip_y, 2, 2, edge);
}

static void nc_visual_draw_tool_glyph_centered(int x,
                                               int y,
                                               int box_size,
                                               int marker_size,
                                               const nc_tool_t *tool,
                                               lvds_color_t bg,
                                               bool selected)
{
    int sx;
    int sy;
    int tip_x;
    int tip_y;
    int corner;

    if (!tool || !tool->valid) {
        return;
    }

    sx = x + ((box_size - marker_size) / 2);
    sy = y + ((box_size - marker_size) / 2);
    tip_x = x + (box_size / 2);
    tip_y = y + (box_size / 2);

    if (tool->orient > 9) {
        int px[4];
        int py[4];
        int n = nc_visual_tool_polygon_points(tip_x, tip_y, tool->orient, marker_size, px, py);
        if (n >= 3) {
            int min_x = px[0];
            int max_x = px[0];
            int min_y = py[0];
            int max_y = py[0];
            int i;
            for (i = 1; i < n; i++) {
                if (px[i] < min_x) min_x = px[i];
                if (px[i] > max_x) max_x = px[i];
                if (py[i] < min_y) min_y = py[i];
                if (py[i] > max_y) max_y = py[i];
            }
            tip_x += (x + (box_size / 2)) - ((min_x + max_x) / 2);
            tip_y += (y + (box_size / 2)) - ((min_y + max_y) / 2);
        }
    } else if (tool->orient > 0 && tool->orient <= 9 && tool->orient != 5) {
        corner = nc_visual_tool_tip_digit(tool->orient);
        switch (corner) {
        case 7:
            tip_x = sx;
            tip_y = sy;
            break;
        case 9:
            tip_x = sx + marker_size;
            tip_y = sy;
            break;
        case 1:
            tip_x = sx;
            tip_y = sy + marker_size;
            break;
        case 3:
        default:
            tip_x = sx + marker_size;
            tip_y = sy + marker_size;
            break;
        }
    }

    nc_visual_draw_tool_glyph(tip_x, tip_y, marker_size, tool, bg, selected);
}

static bool nc_visual_selected_tool_word(char *letter, int *line_index)
{
    nc_word_t word;

    if (letter) {
        *letter = '\0';
    }
    if (line_index) {
        *line_index = -1;
    }
    if (g_nc_visual_mode != NC_MODE_TOOLS ||
        nc_get_selected_word(&g_nc_visual_doc, &word) != NC_OK ||
        g_nc_visual_doc.cursor_line >= g_nc_visual_doc.line_count ||
        !nc_tool_line_is_tool(g_nc_visual_doc.lines[g_nc_visual_doc.cursor_line].text)) {
        return false;
    }
    if (letter) {
        *letter = word.letter;
    }
    if (line_index) {
        *line_index = (int)g_nc_visual_doc.cursor_line;
    }
    return true;
}

static void nc_visual_draw_tool_cell(const char *line,
                                     char letter,
                                     int x,
                                     int y,
                                     int cols,
                                     lvds_color_t fg,
                                     lvds_color_t bg,
                                     bool active)
{
    char value[24];
    lvds_color_t cell_fg = active ? NC_VISUAL_WORD_FG : fg;
    lvds_color_t cell_bg = active ? NC_VISUAL_WORD_BG : bg;

    if (!nc_tool_field_text(line, letter, value, sizeof(value))) {
        value[0] = '-';
        value[1] = '\0';
    }
    if (active) {
        lvds_draw_fill_rect(x - 2, y - 2, (cols * NC_VISUAL_CHAR_W) + 4, 20, cell_bg);
    }
    nc_visual_draw_text_clip(x, y, value, cols, cell_fg, cell_bg, LVDS_FONT_NORMAL);
}

static void nc_visual_draw_tool_param(const char *line,
                                      char letter,
                                      const char *label,
                                      int x,
                                      int y,
                                      int value_cols,
                                      bool active)
{
    char value[24];
    char buf[24];
    lvds_color_t value_fg = active ? NC_VISUAL_WORD_FG : NC_VISUAL_TEXT;
    lvds_color_t value_bg = active ? NC_VISUAL_WORD_BG : NC_VISUAL_BG;

    snprintf(buf, sizeof(buf), "%-7s", label ? label : "");
    nc_visual_draw_text_clip(x, y, buf, 7, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    if (!nc_tool_field_text(line, letter, value, sizeof(value))) {
        value[0] = '-';
        value[1] = '\0';
    }
    if (active) {
        lvds_draw_fill_rect(x + 68, y - 2, (value_cols * NC_VISUAL_CHAR_W) + 4, 20, value_bg);
    }
    nc_visual_draw_text_clip(x + 70, y, value, value_cols, value_fg, value_bg, LVDS_FONT_NORMAL);
}

static int nc_visual_find_tool_line(int selected_tool, int *selected_line)
{
    int count = 0;
    size_t i;

    if (selected_line) {
        *selected_line = -1;
    }
    for (i = 0; i < g_nc_visual_doc.line_count; i++) {
        if (nc_tool_line_is_tool(g_nc_visual_doc.lines[i].text)) {
            if (count == selected_tool && selected_line) {
                *selected_line = (int)i;
            }
            count++;
        }
    }
    return count;
}

static int nc_visual_selected_tool_index(void)
{
    int count = 0;
    size_t i;

    for (i = 0; i < g_nc_visual_doc.line_count; i++) {
        if (nc_tool_line_is_tool(g_nc_visual_doc.lines[i].text)) {
            if (i >= g_nc_visual_doc.cursor_line) {
                return count;
            }
            count++;
        }
    }
    return count > 0 ? count - 1 : 0;
}

#if NC_PREVIEW_DIN_STYLE
static void nc_visual_draw_dashdot_line(int x0,
                                        int y0,
                                        int x1,
                                        int y1,
                                        lvds_color_t color)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int len2 = dx * dx + dy * dy;
    float len;
    int pos = 0;
    static const uint8_t pattern[] = {18, 5, 3, 5};
    int pat = 0;

    if (len2 <= 0) {
        return;
    }
    len = sqrtf((float)len2);
    while (pos < (int)len) {
        int seg = pattern[pat & 3];
        int a = pos;
        int b = pos + seg;

        if (b > (int)len) {
            b = (int)len;
        }
        if ((pat & 1) == 0 && b > a) {
            int xa = x0 + (int)((float)dx * ((float)a / len));
            int ya = y0 + (int)((float)dy * ((float)a / len));
            int xb = x0 + (int)((float)dx * ((float)b / len));
            int yb = y0 + (int)((float)dy * ((float)b / len));
            lvds_draw_line(xa, ya, xb, yb, color);
        }
        pos += seg;
        pat++;
    }
}

static void nc_visual_draw_centerline(int x0, int y0, int x1, int y1)
{
    nc_visual_draw_dashdot_line(x0, y0, x1, y1, NC_VISUAL_DIM);
}

static void nc_visual_draw_origin_marker(int x, int y)
{
    lvds_draw_ellipse(x, y, 11, 11, NC_VISUAL_TEXT);
    lvds_draw_ellipse(x, y, 6, 6, NC_VISUAL_TEXT);
    lvds_draw_line(x - 15, y, x - 8, y, NC_VISUAL_TEXT);
    lvds_draw_line(x + 8, y, x + 15, y, NC_VISUAL_TEXT);
    lvds_draw_line(x, y - 15, x, y - 8, NC_VISUAL_TEXT);
    lvds_draw_line(x, y + 8, x, y + 15, NC_VISUAL_TEXT);
}

static void nc_visual_draw_chuck_hatching(int x, int y, int w, int h, lvds_color_t color)
{
    int s;

    if (w <= 0 || h <= 0) {
        return;
    }
    for (s = -h; s < w; s += 8) {
        int x0 = s > 0 ? s : 0;
        int y0 = s > 0 ? 0 : -s;
        int x1 = (s + h) < w ? s + h : w;
        int y1 = (s + h) < w ? h : w - s;

        y1 = nc_visual_clampi(y1, 0, h);
        lvds_draw_line(x + x0, y + y0, x + x1, y + y1, color);
    }
    for (s = 0; s < w + h; s += 8) {
        int x0 = s < w ? s : w;
        int y0 = s < w ? 0 : s - w;
        int x1 = s < h ? 0 : s - h;
        int y1 = s < h ? s : h;

        x1 = nc_visual_clampi(x1, 0, w);
        y0 = nc_visual_clampi(y0, 0, h);
        lvds_draw_line(x + x0, y + y0, x + x1, y + y1, color);
    }
}

static void nc_visual_draw_arrowhead(int x, int y, int dir_x, int dir_y, lvds_color_t color)
{
    int px = -dir_y;
    int py = dir_x;

    lvds_draw_line(x, y, x - dir_x * 7 + px * 3, y - dir_y * 7 + py * 3, color);
    lvds_draw_line(x, y, x - dir_x * 7 - px * 3, y - dir_y * 7 - py * 3, color);
}

static void nc_visual_draw_diameter_dimension(int x,
                                              int y0,
                                              int y1,
                                              const char *label)
{
    lvds_draw_line(x, y0, x, y1, NC_VISUAL_DIM);
    nc_visual_draw_arrowhead(x, y1, 0, 1, NC_VISUAL_DIM);
    if (label && label[0]) {
        lvds_draw_text(x + 6,
                       y1 - 8,
                       label,
                       NC_VISUAL_DIM,
                       NC_VISUAL_PREVIEW_BG,
                       LVDS_FONT_NORMAL);
    }
}

static void nc_visual_draw_z_point_dimension(int start_x,
                                             int point_x,
                                             int zero_x,
                                             int center_y,
                                             int point_y,
                                             const char *label)
{
    int dim_y = center_y - 15;
    int ext_top = center_y - 20;
    int ext_bottom = point_y + 3;
    int text_x;

    if (ext_bottom < ext_top) {
        int t = ext_bottom;
        ext_bottom = ext_top;
        ext_top = t;
    }
    lvds_draw_line(point_x, ext_top, point_x, ext_bottom, NC_VISUAL_DIM);
    lvds_draw_line(start_x, dim_y, point_x, dim_y, NC_VISUAL_DIM);
    if (start_x == zero_x) {
        lvds_draw_fill_rect(start_x - 1, dim_y - 1, 3, 3, NC_VISUAL_DIM);
    } else {
        nc_visual_draw_arrowhead(point_x, dim_y, -1, 0, NC_VISUAL_DIM);
    }
    if (!label || !label[0]) {
        return;
    }
    text_x = point_x - lvds_draw_text_width(label, LVDS_FONT_SMALL) / 2;
    lvds_draw_text(text_x,
                   center_y - 26,
                   label,
                   NC_VISUAL_DIM,
                   NC_VISUAL_PREVIEW_BG,
                   LVDS_FONT_SMALL);
}

static void nc_visual_draw_x_point_dimension(int dim_x,
                                             int start_y,
                                             int point_y,
                                             int zero_y,
                                             int point_x,
                                             const char *label)
{
    int text_y;

    lvds_draw_line(dim_x, start_y, dim_x, point_y, NC_VISUAL_DIM);
    lvds_draw_line(point_x + 3, point_y, dim_x, point_y, NC_VISUAL_DIM);
    if (start_y == zero_y) {
        lvds_draw_fill_rect(dim_x - 1, start_y - 1, 3, 3, NC_VISUAL_DIM);
    }
    nc_visual_draw_arrowhead(dim_x, point_y, 0, 1, NC_VISUAL_DIM);
    if (!label || !label[0]) {
        return;
    }
    text_y = point_y - 6;
    lvds_draw_text(dim_x + 4,
                   text_y,
                   label,
                   NC_VISUAL_DIM,
                   NC_VISUAL_PREVIEW_BG,
                   LVDS_FONT_SMALL);
}

static void nc_visual_draw_contour_point_marker(int x, int y, bool filled)
{
    if (filled) {
        lvds_draw_fill_ellipse(x, y, 3, 3, NC_VISUAL_TEXT);
    } else {
        lvds_draw_ellipse(x, y, 5, 5, NC_VISUAL_TEXT);
    }
}

#endif

static void nc_visual_draw_chuck(const nc_preview_info_t *preview,
                                 int stock_left,
                                 int stock_top,
                                 int stock_w,
                                 int stock_h)
{
    float c = preview && preview->chuck_c > 0.0f ? preview->chuck_c : 15.0f;
    int c_w = preview && preview->stock_visible_z > 0.0f ?
              (int)((c / preview->stock_visible_z) * (float)stock_w + 0.5f) :
              24;
    int c_h = preview && preview->stock_x > 0.0f ?
              (int)((c / preview->stock_x) * (float)stock_h + 0.5f) :
              24;
    int block_x = 0;
    int block_w;
    int block_y;
    lvds_color_t fill = NC_VISUAL_FOOTER_BUTTON;
    lvds_color_t ink = NC_VISUAL_LINE_NO_SELECTED;

    if (c_w < 8) c_w = 8;
    if (c_h < 8) c_h = 8;
    block_w = stock_left + c_w;
    if (block_w > stock_left + stock_w) {
        block_w = stock_left + stock_w;
    }
    block_y = stock_top + stock_h - (c_h / 2);
    if (block_y < 44) {
        block_y = 44;
    }
    if (block_y + c_h > NC_FOOTER_Y) {
        c_h = NC_FOOTER_Y - block_y;
    }
    if (block_w <= 0 || c_h <= 0) {
        return;
    }

    lvds_draw_fill_rect(block_x, block_y, block_w, c_h, fill);
    lvds_draw_rect(block_x, block_y, block_w, c_h, ink);
#if NC_PREVIEW_DIN_STYLE
    nc_visual_draw_chuck_hatching(block_x, block_y, block_w, c_h, ink);
#endif
}

static void nc_visual_draw_chuck_relief(const nc_preview_info_t *preview,
                                        int stock_left,
                                        int stock_top,
                                        int stock_h)
{
    float c = preview && preview->chuck_c > 0.0f ? preview->chuck_c : 15.0f;
    int c_h = preview && preview->stock_x > 0.0f ?
              (int)((c / preview->stock_x) * (float)stock_h + 0.5f) :
              24;
    int r = c_h / 8;

    if (r < 2) {
        r = 2;
    }
    lvds_draw_fill_ellipse(stock_left,
                           stock_top + stock_h,
                           r,
                           r,
                           NC_VISUAL_PREVIEW_BG);
    lvds_draw_ellipse(stock_left,
                      stock_top + stock_h,
                      r,
                      r,
                      NC_VISUAL_LINE_NO_SELECTED);
}

#if NC_PREVIEW_DIN_STYLE
static void nc_visual_draw_din_layer(const nc_preview_info_t *preview,
                                     int stock_left,
                                     int stock_top,
                                     int stock_w,
                                     int stock_h,
                                     int z0_x)
{
    char label[16];
    int stock_right;
    int stock_bottom;
    int dim_x;
    int id_y;

    if (!preview) {
        return;
    }

    stock_right = stock_left + stock_w;
    stock_bottom = stock_top + stock_h;
    dim_x = stock_right + 14;
    if (dim_x > LVDS_HSTX_WIDTH - 24) {
        dim_x = stock_right - 18;
    }

    nc_visual_draw_centerline(stock_left - 34,
                              stock_top,
                              stock_right + 18,
                              stock_top);
    nc_visual_draw_centerline(z0_x,
                              stock_top - 26,
                              z0_x,
                              stock_bottom + 20);
    nc_visual_draw_origin_marker(z0_x, stock_top);

    /*lvds_draw_line(z0_x, stock_top, z0_x + 24, stock_top - 22, NC_VISUAL_DIM);
    lvds_draw_text(z0_x + 27,
                   stock_top - 29,
                   "+Z",
                   NC_VISUAL_DIM,
                   NC_VISUAL_PREVIEW_BG,
                   LVDS_FONT_NORMAL);
    lvds_draw_line(z0_x, stock_top, z0_x - 18, stock_top - 24, NC_VISUAL_DIM);
    lvds_draw_text(z0_x - 35,
                   stock_top - 39,
                   "+X",
                   NC_VISUAL_DIM,
                   NC_VISUAL_PREVIEW_BG,
                   LVDS_FONT_NORMAL);*/

    if (g_nc_visual_mode != NC_MODE_SIM && g_nc_visual_mode != NC_MODE_RUN) {
        snprintf(label, sizeof(label), "%.0f", preview->stock_x);
        nc_visual_draw_diameter_dimension(dim_x,
                                          stock_top,
                                          stock_bottom,
                                          label);
    }
    if (g_nc_visual_mode != NC_MODE_SIM && g_nc_visual_mode != NC_MODE_RUN &&
        preview->stock_i > 0.0f && preview->stock_i < preview->stock_x) {
        id_y = nc_visual_preview_x(preview, stock_top, stock_h, preview->stock_i);
        nc_visual_draw_centerline(stock_left - 18, id_y, stock_right + 8, id_y);
        snprintf(label, sizeof(label), "%.0f", preview->stock_i);
        nc_visual_draw_diameter_dimension(dim_x - 18,
                                          stock_top,
                                          id_y,
                                          label);
    }
}
#endif

static void nc_visual_draw_sim_contour_points(const nc_document_t *doc,
                                              const nc_preview_info_t *preview,
                                              int z0_x,
                                              int stock_left,
                                              int stock_right,
                                              int stock_w,
                                              int stock_top,
                                              int stock_h)
{
#if NC_PREVIEW_DIN_POINT_MARKERS
    size_t i;
    bool in_region = false;
    float x = preview ? preview->stock_x : 0.0f;
    float z = 0.0f;
    int prev_z_px = z0_x;
    int prev_x_py = stock_top;
    int x_dim = stock_right + 15;
    char label[16];

    if (!doc || !preview ||
        (g_nc_visual_mode != NC_MODE_SIM && g_nc_visual_mode != NC_MODE_RUN)) {
        return;
    }

    for (i = 0; i < doc->line_count; i++) {
        const char *line = doc->lines[i].text;
        g7x_contour_cmd_t cmd;
        bool has_x;
        bool has_z;

        if (g7x_cycle_from_line(line) != G7X_CYCLE_NONE) {
            in_region = true;
            continue;
        }
        if (!in_region) {
            continue;
        }
        cmd = g7x_contour_cmd_from_line(line);
        if (cmd == G7X_CONTOUR_END) {
            break;
        }
        if (cmd == G7X_CONTOUR_NONE || cmd == G7X_CONTOUR_RAPID) {
            continue;
        }

        has_x = nc_sim_line_word_float(line, 'X', &x);
        has_z = nc_sim_line_word_float(line, 'Z', &z);
        if (has_x || has_z) {
            int px = nc_visual_preview_z(preview, z0_x, stock_w, z);
            int py = nc_visual_preview_x(preview, stock_top, stock_h, x);
            float feature = 0.0f;
            if (g_nc_visual_mode == NC_MODE_SIM ||
                (g_nc_visual_mode == NC_MODE_RUN &&
                 !nc_run_active() &&
                 !nc_run_hold() &&
                 !cnc_get_exec_state(EXEC_RUN | EXEC_HOLD))) {
                nc_visual_draw_contour_point_marker(px, py, i == doc->cursor_line);
            }
            snprintf(label, sizeof(label), "%.0f", x);
            nc_visual_draw_x_point_dimension(x_dim, prev_x_py, py, stock_top, px, label);
            prev_x_py = py;
            snprintf(label, sizeof(label), "%.0f", z);
            nc_visual_draw_z_point_dimension(prev_z_px, px, z0_x, stock_top, py, label);
            prev_z_px = px;
            if (nc_sim_line_word_float(line, 'R', &feature) && feature > 0.0001f) {
                snprintf(label, sizeof(label), "R%.0f", feature);
                lvds_draw_text(px + 4, py - 16, label, NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_SMALL);
            } else if (nc_sim_line_word_float(line, 'C', &feature) && feature > 0.0001f) {
                snprintf(label, sizeof(label), "C%.0f", feature);
                lvds_draw_text(px + 4, py - 16, label, NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_SMALL);
            }
        }
    }
    snprintf(label, sizeof(label), "%.0f", -preview->stock_visible_z);
    nc_visual_draw_z_point_dimension(prev_z_px,
                                     stock_left,
                                     z0_x,
                                     stock_top,
                                     stock_top,
                                     label);
#else
    (void)doc;
    (void)preview;
    (void)z0_x;
    (void)stock_left;
    (void)stock_right;
    (void)stock_w;
    (void)stock_top;
    (void)stock_h;
#endif
}

static bool nc_visual_draw_explicit_arc(const nc_preview_info_t *preview,
                                        int z0_x,
                                        int stock_w,
                                        int stock_top,
                                        int stock_h,
                                        float start_x,
                                        float start_z,
                                        float end_x,
                                        float end_z,
                                        float r,
                                        bool cw,
                                        lvds_color_t color,
                                        int width)
{
    nc_preview_v2_t center;
    float a0;
    float sweep;
    float abs_r = nc_visual_absf(r);
    float screen_r;
    int steps;
    int last_px;
    int last_py;
    int i;

    if (!preview || !nc_visual_r_arc_center(start_z, start_x, end_z, end_x, r, cw, &center)) {
        return false;
    }

    a0 = atan2f((start_x * 0.5f) - (center.x * 0.5f), start_z - center.z);
    sweep = nc_visual_directed_arc_sweep(a0,
                                         atan2f((end_x * 0.5f) - (center.x * 0.5f),
                                                end_z - center.z),
                                         cw);
    screen_r = abs_r * (float)stock_w / (preview->stock_z > 0.0001f ? preview->stock_z : 1.0f);
    steps = nc_visual_clampi((int)(nc_visual_absf(sweep) * screen_r * 0.35f) + 8,
                             10,
                             NC_PREVIEW_ARC_MAX_STEPS);
    last_px = nc_visual_preview_z(preview, z0_x, stock_w, start_z);
    last_py = nc_visual_preview_x(preview, stock_top, stock_h, start_x);
    for (i = 1; i <= steps; i++) {
        float a = a0 + sweep * ((float)i / (float)steps);
        float z = center.z + cosf(a) * abs_r;
        float x = ((center.x * 0.5f) + sinf(a) * abs_r) * 2.0f;
        int px = nc_visual_preview_z(preview, z0_x, stock_w, z);
        int py = nc_visual_preview_x(preview, stock_top, stock_h, x);
        lvds_draw_line_w(last_px, last_py, px, py, color, width);
        last_px = px;
        last_py = py;
    }
    return true;
}

static bool nc_visual_draw_center_arc(const nc_preview_info_t *preview,
                                      int z0_x,
                                      int stock_w,
                                      int stock_top,
                                      int stock_h,
                                      float start_x,
                                      float start_z,
                                      float end_x,
                                      float end_z,
                                      float i_off,
                                      float k_off,
                                      bool cw,
                                      lvds_color_t color,
                                      int width)
{
    float center_z = start_z + k_off;
    float center_xr = (start_x * 0.5f) + i_off;
    float start_xr = start_x * 0.5f;
    float end_xr = end_x * 0.5f;
    float dz = start_z - center_z;
    float dx = start_xr - center_xr;
    float radius = sqrtf(dz * dz + dx * dx);
    float screen_r;
    float a0;
    float sweep;
    int steps;
    int last_px;
    int last_py;
    int n;

    if (!preview || radius < 0.0001f) {
        return false;
    }

    a0 = atan2f(start_xr - center_xr, start_z - center_z);
    sweep = nc_visual_directed_arc_sweep(a0,
                                         atan2f(end_xr - center_xr,
                                                end_z - center_z),
                                         cw);
    screen_r = radius * (float)stock_w / (preview->stock_z > 0.0001f ? preview->stock_z : 1.0f);
    steps = nc_visual_clampi((int)(nc_visual_absf(sweep) * screen_r * 0.35f) + 8,
                             10,
                             NC_PREVIEW_ARC_MAX_STEPS);
    last_px = nc_visual_preview_z(preview, z0_x, stock_w, start_z);
    last_py = nc_visual_preview_x(preview, stock_top, stock_h, start_x);
    for (n = 1; n <= steps; n++) {
        float a = a0 + sweep * ((float)n / (float)steps);
        float z = center_z + cosf(a) * radius;
        float x = (center_xr + sinf(a) * radius) * 2.0f;
        int px = nc_visual_preview_z(preview, z0_x, stock_w, z);
        int py = nc_visual_preview_x(preview, stock_top, stock_h, x);
        lvds_draw_line_w(last_px, last_py, px, py, color, width);
        last_px = px;
        last_py = py;
    }
    return true;
}

static void nc_visual_draw_dashed_segment(const nc_preview_info_t *preview,
                                          int z0_x,
                                          int stock_w,
                                          int stock_top,
                                          int stock_h,
                                          float z0,
                                          float x0,
                                          float z1,
                                          float x1,
                                          lvds_color_t color)
{
    float dz = z1 - z0;
    float dx = x1 - x0;
    int px0;
    int py0;
    int px1;
    int py1;
    float plen;
    int pieces;
    int p;

    if (!preview) {
        return;
    }

    px0 = nc_visual_preview_z(preview, z0_x, stock_w, z0);
    py0 = nc_visual_preview_x(preview, stock_top, stock_h, x0);
    px1 = nc_visual_preview_z(preview, z0_x, stock_w, z1);
    py1 = nc_visual_preview_x(preview, stock_top, stock_h, x1);
    plen = sqrtf((float)((px1 - px0) * (px1 - px0) + (py1 - py0) * (py1 - py0)));
    pieces = nc_visual_clampi((int)(plen / 8.0f), 1, 80);
    for (p = 0; p < pieces; p += 2) {
        float a = (float)p / (float)pieces;
        float b = (float)(p + 1) / (float)pieces;
        int xa;
        int ya;
        int xb;
        int yb;

        if (b > 1.0f) {
            b = 1.0f;
        }
        xa = nc_visual_preview_z(preview, z0_x, stock_w, z0 + dz * a);
        ya = nc_visual_preview_x(preview, stock_top, stock_h, x0 + dx * a);
        xb = nc_visual_preview_z(preview, z0_x, stock_w, z0 + dz * b);
        yb = nc_visual_preview_x(preview, stock_top, stock_h, x0 + dx * b);
        lvds_draw_line(xa, ya, xb, yb, color);
    }
}

static bool nc_visual_draw_emitted_motion_line(const nc_preview_info_t *preview,
                                               int z0_x,
                                               int stock_w,
                                               int stock_top,
                                               int stock_h,
                                               const char *line,
                                               nc_preview_segment_t segment,
                                               bool selected,
                                               float *last_x,
                                               float *last_z,
                                               bool *have_last)
{
    g7x_contour_cmd_t cmd;
    float x;
    float z;
    bool has_x;
    bool has_z;
    int x0;
    int y0;
    int x1;
    int y1;
    lvds_color_t color = NC_VISUAL_PREVIEW_CUT;
    int width = selected ? 3 : 1;

    if (!preview || !line || !last_x || !last_z || !have_last) {
        return false;
    }
    cmd = g7x_contour_cmd_from_line(line);
    if (cmd == G7X_CONTOUR_NONE || cmd == G7X_CONTOUR_END) {
        return false;
    }

    x = *last_x;
    z = *last_z;
    has_x = nc_sim_line_word_float(line, 'X', &x);
    has_z = nc_sim_line_word_float(line, 'Z', &z);
    if (!*have_last) {
        *last_x = has_x ? x : preview->stock_x;
        *last_z = has_z ? z : 0.0f;
        *have_last = true;
        return false;
    }
    if (!has_x && !has_z) {
        return false;
    }

    x0 = nc_visual_preview_z(preview, z0_x, stock_w, *last_z);
    y0 = nc_visual_preview_x(preview, stock_top, stock_h, *last_x);
    x1 = nc_visual_preview_z(preview, z0_x, stock_w, z);
    y1 = nc_visual_preview_x(preview, stock_top, stock_h, x);
    if (segment == NC_PREVIEW_SEG_ROUGH) {
        color = NC_VISUAL_PREVIEW_HATCH;
    } else if (segment == NC_PREVIEW_SEG_FINISH) {
        color = NC_VISUAL_TOOL_MARK;
    }

    if (cmd == G7X_CONTOUR_ARC_CW || cmd == G7X_CONTOUR_ARC_CCW) {
        float r = 0.0f;
        float i_off = 0.0f;
        float k_off = 0.0f;
        if ((nc_sim_line_word_float(line, 'I', &i_off) &&
             nc_sim_line_word_float(line, 'K', &k_off) &&
             nc_visual_draw_center_arc(preview,
                                       z0_x,
                                       stock_w,
                                       stock_top,
                                       stock_h,
                                       *last_x,
                                       *last_z,
                                       x,
                                       z,
                                       i_off,
                                       k_off,
                                       cmd == G7X_CONTOUR_ARC_CW,
                                       color,
                                       width)) ||
            (nc_sim_line_word_float(line, 'R', &r) &&
             nc_visual_draw_explicit_arc(preview,
                                         z0_x,
                                         stock_w,
                                         stock_top,
                                         stock_h,
                                         *last_x,
                                         *last_z,
                                         x,
                                         z,
                                         r,
                                         cmd == G7X_CONTOUR_ARC_CW,
                                         color,
                                         width))) {
            /* Arc drawn above. */
        } else {
            lvds_draw_line_w(x0, y0, x1, y1, color, width);
        }
    } else if (cmd == G7X_CONTOUR_RAPID) {
        nc_visual_draw_dashed_segment(preview,
                                      z0_x,
                                      stock_w,
                                      stock_top,
                                      stock_h,
                                      *last_z,
                                      *last_x,
                                      z,
                                      x,
                                      NC_VISUAL_DIM);
    } else {
        lvds_draw_line_w(x0, y0, x1, y1, color, width);
    }

    *last_x = x;
    *last_z = z;
    return true;
}

static void nc_visual_draw_emitted_preview(const nc_document_t *doc,
                                           const nc_preview_info_t *preview,
                                           int z0_x,
                                           int stock_w,
                                           int stock_top,
                                           int stock_h,
                                           size_t max_lines)
{
    nc_emit_stream_t stream;
    float last_x = 0.0f;
    float last_z = 0.0f;
    bool have_last = false;
    size_t emitted_line = 0;
    size_t drawn_lines = 0;
    size_t guard = 0;
    nc_preview_segment_t segment = NC_PREVIEW_SEG_FEED;

    if (!doc || !preview) {
        return;
    }

    nc_emit_stream_begin(&stream, doc, 0);
    nc_emit_stream_set_log(&stream, false);
    while (stream.active && guard++ < max_lines * 8u + 64u) {
        char line[NC_MAX_LINE_LEN];
        nc_emit_result_t result = nc_emit_stream_next(&stream,
                                                      line,
                                                      sizeof(line),
                                                      &emitted_line);
        if (result == NC_EMIT_ERROR) {
            snprintf(g_nc_visual_status, sizeof(g_nc_visual_status),
                     "Preview: %s at line %lu", g7x_result_text(stream.error),
                     (unsigned long)(emitted_line + 1u));
            break;
        }
        if (result == NC_EMIT_LINE) {
            if (line[0] == '(') {
                if (strstr(line, "rough")) {
                    segment = NC_PREVIEW_SEG_ROUGH;
                } else if (strstr(line, "finish")) {
                    segment = NC_PREVIEW_SEG_FINISH;
                }
                continue;
            }
            if (drawn_lines >= max_lines) {
                break;
            }
            if (g_nc_visual_mode == NC_MODE_SIM &&
                ((segment == NC_PREVIEW_SEG_ROUGH && !g_nc_visual_sim_rough) ||
                 (segment != NC_PREVIEW_SEG_ROUGH && !g_nc_visual_sim_path))) {
                drawn_lines++;
                continue;
            }
            (void)nc_visual_draw_emitted_motion_line(preview,
                                                     z0_x,
                                                     stock_w,
                                                     stock_top,
                                                     stock_h,
                                                     line,
                                                     segment,
                                                     emitted_line == doc->cursor_line,
                                                     &last_x,
                                                     &last_z,
                                                     &have_last);
            drawn_lines++;
        } else if (emitted_line >= doc->line_count &&
                   nc_emit_stream_line(&stream) >= doc->line_count) {
            break;
        }
    }
}

static bool nc_visual_runtime_busy(const nc_runtime_state_t *runtime)
{
    return runtime && (runtime->exec_state & (EXEC_RUN | EXEC_HOLD));
}

static const char *nc_visual_run_state_text(const nc_runtime_state_t *runtime)
{
    if (g_nc_visual_mode != NC_MODE_RUN) {
        return nc_menu_mode_name(g_nc_visual_mode);
    }
    if (nc_run_hold() || (runtime && (runtime->exec_state & EXEC_HOLD))) {
        return "RUN HOLD";
    }
    if (nc_visual_runtime_busy(runtime) || nc_run_active()) {
        return "RUN ACTIVE";
    }
    if (nc_run_done()) {
        return "RUN IDLE";
    }
    return "RUN";
}

static const char *nc_visual_file_basename(const char *path)
{
    const char *slash;
    const char *backslash;

    if (!path || !path[0]) {
        return "(no file)";
    }
    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash)) {
        slash = backslash;
    }
    return slash ? slash + 1 : path;
}

static float nc_live_runtime_x_to_diam(float runtime_x)
{
    return nc_visual_absf(runtime_x) * 2.0f;
}

static bool nc_visual_live_tool_rect(const nc_preview_info_t *preview,
                                     const nc_runtime_state_t *runtime,
                                     int z0_x,
                                     int stock_w,
                                     int stock_top,
                                     int stock_h,
                                     int *rx,
                                     int *ry,
                                     int *rw,
                                     int *rh)
{
    int sx;
    int sy;
    int pad = 16;
    int min_x;
    int max_x;
    int min_y;
    int max_y;
    int x0;
    int y0;
    int x1;
    int y1;

    if (!preview || !runtime) {
        return false;
    }

    sx = nc_visual_preview_z(preview, z0_x, stock_w, runtime->z);
    sy = nc_visual_preview_x(preview, stock_top, stock_h, nc_live_runtime_x_to_diam(runtime->x));
    sx = nc_visual_clampi(sx, 0, LVDS_HSTX_WIDTH - 1);
    sy = nc_visual_clampi(sy, 44, LVDS_HSTX_HEIGHT - 1);

    min_x = 0;
    max_x = nc_visual_clampi(z0_x + stock_w + pad, 0, LVDS_HSTX_WIDTH - 1);
    min_y = nc_visual_clampi(stock_top - pad, 44, LVDS_HSTX_HEIGHT - 1);
    max_y = nc_visual_clampi(stock_top + stock_h + pad, 44, LVDS_HSTX_HEIGHT - 1);
    x0 = nc_visual_clampi(sx - pad, min_x, max_x);
    y0 = nc_visual_clampi(sy - pad, min_y, max_y);
    x1 = nc_visual_clampi(sx + pad, min_x, max_x);
    y1 = nc_visual_clampi(sy + pad, min_y, max_y);

    if (rx) *rx = x0;
    if (ry) *ry = y0;
    if (rw) *rw = x1 - x0 + 1;
    if (rh) *rh = y1 - y0 + 1;
    return true;
}

static void nc_visual_draw_live_tool(const nc_preview_info_t *preview,
                                     const nc_runtime_state_t *runtime,
                                     int z0_x,
                                     int stock_w,
                                     int stock_top,
                                     int stock_h,
                                     const nc_tool_t *tool)
{
    int sx;
    int sy;
    int rx;
    int ry;
    int rw;
    int rh;

    if (!preview || !runtime || !tool || !tool->valid) {
        return;
    }

    sx = nc_visual_preview_z(preview, z0_x, stock_w, runtime->z);
    sy = nc_visual_preview_x(preview, stock_top, stock_h, nc_live_runtime_x_to_diam(runtime->x));
    sx = nc_visual_clampi(sx, 0, LVDS_HSTX_WIDTH - 1);
    sy = nc_visual_clampi(sy, 44, LVDS_HSTX_HEIGHT - 1);

    if (nc_visual_live_tool_rect(preview, runtime, z0_x, stock_w, stock_top, stock_h, &rx, &ry, &rw, &rh)) {
        g_nc_live_tool_rect_x = rx;
        g_nc_live_tool_rect_y = ry;
        g_nc_live_tool_rect_w = rw;
        g_nc_live_tool_rect_h = rh;
        g_nc_live_tool_rect_valid = true;
    }
    nc_visual_draw_tool_glyph(sx, sy, 20, tool, NC_VISUAL_PREVIEW_BG, false);
}

static bool nc_live_stock_alloc(void)
{
    if (g_nc_live_stock_mask) {
        return true;
    }
#if NC_VISUAL_HAVE_PSRAM
    if (!lvds_psram_available()) {
        (void)lvds_psram_init();
    }
    if (lvds_psram_available()) {
        g_nc_live_stock_mask = (uint8_t *)lvds_psram_ptr(NC_LIVE_STOCK_PSRAM_OFFSET);
    }
#endif
    return g_nc_live_stock_mask != NULL;
}

static bool nc_live_stock_context_changed(const nc_preview_info_t *preview,
                                          int stock_w,
                                          int stock_h)
{
    if (!preview) {
        return true;
    }
    return !g_nc_live_stock_ready ||
           g_nc_live_stock_w != stock_w ||
           g_nc_live_stock_h != stock_h ||
           g_nc_live_stock_setup_x != preview->stock_x ||
           g_nc_live_stock_setup_z != preview->stock_visible_z ||
           g_nc_live_stock_setup_i != preview->stock_i;
}

static void nc_live_stock_reset(const nc_preview_info_t *preview,
                                int stock_w,
                                int stock_h)
{
    int material_top = 0;
    int y;

    g_nc_live_stock_ready = false;
    g_nc_live_stock_has_last = false;
    if (!preview || !nc_live_stock_alloc()) {
        return;
    }

    stock_w = nc_visual_clampi(stock_w, 1, NC_LIVE_STOCK_MAX_W);
    stock_h = nc_visual_clampi(stock_h, 1, NC_LIVE_STOCK_MAX_H);
    memset(g_nc_live_stock_mask, 0, (size_t)NC_LIVE_STOCK_MAX_W * NC_LIVE_STOCK_MAX_H);
    if (preview->stock_i > 0.0f && preview->stock_x > 0.0f) {
        material_top = nc_visual_clampi((int)((preview->stock_i / preview->stock_x) * (float)stock_h),
                                        0,
                                        stock_h - 1);
    }
    for (y = material_top; y < stock_h; y++) {
        memset(g_nc_live_stock_mask + ((size_t)y * NC_LIVE_STOCK_MAX_W), 1, (size_t)stock_w);
    }

    g_nc_live_stock_w = stock_w;
    g_nc_live_stock_h = stock_h;
    g_nc_live_stock_setup_x = preview->stock_x;
    g_nc_live_stock_setup_z = preview->stock_visible_z;
    g_nc_live_stock_setup_i = preview->stock_i;
    g_nc_live_stock_ready = true;
}

static void nc_live_stock_remove_rect(int x0, int y0, int x1, int y1)
{
    int y;

    if (!g_nc_live_stock_mask || !g_nc_live_stock_ready) {
        return;
    }
    x0 = nc_visual_clampi(x0, 0, g_nc_live_stock_w - 1);
    x1 = nc_visual_clampi(x1, 0, g_nc_live_stock_w - 1);
    y0 = nc_visual_clampi(y0, 0, g_nc_live_stock_h - 1);
    y1 = nc_visual_clampi(y1, 0, g_nc_live_stock_h - 1);
    if (x1 < x0) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y1 < y0) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    for (y = y0; y <= y1; y++) {
        memset(g_nc_live_stock_mask + ((size_t)y * NC_LIVE_STOCK_MAX_W) + x0,
               0,
               (size_t)(x1 - x0 + 1));
    }
}

static void nc_live_stock_cut_sweep(const nc_preview_info_t *preview,
                                    int z0_x,
                                    int stock_left,
                                    int stock_w,
                                    int stock_top,
                                    int stock_h,
                                    float x0,
                                    float z0,
                                    float x1,
                                    float z1)
{
    int sx0;
    int sx1;
    int sy0;
    int sy1;
    int samples;
    int i;

    if (!preview || !g_nc_live_stock_ready) {
        return;
    }
    sx0 = nc_visual_preview_z(preview, z0_x, stock_w, z0) - stock_left;
    sx1 = nc_visual_preview_z(preview, z0_x, stock_w, z1) - stock_left;
    sy0 = nc_visual_preview_x(preview, stock_top, stock_h, x0) - stock_top;
    sy1 = nc_visual_preview_x(preview, stock_top, stock_h, x1) - stock_top;
    samples = nc_visual_absf((float)(sx1 - sx0)) > nc_visual_absf((float)(sy1 - sy0)) ?
              nc_visual_absf((float)(sx1 - sx0)) :
              nc_visual_absf((float)(sy1 - sy0));
    samples = nc_visual_clampi(samples, 1, 80);
    for (i = 0; i <= samples; i++) {
        float t = (float)i / (float)samples;
        int sx = sx0 + (int)((float)(sx1 - sx0) * t);
        int sy = sy0 + (int)((float)(sy1 - sy0) * t);
        nc_live_stock_remove_rect(sx - 1, sy, sx + 1, g_nc_live_stock_h - 1);
    }
}

static void nc_live_stock_update(const nc_preview_info_t *preview,
                                 const nc_runtime_state_t *runtime,
                                 int z0_x,
                                 int stock_left,
                                 int stock_w,
                                 int stock_top,
                                 int stock_h)
{
    float diam_x;

    if (!preview || !runtime || !g_nc_live_stock_ready) {
        return;
    }
    diam_x = nc_live_runtime_x_to_diam(runtime->x);
    if (g_nc_live_stock_has_last) {
        nc_live_stock_cut_sweep(preview,
                                z0_x,
                                stock_left,
                                stock_w,
                                stock_top,
                                stock_h,
                                g_nc_live_stock_last_x,
                                g_nc_live_stock_last_z,
                                diam_x,
                                runtime->z);
    } else {
        nc_live_stock_cut_sweep(preview,
                                z0_x,
                                stock_left,
                                stock_w,
                                stock_top,
                                stock_h,
                                diam_x,
                                runtime->z,
                                diam_x,
                                runtime->z);
    }
    g_nc_live_stock_last_x = diam_x;
    g_nc_live_stock_last_z = runtime->z;
    g_nc_live_stock_has_last = true;
}

static void nc_live_stock_draw(int stock_left,
                               int stock_top,
                               int clip_x,
                               int clip_y,
                               int clip_w,
                               int clip_h)
{
    int y0;
    int y1;
    int y;
    int clip_left = clip_x - stock_left;
    int clip_right = clip_left + clip_w - 1;

    if (!g_nc_live_stock_mask || !g_nc_live_stock_ready) {
        return;
    }
    clip_left = nc_visual_clampi(clip_left, 0, g_nc_live_stock_w - 1);
    clip_right = nc_visual_clampi(clip_right, 0, g_nc_live_stock_w - 1);
    y0 = nc_visual_clampi(clip_y - stock_top, 0, g_nc_live_stock_h - 1);
    y1 = nc_visual_clampi(clip_y + clip_h - stock_top - 1, 0, g_nc_live_stock_h - 1);
    if (clip_right < clip_left || y1 < y0) {
        return;
    }
    lvds_draw_fill_rect(stock_left + clip_left,
                        stock_top + y0,
                        clip_right - clip_left + 1,
                        y1 - y0 + 1,
                        NC_VISUAL_PREVIEW_BG);
    for (y = y0; y <= y1; y++) {
        const uint8_t *row = g_nc_live_stock_mask + ((size_t)y * NC_LIVE_STOCK_MAX_W);
        int x = clip_left;
        while (x <= clip_right) {
            int start;
            while (x <= clip_right && !row[x]) {
                x++;
            }
            start = x;
            while (x <= clip_right && row[x]) {
                x++;
            }
            if (x > start) {
                lvds_draw_fill_rect(stock_left + start,
                                    stock_top + y,
                                    x - start,
                                    1,
                                    NC_VISUAL_PREVIEW_STOCK);
            }
        }
    }
}

static bool nc_visual_draw_live_stock(const nc_preview_info_t *preview,
                                      const nc_runtime_state_t *runtime,
                                      const nc_tool_t *tool,
                                      int stock_left,
                                      int stock_top,
                                      int stock_w,
                                      int stock_h,
                                      int z0_x,
                                      int tool_panel_x,
                                      int tool_panel_y,
                                      int tool_panel_w,
                                      bool draw_static_panel)
{
    bool live_run;
    bool context_changed;
    bool retain_stock;
    int clear_y;
    int clear_h;
    uint32_t t0;
    uint32_t t1;
    uint32_t t2;
    uint32_t t3;
    uint32_t t4;

    live_run = nc_visual_runtime_busy(runtime) || nc_run_active();

    if (!preview || !runtime) {
        g_nc_live_stock_was_active = false;
        g_nc_live_tool_rect_valid = false;
        return false;
    }
    context_changed = nc_live_stock_context_changed(preview, stock_w, stock_h);
    retain_stock = g_nc_visual_mode == NC_MODE_RUN &&
                   g_nc_live_stock_ready &&
                   !context_changed;
    if (!live_run && !retain_stock) {
        g_nc_live_stock_was_active = false;
        g_nc_live_tool_rect_valid = false;
        return false;
    }
    if (live_run && (!g_nc_live_stock_was_active || context_changed)) {
        nc_live_stock_reset(preview, stock_w, stock_h);
        g_nc_live_tool_rect_valid = false;
    }
    g_nc_live_stock_was_active = live_run || retain_stock;
    if (!g_nc_live_stock_ready) {
        nc_visual_draw_text_clip(stock_left,
                                 stock_top + 16,
                                 "Live stock needs PSRAM",
                                 28,
                                 NC_VISUAL_ERROR,
                                 NC_VISUAL_PREVIEW_BG,
                                 LVDS_FONT_NORMAL);
        return true;
    }
    t0 = mcu_micros();
    clear_y = nc_visual_clampi(stock_top - 24, 44, NC_FOOTER_Y - 1);
    clear_h = nc_visual_clampi(stock_top + stock_h + 74 - clear_y, 1, NC_FOOTER_Y - clear_y);
    lvds_draw_fill_rect(tool_panel_x, clear_y, tool_panel_w, clear_h, NC_VISUAL_PREVIEW_BG);
    t1 = mcu_micros();
    if (live_run) {
        nc_live_stock_update(preview, runtime, z0_x, stock_left, stock_w, stock_top, stock_h);
    }
    nc_visual_draw_chuck(preview, stock_left, stock_top, stock_w, stock_h);
    nc_visual_draw_chuck_relief(preview, stock_left, stock_top, stock_h);
    nc_live_stock_draw(stock_left, stock_top, stock_left, stock_top, stock_w, stock_h);
    t2 = mcu_micros();
#if NC_PREVIEW_DIN_STYLE
    if (g_nc_visual_show_dims) {
        nc_visual_draw_din_layer(preview, stock_left, stock_top, stock_w, stock_h, z0_x);
    }
#endif
    if (g_nc_visual_show_dims) {
        nc_visual_draw_sim_contour_points(&g_nc_visual_doc,
                                          preview,
                                          z0_x,
                                          stock_left,
                                          stock_left + stock_w,
                                          stock_w,
                                          stock_top,
                                          stock_h);
    }
#if !NC_PREVIEW_DIN_STYLE
    lvds_draw_line(z0_x, stock_top - 18, z0_x, stock_top + stock_h + 18, NC_VISUAL_DIM);
    lvds_draw_text(z0_x - 12, stock_top - 34, "Z0", NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_NORMAL);
#endif
    t3 = mcu_micros();
    nc_visual_draw_live_tool(preview, runtime, z0_x, stock_w, stock_top, stock_h, tool);
    if (draw_static_panel) {
        nc_visual_draw_preview_tool_panel(tool_panel_x,
                                          tool_panel_y,
                                          tool_panel_w,
                                          58,
                                          tool);
    }
    t4 = mcu_micros();
    g_nc_visual_frame_preview_clear_us += t1 - t0;
    g_nc_visual_frame_preview_stock_us += t2 - t1;
    g_nc_visual_frame_preview_geom_us += t3 - t2;
    g_nc_visual_frame_preview_tool_us += t4 - t3;
    return true;
}

static void nc_visual_draw_thin_preview(const nc_document_t *doc,
                                        const nc_runtime_state_t *runtime,
                                        int x,
                                        int y,
                                        int w,
                                        int h,
                                        bool clear_bg)
{
    nc_preview_info_t preview;
    int stock_w;
    int stock_h;
    int stock_left;
    int stock_top;
    int z0_x;
    float usable_w;
    float usable_h;
    float z_scale;
    float x_scale;
    float scale;
    nc_tool_t tool;
    bool have_tool = false;
    size_t tool_line = doc && doc->line_count ? doc->cursor_line : 0;
    uint32_t t0 = mcu_micros();
    uint32_t t1;
    uint32_t t2;
    uint32_t t3;
    uint32_t t5;
    bool use_live_cache = !clear_bg &&
                          g_nc_visual_mode == NC_MODE_RUN &&
                          g_nc_live_preview_cache_valid;

    if (doc && g_nc_visual_mode == NC_MODE_RUN && nc_run_line() < doc->line_count) {
        tool_line = nc_run_line();
    }

    if (use_live_cache) {
        preview = g_nc_live_preview_cache;
        tool = g_nc_live_tool_cache;
        have_tool = g_nc_live_have_tool_cache;
        stock_w = g_nc_live_stock_w_cache;
        stock_h = g_nc_live_stock_h_cache;
        stock_left = g_nc_live_stock_left_cache;
        stock_top = g_nc_live_stock_top_cache;
        z0_x = g_nc_live_z0_x_cache;
        t1 = t0;
        t2 = t0;
        t3 = t0;
    } else {
        nc_sim_collect_preview(doc, &preview);
        t1 = mcu_micros();
        if (clear_bg) {
            lvds_draw_fill_rect(x, y, w, h, NC_VISUAL_PREVIEW_BG);
            if (g_nc_visual_mode == NC_MODE_SIM) {
                nc_visual_draw_text_clip(x + 12,
                                         y + 10,
                                         "SIM",
                                         4,
                                         NC_VISUAL_ACCENT,
                                         NC_VISUAL_PREVIEW_BG,
                                         LVDS_FONT_NORMAL);
                nc_visual_draw_text_clip(x + 50,
                                         y + 10,
                                         nc_visual_file_basename(doc ? doc->path : ""),
                                         26,
                                         NC_VISUAL_DIM,
                                         NC_VISUAL_PREVIEW_BG,
                                         LVDS_FONT_NORMAL);
            } else {
                nc_visual_draw_text_clip(x + 12,
                                         y + 10,
                                         nc_visual_run_state_text(runtime),
                                         18,
                                         NC_VISUAL_ACCENT,
                                         NC_VISUAL_PREVIEW_BG,
                                         LVDS_FONT_NORMAL);
            }
        }
        t2 = mcu_micros();

        usable_w = (float)(w - 72);
        usable_h = (float)(h - (g_nc_visual_mode == NC_MODE_SIM ? 104 : 84));
        if (usable_w < 40.0f) usable_w = 40.0f;
        if (usable_h < 40.0f) usable_h = 40.0f;
        z_scale = usable_w / preview.stock_visible_z;
        x_scale = usable_h / (preview.stock_x * 0.5f);
        scale = z_scale < x_scale ? z_scale : x_scale;
        if (scale <= 0.0f) scale = 1.0f;
        stock_w = (int)(preview.stock_visible_z * scale + 0.5f);
        stock_h = (int)((preview.stock_x * 0.5f) * scale + 0.5f);
        if (stock_w < 24) stock_w = 24;
        if (stock_h < 24) stock_h = 24;
        if (stock_w > (int)usable_w) stock_w = (int)usable_w;
        if (stock_h > (int)usable_h) stock_h = (int)usable_h;

        stock_left = x + 20;
        stock_top = y + 112;
        z0_x = stock_left + (int)(preview.stock_z * scale + 0.5f);
        if (z0_x < stock_left) z0_x = stock_left;
        if (z0_x > stock_left + stock_w) z0_x = stock_left + stock_w;
        memset(&tool, 0, sizeof(tool));
        if (doc && g_nc_visual_mode == NC_MODE_TOOLS &&
            nc_tool_active_from_table(doc, tool_line, &g_nc_visual_doc, &tool)) {
            have_tool = true;
        } else if (doc && nc_tool_active_from_file(doc,
                                                   tool_line,
                                                   nc_visual_tool_path(),
                                                   &tool)) {
            have_tool = true;
        }
        t3 = mcu_micros();
        if (g_nc_visual_mode == NC_MODE_RUN) {
            g_nc_live_preview_cache = preview;
            g_nc_live_tool_cache = tool;
            g_nc_live_have_tool_cache = have_tool;
            g_nc_live_stock_w_cache = stock_w;
            g_nc_live_stock_h_cache = stock_h;
            g_nc_live_stock_left_cache = stock_left;
            g_nc_live_stock_top_cache = stock_top;
            g_nc_live_z0_x_cache = z0_x;
            g_nc_live_preview_cache_valid = true;
        }
    }
    g_nc_visual_frame_preview_collect_us += (t1 - t0) + (t3 - t2);
    g_nc_visual_frame_preview_clear_us += t2 - t1;
    if (g_nc_visual_mode == NC_MODE_RUN && nc_visual_draw_live_stock(&preview,
                                                                      runtime,
                                                                      have_tool ? &tool : NULL,
                                                                      stock_left,
                                                                      stock_top,
                                                                      stock_w,
                                                                      stock_h,
                                                                      z0_x,
                                                                      x,
                                                                      y,
                                                                      w,
                                                                      clear_bg)) {
        return;
    }

    t0 = mcu_micros();
    nc_visual_draw_chuck(&preview, stock_left, stock_top, stock_w, stock_h);
    nc_visual_draw_chuck_relief(&preview, stock_left, stock_top, stock_h);
    if (g_nc_visual_sim_stock || g_nc_visual_mode != NC_MODE_SIM) {
        lvds_draw_fill_rect(stock_left, stock_top, stock_w, stock_h, NC_VISUAL_PREVIEW_STOCK);
        if (preview.stock_i > 0.0f) {
            int id_h = nc_visual_preview_x(&preview, stock_top, stock_h, preview.stock_i) - stock_top;
            if (id_h > 0 && id_h < stock_h) {
                lvds_draw_fill_rect(stock_left, stock_top, stock_w, id_h, NC_VISUAL_PREVIEW_BG);
            }
        }
    }
    t1 = mcu_micros();
#if NC_PREVIEW_DIN_STYLE
    if (g_nc_visual_show_dims) {
        nc_visual_draw_din_layer(&preview, stock_left, stock_top, stock_w, stock_h, z0_x);
    }
#endif
    if (g_nc_visual_show_dims) {
        nc_visual_draw_sim_contour_points(doc,
                                          &preview,
                                          z0_x,
                                          stock_left,
                                          stock_left + stock_w,
                                          stock_w,
                                          stock_top,
                                          stock_h);
    }
#if !NC_PREVIEW_DIN_STYLE
    lvds_draw_line(z0_x, stock_top - 18, z0_x, stock_top + stock_h + 18, NC_VISUAL_DIM);
    lvds_draw_text(z0_x - 12, stock_top - 34, "Z0", NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_NORMAL);
#endif
    t2 = mcu_micros();
    if (g_nc_visual_mode != NC_MODE_SIM) {
        nc_visual_draw_preview_tool_panel(x, y, w, h, have_tool ? &tool : NULL);
    }
    t3 = mcu_micros();
    nc_visual_draw_emitted_preview(doc,
                                   &preview,
                                   z0_x,
                                   stock_w,
                                   stock_top,
                                   stock_h,
                                   96u);
    t5 = mcu_micros();
    if (have_tool && g_nc_visual_mode != NC_MODE_SIM) {
        nc_visual_draw_live_tool(&preview, runtime, z0_x, stock_w, stock_top, stock_h, &tool);
        nc_visual_draw_preview_tool_panel(x, y, w, h, &tool);
    }
    g_nc_visual_frame_preview_stock_us += t1 - t0;
    g_nc_visual_frame_preview_geom_us += (t2 - t1) + (t5 - t3);
    g_nc_visual_frame_preview_tool_us += (t3 - t2) + (mcu_micros() - t5);
}

static void nc_visual_footer_text(char *out, size_t out_sz)
{
    size_t count;
    size_t i;
    size_t used = 0;
    const nc_footer_item_t *footer = nc_menu_footer(g_nc_visual_mode, nc_files_active(), &count);

    if (!out || out_sz == 0) {
        return;
    }

    out[0] = '\0';
    if (!nc_files_active() &&
        nc_visual_can_edit_code() &&
        g_nc_visual_doc.selected_word >= 0) {
        snprintf(out, out_sz, "B UP|C DOWN|D NEXT|# OK|* DEL");
        return;
    }

    for (i = 0; i < count; i++) {
        int n;
        n = snprintf(out + used,
                     out_sz - used,
                     "%s%s%c %s",
                     i ? "|" : "",
                     footer[i].action == g_nc_visual_selected_action ? "!" : "",
                     footer[i].key,
                     footer[i].label);
        if (n < 0 || (size_t)n >= out_sz - used) {
            out[out_sz - 1] = '\0';
            return;
        }
        used += (size_t)n;
    }
}

static void nc_visual_draw_footer_status(const char *message, const char *footer_text)
{
    char field[24];
    const char *p = footer_text ? footer_text : "";
    int fields = 1;
    int field_w;
    int i;

    lvds_draw_fill_rect(0, NC_FOOTER_Y, LVDS_HSTX_WIDTH, NC_FOOTER_H, NC_VISUAL_FOOTER_BG);
    nc_visual_draw_text_clip(12,
                             NC_FOOTER_Y + 8,
                             message ? message : "",
                             80,
                             NC_VISUAL_FOOTER_TEXT,
                             NC_VISUAL_FOOTER_BG,
                             LVDS_FONT_NORMAL);

    if (!p[0]) {
        return;
    }

    for (i = 0; p[i]; i++) {
        if (p[i] == '|') {
            fields++;
        }
    }
    if (fields < 1) {
        fields = 1;
    }
    field_w = (LVDS_HSTX_WIDTH - 24) / fields;

    for (i = 0; i < fields; i++) {
        const char *bar = strchr(p, '|');
        const char *space;
        char key[8];
        const char *label;
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        bool active = false;
        int x = 12 + i * field_w;
        int key_w;
        lvds_color_t button_bg = NC_VISUAL_FOOTER_BUTTON;
        lvds_color_t button_fg = NC_VISUAL_FOOTER_TEXT;

        if (len >= sizeof(field)) {
            len = sizeof(field) - 1;
        }
        memcpy(field, p, len);
        field[len] = '\0';
        if (field[0] == '!') {
            active = true;
            memmove(field, field + 1, strlen(field));
        }
        if (active) {
            button_bg = NC_VISUAL_FOOTER_VALUE;
        }

        space = strchr(field, ' ');
        if (space) {
            size_t key_len = (size_t)(space - field);
            if (key_len >= sizeof(key)) {
                key_len = sizeof(key) - 1;
            }
            memcpy(key, field, key_len);
            key[key_len] = '\0';
            label = space + 1;
        } else {
            key[0] = '\0';
            label = field;
        }

        lvds_draw_text_clip(x, NC_FOOTER_Y + 31, key, 3, NC_VISUAL_ACCENT, NC_VISUAL_FOOTER_BG, LVDS_FONT_NORMAL);
        key_w = key[0] ? (lvds_draw_text_width(key, LVDS_FONT_NORMAL) + 4) : 0;
        lvds_draw_fill_rect(x + key_w, NC_FOOTER_Y + 28, field_w - key_w - 4, 22, button_bg);
        lvds_draw_line(x + key_w, NC_FOOTER_Y + 49, x + field_w - 5, NC_FOOTER_Y + 49, NC_VISUAL_DIM);
        lvds_draw_line(x + field_w - 5, NC_FOOTER_Y + 28, x + field_w - 5, NC_FOOTER_Y + 49, NC_VISUAL_DIM);
        lvds_draw_text_clip(x + key_w + 4,
                            NC_FOOTER_Y + 31,
                            label,
                            (field_w - key_w - 12) / NC_VISUAL_CHAR_W,
                            button_fg,
                            button_bg,
                            LVDS_FONT_NORMAL);

        p = bar ? (bar + 1) : "";
    }
}

static char nc_visual_key_char(nc_visual_key_t key)
{
    if (key >= NC_VISUAL_KEY_DIGIT_0 && key <= NC_VISUAL_KEY_DIGIT_9) {
        return (char)('0' + (key - NC_VISUAL_KEY_DIGIT_0));
    }

    switch (key) {
    case NC_VISUAL_KEY_BACKSPACE: return '*';
    case NC_VISUAL_KEY_FINISH: return '#';
    case NC_VISUAL_KEY_CANCEL: return 'A';
    case NC_VISUAL_KEY_PREV: return 'B';
    case NC_VISUAL_KEY_NEXT: return 'C';
    case NC_VISUAL_KEY_ACCEPT: return 'D';
    default: return '\0';
    }
}

static const char *nc_visual_new_file_ext(void)
{
    return g_nc_visual_mode == NC_MODE_TOOLS ? ".t" : ".nc";
}

static void nc_visual_new_file_status(void)
{
    snprintf(g_nc_visual_status,
             sizeof(g_nc_visual_status),
             "New:%.40s%s #OK *DEL A",
             g_nc_visual_new_file_name[0] ? g_nc_visual_new_file_name : "_",
             nc_visual_new_file_ext());
}

static bool nc_visual_new_file_handle_key(nc_visual_key_t key)
{
    char ch = nc_visual_key_char(key);
    size_t len;
    char path[NC_PATH_MAX];
    nc_result_t r;

    if (!g_nc_visual_new_file_active) {
        return false;
    }

    if (key == NC_VISUAL_KEY_CANCEL) {
        g_nc_visual_new_file_active = false;
        strncpy(g_nc_visual_status, "New file cancelled", sizeof(g_nc_visual_status) - 1);
        return true;
    }
    if (key == NC_VISUAL_KEY_FINISH || key == NC_VISUAL_KEY_ACCEPT) {
        if (!g_nc_visual_new_file_name[0] ||
            !nc_files_create_named(g_nc_visual_new_file_name, nc_visual_new_file_ext(), path, sizeof(path))) {
            strncpy(g_nc_visual_status, "New file create failed", sizeof(g_nc_visual_status) - 1);
            return true;
        }
        r = nc_load_file(&g_nc_visual_doc, path);
        if (r != NC_OK) {
            snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Create open failed: %s", nc_result_text(r));
            return true;
        }
        g_nc_visual_new_file_active = false;
        nc_files_set_active(false);
        nc_state_remember_path(g_nc_visual_mode, path);
        nc_state_save();
        snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Created %s", nc_visual_file_basename(path));
        return true;
    }
    if (key == NC_VISUAL_KEY_BACKSPACE) {
        len = strlen(g_nc_visual_new_file_name);
        if (len > 0u) {
            g_nc_visual_new_file_name[len - 1u] = '\0';
        }
        nc_visual_new_file_status();
        return true;
    }
    if (ch >= '0' && ch <= '9') {
        len = strlen(g_nc_visual_new_file_name);
        if (len + 1u < sizeof(g_nc_visual_new_file_name)) {
            g_nc_visual_new_file_name[len] = ch;
            g_nc_visual_new_file_name[len + 1u] = '\0';
        }
        nc_visual_new_file_status();
        return true;
    }

    nc_visual_new_file_status();
    return true;
}

static uint8_t nc_visual_footer_action_for_key(nc_visual_key_t key)
{
    size_t count;
    size_t i;
    char key_char = nc_visual_key_char(key);
    const nc_footer_item_t *footer = nc_menu_footer(g_nc_visual_mode, nc_files_active(), &count);

    if (!key_char) {
        return NC_FOOTER_ACTION_NONE;
    }

    for (i = 0; i < count; i++) {
        if (footer[i].key == key_char) {
            return footer[i].action;
        }
    }

    return NC_FOOTER_ACTION_NONE;
}

static void nc_visual_run_arm(size_t line, const char *label)
{
    if (!nc_run_arm(&g_nc_visual_doc, line)) {
        strncpy(g_nc_visual_status, "RUN needs a program", sizeof(g_nc_visual_status) - 1);
        return;
    }

    g_nc_visual_doc.cursor_line = nc_run_line();
    g_nc_visual_doc.selected_word = -1;
    nc_visual_set_mode(NC_MODE_RUN);
    snprintf(g_nc_visual_status,
             sizeof(g_nc_visual_status),
             "%s line %lu",
             label,
             (unsigned long)(nc_run_line() + 1));
}

static void nc_visual_run_step(void)
{
    size_t line = g_nc_visual_doc.cursor_line;

    if (!nc_run_send_document_line(&g_nc_visual_doc, line)) {
        strncpy(g_nc_visual_status, "RUN line skipped", sizeof(g_nc_visual_status) - 1);
        return;
    }
    g_nc_visual_doc.cursor_line = nc_run_line();
    g_nc_visual_doc.selected_word = -1;
    snprintf(g_nc_visual_status,
             sizeof(g_nc_visual_status),
             "RUN sent line %lu",
             (unsigned long)(line + 1u));
}

static bool nc_visual_is_code_view(void)
{
    return g_nc_visual_mode == NC_MODE_PROGRAM ||
           g_nc_visual_mode == NC_MODE_MDI ||
           g_nc_visual_mode == NC_MODE_RUN;
}

static bool nc_visual_uses_file(void)
{
    return nc_visual_is_code_view() ||
           g_nc_visual_mode == NC_MODE_SIM ||
           g_nc_visual_mode == NC_MODE_TOOLS;
}

static bool nc_visual_can_edit_code(void)
{
    return g_nc_visual_mode == NC_MODE_PROGRAM ||
           g_nc_visual_mode == NC_MODE_MDI ||
           g_nc_visual_mode == NC_MODE_TOOLS;
}

static void nc_visual_move_tool_line(int delta)
{
    int selected_tool = nc_visual_selected_tool_index();
    int tool_count = nc_visual_find_tool_line(selected_tool, NULL);
    int selected_line = -1;

    if (tool_count == 0) {
        strncpy(g_nc_visual_status, "No tool rows", sizeof(g_nc_visual_status) - 1);
        return;
    }
    if (delta < 0 && selected_tool > 0) {
        selected_tool--;
    } else if (delta > 0 && selected_tool + 1 < tool_count) {
        selected_tool++;
    }
    (void)nc_visual_find_tool_line(selected_tool, &selected_line);
    if (selected_line >= 0) {
        g_nc_visual_doc.cursor_line = (size_t)selected_line;
        g_nc_visual_doc.selected_word = -1;
        nc_text_edit_clear(&g_nc_visual_edit);
        snprintf(g_nc_visual_status,
                 sizeof(g_nc_visual_status),
                 "Tool %d/%d line %d",
                 selected_tool + 1,
                 tool_count,
                 selected_line + 1);
        nc_visual_serial_selected_line();
    }
}

static void nc_visual_set_code_line(size_t line)
{
    if (g_nc_visual_doc.line_count == 0) {
        g_nc_visual_doc.cursor_line = 0;
        nc_run_set_line(&g_nc_visual_doc, 0);
        return;
    }
    if (line >= g_nc_visual_doc.line_count) {
        line = g_nc_visual_doc.line_count - 1;
    }
    g_nc_visual_doc.cursor_line = line;
    if (g_nc_visual_mode == NC_MODE_RUN) {
        nc_run_set_line(&g_nc_visual_doc, line);
    }
    g_nc_visual_doc.selected_word = -1;
}

static size_t nc_visual_code_line(void)
{
    size_t line = g_nc_visual_mode == NC_MODE_RUN ? nc_run_line() : g_nc_visual_doc.cursor_line;

    return g_nc_visual_doc.line_count && line >= g_nc_visual_doc.line_count ? 0 : line;
}

static void nc_visual_move_code_line(int delta)
{
    size_t line = nc_visual_code_line();

    if ((!nc_visual_is_code_view() && g_nc_visual_mode != NC_MODE_SIM) ||
        g_nc_visual_doc.line_count == 0) {
        strncpy(g_nc_visual_status, "No code lines", sizeof(g_nc_visual_status) - 1);
        return;
    }
    if (delta < 0 && line > 0) {
        line--;
    } else if (delta > 0 && line + 1 < g_nc_visual_doc.line_count) {
        line++;
    }

    nc_visual_set_code_line(line);
    snprintf(g_nc_visual_status,
             sizeof(g_nc_visual_status),
             "%s line %lu",
             g_nc_visual_mode == NC_MODE_RUN ? "RUN" : "Line",
             (unsigned long)(line + 1));
}

static nc_result_t nc_visual_insert_tool_ref(void)
{
    int tool_no = 1;
    char line[16];

    (void)nc_tool_number_for_line(&g_nc_visual_doc, g_nc_visual_doc.cursor_line + 1u, &tool_no);
    if (tool_no <= 0) {
        tool_no = 1;
    }
    snprintf(line, sizeof(line), "T%d", tool_no);
    return nc_insert_line(&g_nc_visual_doc, g_nc_visual_doc.cursor_line + 1u, line);
}

static void nc_visual_open_file_view(const char *root, bool seed_samples)
{
    int made = seed_samples ? nc_files_seed_samples() : 0;

    nc_files_set_active(true);
    g_nc_visual_new_file_active = false;
    if (nc_files_refresh(root)) {
        snprintf(g_nc_visual_status,
                 sizeof(g_nc_visual_status),
                 made ? "Samples loaded: %s" : "Files: %s",
                 nc_files_cwd());
    } else {
        strncpy(g_nc_visual_status, "File list unavailable", sizeof(g_nc_visual_status) - 1);
    }
}

static void nc_visual_insert_preset_action(nc_preset_t preset,
                                           const char *ok,
                                           const char *fail)
{
    if (nc_insert_preset(&g_nc_visual_doc, preset) == NC_OK) {
        strncpy(g_nc_visual_status, ok, sizeof(g_nc_visual_status) - 1);
    } else {
        strncpy(g_nc_visual_status, fail, sizeof(g_nc_visual_status) - 1);
    }
}

static bool nc_visual_handle_selected_word_edit(nc_visual_key_t key)
{
    char key_char = nc_visual_key_char(key);

    if (nc_files_active() ||
        !nc_visual_can_edit_code() ||
        g_nc_visual_doc.selected_word < 0) {
        return false;
    }

    if (nc_text_edit_handle_key(&g_nc_visual_doc,
                                &g_nc_visual_edit,
                                key_char,
                                g_nc_visual_status,
                                sizeof(g_nc_visual_status))) {
        return true;
    }
    return false;
}

static void nc_visual_dispatch_footer_action(uint8_t action)
{
    g_nc_visual_selected_action = action;

    switch (action) {
    case NC_FOOTER_ACTION_FILE:
        nc_visual_open_file_view(NC_FILES_DIR, true);
        break;
    case NC_FOOTER_ACTION_FILES:
        nc_visual_open_file_view(NULL, false);
        break;
    case NC_FOOTER_ACTION_OPEN:
        if (nc_files_active()) {
            char path[NC_PATH_MAX];
            nc_result_t r;
            if (nc_files_selected_is_dir()) {
                if (nc_files_enter_selected()) {
                    snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Dir: %s", nc_files_cwd());
                } else {
                    strncpy(g_nc_visual_status, "Directory open failed", sizeof(g_nc_visual_status) - 1);
                }
                break;
            }
            if (!nc_files_selected_path(path, sizeof(path))) {
                strncpy(g_nc_visual_status, "No NC file selected", sizeof(g_nc_visual_status) - 1);
                break;
            }
            if (g_nc_visual_mode == NC_MODE_TOOLS &&
                !nc_state_tool_path_supported(path)) {
                strncpy(g_nc_visual_status, "TOOLS opens .t files only", sizeof(g_nc_visual_status) - 1);
                break;
            }
            r = nc_load_file(&g_nc_visual_doc, path);
            if (r == NC_OK) {
                nc_files_set_active(false);
                g_nc_visual_new_file_active = false;
                nc_state_remember_path(g_nc_visual_mode, path);
                nc_state_save();
                snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Opened %s", nc_files_name(nc_files_selected()));
            } else {
                snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Open failed: %s", nc_result_text(r));
            }
        } else {
            nc_files_set_active(true);
            g_nc_visual_new_file_active = false;
            if (nc_files_refresh(NULL)) {
                nc_visual_serial_selected_file();
                snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Files: %s", nc_files_cwd());
            } else {
                strncpy(g_nc_visual_status, "File list unavailable", sizeof(g_nc_visual_status) - 1);
            }
        }
        break;
    case NC_FOOTER_ACTION_PRESET_OD:
        nc_visual_insert_preset_action(NC_PRESET_OD, "Inserted OD preset", "OD preset failed");
        break;
    case NC_FOOTER_ACTION_PRESET_ID:
        nc_visual_insert_preset_action(NC_PRESET_ID, "Inserted ID preset", "ID preset failed");
        break;
    case NC_FOOTER_ACTION_PRESET_FACE:
        nc_visual_insert_preset_action(NC_PRESET_FACE, "Inserted FACE preset", "FACE preset failed");
        break;
    case NC_FOOTER_ACTION_PRESET_LINE:
        nc_visual_insert_preset_action(NC_PRESET_LINE, "Inserted line preset", "Line preset failed");
        break;
    case NC_FOOTER_ACTION_PRESET_ARC:
        nc_visual_insert_preset_action(NC_PRESET_ARC, "Inserted arc preset", "Arc preset failed");
        break;
    case NC_FOOTER_ACTION_PRESET_SETUP:
        nc_visual_insert_preset_action(NC_PRESET_SETUP, "Inserted setup preset", "Setup preset failed");
        break;
    case NC_FOOTER_ACTION_PRESET_END:
        nc_visual_insert_preset_action(NC_PRESET_END, "Inserted G80", "G80 preset failed");
        break;
    case NC_FOOTER_ACTION_TOOL:
        if (g_nc_visual_mode == NC_MODE_PROGRAM || g_nc_visual_mode == NC_MODE_MDI) {
            if (nc_visual_insert_tool_ref() == NC_OK) {
                nc_cursor_down(&g_nc_visual_doc);
                g_nc_visual_doc.selected_word = -1;
                snprintf(g_nc_visual_status,
                         sizeof(g_nc_visual_status),
                         "Inserted tool ref line %lu",
                         (unsigned long)(g_nc_visual_doc.cursor_line + 1u));
            } else {
                strncpy(g_nc_visual_status, "Tool ref insert failed", sizeof(g_nc_visual_status) - 1);
            }
        } else if (g_nc_visual_mode != NC_MODE_TOOLS) {
            nc_visual_set_mode(NC_MODE_TOOLS);
            nc_files_set_active(false);
            nc_text_edit_clear(&g_nc_visual_edit);
            if (nc_state_load_document(g_nc_visual_mode, &g_nc_visual_doc)) {
                if (g_nc_visual_doc.line_count && !nc_tool_line_is_tool(g_nc_visual_doc.lines[g_nc_visual_doc.cursor_line].text)) {
                    int first_line = -1;
                    (void)nc_visual_find_tool_line(0, &first_line);
                    if (first_line >= 0) {
                        g_nc_visual_doc.cursor_line = (size_t)first_line;
                    }
                }
                g_nc_visual_doc.selected_word = -1;
                (void)nc_select_next_word(&g_nc_visual_doc);
                snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "TOOLS: %.48s", g_nc_visual_doc.path);
            } else {
                nc_document_init(&g_nc_visual_doc);
                strncpy(g_nc_visual_status, "TOOLS has no file", sizeof(g_nc_visual_status) - 1);
            }
        } else if (nc_insert_tool_default(&g_nc_visual_doc) == NC_OK) {
            nc_cursor_down(&g_nc_visual_doc);
            g_nc_visual_doc.selected_word = -1;
            (void)nc_select_next_word(&g_nc_visual_doc);
            strncpy(g_nc_visual_status, "Inserted TOOL row", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "Tool action stub", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_SAVE:
        if (g_nc_visual_doc.path[0] && nc_save_file(&g_nc_visual_doc, g_nc_visual_doc.path) == NC_OK) {
            nc_state_remember_path(g_nc_visual_mode, g_nc_visual_doc.path);
            nc_state_save();
            strncpy(g_nc_visual_status, "Saved", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "Save needs an opened NC file", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_NEW:
        if (nc_files_active()) {
            g_nc_visual_new_file_active = true;
            g_nc_visual_new_file_name[0] = '\0';
            nc_visual_new_file_status();
            break;
        }
        nc_document_init(&g_nc_visual_doc);
        nc_insert_line(&g_nc_visual_doc, 0, "");
        strncpy(g_nc_visual_doc.path, "new.nc", sizeof(g_nc_visual_doc.path) - 1);
        nc_state_remember_path(g_nc_visual_mode, g_nc_visual_doc.path);
        nc_state_save();
        strncpy(g_nc_visual_status, "New empty NC program", sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_INSERT:
        if (nc_insert_line(&g_nc_visual_doc, g_nc_visual_doc.cursor_line + 1, "") == NC_OK) {
            nc_cursor_down(&g_nc_visual_doc);
            strncpy(g_nc_visual_status, "Inserted blank line", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "Insert failed", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_DELETE:
        if (nc_files_active()) {
            if (nc_files_delete_selected()) {
                strncpy(g_nc_visual_status, "File deleted", sizeof(g_nc_visual_status) - 1);
            } else {
                strncpy(g_nc_visual_status, "Delete file failed", sizeof(g_nc_visual_status) - 1);
            }
            break;
        }
        if (nc_delete_line(&g_nc_visual_doc, g_nc_visual_doc.cursor_line) == NC_OK) {
            strncpy(g_nc_visual_status, "Deleted line", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "Delete failed", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_BACK:
        if (nc_files_active()) {
            nc_files_select_prev();
            nc_visual_serial_selected_file();
            strncpy(g_nc_visual_status, "File up", sizeof(g_nc_visual_status) - 1);
        } else if (g_nc_visual_mode == NC_MODE_TOOLS) {
            nc_visual_move_tool_line(-1);
        } else {
            nc_visual_move_code_line(-1);
            nc_visual_serial_selected_line();
        }
        break;
    case NC_FOOTER_ACTION_STEP:
        if (nc_files_active()) {
            nc_files_select_next();
            nc_visual_serial_selected_file();
            strncpy(g_nc_visual_status, "File down", sizeof(g_nc_visual_status) - 1);
        } else if (g_nc_visual_mode == NC_MODE_TOOLS) {
            nc_visual_move_tool_line(1);
        } else {
            nc_visual_move_code_line(1);
            nc_visual_serial_selected_line();
        }
        break;
    case NC_FOOTER_ACTION_RESET:
        if (nc_files_active()) {
            if (nc_files_go_parent()) {
                snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Dir: %s", nc_files_cwd());
            } else {
                nc_files_set_active(false);
                strncpy(g_nc_visual_status, "Back to NC", sizeof(g_nc_visual_status) - 1);
            }
        } else if (g_nc_visual_mode == NC_MODE_RUN) {
            nc_run_reset();
            g_nc_visual_doc.cursor_line = 0;
            strncpy(g_nc_visual_status, "RUN reset", sizeof(g_nc_visual_status) - 1);
        } else if (g_nc_visual_mode == NC_MODE_SIM) {
            g_nc_visual_sim_stock = true;
            g_nc_visual_sim_path = true;
            g_nc_visual_sim_rough = true;
            g_nc_visual_show_dims = true;
            strncpy(g_nc_visual_status, "SIM view reset", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "Reset is stubbed", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_REFRESH:
        if (nc_files_refresh(NULL)) {
            nc_visual_serial_selected_file();
            snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Refreshed %s", nc_files_cwd());
        } else {
            strncpy(g_nc_visual_status, "Refresh failed", sizeof(g_nc_visual_status) - 1);
        }
        if (!nc_files_active()) {
            nc_files_set_active(true);
        }
        break;
    case NC_FOOTER_ACTION_FULL:
        if (nc_files_active()) {
            char path[NC_PATH_MAX];
            nc_result_t r;
            if (nc_files_selected_is_dir()) {
                if (nc_files_enter_selected()) {
                    snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Dir: %s", nc_files_cwd());
                } else {
                    strncpy(g_nc_visual_status, "Directory open failed", sizeof(g_nc_visual_status) - 1);
                }
                break;
            }
            if (!nc_files_selected_path(path, sizeof(path))) {
                strncpy(g_nc_visual_status, "No NC file selected", sizeof(g_nc_visual_status) - 1);
                break;
            }
            r = nc_load_file(&g_nc_visual_doc, path);
            if (r == NC_OK) {
                nc_files_set_active(false);
                nc_visual_set_mode(NC_MODE_RUN);
                nc_state_remember_path(NC_MODE_RUN, path);
                nc_state_save();
                nc_visual_run_arm(0, "Run loaded");
            } else {
                snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Run open failed: %s", nc_result_text(r));
            }
        } else {
            nc_visual_run_arm(0, "Full run armed");
        }
        break;
    case NC_FOOTER_ACTION_STOCK:
        g_nc_visual_sim_stock = !g_nc_visual_sim_stock;
        strncpy(g_nc_visual_status,
                g_nc_visual_sim_stock ? "SIM stock on" : "SIM stock outline",
                sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_PATH:
        g_nc_visual_sim_path = !g_nc_visual_sim_path;
        strncpy(g_nc_visual_status,
                g_nc_visual_sim_path ? "SIM path on" : "SIM path hidden",
                sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_ROUGH:
        g_nc_visual_sim_rough = !g_nc_visual_sim_rough;
        strncpy(g_nc_visual_status,
                g_nc_visual_sim_rough ? "SIM rough on" : "SIM rough hidden",
                sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_DIMS:
        g_nc_visual_show_dims = !g_nc_visual_show_dims;
        strncpy(g_nc_visual_status,
                g_nc_visual_show_dims ? "Dimensions on" : "Dimensions hidden",
                sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_SINGLE:
        if (g_nc_visual_mode == NC_MODE_MDI) {
            nc_visual_dispatch_footer_action(NC_FOOTER_ACTION_SEND);
        } else {
            nc_visual_run_step();
        }
        break;
    case NC_FOOTER_ACTION_FROM:
        nc_visual_run_arm(g_nc_visual_doc.cursor_line, "Run from");
        break;
    case NC_FOOTER_ACTION_HOLD:
        if (nc_run_toggle_hold()) {
            strncpy(g_nc_visual_status,
                    nc_run_hold() ? "RUN hold" : "RUN resumed",
                    sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "No active RUN", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_STOP:
        nc_run_stop();
        strncpy(g_nc_visual_status, "RUN stopped", sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_SEND:
        if (g_nc_visual_mode == NC_MODE_MDI) {
            size_t line = g_nc_visual_doc.cursor_line;
            if (nc_run_send_document_line(&g_nc_visual_doc, line)) {
                snprintf(g_nc_visual_status,
                         sizeof(g_nc_visual_status),
                         "MDI sent line %lu",
                         (unsigned long)(line + 1u));
            } else {
                strncpy(g_nc_visual_status, "MDI line skipped", sizeof(g_nc_visual_status) - 1);
            }
        } else {
            strncpy(g_nc_visual_status, "Send is stubbed", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_CLEAR:
        if (g_nc_visual_mode == NC_MODE_MDI) {
            nc_document_init(&g_nc_visual_doc);
            (void)nc_insert_line(&g_nc_visual_doc, 0, "");
            strncpy(g_nc_visual_doc.path, NC_MDI_PATH, sizeof(g_nc_visual_doc.path) - 1);
            g_nc_visual_doc.path[sizeof(g_nc_visual_doc.path) - 1] = '\0';
            (void)nc_save_file(&g_nc_visual_doc, NC_MDI_PATH);
            nc_state_remember_path(NC_MODE_MDI, NC_MDI_PATH);
            nc_state_save();
            strncpy(g_nc_visual_status, "MDI cleared", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "Clear is stubbed", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_FIELD:
        if (nc_select_next_word(&g_nc_visual_doc) == NC_OK) {
            strncpy(g_nc_visual_status, "Next word", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "No editable word on this line", sizeof(g_nc_visual_status) - 1);
        }
        break;
    default:
        snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "%s action stub", nc_menu_mode_name(g_nc_visual_mode));
        break;
    }
}

static void nc_visual_cycle_mode(void)
{
    nc_visual_save_current_if_file();
    nc_visual_set_mode((nc_mode_t)((g_nc_visual_mode + 1) % NC_MODE_COUNT));
    g_nc_visual_selected_action = NC_FOOTER_ACTION_NONE;
    if (nc_visual_uses_file()) {
        nc_files_set_active(false);
        nc_text_edit_clear(&g_nc_visual_edit);
        if (nc_state_load_document(g_nc_visual_mode, &g_nc_visual_doc)) {
            snprintf(g_nc_visual_status,
                     sizeof(g_nc_visual_status),
                     "%s: %.48s",
                     nc_menu_mode_name(g_nc_visual_mode),
                     g_nc_visual_doc.path);
        } else {
            nc_document_init(&g_nc_visual_doc);
            snprintf(g_nc_visual_status,
                     sizeof(g_nc_visual_status),
                     "%s has no file",
                     nc_menu_mode_name(g_nc_visual_mode));
        }
    } else {
        snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Mode: %s", nc_menu_mode_name(g_nc_visual_mode));
    }
}

static void nc_visual_draw_text_clip(int x,
                                     int y,
                                     const char *text,
                                     int cols,
                                     lvds_color_t fg,
                                     lvds_color_t bg,
                                     int font)
{
    lvds_draw_text_clip(x, y, text ? text : "", cols, fg, bg, font);
}

static void nc_visual_serial_selected_line(void)
{
    size_t line = nc_visual_code_line();
    const char *text = "";

    if (line < g_nc_visual_doc.line_count) {
        text = g_nc_visual_doc.lines[line].text;
    }
    grbl_stream_printf(__romstr__("[MSG:NC SELECT %s %lu: %.96s]\r\n"),
                       nc_menu_mode_name(g_nc_visual_mode),
                       (unsigned long)(line + 1),
                       text);
}

static void nc_visual_serial_selected_file(void)
{
    int selected = nc_files_selected();
    const char *name = nc_files_name(selected);
    char path[NC_PATH_MAX];

    if (nc_files_selected_path(path, sizeof(path))) {
        grbl_stream_printf(__romstr__("[MSG:NC FILE %d: %.96s]\r\n"), selected + 1, path);
    } else {
        grbl_stream_printf(__romstr__("[MSG:NC FILE %d: %.96s]\r\n"), selected + 1, name);
    }
}

static bool nc_visual_line_is_contour_detail(const nc_document_t *doc, size_t index)
{
    size_t i;
    bool in_region = false;

    if (!doc || index >= doc->line_count) {
        return false;
    }

    for (i = 0; i <= index; i++) {
        const char *line = doc->lines[i].text;
        g7x_cycle_t cycle = g7x_cycle_from_line(line);

        if (cycle != G7X_CYCLE_NONE) {
            in_region = true;
            continue;
        }
        if (g7x_contour_cmd_from_line(line) == G7X_CONTOUR_END) {
            if (i == index) {
                return false;
            }
            in_region = false;
            continue;
        }
    }

    return in_region && g7x_contour_cmd_from_line(doc->lines[index].text) != G7X_CONTOUR_NONE;
}

static void nc_visual_draw_tool_screen(void)
{
    int tool_count;
    int selected_tool;
    int selected_line;
    int first_tool = 0;
    int row;
    int detail_y = 354;
    char buf[80];
    const char *tool_file;
    char active_letter = '\0';
    int active_line = -1;

    tool_count = nc_visual_find_tool_line(nc_visual_selected_tool_index(), NULL);
    selected_tool = nc_visual_selected_tool_index();
    (void)nc_visual_find_tool_line(selected_tool, &selected_line);
    (void)nc_visual_selected_tool_word(&active_letter, &active_line);
    if (selected_tool >= 8) {
        first_tool = selected_tool - 7;
    }

    lvds_draw_fill_rect(18, 58, LVDS_HSTX_WIDTH - 36, 474, NC_VISUAL_BG);
    tool_file = nc_visual_file_basename(g_nc_visual_doc.path);
    nc_visual_draw_text_clip(28, 68, "TOOL TABLE", 16, NC_VISUAL_TEXT, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(432, 68, tool_file, 28, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "%d tools", tool_count);
    nc_visual_draw_text_clip(674, 68, buf, 12, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);

    nc_visual_draw_text_clip(72, 96, "T", 4, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(120, 96, "RADIUS", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(190, 96, "ORIENT", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(260, 96, "FEED", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(326, 96, "FF", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(392, 96, "DOC", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(458, 96, "FDOC", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(524, 96, "RPM", 7, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(604, 96, "XOFF", 7, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(684, 96, "ZOFF", 7, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    lvds_draw_line(28, 118, LVDS_HSTX_WIDTH - 28, 118, NC_VISUAL_DIM);

    if (tool_count == 0) {
        nc_visual_draw_text_clip(44, 146, "No tool rows in this NC file. Press 1 to add T1.", 64,
                                 NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    }

    for (row = 0; row < 8 && first_tool + row < tool_count; row++) {
        int tool_line = -1;
        int y = 132 + row * 26;
        bool selected = (first_tool + row) == selected_tool;
        lvds_color_t bg = selected ? NC_VISUAL_SELECT : NC_VISUAL_BG;
        lvds_color_t fg = selected ? NC_VISUAL_LINE_NO_SELECTED : NC_VISUAL_TEXT;
        nc_tool_t tool;
        const char *line;

        (void)nc_visual_find_tool_line(first_tool + row, &tool_line);
        if (tool_line < 0) {
            continue;
        }
        line = g_nc_visual_doc.lines[tool_line].text;
        (void)nc_tool_from_line(line, &tool);
        if (selected) {
            lvds_draw_fill_rect(24, y - 4, LVDS_HSTX_WIDTH - 48, 24, bg);
        }
        nc_visual_draw_tool_glyph_centered(30, y - 3, 32, 18, &tool, bg, selected);
        nc_visual_draw_tool_cell(line, 'T', 72, y, 4, fg, bg, tool_line == active_line && active_letter == 'T');
        nc_visual_draw_tool_cell(line, 'R', 120, y, 6, fg, bg, tool_line == active_line && active_letter == 'R');
        nc_visual_draw_tool_cell(line, 'O', 190, y, 6, fg, bg, tool_line == active_line && active_letter == 'O');
        nc_visual_draw_tool_cell(line, 'F', 260, y, 6, fg, bg, tool_line == active_line && active_letter == 'F');
        nc_visual_draw_tool_cell(line, 'Q', 326, y, 6, fg, bg, tool_line == active_line && active_letter == 'Q');
        nc_visual_draw_tool_cell(line, 'D', 392, y, 6, fg, bg, tool_line == active_line && active_letter == 'D');
        nc_visual_draw_tool_cell(line, 'E', 458, y, 6, fg, bg, tool_line == active_line && active_letter == 'E');
        nc_visual_draw_tool_cell(line, 'S', 524, y, 7, fg, bg, tool_line == active_line && active_letter == 'S');
        nc_visual_draw_tool_cell(line, 'X', 604, y, 7, fg, bg, tool_line == active_line && active_letter == 'X');
        nc_visual_draw_tool_cell(line, 'Z', 684, y, 7, fg, bg, tool_line == active_line && active_letter == 'Z');
    }

    lvds_draw_line(18, detail_y, LVDS_HSTX_WIDTH - 18, detail_y, NC_VISUAL_DIM);
    nc_visual_draw_text_clip(38, detail_y + 12, "Tool tip", 12, NC_VISUAL_TEXT, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    lvds_draw_line(52, detail_y + 76, 142, detail_y + 76, NC_VISUAL_DIM);
    lvds_draw_line(96, detail_y + 34, 96, detail_y + 120, NC_VISUAL_DIM);
    nc_visual_draw_text_clip(102, detail_y + 34, "X0", 4, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_SMALL);
    nc_visual_draw_text_clip(122, detail_y + 82, "Z0", 4, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_SMALL);

    if (selected_line >= 0) {
        nc_tool_t tool;
        const char *line = g_nc_visual_doc.lines[selected_line].text;

        (void)nc_tool_from_line(line, &tool);
        nc_visual_draw_tool_glyph(96, detail_y + 76, 44, &tool, NC_VISUAL_BG, false);
        snprintf(buf, sizeof(buf), "Line %d: %.48s", selected_line + 1, line);
        nc_visual_draw_text_clip(170, detail_y + 18, buf, 70, NC_VISUAL_TEXT, NC_VISUAL_BG, LVDS_FONT_NORMAL);
        nc_visual_draw_tool_param(line, 'T', "T", 170, detail_y + 44, 8, selected_line == active_line && active_letter == 'T');
        nc_visual_draw_tool_param(line, 'O', "Orient", 170, detail_y + 64, 8, selected_line == active_line && active_letter == 'O');
        nc_visual_draw_tool_param(line, 'R', "Radius", 170, detail_y + 84, 8, selected_line == active_line && active_letter == 'R');
        nc_visual_draw_tool_param(line, 'D', "DOC", 170, detail_y + 104, 8, selected_line == active_line && active_letter == 'D');
        nc_visual_draw_tool_param(line, 'E', "FDOC", 400, detail_y + 44, 8, selected_line == active_line && active_letter == 'E');
        nc_visual_draw_tool_param(line, 'F', "FEED", 400, detail_y + 64, 8, selected_line == active_line && active_letter == 'F');
        nc_visual_draw_tool_param(line, 'Q', "F_FEED", 400, detail_y + 84, 8, selected_line == active_line && active_letter == 'Q');
        nc_visual_draw_tool_param(line, 'S', "RPM", 400, detail_y + 104, 8, selected_line == active_line && active_letter == 'S');
        nc_visual_draw_tool_param(line, 'X', "XOFF", 618, detail_y + 44, 7, selected_line == active_line && active_letter == 'X');
        nc_visual_draw_tool_param(line, 'Z', "ZOFF", 618, detail_y + 64, 7, selected_line == active_line && active_letter == 'Z');
    }
}

static const char *nc_visual_exec_state_text(uint8_t state)
{
    switch (state) {
    case EXEC_HOLD: return "HOLD";
    case EXEC_HOMING: return "HOME";
    case EXEC_JOG: return "JOG";
    case EXEC_RUN: return "RUN";
    case EXEC_LIMITS:
    case EXEC_POSITION_MAYBE_LOST:
    case EXEC_KILL:
        return "ALARM";
    default:
        return "IDLE";
    }
}

static void nc_visual_draw_header(const nc_snapshot_t *s)
{
    char buf[96];
    char fps[72];
    char preview[56];
    const nc_runtime_state_t *runtime = s ? &s->runtime : 0;
    const char *state_text = nc_visual_exec_state_text(runtime ? runtime->exec_state : EXEC_IDLE);

    lvds_draw_fill_rect(0, 0, LVDS_HSTX_WIDTH, 44, NC_VISUAL_HEADER);
    snprintf(buf,
             sizeof(buf),
             "%-5s X:%7.3f Z:%7.3f F:%5.1f S:%-5u",
             state_text,
             (double)(runtime ? runtime->x : 0.0f),
             (double)(runtime ? runtime->z : 0.0f),
             (double)(runtime ? runtime->feed : 0.0f),
             runtime ? runtime->spindle : 0);
    lvds_draw_text(12, 10, buf, NC_VISUAL_TEXT, NC_VISUAL_HEADER, LVDS_FONT_LARGE);
    snprintf(fps,
             sizeof(fps),
             "%u S%u H%u V%u R%u F%u P%u T%u",
             (unsigned)g_nc_visual_fps,
             (unsigned)g_nc_visual_stat_snapshot_ms,
             (unsigned)g_nc_visual_stat_header_ms,
             (unsigned)g_nc_visual_stat_preview_ms,
             (unsigned)g_nc_visual_stat_body_ms,
             (unsigned)g_nc_visual_stat_footer_ms,
             (unsigned)g_nc_visual_stat_present_ms,
             (unsigned)g_nc_visual_stat_total_ms);
    lvds_draw_text(LVDS_HSTX_WIDTH - lvds_draw_text_width(fps, LVDS_FONT_NORMAL) - 12,
                   13,
                   fps,
                   NC_VISUAL_DIM,
                   NC_VISUAL_HEADER,
                   LVDS_FONT_NORMAL);
    snprintf(preview,
             sizeof(preview),
             "C%u B%u S%u G%u L%u",
             (unsigned)g_nc_visual_stat_preview_collect_ms,
             (unsigned)g_nc_visual_stat_preview_clear_ms,
             (unsigned)g_nc_visual_stat_preview_stock_ms,
             (unsigned)g_nc_visual_stat_preview_geom_ms,
             (unsigned)g_nc_visual_stat_preview_tool_ms);
    lvds_draw_text(LVDS_HSTX_WIDTH - lvds_draw_text_width(preview, LVDS_FONT_SMALL) - 12,
                   30,
                   preview,
                   NC_VISUAL_DIM,
                   NC_VISUAL_HEADER,
                   LVDS_FONT_SMALL);
}

static void nc_visual_draw_snapshot(const nc_snapshot_t *s)
{
    int row;
    int line_cols = (NC_RIGHT_PANE_W - NC_LINE_TEXT_X_PAD) / NC_VISUAL_CHAR_W;
    char buf[80];
    char footer_text[160];
    bool sim_full = !nc_files_active() && g_nc_visual_mode == NC_MODE_SIM;
    bool split = nc_files_active() || nc_visual_is_code_view();
    uint32_t t0;
    uint32_t t1;
    uint32_t t2;
    uint32_t t3;
    uint32_t t4;

    lvds_draw_fill_rect(0, 0, LVDS_HSTX_WIDTH, LVDS_HSTX_HEIGHT, NC_VISUAL_BG);
    t0 = mcu_micros();
    nc_visual_draw_header(s);
    t1 = mcu_micros();

    if (sim_full) {
        nc_visual_draw_thin_preview(&g_nc_visual_doc, &s->runtime, 20, 58, LVDS_HSTX_WIDTH - 40, 474, true);
    } else if (split) {
        nc_visual_draw_thin_preview(&g_nc_visual_doc, &s->runtime, NC_LEFT_PANE_X, 58, NC_LEFT_PANE_W, 474, true);
        lvds_draw_line(NC_SPLIT_X, 58, NC_SPLIT_X, 532, NC_VISUAL_DIM);
        lvds_draw_fill_rect(NC_RIGHT_PANE_X, 58, NC_RIGHT_PANE_W, 474, NC_VISUAL_BG);
    } else {
        lvds_draw_fill_rect(20, 62, LVDS_HSTX_WIDTH - 40, 378, NC_VISUAL_PANEL);
        lvds_draw_rect(20, 62, LVDS_HSTX_WIDTH - 40, 378, NC_VISUAL_DIM);
    }
    t2 = mcu_micros();

    if (nc_files_active()) {
        int count = nc_files_count();
        int selected_file = nc_files_selected();
        int first_file = 0;
        const int visible_files = 14;
        if (selected_file >= visible_files) {
            first_file = selected_file - visible_files + 1;
        }
        nc_visual_draw_text_clip(NC_RIGHT_PANE_X + 12, 68, "NC FILES", 18, NC_VISUAL_ACCENT, NC_VISUAL_BG, LVDS_FONT_NORMAL);
        nc_visual_draw_text_clip(NC_RIGHT_PANE_X + 96, 68, nc_files_cwd(), 32, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
        if (count > 0) {
            snprintf(buf, sizeof(buf), "%d/%d", selected_file + 1, count);
            nc_visual_draw_text_clip(NC_RIGHT_PANE_X + NC_RIGHT_PANE_W - 70, 68, buf, 8, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
        }
        if (!nc_files_ready()) {
            nc_visual_draw_text_clip(NC_RIGHT_PANE_X + 16, 106, "File list not ready", 36, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
        } else if (!count) {
            nc_visual_draw_text_clip(NC_RIGHT_PANE_X + 16, 106, "No NC files found", 36, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
        }
        for (row = 0; row < count - first_file && row < visible_files; row++) {
            int file_index = first_file + row;
            int y = 104 + row * 28;
            bool selected = (file_index == selected_file);
            lvds_color_t bg = selected ? NC_VISUAL_SELECT : NC_VISUAL_BG;
            const char *name = nc_files_name(file_index);
            bool is_dir = nc_files_is_dir(file_index);
            snprintf(buf, sizeof(buf), "%c %s", is_dir ? '/' : ' ', name);
            if (selected) {
                lvds_draw_fill_rect(NC_RIGHT_PANE_X + 4, y - 4, NC_RIGHT_PANE_W - 8, 26, bg);
            }
            nc_visual_draw_text_clip(NC_RIGHT_PANE_X + 16,
                                     y,
                                     buf,
                                     line_cols,
                                     selected ? NC_VISUAL_TEXT : NC_VISUAL_DIM,
                                     bg,
                                     LVDS_FONT_NORMAL);
        }
        if (g_nc_visual_new_file_active) {
            snprintf(buf,
                     sizeof(buf),
                     "NEW: %s%s  # OK  * DEL  A CANCEL",
                     g_nc_visual_new_file_name[0] ? g_nc_visual_new_file_name : "_",
                     nc_visual_new_file_ext());
            lvds_draw_fill_rect(NC_RIGHT_PANE_X + 8, 506, NC_RIGHT_PANE_W - 16, 22, NC_VISUAL_BG);
            nc_visual_draw_text_clip(NC_RIGHT_PANE_X + 16,
                                     508,
                                     buf,
                                     line_cols,
                                     NC_VISUAL_TEXT,
                                     NC_VISUAL_BG,
                                     LVDS_FONT_NORMAL);
        }
    } else if (sim_full) {
        const int pane_div_x = LVDS_HSTX_WIDTH / 2;
        const int pane_x = pane_div_x + 18;
        const int pane_w = LVDS_HSTX_WIDTH - pane_x - 22;
        const int pane_y = 64;
        lvds_draw_fill_rect(pane_x - 8, pane_y - 4, pane_w + 16, 74, NC_VISUAL_PREVIEW_BG);
        lvds_draw_line(pane_div_x, pane_y, pane_div_x, pane_y + 66, NC_VISUAL_DIM);
        nc_visual_draw_sim_code_pane(&g_nc_visual_doc,
                                     pane_x,
                                     pane_y,
                                     pane_w);
    } else if (g_nc_visual_mode == NC_MODE_TOOLS) {
        nc_visual_draw_tool_screen();
    } else if (nc_visual_is_code_view()) {
        char path_line[NC_PATH_MAX + 4];

        snprintf(path_line,
                 sizeof(path_line),
                 "%s%s",
                 s->path[0] ? s->path : "(no file)",
                 s->dirty ? " *" : "");
        lvds_draw_text(NC_RIGHT_PANE_X + NC_LINE_NO_X_PAD,
                       62,
                       "   ",
                       NC_VISUAL_DIM,
                       NC_VISUAL_BG,
                       LVDS_FONT_NORMAL);
        nc_visual_draw_text_clip(NC_RIGHT_PANE_X + NC_LINE_TEXT_X_PAD,
                                 62,
                                 path_line,
                                 line_cols,
                                 NC_VISUAL_DIM,
                                 NC_VISUAL_BG,
                                 LVDS_FONT_NORMAL);
        for (row = 0; row < NC_MAX_VISIBLE_LINES; row++) {
            int y = 86 + row * NC_VISUAL_ROW_H;
            size_t line_index = s->first_line + (size_t)row;
            bool selected = line_index == nc_visual_code_line();
            char display_line[NC_MAX_LINE_LEN + 2];
            const char *line_text = s->lines[row];
            int word_start = nc_visual_can_edit_code() ? s->selected_word_start : -1;
            int word_end = nc_visual_can_edit_code() ? s->selected_word_end : -1;
            if (g_nc_visual_mode == NC_MODE_SIM) {
                nc_emit_result_t er = nc_emit_source_line(&g_nc_visual_doc,
                                                          line_index,
                                                          display_line,
                                                          sizeof(display_line));
                line_text = er == NC_EMIT_LINE ? display_line : "(skip)";
            }
            if (line_text[0] != '\t' &&
                line_text[0] != ' ' &&
                g_nc_visual_mode != NC_MODE_SIM &&
                nc_visual_line_is_contour_detail(&g_nc_visual_doc, line_index)) {
                display_line[0] = '\t';
                strncpy(display_line + 1, line_text, sizeof(display_line) - 2);
                display_line[sizeof(display_line) - 1] = '\0';
                line_text = display_line;
                if (word_start >= 0) word_start++;
                if (word_end >= 0) word_end++;
            }
            if (selected &&
                row > 0 &&
                nc_visual_can_edit_code() &&
                g_nc_visual_mode != NC_MODE_RUN &&
                s->selected_label[0]) {
                int hint_y = y - NC_VISUAL_ROW_H;
                lvds_draw_fill_rect(NC_RIGHT_PANE_X + 2, hint_y - 3, NC_RIGHT_PANE_W - 4, NC_VISUAL_ROW_H, NC_VISUAL_BG);
                snprintf(buf, sizeof(buf), "%c  %s", g_nc_visual_doc.selected_word >= 0 ? '>' : ' ', s->selected_label);
                nc_visual_draw_text_clip(NC_RIGHT_PANE_X + NC_LINE_TEXT_X_PAD,
                                         hint_y,
                                         buf,
                                         line_cols,
                                         NC_VISUAL_ACCENT,
                                         NC_VISUAL_BG,
                                         LVDS_FONT_NORMAL);
            }
            snprintf(buf, sizeof(buf), "%3lu", (unsigned long)(s->first_line + (size_t)row + 1));
            lvds_draw_text(NC_RIGHT_PANE_X + NC_LINE_NO_X_PAD, y, buf, selected ? NC_VISUAL_LINE_NO_SELECTED : NC_VISUAL_DIM,
                           selected ? NC_VISUAL_SELECT : NC_VISUAL_BG,
                           LVDS_FONT_NORMAL);
            nc_text_draw_line_with_word(line_text,
                                        NC_RIGHT_PANE_X + NC_LINE_TEXT_X_PAD,
                                        y,
                                        line_cols,
                                        NC_VISUAL_CHAR_W,
                                        NC_VISUAL_ROW_H,
                                        word_start,
                                        word_end,
                                        selected,
                                        NC_VISUAL_TEXT,
                                        NC_VISUAL_DIM,
                                        NC_VISUAL_BG,
                                        NC_VISUAL_SELECT,
                                        NC_VISUAL_WORD_FG,
                                        NC_VISUAL_WORD_BG);
        }
    } else {
        snprintf(buf, sizeof(buf), "%s screen stub", nc_menu_mode_name(g_nc_visual_mode));
        lvds_draw_text(42, 86, buf, NC_VISUAL_TEXT, NC_VISUAL_PANEL, LVDS_FONT_LARGE);
        nc_visual_draw_text_clip(42, 130,
                                 "A cycles modes. Footer actions are fixed per mode.",
                                 70,
                                 NC_VISUAL_DIM,
                                 NC_VISUAL_PANEL,
                                 LVDS_FONT_NORMAL);
    }

    if (nc_text_edit_active(&g_nc_visual_edit)) {
        snprintf(buf, sizeof(buf), "EDIT %s", nc_text_edit_buffer(&g_nc_visual_edit));
        lvds_draw_fill_rect(20, 526, LVDS_HSTX_WIDTH - 40, 18, NC_VISUAL_BG);
        nc_visual_draw_text_clip(34, 526, buf, 70, NC_VISUAL_ACCENT, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    }
    t3 = mcu_micros();

    nc_visual_footer_text(footer_text, sizeof(footer_text));
    nc_visual_draw_footer_status(g_nc_visual_status[0] ? g_nc_visual_status : "NC bring-up",
                                 footer_text);
    t4 = mcu_micros();
    g_nc_visual_frame_header_us += t1 - t0;
    g_nc_visual_frame_preview_us += t2 - t1;
    g_nc_visual_frame_body_us += t3 - t2;
    g_nc_visual_frame_footer_us += t4 - t3;
}

static void nc_visual_draw_live_snapshot(const nc_snapshot_t *s)
{
    uint32_t t0;
    uint32_t t1;
    uint32_t t2;

    if (!s) {
        return;
    }
    t0 = mcu_micros();
    nc_visual_draw_header(s);
    t1 = mcu_micros();
    nc_visual_draw_thin_preview(&g_nc_visual_doc, &s->runtime, NC_LEFT_PANE_X, 58, NC_LEFT_PANE_W, 474, false);
    t2 = mcu_micros();
    g_nc_visual_frame_header_us += t1 - t0;
    g_nc_visual_frame_preview_us += t2 - t1;
}

void nc_visual_init(void)
{
    nc_palette_init();
    nc_files_init();
    nc_run_init();
    nc_state_init();
    g_nc_visual_mode = nc_state_mode();
    if (g_nc_visual_mode < 0 || g_nc_visual_mode >= NC_MODE_COUNT) {
        g_nc_visual_mode = NC_MODE_PROGRAM;
    }
    if (nc_visual_uses_file() &&
        nc_state_load_document(g_nc_visual_mode, &g_nc_visual_doc)) {
        snprintf(g_nc_visual_status,
                 sizeof(g_nc_visual_status),
                 "%s: %.48s",
                 nc_menu_mode_name(g_nc_visual_mode),
                 g_nc_visual_doc.path);
        g_nc_visual_dirty = true;
    } else if (!nc_state_load_document(NC_MODE_PROGRAM, &g_nc_visual_doc)) {
        nc_visual_seed_demo();
        nc_state_remember_path(NC_MODE_PROGRAM, "");
        nc_state_save();
    } else {
        snprintf(g_nc_visual_status,
                 sizeof(g_nc_visual_status),
                 "%s: %.48s",
                 nc_menu_mode_name(g_nc_visual_mode),
                 g_nc_visual_doc.path);
        g_nc_visual_dirty = true;
    }
    lvds_hstx_clear(NC_VISUAL_BG);
    lvds_draw_text(24, 82, "Waiting for NC snapshot", NC_VISUAL_TEXT, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    lvds_hstx_present();
}

void nc_visual_handle_key(nc_visual_key_t key)
{
    uint8_t footer_action;

    if (nc_visual_new_file_handle_key(key)) {
        g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
        g_nc_visual_dirty = true;
        return;
    }

    if (key == NC_VISUAL_KEY_MODE) {
        nc_text_edit_clear(&g_nc_visual_edit);
        g_nc_visual_new_file_active = false;
        nc_visual_cycle_mode();
        g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
        g_nc_visual_dirty = true;
        return;
    }

    if (nc_visual_handle_selected_word_edit(key)) {
        g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
        g_nc_visual_dirty = true;
        return;
    }

    if ((key == NC_VISUAL_KEY_PREV || key == NC_VISUAL_KEY_NEXT) &&
        (nc_files_active() ||
         nc_visual_is_code_view() ||
         g_nc_visual_mode == NC_MODE_TOOLS)) {
        nc_text_edit_clear(&g_nc_visual_edit);
        nc_visual_dispatch_footer_action(key == NC_VISUAL_KEY_PREV ? NC_FOOTER_ACTION_BACK : NC_FOOTER_ACTION_STEP);
        g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
        g_nc_visual_dirty = true;
        return;
    }

    footer_action = nc_visual_footer_action_for_key(key);
    if (footer_action != NC_FOOTER_ACTION_NONE) {
        nc_visual_dispatch_footer_action(footer_action);
        g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
        g_nc_visual_dirty = true;
        return;
    }

    if (!nc_visual_can_edit_code()) {
        strncpy(g_nc_visual_status, "Stub mode. Press A for next mode.", sizeof(g_nc_visual_status) - 1);
        g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
        g_nc_visual_dirty = true;
        return;
    }

    switch (key) {
    case NC_VISUAL_KEY_PREV:
        nc_text_edit_clear(&g_nc_visual_edit);
        nc_cursor_up(&g_nc_visual_doc);
        strncpy(g_nc_visual_status, "Line up", sizeof(g_nc_visual_status) - 1);
        break;
    case NC_VISUAL_KEY_NEXT:
        nc_text_edit_clear(&g_nc_visual_edit);
        nc_cursor_down(&g_nc_visual_doc);
        strncpy(g_nc_visual_status, "Line down", sizeof(g_nc_visual_status) - 1);
        break;
    case NC_VISUAL_KEY_ACCEPT:
    case NC_VISUAL_KEY_FINISH:
        if (nc_select_next_word(&g_nc_visual_doc) == NC_OK) {
            strncpy(g_nc_visual_status, "Next word", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "No editable word on this line", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_VISUAL_KEY_BACKSPACE:
        if (nc_select_prev_word(&g_nc_visual_doc) == NC_OK) {
            strncpy(g_nc_visual_status, "Previous word", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "No editable word on this line", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_VISUAL_KEY_CANCEL:
        nc_text_edit_clear(&g_nc_visual_edit);
        g_nc_visual_doc.selected_word = -1;
        strncpy(g_nc_visual_status, "Selection cleared", sizeof(g_nc_visual_status) - 1);
        break;
    default:
        break;
    }

    g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
    g_nc_visual_dirty = true;
}

bool nc_visual_dirty(void)
{
    return g_nc_visual_dirty;
}

bool nc_visual_periodic_needed(void)
{
    return g_nc_visual_mode == NC_MODE_MANUAL ||
           (g_nc_visual_mode == NC_MODE_RUN &&
            (nc_run_active() ||
             nc_run_hold() ||
             cnc_get_exec_state(EXEC_RUN | EXEC_HOLD) ||
             g_nc_visual_last_runtime_busy));
}

void nc_visual_draw(void)
{
    nc_snapshot_t snapshot;
    uint32_t t0;
    uint32_t t1;
    uint32_t t2;
    uint32_t t3;
    bool full_draw;
    bool runtime_busy;
    size_t run_line;

    if (g_nc_visual_in_draw) {
        return;
    }

    g_nc_visual_in_draw = true;

    g_nc_visual_frame_header_us = 0;
    g_nc_visual_frame_preview_us = 0;
    g_nc_visual_frame_body_us = 0;
    g_nc_visual_frame_footer_us = 0;
    g_nc_visual_frame_preview_collect_us = 0;
    g_nc_visual_frame_preview_clear_us = 0;
    g_nc_visual_frame_preview_stock_us = 0;
    g_nc_visual_frame_preview_geom_us = 0;
    g_nc_visual_frame_preview_tool_us = 0;
    t0 = mcu_micros();
    nc_state_snapshot(&g_nc_visual_doc, &snapshot);
    t1 = mcu_micros();
    run_line = nc_run_line();
    runtime_busy = nc_visual_runtime_busy(&snapshot.runtime);
    full_draw = g_nc_visual_dirty ||
                g_nc_visual_mode != NC_MODE_RUN ||
                (g_nc_visual_last_runtime_busy && !runtime_busy) ||
                (!runtime_busy && !nc_run_active() && !nc_run_hold()) ||
                run_line != g_nc_visual_last_draw_run_line;
    if (full_draw) {
        g_nc_live_tool_rect_valid = false;
        g_nc_live_preview_cache_valid = false;
        nc_visual_draw_snapshot(&snapshot);
    } else {
        nc_visual_draw_live_snapshot(&snapshot);
    }
    t2 = mcu_micros();
    lvds_hstx_present();
    t3 = mcu_micros();
    nc_visual_fps_tick(t1 - t0, t2 - t1, t3 - t2, t3 - t0);

    g_nc_visual_last_draw_run_line = g_nc_visual_mode == NC_MODE_RUN ? run_line : (size_t)-1;
    g_nc_visual_last_runtime_busy = g_nc_visual_mode == NC_MODE_RUN && runtime_busy;
    g_nc_visual_dirty = false;
    g_nc_visual_in_draw = false;
}

static void nc_visual_fps_tick(uint32_t snapshot_us,
                               uint32_t draw_us,
                               uint32_t present_us,
                               uint32_t total_us)
{
    uint32_t now = mcu_millis();
    uint32_t elapsed;

    if (!g_nc_visual_fps_last_ms) {
        g_nc_visual_fps_last_ms = now;
    }
    g_nc_visual_fps_frames++;
    g_nc_visual_acc_snapshot_us += snapshot_us;
    g_nc_visual_acc_draw_us += draw_us;
    g_nc_visual_acc_present_us += present_us;
    g_nc_visual_acc_total_us += total_us;
    g_nc_visual_acc_header_us += g_nc_visual_frame_header_us;
    g_nc_visual_acc_preview_us += g_nc_visual_frame_preview_us;
    g_nc_visual_acc_body_us += g_nc_visual_frame_body_us;
    g_nc_visual_acc_footer_us += g_nc_visual_frame_footer_us;
    g_nc_visual_acc_preview_collect_us += g_nc_visual_frame_preview_collect_us;
    g_nc_visual_acc_preview_clear_us += g_nc_visual_frame_preview_clear_us;
    g_nc_visual_acc_preview_stock_us += g_nc_visual_frame_preview_stock_us;
    g_nc_visual_acc_preview_geom_us += g_nc_visual_frame_preview_geom_us;
    g_nc_visual_acc_preview_tool_us += g_nc_visual_frame_preview_tool_us;
    elapsed = now - g_nc_visual_fps_last_ms;
    if (elapsed >= 1000u) {
        g_nc_visual_fps = (uint16_t)(((uint32_t)g_nc_visual_fps_frames * 1000u) / elapsed);
        if (g_nc_visual_fps_frames) {
            g_nc_visual_stat_snapshot_ms = (uint16_t)((g_nc_visual_acc_snapshot_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_draw_ms = (uint16_t)((g_nc_visual_acc_draw_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_present_ms = (uint16_t)((g_nc_visual_acc_present_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_total_ms = (uint16_t)((g_nc_visual_acc_total_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_header_ms = (uint16_t)((g_nc_visual_acc_header_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_preview_ms = (uint16_t)((g_nc_visual_acc_preview_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_body_ms = (uint16_t)((g_nc_visual_acc_body_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_footer_ms = (uint16_t)((g_nc_visual_acc_footer_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_preview_collect_ms = (uint16_t)((g_nc_visual_acc_preview_collect_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_preview_clear_ms = (uint16_t)((g_nc_visual_acc_preview_clear_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_preview_stock_ms = (uint16_t)((g_nc_visual_acc_preview_stock_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_preview_geom_ms = (uint16_t)((g_nc_visual_acc_preview_geom_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_preview_tool_ms = (uint16_t)((g_nc_visual_acc_preview_tool_us / g_nc_visual_fps_frames + 500u) / 1000u);
        }
        g_nc_visual_fps_frames = 0;
        g_nc_visual_acc_snapshot_us = 0;
        g_nc_visual_acc_draw_us = 0;
        g_nc_visual_acc_present_us = 0;
        g_nc_visual_acc_total_us = 0;
        g_nc_visual_acc_header_us = 0;
        g_nc_visual_acc_preview_us = 0;
        g_nc_visual_acc_body_us = 0;
        g_nc_visual_acc_footer_us = 0;
        g_nc_visual_acc_preview_collect_us = 0;
        g_nc_visual_acc_preview_clear_us = 0;
        g_nc_visual_acc_preview_stock_us = 0;
        g_nc_visual_acc_preview_geom_us = 0;
        g_nc_visual_acc_preview_tool_us = 0;
        g_nc_visual_fps_last_ms = now;
    }
}
