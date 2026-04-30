/*
 * ra8876_ll.h
 *
 * Low-level RA8876 backend for µCNC / RA renderer use.
 */

#ifndef RA8876_LL_H
#define RA8876_LL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#ifndef RA_RESET_PIN
#define RA_RESET_PIN   DOUT4
#endif

#ifndef RA_CS_PIN
#define RA_CS_PIN      SPI2_CS
#endif

#define RA8876_TFT_W     1024
#define RA8876_TFT_H      600

#define RA_SCREEN_W       RA8876_TFT_W
#define RA_SCREEN_H       RA8876_TFT_H
#define RA_BPP_BYTES      2UL

#define RA_PAGE_BYTES     ((uint32_t)(RA_SCREEN_W * RA_SCREEN_H * RA_BPP_BYTES))
#define RA_PAGE_BASE(n)   ((uint32_t)((n) * RA_PAGE_BYTES))

#define RA_PAGE_VISIBLE   0
#define RA_PAGE_BACKBUF   1

#define RA8876_LCD_VBPD         20
#define RA8876_LCD_VFPD         12
#define RA8876_LCD_VSPW          3
#define RA8876_LCD_HBPD        144
#define RA8876_LCD_HFPD        160
#define RA8876_LCD_HSPW         20

#define RA8876_LCD_PCLK_FALLING_RISING   0
#define RA8876_LCD_HSYNC_ACTIVE_POL      0
#define RA8876_LCD_VSYNC_ACTIVE_POL      0
#define RA8876_LCD_DE_ACTIVE_POL         1

#define RA8876_VISIBLE_ADDR 0x00000000UL
#define RA8876_BACKBUF_ADDR RA_PAGE_BASE(RA_PAGE_BACKBUF)

/* RGB565 */
#define RA_BLACK   0x0000
#define RA_WHITE   0xFFFF
#define RA_RED     0xF800
#define RA_GREEN   0x07E0
#define RA_BLUE    0x001F
#define RA_YELLOW  0xFFE0
#define RA_CYAN    0x07FF
#define RA_GRAY    0x8410
#define RA_DGRAY   0x4208
#define RA_AMBER     0xFD20   // warm amber (~#FFBF00)
#define RA_LGRAY     0xC618   // light grey (~#C0C0C0)
#define RA_AMBER_SOFT 0xFCA0   // slightly dimmer amber

typedef enum {
    RA_MODE_GRAPHIC = 0,
    RA_MODE_TEXT    = 1
} ra_mode_t;

typedef enum {
    RA_FONT_SMALL  = 0, /* internal font size 0 */
    RA_FONT_MEDIUM = 1, /* internal font size 1 */
    RA_FONT_LARGE  = 2  /* internal font size 2 */
} ra_font_size_t;

extern ra_mode_t g_mode;
extern ra_font_size_t g_font_size;

void ra_init(void);
bool ra_is_inited(void);

void ra_clear(uint16_t color);
void ra_set_draw_base(uint32_t base);
uint32_t ra_get_draw_base(void);
void ra_set_draw_page(uint8_t page);
void ra_blit(uint32_t src_base,
             uint16_t src_x,
             uint16_t src_y,
             uint32_t dst_base,
             uint16_t dst_x,
             uint16_t dst_y,
             uint16_t width,
             uint16_t height);
void ra_fill_rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void ra_draw_rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void ra_draw_line(int x1, int y1, int x2, int y2, uint16_t color);

void ra_text(uint16_t x, uint16_t y, uint16_t fg, uint16_t bg, const char *s);

void ra_text_mode(void);
void ra_graphic_mode(void);

void ra_set_font_size(ra_font_size_t size);
ra_font_size_t ra_get_font_size(void);

#ifdef __cplusplus
}
#endif

#endif /* RA8876_LL_H */
