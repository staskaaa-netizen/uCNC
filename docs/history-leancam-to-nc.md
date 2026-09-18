# From LeanCamWin to the NC desktop tools: history and working rules

Two generations of "run the machine UI on a PC" exist in this repository. The
first one is the LeanCam Windows app (May 2026). The second is the NC + G7x
module split with its desktop tools (June 2026 onward, extended in September).

The point of this note is not to grade the old app. It is to keep the reasons
behind the current shape, because most of them were learned the hard way, and to
turn them into rules for the next tool or feature.

## What was compared

| File | Lines | Date | Note |
| --- | --- | --- | --- |
| `tools/leancam_win/leancam_win.cpp` | 1983 | added 2026-05-01, last touched 2026-05-08 | Win32/GDI app: editor, preview, sender, local runner |
| `tools/leancam_win/leancam_gcode.c` | 668 | tracked copy from the same era | stale and unused by the build; contains no `G71`/`G72`/`G76` at all |
| `uCNC/src/modules/leanCam/leancam_gcode.c` | 2783 | current | the module generator the tool actually compiles; 79 `G71`/`G72`/`G76` references |
| `uCNC/src/modules/leanCam/*.c` | 21 files | current | editor, files, presets, schema, tools, bridge, dxf, run, viewer |
| `uCNC/src/modules/nc/*.c` | 16 files | from 2026-06-08 | document, files, state, menu, emit, run, visual, sim, block scan |
| `uCNC/src/modules/g7x/*.c` | 3 files | from 2026-06-14 | contour/cycle/thread generation, source contract, history |
| `tools/nc_sender/*` | ~1.0k lines | 2026-09-18 | expansion + Grbl protocol + COM + CLI |
| `tools/nc_ui_win/*` | ~0.6k lines | 2026-09-18 | real NC screen on a host LVDS backend + machine key row |

## Timeline

- 2026-05-01 `99861b67` Add LeanCam Windows editor and sender.
- 2026-05-08 `ac005d0d` last change to that tool.
- 2026-06-08 `80d760cb` "Add thin NC module checkpoint"; `eb8f02b9` NC run
  streams source lines. The NC module starts as the machine-side screen.
- 2026-06-14 `374e55e3`/`336731ba` G7x gains the thread stream and Fanuc G76
  semantics; `d2f393e3` the RP2350 LVDS backend wires it up. Cycle generation
  now lives in its own module.
- 2026-09-15/16 upstream merge and the ownership audit: G7x TODO, NC TODO and
  the bench checklists are split by module.
- 2026-09-18 G7x gains P/Q ranges and Fanuc two-line headers; `tools/nc_sender`
  and `tools/nc_ui_win` reuse the modules on the PC.

## Generation 1: LeanCam module + LeanCamWin

What it established, and it was right:

- a desktop tool that drives the *module's* C generator instead of a rewrite;
- Win32/GDI UI with the same field layout as the embedded screen;
- streaming over COM with `ok`-per-line and `?` status polling driving a live
  marker;
- awareness that the same program can be sent to a uCNC controller or filtered
  for Grbl.

What it cost:

- the tool tracked its own copy of the generator (`tools/leancam_win/
  leancam_gcode.c`, 668 lines, no cycle words). The build moved on to the module
  file, so the copy is dead weight that still looks authoritative: two files
  with the same name and different contents in one repository.
- it re-implemented motion in the app: when no uCNC bridge is connected, `Run`
  uses a "local visual fallback" that interprets `G0/G1/G2/G3/G33` itself. That
  is fine as a demo, but it means the app can show motion the controller would
  never produce, and nothing flags the difference beyond a status string.
- the Grbl difference was a UI checkbox that filtered `G7`/`G8` out of the
  output. The rule lived in the UI, was not covered by tests, and could not
  report *why* a program was not sendable.
- cycle geometry sat in the same generator file as the conversational logic
  (`lc_gcode_stepper` still carries G71/G72 and the older G76 stepping), which is
  exactly the pile the G7x module was later extracted from.
- no automated test for the app itself; the module has a host test
  (`leanCam/tests/leancam_gcode_host_test.c`) but nothing in `tools/` runs it,
  and stale `.exe` files sit next to the sources.
- the tool's Makefile names its sources by hand. When the module grew
  `leancam_code.c` and friends, the tool stopped linking (`undefined reference
  to lc_code_tool_line_is`) without anyone touching the tool.
- the shipped `.exe` needs the toolchain's `libwinpthread-1.dll`.

## Generation 2: NC + G7x modules, desktop tools on top

The NC module owns files, editor, state, menu, the emitter and the screens. The
G7x module owns contour storage, cycle semantics, allowances, approach/retract,
pass generation and threading, plus the source contract for numbered ranges.
The core parser executes generated blocks. Those boundaries are written down in
`uCNC/src/modules/g7x/README.md` and `uCNC/src/modules/nc/TODO.md` rather than
being implied by whichever file happens to call what.

On the desktop, three things reuse that ownership instead of copying it:

- `tools/nc_sender` compiles `nc.c`, `nc_emit.c`, `nc_g7x.c` and the G7x
  sources, expands the program (including one-line and Fanuc two-line P/Q
  ranges) and streams it with a Grbl 1.1 client;
- `tools/nc_ui_win` compiles `nc_visual.c` *unmodified* and gives it a host LVDS
  backend, so the panel layout is the firmware's, and attaches the machine key
  row (F1-F6 modes, F7-F12 soft keys, 3x3 pad) beside it;
- `nc_g7x.c` gives RUN, the preview and the editor one shared answer to "where
  does this G7x block start and end", so the three views cannot disagree.

## Comparison A: old app vs current desktop tools

| Concern | LeanCamWin (May) | NC sender + panel (Sep) |
| --- | --- | --- |
| Generator | copies the generator file into the tool | compiles the module sources, no copies |
| Motion preview | app-side runner as fallback | app-side runner only in the virtual-core build; the real parser/interpolator runs |
| UI | re-implemented in C++ against the RA-style layout | the real screen code on a host renderer backend |
| Dialect handling | `Grbl` checkbox + line filter | explicit target profile (`grbl` vs `ucnc`) with X conversion, G7/G8 handling and `unsupported` errors |
| Protocol | inside the app | `grbl_stream.c` + `grbl_port.h` with a scripted fake controller |
| Tests | none for the tool | `test_nc_sender.py`, `test_nc_ui.py`, plus the four G7x suites |
| Build | hand-listed sources, broke silently | module sources + runner scripts; firmware builds checked as well |
| Runtime deps | needs `libwinpthread-1.dll` | statically linked, system DLLs only |

## Comparison B: LeanCam approach vs NC approach

| Concern | LeanCam | NC + G7x |
| --- | --- | --- |
| Program model | conversational operations converted to G-code | G-code text edited directly, with conversational presets as inserts |
| Cycle logic | inside the LeanCam generator | G7x module, consumed by the NC emitter, the sender and the machine |
| Who can run without a screen | nobody: the generator is tied to the conversational layer | G7x has a standalone parser test target and no NC dependency |
| Dialect/dialect errors | implicit in the generator output | explicit result codes (`G7X_RANGE_MISSING`, `G7X_RANGE_AMBIGUOUS`, `unsupported`) asserted by tests |
| On-screen preview | separate LeanCam preview code | one emitter (`nc_emit`) feeds the panel preview and the desktop sender |
| Numbered ranges (P/Q) | not part of LeanCam | implemented in the parser and the preview from one block scan |

## Do

1. Give every domain exactly one owner, and write the ownership down.
   `g7x/README.md` says who owns contour, semantics, generation and preview, and
   `nc/TODO.md` repeats "Cycle implementation belongs to G7x".
2. Make the owned module runnable without the UI, and prove it in a test target
   (`test_g7x.py standalone` links the real parser with no NC source).
3. Compile the module sources into desktop tools; add only glue. The two new
   tools are ~1.8k lines including headers because the geometry, the dialect and
   the UI already existed.
4. Render the same drawing code on the host when layout must be judged. The
   panel shell ports ~11 LVDS primitives and reuses the firmware fonts; the
   layout cannot drift, and `--dump` makes it reviewable without the LCD.
5. Turn dialect and protocol differences into explicit, tested APIs, not UI
   toggles: `nc_sender_target_t`, `G7X_RANGE_*`, `grbl_stream_*`.
6. Keep the wire protocol behind a port abstraction with a scripted fake
   controller, so streaming behaviour is testable without hardware.
7. Keep limitations next to the code: `g7x/TODO.md` (what is not implemented),
   `g7x/TESTING.md` and `nc/TESTING.md` (what only the bench can confirm).
8. Build host tools statically so a shipped exe has no toolchain DLL
   dependency; check with `objdump -p <exe> | findstr "DLL Name"`.
9. Keep build entry points alive: `tools/test_*.py` both build and run, so a
   broken tool shows up as a failing script rather than a stale binary.

## Don't

1. Don't re-implement motion, expansion or dialect rules in the app. The old
   "local visual fallback" could disagree with the controller and only a status
   string said so.
2. Don't keep a second copy of a module source in the repository. The tracked
   668-line `leancam_gcode.c` in the tool folder has no cycle support while the
   module file has 2783 lines; the build ignores the copy, and the copy silently
   misleads.
3. Don't put the only statement of a rule in a UI checkbox or a status string.
   If a program cannot run on a controller, say it with an error code the tests
   can assert.
4. Don't let a tool freeze a hand-written list of another module's files. That
   is how the LeanCam app stopped linking when the module was split.
5. Don't mix generation, file IO and UI state in one file. The G7x extraction
   exists because cycle geometry lived inside a 116 KB conversational generator.
6. Don't ship or keep build artifacts next to sources; `.gitignore` plus a
   runner script beats stale `.exe` files.
7. Don't require PATH surgery or a sibling DLL to start a build output.
8. Don't mark work finished from software tests alone. `TESTING.md` exists
   because spindle phase, feed hold and the panel layout need the machine.

## What carried over

The old app's good instincts survived in the new tools: Win32/GDI desktop, the
module's C generator as the single source of truth, `ok`-based streaming with
`?` status polling and a live marker, and the observation that the same program
needs a Grbl path and a uCNC path. What changed is that those instincts became
modules, profiles and tests instead of staying inside one application.

## Verification

```powershell
python tools\test_g7x.py all        # generator, NC emitter, parser, standalone
python tools\test_nc_sender.py      # expansion + Grbl protocol with a fake controller
python tools\test_nc_ui.py          # panel shell + headless layout frame
pio run -e RP2350-LEANCAM-LVDS      # the panel change still builds for the machine
```

## Open threads

- LeanCam still owns its own G71/G72/G76 stepping; the G7x module is not yet
  wired into the conversational layer, so the "one owner" rule is only half
  applied on that side.
- `tools/leancam_win` needs a Makefile rebuild (module sources + static link) or
  it should be retired in favour of the NC tools.
- NC RUN on the desktop drives the virtual machine; a Grbl transport for real
  hardware is the next integration step.
- The 3x3 mapping question (side pad vs the on-screen bottom row) is a layout
  decision the panel shell is meant to help answer.
