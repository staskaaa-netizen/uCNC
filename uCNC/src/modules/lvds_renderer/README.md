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

Keep scanout sourced from SRAM. Experimental PSRAM front/back scanout was tested
and rejected because it was slow and caused sync loss on the panel.

Do not poll `fs_file_run_active()` from the LVDS renderer state path. On this
RP2350 SD/LVDS setup that call can deadlock during the live renderer loop.
