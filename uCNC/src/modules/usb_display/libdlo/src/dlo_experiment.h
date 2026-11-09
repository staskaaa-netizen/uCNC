/** @file dlo_experiment.h
 *
 *  @brief Plane-level access for the 8-bit framebuffer experiment.
 *
 *  DisplayLink DL-1x0/DL-1x5 parts have two framebuffers: a 16 bpp plane and an
 *  8 bpp plane. In the library's normal 24 bpp mode both are written for every
 *  pixel, and the 8 bpp plane supplies the low colour bits (see
 *  `register_8bpp` below and the Linux udlfb driver's comment "In 24bpp modes,
 *  the low 323 RGB bits go in the 8bpp framebuffer").
 *
 *  The experiment needs to drive the two planes *separately* to find out
 *  whether the 8 bpp plane can be scanned out on its own. These calls are thin
 *  wrappers over the library's own encoders and register writes - no new
 *  register semantics are introduced - so the tests can only do what libdlo
 *  already does, in combinations libdlo does not normally use.
 *
 *  Compiled only when DLO_EXPERIMENT_API is defined.
 *
 *  SPDX-License-Identifier: LGPL-2.0 (matches the libdlo sources)
 */

#ifndef DLO_EXPERIMENT_H
#define DLO_EXPERIMENT_H

#include "libdlo.h"

#if DLO_EXPERIMENT_API

/** Where the two planes of the current mode live, as the device was told.
 *
 *  @param  uid      Device.
 *  @param  base16   Receives the 16 bpp plane base (registers 0x20-0x22).
 *  @param  base8    Receives the 8 bpp plane base (registers 0x26-0x28).
 */
extern dlo_retcode_t dlo_exp_get_bases(const dlo_dev_t uid, uint32_t *base16, uint32_t *base8);

/** Move the plane base addresses, using the library's own set-base sequence
 *  (registers 0x20-0x22 and 0x26-0x28, plus the register batch framing).
 *
 *  @param  uid      Device.
 *  @param  base16   16 bpp plane base. Must be even.
 *  @param  base8    8 bpp plane base. Must be even. The library's 24 bpp layout
 *                   puts it at base16 + 2 * width * height; the Linux udlfb
 *                   driver instead parks it past the end of the framebuffer,
 *                   which is what the "8 bpp plane parked" experiment does.
 */
extern dlo_retcode_t dlo_exp_set_bases(const dlo_dev_t uid, uint32_t base16, uint32_t base8);

/** Write the scanout colour depth register (0x00) exactly as the library's own
 *  mode path does: batch lock, `af 20 00 depth`, batch unlock.
 *
 *  @param  uid    Device.
 *  @param  depth  0 = 16 bpp scanout (udlfb always uses this), 1 = 24 bpp
 *                 split-plane scanout (libdlo's DLO_VIEW_BPP 24 build). No
 *                 other value is documented anywhere in libdlo or udlfb, and
 *                 this call refuses anything else rather than guessing.
 */
extern dlo_retcode_t dlo_exp_set_color_depth(const dlo_dev_t uid, uint8_t depth);

/** Write pixels into the 16 bpp plane at @a base.
 *
 *  @param  uid     Device.
 *  @param  base    16 bpp plane address for the first pixel.
 *  @param  pixels  Big-endian RGB565 values, as the device expects.
 *  @param  count   Number of pixels (chunked internally to 256 per command).
 *  @param  rlx     true: use the library's mixed raw/run-length encoder, the
 *                  same one the working 16 bpp path uses. false: raw.
 */
extern dlo_retcode_t dlo_exp_write_16bpp(const dlo_dev_t uid, uint32_t base,
                                         const uint16_t *pixels, uint32_t count, bool rlx);

/** Write pixels into the 8 bpp plane at @a base.
 *
 *  @param  uid     Device.
 *  @param  base    8 bpp plane address for the first pixel.
 *  @param  pixels  Raw bytes, as the device expects (libdlo packs them as
 *                  3-3-2 low-order R,G,B bits in 24 bpp mode).
 *  @param  count   Number of pixels (chunked internally to 256 per command).
 *  @param  rle     true: use the library's run-length encoder (`af 61`).
 */
extern dlo_retcode_t dlo_exp_write_8bpp(const dlo_dev_t uid, uint32_t base,
                                        const uint8_t *pixels, uint32_t count, bool rle);

/** How libdlo packs a colour into the 8 bpp plane (3-3-2 low-order bits of
 *  R, G and B). Exposed so the experiment can fill the plane with values whose
 *  meaning is documented, rather than with arbitrary bytes. */
extern uint8_t dlo_exp_low_bits_8bpp(dlo_col32_t colour);

#endif /* DLO_EXPERIMENT_API */

#endif /* DLO_EXPERIMENT_H */
