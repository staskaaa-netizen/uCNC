#include "lvds_hw_renderer.h"

void lvds_hw_text(int x, int y, const char *text,
                  lvds_color_t fg, lvds_color_t bg, int font)
{
    lvds_hstx_text(x, y, text ? text : "", fg, bg, font);
}

int lvds_hw_text_width(const char *text, int font)
{
    return lvds_hstx_text_width(text ? text : "", font);
}

void lvds_hw_line(int x1, int y1, int x2, int y2, lvds_color_t color)
{
    lvds_hstx_line(x1, y1, x2, y2, color);
}

void lvds_hw_line_w(int x1, int y1, int x2, int y2,
                    lvds_color_t color, int width)
{
    lvds_hstx_line_w(x1, y1, x2, y2, color, width);
}

void lvds_hw_rect(int x, int y, int w, int h, lvds_color_t color)
{
    lvds_hstx_rect(x, y, w, h, color);
}

void lvds_hw_fill_rect(int x, int y, int w, int h, lvds_color_t color)
{
    lvds_hstx_fill_rect(x, y, w, h, color);
}

void lvds_hw_ellipse(int x, int y, int rx, int ry, lvds_color_t color)
{
    lvds_hstx_ellipse(x, y, rx, ry, color);
}

void lvds_hw_fill_ellipse(int x, int y, int rx, int ry, lvds_color_t color)
{
    lvds_hstx_fill_ellipse(x, y, rx, ry, color);
}
