# Limits and Caveats

This is the list a consumer has to know before the renderer is used anywhere
other than the bench it was built on. Every item is measured or observed on the
RP2350/LVDS bench, not assumed; `README.md` carries the reasoning, the incidents
and the dates behind each one.

## One copy, three consumers

This directory is the renderer's only copy, and it is used three ways:

- **the uCNC module** - `custom_ucnc_modules` names `lvds_renderer`, the RP2350
  environments compile it with `+<src/modules/lvds_renderer/>`, and
  `LOAD_MODULE(...)` wires it in;
- **the desktop station's drawing core** - `tools/nc_ui_win` compiles
  `lvds_draw_api.c`, `lvds_palette.c` and the two fonts *unmodified* and supplies
  its own backend (`lvds_host.c`) implementing the same primitives over a GDI
  frame, so the bench and the machine draw one picture from one source;
- **a library** - `library.json` is the PlatformIO manifest for a project that
  is not uCNC. Point `lib_extra_dirs` at this directory (or add it as a
  submodule) and supply the panel's numbers yourself; there is no build step
  hidden in uCNC.

What is portable: `lvds_palette.*`, `lvds_draw_api.*` and `lvds_font_*` depend on
nothing but `lvds_hstx.h`. What is RP2350-only: `lvds_hstx.c` (HSTX, DMA, IRQs,
the PLL, core1), `lvds_psram.c` and `lvds_renderer_boot.c`. Those three also call
four uCNC entry points - `mcu_millis()`, `cnc_delay_ms()`, `proto_info()` and
`cnc_is_file_io_critical()` - in about a dozen places. **A non-uCNC consumer has
to give those four definitions itself** (a port header replacing them is not
written yet; that is the next step for the library, not a supported mode today).

## This panel, and only this panel

- Sharp **LQ121S1LG44**, 800x600, single-channel LVDS, driven by the RP2350's
  HSTX through a PicoLVDS-style 7x256 scanline LUT. Pins: GP12..GP17 data pairs,
  GP18/GP19 clock pair.
- The packing emits one pixel clock per **7** serial transfer phases, so the
  pixel clock is `LVDS_SYS_CLOCK_KHZ / 1000 / LVDS_HSTX_CLOCK_DIV * 2 / 7`. The
  panel's 40 MHz nominal is not what it runs at here: it locks below that, and
  the known points are `280000/2` (~40.00 MHz, the machine's setting),
  `370000/3` (~35.24), `270000/3` (~25.71), `370000/4` (~26.43),
  `270000/4` (~19.29) and `280000/3` (~26.67). `420000/3` produced **no video**
  and is a board/overclock boundary, not a panel requirement.
- Clock and architecture changes are not mixed: when testing clocks, change
  `LVDS_SYS_CLOCK_KHZ`, `LVDS_HSTX_PLL_KHZ` and `LVDS_HSTX_CLOCK_DIV` only.
- The glass is mounted **turned a quarter**, and the turn is stated once
  (`LVDS_PANEL_TURN`, 90 here). Screens draw a `LVDS_VIEW_WIDTH x
  LVDS_VIEW_HEIGHT` picture - 600x800 while turned - and `LVDS_PANEL_X/Y` and
  `LVDS_PANEL_RECT_*` map it onto the glass. `270` and `0` are also valid.
- Only **two** primitives write a pixel (`lvds_hstx_pixel()`,
  `lvds_hstx_fill_rect()`); everything else - lines, rectangles, ellipses, text -
  reaches the glass through them, and that is why the turn, and the guards,
  cost nothing. Drawing is clipped to the **screen**, not to a pane: a caller that
  wants a pane clips to it itself.
- A colour is 16-bit 5-6-5. The scanout framebuffer is **4bpp indexed**: a
  colour that is not in the palette is replaced by the nearest one that is.
  There is no anti-aliasing and no blending.

## The frame is 604 active lines against a 600-line glass

**The four spare active lines are load-bearing.** The bench's marker probe
(2026-10-04) put five known colours on the frame's last five lines and one of
them came out **twice**: the close of the active area repeats a line, whatever
content is on it. Neither the frame's total length (four extra *blanking* lines
changed nothing) nor the clock is the cause, and the **root cause is still
unknown** - what is known is that the panel/link repeats or eats a line at the
close, and that spare lines absorb it.

The bench measured how many: at **600** active lines the repeat lands on the
picture, at **602** an extra black line appears, and **603** and **604** are
clean. The frame therefore carries `LVDS_PANEL_ACTIVE_HEIGHT 604` - one line of
margin over the threshold - while the view, the layout and the station stay the
glass's own 600x800 mapped onto the first 600 active lines. **The frame's length
is not a free parameter**: shortening it puts the repeat back on the picture.

The one line just before the active area is *not* blanking - it is
`g_active_line[0]` holding the last row computed into slot 0 (row 596 of a
600-row frame). That was suspected, given the inactive buffer, and the panel
check came back **negative**: it is not what the panel shows. It stays as it
has always been.

## The palette: sixteen slots, taken on first use

- Sixteen slots, shared by everything on the glass. Slot 0 belongs to the
  backend's own black; a colour takes a slot the first time it is *used*, and
  once the table is full a new colour is given the nearest slot's colour.
- Colours are **not** taken when a table is parsed. That was a real fault: a
  picture that brought its own colours found the palette already filled by
  whatever had been parsed before it and came out in the panel's greys (bench,
  2026-09-27).
- A picture that brings colours of its own takes them and gives them back with
  `lvds_hstx_palette_reset()`, which has exactly one caller today (the boot
  splash). Anything else that draws its own palette must do the same.

## Realtime memory rules

- Scanout memory is **static, aligned and in SRAM**: the 4bpp framebuffer, the
  core1 line buffers, the descriptor/control ring, the core1 stack, and the
  HSTX/palette/LUT state. Permanent realtime buffers stay there.
- **No PSRAM scanout, no PSRAM front/back scanout, no DMA present from the PSRAM
  draw buffer.** The last was measured at roughly 7.3 ms for 240,000 bytes and
  still produced display garbage. `lvds_hstx_present()` stays a CPU copy.
- **No health polling, auto-recovery, retry loop, watchdog-on-scanout, or
  chunked present** in release code. A stalled HSTX path is a memory, ownership
  or timing fault: fix the fault.
- Core1 prepares the line the descriptor ring reaches
  `LVDS_HSTX_ACTIVE_LINE_BUFFERS - 1` scanlines later, and that arithmetic
  **wraps into the next frame**. Preparing from the current active `y` instead
  left the previous frame's tail across the header - keep the wrap.
- Nothing large and automatic on the core1 path; nothing new and live in
  Scratch X/Y.

## Not in the boundary

The renderer does not own program state, files, menus, G-code, tool catalogs or
machine safety. Images are **not** in the API yet, though they belong in it: the
boot splash carries its own run-length decode and palette bookkeeping today, and
the pad's key pictures will be the third copy unless it is added (`README.md`,
"Images belong here too").

## Validation status

Software-verified here: the firmware builds for `RP2350-LEANCAM-LVDS` (nc2) and
`RP2350-LEANCAM-LVDS-NCC`, and the station's own checks compile the draw API,
the palette and the fonts for the host and compare frames.

Bench-only, in the operator's hands: the close-of-frame tail above, panel
readability, the clock points, feed hold, Stop during queued motion, and SD
cold-start mounting. No software suite proves any of them - the G33 suites
intercept the encoder, so they prove ordering and error propagation, not spindle
phase.

The torture harness (`LVDS_HSTX_TORTURE_TEST`), canaries, fault injection, the
timing meters and the line-repeat watchdog are history: they are not supported
configurations and must not come back into release code.

## Reusing it outside uCNC, today

1. Take this directory - as a submodule, a copy, or a `lib_extra_dirs` entry.
2. Define the panel's numbers: `LVDS_WIDTH`, `LVDS_HEIGHT`,
   `LVDS_PANEL_ACTIVE_HEIGHT`, `LVDS_PANEL_TURN`, the four clock values and the
   eight pin names (uCNC keeps them in `cnc_hal_overrides.h`).
3. Provide the four uCNC entry points named at the top; leave out
   `lvds_renderer_boot.c` if you arm your own boot, and the PSRAM file unless
   `LEANCAM_USE_PSRAM_BACKBUFFER` is on.
4. Build the sources into your firmware. The portable half will also build for a
   host, which is what the station does.

Missing for a clean standalone build: a port header so those four calls are not
editable source, and an example target to prove it. Both are deliberately not
invented here.
