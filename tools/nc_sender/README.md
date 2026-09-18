# nc_send - desktop NC tool and Grbl/uCNC sender

The NC screen and the G7x cycles normally run on the RP2350 + LVDS machine.
`nc_send` runs the same code on a PC and talks to any Grbl-compatible
controller over a serial port, so the lathe work does not require the custom
display hardware.

It is the same split as `tools/leancam_win`, but for the NC module and the G7x
library instead of LeanCam.

## What is reused

| Layer | Source | Runs on the PC? |
| --- | --- | --- |
| Program text and editing | `modules/nc/nc.c` | yes, as-is |
| Cycle expansion + preview stream | `modules/nc/nc_emit.c`, `modules/g7x/g7x*.c` | yes, as-is |
| G7x block scan (ranges, two-line headers) | `modules/nc/nc_g7x.c` | yes, as-is |
| Editor text helpers | `modules/g7x/g7x_contour.c` | yes, as-is |
| LVDS drawing, keypad, menus | `modules/nc/nc_visual.c`, `nc_menu.c` | replaced by a desktop UI |
| SD card files | `modules/nc/nc_files.c` | replaced by stdio (`nc_sender_load_file`) |
| uCNC parser streaming | `modules/nc/nc_run.c` | replaced by `grbl_stream.c` |
| Controller status text | `modules/nc/nc_feedback.c` | replaced by Grbl status reports |

The generator is the same code the machine runs, so a program expanded here
matches what the LVDS screen previews.

## Build

```powershell
cd tools\nc_sender
mingw32-make
mingw32-make test
```

or, without make:

```powershell
python tools\test_nc_sender.py
```

## Use

```text
nc_send --check program.nc                 expand and report problems
nc_send --expand out.nc program.nc         write the controller program
nc_send --port COM5 program.nc             stream to a controller
nc_send --port COM3 --target ucnc p.nc     keep lathe words for a uCNC machine
nc_send --comments --expand out.nc p.nc    keep comments in the export
```

`--check` never opens a port, so it is safe for a dry run.

## Conversion rules

Grbl does not understand the lathe dialect, so the default `grbl` target
rewrites the program while expanding it:

- `G71`/`G72` contours are expanded by the shared G7x stepper into ordinary
  `G0`/`G1`/`G2`/`G3` blocks, exactly like the on-machine preview;
- `G7`/`G8` are removed and remembered. In the default `G7` diameter mode every
  `X` word is halved, so the controller sees radius values. `G8` programs pass
  through unchanged;
- `G970`-`G973` setup metadata and comments are kept out of the wire output;
- `G33`/`G76` are refused with `unsupported` because stock Grbl cannot
  synchronise the spindle. Use `--target ucnc` for a controller that has the
  G33 module.

The `ucnc` target keeps the program as written: lathe words stay, and `X`
values are not converted.

## Status

Working today:

- host build of the NC + G7x logic with no hardware dependency;
- expansion of one-line and Fanuc two-line `G71`/`G72` headers, numbered ranges
  and `G80` contours, for both targets;
- Grbl 1.1 line protocol: greeting, one line at a time with `ok`/`error:`
  handling, `?` status reports (`WPos`, `FS`), feed hold, resume, soft reset,
  `$X` unlock;
- Win32 COM transport (`\\.\COMnn`, 8N1, non-blocking reads) and a scripted fake
  controller for tests.

Next steps:

- a Win32 GUI shell (program list/editor + preview + connect/run), reusing
  `grbl_stream` and `nc_sender` the same way `leancam_win` reuses LeanCam;
- `G76` expansion through the shared G7x threading generator, target permitting;
- character-count streaming so the controller buffer stays full instead of one
  line per `ok`.
