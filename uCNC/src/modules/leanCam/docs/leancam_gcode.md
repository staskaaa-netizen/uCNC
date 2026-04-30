# LeanCam G-code Generation Rules

## 1. Purpose

This document defines how LeanCam conversational blocks are translated into generated G-code.

LeanCam does **not** treat G-code as the primary programming model.
The conversational program is the source of truth.
G-code is a derived execution form.

---

## 2. General Principles

LeanCam G-code generation should follow these rules:

* deterministic
* simple
* readable
* consistent with LeanCam coordinate model
* compatible with µCNC / GRBL-style limitations
* avoid unsupported advanced controller features

The first version should prioritize reliability over sophistication.

---

## 3. Coordinate Rules

### Z Axis

* `Z0` = front face of part
* `Z negative` = into stock
* `Z positive` = away from stock

### X Axis

* diameter mode
* generated X values are diameters unless internal implementation temporarily uses radius math

### Safety Convention

Unless explicitly overridden by cycle logic:

* `X_safe = setup OUTER_DIAMETER + TOOL_CLEARANCE`
* `Z_safe = setup EXTRA_LENGTH + TOOL_CLEARANCE`

---

## 4. Execution Context

At each cycle, the generator resolves:

### Active setup

Last `SETUP` line above current cycle

### Active tool

Last `TOOL` line above current cycle

### Active cut

Last `CUT` line above current cycle

These become the current execution context for that cycle.

---

## 5. Generation Stages

For each cycle:

1. parse cycle line
2. resolve inherited/default fields
3. validate geometry
4. validate setup/tool/cut dependencies
5. compute toolpath points
6. emit G-code block

---

## 6. Global Header / Startup

The first version should emit a minimal and stable startup block.

Suggested startup form:

```gcode
G21         ; metric, if UNITS=mm
G18         ; XZ plane
G90         ; absolute mode
G7          ; diameter mode, if supported
```

If inch mode is used:

```gcode
G20
```

If `G7` is not supported by the target, LeanCam must still internally assume diameter semantics and emit values accordingly to the chosen controller mode.

---

## 7. Tool / Cut Activation

When a cycle begins, generator may emit:

### Tool selection

```gcode
T1
```

if tool changed or if explicit tool emit is required.

### Spindle / feed setup

Depending on controller support:

```gcode
S1200
F0.25
M3
```

If CSS is supported later:

```gcode
G96 S180
```

If not supported, LeanCam may convert CUT settings into fixed RPM for first version.

### Recommendation for first version

Use simple fixed spindle speed / feed output if CSS is not fully supported in µCNC path.

---

## 8. SETUP Block Semantics

`SETUP` itself does not emit machining motion.

It defines stock and defaults for later cycles.

Fields used by generator:

* `LENGTH`
* `OUTER_DIAMETER`
* `INNER_DIAMETER`
* `CLAMP_LENGTH`
* `EXTRA_LENGTH`
* `TOOL_CLEARANCE`
* `MATERIAL`
* `UNITS`
* `WORK_OFFSET`

Typical non-motion output after setup may be:

```gcode
G54
```

if work offset output is desired.

---

## 9. TOOL Block Semantics

`TOOL` may emit tool selection if needed.

Example:

```gcode
T1
```

In first version, tool block mainly updates active context.

---

## 10. CUT Block Semantics

`CUT` usually does not emit motion by itself.

It updates active cutting parameters:

* rough feed
* finish feed
* rough speed
* finish speed
* rough DOC
* finish DOC

Generator uses these values in later cycles.

---

## 11. ODTURN Generation

## 11.1 Inputs

Resolved fields:

* `START_DIAMETER`
* `END_DIAMETER`
* `END_Z`
* `START_Z`
* `FILLET_RADIUS`
* `TOOL_CLEARANCE`

Context fields:

* setup
* tool
* cut

---

## 11.2 Validation

ODTURN valid if:

* setup exists
* tool exists
* `END_DIAMETER > 0`
* `START_DIAMETER >= END_DIAMETER`
* `END_Z <= START_Z`
* `TOOL_CLEARANCE >= 0`

---

## 11.3 First-Version Path Strategy

First version uses simple longitudinal turning without cutter compensation.

### Safe approach

```text
X = START_DIAMETER + TOOL_CLEARANCE
Z = START_Z + TOOL_CLEARANCE
```

### Roughing

Perform repeated passes from `START_DIAMETER` toward `END_DIAMETER`
using `ROUGH_DEPTH_OF_CUT`.

### Finishing

Perform final pass at `END_DIAMETER`
using finish feed / finish DOC assumptions.

---

## 11.4 Example Emission Pattern

Illustrative example only:

```gcode
; ODTURN
G0 X52 Z1
G1 Z-100 F0.25
G0 X48
G0 Z1
G1 Z-100
G0 X44
G0 Z1
G1 Z-100
G0 X42.5
G0 Z1
G1 X42 F0.12
G1 Z-100
G0 X52 Z1
```

Exact pass planning may vary, but the first version should remain simple and readable.

---

## 12. FACE Generation

## 12.1 Inputs

* `DIAMETER`
* `Z`
* `DEPTH_OF_CUT`
* `TOOL_CLEARANCE`

---

## 12.2 Validation

FACE valid if:

* setup exists
* tool exists
* target `Z` is within allowed setup range
* `DIAMETER > 0`

---

## 12.3 First-Version Path Strategy

Approach from safe positive Z, then feed across face.

Illustrative form:

```gcode
; FACE
G0 X52 Z1
G1 Z0 F0.20
G1 X0
G0 X52 Z1
```

For first version, center handling may simply go to `X0`.

---

## 13. IDBASIC Generation

## 13.1 Inputs

* `START_DIAMETER`
* `END_DIAMETER`
* `END_Z`
* `START_Z`
* `TOOL_CLEARANCE`

---

## 13.2 Validation

IDBASIC valid if:

* setup exists
* tool exists
* `END_DIAMETER >= START_DIAMETER`
* both diameters are positive
* `END_Z <= START_Z`

---

## 13.3 Path Strategy

Simple boring passes:

* safe approach outside hole start
* repeated boring passes using rough DOC
* finish pass at final diameter

---

## 14. IDEXT Generation

Same as `IDBASIC`, plus optional:

* `PILOT_END_Z`
* `FILLET_RADIUS`
* `FACING_DEPTH_OF_CUT`

For first version, unsupported extras may be ignored or reduced to simpler motion if needed.

Recommendation:

* support basic boring first
* add pilot/fillet later only when path logic is stable

---

## 15. GROOVE Generation

## 15.1 Inputs

* `DIAMETER`
* `Z`
* `WIDTH`
* `DEPTH_OF_CUT`
* `TOOL_CLEARANCE`

## 15.2 First-Version Strategy

Simple plunge-style groove:

* rapid to groove Z
* plunge to target diameter
* optional repeated widening passes later

For first build, groove may be implemented as a simple center plunge cycle.

---

## 16. DRILL Generation

## 16.1 Inputs

* `Z_START`
* `DEPTH`
* `PECK`
* `FEED`
* `SPINDLE_RPM`

## 16.2 First-Version Strategy

If peck is zero:

* simple drill feed to depth

If peck > 0:

* repeated peck sequence

Illustrative form:

```gcode
G0 Z1
G1 Z-10 F0.12
G0 Z1
G1 Z-20
G0 Z1
G1 Z-30
G0 Z1
```

Use simplest possible implementation first.

---

## 17. THREAD Generation

`THREAD_EXTERNAL` and `THREAD_INTERNAL` should be postponed until motion synchronization support is confirmed.

For now:

* parser may accept lines
* generator may refuse with clear message
* or emit placeholder comment only

Example:

```gcode
; THREAD_EXTERNAL not yet implemented
```

This is better than fake unsupported threading.

---

## 18. Feed / Speed Resolution

Resolution order:

1. explicit cycle override, if introduced later
2. active CUT block
3. tool/material derived fallback
4. compiled fallback constants

First version may ignore advanced material logic and use CUT directly.

## 18.1 Feed-per-Revolution Fallback for GRBL / µCNC

Some target controllers may not support native feed-per-revolution mode (`G95`).

In this case LeanCam must simulate feed-per-revolution using:

```text
feed_mm_per_min = feed_mm_per_rev × spindle_rpm
```

Example:

* desired feed = `0.25 mm/rev`
* spindle speed = `1200 rpm`

Then emitted feed becomes:

```text
F = 0.25 × 1200 = 300 mm/min
```

### Rules

* conversational values remain defined in machining terms (`mm/rev`)
* generator converts to controller feed units only at output stage
* if spindle RPM changes, feed must be recomputed
* this is a compatibility fallback, not true native feed-per-revolution support

### Implication

This fallback behaves correctly only as long as spindle RPM is treated as fixed for the emitted motion segment.

If RPM changes during motion and controller does not support true spindle-synchronized feed, LeanCam must split the motion into separate segments and recompute `F` for each segment.

---

## 18.2 Constant Surface Speed Fallback

Some target controllers may not support native constant surface speed (`G96`).

In this case LeanCam may simulate CSS by periodically recalculating spindle RPM from current diameter.

### CSS Formula

For metric mode:

```text
rpm = (1000 × css_m_per_min) / (π × diameter_mm)
```

For inch mode:

```text
rpm = (12 × css_ft_per_min) / (π × diameter_in)
```

### Example

* target CSS = `180 m/min`
* current diameter = `50 mm`

Then:

```text
rpm = (1000 × 180) / (π × 50) ≈ 1146 rpm
```

---

## 18.3 Simulated CSS Strategy

Because fallback controllers do not provide true continuous CSS, LeanCam must approximate it.

Possible strategy:

1. compute RPM at start diameter
2. emit a short motion segment
3. compute new diameter
4. update RPM
5. emit next segment

This creates stepped CSS behavior.

### Important Limitation

This is **not true G96 behavior**.

It is only an approximation using discrete RPM updates.

Accuracy depends on:

* step size
* spindle response time
* controller buffering
* machine inertia

---

## 18.4 Recommended First-Version Policy

For first reliable implementation:

### OD / ID longitudinal turning

Use one of these modes:

#### Mode A — Fixed RPM fallback

* compute RPM once from start diameter
* keep RPM constant through the whole pass
* simplest and safest first implementation

#### Mode B — Stepped CSS fallback

* divide pass into short Z segments
* recompute RPM for each segment
* recompute feed if feed-per-rev fallback is active
* more realistic, but more complex

Recommendation:

* implement Mode A first
* add Mode B only after core generator is stable

---

## 18.5 Combined CSS + Feed-per-Rev Fallback

If both are simulated:

1. compute RPM from current diameter
2. compute feed_mm_per_min = feed_mm_per_rev × RPM
3. emit segment with that `S` and `F`

For each new segment:

* new diameter → new RPM
* new RPM → new feed

This preserves approximate cutting conditions even on controllers without native `G95`/`G96`.

### Example

At diameter `50 mm`:

* CSS = `180 m/min`
* feed = `0.25 mm/rev`

Then:

```text
rpm ≈ 1146
feed ≈ 286.5 mm/min
```

At diameter `40 mm`:

```text
rpm ≈ 1432
feed ≈ 358.0 mm/min
```

So both `S` and `F` rise as diameter decreases.

---

## 18.6 Safety Limits

When simulating CSS, LeanCam must clamp RPM to machine or configured limits.

```text
rpm = min(calculated_rpm, max_allowed_rpm)
```

If RPM is clamped, feed-per-revolution fallback must use the clamped RPM, not the theoretical RPM.

This prevents impossible feed commands.

---

## 18.7 Philosophy

When controller support is missing:

* LeanCam keeps conversational intent in machining terms
* generator converts that intent into the closest safe controller-compatible form

This preserves usability without pretending unsupported controller features are truly available.


---

## 19. Error Handling

Generator must stop cycle generation if:

* required setup missing
* tool missing where needed
* invalid geometry
* invalid numeric conversion
* unsupported cycle feature used

Suggested output:

```gcode
; ERROR: ODTURN invalid END_DIAMETER
```

and mark cycle as failed in UI.

---

## 20. Output Style

Generated G-code should remain readable.

Recommended style:

* one comment header per cycle
* grouped setup lines
* stable ordering
* no unnecessary modal spam

Example:

```gcode
; --- ODTURN ---
T1
S1200
M3
G0 X52 Z1
G1 Z-100 F0.25
...
```

---

## 21. First-Version Scope

The first reliable G-code generator should support:

* `SETUP`
* `TOOL`
* `CUT`
* `ODTURN`
* `FACE`
* `IDBASIC`
* `DRILL`

Everything else may remain stubbed until the core is stable.

---

## 22. Out of Scope for First Version

Do not depend on:

* cutter nose compensation
* controller CRC
* advanced canned cycles
* full CSS if controller support is uncertain
* thread synchronization unless proven
* geometric transforms
* arbitrary contouring

---

## 23. Philosophy

LeanCam generation should be:

* explicit
* conservative
* inspectable
* easy to debug

The generated G-code is not meant to be clever.
It is meant to be correct, understandable, and compatible with the actual machine/controller.

---

## 24. Summary

LeanCam conversational blocks are the primary source.

G-code generation works by:

* resolving context
* validating geometry
* computing simple deterministic toolpaths
* emitting readable controller-friendly code

The first version should focus on a small stable set of cycles and only then expand.
