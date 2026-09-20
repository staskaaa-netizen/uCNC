# NC TODO

## Handoff: where the panel stands (2026-09-19, end of session)

Read this first; the sections below are the history behind it.

Software status: `python tools/test_g7x.py all` (ALL PASS, 0 failures),
`python tools/test_nc_sender.py`, `python tools/test_nc_ui.py` (panel frame,
3x3 helper, preset contract, and `--streamtest` for the panel's one-shot
blocks) all pass, `pio run -e RP2350-LEANCAM-LVDS` builds and was uploaded to
the board (the single later change was a comment, so the running image matches
this source), `-e RP2350-G7X-MODULE` builds. Only what says "bench" below is
verified on the machine; everything else is software-verified only.

What the panel shows today:

- Tab strip across the top names the screen (MANUAL EDIT TOOLS RUN), current
  one filled; the message area is at its right end next to the `>` hint. The
  header below carries the DRO on every screen except MANUAL.
- Header DRO, three cells: work position in the offset in use (large), machine
  figure (normal font, own cell, `OFF X.. Z..` named above it), then F/S. FPS
  meter in the right corner.
- [x] Footer: **eight slots on every screen** (RUN and the file list used
  to have nine, which shrank every key and moved them when the screen changed).
  The writer pads every screen to the fixed count, so the keys keep their size
  and place. The entries that only restated a key's fixed job went: `B`/`C`
  stepping the list is what those keys do on every code screen (the editor never
  listed them), and RUN's `# RUN` was the same action as `1 SINGLE` with another
  name. `0` (file) and `D` (accept) keep their slots, and so does `*` where the
  screen has a delete or back action - **not on RUN**, where it deleted a line
  of the program being run. A view screen's unused keys answer `No action here`
  (the old "Stub mode" text was left over from the stub screens).
- Footer: white keys on the page background, slot key in the top-left corner, up
  to three wrapped label lines, same slots on every screen (empty ones keep
  their place, because the physical buttons will sit under the glass). The last
  two or three rows stay background pending the HSTX frame-close fix.
- EDIT/RUN share one body: graphic pane left, code right, the file name as
  the editor's own first row (selectable, Enter opens its folder), `*` for
  dirty, the 3x3 helper floating under its labelled line.
- [x] SIM is gone. It was EDIT with the engine behind it, only full screen, so
  it is that now: `# VIEW` (a full-screen *view* of the program - not a run and not an edit) hides the editor's code pane and the preview takes
  the whole body with no code lines at all, and the same key brings the code
  back. EDIT's strip traded `0 FILE` for `# VIEW` (the file list is still
  reached from the editor's own file-name row), and on the full body it shows
  the preview's keys only: `4 STOCK`, `5 PATH`, `6 ROUGH`, `7 DIM`, `# VIEW`.
  No delete there: while the preview is the screen the operator is looking at
  the drawing, not editing the program - the same rule that took `*` off RUN.
  For the same reason the editing keys are dead on the full body: a digit that
  has no preview meaning answers `No action here` instead of moving the
  selection or writing a field behind the drawing.
  The body is cleared before it is drawn, so hiding or showing the code
  cannot leave the previous, larger preview's pixels in the bottom-left corner.
- [x] EDIT shows the dimension callouts (the Z values above the part, the
  diameter figures to its right) in the pane as well as on the whole body: they
  were gated to the full view, and the ones the pane could not hold were drawn
  past its edge and then wiped by the code pane. `7 DIM` is one switch for both
  views, so what the operator turns off on the full body stays off when the code
  comes back - and back on again the same way.
- [x] Text files are text. The file list carries them beside the programs - the
  panel's own `/D/presets.txt` included - and the editor opens and saves them
  (`nc_path_text()`), but only the program extensions are read as G-code
  (`nc_path_supported()`): a `.txt` gets no preview parse and RUN refuses it
  with `Not a program file`. The preview says `Text file - no preview` instead
  of trying to read the file as a program.
- [x] `0` opens the file list on EDIT, TOOLS and RUN **as a key of the screen,
  not as a footer slot**: the footer carries the keys that mean something
  special on that screen, and opening a file means the same everywhere. MANUAL's
  `0 ZERO` is a footer entry and wins. The `0 FILE` slots left RUN and TOOLS.
- [x] The preview's content box starts 30 px higher than it did (82 px below the
  pane top, not 112): that band reserved room for a caption that no longer
  exists - the tab strip names the screen and the header names the file - so the
  drawing was sitting low in an empty pane.
- [x] The operator's own program and tool table are kept as the module's
  fixtures: `tests/fixtures/facing.nc` (the G971/G973 setup, the `T2` tool line
  and the G80-terminated `G71` contour with its `C`/`R` corners) and
  `tests/fixtures/tool.t` (T1-T7 with their real radii, orientations and feeds).
  `tools/test_nc_ui.py` copies them into a scratch root and runs `--filetest`
  over them - load, the G71 block scan, the expansion RUN and the preview share
  (76 lines), and the `T2` line resolving through the tool table - then renders
  the program in EDIT and in the full-screen view, so layout work is reviewed
  against a real job instead of a toy.
- [x] The preview's drawing box now ends inside its own pane (it used to be
  scaled from a content box that reached `h + 28` past the pane's bottom, which
  is what put stock and dimension labels a little over the footer strip).
- [x] TOOLS footer dropped the `B`/`C` slots (those keys still step the tool
  rows), and RUN gained `# RELOAD`: reset the run and read the program back off
  the card, so a file - or the `G970` setup block in it - edited since it was
  opened is what runs next.
- [ ] TOOLS orientation (`O`) is drawn only as the little glyph on the row and
  in the tip block. `nc_tool_orient_valid()` knows which values exist, but the
  operator has to recognise the drawing: add a legend the screen can show (the
  valid `O` values and what each one looks like, e.g. an entry in the tool tip
  or a `?`-style help key), and write the variants down in this module's
  README so the table and the glyph cannot drift apart. Worth doing while the
  fixture tool table (T1-T7) is the thing on screen.
- MANUAL: no header DRO. One line per axis with three numbers - position in the
  offset in use, the STOP limit with the room left to it, the machine figure -
  and the picked line filled across the pane. Jog pad bottom right: `2`/`8`
  X-/X+, `4`/`6` Z-/Z+, `7`/`9` spindle CCW/CW, `5` spindle stop, `1`/`3` feed
  override, pressed key lit for 250 ms. Footer: `B`/`C` axis, `D` touch,
  `0` zero, `#` step/continuous, `*` stop.
- TOOLS: tool table with the tool tip details near the bottom of the pane.

Leftovers from this session, in the order they came up:

- [ ] **TOOLS still does not select or edit anything** - the last report from
  the bench. Reproduce: `B`/`C` should move the table cursor and the detail
  block follow it; TOOLS footer is `1 ADD`, `7 INS`, `8 FILES`, `9 SAVE`,
  `0 FILE`, `* DEL`, and EDIT's footer `2 TOOL` opens the tool submenu
  (`1 SELECT`, `2 EDIT`, `3 M6`, `4 M3`, `5 STOP`, `6 M4`). Check that a
  selection reaches the `.t` document and that saving writes it. Decide how a
  tool number is entered (the `T` field on the line, the way the G-code field
  works).
- [ ] MANUAL column caption should name the offset in use (`G54`/`G55`/`G92`),
  not only its values. `parser_parameters.coord_system` is not exposed today, so
  this needs a small parser getter beside `parser_get_wco()`.
- [ ] MANUAL wants a helper-message area to the left of the 3x3 pad saying what
  the pad keys do in the current state. Asked for, never built - the value
  readout beside the pad (`1- 3+` with the step and feed values) is the first
  piece of it; the jog and spindle keys are still only named on the keys
  themselves.
- [ ] MANUAL stop is one value per axis, and the panel learns the side the axis
  works on from the first move away from it (`nc_visual_manual_toward_stop`):
  into the wall is clamped and refused at it, away from it is a bounded feed.
  Standing *on* the wall the side is still unknown, so the first move either way
  is allowed and defines it - which is what keeps `*` at the current point from
  locking the axis. If the operator wants a stop per jog direction instead -
  the "two rows in the same line" reading - that is two values per axis, and `*`
  then needs to know which one it sets.
- [ ] 3x3 helper: show the preset `name=` on the key instead of the fixed slot
  label, so a renamed entry reads as renamed on the panel.
- [ ] 3x3 helper: no animation. Decided ("skip animation, it is weird and
  slow") - the helper just appears under its line.
- [ ] G7x `G80` end mark: the G7X submenu has `6 G80`; check it inserts where
  the operator expects, against "same as two N blocks of Fanuc style later".
- [ ] PECK/DRILL/TAP wording and ownership: is `6` a linear cycle with pecking,
  and does tapping belong with the G76/G33 form? Decide, then fix the footer
  text.
- [ ] EDIT: field-by-field entry (`docs/nc-editor-tnc415.md`), including "code
  unknown" for a typed G code, arrow keys walking the fields, and the
  non-mandatory words pre-entered as `0` (`G1 X Z C0 R0`).
- [ ] Real keypad/keyboard pass on the machine has never been done.

Bench items, not software:

- [ ] MANUAL on the machine: a step jog moves the axis, a held direction key in
  feed mode keeps feeding and stops where it is when the key comes up, `*`
  stops a feed, an alarm stops it and the axis never passes the stop, spindle
  keys start/stop, zero and touch-off land, and the console still answers after
  a jogging session.
- [ ] Panel frame close: the last rows do not show framebuffer content, so the
  footer cannot sit flush on the bottom edge yet.
- [ ] Mode-switch lag on RP2350 (about a second) after the idle-flush change.
- [ ] Flashing: a `pio device monitor` holding COM13 makes picotool find no
  BOOTSEL device and the upload fails - free the port first. Uploads are
  incremental in the default `.pio/build`; do not point
  `PLATFORMIO_BUILD_DIR` at a scratch directory unless a full rebuild is
  wanted.

## Next UI priorities (proposed; not implemented)

- [ ] **Large follow-up: build out the NC/CAM workflow.**
  - [ ] 3x3 path builder for creating/editing a usable tool path from the
    panel, with clear preview, insert, cancel and save behavior.
  - [ ] DXF reader/import path, including unit/scale handling and conversion
    into the shared path/document representation rather than a second dialect.
  - [ ] Expand threading support end to end: supported G33/G76 forms, editor
    and preview coverage, clear unsupported-case errors, and the machine
    checklist for spindle synchronization.
  - [ ] Polish offset display: show the active work coordinate system and
    offset clearly in the DRO/readout, and keep work and machine positions
    visually unambiguous.
  - [ ] Add the active tool number and useful tool identity to the DRO header
    without crowding the position, offset or feed/speed fields.
  - [x] MANUAL continuous feed is a held key, and the values are on screen:
    `#` arms it (`Continuous feed` / `Step jog`) and the pane shows both values
    beside the pad - `STEP 0.100 mm` and `FEED 500 mm/min` - with the one the
    mode is using filled. `1`/`3` change the filled value, so what those keys
    change is visible at the moment they are pressed. Holding a direction key
    sends one `$J=G91 <axis><distance-to-stop> F<feed>` block so the
    controller feeds continuously, and the key release cancels the jog where
    the axis stands. `*` (Stop), an alarm or the wall end it as well. Toward the
    stop the block is the whole distance to it, so the jog ends on the wall by
    itself and no arithmetic on a lagging position read can overshoot it; away
    from it - or with no stop set - the feed is a bounded move (the axis travel
    the machine states, `$130`, or a 25 mm cap when it states none), because
    nothing is there to end it. A feed only starts from a standing axis, since
    the distance to the wall is measured from a position. The keypad now
    reports press *and* release for this
    (`cam_keyboard.c`, `ui_input_keypad_held_key()`), and `nc_visual_hold_key()`
    is what a shell - the panel's keypad poll, or `tools/nc_ui_win` holding a
    pad click or PC key - reports it with. Software-verified by
    `test_nc_ui.py --feedtest` on the real parser, planner and virtual MCU;
    the machine pass is a bench item.
  - [ ] The controller's live feed override lost its keys in MANUAL: `1`/`3`
    used to send `CMD_CODE_FEED_DEC/INC_COARSE` and now change the jog value
    (step or feed) the operator asked to see and adjust. Decide where a program
    feed override belongs - RUN, or a second level of the `#` mode.

- [ ] Storage status and explicit file-operation errors: distinguish not
  mounted, open/read failure and save failure. Use filesystem state; there is
  no wired card-detect input on the current board.
- [ ] Keep the unsaved editor document available if autosave on screen change
  fails; show retry/cancel instead of replacing it with another loaded file.
- [ ] Enforce/explain short SD filenames in create/rename flows while
  FF_USE_LFN=0. Keep the working SD driver/configuration unchanged.
  - [ ] Show active G18/G90/G94, G7/G8 and units near RUN so selected-cycle
    execution prerequisites are visible before submitting a command.
- [ ] MANUAL's X figure is the axis position, while the program's X word is a
    diameter (G7, the parser's default): the jog converts axis millimetres to
    the word the parser reads, so a step and a feed agree with the readout. A
    lathe operator usually reads X as a diameter, so decide whether the readout
    should show the diameter instead - and say which it is on screen either way.
- [ ] Add preview zoom/pan with a visible anchor, then finish chuck/setup layers.

Before changing storage behavior, read the history and bench checklist in
[`docs/sd-card-history.md`](../../../../docs/sd-card-history.md).

## Preset file

- [x] MDI removed. It was the editor bound to a fixed file whose path was
  written without the drive (`"mdi.nc"`), so the file was never created and its
  autosave always failed - which, with the autosave guard, locked the operator on
  the screen. Nothing about it was not already EDIT, so the screen is gone: five
  screens now (MANUAL EDIT TOOLS RUN), and `NC_MODE_MDI`/`NC_MDI_PATH` with
  it. `NC_TOOL_PATH` had the same missing drive and is now
  `"/D/nc/files/tool.t"` - that is why TOOLS never had a tool table on the
  machine while the desktop bench, which tolerated the bare name, showed one.
- [x] MANUAL is a readout: the DRO values one per row in the large font, on the
  page background. Its old stub text and panel colour are gone. (Superseded by
  the three-number line above - position, stop, machine - see the handoff
  section.)
- [x] Screen changes do not wait on the card. The remembered state (mode and
  per-screen paths) used to be written by `nc_state_save()` at every step of the
  change - two or three FAT writes per switch, and more on a first visit to
  TOOLS - which is the second of lag the panel showed. Saves now only mark
  the state stale; `nc_visual_idle_tasks()` flushes it from the main loop once
  the screen has been idle for 400 ms. The only card write left inside a screen
  change is the editor's autosave, which must happen before the buffer is reused
  for another file.
- [x] File list browsing: the list opens with nothing marked, lands on the open
  file when it is in the folder, and previews whatever the cursor points at in
  the left pane (`PREVIEW <name>`). Safety: the selected file is read into a
  scratch document, never the open one, and only when the selection changes -
  browsing cannot disturb the program being edited and the card is not read in
  the draw path. Directories and unreadable files simply have no preview.
  Nothing is prepared for execution; no arming, motion or machine state.
- [ ] Panel frame close: the last two or three rows of the panel do not show
  framebuffer content correctly - a marker on the last row appears a couple of
  rows above the bottom edge, and a two-row marker reads as two separate lines.
  Row 0 lands correctly at the top, the rest of the frame is right, and neither
  the vertical front porch nor the back porch changed what the glass shows, so
  this is not a timing knob. The footer therefore keeps the bottom few rows as
  page background rather than drawing its closing border into them. Needs a
  bench session on the HSTX/DMA frame boundary (descriptor ring rewind against
  the DMA pipeline) before the footer can sit flush on the bottom edge.
- [x] One name per thing, screen-wide. The screens are named once in a tab strip
  across the top (MANUAL EDIT TOOLS RUN, current one highlighted, `>` as
  the scroll hint); the header below carries the run state in RUN and otherwise
  only messages. The file name is the editor's own first row, selectable with
  Up from line 1, and Enter on it opens the folder that file lives in. The tab
  strip costs one of the pane's row units, so 17 code rows remain, and the
  footer keys now reach the bottom edge of the panel.
- [x] Cursor movement no longer reports itself: the editor shows the line
  numbers and the highlight, so the header's message line stays for messages
  (errors, run state, rejected edits).
- [x] The floating 3x3 helper is anchored to a line of its own: opening a
  submenu inserts a labelled line under the cursor (the footer action name, the
  text the panel used to draw as a title), hangs the panel under it, and
  replaces that line with the chosen result. Cancel removes it and returns the
  cursor. The panel has no title strip of its own.
- [ ] Footer digits are shadowed by value editing: with a word selected, `4`
  edits the selected value instead of opening the G7X helper, because the pad
  digits and the footer slots share keys. Decide which wins, or give the two
  different keys.
- [x] `/D/presets.txt` owns the floating-menu insert text: one `[id]` section
  per menu entry with `name=` and one or more `line=`. Compiled presets are the
  fallback; a missing file is written from them. See
  [`docs/nc-preset-file.md`](../../../../docs/nc-preset-file.md).
- [x] Lazy resolve: the SD card is mounted from the main loop, so the file is
  settled on the first NC input event that finds the drive instead of during
  `nc_visual_init()`. A damaged file falls back and is left for repair.
- [ ] Show the `name=` of the sections in the floating 3x3 helper instead of the
  fixed slot labels, so a renamed entry reads as renamed on the panel.
- [ ] Preset editor/validator on the machine (create/rename/delete a section,
  reject text the parser cannot read) so the file does not need a PC.
- [ ] Per-section extra words (`R`, `C`, ...) as further `line=` rows once the
  field-by-field entry from `docs/nc-editor-tnc415.md` exists.

## MANUAL readout, jog and one-shot blocks (2026-09-19)

- [x] Every readout line carries three numbers: the position in the offset in
  use, the stop the feed must not cross (small font, with the room left to it),
  and the machine figure the offset is cut from. The picked line fills with the
  selection colour across the whole pane. The caret that only restated the
  column heading is gone, in the pane and in the header DRO.
- [x] The header DRO gives the machine figures a cell of their own, one font
  smaller than the position, with the offset named above them - the two used to
  overlap in one cell.
- [x] The jog keys name their axis: `2`/`8` are X-/X+, `4`/`6` are Z-/Z+, so a
  jog cannot land on the other axis. The pressed key stays lit for 250 ms, which
  is what makes a step jog visible at all.
- [x] The stop is a wall the axis may approach but not cross, and standing on it
  is not a lock: `*` at the current point used to trap the axis, because both
  directions read "At stop".
- [x] Panel blocks get their own short queue on the shared reader
  (`nc_run_send_line`): a jog is two blocks (the incremental move and the G90
  that follows), and the second one used to overwrite the first before the
  parser read it. The queue also hands the reader back to the console when it
  drains, and refuses to cut into a running program (`nc_run_streaming()`).
- [x] Feed mode is a held key: the keypad reports the key that is down
  (`cam_keyboard_key()` and `ui_input_keypad_held_key()`), the panel turns a
  held direction key into one jog block covering the distance to the stop, and
  the release, `*`, an alarm or the wall ends it (`nc_visual_hold_key()`).
  A feed starts only from a standing axis, so the distance to the wall is
  measured from a real position. Software-verified by
  `test_nc_ui.py --feedtest` (the real parser, planner and virtual MCU).
- [x] Every key works twice in a row again. The keypad's event byte has the
  release flag in bit 7 and the key in the low seven bits; the driver decoded a
  release as "no key", so the release never cleared the key, the panel kept
  reading it as held, and the next press of that key was filtered as a repeat -
  every key acted once until another one was pressed. `cam_keyboard_decode_key()`
  now decodes both edges and the press/release edge comes from bit 7. Guarded by
  the bench's `--keytest` (fails if a release decodes as no key). The press edge
  is still kept apart from the held key (`cam_keyboard_pressed_char()`), so a tap
  whose press and release arrive in one late read is not lost either.
- [ ] Bench-check on the machine: a step jog actually moves the axis, a held
  direction key keeps feeding and stops where it is when the key comes up, `*`
  stops a feed, the axis never passes the stop, the console still accepts input
  after a jog, and the spindle keys start/stop the spindle. The host bench
  checks (`test_nc_ui.py --streamtest`, `--feedtest`) prove the blocks and the
  jog state, not spindle phase or real travel.

## UI feedback completed (2026-09-16)

- [x] Persistent settings-invalid banner with `$RST=*` guidance and explicit
  notice that it resets settings/offsets; storage-write failure has its own
  message. No automatic reset or unlock.
- [x] Distinguish controller alarm, untrusted position, door and jog locks in
  on-screen guidance; refresh when controller/settings state changes.
- [x] Show readable command errors with numeric codes and source lines for NC
  RUN/selected-line sends, including rejected parameters and missing G80.
- [x] Hide UI timing counters by default (`NC_UI_DEBUG_TIMING` enables them).
- [x] Stop repeated corner diagnostics during preview redraws
  (`G7X_DEBUG_CORNERS` enables them).
- [x] Preserve selection when switching screens that open the same file during
  the current session; cursor persistence across reboot remains future work.
- [ ] Bench-check banner readability, error recovery and screen selection on
  the actual display. Software checks and firmware build do not validate layout.

## Dedicated depth-per-pass control

- [ ] Add a dedicated rotary encoder or potentiometer for the G71 U depth of
  cut (material removed per pass, not the F feed-rate override).
- [ ] EDIT: use the knob to set the selected cycle's U value and save it in the
  program through the normal editing flow.
- [ ] RUN: apply a live override relative to programmed U, approximately -80%
  to +80% (20%–180% of the programmed depth). Show programmed U, requested
  override and effective depth separately.
- [ ] During an engaged cutting pass, allow only a decrease in depth. Defer any
  requested increase until the G0/retract/return phase, then apply it to the
  next pass. A later reduction must replace any pending higher request.
- [ ] Define and test the safe transition for a decrease during engagement:
  account for already queued motion, avoid abrupt tool movement, and maintain
  contour, finish allowance and remaining-stock bookkeeping. Do not simply
  change U on motion already queued in the planner.
- [ ] Keep this in the shared G7x generator/runtime; NC provides knob input and
  display. Identify the pass phase explicitly rather than treating every G0 as
  permission to increase depth. Decide later how this maps to G72's W word.
- [ ] Bench tests: knob limits/noise, decrease while cutting, deferred increase,
  changed pending request, hold/resume, Stop/reset, and the final shallow pass.

## Completed software work (2026-09-16)

- [x] Serialize generated G7x blocks before subsequent source commands; preserve
  G7/G8 conversion and propagate errors instead of accepting a failed G80.
- [x] Clear cycle state on parse failure, reset and NC Stop; feed hold/resume
  controls actual motion and remains available after the file reaches EOF.
- [x] Enable native single-line G76 with radial first-cut/decreasing infeed,
  G20/G21 scaling, work offsets, input validation and spring passes.
- [x] Make NC preview return explicit errors for invalid/unsupported cycles.
- [x] Add generator, preview and actual parser/planner regression suites:
  `python tools/test_g7x.py all` (G33 motion is intercepted).
- [ ] Machine-test spindle synchronization, physical hold/resume/Stop and reset.
- [ ] NC: G76 preview and document-source adapter. G7x: G70/PQ (see its checklist).

The remaining design backlog follows. Native G76's exact supported dialect is
documented in `../g7x/README.md`; hardware validation is still required.

- Later: from RUN, jump to EDIT with the same file and line marked.
- Tools: TOOLS mode owns the global `.t` table; RUN and EDIT link to it by `Tn`. Later G7x tool/preset choices should live in G7x commands, not by copying tool rows into programs.
- Pre-alpha visual/test pass:
  - Treat the current UI as stable enough for testing, but not final: missing/rough visuals should be tracked instead of hidden in code.
  - Chuck drawing is still partial. Make it a real lathe chuck/stock holder view, not only one jaw/stock clamp hint.
  - `G970` is already used as stock/setup metadata (`X/U/Z/W`) through `nc_sim_collect_preview`, but the preview does not yet draw the setup region as its own explicit visual layer. Use it for stock origin, stock extents, and setup envelope instead of only fitting stock size.
  - Place small corner feature labels (`R2`, `C5`) by cut direction and operation: for normal G71 OD/x-minus cuts use the left/top side of the corner point; use the opposite side for boring/ID and other reversed directions.
  - Add preview zoom/pan. This needs a visible preview cursor/anchor so zoom has a center and users can inspect corners/clearance.
  - [x] Added TESTING.md checklist for EDIT/RUN; bench verification remains open: mode switch, file persistence, G970 stock fit, G71/G72 contour labels, live stock removal, run cursor, and reboot recovery.
  - Keep preview-only line labels and UI helpers in NC, but keep all G71/G72 generator/roughing/finish logic in `g7x`.

## Cycle implementation belongs to G7x

The old mixed promotion/completion list is replaced by the audited
[G7x checklist](../g7x/TODO.md). Completed foundations are checked there;
P/Q, G70, allowance-aware approach and directional roughing remain open.
NC supplies document access and UI, not a second cycle generator.

## Remaining NC integration

- [ ] UI review before the next big step (`docs/nc-ui-review.md`). The review's
  organising finding is that most defects are one logical mismatch: a key or an
  entry carrying two meanings (the Up/Down case - `B`/`C`/`D` are footer actions
  while browsing and sign/point/accept while editing; HOLD is a state drawn as
  an action; toggles were highlighted like actions; the cursor is both browsing
  position and armed run line). Fix as one model - explicit kind per entry
  (action/toggle/mode), one meaning per key per context, state shown for state
  entries - then the instances (HOLD, RUN versus SINGLE, action highlight).
  Second P1: apply-the-draft-on-accept in the editor.
- [ ] Layout v2 (`docs/nc-layout-v2.md`): remove header and footer, add a full
  width bottom bar with DRO/status/message on the left and a dedicated 3x3 key
  grid on the right, 14-16 code rows, tool description at the bottom of the
  graphic pane, file name removed from the code pane. The 3x3 area is where the
  key model from the review should land.
- [ ] **Audit before the split: find the doubled checks and the extra layers,
  remove them, then extract.** The module is at **10,755 lines** (31 files,
  9,772 non-blank) against the ~10k line rule in `AGENTS.md`, and
  `nc_visual.c` alone is **5,693** against the ~2k line rule - over half the
  module. Before cutting the preview half out, sweep for the two things that
  make an extraction harder than it looks:
  - **doubled checks** - the same guard in two places, so a screen can answer
    one way while another acts the other way. Collapsed so far: the
    "axis must be standing" test (`nc_visual_axis_standing()`, shared by the
    MANUAL step and feed paths) and the "is this a program" test
    (`nc_visual_document_is_program()`, shared by RUN arm and RUN step). Look
    for the rest the same way: one predicate, one caller, or say why not.
  - **extra layers** - a wrapper that only forwards, state kept twice, a second
    path to the same answer, a name left over from a screen that is gone
    (`NC_FOOTER_ACTION_CODE` was really a view toggle and is now `..._VIEW`;
    `nc_visual_draw_sim_contour_points()` no longer says sim). Simplify while
    the tests can still catch a change of behaviour.
  Then split: the preview and drawing engine (preview collection, dimension
  layer, live stock, tool glyph, the palette primitives, ~2,000 lines) moves
  into **sources inside this module** - `nc_preview.c` / `.h` beside
  `nc_visual.c`, absorbing `nc_sim.c` (whose name is a leftover: it collects
  preview data, it does not simulate) - **not into a module of its own**. The
  two cannot exist apart: a preview with no screen to put it in and a screen
  that cannot draw are not two modules, they are one module with two files.
  The precedent is `cam_keyboard`: `cam_keyboard.c` and `ui_input_keypad.c` are
  sources the keyboard path and the NC module compile, with no `DECL_MODULE`,
  no module id and no enable flag, and `custom_ucnc_modules` only says where the
  folder comes from. Do the same here: `nc_preview` is compiled as part of NC,
  ships with it, and cannot be switched on by itself.

  The boundary to write down, in `nc_preview.h`:
  - the preview owns its view state (`nc_preview_view_t`: the stock / path /
    rough / dim flags today kept as globals in `nc_visual.c`) and exposes
    setters for the footer keys that toggle them;
  - it draws from data in and nothing else: the document, the runtime figures
    and the rectangle to draw in - the shape
    `nc_visual_draw_thin_preview(doc, runtime, x, y, w, h, clear_bg)` already
    has today;
  - the screen never reaches into the drawing helpers, and the preview never
    calls back into the screen: the call graph stays one-way, as it is now
    (only `nc_visual.c` includes the module's other headers; only
    `nc_module.c` calls `nc_visual_*`);
  - `lvds_*` primitives stay with the renderer module, as they are.

  Build lists to touch when it happens: `tools/nc_ui_win/Makefile` and
  `tools/test_nc_ui.py` name their sources by hand (the firmware build picks up
  the folder). The guard is the frame dumps: a pure extraction must leave
  `tmp\nc-ui-show\*.png` and `tmp\nc-ui-tests\*.bmp` pixel-identical, so diff
  them before and after - a changed pixel is a changed behaviour.

  **Second cut, same commit series: the menu/footer as data, not as UI.**
  `nc_menu.c` should be nothing but the eight slots of the active screen in a
  struct - today that is `{ char key; const char *label; uint8_t action; }`,
  which is already the right shape - and the UI should only ever:
  - ask for the eight slots of the current screen (`nc_visual_footer()`, which
    already answers with the preview's slots while the code pane is hidden),
  - report a press by the **code** it read (the keypad character), letting the
    screen resolve the action - which is what the bench does today by pressing
    real keys rather than calling a footer action,
  - read back the **value** an entry shows: the `label`, and whether a toggle is
    on (`nc_visual_footer_item_is_toggle()`/`_on()`), never the panel's
    internal selection.
  So no UI state may live in the menu: the armed/selected entry
  (`g_nc_visual_selected_action`) and the open 3x3 (`g_nc_modal_*`) stay with
  the screen that owns the cursor, the view flags move into `nc_preview`, and
  both UIs (the LVDS panel and `tools/nc_ui_win`) read exactly the same eight
  texts. A screen that wants a different eight, gives a different table - it
  does not teach the UI a new rule.

  **Order and guard:** commit a checkpoint first (the module carries the whole
  session's work uncommitted), then the preview file as one move, then the menu
  cut as the next, each with the frame dumps diffed before and after. Two
  thousand-line moves are not done inside a working session's tail - a half
  applied extraction is the one outcome worse than the current file.

  **First move done, second move needs a tested tool.** `nc_sim.c` is now
  `nc_preview.c` / `.h` with `nc_preview_collect()` and
  `nc_preview_line_word_float()` (commit `fa65ebae`): the preview's data half
  under its own name, compiled with NC, not a module of its own.

  **Second move done - the tool is the deliverable.** `tools/nc_move_funcs.py`
  with `nc/tests/draw_functions.txt` (38 names, `!` survivors, one `@region`)
  does the cut: spans from the grammar (a definition ends at the first column-0
  `}`, a prototype is never a definition, a `typedef` ends at its own `};`),
  every check before anything is written (each name defined once, no overlapping
  spans, survivors still present, the moved text non-empty, braces balanced in
  both halves), and `--check` to report the numbers without writing. What it
  wrote: `nc_draw.c` / `nc_draw.h` hold the 38 drawing functions (1,244 lines),
  `nc_layout.h` holds the 78 lines of numbers they read with them - pane
  geometry, footer, 3x3 key size, preview limits, `NC_FOOTER_LINES` - and
  `nc_visual.c` went from 5,708 lines to 4,366. The moved code reads the footer
  items (`nc_menu.h`) and the numbers (`nc_layout.h`) through `nc_draw.h`;
  nothing else changed, no caller was touched and the names stay `nc_visual_*`
  for now, so the diff is a move and not a rename mixed into it.

  Verified as a pure move: `tools/test_nc_ui.py` passes (preset, stream, pad,
  feed, key, file), and `tmp/nc-ui-tests/*.bmp` - the frame, the 3x3 helper,
  EDIT, the full-screen view and the `/D` listing - are byte-identical to dumps
  taken immediately before the cut. Both firmware targets build. The two build
  lists that name NC sources by hand were updated: `tools/nc_ui_win/Makefile`
  and `tools/test_nc_ui.py`.

  **Third move done: the preview renderer joined its data half.** `nc_preview.c`
  is now what the name says - `nc_preview_collect()` and the drawing of the
  stock, the contour, the dimension layer, the emitted path, the live tool
  marker and the live stock mask (17 functions, 853 lines) - and `nc_visual.c`
  is 3,478 lines (from 5,708 before the drawing move) with the screens, the
  editor, the key handling and the footer dispatch left in it.

  This one is not a move and does not pretend to be: the preview used to read
  the screen's own globals, and the boundary the header now writes down is a
  request instead - `nc_preview_ctx_t` (the document to draw, the screen's own
  document, the runtime snapshot, the mode, the full-body flag, the run's line,
  the tool path, the status buffer it may write an error into, and the three
  run flags the code asked separately before) plus `nc_preview_layer()` for the
  four switches (`STOCK`, `PATH`, `ROUGH`, `DIM`) the footer toggles: the screen
  reports the key it read, the preview keeps the state and the screen reads the
  value back to light its key. The live stock mask, the tool marker rectangle
  and the one-frame cache stayed with the drawing they belong to. The screen
  builds the request once per frame in `nc_visual_preview()`, so no call path
  reaches from the preview back into the screen.

  Two things fell out of the cut rather than being planned: the same
  "is this line contour geometry of a G7x block" scan was wanted by the editor
  and the preview, so it is now `nc_g7x_line_is_any_contour()` next to the block
  scan that owns it; and `NC_VISUAL_HAVE_PSRAM` became the preview's.

  Same guard as the drawing move: `tools/test_nc_ui.py` passes, the five bench
  dumps and all twelve `tools/nc_ui_show.py` frames are byte-identical to the
  ones taken before the cut, and both firmware targets build. Still to verify on
  the machine, as before: the RUN live-stock path, which needs PSRAM.

  **Fourth cut done: MANUAL is a file.** `nc_manual.c` (771 lines) has the
  screen's own state - the picked axis, the step and feed tables, the stop, the
  spindle direction, the touch field, the pad table - its jog/feed/stop/zero/
  touch/spindle functions, and its pane, drawn out of `nc_visual_draw_snapshot()`
  as `nc_manual_draw()`. That branch was the biggest reason the screen file
  stayed over the 2k rule: `nc_visual.c` is 2,816 lines now.

  The boundary follows the preview's: the screen hands in the status line and
  repaint flag (`nc_manual_screen_t`) and the frame's figures with the work
  offset the readout is cut from (`nc_manual_view_t`), and MANUAL answers
  `nc_manual_key()`, `nc_manual_action()` (its own footer entries: axis, zero,
  touch), `nc_manual_hold()` for a key being held and `nc_manual_key_hint()` for
  a shell that labels its own keypad. The key-to-character table stays in
  `nc_visual.c` - a key and its character are passed together, so there is still
  one table - and the work-offset cache stays there too, because the header
  names the same offset the readout is cut from.

  The 3x3 item above is settled with this: the MANUAL pad and the EDIT helper
  already share one visual (`nc_draw_modal_items()`) and one item shape
  (`nc_footer_item_t` with the lit mask); what each screen keeps is its own nine
  entries and what they mean, which is the screen's business - a jog key and an
  insert key have different actions, and pretending otherwise would mean a
  callback table per screen for no gain. The planned `3x3_path_builder.c` is a
  third user of the same drawing, not a third drawing.

  Verified like the others: `tools/test_nc_ui.py` (including the feed and pad
  checks, which press MANUAL keys), the five bench dumps and the twelve
  `nc_ui_show.py` frames byte-identical, `test_g7x`, `test_nc_sender` and both
  firmware targets.

  **Fifth cut, first half done: the editor's typing.** `nc_editor.c` starts with
  what the HELP + word editing owns outright - the draft a value is typed into
  (`nc_text_edit_t`), the code buffer the helper types, the floating helper
  itself (its menus, the line it inserted and the keys that drive it), the
  selected-word edit path, and the drawing of the draft line and the helper
  panel. `nc_visual.c` goes 2,816 -> 2,527 lines; `nc_editor.c` is 377.

  The boundary: the document is *not* the editor's - it is the screen's buffer,
  which RUN sends, TOOLS holds the tool table in and the preview reads - so it
  is handed in with the status line and repaint flag in `nc_editor_ctx_t`. The
  key map stays with the screen (the order a key is offered to the footer, the
  modes, MANUAL and the editor is the screen's), which is why the editor answers
  `nc_editor_modal_key()` where the screen offers the helper the key first and
  `nc_editor_selected_word_key()` where its own sequence wants it. One thing
  needed a note: the helper's menus hand their chosen entry *back* through
  `ctx->follow`, because what a footer entry does is the screen's - the editor
  never calls the screen.

  Two smaller fixes came with it: `tools/nc_move_funcs.py` lifted a comment only
  when its continuation lines began with `*`, so plain wrapped comments stayed
  behind in the source file - and the frames caught a real mistake in the same
  breath (the two keys the editor's entries take were passed in the wrong
  order, which showed up as the helper opening instead of a digit being typed).

  **The editor's other half is next:** the code-line and tool-row movement, the
  file list and the open/new flows, the word/field key steps and the code pane
  drawing (including the file list overlay). Then `nc_visual.c` is the screen
  frame around the screens: tabs, header, notice, footer strip, key map, the
  TOOLS pane, RUN arming and the frame stats - and the Heidenhain field flow
  above lands in `nc_editor.c`.
  The rename was done before those cuts, as its own commit: the drawn functions
  are `nc_draw_*()` in `nc_draw.c` and `nc_preview_*()` in `nc_preview.c`, and
  `nc_visual_*()` now means what it says - a function of the screen file. It ran
  alone so the diff showed nothing but names, and the frames were re-diffed
  after it.

  The drawing half (`nc_draw`: text clipping and wrapping, the tool glyph
  geometry, chuck/stock hatching, the dimension callouts, the floating 3x3 grid
  and the footer strip - 38 functions, 1,244 lines of the 5,709) was attempted
  by a script and reverted three times, each time leaving the file uncompilable.
  The failures were all in the *extractor*, not the idea, and they say what the
  tool needs before it is used on the real file:
  - a definition's span ends at the first column-0 `}`; a *prototype* that spans
    several lines has no `{` at all, and treating it as a definition swallows
    everything up to the next `}` (the frame-timing globals went that way);
  - `#define NC_FOOTER_LINES` has no `;`, so a scan that waits for one eats the
    block after it;
  - a typedef block must be matched to its own `};`, not to the next `;`.
  So: build the extractor as a *tool* with its own dry run and a byte check of
  the result (function count, lines moved, the globals still present), run it
  against a copy first, and only then on `nc_visual.c` - and render the frames
  immediately before the move, because a stale baseline makes the diff say
  "changed" for a change that was already committed. That tool is the next
  piece of work, not a bigger edit.

  **And the 3x3 grid gets the same treatment: one visual, two users, a third
  later.** The drawing half is already shared -
  `nc_draw_modal_items()` paints both the MANUAL jog pad and the floating
  helper - but each caller brings its own nine labels and its own state, so the
  meaning of a key lives in two places. Target: one unit
  (`nc_pad.c` / `.h`, inside this module, same "sources not a module" rule)
  that takes nine `{ code, label, lit }` entries and draws the grid, with the
  same input contract as the footer: the UI reports the **code** it pressed and
  reads back the selected value - never the panel's internals. Its users:
  MANUAL (the jog pad, labels from the manual table), EDIT (the floating helper,
  labels from the inserted line's menu), and the planned `3x3_path_builder.c`
  once that work starts (see the 3x3 path builder item above). This is a
  consolidation of something already half-shared, not a new layer: if it cannot
  be the two existing callers plus one struct, it is not done yet.
- [ ] **Global editor note - steal the editor flow from Heidenhain TNC 415.**
  Field-by-field entry instead of prefilled lines: start a function, the control
  prompts the first required field (letter + description, no need to type the
  letter), Enter accepts and advances, Enter on an empty field skips it, the
  function stays modal until committed. Includes the word validation reported
  from the Windows editor: a `G` word may only be edited to a supported code
  (the cycle family switches inside its own set, e.g. `G71`/`G72`), always
  positive and integer, and unsigned words must reject a sign. Also: Up/Down on
  a value adjusts and flips its sign so the pad's `-` key is unnecessary, End
  and Del get a real job, `.` is used or removed, and the 3x3 becomes a two or
  three level menu. Letter keys (`G`, `X`, `Y`, `Z`, `N`, `Q`, `U`, `R`, `F`)
  are explicitly not planned until the field flow exists - they would only add
  noise. Full spec: `docs/nc-editor-tnc415.md`.
- [x] Desktop sender (`tools/nc_sender`): the NC document, emitter and G7x
  generators compile host-side and stream expanded programs to Grbl or uCNC,
  with a Grbl 1.1 protocol client, Win32 COM transport and host tests. The
  Win32 GUI shell (editor + preview) is the remaining step; see
  `docs/desktop-sender.md`.
- [x] Windows panel shell (`tools/nc_ui_win`): the real NC screen renders on the
  host through a GDI LVDS backend, with the machine's own key row beside the
  emulated 800x600 panel - F1-F5 modes (there is no MDI screen) and the 4x4
  keypad `cam_keyboard.c` decodes (`*0#D` / `123C` / `456B` / `789A`).
  `nc_visual` grew `nc_visual_select_mode()` for direct mode keys,
  `nc_visual_key_for_char()` for the keypad's key characters (now the single
  statement of what each key means: `nc_module.c` maps the machine keypad
  through it too) and `nc_visual_key_hint()` for the meanings a mode gives a key
  the footer does not name, which is how the shell labels MANUAL's jog digits
  without copying the rules. `--dump-bench` writes the whole bench and
  `--padtest` checks the pad is the machine's matrix and reaches every footer
  key. RUN still drives the virtual parser; a Grbl transport for real hardware
  is the next step.
- [x] Supply a document-source adapter for G7x numbered-block lookup
  (`nc_emit_numbered_source`) and preview both one-line and Fanuc two-line
  `G71/G72 ... P Q` ranges from the document, with missing/ambiguous range
  errors surfaced as preview errors.
- [x] Share one G7x block scan between RUN, the preview and the editor
  (`nc_g7x.c`): block start/end for numbered ranges and G80 cycles, two-line
  headers, and which rows count as contour. RUN sends a whole block for a
  selected line, including either header line of a two-line pair.
- [ ] Add G76 preview through the shared G7x threading generator.
- [ ] Keep nc_emit as preview glue; assess removal only if the preview can consume the
  shared stream directly without losing source-line/error information.
- [ ] Implement the depth knob EDIT/RUN interaction described above; G7x owns
  pass-phase enforcement and generated motion changes.
- [ ] Persist cursor position across reboot; current preservation is in-session.
- [x] Hardware checklist exists in TESTING.md; executing it remains open.
