#include "leancam_display.h"

#include <stddef.h>
#include <string.h>

#include "pico/stdlib.h"

#include "disphstx.h"
#include "leancam_palette.h"

#if LEANCAM_USE_PSRAM_FB || LEANCAM_USE_PSRAM_BACKBUFFER
#include "leancam_psram.h"
#endif

#ifndef LEANCAM_USE_PSRAM_BACKBUFFER
#define LEANCAM_USE_PSRAM_BACKBUFFER 0
#endif

#define LC_DISPLAY_PITCH ((LC_DISPLAY_WIDTH + 4) / 5 * 4)
#define LC_PALETTE_SIZE 64

static uint8_t *g_scanout_buf;
static sDrawCan g_back_can;
static bool g_backbuffer_active;
static int g_last_error;
static int g_dirty_x1;
static int g_dirty_y1;
static int g_dirty_x2;
static int g_dirty_y2;
static uint16_t g_palette_rgb565[LC_PALETTE_SIZE];
static bool g_palette_used[LC_PALETTE_SIZE];
static uint8_t g_palette_next;

static uint16_t rgb_to_565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)((((uint16_t)r & 0xf8) << 8) |
                      (((uint16_t)g & 0xfc) << 3) |
                      (((uint16_t)b & 0xf8) >> 3));
}

static void palette_set(uint8_t index, uint16_t rgb565)
{
    if (index >= LC_PALETTE_SIZE) {
        return;
    }
    g_palette_rgb565[index] = rgb565;
    g_palette_used[index] = true;
    DispHstxPal6b[index] = rgb565;
}

static void palette_reset(void)
{
    for (uint8_t i = 0; i < LC_PALETTE_SIZE; i++) {
        uint8_t r = (uint8_t)(((i >> 4) & 0x03) * 85);
        uint8_t g = (uint8_t)(((i >> 2) & 0x03) * 85);
        uint8_t b = (uint8_t)((i & 0x03) * 85);
        g_palette_rgb565[i] = rgb_to_565(r, g, b);
        g_palette_used[i] = false;
        DispHstxPal6b[i] = g_palette_rgb565[i];
    }

    palette_set(0, rgb_to_565(0, 0, 0));
    g_palette_next = 1;
    lc_palette_reset();
}

static lc_color_t palette_lookup(uint16_t rgb565)
{
    uint32_t best_dist = 0xffffffffu;
    uint8_t best = 0;

    for (uint8_t i = 0; i < LC_PALETTE_SIZE; i++) {
        if (g_palette_used[i] && g_palette_rgb565[i] == rgb565) {
            return i;
        }
    }

    if (g_palette_next < LC_PALETTE_SIZE) {
        uint8_t index = g_palette_next++;
        palette_set(index, rgb565);
        return index;
    }

    for (uint8_t i = 0; i < LC_PALETTE_SIZE; i++) {
        int dr = (int)((g_palette_rgb565[i] >> 11) & 0x1f) - (int)((rgb565 >> 11) & 0x1f);
        int dg = (int)((g_palette_rgb565[i] >> 5) & 0x3f) - (int)((rgb565 >> 5) & 0x3f);
        int db = (int)(g_palette_rgb565[i] & 0x1f) - (int)(rgb565 & 0x1f);
        uint32_t dist = (uint32_t)(dr * dr * 2 + dg * dg + db * db * 2);
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }

    return best;
}

static int font_scale(int font)
{
    return font == LC_FONT_LARGE ? 2 : 1;
}

static int font_height(int font)
{
    return font == LC_FONT_LARGE ? 16 : 14;
}

static void select_font(int font)
{
    if (font == LC_FONT_SMALL) {
        Draw6SelFont6x8();
    } else {
        Draw6SelFont8x14Ibm();
    }
}

static void dirty_reset(void)
{
    g_dirty_x1 = LC_DISPLAY_WIDTH;
    g_dirty_y1 = LC_DISPLAY_HEIGHT;
    g_dirty_x2 = 0;
    g_dirty_y2 = 0;
}

static void dirty_mark(int x, int y, int w, int h)
{
    int x2 = x + w;
    int y2 = y + h;

    if (w <= 0 || h <= 0) {
        return;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > LC_DISPLAY_WIDTH) x2 = LC_DISPLAY_WIDTH;
    if (y2 > LC_DISPLAY_HEIGHT) y2 = LC_DISPLAY_HEIGHT;
    if (x >= x2 || y >= y2) {
        return;
    }

    if (x < g_dirty_x1) g_dirty_x1 = x;
    if (y < g_dirty_y1) g_dirty_y1 = y;
    if (x2 > g_dirty_x2) g_dirty_x2 = x2;
    if (y2 > g_dirty_y2) g_dirty_y2 = y2;
}

static void dirty_mark_line(int x1, int y1, int x2, int y2, int thick)
{
    int min_x = x1 < x2 ? x1 : x2;
    int max_x = x1 > x2 ? x1 : x2;
    int min_y = y1 < y2 ? y1 : y2;
    int max_y = y1 > y2 ? y1 : y2;
    int pad = thick < 1 ? 1 : thick;
    dirty_mark(min_x - pad, min_y - pad, (max_x - min_x) + pad * 2 + 1,
               (max_y - min_y) + pad * 2 + 1);
}

bool lc_display_init(void)
{
    void *fb = NULL;
    palette_reset();
#if LEANCAM_USE_PSRAM_FB
    if (lc_psram_available()) {
        fb = lc_psram_ptr(0);
    }
#endif
    int res = DispVMode800x600x6(DISPHSTX_DISPMODE_DVI, fb);
    g_last_error = res;
    if (res != DISPHSTX_ERR_OK) {
        return false;
    }
    g_scanout_buf = DispHstxBuf();
#if LEANCAM_USE_PSRAM_BACKBUFFER
    if (!fb && g_scanout_buf && lc_psram_available()) {
        g_back_can = DrawCan6;
        g_back_can.buf = (uint8_t *)lc_psram_ptr(0);
        SetDrawCan6(&g_back_can);
        g_backbuffer_active = true;
    }
#endif
    dirty_reset();
    Draw6ClearCol(COL6_BLACK);
    dirty_mark(0, 0, LC_DISPLAY_WIDTH, LC_DISPLAY_HEIGHT);
    lc_display_present();
    return true;
}

int lc_display_last_error(void)
{
    return g_last_error;
}

void *lc_display_scanout_buffer(void)
{
    return g_scanout_buf;
}

bool lc_display_backbuffer_active(void)
{
    return g_backbuffer_active;
}

void lc_display_clear(lc_color_t color)
{
    Draw6ClearCol(color);
    dirty_mark(0, 0, LC_DISPLAY_WIDTH, LC_DISPLAY_HEIGHT);
}

void lc_display_pixel(int x, int y, lc_color_t color)
{
    Draw6Point(x, y, color);
    dirty_mark(x, y, 1, 1);
}

void lc_display_line(int x1, int y1, int x2, int y2, lc_color_t color)
{
    Draw6Line(x1, y1, x2, y2, color);
    dirty_mark_line(x1, y1, x2, y2, 1);
}

void lc_display_line_w(int x1, int y1, int x2, int y2, lc_color_t color, int thick)
{
    Draw6LineW(x1, y1, x2, y2, color, thick, False);
    dirty_mark_line(x1, y1, x2, y2, thick);
}

void lc_display_rect(int x, int y, int w, int h, lc_color_t color)
{
    Draw6FrameW(x, y, w, h, color, 1);
    dirty_mark(x, y, w, h);
}

void lc_display_fill_rect(int x, int y, int w, int h, lc_color_t color)
{
    Draw6Rect(x, y, w, h, color);
    dirty_mark(x, y, w, h);
}

void lc_display_ellipse(int x, int y, int rx, int ry, lc_color_t color)
{
    Draw6Ellipse(x - rx, y - ry, rx * 2, ry * 2, color, 0x0f);
    dirty_mark(x - rx - 1, y - ry - 1, rx * 2 + 2, ry * 2 + 2);
}

void lc_display_fill_ellipse(int x, int y, int rx, int ry, lc_color_t color)
{
    Draw6FillEllipse(x - rx, y - ry, rx * 2, ry * 2, color, 0x0f);
    dirty_mark(x - rx - 1, y - ry - 1, rx * 2 + 2, ry * 2 + 2);
}

void lc_display_text(int x, int y, const char *text, lc_color_t fg, lc_color_t bg, int font)
{
    int scale = font_scale(font);
    int h = font_height(font) * scale;
    int w;

    if (!text) {
        return;
    }

    select_font(font);
    w = lc_display_text_width(text, font);
    Draw6Rect(x, y, w, h, bg);
    Draw6Text(text, -1, x, y, fg, scale, scale);
    dirty_mark(x, y, w, h);
}

int lc_display_text_width(const char *text, int font)
{
    int char_w;
    if (!text) {
        return 0;
    }
    char_w = (font == LC_FONT_SMALL) ? 6 : 8;
    return (int)strlen(text) * char_w * font_scale(font);
}

void lc_display_present(void)
{
    uint8_t *src;
    int byte_x1;
    int byte_x2;
    int bytes;

    if (!g_backbuffer_active || !g_scanout_buf || g_dirty_x1 >= g_dirty_x2 || g_dirty_y1 >= g_dirty_y2) {
        dirty_reset();
        return;
    }

    src = g_back_can.buf;
    byte_x1 = (g_dirty_x1 / 5) * 4;
    byte_x2 = ((g_dirty_x2 + 4) / 5) * 4;
    if (byte_x2 > LC_DISPLAY_PITCH) {
        byte_x2 = LC_DISPLAY_PITCH;
    }
    bytes = byte_x2 - byte_x1;
    for (int y = g_dirty_y1; y < g_dirty_y2; y++) {
        memcpy(g_scanout_buf + y * LC_DISPLAY_PITCH + byte_x1,
               src + y * LC_DISPLAY_PITCH + byte_x1,
               (size_t)bytes);
    }
    dirty_reset();
}

lc_color_t lc_display_rgb565(uint16_t rgb565)
{
    return palette_lookup(rgb565);
}

lc_color_t lc_display_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return palette_lookup(rgb_to_565(r, g, b));
}
