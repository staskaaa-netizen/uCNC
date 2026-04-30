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

Unsupported modules must fail clearly instead of emitting placeholder machining moves.

## Coordinate Rules

* `Z0` is the front face of the part.
* Negative `Z` moves into stock.
* Positive `Z` moves away from stock.
* `X` values are emitted as diameters.
* The converter emits `G8` so the local G7/G8 parser keeps X in diameter mode.

## Context

For each cycle, the caller resolves:

* active setup: last `SETUP` line above the cycle
* active tool: last `TOOL` line above the cycle

The tool line supplies fallback `S`, `R_FEED`, `FIN_FEED`, `R_DOC`, and `FIN_DOC`. The converter also accepts the older long names as aliases.

## Preamble

Each emitted cycle currently starts with:

```gcode
G21
G90
G8
S800 M3
```

The spindle speed comes from the active tool or the cycle override when present.

## Supported Cycle Fields

### `OD`

Required fields:

* `D1` or `DIAMETER_1`
* `Z1` or `Z_1`
* `Z2` or `Z_2`
* `D2` or `DIAMETER_2`

Optional:

* `CLR`, `CLEAR`, or `TOOL_CLEARANCE`
* tool/feed/DOC overrides

Validation:

* `D1 > 0`
* `D2 > 0`
* `D2 <= D1`
* `Z2 <= Z1`
* `CLR >= 0`

### `ID`

Required fields:

* `D1` or `DIAMETER_1`
* `Z1` or `Z_1`
* `Z2` or `Z_2`
* `D2` or `DIAMETER_2`

Validation:

* `D1 > 0`
* `D2 >= D1`
* `Z2 <= Z1`
* `CLR >= 0`

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

Validation:

* `FEED > 0`
* `PECK >= 0`

Positive `DEPTH` is interpreted as distance from `Z1`; negative `DEPTH` is interpreted as an absolute target Z.

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

## Error Handling

Generation stops on the first failing cycle. The caller reports the line number plus the converter error text and removes the incomplete `.nc` file.

## Output Policy

The current generator favors simple, readable, conservative G-code over advanced controller features. Threading, chamfers, radius cycles, CSS simulation, and cutter compensation are intentionally outside the current converter scope.
