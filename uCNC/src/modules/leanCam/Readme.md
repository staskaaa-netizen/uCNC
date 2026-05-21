# LeanCam

LeanCam is a small conversational lathe layer for uCNC. It stores programs as
plain text blocks and turns the selected block or full program into ordinary
G-code.

## Current Model

- `.lcam` files live in the normal uCNC file system.
- One line is one block.
- `SETUP` describes stock and holding.
- `TOOL` describes the active tool and cutting defaults.
- Cycles reference a tool with `T{...}` and use the latest setup/tool context.
- There is no separate PROCESS or T+P catalog in the current minimal model.

## Core Blocks

```text
SETUP|L{}|OD{}|ID{(0)}|CLAMP{(0)}|EXTRA{(0)}|CLR{(1)}
TOOL|T{(1)}|R{(0.8)}|ORIENT{(3)}|R_FEED{(120)}|FIN_FEED{(60)}|DOC{(2.0)}|FIN_DOC{(0.5)}|XOFF{(0)}|ZOFF{(0)}
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
THR_OD|T{(1)}|M{}|P{}|Z1{(0)}|Z2{(-50)}|N{(0)}|ST{(1)}|Q{(0)}
THR_ID|T{(1)}|M{}|P{}|Z1{(0)}|Z2{(-50)}|N{(0)}|ST{(1)}|Q{(2)}
RADIUS_OD|T{(1)}|D{}|Z1{(0)}|Z2{(-20)}|R{}|Q{(0)}
RADIUS_ID|T{(1)}|D{}|Z1{(0)}|Z2{(-20)}|R{}|Q{(2)}
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
