#ifndef NCC_DRAW_H
#define NCC_DRAW_H

#include "../lvds_renderer/lvds_draw_api.h"

#include <stdbool.h>

#define NCC_KEY_CHAMFER_DIVISOR 4

/* ncc's pixel vocabulary: palette roles, bounded text, rectangles and the 3x3
   key face. Everything here is data in and pixels out. */

void ncc_draw_init(void);

lvds_color_t ncc_col_bg(void);
lvds_color_t ncc_col_panel(void);
lvds_color_t ncc_col_header(void);
lvds_color_t ncc_col_text(void);
lvds_color_t ncc_col_dim(void);
lvds_color_t ncc_col_accent(void);
lvds_color_t ncc_col_select(void);
lvds_color_t ncc_col_pad_key(void);
lvds_color_t ncc_col_pad_hot(void);
/* The ink for anything drawn on a key: the pad's number and label, and the
   values a marked row shows. */
lvds_color_t ncc_col_pad_text(void);
lvds_color_t ncc_col_field_bg(void);
lvds_color_t ncc_col_field_fg(void);
/* The weaker mark beside the line the run is on: the source rows of the block
   that line heads. */
lvds_color_t ncc_col_block(void);
/* The run's own colours: the panel's green while the machine is moving, and its
   red for a fault - the two things the machine's strip can say. */
lvds_color_t ncc_col_run(void);
lvds_color_t ncc_col_error(void);
lvds_color_t ncc_col_prev_bg(void);
lvds_color_t ncc_col_prev_frame(void);
lvds_color_t ncc_col_prev_stock(void);
lvds_color_t ncc_col_prev_chuck(void);
lvds_color_t ncc_col_prev_chuck_ink(void);
lvds_color_t ncc_col_prev_hatch(void);
lvds_color_t ncc_col_prev_cut(void);
lvds_color_t ncc_col_prev_profile(void);
/* The nose circle on the live cut. */
lvds_color_t ncc_col_tool(void);
/* Keep a value inside a range: the drawing code is full of it, and it belongs
   with the drawing rather than with each screen that draws. */
int ncc_clampi(int v, int lo, int hi);

int ncc_col_width(int font);
int ncc_text_width(const char *text, int font);

/* Text, and text cut to a column count - the panel draws one character per
   column whatever the string says, so a row of a program can never run into the
   pane beside it. */
void ncc_text(int x, int y, const char *text, lvds_color_t fg, lvds_color_t bg,
              int font);
void ncc_text_clip(int x, int y, const char *text, int cols, lvds_color_t fg,
                   lvds_color_t bg, int font);

void ncc_fill(int x, int y, int w, int h, lvds_color_t color);
void ncc_frame(int x, int y, int w, int h, lvds_color_t color);
/* The 3x3: nine slots, `labels` in key order 1..9 (`""` for a slot that is not
   there), each cell showing its number in the corner and the label under it.
   `hot` is the key whose cell is drawn as the one in play, 0 for none. */
void ncc_draw_pad(int x, int y, int w, int h, const char *const *labels,
                  char hot);

/* Which key a cell stands for. The pad mirrors the machine's keypad, the way
   nc's pad did: the numeric block's own rows run the other way up, so `7 8 9`
   is the top row and `1 2 3` the bottom one (bench: "3x3 is swapped again. i
   have 1 on bottom left corner"). Both the drawing and the checks ask this, so
   the order is stated once. */
char ncc_pad_cell_key(int row, int col);

#endif
