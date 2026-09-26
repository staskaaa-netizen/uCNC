# Firmware images kept as backups

`RP2350-LEANCAM-LVDS_nc-panel_2026-09-26.uf2` - the machine firmware with **nc**
as the panel, built from commit `a4406065` (the last commit before
`cnc_hal_overrides.h` was switched to nc2). Flash this to go back to the panel
that ran before the switch:

    RP2350-LEANCAM-LVDS   flash 211 400 B   RAM 69.5 %

It is the same image those numbers describe, so it can be recognised: the nc2
panel is flash 177 352 B / RAM 65.1 % and is what `pio run -e
RP2350-LEANCAM-LVDS` produces now.

Rebuilding it, if the file is ever lost:

```powershell
git worktree add tmp/old-fw a4406065
$env:PLATFORMIO_CORE_DIR='C:\pio'
pio run -e RP2350-LEANCAM-LVDS -d tmp/old-fw
# tmp/old-fw/.pio/build/RP2350-LEANCAM-LVDS/firmware.uf2
git worktree remove tmp/old-fw
```
