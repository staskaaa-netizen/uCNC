/* The panel's drawing vocabulary - see nc_draw.h. */
#include "nc_draw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../cnc.h"
#include "../lvds_renderer/lvds_draw_api.h"
#include "../lvds_renderer/lvds_hstx.h"

float nc_draw_absf(float v)
{
    return v < 0.0f ? -v : v;
}

int nc_draw_clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

float nc_draw_directed_arc_sweep(float a0, float a1, bool cw)
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

bool nc_draw_r_arc_center(float start_z,
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
    float abs_r = nc_draw_absf(r);
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

    s1 = nc_draw_directed_arc_sweep(atan2f(sx - c1.x * 0.5f, start_z - c1.z),
                                      atan2f(ex - c1.x * 0.5f, end_z - c1.z),
                                      cw);
    s2 = nc_draw_directed_arc_sweep(atan2f(sx - c2.x * 0.5f, start_z - c2.z),
                                      atan2f(ex - c2.x * 0.5f, end_z - c2.z),
                                      cw);
    if (r >= 0.0f) {
        *center = nc_draw_absf(s1) <= nc_draw_absf(s2) ? c1 : c2;
    } else {
        *center = nc_draw_absf(s1) > nc_draw_absf(s2) ? c1 : c2;
    }
    return true;
}

int nc_draw_preview_z(const nc_preview_info_t *p, int z0_x, int stock_w, float z)
{
    if (!p || p->stock_visible_z <= 0.0f) {
        return z0_x;
    }
    return z0_x + (int)((z / p->stock_visible_z) * (float)stock_w);
}

int nc_draw_preview_x(const nc_preview_info_t *p, int stock_top, int stock_h, float x)
{
    if (!p || p->stock_x <= 0.0f) {
        return stock_top;
    }
    return stock_top + (int)(((x * 0.5f) / (p->stock_x * 0.5f)) * (float)stock_h);
}

void nc_draw_preview_tool_panel(int x,
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

int nc_draw_tool_tip_digit(int orient)
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

int nc_draw_tool_orient_digits(int orient, int *digits, int max_digits)
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

bool nc_draw_tool_keypad_point(int digit, int ox, int oy, int step, int *x, int *y)
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

void nc_draw_tool_edges(int orient, bool *left, bool *top, bool *right, bool *bottom)
{
    int o = nc_draw_tool_tip_digit(orient);

    if (left) *left = (o == 1 || o == 4 || o == 7 || o == 2 || o == 5 || o == 8);
    if (top) *top = (o == 7 || o == 8 || o == 9 || o == 4 || o == 5 || o == 6);
    if (right) *right = (o == 3 || o == 6 || o == 9 || o == 2 || o == 5 || o == 8);
    if (bottom) *bottom = (o == 1 || o == 2 || o == 3 || o == 4 || o == 5 || o == 6);
}

void nc_draw_fill_triangle(int x1, int y1,
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

void nc_draw_tool_marker_line(int x1,
                                       int y1,
                                       int x2,
                                       int y2,
                                       lvds_color_t color,
                                       int thick)
{
    x1 = nc_draw_clampi(x1, 0, LVDS_HSTX_WIDTH - 1);
    y1 = nc_draw_clampi(y1, 0, LVDS_HSTX_HEIGHT - 1);
    x2 = nc_draw_clampi(x2, 0, LVDS_HSTX_WIDTH - 1);
    y2 = nc_draw_clampi(y2, 0, LVDS_HSTX_HEIGHT - 1);

    if (thick > 1) {
        lvds_draw_line_w(x1, y1, x2, y2, color, thick);
    } else {
        lvds_draw_line(x1, y1, x2, y2, color);
    }
}

int nc_draw_tool_polygon_points(int tip_x,
                                         int tip_y,
                                         int orient,
                                         int size,
                                         int *px,
                                         int *py)
{
    int digits[4];
    int tip_grid_x;
    int tip_grid_y;
    int step = nc_draw_clampi(size / 2, 6, 56);
    int n = nc_draw_tool_orient_digits(orient, digits, 4);
    int i;

    if (n < 3) {
        return 0;
    }
    if (n == 4) {
        int cut_x;
        int cut_y;
        int z_x;
        int z_y;
        if (!nc_draw_tool_keypad_point(digits[1], 0, 0, step, &cut_x, &cut_y) ||
            !nc_draw_tool_keypad_point(digits[2], 0, 0, step, &z_x, &z_y)) {
            return 0;
        }
        tip_grid_x = cut_x;
        tip_grid_y = z_y;
    } else {
        int tip_digit = nc_draw_tool_tip_digit(orient);
        if (!nc_draw_tool_keypad_point(tip_digit, 0, 0, step, &tip_grid_x, &tip_grid_y)) {
            return 0;
        }
    }

    for (i = 0; i < n; i++) {
        int gx;
        int gy;
        if (!nc_draw_tool_keypad_point(digits[i], 0, 0, step, &gx, &gy)) {
            return 0;
        }
        px[i] = tip_x + gx - tip_grid_x;
        py[i] = tip_y + gy - tip_grid_y;
    }
    return n;
}

void nc_draw_tool_polygon(int tip_x,
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
    int n = nc_draw_tool_polygon_points(tip_x, tip_y, orient, size, px, py);
    int i;

    if (n < 3) {
        return;
    }
    for (i = 1; i + 1 < n; i++) {
        nc_draw_fill_triangle(px[0], py[0], px[i], py[i], px[i + 1], py[i + 1], fill);
    }
    if (n == 3) {
        nc_draw_tool_marker_line(px[1], py[1], px[0], py[0], edge, thick);
        nc_draw_tool_marker_line(px[1], py[1], px[2], py[2], edge, thick);
        nc_draw_tool_marker_line(px[0], py[0], px[2], py[2], mount, 1);
    } else {
        nc_draw_tool_marker_line(px[1], py[1], px[2], py[2], edge, thick > 1 ? thick : 2);
        nc_draw_tool_marker_line(px[3], py[3], px[0], py[0], mount, 1);
    }
}

void nc_draw_tool_glyph(int tip_x,
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
    rr = nc_draw_clampi(rr, 1, size / 3);
    corner = nc_draw_tool_tip_digit(tool->orient);
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
        nc_draw_tool_polygon(tip_x,
                                    tip_y,
                                    tool->orient,
                                    size,
                                    selected ? 2 : 1,
                                    fill,
                                    edge,
                                    NC_VISUAL_DIM);
    } else {
        nc_draw_tool_edges(tool->orient, &left, &top, &right, &bottom);
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

void nc_draw_tool_glyph_centered(int x,
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
        int n = nc_draw_tool_polygon_points(tip_x, tip_y, tool->orient, marker_size, px, py);
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
        corner = nc_draw_tool_tip_digit(tool->orient);
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

    nc_draw_tool_glyph(tip_x, tip_y, marker_size, tool, bg, selected);
}

void nc_draw_tool_cell(const char *line,
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
    nc_draw_text_clip(x, y, value, cols, cell_fg, cell_bg, LVDS_FONT_NORMAL);
}

void nc_draw_tool_param(const char *line,
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
    nc_draw_text_clip(x, y, buf, 7, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    if (!nc_tool_field_text(line, letter, value, sizeof(value))) {
        value[0] = '-';
        value[1] = '\0';
    }
    if (active) {
        lvds_draw_fill_rect(x + 68, y - 2, (value_cols * NC_VISUAL_CHAR_W) + 4, 20, value_bg);
    }
    nc_draw_text_clip(x + 70, y, value, value_cols, value_fg, value_bg, LVDS_FONT_NORMAL);
}

void nc_draw_dashdot_line(int x0,
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

void nc_draw_centerline(int x0, int y0, int x1, int y1)
{
    nc_draw_dashdot_line(x0, y0, x1, y1, NC_VISUAL_DIM);
}

void nc_draw_origin_marker(int x, int y)
{
    lvds_draw_ellipse(x, y, 11, 11, NC_VISUAL_TEXT);
    lvds_draw_ellipse(x, y, 6, 6, NC_VISUAL_TEXT);
    lvds_draw_line(x - 15, y, x - 8, y, NC_VISUAL_TEXT);
    lvds_draw_line(x + 8, y, x + 15, y, NC_VISUAL_TEXT);
    lvds_draw_line(x, y - 15, x, y - 8, NC_VISUAL_TEXT);
    lvds_draw_line(x, y + 8, x, y + 15, NC_VISUAL_TEXT);
}

void nc_draw_chuck_hatching(int x, int y, int w, int h, lvds_color_t color)
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

void nc_draw_arrowhead(int x, int y, int dir_x, int dir_y, lvds_color_t color)
{
    int px = -dir_y;
    int py = dir_x;

    lvds_draw_line(x, y, x - dir_x * 7 + px * 3, y - dir_y * 7 + py * 3, color);
    lvds_draw_line(x, y, x - dir_x * 7 - px * 3, y - dir_y * 7 - py * 3, color);
}

void nc_draw_diameter_dimension(int x,
                                              int y0,
                                              int y1,
                                              const char *label)
{
    lvds_draw_line(x, y0, x, y1, NC_VISUAL_DIM);
    nc_draw_arrowhead(x, y1, 0, 1, NC_VISUAL_DIM);
    if (label && label[0]) {
        lvds_draw_text(x + 6,
                       y1 - 8,
                       label,
                       NC_VISUAL_DIM,
                       NC_VISUAL_PREVIEW_BG,
                       LVDS_FONT_NORMAL);
    }
}

void nc_draw_z_point_dimension(int start_x,
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
        nc_draw_arrowhead(point_x, dim_y, -1, 0, NC_VISUAL_DIM);
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

void nc_draw_x_point_dimension(int dim_x,
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
    nc_draw_arrowhead(dim_x, point_y, 0, 1, NC_VISUAL_DIM);
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

void nc_draw_contour_point_marker(int x, int y, bool filled)
{
    if (filled) {
        lvds_draw_fill_ellipse(x, y, 3, 3, NC_VISUAL_TEXT);
    } else {
        lvds_draw_ellipse(x, y, 5, 5, NC_VISUAL_TEXT);
    }
}

void nc_draw_chuck(const nc_preview_info_t *preview,
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
    nc_draw_chuck_hatching(block_x, block_y, block_w, c_h, ink);
#endif
}

void nc_draw_chuck_relief(const nc_preview_info_t *preview,
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

bool nc_draw_explicit_arc(const nc_preview_info_t *preview,
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
    float abs_r = nc_draw_absf(r);
    float screen_r;
    int steps;
    int last_px;
    int last_py;
    int i;

    if (!preview || !nc_draw_r_arc_center(start_z, start_x, end_z, end_x, r, cw, &center)) {
        return false;
    }

    a0 = atan2f((start_x * 0.5f) - (center.x * 0.5f), start_z - center.z);
    sweep = nc_draw_directed_arc_sweep(a0,
                                         atan2f((end_x * 0.5f) - (center.x * 0.5f),
                                                end_z - center.z),
                                         cw);
    screen_r = abs_r * (float)stock_w / (preview->stock_z > 0.0001f ? preview->stock_z : 1.0f);
    steps = nc_draw_clampi((int)(nc_draw_absf(sweep) * screen_r * 0.35f) + 8,
                             10,
                             NC_PREVIEW_ARC_MAX_STEPS);
    last_px = nc_draw_preview_z(preview, z0_x, stock_w, start_z);
    last_py = nc_draw_preview_x(preview, stock_top, stock_h, start_x);
    for (i = 1; i <= steps; i++) {
        float a = a0 + sweep * ((float)i / (float)steps);
        float z = center.z + cosf(a) * abs_r;
        float x = ((center.x * 0.5f) + sinf(a) * abs_r) * 2.0f;
        int px = nc_draw_preview_z(preview, z0_x, stock_w, z);
        int py = nc_draw_preview_x(preview, stock_top, stock_h, x);
        lvds_draw_line_w(last_px, last_py, px, py, color, width);
        last_px = px;
        last_py = py;
    }
    return true;
}

bool nc_draw_center_arc(const nc_preview_info_t *preview,
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
    sweep = nc_draw_directed_arc_sweep(a0,
                                         atan2f(end_xr - center_xr,
                                                end_z - center_z),
                                         cw);
    screen_r = radius * (float)stock_w / (preview->stock_z > 0.0001f ? preview->stock_z : 1.0f);
    steps = nc_draw_clampi((int)(nc_draw_absf(sweep) * screen_r * 0.35f) + 8,
                             10,
                             NC_PREVIEW_ARC_MAX_STEPS);
    last_px = nc_draw_preview_z(preview, z0_x, stock_w, start_z);
    last_py = nc_draw_preview_x(preview, stock_top, stock_h, start_x);
    for (n = 1; n <= steps; n++) {
        float a = a0 + sweep * ((float)n / (float)steps);
        float z = center_z + cosf(a) * radius;
        float x = (center_xr + sinf(a) * radius) * 2.0f;
        int px = nc_draw_preview_z(preview, z0_x, stock_w, z);
        int py = nc_draw_preview_x(preview, stock_top, stock_h, x);
        lvds_draw_line_w(last_px, last_py, px, py, color, width);
        last_px = px;
        last_py = py;
    }
    return true;
}

void nc_draw_dashed_segment(const nc_preview_info_t *preview,
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

    px0 = nc_draw_preview_z(preview, z0_x, stock_w, z0);
    py0 = nc_draw_preview_x(preview, stock_top, stock_h, x0);
    px1 = nc_draw_preview_z(preview, z0_x, stock_w, z1);
    py1 = nc_draw_preview_x(preview, stock_top, stock_h, x1);
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
        xa = nc_draw_preview_z(preview, z0_x, stock_w, z0 + dz * a);
        ya = nc_draw_preview_x(preview, stock_top, stock_h, x0 + dx * a);
        xb = nc_draw_preview_z(preview, z0_x, stock_w, z0 + dz * b);
        yb = nc_draw_preview_x(preview, stock_top, stock_h, x0 + dx * b);
        lvds_draw_line(xa, ya, xb, yb, color);
    }
}

bool nc_draw_emitted_motion_line(const nc_preview_info_t *preview,
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
    has_x = nc_preview_line_word_float(line, 'X', &x);
    has_z = nc_preview_line_word_float(line, 'Z', &z);
    if (!*have_last) {
        *last_x = has_x ? x : preview->stock_x;
        *last_z = has_z ? z : 0.0f;
        *have_last = true;
        return false;
    }
    if (!has_x && !has_z) {
        return false;
    }

    x0 = nc_draw_preview_z(preview, z0_x, stock_w, *last_z);
    y0 = nc_draw_preview_x(preview, stock_top, stock_h, *last_x);
    x1 = nc_draw_preview_z(preview, z0_x, stock_w, z);
    y1 = nc_draw_preview_x(preview, stock_top, stock_h, x);
    if (segment == NC_PREVIEW_SEG_ROUGH) {
        color = NC_VISUAL_TEXT;
    } else if (segment == NC_PREVIEW_SEG_FINISH) {
        color = NC_VISUAL_TEXT;
    }

    if (cmd == G7X_CONTOUR_ARC_CW || cmd == G7X_CONTOUR_ARC_CCW) {
        float r = 0.0f;
        float i_off = 0.0f;
        float k_off = 0.0f;
        if ((nc_preview_line_word_float(line, 'I', &i_off) &&
             nc_preview_line_word_float(line, 'K', &k_off) &&
             nc_draw_center_arc(preview,
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
            (nc_preview_line_word_float(line, 'R', &r) &&
             nc_draw_explicit_arc(preview,
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
        nc_draw_dashed_segment(preview,
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
        nc_draw_dashed_segment(preview,
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

int nc_draw_wrap_lines(const char *text,
                                int cols,
                                char lines[NC_FOOTER_LINES][24])
{
    int count = 0;
    const char *p = text;

    if (!text || cols < 1) {
        return 0;
    }
    while (*p && count < NC_FOOTER_LINES) {
        const char *start = p;
        const char *wrap_at = 0;
        int len = 0;

        while (p[len] && len < cols) {
            if (p[len] == ' ') {
                wrap_at = p + len;
            }
            len++;
        }
        if (p[len] && wrap_at) {
            len = (int)(wrap_at - start);
            p = wrap_at + 1;
        } else {
            p = start + len;
        }
        if (len > 23) {
            len = 23;
        }
        memcpy(lines[count], start, (size_t)len);
        lines[count][len] = '\0';
        count++;
        while (*p == ' ') {
            p++;
        }
    }
    return count;
}

void nc_draw_footer_status(const char *message, const char *footer_text)
{
    char field[24];
    const char *p = footer_text ? footer_text : "";
    int fields = 1;
    int field_w;
    int i;

    (void)message;

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
    field_w = (LVDS_HSTX_WIDTH - 12) / fields;

    for (i = 0; i < fields; i++) {
        const char *bar = strchr(p, '|');
        const char *space;
        char key[8];
        const char *label;
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        bool active = false;
        /* Keys keep a small gap between them. The bottom few rows of the panel
           are left as page background: the close of the frame does not come out
           clean on the glass there (see nc/TODO.md), and content in those rows
           shows as a broken edge. */
        int bx = 6 + i * field_w + 1;
        int bw = field_w - 3;
        int bh = NC_FOOTER_H - 4;
        int by = NC_FOOTER_Y;
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

        if (!label[0]) {
            p = bar ? (bar + 1) : "";
            continue;
        }

        lvds_draw_fill_rect(bx, by, bw, bh, button_bg);
        lvds_draw_rect(bx, by, bw, bh, NC_VISUAL_DIM);

        {
            /* Footer keys are the same white keys as the 3x3 helper: the
               number sits in the top-left corner and the label has room for up
               to three wrapped lines. */
            char lines[NC_FOOTER_LINES][24];
            int indent = 4;
            int cols;
            int line_h = NC_FONT_NORMAL_H + 2;
            int block_y;
            int n;
            int j;

            if (key[0]) {
                lvds_draw_text(bx + 4, by + 3, key, NC_VISUAL_ACCENT, button_bg,
                               LVDS_FONT_NORMAL);
                indent += lvds_draw_text_width(key, LVDS_FONT_NORMAL) + 6;
            }
            cols = (bw - indent - 4) / NC_VISUAL_CHAR_W;
            n = nc_draw_wrap_lines(label, cols, lines);
            if (n > 0) {
                block_y = by + (bh - n * line_h) / 2;
                for (j = 0; j < n; j++) {
                    nc_draw_text_clip(bx + indent,
                                             block_y + j * line_h,
                                             lines[j],
                                             (int)strlen(lines[j]),
                                             button_fg,
                                             button_bg,
                                             LVDS_FONT_NORMAL);
                }
            }
        }

        p = bar ? (bar + 1) : "";
    }
}

void nc_draw_text_clip(int x,
                                     int y,
                                     const char *text,
                                     int cols,
                                     lvds_color_t fg,
                                     lvds_color_t bg,
                                     int font)
{
    lvds_draw_text_clip(x, y, text ? text : "", cols, fg, bg, font);
}

void nc_draw_modal_items(int x,
                                       int y,
                                       const nc_footer_item_t *items,
                                       size_t count,
                                       uint16_t active_mask)
{
    int row;
    int col;

    /* Only the keys are drawn, with the program still visible above and below
       them, so the helper stays attached to the line it belongs to. */
    for (row = 0; row < NC_MODAL_ROWS; row++) {
        for (col = 0; col < NC_MODAL_COLS; col++) {
            int cx = x + NC_MODAL_PAD + col * NC_MODAL_KEY_W;
            int cy = y + NC_MODAL_PAD + row * NC_MODAL_KEY_H;
            int digit = (NC_MODAL_ROWS - 1 - row) * NC_MODAL_COLS + col + 1;
            const nc_footer_item_t *item = 0;
            lvds_color_t key_bg = (active_mask & (1u << digit))
                                      ? NC_VISUAL_FOOTER_VALUE
                                      : NC_VISUAL_FOOTER_BUTTON;
            size_t i;

            for (i = 0; i < count; i++) {
                if ((int)items[i].key - '0' == digit) {
                    item = &items[i];
                    break;
                }
            }

            lvds_draw_fill_rect(cx, cy, NC_MODAL_KEY_W, NC_MODAL_KEY_H, key_bg);
            lvds_draw_rect(cx, cy, NC_MODAL_KEY_W, NC_MODAL_KEY_H,
                           NC_VISUAL_DIM);
            if (!item) {
                continue;
            }
            {
                /* One font for every key, as on the footer, with the digit as
                   the corner marker. The label is clipped to its own length:
                   the draw helper fills the whole column count it is given. */
                char key[4];
                int cols = (NC_MODAL_KEY_W - 8) / NC_VISUAL_CHAR_W;
                int label_len = (int)strlen(item->label);
                int label_x;

                if (label_len > cols) {
                    label_len = cols;
                }
                label_x = cx + (NC_MODAL_KEY_W - label_len * NC_VISUAL_CHAR_W) / 2;
                if (label_x < cx + 4) {
                    label_x = cx + 4;
                }
                snprintf(key, sizeof(key), "%d", digit);
                lvds_draw_text(cx + 5, cy + 4, key, NC_VISUAL_ACCENT,
                               key_bg, LVDS_FONT_NORMAL);
                nc_draw_text_clip(label_x,
                                         cy + (NC_MODAL_KEY_H - NC_FONT_NORMAL_H) / 2,
                                         item->label,
                                         label_len,
                                         NC_VISUAL_FOOTER_TEXT,
                                         key_bg,
                                         LVDS_FONT_NORMAL);
            }
        }
    }
}
