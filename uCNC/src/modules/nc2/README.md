# nc2 - the NC screen, second iteration

## Why it exists

The bench, 2026-09-25: *"after some code review - it seems this whole nc drifted
from pure nc value editor to something too big to be nice. Visual side is ok to
agree yet it could be made simpler too."* `nc` is **14 758 lines** (13 870
without its host test) and 2 037 of those are one file. `nc2` is the same job
with a budget of **6 700**: the value editor, the entries, the sender and the
preview - and nothing else.

It is a second module, not a refactor of the first: `nc` keeps driving the panel
until `nc2` answers everything `nc` does, and then the panel is switched over
once. No compatibility layer, no shared state, no half-migrated screen.

Two things move out of NC's line count before `nc2` is written, because they are
not NC's to own: the cycle/block scan goes to g7x (below), and the tool table's
screen is left where it is for now.

## What the operator sees

- The program, one word picked at a time.
- **One 3x3 pad, pinned in the bottom right.** It is the only menu there is:
  no footer strip, no submenu tables, no floating helper, and no helper line
  written into the document.
- A DRO that is **not on screen unless the machine is doing something**: in RUN
  it floats over the preview; idle, the pane has the whole body.

## The keys, all of them

| key | means |
| --- | --- |
| `A` | mode (the screens) / cancel - and, on the pad, one level up |
| `B` `C` | step: back and forward through what is picked |
| `D` | the next numeric field; on the last one, accept |
| `#` | insert the picked entry / accept the picked value |
| `*` | delete - the thing under the cursor |
| `0` | the file list |
| `1`-`9` | the pad; with a value picked, they type into it |

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
| the modal helper, its label line written into the document, the accept/cancel bookkeeping (part of `nc_editor.c`) | ~450 |
| the per-screen usage/label tables, the strip's toggle logic (part of `nc_visual.c`) | ~700 |
| the preview's second dialect (the `#if NC_PREVIEW_DIN_*` variants) | ~600 |
| `nc_feedback.c`, and the stub keys (`2 TOOL` -> `2 EDIT` sets a status line and returns - `nc_visual.c:697`) | 35 + 20 |

The six keys `nc` hardcodes inside pads are not lost by dropping them:
`21` SELECT, `31` the `G` field, `44` Q and `45` N are each *a lone letter* -
`T`, `G`, ` Q`, ` N` - which a file can write exactly like any other entry, and
the editor's "a letter with no number is a field waiting for a number" rule does
the rest. That leaves the contour pad (`47`) as the only key that computes
rather than writes; see the open questions.

## Files and budget

| file | target | what it owns |
| --- | --- | --- |
| `nc2.c` | 700 | the document: words, fields, the value editor's keys |
| `nc2_presets.c` | 450 | addresses -> files: names, rows, the pad's tree |
| `nc2_files.c` | 350 | the card: listing, load, save, new, delete |
| `nc2_emit.c` | 700 | the sender: stream, G7x feeding, `U`/`W` |
| `nc2_run.c` | 600 | the run: pacing, the DRO's numbers, hold/stop |
| `nc2_state.c` | 380 | what the panel remembers between boots |
| `nc2_tools.c` | 250 | the tool table |
| `nc2_text.c` | 300 | typing into the picked word |
| `nc2_vocab.c` | 220 | the G-code templates and the legends |
| `nc2_visual.c` | 850 | the screen: code pane, the 3x3, the floating DRO |
| `nc2_preview.c` | 1 100 | stock, contour, dimensions, the live tool |
| `nc2_draw.c` | 450 | primitives, glyphs, the 3x3 grid |
| headers, `nc2_layout.h` | 350 | the boundary and the shared numbers |
| **total** | **6 700** | (the block scan is g7x's now; 300 of slack) |

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
| built | `nc2_presets.c` (address -> file, read, write), `nc2_boot.c` (the shipped entries, the one-time seed, the logo), `--seedtest` |
| next | `nc2.c`: the document and the dumb value editor - a line cut into fields at its letters, `D` walking the numerics, digits typing, nothing checked while editing |
| then | the 3x3 pad and the tree, the code pane and the floating DRO, files/state/run/emit/preview, the g7x block scan moving to `g7x_blocks.c`, the panel switch |

One upstream landmine was found on the way, in `file_system.c`: `fs_opendir()`
writes into the string it is handed to drop a trailing `/` (`char *newpath =
(char *)path; newpath[len - 1] = 0;`), so a literal like `"/D/presets/"` faults on
read-only memory. `nc2` spells the folder without the slash for that one call and
says why in `nc2_presets.h`; the function itself is the core's to fix.
