/* The preview: the stock, the contour, the dimension layer and the live tool
   marker. It draws what the screen hands it and owns only what it draws with -
   see nc_preview.h for the request it is given and the switches it keeps. */
#include "nc_preview.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../cnc.h"
#include "nc_draw.h"
#include "nc_emit.h"
#include "nc_g7x.h"
#include "nc_layout.h"
#include "nc_menu.h"
#include "nc_tools.h"
#include "../lvds_renderer/lvds_draw_api.h"
#include "../lvds_renderer/lvds_hstx.h"
#if __has_include("../lvds_renderer/lvds_psram.h")
#include "../lvds_renderer/lvds_psram.h"
#define NC_PREVIEW_HAVE_PSRAM 1
#else
#define NC_PREVIEW_HAVE_PSRAM 0
#endif

typedef enum {
    NC_PREVIEW_SEG_FEED = 0,
    NC_PREVIEW_SEG_ROUGH,
    NC_PREVIEW_SEG_FINISH
} nc_preview_segment_t;

typedef struct {
    float x;
    float z;
} nc_preview_v2_t;

static float nc_preview_absf(float v)
{
    return v < 0.0f ? -v : v;
}

static float nc_preview_directed_arc_sweep(float a0, float a1, bool cw)
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

static bool nc_preview_r_arc_center(float start_z,
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
    float abs_r = nc_preview_absf(r);
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

    s1 = nc_preview_directed_arc_sweep(atan2f(sx - c1.x * 0.5f, start_z - c1.z),
                                      atan2f(ex - c1.x * 0.5f, end_z - c1.z),
                                      cw);
    s2 = nc_preview_directed_arc_sweep(atan2f(sx - c2.x * 0.5f, start_z - c2.z),
                                      atan2f(ex - c2.x * 0.5f, end_z - c2.z),
                                      cw);
    if (r >= 0.0f) {
        *center = nc_preview_absf(s1) <= nc_preview_absf(s2) ? c1 : c2;
    } else {
        *center = nc_preview_absf(s1) > nc_preview_absf(s2) ? c1 : c2;
    }
    return true;
}

int nc_preview_map_z(const nc_preview_info_t *p, int z0_x, int stock_w, float z)
{
    if (!p || p->stock_visible_z <= 0.0f) {
        return z0_x;
    }
    return z0_x + (int)((z / p->stock_visible_z) * (float)stock_w);
}

int nc_preview_map_x(const nc_preview_info_t *p, int stock_top, int stock_h, float x)
{
    if (!p || p->stock_x <= 0.0f) {
        return stock_top;
    }
    return stock_top + (int)(((x * 0.5f) / (p->stock_x * 0.5f)) * (float)stock_h);
}

static void nc_preview_tool_panel(int x,
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
    nc_draw_tool_glyph(panel_x + 28, panel_y + 26, 20, tool, NC_VISUAL_PREVIEW_BG, false);
    snprintf(buf, sizeof(buf), "T%d O%d", tool->t, tool->orient);
    nc_draw_text_clip(panel_x + 54, panel_y + 10, buf, 8, NC_VISUAL_TEXT, NC_VISUAL_PREVIEW_BG, LVDS_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "R %.2g", (double)tool->r);
    nc_draw_text_clip(panel_x + 54, panel_y + 28, buf, 8, NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_NORMAL);
}

static void nc_preview_dashdot_line(int x0,
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

static void nc_preview_centerline(int x0, int y0, int x1, int y1)
{
    nc_preview_dashdot_line(x0, y0, x1, y1, NC_VISUAL_DIM);
}

static void nc_preview_origin_marker(int x, int y)
{
    lvds_draw_ellipse(x, y, 11, 11, NC_VISUAL_TEXT);
    lvds_draw_ellipse(x, y, 6, 6, NC_VISUAL_TEXT);
    lvds_draw_line(x - 15, y, x - 8, y, NC_VISUAL_TEXT);
    lvds_draw_line(x + 8, y, x + 15, y, NC_VISUAL_TEXT);
    lvds_draw_line(x, y - 15, x, y - 8, NC_VISUAL_TEXT);
    lvds_draw_line(x, y + 8, x, y + 15, NC_VISUAL_TEXT);
}

static void nc_preview_chuck_hatching(int x, int y, int w, int h, lvds_color_t color)
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

        y1 = nc_draw_clampi(y1, 0, h);
        lvds_draw_line(x + x0, y + y0, x + x1, y + y1, color);
    }
    for (s = 0; s < w + h; s += 8) {
        int x0 = s < w ? s : w;
        int y0 = s < w ? 0 : s - w;
        int x1 = s < h ? 0 : s - h;
        int y1 = s < h ? s : h;

        x1 = nc_draw_clampi(x1, 0, w);
        y0 = nc_draw_clampi(y0, 0, h);
        lvds_draw_line(x + x0, y + y0, x + x1, y + y1, color);
    }
}

static void nc_preview_arrowhead(int x, int y, int dir_x, int dir_y, lvds_color_t color)
{
    int px = -dir_y;
    int py = dir_x;

    lvds_draw_line(x, y, x - dir_x * 7 + px * 3, y - dir_y * 7 + py * 3, color);
    lvds_draw_line(x, y, x - dir_x * 7 - px * 3, y - dir_y * 7 - py * 3, color);
}

static void nc_preview_diameter_dimension(int x,
                                              int y0,
                                              int y1,
                                              const char *label)
{
    lvds_draw_line(x, y0, x, y1, NC_VISUAL_DIM);
    nc_preview_arrowhead(x, y1, 0, 1, NC_VISUAL_DIM);
    if (label && label[0]) {
        lvds_draw_text(x + 6,
                       y1 - 8,
                       label,
                       NC_VISUAL_DIM,
                       NC_VISUAL_PREVIEW_BG,
                       LVDS_FONT_NORMAL);
    }
}

static void nc_preview_z_point_dimension(int start_x,
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
        nc_preview_arrowhead(point_x, dim_y, -1, 0, NC_VISUAL_DIM);
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

static void nc_preview_x_point_dimension(int dim_x,
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
    nc_preview_arrowhead(dim_x, point_y, 0, 1, NC_VISUAL_DIM);
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

static void nc_preview_contour_point_marker(int x, int y, bool filled)
{
    if (filled) {
        lvds_draw_fill_ellipse(x, y, 3, 3, NC_VISUAL_TEXT);
    } else {
        lvds_draw_ellipse(x, y, 5, 5, NC_VISUAL_TEXT);
    }
}

static void nc_preview_chuck(const nc_preview_info_t *preview,
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
    nc_preview_chuck_hatching(block_x, block_y, block_w, c_h, ink);
#endif
}

static void nc_preview_chuck_relief(const nc_preview_info_t *preview,
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

static bool nc_preview_explicit_arc(const nc_preview_info_t *preview,
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
    float abs_r = nc_preview_absf(r);
    float screen_r;
    int steps;
    int last_px;
    int last_py;
    int i;

    if (!preview || !nc_preview_r_arc_center(start_z, start_x, end_z, end_x, r, cw, &center)) {
        return false;
    }

    a0 = atan2f((start_x * 0.5f) - (center.x * 0.5f), start_z - center.z);
    sweep = nc_preview_directed_arc_sweep(a0,
                                         atan2f((end_x * 0.5f) - (center.x * 0.5f),
                                                end_z - center.z),
                                         cw);
    screen_r = abs_r * (float)stock_w / (preview->stock_z > 0.0001f ? preview->stock_z : 1.0f);
    steps = nc_draw_clampi((int)(nc_preview_absf(sweep) * screen_r * 0.35f) + 8,
                             10,
                             NC_PREVIEW_ARC_MAX_STEPS);
    last_px = nc_preview_map_z(preview, z0_x, stock_w, start_z);
    last_py = nc_preview_map_x(preview, stock_top, stock_h, start_x);
    for (i = 1; i <= steps; i++) {
        float a = a0 + sweep * ((float)i / (float)steps);
        float z = center.z + cosf(a) * abs_r;
        float x = ((center.x * 0.5f) + sinf(a) * abs_r) * 2.0f;
        int px = nc_preview_map_z(preview, z0_x, stock_w, z);
        int py = nc_preview_map_x(preview, stock_top, stock_h, x);
        lvds_draw_line_w(last_px, last_py, px, py, color, width);
        last_px = px;
        last_py = py;
    }
    return true;
}

static bool nc_preview_center_arc(const nc_preview_info_t *preview,
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
    sweep = nc_preview_directed_arc_sweep(a0,
                                         atan2f(end_xr - center_xr,
                                                end_z - center_z),
                                         cw);
    screen_r = radius * (float)stock_w / (preview->stock_z > 0.0001f ? preview->stock_z : 1.0f);
    steps = nc_draw_clampi((int)(nc_preview_absf(sweep) * screen_r * 0.35f) + 8,
                             10,
                             NC_PREVIEW_ARC_MAX_STEPS);
    last_px = nc_preview_map_z(preview, z0_x, stock_w, start_z);
    last_py = nc_preview_map_x(preview, stock_top, stock_h, start_x);
    for (n = 1; n <= steps; n++) {
        float a = a0 + sweep * ((float)n / (float)steps);
        float z = center_z + cosf(a) * radius;
        float x = (center_xr + sinf(a) * radius) * 2.0f;
        int px = nc_preview_map_z(preview, z0_x, stock_w, z);
        int py = nc_preview_map_x(preview, stock_top, stock_h, x);
        lvds_draw_line_w(last_px, last_py, px, py, color, width);
        last_px = px;
        last_py = py;
    }
    return true;
}

static void nc_preview_dashed_segment(const nc_preview_info_t *preview,
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

    px0 = nc_preview_map_z(preview, z0_x, stock_w, z0);
    py0 = nc_preview_map_x(preview, stock_top, stock_h, x0);
    px1 = nc_preview_map_z(preview, z0_x, stock_w, z1);
    py1 = nc_preview_map_x(preview, stock_top, stock_h, x1);
    plen = sqrtf((float)((px1 - px0) * (px1 - px0) + (py1 - py0) * (py1 - py0)));
    pieces = nc_draw_clampi((int)(plen / 8.0f), 1, 80);
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
        xa = nc_preview_map_z(preview, z0_x, stock_w, z0 + dz * a);
        ya = nc_preview_map_x(preview, stock_top, stock_h, x0 + dx * a);
        xb = nc_preview_map_z(preview, z0_x, stock_w, z0 + dz * b);
        yb = nc_preview_map_x(preview, stock_top, stock_h, x0 + dx * b);
        lvds_draw_line(xa, ya, xb, yb, color);
    }
}

static bool nc_preview_emitted_motion_line(const nc_preview_info_t *preview,
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
    lvds_color_t color = NC_VISUAL_TEXT;
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
    has_x = nc_line_word_float(line, 'X', &x);
    has_z = nc_line_word_float(line, 'Z', &z);
    if (!*have_last) {
        *last_x = has_x ? x : preview->stock_x;
        *last_z = has_z ? z : 0.0f;
        *have_last = true;
        return false;
    }
    if (!has_x && !has_z) {
        return false;
    }

    x0 = nc_preview_map_z(preview, z0_x, stock_w, *last_z);
    y0 = nc_preview_map_x(preview, stock_top, stock_h, *last_x);
    x1 = nc_preview_map_z(preview, z0_x, stock_w, z);
    y1 = nc_preview_map_x(preview, stock_top, stock_h, x);
    if (segment == NC_PREVIEW_SEG_ROUGH) {
        color = NC_VISUAL_TEXT;
    } else if (segment == NC_PREVIEW_SEG_FINISH) {
        color = NC_VISUAL_TEXT;
    }

    if (cmd == G7X_CONTOUR_ARC_CW || cmd == G7X_CONTOUR_ARC_CCW) {
        float r = 0.0f;
        float i_off = 0.0f;
        float k_off = 0.0f;
        if ((nc_line_word_float(line, 'I', &i_off) &&
             nc_line_word_float(line, 'K', &k_off) &&
             nc_preview_center_arc(preview,
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
            (nc_line_word_float(line, 'R', &r) &&
             nc_preview_explicit_arc(preview,
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
        nc_preview_dashed_segment(preview,
                                      z0_x,
                                      stock_w,
                                      stock_top,
                                      stock_h,
                                      *last_z,
                                      *last_x,
                                      z,
                                      x,
                                      NC_VISUAL_ERROR);
    } else {
        nc_preview_dashed_segment(preview,
                                      z0_x,
                                      stock_w,
                                      stock_top,
                                      stock_h,
                                      *last_z,
                                      *last_x,
                                      z,
                                      x,
                                      color);
    }

    *last_x = x;
    *last_z = z;
    return true;
}

/* The switches the footer keys toggle. They are the preview's own state: the
   screen reports the key it read and reads back whether the layer is on. */
static bool g_nc_preview_layer[NC_PREVIEW_LAYER_COUNT] = { true, true, true, true };

/* The live stock: the mask of what is still there, drawn on the machine when
   RUN is cutting, so the operator sees the material the program has taken off.
   The area that path redraws is one band (nc_live_band), and the tool marker is
   held inside it: the marker is erased by the same band being cleared, so a
   marker outside it would be drawn once and never taken back. */
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
/* While RUN is streaming and nothing about the drawing changed, the collected
   preview and the box it was fitted into are reused instead of recomputed. */
static bool g_nc_live_preview_cache_valid;
static nc_preview_info_t g_nc_live_preview_cache;
static nc_tool_t g_nc_live_tool_cache;
static bool g_nc_live_have_tool_cache;
static int g_nc_live_stock_w_cache;
static int g_nc_live_stock_h_cache;
static int g_nc_live_stock_left_cache;
static int g_nc_live_stock_top_cache;
static int g_nc_live_z0_x_cache;

bool nc_preview_layer(nc_preview_layer_t layer)
{
    if (layer < 0 || layer >= NC_PREVIEW_LAYER_COUNT) {
        return false;
    }
    return g_nc_preview_layer[layer];
}

bool nc_preview_toggle_layer(nc_preview_layer_t layer)
{
    if (layer < 0 || layer >= NC_PREVIEW_LAYER_COUNT) {
        return false;
    }
    g_nc_preview_layer[layer] = !g_nc_preview_layer[layer];
    return g_nc_preview_layer[layer];
}

void nc_preview_invalidate(void)
{
    g_nc_live_preview_cache_valid = false;
}

/* What there is to draw: the stock, the contour extent and the cycle,
   collected from the document before anything is painted. */
void nc_preview_collect(const nc_document_t *doc, nc_preview_info_t *p)
{
    float last_x = 0.0f;
    float last_z = 0.0f;
    bool have_last = false;
    size_t i;

    if (!p) {
        return;
    }

    memset(p, 0, sizeof(*p));
    p->stock_x = 50.0f;
    p->stock_z = 75.0f;
    p->stock_i = 0.0f;
    p->stock_e = 3.0f;
    p->chuck_c = 15.0f;
    p->min_x = 1000000.0f;
    p->min_z = 1000000.0f;

    if (!doc) {
        return;
    }

    for (i = 0; i < doc->line_count; i++) {
        const char *line = doc->lines[i].text;
        g7x_cycle_t cycle;
        g7x_contour_cmd_t cmd;
        float px = last_x;
        float pz = last_z;
        bool has_x;
        bool has_z;

        if (!line) {
            continue;
        }

        if (g7x_command_is(line, "G971")) {
            (void)nc_line_word_float(line, 'X', &p->stock_x);
            (void)nc_line_word_float(line, 'Z', &p->stock_z);
            (void)nc_line_word_float(line, 'I', &p->stock_i);
            (void)nc_line_word_float(line, 'E', &p->stock_e);
        }
        if (g7x_command_is(line, "G972")) {
            (void)nc_line_word_float(line, 'C', &p->chuck_c);
        }

        cycle = g7x_cycle_from_line(line);
        if (cycle != G7X_CYCLE_NONE) {
            p->cycles++;
            p->last_cycle = cycle;
        }

        cmd = g7x_contour_cmd_from_line(line);
        if (cmd == G7X_CONTOUR_NONE || cmd == G7X_CONTOUR_END) {
            continue;
        }

        has_x = nc_line_word_float(line, 'X', &px);
        has_z = nc_line_word_float(line, 'Z', &pz);
        if (!have_last) {
            last_x = has_x ? px : p->stock_x;
            last_z = has_z ? pz : 0.0f;
            have_last = true;
        }
        if (!has_x && !has_z) {
            continue;
        }

        if (px < p->min_x) p->min_x = px;
        if (px > p->max_x) p->max_x = px;
        if (pz < p->min_z) p->min_z = pz;
        if (pz > p->max_z) p->max_z = pz;
        if (cmd == G7X_CONTOUR_RAPID) {
            p->rapid_segments++;
        } else if (cmd == G7X_CONTOUR_ARC_CW || cmd == G7X_CONTOUR_ARC_CCW) {
            p->arc_segments++;
            p->path_segments++;
        } else {
            p->path_segments++;
        }
        last_x = px;
        last_z = pz;
    }

    if (p->stock_x <= 0.0f) p->stock_x = 50.0f;
    if (p->stock_z <= 0.0f) p->stock_z = 75.0f;
    if (p->stock_i < 0.0f) p->stock_i = 0.0f;
    if (p->stock_i >= p->stock_x) p->stock_i = 0.0f;
    if (p->stock_e < 0.0f) p->stock_e = 0.0f;
    if (p->chuck_c <= 0.0f) p->chuck_c = 15.0f;
    p->stock_visible_z = p->stock_z + p->stock_e;
    if (p->stock_visible_z <= 0.0f) p->stock_visible_z = p->stock_z;
    if (p->min_x > p->max_x) {
        p->min_x = 0.0f;
        p->max_x = p->stock_x;
    }
    if (p->min_z > p->max_z) {
        p->min_z = 0.0f;
        p->max_z = p->stock_z;
    }
}

#if NC_PREVIEW_DIN_STYLE
static void nc_preview_din_layer(const nc_preview_ctx_t *ctx,
                                     const nc_preview_info_t *preview,
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

    nc_preview_centerline(stock_left - 34,
                              stock_top,
                              stock_right + 18,
                              stock_top);
    nc_preview_centerline(z0_x,
                              stock_top - 26,
                              z0_x,
                              stock_bottom + 20);
    nc_preview_origin_marker(z0_x, stock_top);

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

    if (ctx->mode != NC_MODE_RUN) {
        snprintf(label, sizeof(label), "%.0f", preview->stock_x);
        nc_preview_diameter_dimension(dim_x,
                                          stock_top,
                                          stock_bottom,
                                          label);
    }
    if (ctx->mode != NC_MODE_RUN &&
        preview->stock_i > 0.0f && preview->stock_i < preview->stock_x) {
        id_y = nc_preview_map_x(preview, stock_top, stock_h, preview->stock_i);
        nc_preview_centerline(stock_left - 18, id_y, stock_right + 8, id_y);
        snprintf(label, sizeof(label), "%.0f", preview->stock_i);
        nc_preview_diameter_dimension(dim_x - 18,
                                          stock_top,
                                          id_y,
                                          label);
    }
}
#endif

static void nc_preview_contour_points(const nc_preview_ctx_t *ctx,
                                          const nc_document_t *doc,
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
    float x = preview ? preview->stock_x : 0.0f;
    float z = 0.0f;
    int prev_z_px = z0_x;
    int prev_x_py = stock_top;
    int x_dim = stock_right + 15;
    char label[16];

    /* The dimension callouts follow the same switch as the rest of the layer:
       EDIT shows them in the pane and on the whole body alike, so turning `DIM`
       off on the full body is what hides them in the split view too. */
    if (!doc || !preview || ctx->mode == NC_MODE_MANUAL ||
        ctx->mode == NC_MODE_TOOLS) {
        return;
    }

    for (i = 0; i < doc->line_count; i++) {
        const char *line = doc->lines[i].text;
        g7x_contour_cmd_t cmd;
        bool has_x;
        bool has_z;

        if (!nc_g7x_line_is_any_contour(doc, i)) {
            continue;
        }
        cmd = g7x_contour_cmd_from_line(line);
        if (cmd == G7X_CONTOUR_NONE || cmd == G7X_CONTOUR_RAPID) {
            continue;
        }

        has_x = nc_line_word_float(line, 'X', &x);
        has_z = nc_line_word_float(line, 'Z', &z);
        if (has_x || has_z) {
            int px = nc_preview_map_z(preview, z0_x, stock_w, z);
            int py = nc_preview_map_x(preview, stock_top, stock_h, x);
            float feature = 0.0f;
            if (ctx->full ||
                (ctx->mode == NC_MODE_RUN &&
                 !ctx->streaming &&
                 !ctx->hold &&
                 !cnc_get_exec_state(EXEC_RUN | EXEC_HOLD))) {
                nc_preview_contour_point_marker(px, py, i == doc->cursor_line);
            }
            snprintf(label, sizeof(label), "%.0f", x);
            nc_preview_x_point_dimension(x_dim, prev_x_py, py, stock_top, px, label);
            prev_x_py = py;
            snprintf(label, sizeof(label), "%.0f", z);
            nc_preview_z_point_dimension(prev_z_px, px, z0_x, stock_top, py, label);
            prev_z_px = px;
            if (nc_line_word_float(line, 'R', &feature) && feature > 0.0001f) {
                snprintf(label, sizeof(label), "R%.0f", feature);
                lvds_draw_text(px + 4, py - 16, label, NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_SMALL);
            } else if (nc_line_word_float(line, 'C', &feature) && feature > 0.0001f) {
                snprintf(label, sizeof(label), "C%.0f", feature);
                lvds_draw_text(px + 4, py - 16, label, NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_SMALL);
            }
        }
    }
    snprintf(label, sizeof(label), "%.0f", -preview->stock_visible_z);
    nc_preview_z_point_dimension(prev_z_px,
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

static void nc_preview_emitted_preview(const nc_preview_ctx_t *ctx,
                                           const nc_document_t *doc,
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
            snprintf(ctx->status, ctx->status_size,
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
            if (ctx->mode == NC_MODE_PROGRAM &&
                ((segment == NC_PREVIEW_SEG_ROUGH && !nc_preview_layer(NC_PREVIEW_LAYER_ROUGH)) ||
                 (segment != NC_PREVIEW_SEG_ROUGH && !nc_preview_layer(NC_PREVIEW_LAYER_PATH)))) {
                drawn_lines++;
                continue;
            }
            (void)nc_preview_emitted_motion_line(preview,
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

static float nc_live_runtime_x_to_diam(float runtime_x)
{
    return nc_preview_absf(runtime_x) * 2.0f;
}

/* The area the live path redraws every frame: the stock and the room the
   drawing needs around it, and nothing else - the band that carries the tool
   panel box above and the footer below is left alone, because that strip is
   not repainted. The clear and the tool marker's guard are the same rectangle,
   so the marker can never sit outside the area that takes it back. */
static void nc_live_band(int stock_top, int stock_h, int *band_y, int *band_h)
{
    *band_y = nc_draw_clampi(stock_top - 24, 44, NC_FOOTER_Y - 1);
    *band_h = nc_draw_clampi(stock_top + stock_h + 74 - *band_y, 1,
                             NC_FOOTER_Y - *band_y);
}

static void nc_preview_live_tool(const nc_preview_info_t *preview,
                                     const nc_runtime_state_t *runtime,
                                     int z0_x,
                                     int stock_w,
                                     int stock_top,
                                     int stock_h,
                                     int area_x,
                                     int area_y,
                                     int area_w,
                                     int area_h,
                                     const nc_tool_t *tool)
{
    int sx;
    int sy;

    if (!preview || !runtime || !tool || !tool->valid) {
        return;
    }

    sx = nc_preview_map_z(preview, z0_x, stock_w, runtime->z);
    sy = nc_preview_map_x(preview, stock_top, stock_h, nc_live_runtime_x_to_diam(runtime->x));
    /* Inside the area that was redrawn, or not at all: the cursor may only be
       where the frame takes it back. A marker clamped to the whole screen is
       drawn over the header, the code pane or the footer when the axis is out
       of view, and - worse - a marker outside the redrawn area (the live band,
       which is narrower than the pane: the tool panel's strip above and the
       room above the footer are not repainted) is drawn once and stays there,
       the "cursor drawn but not cleared". A wild touch-off is what puts the
       axis there. Out of view, the DRO says where the axis is; the preview does
       not guess. */
    if (sx < area_x || sx + NC_LIVE_TOOL_GLYPH > area_x + area_w ||
        sy < area_y || sy + NC_LIVE_TOOL_GLYPH > area_y + area_h) {
        return;
    }

    nc_draw_tool_glyph(sx, sy, NC_LIVE_TOOL_GLYPH, tool, NC_VISUAL_PREVIEW_BG, false);
}

static bool nc_live_stock_alloc(void)
{
    if (g_nc_live_stock_mask) {
        return true;
    }
#if NC_PREVIEW_HAVE_PSRAM
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

    stock_w = nc_draw_clampi(stock_w, 1, NC_LIVE_STOCK_MAX_W);
    stock_h = nc_draw_clampi(stock_h, 1, NC_LIVE_STOCK_MAX_H);
    memset(g_nc_live_stock_mask, 0, (size_t)NC_LIVE_STOCK_MAX_W * NC_LIVE_STOCK_MAX_H);
    if (preview->stock_i > 0.0f && preview->stock_x > 0.0f) {
        material_top = nc_draw_clampi((int)((preview->stock_i / preview->stock_x) * (float)stock_h),
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
    x0 = nc_draw_clampi(x0, 0, g_nc_live_stock_w - 1);
    x1 = nc_draw_clampi(x1, 0, g_nc_live_stock_w - 1);
    y0 = nc_draw_clampi(y0, 0, g_nc_live_stock_h - 1);
    y1 = nc_draw_clampi(y1, 0, g_nc_live_stock_h - 1);
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
    sx0 = nc_preview_map_z(preview, z0_x, stock_w, z0) - stock_left;
    sx1 = nc_preview_map_z(preview, z0_x, stock_w, z1) - stock_left;
    sy0 = nc_preview_map_x(preview, stock_top, stock_h, x0) - stock_top;
    sy1 = nc_preview_map_x(preview, stock_top, stock_h, x1) - stock_top;
    samples = nc_preview_absf((float)(sx1 - sx0)) > nc_preview_absf((float)(sy1 - sy0)) ?
              nc_preview_absf((float)(sx1 - sx0)) :
              nc_preview_absf((float)(sy1 - sy0));
    samples = nc_draw_clampi(samples, 1, 80);
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
    clip_left = nc_draw_clampi(clip_left, 0, g_nc_live_stock_w - 1);
    clip_right = nc_draw_clampi(clip_right, 0, g_nc_live_stock_w - 1);
    y0 = nc_draw_clampi(clip_y - stock_top, 0, g_nc_live_stock_h - 1);
    y1 = nc_draw_clampi(clip_y + clip_h - stock_top - 1, 0, g_nc_live_stock_h - 1);
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

static bool nc_preview_live_stock(const nc_preview_ctx_t *ctx,
                                      nc_preview_times_t *times,
                                      const nc_preview_info_t *preview,
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
    int band_y;
    int band_h;
    uint32_t t0;
    uint32_t t1;
    uint32_t t2;
    uint32_t t3;
    uint32_t t4;

    live_run = ctx->runtime_busy || ctx->streaming;

    if (!preview || !runtime) {
        g_nc_live_stock_was_active = false;
        return false;
    }
    context_changed = nc_live_stock_context_changed(preview, stock_w, stock_h);
    retain_stock = ctx->mode == NC_MODE_RUN &&
                   g_nc_live_stock_ready &&
                   !context_changed;
    if (!live_run && !retain_stock) {
        g_nc_live_stock_was_active = false;
        return false;
    }
    if (live_run && (!g_nc_live_stock_was_active || context_changed)) {
        nc_live_stock_reset(preview, stock_w, stock_h);
    }
    g_nc_live_stock_was_active = live_run || retain_stock;
    if (!g_nc_live_stock_ready) {
        nc_draw_text_clip(stock_left,
                                 stock_top + 16,
                                 "Live stock needs PSRAM",
                                 28,
                                 NC_VISUAL_ERROR,
                                 NC_VISUAL_PREVIEW_BG,
                                 LVDS_FONT_NORMAL);
        return true;
    }
    t0 = mcu_micros();
    nc_live_band(stock_top, stock_h, &band_y, &band_h);
    lvds_draw_fill_rect(tool_panel_x, band_y, tool_panel_w, band_h,
                        NC_VISUAL_PREVIEW_BG);
    t1 = mcu_micros();
    if (live_run) {
        nc_live_stock_update(preview, runtime, z0_x, stock_left, stock_w, stock_top, stock_h);
    }
    nc_preview_chuck(preview, stock_left, stock_top, stock_w, stock_h);
    nc_preview_chuck_relief(preview, stock_left, stock_top, stock_h);
    nc_live_stock_draw(stock_left, stock_top, stock_left, stock_top, stock_w, stock_h);
    t2 = mcu_micros();
#if NC_PREVIEW_DIN_STYLE
    if (nc_preview_layer(NC_PREVIEW_LAYER_DIMS)) {
        nc_preview_din_layer(ctx, preview, stock_left, stock_top, stock_w, stock_h, z0_x);
    }
#endif
    if (nc_preview_layer(NC_PREVIEW_LAYER_DIMS)) {
        nc_preview_contour_points(ctx, ctx->screen_doc,
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
    nc_preview_live_tool(preview, runtime, z0_x, stock_w, stock_top, stock_h,
                             tool_panel_x, band_y, tool_panel_w, band_h, tool);
    if (draw_static_panel) {
        nc_preview_tool_panel(tool_panel_x,
                                          tool_panel_y,
                                          tool_panel_w,
                                          58,
                                          tool);
    }
    t4 = mcu_micros();
    times->clear += t1 - t0;
    times->stock += t2 - t1;
    times->geom += t3 - t2;
    times->tool += t4 - t3;
    return true;
}

/* A file that is not a program, shown read-only: the same rows the editor
   would show, with their line numbers, as many as fit. There is nothing to draw
   from such a file (the preset file is the one this is for), so the pane hands
   the operator the text instead of a blank "no preview". */
static void nc_preview_text_file(const nc_document_t *doc, int x, int y, int w, int h)
{
    int text_x = x + 8 + 4 * NC_VISUAL_CHAR_W;
    int cols = (w - (text_x - x) - 8) / NC_VISUAL_CHAR_W;
    int top = y + 24;                 /* below the pane's own caption row */
    int rows = (y + h - 8 - top) / NC_VISUAL_ROW_H;
    int row;

    if (cols < 8) {
        return;
    }
    if (rows < 1) {
        rows = 1;
    }
    for (row = 0; row < rows && (size_t)row < doc->line_count; row++) {
        char number[8];
        int ry = top + row * NC_VISUAL_ROW_H;

        snprintf(number, sizeof(number), "%3lu", (unsigned long)(row + 1u));
        lvds_draw_text(x + 8, ry, number, NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG,
                       LVDS_FONT_NORMAL);
        nc_draw_text_clip(text_x, ry, doc->lines[row].text, cols,
                          NC_VISUAL_TEXT, NC_VISUAL_PREVIEW_BG,
                          LVDS_FONT_NORMAL);
    }
    if (doc->line_count > (size_t)rows) {
        /* More lines than the pane can hold: say how many, rather than look
           like the file ends here. */
        char more[24];

        snprintf(more, sizeof(more), "+%u lines",
                 (unsigned)(doc->line_count - (size_t)rows));
        lvds_draw_text(x + 8, y + h - NC_VISUAL_ROW_H, more, NC_VISUAL_DIM,
                       NC_VISUAL_PREVIEW_BG, LVDS_FONT_NORMAL);
    }
}

void nc_preview_draw(const nc_preview_ctx_t *ctx,
                     int x,
                     int y,
                     int w,
                     int h,
                     bool clear_bg,
                     nc_preview_times_t *times)
{
    const nc_document_t *doc = ctx->doc;
    const nc_runtime_state_t *runtime = ctx->runtime;
    nc_preview_times_t discard = { 0 };
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
                          ctx->mode == NC_MODE_RUN &&
                          g_nc_live_preview_cache_valid;

    if (!times) {
        times = &discard;
    }

    if (doc && doc->path[0] && !nc_path_supported(doc->path)) {
        /* Text is not a program. The preview must not read it as G-code - the
           preset file is the file this is for - so it shows the text itself,
           which is what the operator opened it for. */
        if (clear_bg) {
            lvds_draw_fill_rect(x, y, w, h, NC_VISUAL_PREVIEW_BG);
        }
        nc_preview_text_file(doc, x, y, w, h);
        return;
    }

    if (doc && ctx->mode == NC_MODE_RUN && ctx->run_line < doc->line_count) {
        tool_line = ctx->run_line;
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
        nc_preview_collect(doc, &preview);
        t1 = mcu_micros();
        if (clear_bg) {
            lvds_draw_fill_rect(x, y, w, h, NC_VISUAL_PREVIEW_BG);
            /* No caption: the tab strip carries the screen name and the header
               carries the file or the run state. */
        }
        t2 = mcu_micros();

        /* Content box inside the pane: the stock starts below a short band at
           the top (the caption that used to live there is gone - the tab strip
           says the screen and the header says the file) and the drawing stops
           short of the pane's own bottom edge, so no part of the preview -
           stock, chuck or a dimension label - can be drawn over the footer
           strip below it. SIM keeps a little more room under the stock for its
           labels. */
        usable_w = (float)(w - 72);
        usable_h = (float)(h - NC_PREVIEW_TOP_BAND -
                           (ctx->full ? 28 : 8));
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
        stock_top = y + NC_PREVIEW_TOP_BAND;
        z0_x = stock_left + (int)(preview.stock_z * scale + 0.5f);
        if (z0_x < stock_left) z0_x = stock_left;
        if (z0_x > stock_left + stock_w) z0_x = stock_left + stock_w;
        memset(&tool, 0, sizeof(tool));
        if (doc && ctx->mode == NC_MODE_TOOLS &&
            nc_tool_active_from_table(doc, tool_line, ctx->screen_doc, &tool)) {
            have_tool = true;
        } else if (doc && nc_tool_active_from_file(doc,
                                                   tool_line,
                                                   ctx->tool_path,
                                                   &tool)) {
            have_tool = true;
        }
        t3 = mcu_micros();
        if (ctx->mode == NC_MODE_RUN) {
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
    times->collect += (t1 - t0) + (t3 - t2);
    times->clear += t2 - t1;
    if (ctx->mode == NC_MODE_RUN &&
        nc_preview_live_stock(ctx,
                                  times,
                                  &preview,
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
    nc_preview_chuck(&preview, stock_left, stock_top, stock_w, stock_h);
    nc_preview_chuck_relief(&preview, stock_left, stock_top, stock_h);
    if (nc_preview_layer(NC_PREVIEW_LAYER_STOCK) || !ctx->full) {
        lvds_draw_fill_rect(stock_left, stock_top, stock_w, stock_h, NC_VISUAL_PREVIEW_STOCK);
        if (preview.stock_i > 0.0f) {
            int id_h = nc_preview_map_x(&preview, stock_top, stock_h, preview.stock_i) - stock_top;
            if (id_h > 0 && id_h < stock_h) {
                lvds_draw_fill_rect(stock_left, stock_top, stock_w, id_h, NC_VISUAL_PREVIEW_BG);
            }
        }
    }
    t1 = mcu_micros();
#if NC_PREVIEW_DIN_STYLE
    if (nc_preview_layer(NC_PREVIEW_LAYER_DIMS)) {
        nc_preview_din_layer(ctx, &preview, stock_left, stock_top, stock_w, stock_h, z0_x);
    }
#endif
    if (nc_preview_layer(NC_PREVIEW_LAYER_DIMS)) {
        nc_preview_contour_points(ctx,
                                      doc,
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
    if (!ctx->full) {
        nc_preview_tool_panel(x, y, w, h, have_tool ? &tool : NULL);
    }
    t3 = mcu_micros();
    nc_preview_emitted_preview(ctx, doc,
                                   &preview,
                                   z0_x,
                                   stock_w,
                                   stock_top,
                                   stock_h,
                                   96u);
    t5 = mcu_micros();
    if (have_tool && !ctx->full) {
        /* This path drew the whole pane a moment ago, so the pane itself is the
           area the frame takes back. */
        nc_preview_live_tool(&preview, runtime, z0_x, stock_w, stock_top,
                                 stock_h, x, y, w, h, &tool);
        nc_preview_tool_panel(x, y, w, h, &tool);
    }
    times->stock += t1 - t0;
    times->geom += (t2 - t1) + (t5 - t3);
    times->tool += (t3 - t2) + (mcu_micros() - t5);
}


/* --- the footer entries the preview owns ---------------------------------- */

/* The layer switches. The message that goes with the new state belongs to the
   layer it describes, so the screen only has to show what it is handed.
   Returns NULL when the action is not one of the preview's. */
const char *nc_preview_action(uint8_t action)
{
    switch (action) {
    case NC_FOOTER_ACTION_STOCK:
        return nc_preview_toggle_layer(NC_PREVIEW_LAYER_STOCK)
                   ? "Stock on" : "Stock outline";
    case NC_FOOTER_ACTION_PATH:
        return nc_preview_toggle_layer(NC_PREVIEW_LAYER_PATH)
                   ? "Path on" : "Path hidden";
    case NC_FOOTER_ACTION_ROUGH:
        return nc_preview_toggle_layer(NC_PREVIEW_LAYER_ROUGH)
                   ? "Rough on" : "Rough hidden";
    case NC_FOOTER_ACTION_DIMS:
        return nc_preview_toggle_layer(NC_PREVIEW_LAYER_DIMS)
                   ? "Dimensions on" : "Dimensions hidden";
    default:
        return 0;
    }
}
