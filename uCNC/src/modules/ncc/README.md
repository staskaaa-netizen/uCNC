# ncc - the NC panel, third iteration

## Why it exists

`nc` was the panel that grew too big to be nice (12 316 lines of `.c`/`.h`), and
`nc2` was the clean rebuild of its screens (8 015), but nc2 answered three of the
bench's questions differently than the operator wanted:

- the **preview modes** nc had went away with its footer, and nc2 drew one
  picture (`nc2_preview.h` said the switches "wait for a home if they are
  wanted");
- the **entries** became a folder of files, seeded onto the card on first start,
  and every pad label and insert then read the card (`nc2_labels_at()` asked
  9 slots, and each asked 1 read plus up to 9 `exists()`);
- the **layout** is one shape, the portrait one.

`ncc` is nc2's tree with those three answered the way the bench asked, and
nothing else moved:

- the entries live in **RAM**, laid down from one table at boot, with one
  optional card file read once at boot;
- the drawing is switched by **`#`** - DIN, or the stock and the path alone -
  and the run screen shows the stock while the machine cuts;
- the layout is written **once for both orientations** and picks its geometry
  from the picture's own shape.

It is a third module, not a fork that replaces nc2: `nc` and `nc2` both stay in
the tree and either panel can be the one the machine runs (see *The build
switch*).

## The panel

The screens are nc2's, unchanged: MANUAL, EDIT, TOOLS and RUN in the header, the
drawing under it, the machine's own strip across the middle (work X and Z, feed,
spindle, state word), and the bottom band with the text pane on the left and the
3x3 in the corner. `ncc/README.md` is not the place to re-describe them -
`uCNC/src/modules/nc2/README.md` is, and everything it says about the keys, the
run, the tools and the live stock holds here. What follows is only what ncc
changed.

### The picture it starts with

The panel comes up on a picture, not on a blank screen or the last thing it
showed: `assets/logo.bmp`, painted for **ten seconds** (`NCC_BOOT_HOLD_MS`) or
until the first key, then the work screen takes over.

The picture brings **its own sixteen colours**. The panel's table has no blue in
it and its greens are bright, so quantising the picture onto the panel's colours
turns it grey (the first attempt did: the overalls and the machine both lost
their colour). The splash therefore takes the renderer's palette for itself when
it starts (`lvds_hstx_rgb()`) and **hands it back** when it lets go
(`ncc_boot_release()` -> `lvds_hstx_palette_reset()`, the one call added to the
renderer for this), so the work screen is drawn with the panel's own sixteen and
nothing about the panel's colours changes.

### Where the panel update runs

The module is registered on `cnc_io_dotasks`, not `cnc_dotasks`. The controller's
normal module-task path returns early while an E-stop, reset or other alarm is
latched; putting the panel there left the scanout running but never painted its
first frame, so the glass stayed black. `cnc_io_dotasks` still runs in the reset
wait loop, allowing the splash and fault screen to be visible while motion is
locked out.

Bench, 2026-11-16: with the machine latched in E-stop reset, the old registration
reported zero NCC updates and an all-zero SRAM framebuffer. Moving the listener to
`cnc_io_dotasks` painted 63,093 framebuffer bytes and the boot picture appeared
on the glass. The temporary counters used for that check were removed.

### The raw input diagnostic

The bottom diagnostic row carries the complete four-register 74HC165 chain as
one binary value, `0b...`. It is sampled every 10 ms through the same
full-duplex PIO transfer that owns the 74AHCT595 chain. The two-pulse output
latch preserves the current motion outputs while a sample is taken; it is a
bench reading, not a second input owner and not a motion-path fallback.

`ncc_boot_image.c` is that picture, generated - the way the renderer's fonts are
generated files beside their source:

```powershell
python tools\boot_image.py uCNC\src\modules\ncc\assets\logo.bmp
python tools\boot_image.py <picture> --preview out.bmp      # look before flashing
python tools\boot_image.py <picture> --panel-palette        # no palette of its own
```

The tool reads the panel's colours out of `lvds_palette.c` (so the two cannot
drift), fits the picture to the view - 600 wide, the glass being mounted turned -
quantises it to sixteen, medians the speckle out (the source is a resampled
illustration, not flat pixel art: the cleanup both sharpens it and takes a
sixteenth off the flash), and writes the runs. The picture costs about 150 KiB of
flash; the release has sixteen megabytes and uses under two hundred kilobytes of
it.

**It carries the build's own stamp** - the date and time the image was compiled -
under the picture, so the glass always says which image is in it. That is not
decoration: on 2026-09-27 a stale build directory silently re-sent an old image
for two flashes while the source said otherwise, and the stamp is what makes that
impossible to hide. `--dump-boot` writes the splash frame, stamp and all.

### The strip: what the machine is doing, and what it is doing it with

The DRO reads left to right as *the machine, then the setup*: the **state word**
first, at the left end where the eye lands, then work **X** (a diameter),
**Z**, the **feed** and the **spindle's speed**, and at the right edge the three
words an operator sets up with but rarely reads in a hurry - the **tool in the
spindle** (`T2`, the machine's own answer from `ncc_run_tool()`, `-1` until a
`T` has gone out), the **compensation in play** (`G40`/`G41`/`G42`, the modal
state the panel keeps for itself, `ncc_run_comp_mode()`), and the **coordinate
system** the figures above are read in (`G54...` from the core's own
`parser_get_coord_system()`). `uCNC` left the strip for the **header's right
corner**, the state word took the strip's left end, and the **frame meter moved
to the bottom** (the header's right corner was its old home; a diagnostic does
not need the corner an eye lands on) - bench, 2026-09-28: *"put uCNC on the
rigth side of header. move fps to bottom"*.

**And EDIT draws the offset paths in green - the *finish* ones, all three.** Over
the programmed profile, the pane draws the path the tool's centre follows for
**each shift at once**: `G40` (the path as programmed, no shift at all), `G41`
(the tool on the left) and `G42` (on the right), each walked the way the cycle
would cut it (`ncc_offset_preview()` and its `ncc_offset_walk()`, `ncc_preview.c`).
Two of them are the offsets and the middle one is the path they came from, which
is what makes them readable against each other (bench, 2026-09-28: *"now on
offset for debug - drawm all three variants g40 g41 and 42. all in green is
ok"*).

**The finish pass only**: the roughing passes are the cycle's own staircase, and
drawing them green buried the profile under them (bench, 2026-09-28: *"all lines
are green. now only finish should be taken into account?"*). A program with no
cycle draws all of itself, because then it *is* the profile. The walk is the
sender's own, with the compensation set per variant, and a program that carries
its own `G41`/`G42` overrides the direction as the walk meets it - the three
lines are the *candidates* a debug line is for. No radius in the table, no line:
it never claims a shift that cannot happen (bench: *"add green line in edit to
show offseted tool path at r0 of tool for debug"*).

**The radius is the card's table, for the tool the program names.** The table
used to be read only where it is *shown* - TOOLS, and RUN for the brush's
radius - so EDIT drew the three shifts at the built-in row's `R0.8` whatever
the operator had measured, and a card whose tool really said `R0` still got a
green line (`ncc_visual_init()` / `ncc_visual_select_mode()` now read it for
`NCC_MODE_PROGRAM` as well; bench, 2026-09-28: *"only line is seen here? or its
takes no tool without running?"*). The tool is the last `T` the program speaks
at or above the line (`ncc_tools_active()`), and a program that names **none**
has no line at all - answering with the table's *first* row is the guess that
put the wrong shift on the glass while the program cut with another tool (bench,
2026-10-04: *"still takes first tool radius for g71"*). `--offset2test` pins it:
the program names `T1`, `R0` draws nothing and `R3` draws the line *off* the
profile.

**A program with no `G71`/`G70` contour has nothing to shift yet.** Compensation
is applied where the contour is built - G7x's cycle region
(`g7x_stream_set_comp()`) - so on a plain `G0`/`G1` program all three walks emit
the *same* lines and the one green line the operator sees is all three on top of
each other. Shifting a program written outside a cycle is
`docs/lathe-cutter-comp.md`'s stage 4 (lead-in/lead-out, `G40` mid-contour),
not something the preview can do on its own.

### The preview: `#`

`#` switches the drawing between two states:

| state | the drawing |
| --- | --- |
| **DIN** | the whole picture: stock, chuck, the contour, the emitted path, the rulers and the dimension callouts |
| **stock + path** | the same without the DIN layer - the plain block and what the program cuts |

On RUN the key is the run's (`#` reloads the program), and the screen does not
ask: **while the machine is cutting it draws the stock** - the cut is what the
operator is watching - and **while it is parked it draws the path**, both wearing
whatever DIN state EDIT left behind. That is one flag (`ncc_preview_din()`),
because the bench's own words were "# switches between preview modes - din or
only stock and path", not four independent switches.

**The DIN layer is the run screen's too.** The rulers, the centrelines and the
stock's dimension callouts are drawn while the machine cuts, not only when it is
parked (bench, 2026-09-28: *"enable din in run preview i think i see the
problem"*): the cut is only readable against those numbers, and a frame the
operator cannot measure is a frame they have to take on trust. **The rows' own
points and dimensions come with it** - each row's contour point with its X/Z
callout and its `C`/`R` label, redrawn every frame so a live band repaint cannot
leave a piece of a callout behind (bench: *"and dimensions too ?"*). What the
run screen still does **not** draw while cutting is the program's *path* - the
emitted moves and the three offset lines - because the glass is already showing
the cut itself; those come back as soon as it parks.

`#` is the editor's key too, and the order is fixed: the path builder is modal
and takes `#` as its step key (`nc`'s own); a value that is *picked* takes it as
the editor's accept, exactly as nc2 had it - typing a number is the one time the
key is not the drawing's; everywhere else it switches the preview.
`--preview2test` pins all three.

**The editor's own line is marked on the drawing.** Every contour point carries
a ring, and the point of the row the cursor is on is *filled*; the emitted
motion of that row is drawn three pixels wide instead of one
(`ncc_contour_point_marker()`, nc's `nc_preview_contour_point_marker()`). So the
line the operator is reading in the editor is the line the drawing points at
(bench, 2026-09-28: *"we have missied in some point marking active select line in
edit to line on preview screen"*).

**Arcs are drawn as arcs, in the frame the program uses.** A programmed corner
radius (`R`) and a compensated one are emitted as `G2`/`G3`, and the preview
walks every point of the arc through the same axis mapping as the rest of the
drawing. Two things about that frame are worth stating, because both were wrong
once: **`I` and `R` are *radius* values while the program's `X` is a diameter**
(Fanuc's own rule - the cycle emits `I5.000` for a five-millimetre corner), so
the arc's geometry is worked in the radius plane and doubled back when mapped;
and the *commanded* turn decides which half of the circle is drawn, a `G2`
sweeping one way and a `G3` the other. Getting either wrong drew a corner as a
wide loop over the profile, or as the straight chord it used to be (bench:
*"gren line of ofssete path for rnd shows chamfer path"*, then *"seem still
straigh line in panel"*).

### Typing a G code

The `WORD` pad's `G` entry (the address `31`) writes a bare `G` on a line of its
own and picks its value, so the operator types the number. **That is not where
it ends**: the panel knows the
codes - `ncc_vocab.c` holds each one's name, the words it takes, and the line it
is written as - and when the field is finished the line becomes the code's own
line and the first **value** is picked:

```text
   G71                             after typing 7 1
   G71 U0 R0 X0 Z0 F0 P0 Q0        after # (the value is picked, ready to type)
   â†‘ status: G71 OD roughing: U R X Z F P Q
```

`nc` did this when its G field was accepted; nc2 dropped the table, which is why
a typed `G71` stayed a bare `G71` with nothing to walk into (bench: *"it does
not prefill it ... it will not be resolved later to x z additional values"*).
The line is only rewritten when it is **nothing but** the `G` word - a `G` word
inside an `N`-numbered line is named in the status and left alone - and a code
the table holds with no words of its own (`G21` metric) is named and left alone
too, because it is already a whole line. `--gcode2test` pins all three.

**And only a code that was just *typed* is resolved**: the last word is let go
with `#`, or stepped off with `D` after the operator typed into it - the two
ways a typed code is finished (nc resolved on its G field's own accept). The
cursor *travelling* over a line with `B`/`C` leaves it exactly as it stands. The
port ran the resolve on every key the editor took, so a bare `G71` in the
program expanded into its template and a `G41` was picked just because the
cursor passed over it - and a lone `M` word (which the vocabulary does not hold)
did neither, which is the difference the bench saw (2026-09-28: *"it still wants
to select this g41 or g42 while i travel along. and not m commands"*).
`--gcode2test` travels over `G41`, a bare `G71` and `M2` and insists none of
them changed and that the cursor kept moving.

Two things the words themselves got right, from the same bench pass:

- a general **`G1`** is written as `G1 X0 Z0 F0`. nc's template carried `C0 R0`
  - the chamfer and the corner radius - but those are *contour* words: they
  belong to a row the G7X path builder writes, not to every feed move (bench:
  *"g1 with c and r is wrong. it does fit only to the g7x path but not general
  code"*). The word labels for `C` and `R` on a `G1` are unchanged, so a contour
  row still reads as what it is;
- the pad's `WORD` group has an **`F`** key (the address `37`). Pressed on a line
  it lands as ` F` and leaves the value picked, so a feed can be added without
  typing the letter (bench: *"now i do not see how to add feed?"*). `C0`, `R0`
  and `N` land the same way - `N` at the *front* of the line, where a line
  number belongs. The other lone letters are words of their own: `G`, `M` and
  `T` write a line each (bench, 2026-09-28: *"Tn insertes in same line - but it
  sould not"*). That rule, and where the rows a key writes come from, is "The
  entries, in RAM" below.

### The run owns the file it is cutting

EDIT and RUN are the same program, and while the pacer is feeding that program
to the machine **the buffer belongs to the run**. Two things follow, and both
are the bench's own words of 2026-09-28 (*"if i go from run while it is runnig
to edit i am stuck here + its need a safety not to edit [a] running file"*):

- **A screen change never re-reads the program under a run.** The document is
  given back by TOOLS only - the one screen that puts something *else* in the
  buffer, the tool table - and by a screen that never had a program at all
  (`ncc_visual_select_mode()`). Coming from RUN the buffer already **is** the
  program, so the mode key leaves it alone: re-reading the card mid-cut swapped
  the program the machine was half way through, and a FAT read inside the frame
  loop is the delay the tool table is loaded once to avoid.
- **EDIT becomes a viewer while the run is in flight.** `B`/`C` walk the cursor,
  `#` switches the drawing, `A`/`0` leave; every key that would write the text -
  a digit, `*`, `D`, a pad press, the path builder - and every way of swapping
  the file - the picker's `D`/`#` (`ncc_visual_open()` refuses it), its `5` and
  `6` (make, delete) - answers *"Program running - the file is the run's"* and
  lands nothing. nc's own sentence for the same idea was "RUN owns the line"
  (`nc_editor.c`), and this is that rule with the editor as the protected side.

`--runlock2test` pins both: it gives the card a *different* program behind the
panel's back after the run is armed, so a switch that re-reads it shows in the
machine's own end position.

### The tool table's group is the TOOLS screen's

The pad's root carries the six groups nc's footer had - OPS, TOOL, WORD, G7X,
THREAD, PECK. The seventh, `7 TABLE`, was the TOOLS screen's own pad root shown a
second time (a screen's pad opens at `7` there), so the root of the other
screens no longer offers it (bench: *"push 7 table to tools? because it seems to
be the same"*). The entries are still the table's, and `--pad2test` insists the
table is reachable on TOOLS and not from EDIT's root.

That pad is a **row editor**, not nine templates: `1` is the table's own `[71]`
entry - it writes a new row, with the tool's number picked so the digits type it
- and the keys after it pick a word of the row the cursor is on, the key's
number being the word's number: `2` the radius (`ncc_pad_word_index()`, and the
key's label is that word's own letter). The row carries `T` plus its eight
fields, so keys `2`..`9` pick them in order.
The nine fixed `ADD T1`..`ADD T9` rows were a number in a template and nothing
else (bench, 2026-09-28: *"remove this add t1 add t7 etc. just insert new on 1
and then each next number is edit selected field so 2 will be radius 3 will be
orient"*).

### The pad's keys are square, with a cut corner

Every key of the 3x3 is a **square** whose **top-right corner is cut at 45Â°, a
quarter of the key's height** (`NCC_KEY_CHAMFER_DIVISOR`) - the shape nc's own
keys had, which nc2's port had lost. The cut is the page background and its edge
is drawn like the rest of the outline, so the key reads as one machined button.
The box is exactly three keys across and down, so nothing is stretched.

The **portrait key is 70 px, a shade under nc's own 72**. The bench walked nc's
72, then 79 and 86, and landed here (*"now 70 wide"*): a line of the panel's font
holds **eight** characters at either 72 or 70, so the smaller key costs the code
no width and buys the text pane the room back. The pane is **366 px** wide
against the 318 it had at 86. The **landscape** shape keeps its 64 px key: the
room there already belongs to the fault block. `--chamfer2test` and the host
test's geometry check both read the layout back off the glass.

### The two colours that say what a word is

- the **word being typed** wears nc's own box: **white on the panel's black**
  (`ncc_col_field_fg()` on `ncc_col_field_bg()`), which is what makes it read as
  the thing the digits go into. The port had it the other way round - black on
  white, the same as the key faces beside it;
- the **legend** (the word's name, on the row above the cursor) is **black on
  the pale yellow** the panel already marks a cycle's rows with
  (`ncc_col_block()`), not yellow text on the pane's grey. It is the same "this
  is about the word" mark wherever the row lands - bench: *"label is with yellow
  text - but should be same black with yellow background as before. better -
  pale yellow as in g7x group mark. text to be edit - white on black
  background"*.

The **row's own mark** is the band its text sits on, not the whole row, and the
**box is a row taller than it** (`NCC_ROW_H - 4`, drawn before the value): nc
drew the mark with each text run's background, so the black box of the word
being typed stands proud of the yellow band above and below it - bench: *"on nc
we have its black bigger in height compared to yellow strip. do same here"*.

The **keys are one font too**: the number and the name on a pad key are both the
normal font (a name too long for the key wraps onto its second line), where the
port had the name in the small print under a big digit - bench: *"then number is
bigger. it is wrong"*.

`--colour2test` reads both off the glass, and off the key faces, so the three
cannot be confused again.

The box is as wide as the word itself - it always was; what was wrong was what
went into it. The row drawing handed the renderer a **pointer into the line**
(the rest of the line) instead of the word, and `lvds_draw_text_clip()` ends any
string *longer* than its column count with `...`, so a picked word came out as
`G...` - the bench: *"g970 one edit shows ... still if i select 3 digit
number"*. Each piece is now copied out of the line before it is drawn, the way
nc's `nc_text_draw_line_with_word()` did it, and `--colour2test` compares the ink
of the word in the box against the same word on the row: three dots carry less
than half the ink of the four letters they replaced.

### What the panel says, in full

A plain message above the 3x3 is **one of the labels**: the labels' own font,
the whole column's width, and a second line when the sentence needs one - a
refusal used to be cut at the column's edge (bench: *"error ms is being clipped.
no - use full label space to show it"*).

A **fault** is nc's own block: **white letters on the fault's red**, in the
panel's font, as many lines as the sentence needs (`nc_visual_draw_fault()`,
`NCC_FAULT_LINES` - bench: *"error style - red background with white letters.
same as nc"*). It is the one thing an operator must not miss, so it is the one
thing that gets the room - and it **fits everything the panel can say**: the
status line is `NCC_STATUS_MAX` characters, and the block measures the sentence
against its own columns, dropping to the labels' smaller font when the panel's
would not hold it (bench: *"there is nothing more annoying than an error you
cannot read. So it should fit all possible messages"*). The header's message
gets everything between the screens and the frame meter, which is what the file
name used to take.

**`G41`/`G42` is the panel's word, and the tool table's radius.** The controller
parses the compensation group and does nothing with it
(`docs/lathe-cutter-comp.md`: *"the most dangerous part of the current state"*),
so the panel takes the modal word for itself: `G40`/`G41`/`G42` never go to the
machine, and the cycle is handed a contour shifted by the nose radius
(`lathe_comp`'s geometry, G7x applying it - `g7x_stream_set_comp()`).

**The word names the side; the radius belongs to the contour.** The word records
the **side** and nothing else, and the nose radius is read from the **tool
table** for the tool in play **at the cycle's contour** - every time, not once
at the word: a program may call its tool *after* the word (`M3 S450` / `G42` /
`T2` / `G0` / `G71` is the bench's own order), and it may change it again before
the cut (`G42` with `T2` in the spindle, `T1` called, then the cycle: the cut is
`T1`'s). Reading it at the word is what put the table's **first row's** `R` on a
`G71` whose tool had not been called yet (bench, 2026-10-04: *"still takes first
tool radius for g71"*). A compensation the table can give no radius for **stops
the run**, at the cycle the operator has to fix, rather than cutting the
programmed path. `ncc_tools_number()` has no tool-1 fallback either: a number the
table does not carry has no nose radius, and the built-in row is only what a
machine with no table at all has. The pane still draws the programmed contour,
so the preview is the next thing to teach (`lathe_comp/README.md`, "What is not
here yet").

**And the tool and the side a screen *wears* are the machine's own line - read by
G7x, not by this panel.** The strip's `T`/`G40`-`G42` and the brush's radius
come from **G7x's state machine**
(`g7x_state_at()`, asked through `ncc_run_tool()`/`ncc_run_comp_mode()`): the
last `Tn` and the last `G40`/`G41`/`G42` at or above the line the **machine** is
executing. The two words are the dialect's, so G7x owns their reading
(`g7x_line_tool()` for the `T`, `g7x_command_is()` for the side, the side in
G7x's own `g7x_comp_side_t`) and `ncc` only says *where* the machine is: the
panel grows no second reading of a program, and G7x needs no panel to answer -
it stays a module of its own (`g7x/TESTING.md`, "state words").

The sender is a block ahead by design - it has to be, to expand a cycle - so an
answer taken from the sender's own walk showed `G42` while the `G41` part of the
program was still being cut, and a run **started inside** a compensated portion
(`2 FROM` on a later cycle) armed from a global that had just been cleared and
cut the block with `G40` (bench, 2026-10-04: *"now g42 still appears while g41
portion was not finished. so make it very state machine in g7x for tool and for
offset"*). Nothing is carried: re-reading the rows up to the machine's line is
the whole of the state, so the same answer comes after a re-arm, after a step,
and for a run that is over - which keeps the last answer it had, so the parked
picture stays the picture of the part. `--state2test` pins the panel's half and
`state words` the module's.

**The cut's shape is the row's own nose radius.** The live stock is taken off by
a brush that walks the machine's path - the framebuffer's own removal, the one
that paints the ground where the material was (`ncc_live_clear()`) - and at every
step of that walk the brush is a **circle** of the row's `R` at the drawing's
scale. Clearing from the circle's *outer* half instead left the ends flaring
deeper and read as a square block (bench, 2026-09-28: *"just brush is still
square. not round"*).

**The brush is the nose's disc, and its upper arc is the surface in both
modes.** The material goes below `top = sy - h`, one rule for both. `G41`/`G42`
put the machine on the tool's **centre** path - one nose radius outside the
surface the nose cuts - so the tangent lands on the programmed row. Without a
shift (`G40`) the machine's point is the tool **corner**, and the same arc is
one radius above that corner: with `T1 R3` and a row of X20 the material edge is
X14, one nose diameter less. Moving the `G40` bound down by a radius
(`sy + r - h`) forced the edge onto X20 on the host, but on the glass it cleared
one radius too deep while the machine point was right (2026-10-17); that arm is
gone. The old `G41` over-compensation was a different fault - drawn on
the machine's point instead of the disc, it left X26.03 where the row said X20
(bench, 2026-09-04: *"only drawing seems to be wrong ... check position of
actual clening brush vs sendded coordinates and real mashien coordinates"*).
`--brush2test` reads the material's edge in X and Z off the glass on `G41`,
`G40`, and `G40` with an active `G54` offset, and takes one final frame with the
run parked, so no live ink is in the measurement.

**The run draws only the nose circle.** The flanks, the polygon and the fill
were taken out: the first versions read wrong on the glass, and the panel
dropped to 20 fps from 40 with them in the frame (bench, 2026-10-17: *"skip
flanks too. its sounds heavy we are only on 20 fps from 40"*). What is left is
the brush's own circle; there is no insert body. The **TOOLS screen** keeps the
row's fields - `T R F Q D E S X Z` - but the `O` orientation, the insert picture
and the rays are gone, and the operator checks the insert itself.

**The brush's radius is the drawing's own scale.** The table holds no insert size,
so the one number that matters is the row's `R`, in the pane's millimetres of
radius (`ncc_map_x()`'s own scale, doubled): the same `R` that the compensation
geometry uses, so the picture and the cutter agree about how wide the cut is.

**The mark is the line the controller says it is on.** The panel numbers every
block it hands over (`N12 G1 X30.000 F500.000`), the controller carries that
number with the line and reports the one it is *executing* - `Ln:` in its own
status, `itp_get_rt_line_number()` in the core - and the pane maps it back to
the line of the program that number came from (`ncc_run_executing_line()`, a
ring of `number -> line` kept as the blocks go out). That pair is the whole of
the map, and it is the controller's own answer rather than the panel's guess:
the mark used to follow what the *sender* had handed over, which is a step ahead
of the tool inside a block, so it sat on the next line while the cut was still
on this one (bench, 2026-09-28: *"it marks next line sometimes ... we need to
get this line number from ucnc core itself"*). A cycle is one line of the
program and many blocks of the stream, so the number maps to the *block's* line
and a cycle is still marked as the cycle.

**A finished run hands the machine back.** The strip says RUN while the machine
is *executing* what it gave it, not while the panel believes the machine's
queues are busy or while a flag it picked for itself stays up. The word comes
from the controller's own status (`cnc_get_status()`, the answer its `<...>`
report carries), and the colour from the same place, so the strip cannot
disagree with the machine (bench, 2026-09-28: *"status is run ... if i get ucnc
status - it shows idle correctly"*). `EXEC_RUN`, which the panel used to test
for itself, is not the flag the controller reads and stayed set after the last
block.

**A program ends with `M2`, and the end-of-program lock is off.** `M5` is the
spindle stop, not the end of a program; uCNC answers `M2`/`M30` with a program
stop of its own, and - unless the lock is switched off - with
`EXEC_POSITION_MAYBE_LOST` and then `STATUS_PROGRAM_ENDED` for every line after
it, so a run ends *locked* and the next one is refused until the machine is
unlocked. The station built with the lock off and the machine did not, which is
the kind of difference the host can never show; `rp2350.ini` now switches it off
for the panel's own builds (`-D DISABLE_ENDPROGRAM_LOCK`), and the strip names
`LOCKED` if a machine is ever in that state (bench, 2026-09-28: *"it seems like
m5 was wrong command for ucnc core. with m2 it nicely goes to the end"*).

**A fault does not move the operator.** The run's mark stays on the line that
failed, and answering the fault with `# RELOAD` - which reads the file back and
clears the run - leaves the mark where it was instead of carrying it to the top
of the program; the editor's own cursor is the session's and never follows the
machine (bench: *"on error - it still jumps to first line. no i should stay
here"*). `--fault2test` reads the block's white-on-red off the glass, the mark
after the fault, the mark after `RELOAD`, and the editor's line after the whole
pass.

**A run's line is the block it is in, not the line the stream has walked to.**
The bench, 2026-09-27: *"on errors - again 1 line is selected and this one will
not run"*. Three things were wrong at once, and each one put the mark somewhere
the operator could not use:

- **the generator's own motions answered for line 1.** A clearance rapid, a
  pass retract, a change of pass - none of them is a row of the program, and the
  blocks the generator makes say so with `(size_t)-1`. That value was written
  before the block was cleared instead of after, so the clear left 0 and every
  generated motion claimed the program's first line - which, in a program that
  opens with `G971`, is a stock definition `1 SINGLE` cannot run. The fix is in
  the owner (`g7x_block_set()`, and `tools/test_g7x.py generator` fails without
  it);
- **a block's own motions answered for the *next* unit.** The stream walks
  `source_line` past a cycle's rows before the generator can answer for the
  whole of it, so the raw answer for a generated motion is a line the run has
  not reached. The stream now carries `block_line` - the cycle's header while
  its expansion walks, the line itself for a line handed over as it stands - and
  that is what a fault marks. `--blockline2test` pins it;
- **a `G70` took the mark back.** The run's unit was the line's *path*, and for
  a finish cut that answers with the range it re-cuts - rows of a block the run
  has already finished. It is `nc`'s own answer now
  (`g7x_doc_block_containing`, so a `G70` is a unit of its own line), which also
  fixes `1 SINGLE` on a finish cut: it sent the profile's rows instead of the
  cut. `--pace2test` builds its list of markable lines from the same rule, so
  the check and the run cannot disagree again.

The sender's line also only ever goes forward now (`emitted_line + 1u` clamped):
with a block's motions answering for the block, the raw answer walks back to it
after a row has gone out, and the pane reads this line as "handed over up to".
`--refuse2test` still covers the other half of the same story - a run the
*panel* refuses, never seen by the controller, marks the cycle it could not
expand rather than the line after it.

**And the fault says which block was refused - because that is how the real one
was found.** The sentence names the line of the *program* the fault belongs to,
and that line is a cycle as often as not: the whole of the block is what went
out, and which of the motions the machine read is the one thing the program's
line cannot say. The block is drawn in the same red under the sentence and
logged (`[MSG:NCC REFUSED %.96s]`), read from the sender's own queue - the block
the parser was reading, or the one just behind it when the refusal came after
the line was already in the parser. The bench answered the question with it in
one run (2026-09-27):

```
[MSG:NCC G7X OUT G0 X]
[MSG:NCC SEND G0 X]
error:2
[MSG:NCC REFUSED G0 X]
```

**The controller was right, and the panel was sending nonsense.** The generator
meant `G0 X55.000`; the machine got `G0 X` - a word with no number - and said
`error 2`. The *comments* were short in the same way, `(G71 rough X)` where the
panel meant `(G71 rough X23.000)`: every float in the string had gone. The
cause is not in this module at all. The RP2350 core links **newlib-nano**, whose
`snprintf` *drops float conversions* unless the float formatter is pulled in,
and a dropped conversion is an empty string. `rp2350.ini` now asks for it
(`-Wl,-u,_printf_float`), which is what makes every number the panel says
reach the glass and the wire: the DRO, the preview's dimensions, the tool
table, and the blocks a cycle expands to.

The host cannot see this class of fault at all: it links glibc, which formats
floats whether or not anyone asks. So the one check for it reads the ELF the
machine build makes - `tools/test_ncc.py`'s `check_float_printf()`, which fails
without the link flag ("every number the panel prints would be an empty string
on the machine") and has been walked against the ELF that had the bug.

### The chuck, and `G972 C`

The jaws are nc's own drawing (`nc_preview.c`): a hatched bar whose **length
along Z and thickness both come from `G972 C`**, from the pane's left edge to a
little past the stock's left end, with the jaw's pivot circle hanging under the
stock's foot. The port had replaced this with a vertical block *inside* the
stock and then filled the stock over it, so the chuck was never on the glass and
`C` had nothing to change (bench: *"chuck g972 stopped working"*).

**Half of it is under the stock**, exactly as nc drew it: the bar straddles the
stock's bottom edge, the jaws' pivot circle is centred on the stock's bottom
corner, and the stock - drawn after them - is what covers the upper half, so
what the operator sees is the lower half of the bar and of the circle. It is
drawn before the material on every whole-pane frame, so a band repaint can never
leave it behind. `--chuck2test` counts the jaws' own colour with a small clamp
and a large one.

### The file's row, and the legend

The pane's own top row is **the file the program belongs to** - the path and the
`*` that says it is unsaved, or `(no file)`. It is nc's row, back, and it is
there for three reasons:

- it names what is being edited, in the pane the operator is already reading;
- it is the row the legend uses when the cursor is on **line 1** - the word
  being typed is named on the row above the cursor's, and on line 1 that row is
  the file's, so no program line is ever covered. nc2 had taken the row out and
  the legend landed on line 1 itself (bench: *"editing the first line does mark
  it wrongly"*);
- the cursor can sit on it: **`B`** from line 1 steps up onto it, where it reads
  as a selected field; **`D`** there opens **the folder the file lives in, with
  the file itself selected**, so the way back to the card from a program lands on
  the program; **`C`** steps back to line 1, and any other key hands the cursor
  back to the code and does its own job. A program that was never saved falls
  back to the card's root, which is what `0` opens.

**`0` is the same move.** It opens the card on the folder the open file lives
in, on the file - not on the card's root, which meant walking down to the folder
again on every visit, and not on "whatever sorts first" (bench, 2026-09-27:
*"it was better approach in nc - same folder if 0 pressed not root"*, nc's
`nc_files_select_path()` doing exactly this). With nothing open - a program that
was never saved - it falls back to the root, which is the only place there is.

**A refresh does not move the pick.** `8` reads the folder again and the entry
the operator was standing on stays picked; it used to fall back to the first
entry of the fresh list, which in any folder under the root is `..`, so the mark
jumped off the file they were about to open (bench: *"on refresh button pressed
- it select next file"*). A change of folder is a different thing and does start
on the list rather than on an entry.

**The screen the panel is left on is the screen the card gives back.** The state
file (`/D/nc_state.txt`, nc's own keys) carries the mode as well as the open
file, and it is written **when the screen changes** - nc's `nc_state_save()` in
its mode setter, which the port had dropped. Only a file open or a dirty
document wrote it, so the panel came back to the first screen it had ever
written: EDIT, always (bench: *"state was better transferred in nc. now it
always starts in edit"*). `--file2test` leaves the panel on MANUAL, initialises
it again and insists it comes back there.

The program is drawn one row below the file's row, so the pane holds one line
fewer than the band's rows divide into. `--firstline2test` pins the mark, the
row's own light, and `D` landing on the file.

The **header does not name the file**: nc2 put the path there because it had
taken this row out of the pane, and with the row back the header naming it again
is the same path twice, one above the other (bench: *"file name is still header
next to run"*). The header keeps the four screens, the machine's message and the
frame meter; the pane's row is the one place the file is named.

### The entries, in RAM

An entry is an **address** - the digits walked on the pad, `4` then `7` is `47`
- a **name**, and the **rows** a key writes. The shipped set is one table in
`ncc_presets.c`, and it is the only place those addresses and rows exist.

At boot `ncc_presets_init()` copies that table into a RAM table, and from then
on the pad reads RAM: `ncc_slot()` is a walk, and a press is a copy. No key
press touches the card.

The card is read once, and only for what it wants to change:

```text
D:\ncc_presets.txt

    [11] SETUP
    G970 X0 U0 Z0 W0
    G971 X0 Z0 I0 E0
    G972 C0
    G973 P0

    [41] G71
    G71 U0 R0 X0 Z0 F0 P0 Q0

    [37] Fn
     F
```

- a line starting `[` opens a block: `[address] name`;
- the lines under it are the rows the key writes. A row that starts with a space
  is a **join** and everything else is a **line of its own**;
- a block **replaces that address**; every address the file does not name keeps
  the shipped entry, so a card can change one word and leave the rest alone;
- `#` and `;` start a comment line, and a block with no rows keeps the shipped
  rows (naming an address only to rename it cannot wipe the entry).

**The join rule, both halves of it.** Inside a block, a leading-space row
continues the row *above* it in the file, so a `Q` or a `C` gets onto the row
already written without the controller seeing a break inside a block. As the
block's **first** row it joins the line the press began on: ` F`, ` C0` and ` R0`
land at the end of that line, and `N` lands at its *front*, where a line number
belongs (bench, 2026-09-28: *"appends are append for all plus prepend for n"*).
What it joined is left picked, so the digits type its number - which is how a
line that already exists gets its feed, its line number or its chamfer without
the operator typing the letter. A row that does not start with a space is a line
of its own, below the point the pad was opened at.

That is what makes the **same file** say both kinds of key. `[11] SETUP` is four
rows and writes the whole stock block; `[14] VIEW`, `[15] STOCK` and `[16] CLAMP`
are one row each and write it line by line; `[37] Fn` is one join row and adds
the feed to the line the operator is on. The rows a *table* entry writes are
plain lines - the tool table is rows appended whole, which is what "it is static
as it should be" means.

**A press that writes ends the pad**, which is nc's own rule: its menu called
`nc_editor_modal_cancel()` before every entry, so the keys went back to the root
and the next digit opened a group rather than the next slot of the one that had
just written. The port left the pad up and made `A` the only way out, which is
what the bench hit at the compensation keys (2026-09-28: *"g4x does not exit
after insert normally only via A"*). The row that landed keeps its value picked,
so the digits type it, and the next entry lands directly under that row - the
cursor is on it. `A` still steps a level up *while* choosing, and `0` still
leaves the pad in one press. The walk that genuinely stays up for every point is
the path builder's own (`47`), which is a different machine (`ncc_contour_*`).

**What a landed row leaves picked is a *value*, never a code.** `G71 U0 R0 X0 Z0
F0 P0 Q0` walks into `U0`; a tool row (`T1 R0.8`) into the tool number,
which is what the digits type over there; a lone letter that is waiting for its
number (`G` before the operator types `71`, `T`, and the joining `F`/`C`/`N`)
into itself. A row that is **nothing but its command** - `G41`, `G40`, `G42`,
`M2`, `M6`, `G80` - leaves nothing picked at all, so `B` and `C` stay the
cursor's keys (bench, 2026-09-28: *"if i am over g41 - pressing b/c tries to edit
it"*). nc's own rule was stronger still: a row it wrote left nothing picked, and
only the word a *join* had put on the line stayed selected
(`nc_insert_preset_id()`).

The file is at the card root so the panel's own file list and editor can reach
it: **the editor is the preset editor**. Editing it and rebooting is how an
entry is changed; a card with no file is a card with the shipped entries.

What this is not: an entry cannot be *deleted* from the card file (the shipped
table is always there), and the file cannot express "rows are exactly one empty
line" (the shipped `[45] PATH` already is that - nc had the same ability at
`[11]`). Both were traded for having no folder, no seeding pass and no card
access while the operator works.

### The layout: two shapes, one set of functions

The renderer owns the turn (`lvds_hstx.h`), so a screen draws in
`LVDS_VIEW_WIDTH x LVDS_VIEW_HEIGHT`: 600x800 when the panel is mounted turned,
800x600 when it is not. `ncc_layout.h` branches on **that shape** and nothing
else - the header, the drawing, the strip, the text pane, the notes and the 3x3
are one set of functions for both, and only the numbers move.

| | portrait (600x800) | landscape (800x600) |
| --- | --- | --- |
| drawing | full width, top half | full width, upper third |
| strip | centre line, risen 25 px | centre line |
| text pane | 342 wide, left of the pad | the width the pad leaves |
| 3x3 | 234x216, bottom-right | 234x192, bottom-right |

The machine's panel is mounted turned, so its build is portrait; adding
`-D LVDS_PANEL_TURN=0` to the env builds the landscape one (see the env's own
note). Both are compiled and checked on the host.

## The scaffolding: what the panel keeps for itself

The panel is a *client* of the core and of G7x, and the pieces it keeps for
itself are worth having in one list - what is a bridge the panel must own, what
is an owner's answer borrowed, and what is the panel guessing at something the
core already knows. (nc and nc2 keep the same pieces; the sizes are ncc's, and
they are what a clean-up would win back.)

| what the panel keeps | where | size | who should own it |
| --- | --- | --- | --- |
| the sender's `N` in front of every block it hands over, and the ring that maps the controller's `Ln` back to the line of the program | `../nc_sender/nc_sender_run.c` | ~60 lines | **the sender, and it must**: one line of the program is many blocks of the stream, which only it knows. The core's own text-line counter (`GCODE_COUNT_TEXT_LINES`) would avoid the numbers on the wire but drifts with anything else that feeds the machine |
| the mark's fallback: the unit in play, its first and last line, and "a unit the run has passed does not take the mark back" | `../nc_sender/nc_sender_run.c` | ~25 lines | **the core**, and it mostly does now: the mark is `itp_get_rt_line_number()` mapped back, and this is only what the pane shows *before* the first block is executing. First candidate to delete |
| the word the strip carries (`RUN`, `HOLD`, `ALARM`, ...) | `ncc_visual.c` | ~30 lines | **the core**, and it does: `cnc_get_status()`. The bits the panel picked for itself were the scaffolding, and they lied (bench, 2026-09-28 - the strip said RUN with the machine idle). Gone |
| the strip's fault sentence, and the text of the block underneath it | `ncc_visual.c`, `../nc_sender/nc_sender_run.c` | ~35 lines | the core gives the code and the line, the sentence is the panel's words (`nc_feedback_error()`'s table). The refused text earned its place by finding the float bug, so it stays |
| which lines the controller may see at all (`G0`-`G3`, `G7`/`G8`, `M`, `S`, `T`, and the panel's own `G970`-`G973` setup lines kept out) | `../nc_sender/nc_sender.c` | ~40 lines | **the sender**, and it must: `G970`-`G973` are this dialect's setup lines and the controller has never heard of them |
| the unit a line belongs to (the pale block, and what a step hands over whole) | `../nc_sender/nc_sender_run.c` | 8 lines | G7x owns it (`g7x_doc_block_containing()`); the panel only asks |
| the program-to-block sender and pacer | `../nc_sender/` | 1,892 lines including its headers | **`nc_sender`**: generic program lines in, controller blocks and source-line metadata out, then one unit at a time to the machine |
| one exported function the panel no longer calls: `ncc_file_new_backspace()` | `ncc_files.c` | ~5 lines | **the file picker**: it should be *wired* rather than deleted - a new file's name can be typed but not corrected (`*` cancels) |

What is left of it is tracked in `TODO.md` - the two deletions above, and the
pad's keys getting pictures of their own, which needs the splash's image
machinery moved into `lvds_renderer` (see that module's README, "Public
Boundary").

## What stayed behind

Everything nc2 dropped is still dropped - the footer strip, the per-screen key
tables, `nc_feedback.c`, the floating helper - and three more things went with
them:

| gone | because |
| --- | --- |
| `ncc_boot.c` (the first-start writer and its logo, 173 lines) | nothing is seeded onto the card any more |
| the folder of one-file-per-address entries | one table in RAM answers every address |
| the per-address reader/writer | the one file is read once |

## Files

| file | what it owns |
| --- | --- |
| `ncc.c` | the document: words, fields, the value editor's keys, the pad's helper and its label line |
| `ncc_presets.c` | the shipped table, the RAM copy, the one card file, the address walk |
| `ncc_files.c` | the card: listing, load, save, new, delete |
| `../nc_sender/nc_sender.c` | the shared sender: program text, G7x feeding, `U`/`W`, source-line metadata |
| `ncc_manual.c` | MANUAL: the jog, the stops, the spindle, zero and touch-off |
| `ncc_state.c` | what the panel remembers between boots |
| `ncc_tools.c` | the tool table |
| `ncc_boot.c` | the picture the panel starts on, and the build stamp it carries |
| `ncc_boot_image.c` | the picture itself: generated from `assets/logo.bmp` by `tools/boot_image.py` |
| `ncc_vocab.c` | the word legends, and the G codes: name, words, and the line each is written as |
| `ncc_preview.c` | the drawing, the DIN state, the live stock mask |
| `ncc_draw.c` | the panel's pixel vocabulary: palette roles, text/fill helpers and key face |
| `../nc_sender/nc_sender_run.c` | run pacing, queue ownership, hold/stop and execution-line mapping |
| `ncc_visual.c` | the controller: init, the document, screen selection, input, the pad's state, and status |
| `ncc_view.c` | the framebuffer view: the bands, rows, notes, DRO, frame-invalidation keys, and draw/present |
| `ncc_visual_internal.h` | the read-only boundary between the controller and the view |
| `ncc_layout.h` | the two shapes' geometry |
| `ncc_module.c` | the firmware entry (keypad, renderer boot, main-loop hook) |

The controller/view split follows the repository's size trigger. The view asks
the controller only for the document, screen/list state, status/refusal text,
the active pad labels and the run line. It does not receive writable panel
state. `tools/test_ncc.py` fails if either file - or any other handwritten `ncc`
source - crosses 2,000 lines again.

The program-to-block sender and its pacer are one owner in `nc_sender`. That
module reads generic `g7x_doc_t` lines, returns controller blocks and owns their
hand-over timing. The state-free pixel vocabulary stays in `ncc`;
`test_ncc.py` checks both the per-file and module totals.

## The build switch

Three panels, one env each, and only one at a time:

| env | panel | flag |
| --- | --- | --- |
| `RP2350-LEANCAM-LVDS` | nc2 | `ENABLE_NC_MODULE` |
| `RP2350-LEANCAM-LVDS-NCC` | ncc | `ENABLE_NCC_MODULE` |
| `RP2350-G7X-MODULE` | none (G7x only) | - |

`module.c` and `cnc_hal_overrides.h` each pick the load line from those flags.
`nc` and `nc2` stay in the tree either way - `nc` is still the reference
`tools/test_g7x.py` compiles against, and nc2 is still the panel the default
target runs.

## Verification

`python tools\test_ncc.py` is the check. It builds the firmware for the board in
both shapes; builds `tools/ncc_ui_win/` - nc2's station forked for this module -
and runs its 39 checks, the ones nc2's suite had with the folder-and-seed checks
rewritten for RAM; and runs `ncc/tests/ncc_host_test.c` in both shapes for what
a frame cannot show (the card file's rules, the dumped file read back as the
table, the layout's arithmetic). `ncc/TESTING.md` lists every flag, and
`python tools\test_ncc.py --brush2test` (or any other station flag) builds the
station alone and runs just the checks named.

The bench's own list, still open: does a pad label come up without the card
stall, does `#` switch the drawing while a program is open, does RUN show the
stock while cutting and the path when parked, and does the one card file survive
a real FAT card.
## The cut: the arithmetic

Four frames meet in the RUN pane, and most of the faults in this area came from
converting between them in more than one place:

- **the program's frame**: what the document writes. X is a **diameter** in
  `G7` (the lathe's default) and a **radius** in `G8`; Z is the length, in
  millimetres.
- **the machine's frame**: the axis position the runtime reports. X is a
  **radius**, and the work offset (`G54...` plus `G92`) is still in it.
- **the drawing's frame**: X is always a **diameter**, so the stock, the rows
  and the tool's `R` have one meaning in every mode.
- **the mask's frame**: the stock's own rows and columns in PSRAM, row 0 at the
  stock's top and column 0 at its left - the drawing's frame without the stock's
  origin.

The conversions, stated once:

```
drawing_x   = G7 ? program_x : program_x * 2
drawing_x   = (machine_axis_x - work_x) * 2
drawing_z   = machine_axis_z - work_z
row(x)      = drawing_x / stock_x * stock_h
col(z)      = (stock_z + z) / visible_z * stock_w
r_px        = R * 2 * stock_h / stock_x                // R in mm, a radius
pane        = (stock_left, stock_top) + mask
mask        = pane - (stock_left, stock_top)
```

`G971`'s `X` and `I` are the stock's own diameters and stay so in both modes;
`G8` changes how the **program's** X words are read, not how the stock is
defined. The sender's cycle expansion is still a diameter path
(`g7x_stream_add_line()` halves X for the contour it collects), so a `G8`
program with a `G71`/`G72` range needs a sender-side change; the drawing and
plain `G0`/`G1` moves honour `G8` today.

`ncc_frame_line()` walks the document's `G7`/`G8` words as it reads it;
`ncc_frame_machine()` reads the active coordinate system and `G92` from the
core. `ncc_stock_row()` / `ncc_stock_col()` are the only conversion to the
stock's pixels, and `ncc_pane_x()` / `ncc_pane_y()` are the only way between
those and the pane. The cut and the drawing go through them - the brush is
called with mask coordinates and the emitted path with pane pixels.

**The disc.** The nose's clearing bound is the upper half of a circle of radius
`r_px` around the machine's point `(sx, sy)`:

```
dx in [-r, r]:  h = sqrt(r^2 - dx^2)
                top(dx) = sy - h
remove the column [sx + dx, top .. bottom of the mask]
```

One rule, both modes. With `G41`/`G42` the machine's point is the tool's
**centre**, so the tangent is one radius above it and lands on the programmed
row. With `G40` the point is the tool **corner**, so the tangent is one radius
above the corner: `T1 R3` programmed to X20 leaves the material at **X14**, one
nose diameter less. `--brush2test` reads the glass for `G41` (X20), `G40` (X14)
and `G40` with a `G54` offset (X14). An attempt to make the G40 edge read X20 by
moving this bound down by `r` cleared one radius too deep on the bench and was
taken back. The nose's circle is drawn at the machine's point, the same circle
the brush clears; there is no body and no second offset anywhere.
