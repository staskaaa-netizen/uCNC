#include "../../cnc.h"
#include "lvds_hstx.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pll.h"
#include "hardware/regs/clocks.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "lvds_fonts.h"
#include "lvds_palette.h"

#ifndef LEANCAM_USE_PSRAM_BACKBUFFER
#define LEANCAM_USE_PSRAM_BACKBUFFER 0
#endif

#if LEANCAM_USE_PSRAM_BACKBUFFER
#include "lvds_psram.h"
#endif

#define LVDS_HSTX_H_FRONT_PORCH 40
#define LVDS_HSTX_H_SYNC_WIDTH 128
#define LVDS_HSTX_H_BACK_PORCH 88
#define LVDS_HSTX_V_FRONT_PORCH 1
#define LVDS_HSTX_V_SYNC_WIDTH 4
#define LVDS_HSTX_V_BACK_PORCH 61
#define LVDS_HSTX_H_INACTIVE (LVDS_HSTX_H_FRONT_PORCH + LVDS_HSTX_H_SYNC_WIDTH + LVDS_HSTX_H_BACK_PORCH)
#define LVDS_HSTX_V_INACTIVE (LVDS_HSTX_V_FRONT_PORCH + LVDS_HSTX_V_SYNC_WIDTH + LVDS_HSTX_V_BACK_PORCH)
#define LVDS_HSTX_H_TOTAL (LVDS_HSTX_WIDTH + LVDS_HSTX_H_INACTIVE)
#define LVDS_HSTX_V_TOTAL (LVDS_HSTX_HEIGHT + LVDS_HSTX_V_INACTIVE)

#define LVDS_HSTX_H_FRONT_TRANSFERS (7 * LVDS_HSTX_H_FRONT_PORCH / 8)
#define LVDS_HSTX_H_SYNC_TRANSFERS (7 * LVDS_HSTX_H_SYNC_WIDTH / 8)
#define LVDS_HSTX_H_INACTIVE_TRANSFERS (7 * LVDS_HSTX_H_INACTIVE / 8)
#define LVDS_HSTX_H_ACTIVE_TRANSFERS (7 * LVDS_HSTX_WIDTH / 8)
#define LVDS_HSTX_LINE_TRANSFERS (LVDS_HSTX_H_INACTIVE_TRANSFERS + LVDS_HSTX_H_ACTIVE_TRANSFERS)

#define LVDS_HSTX_FB_SIZE (LVDS_HSTX_WIDTH * LVDS_HSTX_HEIGHT / 2)
#define LVDS_HSTX_CLOCK 0xC6000000u
#define LVDS_HSTX_ACTIVE (LVDS_HSTX_CLOCK | 0x00E00000u)
#define LVDS_HSTX_INACTIVE (LVDS_HSTX_CLOCK | 0x00600000u)
#define LVDS_HSTX_INACTIVE_HS (LVDS_HSTX_CLOCK | 0x00400000u)
#define LVDS_HSTX_INACTIVE_VS (LVDS_HSTX_CLOCK | 0x00200000u)
#define LVDS_HSTX_INACTIVE_HSVS LVDS_HSTX_CLOCK

#define LVDS_HSTX_DMACH_PING 10
#define LVDS_HSTX_DMACH_PONG 11
#define LVDS_HSTX_SYS_CLOCK_KHZ LVDS_SYS_CLOCK_KHZ

#ifndef LEANCAM_USE_HSTX_PLL
#define LEANCAM_USE_HSTX_PLL 0
#endif

#ifndef LVDS_HSTX_PLL_KHZ
#define LVDS_HSTX_PLL_KHZ LVDS_HSTX_SYS_CLOCK_KHZ
#endif

#ifndef LVDS_HSTX_CLOCK_DIV
#define LVDS_HSTX_CLOCK_DIV 2
#endif

static uint8_t g_framebuffer[LVDS_HSTX_FB_SIZE] __attribute__((aligned(4)));
static uint8_t *g_draw_buffer = g_framebuffer;
static uint32_t g_active_line[3][LVDS_HSTX_LINE_TRANSFERS] __attribute__((aligned(4)));
static uint32_t g_inactive_line[LVDS_HSTX_LINE_TRANSFERS] __attribute__((aligned(4)));
static uint32_t g_inactive_line_vs[LVDS_HSTX_LINE_TRANSFERS] __attribute__((aligned(4)));
static uint32_t g_colour_lut[7][256] __attribute__((aligned(4)));
static uint16_t g_palette_rgb565[16];
static bool g_palette_used[16];
static uint8_t g_palette_next;
static volatile bool g_display_started;
static bool g_backbuffer_active;
static bool g_direct_scanout = true;
static bool g_dirty;
static int g_last_error;
static volatile bool g_core1_ready;

static uint16_t rgb_to_565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)((((uint16_t)r & 0xf8u) << 8) |
                      (((uint16_t)g & 0xfcu) << 3) |
                      (((uint16_t)b & 0xf8u) >> 3));
}

static uint32_t lvds_colour_from_rgb565(uint16_t rgb565)
{
    uint8_t r5 = (uint8_t)((rgb565 >> 11) & 0x1f);
    uint8_t g6 = (uint8_t)((rgb565 >> 5) & 0x3f);
    uint8_t b5 = (uint8_t)(rgb565 & 0x1f);
    uint8_t r6 = (uint8_t)((r5 << 1) | (r5 >> 4));
    uint8_t b6 = (uint8_t)((b5 << 1) | (b5 >> 4));

    // LQ121S1LG44 single-channel LVDS:
    // RXIN0 R0 R1 R2 R3 R4 R5 G0, RXIN1 G1..G5 B0 B1,
    // RXIN2 B2 B3 B4 B5 HS VS DE.
    return LVDS_HSTX_ACTIVE |
           ((uint32_t)(r6 & 0x3f) << 1) |
           ((uint32_t)(g6 & 0x3e) << 8) |
           ((uint32_t)(g6 & 0x01) << 7) |
           ((uint32_t)(b6 & 0x3c) << 15) |
           ((uint32_t)(b6 & 0x03) << 14);
}

static void lvds_rebuild_lut(void)
{
    for (uint16_t i = 0; i < 16; i++) {
        for (uint16_t j = 0; j < 16; j++) {
            uint16_t colours = (uint16_t)((j << 4) | i);
            uint32_t first_pixel = lvds_colour_from_rgb565(g_palette_rgb565[i]);
            uint32_t next_pixel = lvds_colour_from_rgb565(g_palette_rgb565[j]);
            g_colour_lut[0][colours] = ((first_pixel << 0) & 0xFEFEFEFEu) | ((next_pixel >> 7) & 0x01010101u);
            g_colour_lut[1][colours] = ((first_pixel << 1) & 0xFCFCFCFCu) | ((next_pixel >> 6) & 0x03030303u);
            g_colour_lut[2][colours] = ((first_pixel << 2) & 0xF8F8F8F8u) | ((next_pixel >> 5) & 0x07070707u);
            g_colour_lut[3][colours] = ((first_pixel << 3) & 0xF0F0F0F0u) | ((next_pixel >> 4) & 0x0F0F0F0Fu);
            g_colour_lut[4][colours] = ((first_pixel << 4) & 0xE0E0E0E0u) | ((next_pixel >> 3) & 0x1F1F1F1Fu);
            g_colour_lut[5][colours] = ((first_pixel << 5) & 0xC0C0C0C0u) | ((next_pixel >> 2) & 0x3F3F3F3Fu);
            g_colour_lut[6][colours] = ((first_pixel << 6) & 0x80808080u) | ((next_pixel >> 1) & 0x7F7F7F7Fu);
        }
    }
}

static void palette_set(uint8_t index, uint16_t rgb565)
{
    if (index >= 16) {
        return;
    }
    g_palette_rgb565[index] = rgb565;
    g_palette_used[index] = true;
    lvds_rebuild_lut();
}

static void palette_reset(void)
{
    static const uint16_t defaults[16] = {
        0x0000, 0xffff, 0xf800, 0x07e0, 0x001f, 0xffe0, 0xf81f, 0x07ff,
        0x8410, 0xc618, 0x4208, 0xfc00, 0x03e0, 0x0010, 0xfd20, 0x07ff
    };
    for (uint8_t i = 0; i < 16; i++) {
        g_palette_rgb565[i] = defaults[i];
        g_palette_used[i] = false;
    }
    g_palette_used[0] = true;
    g_palette_next = 1;
    lvds_palette_reset();
    lvds_rebuild_lut();
}

static lvds_color_t palette_lookup(uint16_t rgb565)
{
    uint32_t best_dist = 0xffffffffu;
    uint8_t best = 0;

    for (uint8_t i = 0; i < 16; i++) {
        if (g_palette_used[i] && g_palette_rgb565[i] == rgb565) {
            return i;
        }
    }
    if (g_palette_next < 16) {
        uint8_t index = g_palette_next++;
        palette_set(index, rgb565);
        return index;
    }
    for (uint8_t i = 0; i < 16; i++) {
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

static void compute_line(uint32_t pixel_template, uint32_t *line, uint16_t from, uint16_t to)
{
    uint32_t px[7];
    px[0] = ((pixel_template << 0) & 0xFEFEFEFEu) | ((pixel_template >> 7) & 0x01010101u);
    px[1] = ((pixel_template << 1) & 0xFCFCFCFCu) | ((pixel_template >> 6) & 0x03030303u);
    px[2] = ((pixel_template << 2) & 0xF8F8F8F8u) | ((pixel_template >> 5) & 0x07070707u);
    px[3] = ((pixel_template << 3) & 0xF0F0F0F0u) | ((pixel_template >> 4) & 0x0F0F0F0Fu);
    px[4] = ((pixel_template << 4) & 0xE0E0E0E0u) | ((pixel_template >> 3) & 0x1F1F1F1Fu);
    px[5] = ((pixel_template << 5) & 0xC0C0C0C0u) | ((pixel_template >> 2) & 0x3F3F3F3Fu);
    px[6] = ((pixel_template << 6) & 0x80808080u) | ((pixel_template >> 1) & 0x7F7F7F7Fu);
    for (uint16_t i = from; i < to; i++) {
        line[i] = px[i % 7];
    }
}

static void init_lines(void)
{
    compute_line(LVDS_HSTX_INACTIVE, g_inactive_line, 0, LVDS_HSTX_H_FRONT_TRANSFERS);
    compute_line(LVDS_HSTX_INACTIVE, g_active_line[0], 0, LVDS_HSTX_H_FRONT_TRANSFERS);
    compute_line(LVDS_HSTX_INACTIVE, g_active_line[1], 0, LVDS_HSTX_H_FRONT_TRANSFERS);
    compute_line(LVDS_HSTX_INACTIVE, g_active_line[2], 0, LVDS_HSTX_H_FRONT_TRANSFERS);
    compute_line(LVDS_HSTX_INACTIVE_VS, g_inactive_line_vs, 0, LVDS_HSTX_H_FRONT_TRANSFERS);

    compute_line(LVDS_HSTX_INACTIVE_HS, g_inactive_line, LVDS_HSTX_H_FRONT_TRANSFERS, LVDS_HSTX_H_FRONT_TRANSFERS + LVDS_HSTX_H_SYNC_TRANSFERS);
    compute_line(LVDS_HSTX_INACTIVE_HS, g_active_line[0], LVDS_HSTX_H_FRONT_TRANSFERS, LVDS_HSTX_H_FRONT_TRANSFERS + LVDS_HSTX_H_SYNC_TRANSFERS);
    compute_line(LVDS_HSTX_INACTIVE_HS, g_active_line[1], LVDS_HSTX_H_FRONT_TRANSFERS, LVDS_HSTX_H_FRONT_TRANSFERS + LVDS_HSTX_H_SYNC_TRANSFERS);
    compute_line(LVDS_HSTX_INACTIVE_HS, g_active_line[2], LVDS_HSTX_H_FRONT_TRANSFERS, LVDS_HSTX_H_FRONT_TRANSFERS + LVDS_HSTX_H_SYNC_TRANSFERS);
    compute_line(LVDS_HSTX_INACTIVE_HSVS, g_inactive_line_vs, LVDS_HSTX_H_FRONT_TRANSFERS, LVDS_HSTX_H_FRONT_TRANSFERS + LVDS_HSTX_H_SYNC_TRANSFERS);

    compute_line(LVDS_HSTX_INACTIVE, g_inactive_line, LVDS_HSTX_H_FRONT_TRANSFERS + LVDS_HSTX_H_SYNC_TRANSFERS, LVDS_HSTX_H_INACTIVE_TRANSFERS);
    compute_line(LVDS_HSTX_INACTIVE, g_active_line[0], LVDS_HSTX_H_FRONT_TRANSFERS + LVDS_HSTX_H_SYNC_TRANSFERS, LVDS_HSTX_H_INACTIVE_TRANSFERS);
    compute_line(LVDS_HSTX_INACTIVE, g_active_line[1], LVDS_HSTX_H_FRONT_TRANSFERS + LVDS_HSTX_H_SYNC_TRANSFERS, LVDS_HSTX_H_INACTIVE_TRANSFERS);
    compute_line(LVDS_HSTX_INACTIVE, g_active_line[2], LVDS_HSTX_H_FRONT_TRANSFERS + LVDS_HSTX_H_SYNC_TRANSFERS, LVDS_HSTX_H_INACTIVE_TRANSFERS);
    compute_line(LVDS_HSTX_INACTIVE_VS, g_inactive_line_vs, LVDS_HSTX_H_FRONT_TRANSFERS + LVDS_HSTX_H_SYNC_TRANSFERS, LVDS_HSTX_H_INACTIVE_TRANSFERS);

    compute_line(LVDS_HSTX_INACTIVE, g_inactive_line, LVDS_HSTX_H_INACTIVE_TRANSFERS, LVDS_HSTX_LINE_TRANSFERS);
    compute_line(LVDS_HSTX_INACTIVE_VS, g_inactive_line_vs, LVDS_HSTX_H_INACTIVE_TRANSFERS, LVDS_HSTX_LINE_TRANSFERS);
}

static void __no_inline_not_in_flash_func(compute_active_line)(uint16_t y, uint8_t buffer_index)
{
    uint32_t base = (uint32_t)y * (LVDS_HSTX_WIDTH / 2);
    uint16_t offset = 0;
    uint32_t *line = g_active_line[buffer_index % 3u];
    for (uint16_t i = LVDS_HSTX_H_INACTIVE_TRANSFERS; i < LVDS_HSTX_LINE_TRANSFERS; i += 7) {
        uint32_t eight_pixels = *(const uint32_t *)(g_framebuffer + base + 4u * offset++);
        line[i + 0] = g_colour_lut[0][eight_pixels & 0xffu];
        eight_pixels >>= 4;
        line[i + 1] = g_colour_lut[1][eight_pixels & 0xffu];
        eight_pixels >>= 4;
        line[i + 2] = g_colour_lut[2][eight_pixels & 0xffu];
        eight_pixels >>= 4;
        line[i + 3] = g_colour_lut[3][eight_pixels & 0xffu];
        eight_pixels >>= 4;
        line[i + 4] = g_colour_lut[4][eight_pixels & 0xffu];
        eight_pixels >>= 4;
        line[i + 5] = g_colour_lut[5][eight_pixels & 0xffu];
        eight_pixels >>= 4;
        line[i + 6] = g_colour_lut[6][eight_pixels & 0xffu];
    }
}

static uint g_v_scanline = 2;

static void __no_inline_not_in_flash_func(prepare_next_dma_line)(uint ch_num)
{
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    static uint16_t capture_line;

    ch->transfer_count = LVDS_HSTX_LINE_TRANSFERS;

    if ((g_v_scanline < LVDS_HSTX_V_FRONT_PORCH) ||
        ((g_v_scanline >= LVDS_HSTX_V_FRONT_PORCH + LVDS_HSTX_V_SYNC_WIDTH) &&
         (g_v_scanline < LVDS_HSTX_V_INACTIVE))) {
        ch->read_addr = (uintptr_t)g_inactive_line;
        if (g_v_scanline == (LVDS_HSTX_V_INACTIVE - 1)) {
            ch->read_addr = (uintptr_t)g_active_line[0];
        }
    } else if ((g_v_scanline >= LVDS_HSTX_V_FRONT_PORCH) &&
               (g_v_scanline < LVDS_HSTX_V_FRONT_PORCH + LVDS_HSTX_V_SYNC_WIDTH)) {
        ch->read_addr = (uintptr_t)g_inactive_line_vs;
    } else {
        capture_line = (uint16_t)(g_v_scanline - LVDS_HSTX_V_INACTIVE);
        ch->read_addr = (uintptr_t)g_active_line[capture_line % 3u];
        if ((capture_line + 2u) < LVDS_HSTX_HEIGHT) {
            uint16_t prepare_line = (uint16_t)(capture_line + 2u);
            compute_active_line(prepare_line, (uint8_t)(prepare_line % 3u));
        }
    }
    g_v_scanline = (g_v_scanline + 1) % LVDS_HSTX_V_TOTAL;
}

static void __no_inline_not_in_flash_func(poll_dma_scanout)(void)
{
    const uint32_t ping_bit = 1u << LVDS_HSTX_DMACH_PING;
    const uint32_t pong_bit = 1u << LVDS_HSTX_DMACH_PONG;

    while (1) {
        uint32_t done = dma_hw->intr & (ping_bit | pong_bit);

        if (!done) {
            tight_loop_contents();
            continue;
        }
        if (done & ping_bit) {
            dma_hw->intr = ping_bit;
            prepare_next_dma_line(LVDS_HSTX_DMACH_PING);
        }

        if (done & pong_bit) {
            dma_hw->intr = pong_bit;
            prepare_next_dma_line(LVDS_HSTX_DMACH_PONG);
        }
    }
}

static void init_hstx(void)
{
    irq_set_enabled(DMA_IRQ_1, false);
    dma_channel_abort(LVDS_HSTX_DMACH_PING);
    dma_channel_abort(LVDS_HSTX_DMACH_PONG);
    dma_hw->inte1 &= ~((1u << LVDS_HSTX_DMACH_PING) | (1u << LVDS_HSTX_DMACH_PONG));
    dma_hw->ints1 = (1u << LVDS_HSTX_DMACH_PING) | (1u << LVDS_HSTX_DMACH_PONG);

    if (!dma_channel_is_claimed(LVDS_HSTX_DMACH_PING)) {
        dma_channel_claim(LVDS_HSTX_DMACH_PING);
    }
    if (!dma_channel_is_claimed(LVDS_HSTX_DMACH_PONG)) {
        dma_channel_claim(LVDS_HSTX_DMACH_PONG);
    }

    hstx_ctrl_hw->csr = 0;
    *((volatile uint32_t *)(0x40010000u + 0x58u)) = ((uint32_t)LVDS_HSTX_CLOCK_DIV << 16u);
    hstx_ctrl_hw->csr =
        4u << HSTX_CTRL_CSR_CLKDIV_LSB |
        4u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        30u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // Ascending LVDS pinout from the HAL config.
    static const int lane_to_output_bit[4] = {0, 2, 4, 6};
    static const uint8_t hstx_pins[8] = {
        LVDS_HSTX_D0P, LVDS_HSTX_D0M,
        LVDS_HSTX_D1P, LVDS_HSTX_D1M,
        LVDS_HSTX_D2P, LVDS_HSTX_D2M,
        LVDS_HSTX_CLKP, LVDS_HSTX_CLKM
    };
    for (uint lane = 0; lane < 4; lane++) {
        int bit = lane_to_output_bit[lane];
        uint32_t sel = (lane * 8 + 7) << HSTX_CTRL_BIT0_SEL_P_LSB |
                       (lane * 8 + 6) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit] = sel;
        hstx_ctrl_hw->bit[bit + 1] = sel | HSTX_CTRL_BIT0_INV_BITS;
    }
    for (uint i = 0; i < 8; i++) {
        gpio_set_function(hstx_pins[i], 0);
    }

    dma_channel_config c = dma_channel_get_default_config(LVDS_HSTX_DMACH_PING);
    channel_config_set_chain_to(&c, LVDS_HSTX_DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(LVDS_HSTX_DMACH_PING, &c, &hstx_fifo_hw->fifo, g_inactive_line, LVDS_HSTX_LINE_TRANSFERS, false);

    c = dma_channel_get_default_config(LVDS_HSTX_DMACH_PONG);
    channel_config_set_chain_to(&c, LVDS_HSTX_DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(LVDS_HSTX_DMACH_PONG, &c, &hstx_fifo_hw->fifo, g_inactive_line, LVDS_HSTX_LINE_TRANSFERS, false);

    dma_hw->ints1 = (1u << LVDS_HSTX_DMACH_PING) | (1u << LVDS_HSTX_DMACH_PONG);
    dma_hw->inte1 &= ~((1u << LVDS_HSTX_DMACH_PING) | (1u << LVDS_HSTX_DMACH_PONG));
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
    dma_channel_start(LVDS_HSTX_DMACH_PING);
}

static void __no_inline_not_in_flash_func(core1_entry)(void)
{
    init_hstx();
    g_core1_ready = true;
    poll_dma_scanout();
}

static void configure_lvds_clocks(void)
{
#if LEANCAM_USE_HSTX_PLL
    uint32_t hstx_hz = (uint32_t)LVDS_HSTX_PLL_KHZ * 1000u;
    uint32_t sys_hz = (uint32_t)LVDS_HSTX_SYS_CLOCK_KHZ * 1000u;

    set_sys_clock_pll(hstx_hz * 5u, 5, 1);
    clock_configure(clk_sys,
                    CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
                    CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    hstx_hz,
                    sys_hz);
    clock_configure(clk_hstx,
                    0,
                    CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    hstx_hz,
                    hstx_hz);
#else
    set_sys_clock_khz(LVDS_HSTX_SYS_CLOCK_KHZ, false);
#endif
}

static void put_pixel_raw(int x, int y, uint8_t color)
{
    uint32_t p;
    if ((unsigned)x >= LVDS_HSTX_WIDTH || (unsigned)y >= LVDS_HSTX_HEIGHT) {
        return;
    }
    p = (uint32_t)y * LVDS_HSTX_WIDTH + (uint32_t)x;
    color &= 0x0f;
    if (p & 1u) {
        g_draw_buffer[p >> 1] = (uint8_t)((g_draw_buffer[p >> 1] & 0x0f) | (color << 4));
    } else {
        g_draw_buffer[p >> 1] = (uint8_t)((g_draw_buffer[p >> 1] & 0xf0) | color);
    }
    g_dirty = true;
}

bool lvds_hstx_init(void)
{
    palette_reset();
    memset(g_framebuffer, 0, sizeof(g_framebuffer));
    g_draw_buffer = g_framebuffer;
    g_backbuffer_active = false;
    g_direct_scanout = true;
    g_dirty = false;
#if LEANCAM_USE_PSRAM_BACKBUFFER
    if (lvds_psram_available()) {
        g_draw_buffer = (uint8_t *)lvds_psram_ptr(0);
        memset(g_draw_buffer, 0, LVDS_HSTX_FB_SIZE);
        g_backbuffer_active = true;
        g_direct_scanout = false;
    }
#endif
    init_lines();
    compute_active_line(0, 0);
    compute_active_line(1, 1);
    compute_active_line(2, 2);
    configure_lvds_clocks();
    g_core1_ready = false;
    multicore_launch_core1(core1_entry);
    while (!g_core1_ready) {
        tight_loop_contents();
    }
    g_display_started = true;
    g_last_error = 0;
    lvds_hstx_clear(0);
    return true;
}

int lvds_hstx_last_error(void)
{
    return g_last_error;
}

void *lvds_hstx_scanout_buffer(void)
{
    return g_framebuffer;
}

bool lvds_hstx_backbuffer_active(void)
{
    return g_backbuffer_active;
}

void lvds_hstx_direct_scanout(bool direct)
{
    if (!g_backbuffer_active) {
        g_direct_scanout = true;
        g_draw_buffer = g_framebuffer;
        return;
    }
    if (g_direct_scanout == direct) {
        return;
    }
    g_direct_scanout = direct;
    if (direct) {
        memcpy(g_framebuffer, g_draw_buffer, LVDS_HSTX_FB_SIZE);
        g_draw_buffer = g_framebuffer;
        g_dirty = false;
    } else {
#if LEANCAM_USE_PSRAM_BACKBUFFER
        memcpy((uint8_t *)lvds_psram_ptr(0), g_framebuffer, LVDS_HSTX_FB_SIZE);
        g_draw_buffer = (uint8_t *)lvds_psram_ptr(0);
        g_dirty = false;
#else
        g_direct_scanout = true;
#endif
    }
}

void lvds_hstx_clear(lvds_color_t color)
{
    uint8_t c = (uint8_t)(color & 0x0f);
    memset(g_draw_buffer, (int)((c << 4) | c), LVDS_HSTX_FB_SIZE);
    g_dirty = true;
}

void lvds_hstx_pixel(int x, int y, lvds_color_t color)
{
    put_pixel_raw(x, y, (uint8_t)color);
}

void lvds_hstx_line(int x1, int y1, int x2, int y2, lvds_color_t color)
{
    int dx = abs(x2 - x1);
    int sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1);
    int sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        lvds_hstx_pixel(x1, y1, color);
        if (x1 == x2 && y1 == y2) {
            break;
        }
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

void lvds_hstx_line_w(int x1, int y1, int x2, int y2, lvds_color_t color, int thick)
{
    int r = thick > 1 ? thick / 2 : 0;
    for (int oy = -r; oy <= r; oy++) {
        for (int ox = -r; ox <= r; ox++) {
            lvds_hstx_line(x1 + ox, y1 + oy, x2 + ox, y2 + oy, color);
        }
    }
}

void lvds_hstx_rect(int x, int y, int w, int h, lvds_color_t color)
{
    lvds_hstx_line(x, y, x + w - 1, y, color);
    lvds_hstx_line(x, y + h - 1, x + w - 1, y + h - 1, color);
    lvds_hstx_line(x, y, x, y + h - 1, color);
    lvds_hstx_line(x + w - 1, y, x + w - 1, y + h - 1, color);
}

void lvds_hstx_fill_rect(int x, int y, int w, int h, lvds_color_t color)
{
    int x2 = x + w;
    int y2 = y + h;
    uint8_t c = (uint8_t)(color & 0x0f);
    uint8_t packed = (uint8_t)((c << 4) | c);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > LVDS_HSTX_WIDTH) x2 = LVDS_HSTX_WIDTH;
    if (y2 > LVDS_HSTX_HEIGHT) y2 = LVDS_HSTX_HEIGHT;
    if (x >= x2 || y >= y2) return;

    for (int yy = y; yy < y2; yy++) {
        uint32_t row = (uint32_t)yy * (LVDS_HSTX_WIDTH / 2);
        int left = x;
        int right = x2;

        if (left & 1) {
            uint32_t byte_index = row + ((uint32_t)left >> 1);
            g_draw_buffer[byte_index] = (uint8_t)((g_draw_buffer[byte_index] & 0x0f) | (c << 4));
            left++;
        }

        if (right & 1) {
            right--;
            uint32_t byte_index = row + ((uint32_t)right >> 1);
            g_draw_buffer[byte_index] = (uint8_t)((g_draw_buffer[byte_index] & 0xf0) | c);
        }

        if (left < right) {
            memset(g_draw_buffer + row + ((uint32_t)left >> 1), packed, (size_t)(right - left) >> 1);
        }
    }
    g_dirty = true;
}

void lvds_hstx_ellipse(int x, int y, int rx, int ry, lvds_color_t color)
{
    if (rx <= 0 || ry <= 0) return;
    for (int a = 0; a < 360; a += 2) {
        float rad = (float)a * 0.01745329252f;
        lvds_hstx_pixel(x + (int)(rx * cosf(rad)), y + (int)(ry * sinf(rad)), color);
    }
}

void lvds_hstx_fill_ellipse(int x, int y, int rx, int ry, lvds_color_t color)
{
    if (rx <= 0 || ry <= 0) return;
    for (int yy = -ry; yy <= ry; yy++) {
        int span = (int)(rx * sqrtf(1.0f - ((float)(yy * yy) / (float)(ry * ry))));
        lvds_hstx_line(x - span, y + yy, x + span, y + yy, color);
    }
}

static int font_scale(int font)
{
    return font == LVDS_FONT_LARGE ? 2 : 1;
}

static int font_char_w(int font)
{
    return font == LVDS_FONT_SMALL ? 6 : 8;
}

static int font_char_h(int font)
{
    return font == LVDS_FONT_SMALL ? 8 : 14;
}

static const uint8_t *font_bitmap(int font)
{
    return font == LVDS_FONT_SMALL ? FontCond6x8 : FontIbm8x14;
}

static void draw_char_scaled(int x, int y, unsigned char ch, lvds_color_t fg, lvds_color_t bg, int font_id)
{
    int scale = font_scale(font_id);
    int cw = font_char_w(font_id);
    int chh = font_char_h(font_id);
    const uint8_t *bits = font_bitmap(font_id);

    lvds_hstx_fill_rect(x, y, cw * scale, chh * scale, bg);
    for (int row = 0; row < chh; row++) {
        uint8_t line = bits[row * 256 + ch];
        for (int col = 0; col < cw; col++) {
            if (line & 0x80u) {
                lvds_hstx_fill_rect(x + col * scale, y + row * scale, scale, scale, fg);
            }
            line <<= 1;
        }
    }
}

void lvds_hstx_text(int x, int y, const char *text, lvds_color_t fg, lvds_color_t bg, int font_id)
{
    int step = font_char_w(font_id) * font_scale(font_id);
    if (!text) return;
    while (*text) {
        draw_char_scaled(x, y, (unsigned char)*text++, fg, bg, font_id);
        x += step;
    }
}

int lvds_hstx_text_width(const char *text, int font_id)
{
    if (!text) return 0;
    return (int)strlen(text) * font_char_w(font_id) * font_scale(font_id);
}

void lvds_hstx_present(void)
{
    if (g_backbuffer_active && !g_direct_scanout) {
        memcpy(g_framebuffer, g_draw_buffer, LVDS_HSTX_FB_SIZE);
        g_dirty = false;
    }
    (void)g_display_started;
}

lvds_color_t lvds_hstx_rgb565(uint16_t rgb565)
{
    return palette_lookup(rgb565);
}

lvds_color_t lvds_hstx_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return palette_lookup(rgb_to_565(r, g, b));
}
