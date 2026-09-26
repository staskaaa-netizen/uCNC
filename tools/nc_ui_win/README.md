# nc_ui - the programming station on Windows

Runs the nc2 screen exactly as the LVDS panel draws it, plus the machine's own
keypad and the screen's own usage notes beside it: the machine on the left, the
operator's desk on the right. The firmware layout, coordinate system, palette
and bitmap fonts are used as-is, so what the window shows is what the panel
shows - which is what makes it a test bench as well as a station.

The station is built for the PC because that is where a program is written and
read before it is cut: the same nc2 module, the same G7x cycles, the same key
meanings. It carries no second copy of a screen's rules - the keys, the usage
lines and the spindle signal all come from the firmware sources.

## How the layout stays identical

`modules/nc2/nc2_visual.c` draws through a small primitive API (`lvds_hstx_*` and
`lvds_draw_*`). On the machine the RP2350/HSTX backend implements it; here
`lvds_host.c` implements the same API over a 32bpp frame that GDI blits into the
window, and the firmware bitmap fonts (`lvds_font_cond_6x8.c`,
`lvds_font_ibm_8x14.c`) render the glyphs. Nothing in the screen code changes,
so a host frame is what the panel shows.

The rest of the stack is the real firmware too, built for the virtual MCU the
same way the parser test suite builds it: core, parser, nc2's state/run/MANUAL,
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
build\nc_ui.exe --streamtest                     # a jog delivers both blocks
build\nc_ui.exe --keytest                        # every key decodes on both edges
build\nc_ui.exe --painttest                      # the bench is composed once
build\nc_ui.exe --seedtest                       # nc2's first start seeds a card
build\nc_ui.exe --edit2test                      # nc2's fields and value editor
build\nc_ui.exe --pad2test                       # nc2's pad is the file tree
build\nc_ui.exe --screen2test                    # nc2's screen draws and writes
build\nc_ui.exe --file2test                      # nc2's card list walks and opens
build\nc_ui.exe --emit2test                      # the sender still makes what nc made
build\nc_ui.exe --run2test                       # nc2's run and its own strip
build\nc_ui.exe --live2test                      # the tool takes the stock off
build\nc_ui.exe --manual2test                    # nc2's jog panel
build\nc_ui.exe --tools2test                     # nc2's tool table is a file
build\nc_ui.exe --contour2test                   # nc2's path builder (4 then 7)
build\nc_ui.exe --block2test                     # the line in play, and its block
build\nc_ui.exe --label2test                     # the DRO, and the state said once
build\nc_ui.exe --pace2test                      # one unit, then wait
build\nc_ui.exe --files tmp\demo --demo2test     # the demo seeds and expands
build\nc_ui.exe --dump-nc2 screen.bmp            # nc2's screen on its own
python tools\test_nc2.py                         # nc2's own target (AGENTS.md 8)
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
examples\presets\*.txt         the words the keys write, one file per address
```

`examples\` is also the station's first-run demo: a card with no program of its
own is seeded from the folder beside the exe (`host_seed_card()`, once, without
overwriting anything), so an unpacked zip opens with something to look at.
`--files DIR` names another card; a card that already has a program is never
touched.

`examples\presets` is the second half of that: the entries the panel ships,
written out as the card's own files (`--dump-presets`, generated from the
module's own table so the two cannot drift - `tools/test_nc_ui.py` regenerates
the folder and compares it). `host_seed_presets()` copies them into a card whose
`presets` folder holds no entry file yet, which is a rule of its own: an
operator who has been using the station already has programs, and that is
exactly the card whose `presets` folder is empty. Nothing is overwritten, and
a deleted file simply means that address is not an entry any more.

`lathe-demo.nc` is one of the machine's own runs, written down as a file: the
preview setup rows, a tool and a spindle, one rapid, and two numbered `G71`
ranges - each finished by its own `G70 P Q` - with an `R` round, a `C` chamfer
and a straight finishing move. It is not a bench program: it is a program to
open, walk with the cursor, preview and watch RUN send. `--demo2test` checks it
loads, scans as two cycles and expands, and that the seeding happens once.

Every line in the demo is short enough for the panel's own line width
(`NC_WRAP_LINE_LEN`, 46 characters): a longer line is wrapped with a tab and the
screen shows two rows, which is fine for a program and ugly for a sample.

`--dump out.bmp` writes the 600x800 panel frame - the firmware layout as the
operator reads it, nothing of the shell. `--dump-bench out.bmp` writes the whole
900x800 bench: the panel
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

That one is EDIT followed by the keypad's `4`, which in EDIT is the pad's
`4 G7X`, so the dump is the floating 3x3 helper with its labelled line.

Or straight from the tool directory:

```powershell
cd tools\nc_ui_win
mingw32-make
build\nc_ui.exe --dump frame.bmp
build\nc_ui.exe
```

## Window layout

The window is the emulated panel with the machine's keypad attached to the right
side, the way the proprietary keyboard sits next to the display. This is the
*programming station* side of the tool: the panel is the machine, and the strip
beside it is the desk. The keys are the hardware's, not a second menu:

The panel half is **600x800**: the glass is 800x600 and is mounted turned a
quarter, so the window shows the picture the operator reads, not the framebuffer
the scanout reads (`lvds_hstx.h` owns the turn, and `lvds_host.c` and this window
go through the same statement - a `--dump` is written upright for the same
reason). On that picture: the header band across the top - the four screens with
the one in play wearing its block, the file, and everything the screen has to
say at the right end, and the frame meter in the far corner (dim, small, a debug
reading - the bench asked for it back to test what a change costs the panel) -
the drawing under it, the machine's own strip across the middle (work X and Z,
feed, spindle, and the state word), and then the program on the left with the
3x3 in the corner beside it and the screen's helpers in the space above them.

| Keys | Function |
| --- | --- |
| F1-F4 | operation modes: MANUAL, EDIT, TOOLS, RUN |
| MODE | the machine's own `A`: up a level in the pad, and the mode key at the root |
| keypad | the 4x4 matrix `cam_keyboard.c` decodes, row 1 on top: `* 0 # D` / `1 2 3 C` / `4 5 6 B` / `7 8 9 A` |

Every pad key does exactly what the machine key does: the character goes through
the screen's own key table (`nc2_visual_key()`, the same call `nc2_module.c`
makes) and the label under the key is what the active screen says the key means -
the entry that key writes, or the screen's own word for a key no entry carries
(MANUAL's jog digits: `7`/`9` spindle, `2`/`8` X, `4`/`6` Z, `5` spindle stop,
`1`/`3` the value on screen). A key the screen does not use stays unlabelled.
Nothing about the meanings lives in the shell, so a screen that renames a key
renames it here too.

The strip answers "what is this key" for the whole keypad, including the keys
the menu does not name:

* the screen's name and a few lines saying what it is for and how its keys
  drive it - `nc2_visual_screen_name()` and `nc2_visual_usage()`, the screen's own
  words, so a screen that renames a key or gains one renames it here too. The
  usage lines are what an operator reads instead of a manual: the off-menu keys
  (`B`/`C`, `#`) are named there;
* the keypad's step keys are drawn with the arrow they act as, because on those
  screens they are the arrows: MANUAL's axis pick, and the line walk everywhere
  else (`nc2_visual_key_meaning()`);
* `nc2_visual_key_meaning()` is the one answer for all of it - the label, the
  screen's own word, and whether the key steps a field - so the strip and any
  future shell read the same table.

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
holding a direction key feeds until the key comes up, `nc2_visual_hold_key()` -
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
work on any layout); the arrows send the machine's own `B`/`C` (line, and the
sign and the point while a value is picked) and `Right` the field walk `D`; Enter
= `D`, Esc = `A`, Backspace = `*`. There is no second key table: what the PC
sends is what the keypad sends.

The keypad's `#` - the finish key - is on **`W`**, because `#` cannot be trusted
as a PC key: it needs Shift+3 on most layouts and AltGr on the rest, and on some
there is no key for it at all. Delete sends it too, and the layout's own `#`
still works where it exists (`host_pc_machine_key()` is the map that does not
ask Windows). Nothing else on the keyboard is taken: a letter that is not `A`-`D`
or `W` types nothing here, so the program text is still written on the machine's
keypad and the screen's own 3x3.

`W` is not a second key: it is the machine's `#`, so every screen's meaning for
that key applies - in a value field it is the type key, on the file list it opens
the selected file, on MANUAL it is step/feed. `tools/test_nc_ui.py` presses both
and compares the frames: the picture `W` draws is byte-identical to the one `#`
draws, and the key really did something (the list is gone).

`nc2_visual_select_mode()` lets the F1-F4 keys jump straight to a screen instead
of cycling with the `A` key; the mode key still cycles for the machine.

## Headless checks

**Pre-flight with the compiler the CI uses.** This build passes `-w` (the
firmware module sources carry warnings this tool does not own), so a missing
`#include` is silent here - and gcc 14 and newer make an implicit function
declaration a *hard error*, which is how a check first broke the release
job instead of the local build. The CI's toolchain is MSYS2's MinGW-w64; before
pushing, build with it the same way:

```powershell
$env:CC = 'C:\acc\openconnect\msys64\mingw64\bin\gcc.exe'
python tools\test_nc_ui.py
```

- `--fstest` lists `/D` through the firmware `fs_*` API.
- `--dump-presets DIR` writes every entry the panel ships into `DIR`, one file
  per address, in the format `/D/presets` reads: this is where
  `examples\presets` comes from, and `tools/test_nc_ui.py` regenerates it and
  compares it byte for byte, so the shipped files and the module's own first
  start cannot drift.
- `--streamtest` checks the two blocks a jog queues both reach the reader.
- `--keytest` decodes every keypad event byte on both edges (press and release)
  and fails if a release decodes as "no key" - the fault that made every key act
  once and only once on the machine.
- `--seedtest` checks nc2's first start: a card with no entry file gets the
  entries the module ships, written once (`nc2_boot_seed()` lays down the table
  in `nc2_boot.c`), the logo stands while it happens and leaves on its own, a
  folder that already holds an entry is never touched, a file that is there is
  never replaced, and deleting a file is how an address stops being an entry -
  there is no compiled table behind the files to fall back on.
- `--edit2test` checks nc2's value editor: a line cuts into one field per letter
  (comments are not fields, a lone letter is a field waiting for a number), `D`
  walks them, the first digit typed replaces the value and the rest of the line
  stands still, `B`/`C` are the sign and the point while a value is picked (the
  keypad has no `-` and no `.`), the cursor and the line keys work with nothing
  picked - and the pad's helper writes its name as a line under the cursor, lands
  the entry's rows where that name stood, and takes the name back when the pad is
  left without a pick.
- `--pad2test` checks that nc2's pad *is* the file tree: the digits walked are
  the address and the file with that name is the slot, a group's label is its
  file's first row, a slot that holds children opens and one that holds rows does
  not, pressing an entry writes it where the pad's name stood and the pad stays
  for the next press (which lands under it), `A` steps back up, and a slot nobody
  wrote a file for simply is not there.
- `--screen2test` checks nc2's screen: it draws a program down the left and the
  pad's corner on the right (and nothing where the footer used to be), `4` walks
  into G7X and the pad's name is written as the line the entry will land on, `1`
  writes that entry where the name stood with the pad still up, `D` and a digit
  type over the value the entry landed with, `A` goes back up a level - and the
  program that ends on the card is the one the screen wrote. `--dump-nc2 out.bmp`
  renders nc2's screen on its own, the way `--dump` does for the panel.
- `--file2test` checks nc2's card list: `0` opens `/D` with its folders in it and
  no `..` (the root is as far up as it goes), `C`/`D` step and enter a folder,
  opening a program puts it in the editor and closes the list, `5` then digits
  then `#` makes a numbered program in the folder being listed and opens it, `6`
  deletes the selected file, and `8` reads the folder again so the deleted file
  is gone from the list.
- `--emit2test` checks the sender: the expansion of a program with two roughing
  cycles and a finish cut each, the same program started mid-file, and a contour
  written with Fanuc's `U`/`W` increments all have to come out as the lines
  `EMIT_GOLDEN_*` holds - the answer nc gave for the same programs, line for
  line, taken while both modules were still built. The increments have to leave
  as the absolute words the controller reads.
- `--run2test` checks nc2's run: `3 FULL` hands the controller exactly what
  `nc2_emit` says the program means (the generator's own `(G71 rough X23.000)`
  notes are a line of the stream, not a line of the program, so they are not
  sent), the machine really runs it one unit at a time and arrives where the
  program says - the program's X is a diameter and the axis works in the radius,
  so the machine's figure is half - the strip wears the machine's grey while
  nothing happens and the run's green while it is armed, and the run entered
  through the mode key (EDIT, TOOLS, RUN) holds the *program*, not the tool
  table the screen before it had open. `1 SINGLE` sends the unit the mark is on
  and leaves the mark on the line the operator stepped from.
- `--live2test` checks the live stock, off the glass: the tool is parked off the
  stock with the panel's own sender, the idle RUN screen is read column by
  column for the stock's colour, a taper is cut with the machine running and the
  screen drawing every turn of its loop, and the stock's columns are read again
  - the cut is where the mask puts it (material gone from the bottom, the top of
  every column where it was), the parked RUN screen still holds the part, and
  EDIT draws the whole stock. No picture is compared: the counts of the stock's
  own pixels are.
- `--fps2test` checks the frame meter: the screen draws a second of frames -
  one per millisecond of the panel's own clock - and the meter has to have
  counted them, with the reading drawn in the header's far corner. It also
  damages a pixel in the pad's corner and draws again: a frame that changes
  nothing has to leave it (only the drawing and the strip are painted), and a
  key has to paint over it.
  The meter logs its breakdown to the console once a second
  (`[MSG:NC2 fps ... draw ... screen ... stock ... geom ... strip ... present]`,
  milliseconds per frame) - the machine's console is where a frame's cost is
  read, and the test only insists the count is right.
- `--scroll2test` checks the code pane's scroll: twenty lines down a thirty-line
  program, the cursor's row has to be six rows short of the pane's last, and the
  six rows under it have to carry the next lines.
- `--fault2test` checks what a refused line says: a program whose third line is
  `G0 X` (a word with no number - the controller answers `error:2`) has to end
  with the screen reading `Line 3 error 2: Invalid number`, and the sentence has
  to be in the notes above the 3x3 in the fault's red.
- `--manual2test` checks nc2's MANUAL: the digits jog the axis each names (X is
  a diameter, so a 0.100 mm step is written `X0.200`), a jog is always the
  `G91 G1 ...` block followed by the `G90` that puts the machine back, `1`/`3`
  change the step the next press uses, the spindle keys send `M3`/`M4`/`M5` at
  the remembered speed, `*` types the two stops of the picked axis, `#` swaps a
  step for feeding and a held direction key sends one `$J=G91` block toward the
  stop, `0` zeroes the axis and `D` touches it off.
- `--tools2test` checks that nc2's TOOLS is the editor on the tool table: the
  file is made with the shipped row when the card has none, a pad press lands in
  it, leaving TOOLS leaves the program where it was, and the screen is the
  bench's two halves: the table's rows in the top pane and the tool the cursor
  is on drawn in the one below, in the tool's own colours.
- `--contour2test` checks the path builder (G7X's `7`, the address `47`) and the
  pad's own order: the digits run up (`7 8 9` is the top row), one press writes
  one `G1` row with the axis that does not move carried over from the row above,
  the value that lands is picked so the digits type over it, `#` steps the
  distance, `*` drops the point being entered, and `5` ends it with the rows
  still in the program.
- `--case2test` checks the card's own names: the machine's FatFs is built without
  long filenames, so what a directory hands back is 8.3 with capitals
  (`LATHE-~1.NC` for a file written on a PC as `lathe-demo.nc`). The extension
  question is case-insensitive, so a card full of old programs is listed, opened
  and run - and the preset scan asks the same question, or a card in use would be
  seeded again on every boot.
- `--walk2test` walks the card the way the operator does - into a folder and back
  out with `..` - and checks that asking for the folder above *answers* it
  instead of scanning it and returning nothing, which had the caller scan an
  uninitialised path. Every list the panel reads is logged to the console
  (`[MSG:NC2 list + NAME]`, `- NAME`, and a summary line), because the list a card
  hands back is the one thing the panel cannot show the operator.
- `--block2test` reads the marks off the drawn frame on both code screens: the
  line in play is the bright selection colour, the rows of the block it heads are
  the pale one, and everything outside both is the pane's own ground. A mark the
  snapshot carries but the pane never paints passes a check on the document and
  still is not there, so this reads the glass.
- `--label2test` checks the machine's strip: the panel's own colour with `IDLE`
  on it while nothing happens, green while a run is armed, and the fault's red
  with the state word still drawn on it when the machine faults. It also pins the
  bench's "i do see idle in two places ... only this one should remain": the
  header band never wears the strip's colour, so the state is said once.
- `--pace2test` checks the pacer: a line may only be handed over while the
  machine is still running when it belongs to a block the sender is expanding (a
  contour is one cut), the mark never names a line the sender has not handed
  over, and the program ends with the machine where the program says. It reads
  the machine where the pacer reads it - between the parse and the tasks - so a
  unit that ends inside a pump cannot look like one that had not.
- `--demo2test` checks the demo the release carries (`examples\`): a fresh card
  is seeded from it exactly once, the sample loads, scans as the two numbered
  `G71` ranges with their `G70` finish cuts and expands, and a pad press writes
  the card's own entry row rather than a table compiled in.
- `--painttest` checks the window's composition: the same screen composes the
  same picture, a screen change redraws both the panel and the strip, and the
  strip is stable across frames (nothing in it rebuilt differently each time).
  It is the check for the flicker the strip used to have when it was drawn
  straight onto the window.
- `--version` prints which build the exe is (its own file's timestamp and size,
  the same line the window title carries). It answers "is this the station I
  just built, or a copy from last night?" without opening the window.
- `--painttest` checks the window's composition: the same screen composes the
  same picture, a screen change redraws both the panel and the strip, and the
  strip is stable across frames (nothing in it rebuilt differently each time).
  It is the check for the flicker the strip used to have when it was drawn
  straight onto the window.

## Screenshot show

`python tools\test_nc_ui.py` runs the checks; the *show* is the same idea with
an eye on it - one frame per action, in the order an operator meets them. Every
frame is the
panel alone (800x600, exactly what the machine draws - the key row beside the
panel here is a PC aid and stays out of the images), written as `.bmp` and
`.png` in `tmp\nc-ui-show\`, with a contact sheet and a `SHOW.md` that says what
each frame is. They are rendered from the fixtures above, so a show is the same
job the tests check. (The script that drove the show listed nc's screens and is
being brought over to nc2; the panel dumps it needs are all here.)

## Open questions

- RUN currently exercises the virtual machine only. Wiring RUN to a real
  controller means giving `nc2_run` a Grbl transport (see
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
