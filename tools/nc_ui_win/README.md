# nc_ui - NC panel on Windows

Runs the NC screen exactly as the LVDS panel draws it, plus the machine's own
keypad beside it. This is a UI test bench: the firmware layout, coordinate
system, palette and bitmap fonts are used as-is.

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

`host_fs.c` is a real `fs_t` driver: drive `/D` is mapped onto `nc-files\` (or
`--files DIR`), so the file manager, the state store and the TOOLS document
and the preset file behave on the desktop exactly as they do on the card. The
uCNC fs layer splits the drive letter off before it calls a driver, so the paths
that arrive here are already drive relative (`/nc/files`, `/presets.txt`,
`/` for the root) - the shape FatFs sees on the machine. The driver closes
handles only; uCNC's `fs_close()` releases the driver's memory, the same
contract `sd_card_v2` relies on.

Files are opened in binary mode. The firmware's readers mix reads with seeks and
ask `fs_available()` for what is left; in text mode Windows translates bytes and
makes `ftell`/`fseek` positions meaningless, so `fs_available()` reported zero
after the first read and every file loaded as a single line.

```powershell
build\nc_ui.exe --files tmp\ncroot --fstest      # list /D through fs_*
build\nc_ui.exe --files tmp\ncroot --presettest # check /D/presets.txt
build\nc_ui.exe --streamtest                     # a jog delivers both blocks
build\nc_ui.exe --padtest                        # the keypad is the machine's
```

## Build and render a frame

```powershell
python tools\test_nc_ui.py     # builds, dumps a panel frame and a bench frame,
                               # then runs the headless checks below
```

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
`A`-`D`), the modes `F1`-`F5`, and named keys such as `ACCEPT`, `NEXT`,
`FINISH`, `BACK`, `CANCEL`, `MODE`, `UP`, `DOWN`, `LEFT`, `RIGHT`, `MINUS`,
`DOT` - before the frame is drawn. `HOLD<key>` presses a machine key and keeps
it down, `RELEASE` lets it go, so a held feed can be scripted as well; `WAIT<n>`
runs the machine for `n` ticks first.

```powershell
# MANUAL: stop at X0, eight X+ steps, continuous feed, feed 500 -> 50, hold X-
build\nc_ui.exe --files tmp\shots --dump-bench tmp\feeding.bmp ^
    --keys "F1,*,8,WAIT2,8,WAIT2,8,WAIT2,8,WAIT2,8,WAIT2,8,WAIT2,8,WAIT2,8,WAIT2,#,1,1,1,1,HOLD2,WAIT5"
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
sits next to the display. The keys are the hardware's, not a second menu:

| Keys | Function |
| --- | --- |
| F1-F4 | operation modes: MANUAL, EDIT, TOOLS, RUN |
| keypad | the 4x4 matrix `cam_keyboard.c` decodes, row 1 on top: `* 0 # D` / `1 2 3 C` / `4 5 6 B` / `7 8 9 A` |

Every pad key does exactly what the machine key does: the character goes through
the screen's own key table (`nc_visual_key_for_char()`, the same one
`nc_module.c` uses), and the label under the key is what the active screen says
the key means - the footer entry that carries that key, or the screen's own hint
for a key the footer does not name (MANUAL's jog digits: `7`/`9` spindle,
`2`/`8` X, `4`/`6` Z, `5` spindle stop, `1`/`3` the value on screen). A key the mode
does not use stays unlabelled. Nothing about the meanings lives in the shell, so
a mode that renames a key renames it here too.

MANUAL also shows the two values the jog keys use, beside the pad: `STEP` and
`FEED`, with the one the feed mode is on filled, and `1`/`3` change the filled
one. While a feed runs the pad lights the key holding it, which is what makes a
held key visible at all.

Keys that mean the same thing on every document screen are keys, not slots: `0`
opens the file list on EDIT, TOOLS and RUN, and `B`/`C` step the list, so the
strip keeps only what the screen itself has to say. The list carries text files
beside the programs (`presets.txt` included) - they open in the editor, but only
the program extensions are read as G-code, so a text file gets no preview and no
RUN.

The pad also reports the key the way the keypad driver does: down while it is
held and up when it comes up (a pad click holds the key until the mouse button
is released, a PC key until the key is released). MANUAL's feed mode needs it -
holding a direction key feeds until the key comes up, `nc_visual_hold_key()` -
and a window that loses focus drops the held key, because the machine keypad
cannot lose a release that way.

The PC keyboard maps the same way: F1-F4 modes; digits and the numeric pad; `*`,
`#` and `A`-`D` as the keypad's own keys (asked of the keyboard layout, so they
work on any layout); arrows line/field; Enter = `D`, Esc = `A`, Backspace = `*`,
Del = `#`; `-`/`.` sign and point.

`nc_visual_select_mode()` was added to the NC module for this shell so the
F1-F4 keys jump straight to a mode instead of cycling with the MODE key; the
MODE key still cycles for the machine.

## Headless checks

- `--fstest` lists `/D` through the firmware `fs_*` API.
- `--presettest` checks the `/D/presets.txt` contract (materialise, edit, fall
  back on an unparsable file).
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
- `--state` prints what the machine thinks it is doing after the keys and ticks
  have run (`exec`, `run`, `jog`, `hold`, `alarm`, `canceling`, the X/Z figures,
  the planner/interpolator/reader fill). It is how a scripted feed is checked
  without opening the window - a feed only starts from a standing axis, so
  `Wait for stop` with `run=1` means the previous jog was still moving.

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

- Run currently exercises the virtual machine only. Wiring RUN to a real
  controller means giving `nc_run` a Grbl transport (see
  `tools/nc_sender/grbl_stream.c`) instead of the parser stream.
