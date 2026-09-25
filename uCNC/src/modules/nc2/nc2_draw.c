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

void nc2_draw_pad(int x, int y, int w, int h, const char *const *labels,
                  char hot)
{
    int col;
    int row;
    int cell_w = w / 3;
    int cell_h = h / 3;

    for (row = 0; row < 3; row++) {
        for (col = 0; col < 3; col++) {
            char key = (char)('1' + row * 3 + col);
            const char *label = labels ? labels[row * 3 + col] : "";

            if (!label) {
                label = "";
            }
            nc2_draw_pad_cell(x + col * cell_w, y + row * cell_h, cell_w, cell_h,
                              key, label, key == hot);
        }
    }
}
