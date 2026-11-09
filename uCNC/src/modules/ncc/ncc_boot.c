#include "ncc_boot.h"

#include "ncc_boot_image.h"
#include "ncc_layout.h"

#include "../../cnc.h"
#include "../../interface/grbl_stream.h"
#include "../lvds_renderer/lvds_draw_api.h"
#include "../lvds_renderer/lvds_hstx.h"

#include <stdio.h>

/* The picture's colours, as the renderer gave them to us: one palette value per
   colour of the asset. */
static lvds_color_t g_ncc_boot_colour[NCC_BOOT_IMAGE_COLOURS];
static lvds_color_t g_ncc_boot_stamp_fg;
static lvds_color_t g_ncc_boot_stamp_bg;
static unsigned g_ncc_boot_left_ms;
static unsigned g_ncc_boot_say_ms;
static bool g_ncc_boot_active;

/* The line the machine says while the picture stands: the build's own stamp,
   where a host on the serial can read it. It is repeated once a second because
   the USB serial only carries what somebody is listening for - a flash that
   went to the wrong image has to be visible without eyes on the glass
   (2026-09-27: two flashes went nowhere before this). */
static void ncc_boot_say(void)
{
    grbl_stream_printf("[MSG:NCC boot %s %s, picture %dx%d, hold %u ms, keys "
                       "%d]\r\n", __DATE__, __TIME__, NCC_BOOT_IMAGE_W,
                       NCC_BOOT_IMAGE_H, (unsigned)NCC_BOOT_HOLD_MS,
                       (int)NCC_PAD_KEY);
}

void ncc_boot_start(void)
{
    size_t i;

    for (i = 0u; i < NCC_BOOT_IMAGE_COLOURS; i++) {
        g_ncc_boot_colour[i] = lvds_hstx_rgb(ncc_boot_image_colours[i].r,
                                             ncc_boot_image_colours[i].g,
                                             ncc_boot_image_colours[i].b);
    }
    /* The stamp reads on the picture's own background, in whichever of its
       colours is closest to white: the picture is the palette while it is up,
       so there is nothing else to draw with. */
    {
        const ncc_boot_colour_t *bg = &ncc_boot_image_colours[NCC_BOOT_IMAGE_BG];
        int best = 0;
        int best_d = -1;

        for (i = 0u; i < NCC_BOOT_IMAGE_COLOURS; i++) {
            const ncc_boot_colour_t *c = &ncc_boot_image_colours[i];
            int d = (int)c->r + (int)c->g + (int)c->b;

            (void)bg;
            if (d > best_d) {
                best_d = d;
                best = (int)i;
            }
        }
        g_ncc_boot_stamp_fg = g_ncc_boot_colour[best];
        g_ncc_boot_stamp_bg = g_ncc_boot_colour[NCC_BOOT_IMAGE_BG];
    }
    g_ncc_boot_left_ms = NCC_BOOT_HOLD_MS;
    g_ncc_boot_say_ms = 0u;
    g_ncc_boot_active = true;
    ncc_boot_say();
}

bool ncc_boot_active(void)
{
    return g_ncc_boot_active;
}

void ncc_boot_release(void)
{
    if (!g_ncc_boot_active) {
        return;
    }
    g_ncc_boot_active = false;
    /* The panel's own colours are taken again as it draws; without this the
       picture's sixteen would still be what the palette holds. */
    lvds_hstx_palette_reset();
}

void ncc_boot_tick(unsigned ms)
{
    if (!g_ncc_boot_active) {
        return;
    }
    g_ncc_boot_say_ms += ms;
    if (g_ncc_boot_say_ms >= 1000u) {
        g_ncc_boot_say_ms = 0u;
        ncc_boot_say();
    }
    if (ms >= g_ncc_boot_left_ms) {
        ncc_boot_release();
        return;
    }
    g_ncc_boot_left_ms -= ms;
}

void ncc_boot_draw(void)
{
    uint32_t at = 0u;
    int row;

    lvds_hstx_clear(g_ncc_boot_colour[NCC_BOOT_IMAGE_BG]);
    for (row = 0; row < NCC_BOOT_IMAGE_H; row++) {
        int x = NCC_BOOT_IMAGE_X;

        while (at < ncc_boot_image_run_count &&
               x < NCC_BOOT_IMAGE_X + NCC_BOOT_IMAGE_W) {
            uint8_t index = (uint8_t)ncc_boot_image_runs[at * 2u];
            uint16_t run = ncc_boot_image_runs[at * 2u + 1u];

            if (index < NCC_BOOT_IMAGE_COLOURS) {
                lvds_hstx_fill_rect(x, NCC_BOOT_IMAGE_Y + row, (int)run, 1,
                                    g_ncc_boot_colour[index]);
            }
            x += (int)run;
            at++;
        }
    }
    /* The build's own stamp, under the picture where nothing it says can be
       mistaken for part of the drawing. */
    {
        char line[48];
        int width;
        int x;
        int y = NCC_BOOT_IMAGE_Y + NCC_BOOT_IMAGE_H + 28;

        /* The key size is on the stamp as well as the build: the bench asked
           for it twice and the glass could not say (2026-09-27 - "buttons are
           still same old size at machine"). Now it says. */
        snprintf(line, sizeof(line), "uCNC ncc  %s %s  keys %d",
                 __DATE__, __TIME__, (int)NCC_PAD_KEY);
        width = lvds_hstx_text_width(line, LVDS_FONT_NORMAL);
        x = (LVDS_VIEW_WIDTH - width) / 2;
        lvds_hstx_fill_rect(x - 8, y - 5, width + 16, 24,
                            g_ncc_boot_stamp_bg);
        lvds_hstx_text(x, y, line, g_ncc_boot_stamp_fg, g_ncc_boot_stamp_bg,
                       LVDS_FONT_NORMAL);
    }
}
