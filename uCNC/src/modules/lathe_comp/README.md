# lathe_comp - cutter radius compensation geometry

The geometry of a lathe contour shifted by the tool's nose radius, and the
checks that say when the tool cannot cut what the program asks for. It is the
first stage of `docs/lathe-cutter-comp.md`, which is the spec this module
serves.

## Why it is its own module

Compensation is a **path** transformation: by the time motion reaches the
planner the geometry has to be final. The doc's rule is that G7x *consumes* a
compensated contour and never implements compensation itself - so the geometry
lives here, and G7x (or any other consumer) hands in a contour and gets the
shifted one back.

The contour is in **radius space in X**, which is the space G7x's region is
built in: a program's `X` is a diameter and the sender converts it when it
writes a line out, while a nose radius is a real length and is added here
directly. That is the doc's point 1, and it is why this module never halves
anything.

The plane and the turn sense are G7x's own (`g7x_v2_*`, and its note that G18's
Z/X orientation reverses CW against an X/Z drawing): the centre of a `cw` arc
lies to the left of its chord.

## What it does

- **A line** moves along its own normal, and two adjacent lines meet on their
  intersection (an outside corner extended, an inside one trimmed). This is the
  bench's *"all lines should be moved by r of tool"*.
- **An arc** keeps its centre. The offset that lands on the same side as the
  centre turns the tool *inside* the arc, and that arc **loses** the radius
  (`R - r`) - the bench's *"circles R should be substracted by r"*, and a
  shoulder fillet is the case. The other way the arc gains it.
- **`R > r`** is checked on every arc the offset turns inside of: an arc no
  bigger than the tool is refused (`LATHE_COMP_ARC_TOO_SMALL`), as the bench
  asked, rather than cut with a gouged corner.
- **A join the simple offset has no answer for** - a corner where an arc meets
  its neighbour at an angle rather than tangentially - is refused
  (`LATHE_COMP_NO_TURN`) instead of being guessed. Every fillet and round the
  editor writes is tangent, and a tangent join is exact here.

## What is not here yet

- ~~**The caller.**~~ Done: `g7x_stream_set_comp()` and the offset at
  `g7x_stream_prepare()` (G7x *consumes* the answer), checked by
  `test_g7x.py generator`'s `test_cutter_comp_offset()`; the tool table is the
  only source of the radius - the panel asks `ncc_tools_active()` for the tool in
  play at the `G41`/`G42` line and hands its `R` on (`ncc_emit.c`); and both
  panel builds compile this directory (the RP2350 envs by their `+<*>` base, the
  station through `tools/test_nc_ui.py`'s module list).
- ~~**Where the radius comes from.**~~ The tool table, decided at the bench
  (2026-09-28): *"take R from tool table of course. no other options"*. A
  compensation asked for with no radius to apply stops the run where it is
  written rather than cutting the programmed path.
- **The preview does not draw it yet.** The pane still draws the contour the
  operator programmed; with compensation on, the cut is the offset one. The
  preview has to ask G7x for the same compensated region (and `G40` mid-contour,
  lead-in/lead-out, gouging and the cycles' own use are stages 4 and 5).
- **Lead-in and lead-out, `G40` mid-contour, gouging and self-intersection,
  and the cycles** are stages 4 and 5 of the doc.

## Checking it

```powershell
python tools\test_lathe_comp.py
```

The checks are the geometry a lathe profile is made of: the lines and the sharp
corner, the fillet that loses the radius (and gains it from the other side), the
arc too small for the tool, and the corner the offset refuses.
