#ifndef NCC_BOOT_H
#define NCC_BOOT_H

#include <stdbool.h>

/* The picture the panel shows while it starts.

   It is the picture's own colours for as long as it stands - the splash takes
   sixteen slots of the renderer's palette for itself (`lvds_hstx_rgb()`) and
   gives them back when it lets go (`lvds_hstx_palette_reset()`), so the panel
   that follows is drawn with the panel's own sixteen and nothing about its
   colours changes. `ncc_boot_image.c` is the picture, generated from
   `assets/logo.bmp` by `tools/boot_image.py`.

   It carries the build's own stamp - the date and time the image was compiled -
   so the glass always says which image is in it. That is not decoration: it is
   how a flash that went to the wrong build got caught (2026-09-27, a stale
   build directory kept re-sending an old image while the source said
   otherwise). */

/* Take the picture's colours and raise it. Called once, at module init. */
void ncc_boot_start(void);

/* How long it stands, milliseconds. The operator who has read it once does not
   wait for it again: a key ends it sooner (`ncc_visual_key()`). The bench's own
   number, in one place so the module and its check cannot disagree. */
#define NCC_BOOT_HOLD_MS 10000u

/* The picture is up: the screen a caller draws is this one instead of the work
   screen. */
bool ncc_boot_active(void);

/* Advance the splash's own clock, with the elapsed milliseconds. */
void ncc_boot_tick(unsigned ms);

/* Draw it over the whole panel. */
void ncc_boot_draw(void);

/* Let go: the picture's palette goes back to the renderer and the work screen
   takes over. Whatever asked for it - the clock, or the operator's first key -
   calls this once. */
void ncc_boot_release(void);

#endif
