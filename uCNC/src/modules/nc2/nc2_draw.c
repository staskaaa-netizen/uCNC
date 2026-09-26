#include "nc2_draw.h"

#include "../../cnc.h"
#include "../lvds_renderer/lvds_hstx.h"
#include "../lvds_renderer/lvds_palette.h"

#include <string.h>

void nc2_draw_init(void)
{
    lvds_palette_init();
}

lvds_color_t nc2_col_bg(void) { return lvds_palette_element(LC_ELEM_BACKGROUND); }
lvds_color_t nc2_col_panel(void) { return lvds_palette_element(LC_ELEM_PANEL); }
lvds_color_t nc2_col_header(void) { return lvds_palette_element(LC_ELEM_HEADER); }
lvds_color_t nc2_col_text(void) { return lvds_palette_element(LC_ELEM_TEXT); }
lvds_color_t nc2_col_dim(void) { return lvds_palette_element(LC_ELEM_SECONDARY_TEXT); }
lvds_color_t nc2_col_accent(void) { return lvds_palette_element(LC_ELEM_VALUE_TEXT); }
lvds_color_t nc2_col_select(void) { return lvds_palette_element(LC_ELEM_SELECTED_ROW); }
/* The pad's keys are the machine's own buttons: white with black on them, the way
   the footer's keys were - `LC_ELEM_PANEL` is the *screen's* green, not a key. */
lvds_color_t nc2_col_pad_key(void) { return lvds_palette_color(white_warm); }
lvds_color_t nc2_col_pad_hot(void) { return lvds_palette_color(yellow); }
/* The field being typed into: a bright box that stands out on the cursor's own
   row as much as on any other. */
lvds_color_t nc2_col_field_bg(void) { return lvds_palette_color(white_warm); }
lvds_color_t nc2_col_field_fg(void) { return lvds_palette_color(black); }
lvds_color_t nc2_col_block(void) { return lvds_palette_color(yellow_light); }
lvds_color_t nc2_col_run(void) { return lvds_palette_color(green); }
lvds_color_t nc2_col_error(void) { return lvds_palette_element(LC_ELEM_ERROR); }

/* The preview's own elements: the drawing has its own background, frame, stock
   and cut colours, so a pane can be read as a drawing and not as a screen. */
lvds_color_t nc2_col_prev_bg(void) { return lvds_palette_element(LC_ELEM_PREVIEW_BG); }
lvds_color_t nc2_col_prev_frame(void) { return lvds_palette_element(LC_ELEM_PREVIEW_FRAME); }
lvds_color_t nc2_col_prev_stock(void) { return lvds_palette_element(LC_ELEM_PREVIEW_STOCK); }
lvds_color_t nc2_col_prev_hatch(void) { return lvds_palette_element(LC_ELEM_PREVIEW_HATCH); }
lvds_color_t nc2_col_prev_cut(void) { return lvds_palette_element(LC_ELEM_PREVIEW_CUT); }
lvds_color_t nc2_col_prev_profile(void) { return lvds_palette_element(LC_ELEM_PREVIEW_PROFILE); }
lvds_color_t nc2_col_tool(void) { return lvds_palette_element(LC_ELEM_PREVIEW_TOOL); }
lvds_color_t nc2_col_tool_fill(void) { return lvds_palette_element(LC_ELEM_PREVIEW_TOOL_MARK); }

int nc2_clampi(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

int nc2_col_width(int font)
{
    return nc2_text_width("0", font);
}

int nc2_text_width(const char *text, int font)
{
    return lvds_draw_text_width(text, font);
}

void nc2_text(int x, int y, const char *text, lvds_color_t fg, lvds_color_t bg,
              int font)
{
    lvds_draw_text(x, y, text ? text : "", fg, bg, font);
}

void nc2_text_clip(int x, int y, const char *text, int cols, lvds_color_t fg,
                   lvds_color_t bg, int font)
{
    lvds_draw_text_clip(x, y, text ? text : "", cols, fg, bg, font);
}

void nc2_fill(int x, int y, int w, int h, lvds_color_t color)
{
    lvds_draw_fill_rect(x, y, w, h, color);
}

void nc2_frame(int x, int y, int w, int h, lvds_color_t color)
{
    lvds_draw_rect(x, y, w, h, color);
}

void nc2_hline(int x, int y, int w, lvds_color_t color)
{
    lvds_draw_line(x, y, x + w - 1, y, color);
}

/* One cell, in the shape the machine's own buttons have: the number in the top
   left, the label under it, cut into at most two lines so a long name stays
   inside the key. Words too long for a line are cut, not wrapped mid-word - the
   key is a button, and a half-word reads worse than a clipped one. */
static void nc2_draw_pad_cell(int x, int y, int w, int h, char key,
                              const char *label, bool hot)
{
    lvds_color_t key_col = hot ? nc2_col_pad_hot() : nc2_col_pad_key();
    lvds_color_t fg = nc2_col_field_fg();
    char number[2];
    const char *rest = label;
    int line_y = y + 18;
    int lines = 0;

    nc2_fill(x, y, w, h, key_col);
    nc2_frame(x, y, w, h, nc2_col_dim());
    number[0] = key;
    number[1] = '\0';
    nc2_text(x + 4, y + 3, number, fg, key_col, LVDS_FONT_NORMAL);
    while (rest && *rest && lines < 2) {
        const char *end = rest + strlen(rest);
        char piece[24];
        size_t len;

        /* Two lines at the cell's width: the second one takes what is left. */
        while (end > rest && (size_t)(end - rest) > (size_t)(w - 8) /
                             (size_t)nc2_col_width(LVDS_FONT_SMALL) + 1u) {
            end--;
        }
        len = (size_t)(end - rest);
        if (!lines && *end) {
            const char *space = end;

            while (space > rest && *space != ' ') {
                space--;
            }
            if (space > rest) {
                len = (size_t)(space - rest);
                end = space;
            }
        }
        if (len >= sizeof(piece)) {
            len = sizeof(piece) - 1u;
        }
        memcpy(piece, rest, len);
        piece[len] = '\0';
        nc2_text_clip(x + 4, line_y, piece, (w - 8) / nc2_col_width(LVDS_FONT_SMALL),
                      fg, key_col, LVDS_FONT_SMALL);
        line_y += 14;
        lines++;
        while (*end == ' ') {
            end++;
        }
        rest = end;
    }
}

char nc2_pad_cell_key(int row, int col)
{
    if (row < 0 || row > 2 || col < 0 || col > 2) {
        return 0;
    }
    return (char)('1' + (2 - row) * 3 + col);
}

/* --- the tool drawing -----------------------------------------------------

   A tool's shape comes from its orientation code, the `O` word of its row: one
   digit is a corner (`1`-`9`, the keypad's own arrangement, `5` the middle), and
   four digits are the insert's four corners around the tip. `nc` drew its tools
   this way (`nc_draw.c`) and the panel draws them the same way now - the tool
   table and its picture are `nc2`'s, with nc's file as the record. */

/* The corner the tip points at: the digit that says it, one digit as it is, four
   digits as the second from the right (the cutting edge, as nc read it). */
static int nc2_tool_tip_digit(int orient)
{
    int digits[4];
    int n = 0;
    int tmp = orient;

    if (orient >= 1 && orient <= 9 && orient != 5) {
        return orient;
    }
    if (orient > 9) {
        while (tmp > 0 && n < 4) {
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

/* The digits of an orientation code, most significant first. */
static int nc2_tool_orient_digits(int orient, int *digits, int max_digits)
{
    int tmp[4];
    int n = 0;
    int i;

    while (orient > 0 && n < 4) {
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

/* Where one digit of the orientation sits, as a point on the keypad: the digit
   is the key, `step` the distance between two of them. */
static bool nc2_tool_digit_point(int digit, int step, int *x, int *y)
{
    if (digit < 1 || digit > 9) {
        return false;
    }
    if (x) {
        *x = ((digit - 1) % 3 - 1) * step;
    }
    if (y) {
        *y = (1 - (digit - 1) / 3) * step;
    }
    return true;
}

/* A filled triangle: three lines' worth of spans, which is all a tool shape
   needs - the renderer has no polygon. */
static void nc2_fill_triangle(int x1, int y1, int x2, int y2, int x3, int y3,
                              lvds_color_t color)
{
    int min_y = y1;
    int max_y = y1;
    int y;

    if (y2 < min_y) min_y = y2;
    if (y3 < min_y) min_y = y3;
    if (y2 > max_y) max_y = y2;
    if (y3 > max_y) max_y = y3;
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
            if (xb >= xa) {
                nc2_fill(xa, y, xb - xa + 1, 1, color);
            }
        }
    }
}

/* The corners of an insert: the orientation's own digits, moved so the tip sits
   on the point the caller asked for. */
static int nc2_tool_polygon_points(int tip_x, int tip_y, int orient, int size,
                                   int *px, int *py)
{
    int digits[4];
    int tip_grid_x = 0;
    int tip_grid_y = 0;
    int step = nc2_clampi(size / 2, 6, 56);
    int n = nc2_tool_orient_digits(orient, digits, 4);
    int i;

    if (n < 3) {
        return 0;
    }
    if (n == 4) {
        int cut_x = 0;
        int cut_y = 0;
        int z_x = 0;
        int z_y = 0;

        /* Four digits: the second is the cutting edge, the third the zero
           direction, and the tip is where the two cross. */
        if (!nc2_tool_digit_point(digits[1], step, &cut_x, &cut_y) ||
            !nc2_tool_digit_point(digits[2], step, &z_x, &z_y)) {
            return 0;
        }
        tip_grid_x = cut_x;
        tip_grid_y = z_y;
    } else if (!nc2_tool_digit_point(nc2_tool_tip_digit(orient), step,
                                     &tip_grid_x, &tip_grid_y)) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        int gx = 0;
        int gy = 0;

        if (!nc2_tool_digit_point(digits[i], step, &gx, &gy)) {
            return 0;
        }
        px[i] = tip_x + gx - tip_grid_x;
        py[i] = tip_y + gy - tip_grid_y;
    }
    return n;
}

/* Which sides of a corner tool carry an edge. */
static void nc2_tool_edges(int orient, bool *left, bool *top, bool *right,
                           bool *bottom)
{
    int o = nc2_tool_tip_digit(orient);

    if (left) *left = (o == 1 || o == 4 || o == 7 || o == 2 || o == 5 || o == 8);
    if (top) *top = (o == 7 || o == 8 || o == 9 || o == 4 || o == 5 || o == 6);
    if (right) *right = (o == 3 || o == 6 || o == 9 || o == 2 || o == 5 || o == 8);
    if (bottom) *bottom = (o == 1 || o == 2 || o == 3 || o == 4 || o == 5 || o == 6);
}

/* The tool, with its tip on (tip_x, tip_y) and its body the size given: the
   shape nc drew, in the panel's own colours. */
void nc2_draw_tool_glyph(int tip_x, int tip_y, int size, const nc2_tool_t *tool,
                         lvds_color_t bg)
{
    int rr;
    int sx;
    int sy;
    bool left;
    bool top;
    bool right;
    bool bottom;
    lvds_color_t edge = nc2_col_tool();
    lvds_color_t fill = nc2_col_tool_fill();

    if (!tool || !tool->valid || size <= 0) {
        return;
    }
    rr = tool->r > 0.0f ? (int)(tool->r * 8.0f) : 2;
    rr = nc2_clampi(rr, 1, size / 3);
    switch (nc2_tool_tip_digit(tool->orient)) {
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
        /* No orientation written: a round nose, which is what a `T` with no `O`
           means. */
        nc2_fill(tip_x - 4, tip_y - 4, 9, 9, fill);
        nc2_frame(tip_x - 4, tip_y - 4, 9, 9, edge);
    } else if (tool->orient == 5) {
        /* The middle key: a round insert seen from above. */
        nc2_fill(tip_x - size / 2, tip_y - size / 2, size, size, fill);
        nc2_frame(tip_x - size / 2, tip_y - size / 2, size, size, edge);
        lvds_draw_line(tip_x - size / 2, tip_y, tip_x + size / 2, tip_y, edge);
        lvds_draw_line(tip_x, tip_y - size / 2, tip_x, tip_y + size / 2, edge);
    } else if (tool->orient > 9) {
        int px[4];
        int py[4];
        int n = nc2_tool_polygon_points(tip_x, tip_y, tool->orient, size, px, py);
        int i;

        if (n < 3) {
            return;
        }
        for (i = 1; i + 1 < n; i++) {
            nc2_fill_triangle(px[0], py[0], px[i], py[i], px[i + 1], py[i + 1],
                              fill);
        }
        if (n == 3) {
            lvds_draw_line_w(px[1], py[1], px[0], py[0], edge, 1);
            lvds_draw_line_w(px[1], py[1], px[2], py[2], edge, 1);
            lvds_draw_line(px[0], py[0], px[2], py[2], nc2_col_dim());
        } else {
            lvds_draw_line_w(px[1], py[1], px[2], py[2], edge, 2);
            lvds_draw_line(px[3], py[3], px[0], py[0], nc2_col_dim());
        }
    } else {
        nc2_tool_edges(tool->orient, &left, &top, &right, &bottom);
        nc2_fill(sx + 1, sy + 1, size - 1, size - 1, fill);
        if (left) lvds_draw_line(sx, sy, sx, sy + size, edge);
        if (top) lvds_draw_line(sx, sy, sx + size, sy, edge);
        if (right) lvds_draw_line(sx + size, sy, sx + size, sy + size, edge);
        if (bottom) lvds_draw_line(sx, sy + size, sx + size, sy + size, edge);
        nc2_fill(tip_x - rr, tip_y - rr, rr * 2 + 1, rr * 2 + 1, edge);
        nc2_fill(tip_x - rr + 1, tip_y - rr + 1, rr * 2 - 1, rr * 2 - 1, bg);
    }
    nc2_fill(tip_x - 1, tip_y - 1, 3, 3, edge);
}

/* The same glyph centred in a box of its own, so a table row can carry a small
   tool and the view a large one. */
void nc2_draw_tool_glyph_centered(int x, int y, int box_size, int marker_size,
                                  const nc2_tool_t *tool, lvds_color_t bg)
{
    int tip_x;
    int tip_y;

    if (!tool || !tool->valid) {
        return;
    }
    tip_x = x + box_size / 2;
    tip_y = y + box_size / 2;
    if (tool->orient > 9) {
        int px[4];
        int py[4];
        int n = nc2_tool_polygon_points(tip_x, tip_y, tool->orient, marker_size,
                                        px, py);

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
            tip_x += (x + box_size / 2) - ((min_x + max_x) / 2);
            tip_y += (y + box_size / 2) - ((min_y + max_y) / 2);
        }
    } else if (tool->orient > 0 && tool->orient <= 9 && tool->orient != 5) {
        switch (nc2_tool_tip_digit(tool->orient)) {
        case 7:
            tip_x = x;
            tip_y = y;
            break;
        case 9:
            tip_x = x + box_size;
            tip_y = y;
            break;
        case 1:
            tip_x = x;
            tip_y = y + box_size;
            break;
        case 3:
        default:
            tip_x = x + box_size;
            tip_y = y + box_size;
            break;
        }
    }
    nc2_draw_tool_glyph(tip_x, tip_y, marker_size, tool, bg);
}

void nc2_draw_pad(int x, int y, int w, int h, const char *const *labels,
                  char hot)
{
    int col;
    int row;
    int cell_w = w / 3;
    int cell_h = h / 3;

    for (row = 0; row < 3; row++) {
        for (col = 0; col < 3; col++) {
            char key = nc2_pad_cell_key(row, col);
            /* The labels come in key order 1..9, so a cell takes its own key's
               label - not the one that happens to sit at its row and column. */
            const char *label = labels ? labels[key - '1'] : "";

            if (!label) {
                label = "";
            }
            nc2_draw_pad_cell(x + col * cell_w, y + row * cell_h, cell_w, cell_h,
                              key, label, key == hot);
        }
    }
}
