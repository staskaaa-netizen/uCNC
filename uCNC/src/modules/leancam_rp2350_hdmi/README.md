# LeanCam RP2350 HDMI Backend Staging

This folder stages the standalone RP2350 LeanCam display backend for migration
into uCNC.

Current status:

- copied from the working standalone `leancam_hdmi_pio_usb` proof
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
- I2C keyboard avoided
- 74HC595/74HC165 disabled

Build integration:

- `leancam_disphstx.py` adds the required DispHSTX C and M33 assembly sources
  from `C:/acc/rp2350/DispHSTX`
- local `config.h` keeps the Waveshare 800x600 HSTX/DVI pin mapping aligned
  between C and assembly
- `leancam_hdmi_boot.c` is intentionally only a smoke-test module, not the final
  LeanCam UI bridge

Next stage is to connect the real LeanCam snapshot/bridge drawing to
`leancam_display` primitives, then add the USB keyboard path.
