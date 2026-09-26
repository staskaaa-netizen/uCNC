# nc2 - the NC screen, second iteration

## Why it exists

The bench, 2026-09-25: *"after some code review - it seems this whole nc drifted
from pure nc value editor to something too big to be nice. Visual side is ok to
agree yet it could be made simpler too."* `nc` is **14 758 lines** (13 870
without its host test) and 2 037 of those are one file. `nc2` is the same job
with a budget of **6 700**: the value editor, the entries, the sender and the
preview - and nothing else. The estimate after the bench's own corrections is
**~6 300** (see *What the preview will cost*).

It is a second module, not a refactor of the first: `nc` keeps driving the panel
until `nc2` answers everything `nc` does, and then the panel is switched over
once. No compatibility layer, no shared state, no half-migrated screen.

Two things move out of NC's line count before `nc2` is written, because they are
not NC's to own: the cycle/block scan goes to g7x (below), and the tool table's
screen is left where it is for now.

## What the operator sees

- The program, one word picked at a time.
- **One 3x3 pad, pinned in the bottom right.** It is the only menu there is: no
  footer strip and no submenu tables, and **no borders around the panes** - the
  one line between them is the whole of the layout's furniture (the bench: *"only
  middle line is need, not borders from all sides"*). The pad's keys keep their
  own outlines, because they are buttons. The pad keeps the helper's own trick -
  opening it writes its name as a line at the cursor, which is both the title
  and the place the entry lands (the bench kept this one: *"this one is not big
  and we may preserve it"*).
- A DRO that is **not on screen unless the machine is doing something**: in RUN
  it floats over the preview; idle, the pane has the whole body.
- **One band, and only at the top.** It carries the four screens - MANUAL,
  EDIT, TOOLS, RUN - with the one in play **wearing the block** its name is on
  (the panel's own selection colour, the way nc's tabs did), the file (the
  folder, while the card's list is up), and what the panel has to say at the
  right end. There is no footer and no status strip along the bottom: what a
  footer said is one line, and one line belongs where the operator is already
  looking (bench: *"i do not need any footer here. put messages into header"*).
  A mark drawn as a line under the name was an invention of the port and the
  bench caught it: *"marking manual tools run with line but not with full yellow
  background as it was before"*.
- **A drawing that is the part.** While the tool moves, the stock is drawn from
  what the tool has taken off (nc's live stock): the machine's own position,
  frame by frame, painted as the material still there. A run that has parked
  keeps the finished part on the RUN screen until the drawing is asked for
  something else; EDIT draws the stock whole again.
- **The word under the cursor is named**, on the row above it, in EDIT and
  TOOLS: `>  X position`, `>  Depth/pass`, from the same table nc kept
  (`nc2_vocab.c`). The pane's own top line is used when the cursor is on the
  first row - the row nc's legend sat on.

## The keys, all of them

| key | means |
| --- | --- |
| `A` | mode (the screens) / cancel - and, on the pad, one level up |
| `B` `C` | step: back and forward through what is picked |
| `D` | the next numeric field; on the last one, accept |
| `#` | insert the picked entry / accept the picked value |
| `*` | delete - the thing under the cursor |
| `0` | **the exit, always**: out of a pad in one press whatever level it is on, out of the file list, and at the root the card itself |
| `1`-`9` | the pad; with a value picked, they type into it |

`0` is the exit everywhere (the bench: *"0 should be an exit always"*): `A` steps
up one level, `0` leaves the whole pad - and only when there is nothing to leave
does it open the card, which is where the programs are.

**The pad's rows run up, as nc's did.** The 3x3 is the machine keypad's numeric
block, and that block's own rows are the other way up: `7 8 9` is the top row and
`1 2 3` the bottom one. It is one function (`nc2_pad_cell_key()`) that the drawing
and the checks both ask, because the bench has had to say this twice (*"3x3 is
swapped again. i have 1 on bottom left corner"*). What it buys: MANUAL's `X-` sits
above `X+`, matching the drawing's own sense of the axis (up is toward the
centre), and G7X's pad lands with `7`/`48`/`46` in the cells nc put them in.

**G7X's `7` is the path builder** - the address `47`, nc's contour pad. It is a
key that *does* something rather than one that writes a row of its own, which is
why it is not an entry a card can hold (nc's own reason). The pad becomes nine
directions and stays until `5`; each press writes one `G1` row below the cursor,
the axes that move at the step and the others carried over from the point the row
above reaches, which makes a profile one press per point and a chamfer one press.
The row that lands keeps its value picked, so the digits type the real number
over the step's starting point.

The machine has no `-` and `.` keys and `nc2` does not add any: a value that
needs a sign or a point is typed with the pad's own keys where the screen puts
them, or written on the card. The value editor is the dumb one the bench asked
for: **a line is cut into fields at its letters**, `D` walks the numeric ones,
digits type into the picked one, and nothing is checked while editing. Sanity
checking happens where it already does - the loader, G7x and the sender.

## The entries: one file per address

**Decided: a folder of files, one per address - not one big file.** The single
file was tried and removed (`docs/nc-preset-file.md`, "What this replaced"): it
needed a parser, an alias table and a record table, and the bench's words for it
were *"good and smelly scaffolding"*. A folder needs **no format at all** - the
file name is the address, the first row is the name, the rest are the rows the
key writes - and the panel's own file list and editor *are* the entry editor.

An address is the key path: `3` then `4` is `presets\34.txt`. A level deeper is
one more digit (`presets\341.txt`), which is how the pad can hold more than nine
things without a second mechanism.

## What is not carried over

| dropped | lines today |
| --- | --- |
| the footer strips, the submenu tables, every per-screen key table (`nc_menu.c`) | 195 |
| the per-screen usage/label tables, the strip's toggle logic (part of `nc_visual.c`) | ~700 |
| `nc_feedback.c`, and the stub keys (`2 TOOL` -> `2 EDIT` sets a status line and returns - `nc_visual.c:697`) | 35 + 20 |

**Kept, against it:** the preview's `DIN` layer, whole - *"Din must stay here too.
it is nice feature ... then do it as is"* - so that row is not a saving either.

**Kept, against that list:** the pad's helper and the label line it writes into
the document. It is small, and it is two things at once - the pad's name where
the operator is looking, and the line the entry lands on - so it is not counted
as a saving (the bench: *"this one is not big and we may preserve it, does not
count its lines up here"*). Its own ~450 lines are why `nc2.c`'s target below is
larger than a bare editor would need.

The six keys `nc` hardcodes inside pads are not lost by dropping them:
`21` SELECT, `31` the `G` field, `44` Q and `45` N are each *a lone letter* -
`T`, `G`, ` Q`, ` N` - which a file can write exactly like any other entry, and
the editor's "a letter with no number is a field waiting for a number" rule does
the rest. That leaves the contour pad (`47`) as the only key that computes
rather than writes - and it is files too (see *Settled from the bench*, 4).

## The layout, and what it is worth in lines

**The layout is nc's, unchanged:** the code pane on the left, the drawing on the
right, the same split (`NC2_LEFT_PANE_X` and `NC2_SPLIT_X` are nc's numbers), and
the pad pinned in the bottom-right corner of the drawing's pane. What is gone is
the footer strip and the DRO band - the pad is the keys and the machine's numbers
wait for the run - so the two panes are taller by exactly what those bands took.
The bench was plain about this: *"no do not change it. only 3x3 in place corner,
same dual screen as before and no footer or dro."*

The bench's own estimate of what that is worth, after the first three pieces
landed: *"we will have less line of code, but not by that much; my guesstimate is
around 15-16 will fit easy."* So this is **not a size exercise** - the board's
flash has room, and the point is fewer *things*, not fewer lines. The table below
stays as a shape to aim at and a way to notice a file growing past its job, not
as a target to squeeze into.

## Files

| file | working figure | what it owns |
| --- | --- | --- |
| `nc2.c` | 950 | the document: words, fields, the value editor's keys, the pad's helper and its label line |
| `nc2_presets.c` | 450 | addresses -> files: names, rows, the pad's tree |
| `nc2_files.c` | 350 | the card: listing, load, save, new, delete |
| `nc2_emit.c` | 700 | the sender: stream, G7x feeding, `U`/`W` |
| `nc2_run.c` | 600 | the run: pacing, hold/stop, the panel's own blocks |
| `nc2_manual.c` | 700 | MANUAL: the jog, the stops, the spindle, zero and touch-off |
| `nc2_state.c` | 380 | what the panel remembers between boots |
| `nc2_tools.c` | 250 | the tool table |
| `nc2_text.c` | 300 | typing into the picked word |
| `nc2_vocab.c` | 220 | the G-code templates and the legends |
| `nc2_visual.c` | 850 | the screen: code pane, the 3x3, the floating DRO |
| `nc2_preview.c` | 1 100 | stock, the live stock's mask, contour, dimensions |
| `nc2_draw.c` | 450 | primitives, glyphs, the 3x3 grid |
| headers, `nc2_layout.h` | 350 | the boundary and the shared numbers |
| **total** | **~7 000** | the block scan is g7x's now, and the helper is kept - and neither figure is a ceiling |

Its own test target, `tools/test_nc2.py`, builds and runs the module against the
same virtual machine the station uses (AGENTS.md 7), and `nc2/TESTING.md` keeps
the bench items.

## What would stay hardcoded

Nothing yet - that is the point of this page. The list has to be exactly: the
named keys above, the address -> file rule, and whatever the open questions
settle. Anything else is a file.

## Settled from the bench (2026-09-25)

1. **No footer, and no second copy of the buttons.** The footer's items are the
   pad's items somewhere else on the screen (*"it is basically same buttons just
   different placement on most of screens"*), so the pad is the only menu and
   everything in it comes from files - *"if we left scaffolding it will grow
   back. so no - just files or one file."*
2. **No compiled fallback.** The entries are files, full stop. A card with no
   files has no entries; the release carries them (see the one question below).
3. **The tool table is a file like the others** and is edited with the same
   editor - it is rows, like every other entry. The TOOLS *screen* is a separate
   question and is isolated: it is left as it is for now rather than rewritten
   (*"spend on tools and menu a small amount of time ... if it can be isolated -
   leave it as is for now"*).
4. **The contour's directions are files too**, and the recursion is a rule
   rather than a mechanism: *a slot that has children is a pad, and writing from
   a slot that has children returns to it.* That is the whole trick - the pad's
   address is the digits walked, so a direction file (`478.txt` = `X-`,
   `G1 U-0.5`) writes its row and the pad is still there for the next point,
   while `A` steps up one level. It needs no marker row, no flag and no new kind
   of file, and `475.txt` = `END` is the way out.

   **Built that way, with one correction:** the pad does not close when an entry
   lands - it *stays*, whatever kind of slot was pressed. So there is no marker
   row, no flag and no `END` file: the walk continues because nothing ends it,
   and `A` is the way out (at the root, `A` is the mode key). The one thing this
   drops is the `5`-to-end the old pad had; see the note at the end of this
   page.

## The address tree

The digits pressed are the address, and the file with that name is the slot:

```
1.txt .. 9.txt        the groups (their names are the files' first rows)
11.txt .. 69.txt      the entries, as today
471.txt .. 479.txt    a level deeper, where a pad needs more than one screen
```

Each rule is one line: **a digit opens its slot - rows are written, children are
a pad; `A` goes up a level**, and at the root `A` is the mode key. The root
therefore holds the six groups `nc` gave footer keys to (plus three spare) with
nothing hardcoded about them but their files.

Two things the pad does that the files therefore describe rather than the code:
the pad writes its name as a line at the cursor while the operator is choosing
(the title and the place the entry lands), and the entry's rows land where that
name stood. A second press lands under the *last* row of the first entry, not
under the cursor - the cursor stays on the first row, which is the one to edit.

**Open, and one word from the bench decides it:** the old pad ended with `5`.
Here the pad ends with `A`, because `5` is a slot like any other - `25.txt` is
`M5`, `35.txt` is `W INC`, and a rule that swallowed the centre key would make
both unreachable. If `5` should end the pad instead, the smallest way is *an
empty centre ends the pad*: `5` closes it when the address has no `x5` file, and
the shipped `M5`/`W INC` keep working because their files are there. Say which
and it is five lines.

## `nc_g7x` moves to g7x, and nc2 has no scan at all

`nc_g7x.c` is 306 lines of "which rows are this cycle's profile" - block
start/end, the Fanuc two-line header pair, the range above a `G70`, and whether
a row is inside any contour. The *contract* for that is already g7x's
(`g7x_source.h`: *"G7x never reads files or NC documents itself. A caller that
owns program text supplies this cursor ... currently the NC preview"*), and
every G-code question inside it is asked of g7x - but g7x answers the same
question a second way, for the stream, and the two have drifted before (the
README records a block that answered with two different blocks for one line).

So the walk belongs in g7x as `g7x_blocks.c`, over a line-provider callback (the
one thing g7x must not do is read an NC document itself - the callback keeps
that true), and the sender, the preview and the mark all ask it. `nc2` then has
**no `nc2_g7x.c`**: it supplies text and asks g7x. That is ~300 lines leaving the
NC side and the same amount leaving nc2's budget, so its target is 6 700 with
300 of slack.

## The first start: a separate thing that writes the files

**Answered by the bench, and built:** a card with no entry file of its own gets
the entries the panel ships, written once, with a boot logo on the screen while
it happens - *"fallback - ok make it but as a separate class / thing with sort of
booting logo and writing default files. it should not be big."*

`nc2_boot.c` is that thing: one table (the groups and the words), one pass
writing them, one screen. It is a **writer, never a fallback** - nothing at run
time consults the table, so after the first start every entry is a file and
deleting one is how an address stops being an entry. A folder that already holds
an entry file is the operator's and is not touched at all.

`nc2_presets.c` is the rest of the coin: the address to file name rule, the
format's reader and its writer. `--seedtest` in the station checks all of it.

## What is built, and what is next

| | |
| --- | --- |
| built | `nc2_presets.c` (address -> file, read, write), `nc2_boot.c` (the shipped entries, the one-time seed, the logo), `nc2.c` (the document, the fields, the value editor's keys, the pad's helper and its label line), `--seedtest` and `--edit2test` |
| built (2) | the pad's tree: `nc2_address_*` and `nc2_slot()` in `nc2_presets.c`, `nc2_pad_open/write/close()` in `nc2.c`, `--pad2test` |
| built (3) | the screen: `nc2_draw.c` (colours, text, the 3x3) and `nc2_visual.c` (the program down the left, the pad's corner on the right, no footer), the program read and written back (`nc2_files.c`), `--screen2test`, `--dump-nc2` |
| built (4) | the two panes as nc has them (the same split, no footer, no DRO), and the card's file list: `0` opens `/D`, folders are entered, a program opens into the editor, `5` + digits + `#` makes a numbered one, `6` deletes, `8` re-reads, `*`/`0` come back - `--file2test` |
| built (5) | the cycle/block scan moved out of NC and into g7x (`g7x_blocks.c`, over a line provider), so nc2 will have **no scan file at all** - and the duplication the bench asked about is gone from nc too (`nc_g7x.c` is an adapter now) |
| built (6) | the sender: `nc2_emit.c` (the stream, the G7x feeding, the numbered range above a `G70`, `U`/`W` written out as the absolutes they mean) - `--emit2test` runs **both** modules' senders over the same programs and requires the same output, from the top and from a run started in the middle |
| built (7) | the preview: `nc2_preview.c` drew the stock, the chuck, the DIN rulers and callouts, the contour's point dimensions and the emitted path - the drawing pane is nc's picture, checked for ink and looked at against nc's own frame |
| built (8) | what the panel remembers: `nc2_state.c` reads and writes `/D/nc_state.txt` with nc's own keys (`MODE=`, `SPINDLE=`, one key per mode holding the file it had open), so a card carries its state across a reboot and across the panel switch - the cursor is the session's, the way nc keeps it. `nc2_visual_init()` loads the remembered program (an empty one on a card that remembers nothing) and `open`/`save` flush the path and cursor. `nc2_state_runtime()` reads the machine's own numbers, `nc2_state_busy()` answers whether it is doing anything |
| built (9) | the run: `nc2_run.c` is the pacer over `nc2_emit` - the panel hands the machine one unit and waits, so the mark is the line the tool is on - with `1 SINGLE`/`2 FROM`/`3 FULL`, `4 HOLD`, `5 STOP` and `#` reload, and the floating DRO that appears only while the machine is busy (work X/Z, feed, spindle, and the machine's own state word). The generator's own notes (`(G71 rough X23.000)`) are a line of the stream but not a line of the program, so they are not sent. `--run2test` reads what the run hands over, requires the machine to arrive where the program says and the DRO to be up only while it is busy |
| built (10) | MANUAL, its own module: `nc2_manual.c` is the machine panel - the pad is the jog keys (`2`/`8` X, `4`/`6` Z, `7`/`9` the spindle, `5` its stop, `1`/`3` the value the pane shows, `#` swaps a step for feeding, `*` types the two stops of the picked axis, `D` touches off, `0` zeroes, `B`/`C` pick the axis) - and the pane carries the stops with the axis limit the setup states dimmer behind them and the STEP or FEED value. A jog is the `G91 G1` pair with the `G90` that puts the machine back, a held key feeds toward the stop, and the spindle starts at the speed the machine has. `--manual2test` checks what each key sends, that the axis moves, and that the stops and the value are the ones on the glass |
| built (11) | TOOLS: the bench's "tool table is just as file as other ... basically just inserts" taken at its word, so the screen is the editor on `/D/nc/files/tool.t` - the same file `nc` wrote - with the shipped row as the table when the card has none, and the pad inserting into it like any program. A look at the tools does not lose the program's place. `--tools2test` checks the table is made, written, inserted into, and that the program is where it was |
| built (12) | the four checks nc's suite had that nc2 did not: `--block2test` (the marks read off the glass), `--label2test` (the DRO, and the state said once), `--pace2test` (one unit at a time), `--demo2test` (the demo card). Writing `--block2test` found a real gap - EDIT marked only the cursor's row - so the editor colours the block pale too, from the same g7x answer RUN uses |
| built (13) | the path builder, which the port had dropped: G7X's `7` (the address `47`) turns the pad into nc's nine contour directions and stays until `5`, one `G1` row per press with the axis that does not move carried over - and the pad's rows run up (`7 8 9` on top), as nc's did. `--contour2test` pins both. Two things it also fixed on the way: a picked value now takes `0` as a digit (`0` is the exit only when nothing is being typed), and the pad's order is one function both the drawing and the checks ask |
| built (14) | the card's own names: a FAT card read without long filenames gives `LATHE-~1.NC` - 8.3, capitals - so the extension question is case-insensitive now (`nc2_path_has_suffix()`, the same answer nc's `nc_has_suffix_ci()` gave). nc2 compared with strcmp and dropped every program the operator had: the list showed none of them and RUN would not open one. The preset scan asks the same question the same way, or a card in use gets seeded again on every boot. `--case2test` writes the shapes a real card has (`.NC`, `.TXT`, `41.TXT`) and insists they are offered. A remembered file that cannot be opened now says so instead of leaving an empty program |
| built (15) | the list survived the card: the entries the driver hands over are checked before they reach the glass (`nc2_name_is_usable()` - printable ASCII, not `.` or `/`, nc's `nc_files_valid_entry_name()`), the info block is cleared before every `fs_next_file()` (a driver that only appends to the name would grow a path out of the last entry - nc cleared it and nc2 did not), and `nc2_file_selected_path()` is a question again: `..` answers the folder above instead of scanning it and returning nothing, which had the caller scan an uninitialised path (`"it is stuck"`). The scan logs what it saw - `[MSG:NC2 list + name]` / `- name` for the first 20 entries and a summary line - because the list a card hands back is the one thing the panel cannot show the operator. `--walk2test` walks into a folder and back out |
| built (16) | the three things the port had invented or dropped, from one bench pass: the drawing's X is a **diameter** again (`nc_preview_map_x()`'s halving - the stock came out right and the profile twice its size), the screen in play wears the **block**, and the **legend** is back (`nc2_vocab.c`, nc's own table). `--vocab2test` insists every word the panel can write is named |
| built (17) | the live stock, the one thing the port had not carried over: while the tool moves, `nc2_preview.c` paints the material still there from the machine's own position, one sample per turn of the screen's loop, into a one-byte-per-pixel mask in PSRAM (nc's own offset, 512 KiB). The tool takes the material off from its X down to the axis, so what is left of a column still starts where the whole stock did; the stock's bore is not material; a run that parks keeps the part on RUN and EDIT draws the stock whole. `--live2test` reads the glass column by column: the cut is where nc's mask puts it, the tops are untouched, the parked screen holds the part, and the editor has the whole stock again |
| **switched over** | `nc` is retired: `module.c` loads `nc2`, `rp2350.ini` compiles `modules/nc2/`, and the station builds and drives nc2 (`tools/nc_ui_win/`, `tools/test_nc_ui.py`). nc's sources stay in the tree, unbuilt, as the record of the dialect nc2 replaces (`nc/TODO.md` says so at the top) |
| next | the drawing's tool panel and the tool glyph that rides the live stock (they need the tool table parsed, which is its own module later) |

One upstream landmine was found on the way, in `file_system.c`: `fs_opendir()`
writes into the string it is handed to drop a trailing `/` (`char *newpath =
(char *)path; newpath[len - 1] = 0;`), so a literal like `"/D/presets/"` faults on
read-only memory. `nc2` spells the folder without the slash for that one call and
says why in `nc2_presets.h`; the function itself is the core's to fix.

## What the preview will cost

The bench, on nc's `DIN` preview layer: *"Din must stay here too. it is nice
feature ... then do it as is and then we will see how it will look."* So the
preview is not the place to save lines: `nc2_preview.c` carries the stock, the
chuck, the dimension and ruler layer (DIN), the contour points, the generated
motion of the cycles, and the live stock - as nc has them - which is ~1 700 lines
and takes the estimate for the finished module from ~5 700 to **~6 300**. The
live stock's mask is one byte per pixel of the drawing's stock, held at the PSRAM
offset nc held it at (512 KiB, the live-sim region `docs` names); a pane that is
200 pixels wide and 100 tall costs 20 KB, and the module's ceiling of 680x380
is 258 KB.

The one part of nc's live layer not carried over is the **tool glyph** that rode
the stock: it is drawn from the tool table (`nc_tool_t`), and the tool table is
its own module's business before it is the preview's. Until then the DRO's X and
Z are the tool's position on the glass, which is what a run is followed by.

## The run, and the DRO

`nc2_run.c` is the pacer, and the boundary between it and `nc`'s `nc_run.c` is
one sentence: **`nc` sends the program as written and the machine's own G7x
parser expands the cycles; `nc2` expands them in the panel** (that is
`nc2_emit.c`, the layer `--emit2test` compares line for line against `nc`'s), so
what travels to the controller is plain motion. The pacer is the same
one-unit-at-a-time rule: it hands the machine one line and waits until the
planner and the interpolator are empty before handing over the next, *except*
while a block is still being expanded - a contour is one cut, and its lines go
out back to back rather than a stop per row. The mark is the line the operator
stepped from, which is the line in play, and the pacer keeps it there until the
operator takes the cursor with a line key.

One thing `nc` does not have to say out loud: the generator's expansion carries
its own notes - `(G71 rough X23.000)`, `(G7x finish contour)`. They are a line
of the *stream* but not a line of the *program*, so the pacer does not hand them
to the controller (it logs the skip) - the mistake that first stopped a run at
the header of a cycle.

The floating DRO is not a strip: it is drawn over the preview's top only while
the machine has something to say - a run, a jog, a hold, a fault
(`nc2_state_busy()` and the run's own state) - so a machine that is not moving
keeps the whole drawing. It wears the panel's green while it runs and its red for
a fault, and it carries the same state word (`uCNC RUN`, `HOLD`, `ALARM`) on
every screen, which is why the tab strip above no longer repeats it: the bench
asked for one place, and the DRO's corner is it.
