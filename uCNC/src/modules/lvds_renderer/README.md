# LeanCam RP2350 LVDS Backend

This folder contains the RP2350 LeanCam display backend for uCNC.

Current status:

- Sharp LQ121S1LG44 800x600 single-channel LVDS output over RP2350 HSTX
- 4bpp indexed SRAM framebuffer
- PicoLVDS-style 7x256 scanline LUT
- core1 converts framebuffer scanlines into SRAM HSTX line data
- DMA feeds prepared scanlines to HSTX
- core0 keeps running uCNC and the LeanCam renderer
- PSRAM draw buffer is enabled for LeanCam drawing; scanout uses SRAM
- LeanCam UI data arrives through `ui_snapshot`; the renderer does not parse
  files or own LeanCam editing state
- live-run preview is visual only; it does not affect G-code execution or
  machine safety logic

PlatformIO target:

- env: `RP2350-LEANCAM-LVDS`
- SD/filesystem is enabled in the normal LVDS env
- UF2: `.pio/build/RP2350-LEANCAM-LVDS/firmware.uf2`
- board map: `src/hal/boards/rp2350/boardmap_waveshare_pizero_minimal.h`

LVDS pinout:

- GP12 D0+
- GP13 D0-
- GP14 D1+
- GP15 D1-
- GP16 D2+
- GP17 D2-
- GP18 CLK+
- GP19 CLK-

Sharp LQ121S1LG44 clock notes:

- Nominal panel pixel clock is 40 MHz, but this panel locks below that in
  practice.
- The current HSTX packing emits one pixel clock for each 7 serial transfer
  phases, so the useful bring-up estimate is:
  `pixel_clock_mhz = LVDS_SYS_CLOCK_KHZ / 1000 / LVDS_HSTX_CLOCK_DIV * 2 / 7`
- Known working test points:
  - `370000 / div 3` -> about 35.24 MHz
  - `270000 / div 3` -> about 25.71 MHz, video output still works
  - `270000 / div 4` -> about 19.29 MHz, video output still works
  - `370000 / div 4` -> about 26.43 MHz, video output still works
  - `280000 / div 3` -> about 26.67 MHz, currently preferred empirically
- `420000 / div 3` did not produce video in testing. Treat that as an RP2350
  overclock/voltage/PSRAM-flash stability boundary for this board, not as a
  panel timing requirement.
- Earlier experiments failed because they changed more than the divider/clock
  test point at once: the HSTX PLL/sys-clock path and PSRAM behavior were also
  being disturbed. The working tests above keep the normal clock path and only
  vary `LVDS_SYS_CLOCK_KHZ`, `LVDS_HSTX_PLL_KHZ`, and
  `LVDS_HSTX_CLOCK_DIV`.

Keep scanout sourced from SRAM. Experimental PSRAM front/back scanout was tested
and rejected because it was slow and caused sync loss on the panel.

Do not poll `fs_file_run_active()` from the LVDS renderer state path. On this
RP2350 SD/LVDS setup that call can deadlock during the live renderer loop.

Do not generate LeanCam G-code inside the LVDS renderer. This was tested with
local preview burst modes that built the generated G-code cache on the renderer
side and then skipped both parsing and pixel drawing; that still reset uCNC.
The likely mechanism is not the final pixel draw itself, but running the
LeanCam generation/state/cache path in the render/HSTX timing domain while
core1 scanout, PSRAM backbuffer, snapshot publication, and uCNC tasks are
active. In practice it can starve or corrupt timing-sensitive work enough to
reset the controller.

Safe generated preview architecture:

- LeanCam bridge owns G-code generation.
- The bridge queues generated lines and advances them at a fixed pace.
- The snapshot carries only the current generated preview line and sequence.
- The LVDS renderer consumes snapshot data only and draws at most one generated
  line per preview tick.
- Do not call `lc_raw_preview_cached_region_gcode()` from sim preview drawing.

LeanCam renderer scope:

- normal editor rows and right-side stock/cycle preview
- tool catalog preview for `TOOL|...` entries
- full-screen live material-removal view while running/holding
- no PROCESS or T+P catalog preview path in the current minimal model

Tool preview notes:

- `TOOL.ORIENT` is interpreted as a keypad code using the 3x3 layout:
  `7 8 9 / 4 5 6 / 1 2 3`
- `0` or missing `ORIENT` draws only a red DOC-sized dot at X0/Z0
- one digit draws the legacy square/drill preview
- three or four digits draw a simple polygon insert preview from those keypad
  points
- three-digit shapes anchor at the middle digit; four-digit shapes use their
  middle pair as the X/Z reference, so `2486` hangs from the origin by `4-8`
  instead of being centered on it
- examples: `276` rhombic, `183`/`176`/`172`/`679` triangular, `2486`
  rotated square with `4-8` emphasized as the cutting side
- non-cutting polygon sides are filled but not outlined in red; for example
  `172` does not draw a red border between `1` and `2`
- live simulation still uses the fast legacy marker path; for multi-digit
  shapes it collapses the code to the inferred tip direction
