# LeanCam — µCNC Conversational Lathe Mode

LeanCam is a lightweight, operator-focused conversational programming layer for µCNC.
It provides fast, keypad-driven lathe programming using structured text blocks instead of raw G-code.

---

## 1. Purpose

LeanCam is designed to:

* enable fast shop-floor programming
* minimize required inputs
* provide predictable and consistent behavior
* work within µCNC / GRBL-style constraints
* feel like a real conversational controller without full CNC complexity

LeanCam is intentionally small. It is not a full CAM system. It is a deterministic shop-floor programming layer.

---

## 2. Coordinate System (LOCKED)

### Z Axis

* `Z0` = front face of the part
* `Z negative` = into material, towards chuck
* `Z positive` = away from material

Example:

* front face → `Z 0`
* 50 mm into stock → `Z -50`

### X Axis

* Diameter mode
* `X = diameter`

### Stock Range

```text
Z range = [ +eL ... -L ]
```

### Safety Positions

* `X_safe = oD + safe`
* `Z_safe = +eL + safe`

---

## 3. UI Model

LeanCam uses conversational blocks:

1. Sticky setup block at the top
2. Cycle list
3. Draft/edit area
4. Simulation area, separate from text

G-code is derived from conversational blocks. The operator-facing representation is the LeanCam block list.

---

## 4. Setup Block

### Required

* `OUTER_DIAMETER`
* `LENGTH`

### Defaults

* `INNER_DIAMETER = 0`
* `CLAMP_LENGTH = 0`
* `EXTRA_LENGTH = 0`
* `TOOL_CLEARANCE = 1`
* `MATERIAL = ST45`
* `UNITS = mm`
* `WORK_OFFSET = G54`

Example:

```text
SETUP|LENGTH{45}|OUTER_DIAMETER{58}|INNER_DIAMETER{0}|CLAMP_LENGTH{0}|EXTRA_LENGTH{0}|TOOL_CLEARANCE{1}|MATERIAL{ST45}|UNITS{mm}|WORK_OFFSET{G54}
```

---

## 5. Default Resolution

LeanCam templates may contain default expressions during draft mode.

Supported expression forms:

```text
{(SETUP.FIELD)}
{(THIS.FIELD)}
{(literal)}
{}
```

Meaning:

* `{}` = required user input
* `{(0)}` = literal default
* `{(SETUP.FIELD)}` = default from setup block
* `{(THIS.FIELD)}` = default from current cycle/block

Example OD template:

```text
OD|DIAMETER_1{(SETUP.OUTER_DIAMETER)}|Z_1{(0)}|Z_2{(THIS.Z_1)}|DIAMETER_2{(THIS.DIAMETER_1)}|TOOL_CLEARANCE{(SETUP.TOOL_CLEARANCE)}
```

### Important Rule

Expressions are allowed only in draft/template state.
Saved `.lcam` program lines must contain resolved values only.

Example:

```text
Draft: OD|DIAMETER_1{58}|Z_1{-20}|Z_2{(THIS.Z_1)}|DIAMETER_2{(THIS.DIAMETER_1)}|TOOL_CLEARANCE{(SETUP.TOOL_CLEARANCE)}
Saved: OD|DIAMETER_1{58}|Z_1{-20}|Z_2{-20}|DIAMETER_2{58}|TOOL_CLEARANCE{1}
```

This keeps stored programs deterministic and parser-friendly.

---

## 6. Cycle Set

Initial visible cycle set:

* `OD`
* `ID`
* `FACE`
* `DRILL`
* `TAP`
* `CUT`
* `CHAMFER`
* `THREAD OD`
* `THREAD ID`

Current implementation focus:

* OD is the first cycle connected to G-code generation and µCNC streaming.
* FACE is the next likely simple cycle.
* Tool/material database is deferred.

---

## 7. Draft Entry Mode

When a cycle is selected, LeanCam enters draft mode.

### Behavior

* Draft line is shown but not committed
* First editable field is selected automatically
* Active field is highlighted
* Input is temporary until accepted
* Default expressions are displayed in user-friendly form
* Program row selection is hidden while draft mode is active

### Workflow

1. Select cycle
2. Draft appears
3. Enter value
4. Press `D` to accept field
5. Move to next field
6. Skip defaulted fields with `D`
7. Press `#` to commit cycle
8. Press `A` to cancel cycle
9. Press `*` for backspace/cancel behavior

### Numeric Input

Draft input supports:

* digits `0–9`
* `B` → toggle negative sign
* `C` → decimal point

Examples:

```text
-45
0.25
-12.5
26.
```

`26.` is ugly but valid and parses as `26`.

---

## 8. Input Model (Keypad)

The keypad is layered:

```text
raw keyboard scan → cam_key_t → UI/bridge action
```

The keyboard module reports which key was pressed.
The LeanCam bridge decides what the key means in the current mode.

### File Mode

```text
B / C → up/down
D     → open / NEW
A     → cancel
```

### New File Mode

```text
0–9   → name input
*     → backspace
D / # → create
A     → cancel
```

### Program Mode

```text
A     → file menu
B / C → line up/down
*     → delete line
D     → run/edit selected line
```

For OD lines, `D` currently triggers OD G-code generation/streaming.

### Draft Mode

```text
0–9 → numeric input
B   → toggle sign
C   → decimal point
D   → accept field
#   → commit cycle
*   → backspace / cancel
A   → cancel cycle
```

---

## 9. Field States

Each field can be:

* required
* defaulted
* inherited from setup
* inherited from the current cycle
* filled
* invalid

Saved fields should be values only:

```text
GOOD: Z_2{-20}
BAD:  Z_2{(THIS.Z_1)}
```

---

## 10. Field Highlighting

Active draft field is visually highlighted.

* Windows prototype → inverse colors
* Embedded UI → color/box highlight or text overlay

While editing:

* input buffer is shown live
* value is applied only after `D`
* draft line is displayed with friendly resolved defaults where possible

### Program Row Highlighting

During draft mode the previously selected program row must not remain highlighted.

Renderer/snapshot rule:

```c
highlight = (i == g_leancam_ui.cur_line) && (!g_leancam_ui.draft_active);
```

This keeps draft mode visually focused on the active field only.

---

## 11. Tool Behavior

Current temporary rule:

* OD generator may use temporary defaults such as `T1`, fixed spindle RPM, and fixed feed if no tool context exists yet.

Planned rule:

* first cycle requires/selects tool context
* subsequent cycles reuse tool automatically
* operator can override at any time
* tool + material determines feed, speed, DOC

Tool context should be added after setup is stable.

---

## 12. Material Handling

Material is defined in setup:

```text
MATERIAL{ST45}
```

Eventually tool + material will determine:

* feed
* speed
* DOC

Implementation strategy is deferred.

---

## 13. Validation

### Setup valid if:

* `OUTER_DIAMETER > 0`
* `LENGTH > 0`
* `INNER_DIAMETER < OUTER_DIAMETER`

### OD cycle valid if:

* setup exists
* `DIAMETER_1` is valid
* `DIAMETER_2` is valid
* `Z_1` and `Z_2` are valid
* final diameter does not exceed the intended stock/cycle logic
* required fields are resolved before commit

---

## 14. Display Model

* Setup block always visible
* Stock drawing reflects setup
* Simulation is separate from text
* Text rows are cleared with padded text where possible
* Renderer should remain dumb: it draws snapshot data and should not infer field logic

Draft display may hide expressions:

```text
Internal: Z_2{(THIS.Z_1)}
Display:  Z_2{-20}
```

Saved program lines must already be resolved.

---

## 15. Asset Mapping (LOCKED)

UI assets are derived from module name.

Example:

```text
OD → od.bmp
OD → od.ico
```

Rules:

* module name = text before first `|`
* filenames are lowercase
* no metadata fields required

---

## 16. Implementation Notes

LeanCam uses plain text as the schema.

* Conversational lines are plain text
* Parser is string-based
* No AST
* No field registry unless a later need proves it necessary
* Templates define structure and defaults
* Draft buffer is used before commit
* UI and logic are separated

### Current Architecture

```text
Template (.lcam-style string)
    ↓
Draft line with expressions
    ↓
Bridge / UI commit boundary
    ↓
Resolved saved .lcam line
    ↓
g_2_lcam parser / mapper
    ↓
Lathe cycle generator in C
    ↓
cam_stream
    ↓
µCNC parser / stream
```

### Important Boundary

```text
Draft = expressions allowed
Saved .lcam = values only
G-code generator = reads values only
```

---

## 17. Development Status

LeanCam currently provides:

* working parser model
* draft entry workflow
* setup-based defaults
* `THIS.FIELD` current-cycle defaults
* field navigation and editing
* correct draft field highlighting
* correct hiding of previous program-row selection during draft mode
* numeric input with sign and decimal point
* commit-time expression resolution
* deterministic saved `.lcam` lines
* first OD → G-code → µCNC stream path

Next steps:

* tighten OD generator details: roughing, finishing, DOC, feed rules
* add FACE cycle
* add tool context block
* add material/feed database
* improve simulation/preview
* file save/load polish

---

## 18. Philosophy

LeanCam follows:

* simplicity over completeness
* deterministic behavior over magic
* operator speed over configurability
* correctness before presentation
* input freedom, output determinism

Important internal rule:

```text
More layers only when they remove confusion.
No Python-style abstraction tables unless C actually benefits from them.
```

---

## 19. File Format

LeanCam uses a simple text-based `.lcam` format.

Example saved program:

```text
SETUP|LENGTH{45}|OUTER_DIAMETER{58}|INNER_DIAMETER{0}|CLAMP_LENGTH{0}|EXTRA_LENGTH{0}|TOOL_CLEARANCE{1}|MATERIAL{ST45}|UNITS{mm}|WORK_OFFSET{G54}
OD|DIAMETER_1{58}|Z_1{0}|Z_2{-40}|DIAMETER_2{45}|TOOL_CLEARANCE{1}
```

Rules:

* one block per line
* block name is before first `|`
* fields use `NAME{value}`
* saved lines must not contain `SETUP.` or `THIS.` expressions
* autosave happens on commit

---

## 20. Summary

LeanCam is a minimal but practical conversational layer that:

* reduces input to essentials
* keeps logic transparent
* enables fast real-world usage
* supports real lathe coordinates including negative Z
* resolves smart defaults before saving
* produces deterministic `.lcam` files
* can already execute the first OD cycle path as generated G-code

