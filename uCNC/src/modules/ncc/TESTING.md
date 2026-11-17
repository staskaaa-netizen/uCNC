# ncc - how it is checked

`ncc` is the third NC module: nc2's screens, with the entries held in RAM, the
preview switched by `#`, and one layout file for both orientations. Its whole
software check is one command:

```powershell
python tools\test_ncc.py
```

It needs a host C compiler (`CC`, `gcc` on `PATH`, or one of the MinGW trees
under `C:\acc` - the script looks) and PlatformIO. The script builds the
firmware twice, runs the host test twice, builds the station, runs the module's
checks and compares the shipped entries file; the log of each piece is left in
`tmp/ncc-tests/`. It also reads the portrait firmware's ELF with the
toolchain's `nm` and fails if the float formatter is not linked
(`check_float_printf()`): the host links glibc and cannot see that one, and
without it every number the panel prints is an empty string on the machine -
which is what handed the controller `G0 X` instead of `G0 X55.000`.

## What is software-verified

| check | what it proves |
| --- | --- |
| `pio run -e RP2350-LEANCAM-LVDS-NCC` | the module compiles and links for the board, portrait, and `LOAD_MODULES_OVERRIDE()` loads `ncc` |
| the portrait firmware's ELF, read with `nm` | the float formatter is linked: `%.3f` prints a number on the machine and not an empty string (`check_float_printf()`, bench 2026-09-27) |
| the same env with `-DLVDS_PANEL_TURN=0` | the landscape branch of `ncc_layout.h` compiles and links - the second shape is not dead code |
| `pio run -e RP2350-LEANCAM-LVDS` | nc2 still builds after `module.c` / `cnc_hal_overrides.h` learned about `ENABLE_NCC_MODULE` |
| `pio run -e RP2350-G7X-MODULE` | the NC-free target still builds with `ncc` excluded from its sources |
| `tools/ncc_ui_win/` + the checks below | the panel itself: every screen, the pad, the run, the live stock, the sender, MANUAL, the tool table - and the frame dumps |
| `ncc_host_test.c`, both shapes | the shipped table in RAM, the one card file's rules, the dumped file read back as the table, and the layout's arithmetic |
| `--dump-presets` compared with `examples\ncc_presets.txt` | the release's readable copy of the entries is exactly what the panel's table writes |
| source-budget check | every handwritten `ncc` C source stays below the repository's 2,000-line trigger; the generated `ncc_boot_image.c` is data and is excluded |
| module-budget check | the handwritten `ncc` module stays below the 10,000-line module trigger after the `nc_sender` extraction |
| `python tools\test_nc_sender_module.py` | the extracted sender builds and runs without the panel, document model, filesystem, framebuffer or controller |

## Bench verification

2026-11-16: with the controller held in the E-stop reset wait, the NCC boot
picture was visible on the panel. This verifies that panel updates run while
motion is locked out; the host suites cannot exercise that controller task
ordering.

The same Batch 1 board also mounted the SD card as `/D`, ran the three-axis
STEP/DIR/shared-enable path through the shift registers, and produced 4.68 V on
AO0 for `M3 S450` with `$30=1000`. The general-output and input-chain electrical
paths work, but their physical-to-logical mapping is still a Batch 1 follow-up.

The `E0:`, `E1:` and `E2:` fields are bench items: turn each encoder one detent
and verify that its displayed count changes, then stays still with the shaft
stationary. ENC0 can also be compared with the standard serial `EC` field.

`AI0:` through `AI3:` show input volts for J24, converted from the 10-bit ADC
reading at 13.6 mV per count. Apply a known input voltage and compare the
displayed value with the 100k/33k divider calculation; this is a screen-only
diagnostic and must not add fields to the serial status.
Batch 1 measured approximately `361` at 5.0 V input and `250` at 3.3 V input,
confirming the divider path and polarity. Full two-point gain/offset
calibration remains a bench item.

## The module's checks

`tools/ncc_ui_win/` is nc2's station forked for this module; the 39 checks are
nc2's, with the ones that were wired to nc2's folder-and-seed entries rewritten
for ncc's one-table-in-RAM model. The full run is the release check; a bench
loop can ask for one (or several) without building the firmware:

```powershell
python tools\test_ncc.py --brush2test
python tools\test_ncc.py --frame2test
```

| flag | what it pins |
| --- | --- |
| `--seedtest` | the shipped entries answer from RAM, the card is not written, and the one card file overrides only the addresses it names |
| `--preview2test` | `#` switches the drawing with nothing picked, and a picked value keeps the key (the accept) |
| `--gcode2test` | a typed `G` code becomes its own line with its words (a general `G1` is `G1 X0 Z0 F0`, not the contour's `C R`), the first value picked, and the sentence in the notes; `G21` is named and left alone; the `WORD` pad's `F` adds the feed to the line the cursor is on - it is the *entry* that ends the pad, not the key; and travelling over `G41`, a bare `G71` and `M2` changes none of them, because only a code that was just *typed* resolves |
| `--chamfer2test` | the pad's keys are square, and the top-right corner is page background a quarter of the key down |
| `--refuse2test` | a cycle the panel's own generator refuses is marked on its own line, the line `1 SINGLE` has to step from |
| `--blockline2test` | a run's line is the block it came out of: the cycle's own motions answer for its header, a line handed over as it stands answers for itself, and none answers past the cycle |
| `--colour2test` | the word being typed is white on the panel's black and carries its *own* letters (not the renderer's `...`), and its legend is black on the pale yellow band |
| `--chuck2test` | the plain dark-grey jaws are on the glass at all, and `G972 C` sizes them |
| `--boot2test` | the start-up picture is painted in its own colours with the build's stamp under it, and it hands the palette back - by a key and by its clock both |
| `--firstline2test` | the file's own row above line 1: line 1 keeps its mark while the legend shows, `B` selects the row, and `D` opens the file's folder with the file selected |
| `--edit2test`, `--pad2test`, `--screen2test`, `--file2test` | the document, the field editor, the pad's join rule (a leading-space row finishes the line the press began on - at its end, or at its front for `N` - and leaves the word picked; every other row stands on its own line), what a landed row leaves picked (its first *value*: `U0` on a cycle, the tool number on a table row, and nothing at all on `G41`/`M2`, so `B`/`C` stay the cursor's), the press that writes ending the pad with the cursor on the row it wrote, the pad's tree (and that the table's group is only on TOOLS), the program it writes, the card picker: `0` lands on the open file's own folder with the file picked, `8` refreshes without moving the pick, a selected `.nc` previews without replacing the active document, a folder or text pick leaves that document active, malformed `SETUP`/`TOOLCALL` macro text is refused and the pane says “Unable to generate preview”, and the screen the panel was left on is the one it comes back to |
| `--emit2test` | the sender still makes what nc made, line for line |
| `--run2test`, `--pace2test`, `--block2test` | what a run hands over, one unit at a time, and the marks read off the glass. `--run2test` strips the sender's own `N` from every block and insists each had one (that number is how the pane follows the machine), proves `0` reaches the file picker even on an empty RUN document, proves the RUN file row takes `B`, stays on `B`, opens with `D`, returns on `C`, and cannot be taken while `EXEC_RUN` is active, proves the selected program survives a RUN→TOOLS→RUN round trip, proves `B`/`C` still select a row while HOLD is set but not executing, proves a fresh `FULL` clears a stale HOLD, `--pace2test` pins the run's unit rule (`g7x_doc_block_containing`) by building its list of markable lines from it - the mark only moves onto a block's own first line or a line outside every block, only ever forward, and a row of a block goes out only while that block is being expanded - and asserts the mark *is* the line the controller reports executing (`ncc_run_executing_line()`), so the pane cannot follow the sender instead of the machine |
| `--runlock2test` | the run owns the file: RUN -> EDIT in the middle of a cut keeps the buffer the pacer is feeding (the card is given a *different* program behind the panel's back, so a switch that re-reads it is read off the machine's own end position), every key that would write the text answers `Program running - the file is the run's` and none of them lands, and the keys that only look (`#` for the drawing, `B`/`C` for the cursor) still work |
| `--live2test` | the tool takes the stock off with the **nose's own circle** as the brush - the cut's bound is the circle's inner arc, so an end comes out in the tool's radius, and the cut's edge reaches one nose radius past the contour's last point - with the **tip glyph** (the insert's filled body and its nose's disc, drawn where it lands with each row clipped to the pane rather than slid inwards) riding it on the glass, taken back with the same box so no frame leaves a trail; all four amber depth bands are present after the cut, the top stays, the cached dirty-column redraw matches a whole redraw, and a parked run keeps the part |
| `--brush2test` | the clearing brush against the coordinates: the same program (a face, an OD and a shoulder, `T1 R3`) is drawn along a `G41` path, a `G40` path, and a `G40` path whose machine frame is moved by a live `G54` offset. The last frame before the measurement is parked, so the glyph is not in the frame. The rule is `top = sy - h` in both modes: `G41` runs the machine centre on X26 and the tangent lands on **X20**; `G40` runs the corner on X20 and the tangent is one nose radius above it, **X14**; the `G54` pass repeats X14. Z stays at the shoulder's Z-20 within the same tolerance |
| `--frame2test` | the program's frame against the drawing's: a `G8` program writes the same part as `X10` where the `G7` one writes `X20`, and both profile lines land on the stock's `X20` row (read off the glass at `Z-10`, not from the different labels) |
| `--glyph2test` | the run's glyph is the **nose circle only**: it is on the machine's point, and the old body/flank area is stock |
| `--manual2test` | the jog keys, the stops, the spindle, zero and touch-off |
| `--offset2test` | the editor's offset line, and where its radius comes from: the card's tool table, read on the program screen the same way RUN reads it - not a built-in row. The program names its tool (`T1`): `R0` draws no shift line and `R3` draws one *off* the programmed profile |
| `--comp2test` | the tool a cycle cuts with, and when its radius is read: the card carries `T1 R2` and `T2 R3`, the program calls `T2` **below** its `G42` - the bench's own order, `M3` / `G42` / `T2` / `G0` / `G71` - and both cycles' finish contours come out on the tool-centre path `X20 - 2*R3 = X14`. Read as `T1` the cut is `X16` (the table's first row, a millimetre shallow), and read at the `G42` word itself there is no tool in the spindle yet and the run refuses: the radius belongs to the contour, so the same program cuts the same either way, and the run's own tool record is `T2` |
| `--state2test` | the tool and the compensation a screen *wears* are G7x's state machine, asked at the machine's own line: two cycles (`T2`/`G41` then `T1`/`G42`) are armed and cut one block at a time - `1 SINGLE`/`2 FROM`'s path - and each arms with its own rows' words before a move goes out (the old code armed the second cycle from a global `ncc_run_start()` had just zeroed: `T-1 G40`, and it cut `X20` where the `G42` offset is `X16`), holds that state while the machine cuts the block (the sender is a block ahead; the state is not), and really cuts its own tool-centre path (`X26` for `T2 R3` under `G41`, `X16` for `T1 R2` under `G42`). Two `G40` shapes come with it, because "no shift" has to be visible in the pane: a plain program with `T2 R3` corners on X20 and leaves the material's edge at `X14.25` (read off the glass), and a compensation word written *among a cycle's contour rows* changes neither the cut nor the words after the block. What the host cannot reach: a run that **hands over** a `T`/`M3` line (the interpreter's `itp_sync()` needs the machine's clock and spindle), so a full run of such a program is a bench item |
| `--tools2test` | the tool table is a file the editor writes; the pad is the row editor (`1` writes a new row with its tool number picked, `2` picks the row's `R`, `3` its `F`); the view shows the row's fields, the cursor follows the machine's tool, and leaving TOOLS returns to the program |
| `--label2test`, `--fault2test`, `--scroll2test`, `--fps2test` | the strip's three colours; the refusal as nc's block of white on red, a full-length message (`NCC_STATUS_MAX`) drawn in the block, the text the panel handed over for the refused line named under it (`ncc_run_refused_text()`), the run's mark staying on the line that failed through `# RELOAD`, and the editor keeping its own line; the six rows after the cursor; the frame meter |
| `--demo2test` | the demo card seeds, the sample scans as two cycles and expands, and a key writes the shipped entry |
| `--contour2test` | the path builder, `#` as its step key, `*` and `5` |
| `--case2test`, `--walk2test`, `--vocab2test` | a real card's names, the folder walk, the word legends |
| `--present2test`, `--pump2test` | every screen reaches the panel, and the screen's own loop draws only when it should |
| `--dump-boot FILE` | writes the start-up picture as a .bmp (the stamp included), so the splash can be looked at without the station's window |

## Bench (needs the machine)

None of the following can be decided from the source or the host:

- a pad label and an insert with the card in the socket and with it pulled: the
  RAM table should make both immediate;
- the one card file on a real FAT card, including one the operator edited in the
  panel's own editor;
- the pad's word keys on a line the operator is already on: `F`, `C` and `R`
  must land at the end of that line and `N` in front of it, each with its value
  picked, and the digits must replace that value - the host checks the document
  and the picked field, not the operator's finger on the glass;
- and that a key press really does hand the pad back: after an entry the next
  digit must open a group again, with the keys showing the root;
- and which word is picked after a row lands: the operator's `B`/`C` must move
  the cursor on a line that carries nothing to type (`G41`, `M2`), and must not
  on a line whose value is still being typed;
- the preview's **arcs** against the emitted path: a programmed corner radius
  (`R`) is emitted as a `G2`/`G3` with `I`/`K` (checked in G7x's own suite) and
  the preview walks that arc point by point - a chamfer on the glass where an
  `R` was written is the preview's fault, not the cycle's (bench, 2026-09-28:
  *"gren line of ofssete path for rnd shows chamfer path"*);
- the **editor's own line on the drawing**: the contour point of the row the
  cursor is on has to read as a filled dot (the others are rings) and that row's
  emitted motion as a wide line - the module draws both, the glass is the check
  (bench: *"marking active select line in edit to line on preview screen"*);
- the live frame with the **nose circle only**: the run shows the mask and the
  circle, and the frame meter should be back near 40 fps. The TOOLS screen has
  no insert picture; the operator reads the row fields and checks the insert
  itself;
- the TOOLS pad's row editor with the operator's own hand: `1`, the tool's
  number, `#`, then `2` for the radius;
- that walking the cursor (`B`/`C`) over a `G` word leaves it exactly as it is -
  the port used to resolve and pick it on the way past, and to leave a lone `M`
  word alone, so `G` and `M` lines behaved differently under the same keys;
- `#` while a program is open: the drawing switches, the program does not move;
- RUN: the stock while cutting, the path when parked, and the DIN state EDIT
  left behind - the host checks the drawing's ink, not the operator's reading of
  it;
- the panel's readability in whichever shape the glass is mounted for.

## Still to do

- The station's `examples\` carries the program and the entries file; the
  release zip and the packing script still need to carry the ncc station's
  folder (nc2's `dist\` is untouched).
- `nc` and `nc2` keep their own stations; the three share one core list
  (`tools/test_nc_ui.py`), so a core change is one change.
