#ifndef LVDS_HOST_H
#define LVDS_HOST_H

/* Host replacement for the RP2350 LVDS/HSTX pixel backend.

   The NC screen code (modules/nc/nc_visual.c) draws through the same LVDS
   primitive API on both targets, so the desktop build renders the exact
   firmware layout: identical coordinates, palette and bitmap fonts. Only the
   pixel destination changes (GDI window or a BMP dump). */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Attach a window so lvds_hstx_present() repaints it. NULL keeps the frame in
   memory only (headless dump mode). */
void lvds_host_attach_window(void *hwnd);

/* 32bpp BGRA frame, LVDS_HOST_WIDTH x LVDS_HOST_HEIGHT, for GDI blitting. */
const void *lvds_host_pixels(void);
int lvds_host_stride(void);
int lvds_host_width(void);
int lvds_host_height(void);

/* Write the current frame as a 24-bit BMP. Used by the headless smoke test. */
bool lvds_host_save_bmp(const char *path);

#ifdef __cplusplus
}
#endif

#endif
