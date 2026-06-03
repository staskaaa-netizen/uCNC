# LeanCam

LeanCam is a small conversational lathe layer for uCNC. It stores programs as
plain text rows and turns the selected block or full program into ordinary
G-code.

LeanCam owns program state, editing, file/catalog handling, generated G-code
stepping, preview state, and the UI snapshot. It does not own LVDS, HSTX,
scanout DMA, panel timing, or framebuffer internals.

## Current Model

- `.lcam` files live in the normal uCNC file system.
- One text row is one program block.
- Saved rows are plain resolved values, not template expressions.
- `SETUP` describes stock and holding.
- `TOOL` describes catalog/local tool geometry and cutting defaults.
- `TOOLCALL` selects a tool from the local program or `tools.lct` catalog.
- Cycle rows use the latest setup/tool context above the selected row.
- Tool lookup is by numeric `T`.
- A local `TOOL Tn ...` row in the program overrides catalog `Tn` by position.
- Catalog `T` numbers must be unique; duplicates are rejected as ambiguous.
- There is no active PROCESS/T+P database in the current minimal path.

## Coordinates

- Z0 is the front face.
- Negative Z goes into the stock toward the chuck.
- Positive Z moves away from the stock.
- X is diameter mode.
- Setup `OD`, `ID`, `L`, `CLAMP`, `EXTRA`, and `CLR` define preview stock and
  conservative safe moves.

## Core Rows

```text
SETUP L{} OD{} ID{(0)} CLAMP{(0)} EXTRA{(0)} CLR{(1)}
TOOL T{(1)} R{(0.8)} ORIENT{(3)} R_FEED{(120)} FIN_FEED{(60)} DOC{(2.0)} FIN_DOC{(0.5)} RPM{(800)} XOFF{(0)} ZOFF{(0)}
TOOLCALL T{(1)} R_FEED{(TOOL.R_FEED)} FIN_FEED{(TOOL.FIN_FEED)} DOC{(TOOL.DOC)} FIN_DOC{(TOOL.FIN_DOC)} RPM{(TOOL.RPM)}
```

Template expressions are draft-time only:

```text
{}                  required user input
{(literal)}         default literal
{(SETUP.FIELD)}     default from the active SETUP row
{(TOOL.FIELD)}      default from the selected TOOL row
{(TOOLCALL.FIELD)}  default from the current TOOLCALL row
{(THIS.FIELD)}      default from this same row
```

Saved `.lcam` rows must contain resolved values only.

## G-code-ish Rows

These rows are stored directly and are also the internal shape for generated
presets:

```text
G71 U{} R{} X{} Z{} F{}
G72 W{} R{} X{} Z{} F{}
G1 X{} Z{} C{(0)} R{(0)}
G2 X{} Z{} R{}
G3 X{} Z{} R{}
G80
G74 Z{} K{} F{}
G84 Z{} PITCH{} RPM{}
G33 X{} Z{} K{}
G76 Z{} P{} K{} J{} H{(1)} Q{(29.5)} R{(2)}
```

Current preset templates expand into visible `G71/G72/G1/G80` regions:

```text
OD
ID
FACE
RECESS
```

`G71/G72` regions start with the cycle header, contain contour rows, and end
with `G80`. The current roughing generator is intentionally conservative:

- `G71`: monotonic Z contour, roughs by X depth, feeds along Z.
- `G72`: monotonic X contour, roughs by Z depth, feeds along X.
- `G1 C` and `G1 R` corner shortcuts are expanded before roughing.
- `G2/G3` arc roughing is limited; G72 arcs fail clearly.
- Pocket/type-II roughing is future scope, not hidden fallback behavior.

`G76` is expanded into explicit `G33` passes. It accepts LinuxCNC-style words
where they map cleanly: `P` pitch, `J` first cut/depth of cut, `K` full depth,
`R` degression, `Q` compound angle, and `H` spring passes.

## Runtime Generation

Selected LeanCam runs are demand-fed into the normal uCNC stream:

```text
selected row/range
  -> leancam_gcode_stepper_begin()
  -> grbl_stream_readonly()
  -> leancam_gcode_stepper_next()
  -> parser/planner
```

LeanCam does not generate the whole selected `G71/G72/G76` run first. The
stepper emits one generated line when uCNC asks for one. This avoids the old
burst-generation memory and timing failure class.

File generation and preflight may still use the full emitter because they are
not the live selected-run path.

## Preview Flow

LeanCam generated preview uses the same stepper idea, but the consumer is the
snapshot/renderer path instead of the uCNC parser:

```text
button press
  -> leancam_bridge arms a preview stepper for the selected range
execution_controller_poll()
  -> leancam_bridge_tick()
  -> if renderer acked previous sequence:
       leancam_gcode_stepper_next()
       publish one current preview line
leancam_bridge_fill_snapshot()
  -> copy line/sequence/index into ui_snapshot_frame_t
visual renderer
  -> draw the one new line
  -> leancam_bridge_preview_ack(seq)
```

This is task-loop stepping with renderer acknowledgement. It is not a fixed
millisecond pacing layer and it is not a generated-line FIFO.

Rules:

- The bridge owns the generated preview stepper.
- The snapshot carries only the current generated preview line and sequence.
- The renderer consumes snapshot data only.
- The renderer must not call `leancam_gcode_*()`.
- The renderer must not own a generated G-code cache.
- File IO and program mutation stay out of visual drawing code.

## UI

- File view: open/create/delete files and open tool catalogs.
- Program view: insert templates, edit selected line, run selected line/range.
- Draft view: edit fields one at a time, then commit with `#`.
- NC view: inspect generated G-code.
- Visual preview is a sanity check, not machine safety logic.

LeanCam snapshots may be consumed by display renderers such as
`lvds_renderer`. LeanCam itself does not own LVDS or HSTX scanout.

## Tool Data

`TOOL` is both the tool identity and the cutting defaults in the current
minimal controller workflow.

- `T`: tool number
- `R`: nose radius
- `ORIENT`: keypad-style tool orientation/insert shape
- `R_FEED`, `FIN_FEED`: roughing and finishing feed defaults
- `DOC`, `FIN_DOC`: roughing and finishing depth defaults
- `RPM`: default spindle speed
- `XOFF`, `ZOFF`: measured offsets

The file `tools.lct` is the simple tool catalog. It uses the same `TOOL ...`
row format and is visible in the file manager.

## Tool Preview

`TOOL.ORIENT` uses the numeric keypad as a 3x3 tool-shape grid:

```text
7 8 9
4 5 6
1 2 3
```

Single digit values keep the legacy simple orientation behavior:

- `0` or missing `ORIENT`: no insert shape, only a red DOC-sized dot at X0/Z0
- `1`, `3`, `7`, `9`: square insert anchored on that corner
- `2`, `4`, `6`, `8`: square insert sharing the nearest corner behavior
- `5`: center drill-style marker

Multi-digit values describe an insert polygon by walking keypad points in
order. Three digits make a triangle. Four digits make a quadrilateral.

For three-digit shapes, the middle digit is the measured X/Z tool-tip anchor.
For four-digit shapes, the middle digit pair is the reference cutting line. The
first middle digit supplies the X-side anchor and the second supplies the
Z-side anchor, so `2486` hangs from the origin by its `4-8` cutting edge
instead of being centered on it.

Examples:

- `276`: rhombic insert
- `183`, `176`, `172`, `679`: triangular insert variants
- `2486`: rotated square/diamond with `4-8` emphasized as the cutting side

The renderer fills the insert polygon. Red outlines are drawn only on
cutting/reference sides; for example, `172` does not draw a red border between
`1` and `2`.

Live simulation still uses the fast legacy marker path. For multi-digit shapes
it collapses the code to the inferred tip direction.

## Module Boundary

LeanCam module ownership:

- `leancam_bridge`: app orchestration, selected-run and preview stepping.
- `leancam_gcode`: G-code generation and resumable runtime steppers.
- `leancam_code`: cheap row parsing and command/field helpers.
- `leancam_tool_catalog`: tool catalog lookup/cache.
- `leancam_resource`: fixed shared memory region access.
- `visual/leancam_visual*`: snapshot-to-pixels drawing and local visual state.

Hardware renderer ownership belongs to `src/modules/lvds_renderer`.

Do not add:

- generated run/preview text buffers in PSRAM
- renderer-side G-code generation
- renderer-side program/file IO
- hidden timers/tasks advancing LeanCam state
- LVDS/HSTX config knobs inside LeanCam code

## Notes

- Keep the saved format simple text.
- Keep process databases and T+P override tables out of the current code path.
- Keep generated selected runs one-way and demand-fed.
- Keep preview and machine execution separated.
- Keep historical HSTX failure/recovery notes in `lvds_renderer/README.md`.

## Future Work

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
