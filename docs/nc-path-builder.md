# NC path builder: the pad walks a contour

> **Superseded by a proposal, not yet decided** (`uCNC/src/modules/nc/TODO.md`,
> "The path builder is 728 lines, and Fanuc needs two words"): this dialect has
> no `U`/`W` word, so the in-place point this pad keeps is the only way to write
> an increment today. If the parser takes `U`/`W` as the incremental twins of
> `X`/`Z`, every part of this file describes code that no longer has a job -
> `G1 W-10.0 F0.2` carries the increment itself.

The path builder is a contour-entry tool for EDIT. Insert a cycle and its end
mark from the G7X vocabulary first, place the cursor inside that closed block,
then open PATH. The pad appends `G1` contour rows before the block's end mark.
Each row copies the unmoved axis and selects the axis or axes to enter through
the editor's existing numeric field flow.

The pad shares its drawing with MANUAL and the editor helper. It does not own
G-code templates, cycle semantics, or screen-level editing.

## Ownership

| Concern | Owner |
| --- | --- |
| G71/G72, G80 and other command templates | NC vocabulary and presets |
| Cycle/block boundaries and contour classification | `nc_g7x.c`, using G7x syntax |
| Pad direction, current point, inserted contour rows and pending word | `nc_path_builder.c` |
| Numeric field entry and document edits | NC editor |
| Pad drawing | `nc_draw_modal_items()` |
| Preview stock corner | `nc_preview_collect()` |

PATH never creates a cycle header, end mark, or approach move. It starts only
inside a closed block recognized by `nc_g7x_block_containing()`. Outside one,
it reports `Select a closed G71/G72 block` and leaves the document unchanged.
The operator chooses OD, ID, facing, or boring explicitly through the existing
G7X vocabulary before opening PATH.

## Key map

The pad uses the same layout as MANUAL: `7 8 9` / `4 5 6` / `1 2 3`.
The X direction follows the preview: X- moves toward the centreline, so `8` is
X- and `2` is X+. Z+ points right, so `4` is Z- and `6` is Z+. Corners move
both axes.

| Key | Row and selected word |
| --- | --- |
| `4` / `6` | Z move; copy X and select Z |
| `8` / `2` | X move; copy Z and select X |
| `7`, `9`, `1`, `3` | Diagonal; select X, then Z |
| `5` | Finish path entry; writes no extra row |
| `#` | Advance the prefill step: 0.5, 1, 2, 5, 10, 25, 50 mm, wrapping |
| `*` | Delete the pending row, or undo the last completed row |
| `0` | Remove rows inserted in this PATH session and exit |
| `A` | Leave the screen; keep the program text already written |

The default point step is 10 mm. PATH refuses a G20 block; switch the program to
G21 first. The point values are prefills only; the operator can replace them
with the editor's regular digits, sign, decimal, and accept keys. The X word is
a diameter in G7 mode, as in the program itself.

The first point in an empty cycle starts at the preview's stock face
(`X = stock OD`, `Z = 0`). Continuing an existing contour starts from its last
point, regardless of where the cursor sits inside the block. New rows are
inserted immediately before that block's end mark. Finishing with `5` adds no
rapid or return move: machine approach and retract remain the program's job.

## Verification

`tools/test_nc_ui.py --buildertest` exercises the host NC shell, checks typed
and copied axes, diagonal word entry, undo, session cancel, continuation from
the last point, and refusal outside a closed block. It reads the saved program
back from the host filesystem. These are software checks; cycle cutting,
profile direction, and panel readability still need machine testing.
