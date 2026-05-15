# LeanCam RP2350 SD Module

This module owns the working RP2350 SD-card path for the LeanCam HDMI target.
It is separate from `leancam_rp2350_hdmi`: HDMI owns video, while this module
mounts the card and exposes file access to LeanCam/uCNC.

Current status:

- uses the known-working no-OS FatFs SD driver from
  `C:/acc/RP2350-PiZero/C/03-MicroSD/src`
- configures the Waveshare PiZero SD socket pins in `leancam_sd_hw_config.c`
- mounts FatFs drive `0:`
- registers a uCNC filesystem drive as `/S`
- provides LeanCam file save/load through `leancam_files_fatfs.c`
- keeps short `$SL` and `$SC` diagnostic commands for bring-up

This is not an HSTX/display dependency. It is a storage module loaded beside
the HDMI module by the RP2350 LeanCam target.

Important bring-up note:

Do not replace this module's small `fs_*` bridge by loading full
`src/modules/file_system.c` in `RP2350-LEANCAM-HDMI` without a staged boot test.
That change has twice resulted in dead HDMI output and dead USB serial during
startup. Keep full uCNC filesystem integration as a separate step.
