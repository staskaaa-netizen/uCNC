#ifndef LVDS_HSTX_H
#define LVDS_HSTX_H

#include <stdint.h>
#include <stdbool.h>



#ifdef __cplusplus
extern "C" {
#endif

#define LVDS_HSTX_WIDTH LVDS_WIDTH
#define LVDS_HSTX_HEIGHT LVDS_HEIGHT

/* The glass, and how it is mounted.

   The panel is 800x600, and the operator reads it **turned**: the bench asked
   for the screen rotated a quarter. The turn is the renderer's business - it
   owns the pixels - so every screen above keeps drawing a
   LVDS_VIEW_WIDTH x LVDS_VIEW_HEIGHT picture, and the primitives that touch a
   pixel (or a filled rectangle) put it on the glass the right way up:

     - `LVDS_PANEL_TURN` is how far the **panel** is turned clockwise when it is
       mounted (90 = its top edge ends up on the operator's right, so the
       picture has to be turned back a quarter anticlockwise).
     - `LVDS_VIEW_*` is what the screens draw in: 600 wide and 800 tall while
       the panel is turned, the panel's own size when it is not.
     - `LVDS_PANEL_X/Y` map one view point, and `LVDS_PANEL_RECT_*` one view
       rectangle (a quarter turn swaps its width and height).

   Text turns with it for free: a glyph is a few filled rectangles and the text
   code never touches a pixel itself. A build whose panel is not turned sets
   `-DLVDS_PANEL_TURN=0`; there are two ways round and this is the machine's
   own answer, so it is one number here. */
#ifndef LVDS_PANEL_TURN
#define LVDS_PANEL_TURN 90
#endif

#if LVDS_PANEL_TURN == 90
#define LVDS_VIEW_WIDTH LVDS_HSTX_HEIGHT
#define LVDS_VIEW_HEIGHT LVDS_HSTX_WIDTH
#define LVDS_PANEL_X(vx, vy) (vy)
#define LVDS_PANEL_Y(vx, vy) (LVDS_HSTX_HEIGHT - 1 - (vx))
#define LVDS_PANEL_RECT_X(vx, vy, vw, vh) (vy)
#define LVDS_PANEL_RECT_Y(vx, vy, vw, vh) (LVDS_HSTX_HEIGHT - ((vx) + (vw)))
#define LVDS_PANEL_RECT_W(vw, vh) (vh)
#define LVDS_PANEL_RECT_H(vw, vh) (vw)
#elif LVDS_PANEL_TURN == 270
#define LVDS_VIEW_WIDTH LVDS_HSTX_HEIGHT
#define LVDS_VIEW_HEIGHT LVDS_HSTX_WIDTH
#define LVDS_PANEL_X(vx, vy) (LVDS_HSTX_WIDTH - 1 - (vy))
#define LVDS_PANEL_Y(vx, vy) (vx)
#define LVDS_PANEL_RECT_X(vx, vy, vw, vh) (LVDS_HSTX_WIDTH - ((vy) + (vh)))
#define LVDS_PANEL_RECT_Y(vx, vy, vw, vh) (vx)
#define LVDS_PANEL_RECT_W(vw, vh) (vh)
#define LVDS_PANEL_RECT_H(vw, vh) (vw)
#else
#define LVDS_VIEW_WIDTH LVDS_HSTX_WIDTH
#define LVDS_VIEW_HEIGHT LVDS_HSTX_HEIGHT
#define LVDS_PANEL_X(vx, vy) (vx)
#define LVDS_PANEL_Y(vx, vy) (vy)
#define LVDS_PANEL_RECT_X(vx, vy, vw, vh) (vx)
#define LVDS_PANEL_RECT_Y(vx, vy, vw, vh) (vy)
#define LVDS_PANEL_RECT_W(vw, vh) (vw)
#define LVDS_PANEL_RECT_H(vw, vh) (vh)
#endif

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
