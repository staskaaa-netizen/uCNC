# G71/G72 Notes

This directory is currently a reference and staging area for LinuxCNC G71/G72
examples and for a possible standalone uCNC G7x generator module.

The files copied from LinuxCNC are not firmware code for uCNC. They are useful
for semantics, edge cases, and future tests.

Additional public reference:

- GCodeTutor's CNC lathe cycle overview:
  https://gcodetutor.com/cnc-machine-training/cnc-lathe-programming.html

The useful point from that page is the common lathe-cycle grouping:

- `G70`: finishing cycle, normally follows a roughing cycle and reuses the
  programmed contour.
- `G71`: rough turning cycle, cuts mainly along Z and leaves X/Z finishing
  allowance.
- `G72`: facing roughing cycle, analogous to G71 but cutting mainly along X.
- `G73`: pattern repeating roughing cycle, with X/Z material amounts, pass
  count, contour range, and finish allowances.
- `G75`, `G76`, `G83`, `G84`, `G87`, and `G88` are nearby lathe cycles, but
  they should not be dragged into the first G71/G72 split.

## Current State

The first support boundary now exists here:

- `src/modules/g71_g72/g71_g72.c`
- `src/modules/g71_g72/g71_g72.h`

This first pass owns common G7x result/cycle enums, modal context helpers,
boring source-row parsing/classification, contour element vocabulary, fixed
contour storage limits, and the small cycle profile table that names each
cycle's pass axis, cut axis, monotonic contour axis, and rough DOC word. The
active roughing generator still lives inside LeanCam:

- `src/modules/leanCam/leancam_gcode.c`
- `src/modules/leanCam/leancam_gcode.h`
- `src/modules/leanCam/tests/leancam_gcode_host_test.c`

LeanCam currently supports:

- G-code-ish source rows: `G71`, `G72`, contour `G0/G1/G2/G3`, and `G80`.
- conservative monotonic roughing checks.
- G71 stepped runtime output for demand-fed execution.
- G72 stepped runtime output for the same conservative monotonic subset.
- raw full emit paths for preflight/file generation.
- G76 stepping in the same generator file, but G76 is not part of this module
  boundary yet.

## Why Split G7x

G71/G72 has become real domain logic, not UI glue:

- contour ownership and validation
- contour element storage
- R/C corner expansion
- monotonic roughing checks
- roughing pass generation
- finish contour emission
- stepped runtime state

Keeping that inside `leancam_gcode.c` makes the LeanCam generator too broad.
The better long-term shape is a boring C helper module that LeanCam calls.

## Proposed First Split

Grow the existing small support module into a generator-only module:

```text
src/modules/g71_g72/g71_g72.c
src/modules/g71_g72/g71_g72.h
```

Keep it plain C:

- no file IO
- no UI
- no snapshots
- no LVDS
- no keypad/menu state
- no dynamic allocation
- caller-provided output callback
- fixed contour limits
- explicit stepper state object supplied by the caller

LeanCam would remain responsible for:

- finding the source range in the visible program
- reading source rows
- resolving setup/tool defaults
- deciding whether this is preview, preflight, file generation, or run stream
- feeding generated lines into `grbl_stream_readonly()`

The G7x module would own:

- parsing `G71/G72` cycle words
- collecting contour rows until `G80`
- validating the contour
- expanding R/C helper geometry where supported
- emitting rough/finish lines
- stepping one generated line at a time

## uCNC RS274 Reuse

Use uCNC's RS274 modal contract and modal snapshot API, not parser internals.

Useful public pieces:

- `G20/G21` and `G90/G91` encoding from `parser.h`. The G7x header mirrors
  that 0/1 encoding as `G7X_UNITS_*` and `G7X_DISTANCE_*` so leaf code does not
  need to include the whole parser/motion stack.
- `parser_get_modes()` when a caller wants to seed G7x from the live machine
  modal state.
- word/group defines such as `GCODE_WORD_X`, `GCODE_GROUP_UNITS`, and
  `GCODE_GROUP_DISTANCE` for shared naming and future validation.

Do not call or clone the live parser's private token/validation functions for
this module. They are stream-stateful, many are `static`, and LeanCam needs
offline source parsing for preview, preflight, file generation, and demand-fed
run streaming. G7x therefore keeps a tiny row reader for source rows and emits
ordinary `G0/G1/G2/G3` lines back through uCNC's normal parser/planner path.

The closest upstream uCNC model is the canned-cycle block executor:

```text
core/parser.c: parser_exec_command_block()
```

That code expands modal canned cycles by copying `parser_state_t`,
`parser_words_t`, and `parser_cmd_explicit_t`, changing the motion fields, and
calling `parser_exec_command()` for each generated move. It also owns sticky
cycle state such as retract and target depth.

That is a good future pattern for a true parser-level G7x implementation, but
it is intentionally not the first LeanCam split. G71/G72 currently needs a
multi-line contour between `G71/G72` and `G80`, plus preview/preflight/file
generation without executing motion. The present module should therefore stay
as a generator that emits normal lines. A later parser-level G7x can reuse the
canned-cycle style once uCNC has a clear contour storage contract.

## Not Yet

Do not turn this into a uCNC parser extension yet.

The current machine-safe path is:

```text
LeanCam source rows
  -> G7x generator emits ordinary G0/G1/G2/G3 lines
  -> uCNC parser/planner/motion own execution
```

A parser-level `G71/G72` module can be considered later, but it would need a
real contour storage and multi-line ownership contract inside uCNC. That is a
bigger change than this cleanup needs.

Do not implement `G70` or `G73` in the first extraction.

- `G70` needs contour reference/replay semantics. In the current LeanCam model
  the G71/G72 generator already emits the finish contour after roughing, so a
  standalone G70 finishing cycle would be a separate feature, not a prerequisite.
- `G73` is pattern repeating roughing, not the same scanline roughing model as
  current G71/G72. Keep it as future scope.
- `G75/G76/G83/G84/G87/G88` belong to a broader lathe-cycle family. G76 already
  has separate LeanCam thread stepping logic; it should not be mixed into this
  first contour-owned G7x extraction.

Do not copy the full LinuxCNC G7x pocket engine into LeanCam.

The LinuxCNC C++ reference normalizes both cycles by rotating/flipping the
profile: `G72` calls the same internal roughing routine as `G71` with a
90-degree rotation, and the path is then forced into one canonical direction.
It also handles pockets and richer offset geometry.

The current uCNC staging module borrows only the safe part of that idea: cycle
metadata is explicit, so `G71` and `G72` are described by axes instead of by
scattered string checks. The firmware generator still uses LeanCam's simpler
monotonic contour model.

LeanCam's current implementation is deliberately smaller:

- monotonic contour only
- no pocket roughing
- G71 roughs by X pass depth and feeds along Z
- G72 roughs by Z pass depth and feeds along X
- both full emit and stepped runtime choose the pass direction from the contour
  start side

## Suggested API Shape

Keep names neutral so LeanCam is only one caller:

```c
typedef int (*g7x_emit_fn)(const char *line, void *user);

typedef enum {
    G7X_OK = 0,
    G7X_BAD_FIELD,
    G7X_UNSUPPORTED,
    G7X_WRITE_FAILED
} g7x_result_t;

typedef struct g7x_region g7x_region_t;
typedef struct g7x_stepper g7x_stepper_t;

void g7x_region_reset(g7x_region_t *r);
g7x_result_t g7x_region_begin(g7x_region_t *r, const char *cycle_line,
                              const char *setup_line, const char *tool_line,
                              char *err, unsigned err_len);
g7x_result_t g7x_region_add_contour(g7x_region_t *r, const char *line,
                                    char *err, unsigned err_len);
g7x_result_t g7x_region_emit(const g7x_region_t *r, g7x_emit_fn emit,
                             void *user, char *err, unsigned err_len);

void g7x_stepper_reset(g7x_stepper_t *s);
g7x_result_t g7x_stepper_begin(g7x_stepper_t *s, const g7x_region_t *r,
                               char *err, unsigned err_len);
int g7x_stepper_next(g7x_stepper_t *s, char *out, unsigned out_len,
                     char *err, unsigned err_len);
```

The actual structs can stay fixed-size and opaque in the header, like
`lc_gcode_stepper_t`.

## Test Direction

Use the LinuxCNC examples here as reference cases, but convert tests to the
uCNC-supported source model:

```text
G71/G72 header
contour rows
G80
```

For now the acceptance gate should stay conservative:

- monotonic G71/G72 contours pass.
- unsupported roughing fails clearly.
- no finish-only fallback.
- no full generated run buffer is required for selected run execution.

Future test buckets after the split:

- `G70` contour finishing/replay: not implemented yet.
- `G73` pattern-repeat roughing: not implemented yet.
- LinuxCNC/GCodeTutor examples converted into the current `G71/G72 ... G80`
  source model where possible.
