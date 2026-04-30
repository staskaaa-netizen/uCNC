/*
 * ra8876_ll.c
 *
 * Low-level RA8876 backend for µCNC / Core0 renderer use.
 *
 * Based on the same proven building blocks you already used:
 *   - softspi/HARDSPI transport
 *   - command/data register access
 *   - PLL + SDRAM + panel init
 *   - text mode + graphic mode
 *   - fill rectangle + text print
 */

#include "../../cnc.h"
#include "../softspi.h"
#include "ra8876_ll.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define RA_CS_PIN    SPI2_CS

// Start slow for transport debug first
HARDSPI(ra8876_spi, SPI2_FREQ, 0, mcu_spi2_port);
#define RA_PORT ((softspi_port_t *)&ra8876_spi)

/* ------------------------------------------------------------------------- */
/*                            INTERNAL STATE                                 */
/* ------------------------------------------------------------------------- */

static bool g_ra8876_inited = false;
ra_mode_t g_mode = RA_MODE_GRAPHIC;
ra_font_size_t g_font_size = RA_FONT_SMALL;
static uint32_t g_draw_base = RA8876_VISIBLE_ADDR;

/* ------------------------------------------------------------------------- */
/*                       LOW LEVEL SPI / REGISTER I/O                        */
/* ------------------------------------------------------------------------- */

static inline void ra_spi_start(void)
{
	softspi_start(RA_PORT);
	mcu_clear_output(RA_CS_PIN);
}

static inline void ra_spi_stop(void)
{
	mcu_set_output(RA_CS_PIN);
	softspi_stop(RA_PORT);
}

static inline uint8_t ra_rw(uint8_t v)
{
    return softspi_xmit((softspi_port_t *)&ra8876_spi, v);
}

static inline void ra_send_cmd(uint8_t cmd)
{
    ra_spi_start();
    ra_rw(0x00);
    ra_rw(cmd);
    ra_spi_stop();
}

static inline void ra_send_data8(uint8_t data)
{
    ra_spi_start();
    ra_rw(0x80);
    ra_rw(data);
    ra_spi_stop();
}

static inline void ra_send_cmd_data8(uint8_t cmd, uint8_t data)
{
    ra_spi_start();
    ra_rw(0x00);
    ra_rw(cmd);
    ra_rw(0x80);
    ra_rw(data);
    ra_spi_stop();
}

static inline void ra_send_cmd_data16(uint8_t cmdLo, uint16_t val)
{
    ra_send_cmd_data8((uint8_t)(cmdLo + 0), (uint8_t)(val & 0xFF));
    ra_send_cmd_data8((uint8_t)(cmdLo + 1), (uint8_t)(val >> 8));
}

static inline void ra_send_cmd_data32(uint8_t cmdLo, uint32_t val)
{
    ra_send_cmd_data8((uint8_t)(cmdLo + 0), (uint8_t)(val & 0xFF));
    ra_send_cmd_data8((uint8_t)(cmdLo + 1), (uint8_t)((val >> 8) & 0xFF));
    ra_send_cmd_data8((uint8_t)(cmdLo + 2), (uint8_t)((val >> 16) & 0xFF));
    ra_send_cmd_data8((uint8_t)(cmdLo + 3), (uint8_t)((val >> 24) & 0xFF));
}

static uint8_t ra_status(void)
{
    uint8_t r;

    ra_spi_start();
    ra_rw(0x40);
    r = ra_rw(0x00);
    ra_spi_stop();

    return r;
}

static uint8_t ra_read_data(void)
{
    uint8_t r;

    ra_spi_start();
    ra_rw(0xC0);
    r = ra_rw(0x00);
    ra_spi_stop();

    return r;
}

static void ra_cmd(uint8_t c)               { ra_send_cmd(c); }
static void ra_data(uint8_t d)              { ra_send_data8(d); }
static void ra_reg(uint8_t r, uint8_t v)    { ra_send_cmd_data8(r, v); }
static void ra_reg16(uint8_t r, uint16_t v) { ra_send_cmd_data16(r, v); }
static void ra_reg32(uint8_t r, uint32_t v) { ra_send_cmd_data32(r, v); }

/* ------------------------------------------------------------------------- */
/*                          WAIT / DELAY HELPERS                             */
/* ------------------------------------------------------------------------- */

static void ra_delay(uint32_t ms)
{
    uint32_t t0 = mcu_millis();

    while ((mcu_millis() - t0) < ms)
    {
       vTaskDelay(1);
    }
}

static void ra_wait_ready(void)
{
    uint32_t t0 = mcu_millis();

    while (ra_status() & 0x02)
    {
        if ((mcu_millis() - t0) > 20)
            break;

        vTaskDelay(1);
    }
}

static void ra_wait_sdram_ready_timeout(void)
{
    uint32_t t0 = mcu_millis();

    while ((ra_status() & 0x04) == 0)
    {
        if ((mcu_millis() - t0) > 800)
            break;

        vTaskDelay(1);
    }
}

static void ra_wait_draw_done(void)
{
    uint32_t t0 = mcu_millis();

    while (ra_status() & 0x08)
    {
        if ((mcu_millis() - t0) > 100)
            break;

        vTaskDelay(1);
    }
}

static void ra_wait_text_done(void)
{
    uint32_t t0 = mcu_millis();

    while (ra_status() & 0x08)
    {
        if ((mcu_millis() - t0) > 50)
            break;

        vTaskDelay(1);
    }
}

static void ra_wait_bte_done(void)
{
    uint32_t t0 = mcu_millis();

    while (ra_status() & 0x08)
    {
        if ((mcu_millis() - t0) > 250)
            break;

        vTaskDelay(1);
    }
}

/* ------------------------------------------------------------------------- */
/*                              CHIP SETUP                                   */
/* ------------------------------------------------------------------------- */

static void ra_reset(void)
{
    io_set_output(RA_RESET_PIN);
    io_set_output(RA_CS_PIN);

    io_set_pinvalue(RA_CS_PIN, 1);
    io_set_pinvalue(RA_RESET_PIN, 1);
    ra_delay(20);

    io_set_pinvalue(RA_RESET_PIN, 0);
    ra_delay(20);

    io_set_pinvalue(RA_RESET_PIN, 1);
    ra_delay(50);
}

static void ra_pll_init(void)
{
    /* SCAN_FREQ = 50 MHz */
    ra_cmd(0x05); ra_data(0x06);
    ra_cmd(0x06); ra_data((50 * 8 / 10) - 1);

    /* DRAM_FREQ = 100 MHz */
    ra_cmd(0x07); ra_data(0x04);
    ra_cmd(0x08); ra_data((100 * 4 / 10) - 1);

    /* CORE_FREQ = 100 MHz */
    ra_cmd(0x09); ra_data(0x04);
    ra_cmd(0x0A); ra_data((100 * 4 / 10) - 1);

    ra_cmd(0x01);
    ra_cmd(0x00);
    ra_delay(1);

    ra_cmd(0x80);
    ra_delay(1);
}

static void ra_sdram_init(void)
{
    uint16_t sdram_itv;

    ra_reg(0xE0, 0x29);
    ra_reg(0xE1, 0x03);

    sdram_itv = (64000000UL / 8192UL) / (1000UL / 60UL);
    sdram_itv -= 2U;

    ra_reg(0xE2, (uint8_t)(sdram_itv & 0xFF));
    ra_reg(0xE3, (uint8_t)(sdram_itv >> 8));
    ra_reg(0xE4, 0x01);

    ra_wait_sdram_ready_timeout();
    ra_delay(1);
}

static void ra_panel_init(void)
{
    ra_reg(0x01, 0x11); /* TFT16 + host16 */
    ra_reg(0x02, 0x40); /* 16bpp, L->R, T->B */
    ra_reg(0x03, 0x00); /* graphic mode, SDRAM */

    {
        uint8_t v = 0x00;
        if (RA8876_LCD_PCLK_FALLING_RISING)
            v |= 0x80;
        ra_reg(0x12, v);
    }

    {
        uint8_t v = 0x00;
        if (RA8876_LCD_HSYNC_ACTIVE_POL)
            v |= 0x80;
        if (RA8876_LCD_VSYNC_ACTIVE_POL)
            v |= 0x40;
        if (!RA8876_LCD_DE_ACTIVE_POL)
            v |= 0x20;
        ra_reg(0x13, v);
    }

    if (RA8876_TFT_W < 8)
    {
        ra_reg(0x14, 0x00);
        ra_reg(0x15, RA8876_TFT_W);
    }
    else
    {
        ra_reg(0x14, (uint8_t)((RA8876_TFT_W / 8) - 1));
        ra_reg(0x15, (uint8_t)(RA8876_TFT_W % 8));
    }

    ra_reg16(0x1A, (uint16_t)(RA8876_TFT_H - 1));

    if (RA8876_LCD_HBPD < 8)
    {
        ra_reg(0x16, 0x00);
        ra_reg(0x17, RA8876_LCD_HBPD);
    }
    else
    {
        ra_reg(0x16, (uint8_t)((RA8876_LCD_HBPD / 8) - 1));
        ra_reg(0x17, (uint8_t)(RA8876_LCD_HBPD % 8));
    }

    ra_reg(0x18, (uint8_t)((RA8876_LCD_HFPD < 8) ? 0 : ((RA8876_LCD_HFPD / 8) - 1)));
    ra_reg(0x19, (uint8_t)((RA8876_LCD_HSPW < 8) ? 0 : ((RA8876_LCD_HSPW / 8) - 1)));

    ra_reg16(0x1C, (uint16_t)(RA8876_LCD_VBPD - 1));
    ra_reg(0x1E, (uint8_t)(RA8876_LCD_VFPD - 1));
    ra_reg(0x1F, (uint8_t)(RA8876_LCD_VSPW - 1));

    ra_reg(0x10, 0x04); /* main window 16bpp */
    ra_reg(0x5E, 0x01); /* XY mode + 16bpp */

    ra_reg32(0x20, RA8876_VISIBLE_ADDR);
    ra_reg16(0x24, RA8876_TFT_W);
    ra_reg16(0x26, 0);
    ra_reg16(0x28, 0);

    ra_reg32(0x50, RA8876_VISIBLE_ADDR);
    ra_reg16(0x54, RA8876_TFT_W);

    ra_reg16(0x56, 0);
    ra_reg16(0x58, 0);
    ra_reg16(0x5A, RA8876_TFT_W);
    ra_reg16(0x5C, RA8876_TFT_H);

    {
        uint8_t v = 0x40;
        if (RA8876_LCD_PCLK_FALLING_RISING)
            v |= 0x80;
        ra_reg(0x12, v); /* display on */
    }
}

static void ra_apply_font_size(void)
{
    /*
     * CCR0[5:4] selects one of the internal font sizes.
     * Encoding remains ISO-8859-1 for now.
     */
    ra_reg(0xCC, (uint8_t)((g_font_size & 0x03) << 4));
    //ra_delay(10);
}

static void ra_text_setup(void)
{
    ra_apply_font_size();
    ra_reg(0xCD, 0x00); /* normal orientation, bg color mode */
    ra_reg(0xD0, 0x00); /* line distance */
    ra_reg(0xD1, 0x00); /* char spacing */
}

/* ------------------------------------------------------------------------- */
/*                           DRAWING HELPERS                                 */
/* ------------------------------------------------------------------------- */

static void ra_fg(uint16_t c)
{
    ra_reg(0xD2, (uint8_t)(c >> 8));
    ra_reg(0xD3, (uint8_t)(c >> 3));
    ra_reg(0xD4, (uint8_t)(c << 3));
}

static void ra_bg(uint16_t c)
{
    ra_reg(0xD5, (uint8_t)(c >> 8));
    ra_reg(0xD6, (uint8_t)(c >> 3));
    ra_reg(0xD7, (uint8_t)(c << 3));
}

static void ra_text_xy(uint16_t x, uint16_t y)
{
    ra_reg16(0x63, x);
    ra_reg16(0x65, y);
}

void ra_text_mode(void)
{
    if (g_mode == RA_MODE_TEXT)
        return;

    ra_reg(0x03, 0x04);
    g_mode = RA_MODE_TEXT;
}

void ra_graphic_mode(void)
{
    if (g_mode == RA_MODE_GRAPHIC)
        return;

    ra_reg(0x03, 0x00);
    g_mode = RA_MODE_GRAPHIC;
}

void ra_set_font_size(ra_font_size_t size)
{
    if (size > RA_FONT_LARGE)
        size = RA_FONT_LARGE;

    if (g_font_size == size)
        return;

    g_font_size = size;
    ra_apply_font_size();
}

ra_font_size_t ra_get_font_size(void)
{
    return g_font_size;
}

/* ------------------------------------------------------------------------- */
/*                               PUBLIC API                                  */
/* ------------------------------------------------------------------------- */

void ra_init(void)
{
    if (g_ra8876_inited)
        return;

    mcu_config_output(RA_CS_PIN);
    mcu_set_output(RA_CS_PIN);

    mcu_config_output(RA_RESET_PIN);
    mcu_set_output(RA_RESET_PIN);

    ra_reset();
    ra_pll_init();
    ra_sdram_init();
    ra_panel_init();
    g_mode = RA_MODE_GRAPHIC;
    g_font_size = RA_FONT_SMALL;
    g_draw_base = RA8876_VISIBLE_ADDR;
    ra_text_setup();
    ra_text_mode();

    g_ra8876_inited = true;
}

bool ra_is_inited(void)
{
    return g_ra8876_inited;
}

void ra_clear(uint16_t color)
{
    ra_fill_rect(0, 0, RA8876_TFT_W - 1, RA8876_TFT_H - 1, color);
}

void ra_set_draw_base(uint32_t base)
{
    if (!g_ra8876_inited)
        return;

    if (g_draw_base == base)
        return;

    ra_wait_draw_done();
    ra_wait_bte_done();

    ra_reg32(0x50, base);
    ra_reg16(0x54, RA8876_TFT_W);

    ra_reg16(0x56, 0);
    ra_reg16(0x58, 0);
    ra_reg16(0x5A, RA8876_TFT_W);
    ra_reg16(0x5C, RA8876_TFT_H);

    g_draw_base = base;
}

uint32_t ra_get_draw_base(void)
{
    return g_draw_base;
}

void ra_set_draw_page(uint8_t page)
{
    ra_set_draw_base(RA_PAGE_BASE((uint32_t)page));
}

void ra_blit(uint32_t src_base,
             uint16_t src_x,
             uint16_t src_y,
             uint32_t dst_base,
             uint16_t dst_x,
             uint16_t dst_y,
             uint16_t width,
             uint16_t height)
{
    if (!g_ra8876_inited || width == 0 || height == 0)
        return;

    if (src_x >= RA8876_TFT_W || src_y >= RA8876_TFT_H ||
        dst_x >= RA8876_TFT_W || dst_y >= RA8876_TFT_H)
        return;

    if ((uint32_t)src_x + width > RA8876_TFT_W)
        width = (uint16_t)(RA8876_TFT_W - src_x);
    if ((uint32_t)dst_x + width > RA8876_TFT_W)
        width = (uint16_t)(RA8876_TFT_W - dst_x);
    if ((uint32_t)src_y + height > RA8876_TFT_H)
        height = (uint16_t)(RA8876_TFT_H - src_y);
    if ((uint32_t)dst_y + height > RA8876_TFT_H)
        height = (uint16_t)(RA8876_TFT_H - dst_y);
    if (width == 0 || height == 0)
        return;

    ra_wait_draw_done();
    ra_wait_bte_done();
    ra_graphic_mode();

    /* Source 0: hidden/visible framebuffer rectangle. */
    ra_reg32(0x93, src_base);
    ra_reg16(0x97, RA8876_TFT_W);
    ra_reg16(0x99, src_x);
    ra_reg16(0x9B, src_y);

    /* Destination: visible/hidden framebuffer rectangle. */
    ra_reg32(0xA7, dst_base);
    ra_reg16(0xAB, RA8876_TFT_W);
    ra_reg16(0xAD, dst_x);
    ra_reg16(0xAF, dst_y);

    ra_reg16(0xB1, width);
    ra_reg16(0xB3, height);

    /* S0/S1/destination are all RGB565. ROP C copies S0 to destination. */
    ra_reg(0x92, 0x25);
    ra_reg(0x91, 0xC2);
    ra_reg(0x90, 0x10);

    ra_wait_bte_done();
    ra_text_mode();
}

void ra_draw_line(int x1, int y1, int x2, int y2, uint16_t color)
{
    if (!g_ra8876_inited)
        return;

    /* clamp to screen */
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 < 0) x2 = 0;
    if (y2 < 0) y2 = 0;

    if (x1 >= RA8876_TFT_W) x1 = RA8876_TFT_W - 1;
    if (x2 >= RA8876_TFT_W) x2 = RA8876_TFT_W - 1;
    if (y1 >= RA8876_TFT_H) y1 = RA8876_TFT_H - 1;
    if (y2 >= RA8876_TFT_H) y2 = RA8876_TFT_H - 1;

    ra_graphic_mode();
    ra_fg(color);

    /* start point: 68h..6Bh */
    ra_reg16(0x68, (uint16_t)x1);
    ra_reg16(0x6A, (uint16_t)y1);

    /* end point: 6Ch..6Fh */
    ra_reg16(0x6C, (uint16_t)x2);
    ra_reg16(0x6E, (uint16_t)y2);

    /* start line draw: 67h = 0x80 */
    ra_reg(0x67, 0x80);

    ra_wait_draw_done();
    ra_text_mode();
}
void ra_draw_rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    if (!g_ra8876_inited)
        return;

    if (x1 >= RA8876_TFT_W) x1 = RA8876_TFT_W - 1;
    if (x2 >= RA8876_TFT_W) x2 = RA8876_TFT_W - 1;
    if (y1 >= RA8876_TFT_H) y1 = RA8876_TFT_H - 1;
    if (y2 >= RA8876_TFT_H) y2 = RA8876_TFT_H - 1;

    if (x2 < x1)
    {
        uint16_t t = x1;
        x1 = x2;
        x2 = t;
    }

    if (y2 < y1)
    {
        uint16_t t = y1;
        y1 = y2;
        y2 = t;
    }

    ra_graphic_mode();
    ra_fg(color);

    ra_reg16(0x68, x1);
    ra_reg16(0x6A, y1);
    ra_reg16(0x6C, x2);
    ra_reg16(0x6E, y2);

    ra_send_cmd_data8(0x76, 0xA0);
    ra_wait_draw_done();

    ra_text_mode();
}

void ra_fill_rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    if (!g_ra8876_inited)
        return;

    /*
     * Clamp to screen.
     * Keeps callers simple and avoids odd overflows.
     */
    if (x1 >= RA8876_TFT_W) x1 = RA8876_TFT_W - 1;
    if (x2 >= RA8876_TFT_W) x2 = RA8876_TFT_W - 1;
    if (y1 >= RA8876_TFT_H) y1 = RA8876_TFT_H - 1;
    if (y2 >= RA8876_TFT_H) y2 = RA8876_TFT_H - 1;

    if (x2 < x1)
    {
        uint16_t t = x1;
        x1 = x2;
        x2 = t;
    }

    if (y2 < y1)
    {
        uint16_t t = y1;
        y1 = y2;
        y2 = t;
    }

    ra_graphic_mode();
    ra_fg(color);

    ra_reg16(0x68, x1);
    ra_reg16(0x6A, y1);
    ra_reg16(0x6C, x2);
    ra_reg16(0x6E, y2);

    ra_send_cmd_data8(0x76, 0xE0);
    ra_wait_draw_done();

    ra_text_mode();
}

void ra_text(uint16_t x, uint16_t y, uint16_t fg, uint16_t bg, const char *s)
{
    if (!g_ra8876_inited || !s)
        return;

    ra_fg(fg);
    ra_bg(bg);
    ra_text_xy(x, y);
    ra_text_mode();

    /*
     * Write to MRWC and then stream text bytes.
     */
    ra_spi_start();
    ra_rw(0x00);
    ra_rw(0x04);
    ra_spi_stop();

    ra_spi_start();
    ra_rw(0x80);
    while (*s)
    {
        ra_rw((uint8_t)(*s));
        s++;
    }
    ra_spi_stop();

    ra_wait_ready();
    ra_wait_text_done();
}
