#ifndef LVDS_HSTX_H
#define LVDS_HSTX_H

#include <stdint.h>
#include <stdbool.h>



#ifdef __cplusplus
extern "C" {
#endif

#define LVDS_HSTX_WIDTH LVDS_WIDTH
#define LVDS_HSTX_HEIGHT LVDS_HEIGHT

#define LVDS_HSTX_D0P LVDS_D0P
#define LVDS_HSTX_D0M LVDS_D0M
#define LVDS_HSTX_D1P LVDS_D1P
#define LVDS_HSTX_D1M LVDS_D1M
#define LVDS_HSTX_D2P LVDS_D2P
#define LVDS_HSTX_D2M LVDS_D2M
#define LVDS_HSTX_CLKP LVDS_CLKP
#define LVDS_HSTX_CLKM LVDS_CLKM

typedef uint16_t lvds_color_t;

enum {
    LVDS_FONT_SMALL = 0,
    LVDS_FONT_NORMAL = 1,
    LVDS_FONT_LARGE = 2
};

bool lvds_hstx_init(void);
int lvds_hstx_last_error(void);
void *lvds_hstx_scanout_buffer(void);
bool lvds_hstx_backbuffer_active(void);
void lvds_hstx_direct_scanout(bool direct);
void lvds_hstx_clear(lvds_color_t color);
void lvds_hstx_pixel(int x, int y, lvds_color_t color);
void lvds_hstx_line(int x1, int y1, int x2, int y2, lvds_color_t color);
void lvds_hstx_line_w(int x1, int y1, int x2, int y2, lvds_color_t color, int thick);
void lvds_hstx_rect(int x, int y, int w, int h, lvds_color_t color);
void lvds_hstx_fill_rect(int x, int y, int w, int h, lvds_color_t color);
void lvds_hstx_ellipse(int x, int y, int rx, int ry, lvds_color_t color);
void lvds_hstx_fill_ellipse(int x, int y, int rx, int ry, lvds_color_t color);
void lvds_hstx_text(int x, int y, const char *text, lvds_color_t fg, lvds_color_t bg, int font);
int lvds_hstx_text_width(const char *text, int font);
void lvds_hstx_present(void);

lvds_color_t lvds_hstx_rgb565(uint16_t rgb565);
lvds_color_t lvds_hstx_rgb(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif

#endif /* LVDS_HSTX_H */
