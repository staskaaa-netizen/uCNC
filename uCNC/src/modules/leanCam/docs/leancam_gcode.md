# LeanCam G-code Generation Rules

## Purpose

LeanCam treats the `.lcam` conversational program as the source of truth. G-code is a derived output produced by `leancam_gcode.c`.

## Current Scope

The active converter supports:

* `OD`
* `ID`
* `FACE`
* `DRILL`
* `CUT`
* `PART`
* `GROOVE`
* `THR_OD`
* `THR_ID`

Unsupported modules must fail clearly instead of emitting placeholder machining moves.

## Coordinate Rules

* `Z0` is the front face of the part.
* Negative `Z` moves into stock.
* Positive `Z` moves away from stock.
* `X` values are emitted as diameters.
* The converter emits `G7` so the local G7/G8 parser keeps X in diameter mode.
* Safe/retract X words are also diameter-mode X words. `CLR` is added to the emitted X diameter value; it is not doubled again as a radius clearance.
* Arc center offsets such as `I` stay in machine radius-space.

## Context

For each cycle, the caller resolves:

* active setup: last `SETUP` line above the cycle
* active tool: last `TOOL` line above the cycle

The tool line supplies fallback `S`, `R_FEED`, `FIN_FEED`, `R_DOC`, `FIN_DOC`, and display/comment metadata such as `T` and tool diameter. The converter also accepts the older long names as aliases.

## Preamble And Program Files

The single-line/run-now path emits a complete wrapper around one cycle:

```gcode
G21
G90
G18
G7
S800 M3
```

The spindle speed comes from the active tool or the cycle override when present.

The file generator uses a larger program pass:

* load and validate every cycle before opening the `.nc` output
* emit the modal setup once at the top of the file
* emit cycle comments and `S... M3` per cycle so tool/spindle context remains visible
* emit one final footer:

```gcode
M5
M30
```

This keeps generated files easier to inspect and avoids leaving a partial `.nc` when one later cycle has bad input.

## Supported Cycle Fields

### `OD`

Required fields:

* `D1` or `DIAMETER_1`
* `Z1` or `Z_1`
* `Z2` or `Z_2`
* `D2` or `DIAMETER_2`

Optional:

* `DT`, `D_TAPER`, or `TAPER_DIAMETER`
* `RND`, `ROUND`, or `RADIUS`
* `CHMF`, `CHAMFER`, or `C`
* `CLR`, `CLEAR`, or `TOOL_CLEARANCE`
* tool/feed/DOC overrides

Validation:

* `D1 > 0`
* `D2 > 0`
* `D2 <= D1`
* `DT <= D1` when present
* `Z2 <= Z1`
* `CLR >= 0`
* `RND` and `CHMF` are mutually exclusive
* corner amount must fit inside the Z span and the available diameter range

Without `DT`, OD finishes as a straight profile at `D2`. With `DT`, the finish profile runs from `DT` at `Z1` to `D2` at `Z2`, or to the corner tangent point when `RND`/`CHMF` is used. `RND` emits a quarter-radius corner at the end of the profile; `CHMF` emits a straight chamfer.

OD cornered rough passes use a pass-local retract diameter. After a chamfer/radius, the generator does not rapid X inward to the global safe diameter; it retracts to the current pass envelope plus clearance, then returns Z.

### `ID`

Required fields:

* `D1` or `DIAMETER_1`
* `Z1` or `Z_1`
* `Z2` or `Z_2`
* `D2` or `DIAMETER_2`

Optional:

* `DT`, `D_TAPER`, or `TAPER_DIAMETER`
* `RND`, `ROUND`, or `RADIUS`
* `CHMF`, `CHAMFER`, or `C`
* `CLR`, `CLEAR`, or `TOOL_CLEARANCE`

Validation:

* `D1 > 0`
* `D2 >= D1`
* `DT >= D1` when present
* `Z2 <= Z1`
* `CLR >= 0`
* `RND` and `CHMF` are mutually exclusive
* corner amount must fit inside the Z span and the available diameter range

Without `DT`, ID finishes as a straight profile at `D2`. With `DT`, the finish profile runs from `DT` at `Z1` to `D2` at `Z2`, or to the corner tangent point when `RND`/`CHMF` is used.

Tool `R_DOC` and `FIN_DOC` values are treated as magnitudes, matching the original conversational reference: negative entries are made positive before rough/finish pass planning.

Straight ID passes feed X back inward before returning Z, matching the original conversational reference. ID `RND`/`CHMF` passes leave the corner in Z first, then move X after the tool is out of the corner zone.

### `FACE`

Required fields:

* `Z` or `Z_2`

Optional:

* `D`, `OD`, or setup `OD`
* `Z1` or `Z_1`
* `DOC` or `ROUGH_DOC`
* `CLR`, `CLEAR`, or `TOOL_CLEARANCE`

Validation:

* face diameter must be greater than zero
* setup `ID` must be less than the face diameter
* `Z <= Z1`
* `DOC > 0`
* `CLR >= 0`

### `DRILL`

Required fields:

* `Z1` or `Z_START`
* `DEPTH`

Optional:

* `PECK`
* `FEED`
* `S`, `RPM`, or `SPINDLE_RPM`
* tool diameter from `TD`, `TOOL_DIAMETER`, `TOOL_DIA`, `DIA`, `DIAMETER`, or `D`

Validation:

* `FEED > 0`
* `PECK >= 0`
* target Z must be below `Z1`
* peck output is capped so very small pecks fail instead of blocking generation

Positive `DEPTH` is interpreted as distance from `Z1`; negative `DEPTH` is interpreted as an absolute target Z.

The first emitted drill comment includes resolved context when available, for example:

```gcode
(LC DRILL T3 D 6.000 Z1 0.000 Z -60.000 PECK 5.000 F 90.000)
```

### `CUT` / `PART`

Required fields:

* `D` or `DIAMETER`
* `Z`
* `WIDTH`

Validation:

* `D >= 0`
* `WIDTH >= 0`
* `D < SETUP.OD`
* `CLR >= 0`

### `GROOVE`

Required fields:

* `D1`
* `D2`
* `Z1`
* `Z2`
* `WIDTH`

Validation:

* `D2 < D1`
* `D2 >= 0`
* `Z2 <= Z1`
* `WIDTH >= 0`
* `CLR >= 0`

### `THR_OD` / `THR_ID`

Threading emits multi-pass `G33` moves for a metric 60 degree thread. The compact form is preferred:

```text
THR_OD|M{20}|P{1.5}|Z1{0}|Z2{-20}|DOC{0.3}|N{0}|ST{1}|CLR{1}
THR_ID|M{20}|P{1.5}|Z1{0}|Z2{-20}|DOC{0.3}|N{0}|ST{1}|CLR{1}
```

Fields:

* `M` - nominal metric diameter, for example `20` in `M20x1.5`
* `P` or `PITCH` - thread pitch, also used as the `G33 K` value
* `Z1` / `Z2` - start and end Z
* `DOC` - rough depth per pass when `N` is zero
* `N` - pass count; `0` means auto-calculate from `DOC`
* `ST` - pass strategy; `1` is degressive, `0` is linear
* `CLR` - X clearance diameter

Auto diameter rules:

* `THR_OD` starts from `M` and cuts to `M - 1.22687 * P`
* `THR_ID` starts from bore diameter `M - 1.08253 * P` and cuts to `M`

Manual overrides are still accepted for special cases: `D`, `D1`, `D2`, `THR_DEPTH`, `TAPER`, and `LEAD`.

## Error Handling

Generation stops on the first failing cycle. File generation preflights the whole `.lcam` before creating output, so bad geometry, unsupported cycles, malformed numbers, excessive pecks, or missing setup are reported with the LeanCam line number before a partial `.nc` is written. If storage rejects a write after the file is opened, the caller removes the incomplete `.nc` file.

## Host Safety Tests

A small host-side test harness exercises the supported cycles plus common bad inputs:

```powershell
gcc -std=c99 -Wall -Wextra -IuCNC/src/modules/leanCam `
  uCNC/src/modules/leanCam/tests/leancam_gcode_host_test.c `
  uCNC/src/modules/leanCam/leancam_gcode.c `
  -o $env:TEMP\leancam_gcode_host_test.exe
& $env:TEMP\leancam_gcode_host_test.exe
```

The tests check that normal OD/ID/FACE/DRILL/CUT/PART/GROOVE/THR_OD/THR_ID cycles emit output, while malformed numbers, missing setup, impossible geometry, unsupported cycles, excessive pecks, and oversized values fail cleanly. They also check the program-file wrapper so the shared header/footer path does not drift away from the single-cycle path.

## Output Policy

The current generator favors simple, readable, conservative G-code over advanced controller features. OD/ID taper, chamfer, radius corners, and simple G33 threading are implemented. CSS simulation, cutter compensation, canned threading cycles, and dedicated radius-only cycles are intentionally outside the current converter scope.
