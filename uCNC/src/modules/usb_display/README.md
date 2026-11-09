# USB (DisplayLink) display backend

The second display backend for uCNC, alongside `lvds_renderer`. Same machine,
same panels, same palette, fonts and draw API - the picture leaves the board
over USB to a DisplayLink adapter instead of over HSTX to an LVDS panel.

It is an experiment that works, kept because it is the right answer for small
screens and small updates - a read-out, a status page, a preview that changes in
patches. It is **not** a drop-in replacement for a 60 fps desktop: see
"What it costs" below.

## What was done

The port replaced the backend, not the UI:

- `usb_display.c` implements the same seam the HSTX backend does (`lvds_hstx_*`
  in `../lvds_renderer/lvds_hstx.h`), so a screen written for the LVDS glass
  drives this one unchanged. A build compiles **one** backend, never both - the
  environment here picks `usb_display` and leaves `lvds_hstx.c` out.
- The palette, the fonts, the draw API, the PSRAM buffers and the boot path are
  **not** copied: they are taken from `../lvds_renderer/`, which owns them. The
  only thing this module adds is where the pixels end up.
- The frame lives in the **adapter's memory**, not in SRAM: two 800x600 16 bpp
  banks at 1 MB and 2 MB of its address window. Core0 draws 4 bpp indices into
  the PSRAM buffer exactly as it did into the SRAM one; `lvds_hstx_present()`
  compares that buffer against a copy of what has already gone out and publishes
  only the bounding rectangle that changed.
- Core1 owns TinyUSB and libdlo: it claims the adapter, programs the mode,
  expands the published rectangle to RGB565, writes it into the back bank,
  flips the plane base, and then writes the same rectangle into the new back
  bank so the two stay identical. Core0 never waits for USB.
- No HSTX, no DMA descriptor ring, no line preparation, no CDC console: the
  native USB port is the display's host, so the console and the G-code protocol
  move to UART0 (GP0/GP1) - `boardmap_waveshare_pizero_usb.h`.

## Measured, on the bench (2026-10-16, TVS UM80, 800x600)

| What | Result |
| --- | --- |
| SRAM used by the firmware | **105 KB**, against 343 KB for the LVDS target |
| Why | the 240 KB SRAM scanout buffer, the active line buffers and the descriptor ring are gone |
| Whole-screen repaint | 0.8-2 s (960 KB of frame against a ~1.15 MB/s link) |
| Small update (a number, a bar) | tens of milliseconds - one rectangle |
| Unchanged screen | nothing on the wire at all |
| Adapter address space | 16 MB, wrapping: a write at 20 MB landed on 4 MB |

The wrapping number is the one to design against: an address above 16 MB does
not fail, it silently becomes a low address and overwrites whatever is there.
Both banks, the glyph atlas and the workpiece buffers have to stay under it.

## What it costs

The link is the limit, and it is not a fast one:

- about one command per *row* of anything, and roughly 1500-2000 commands/s;
- about 1.15 MB/s of bulk payload;
- so a whole frame is seconds, and only the changed rectangle is cheap.

For scale: the LVDS/HSTX machine this backend sits beside runs whole-screen video at 30-40 fps, and this one runs a whole frame at 0.5 fps. The ceilings are the panel's ~40 MHz pixel clock against a 12 Mbit/s bus - about one seventieth of the picture per second - so the gap is the wire, not the code.

What follows from that: screen switches and anything that clears and repaints
the whole panel are slow, by construction. A screen that merely changes a few
numbers is fast. If a screen repaints itself every frame, this backend cannot
help it - that is a screen-side change (hand the renderer the rectangle that
changed, rather than a fresh screen).

## Enabling it

The environment is `env.ini` beside this file, and `platformio.ini` lists it in
`extra_configs`, so the target exists and can be built by name. It is **not** in
`default_envs`: a bare `pio run` still builds the LVDS machine, and nothing in
this tree is built unless it is asked for:

```ini
extra_configs =
  uCNC/src/hal/boards/rp2350/rp2350.ini
  uCNC/src/modules/usb_display/env.ini
```

Then build it by name - `pio run -e RP2350-NCC-USB` - and flash
`.pio/build/RP2350-NCC-USB/firmware.uf2`. A bare `pio run` still builds the LVDS
machine, because `default_envs` is untouched.

Two build facts worth knowing, both of which cost time to find:

- **The Arduino-Pico framework links a prebuilt TinyUSB device stack**, and a
  prebuilt device stack cannot be turned into a host at link time. So this
  target compiles TinyUSB's host half from the SDK sources the framework already
  ships (`ucnc_usb_host.py`), with `tusb_config.h` force-included so both halves
  agree on one configuration.
- **The library is vendored**, not a `lib_deps` entry: `libdlo/` is the same
  tusb_libdlo the standalone `tvs_um80_rp2350` firmware uses, with its patches
  recorded in its `PATCHES.md`.

## What is not done

- The panel-side half: screens still clear and repaint, which is what forces
  whole frames. Reporting rectangles from the screens is the next real win.
- The seam is still called `lvds_hstx_*`. Renaming it to something neutral is a
  joint change across both backends, the panels and the desktop station, so it
  belongs to whoever does that, not to this module alone.
- No host-side test for the rectangle encoder: the backend is verified by
  building it and by looking at the glass.

## Validation status

Software-verified: the target builds (`RP2350-NCC-USB`), and the LVDS target it
shares the portable half with still builds unchanged.

Bench-only, in the operator's hands: the panel itself, the whole-frame and
small-update rates above, and readability. Those numbers came from one board on
one afternoon and are reported as such.

## What the port taught about dividing peripherals on a two-core machine

Written down because the same questions come back on every machine with two
cores and more peripherals than controllers.

1. **A peripheral controller has one owner, and ownership includes the init.**
   The first failure here was two owners: the Arduino core's USB device stack and
   this module's host stack both initialising the same controller. That is not a
   race to lock, it is an ownership question, and it gets answered at boot
   before anything else runs - in this target by removing `MCU_HAS_USB` from the
   board map, so the device side never exists.
2. **What one half takes, the other pays for.** Making the native port a host
   took the console with it, which forced the console and the G-code protocol
   onto UART0. A PIO USB host pays differently: a PIO block, a clock that is an
   exact multiple of 12 MHz, and CPU time for bit-banging. There is no free port;
   choose the currency deliberately.
3. **The interface between the cores is data, not calls.** Core0 does not call
   into the USB stack; it publishes a rectangle and returns. Core1 never asks
   core0 to wait. One producer, one consumer, a sequence counter, and one buffer
   each - which is why no lock appears anywhere.
4. **The slow side has to be allowed to fail alone.** If the video core falls
   behind, the picture degrades and motion does not notice. That is what makes
   "non blocking" something you can test: the fast side's worst case does not
   contain the slow side's latency.
5. **Move work to the idle core, not the work you dislike.** The old scanout
   copied 240 KB per frame on core0 and kept 240 KB of SRAM to do it. Putting the
   frame in the adapter's own memory removed both - 343 KB of RAM became 105 KB -
   and the unit of exchange between the two halves became a rectangle instead of
   a buffer.
6. **Resources the halves share are allocated once, in one place.** PIO blocks,
   DMA channels, IRQ lines and clocks: name an owner for each and write the
   conflicts down. An encoder that lives in PIO and a display that wants a PIO
   block is exactly such a conflict, and it is cheaper to find on paper.
7. **A rectangle is the useful unit of a display interface.** The wire carries
   about one command per row, so the interface that survives is "what changed",
   not "the picture" - whether the caller computes it or the backend does, as
   this one does.

Rewriting the comparison of the two DisplayLink implementations, and the
measurements both rest on, is in the `tvs_um80_rp2350` project:
`docs/DISPLAYLINK-APPROACHES.md`.