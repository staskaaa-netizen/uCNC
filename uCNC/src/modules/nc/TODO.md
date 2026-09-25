# NC TODO

## Handoff: where the panel stands (2026-09-19, end of session)

Read this first; the sections below are the history behind it.

Software status: `python tools/test_g7x.py all` (ALL PASS, 0 failures),
`python tools/test_nc_sender.py`, `python tools/test_nc_ui.py` (panel frame,
3x3 helper, the 3x3 path builder's `--buildertest`, preset contract, and
`--streamtest` for the panel's one-shot blocks, `--runtest`, `--dirtytest` and
`--blocktest` for what RUN marks and sends) all pass, `pio run -e
RP2350-LEANCAM-LVDS` builds and was uploaded to the board (the single later
change was a comment, so the running image matches this source),
`-e RP2350-G7X-MODULE` builds. Only what says "bench" below is verified on the
machine; everything else is software-verified only.

The 3x3 path builder (`nc_path_builder.c`, `docs/nc-path-builder.md`) is the
session after that handoff: every suite above passes with it, both firmware
targets build, and the image carrying it has **not** been flashed yet. Those
results predate the 2026-09-24 scope reduction and need to be rerun against the
current source.

### Path builder scope (2026-09-24)

The original prototype also created a G71 block, inserted its header/end mark
and clearance move, and rolled those rows back on cancel. That mixed template
ownership into point entry. Current behavior is narrower: insert a cycle from
the NC G7X vocabulary first; PATH then appends rows only inside a closed block
found by `nc_g7x_block_containing()`. It refuses outside one without changing
the program, and refuses G20 because its step table is in millimetres. The pad
retains point undo and cancel for rows it inserted during that session. The
older design notes below record the superseded prototype.

## The programming station's side panel (2026-09-24)

The desktop shell (`tools/nc_ui_win`, the PC programming station) grew the
parts a PC operator needs, and none of them is a second copy of a screen's
rules - each comes from the screen that acts on the key:

- `nc_visual_screen_name()` names what the screen is showing (`MANUAL`, `EDIT`,
  `TOOLS`, `RUN`, and `FILES` / `PREVIEW` / `DRAW` while a view has taken the
  screen over);
- `nc_visual_usage()` hands out the screen's own lines saying what it is for and
  how its keys drive it. The off-menu keys (`B`/`C`, `#`, the builder's pad) are
  named there, because the footer never will name them;
- `nc_visual_key_meaning()` is the one answer per pad key: the footer entry that
  carries it (drawn green on the strip), the screen's own word for a key the
  footer does not name (drawn grey), and whether the key steps a field or the
  axis - the shell draws those keys with the arrow they act as. `--padtest`
  checks that every footer key has a menu meaning, that `B`/`C` read as step
  keys in every mode, and that every screen has a name and usage lines;
- the station's spindle is read from the machine's own signals
  (`tools/nc_ui_win/host_spindle.c`: PWM0 the speed, DOUT0 the direction - the
  pins `spindle_pwm` writes) instead of from an encoder the desktop does not
  have, checked by `--spindletest`.

All of it is software-verified by `tools/test_nc_ui.py`. What the station still
cannot do is drive a real controller: RUN streams to the virtual machine (see
"Remaining NC integration").

## G7x cycles the panel now hands over (2026-09-23)

`G70 P Q` is implemented in the g7x module, and the panel's part of it is
deliberately small: **RUN sends the `G70` line as ordinary source** and the
machine replays the range it collected earlier in the same run (see
`../g7x/README.md`, "G70: the finish cut of a collected range"). Nothing in NC
expands a G70 - a range that never streamed past is refused by the module.

The *preview* is the one place that has to know where a G70's range lives: the
rows are **above** the line, not below it, so `nc_emit.c` walks back to `N(Q)`,
then up to `N(P)`, and feeds those rows (`nc_emit_feed_g7x_range_above()`),
instead of running the forward scan every other cycle uses. A missing range is
reported as `G7X_RANGE_MISSING`, never guessed. `nc_emit_host_test.c` pins the
emitted sequence (roughing cycle, then the finish cut of the same profile, then
the source after the G70).

The panel also does not offer G70 in the `G7X` submenu: the presets write a
roughing block whose inline finish already cuts the profile, so a program only
needs G70 when it wants a *second* finish pass (or a finish with its own feed).
Queued as an operator convenience, not a correctness item.

The rest of the panel pass that followed:

- **A cycle refusal says why, on the glass.** The status codes are Grbl's wire
  codes, so a module cannot invent one for "G73 is not implemented". Instead the
  g7x module hands the panel its reason (`g7x_take_refusal_text()`, read once so
  it can never explain a later line) and `nc_visual.c` shows it in place of the
  status name: `Line 4 error 20: G73 not implemented`,
  `Line 5 error 20: no G41/G42 in a cycle`,
  `Line 8 error 3: G70 range not run yet`. Every refusal path in the module has
  a short reason (the panel's message window is ~44 characters, so they are
  written to fit); the console keeps the longer wording.
- **`8 FINISH` in the `G7X` submenu** inserts `G70 P0 Q0` (preset `[44]`), so a
  finish cut can be written from the panel now that the module runs one. It is
  the submenu's last key because the block it belongs to is written first.
  The first version failed on the bench - "G70 preset failed" - because
  `/D/presets.txt` *replaced* the compiled set instead of extending it, so a
  card whose file predated `[44]` had no entry to insert. The file now **defines
  the entries it names and leaves the rest alone** (`nc_preset_apply_section()`),
  which is what the docs always claimed for an id the file does not mention:
  an edited `[41]` stays the operator's, and a new compiled section arrives
  without the card being regenerated. `--presettest` covers it (a file with only
  `[41]` still offers `[43]` and `[44]`).
- The `G7X` submenu's `6 G80` was checked: it inserts `G80` in place, on the
  line the cursor is on, replacing the helper's labelled line - which is where
  the inline-mode terminator goes.
- `nc_editor_draw_pane()`'s name buffer went from 80 to 128 bytes, so a full SD
  path plus the dirty mark cannot be truncated (the firmware build had been
  warning about it).
- The G76 preview stop is now a test (`test_g76_preview_error()` in
  `nc_emit_host_test.c`): the emitted stream stops with an error rather than
  drawing a partial thread. The screen shows it as `Preview: ... at line N`
  one frame after the notice is drawn, which is why a single-frame dump does not
  show it.
- **The DRO's running colour is green, and a fault takes it away** (bench: "now
  dro color for run. now it is yellow. does it have its own entry point? and if
  we found an error while it is running it will be yellow still? ie - lets make
  it green for run. and in any error case - make it dark grey and show this red
  alert label on right as it is now. but check before - we are limited in
  absolute number of colors here").
  - Yes, it has its own entry point: `NC_VISUAL_HEADER_RUN` →
    `nc_col_header_run()` in `nc_palette.c`. It now returns the palette's
    `green` instead of `yellow`.
  - **The colour budget is the point to check first, and it is spent**: the
    colour table holds exactly sixteen entries against
    `LVDS_RENDERER_MAX_PALETTE_COLORS` (16; the HSTX output is paletted and
    `lvds_palette.c` fails the build above it). So the green *reuses* `green`
    rather than adding one, and the note is now in `lvds_palette.h` where the
    next person will look: a new colour means retiring one, or giving the new
    thing an element instead (elements are not capped).
  - A fault takes the running colour away - `nc_visual_draw_header()` chooses
    grey whenever the notice is a fault - so a machine stopped by a problem never
    still says "running", and the red block carries the alarm. `--labeltest`
    checks the four states from the drawn frame: idle grey, running green with
    the strip still grey, fault grey with the block up, and grey after `#`.
- **RUN has one cursor** (bench: "it was marking next line after current g71,
  but then i pressed run single - it still seems to have marked original one with
  g71. only if i go back/forward it is ok"). There were two: the sender's line
  (`nc_run_line()`, which a run advances and which the line keys move) and the
  document's cursor (which the pane *marks* and `1 SINGLE` *sent*), and only the
  line keys kept them together. So a streamed run left the mark a line behind
  while SINGLE acted on the stale one - and pressing the line keys "fixed" it
  because that move syncs both. Fixed:
  - `nc_visual_run_step()` takes its line from `nc_visual_run_line()` (the
    sender's line, clamped into the program), so the key acts on what is marked;
  - the frame syncs the document's cursor to that line in RUN, so the mark
    follows the run;
  - `nc_run_send_document_line()` resolves the step through
    `nc_g7x_block_containing()`, so a marked line *inside* a G7x block sends the
    block - the documented "or the whole block it belongs to", which the old
    header-only check did not actually do. A contour row sent on its own would
    have executed as plain motion outside its cycle.
  `--runtest` covers both cases. This also answers the first half of the queued
  "RUN does not mark the line that is running": the mark is the sender's line,
  which during a stream is the line being executed. The second half stays open -
  one source line expands into many generated blocks, so "which block of this
  line is cutting" is still a design question.
- **RUN also marks the block, so the two halves of the question are answered
  without extracting anything** (bench: "now it seems it is able to mark next
  g71 line while running previous one. so it seems like it can do marking
  current one + its path block? for path block it asks for some pale yellow? so
  we will be more consistent and we will not need this extracted paths?"). The
  answer is yes, and it needs no separate path list: `nc_visual_draw()` asks
  `nc_g7x_block_containing()` - the same scan that decides what `1 SINGLE`
  *sends* - which block the marked line is in, publishes it through
  `nc_state_set_run_block()`, and the editor's pane colours the rows: bright
  `NC_VISUAL_SELECT` on the marked line, pale `NC_VISUAL_SELECT_BLOCK` on the
  rest of that block, the pane's own background on everything else. The pale
  colour is the palette's existing `yellow_light` (`#FFF582`): the 16-entry
  colour table is full, so the second mark reuses a colour rather than adding
  one, and `nc_palette.c` says so where the next reader will look.
  - Nothing is extracted, so nothing can drift: the pale rows and the block a
    step sends are the same question asked once.
  - A line outside every cycle marks only itself (`T2`, a bare `G0`, a tool
    change) and the pale colour clears with the mark - the pane paints from the
    snapshot and the screen re-publishes the block on every frame.
  - A first attempt passed `block_first`/`block_last` straight out of the call
    that fills them (`nc_state_set_run_block(nc_g7x_block_containing(...),
    block_first, block_last)`); C reads those arguments in an unspecified
    order, so GCC passed the un-updated values and the whole block stayed
    plain. It is now two statements, and `--blocktest` reads the drawn frame
    rather than the snapshot, which is what made the difference visible.
  - `--blocktest` samples the pane row by row (`host_pane_row_bg()`: the modal
    pixel colour of the row's text band) with the mark inside the fixture's
    `G71` block and again on `T2`: `#FFF700` marked, `#FFF784` block, `#C6C3C6`
    plain, as the 5-6-5 wire format rounds them. A check on the snapshot would
    have passed with the pane painting flat.
  - The other half of the old item - *which generated block of this line is
    cutting* - is still open and still a design question. The mark says which
    **source** line and which **source** block are running, not which of the
    emitted moves is in the planner.
  - That boundary is visible at the bench: a streamed sender reads ahead, so
    the bright line can be the *next* source line while the block before it is
    still cutting - and the pale rows go with the mark, so they leave the block
    as it steps out of it. Marking "still cutting" would need the planner's
    position, which is the open half above; the pale rows answer "this step
    sends this block", not "this motion is still moving".
- **The sender is paced now, so the mark is the machine's** (bench: "do not
  hijack controller flow. lets play now g7x sends one command and wait till
  buffer is empty. pause here is ok and proper. no worry for it"). The first
  answer to "which line is running" was architecture, not a mark: the sender
  handed the whole program to the reader at once, so the marked line was ahead
  of the tool by the controller's look-ahead depth. `nc_run_pace()` now hands
  over one unit per main-loop pass and waits for the machine in between.
  - **Nothing in the controller flow was touched.** No line numbers were added
    to the wire, no parser/interpolator hook was added, and `itp_get_rt_line_
    number()` is *not* read (it exists, and it was the tempting shortcut - the
    user said no). The pacing is entirely the panel's own send policy.
  - The machine answers when a unit is over, in its own terms: a cycle still
    **collecting** rows keeps taking them (`g7x_parser_collecting()`, a new
    query on the module - `g7x_parser_busy()` alone would either stall the
    contour or interleave a foreign line into it), and everything else waits for
    `planner_buffer_is_empty() && itp_is_empty()`.
  - The mark is the *unit*: the block's header for a cycle (with the pale block
    behind it), or the line itself outside a cycle. The line keys still move it
    (they take the cursor, and the mark returns to the machine at the next
    block); `1 SINGLE` acts on it, and while a run is in flight the reader is the
    console's between blocks, exactly as it is between one-shot jogs.
  - Two real bugs fell out of this, both found by `--pacetest`, not by reading:
    the send slot retires *lazily* (`available()`/`getc()` after the last
    character), so the pacer saw a drained slot as "still busy" and pushed every
    other pass; and the run's end cancelled a cycle that was legitimately
    **running** (it asked `g7x_parser_busy()`), which made the machine's own
    exec loop answer `STATUS_SYSTEM_GC_LOCK`. The finish now asks whether the
    module is still *collecting* - the only case where the panel has rows it
    never sent.
  - The g7x parser test had to follow: it drove a program with one
    `cnc_parse_cmd()` per line, which a paced sender no longer offers. It now
    gives the pacer a pass (`nc_parse_block()`, `nc_run_to_end()`) and drains the
    machine before a run starts. `test_g7x.py all` passes with it.
  - What still needs the machine: the finish left by a stop between every block
    (accepted, but it has to be judged on a real cut), Hold/Stop inside a paced
    program, and a desktop sender left connected during a panel run.
- **The same mark, in EDIT** (bench: "did you have an idea to use same highlight
  of current g7x main line its path in edit?"). Yes, and it is the same rule
  rather than a copy of the look: `nc_visual_draw()` publishes the block of the
  *line in play* on every code screen now - RUN's line comes from the sender,
  EDIT's from the document's cursor - and `nc_editor_draw_pane()` already paints
  whatever the snapshot carries. So EDIT shows the cursor bright with its cycle's
  path pale behind it, which is what the operator wants while changing a profile
  row: the block is visible around the edit.
  - The editor keeps the bright mark on its **own cursor**, not on the block's
    header as RUN does. The cursor is what the digits type into and what the
    legend names; an editor that moved its cursor mark to another line would be
    lying about where the typing goes. (If the header-as-head reading is ever
    wanted on EDIT, the cursor needs its own indicator first.)
  - `--blocktest` now walks both screens and both cases from the drawn frame.
    They publish from different places, which is exactly why one screen could be
    left flat by a fix to the other.
  - The word legend keeps the mark of the row it covers (`hint_bg` in
    `nc_editor_draw_pane()`): the row above the cursor is part of the pale path,
    so a legend on plain grey punched a hole in the path exactly while a word of
    that path was being edited.
- **Three things the bench found in that scheme** (2026-09-23, same day):
  1. *"it does not mark path region if g70 is after g71 with region described"* -
     `G70 P Q` names a profile that sits *above* it, so the line is in no block
     and had no path at all. The range finder moved into the owner's scanner
     (`nc_g7x_range_above()`, `nc_g7x_line_path()`) and the preview feeds from the
     same answer instead of its own walk - so the pane marks exactly the rows the
     finish cut will replay, and the two cannot disagree.
  2. *"in single run it marks next line so mark is on next g7x and its path if
     present - yet it is nice indeed"* - kept as is: a one-shot leaves the sender
     on the next line, and with (1) the next `G70` now shows its own path.
  3. *"i can move cursor in run mode which should be non - if i do run ucnc is
     owner of the cursor"* - the line keys are refused while a run is in flight
     (`RUN owns the line` in the strip's message area) and work again when the
     machine is idle. The one-shot path stopped setting the *program* run flag as
     part of the same fix, so a single step no longer leaves the DRO green and the
     corner saying `RUN` until the next reset - the panel's "it does not
     shout when it is not needed" rule, which that flag was breaking.
  - `--blocktest` now uses a fixture with a P/Q block *and* the `G70` below it,
    and walks EDIT and RUN through all three cases (inside the cycle, on the
    finish cut, outside both) plus the cursor lock and its release.
- **The stock dimensions had no way in** (bench, last check of the session: "it
  seems it lacks ability to insert stock dimensions?"). They did exist - compiled
  preset `10 SETUP`, four lines of `G970`/`G971`/`G972`/`G973`, and the action
  that inserts them - but **no menu entry offered the action**: the OPS submenu
  stopped at `5 DEL`. So the block could only be typed one line at a time with
  `3 G`, and the four-line setup (the way every sample file and the fixture
  start) could not be written from the panel at all.
  - OPS now has `6 SETUP` (`g_nc_submenu_ops`), which inserts the block at the
    cursor through the same preset path the file uses, so an edited `[10]` in
    `/D/presets.txt` is what the operator gets.
  - It is named `SETUP`, not `STOCK`: `STOCK` is the preview footer's display
    toggle, and one word meaning two things on two screens is how a panel gets
    misread.
  - Two more actions of the same shape are still orphans and are *not* wired:
    `NC_FOOTER_ACTION_PRESET_LINE` (`21`) and `PRESET_ARC` (`22`). They are
    reachable today by typing the G-code in `3 G` (the vocab has G1/G2
    templates), so they are redundant rather than missing. *Resolved below: the
    bench answered "we do not need any extras" and they went, presets and all.*
  - `--editortest` presses `1 OPS` `6 SETUP` and reads the file back: the four
    lines have to be in the program, in order.
  - The bench then read `docs/nc-preset-file.md` and asked the right question:
    "*in presets it is set as `[10]` but i need to press 16?*" - the section name
    and the key path had drifted apart: the doc called the IDs "stable menu
    keys", which was true for `41` (`4 G7X` then `1`) but not for `44 FINISH`
    (the G7X menu's key `8`), `80` (the OPS menu's `3`) or this entry `10` (the
    OPS menu's `6`). A first pass answered with a table of "id beside keys",
    which is a doc describing a fault; the fix below is the fault going away.
  - Then the bench said to do it - "*yes it was the task so i can rebuild/modify
    as i want. with only one [level] it is 0 to quit, and up to three levels; now
    it is two levels only*" - so **the IDs are the key path now**, for every
    entry, and the encoding is written down: one digit per level, up to three,
    with `0` as the pad's quit key so a path that ends at the footer is `<key>0`.
    - Renumbered: setup `10`→`16`, finish `44`→`48`, end `80`→`13`, and the two
      orphans line `21`→`17`, arc `22`→`18` (they needed a menu home first, so
      OPS gained `7 LINE` and `8 ARC` - with the `C`/`R` and `R I K` words the
      plain `G1`/`G2` templates lack).
    - `10`/`44`/`80` are still **read as the entries they always named**
      (`nc_preset_id_current()`), because a card's edited `[10]` is the
      operator's text and a renumbering must not throw it away; the file is not
      rewritten behind their back.
    - The ids are written down once, in `nc_presets.h` (`NC_PRESET_ID_*`). That
      was not cosmetic: the path builder inserted its end mark with a literal
      `80`, so renaming the section left the builder unable to write a block -
      `--buildertest` caught it, and it is exactly the class of bug this
      renumbering could have spread quietly.
  - **This also settles the earlier "some IDs are menu keys and some are not"
      question**: there is no such split any more, and `g_nc_submenu_ops` is the
      list an id has to match.
- **The OPS pad kept only what is nowhere else** (bench: "except save - we do not
  need any extras. `*` is delete, open [is `0`] so 5 is not needed, end could be
  skipped etc"). OPS is now `1 INS` (a blank line - the one thing no other key
  makes), `4 SAVE` and `6 SETUP`.
  - What went, and why: `2 FILES` (`0` is the file key on every screen that works
    on a document), `5 DEL` (`*` is the delete), `3 END` (the G7X menu's `6 G80`
    is the one key for the block terminator), and the `7 LINE`/`8 ARC` entries
    added earlier the same day.
  - `LINE`/`ARC` went further than the menu: their **presets are gone too**
    (`NC_PRESET_LINE`/`ARC`, the `NC_FOOTER_ACTION_PRESET_LINE`/`ARC` actions and
    the ids `17`/`18`). Their text was the `3 G` field's own templates for `G1`
    and `G2`, word for word (`G1 X0 Z0 C0 R0`, `G2 X0 Z0 R0 I0 K0 F0`) - two
    owners of one line, which is the duplication rule 4 of `AGENTS.md` bans, and
    the reason they had no menu key to begin with.
  - The end mark's id followed it to its one key: `13` (which never shipped) is
    gone, the end mark is `46` (`4 G7X` then `6 G80`), and the shipped `80` maps
    to it.
  - `--editortest` now checks the trim from the operator's side: `1 OPS` `4` still
    saves, and `1 OPS` `2` no longer opens the file list.
- **The blank line is a template now, not an action** (bench: "blank line could
  also be made using templates. same as m or s command where appropriate"). So
  the last hardcoded *text* insert in the editor is gone: `1 INS` is section
  `[11]`, one `line=` whose compiled default is empty, and the card decides what
  that key writes - a blank line, a separator comment (`line=(---)`), or a
  command the operator keeps reaching for. The TOOLS footer's `7 INS` stays a
  plain blank row: the tool table is the panel's own file, not the program.
  - Ids: `11` is `OPS 1` then key `1` - the path, so nothing had to move.
  - `--presettest` checks both halves: no file means the compiled blank line, and
    a card that says `[11] name=SEP line=(---)` gets that instead.
- **The save key is gone: the panel saves** (bench: "save is basically not needed
  too. it is fine with auto save. and it should be"). `NC_FOOTER_ACTION_SAVE`,
  its handler and both SAVE entries (OPS `4`, the TOOLS footer's `9`) went with
  it. The write itself was already there for the paths that reuse the buffer
  (screen change, opening a file, creating one); what was missing is the case the
  operator cares about - sitting in an edit - so `nc_visual_idle_tasks()` now
  writes a dirty document once the screen has been quiet for the same 400 ms it
  uses for the remembered state.
  - One write per quiet period, not per keystroke (`g_nc_visual_idle_flushed`,
    cleared by any key), so a card that cannot be written to is not hammered on
    every main-loop pass.
  - The dirty `*` in the file-name row is the confirmation: it appears while
    typing and clears when the write lands. That is why dropping the key is not
    dropping the feedback.
  - The **harness had no idle task at all**: `host_pump()` now calls
    `nc_visual_idle_tasks()`, the way the firmware's `nc` module does in its main
    loop. Before this, the host could not see an auto-save happen - which is
    exactly what the failing editortest said, and the reason the check was
  rewritten to save the way the operator does (stop touching it, let the clock
  pass) instead of pressing a key that no longer exists.
- **The last hardcoded program text went too** (bench: "so now hardcoded things in
  3x3 all goes from presets file?" - and the honest answer was "all but one
  family"). The TOOL pad's `3 M6`, `4 M3 S1000`, `5 M5`, `6 M4 S1000` were the
  four literal strings left in the editor; they are sections now (`[23]`..`[26]`,
  the keys being `2 TOOL` then `3`/`4`/`5`/`6`).
  - So every entry in every pad that writes text into the program is the card's.
    The spindle speed is the reason it matters: the panel's own MANUAL spindle
    keys send the speed the machine remembers, so a hardcoded `S1000` in the
    *program* was the odd one out.
  - What stays the panel's, and is not "hardcoded text": entries that edit a word
    on the line (G7X `4 Q`, `5 N`), the contour builder (`7 PATH`), the `T` field
    (`1 SELECT`), and the actions (file list, delete, opening the tool table).
  - `--presettest` checks a card can set the speed (`[24] line=M3 S2000`), and
    `--editortest` presses `2 TOOL` `3` and reads `M6` back out of the program.
- **The generator got its last rule: a row that starts with a space continues the
  line above** (bench: "on N/Q or other things to be added inline - just use trick
  by not have a new line before values. so controller will know it all... this
  needs one more sweep. to make it as minimal as possible with all extra
  scaffolding removed"). `nc_preset_insert_lines()` appends such a row to the line
  the entry is writing under (or to the row it wrote itself) instead of starting a
  new one, and leaves the word it wrote picked so the number is typed straight
  into it. The whole generator is now three sentences: an id is the key path, a
  name is what the key reads as, and rows are what the key writes - with a leading
  space meaning "not a new line".
  - That is what makes an entry like `G1 X0 Z0` + ` C0` + ` R0` possible from the
    card, which is the shape the bench described for the corner words (`RND`/
    `CHMF` as their own rows of the entry).
  - **Correction to the same message, for the record**: `N` cannot become its own
    program line. The module refuses a range whose `N(P)`/`N(Q)` block is not a
    movement ("N(P) must be a move"), so the number has to stay on the row it
    numbers - which is what the `4 Q`/`5 N` keys already do (they add the word to
    the cursor's line, no line break). Those two stay the panel's word entries;
    their `Q0`/`N0` is a value to type over, not a line the card owns.
  - **What is still scaffolding, and the next step**: the pads' entries and their
    labels are still listed twice - once as a menu row (`nc_menu.c`) and once as a
    section (`nc_presets.c`), with an action and a switch (in `nc_editor_action`)
    to carry the id between them. The minimal form is the one the bench is
    describing: **derive each pad slot from its id** (`<footer key><pad key>`,
    which the ids already are), let the section's `name=` be the label, and keep
    the tables for the entries that are *not* text (Q, N, PATH, SELECT, EDIT, the
    actions). Then a card could add `[12]` and have a key of its own appear, and
    the label would stop being a second copy. That is a pad-plumbing change, not a
    preset one, and it is the natural next sweep.
  - **Done in the sweep after that, and the code came out smaller** (bench: "do it
    fully and slowly. and i expect to have less code on output not some scaffolding
    done"):
    - `nc_editor_modal_build()` fills a pad from the ids: a key that writes
      something is the section at `<footer key><pad key>`, its `name=` is the
      label, and a slot the table does not claim is offered as soon as a section
      has that id. So `nc_menu.c` lost the eleven rows that duplicated the
      sections, `nc_menu.h` lost the eleven actions, `nc_editor.c` lost their
      handlers and `nc_presets.c` lost the enum and the switch that carried the id
      between them (`nc_insert_preset()`, `nc_editor_insert_preset()`). A card can
      now add `[12] name=ROUGH line=(rough 1)` and OPS `2` is that entry.
    - `3 G` became **`3 WORD`**: the pad of single lines, whose `1 G` is the field
      that writes one line by name or number, and whose other slots are the card's
      - `[32] CHMF` and `[33] RND` ship as the two corner words, each a row that
      starts with a space, so they add to the line the cursor is on. That is the
      bench's design: "chamfer and round could be just separate inserts in 3...
      as extra lines to be inserted". The `C`/`R` words in the vocabulary's own
      row template stay as the dialect's default (`G1 X0 Z0 C0 R0`); these two
      entries are the card's way to add either word to a row already written.
- **The line under the DRO is gone** (bench: "we have one black line under dro,
  now it is obsolete"). It was drawn along the bottom of the header band back
  when the band and the code pane were both plain grey and needed separating;
  the band's own colour is the boundary now - green while a run is going - so it
  was a line that said nothing. Only that rule went: the three columns inside
  the band keep their separators, because those divide one reading from the
  next.
- **From the bench, reading a `G70` run** (2026-09-23, end of session):
  1. *"for this seems the wrong radius?"* - it was: the generator *fitted* a
     corner that did not fit and said nothing, so `R35` on a 20 mm move with a
     15 mm step came out as `R3.375`. The bench's answer was *"it should reject
     it loudly"*, and that is what it does now: `G7X_CORNER_TOO_LARGE` ("contour
     rejected: corner does not fit") for a corner the moves cannot hold, a fit
     check before expansion (`g7x_expand_corners()` asks `g7x_corner_fit()`),
     and no silent clamping anywhere.
     - The *limit* had to be fixed with it: it was 45% of the shorter move (a
       local way to keep two corners from overlapping), which is 2.2x stricter
       than the geometry and refused legitimate rounds - the NC fixtures' own
       `R2`/`R5` corners stopped expanding, which is how the filetest caught it.
       It is the geometric limit now (the tangent has to land *on* the move) with
       a first-fit budget: the walk is left to right and each corner moves the
       element to its tangent point, so the next corner measures what is left.
       `../g7x/README.md` has the whole rule.
  2. *"one more 0 for single run: it still pushes cursor to the next command and
     marks it... it should push cursor only after command is finished. in single
     run"* - `nc_run_program_finish()` cleared the running mark the moment the
     block was *handed over*, so a `1 SINGLE` on a block marked the next line
     while the block was still cutting. It no longer clears it: the mark stays on
     the unit that is running. The first cut of that fix then moved the mark on
     when the machine **finished** the unit, which is its own fault - see (4).
  3. *"after error it still goes to next line"* - the sender's position had
     already stepped past the line it handed over (that is how the pacer walks a
     program), so a refused line left the run standing on the line *after* the
     one that failed: the mark, the message's line number and the next
     `1 SINGLE` all pointed past the problem. `nc_run_failed()` now puts the
     position back to `nc_run_error_line` - the line the module refused - and
     drops the running mark, so the pane marks what has to be fixed. The check
     is in `--blocktest`: its fixture carries a bad row *with a line after it*,
     because a bad last line would hide the fault behind the clamp.
  4. *"now it runs but it marks also next g71. which it should not mark"*
     (2026-09-23, the turn after) - the fault (2) left behind. Once the mark had
     a run of its own, the pacer dropped it when the last unit of a run was over
     and the pane fell back to the sender's line, which by then pointed one line
     past the block. In the bench's file that line is the *next* `G71` - a cycle
     that was never handed over, with no path for the pale mark to draw, so the
     screen showed a bright line and nothing else and the code that had just cut
     was no longer marked at all.
     - The mark is now the machine's for as long as the operator leaves it alone:
       a finished unit keeps the mark (the block's head bright, its path pale)
       and it moves only when the machine starts another unit - or when the
       operator takes the cursor: a line key (`nc_run_set_line()`), a new step
       or run (`nc_run_arm()`), `# RELOAD` (`nc_run_reset()`) or a refused line
       (`nc_run_failed()`, which puts the mark back on the line that failed).
       (The *head* half of that is superseded by (5), which moved the bright
       line to the line in play; "it stays put" is what stands.)
       Deliberately *not* tied to the sender's position: the sender is the
       panel's walk through the program and it does move on (a `1 SINGLE` on a
       block leaves it on the line after the block), which is exactly why the
       pane cannot use it for the mark. A `STOP` leaves the mark where the tool
       stopped, on the unit it was given - the same reading, and the same
       question as the stop itself, which is a bench item.
     - `--blocktest` now reads the frame *after* a block step with the machine
       idle and requires the mark to still be on the block - and the sender to
       have moved past it, so the two answers are checked apart. The step on a
       line outside every block keeps its own line marked for the same reason.
     - Open, and for the bench: with the mark held on the block that ran, a
       second `1 SINGLE` re-runs it (the key acts on what is marked, which is the
       rule this whole arrangement exists to keep), and a line key steps on from
       the *marked* line, so walking past a block means walking through it. If
       the bench wants the keys to step on from the sender's position instead,
       it is one line in `nc_editor_move_line()` - but it has to be decided with
       the machine in hand, not guessed here.
  5. *"nope it still marks next row with g71, not the one starting with N50"*
     (2026-09-23, the turn after (4)) - the second half of the same question, and
     it said the rule in (4) was still wrong about **which** line is the mark.
     Reproduced from the frames: with the cursor on the `N50` row, `1 SINGLE`
     left the pane bright on the `G71` row *above* it. Two causes, both fixed:
     - The mark was the **block's head**, so a step taken from a contour row
       moved the operator's mark up to the header. It is the **line in play**
       now - the line the step was taken from, with the cycle's rows pale around
       it - and the whole block is still what goes out to the machine. A run
       that walks on its own still marks the unit it is on, which is what makes
       the pane follow a `3 FULL`.
     - `nc_g7x_block_containing()` answered with **two different blocks** for one
       cycle when the `P/Q` range sits on the *first* line of a two-line header
       (the bench's file): from the first header it found the pair (`5..11`),
       from the second header or any contour row it found `6..12` - the
       continuation scanned as a cycle of its own. So a step from `N50` handed
       the machine the profile *without* the line that names its range, and the
       pane's pale path started at the wrong line. The scan now resolves a
       header to the *pair* it heads (`nc_g7x_header_head()`) before it closes
       the block, so every row of a cycle answers with the block the pair
       starts. `test_g7x.py nc` pins both shapes in `test_g7x_block_scan()`, and
       the bench's own file was replayed through the harness: from the `N50` row
       the mark stays on `N50`, rows 6-12 are the block, and the naked `G71`
       below is untouched.
     - Still open from the same reading: `2 FROM` an *inner* row of a cycle
       walks from that row (the runtest pins it), so the machine is handed a
       profile with no header. The block scan can name the head for it, but
       "from here" is the operator's word and the change has to be made with the
       machine in hand.
  6. *"strange. now it colorize both g71 and path if any g71 is select. with pq
     or with g80"* (2026-09-23, the turn after (5)) - the mark now colours the
     whole cycle, but the two ways of writing a cycle stopped in different
     places, which is what the "with pq or with g80" in that sentence names.
     - `nc_g7x_block_end()` ended a range-terminated cycle at `N(Q)` and left a
       `G80` written after it outside the block, while a cycle with no range at
       all ended *at* its `G80`. Same contour, two pale blocks - and a step from
       inside the range cycle handed the machine the profile with the end mark
       as a separate unit, where the `G80` cycle sent one piece.
     - The end mark is part of the block now: after `N(Q)` the next content line
       is swallowed when it is a contour end (`G80`), so both flavours answer
       with the header pair, the contour and the end mark. `test_g7x.py nc` pins
       both flavours in `test_g7x_block_scan()`, and the bench's file was
       replayed: with the cursor anywhere in lines 6..13 the pane is bright on
       that line and pale on the rest of the cycle, line 14 (the naked header)
       untouched.
     - Open, and for the bench: the pale mark now includes the **header rows**,
       not just the contour. That is what "marking current one + its path block"
       asked for and it is the same set the step hands the machine - but if the
       operator wants the pale to mean *the profile only*, the header rows would
       have to be left out of the pane's path while staying in the block that is
       sent. Decide with the machine in hand.
- **A fault is a label, and the controller's state moved into the DRO** (bench
  request). Two drawings changed, no state did:
  - the message area draws a fault as **white letters on red** instead of red
    text, and the block is a **panel**: it starts in the strip's message area and
    continues down over the DRO beside the machine's figures, wrapping its text
    over up to three rows (`NC_FAULT_LINES`). Bench refinement while verifying:
    "it wants naturally to occupy all field below itself on dro strip too except
    this bottom right line reserved for ucnc status, and make it a little
    narrower so it does not push itself over F and S values" - so the block's
    geometry is `NC_FAULT_X/W/H` in `nc_layout.h`: left of the F/S column, bottom
    above the state corner, otherwise to the panel's right edge. (My first pass
    also kept a right margin for the FPS and the `>` hint; the bench answered
    "fps is just for debug, so move it all to the right" - the block runs to the
    edge now, and the FPS moved below the DRO, right side, where a debug reading
    belongs.) `--labeltest` checks the block's edges - red over the message area,
    none over F/S, none over the state corner - not just the colours.
  Second round from the bench: "it is still not the most right and ucnc status
  can get at least 10 px lower, and check when it clears" - `NC_FAULT_RIGHT_PAD`
  is 0 (the block ends at the panel edge), the DRO's state moved from `hy + 37`
  to `hy + 47` and then to `hy + 50` ("make ucnc status even lower, i am
  guessing around 3 pixels" - the label's bottom edge is now one pixel above the
  band's separator line, which is the floor), and the clearing was checked
  rather than assumed: the message
  stood until *something answered it*, and no panel key did - the only clear was
  a parser reset (boot, a `$`-command, `M30`), which the operator cannot reach
  from the panel. Now `# RELOAD` (RUN) and `* BACK` (file list) clear it, and a
  fresh run clears the previous message before it starts. `--labeltest` presses
  `#` after a fault and checks the red is gone from the whole panel area.
    Warnings and ordinary messages keep the strip's own colours and stay a line
    of text.
  - the DRO's **bottom-right corner** carries the controller's state
    (`nc_visual_state_label()`: `uCNC IDLE`/`RUN`/`HOLD`/`JOG`/`DOOR`/`ALARM`/
    `KILLED`/`LIMITS`/`POS LOST`/`SETTINGS`/`ERROR`). It is on every screen, so
    the answer to "is the machine running, held, or in a fault" needs no screen
    change; a fault wears the same red label there. *`for start`*: more of the
    machine's own status can move into that corner as the panel grows.
  `--labeltest` reads the frame back through the host pixels and checks both
  labels in the colours as *drawn* - the palette is 5-6-5 on the wire, so
  `#E60000` arrives as (231,0,0) and `#F0F0F0` as (247,243,247); checking the
  hex directly is what the first version of that test got wrong.
- **Text files open, save and preview as text** (bench report: the file list
  carried `/D/PRESETS.TXT` but opening it answered `open failed unsupported NC
  file`). `nc_load_file()` now takes anything `nc_path_text()` accepts - the
  programs *and* the `.txt` beside them - and `nc_editor_save_current()` uses the
  same rule, so the operator can open the preset file from the list, edit it and
  have it saved. RUN is unaffected: it asks `nc_path_supported()` and still
  answers `Not a program file`. The preview's "Text file - no preview" caption is
  replaced by a read-only text preview of the file (`nc_preview_text_file()`:
  line numbers plus the rows), because a list that shows the file and a pane that
  refuses to show anything of it is the worst of both. `--filetest` covers the
  open, the save and the drawing.
- **The editor's legend names every word the panel writes.** `P`, `Q` and `N` in
  a G71/G72 header fell through to the legend's fallback - `P`/`Q` said "NC
  word" and `N` said "Line number", which is wrong there (N numbers a profile
  *block*, and on the header line the module ignores it). Now `P`/`Q` read
  "Profile start block"/"Profile end block" (G70/G71/G72), `N` reads "Block
  number" everywhere, and the words the panel's own templates carry that were
  also unnamed got theirs: `G4 P` -> "Dwell time", `G33 K` -> "Thread pitch",
  `G2/G3 I/K` -> "Arc centre X/Z". `--vocabtest` (in `test_nc_ui.py`) walks
  every word of every `nc_vocab_gcode_template()` and fails if one of them falls
  back to "NC word", so a new template word cannot arrive without a legend.
- **The `G7X` submenu's `4 Q` and `5 N` keys write the word** (bench: "press 4
  then 4 again to put Q blocks - it did nothing"; they were stubs that only set
  a status line). `nc_editor_insert_range_word()`:
  - `5 N` puts `N0 ` at the *start* of the line the cursor is on and selects it -
    a block number comes before the motion word (`N100 G1 X20 Z0`), and the
    typed digits replace the zero;
  - `4 Q` puts ` Q0` after the picked word (or at the end of the line);
  - if the line already carries that word - a header written from the presets has
    `P0 Q0` - the key *picks* it instead of writing a second one, which the parser
    would refuse as a repeated word.
  That is how a P/Q range is written on the panel: `4 5` on each row, then the
  number; the header's `P`/`Q` come from the OD/ID/FACE preset and are edited in
  place. `--editortest` covers both keys.

  *Still awkward:* while a value is picked the footer digits edit it, so calling
  another submenu entry means dropping the pick first (Up from line 1 reaches the
  file-name row, which does). That is the queued "footer digits are shadowed by
  value editing" item, and it now costs a keystroke in a real workflow.
- Found while reproducing the bench's `P1 Q1` report: the machine's armed-but-
  never-opened range is only cleaned up by the panel (`nc_run_stream_clear()` →
  `g7x_parser_cancel()`, reported as "incomplete G7x cycle"). A sender that does
  not do that leaves `g7x_parser_busy()` set, so the *next* cycle header in the
  same run is refused as "conflicting modal commands" (21) - the parser-test
  probe showed it. The module should end an armed range that no `N(P)` row ever
  opened when the program moves past it.
- The header templates no longer end in `N0`: `N` numbers a profile *row* and
  the cycle reads its range from `P` and `Q`, so the word on the header line was
  one this dialect ignores - the bench asked why it was there. Removed from
  `nc_vocab_gcode_template()` (71/72), the parameter legend ("U R X Z F P Q")
  and the compiled `[41]`/`[42]`/`[43]` presets. **A card whose `presets.txt`
  already defines those sections keeps its own text** - the file owns the
  entries it names - so the old `N0` stays until that section is edited on the
  panel or the file is deleted and rewritten from the compiled defaults.

Two other things came with that session. RUN's `B`/`C` line keys now mark the
screen for repaint: they moved the run line and returned without the flag, so
the highlight stayed on the old line until the next footer key set it - the
`nc_editor_key_edit()` branch for a screen that cannot be edited. `--dirtytest`
covers it and the periodic frame an armed run relies on (see `TESTING.md`).

And the live cursor is now held inside the area the live path redraws. RUN while
streaming does not repaint the pane - it repaints one band around the stock
(`nc_live_band()`) - while the cursor was guarded against the whole pane, so an
axis off the stock (a wild touch-off) left the cursor drawn in the strip below
the band, where nothing takes it back: the "cursor drawn but not cleared"
class again. The clear and the guard are now the same rectangle, so the cursor
is only drawn where the frame erases it; during a live frame a tool far off the
stock shows no cursor and the DRO carries the position. The remembered
marker-rectangle state went with it - it had no reader left once the band clear
became the erase. Reproduced and checked headlessly (a program parking the tool
past the stock, guard against the band vs against the pane), and left as a bench
item in `TESTING.md`: the live path needs PSRAM, so the machine is the judge.

And the TOOLS tool tip block has a colour of its own: `tool_tip` = `#EAFA41` in
`lvds_palette.c`, reached as `NC_VISUAL_TOOL_TIP` through `nc_palette.c`. It
colours the block's text - `Tool tip`, the `Line n: ...` line, the parameter
labels and values and the `X0`/`Z0` captions - while the field being edited
keeps the editor's word colours and the glyph keeps the tool's.

The DRO now says the machine is running by wearing the panel's yellow: the
header band is filled with `NC_VISUAL_HEADER_RUN` (`yellow`, the colour the
selections and values already use) while the machine is in a run - held counts,
the run is not over - and with its own grey otherwise. The tab strip above keeps
its grey. One definition of "running" (`nc_visual_running()`), the same one the
DRO's own state word uses, so the band and the word in its corner cannot
disagree. Nothing moved but the fill colour; the figures stay dark and read on
both.

The criterion behind it, as the bench put it: **it does not shout when it is not
needed** (2026-09-22). The loud colour is spent only on the state that is
happening - an idle machine keeps the quiet grey - which is why "does the RUN
screen have a program" is not the question and "is the machine in a run" is.
Anything else the panel wants to make loud should answer the same way.

Then the X pair of both 3x3 pads was flipped (`8`/`2` are X-/X+): the preview
draws X downward from the top of the stock (`nc_preview_map_x()`), so the tool
sits at the bottom of the stock and the key pointing up was moving it the other
way on the glass. The builder's diagonals follow (`7`/`9` are Z-X-/Z+X-, `1`/`3`
are Z-X+/Z+X+), and MANUAL's pad table and jog signs were flipped with it.
X is a diameter in the program in both cases - only the key that moves it
changed.

The builder's delete is `*`, the panel's own delete key: it takes the row of the
point being entered away, or the last one written. `A` was taken back out of the
builder - it is the screen's mode key, so it leaves the builder and the written
lines stay, and only `0` takes a session back. While a word is open `0` is a
digit (a typed `30` or `0` has to be enterable) and a draft that is only a signed
zero (`-0`) no longer reaches the program.

`5` no longer closes with a `G0`: the contour is the part's geometry, the cycle
owns its approach and retract, and a rapid row inside the profile breaks the
generator's monotonic X/Z rule (`g7x_stream_prepare()`), so a built block with
that row did not expand at all - the bench now emits every built profile and
proves the block still runs as a cycle.

Where the `G0` belongs was settled with it: a new block gets the **cycle start
`G0 X<OD+clearance> Z<clearance>` in front of its header** - the operator's own
spelling of the start and return point, which the machine uses and the generator
ignores (the bench expands the same program with and without it and gets the
same 77 lines), while the **profile starts at the stock corner** (the face at
the stock OD), not at the clearance corner. So the pad's first press copies the
face and the pattern is `8 4 2 4 2` then `5`, with the clearance in the `G0`
in front of the cycle as `G0 X52 Z2` for a 50 mm stock.

Two generator items from the bench were fixed in the generator (`../g7x/TODO.md`):
the roughing now stops **on** the finish allowance instead of a whole step short
of it, so the automatic finish only takes what the allowance reserved (it used
to leave 1.75 mm of radius extra on an X50-to-X30 profile with
`G71 U2 R1 X0.5 Z0.5`, and 1.5 mm of Z with `W2`/`Z0.5`); and a block now ends
back at the clearance point - the corner a `G0` before the cycle establishes, and
where Fanuc returns to - instead of stopping at the last cut's Z.

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
- MANUAL takes the same header DRO as every other screen (it used to be the one
  blank header), and the split is now: **the DRO is the current state** - work
  position, the machine figures, F/S, once, at the top - while **the pane is the
  wanted state**, the things the operator sets up: one line per axis with its
  two limits, then STEP and FEED. The position is not copied into the pane, and
  the row's label says which of the two it carries - `X LIMIT`/`Z LIMIT`, not the
  bare axis letter - because the numbers are the limits, not a position. The
  label is written in the pane's own font, the one the stops and STEP/FEED use,
  not the header DRO's larger one: the row is one table, and a label set larger
  than its own numbers reads as a different field. A side
  whose stop was never typed shows the **axis limit
  the setup states**, dimmer than a typed one - there is no `--` and no "no stop"
  state; the same limit is what a move is stopped by until the operator narrows
  it. The distance to go is **not** shown beside a stop - the DRO has the
  position and the stop's job is to stop the move - and every number in a column
  is right-aligned, so the stops and the STEP/FEED values share a right edge
  (name in the axis column, number in the stop column). The stops sit left of
  centre and are centred in their row (`NC_MANUAL_COL_STOP`/`_PLUS`,
  `NC_MANUAL_TEXT_DY`); there is no `1- 3+` caption, because the pad's own keys
  already carry `FD-`/`FD+`. A value being typed
  appears **in the cell it belongs to** - the stop cell for a limit, and the
  touch-off's own column between the label and the first stop for a value `D`
  types (`NC_MANUAL_COL_TOUCH`) - in the editor's word colours, so there is no
  second field line to read.
  The line the keys act on is marked **across the values it
  owns, not the whole pane** (it stops short of the pad). Jog pad bottom right:
  `8`/`2` X-/X+, `4`/`6` Z-/Z+, `7`/`9` spindle CCW/CW, `5` spindle stop, `1`/`3`
  feed override, pressed key lit for 250 ms. Footer: `B`/`C` axis, `D` touch,
  `0` zero, `#` step/continuous, `*` stop.
- TOOLS: tool table with the tool tip details near the bottom of the pane.

Leftovers from this session, in the order they came up:

- [x] **MANUAL is a machine screen, not a text editor** (decided 2026-09-22,
  from the bench's own worry that the stops and the touch field were turning
  into one). A value here is only ever typed *inside the function that owns it*:
  `0` zeroes, `D` touches off, `*` stops, `#` swaps step for feed, and a value
  field is opened by the key of the function it belongs to and taken by that key
  again. **`D` never walks from one field to the next** - the field-by-field
  walk is the EDIT screen's word entry (`docs/nc-editor-tnc415.md`), and bringing
  it here would make a jog panel into a text editor. The stop entry is the one
  judgement call: `*` opens the *minus* stop and `*` again takes it and opens the
  *plus* one, which is a two-step cycle inside the stop function itself, not a
  walk across the screen's values - and if the bench says even that is one step
  too far, the minus and the plus become two keys instead (see the keypad item
  below: on this 16-key pad they would have to be taken from another function).

- [ ] RUN does not mark the line that is *running*. The pane highlights
  `nc_run_line()` - the next line handed to the reader - and the reader, parser
  and planner pull ahead as fast as they can, so for a cycle the mark is at the
  end of the program before the first cut. The machine does keep a per-block
  identity: the block's `line` (filled from the parser's `words->n`,
  `parser.c:1463`), read back by `itp_get_rt_line_number()` and published as
  `|Ln:` in the status report. But `words->n` is the line's **N word** today
  (`parser.c:2772`), and the panel's programs carry none - so the machine
  reports 0 while it cuts. The firmware's own switch for this exists:
  `GCODE_COUNT_TEXT_LINES` in `cnc_config.h` ("ignore the value in the N
  parameter and count real text lines"). Measured: with it on, the parser
  path's `P/Q` and two-line Fanuc ranges fail - 9 failures in
  `test_g7x.py all` ("P/Q retention: visited=0", "two-line range cleanup", ...)
  - because `g7x_parser_block_number()` matches the range on `words->n` and the
  parse/exec events carry no text. So it would be one of: give the parse/exec
  events the raw line so g7x reads its `N` from the text, or have the panel
  prefix `N<line>` only where the program carries no `N` and no range.
  **The operator ruled the numbering out** (2026-09-21): g7x expands one source
  row into many motion blocks, so a per-block number cannot say which row of the
  program is being cut - and during a cycle the number would sit on the cycle's
  header row while the whole block runs. Marking the *running* line is therefore
  still an open design question, and the mark (cursor or a marker beside it) has
  not been decided either.
- [x] RUN: `3 FULL` and `2 FROM` did nothing while `1 SINGLE` worked (bench
  report 2026-09-21). The arm path only called `nc_run_arm()`, which sets the
  run state and prints "Run from line N" but never hands the reader to the run,
  so not one character reached the machine and the run stayed armed forever.
  `1 SINGLE` worked because `nc_run_send_document_line()` starts the stream
  (`grbl_stream_readonly()`) for the line or the block. The arm path now starts
  the same stream with no end line - the whole program from that line - and
  carries the single step's lock guard. Software-verified by
  `test_nc_ui.py --runtest`: FULL sends the fixture's eight sendable rows in
  order (the `G970`-`G973` setup rows skipped) and the run is done at the end;
  FROM sends the six rows from the cursor. Putting `nc_run_arm()` back fails it
  with an empty send. The machine pass - the axes actually moving through the
  whole program - is still a bench item.
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
- [x] **MANUAL stop: two limits per axis, and the numbers come from the pad**
  (decided 2026-09-21, built 2026-09-22). Each axis has a minus and a plus stop
  in machine millimetres, replacing the single wall whose side was learned from
  the first move off it (`g_nc_manual_stop_side[]` and `nc_manual_toward_stop()`
  are gone):
  1. `*` (`STOP`) opens the **minus** stop as a value field. The digits type into
     it, `B` is the sign and `C` the point - the keypad has no minus key, so it
     uses the editor's own field keys - and the first character typed replaces
     what the field held, because `*` is the accept key here;
  2. `*` (or `#`) takes that value and opens the **plus** stop field;
  3. `*` again takes that and closes. `D` does not delete anything: it puts the
     **axis limit the setup states** in the field, and `A` leaves the field
     alone without changing the stop.

  An **empty field takes that same limit**, so three presses leave the machine's
  own travel as the stops and the operator only ever narrows them. The limit is
  the machine frame the kinematics clamp to, `[0, $130]` per axis - the figure
  the feed cap already reads - because that is the setup the MANUAL screen can
  see. The program's `G970` envelope (`X`/`U`, `Z`/`W`; a min/max pair per axis,
  and what "from setup" first suggested) lives in a document the MANUAL screen
  does not hold; if the job's envelope should win instead, the screen has to be
  handed those four figures.

  A step into a stop is shortened to land on it, a step from on it into the same
  side is refused, and the other side keeps its own stop - so standing on one is
  never a lock, which is what the old side learning was for. A held feed covers
  the room to the stop it is headed for, and is still bounded by the travel when
  that side has none. The pane shows both stops with the room left to each
  (`-STOP` and `+STOP` columns; the machine figure moved right to make room).

  Software-verified by `test_nc_ui.py --stoptest` (typed stops, `D` from the
  setup, both clamps, the feed covering the room) and the rewritten
  `--feedtest`; the machine pass is a bench item.

  Building it exposed two things about the screen. The axis keys did not work at
  all: `B`/`C` are the footer's `AXIS-`/`AXIS+`, but the keypad has no character
  of its own for them, so they arrive as the field-step keys - and the editor's
  key handling, which the screen offers before the footer lookup, took them as a
  code-line move. MANUAL now answers them itself (and the shell's arrows with
  them, since there is nothing else on this screen for those to move). And the
  stops are read as numbers, so they are drawn in the same font as the machine
  figures, one normal font height apart - two rows fit the row height, and the
  columns are 80 px apart for 64 px of text.
- [ ] 3x3 helper: show the preset `name=` on the key instead of the fixed slot
  label, so a renamed entry reads as renamed on the panel.
- [ ] The keypad is where the panel's keys run out. The machine's 4x4 has no
  minus and no point, so the sign and the decimal point ride on `B`/`C` - which
  are also MANUAL's axis keys and the editor's field-step keys - and the stop
  entry, the touch field and the value fields all borrow the digits. A keyboard
  with a sign column (and a few more keys) would let the panel give each of those
  its own key instead of a borrowed one. Not a change to make in the panel alone:
  it is a keyboard and a key-map decision.
- [ ] There is no font size between the two the renderer carries. `lvds_hstx.c`
  has a 6x8 condensed and an 8x14, and `LVDS_FONT_NORMAL` and `LVDS_FONT_LARGE`
  select the *same* 8x14 glyphs - the "large" name only reads as large next to
  the 6x8 captions. MANUAL's values are therefore 8x14 (as the DRO's numbers
  are) and its captions 6x8, with nothing in between. A middle size - or a
  bigger one for the DRO - means adding a font table to the renderer module
  (`lvds_fonts.h` and a third glyph table), which is its own change with its own
  look at the panel.
- [x] The spindle keys no longer force `S1000` (bench report 2026-09-22). `M3` /
  `M4` start at the machine's **modal speed** - `parser_get_modes()` answers the
  `S` a program or an earlier command left - and the panel remembers the figure
  in the state file (`SPINDLE=`), so a reboot starts where the operator finished.
  Software-verified in `--stoptest` (the machine is given `S1500`, `M3` then
  sends `M3 S1500`, and the state file carries `SPINDLE=1500`). The panel still
  has no key of its own for `S`: with none, the modal one comes from a program
  (or the state file, which the operator can edit like `presets.txt`); a key (or
  a third value in the `#` cycle) is the keyboard decision already filed above.
- [ ] 3x3 helper: no animation. Decided ("skip animation, it is weird and
  slow") - the helper just appears under its line.
- [ ] 3x3 helper keys still have square corners. The footer strip's keys are cut
  at the top-right corner by a quarter of their height
  (`NC_KEY_CHAMFER_DIVISOR`, `nc_draw_footer_status()`); decide whether the
  helper's nine keys get the same cut, since they are meant to read as the same
  white keys as the footer's. One call to the same shape, not a second drawing.
- [x] G7x `G80` end mark: the G7X submenu's `6 G80` inserts `G80` in place, on
  the line the cursor is on (checked on the panel: the helper's labelled line is
  replaced by it, exactly like `8 FINISH`). That is where the inline-mode
  terminator goes; the Fanuc numbered-range alternative is the same submenu's
  `4 Q` / `5 N`.
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
  - [x] **3x3 path entry: the pad walks a cycle contour.** The NC editor must
    first insert a G71/G72 header and G80 from its existing vocabulary. `PATH`
    then operates only inside a closed block found by `nc_g7x_block_containing()`;
    it appends G1 rows before that block's end mark and uses the editor's normal
    word entry. It does not create cycle templates or an approach move. Outside
    a closed block it refuses without changing the document. The operator picks
    OD/ID/facing/boring explicitly before PATH. Software checks live in
    `tools/test_nc_ui.py --buildertest`; remaining bench checks are listed in
    `TESTING.md` and `docs/nc-path-builder.md`.

  - [ ] **A line that calls another file, and the 3x3 as its file search.** Two
    steps, in this order, because the second is useless without the first:
    1. the link: one line that names another program on the card and calls it -
       the subprogram/template case. RUN has to resolve the name to a path, load
       it, run it and come back to the line after the call; the preview shows it
       as the program it is, and a missing or unreadable file is an error on that
       line, not a silent skip. The name is a file name, so it obeys the card's
       short-name rule (`docs/sd-card-history.md`) and nothing else: no second
       dialect, no `M98`-flavoured copy of the file list.
    2. the search on top of it: the floating 3x3 in EDIT is already a number
       pad, so a typed sequence is also a file name - `4` then `2` means the file
       whose name is **exactly** `42` (`42.nc`). `#` takes the file it found and
       the line gets the call; `*` (or `A`) backs out a digit; the footer shows
       what is typed, the way it shows a typed word. The operator types the
       number that is painted on the part or written on the drawing instead of
       walking a list of a hundred short names.

       The lookup belongs to `nc_files`, beside the list: one function that
       turns a typed sequence into a path, asked by the pad, by a "type to jump"
       in the file list, and later by `3x3_path_builder.c` when it wants the
       next file. The pad only chooses the file - what lands in the program is
       the call from step 1.

       **And the first line is the program's name.** A short name like `42` says
       nothing about what the file does, so the panel shows the program's own
       first line back as its functional name while the digits are being typed
       (`SETUP OD 42` on line 1 of `42.nc`), and the same in the file list next
       to a typed jump. The read comes from the scratch document the file list's
       preview already loads when the selection changes
       (`nc_files_preview_sync`) - no second reader, and no card read per frame.
       The convention is data, not syntax: nothing parses that line, the panel
       only displays it, the program keeps an ordinary first row, and a file
       whose first line is empty or unreadable just shows its name.
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

### Input, words and presets (bench report 2026-09-25)

Written down from an operator's afternoon of real programs on the station. Each
one is an option to weigh, not a decision - except the first, which is already
possible today and only needs saying out loud.

- [ ] **A word the pads do not offer is addable from the card today.** Every
  helper entry is a section, and a section whose id is a free key path appears on
  that pad, so the missing words do not have to wait for code: OPS `12`-`15` and
  `17`-`19`, TOOL `27`-`29`, WORD `34`-`39`, G7X `49`, THREAD `54`-`59`, PECK
  `64`-`69` (`10`, `44` and `80` are the older spellings of the setup, finish and
  end mark - leave those alone). Two limits bite before the slots run out, and
  `docs/nc-preset-file.md` now says why: 24 records of 8 rows of 96 bytes, of
  which nineteen are the compiled defaults, so **five** new names fit (editing an
  existing id costs none). Open: which entries the panel should ship as defaults
  (coolant `M8`/`M9`, work offsets `G54`-`G59`, program end `M0`/`M30`, ...), and
  whether a section dropped for want of a record should report itself instead of
  vanishing quietly.
- [x] **A `presets/` folder - the entry as an address, and the file as the
  format.** Done 2026-09-25, after the bench called the old shape what it was
  (*"it all now is good and smelly scaffolding, while this all overall is just
  basically addresses"*): `nc_presets.c` reads `/D/presets/<address>.txt` - the
  first row the name, the rest the rows, a row starting with a space continuing
  the one above - keeps only the names in RAM, reads the rows when a key writes
  them, and creates the folder once. The parser, the alias table (`10`/`44`/`80`)
  and the 24-record table are gone; `docs/nc-preset-file.md` is the contract and
  keeps the old format as history. What is still open, and why:
  - a card with the old `presets.txt` is not read: its entries are moved by hand,
    because an automatic migration would keep the parser alive;
  - the address space is two levels (`10`-`69`, one digit per pad and slot). A
    third level (9*9*9) was considered and is not needed until a pad has
    sub-slots, which no pad has;
  - creating an entry from the panel: the file list browses into `presets\` and
    the editor edits what it opens, so the missing half is the new-file flow
    landing there with an address for a name.
- [ ] **Nothing can type a character the keypad does not have.** The editor takes
  digits into the selected word and inserts whole entries from the 3x3 and the
  card; letters and symbols are unreachable (`%`, `(`, `)`, `;`, `[`, `]`, a
  second `G` on a line, any word no entry names). The machine's keypad is a 4x4
  pad with no letters, so the question is about the shell: `tools/nc_ui_win`
  could type any printable character into the selected word the way it already
  types a digit, leaving the panel's own model untouched. Decide whether that is
  a station feature (a PC keyboard where the machine has a pad) or a change the
  screens should carry for both.
- [ ] **Select code and paste it somewhere - or keep it as a preset.** Take a
  range of lines and put them back into the program at another place, or save
  them as an entry with its own key. The pieces are here: the editor's cursor and
  the document model (`nc_insert_line`, `nc_set_line`), the run mark that already
  selects a unit of code, `nc_presets`' writer, and the file list for a name.
  Missing are the selection itself (a mark that survives editing around it) and
  the two destinations: paste at the cursor, and keep as an entry. The operator's
  own programs are the source, so what lands in the card is the same
  `[id]`/`line=` shape the file already uses, and the library grows out of the
  work rather than out of the compiled defaults.

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
- [x] ~~`/D/presets.txt` owns the floating-menu insert text: one `[id]` section
  per menu entry with `name=` and one or more `line=`. Compiled presets are the
  fallback; a missing file is written from them.~~ Superseded 2026-09-25: the
  entries are now one file per address in `/D/presets` - first row the name, the
  rest the rows - so there is no format to parse and the file list and editor
  *are* the preset editor. The parser, the alias table and the fixed record table
  are gone with it; see
  [`docs/nc-preset-file.md`](../../../../docs/nc-preset-file.md).
- [x] Lazy resolve: the SD card is mounted from the main loop, so the entries are
  settled on the first NC input event that finds the drive instead of during
  `nc_visual_init()`. A drive that cannot answer yet is retried; the compiled
  entries are in use until then.
- [x] Show the name of the entries in the floating 3x3 helper instead of the
  fixed slot labels, so a renamed entry reads as renamed on the panel. The name
  is the entry file's first row.
- [ ] Preset entries created and edited on the machine: the file list already
  browses into `presets\` and the editor edits what it opens, so what is left is
  the *creating* half - a new file at a free address from the panel, and the
  folder created on first use (done: `nc_presets_sync()` makes it).
- [ ] Per-entry extra words (`R`, `C`, ...) as further rows once the
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
  is what makes a step jog visible at all. (The X pair has since been flipped -
  `8`/`2` are X-/X+ now - so that the key points the way the drawing moves the
  tool; see the handoff at the top of this file.)
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
- [x] NC: G76 preview stops with an explicit error (pinned by
  `test_g76_preview_error()`); expanding it through the shared threading
  generator is still open. G7x: `G70`/P-Q are done (see its checklist); G73 and
  the finish-stock/allowance-sign work remain.

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
P/Q, G70, the allowance-aware approach and directional roughing are done; the
open items are G73, finish-stock signs/45-degree retract, applied profile S/T,
a single-axis `P` block and the bench-only threading checks. G70's own panel
notes are at the top of this file.
NC supplies document access and UI, not a second cycle generator.

## Remaining NC integration

### Preview geometry ownership (2026-09-24)

Preview-specific coordinate mapping, stock/chuck and dimension geometry,
tool-panel rendering, and emitted-motion/arc drawing now live in `nc_preview.c`.
`nc_draw.c` retains shared panel primitives and tool glyphs; its geometry-only
tool orientation helpers are private. MANUAL and the path builder use the
preview-owned coordinate mapping. This is an ownership/API cleanup, not a claim
that the NC module's total source size decreased.

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

  **Sixth cut done: the editor's other half - and the editor is out.** The rest
  of the EDIT screen went the same way: the code-line and tool-row movement, the
  file list and the open/new flows, the word and field key steps, the selection
  log, saving before the buffer is reused, and the drawing of the file list and
  the code pane with its name row. `nc_editor.c` is 1,119 lines; `nc_visual.c`
  is 1,889 - down from 5,708 at the start of the split and inside the 2k rule.

  What is left in `nc_visual.c` is the screen frame around the screens, which is
  what the file's name says: the mode and view predicates, the document, the
  snapshot and the frame stats, the tabs, the header, the notice, the footer
  strip, the key map (the order a key is offered in), the footer dispatch, the
  TOOLS pane, RUN arming and stepping, MANUAL's and the editor's contexts.

  The key map is now four steps the screen offers in order - the helper, the
  name row, the edit keys (new-file field, words, fields, the selected value)
  and the cursor keys - each of which is the editor's, called from where the
  screen's own sequence wants it. The one place the editor has to hand something
  back is `ctx->follow`: a helper menu entry names a footer action and the screen
  runs it, because what a footer entry does is the screen's.

  **Seventh cut done: the footer actions went to their owners.** The 430-line
  `nc_visual_dispatch_footer_action()` was the largest thing left in the screen
  file, and nearly every case belonged to something else. The document and file
  actions - the helper's menus, the one-line inserts (a tool change, a spindle
  word, a cycle preset by id), the file list with open/new/delete/refresh,
  saving, the cursor steps - are `nc_editor_action()`; the four layer switches
  are `nc_preview_action()`, which hands back the message that goes with the new
  state; the axis/zero/touch entries were already `nc_manual_action()`. What is
  left in the screen's dispatch is a router plus fourteen of the screen's own
  cases: the TOOLS mode switch and its two stubs, RESET, FULL, VIEW, the RUN
  keys (SINGLE, FROM, HOLD, STOP) and the SEND/CLEAR stubs.

  `nc_visual.c` is 1,633 lines and `nc_editor.c` 1,377 - the module's two
  biggest files are both inside the rule now, and the RUN keys and the TOOLS
  mode switch are the pieces a later cut would hand to `nc_run.c` and
  `nc_tools.c`.

  **The check that the editor cut needed.** The frame dumps could not see the new-file
  field - it is only drawn while the file list is up - and the extraction had
  left the field handler with a local copy of the key character, so every digit
  was dropped and a nameless file was created. The bench now has
  `--newfiletest` (`tools/test_nc_ui.py` runs it): it opens the list, presses
  `5 NEW`, types `1 2`, commits with `#`, and loads `/D/nc/files/12.nc` back -
  then repeats it with `A` and proves no file was left behind. With the handler
        passing the key on, the check passes; with the bug put back, it fails.

        The class had a second instance, found later by grepping the module for
        `x = x;` self-assignments: the helper's submenu path kept `ch = ch;`,
        the fossil of a line that used to read the key's character. It was
        harmless there (the parameter already held it), but it is exactly the
        shape that hid the first one, so it is gone. `--editortest` now covers
        the whole class (`tools/test_nc_ui.py` runs it): a digit typed at a
        selected word has to reach the line, a digit that picks a helper entry
        has to run that entry, the G field has to take `1 D` and write the `G1`
        template, and the footer's `*` has to delete through the editor - each
        step proved by saving through the helper and reading the program back
        off the card, so the check is on the document, not on what the panel
        remembers. Putting the bug back fails it, which is how it was validated.
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
- [x] Keep numeric NC word lookup with the NC parser in `nc.c`, not preview:
  preview and the path builder share `nc_line_word_float()`. The compiled
  G71/G72 preset rows also come from the vocabulary templates, while
  `/D/presets.txt` remains the operator-editable override. Preview's layer key
  reads `TRACE`; G7X's contour-entry action reads `DRAW`.
- [ ] Persist cursor position across reboot; current preservation is in-session.
- [x] Hardware checklist exists in TESTING.md; executing it remains open.
