# From LeanCamWin to the NC desktop tools: history and working rules

This note covers the desktop tools. The whole project arc - ESP32/RA8876,
PCNT/PIO encoders, G33/G76, the HDMI detour, LVDS, the NC module and the G7x
extraction - is in [history-project.md](history-project.md).

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
  backend, so the panel layout is the firmware's, and attaches the machine's own
  key row (F1-F5 modes, and the 4x4 keypad `cam_keyboard.c` decodes) beside it.
  The pad sends the character through `nc_visual_key_for_char()` - the same
  table `nc_module.c` maps the hardware keypad through - and takes its labels
  from the screen, so the bench cannot state a key meaning of its own;
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
10. Treat shared devices like modules: one owner at a time (SD card, display
    scanout), and write the bring-up combination down with a baseline to roll
    back to. `docs/sd-card-history.md` is the template.
11. Keep stress and health harnesses as history once they have done their job,
    and record what they measured - the 7.3 ms PSRAM present number prevents
    someone re-trying it next year.
12. Use the size trigger: at ~10k lines in a "simple" module, or ~2k in one
    file, extract the domain and rebuild the consumers instead of patching.

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
9. Don't mask a realtime fault (stack overflow, memory overlap, descriptor
   ownership) with fallback behaviour; the torture findings show those are
   architecture faults.
10. Don't re-enable a disabled bring-up workaround (long filenames, DMA,
    direct scanout) as a feature without the bench checklist.
11. Don't split a module and leave its consumers behind. LeanCamWin is that
    mistake: the module moved on, the tool froze, and its copied generator kept
    looking authoritative.

## What carried over

The old app's good instincts survived in the new tools: Win32/GDI desktop, the
module's C generator as the single source of truth, `ok`-based streaming with
`?` status polling and a live marker, and the observation that the same program
needs a Grbl path and a uCNC path. What changed is that those instincts became
modules, profiles and tests instead of staying inside one application.

## Quirks and war stories

Three of them shaped the current rules more than any design document.

### 1. Two modules fighting over the SD card

`docs/sd-card-history.md` is the full record. The shape of the problem: the
upstream SD driver and the local board bring-up both wanted to own mount
timing, and the machine has no card-detect pin to arbitrate.

What the bring-up actually depends on today (all of it learned by trial):

- software SPI on CLK GPIO30 / MOSI GPIO31 / MISO GPIO40 / CS GPIO43, because
  this pin assignment is not a valid RP2350 hardware-SPI group;
- DMA disabled, card detect undefined (255), one mount attempt with a 500 ms
  boot delay and up to three retries;
- the NC module drives the mounted `/D` filesystem and `SD_CARD_NO_SYSTEM_MENU`
  keeps a second menu from mounting the same card;
- short filenames (`FF_USE_LFN=0`) are an upstream decision made *because long
  names did not work*. Do not re-enable them as a UI improvement; enforce and
  explain the limit in the NC file UI instead.
- unmount/remount only while no file operation is active, and no claim of
  removal detection without a detect pin.

One incident is recorded precisely because it is easy to mis-attribute: a
settings-invalid lock after a firmware update was a settings validation failure.
A working SD mount neither explains nor repairs it, and the audit says so
explicitly.

Rules that came out of it: one owner per device (card, and by extension the
display path); pin the working combination (checkpoint plus a saved baseline
UF2) before touching bring-up; every mount change ships with the bench checklist
in the same document.

### 2. The direct-output torture harness

`LVDS_HSTX_TORTURE_TEST` was a temporary compile-time stress harness for the
LVDS target: repeated program runs, snapshot churn, framebuffer presents,
line-repeat fault injection, stack and canary diagnostics, with compact serial
summaries. It is gone from release code and its findings live in
`uCNC/src/modules/lvds_renderer/README.md` and
`uCNC/src/modules/leanCam/docs/architecture.md`.

What it bought:

- the "display froze while the machine kept moving" class was a stalled
  HSTX DMA/control path, not a core crash. The descriptor-ring backend replaced
  the old ping/pong handoff, and the recovery code left with it;
- measured rejections: PSRAM scanout was slow and lost sync; DMA present from
  PSRAM into the SRAM scanout framebuffer took about 7.3 ms for 240,000 bytes
  and still produced garbage, even with HSTX DMA priority and paced present DMA;
- chunked present was a pacing experiment for older layouts and is gone.

Rules that came out of it: keep `lvds_hstx_present()` on the CPU copy path;
keep realtime memory visible and boring; a stack overflow, memory overlap or
descriptor ownership bug is an architecture fault, not something to hide behind
a fallback; draw guards belong in the low-level API; torture and health knobs
stay in history, not in release configuration.

### 3. The ~10k-line restart rule

Measured today: the LeanCam module is **10,879 lines** across 21 C files, with
two of them carrying most of it (`leancam_bridge.c` 99 KB / 2809 lines,
`leancam_gcode.c` 116 KB / 2783 lines) and mixing UI state, file IO and
generation. The NC module is **6,638 lines** across 16 files, and the geometry
that used to live inside the LeanCam generator now sits in three G7x files.

That extraction is what made everything else possible: host tests, the desktop
sender, the panel shell, and a preview that cannot disagree with the machine.
LeanCamWin is the counter-example: built while the module was small, never
rebuilt after the module crossed the line, and it rotted in place.

The rule: when a "simple" module passes roughly 10k lines (or one file passes
~2k), stop patching it. Extract the domain into its own module, write down the
ownership boundary and give it a standalone test target, then rebuild the
consumers on top. Budget the consumer rewrite as part of the split - the split
without it is exactly how a tool becomes a stale snapshot.

### 4. Repairs applied while writing this note

- `tools/leancam_win` builds again: its Makefile now compiles the module sources
  the generator needs (generator, `leancam_code.c`, the G7x text helpers) plus a
  host stub for the tool-catalog lookup, which is the same contract the module's
  own host test uses, and links statically so no `libwinpthread-1.dll` is
  required. The old prebuilt exe failed with `STATUS_DLL_NOT_FOUND`; the rebuilt
  one links and starts, but should be launched on a normal desktop session to
  confirm the window (a headless run exits immediately).
- the tracked duplicate `tools/leancam_win/leancam_gcode.c` is still in the tree
  and still unused; it should be deleted so nobody edits the wrong copy.

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
- `tools/leancam_win` builds again (module sources + host stub + static link),
  but the app should be started from a normal desktop session to confirm the
  window, and the tracked duplicate `leancam_gcode.c` copy should be deleted.
- NC RUN on the desktop drives the virtual machine; a Grbl transport for real
  hardware is the next integration step.
- The key layout question is answered: the shell's keys are the machine's key
  row (the 4x4 keypad plus the mode keys), each label asked of the active
  screen, while the panel's on-screen 3x3 stays what the firmware draws - in
  MANUAL the digits' jog meanings, elsewhere the floating helper. An earlier
  side pad mapped its cells to footer positions, which sent the wrong key as
  soon as a mode labelled its slots with letters (MANUAL's `B`/`C`/`D`) or
  skipped a slot; `--padtest` now fails if a mode offers a key the keypad has
  not.
