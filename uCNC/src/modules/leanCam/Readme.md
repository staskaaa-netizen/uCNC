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

---

## 2. Coordinate System (LOCKED)

### Z Axis

* `Z0` = front face of the part
* `Z negative` = into material (towards chuck)
* `Z positive` = away from material

Example:

* front face → `Z 0`
* 50 mm into stock → `Z -50`

---

### X Axis

* Diameter mode
* `X = diameter`

---

### Stock Range

```
Z range = [ +eL ... -L ]
```

---

### Safety Positions

* `X_safe = oD + safe`
* `Z_safe = +eL + safe`

---

## 3. UI Model

LeanCam uses conversational blocks:

1. Sticky setup block (top)
2. Cycle list
3. Draft/edit area
4. Simulation area (separate)

G-code is optional and derived.

---

## 4. Setup Block

### Required

* `oD` — outer diameter
* `L` — length

### Defaults

* `iD = 0`
* `cL = 0`
* `eL = 0`
* `mT = ST45`
* `safe = 2`

---

## 5. Default Resolution (LOCKED)

Fields referencing setup are resolved **during draft creation**.

Example:

```
D1{(SETUP.OD)}
-> D1{60}
```

---

## 6. Cycle Set (Initial)

* OD turn
* Face
* Groove
* Drill
* Bore

---

## 7. Draft Entry Mode (CORE BEHAVIOR)

When a cycle is selected, LeanCam enters **draft mode**.

### Behavior

* Line is shown but not committed
* First required field is selected automatically
* Active field is highlighted
* Input is temporary until accepted

---

### Workflow

1. Select cycle
2. Draft appears
3. Enter value
4. Press `ENTER` → accept field
5. Move to next field
6. Skip optional fields with ENTER
7. Press `END (#)` → commit
8. Press `ESC (*)` → cancel

---

## 8. Input Model (Keypad)

* digits → numeric input
* `ENTER` → accept field
* `#` → commit block
* `*` → cancel

Navigation:

* `8 / 2` → up / down
* `4 / 6` → field navigation

---

## 9. Field States

Each field can be:

* required
* defaulted
* inherited
* filled
* invalid

---

## 10. Field Highlighting

Active field is visually highlighted.

* Windows prototype → inverse colors
* Embedded UI → color/box highlight

While editing:

* input buffer is shown live
* value applied only after ENTER

---

## 11. Tool Behavior

* First cycle requires tool
* Subsequent cycles reuse tool automatically
* Operator can override at any time

---

## 12. Material Handling

Material is defined in setup (`mT`).

Tool + material determines:

* feed
* speed
* DOC

Implementation strategy is deferred.

---

## 13. Validation (Initial)

### Setup valid if:

* `oD > 0`
* `L > 0`
* `iD < oD`

### OD cycle valid if:

* setup exists
* tool exists
* final diameter ≤ stock diameter

---

## 14. Display Model

* Setup block always visible
* Stock drawing reflects setup
* Simulation separate from text

### Preview / sanity view

The simulation area is the operator sanity view. It should show what LeanCam understood from the current line, not try to become a full CAM backplotter.

Current intent:

* setup defines stock length, OD, clamp, and extra length
* selected cycle defines the highlighted material or drill path
* active tool can affect preview, for example drill diameter
* drill tool `D` is a diameter value, while the half-section preview draws its radius

For larger programs, setup and tool context need to become more visible while scrolling:

* selected cycles should resolve against the latest setup and tool above them
* current setup/tool should stay sticky or easy to see
* all cycle lines should be scrollable without losing that context

This is deferred until the small-program workflow is stable.

---

## 15. Asset Mapping (LOCKED)

UI assets are derived from module name.

Example:

```
OD -> od.bmp   (helper image)
OD -> od.ico   (button icon)
```

Rules:

* module name = text before first `|`
* filenames are lowercase
* no metadata fields required

---

## 16. Implementation Notes

* Conversational lines are plain text
* Parser is string-based
* No AST or complex structure
* Draft buffer used before commit
* UI and logic are strictly separated

---

## 17. Development Status

LeanCam currently provides:

* working parser model
* draft entry workflow
* setup-based default resolution
* field navigation and editing
* UI highlighting

Next steps:

* file save/load
* G-code generation
* tool/material database
* ESP32 UI integration

---

## 18. Philosophy

LeanCam follows:

* simplicity over completeness
* deterministic behavior over magic
* operator speed over configurability

---

## 19. Summary

LeanCam is a minimal but practical conversational layer that:

* reduces input to essentials
* keeps logic transparent
* enables fast real-world usage

## File Format

LeanCam uses a simple text-based `.lcam` format.

See:
- docs/leancam_spec.md
- examples/ for real programs

## FILE MODE
B/C → up/down
D → open / NEW
A → cancel
## NEW FILE
0–9 → name
* → backspace
D / # → create
A → cancel
## PROGRAM MODE
A → file menu
B/C → line up/down
* → delete line
D → edit (later)
## Cycles
0 TOOL
1 OD
2 ID
3 FACE
4 DRILL
5 TAP
6 CUT
7 CHAMFER
8 THREAD OD
9 THREAD ID
## DRAFT MODE
0–9 → input
D → accept field
# → commit cycle
* → backspace / cancel
A → cancel cycle
B/C → prev/next field (later)
## RULES
# = finish
* = back/delete
autosave on commit
3×3 menu only in program/draft


