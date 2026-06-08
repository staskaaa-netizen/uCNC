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
#include "../g71_g72/g71_g72.h"
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

#define NC_PREVIEW_ARC_MAX_STEPS 32
#define NC_PREVIEW_CONTOUR_MAX 48
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
static void nc_visual_draw_tool_glyph(int tip_x,
                                      int tip_y,
                                      int size,
                                      const nc_tool_t *tool,
                                      lvds_color_t bg,
                                      bool selected);

static nc_mode_t g_nc_visual_mode = NC_MODE_MANUAL;
static uint8_t g_nc_visual_selected_action = NC_FOOTER_ACTION_NONE;

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
    int panel_y = y + h - panel_h - 10;
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

static void nc_visual_tool_edges(int orient, bool *left, bool *top, bool *right, bool *bottom)
{
    int o = nc_visual_tool_tip_digit(orient);

    if (left) *left = (o == 1 || o == 4 || o == 7 || o == 2 || o == 5 || o == 8);
    if (top) *top = (o == 7 || o == 8 || o == 9 || o == 4 || o == 5 || o == 6);
    if (right) *right = (o == 3 || o == 6 || o == 9 || o == 2 || o == 5 || o == 8);
    if (bottom) *bottom = (o == 1 || o == 2 || o == 3 || o == 4 || o == 5 || o == 6);
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
    lvds_color_t fill = selected ? NC_VISUAL_LINE_NO_SELECTED : NC_VISUAL_TOOL_FILL;

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
        lvds_draw_fill_rect(tip_x - size / 2, tip_y - size / 2, size, size, fill);
        lvds_draw_rect(tip_x - size / 2, tip_y - size / 2, size, size, edge);
        lvds_draw_line(tip_x - size / 2, tip_y + size / 2, tip_x + size / 2, tip_y - size / 2, edge);
        lvds_draw_fill_ellipse(tip_x, tip_y, rr, rr, edge);
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
    lvds_color_t value_fg = active ? NC_VISUAL_WORD_FG : NC_VISUAL_FOOTER_VALUE;
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

static void nc_visual_preview_label(int x, int y, const char *text, bool selected)
{
    lvds_color_t bg = selected ? NC_VISUAL_SELECT : NC_VISUAL_PREVIEW_BG;
    lvds_color_t fg = selected ? NC_VISUAL_LINE_NO_SELECTED : NC_VISUAL_FOOTER_VALUE;
    int w;

    if (!text || !text[0]) {
        return;
    }
    w = lvds_draw_text_width(text, LVDS_FONT_NORMAL) + 6;
    lvds_draw_fill_rect(x - 3, y - 2, w, 16, bg);
    lvds_draw_text(x, y, text, fg, bg, LVDS_FONT_NORMAL);
}

static void nc_visual_draw_chuck(int stock_left, int stock_top, int stock_w, int stock_h)
{
    int jaw_w = 26;
    int jaw_x = stock_left - jaw_w;
    int jaw_h = 58;
    int jaw_y = stock_top + stock_h - 18;
    int clamp_w = stock_w / 4;

    if (jaw_x < 0) {
        jaw_x = 0;
        jaw_w = stock_left;
    }
    if (clamp_w < 24) {
        clamp_w = 24;
    }
    if (clamp_w > stock_w) {
        clamp_w = stock_w;
    }

    lvds_draw_fill_rect(jaw_x, jaw_y, jaw_w, jaw_h, NC_VISUAL_PREVIEW_CHUCK);
    lvds_draw_fill_rect(stock_left, stock_top + stock_h + 1, clamp_w, 16, NC_VISUAL_PREVIEW_CHUCK);
    lvds_draw_text(jaw_x + 4,
                   jaw_y + 20,
                   "CH",
                   NC_VISUAL_PREVIEW_CHUCK_TEXT,
                   NC_VISUAL_PREVIEW_CHUCK,
                   LVDS_FONT_NORMAL);
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
    steps = nc_visual_clampi((int)(nc_visual_absf(sweep) * abs_r * 0.45f) + 6,
                             6,
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
    int width = 2;

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
        width = 1;
    } else if (segment == NC_PREVIEW_SEG_FINISH) {
        color = NC_VISUAL_PREVIEW_CUT;
        width = 3;
    }

    if (cmd == G7X_CONTOUR_ARC_CW || cmd == G7X_CONTOUR_ARC_CCW) {
        float r = 0.0f;
        if (!nc_sim_line_word_float(line, 'R', &r) ||
            !nc_visual_draw_explicit_arc(preview,
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
                                         width)) {
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
            (void)nc_visual_draw_emitted_motion_line(preview,
                                                     z0_x,
                                                     stock_w,
                                                     stock_top,
                                                     stock_h,
                                                     line,
                                                     segment,
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

static const char *nc_visual_run_state_text(void)
{
    if (g_nc_visual_mode != NC_MODE_RUN) {
        return nc_menu_mode_name(g_nc_visual_mode);
    }
    if (nc_run_hold()) {
        return "RUN HOLD";
    }
    if (nc_run_active()) {
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

    if (!preview || !runtime || !tool || !tool->valid) {
        return;
    }

    sx = nc_visual_preview_z(preview, z0_x, stock_w, runtime->z);
    sy = nc_visual_preview_x(preview, stock_top, stock_h, nc_live_runtime_x_to_diam(runtime->x));
    sx = nc_visual_clampi(sx, 0, LVDS_HSTX_WIDTH - 1);
    sy = nc_visual_clampi(sy, 44, LVDS_HSTX_HEIGHT - 1);

    lvds_draw_line(sx, stock_top - 18, sx, stock_top + stock_h + 18, NC_VISUAL_TOOL_CROSSHAIR);
    lvds_draw_line(z0_x - 4, sy, z0_x + stock_w + 4, sy, NC_VISUAL_TOOL_CROSSHAIR);
    lvds_draw_fill_ellipse(sx, sy, 2, 2, NC_VISUAL_TOOL_CROSSHAIR);
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

static void nc_live_stock_draw(int stock_left, int stock_top)
{
    int y;

    if (!g_nc_live_stock_mask || !g_nc_live_stock_ready) {
        return;
    }
    lvds_draw_fill_rect(stock_left, stock_top, g_nc_live_stock_w, g_nc_live_stock_h, NC_VISUAL_PREVIEW_BG);
    for (y = 0; y < g_nc_live_stock_h; y++) {
        const uint8_t *row = g_nc_live_stock_mask + ((size_t)y * NC_LIVE_STOCK_MAX_W);
        int x = 0;
        while (x < g_nc_live_stock_w) {
            int start;
            while (x < g_nc_live_stock_w && !row[x]) {
                x++;
            }
            start = x;
            while (x < g_nc_live_stock_w && row[x]) {
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
                                      int z0_x)
{
    bool live_run = false;

    if (runtime) {
        live_run = (runtime->exec_state & (EXEC_RUN | EXEC_HOLD)) != 0;
    }
    live_run = live_run || nc_run_active();

    if (!preview || !runtime || !live_run) {
        g_nc_live_stock_was_active = false;
        return false;
    }
    if (!g_nc_live_stock_was_active ||
        nc_live_stock_context_changed(preview, stock_w, stock_h)) {
        nc_live_stock_reset(preview, stock_w, stock_h);
    }
    g_nc_live_stock_was_active = true;
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
    nc_live_stock_update(preview, runtime, z0_x, stock_left, stock_w, stock_top, stock_h);
    nc_live_stock_draw(stock_left, stock_top);
    nc_visual_draw_chuck(stock_left, stock_top, stock_w, stock_h);
    lvds_draw_rect(stock_left, stock_top, stock_w, stock_h, NC_VISUAL_PREVIEW_FRAME);
    lvds_draw_line(z0_x, stock_top - 18, z0_x, stock_top + stock_h + 18, NC_VISUAL_DIM);
    lvds_draw_text(z0_x - 12, stock_top - 34, "Z0", NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_live_tool(preview, runtime, z0_x, stock_w, stock_top, stock_h, tool);
    return true;
}

static void nc_visual_draw_thin_preview(const nc_document_t *doc,
                                        const nc_runtime_state_t *runtime,
                                        int x,
                                        int y,
                                        int w,
                                        int h)
{
    nc_preview_info_t preview;
    int stock_w;
    int stock_h;
    int stock_left;
    int stock_top;
    int z0_x;
    int profile_left;
    int profile_right;
    int profile_top;
    int profile_bottom;
    float usable_w;
    float usable_h;
    float z_scale;
    float x_scale;
    float scale;
    float contour_z[NC_PREVIEW_CONTOUR_MAX];
    float contour_x[NC_PREVIEW_CONTOUR_MAX];
    size_t contour_line[NC_PREVIEW_CONTOUR_MAX];
    uint8_t contour_count = 0;
    size_t i;
    nc_tool_t tool;
    bool have_tool = false;
    size_t tool_line = doc && doc->line_count ? doc->cursor_line : 0;

    if (doc && g_nc_visual_mode == NC_MODE_RUN && nc_run_line() < doc->line_count) {
        tool_line = nc_run_line();
    }

    nc_sim_collect_preview(doc, &preview);
    lvds_draw_fill_rect(x, y, w, h, NC_VISUAL_PREVIEW_BG);
    nc_visual_draw_text_clip(x + 12,
                             y + 10,
                             nc_visual_run_state_text(),
                             18,
                             NC_VISUAL_ACCENT,
                             NC_VISUAL_PREVIEW_BG,
                             LVDS_FONT_NORMAL);

    usable_w = (float)(w - 72);
    usable_h = (float)(h - 84);
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
    if (doc && nc_tool_active_for_line(doc, tool_line, &tool)) {
        have_tool = true;
    }
    if (g_nc_visual_mode == NC_MODE_RUN && nc_visual_draw_live_stock(&preview,
                                                                      runtime,
                                                                      have_tool ? &tool : NULL,
                                                                      stock_left,
                                                                      stock_top,
                                                                      stock_w,
                                                                      stock_h,
                                                                      z0_x)) {
        return;
    }

    if (g_nc_visual_sim_stock || g_nc_visual_mode != NC_MODE_SIM) {
        lvds_draw_fill_rect(stock_left, stock_top, stock_w, stock_h, NC_VISUAL_PREVIEW_STOCK);
        if (preview.stock_i > 0.0f) {
            int id_h = nc_visual_preview_x(&preview, stock_top, stock_h, preview.stock_i) - stock_top;
            if (id_h > 0 && id_h < stock_h) {
                lvds_draw_fill_rect(stock_left, stock_top, stock_w, id_h, NC_VISUAL_PREVIEW_BG);
            }
        }
    }
    nc_visual_draw_chuck(stock_left, stock_top, stock_w, stock_h);
    lvds_draw_rect(stock_left, stock_top, stock_w, stock_h, NC_VISUAL_PREVIEW_FRAME);
    lvds_draw_line(z0_x, stock_top - 18, z0_x, stock_top + stock_h + 18, NC_VISUAL_DIM);
    lvds_draw_text(z0_x - 12, stock_top - 34, "Z0", NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_preview_tool_panel(x, y, w, h, have_tool ? &tool : NULL);
    memset(contour_z, 0, sizeof(contour_z));
    memset(contour_x, 0, sizeof(contour_x));

    if (doc) {
        for (i = 0; i < doc->line_count; i++) {
            const char *line = doc->lines[i].text;
            g7x_contour_cmd_t cmd = g7x_contour_cmd_from_line(line);
            float xw;
            float zw;

            if (cmd == G7X_CONTOUR_END) {
                break;
            }
            if (cmd == G7X_CONTOUR_NONE || contour_count >= NC_PREVIEW_CONTOUR_MAX) {
                continue;
            }
            if (!nc_sim_line_word_float(line, 'X', &xw) ||
                !nc_sim_line_word_float(line, 'Z', &zw)) {
                continue;
            }
            contour_z[contour_count] = zw;
            contour_x[contour_count] = xw;
            contour_line[contour_count] = i;
            contour_count++;
        }
    }

    profile_left = nc_visual_preview_z(&preview, z0_x, stock_w, preview.min_z);
    profile_right = nc_visual_preview_z(&preview, z0_x, stock_w, preview.max_z);
    profile_top = nc_visual_preview_x(&preview, stock_top, stock_h, preview.min_x);
    profile_bottom = nc_visual_preview_x(&preview, stock_top, stock_h, preview.max_x);
    if (profile_right > profile_left && profile_bottom > profile_top) {
        lvds_draw_rect(profile_left, profile_top, profile_right - profile_left, profile_bottom - profile_top, NC_VISUAL_DIM);
    }

    if (g_nc_visual_sim_path || g_nc_visual_mode != NC_MODE_SIM) {
        nc_visual_draw_emitted_preview(doc,
                                       &preview,
                                       z0_x,
                                       stock_w,
                                       stock_top,
                                       stock_h,
                                       96u);
    }
    for (i = 0; i < contour_count; i++) {
        char label[8];
        int lx = nc_visual_preview_z(&preview, z0_x, stock_w, contour_z[i]);
        int ly = nc_visual_preview_x(&preview, stock_top, stock_h, contour_x[i]);
        bool selected = doc && contour_line[i] == doc->cursor_line;

        snprintf(label, sizeof(label), "C%u", (unsigned)(i + 1u));
        nc_visual_preview_label(lx + 4, ly - 10, label, selected);
    }
    if (have_tool) {
        nc_visual_draw_live_tool(&preview, runtime, z0_x, stock_w, stock_top, stock_h, &tool);
        nc_visual_draw_preview_tool_panel(x, y, w, h, &tool);
    }
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
    if (!nc_run_start_stream(&g_nc_visual_doc, g_nc_visual_doc.cursor_line)) {
        strncpy(g_nc_visual_status, "RUN needs a program", sizeof(g_nc_visual_status) - 1);
        return;
    }
    g_nc_visual_doc.cursor_line = nc_run_line();
    g_nc_visual_doc.selected_word = -1;
    snprintf(g_nc_visual_status,
             sizeof(g_nc_visual_status),
             "RUN stream from %lu",
             (unsigned long)(g_nc_visual_doc.cursor_line + 1u));
}

static bool nc_visual_is_code_view(void)
{
    return g_nc_visual_mode == NC_MODE_PROGRAM ||
           g_nc_visual_mode == NC_MODE_SIM ||
           g_nc_visual_mode == NC_MODE_MDI ||
           g_nc_visual_mode == NC_MODE_RUN;
}

static bool nc_visual_uses_file(void)
{
    return nc_visual_is_code_view() || g_nc_visual_mode == NC_MODE_TOOLS;
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
    return g_nc_visual_mode == NC_MODE_RUN ? nc_run_line() : g_nc_visual_doc.cursor_line;
}

static void nc_visual_move_code_line(int delta)
{
    size_t line = nc_visual_code_line();

    if (!nc_visual_is_code_view() || g_nc_visual_doc.line_count == 0) {
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

static bool nc_visual_handle_selected_word_edit(nc_visual_key_t key)
{
    char key_char = nc_visual_key_char(key);

    if (nc_files_active() ||
        !nc_visual_can_edit_code() ||
        g_nc_visual_doc.selected_word < 0) {
        return false;
    }

    return nc_text_edit_handle_key(&g_nc_visual_doc,
                                   &g_nc_visual_edit,
                                   key_char,
                                   g_nc_visual_status,
                                   sizeof(g_nc_visual_status));
}

static void nc_visual_dispatch_footer_action(uint8_t action)
{
    g_nc_visual_selected_action = action;

    switch (action) {
    case NC_FOOTER_ACTION_FILES:
        nc_files_set_active(true);
        g_nc_visual_new_file_active = false;
        if (nc_files_refresh(NULL)) {
            snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Files: %s", nc_files_cwd());
        } else {
            strncpy(g_nc_visual_status, "File list unavailable", sizeof(g_nc_visual_status) - 1);
        }
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
        if (nc_insert_preset(&g_nc_visual_doc, NC_PRESET_OD) == NC_OK) {
            strncpy(g_nc_visual_status, "Inserted OD preset", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "OD preset failed", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_PRESET_ID:
        if (nc_insert_preset(&g_nc_visual_doc, NC_PRESET_ID) == NC_OK) {
            strncpy(g_nc_visual_status, "Inserted ID preset", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "ID preset failed", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_PRESET_FACE:
        if (nc_insert_preset(&g_nc_visual_doc, NC_PRESET_FACE) == NC_OK) {
            strncpy(g_nc_visual_status, "Inserted FACE preset", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "FACE preset failed", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_PRESET_LINE:
        if (nc_insert_preset(&g_nc_visual_doc, NC_PRESET_LINE) == NC_OK) {
            strncpy(g_nc_visual_status, "Inserted line preset", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "Line preset failed", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_PRESET_ARC:
        if (nc_insert_preset(&g_nc_visual_doc, NC_PRESET_ARC) == NC_OK) {
            strncpy(g_nc_visual_status, "Inserted arc preset", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "Arc preset failed", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_PRESET_SETUP:
        if (nc_insert_preset(&g_nc_visual_doc, NC_PRESET_SETUP) == NC_OK) {
            strncpy(g_nc_visual_status, "Inserted setup preset", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "Setup preset failed", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_PRESET_END:
        if (nc_insert_preset(&g_nc_visual_doc, NC_PRESET_END) == NC_OK) {
            strncpy(g_nc_visual_status, "Inserted G80", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "G80 preset failed", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_TOOL:
        if (g_nc_visual_mode != NC_MODE_TOOLS) {
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
            char emit[64];
            nc_emit_result_t er = nc_emit_source_line(&g_nc_visual_doc,
                                                      g_nc_visual_doc.cursor_line,
                                                      emit,
                                                      sizeof(emit));
            if (er == NC_EMIT_LINE) {
                snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "MDI send: %.46s", emit);
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
    nc_visual_draw_text_clip(116, 96, "R", 5, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(170, 96, "O", 5, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(224, 96, "FEED", 5, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(282, 96, "FF", 5, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(338, 96, "DOC", 5, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(394, 96, "FDOC", 5, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(454, 96, "RPM", 5, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(516, 96, "XOFF", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(584, 96, "ZOFF", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
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
        nc_visual_draw_tool_glyph(46, y + 8, 18, &tool, bg, selected);
        nc_visual_draw_tool_cell(line, 'T', 72, y, 4, fg, bg, tool_line == active_line && active_letter == 'T');
        nc_visual_draw_tool_cell(line, 'R', 116, y, 5, fg, bg, tool_line == active_line && active_letter == 'R');
        nc_visual_draw_tool_cell(line, 'O', 170, y, 5, fg, bg, tool_line == active_line && active_letter == 'O');
        nc_visual_draw_tool_cell(line, 'F', 224, y, 5, fg, bg, tool_line == active_line && active_letter == 'F');
        nc_visual_draw_tool_cell(line, 'Q', 282, y, 5, fg, bg, tool_line == active_line && active_letter == 'Q');
        nc_visual_draw_tool_cell(line, 'D', 338, y, 5, fg, bg, tool_line == active_line && active_letter == 'D');
        nc_visual_draw_tool_cell(line, 'E', 394, y, 5, fg, bg, tool_line == active_line && active_letter == 'E');
        nc_visual_draw_tool_cell(line, 'S', 454, y, 5, fg, bg, tool_line == active_line && active_letter == 'S');
        nc_visual_draw_tool_cell(line, 'X', 516, y, 6, fg, bg, tool_line == active_line && active_letter == 'X');
        nc_visual_draw_tool_cell(line, 'Z', 584, y, 6, fg, bg, tool_line == active_line && active_letter == 'Z');
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
        nc_visual_draw_tool_param(line, 'O', "ORIENT", 170, detail_y + 64, 8, selected_line == active_line && active_letter == 'O');
        nc_visual_draw_tool_param(line, 'R', "RADIUS", 170, detail_y + 84, 8, selected_line == active_line && active_letter == 'R');
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
}

static void nc_visual_draw_snapshot(const nc_snapshot_t *s)
{
    int row;
    int line_cols = (NC_RIGHT_PANE_W - NC_LINE_TEXT_X_PAD) / NC_VISUAL_CHAR_W;
    char buf[80];
    char footer_text[160];
    bool split = nc_files_active() || nc_visual_is_code_view();

    lvds_draw_fill_rect(0, 0, LVDS_HSTX_WIDTH, LVDS_HSTX_HEIGHT, NC_VISUAL_BG);
    nc_visual_draw_header(s);

    if (split) {
        nc_visual_draw_thin_preview(&g_nc_visual_doc, &s->runtime, NC_LEFT_PANE_X, 58, NC_LEFT_PANE_W, 474);
        lvds_draw_line(NC_SPLIT_X, 58, NC_SPLIT_X, 532, NC_VISUAL_DIM);
        lvds_draw_fill_rect(NC_RIGHT_PANE_X, 58, NC_RIGHT_PANE_W, 474, NC_VISUAL_BG);
    } else {
        lvds_draw_fill_rect(20, 62, LVDS_HSTX_WIDTH - 40, 378, NC_VISUAL_PANEL);
        lvds_draw_rect(20, 62, LVDS_HSTX_WIDTH - 40, 378, NC_VISUAL_DIM);
    }

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

    nc_visual_footer_text(footer_text, sizeof(footer_text));
    nc_visual_draw_footer_status(g_nc_visual_status[0] ? g_nc_visual_status : "NC bring-up",
                                 footer_text);
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

    if (nc_visual_handle_selected_word_edit(key)) {
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

void nc_visual_draw(void)
{
    nc_snapshot_t snapshot;

    if (g_nc_visual_in_draw) {
        return;
    }

    g_nc_visual_in_draw = true;

    nc_state_snapshot(&g_nc_visual_doc, &snapshot);
    nc_visual_draw_snapshot(&snapshot);
    lvds_hstx_present();

    g_nc_visual_dirty = false;
    g_nc_visual_in_draw = false;
}
