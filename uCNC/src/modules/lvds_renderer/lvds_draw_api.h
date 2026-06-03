#ifndef LVDS_DRAW_API_H
#define LVDS_DRAW_API_H

#include "lvds_hstx.h"

#ifdef __cplusplus
extern "C" {
#endif

void lvds_draw_text(int x, int y, const char *text,
                    lvds_color_t fg, lvds_color_t bg, int font);
int lvds_draw_text_width(const char *text, int font);
void lvds_draw_text_clip(int x, int y, const char *text, int cols,
                         lvds_color_t fg, lvds_color_t bg, int font);
void lvds_draw_line(int x1, int y1, int x2, int y2, lvds_color_t color);
void lvds_draw_line_w(int x1, int y1, int x2, int y2,
                      lvds_color_t color, int width);
void lvds_draw_rect(int x, int y, int w, int h, lvds_color_t color);
void lvds_draw_fill_rect(int x, int y, int w, int h, lvds_color_t color);
void lvds_draw_ellipse(int x, int y, int rx, int ry, lvds_color_t color);
void lvds_draw_fill_ellipse(int x, int y, int rx, int ry, lvds_color_t color);

#ifdef __cplusplus
}
#endif

#endif
