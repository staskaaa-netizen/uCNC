# NC Pre-Alpha Testing

Use this as a short hardware pass list while NC is still pre-alpha.

## Boot and Stability

- Screens name themselves once: the tab strip across the top marks the current
  screen, the header shows the run state in RUN and otherwise only messages, and
  the file name appears once, as the editor's own first row. Walk all six
  screens and check nothing is named twice.
- Messages live at the right end of the tab strip, left of the `>` hint: command
  errors, run errors, transient messages and the screen's own status all land
  there. A fault is not red text but a **block: white letters on red**
  (`#E60000`, which the panel's 5-6-5 wire format rounds to (231,0,0)) - it
  should be read before the words are. The block starts in the strip's message
  area, continues down over the DRO beside the machine's figures, and wraps its
  text over up to `NC_FAULT_LINES` rows (`nc_visual_draw_fault()`). Its geometry
  is what keeps the readings visible, and it is checked that way: it stops left
  of the F/S column and above the DRO's bottom-right corner, which belongs to
  the uCNC state, and otherwise runs **to the panel's right edge**. The DRO
  block below carries the numbers and that state; there is no message line in
  it.
- The DRO's state sits low in the band: bottom-right, text top at `hy + 50`, so
  its label's bottom edge is one pixel above the bottom of the band (it moved
  down in two bench steps, 10 px then 3 px, and this is the floor). The fault
  block ends well above it.
- The band has **no rule along its bottom** (bench: "we have one black line under
  dro, now it is obsolete"). It was drawn when the band and the code pane below
  it were the same grey, so that the two read as two things; the band's own
  colour is the boundary now - it wears the panel's green while a run is going -
  and the line was one more thing on the glass that said nothing. The columns'
  vertical separators stay: those divide readings *inside* the band.
- A fault **stands until it is answered**, and the two answers are the panel's
  own: `# RELOAD` in RUN (the RESET action: run reset + file re-read) and `* BACK`
  in the file list. A fresh run (`1 SINGLE`, `2 FROM`, `3 FULL`) clears the
  previous message before it starts, because the operator has read it and is
  doing something. Nothing else clears it - not a screen change, not a repaint.
  `--labeltest` shows a fault, presses `#`, and checks the red is gone from the
  whole strip and DRO.
- The frame counter is a **debug reading and lives below the DRO**, right side
  (`NC_FPS_Y`), not in the header band: nothing the operator reads makes room for
  it, and a fault block may run over the corner where it used to be. It is drawn
  at the end of both frame paths, so it keeps counting while a run repaints only
  the live band.
- The DRO takes three columns from the panel edge: the **work** position
  (`X`/`Z`, large, `parser_machine_to_work()`), the machine figure each work
  value is cut from (normal font, its own cell, `OFF X.. Z..` named above it),
  then `F`/`S`. The two left cells must never meet, and the numbers must still
  be readable after `G92`/`G10` puts a large offset between them.
- The DRO's bottom-right corner carries the controller's own state - `uCNC IDLE`,
  `RUN`, `HOLD`, `JOG`, `DOOR`, `ALARM`, `KILLED`, `LIMITS`, `POS LOST`,
  `SETTINGS`, `ERROR` - on every screen, so "is the machine running, held or in
  a fault" needs no screen change to answer (`nc_visual_state_label()`). A fault
  wears the same white-on-red label there as the message area does. The tab
  strip still carries the sentence explaining it. `--labeltest` reads the drawn
  frame back and asserts both labels **and the fault block's four edges** (red
  over the message area; no red over F/S, over the FPS/hint column, or over the
  state corner), so the layout is checked as drawn, not as intended.
- The run state is said **once**: in the DRO's own corner (`uCNC IDLE`, `RUN`,
  `HOLD`, `JOG`, `DOOR`, `ALARM` and the rest - `nc_visual_state_label()`). The
  tab strip carries the screen names and, at its right end, the message; it must
  not repeat the state the corner already has (it used to say `IDLE` in the
  middle of the strip as well).
- The DRO wears the panel's **green** (`NC_VISUAL_HEADER_RUN`, the palette's
  `green` - the 16-colour table is full, so a running colour has to be one the
  panel already owns) while the machine is in a run - including a held one, and
  on every screen, so a glance at the top says whether the machine is running.
  **A fault takes the colour back**: an alarm, a lock or a refused line puts the
  band in its own dark grey, even while the run is still active, because a
  machine stopped by a problem must not still say "running" - the red block on
  the right is what carries the alarm. The tab strip above stays grey in every
  state: the screen names live there. The DRO's figures are dark in all of them
  and must stay readable on the green. The states can be dumped from the host build
  (`nc_ui --keys F4 --ticks 2` for a program sitting idle against `--keys F4,3`
  mid-run, with `/D/nc_state.txt`'s `RUN=` pointing at a program); what needs
  the machine is that the colour is visible from across the shop, and that it is
  there while the machine runs and not only in RUN - start a run and walk the
  screens, then trip a fault mid-run and check the band goes grey. It does not
  shout when nothing is happening: RUN up with a program loaded and nothing
  started stays grey. `--labeltest` checks all four states by reading the drawn
  frame (idle grey, running green with the strip still grey, fault grey with the
  block up, and grey again after `#`).
- The editor's file-name row is reachable: hold Up from line 1 and the row above
  it takes the highlight; Down returns to line 1. Enter on it opens the file
  list at the folder the open file lives in.
- Moving the cursor reports nothing in the header: the editor numbers the lines
  and highlights the current one, so no "Line n" message should appear.
- Header and footer spacing: with the tab strip in place the header still
  clears the code rows, and the footer keys sit on the bottom edge of the panel
  with no margin below them.
- Footer key shape: every key is cut at its **top-right corner**, the cut being
  a quarter of the key's height (`NC_KEY_CHAMFER_DIVISOR`) - the page background
  shows through the cut, the cut's edge is drawn like the rest of the outline,
  and the key's number still sits in the untouched top-left corner. On all eight
  slots of every screen, including the filled ones (`STOCK`/`TRACE`/`ROUGH`/
  `DIM`): the cut must be square (a straight edge, not rounded), the same size on
  every key of one strip, and a three-line label must stop before it instead of
  running over the cut.
- The preview layer toggle is labeled `TRACE`, and no other screen's key reads
  `PATH`; `--padtest` checks the label the strip draws.
- EDIT full screen (`# FULL`): the preview is the screen - it takes the whole
  body, with no code lines at all - and the strip shows the preview's own keys:
  `4 STOCK`, `5 TRACE`, `6 ROUGH`, `7 DIM`, `# FULL`, and **no delete** (the
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
- RUN has **one cursor**: the pane marks the line in play - the line a step was
  taken from, the unit a run is walking through, the line the keys last moved -
  and `1 SINGLE` acts on what is marked - that line, or the G7x block it sits in
  (`nc_g7x_block_containing()`, so a lone contour row is never sent out of its
  cycle). The mark used to be the document's cursor, which a streamed run never
  moved: the pane showed the line after the block while SINGLE re-sent the block
  itself, and only the line keys - which sync the two - put it right. `--runtest`
  pins both halves now (the step after a run sends the block the mark is in; a
  line outside every block is its own step).
- RUN marks the cursor **and the block it is working in**: the line in play
  wears the bright selection yellow and the rest of its cycle block wears
  the pale `yellow_light` the palette already owns (`NC_VISUAL_SELECT_BLOCK`;
  the 16-colour table is full, so a second mark reuses a colour instead of
  adding one). A line outside every cycle marks only itself, and the pale rows
  clear with the mark. Both the mark and the block come from the same
  `nc_g7x_block_containing()` the sender uses, so no extracted path list has to
  be kept in step with the stream. `--blocktest` reads the **drawn frame** row by
  row while the mark is inside the fixture's `G71` block and again with it on
  the `T2` row above: marked `#FFF700`, block `#FFF784`, plain `#C6C3C6` as the
  5-6-5 wire format rounds them. Check the pale yellow is *readable*, not just
  present: the block rows keep the pane's dark text and their line numbers.
- **RUN's mark is the line in play, not the sender's position** - two bench
  sentences, one answer: *"now it runs but it marks also next g71. which it
  should not mark"* (a `1 SINGLE` left the pane on the *next* cycle header, a
  line that had never been handed over and has no path to draw) and *"it still
  marks next row with g71, not the one starting with N50"* (the step was taken
  from the `N50` row, and the mark jumped **up** to the header above it).
  - A step taken from a row inside a cycle keeps the mark on that row - the line
    the keys and the digits are on - with the cycle's other rows pale around it.
    The whole block is still what goes out; the mark is not where the block
    starts but where the operator is. A run that walks on its own (`2 FROM`,
    `3 FULL`) marks the unit it is on and then the next one when the machine
    starts it, and a unit that has finished keeps the mark until the operator
    takes the cursor (a line key, a new step, `# RELOAD`).
  - Nothing here moves on by itself: the *sender* walks on - it is the panel's
    progress through the program and the line the next step comes from - so the
    two are deliberately different answers. `nc_run_display_line()` is what the
    pane reads, `nc_run_line()` is what the next step sends, and `--blocktest`
    reads the frame with the machine idle to require the mark on the stepped
    line **and** the sender past the block.
  - A `STOP` leaves the mark where the tool stopped - the unit it was given -
    which is the same question as the stop itself: a bench item.
- **EDIT marks the path the same way** (bench: "did you have an idea to use same
  highlight of current g7x main line its path in edit?"). Two colours, one
  meaning, on both code screens: the bright line is the line in play and the pale
  rows are the rest of the cycle it heads. In RUN the line in play is the unit
  the machine is on; in EDIT it is the cursor - the line the digits type into and
  the line the legend names - with its path pale behind it, so a change to a
  profile row is made with the block visible around it. The editor deliberately
  keeps the bright mark on its own cursor instead of moving it to the block's
  header (which is what RUN shows): an editor whose cursor mark sat on another
  line would be lying about where the typing goes. A cursor outside every cycle
  marks only its own line, and moving it in and out of the block moves the pale
  rows with it. `--blocktest` walks **both** screens and both cases (in the cycle
  and outside it) from the drawn frame; the two publish the mark from different
  places, so a fix to one screen cannot quietly leave the other flat.
- **A finish cut's path is the range it names** (bench: "it does not mark path
  region if g70 is after g71 with region described"). `G70 P Q` sits *below* the
  profile it replays, so it is in no block and had no path: put the cursor on it
  and the numbered rows it will cut (N(P)..N(Q)) are pale, the roughing header
  and the rest of the block are not. That is one answer, not two:
  `nc_g7x_range_above()` finds it and both the pane (`nc_g7x_line_path()`) and
  the preview (`nc_emit_feed_g7x_range_above()`) use it, so the rows drawn and
  the rows expanded cannot disagree. A range that is not there marks nothing -
  the same "report it, never guess" rule the preview follows.
- **RUN owns the cursor** (bench: "i can move cursor in run mode which should be
  non - if i do run ucnc is owner of the cursor"). While a run is in flight the
  line keys do not move the line: it is the sender's, and a key that moved it
  would move what the program does next, which is the fault the one-cursor work
  exists to prevent. The panel says `RUN owns the line` instead of doing nothing.
  The keys work again the moment the machine is idle - that is what they are for
  in RUN, choosing where `2 FROM` starts.
  - With it, a one-shot step (`1 SINGLE`) no longer leaves the panel believing a
    run is active: the run state for a step is the machine's own "there is motion
    left", so the DRO goes back to grey and its corner reads `IDLE` when the step
    is done, instead of staying lit until the next reset.
- **The RUN sender is paced, one block at a time** (bench: "g7x sends one
  command and wait till buffer is empty. pause here is ok and proper"). The old
  sender handed the reader the whole program at once, so the marked line ran
  ahead of the tool by the depth of the controller's look-ahead - the bench saw
  it as "it is marking next line while running previous one". Now
  `nc_run_pace()` hands the machine one *unit* (a whole G7x block, or a single
  line outside every block), waits for the machine to have finished it, and only
  then hands over the next one. The machine is what says when a unit is over:
  a cycle that is still collecting rows keeps taking them
  (`g7x_parser_collecting()`, or the contour would never be completed), and
  anything else waits for `planner_buffer_is_empty()` and `itp_is_empty()`.
  - **The pause is the price and it is accepted**: every move now comes to rest
    before the next block is handed over. Watch a plain program of short moves
    and a roughing cycle run through from RUN and judge the finish; if the
    stutter costs more than the exact mark is worth, the pacer's gate is the one
    place to relax.
  - **The mark follows the machine, not the reader**: the marked line is the
    unit that is running (the block's header for a cycle, with the block pale
    behind it), so it must not be seen running ahead of the cut. Check the
    fixture at the bench: the mark stays on the `G71` header while the cycle
    cuts, and is still on it when the cut ends - it moves on only when the pacer
    hands over the next unit, never on its own.
  - **The reader is the console's between blocks**, the same way it is between
    one-shot blocks (a jog, a touch-off): the panel takes it again for the next
    unit. So a desktop sender must stay quiet during a panel run - a line typed
    into the console while a program waits lands in the middle of it.
  - A controller in an **alarm** is never fed: the pacer stops with the run
    still armed, the DRO says ALARM, and `# RELOAD` is the operator's way out.
  - Software-verified by `--pacetest`: it watches the sender and the machine
    between main-loop passes of the real parser, planner and virtual MCU, and
    fails if a new unit goes out while one is running, if the mark ever names a
    line the sender has not handed over, or if the cycle is not waited for; it
    also checks the machine actually ran the program (the last line's position).
    `--runtest` still pins *what* is sent and in what order.
- RUN: `2 FROM` runs from the cursor to the end of the program and `3 FULL` runs
  the whole program. FROM and FULL only *armed* the run once - the status said
  "Run from line N" and the machine received nothing - so the software check is
  `test_nc_ui.py --runtest` (the rows the panel sends, in order, with the
  `G970`-`G973` setup rows skipped, and the run done at the end). What still
  needs the machine: the axes actually moving through the whole program, Hold
  and Stop inside it, and the console answering afterwards.
  - Hold and Stop during a paced run are bench items too: the pacer stops
    handing over while the panel is held and never feeds an alarmed controller,
    but only the machine shows how the axes behave when a block is held between
    two others.
- RUN's live cursor is held inside the area the live path redraws, not the whole
  pane: that path repaints one band around the stock (`nc_live_band()`, the same
  rectangle its clear uses), so a cursor outside that band would be drawn once
  and never taken back. Feed an axis off the stock (a wild touch-off is the
  bench way) while a run is active and check **no cursor ink is left behind**
  below the stock once the axis comes back; the DRO is what says where the axis
  is while it is out there. The host build can show the same case headlessly:
  point `/D/nc_state.txt`'s `RUN=` at a program whose `G0` parks the tool far
  past the stock, start `3 FULL` and dump a frame mid-run - the cursor must not
  appear in the pane (with the guard against the pane instead of the band it
  does, ~30 px below the stock box in the 800x600 layout).
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
- Line endings: a program written on a PC editor - or checked out by git on
  Windows, which is where the station's demo comes from - arrives as CRLF, and
  it has to load as the same document an LF file does. It did not: the loader
  carried the CR into the line, and the wrap loop could not consume it, so it
  inserted a tab line per pass until the document hit its line limit and the
  file failed with `too many NC lines`. The loader drops the CR now (a lone CR
  is dropped too, not treated as a break, so CRLF does not become a blank line)
  and the wrap loop stops when a chunk measures empty with text still there.
  Software-verified: `tests/test_nc_ui.py --demotest` loads the demo, rewrites
  it on the card with CRLF endings and requires the same line count; and
  `--filetest` passes on a CRLF copy of the NC fixture.
- `0` opens the file list on EDIT, TOOLS and RUN (no footer slot, by design);
  MANUAL's `0 ZERO` still wins there. In the list, browse up to the card root:
  the text files must be listed beside the programs (`presets\` among them).
- Opening a `.txt` shows it as text, and the preview shows the same text
  read-only (line numbers plus the rows, `nc_preview_text_file()`) instead of a
  blank "no preview" pane: the preset file is the file this is for, and the
  operator opens it to read or edit it. RUN still answers `Not a program file`
  instead of feeding it to the parser. `--filetest` pins that a `.txt` loads,
  saves, is not `nc_path_supported()`, and that its preview draws rows low in
  the pane (a one-line caption cannot).
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
- The legend **keeps the mark of the row it covers**: while the cursor is inside
  a cycle, the row above it is part of the pale path, and a legend drawn on plain
  grey would punch a hole in the path at exactly the moment the operator is
  editing a word of it. It is the same dark text on the row's own colour, so it
  reads on both.
- Screen switching is quick: with an unchanged editor it should be a card read
  and nothing more. With unsaved edits the program is written before the buffer
  is reused (that is deliberate), so a switch from a dirty editor is the slowest
  case. The remembered state is written once the screen has been idle, so it
  must still survive a reboot after switching screens and waiting a moment.
- **There is no save key: the panel saves** (bench: "save is basically not needed
  too. it is fine with auto save. and it should be"). An edit is written when the
  screen goes quiet - the same 400 ms idle period the remembered state uses,
  `nc_visual_idle_tasks()` - and, as above, before the buffer is reused. The
  editor's own dirty `*` in the file-name row is the confirmation: it appears
  while typing and goes when the write lands.
  - One write per quiet period, not one per keystroke: a key starts a new period,
    so a card that cannot be written to is retried when the operator works again,
    not on every main-loop pass - and the `*` stays up until it succeeds.
  - At the bench: type a digit into a word, stop, and the `*` should clear on its
    own within a blink. Then pull the card mid-edit: the `*` must stay, and
    switching screens must still refuse with `Save failed - ...` rather than
    dropping the edit.
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
- 3x3 helper: OPS `6 SETUP` inserts the **stock dimensions** as one block -
  `G970` (preview extents), `G971` (stock), `G972` (chuck clamp), `G973`
  (preview mode) - at the cursor, in that order (bench: "it seems it lacks
  ability to insert stock dimensions?"). Before this the block could only be
  typed one G-code at a time (`3 WORD` then `1 G` writes a single line), and the
  it (`docs/nc-preset-file.md`, id `16`) had no menu entry at all. The entry is
  named `SETUP`, not `STOCK`: `STOCK` is the preview footer's display toggle, and
  two meanings for one word on two screens is how a panel gets misread. Typing
  the numbers is still the editor's own field flow - the inserted lines are
  ordinary program text.
- 3x3 helper: OPS `1 INS` is a **template**, not a hardcoded blank insert: it is
  section `[11]` in the card, whose compiled default is an empty line, so the
  same key can be told to write a separator comment or a command the operator
  keeps needing (bench: "blank line could also be made using templates. same as m
  or s command where appropriate"). Its `line=` may be empty, which is how a
  blank line is written as one inserted line - the panel has no other way to make
  one, and now no need for one.
- 3x3 helper: the TOOL pad's machine words are templates too (`[23] M6`, `[24] M3
  S1000`, `[25] M5`, `[26] M4 S1000`). With them, *every* entry in every pad that
  writes text into the program is the card's - the check of that is the pad table
  in `docs/nc-preset-file.md` against `nc_menu.c`'s submenus, and the one thing
  they were not before is why a card could not decide its own spindle speed.
  - What is still the panel's, and is not "hardcoded text": the entries that edit
    a word on the line (G7X `4 Q`, `5 N`), the `T` field (`1 SELECT`), and the
    actions - the file list, delete, opening the tool table. None of them writes
    a line of program text of its own.
- 3x3 helper: **a pad is the file**. Every key that *writes* something is the
  section whose id is that key path, and the section's `name=` is what the key
  reads as - the panel keeps no second list, so a key cannot say one thing and
  write another, and a card that adds a section with a free id (`[12]`, `[49]`)
  gets an entry of its own on that pad. What is left in `nc_menu.c` are the
  entries a key *does* rather than writes: `Q`, `N`, the `G` field, the `T`
  field, the tool table.
- 3x3 helper: `3 WORD` is the pad of single lines. `1 G` is the field that writes
  one line by name or number (a G-code from the dialects's vocabulary), and
  beside it the card's own entries - `2 CHMF` and `3 RND` are shipped as `[32]`
  and `[33]`, each a row that starts with a space, so they add their word *to the
  line the cursor is on* rather than starting a new one (bench: "chamfer and round
  could be just separate inserts in 3 - which will be renamed from G to Word for
  example? so it can host all single lines, universal G and some wild ones like
  CHMF and RND as extra lines to be inserted"). The G-codes stay the code's
  vocabulary; everything else in that pad is the card's.
- 3x3 helper: the pads hold **only what has no other key** (bench: "except save -
  we do not need any extras. `*` is delete, open [is `0`] so 5 is not needed, end
  could be skipped etc"). What is not there: the file list (`0` is the file key),
  delete (`*`), the end mark besides the G7X menu's `6 G80`, the save (the panel
  writes by itself when the screen goes quiet), and the line/arc rows - the `3
  WORD` field's own templates for `G1`/`G2` already write them, word for word.
- Fanuc's increments, `U` and `W` (`3 WORD` then `4`/`5`, appended to the row
  under the cursor): on a row that moves they are X and Z as distances from where
  the tool is, and the panel resolves them into the ordinary absolute line the
  controller reads - the program keeps the spelling. Software check:
  `tools/test_nc_ui.py --uwtest` proves the same profile written both ways
  leaves the sender as the same lines, plain *and* inside a `G71` block, leaves
  an increment with no absolute base as written for the controller to refuse, and
  does not rewrite the document; the same script renders the two spellings and
  compares the drawings, so the preview's contour and its callouts read the rule
  too. Machine checks: a `G1 W-10` travels 10 mm, and a `U` is a diameter
  increment in G7 and a radius one in G8.
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
- The station's side panel (the PC programming station, `tools/nc_ui_win`): the
  screen names itself (`nc_visual_screen_name()`), says in its own words what it
  is for and how its keys drive it (`nc_visual_usage()`), and every pad key
  answers with one meaning (`nc_visual_key_meaning()`) - the footer entry that
  carries it, the screen's own word for a key the footer does not name, or that
  it steps a field. `--padtest` checks that every footer key reads as a menu
  key, that `B`/`C` read as step keys in every mode, and that every screen has a
  name and usage lines; a screen added without them fails there. The station's
  spindle is read from the signals the tool drives (PWM0/DOUT0), not from an
  encoder - `--spindletest`. All software-verified; the panel and the strip are
  one image, so the machine pass is the same panel pass as the keypad above.
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
- A key that changes what the panel shows asks for the repaint itself: on a
  screen that is otherwise idle the dirty flag is the only thing the module's
  update hook watches, so a handler that moves the cursor and returns without
  it leaves the highlight where it was until the next footer key happens to set
  the flag. RUN's line keys (`B`/`C`, which move the run line) did exactly that.
  Software-verified by `test_nc_ui.py --dirtytest`, which also checks the other
  half: while a run is armed the panel keeps asking to draw by itself, and that
  periodic frame is what carries the cursor through a stream of emitted lines.
- Presets: boot with a card that has no `presets` folder; the folder appears and
  the pads still insert the compiled entries. `--presettest` covers this.
- Presets: put a file at `presets\41.txt` (first row a new name, then the rows),
  restart, and check the OD entry in the floating 3x3 reads the new name and
  inserts the file's rows; delete the file and the compiled entry is back.
- Presets: give a free address a file (`presets\12.txt` with `COOLANT` and `M8`)
  and check OPS `2` offers it - this is how a word the pads do not offer is
  added, with no code change.
- Presets: boot with no card, insert the card afterwards, press a key, and check
  the folder is found without a reboot. The screen must keep working with the
  compiled entries while the card is absent.

## Preview

- `G970 X/U/Z/W` stock/setup gives expected stock size and origin.
- Chuck/stock holder is visible and does not cover the working contour.
- Contour labels `C1`, `C2`, ... stay readable and follow corners.
- Rapid lines are dashed and visible; feed/finish lines are distinct.
- A `G71 ... P100 Q200` range draws the same contour as the equivalent
  `G80`-terminated program and leaves the cursor on the line after `N200`.
- A Fanuc two-line header previews as one cycle, and a step taken from **any**
  row of it sends the same block: both header lines plus the numbered profile.
  That has to hold whichever line carries the range - the usual
  `G71 U1 R1` then `G71 P100 Q200 U0.5 W0.25 F120`, and the bench's file with
  the range on the *first* line (`G71 U2 R1 X0 Z0 F50 P50 Q55` then
  `G71 U2 R0.2 X0.5 Z0.5 F450`). The second line of a pair used to be scanned
  as a cycle of its own, so a step from inside the contour sent the profile
  without the line that names its range: `nc_g7x_block_containing()` now asks
  for the pair's first line (`nc_g7x_header_head()`) before it scans, and every
  row of the cycle answers with one block. `test_g7x.py nc` pins the scan for
  both shapes.
- **A cycle's rows are the same whichever way it is written** (bench: "now it
  colorize both g71 and path if any g71 is select. with pq or with g80"). The
  block is the header pair, the contour **and the end mark**: `N(Q)` names the
  profile, and a `G80` written after it is still that cycle's end mark, so it
  belongs to the same block - the rows a cycle with no range at all already had.
  Until this, a range-terminated cycle stopped at `N(Q)`: a step from inside it
  handed the machine the contour and left the `G80` behind as a unit of its own,
  and the pane drew a different pale block from the one the same contour drew in
  a file that ends with `G80`. Put the cursor on any row of either flavour and
  that row is bright with the whole cycle pale; a header with no rows still marks
  only itself.
- A numbered range whose `N(Q)` block is missing, repeated or out of order
  shows the preview error instead of drawing a partial contour.
- `G70 P Q` previews as the finish cut of the range **above** it: the roughing
  block, then the same profile again (rapid to the `P` block, the rows at their
  own feed), with the source continuing after the `G70` line. A `G70` whose
  range is not in the file shows the preview error rather than a partial
  drawing.
- `8 FINISH` in the `G7X` submenu inserts `G70 P0 Q0` where the cursor is (the
  helper's labelled line is replaced by it), the way `6 G80` inserts the end
  mark, **including on a card that has no entry files at all**: an address with
  no file keeps its compiled entry (`--presettest` checks that a folder holding
  only `41.txt` still offers `43` and `48`). The reverse is also true and worth
  knowing at the bench: a file that *does* exist wins, so `41.txt` writes what it
  says - edit that file on the panel or on the PC, or delete it to go back to
  the compiled entry.
- Writing a P/Q range on the panel: `4 G7X` then `5 N` on each profile row puts
  `N0` at the start of the line and picks it, so the number is typed straight in
  (`N100 G1 X20 Z0`); `4 Q` on the header picks the `Q0` the preset already
  wrote. `P` and `Q` are those `N` numbers - not the line numbers the editor
  shows - and a range needs at least two rows, rising, with no `G80` inside it.
- A refused cycle says why on the glass, not just the status number:
  `Line 4 error 20: G73 not implemented`, `Line 5 error 20: no G41/G42 in a
  cycle`, `Line 8 error 3: G70 range not run yet`. The reason comes from the
  module that refused the line and is taken once, so a later unrelated error
  cannot borrow it.
- The editor's legend names the word under the cursor: select `P`/`Q` in a
  `G70`/`G71`/`G72` header and it reads "Profile start block"/"Profile end
  block", `N` reads "Block number", `U`/`W` read "Depth/pass". `--vocabtest`
  (part of `test_nc_ui.py`) fails if any word of any template the panel writes
  falls back to the legend's "NC word".
- Future: zoom/pan has a visible cursor/anchor.

## G7x Runtime

Automated software checks: `python tools/test_g7x.py all` with MinGW GCC on
PATH. The parser fixture intercepts G33; it does not validate spindle timing.
The following machine checks remain open:

- `G71/G72` collect contour until `G80`.
- Source contour lines do not execute directly during RUN.
- **RUN sends `G70 P Q` as an ordinary line** and the machine replays the range
  it collected earlier in the same run: the machine pass is that the finish
  follows the profile with no roughing, and that starting RUN at the `G70` line
  (which never streamed the range) refuses it instead of cutting something else.
- **A profile row's `F` is the finish feed and its `S`/`T` are not acted on**:
  check the feed change at the row and that the spindle speed/tool do not change
  mid-cut. The machine checks for the `P` block as an approach, a rapid written
  inside the profile, the compensation refusal and `G73` are listed in
  `../g7x/TESTING.md`.
- A contour written with increments runs as the selected cycle: `test_nc_ui.py
  --uwtest` proves the rows reach the generator as the points they mean, so the
  generated motion is the same as the absolute spelling's, row for row. Machine
  testing must confirm the rough pass follows the drawn path and the program's
  approach clears the work.
- Generated rough and finish blocks execute through parser helper.
- The cycle ends back at the clear point - `G0 X<clearance>`, then the Z return -
  which is the corner a `G0` before the cycle established (Fanuc's start point
  for a stock-removal cycle). The per-pass returns are trimmed to what each pass
  needs and no rapid repeats a motion that would not move the tool (bench report,
  2026-09-21: the repeated rapids, the `G72` return to the start Z between passes
  and the old full return were all in that class; the per-pass trims stay, the
  end-of-cycle return came back because the operator's program expects the tool
  where its `G0` put it). The **last roughing pass is the allowance boundary**,
  not a whole depth above it, so the finish only takes the allowance.
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
- G76 preview is unsupported and shows an explicit error (`Preview: ... at line
  N`): `test_g76_preview_error()` in `nc_emit_host_test.c` pins that the emitted
  stream stops instead of drawing a partial thread. The message appears one
  frame after the notice, so a single-frame dump will not show it - check it on
  the machine by opening a threading program and looking at the message area.

## Tools

## MANUAL

- The split on MANUAL is current versus wanted. The **header DRO** is the
  current state, and it is the only place the work position, the machine figures
  and F/S appear - the pane must not repeat them. The **pane** is what the
  operator sets up: one line per axis with its two stops and the room left to
  each, then the STEP and FEED values the `1`/`3` keys change, lined up with the
  values above them: name in the axis column, number right-aligned in the stop
  column so every number shares a right edge, in the readout font, with the row
  in use marked. There is **no distance-to-go line** under a stop. The line the
  keys act on is marked with the selection colour across the values it owns, not
  the whole pane (it stops short of the pad); it must be obvious which line is
  picked without reading the values.
- The spindle keys start at the speed the machine has (`S` from the modal state,
  remembered across a reboot) - `M3`/`M4` must not force a default: give the
  machine an `S` from a program, then start the spindle from MANUAL and read the
  block that goes out.
- A side whose stop was never typed shows the axis limit the setup states,
  dimmer than a typed stop, and a move is stopped by it: on a machine that
  states its travel there is no `--` and no "no stop" state. Only a machine that
  states no travel for the axis has nothing to fall back on, and then the side
  shows `--` and a feed is bounded by the travel/cap instead.
- The axis row is named for what it carries: the label reads `X LIMIT`/`Z LIMIT`
  (the position is the header DRO's, not this row's). The label is the pane's own
  font, the one the stops and STEP/FEED use - not the header DRO's larger one - so
  a row reads as one table, and a `D` touch-off value lands between the label and
  the first stop (`NC_MANUAL_COL_TOUCH`), in that same font.
- A value being typed shows in the cell it belongs to, in the editor's word
  colours with a cursor - the stop cell for a limit, the touch-off column for a
  value `D` types - and nothing is drawn in a separate field line.
- A side with no stop shows `--`: the stop is not silently zero.
- `B`/`C` (`AXIS-`/`AXIS+`) pick X or Z. `0` (`ZERO`) writes the work offset
  for the picked axis, `D` (`TOUCH`) opens a value field that a second `D` (or
  Enter) applies. The footer carries only those four; files and deletion belong
  to the screens that work on files.
- The 3x3 jogs, drives the spindle and sets the jog values: `4`/`6` Z-/Z+,
  `8`/`2` X-/X+ (up is the smaller diameter: the preview draws X downward from
  the top of the stock), `7`/`9` spindle CCW/CW, `5` spindle stop, `1`/`3` down/up on
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
- The stops: each axis has a minus and a plus limit, typed on the pad. `*` opens
  the minus field (digits type, `B` is the sign, `C` the point), `*` again takes
  it and opens the plus field, `*` once more takes that; `D` puts the axis limit
  the setup states in the field, `A` leaves the field alone, and an empty field
  takes that same limit. A step into a stop ends *on* it, a step from on it into
  the same side is refused, and the other side still moves - neither side is a
  lock, which is what the old "first move defines the side" behaviour was for.
  A held feed covers exactly the room to the stop it is headed for and stops
  there; with no stop on that side it is bounded by the travel the machine
  states. The pane shows both limits with the room left to each. Software check:
  `test_nc_ui.py --stoptest` and `--feedtest`; the machine pass - that the axis
  really stops on each limit, and that the numbers match the travel in `$130` -
  is still open.
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
  parser, planner and virtual MCU; `--uwtest` is the same kind of proof for
  Fanuc's increments - it expands the same profile both ways and compares the
  lines): a step jog actually moves the axis and
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
