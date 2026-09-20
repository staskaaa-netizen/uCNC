# Desktop NC sender

See [history-leancam-to-nc.md](history-leancam-to-nc.md) for how this work grew
out of the earlier LeanCamWin app, and [history-project.md](history-project.md)
for the full arc from the ESP32/RA8876 machine to the module split.

The NC screen, the editor and the G7x cycles only ran on the RP2350 board with
the LVDS display and the keypad. That hardware is capable but unusual: a normal
PC cannot use the NC workflow, and a Grbl controller cannot preview or expand
the lathe cycles on its own.

`tools/nc_sender` closes both gaps. It runs the NC + G7x module code on Windows
and speaks the Grbl protocol, so the same programs and the same expansion can be
edited, previewed and sent without the custom display.

## Layering

```
        NC document + editor        nc.c, nc_presets.c
                  |
        expansion to controller     nc_emit.c + g7x*.c (unchanged generators)
                  |
        block scan / dialect         nc_g7x.c (ranges, two-line headers, G80)
                  |
        sender core                 tools/nc_sender/nc_sender.c
                  |
        Grbl line protocol          tools/nc_sender/grbl_stream.c
                  |
        serial port                 grbl_port.h: Win32 COM or a fake for tests
```

Everything above the sender core is the firmware code, compiled host-side with
`G7X_HOST_TEST`/`NC_HOST_TEST` and the same source-file adapter the on-machine
preview uses. Only the four hardware layers (LVDS drawing, keypad menus, SD
card, uCNC parser streaming) are replaced.

## What the conversion does

The default `grbl` target turns a lathe program into something a Grbl
controller can execute:

- `G71`/`G72` contours, including numbered `P`/`Q` ranges and Fanuc two-line
  headers, become plain `G0`/`G1`/`G2`/`G3` blocks;
- `G7`/`G8` are removed and the mode is remembered; in diameter mode every `X`
  word is halved so the controller receives radius values;
- `G970`-`G973` setup rows and comments stay out of the wire stream;
- `G33`/`G76` are refused with `unsupported`, because stock Grbl has no spindle
  synchronisation. `--target ucnc` keeps the lathe words for a controller that
  does.

## Status and next steps

Done: host build, expansion for both targets, Grbl 1.1 protocol (greeting,
`ok`/`error:`, `?` status with `WPos`/`FS`, hold, resume, soft reset, unlock),
Win32 COM transport, scripted fake-controller tests and CLI smoke checks
(`python tools/test_nc_sender.py`).

On the UI side, `tools/nc_ui_win` runs the real panel layout on Windows: the NC
screen code draws through the host LVDS backend, so the 800x600 layout, palette
and fonts are the firmware's, and the machine keys (F1-F5 mode row and the
machine's 4x4 keypad) sit next to the emulated panel. `python
tools/test_nc_ui.py` renders a panel frame and a whole-bench frame headlessly
and runs the headless checks (`--fstest`, `--presettest`, `--streamtest`,
`--padtest`).

Next:

1. Wire the panel shell's RUN path to a real controller: NC RUN currently drives
   the virtual parser, so it needs a Grbl transport built on
   `tools/nc_sender/grbl_stream.c` (open port, expand, stream, status, hold).
2. `G76` expansion through the shared G7x threading generator, allowed only for
   threading-capable targets.
3. Character-count streaming to keep the controller buffer full instead of one
   line per acknowledgement.
4. ~~Host filesystem driver so the desktop UI can reuse `nc_files`-style
   listing and the document path handling instead of the stdio shim.~~ Done:
   `tools/nc_ui_win/host_fs.c` is a real `fs_t` driver for `/D`.
