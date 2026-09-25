# nc2 - the NC screen, second iteration

## Why it exists

The bench, 2026-09-25: *"after some code review - it seems this whole nc drifted
from pure nc value editor to something too big to be nice. Visual side is ok to
agree yet it could be made simpler too."* `nc` is **14 758 lines** (13 870
without its host test) and 2 037 of those are one file. `nc2` is the same job
with a budget of **7 000**: the value editor, the entries, the sender and the
preview - and nothing else.

It is a second module, not a refactor of the first: `nc` keeps driving the panel
until `nc2` answers everything `nc` does, and then the panel is switched over
once. No compatibility layer, no shared state, no half-migrated screen.

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
| `nc2_g7x.c` | 300 | document scanning for G7x blocks |
| `nc2_run.c` | 600 | the run: pacing, the DRO's numbers, hold/stop |
| `nc2_state.c` | 380 | what the panel remembers between boots |
| `nc2_tools.c` | 250 | the tool table |
| `nc2_text.c` | 300 | typing into the picked word |
| `nc2_vocab.c` | 220 | the G-code templates and the legends |
| `nc2_visual.c` | 850 | the screen: code pane, the 3x3, the floating DRO |
| `nc2_preview.c` | 1 100 | stock, contour, dimensions, the live tool |
| `nc2_draw.c` | 450 | primitives, glyphs, the 3x3 grid |
| headers, `nc2_layout.h` | 350 | the boundary and the shared numbers |
| **total** | **7 000** | |

Its own test target, `tools/test_nc2.py`, builds and runs the module against the
same virtual machine the station uses (AGENTS.md 7), and `nc2/TESTING.md` keeps
the bench items.

## What would stay hardcoded

Nothing yet - that is the point of this page. The list has to be exactly: the
named keys above, the address -> file rule, and whatever the open questions
settle. Anything else is a file.

## Open questions, to answer before code

1. **The pad is the whole navigation.** With no footer, the first level of the
   pad has to hold the groups `nc` gives footer keys to (OPS, TOOL, WORD, G7X,
   THREAD, PECK) - nine slots, six used. Deeper levels are more addresses. Does
   `A` walk up one level, and is the root one press away?
2. **A card with no files.** `nc` falls back on 21 compiled entries (~60 lines
   of data) so a bare card still has words. Keep that fallback in `nc2`, or are
   the entries *only* files?
3. **The other screens.** MANUAL (its own 3x3 jog pad and DRO) and TOOLS (the
   tool table) stay modes of their own, or does the tool table become files too?
4. **The contour pad (`47`).** It is the one key that computes its rows instead
   of writing them. Keep it as the single hardcoded key, or make its nine
   directions files too (the `471`-`479` shape, one more digit per point)?
