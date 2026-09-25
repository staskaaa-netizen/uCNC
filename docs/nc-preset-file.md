# NC preset file

`nc_presets.c` owns `/D/presets.txt` and keeps compiled fallback presets when
the file is absent or malformed. The file supplies insert text only; G-code
meaning and validation remain with `nc_vocab`, the parser and G7x.

## When the file is resolved

The SD card is mounted from the main loop, not before module init, so the file
cannot be touched during `nc_visual_init()`. Resolving it is therefore lazy:

- `nc_presets_init()` (boot) loads the compiled presets and makes the first
  attempt.
- `nc_presets_sync()` is retried from the NC input path until the file is
  settled. It is a single flag check once that has happened.
- When the file is there, it is read once and its sections replace the compiled
  ones.
- When it is not there, the compiled presets are written to the card so there is
  a real file to edit. A failed write means the drive cannot answer yet (no
  card, or a card inserted later): the compiled presets stay in use and the
  next call tries again.
- A file that parses to no usable section (for example a damaged or hand-edited
  one) falls back to the compiled presets and is **left untouched**, so the
  operator's text can still be repaired. Only a missing file is created.
- A write that fails half way is removed again rather than left behind looking
  like the operator's own file.

No scanout/realtime path reads or writes it. The input path is the main-loop
task, the same one the file manager already uses.

## Format

```text
[41]
name=OD ROUGH
line=G71 U0 R0 X0 Z0 F0 P0 Q0

[51]
name=THREAD OD
line=G76 X0 Z0 P0 Q0 F0 I0 L0 R0
```

- **An ID is the key path that inserts the entry**, one digit per level, up to
  three levels. `0` is the pad's own quit key, so a path that ends at the footer
  is written `<key>0`. The panel never asks the operator to type an ID: you press
  the keys, and the file's `[n]` says which entry an edited section defines.
- **The pads are filled from the file.** A key's slot is the section whose id is
  that key path, and the section's `name=` is what the key reads as: the panel
  keeps no second list of names. A section with an id no key uses is offered all
  the same - write `[12] name=ROUGH line=(rough 1)` and OPS `2` is that entry -
  so the generator is extended by adding to the card, not by asking for a key.
  (The entries a key *does* rather than writes - `Q`, `N`, `DRAW`, the `G` field,
  the `T` field, the tool table - are the panel's and are not sections.)

  | ID | Entry | Keys |
  | --- | --- | --- |
  | `11` | a new line - blank by default, and whatever else the card says (a separator comment, a command the operator keeps needing) | `1 OPS` then `1 INS` |
  | `16` | setup - the stock dimensions, as one block: `G970` (preview extents), `G971` (stock), `G972` (chuck clamp), `G973` (preview mode) | `1 OPS` then `6 SETUP` |
  | `23` | tool change (`M6`) | `2 TOOL` then `3 M6` |
  | `24` | spindle on (`M3 S1000`) | `2 TOOL` then `4 M3` |
  | `25` | spindle stop (`M5`) | `2 TOOL` then `5 STOP` |
  | `26` | spindle reverse (`M4 S1000`) | `2 TOOL` then `6 M4` |
  | `32` | chamfer the row the cursor is on (` C0`, inline) | `3 WORD` then `2 CHMF` |
  | `33` | round the row the cursor is on (` R0`, inline) | `3 WORD` then `3 RND` |
  | `46` | end mark | `4 G7X` then `6 G80` |
  | `41` `42` `43` | OD, ID, face | `4 G7X` then `1`, `2`, `3` |
  | `48` | finish cut (`G70 P Q`) | `4 G7X` then `8 FINISH` |
  | `51` `52` `53` | thread OD, thread ID, tap | `5 THREAD` then `1`, `2`, `3` |
  | `61` `62` `63` | drill, peck, dwell | `6 PECK` then `1`, `2`, `3` |

  **Every entry that writes text into the program is on this side**, including
  the ones that look like commands: the blank line, the tool change and the
  spindle words. The id of each one is the key path that inserts it, and their
  compiled text is only a default - the `S` in `M3` is the operator's choice, not
  the panel's (the MANUAL spindle keys send the speed the machine remembers; a
  hardcoded one in the program's text was the odd one out).

  What is *not* here, and why: the entries that **edit the line** rather than
  insert fixed text (G7X `4 Q` and `5 N` write or pick a word on the cursor's
  line, `7 DRAW` is the contour builder), the ones that take a **typed value**
  (`1 SELECT` opens the `T` field), and the ones that run an **action** (the file
  list `0`, delete `*`, opening the tool table, the save - which the panel does by
  itself when the screen goes quiet). They are the panel's keys and have nothing
  for a card to define. That is the line between the two owners: **what a key
  means is the panel's; what an entry writes into the program is this file's.**

- **IDs the menus used to hold still name their entry**, so a card written before
  the menus were renumbered keeps the operator's own text instead of silently
  falling back to the compiled default: `10` is the setup section (`16`), `44`
  the finish cut (`48`), `80` the end mark (`46`). Two sections that name the
  same entry are read in file order and the last one wins, so an old and a new
  section for one entry in the same file is not an error - but the name in the
  file is the operator's to tidy, and nothing rewrites it behind their back.

  (Both halves of this came from the bench reading the file: "*in presets it is
  set as `[10]` but i need to press 16?*" - the old number was a path on the
  older footer, which is why it read like one; and the fix was to make every id
  the path it is today, not to explain the difference away.)
- `name` is the section's short label. It is **required** - a section without one
  is skipped, and the compiled entry it would replace stays - but the floating
  3x3 helper still shows its own fixed slot labels. Showing the file's `name=`
  there is the open item in `uCNC/src/modules/nc/TODO.md`.
- Each `line=` is one inserted editor line, in order.
- A `line=` that **starts with a space continues the line above** instead of
  starting one. That is the whole trick for a value that belongs on a line
  already written - a `Q` on a cycle header, a `C`/`R` on a contour row, a second
  word on any row - and it means the controller never sees a line break inside a
  block it has to read as one (bench: "on N/Q or other things to be added inline
  - just use trick by not have a new line before values. so controller will know
  it all"). The word just written is left picked, so the number is typed straight
  into it. With two rows the same entry can write a row *and* its corner words:

  ```text
  [17]
  name=ROW
  line=G1 X0 Z0
  line= C0
  line= R0
  ```

  inserts the single line `G1 X0 Z0 C0 R0`.
- Maximum 24 sections, 8 lines per section, and `NC_MAX_LINE_LEN` per line.

  Those are the module's storage, not a policy. `nc_presets.c` keeps
  `NC_PRESET_MAX` (24) records of `NC_PRESET_MAX_LINES` (8) rows of
  `NC_MAX_LINE_LEN` (96) bytes in one static array - under 20 KB of RAM that is
  visible in the map and never allocated - because the panel's side of the
  firmware has no heap, and a card must not be able to grow it. The compiled
  defaults occupy nineteen of those records, so a card's own entries have five
  free ones (editing a section that reuses an existing id costs none, and a
  section that arrives with the array full is dropped without a word - see
  `nc/TODO.md`). `NC_WRAP_LINE_LEN` (46) is a different kind of number: it is the
  panel's own row width, so a longer row is drawn as two panel rows with a tab
  marking the continuation - fine for a program, awkward for a preset's text.
- The file defines the entries it names: a section replaces the compiled entry
  with the same id (an edited `[41]` is the operator's OD preset and stays so).
  An id the file does not mention keeps its **compiled** text, so a card written
  before a new entry existed still offers it - which is how `44 FINISH` reaches a
  card whose `presets.txt` predates it. Sections without a `name=` or without a
  `line=` are skipped, leaving the compiled entry in place; if no valid section
  exists at all, the compiled set is what the panel uses and the file is left
  alone for repair.

Editing the file on the machine, and semantic validation of what it contains,
are later work.

The file is reachable from the panel: the file list carries text files beside
the programs (`0` opens the list on EDIT, TOOLS and RUN), so `/D/presets.txt`
can be opened and saved as text. It is not a program: `nc_path_supported()` is
what decides that, so the preview does not parse it and RUN refuses it.

## Checks

`python tools\test_nc_ui.py` builds the NC screen for the desktop and then runs
the same binary with `--presettest`, which drives the firmware `fs_*` API in a
scratch root and asserts the contract above: a missing file is created, an
edited file is used, an unparsable file falls back and is left alone, every
`line=` of a section is inserted in order (through the same call the panel's OD
entry makes), and a section without a `name=` is skipped. Boot with a card,
without a card, and with a card inserted after boot remain bench items.
