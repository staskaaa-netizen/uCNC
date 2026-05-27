# LeanCam

LeanCam is a small conversational lathe layer for uCNC. It stores programs as
plain text blocks and turns the selected block or full program into ordinary
G-code.

## Current Model

- `.lcam` files live in the normal uCNC file system.
- One line is one block.
- `SETUP` describes stock and holding.
- `TOOL` describes the active tool and cutting defaults.
- `TOOLCALL` selects a tool from `tools.lct` without copying its definition.
- Cycles reference a tool with `T{...}` and use the latest setup/tool context.
- There is no separate PROCESS or T+P catalog in the current minimal model.
- Tool lookup is by numeric `T`. A local `TOOL|T{n}` in the program overrides
  the catalog by position. Catalog `T` numbers must be unique; duplicate
  catalog entries are rejected as ambiguous.

## Core Blocks

```text
SETUP|L{}|OD{}|ID{(0)}|CLAMP{(0)}|EXTRA{(0)}|CLR{(1)}
TOOL|T{(1)}|R{(0.8)}|ORIENT{(3)}|R_FEED{(120)}|FIN_FEED{(60)}|DOC{(2.0)}|FIN_DOC{(0.5)}|XOFF{(0)}|ZOFF{(0)}
TOOLCALL|T{(1)}
```

## Cycle Templates

```text
OD|T{(1)}|D1{(SETUP.OD)}|Z1{(0)}|Z2{(-50)}|D2{(THIS.D1)}|RND{(0)}|CHMF{(0)}|DT{(THIS.D2)}|Q{(0)}
ID|T{(1)}|D1{(10)}|Z1{(0)}|Z2{(-50)}|D2{(20)}|RND{(0)}|CHMF{(0)}|DT{(THIS.D2)}|Q{(2)}
FACE|T{(1)}|D{(SETUP.OD)}|Z1{(1)}|Z{(0)}|Q{(0)}
DRILL|T{(1)}|Z1{(0)}|DEPTH{}|PECK{(0)}|FEED{(120)}|S{(800)}
TAP|T{(1)}|Z1{(0)}|DEPTH{}|PITCH{}|RPM{(300)}
CUT|T{(1)}|D{(0)}|Z{(-50)}|WIDTH{(3)}|Q{(1)}
CHAMFER|T{(1)}|D{}|Z{}|SIZE{(1.0)}|Q{(0)}
CHMF_ID|D{}|Z{}|SIZE{(1.0)}|Q{(2)}
THR_OD|T{(1)}|M{}|P{}|Z1{(0)}|Z2{(-50)}|N{(0)}|ST{(1)}|Q{(0)}
THR_ID|T{(1)}|M{}|P{}|Z1{(0)}|Z2{(-50)}|N{(0)}|ST{(1)}|Q{(2)}
R_OD|D{}|Z1{(0)}|Z2{(-20)}|R{}|Q{(0)}
R_ID|D{}|Z1{(0)}|Z2{(-20)}|R{}|Q{(2)}
GROOVE|T{(1)}|D1{(SETUP.OD)}|D2{(40)}|Z1{(-20)}|Z2{(-40)}|WIDTH{(3)}|Q{(1)}
PART|T{(1)}|D{(0)}|Z{(-50)}|WIDTH{(3)}|Q{(1)}
```

`Q` is retract mode:

- `Q0`: direct diagonal rapid out
- `Q1`: X first, then Z
- `Q2`: Z first, then X

## Coordinates

- Z0 is the front face.
- Negative Z goes into the stock toward the chuck.
- X is diameter mode.
- Setup `OD`, `ID`, `L`, `CLAMP`, `EXTRA`, and `CLR` define the preview and safe moves.

## Tool Data

`TOOL` is currently both the tool identity and the cutting defaults. That is
intentional for the minimal controller workflow.

- `T`: tool number
- `R`: nose radius
- `ORIENT`: keypad-style tool orientation
- `R_FEED`, `FIN_FEED`: roughing and finishing feed defaults
- `DOC`, `FIN_DOC`: roughing and finishing depth defaults
- `XOFF`, `ZOFF`: measured offsets

### ORIENT Keypad Codes

`ORIENT` uses the numeric keypad as a 3x3 tool-shape grid:

```text
7 8 9
4 5 6
1 2 3
```

Single digit values keep the old simple tool orientation behavior:

- `0` or missing `ORIENT`: no insert shape, just a red DOC-sized dot at X0/Z0
- `1`, `3`, `7`, `9`: square insert anchored on that corner
- `2`, `4`, `6`, `8`: square insert sharing the nearest corner behavior
- `5`: center drill-style marker

Multi-digit values describe an insert polygon by walking the keypad points in
order. Think of the code as a tiny shape generator: each digit is one vertex on
the keypad grid. Three digits make a triangle, four digits make a quadrilateral.
For three-digit shapes, the middle digit is the measured X/Z tool tip anchor.
For four-digit shapes, the middle digit pair is the reference cutting line. The
first middle digit supplies the X-side anchor and the second middle digit
supplies the Z-side anchor, so `2486` hangs from the X/Z origin by its `4-8`
cutting edge instead of being centered on the origin.

Examples:

- `276`: rhombic insert, same X/Z tip anchor as `7`
- `183`, `176`, `172`, `679`: triangular insert variants
- `2486`: 45 degree rotated square/diamond; `4-8` is shown as the cutting side,
  `2-6` as the mounted side

Useful patterns:

```text
7       square, tip at keypad 7
3       square, tip at keypad 3
276     rhombic/diamond-style insert around tip 7
183     triangle from points 1 -> 8 -> 3, tip at 8
176     triangle from points 1 -> 7 -> 6, tip at 7
172     triangle from points 1 -> 7 -> 2, tip at 7
679     triangle from points 6 -> 7 -> 9, tip at 7
2486    rotated square/diamond, cutting edge 4 -> 8
```

The renderer fills the whole insert polygon, but red outlines are only drawn on
the cutting/reference sides. For example, `172` draws the `7-1` and `7-2` sides
but does not draw a red border between `1` and `2`.

Valid `ORIENT` input is `0` or an integer from one to four digits long. For
nonzero shape codes each digit must be `1` through `9`; `0` is not valid inside
a multi-digit orientation code.

The file `tools.lct` is used as the simple tool catalog. It uses the same
`TOOL|...` line format and is visible in the file manager.

## UI

- File view: open/create/delete files and open the tool catalog.
- Program view: insert cycles, edit selected line, run selected line.
- Draft view: edit fields one at a time, then commit with `#`.
- NC view: inspect generated G-code.

LeanCam snapshots are consumed by display renderers such as `lvds_renderer`.
LeanCam itself does not own LVDS or HSTX scanout.

## Notes

- Keep the saved format simple text.
- Keep process databases and T+P override tables out of the current code path.
- Renderer previews are sanity checks, not machine safety logic.

## TODO

### Simple ATC Turret Neighbor Collision Shadow Preview

Add a conservative 2D collision preview for neighboring turret/ATC tools. Do
not build a full 3D turret simulation; keep it in the flat X/Z preview by
drawing extra shadow planes for the previous and next turret stations.

Inputs to add later:

- `TURRET.N_TOOLS`
- `TURRET.PCD` or `TURRET_RADIUS`
- active tool station, either from `TOOL.STATION` or `T`
- `TOOL.TYPE`
- `TOOL.SHADOW_ENABLE`
- `TOOL.SHADOW_DIA` or `HOLDER_DIA`
- optional `TOOL.SHADOW_LEN`

Preview concept:

- Compute station spacing as `360 / N`.
- Neighbor stations are active station `-1` and `+1` modulo `N`.
- Estimate each neighbor tool axis from turret radius and station angle.
- Project each qualifying neighbor into the current X/Z preview as a
  conservative triangle/wedge shadow.
- Draw shadows dimly and label them `ADJ-` / `ADJ+` when space allows.

Only show shadows by default for tools that can realistically intrude into the
work envelope:

- boring bars
- drills
- reamers
- long internal tools
- tools with explicit `SHADOW_ENABLE`

OD tools should not clutter the preview unless explicitly enabled.

Collision behavior:

- If stock/profile/toolpath intersects a neighbor shadow, show a warning in the
  preview and mark the process as collision-risk.
- First version should warn only; do not block running yet.

Acceptance:

- Current cycle preview works unchanged when no turret config exists.
- With turret config, selected tool previews previous/next neighbor shadows.
- Boring/drill neighbors create visible warning triangles.
- OD neighbors are hidden by default.
- Intersections produce a warning.
- No full 3D model is required.
