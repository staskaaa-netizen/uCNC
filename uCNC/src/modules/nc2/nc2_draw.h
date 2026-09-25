#ifndef NC2_DRAW_H
#define NC2_DRAW_H

#include "../lvds_renderer/lvds_draw_api.h"

#include <stdbool.h>

/* nc2's drawing vocabulary: the colours it uses, text with the panel's own
   clipping, and the 3x3 pad. Everything here is data in and pixels out - the
   screens and the pad draw with it and none of it owns state. */

void nc2_draw_init(void);

lvds_color_t nc2_col_bg(void);
lvds_color_t nc2_col_panel(void);
lvds_color_t nc2_col_header(void);
lvds_color_t nc2_col_text(void);
lvds_color_t nc2_col_dim(void);
lvds_color_t nc2_col_accent(void);
lvds_color_t nc2_col_select(void);
lvds_color_t nc2_col_pad_key(void);
lvds_color_t nc2_col_pad_hot(void);
lvds_color_t nc2_col_field_bg(void);
lvds_color_t nc2_col_field_fg(void);
lvds_color_t nc2_col_prev_bg(void);
lvds_color_t nc2_col_prev_frame(void);
lvds_color_t nc2_col_prev_stock(void);
lvds_color_t nc2_col_prev_hatch(void);
lvds_color_t nc2_col_prev_cut(void);
lvds_color_t nc2_col_prev_profile(void);

/* Keep a value inside a range: the drawing code is full of it, and it belongs
   with the drawing rather than with each screen that draws. */
int nc2_clampi(int v, int lo, int hi);

int nc2_col_width(int font);
int nc2_text_width(const char *text, int font);

/* Text, and text cut to a column count - the panel draws one character per
   column whatever the string says, so a row of a program can never run into the
   pane beside it. */
void nc2_text(int x, int y, const char *text, lvds_color_t fg, lvds_color_t bg,
              int font);
void nc2_text_clip(int x, int y, const char *text, int cols, lvds_color_t fg,
                   lvds_color_t bg, int font);

void nc2_fill(int x, int y, int w, int h, lvds_color_t color);
void nc2_frame(int x, int y, int w, int h, lvds_color_t color);
void nc2_hline(int x, int y, int w, lvds_color_t color);

/* The 3x3: nine slots, `labels` in key order 1..9 (`""` for a slot that is not
   there), each cell showing its number in the corner and the label under it.
   `hot` is the key whose cell is drawn as the one in play, 0 for none. */
void nc2_draw_pad(int x, int y, int w, int h, const char *const *labels,
                  char hot);

#endif
