# The uCNC programming station - how to use it

This is the lathe's own NC panel running on a PC. The left 800x600 is exactly
what the machine's screen shows - same layout, same fonts, same keys, same
cycles - so what you learn here is what you do at the machine. The strip on the
right is the desk's: the modes, what the screen you are looking at is for, the
machine's keypad, the keys your PC keyboard adds, and the spindle.

Today **RUN drives the built-in virtual machine**, not a real controller: the
program is parsed, planned and executed, so a wrong move shows up here before it
costs a part. Sending a program to a real Grbl-style lathe is what
`tools/nc_sender` is for.

## Getting started

1. Unpack `uCNC-programming-station-win64.zip` anywhere (a folder on the desktop
   is fine). Windows 10/11 64-bit, no installer and no runtime: the exe is
   statically linked.
2. Run `uCNC-programming-station.exe`.
3. It keeps its card in an `nc-files` folder **beside the exe**, and makes one on
   the first start. If that card has no program of its own yet, the station seeds
   it from the `examples\` folder next to it, so the first start opens with
   `lathe-demo.nc` in the file list instead of an empty one.
4. The window title says which build you are running. So does
   `uCNC-programming-station.exe --version`:

   ```text
   nc_ui: uCNC programming station (PC), built 2026-09-25 00:31:22, 643793 bytes
   ```

   Those are the figures Explorer shows under Properties. If they are older than
   the station you meant to run, you are looking at a copy.

Use `--files DIR` to keep the card somewhere else (a USB stick, a project
folder), and nothing else on the PC is touched.

## The window

**The panel (left)** is the machine: the mode strip across the top, the DRO
band under it with the work position, the machine figures, F and S and the
controller's state in the corner, the body of the current screen, and the soft
key strip along the bottom. The keys at the bottom are the machine's own - the
strip beside the panel labels them from the screen you are on.

**The strip (right)** is not a second menu:

- `F1`-`F4` and `MODE` jump between MANUAL, EDIT, TOOLS and RUN;
- **screen** names the screen and says in its own words what it is for and how
  its keys drive it. The keys the bottom strip does not name are named here;
- **machine keypad** is the 4x4 keypad the machine has in hardware. A key the
  bottom strip carries is labelled in green; a key the screen has but the strip
  does not name is grey; a key that steps a field or the axis is drawn with the
  arrow it acts as. A key that means nothing here stays blank;
- **spindle** is what the tool is actually being told (speed and direction), not
  what a program asked for;
- **PC keyboard** lists what the keyboard adds.

## The keys

The machine keypad, row 1 on top:

| | | | |
| --- | --- | --- | --- |
| `*` | `0` | `#` | `D` |
| `1` | `2` | `3` | `C` |
| `4` | `5` | `6` | `B` |
| `7` | `8` | `9` | `A` |

Every one of them means what the screen says it means at that moment: the
labelled meaning under each key on the strip is the live one. The letters keep
four jobs across the screens: `A` leaves a field / cycles the screen, `B`/`C`
step a field or pick the axis, `D` accepts, `*` deletes or goes back.

The PC keyboard adds:

| Key | Does |
| --- | --- |
| `F1`-`F4`, or the `MODE` button | the four operation modes |
| digits, numeric pad | the keypad's digits |
| arrows | word by word on a code screen; steps the field / picks the axis where the screen has fields |
| `Enter` | `D` - accept |
| `Esc` | `A` - cancel / leave |
| `Backspace` | `*` - delete back |
| `W` or `Del` | `#` - finish / VIEW |
| `-` `.` | sign and decimal point |

`W` carries `#` because `#` needs Shift+3 on most layouts and AltGr on the rest.
No other letter is taken: a letter that is not `A`-`D` or `W` types nothing, so
program text is written with the keypad and the on-screen 3x3 pad.

## The screens

### MANUAL - the machine by hand

Digits jog: `2`/`8` are X-/X+ (up is the smaller diameter), `4`/`6` are Z-/Z+,
`7`/`9` start the spindle CCW/CW, `5` stops it. `1`/`3` pick the step or the
feed - the value beside the pad shows which. `#` swaps step for feed; while a
feed is on, holding a direction key feeds until you let go.

`B`/`C` (or the arrows) pick the axis the readout and the offsets act on. `0`
zeroes that axis, `D` opens a touch-off value.

`*` is the stop: each axis has a minus and a plus limit, and a jog or feed never
crosses the one it is headed for. `*` opens the minus stop, digits type it,
`B` is the sign and `C` the point, `*` again takes it and opens the plus one;
`D` puts the axis limit the setup states in the field. A stop you never type is
the machine's own travel limit - an axis always has a wall.

### EDIT - the program

The program is text. The arrows move the cursor by word, digits type a value
into the selected word, `*` deletes a line (or backs a field up), `#` shows the
whole-screen view of what you have written, and `0` opens the file list.

`B`/`C` step between equal words - field by field, the way the on-screen 3x3
helper works. The digit keys open the helper: `1 OPS` (the stock and setup
rows), `2 TOOL`, `3 WORD` (one line by its name or number), `4 G7X` (the lathe
cycles - inside that pad, `7` walks a profile, see below), `5 THREAD`,
`6 PECK`.

What those entries insert is not burned in: it is the card's own files, which
you can open and edit like any text file. The section below is what they are.

### TOOLS - the tool table

One tool per line, the fields the table draws. `1` adds, `7` inserts, `*`
deletes, `8` opens the file list. The table is written to the card by the same
idle task that writes the program, so leaving the screen saves it. `Tn` in a
program picks the tool; the tip the preview draws comes from this table.

### RUN - the program at the machine

Before anything else, RUN shows the program the way the sender walks it. `1
SINGLE` runs one block, `2 FROM` runs from the cursor line, `3 FULL` runs the
whole program. `4 HOLD` pauses and resumes, `5 STOP` stops, `#` reloads the file
from the card and resets the run, `6 DIM` dims the trace.

The line in play is marked bright on the pane and the cycle it belongs to is
pale, so the block being cut is visible while the sender waits for the machine.
One block at a time - the run only moves on when the machine has finished the
last one.

### The file list

`0` opens it on EDIT, TOOLS and RUN. `B`/`C` or the arrows step the list, `4` or
`D` opens the file, `5` makes a new one, `6` deletes, `8` refreshes, `#` runs it
from this screen, `*` goes back. Text files are listed beside the programs - the
entry files in `presets\` are text files - they open in the editor, but only
program extensions are read as G-code, so a text file gets no preview and no RUN.

### The view (whole screen)

`#` on EDIT hands the whole body to the preview: the part as the program cuts
it, with the stock, the trace of the path and the roughing passes as layers -
`4 STOCK`, `5 TRACE`, `6 ROUGH`, `7 DIM`. `#` brings the code back. The layers
are switches, and they are not saved: a reboot starts with the default view.
On RUN the pane carries the same drawing beside the code, where `6 DIM` dims
the trace.

## Writing a program

The demo `lathe-demo.nc` is a complete, runnable example: open it and walk it.
It shows the whole flow the station expects:

```text
G970 X-5 U60 Z-60 W5     (preview setup: what the screen draws, not motion)
G971 X50 Z50 I0 E0       (the stock)
G973 P7                  (preview layers)
T2                       (the tool, from the tool table)
M3 S450                  (spindle on)
G0 X52 Z2                (a rapid to the clearance)
G71 U3 R1 X1 Z1 F500 P50 Q55   (rough the range P50..Q55 in Z)
N50 G1 X30 Z2
G1 X30 Z-15 C2           (a chamfer)
G1 X35 Z-15
G1 X35 Z-25
N55 G1 X52 Z-25
G70 P50 Q55              (finish that range)
M5                       (spindle off)
```

Worth knowing before you write your own:

- **Lines are short.** The panel wraps a line longer than 46 characters onto a
  second row (a tab marks the continuation). It works, but a program written to
  that width reads better on the machine's screen.
- **A cycle needs a closed profile.** `G71`/`G72` with `P`/`Q` takes the rows
  between the `N` numbers as the contour; the numbers must exist, rise, and stay
  inside the range. A range that is missing or ambiguous is refused loudly, never
  guessed.
- **A cycle header carries its own values**: `U` (or `W`, for a facing cycle) is
  the depth of cut, `R` the retract, and `X`/`Z` the finish allowance the
  roughing leaves. The roughing stops on the boundary, so the finish cut takes
  exactly what was left.
- **Rounds and chamfers** (`R`, `C`) are cut to the number written or refused -
  a radius too big for the moves beside it is an error, not a smaller corner.
- **`G70 P Q`** re-runs the range the run already collected - the run has to have
  seen it, so starting mid-program at a `G70` is refused instead of guessed.
- **A move can be written as a distance** instead of a position: on a contour
  row `U` is X and `W` is Z, counted from where the tool is. See below.
- **Threading** (`G33`/`G76`) needs spindle synchronisation. The station can
  write and expand it, but the spindle phase and pitch can only be proven on the
  machine.

### Moves written as distances: `U` and `W`

`U` and `W` are Fanuc's incremental X and Z. On a row that moves - `G0`, `G1`,
`G2`, `G3` - they are the distance from where the tool is, whichever distance
mode is active:

```text
G0 X50 Z0      (absolute: where the tool is)
G1 W-10 F0.2   (Z 10 mm towards the chuck)
G1 U-5         (X 5 mm smaller)
```

That is the same part as writing the two positions out (`G1 Z-10` then
`G1 X45`), and the panel treats it that way everywhere: the drawing, the
dimension callouts, the cycle expansion and what is sent to the machine all read
the one rule, and the controller is handed the ordinary absolute line - it never
sees a `U` or a `W` from a move. The program keeps the spelling you typed.

- **`3 WORD` then `4` or `5`** appends the word to the row under the cursor with
  the value left picked, so the distance is typed straight in.
- **An axis has to be given absolutely before it can be incremented.** A program
  that moves by `U` or `W` before it has said where the tool is is refused by
  name - the panel will not invent a starting point - and so is a cycle whose
  range is not there.
- **On any other line the same letters mean what that line means**: in
  `G71 U3 R1 X1 Z1` the `U` is the depth of cut (`U` for the X side, `W` for the
  face), and in the demo's `G970 X-5 U60 Z-60 W5` they are the preview's X
  bounds, the smallest and largest diameter it draws. A row that moves is
  resolved; a line that carries parameters is not.
- Both spellings are one profile: `G1 X30 Z0` / `G1 Z-15` / `G1 X50 Z-15` and
  `G1 X30 Z0` / `G1 W-15` / `G1 U20` draw the same part and expand to the same
  motion.

### Walking a profile: `4 G7X`, then `7`

The pad can write the contour for you, one row per press. `4` `7` turns the
three-by-three into the profile pad - `2`/`8` move X, `4`/`6` move Z, the
corners move both at once - and it stays up until `5` ends it:

```text
4 7      open it on the line the profile continues from
2 4 6    one G1 per press, the axis that does not move is carried over
digits   while the value is picked, type the number you actually want
D        take it (and step on to the other axis of a corner)
#        step the distance (0.5/1/2/5/10/20/50 mm)
*        take the point back (the panel's own delete key)
5        end the contour
```

- **Each press is one line**, written below the cursor: the axis that moves at
  the step, the other carried over from the point the row above reached, and the
  value just written left picked so you can type the real number over it.
- **The digits belong to the value while it is picked**, so a dimension is typed
  the one way this panel types values; `D` takes it and gives the pad its digits
  back. `#` accepts it too - and only then does `#` step the distance.
- **The point comes from the program**, not from a memory of the pad: a `G0`
  before the cycle is what the first press counts from, and after that each row
  is what the next one continues from. If the program has no position yet, the
  first row starts at the stock's corner from the `G971` setup.
- **It is a contour builder anywhere**, not only inside a cycle: walk a profile
  with `4` `7` and it is an ordinary list of `G1` rows the machine cuts. Inside
  a `G71` block the rows are that cycle's profile - insert the cycle first (`4`
  `1`..`3`), set its `P`/`Q` range (`4` `4`, `4` `5`) and walk it.
- What it does *not* do: no header, no end mark, no undoing a whole session.
  The cycle template and the `G80` come from their own keys, and `*` takes back
  one row at a time. That is the whole pad - about a hundred lines, against the
  seven-hundred-line builder it replaced.

## The preset entries - the words the screens insert

Every helper entry that **writes text into the program** is the card's, not the
panel's: the blank line, the setup block, the tool change and the spindle words,
the chamfer and round, the cycle templates, the end mark, the thread and peck
entries. The key that inserts an entry keeps its meaning; what it *writes* is
the card's. That is how the panel is made to speak the words your shop uses - a
`G71` header with your usual allowances, an `M3` at your usual speed, a
separator you keep needing.

**An entry is an address and, at that address, two things: a name and the rows
it writes.** The address is the key path that inserts it - `42` is G7X `4` then
`2`. The card spells it as a file per address, in `nc-files\presets\`:

```text
nc-files\presets\41.txt        "4 G7X then 1" - the OD cycle

    OD ROUGH                   first row: the name the key reads as
    G71 U2 R1 X0.5 Z0.5 F500   every row after it: what the key writes
    G1 X30 Z0
```

There is no format to learn and no separate editor: `presets\41.txt` is an
ordinary text file. Edit it on the PC, or on the screen - `0` opens the file
list on EDIT, TOOLS and RUN, browse into `presets`, and a file opens, edits and
saves like any other text file. Leave the first row empty and the key keeps the
name it shipped with.

- **A row that starts with a space continues the row above** instead of starting
  a new one - that is how a value joins a line already written without the
  controller seeing a line break inside a block:

  ```text
  ROW
  G1 X0 Z0
   C0
   R0
  ```

  writes the one line `G1 X0 Z0 C0 R0`, with the word just written left picked so
  the number is typed straight into it.
- **The name may be empty; the rows may not.** A file with a name and nothing
  else is not an entry, so the address behaves as if the file were not there.
- Keep the rows short: the panel draws one row at 46 characters, and a longer
  row shows as two with a tab between them.
- A card with no `presets` folder, or no file for an address, uses the entries
  the panel ships with - the folder only ever *replaces* what it names.
- Every entry the panel ships is already in that folder when the station seeds
  your card (see *The card* below), so the table above is also a list of files
  you can open: `presets\34.txt` and `presets\35.txt` are the two increments.

**A word the pads do not offer** is added the same way, and there is no limit to
how many: drop a file at an address whose slot is free on the pad you want it on
- OPS `12`-`15` and `17`-`19`, TOOL `27`-`29`, WORD `36`-`39`, G7X `49`,
THREAD `54`-`59`, PECK `64`-`69`. So `presets\12.txt` reading

```text
COOLANT
M8
```

puts a `COOLANT` word on OPS `2`. (The addresses run `10`-`69`: one digit for
the pad, one for the slot. Anything named outside that is just a text file.)

The entries you are most likely to edit:

| Address | Entry | Pressed as |
| --- | --- | --- |
| `11` | a new line (blank by default - a separator or a command goes here) | `1 OPS` then `1 INS` |
| `16` | setup - the stock and preview rows (`G970`-`G973`) | `1 OPS` then `6 SETUP` |
| `23`-`26` | tool change (`M6`), spindle on (`M3`), stop (`M5`), reverse (`M4`) | `2 TOOL` then `3`-`6` |
| `32` `33` | chamfer (` C0`) and round (` R0`) on the cursor's row, inline | `3 WORD` then `2` / `3` |
| `34` `35` | `U` and `W`, the X and Z increments, on the cursor's row | `3 WORD` then `4` / `5` |
| `41` `42` `43` | OD, ID and face cycle templates | `4 G7X` then `1`, `2`, `3` |
| `46` | the end mark (`G80`) | `4 G7X` then `6` |
| `48` | the finish cut (`G70 P Q`) | `4 G7X` then `8 FINISH` |
| `51`-`53` | thread OD, thread ID, tap | `5 THREAD` then `1`-`3` |
| `61`-`63` | drill, peck, dwell | `6 PECK` then `1`-`3` |

Not every key is an entry, and that line is deliberate: **what a key means is the
panel's; what an entry writes into the program is the card's.** The keys that
*edit the line* rather than insert text (`4 G7X`'s `4 Q` and `5 N`, and its `7`,
which walks a profile a row at a time), the ones that take a typed value (`1
SELECT` opens the `T` field), and the ones that run an action (the file list,
delete, the tool table, saving) are not entries and cannot be redefined from the
card.

## The card

The card is a folder on the PC:

```text
nc-files\
  nc\files\      your programs (.nc), tool tables (.t) and text files
  presets\       one file per entry, named after its key path (the words
                 the screens insert - see above)
  nc_state.txt   what the panel remembers: mode, the file you had open, stops,
                 the spindle speed, the jog values
```

Copy the folder to back it up or move it to another PC; `--files DIR` points the
station at another one. The demo is only ever copied in once, into a card that
has no program of its own - your file is never overwritten by an upgrade.

The `presets` folder arrives **filled**: one file per entry, named after the key
path that inserts it (`34.txt` is WORD `4` = `U INC`, `41.txt` is G7X `1` = the
OD cycle), and they are seeded when the folder has no entry of its own - so a
card you already program also gets them, and nothing you wrote is overwritten.
Delete a file and that key goes back to the entry the panel ships with.

## Running the checks yourself

Everything in this guide that can be checked without the machine is checked by
one script, from the repository root:

```powershell
python tools\test_nc_ui.py        # builds the station and runs every check
```

It renders the panel, drives the keypad, runs a program on the virtual machine,
checks the demo expands as a cycle and compares frames where two paths must
agree. What it cannot check is the machine itself: spindle phase and pitch
through `G33`/`G76`, feed hold, Stop during queued motion, mounting the SD card
on a cold start and how readable the panel is. Those are bench items, listed in
`uCNC/src/modules/nc/TESTING.md` and `g7x/TESTING.md`.

## When something is wrong

| What you see | What to look at |
| --- | --- |
| The window looks like an older version | `--version` (or the title) against the file's Properties; a copy from an earlier build is the usual cause |
| The file list is empty | the card is somewhere else (`--files`), or you deleted the demo - `examples\` beside the exe is still there to copy from |
| A program will not open | only `.nc`, `.t` and text files open; anything else is refused |
| A move does not happen | a jog is only sent from a standing axis, and MANUAL refuses a step that would cross a stop |
| `Too many NC lines` with a text file | the file uses an ancient line ending (a lone carriage return); re-save it |
| Nothing reaches a real machine | RUN drives the virtual machine today - see `tools/nc_sender` for the Grbl path |
