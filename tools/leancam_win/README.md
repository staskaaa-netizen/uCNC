# LeanCamWin

Small Windows desktop prototype for LeanCam conversational lathe programs.

It uses the real LeanCam C G-code generator from `uCNC/src/modules/leanCam/leancam_gcode.c`, then previews and runs the generated toolpath in a Win32/GDI simulator.

## Build

```powershell
cd tools\leancam_win
mingw32-make
.\build\LeanCamWin.exe
```

## Download

A prebuilt Windows executable is included for quick testing:

```text
tools/leancam_win/dist/LeanCamWin.zip
```

Unzip it and run `LeanCamWin.exe`.

## What It Does

* view `.lcam` commands in the same two-row LeanCam style as the RA8876 UI: field labels on top, field values below
* use the same RA8876 RGB565-derived color palette as the ESP32 renderer
* use an equal-width split between the scrollable LeanCam program view and graphical preview, with a compact 20% code tab pane below the program view
* navigate/edit like the embedded LeanCam flow: Up/Down select lines, Enter edits or accepts a field, the last Enter auto-commits the draft line, End can still force commit, Delete removes a line
* show typed draft input immediately in the active value cell and preview, before `Enter`/`End`
* preserve an active field's negative sign while typing replacement digits, matching ESP32 draft entry behavior
* show RA-style preview corner labels such as `Z1`, `D1`, `Z2`, and `D2`
* insert new operations from the table view with the same style of number shortcuts: `0` tool, `1` OD, `2` ID, `3` FACE, `4` DRILL, `6` CUT
* create a starter program
* open/save `.lcam`
* generate G-code through the shared LeanCam converter
* save generated `.nc` files, with optional Grbl filtering
* draw stock, chuck length, cycle preview, and generated toolpath
* stream generated G-code to a uCNC virtual COM port and animate from real uCNC status reports
* stream Grbl-compatible sender output over COM when the `Grbl` checkbox is enabled
* fall back to a local visual `G0`/`G1` runner when no uCNC COM bridge is connected

## uCNC Virtual Core Run

The `Connect` button opens the COM port in the box next to it, default `COM14`.

When connected, `Run` uses the real controller communication path:

1. LeanCam source is converted to G-code by `leancam_gcode.c`.
2. G-code is streamed line-by-line over COM, waiting for `ok` before the next line.
3. The app polls with Grbl/uCNC `?` status requests.
4. The live marker is driven from `<...|WPos/MPos:...|FS:...>` reports.

Enable the `Grbl` checkbox before `Run` or `Save NC` to filter uCNC-only `G7`/`G8` diameter-mode lines from the sender/export output. The generated pane still shows the original uCNC-flavored output, so it remains clear what LeanCam produced.

That is the intended ESP32-equivalent simulation path: the parser/planner/interpolator live in uCNC, while the Windows app is only the UI and visualization.

The current repository virtual MCU has `MCU_HAS_UART` commented out in `uCNC/src/hal/mcus/virtual/mcumap_virtual.h`, and PlatformIO is not installed in this workspace. To use this bridge, build/start the Windows virtual target with UART enabled and point both sides at a paired virtual COM port. If no COM bridge is connected, `Run` is clearly marked as `local visual fallback` and does not claim to be the real uCNC core.
