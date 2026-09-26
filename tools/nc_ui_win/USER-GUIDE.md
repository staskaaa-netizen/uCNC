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

**The panel (left)** is the machine. It has just three things:

- the band across the top: the four screens - MANUAL, EDIT, TOOLS, RUN - with
  the one you are on bright and underlined, the file it has open, and whatever
  the panel has to say at the right end (a run starting, a stop, a key you
  pressed, a fault). Everything the panel tells you appears there and nowhere
  else; there is no strip along the bottom;
- the body: the program down the left, the drawing of the part on the right;
- the 3x3 pad in the bottom-right corner of the drawing, which is the machine's
  own nine keys.

While the machine is moving - a run, a jog, a held feed, an alarm - a small DRO
floats over the top of the drawing with the work position, the feed, the spindle
and the controller's state word (`uCNC RUN`, `HOLD`, `JOG`, `ALARM`). It leaves
with the motion, so a machine standing still gives the whole drawing back. That
word is the only place the state is said.

**The strip (right)** is not a second menu:

- `F1`-`F4` jump between MANUAL, EDIT, TOOLS and RUN;
- **screen** names the screen and says in its own words what it is for and how
  its keys drive it. The keys the pad does not name are named here;
- **machine keypad** is the 4x4 keypad the machine has in hardware. The label
  under each key is what that key does on this screen right now, and a key that
  steps a field or the axis is drawn with the arrow it acts as. A key that means
  nothing here stays blank;
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

Every one of them means what the strip says under it at that moment. Four of
them keep the same job wherever you are:

| Key | On the screens |
| --- | --- |
| `A` | up a level in the pad; at the root, the mode key (MANUAL, EDIT, TOOLS, RUN) |
| `0` | out of whatever is up - the pad in one press - and, with nothing to leave, the card's file list |
| `B` / `C` | the line above / below, and the sign / decimal point while a value is being typed |
| `D` | the next field of the line |
| `#` | the type key: it accepts the field being typed |
| `*` | delete (a text character while typing, the line otherwise) |

The nine digits are the pad's own entries on EDIT and TOOLS, and the machine's
jog keys on MANUAL.

The PC keyboard adds:

| Key | Does |
| --- | --- |
| `F1`-`F4` | the four screens |
| digits, numeric pad | the keypad's digits |
| arrows | `Up`/`Down` are the machine's `B`/`C` (line, sign, point); `Right` is `D`, the field walk |
| `Enter` | `D` - the next field |
| `Esc` | `A` - up a level / cancel |
| `Backspace` | `*` - delete back |
| `W` or `Del` | `#` - the type key |

`W` carries `#` because `#` needs Shift+3 on most layouts and AltGr on the rest.
No other letter is taken: a letter that is not `A`-`D` or `W` types nothing, so
program text is written with the keypad and the on-screen 3x3 pad.

## The screens

### MANUAL - the machine by hand

Digits jog: `2`/`8` are X-/X+ (up is the smaller diameter), `4`/`6` are Z-/Z+,
`7`/`9` start the spindle CCW/CW, `5` stops it. `1`/`3` pick the step or the
feed - the pane shows which, and how big. `#` swaps step for feed; while a feed
is on, holding a direction key feeds until you let go.

`B`/`C` (or the arrows) pick the axis the readout and the offsets act on. `0`
zeroes that axis, `D` opens a touch-off value.

`*` is the stop: each axis has a minus and a plus limit, and a jog or feed never
crosses the one it is headed for. `*` opens the minus stop, digits type it, `*`
again takes it and opens the plus one; `D` puts the axis limit the setup states
in the field. A stop you never type is the machine's own travel limit - an axis
always has a wall.

### EDIT - the program

The program is text, one line at a time. `B`/`C` move the cursor by line, `D`
walks the fields of the line the cursor is on (an `X45.2` is the field `X` with
the value `45.2`), and the digits type into the field that is picked - the first
digit replaces what was there, `B` is the sign and `C` the point, because the
keypad has neither key. `#` accepts, `*` deletes the line, `0` opens the card.

The line in play is marked bright and the cycle it belongs to - the `G71` block
it sits in, or the range a `G70` finishes - is pale around it, so the block you
are in is visible as you read.

The pad is the card's entries, and it is a tree: `1`-`9` press what is at the
address you are standing at (`1 OPS`, `2 TOOL`, `3 WORD`, `4 G7X`, `5 THREAD`,
`6 PECK` at the root), `A` steps up a level, and pressing a group opens it.
Pressing an entry writes its rows into the program where the cursor is - with
the pad still up, so a profile is one press per point.

What those entries write is not burned in: it is the card's own files, which you
can open and edit like any text file. The section below is what they are.

### The path builder: `4` then `7`

Under G7X, `7` is the **PATH** key - and it is the address `47`, where a path
builder has always been. Pressing it turns the 3x3 into the nine directions and
the pad *stays* until `5` ends it:

```text
4 7      open it on the point the profile continues from
1..9     one G1 per press - the axes that move at the step, the rest carried
         over from the row above (the corners move both axes, so a chamfer,
         a taper or a radius lead-in is one press)
digits   while the value is picked, type the number you actually want
D        take it (and step on to the second word of a corner)
#        step the distance (0.5/1/2/5/10/20/50 mm)
*        take the point back (the row goes with it)
5        end the builder - the rows it wrote stay
```

- **Each press is one row**, written below the cursor, and the cursor sits on it:
  that is the whole of the "walk", because the next press reads its point from
  the row just written. The axis that does not move is carried over, the way a
  program written by hand reads.
- **The point comes from the program**, not from a memory of the pad: a `G0`
  before it is what the first press counts from, and if the program has not said
  where the tool is yet, the first point starts at the stock's corner (the
  `G971` diameter).
- **The cells are the directions**: with the pad's rows running up (the machine
  keypad's own block), the middle of the top row is `X-` and the middle of the
  bottom row is `X+`, so "up" on the pad is "toward the centre" on the part.
- **It is a contour builder anywhere**, not only inside a cycle: walk a profile
  and it is an ordinary list of `G1` rows the machine cuts. Inside a `G71` block
  the rows are that cycle's profile - insert the cycle first (`4` `1`..`3`), set
  its `P`/`Q` range, and walk it.
- `A` or `0` leaves it, and the rows stay: a builder is a view of the program,
  not a mode the program is in.

### TOOLS - the tool table

One tool per line, and it is the same editor: the table is the file
`nc-files\nc\files\tool.t`, opened and written like any other. `Tn` in a program
picks the tool.

### RUN - the program at the machine

`1 SINGLE` runs the block the mark is on, `2 FROM` runs from that line to the
end, `3 FULL` runs the whole program. `4 HOLD` pauses and resumes, `5 STOP`
stops, `#` reloads the file from the card and resets the run. `B`/`C` pick the
line while nothing is running.

The line in play is marked bright on the pane and the block it belongs to is
pale, so the code being cut is visible while the sender waits for the machine.
One block at a time - the run only moves on when the machine has finished the
last one.

### The file list

`0` opens it on EDIT, TOOLS and RUN. `B`/`C` step the list, `D` or `#` opens the
file (a folder is entered), `5` makes a new one (a number, then `#`), `6`
deletes, `8` refreshes, `0` goes back to the program. Text files are listed
beside the programs - the entry files in `presets\` are text files - they open
in the editor, but only program extensions are read as G-code, so a text file
gets no drawing and no RUN.

There is no whole-screen view: the drawing is always the right pane, and the
code the left one.
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
- A card with no `presets` folder, or no file for an address, simply has no
  entry there: the file *is* the entry, and deleting one is how an address stops
  being one. The panel writes the shipped set onto a card that has never seen an
  entry file - once, and never over yours.
- Every entry the panel ships is already in that folder when the station seeds
  your card (see *The card* below), so the table above is also a list of files
  you can open: `presets\34.txt` and `presets\35.txt` are the two increments.

**A word the pads do not offer** is added the same way, and there is no limit to
how many: drop a file at an address whose slot is free on the pad you want it on.
An address is one to three digits - one digit per level, nine slots at each - so
OPS has `11`-`19` under it, TOOL `21`-`29`, WORD `31`-`39`, G7X `41`-`49`,
THREAD `51`-`59` and PECK `61`-`69`. So `presets\12.txt` reading

```text
COOLANT
M8
```

puts a `COOLANT` word on OPS `2`. (The addresses run `10`-`69`: one digit for
the pad, one for the slot; a third digit is a level deeper still. A name that is
not one to three digits is just a text file.)

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
walk and edit the program rather than insert text (`A`, `0`, `B`/`C`, `D`, `#`,
`*`), and the ones that run an action (the file list, delete, the tool table,
saving) are not entries and cannot be redefined from the card. Every digit of the
pad is one, and a free slot is a slot waiting for a file.

## The card

The card is a folder on the PC:

```text
nc-files\
  nc\files\      your programs (.nc), tool tables (.t) and text files
  presets\       one file per entry, named after its key path (the words
                 the screens insert - see above)
  nc_state.txt   what the panel remembers: the screen you were on, the file it
                 had open, and the spindle speed
```

Copy the folder to back it up or move it to another PC; `--files DIR` points the
station at another one. The demo is only ever copied in once, into a card that
has no program of its own - your file is never overwritten by an upgrade.

The `presets` folder arrives **filled**: one file per entry, named after the key
path that inserts it (`34.txt` is WORD `4` = `U INC`, `41.txt` is G7X `1` = the
OD cycle). They are written once, onto a card whose `presets` folder holds no
entry of its own - so a card you already program is left exactly as it was, and
the station's own `examples\presets` folder is still there to copy from.
Delete a file and that address has no entry any more; put one back and it does.

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
`uCNC/src/modules/nc2/TESTING.md` and `g7x/TESTING.md`.

## When something is wrong

| What you see | What to look at |
| --- | --- |
| The window looks like an older version | `--version` (or the title) against the file's Properties; a copy from an earlier build is the usual cause |
| The file list is empty | the card is somewhere else (`--files`), or you deleted the demo - `examples\` beside the exe is still there to copy from |
| A program will not open | only `.nc`, `.t` and text files open; anything else is refused |
| A move does not happen | a jog is only sent from a standing axis, and MANUAL refuses a step that would cross a stop |
| `Too many NC lines` with a text file | the file uses an ancient line ending (a lone carriage return); re-save it |
| Nothing reaches a real machine | RUN drives the virtual machine today - see `tools/nc_sender` for the Grbl path |
