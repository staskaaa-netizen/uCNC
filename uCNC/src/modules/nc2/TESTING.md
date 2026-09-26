# nc2 bench checklist

`nc2` is the module that replaces `nc`. Everything the host tool proves is
**software-verified** (`python tools\test_nc2.py`, and the station's own suite
`python tools\test_nc_ui.py`); what follows needs the machine, because a host
cannot have the spindle, the travel or the panel's glass.

## The card

- Put a card in with a `presets` folder that has no entry of its own, boot, and
  watch the first start: the logo stands while the shipped entries are written,
  once. Boot again - nothing is written the second time, and the logo does not
  come back.
- Delete one entry file and boot: that address is simply not there any more.
  There is no compiled table behind the files to fall back on.
- Pull the card mid-boot and put it back: the panel must not hang, and a card
  that answers nothing must not look like a card with entries.

## The editor and the pad

- Walk a program with `B`/`C` and type a value: the field is picked with `D`,
  the first digit replaces the value, `B` is the sign and `C` the point. Check
  the box drawn around the picked field is where the digits land.
- Press `1`-`9` on the pad at the address root, then walk in with `4` and press
  `7`, and check the rows the entries write are the rows the files hold. Press
  `0` and check the pad comes out in one press; press `A` and check it goes up
  exactly one level.
- Write a program long enough to scroll (more than 17 rows) and check the cursor
  keeps the line in view and the line numbers still line up with the rows.
- Walk the cursor down a long program: the **text** has to move while six rows
  of it stay in view after the cursor - the cursor must not reach the pane's
  last row (`--scroll2test` proves the rule, the glass has to look right).
- On TOOLS: the table's rows above and the tool the cursor is on below - its
  shape on the crosshair and its numbers under it. Walk the cursor down the
  table and the drawing has to change with it; a row that is not a tool shows
  nothing.
- In a run: the **mark follows the cut**, line by line, and the tool glyph rides
  the machine's own position on the drawing. Walk the cursor before the run and
  the mark is the line the operator left it on.

## The run

- `3 FULL` on a two-cycle program: the tool has to follow the same path the
  preview draws, and the marked line has to be the line being cut - it moves
  with the cut and is never one ahead of what has been handed over (that is the
  fault the pacer exists for).
- `4 HOLD` mid-cut and `4` again: the axis stops and resumes without losing the
  block. `5 STOP` mid-cut: the machine stops and the panel says so.
- `1 SINGLE` from a row inside a cycle: the whole block goes out, and the mark
  stays on the row the operator stepped from.
- `# RELOAD` after a fault: the fault clears, the file is read back off the
  card, and the run is over.
- **The live stock**, on the glass: start a run with the tool parked off the
  stock and watch the material come off from the tool's X down as the cut
  travels - the first frame must not read the parking position as a cut, and a
  run that parks must leave the finished part on the RUN screen. Switch to EDIT
  and the stock is whole again. This is the one place the mask's timing can be
  seen: `--live2test` proves the same rule over frames the host draws, but not
  the panel's frame rate or what a long program costs a frame.
- Feed hold, Stop during queued motion, and a run that walks off the end of the
  card's file are bench items - the suites prove targets and ordering, not
  motion.

## MANUAL

- Jog each axis both ways with the step keys and watch the DRO's figure move by
  the step: X must move by what the readout calls the step, remembering that the
  program's X is a diameter.
- Hold a direction key with `#` in feed mode: the axis feeds toward the stop and
  stops on it, and letting the key go stops it where it stands.
- Type the two stops with `*` and check the axis refuses to cross the one it is
  headed for while still being able to move away from it.
- Start and stop the spindle with `7`/`9`/`5` and check the speed is the one the
  machine had, not a fixed default.
- `0` zeroes the picked axis and `D` touches it off: a program run after either
  must agree with the DRO.

## The panel itself

- **Read the screen turned**, as it is bolted: the header at the top, the drawing
  under it, the machine's strip across the middle and the program with the 3x3
  beside it. If the picture comes out upside down, `LVDS_PANEL_TURN` in
  `lvds_hstx.h` is the wrong quarter - one number, and the station's window and
  dumps follow it.
- The strip has to say `uCNC IDLE`, `RUN`, `HOLD`, `JOG`, `ALARM` as the machine
  does them, on every screen: the panel's grey while nothing happens, the run's
  green while it moves, the red for a fault. It is the only place the state is
  said - the header must not repeat it.
- The strip has to sit **on the screen's centre line**, and the space above the
   3x3 has to carry the notes: the error first (red) and the screen's helpers
   under it, with the pad's caption the line directly above the keys.
- **The frame meter** (the reading in the header's far corner) is the instrument
   for the rest of this list: read it while the machine is *cutting* - a still
   screen legitimately reads low, because it only redraws what changed. Compare
   it against the last build before a change to the drawing or the live stock.
  With the tool glyph on the drawing and the live stock under it, a cut is the
  heaviest frame the panel draws: if the meter drops below the panel's own
  period there, the next change is in the drawing, not in the layout.
- The drawing is on every screen, MANUAL included: start a jog and the part is
  in view above the strip.
- Read the code pane outdoors and with the panel at an angle: the selected row,
  the pale block path and the background have to stay distinguishable.
- A run entered straight after a look at TOOLS must send the **program**: walk
  EDIT to TOOLS to RUN with the mode key and press `3 FULL` - the machine has to
  cut the program, not the tool table.
- The spindle has to read off the machine's own signals; the host station has no
  encoder and must not need one.
