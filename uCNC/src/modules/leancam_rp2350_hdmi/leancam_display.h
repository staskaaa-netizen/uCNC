#ifndef LEANCAM_DISPLAY_H
#define LEANCAM_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LC_DISPLAY_WIDTH 800
#define LC_DISPLAY_HEIGHT 600

typedef uint16_t lc_color_t;

enum {
    LC_FONT_SMALL = 0,
    LC_FONT_NORMAL = 1,
    LC_FONT_LARGE = 2
};

bool lc_display_init(void);
int lc_display_last_error(void);
void *lc_display_scanout_buffer(void);
bool lc_display_backbuffer_active(void);
void lc_display_direct_scanout(bool direct);
void lc_display_clear(lc_color_t color);
void lc_display_pixel(int x, int y, lc_color_t color);
void lc_display_line(int x1, int y1, int x2, int y2, lc_color_t color);
void lc_display_line_w(int x1, int y1, int x2, int y2, lc_color_t color, int thick);
void lc_display_rect(int x, int y, int w, int h, lc_color_t color);
void lc_display_fill_rect(int x, int y, int w, int h, lc_color_t color);
void lc_display_ellipse(int x, int y, int rx, int ry, lc_color_t color);
void lc_display_fill_ellipse(int x, int y, int rx, int ry, lc_color_t color);
void lc_display_text(int x, int y, const char *text, lc_color_t fg, lc_color_t bg, int font);
int lc_display_text_width(const char *text, int font);
void lc_display_present(void);

lc_color_t lc_display_rgb565(uint16_t rgb565);
lc_color_t lc_display_rgb(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif

#endif
