# NC preset entries

The NC pads are filled from the card. What a helper key *writes into the
program* is not a constant in the firmware and not a second syntax inside it: it
is a file the operator owns. `nc_presets.c` owns the reading, `nc_vocab` owns
what a word means, and the parser and G7x own what the result does.

## The model

An entry is **an address, and at that address two things**:

- an **address**, which is the key path that inserts the entry - `42` is G7X `4`
  then `2`. The address is the one thing the pads need, so it is the id;
- a **name**, which may be empty - the label the key reads as;
- the **rows** it writes, which may not be empty. They are lines of a program,
  and a row that starts with a space continues the row before it.

That is the whole contract. Everything else is the card's spelling of it.

## The card

```text
D:\presets\            one file per entry, named after its address
    11.txt             "1 OPS then 1" - the new line
    16.txt             "1 OPS then 6" - the setup block
    41.txt             "4 G7X then 1" - the OD cycle
    ...
```

- The **first row is the name**. An empty first row means no name of its own,
  and the address keeps the compiled name (below).
- **Every row after it is written**, in order, when the key is pressed. A row
  that starts with a space **continues the row above** instead of starting one:
  that is how a value that belongs on a line already written - a `Q` on a cycle
  header, a `C`/`R` on a contour row - gets into the program without the
  controller ever seeing a line break inside a block it has to read as one
  (bench: "on N/Q or other things to be added inline - just use trick by not
  have a new line before values. so controller will know it all").
- **A file with no row after the name is not an entry**: the rows are the
  mandatory half. The address stays as it was.
- **The addresses the pads can reach are `10`-`69`**: one digit for the pad, one
  for the slot. A file named anything else is left alone (it is just a text file
  in a folder).
- The extension is `.txt` so the panel's own file list and editor can see it:
  **the file list and the editor are the preset editor.** There is no second
  editor and no format to parse. `presets\41.txt` is ordinary text - open it,
  change it, save it.
- New entries are added the same way: a file at an address no compiled entry
  uses. `presets\12.txt` with `COOLANT` and `M8` puts a coolant word on OPS `2`.

## The compiled entries

A card with no folder, or no file for an address, answers with the compiled
table in `nc_presets.c` - the nineteen entries the panel ships with (the new
line, the setup block, the tool and spindle words, the corner words, the cycle
templates, the thread and peck entries, the end mark). They are the only part of
this that lives in flash, and they are a *fallback*, not a copy the card has to
match:

- a file at an address **replaces** that entry (name and rows);
- an empty first row keeps the compiled *name* while the file owns the rows;
- an address with no file behaves exactly as if the folder did not exist.

The address list is in `nc_presets.h`; the compiled names and rows are next to
it in `nc_presets.c`.

## When it is read

The card is mounted from the main loop, long after module init, so the folder is
resolved lazily: `nc_presets_init()` at boot, `nc_presets_sync()` retried from
the NC input path until the drive answers. The settle **creates the folder** if
it is not there - the one write the panel makes by itself - and reads each
entry's *name* into RAM (about a kilobyte). The rows are not kept: they are read
from the file when a key writes them, so nothing on the card can grow the
panel's memory.

The folder is read once per settle. A file added or edited on the card while the
panel is running is picked up on the next boot (an address the panel already
knows about still writes what the file says, because the rows are read then).

## What this replaced

Until 2026-09-25 the entries lived in one file at the card root, in a format this
module parsed. That format, its parser, an alias table for addresses whose paths
had moved, a fixed table of 24 records with a cap, and a step that wrote the
compiled defaults out when the file was missing are all gone: the map above never
needed any of it. The bench said it in one line - *"it all now is good and smelly
scaffolding, while this all overall is just basically addresses."*

Nothing reads that old file now, and nothing writes a defaults file at boot: the
only write left is the folder creation. A card that still carries the old file
keeps it as ordinary text; its entries are moved by hand, because a migration
would be the parser again.

## Checks

`python tools\test_nc_ui.py` builds the NC screen for the desktop and runs
`--presettest`, which drives the firmware `fs_*` API in a scratch root: a card
with no folder answers with the compiled entries and gets the folder created; a
file replaces its address, both halves; an empty first row keeps the compiled
name; an address no compiled entry uses becomes an entry; every row is written
in order; a row that starts with a space continues the row above; a file with no
rows is not an entry; an address outside `10`-`69` is not one either; and the old
file this replaced is not read. Booting with a card, without a card and with a
card inserted later remain bench items.
