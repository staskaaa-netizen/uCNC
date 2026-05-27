#ifndef LVDS_HW_RENDERER_H
#define LVDS_HW_RENDERER_H

#include "lvds_hstx.h"

#ifdef __cplusplus
extern "C" {
#endif

void lvds_hw_text(int x, int y, const char *text,
                  lvds_color_t fg, lvds_color_t bg, int font);
int lvds_hw_text_width(const char *text, int font);
void lvds_hw_line(int x1, int y1, int x2, int y2, lvds_color_t color);
void lvds_hw_line_w(int x1, int y1, int x2, int y2,
                    lvds_color_t color, int width);
void lvds_hw_rect(int x, int y, int w, int h, lvds_color_t color);
void lvds_hw_fill_rect(int x, int y, int w, int h, lvds_color_t color);
void lvds_hw_ellipse(int x, int y, int rx, int ry, lvds_color_t color);
void lvds_hw_fill_ellipse(int x, int y, int rx, int ry, lvds_color_t color);

#ifdef __cplusplus
}
#endif

#endif
