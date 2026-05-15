# LeanCam RP2350 HDMI Backend Staging

This folder contains only the RP2350 LeanCam display backend for uCNC.

Current status:

- 800x600 RGB222 HSTX/DVI drawing primitives
- internal SRAM scanout buffer
- optional PSRAM backbuffer with dirty-row copy to scanout
- wired into a dedicated uCNC HDMI smoke-test environment
- the old serial-only minimal environment remains available as fallback

uCNC targets:

- PlatformIO env: `RP2350-LEANCAM-MINIMAL`
- UF2: `.pio/build/RP2350-LEANCAM-MINIMAL/firmware.uf2`
- purpose: small RP2350 uCNC base with no HDMI module loaded
- PlatformIO env: `RP2350-LEANCAM-HDMI`
- UF2: `.pio/build/RP2350-LEANCAM-HDMI/firmware.uf2`
- purpose: same minimal base, plus DispHSTX and `leancam_display`
- boot behavior: initializes HDMI and draws a static backend smoke screen
- board map: `src/hal/boards/rp2350/boardmap_waveshare_pizero_minimal.h`
- G7/G8 parser extension disabled
- G33 not loaded
- encoders disabled
- 74HC595/74HC165 disabled

Build integration:

- `leancam_disphstx.py` adds the required DispHSTX C and M33 assembly sources
  from `C:/acc/rp2350/DispHSTX`
- local `config.h` keeps the Waveshare 800x600 HSTX/DVI pin mapping aligned
  between C and assembly
- `leancam_hdmi_boot.c` starts the LeanCam UI renderer on the display core

Storage is intentionally not owned by this module. The RP2350 SD/FatFs bridge
lives in `../leancam_rp2350_sd` so the LeanCam UI and uCNC core can share it
without coupling storage to HSTX video.
