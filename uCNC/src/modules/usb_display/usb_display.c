/**
 * DisplayLink scanout backend for uCNC.
 *
 * This is the HSTX/LVDS backend's replacement, and it keeps the same promise:
 * the screens draw bounded primitives into an indexed draw buffer and never
 * touch the wire. What changes is where the frame is scanned out of.
 *
 * - The draw buffer is the 4 bpp panel image it always was (one nibble per
 *   pixel, LVDS_PANEL_* applies the turn as the pixel is written), held in
 *   PSRAM.
 * - The glass is a DisplayLink adapter whose own memory holds two 16 bpp banks
 *   of that image. The adapter scans them out; nothing on the RP2350 has to
 *   keep a frame's worth of pixels in SRAM, and no DMA ring exists.
 * - Core0 draws and calls lvds_hstx_present(), which only publishes the
 *   rectangle that changed and returns. It never waits for USB.
 * - Core1 owns TinyUSB, libdlo and the endpoint: it expands the published
 *   rectangle, writes it into the back bank, flips the plane base, and writes
 *   the same rectangle into the new back bank so the two stay identical.
 *
 * Why rectangles: the wire costs about one command per row of everything, so a
 * full repaint is background work while a read-out-sized rectangle is tens of
 * milliseconds. See docs/DEVICE-MEMORY.md in the TVS UM80 bring-up for the
 * measurements behind that (device memory window, command rate, RLE ratio).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "../../cnc.h"
#include "../lvds_renderer/lvds_hstx.h"

#include <stddef.h>
#include <string.h>

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "tusb.h"

#include "libdlo/src/dlo_experiment.h"
#include "libdlo/src/dlo_usb.h"
#include "libdlo/src/libdlo.h"
#include "../lvds_renderer/lvds_fonts.h"
#include "../lvds_renderer/lvds_palette.h"
#include "../lvds_renderer/lvds_psram.h"

/* The draw buffer is the panel image in 4 bpp: LVDS_HSTX_WIDTH x
 * LVDS_HSTX_HEIGHT pixels, two to a byte. */
#define LVDS_HSTX_FB_SIZE (LVDS_HSTX_WIDTH * LVDS_HSTX_HEIGHT / 2)
#define LVDS_HSTX_COORD_GUARD 4096
#define LVDS_HSTX_TEXT_GUARD 256

/* The adapter's memory, in the 16 MB the link can name. Both banks hold the
 * whole 800x600 frame at 16 bpp (960,000 bytes), and each is written by
 * 16 bpp writes only, so a plain base is enough. */
#define DLO_FRAME_BYTES (LVDS_HSTX_WIDTH * LVDS_HSTX_HEIGHT * 2u)
#define DLO_BANK_A 0x00100000u
#define DLO_BANK_B (DLO_BANK_A + DLO_FRAME_BYTES)

/* A 4 bpp copy of what has already gone out, in PSRAM well past the draw
 * buffer and the preview masks. Core0 owns it: comparing the draw buffer
 * against it turns "the screen redrew itself" into "these pixels are different",
 * which is the difference between one rectangle and a whole frame on the wire.
 * A screen that has not changed costs nothing at all. */
#define DLO_SENT_OFFSET (4u * 1024u * 1024u)

#ifndef LVDS_HSTX_CORE1_STACK_WORDS
#define LVDS_HSTX_CORE1_STACK_WORDS 4096u
#endif

/* One published rectangle of the draw buffer, in panel coordinates. */
typedef struct {
    int16_t x0;
    int16_t y0;
    int16_t x1;   /* inclusive */
    int16_t y1;   /* inclusive */
} dlo_dirty_t;

typedef struct {
    uint32_t pad;
    uint32_t data[LVDS_HSTX_CORE1_STACK_WORDS];
} dlo_stack_block_t;

static dlo_stack_block_t g_core1_stack_block __attribute__((aligned(64)));

static uint8_t *g_draw_buffer;
static uint16_t g_palette_rgb565[16];
static bool g_palette_used[16];
static uint8_t g_palette_next;

/* Dirty rectangles: core0 accumulates into g_accumulate and publishes it into
 * the slot core1 is not using. Nothing blocks; a rectangle that arrives while
 * core1 is busy is merged into the next one. */
static dlo_dirty_t g_accumulate;
static dlo_dirty_t g_slot[2];
static volatile uint32_t g_published;   /* seq of the slot core1 may take */
static volatile uint32_t g_consumed;    /* seq core1 has finished with */
static volatile bool g_frame_whole;     /* force a full frame (init, clear) */

/* Core1's own report, for the console. */
static volatile bool g_panel_ready;
static volatile uint32_t g_panel_width;
static volatile uint32_t g_panel_height;
static volatile uint32_t g_frames_done;
static volatile uint32_t g_last_rows;

static uint16_t g_line[LVDS_HSTX_WIDTH];

static uint8_t *g_sent;

/* The rectangle bookkeeping is defined below, next to the state it touches. */
static void dirty_reset(dlo_dirty_t *r);
static void dirty_add(dlo_dirty_t *r, int x0, int y0, int x1, int y1);
static bool dirty_empty(const dlo_dirty_t *r);
static void dirty_union(dlo_dirty_t *dst, const dlo_dirty_t *src);

/** Bounding box of the pixels where the draw buffer differs from what has gone
 *  out, or an empty rectangle when the two are identical. */
static dlo_dirty_t diff_against_sent(void)
{
    dlo_dirty_t rect;
    uint32_t row_bytes = LVDS_HSTX_WIDTH / 2u;

    dirty_reset(&rect);

    if (!g_sent) {
        return rect;
    }

    for (uint32_t row = 0; row < LVDS_HSTX_HEIGHT; row++) {
        const uint8_t *now = g_draw_buffer + (row * row_bytes);
        const uint8_t *sent = g_sent + (row * row_bytes);
        uint32_t i;

        for (i = 0; i < row_bytes; i++) {
            if (now[i] == sent[i]) {
                continue;
            }
            /* One byte is two pixels; a change in either marks both. */
            dirty_add(&rect, (int)(i * 2u), (int)row, (int)(i * 2u) + 1, (int)row);
        }
    }

    return rect;
}

/** Remember that the pixels of @a r have gone out, so the next comparison
 *  starts from them. */
static void mark_sent(const dlo_dirty_t *r)
{
    uint32_t row_bytes = LVDS_HSTX_WIDTH / 2u;
    uint32_t first_byte = (uint32_t)r->x0 >> 1;
    uint32_t last_byte = ((uint32_t)r->x1 >> 1) + 1u;

    if (!g_sent) {
        return;
    }
    if (last_byte > row_bytes) {
        last_byte = row_bytes;
    }

    for (int y = r->y0; y <= r->y1; y++) {
        uint32_t row = (uint32_t)y * row_bytes;

        memcpy(g_sent + row + first_byte, g_draw_buffer + row + first_byte,
               last_byte - first_byte);
    }
}

static void dirty_union(dlo_dirty_t *dst, const dlo_dirty_t *src)
{
    if (dirty_empty(src)) {
        return;
    }
    dirty_add(dst, src->x0, src->y0, src->x1, src->y1);
}

static void dirty_reset(dlo_dirty_t *r)
{
    r->x0 = 0x7fff;
    r->y0 = 0x7fff;
    r->x1 = -1;
    r->y1 = -1;
}

static void dirty_add(dlo_dirty_t *r, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)LVDS_HSTX_WIDTH - 1) x1 = (int)LVDS_HSTX_WIDTH - 1;
    if (y1 > (int)LVDS_HSTX_HEIGHT - 1) y1 = (int)LVDS_HSTX_HEIGHT - 1;
    if (x1 < x0 || y1 < y0) {
        return;
    }
    if (x0 < r->x0) r->x0 = (int16_t)x0;
    if (y0 < r->y0) r->y0 = (int16_t)y0;
    if (x1 > r->x1) r->x1 = (int16_t)x1;
    if (y1 > r->y1) r->y1 = (int16_t)y1;
}

static bool dirty_empty(const dlo_dirty_t *r)
{
    return r->x1 < r->x0 || r->y1 < r->y0;
}

/* --- palette ------------------------------------------------------------ */

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
    uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
    uint8_t g = (uint8_t)((g6 << 2) | (g6 >> 4));
    uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void palette_set(uint8_t index, uint16_t rgb565)
{
    if (index >= 16u) {
        return;
    }
    g_palette_rgb565[index] = rgb565;
    g_palette_used[index] = true;
}

/** Take the sixteen slots again: the next colour asked for takes the first of
 *  them. The boot picture brings colours of its own and hands them back. */
void lvds_hstx_palette_reset(void)
{
    memset(g_palette_used, 0, sizeof(g_palette_used));
    memset(g_palette_rgb565, 0, sizeof(g_palette_rgb565));
    g_palette_next = 0;
}

static void palette_reset(void)
{
    lvds_hstx_palette_reset();
}

static lvds_color_t palette_lookup(uint16_t rgb565)
{
    for (uint8_t i = 0; i < 16u; i++) {
        if (g_palette_used[i] && g_palette_rgb565[i] == rgb565) {
            return (lvds_color_t)i;
        }
    }
    if (g_palette_next >= 16u) {
        return (lvds_color_t)(15u);
    }
    palette_set(g_palette_next, rgb565);
    return (lvds_color_t)g_palette_next++;
}

lvds_color_t lvds_hstx_rgb565(uint16_t rgb565)
{
    return palette_lookup(rgb565);
}

lvds_color_t lvds_hstx_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return palette_lookup(rgb_to_565(r, g, b));
}

/* --- the drawing surface (core0) ---------------------------------------- */

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
    dirty_add(&g_accumulate, x, y, x, y);
}

static bool hstx_ready_for_draw(void)
{
    return g_draw_buffer != NULL;
}

static bool hstx_coord_sane(int v)
{
    return v >= -LVDS_HSTX_COORD_GUARD && v <= LVDS_HSTX_COORD_GUARD;
}

static bool hstx_rect_sane(int x, int y, int w, int h)
{
    if (!hstx_ready_for_draw() || w <= 0 || h <= 0)
        return false;
    if (!hstx_coord_sane(x) || !hstx_coord_sane(y) ||
        !hstx_coord_sane(w) || !hstx_coord_sane(h))
        return false;
    /* The screens draw the view, so it is the view's bounds that decide; the
       turn onto the glass happens once, below. */
    return x < LVDS_VIEW_WIDTH && y < LVDS_VIEW_HEIGHT &&
           x + w > 0 && y + h > 0;
}

void lvds_hstx_clear(lvds_color_t color)
{
    uint8_t c = (uint8_t)(color & 0x0f);

    if (!hstx_ready_for_draw()) {
        return;
    }
    memset(g_draw_buffer, (int)((c << 4) | c), LVDS_HSTX_FB_SIZE);
    memset(&g_accumulate, 0, sizeof(g_accumulate));
    dirty_reset(&g_accumulate);
    g_frame_whole = true;
}

/* --- publishing, and what core1 does with it ---------------------------- */

void lvds_hstx_present(void)
{
    uint32_t consumed;

    if (!hstx_ready_for_draw()) {
        return;
    }

    /* What has changed since the last thing that went out. A screen that
     * redrew itself without changing anything costs one comparison and puts
     * nothing on the wire. */
    if (g_frame_whole) {
        dirty_add(&g_accumulate, 0, 0, (int)LVDS_HSTX_WIDTH - 1, (int)LVDS_HSTX_HEIGHT - 1);
        g_frame_whole = false;
    } else {
        dlo_dirty_t changed = diff_against_sent();

        dirty_union(&g_accumulate, &changed);
    }

    consumed = g_consumed;

    /* Core1 is still working through the last rectangle, or there is nothing
     * new: either way nothing waits, and nothing is lost - whatever accumulates
     * travels with the next rectangle that does go out. */
    if (consumed != g_published || dirty_empty(&g_accumulate)) {
        return;
    }

    g_slot[consumed & 1u] = g_accumulate;
    mark_sent(&g_accumulate);
    dirty_reset(&g_accumulate);
    g_published = consumed + 1u;
}

/** Expand one panel row of the published rectangle into 16 bpp. */
static void expand_row(dlo_dirty_t *r, int y, const dlo_dev_t uid, uint32_t base)
{
    int x0 = r->x0;
    int width = r->x1 - r->x0 + 1;
    uint32_t p;

    p = (uint32_t)y * LVDS_HSTX_WIDTH + (uint32_t)x0;

    for (int i = 0; i < width; i++) {
        uint32_t pixel = p + (uint32_t)i;
        uint8_t byte = g_draw_buffer[pixel >> 1];

        g_line[i] = g_palette_rgb565[(pixel & 1u) ? (byte >> 4) : (byte & 0x0f)];
    }

    (void)dlo_exp_write_16bpp(uid, base + (p * 2u), g_line, (uint32_t)width, true);
}

/** Write one published rectangle into one bank. Returns the rows written. */
static uint32_t write_rect(const dlo_dev_t uid, uint32_t bank, dlo_dirty_t *r)
{
    uint32_t rows = 0;

    for (int y = r->y0; y <= r->y1; y++) {
        expand_row(r, y, uid, bank);
        rows++;
    }
    return rows;
}

static bool pump_until_idle(const dlo_dev_t uid, uint32_t timeout_ms)
{
    uint32_t start = to_ms_since_boot(get_absolute_time());

    while (dlo_usb_endpoint_busy(uid)) {
        tuh_task();
        if ((uint32_t)(to_ms_since_boot(get_absolute_time()) - start) > timeout_ms) {
            return false;
        }
    }
    return true;
}

/* The device address TinyUSB reported, if any: the claim needs it, and the
 * mount callback is the only place it appears. */
static volatile uint8_t g_mount_addr;
static volatile bool g_mounted;

void tuh_mount_cb(uint8_t daddr)
{
    g_mount_addr = daddr;
    g_mounted = true;
}

void tuh_umount_cb(uint8_t daddr)
{
    (void)daddr;
    g_mounted = false;
}

/** Claim the monitor, program the mode, and point the scanout at our bank. */
static bool panel_bring_up(dlo_dev_t *out_uid)
{
    dlo_init_t init_flags = { 0 };
    dlo_claim_t claim_flags = { 0 };
    dlo_mode_t requested = { 0 };
    dlo_dev_t uid;
    dlo_mode_t *mode;

    if (dlo_init(init_flags) != dlo_ok) {
        return false;
    }
    if (dlo_check_device(g_mount_addr) != dlo_ok) {
        return false;
    }

    uid = dlo_claim_first_device(claim_flags, 0);
    if (!uid) {
        return false;
    }

    /* The panel's own mode first - 800x600 at this build's scanout depth, the
     * TVS UM80's preferred timing - then whatever the EDID asks for if that is
     * refused. */
    requested.view.width = LVDS_HSTX_WIDTH;
    requested.view.height = LVDS_HSTX_HEIGHT;
    requested.view.bpp = DLO_VIEW_BPP;
    requested.refresh = 60;
    if (dlo_set_mode(uid, &requested) != dlo_ok && dlo_set_mode(uid, 0) != dlo_ok) {
        return false;
    }

    mode = dlo_get_mode(uid);
    g_panel_width = mode ? mode->view.width : 0;
    g_panel_height = mode ? mode->view.height : 0;

    /* The glass scans out of the adapter's own memory, so both banks sit past
     * the frame libdlo's own modeset used, and by the time the first frame is
     * written the visible base is the bank that was just completed. */
    if (dlo_exp_set_bases(uid, DLO_BANK_A, 0) != dlo_ok) {
        return false;
    }

    *out_uid = uid;
    return true;
}

static void core1_entry(void)
{
    dlo_dev_t uid = 0;
    uint32_t bank[2];
    uint32_t visible = 0;
    uint32_t last_seq = 0;
    bool claimed = false;

    /* The adapter is claimed on this core and never touched from the other:
     * TinyUSB host state is not shared. */
    tusb_rhport_init_t rh_init = { .role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_FULL };

    tusb_init(BOARD_TUH_RHPORT, &rh_init);

    for (;;) {
        tuh_task();

        if (!claimed) {
            if (!g_mounted) {
                sleep_us(500);
                continue;
            }
            if (!panel_bring_up(&uid)) {
                sleep_ms(200);
                continue;
            }
            bank[0] = DLO_BANK_A;
            bank[1] = DLO_BANK_B;
            visible = 0;
            claimed = true;

            /* The glass is scanning a bank nobody has written yet - that is the
             * snow a freshly programmed adapter shows. Put the whole draw
             * buffer into the back bank, flip to it, then make the other bank
             * identical, before a single rectangle is served. */
            {
                dlo_dirty_t whole;

                whole.x0 = 0;
                whole.y0 = 0;
                whole.x1 = (int16_t)(LVDS_HSTX_WIDTH - 1);
                whole.y1 = (int16_t)(LVDS_HSTX_HEIGHT - 1);

                (void)write_rect(uid, bank[visible ^ 1u], &whole);
                (void)dlo_flush_usb(uid, true);
                (void)pump_until_idle(uid, 5000);

                (void)dlo_exp_set_bases(uid, bank[visible ^ 1u], 0);
                (void)dlo_flush_usb(uid, true);
                visible ^= 1u;

                (void)write_rect(uid, bank[visible ^ 1u], &whole);
                (void)dlo_flush_usb(uid, true);
                (void)pump_until_idle(uid, 5000);
            }

            /* Whatever the screens published while the adapter was enumerating
             * is part of the frame that just went out. */
            g_consumed = g_published;
            last_seq = g_published;
            g_panel_ready = true;
        } else if (!g_mounted) {
            /* Unplugged: drop the claim and wait for the monitor to come back. */
            dlo_final_t final_flags = { 0 };

            dlo_release_device(uid);
            dlo_final(final_flags);
            g_panel_ready = false;
            claimed = false;
            continue;
        }

        /* Take a published rectangle, if there is one. */
        if (g_published != last_seq) {
            uint32_t seq = g_published;
            dlo_dirty_t rect = g_slot[(seq - 1u) & 1u];
            uint32_t rows;

            g_consumed = seq;
            last_seq = seq;

            rows = write_rect(uid, bank[visible ^ 1u], &rect);
            (void)dlo_flush_usb(uid, true);
            (void)pump_until_idle(uid, 3000);

            /* Show the bank that is now complete. */
            (void)dlo_exp_set_bases(uid, bank[visible ^ 1u], 0);
            (void)dlo_flush_usb(uid, true);
            visible ^= 1u;

            /* Keep the other bank identical, so the next flip shows the same
             * picture plus whatever comes next. It is not visible, so this
             * needs no wait. */
            (void)write_rect(uid, bank[visible ^ 1u], &rect);
            (void)dlo_flush_usb(uid, true);

            g_last_rows = rows;
            g_frames_done++;
        } else {
            tuh_task();
            sleep_us(200);
        }
    }
}

bool lvds_hstx_init(void)
{
    palette_reset();
    dirty_reset(&g_accumulate);
    dirty_reset(&g_slot[0]);
    dirty_reset(&g_slot[1]);
    g_published = 0;
    g_consumed = 0;
    g_frames_done = 0;
    g_panel_ready = false;

    if (!lvds_psram_available()) {
        return false;
    }

    g_draw_buffer = (uint8_t *)lvds_psram_ptr(0);
    if (!g_draw_buffer) {
        return false;
    }
    memset(g_draw_buffer, 0, LVDS_HSTX_FB_SIZE);

    /* Nothing has gone out yet: filling the "sent" copy with ones makes the
     * first comparison find the whole frame different, so the first thing the
     * adapter receives is a complete picture. */
    g_sent = (uint8_t *)lvds_psram_ptr(DLO_SENT_OFFSET);
    if (g_sent) {
        memset(g_sent, 0xff, LVDS_HSTX_FB_SIZE);
    }
    g_frame_whole = true;

    /* Core1 owns the wire from here. This call does not wait for the monitor to
     * be claimed: the screens draw while the adapter enumerates, and the first
     * frame goes out when it is ready. */
    multicore_reset_core1();
    multicore_launch_core1_with_stack(core1_entry, g_core1_stack_block.data,
                                      sizeof(g_core1_stack_block.data));
    return true;
}

bool lvds_hstx_panel_ready(void)
{
    return g_panel_ready;
}

uint32_t lvds_hstx_frames(void)
{
    return g_frames_done;
}

uint32_t lvds_hstx_last_rows(void)
{
    return g_last_rows;
}
void lvds_hstx_pixel(int x, int y, lvds_color_t color)
{
    if (!hstx_ready_for_draw()) {
        return;
    }
    /* The view's point, on the glass: the panel is mounted turned, and the
       picture is turned back (see `lvds_hstx.h`). */
    put_pixel_raw(LVDS_PANEL_X(x, y), LVDS_PANEL_Y(x, y), (uint8_t)color);
}

void lvds_hstx_line(int x1, int y1, int x2, int y2, lvds_color_t color)
{
    int dx = abs(x2 - x1);
    int sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1);
    int sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    if (!hstx_ready_for_draw() ||
        !hstx_coord_sane(x1) || !hstx_coord_sane(y1) ||
        !hstx_coord_sane(x2) || !hstx_coord_sane(y2)) {
        return;
    }
    if ((x1 < 0 && x2 < 0) || (x1 >= LVDS_VIEW_WIDTH && x2 >= LVDS_VIEW_WIDTH) ||
        (y1 < 0 && y2 < 0) || (y1 >= LVDS_VIEW_HEIGHT && y2 >= LVDS_VIEW_HEIGHT)) {
        return;
    }
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
    if (thick <= 0 || thick > 16) {
        return;
    }
    for (int oy = -r; oy <= r; oy++) {
        for (int ox = -r; ox <= r; ox++) {
            lvds_hstx_line(x1 + ox, y1 + oy, x2 + ox, y2 + oy, color);
        }
    }
}

void lvds_hstx_rect(int x, int y, int w, int h, lvds_color_t color)
{
    if (!hstx_rect_sane(x, y, w, h)) {
        return;
    }
    lvds_hstx_line(x, y, x + w - 1, y, color);
    lvds_hstx_line(x, y + h - 1, x + w - 1, y + h - 1, color);
    lvds_hstx_line(x, y, x, y + h - 1, color);
    lvds_hstx_line(x + w - 1, y, x + w - 1, y + h - 1, color);
}

void lvds_hstx_fill_rect(int x, int y, int w, int h, lvds_color_t color)
{
    int x2;
    int y2;
    uint8_t c = (uint8_t)(color & 0x0f);
    uint8_t packed = (uint8_t)((c << 4) | c);
    if (!hstx_rect_sane(x, y, w, h)) {
        return;
    }
    /* A filled rectangle is still a rectangle after a quarter turn, so the
       whole fill turns with two numbers and one swap - a screen clear is one
       call, not one pixel at a time. */
    {
        int px = LVDS_PANEL_RECT_X(x, y, w, h);
        int py = LVDS_PANEL_RECT_Y(x, y, w, h);
        int pw = LVDS_PANEL_RECT_W(w, h);
        int ph = LVDS_PANEL_RECT_H(w, h);

        x = px;
        y = py;
        w = pw;
        h = ph;
    }
    x2 = x + w;
    y2 = y + h;
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
    /* What was just written, in panel coordinates, is what core1 has to send. */
    dirty_add(&g_accumulate, x, y, x2 - 1, y2 - 1);
}

void lvds_hstx_ellipse(int x, int y, int rx, int ry, lvds_color_t color)
{
    if (!hstx_ready_for_draw() || rx <= 0 || ry <= 0 ||
        rx > LVDS_VIEW_WIDTH || ry > LVDS_VIEW_HEIGHT ||
        !hstx_coord_sane(x) || !hstx_coord_sane(y)) return;
    for (int a = 0; a < 360; a += 2) {
        float rad = (float)a * 0.01745329252f;
        lvds_hstx_pixel(x + (int)(rx * cosf(rad)), y + (int)(ry * sinf(rad)), color);
    }
}

void lvds_hstx_fill_ellipse(int x, int y, int rx, int ry, lvds_color_t color)
{
    if (!hstx_ready_for_draw() || rx <= 0 || ry <= 0 ||
        rx > LVDS_VIEW_WIDTH || ry > LVDS_VIEW_HEIGHT ||
        !hstx_coord_sane(x) || !hstx_coord_sane(y)) return;
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
    int guard = 0;
    if (!hstx_ready_for_draw() || !text || !hstx_coord_sane(x) || !hstx_coord_sane(y)) return;
    while (*text && guard++ < LVDS_HSTX_TEXT_GUARD) {
        draw_char_scaled(x, y, (unsigned char)*text++, fg, bg, font_id);
        x += step;
    }
}

int lvds_hstx_text_width(const char *text, int font_id)
{
    if (!text) return 0;
    return (int)strlen(text) * font_char_w(font_id) * font_scale(font_id);
}

