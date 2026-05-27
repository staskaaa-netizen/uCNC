#include "lvds_draw_api.h"

#include <stdio.h>
#include <string.h>

void lvds_draw_text(int x, int y, const char *text,
                    lvds_color_t fg, lvds_color_t bg, int font)
{
    lvds_hw_text(x, y, text, fg, bg, font);
}

int lvds_draw_text_width(const char *text, int font)
{
    return lvds_hw_text_width(text, font);
}

void lvds_draw_text_clip(int x, int y, const char *text, int cols,
                         lvds_color_t fg, lvds_color_t bg, int font)
{
    char buf[160];
    int len;

    if (!text) {
        text = "";
    }
    if (cols < 1) {
        return;
    }
    if (cols >= (int)sizeof(buf)) {
        cols = (int)sizeof(buf) - 1;
    }
    len = (int)strlen(text);
    if (len > cols) {
        len = cols;
    }
    snprintf(buf, sizeof(buf), "%-*.*s", cols, len, text);
    if ((int)strlen(text) > cols && cols > 3) {
        buf[cols - 3] = '.';
        buf[cols - 2] = '.';
        buf[cols - 1] = '.';
        buf[cols] = '\0';
    }
    lvds_hw_text(x, y, buf, fg, bg, font);
}

void lvds_draw_line(int x1, int y1, int x2, int y2, lvds_color_t color)
{
    lvds_hw_line(x1, y1, x2, y2, color);
}

void lvds_draw_line_w(int x1, int y1, int x2, int y2,
                      lvds_color_t color, int width)
{
    lvds_hw_line_w(x1, y1, x2, y2, color, width);
}

void lvds_draw_rect(int x, int y, int w, int h, lvds_color_t color)
{
    lvds_hw_rect(x, y, w, h, color);
}

void lvds_draw_fill_rect(int x, int y, int w, int h, lvds_color_t color)
{
    lvds_hw_fill_rect(x, y, w, h, color);
}

void lvds_draw_ellipse(int x, int y, int rx, int ry, lvds_color_t color)
{
    lvds_hw_ellipse(x, y, rx, ry, color);
}

void lvds_draw_fill_ellipse(int x, int y, int rx, int ry, lvds_color_t color)
{
    lvds_hw_fill_ellipse(x, y, rx, ry, color);
}
