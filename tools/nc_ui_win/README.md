# nc_ui - the programming station on Windows

Runs the NC screen exactly as the LVDS panel draws it, plus the machine's own
keypad and the screen's own usage notes beside it: the machine on the left, the
operator's desk on the right. The firmware layout, coordinate system, palette
and bitmap fonts are used as-is, so what the window shows is what the panel
shows - which is what makes it a test bench as well as a station.

The station is built for the PC because that is where a program is written and
read before it is cut: the same NC module, the same G7x cycles, the same key
meanings. It carries no second copy of a screen's rules - the keys, the usage
lines and the spindle signal all come from the firmware sources.

## How the layout stays identical

`modules/nc/nc_visual.c` draws through a small primitive API (`lvds_hstx_*` and
`lvds_draw_*`). On the machine the RP2350/HSTX backend implements it; here
`lvds_host.c` implements the same API over a 32bpp frame that GDI blits into the
window, and the firmware bitmap fonts (`lvds_font_cond_6x8.c`,
`lvds_font_ibm_8x14.c`) render the glyphs. Nothing in the screen code changes,
so a host frame is what the panel shows.

The rest of the stack is the real firmware too, built for the virtual MCU the
same way the parser test suite builds it: core, parser, NC state/menu/run,
G7x, palette.

## Desktop filesystem

`host_fs.c` is a real `fs_t` driver: drive `/D` is mapped onto a folder of the
PC (`--files DIR`, or `nc-files\` beside the exe - so a shortcut or a download
opened from anywhere mounts the same card), and the file manager, the state
store, the TOOLS document and the preset file behave on the desktop exactly as
they do on the card. The
uCNC fs layer splits the drive letter off before it calls a driver, so the paths
that arrive here are already drive relative (`/nc/files`, `/presets`, `/presets/41.txt`,
`/` for the root) - the shape FatFs sees on the machine. The driver closes
handles only; uCNC's `fs_close()` releases the driver's memory, the same
contract `sd_card_v2` relies on.

Files are opened in binary mode. The firmware's readers mix reads with seeks and
ask `fs_available()` for what is left; in text mode Windows translates bytes and
makes `ftell`/`fseek` positions meaningless, so `fs_available()` reported zero
after the first read and every file loaded as a single line.

```powershell
build\nc_ui.exe --files tmp\ncroot --fstest      # list /D through fs_*
build\nc_ui.exe --files tmp\ncroot --presettest # check the /D/presets entries
build\nc_ui.exe --streamtest                     # a jog delivers both blocks
build\nc_ui.exe --padtest                        # the keypad is the machine's
build\nc_ui.exe --uwtest                         # increments collapse to moves
build\nc_ui.exe --dirtytest                      # a key repaints what it changed
build\nc_ui.exe --runtest                        # FROM/FULL send the program
build\nc_ui.exe --blocktest                      # RUN marks the block it runs
build\nc_ui.exe --pacetest                       # one block, then wait
build\nc_ui.exe --stoptest                       # the MANUAL stops are typed
build\nc_ui.exe --spindletest                    # the spindle is the signals
build\nc_ui.exe --files tmp\demo --demotest      # the demo seeds and expands
```

## Build and render a frame

```powershell
python tools\test_nc_ui.py     # builds, dumps a panel frame and a bench frame,
                               # then runs the headless checks below
```

There is one build of the station in this tree and it is the one the checks and
the pack use: `tmp\nc-ui-tests\nc_ui.exe`. The tool's own `Makefile` writes a
second, `build\nc_ui.exe`, for building the window alone (`mingw32-make` in
`tools\nc_ui_win`) - both are rebuilt from the same sources, and both are stale
the moment a source changes, so rebuild rather than copy an exe around.

**Which build is this?** The station has no version resource, so its own file is
the answer:

```powershell
nc_ui.exe --version            # built 2026-09-25 00:03:10, 639199 bytes
```

The window title carries the same line, and `tools\pack_nc_ui.py` prints it for
the exe it puts in the zip. Those are the figures Explorer shows under
Properties - if the station you are looking at was built earlier than the
sources, it is an old copy. `pack_nc_ui.py --skip-build` refuses to pack an exe
that is older than the sources, so a release cannot quietly carry last night's
binary.

## What a release ships (the demo card)

`python tools\pack_nc_ui.py` builds the station, runs its checks and writes
`tools\nc_ui_win\dist\uCNC-programming-station-win64.zip` - the same zip
`.github/workflows/nc-ui-release.yaml` attaches to a `v*` release (the station
is the whole of what a release carries; the RP2350 image is bench-only):

```text
uCNC-programming-station.exe   the station (statically linked, no DLL needed)
USER-GUIDE.md                  how to use it, for the operator
README.md                      this file, the tool's own reference
desktop-sender.md              how the station fits the desktop tools
examples\lathe-demo.nc         the program below
examples\tool.t                the table it calls T2 from
```

`examples\` is also the station's first-run demo: a card with no program of its
own is seeded from the folder beside the exe (`host_seed_card()`, once, without
overwriting anything), so an unpacked zip opens with something to look at.
`--files DIR` names another card; a card that already has a program is never
touched.

`lathe-demo.nc` is one of the machine's own runs, written down as a file: the
preview setup rows, a tool and a spindle, one rapid, and two numbered `G71`
ranges - each finished by its own `G70 P Q` - with an `R` round, a `C` chamfer
and a straight finishing move. It is not a bench program: it is a program to
open, walk with the cursor, preview and watch RUN send. `--demotest` checks it
loads, scans as two cycles and expands, and that the seeding happens once.

Every line in the demo is short enough for the panel's own line width
(`NC_WRAP_LINE_LEN`, 46 characters): a longer line is wrapped with a tab and the
screen shows two rows, which is fine for a program and ugly for a sample.

`--dump out.bmp` writes the 800x600 panel frame - the firmware layout, nothing
of the shell. `--dump-bench out.bmp` writes the whole 1100x600 bench: the panel
plus the machine keys, so the key row is reviewable (and diffable) without
opening the window.

The window is a machine, not a picture: every 30 ms it runs the firmware's main
loop - parse the queued blocks, run `cnc_dotasks()` and advance the virtual
clock - so a jog, a feed and RUN's stream actually execute and the DRO moves.
`--ticks N` does the same N times before a dump, which is how a feeding screen
is captured; `WAIT<n>` inside `--keys` lets the machine run between presses (the
panel's reader queue holds four blocks, so a script that presses many jog keys
back to back drops the ones past that).

A screen that only appears after input can be dumped too: `--keys` replays the
machine's own key path - the keypad's own key characters (`0`-`9`, `*`, `#`,
`A`-`D`), the modes `F1`-`F5`, the PC keys that have a fixed meaning (`W` for
the keypad's `#`, Delete, Enter, Esc, Backspace), and named keys such as
`ACCEPT`, `NEXT`, `FINISH`, `BACK`, `CANCEL`, `MODE`, `UP`, `DOWN`, `LEFT`,
`RIGHT`, `MINUS`, `DOT` - before the frame is drawn. `HOLD<key>` presses a
machine key and keeps it down, `RELEASE` lets it go, so a held feed can be
scripted as well; `WAIT<n>` runs the machine for `n` ticks first.

```powershell
# MANUAL: stop at X0, eight X+ steps, continuous feed, feed 500 -> 50, hold X-
build\nc_ui.exe --files tmp\shots --dump-bench tmp\feeding.bmp ^
    --keys "F1,*,2,WAIT2,2,WAIT2,2,WAIT2,2,WAIT2,2,WAIT2,2,WAIT2,2,WAIT2,2,WAIT2,#,1,1,1,1,HOLD8,WAIT5"
```

`--streamtest` is the headless check of the one-shot blocks the panel sends on
the reader the RUN stream also uses: it queues the two blocks a jog sends and
asserts the reader delivers both, in order, and goes back to the console when
they are done. Without that, the G90 written over the move left the machine
standing still on a jog.

```powershell
build\nc_ui.exe --files tmp\shots --keys "F2,4" --dump tmp\helper.bmp
```

That one is EDIT followed by the keypad's `4`, which in EDIT is the footer's
`4 G7X`, so the dump is the floating 3x3 helper with its labelled line.

Or straight from the tool directory:

```powershell
cd tools\nc_ui_win
mingw32-make
build\nc_ui.exe --dump frame.bmp
build\nc_ui.exe
```

## Window layout

The window is the emulated panel (800x600, exactly as the panel draws) with the
machine's keypad attached to the right side, the way the proprietary keyboard
sits next to the display. This is the *programming station* side of the tool:
the panel is the machine, and the strip beside it is the desk. The keys are the
hardware's, not a second menu:

| Keys | Function |
| --- | --- |
| F1-F4 | operation modes: MANUAL, EDIT, TOOLS, RUN |
| MODE | cycles the modes like the machine's own MODE (`A`) key |
| keypad | the 4x4 matrix `cam_keyboard.c` decodes, row 1 on top: `* 0 # D` / `1 2 3 C` / `4 5 6 B` / `7 8 9 A` |

Every pad key does exactly what the machine key does: the character goes through
the screen's own key table (`nc_visual_key_for_char()`, the same one
`nc_module.c` uses), and the label under the key is what the active screen says
the key means - the footer entry that carries that key, or the screen's own word
for a key the footer does not name (MANUAL's jog digits: `7`/`9` spindle,
`2`/`8` X, `4`/`6` Z, `5` spindle stop, `1`/`3` the value on screen). A key the mode
does not use stays unlabelled. Nothing about the meanings lives in the shell, so
a mode that renames a key renames it here too.

The strip answers "what is this key" for the whole keypad, including the keys
the menu does not name:

* the screen's name and a few lines saying what it is for and how its keys
  drive it - `nc_visual_screen_name()` and `nc_visual_usage()`, the screen's own
  words, so a screen that renames a key or gains one renames it here too. The
  usage lines are what an operator reads instead of a manual: the off-menu keys
  (`B`/`C`, `#`) are named there;
* a key the footer carries is labelled in green and a key the footer does not
  name - the screen's own key - is labelled in grey, so "the menu does not list
  this" is visible at a glance. A key that means nothing here stays unlabelled;
* the keypad's step keys (`B`/`C`) are drawn with the arrow they act as, because
  on those screens they are the arrows: MANUAL's axis pick, and the field and
  list walk everywhere else (`nc_visual_key_meaning()`);
* `nc_visual_key_meaning()` is the one answer for all of it - the footer entry,
  the screen's own word, and whether the key steps a field - so the strip, the
  `--padtest` check and any future shell read the same table.

The station has no spindle encoder, and must not need one: the spindle is read
off the signals the tool drives on the emulated MCU - PWM0 the speed, DOUT0 the
direction, the same pins `spindle_pwm` writes (`host_spindle.c`) - and shown
beside the keypad as `spindle 1000 rpm CW` or `spindle off`. A spindle that is
not being told to run cannot read as running.

MANUAL also shows the two values the jog keys use, beside the pad: `STEP` and
`FEED`, with the one the feed mode is on filled, and `1`/`3` change the filled
one. While a feed runs the pad lights the key holding it, which is what makes a
held key visible at all.

Keys that mean the same thing on every document screen are keys, not slots: `0`
opens the file list on EDIT, TOOLS and RUN, and `B`/`C` step the list, so the
strip keeps only what the screen itself has to say. The list carries text files
beside the programs (the preset entries in `presets\` are text files) - they
open in the editor, but only the program extensions are read as G-code, so a
text file gets no preview and no RUN.

The pad also reports the key the way the keypad driver does: down while it is
held and up when it comes up (a pad click holds the key until the mouse button
is released, a PC key until the key is released). MANUAL's feed mode needs it -
holding a direction key feeds until the key comes up, `nc_visual_hold_key()` -
and a window that loses focus drops the held key, because the machine keypad
cannot lose a release that way.

The window is composed in one memory bitmap and blitted in a single operation:
the panel is one buffer handed to GDI, but the strip is dozens of calls (fills,
rounded keys, text), and painting those straight onto the window put them on the
glass one at a time - the strip visibly blinked while a feed or a run repainted
it every 20 ms. The panel half is redrawn every frame, the strip only when what
it shows has changed (the screen's name, its usage lines, every pad key's
meaning, the spindle - `host_side_signature()`), and the window does not erase
its background first. `--painttest` checks both halves of that: the same screen
composes the same picture, and a screen change redraws the strip.

The PC keyboard maps the same way: F1-F4 modes; digits and the numeric pad; `*`,
`#` and `A`-`D` as the keypad's own keys (asked of the keyboard layout, so they
work on any layout); the arrows move by word on a code screen and step the field
or pick the axis where the screen has fields; Enter = `D`, Esc = `A`,
Backspace = `*`, `-`/`.` sign and point.

The keypad's `#` - the finish key - is on **`W`**, because `#` cannot be trusted
as a PC key: it needs Shift+3 on most layouts and AltGr on the rest, and on some
there is no key for it at all. Delete sends it too, and the layout's own `#`
still works where it exists (`host_pc_machine_key()` is the map that does not
ask Windows). Nothing else on the keyboard is taken: a letter that is not `A`-`D`
or `W` types nothing here, so the program text is still written on the machine's
keypad and the screen's own 3x3.

`W` is not a second key: it is the machine's `#`, so every screen's meaning for
that key applies - on EDIT it is the footer's `VIEW` (the whole-body preview), in
a value field it is `OK`, on MANUAL it is step/feed. `tools/test_nc_ui.py` presses
both and compares the frames: the picture `W` draws on EDIT is byte-identical to
the one `#` draws.

`nc_visual_select_mode()` was added to the NC module for this shell so the
F1-F4 keys jump straight to a mode instead of cycling with the MODE key; the
MODE key still cycles for the machine.

## Headless checks

- `--fstest` lists `/D` through the firmware `fs_*` API.
- `--presettest` checks the preset entries, which are the card's: one file per
  address in `/D/presets` (`41.txt` is G7X `4` then `2`), first row the name,
  the rest the rows to write, a row starting with a space continuing the one
  above. It checks that a card with no folder answers with the compiled entries
  and gets the folder created, that a file replaces its address - name and rows -
  that an empty first row keeps the compiled name, that a free address becomes an
  entry (how a word the pads do not offer is added), that a file with no rows is
  not an entry, and that an address outside the pads' space is ignored.
  `docs/nc-preset-file.md` is the contract.
- `--streamtest` checks the two blocks a jog queues both reach the reader.
- `--padtest` checks the pad is the machine's 4x4 matrix and that every footer
  key of every mode is on it. A screen that offers a key the keypad has not is
  unreachable on the machine too, so this is a check of the key model, not of
  the shell. It also checks that no screen's footer (its own or the file list's)
  needs more than the eight slots the strip is drawn with, and that a screen
  which does not edit the program - RUN, or EDIT's full-screen preview - offers
  no delete key.
- `--keytest` decodes every keypad event byte on both edges (press and release)
  and fails if a release decodes as "no key" - the fault that made every key act
  once and only once on the machine.
- `--uwtest` checks Fanuc's increments, `U` and `W`: the same contour written
  with positions and written with distances has to leave the sender as the same
  lines, plain and inside a `G71` block (so the rows reached the generator as the
  points they mean and the generated motion is identical), an increment whose
  axis was never given absolutely is left as written for the controller to
  refuse, and the document keeps the spelling the operator typed - only the wire
  is absolute. `nc_emit_line_point()` is the one place the rule lives; the
  preview's drawing and its dimension callouts read it through the same
  question the sender asks (`nc_emit_line_is_direct()`), which
  `tools/test_nc_ui.py` checks by rendering the two spellings and comparing the
  drawings.
- `--dirtytest` checks the repaint contract: a key that changes the screen has
  to ask for the draw itself. RUN's line keys (`B`/`C`) did not - they moved the
  run line and returned without the dirty flag, so the highlight sat on the old
  line until the next footer key. The check also covers the other half: while a
  run is armed the panel keeps asking to draw on its own, which is the periodic
  frame that carries the cursor through a stream of emitted lines.
- `--runtest` checks the RUN keys that start a program: `3 FULL` sends the
  fixture's sendable rows in order (the `G970`-`G973` setup rows are skipped)
  and the run is done when the program ends, and `2 FROM` sends the rows from
  the cursor. Both used to arm the run without handing the reader to it, so
  nothing at all reached the machine while `1 SINGLE` worked.
- `--blocktest` walks a fixture that holds a `P/Q` cycle **and** the `G70` below
  it, and reads the **drawn frame** row by row in all three cases - the line in
  play inside the cycle, on the finish cut, and outside both - on **both code
  screens**, EDIT (the cursor is the line in play) and RUN (the sender's line is,
  while nothing is running). The marked line has to be the bright selection
  colour, its path the pale one (`NC_VISUAL_SELECT_BLOCK`) and every other row the
  pane's background; nothing but the marked line may carry the pale colour where
  there is no path. The finish cut is the case that used to mark nothing: its
  path is the numbered range *above* it, which the pane and the preview now get
  from one finder. A row's colour is the modal pixel colour of its text band, so
  the check is on the glass: a snapshot carrying the block while the pane paints
  every row flat fails here - and did. The first version of the screen code
  passed the block bounds to the setter as arguments of the call that fills them,
  and C's unspecified argument order handed over the old values.
  It also checks that the line keys are refused while a run is in flight and work
  again once it is over (`RUN owns the cursor`), and that a step taken from a row
  inside the cycle leaves the mark on **that row** - the whole block goes out,
  but the bright line is the line in play, with the cycle pale around it, and it
  is read again with the machine idle while the sender has moved past the block
  (`now it runs but it marks also next g71. which it should not mark`, and
  `it still marks next row with g71, not the one starting with N50`).
- `--pacetest` checks the sender's pacing: a program is handed to the machine
  one unit at a time, and the next one only goes out once the machine has
  finished the last. It drives the real parser, planner and virtual MCU and
  watches both sides between main-loop passes - the sender must not start a new
  unit while one is running (unless the machine is still collecting the
  contour's rows, which have to keep coming), the mark must never name a line the
  sender has not handed over, the cycle has to be waited for with the mark on its
  header, and the program has to have really run (the machine's position is the
  last line's target). `--runtest` covers *what* is sent, in order; this one
  covers *when*, and it is the check that caught the lazy slot retirement and the
  run-end cancelling a running cycle.
- `--stoptest` checks the MANUAL stops, which are typed rather than armed at the
  current point: `*` opens the minus field, `*` takes it and opens the plus one,
  `*` again takes that; `D` puts the axis limit the setup states in the field and
  an empty field takes that same limit; a step lands on each stop and is refused
  from on it while the other side still moves; and a held feed covers exactly the
  room to the stop it is headed for. `--feedtest` sets its stops the same way.
- `--filetest` loads the NC module's fixtures
  (`uCNC/src/modules/nc/tests/fixtures/facing.nc` and `tool.t`, copied into the
  `--files` root by the test runner) and checks what the eye cannot: the G71
  block scan finds the block, the expansion RUN and the preview share comes out
  of it, the tool table parses, and the `T2` line above the block resolves to
  the R3 O176 tool. The same fixture is then rendered in EDIT and in the
  full-screen view, so the dumps show a real job.

- `--feedtest` runs MANUAL's held feed on the real parser, planner and virtual
  MCU: `1`/`3` change what the next block carries (`STEP` in step mode, `FEED`
  in continuous mode), a held direction key toward the stop queues exactly one
  `$J=G91 <axis><distance-to-stop> F<feed>` block and nothing else while it
  stays down, the controller takes it and the axis lands on the wall without
  crossing it, on the wall the crossing direction is refused while the other one
  feeds away (bounded - the wall is one-sided), letting the key go stops that
  feed, and with no stop set a feed is still a bounded move. What it proves is
  the blocks and the jog state; spindle phase, real travel and the deceleration
  are bench items.
- `--spindletest` checks the station's spindle, which is read from the machine's
  own signals and not from an encoder the desktop does not have: `9` (M3) sets
  the direction signal and puts a speed on the PWM, `7` (M4) sets the other
  direction at the same speed, `5` (M5) leaves no speed on the wire, and the
  figure the signal carries is the figure the rest of the panel reads back
  (`tool_get_speed()`), so a running DRO over a dead signal fails here. What it
  proves is the wiring and the round trip; real rpm and the spindle being on at
  all are bench items.
- `--demotest` checks the demo the release carries (`examples\`): a fresh card
  is seeded from it exactly once and a card already in use is left alone, and
  the sample program itself loads, scans as the two numbered `G71` ranges with
  their `G70` finish cuts, resolves its `T2` from the demo's tool table and
  expands through the emitter RUN and the preview share. A sample that is only
  rows that look right fails here. It also rewrites the demo on the card with
  Windows line endings and requires it to load as the same document: a CRLF
  program used to fail with `too many NC lines` (the loader carried the CR into
  the wrap loop), which is how the first release run of this workflow failed -
  a git checkout on Windows hands the demo over as CRLF.
- `--state` prints what the machine thinks it is doing after the keys and ticks
  have run (`exec`, `run`, `jog`, `hold`, `alarm`, `canceling`, the X/Z figures
  and the spindle, the planner/interpolator/reader fill). It is how a scripted
  feed is checked without opening the window - a feed only starts from a
  standing axis, so `Wait for stop` with `run=1` means the previous jog was
  still moving.
- `--version` prints which build the exe is (its own file's timestamp and size,
  the same line the window title carries). It answers "is this the station I
  just built, or a copy from last night?" without opening the window.
- `--painttest` checks the window's composition: the same screen composes the
  same picture, a screen change redraws both the panel and the strip, and the
  strip is stable across frames (nothing in it rebuilt differently each time).
  It is the check for the flicker the strip used to have when it was drawn
  straight onto the window.

## Screenshot show

`python tools\test_nc_ui.py` runs the checks; `python tools\nc_ui_show.py`
takes the *show*: one frame per action, in the order an operator meets them -
manual jog and feed, opening a file, the 3x3 helper in EDIT, the full-screen
view, the tool table and its own 3x3, and two steps of RUN. Every frame is the
panel alone (800x600, exactly what the machine draws - the key row beside the
panel here is a PC aid and stays out of the images), written as `.bmp` and
`.png` in `tmp\nc-ui-show\`, with a contact sheet and a `SHOW.md` that says what
each frame is. They are rendered from the fixtures above, so a show is the same
job the tests check.

## Open questions

- RUN currently exercises the virtual machine only. Wiring RUN to a real
  controller means giving `nc_run` a Grbl transport (see
  `tools/nc_sender/grbl_stream.c`) instead of the parser stream.
- `host_tests.c` is the station's whole headless harness. It is a flat list of
  independent checks, so it can be split by theme (files and presets, the panel
  frame, RUN and the sender, the editor) when it next grows past the size
  trigger in `AGENTS.md`.

## Where the code lives

| File | Owns |
| --- | --- |
| `main.c` | the window: the emulated panel, the operator's strip, the keypad and the PC keyboard, and the argument line |
| `host_shell.h` (defined in `main.c`) | the emulated machine both the window and the checks drive: geometry, the keypad matrix, the mount root, `--keys`/`--ticks`, the main loop, the dumps |
| `host_tests.c` | every headless check and `host_tests_run()` |
| `host_spindle.h/.c` | the spindle read off the machine's own signals |
| `examples/` | the demo card the release ships and a fresh station seeds itself from |
| `host_fs.h/.c` | the `/D` driver over a PC folder |
| `lvds_host.h/.c` | the host LVDS backend the firmware screen draws through |
| `host_shim.c` | the small amounts of core the host build has to satisfy |
