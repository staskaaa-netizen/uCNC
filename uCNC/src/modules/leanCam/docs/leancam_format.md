# LeanCam File Format (`.lcam`)

## Overview

`.lcam` is a simple text format for LeanCam conversational lathe programs. Each non-empty line is one logical block, processed from top to bottom.

## Line Format

```text
MODULE|KEY{VALUE}|KEY{VALUE}|KEY{VALUE}
```

Example:

```text
OD|D1{50}|Z1{0}|Z2{-100}|D2{42}|CLR{1}
```

## Current Modules

The active converter currently supports:

* `SETUP` - stock and global defaults
* `TOOL` - active tool, spindle, feed, and DOC defaults
* `OD` - external turning
* `ID` - boring
* `FACE` - facing
* `DRILL` - center drilling
* `CUT` / `PART` - parting or simple cut
* `GROOVE` - simple groove
* `THR_OD` - outside metric threading using G33 passes
* `THR_ID` - inside metric threading using G33 passes

Other UI templates may exist, but generation should treat them as unsupported until `leancam_gcode.c` implements them.

## Fields

Required fields use an empty value or `*` while drafting:

```text
Z2{*}
```

Default values are stored in parentheses in templates:

```text
CLR{(1)}
MAT{(ST45)}
```

References are resolved when the draft is committed:

```text
D1{(SETUP.OD)}
```

After resolution:

```text
D1{50}
```

## Execution Context

The active context is resolved by line order:

* active setup = last `SETUP` above the cycle
* active tool = last `TOOL` above the cycle

Cycles inherit defaults from this context, then may override fields locally.

## Example Program

```text
SETUP|L{120}|OD{50}|ID{0}|CLAMP{0}|EXTRA{0}|CLR{1}|MAT{ST45}|WOFF{G54}
TOOL|T{1}|D{6}|S{800}|R_FEED{120}|FIN_FEED{60}|R_DOC{2.0}|FIN_DOC{0.5}
OD|D1{50}|Z1{0}|Z2{-100}|D2{42}|CLR{1}
FACE|D{42}|Z1{1}|Z{0}|DOC{1.0}|CLR{1}
THR_OD|M{20}|P{1.5}|Z1{0}|Z2{-20}|DOC{0.3}|N{0}|ST{1}|CLR{1}
```

## Editing Rules

* file order must be preserved
* unknown modules fail generation cleanly
* unknown fields are ignored by cycles that do not use them
* unresolved required values prevent draft commit or fail conversion

## Philosophy

`.lcam` is intentionally linear and easy to inspect. It is not G-code; it is the conversational source that LeanCam converts into G-code.
