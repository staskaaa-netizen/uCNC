# NC Pre-Alpha Testing

Use this as a short hardware pass list while NC is still pre-alpha.

## Boot and Stability

- Screens name themselves once: the tab strip across the top marks the current
  screen, the header shows the run state in RUN and otherwise only messages, and
  the file name appears once, as the editor's own first row. Walk all six
  screens and check nothing is named twice.
- Messages live at the right end of the tab strip, left of the `>` hint: command
  errors, run errors, transient messages and the screen's own status all land
  there, in the error colour when they are faults. The DRO block below carries
  the numbers and, in RUN, the run state - no message line.
- The DRO takes three columns from the panel edge: the **work** position
  (`X`/`Z`, large, `parser_machine_to_work()`), the machine figure each work
  value is cut from (normal font, its own cell, `OFF X.. Z..` named above it),
  then `F`/`S`. The two left cells must never meet, and the numbers must still
  be readable after `G92`/`G10` puts a large offset between them.
- The run state (`IDLE`, `ACTIVE`, `HOLD`) sits in the tab strip, left of the
  message area; the plain idle "RUN" is left out - the tab already says RUN.
- The editor's file-name row is reachable: hold Up from line 1 and the row above
  it takes the highlight; Down returns to line 1. Enter on it opens the file
  list at the folder the open file lives in.
- Moving the cursor reports nothing in the header: the editor numbers the lines
  and highlights the current one, so no "Line n" message should appear.
- Header and footer spacing: with the tab strip in place the header still
  clears the code rows, and the footer keys sit on the bottom edge of the panel
  with no margin below them.
- EDIT full screen (`# FULL`): the preview is the screen - it takes the whole
  body, with no code lines at all - and the strip shows the preview's own keys:
  `4 STOCK`, `5 PATH`, `6 ROUGH`, `7 DIM`, `# FULL`, and **no delete** (the
  preview is the screen, the program is not being edited). `# FULL` again
  brings the code pane back with the cursor still on the same line, and **twice
  in a row must leave no leftovers** - the bottom left is where the larger
  preview's pixels used to stay behind, so the whole body is cleared before
  either state is drawn. No part of the preview - stock, chuck, dimension label
  - may be drawn over the footer strip in either state; the drawing box has to
  stay inside the pane. The editing keys are dead on the full body: a stray
  digit answers `No action here` instead of moving the selection - or writing a
  field - behind a drawing the operator cannot see it happen in.
- RUN: `# RELOAD` resets the run and reads the program back off the card. Edit
  the file (or its `G970` setup block) on another screen, press `#` in RUN and
  check the preview and the run both come from the edited file.
- With invalid saved settings, every screen shows the settings/reset guidance
  without requiring a serial terminal; ordinary navigation must not hide it.
- After an intentional settings reset, the banner clears when the settings
  error clears. No UI action should reset settings automatically.
- Submit an invalid RUN parameter: check readable error, numeric code and
  correct source line; correct/retry and confirm the old error clears.
- A runtime alarm, door or untrusted-position lock updates the header even when
  the current screen is otherwise idle.

- Reflash, boot, and verify activity/status keeps updating.
- Reboot with last EDIT/RUN/TOOLS files stored and verify paths reopen.
- Switch modes with `A` through MANUAL, EDIT, TOOLS, RUN.

## Files and Text

- File manager lists only valid files/folders.
- Open an NC file in EDIT and RUN independently, and check EDIT keeps it across the full-screen toggle.
- `0` opens the file list on EDIT, TOOLS and RUN (no footer slot, by design);
  MANUAL's `0 ZERO` still wins there. In the list, browse up to the card root:
  the text files (`presets.txt`) must be listed beside the programs.
- Opening a `.txt` shows it as text - the preview says `Text file - no preview`
  and RUN answers `Not a program file` instead of feeding it to the parser.
- Opening the file list marks nothing: the list only gets a cursor once Up/Down
  is pressed, and the first press marks the last (Up) or first (Down) entry.
- Opening the list from a screen with a file open lands the cursor on that file
  when it is in the folder; the header count reflects it.
- Moving the cursor previews the file it points at in the left pane, with
  `PREVIEW <name>` above it. Directories and files that cannot be read show no
  preview and the open program's picture stays. The open document, its cursor
  and its dirty flag must not change while browsing.
- The field legend (`> G-code 970`, `> X position`, ...) shows for every line,
  including line 1: it takes the row above the cursor line, and for line 1 (and
  line 2) that row is the file-name row, so the legend sits over the name for as
  long as it is showing. The name comes back the moment the legend goes - it is
  a temporary overlay, not a permanent arrangement.
- Screen switching is quick: with an unchanged editor it should be a card read
  and nothing more. With unsaved edits the program is written before the buffer
  is reused (that is deliberate), so a switch from a dirty editor is the slowest
  case. The remembered state is written once the screen has been idle, so it
  must still survive a reboot after switching screens and waiting a moment.
- Unsaved edits are never dropped silently: switching screens, opening another
  file and creating one all write the open document first, and if that write
  fails the action is refused with `Save failed - ...` while the edits stay in
  the editor. Check with a write-protected or removed card.
- `* BACK` leaves the file list in one press and returns to the screen it was
  opened from; the `..` entry is what walks up a folder, so browsing to another
  folder and coming back does not need a walk down again.
- Long lines wrap/read cleanly.
- `B`/`C` line movement stays sticky up/down in RUN and TOOLS.
- 3x3 helper: pressing a footer submenu (EDIT `4` G7X, `5` THREAD, `6` PECK,
  `2` TOOL, `1` OPS) puts a labelled line under the cursor - `G7X` in the line
  itself - with the 3x3 hanging under that line, not over the line being read.
- 3x3 helper: choosing an entry writes on that line (the label is replaced by
  the template, or by the `T` field for TOOL SELECT) and the cursor stays at the
  same place in the program.
- 3x3 helper: only the nine keys are drawn - no panel box around them - and they
  span the code pane, reaching its bottom on the lowest selectable line. Empty
  slots stay as white keys so the pad keeps its 1-at-bottom-left layout.
- 3x3 helper: `0` or cancel removes the line and returns the cursor to where the
  helper was opened; nothing is left behind in the program.
- Footer: **eight slots on every screen** - the same key size in the same places
  from screen to screen, with the entries a screen does not use left empty (the
  strip used to grow to nine and shrink its keys on RUN and the file list).
  One white key per slot on the page background (no dark strip), the slot's key
  or number in the top-left corner, and room for a label wrapped over up to
  three lines. `D` keeps its slot where the screen names what it does (MANUAL
  TOUCH, file list OPEN) and `0` is the file key. `*` is the delete/back key on
  the screens that have one (EDIT and TOOLS delete, the file list goes back,
  MANUAL stops the feed) - **not on RUN**, where it used to delete a line of the
  program being run. `B`/`C` do not take a slot: stepping the list is what those
  keys do on every code screen, and the editor does not list them either.
  A key a view screen does not use answers `No action here`.
- Footer edges: neighbouring keys share their border (one divider line, not two
  with page showing between them) and the closing border sits on the last row of
  the panel, with no gap under it.
- Keypad: the machine's key characters (`A`-`D`, `*`, `#`, digits) do what the
  active footer says - `A` cancels/cycles, `B`/`C` step (or pick the MANUAL
  axis), `D` accepts, `*` deletes back, `#` finishes. Software-verified: the NC
  host test pins the character map and the desktop bench's `--padtest` checks
  the pad is the machine's matrix and reaches every footer key. The machine
  keypad pass itself is still open.
- Keypad repeat: pressing the *same* key twice in a row must act twice - every
  key, every screen. This is the fault the driver had: a release event (bit 7
  of the keypad's event byte) decoded as "no key", so the release never cleared
  the key, the panel kept taking the next press of that key for a repeat, and
  each key worked once until another one was pressed. Software-verified: the
  bench's `--keytest` decodes every key on both edges, and fails if a release
  decodes as no key.
- Keypad timing: a quick tap must register exactly once, including one made
  right after a screen change (the card read and the redraw make that poll
  late, so the press and its release can arrive in the same read). The machine
  timing needs the bench; the host bench checks the *decoding* only.
- Presets: boot with a card and no `/D/presets.txt`; after the first key press
  the file exists and opens as readable text (`[41]` / `name=` / `line=`).
- Presets: edit `line=` for `41`, restart, and check the OD entry in the
  floating 3x3 inserts the edited text.
- Presets: boot with no card, insert the card afterwards, press a key, and check
  the file appears without a reboot. The screen must keep working with the
  compiled presets while the card is absent.
- Presets: damage the file (delete every `line=`), restart, and check the OD
  entry still inserts the compiled text **and** the damaged file is still there
  to repair.

## Preview

- `G970 X/U/Z/W` stock/setup gives expected stock size and origin.
- Chuck/stock holder is visible and does not cover the working contour.
- Contour labels `C1`, `C2`, ... stay readable and follow corners.
- Rapid lines are dashed and visible; feed/finish lines are distinct.
- A `G71 ... P100 Q200` range draws the same contour as the equivalent
  `G80`-terminated program and leaves the cursor on the line after `N200`.
- A Fanuc two-line header (`G71 U1 R1` then `G71 P100 Q200 U0.5 W0.25 F120`)
  previews as one cycle, and selecting either header line in RUN sends both
  header lines plus the numbered profile.
- A numbered range whose `N(Q)` block is missing, repeated or out of order
  shows the preview error instead of drawing a partial contour.
- Future: zoom/pan has a visible cursor/anchor.

## G7x Runtime

Automated software checks: `python tools/test_g7x.py all` with MinGW GCC on
PATH. The parser fixture intercepts G33; it does not validate spindle timing.
The following machine checks remain open:

- `G71/G72` collect contour until `G80`.
- Source contour lines do not execute directly during RUN.
- Generated rough and finish blocks execute through parser helper.
- Finish ends at first contour point plus clearance in the opposite first-vector direction.
- Bad contour/status paths return useful errors without locking the UI.
- Queue a command after G80 and verify the whole generated cycle precedes it.
- Inject a generated-motion failure and verify no later source line is consumed.
- Hold/resume and Stop during a cycle, during normal motion, and after the last
  source line has been read but motion remains queued. Check that Stop also
  works immediately after resume, before motion restarts.
- Reset during contour collection and threading; no stale cycle may resume.
- Check G76 with G7/G8, G20/G21 and nonzero work offsets using the documented
  native contract in `../g7x/README.md`.
- Check spindle index/phase repeatability across all G76 passes, physical pitch,
  lead-in/out clearance, spindle loss and G33 error handling before cutting.
- G76 preview is currently unsupported and must show an explicit error.

## Tools

## MANUAL

- The header DRO is hidden on MANUAL: the pane is the readout. Every axis line
  carries three numbers - the position in the offset in use (large), the stop
  `*` set for that axis with the room left to it (small, two rows in the same
  line), and the machine figure the offset is cut from. The line the keys act on
  is filled with the selection colour across the whole pane; it must be obvious
  which line is picked without reading the values.
- A line with no stop shows `--` and `press *`: the stop is not silently zero.
- `B`/`C` (`AXIS-`/`AXIS+`) pick X or Z. `0` (`ZERO`) writes the work offset
  for the picked axis, `D` (`TOUCH`) opens a value field that a second `D` (or
  Enter) applies. The footer carries only those four; files and deletion belong
  to the screens that work on files.
- The 3x3 jogs, drives the spindle and sets the jog values: `4`/`6` Z-/Z+,
  `2`/`8` X-/X+, `7`/`9` spindle CCW/CW, `5` spindle stop, `1`/`3` down/up on
  the value the mode is using. The jog keys name their own axis, so `4` must
  move Z even while X is the picked line, and the pressed key stays lit long
  enough to see which one the machine took.
- The value readout beside the pad shows both values the mode can use -
  `STEP 0.100 mm` and `FEED 500 mm/min` - with the active one filled, and
  `1`/`3` change the filled one (`STEP` in step mode, `FEED` in continuous
  mode). The block the next jog sends must carry the value on screen: a step
  block at the new step, a feed jog at the new feed.
- The readout, the stop and the room left are axis millimetres, so a step of
  `0.100` must move the X readout by 0.100 and a feed must land on the stop
  instead of stopping half way: the lathe programs X as a diameter (G7, the
  parser's default), so the X word a jog sends is written twice the axis
  distance. Check both X and Z (Z has no such scaling), and check the same in
  G8 (radius) mode, where the word is the axis distance.
- The stop is a wall the axis may not cross. Standing exactly on it must not
  lock the axis (`*` at the current point is the normal way to set it): the
  first move away is what tells the panel which side the axis works on, and
  only the direction that would cross is clamped (a step ends on the wall, a
  feed stops at `At stop`).
- Feed mode (`#`, `FEED`): the panel says `Continuous feed` (or `Step jog`) and
  the jog keys of the picked axis light up. Holding a direction key feeds toward
  the stop; letting it go stops the axis where it is. `*` stops a feed, an alarm
  stops it, and the wall ends it. A feed needs a standing axis: the distance to
  the wall is measured from a position, so a
  feed pressed while the axis is still moving says `Wait for stop` instead of
  letting the controller answer with `Error 8`, and `At stop` is the refusal on
  the wall itself. Away from the stop - and with no stop set at all - the feed
  is a bounded move rather than a refusal, so the axis is never left with keys
  that do nothing: the bound is the axis travel the machine states (`$130`), or
  25 mm per hold when it states none. While a jog runs the header shows the
  controller's own notice for it.
- Machine checks (the host bench proves the blocks and the jog state only -
  `test_nc_ui.py --streamtest` asserts a step jog delivers both `G91 G1 ...`
  and `G90`, and `--feedtest` runs the held feed and the value keys on the real
  parser, planner and virtual MCU): a step jog actually moves the axis and
  leaves the machine back
  in G90, a held key keeps the axis feeding at the jog feed, release and `*`
  stop it where it stands, the axis never passes the stop, the USB console still
  accepts commands after a jogging session, the spindle starts and stops, `0`
  zeroes the picked axis, `D` + value + `D` sets it, and the feed override steps
  without touching the program.

- TOOLS mode edits only the active `.t` tool file.
- Programs link tools by `Tn`; adding/editing tools does not append rows to the active NC file.
- Tool glyphs for one-digit and three-digit orientations are centered and readable.
- TOOLS layout: the table clears the header's bottom edge, and the tool tip
  details sit near the bottom of the pane. With eight tool rows visible the
  table still clears the details; with fewer rows the gap is between them.
