# G71/G72 and G76 Notes

See [TODO.md](TODO.md) for the audited completion checklist and module ownership.
See [TESTING.md](TESTING.md) for NC-independent tests and serial bench programs.

## Runtime status (2026-09-16)

Native inline `G71/G72 ... G80` and single-line `G76` now run through the
parser. Generated blocks finish being submitted before the next source command
is consumed. A pending-execution event runs outside the modifier callback so
nested blocks also receive G7/G8 conversion. Errors propagate to the source
command and clear the collector/runner. This is software-tested, pre-alpha
firmware; physical threading synchronization remains unverified.

Native cycles require `G18 G90 G94`. Both `G7/G8` and `G20/G21` are supported.
The conservative inline contour subset requires monotonic X and Z, including
arc interiors. G71/G72 still include their existing finishing pass. `G70` and
the two-line Fanuc/Haas cycle headers are not implemented; the one-line P/Q
range is described below.

### Fanuc headers and numbered P/Q ranges

Fanuc writes the roughing cycle as two blocks: the first carries the depth of
cut and the retract amount, the second the profile range and the finish
allowances. Both the two-line form and the one-line project spelling select the
profile by block number instead of an explicit `G80`:

```text
G71 U1 R1
G71 P100 Q200 U0.5 W0.25 F120
N100 G0 X50 Z0
G1 Z-30
N200 X40
```

```text
G71 U1 R1 P100 Q200 X0.5 Z0.5 F120
N100 G0 X50 Z0
G1 Z-30
N200 X40
```

Rules implemented by both the parser run path and the NC preview:

- The range starts at the block carrying `N(P)`. Earlier blocks are ordinary
  program text and still execute; they are not swallowed by the cycle.
- Unnumbered rows inside the range are contour rows, so only the first and last
  profile blocks need numbers.
- Numbered rows must increase and stay inside `[P, Q]`. The block carrying
  `N(Q)` is the last profile row and closes the range in place of `G80`.
- `G80` inside a numbered range, a repeated or decreasing number, a number
  above `Q`, an incomplete `P`/`Q` pair and `Q < P` all fail the source command
  and clear collection.

Two-line word meanings follow Fanuc: the first block's `U` (G71) or `W` (G72)
is the depth of cut and `R` the retract amount, while the second block's `U` and
`W` are the X and Z finish allowances. A second block of the same cycle
completes the open header only while no contour row has been collected, so an
unrelated repeat of `G71`/`G72` is still rejected. `X`/`Z` are accepted as the
project's spelling of the same allowances.

Numbered profile rows are retained in a bounded ring
(`G7X_MAX_RETAINED_BLOCKS`, `G7X_RETAINED_TEXT_LEN` in
`g7x_source.h`). `g7x_parser_numbered_history()` exposes it for diagnostics and
later replay. `g7x_history_visit_range()` walks a retained `[P, Q]` range and
reports `G7X_RANGE_MISSING` for a block that is absent or was evicted and
`G7X_RANGE_AMBIGUOUS` for duplicate numbers or a reversed range; it never
guesses a profile.

Callers that own program text can supply it through the `g7x_source_t` cursor
contract in `g7x_source.h`. NC implements it for its own documents with
`nc_emit_numbered_source()`, so G7x stays independent of NC storage while NC
keeps ownership of the file text.

Not implemented yet: `G70 P/Q` replay from the retained range, treating the
first `P` block as approach-only, and `S`/`T` words on profile rows (they
currently reject the row). Haas' single-line form, which puts the finish
allowances in `U`/`W` and the depth in `D`, is not supported: this parser stores
`D` and `Q` in the same word slot, so `D` cannot be used next to a `Q` range.

### Native G76 contract

With `G7X_ENABLE_G76` enabled (default) and `G33_ENCODER` configured, G76 expands
to ordinary G0 and G33 blocks. The G33 module must be loaded for execution.
Start at the thread crest X and thread start Z in the active work coordinates.
Required words are `X Z P Q F`: final thread X, end Z, radial thread height,
radial first cut, and lead per revolution. P must match the crest-to-final-X
height. Optional `R` is radial finishing allowance, `I` is radial taper, and
`L` is the number of spring passes (integer 0–255). Lengths use the active
G20/G21 units; P/Q/R/I remain radial in both G7 and G8.

For example, after `G18 G90 G94 G21 G7` and positioning at `X40 Z0`,
`G76 X36 Z-20 P2 Q1 F1.5 L1` generates three cutting passes and one spring pass.
Subsequent roughing cuts decrease by a factor of 0.75 with a minimum of Q/4.
At most 500 total passes are accepted. Clearance and lead-in/out need checking
against the actual workholding before machine use.

This is the project's single-line dialect. Packed two-line Fanuc parameters,
tool-angle infeed and chamfer are unsupported. Extended source-library aliases
such as `MIN_Q/QMIN`, `FINISH_R`, `D` taper and `H` spring passes are not native
parser words; use the native contract above. NC preview does not expand G76
yet and reports an explicit unsupported-cycle error.

### Regression checks

On Windows with MinGW GCC on PATH, run `python tools/test_g7x.py all`.
The suites cover the generator, NC preview adapter, and real virtual-MCU
parser/planner integration: ordering, G7/G8, inch/work-offset scaling, malformed
contours, generated failures, G76 passes, and NC Stop/Hold including queued
motion after source EOF. G33 is intercepted in the parser fixture; these tests
do not measure physical spindle phase or pitch accuracy. See
`../nc/TESTING.md` for the remaining machine checks.

This directory owns the standalone uCNC G7x generator/parser module for
`G71/G72` contour roughing and `G76` thread expansion. NC and LeanCam-style
screens may consume it for preview, but the module must be able to run without
any NC UI module loaded.

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
- `G75`, `G83`, `G84`, `G87`, and `G88` are nearby lathe cycles, but they
  should not be dragged into the first contour roughing split.

## Current State

The first support boundary now exists here:

- `src/modules/g7x/g7x.c`
- `src/modules/g7x/g7x.h`
- `src/modules/g7x/g7x_contour.c`
- `src/modules/g7x/g7x_contour.h`

This module owns common G7x result/cycle enums, modal context helpers, boring
source-row parsing/classification, contour element vocabulary, fixed contour
storage limits, and the small cycle profile table that names each cycle's pass
axis, cut axis, monotonic contour axis, and rough DOC word.

The module also exposes a stepped stream generator. NC SIM/preview may use that
API directly, but it is an optional consumer path, not the execution owner.
G76 has a separate `g7x_thread_stream_t` that expands one parsed/source G76 row
into safe moves, pass comments, `G33` thread blocks, and final retract moves.

Parser integration is the modal-region owner:

- `G71` / `G72` are recognized as parser extension headers.
- `U` / `W` rough-depth header words are accepted for those headers.
- the header initializes a parser-side `g7x_stream_t`.
- while a G7x region is active, parsed contour `G0`/`G1`/`G2`/`G3` rows are
  stored into that stream and suppressed so they do not accidentally execute as
  normal motion.
- parsed line `R` round and `C` chamfer metadata are preserved for the contour
  where uCNC exposes those words to the module.
- `G80` ends and prepares the active parser-side region.
- after `G80`, generated rough/finish rows execute as parsed motion blocks via
  the parser generated-block helper, following the same broad model as canned
  cycles.

Current parser integration limitations:

- parser-side bad-contour/status reporting is still coarse.
- generated block failure paths need more machine-side testing.
- NC SIM/preview still uses the stepped stream API directly. That is allowed as
  preview glue, but NC must not grow a second G71/G72 generator.

LeanCam still has older local generator code in:

- `src/modules/leanCam/leancam_gcode.c`
- `src/modules/leanCam/leancam_gcode.h`
- `src/modules/leanCam/tests/leancam_gcode_host_test.c`

LeanCam currently supports:

- G-code-ish source rows: `G71`, `G72`, contour `G0/G1/G2/G3`, and `G80`.
- conservative monotonic roughing checks.
- G71 stepped runtime output for demand-fed execution.
- G72 stepped runtime output for the same conservative monotonic subset.
- raw full emit paths for preflight/file generation.
- older G76 stepping in the same generator file, kept as a compatibility
  reference until LeanCam is fully rewired to the G7x module.

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
The better long-term shape is this boring C helper/parser module, with any UI
layer acting only as a caller or preview consumer.

## Native Parser Direction

Grow the existing small support module into a parser-owned modal cycle:

```text
src/modules/g7x/g7x.c
src/modules/g7x/g7x.h
src/modules/g7x/g7x_contour.c
src/modules/g7x/g7x_contour.h
```

Keep it plain C:

- no file IO in the geometry/generator core
- no UI
- no snapshots
- no LVDS
- no keypad/menu state
- no dynamic allocation
- fixed contour limits
- one parser-side active region collector
- explicit stepper state object supplied by parser RUN or NC preview

NC remains responsible for:

- showing/editing source files
- streaming the selected source file to uCNC
- drawing preview from the shared G7x generator while parser stream injection is
  still being finished

The G7x module would own:

- accepting `G71/G72` cycle headers from parser hooks
- collecting parsed contour rows until `G80`
- validating the contour
- expanding R/C helper geometry where supported
- feeding generated rough/finish blocks through parser execution
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

Do not clone the live parser's private token/validation functions for parser
RUN. Parser RUN should capture already-parsed words from uCNC hooks and emit
ordinary generated `G0/G1/G2/G3` blocks through uCNC's normal parser/planner
path.

The source-row reader stays only as a temporary offline/preview adapter and as
a small host-test helper. It must not grow into a second runtime parser.

The closest upstream uCNC model is the canned-cycle block executor:

```text
core/parser.c: parser_exec_command_block()
```

That code expands modal canned cycles by copying `parser_state_t`,
`parser_words_t`, and `parser_cmd_explicit_t`, changing the motion fields, and
calling `parser_exec_command()` for each generated move. It also owns sticky
cycle state such as retract and target depth.

That remains useful as a reference for generated execution, but G71/G72 needs a
multi-line contour between `G71/G72` and `G80`. The live implementation should
therefore behave like a small modal region plus generated parser blocks, not
like a single G33-style motion command.

The intended machine path is:

```text
NC source file
  -> uCNC parser sees G71/G72 and opens G7x region
  -> parsed contour rows are captured and suppressed
  -> G80 closes the region
  -> G7x generated blocks execute ordinary G0/G1/G2/G3 motion
  -> uCNC parser/planner/motion own execution
```

Do not implement `G70` or `G73` in the first extraction.

- `G70` needs contour reference/replay semantics. In the current LeanCam model
  the G71/G72 generator already emits the finish contour after roughing, so a
  standalone G70 finishing cycle would be a separate feature, not a prerequisite.
- `G73` is pattern repeating roughing, not the same scanline roughing model as
  current G71/G72. Keep it as future scope.
- `G75/G83/G84/G87/G88` belong to a broader lathe-cycle family and remain
  future scope. G76 is intentionally separate from the contour stream because it
  expands to thread passes rather than contour rough/finish motion.

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
- basic G76 OD and ID/taper thread expansion passes.
- no finish-only fallback.
- no full generated run buffer is required for selected run execution.

Future test buckets after the split:

- `G70` contour finishing/replay: not implemented yet.
- `G73` pattern-repeat roughing: not implemented yet.
- LinuxCNC/GCodeTutor examples converted into the current `G71/G72 ... G80`
  source model where possible.
