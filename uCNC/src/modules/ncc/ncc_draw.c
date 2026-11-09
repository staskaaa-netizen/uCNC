#include "ncc_draw.h"

#include "../lvds_renderer/lvds_hstx.h"
#include "../lvds_renderer/lvds_palette.h"

#include <math.h>
#include <string.h>

void ncc_draw_init(void)
{
    lvds_palette_init();
}

lvds_color_t ncc_col_bg(void) { return lvds_palette_element(LC_ELEM_BACKGROUND); }
lvds_color_t ncc_col_panel(void) { return lvds_palette_element(LC_ELEM_PANEL); }
lvds_color_t ncc_col_header(void) { return lvds_palette_element(LC_ELEM_HEADER); }
lvds_color_t ncc_col_text(void) { return lvds_palette_element(LC_ELEM_TEXT); }
lvds_color_t ncc_col_dim(void) { return lvds_palette_element(LC_ELEM_SECONDARY_TEXT); }
lvds_color_t ncc_col_accent(void) { return lvds_palette_element(LC_ELEM_VALUE_TEXT); }
lvds_color_t ncc_col_select(void) { return lvds_palette_element(LC_ELEM_SELECTED_ROW); }
/* The pad's keys are the machine's own buttons: white with black on them, the way
   the footer's keys were - `LC_ELEM_PANEL` is the *screen's* green, not a key. */
lvds_color_t ncc_col_pad_key(void) { return lvds_palette_color(white_warm); }
lvds_color_t ncc_col_pad_hot(void) { return lvds_palette_color(yellow); }
/* What is drawn *on a key*: the pad's number and label, and the values on a
   marked row, are the panel's dark ink on the key's own white. */
lvds_color_t ncc_col_pad_text(void) { return lvds_palette_color(black); }
/* The field being typed into: nc's own word box - the value in white on the
   panel's black, which is what makes it read as the thing the digits go into
   (bench: "text to be edit - white on black background"). */
lvds_color_t ncc_col_field_bg(void) { return lvds_palette_color(black); }
lvds_color_t ncc_col_field_fg(void) { return lvds_palette_color(white_warm); }
lvds_color_t ncc_col_block(void) { return lvds_palette_color(yellow_light); }
lvds_color_t ncc_col_run(void) { return lvds_palette_color(green); }
lvds_color_t ncc_col_error(void) { return lvds_palette_element(LC_ELEM_ERROR); }

/* The preview's own elements: the drawing has its own background, frame, stock
   and cut colours, so a pane can be read as a drawing and not as a screen. */
lvds_color_t ncc_col_prev_bg(void) { return lvds_palette_element(LC_ELEM_PREVIEW_BG); }
lvds_color_t ncc_col_prev_frame(void) { return lvds_palette_element(LC_ELEM_PREVIEW_FRAME); }
lvds_color_t ncc_col_prev_stock(void) { return lvds_palette_element(LC_ELEM_PREVIEW_STOCK); }
/* The chuck: nc's own white bar with its dark hatching, and the pivot circle it
   hangs on. */
lvds_color_t ncc_col_prev_chuck(void) { return lvds_palette_color(white_warm); }
lvds_color_t ncc_col_prev_chuck_ink(void) { return lvds_palette_color(black); }
lvds_color_t ncc_col_prev_hatch(void) { return lvds_palette_element(LC_ELEM_PREVIEW_HATCH); }
lvds_color_t ncc_col_prev_cut(void) { return lvds_palette_element(LC_ELEM_PREVIEW_CUT); }
lvds_color_t ncc_col_prev_profile(void) { return lvds_palette_element(LC_ELEM_PREVIEW_PROFILE); }
lvds_color_t ncc_col_tool(void) { return lvds_palette_element(LC_ELEM_PREVIEW_TOOL); }
int ncc_clampi(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

int ncc_col_width(int font)
{
    return ncc_text_width("0", font);
}

int ncc_text_width(const char *text, int font)
{
    return lvds_draw_text_width(text, font);
}

void ncc_text(int x, int y, const char *text, lvds_color_t fg, lvds_color_t bg,
              int font)
{
    lvds_draw_text(x, y, text ? text : "", fg, bg, font);
}

void ncc_text_clip(int x, int y, const char *text, int cols, lvds_color_t fg,
                   lvds_color_t bg, int font)
{
    lvds_draw_text_clip(x, y, text ? text : "", cols, fg, bg, font);
}

void ncc_fill(int x, int y, int w, int h, lvds_color_t color)
{
    lvds_draw_fill_rect(x, y, w, h, color);
}

void ncc_frame(int x, int y, int w, int h, lvds_color_t color)
{
    lvds_draw_rect(x, y, w, h, color);
}

/* One cell, in the shape the machine's own buttons have: the number in the top
   left, the label under it, cut into at most two lines so a long name stays
   inside the key. Words too long for a line are cut, not wrapped mid-word - the
   key is a button, and a half-word reads worse than a clipped one. */
static void ncc_fill_triangle(int x1, int y1, int x2, int y2, int x3, int y3,
                              lvds_color_t color);

static void ncc_draw_pad_cell(int x, int y, int w, int h, char key,
                              const char *label, bool hot)
{
    lvds_color_t key_col = hot ? ncc_col_pad_hot() : ncc_col_pad_key();
    lvds_color_t fg = ncc_col_pad_text();
    char number[2];
    const char *rest = label;
    int line_y = y + 18;
    int lines = 0;

    ncc_fill(x, y, w, h, key_col);
    ncc_frame(x, y, w, h, ncc_col_dim());
    /* The top-right corner is cut at 45 degrees, a quarter of the key's height,
       the way nc's own keys were: the cut is the page's own background and its
       edge is drawn like the rest of the outline, so the key reads as one
       machined button. */
    {
        int chamfer = h / NCC_KEY_CHAMFER_DIVISOR;

        if (chamfer > 0) {
            ncc_fill_triangle(x + w - 1 - chamfer, y, x + w - 1, y,
                              x + w - 1, y + chamfer, ncc_col_bg());
            lvds_draw_line(x + w - 1 - chamfer, y, x + w - 1, y + chamfer,
                           ncc_col_dim());
        }
    }
    number[0] = key;
    number[1] = '\0';
    ncc_text(x + 4, y + 3, number, fg, key_col, LVDS_FONT_NORMAL);
    while (rest && *rest && lines < 2) {
        const char *end = rest + strlen(rest);
        char piece[24];
        size_t len;

        /* Two lines at the cell's width: the second one takes what is left. */
        /* One font for the key, the number and the label together - nc's own
           rule for its keys ("one font for every key, as on the footer") - so
           the name is not the small print under a big digit (bench: "then
           number is bigger. it is wrong"). A name too long for the key wraps
           onto the second line rather than shrinking. */
        while (end > rest && (size_t)(end - rest) > (size_t)(w - 8) /
                             (size_t)ncc_col_width(LVDS_FONT_NORMAL) + 1u) {
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
        ncc_text_clip(x + 4, line_y, piece,
                      (w - 8) / ncc_col_width(LVDS_FONT_NORMAL),
                      fg, key_col, LVDS_FONT_NORMAL);
        line_y += 16;
        lines++;
        while (*end == ' ') {
            end++;
        }
        rest = end;
    }
}

char ncc_pad_cell_key(int row, int col)
{
    if (row < 0 || row > 2 || col < 0 || col > 2) {
        return 0;
    }
    return (char)('1' + (2 - row) * 3 + col);
}

/* A filled triangle: three lines' worth of spans, for the pad key's chamfer. */
static void ncc_fill_triangle(int x1, int y1, int x2, int y2, int x3, int y3,
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
                ncc_fill(xa, y, xb - xa + 1, 1, color);
            }
        }
    }
}


void ncc_draw_pad(int x, int y, int w, int h, const char *const *labels,
                  char hot)
{
    int col;
    int row;
    int cell_w = w / 3;
    int cell_h = h / 3;

    for (row = 0; row < 3; row++) {
        for (col = 0; col < 3; col++) {
            char key = ncc_pad_cell_key(row, col);
            /* The labels come in key order 1..9, so a cell takes its own key's
               label - not the one that happens to sit at its row and column. */
            const char *label = labels ? labels[key - '1'] : "";

            if (!label) {
                label = "";
            }
            ncc_draw_pad_cell(x + col * cell_w, y + row * cell_h, cell_w, cell_h,
                              key, label, key == hot);
        }
    }
}
