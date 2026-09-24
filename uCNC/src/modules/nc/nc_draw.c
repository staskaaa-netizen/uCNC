/* The panel's drawing vocabulary - see nc_draw.h. */
#include "nc_draw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../cnc.h"
#include "../lvds_renderer/lvds_draw_api.h"
#include "../lvds_renderer/lvds_hstx.h"


int nc_draw_clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}






static int nc_draw_tool_tip_digit(int orient)
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

static int nc_draw_tool_orient_digits(int orient, int *digits, int max_digits)
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

static bool nc_draw_tool_keypad_point(int digit, int ox, int oy, int step, int *x, int *y)
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

static void nc_draw_tool_edges(int orient, bool *left, bool *top, bool *right, bool *bottom)
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

static void nc_draw_tool_marker_line(int x1,
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

static int nc_draw_tool_polygon_points(int tip_x,
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

static void nc_draw_tool_polygon(int tip_x,
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
    /* The tool tip reads in its own colour: the field being edited keeps the
       editor's word colours, so the cursor is still the odd one out. */
    lvds_color_t value_fg = active ? NC_VISUAL_WORD_FG : NC_VISUAL_TOOL_TIP;
    lvds_color_t value_bg = active ? NC_VISUAL_WORD_BG : NC_VISUAL_BG;

    snprintf(buf, sizeof(buf), "%-7s", label ? label : "");
    nc_draw_text_clip(x, y, buf, 7, NC_VISUAL_TOOL_TIP, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    if (!nc_tool_field_text(line, letter, value, sizeof(value))) {
        value[0] = '-';
        value[1] = '\0';
    }
    if (active) {
        lvds_draw_fill_rect(x + 68, y - 2, (value_cols * NC_VISUAL_CHAR_W) + 4, 20, value_bg);
    }
    nc_draw_text_clip(x + 70, y, value, value_cols, value_fg, value_bg, LVDS_FONT_NORMAL);
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
        /* The top-right corner is cut. Everything that has to stay inside the
           key - the label's wrap width - is measured against the cut side. */
        int chamfer = bh / NC_KEY_CHAMFER_DIVISOR;
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
        if (chamfer > 0) {
            /* Cut, not rounded: the corner is page background, and the edge of
               the cut is drawn like the rest of the outline. */
            nc_draw_fill_triangle(bx + bw - 1 - chamfer, by,
                                  bx + bw - 1, by,
                                  bx + bw - 1, by + chamfer,
                                  NC_VISUAL_BG);
            lvds_draw_line(bx + bw - 1 - chamfer, by,
                           bx + bw - 1, by + chamfer,
                           NC_VISUAL_DIM);
        }

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
            cols = (bw - indent - 4 - chamfer) / NC_VISUAL_CHAR_W;
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
