/* GDI/bitmap implementation of the LVDS draw API. See lvds_host.h. */

#include "lvds_host.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lvds_draw_api.h"
#include "lvds_fonts.h"

#ifndef LVDS_HOST_WIDTH
#define LVDS_HOST_WIDTH 800
#endif
#ifndef LVDS_HOST_HEIGHT
#define LVDS_HOST_HEIGHT 600
#endif

static uint32_t g_frame[LVDS_HOST_WIDTH * LVDS_HOST_HEIGHT];
static void *g_window;

int lvds_host_width(void) { return LVDS_HOST_WIDTH; }
int lvds_host_height(void) { return LVDS_HOST_HEIGHT; }
int lvds_host_stride(void) { return LVDS_HOST_WIDTH * 4; }
const void *lvds_host_pixels(void) { return g_frame; }

void lvds_host_attach_window(void *hwnd)
{
    g_window = hwnd;
}

/* RGB565 -> 0x00RRGGBB (the firmware's indexed palette expands to the same
   values the panel shows). */
static uint32_t host_rgb888(lvds_color_t color)
{
    uint32_t r = (uint32_t)((color >> 11) & 0x1Fu);
    uint32_t g = (uint32_t)((color >> 5) & 0x3Fu);
    uint32_t b = (uint32_t)(color & 0x1Fu);

    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}

static void host_pixel(int x, int y, uint32_t rgb)
{
    if (x < 0 || y < 0 || x >= LVDS_HOST_WIDTH || y >= LVDS_HOST_HEIGHT)
        return;
    g_frame[(size_t)y * LVDS_HOST_WIDTH + (size_t)x] = rgb;
}

void lvds_hstx_pixel(int x, int y, lvds_color_t color)
{
    host_pixel(x, y, host_rgb888(color));
}

void lvds_hstx_clear(lvds_color_t color)
{
    uint32_t rgb = host_rgb888(color);
    size_t i;

    for (i = 0; i < (size_t)LVDS_HOST_WIDTH * LVDS_HOST_HEIGHT; i++)
        g_frame[i] = rgb;
}

void lvds_hstx_line(int x1, int y1, int x2, int y2, lvds_color_t color)
{
    int dx = abs(x2 - x1);
    int dy = -abs(y2 - y1);
    int sx = x1 < x2 ? 1 : -1;
    int sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    uint32_t rgb = host_rgb888(color);

    for (;;) {
        host_pixel(x1, y1, rgb);
        if (x1 == x2 && y1 == y2)
            break;
        {
            int e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x1 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y1 += sy;
            }
        }
    }
}

void lvds_hstx_line_w(int x1, int y1, int x2, int y2, lvds_color_t color, int thick)
{
    int i;

    if (thick <= 1) {
        lvds_hstx_line(x1, y1, x2, y2, color);
        return;
    }
    for (i = 0; i < thick; i++)
        lvds_hstx_line(x1 + i, y1, x2 + i, y2, color);
}

void lvds_hstx_rect(int x, int y, int w, int h, lvds_color_t color)
{
    if (w <= 0 || h <= 0)
        return;
    lvds_hstx_line(x, y, x + w - 1, y, color);
    lvds_hstx_line(x, y + h - 1, x + w - 1, y + h - 1, color);
    lvds_hstx_line(x, y, x, y + h - 1, color);
    lvds_hstx_line(x + w - 1, y, x + w - 1, y + h - 1, color);
}

void lvds_hstx_fill_rect(int x, int y, int w, int h, lvds_color_t color)
{
    uint32_t rgb = host_rgb888(color);
    int yy;

    if (w <= 0 || h <= 0)
        return;
    for (yy = y; yy < y + h; yy++) {
        int xx;
        if (yy < 0 || yy >= LVDS_HOST_HEIGHT)
            continue;
        for (xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= LVDS_HOST_WIDTH)
                continue;
            g_frame[(size_t)yy * LVDS_HOST_WIDTH + (size_t)xx] = rgb;
        }
    }
}

void lvds_hstx_ellipse(int x, int y, int rx, int ry, lvds_color_t color)
{
    int a;

    if (rx <= 0 || ry <= 0)
        return;
    for (a = 0; a < 360; a++) {
        double rad = (double)a * 3.14159265358979 / 180.0;
        host_pixel(x + (int)((double)rx * cos(rad)),
                   y + (int)((double)ry * sin(rad)),
                   host_rgb888(color));
    }
}

void lvds_hstx_fill_ellipse(int x, int y, int rx, int ry, lvds_color_t color)
{
    int yy;

    if (rx <= 0 || ry <= 0)
        return;
    for (yy = -ry; yy <= ry; yy++) {
        double t = 1.0 - ((double)(yy * yy) / (double)(ry * ry));
        int span;
        if (t < 0.0)
            continue;
        span = (int)((double)rx * sqrt(t));
        lvds_hstx_line(x - span, y + yy, x + span, y + yy, color);
    }
}

/* Bitmap fonts are the firmware tables, so glyphs and spacing match exactly. */
static int host_font_scale(int font)
{
    return font == LVDS_FONT_LARGE ? 2 : 1;
}

static int host_font_char_w(int font)
{
    return font == LVDS_FONT_SMALL ? 6 : 8;
}

static int host_font_char_h(int font)
{
    return font == LVDS_FONT_SMALL ? 8 : 14;
}

static const uint8_t *host_font_bitmap(int font)
{
    return font == LVDS_FONT_SMALL ? FontCond6x8 : FontIbm8x14;
}

static void host_draw_char(int x, int y, unsigned char ch, lvds_color_t fg,
                           lvds_color_t bg, int font)
{
    int scale = host_font_scale(font);
    int cw = host_font_char_w(font);
    int chh = host_font_char_h(font);
    const uint8_t *bits = host_font_bitmap(font);
    int row;

    lvds_hstx_fill_rect(x, y, cw * scale, chh * scale, bg);
    for (row = 0; row < chh; row++) {
        uint8_t line = bits[row * 256 + ch];
        int col;
        for (col = 0; col < cw; col++) {
            if (line & 0x80u)
                lvds_hstx_fill_rect(x + col * scale, y + row * scale,
                                    scale, scale, fg);
            line = (uint8_t)(line << 1);
        }
    }
}

void lvds_hstx_text(int x, int y, const char *text, lvds_color_t fg,
                    lvds_color_t bg, int font)
{
    int step = host_font_char_w(font) * host_font_scale(font);
    int guard = 0;

    if (!text)
        return;
    while (*text && guard++ < 4096) {
        host_draw_char(x, y, (unsigned char)*text++, fg, bg, font);
        x += step;
    }
}

int lvds_hstx_text_width(const char *text, int font)
{
    if (!text)
        return 0;
    return (int)strlen(text) * host_font_char_w(font) * host_font_scale(font);
}

lvds_color_t lvds_hstx_rgb565(uint16_t rgb565)
{
    return (lvds_color_t)rgb565;
}

lvds_color_t lvds_hstx_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (lvds_color_t)(((uint16_t)(r >> 3) << 11) |
                          ((uint16_t)(g >> 2) << 5) |
                          (uint16_t)(b >> 3));
}

void lvds_hstx_present(void)
{
    if (g_window)
        InvalidateRect((HWND)g_window, NULL, FALSE);
}

/* No PSRAM on the desktop: callers fall back to the SRAM path. */
bool lvds_psram_init(void) { return false; }
bool lvds_psram_available(void) { return false; }
uint32_t lvds_psram_clock_hz(void) { return 0u; }
void *lvds_psram_ptr(size_t offset) { (void)offset; return NULL; }

bool lvds_host_save_bmp(const char *path)
{
    FILE *fp;
    uint8_t header[54];
    int row;
    int stride = LVDS_HOST_WIDTH * 3;
    int pad = (4 - (stride % 4)) % 4;
    uint32_t size = (uint32_t)(54 + (stride + pad) * LVDS_HOST_HEIGHT);
    uint8_t zeros[3] = { 0, 0, 0 };

    if (!path)
        return false;
    fp = fopen(path, "wb");
    if (!fp)
        return false;
    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    memcpy(header + 2, &size, 4);
    {
        uint32_t offset = 54;
        uint32_t info = 40;
        uint32_t w = LVDS_HOST_WIDTH;
        uint32_t h = LVDS_HOST_HEIGHT;
        uint16_t planes = 1;
        uint16_t bpp = 24;
        memcpy(header + 10, &offset, 4);
        memcpy(header + 14, &info, 4);
        memcpy(header + 18, &w, 4);
        memcpy(header + 22, &h, 4);
        memcpy(header + 26, &planes, 2);
        memcpy(header + 28, &bpp, 2);
    }
    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header)) {
        fclose(fp);
        return false;
    }
    for (row = LVDS_HOST_HEIGHT - 1; row >= 0; row--) {
        int col;
        for (col = 0; col < LVDS_HOST_WIDTH; col++) {
            uint32_t rgb = g_frame[(size_t)row * LVDS_HOST_WIDTH + (size_t)col];
            uint8_t bgr[3];
            bgr[0] = (uint8_t)(rgb & 0xFFu);
            bgr[1] = (uint8_t)((rgb >> 8) & 0xFFu);
            bgr[2] = (uint8_t)((rgb >> 16) & 0xFFu);
            if (fwrite(bgr, 1, 3, fp) != 3) {
                fclose(fp);
                return false;
            }
        }
        if (pad && fwrite(zeros, 1, (size_t)pad, fp) != (size_t)pad) {
            fclose(fp);
            return false;
        }
    }
    fclose(fp);
    return true;
}
