# LeanCam / µCNC — Refactor + Real G-code Roadmap
## 0 step: 

µCNC project rule — dead simple first

For this project, prefer boring, readable, minimal solutions over modern/fancy architecture.

Before adding any abstraction, framework, task system, event bus, dynamic allocation, template magic, generic engine, or clever pattern, ask:

1. Can this be one plain C file?
2. Can this be one struct + few functions?
3. Can this be a fixed-size array?
4. Can this be drawn directly instead of modeled twice?
5. Can this be parsed line-by-line instead of building an AST?
6. Can this be solved with explicit states instead of generic callbacks?
7. Can this be debugged from serial print output?

Default style:

- plain C
- static buffers
- fixed limits
- explicit state machines
- simple enums
- readable names
- direct data flow
- few layers
- no hidden magic
- no “future-proofing” unless needed now
- no clever compression of logic
- no new dependency unless clearly justified
- no malloc/free in controller/runtime paths
- no background task unless timing demands it
- no generic UI engine if fixed screens are enough

Preferred answer format:

1. simplest workable design
2. files touched
3. small patch
4. test steps
5. risks

Forbidden tendency:

Do not redesign µCNC into a modern application framework.
Do not invent a second operating system.
Do not hide simple machine logic behind fashionable patterns.

Engineering target:

A tired operator, with serial logs and source code open, must be able to understand what happens.
If the code needs a diagram to explain, it is probably too clever.

Project philosophy:

Less architecture. More visible behavior.
Less abstraction. More determinism.
Less magic. More repairability.

Upstream/module configuration rule:

* `cnc_hal_overrides.h` must always remain active. Do not add guards or switches that disable it.
* Treat `cnc_hal_overrides.h` and web-builder generated overrides as the normal place for user/module configuration.
* Core µCNC may expose generic hooks, APIs, and low-cost infrastructure, but must not learn board-specific or module-specific policy.
* Architecture-specific implementations such as ESP32 PCNT, RP2040/RP2350 PIO, display backends, storage backends, and G-code extensions belong in modules unless upstream explicitly asks for core support.
* Module-specific enable flags, default pins, backend enum names, debug toggles, and compatibility aliases belong in the module source/README or in user overrides, not in `cnc_hal_config_helper.h`.
* New optional behavior must be opt-in at compile time. Keep the default/no-feature path light in RAM, flash, and task hooks.
* Prefer generic `ENCx`/indexed APIs over `ENC0`-only helpers. If a feature is useful for one encoder, design the public API so it can apply to any encoder.
* If a module needs core support, add the smallest generic primitive and document the module contract. Do not smuggle a private board patch into core.
* During PR cleanup, scan for suspicious config glue: forced `ENABLE_*` defines, backend names in core, board-specific constants, and one-off compatibility macros.

## Main decision

King is dead. Long live the king: **G-code**.

LeanCam should stop storing private pipe-language rows like:

```text
G71|U{2}|R{1}|X_ALLOW{0.5}|Z_ALLOW{0.5}|F{120}
```

and move toward real-world G-code-ish text:

```gcode
(OD)
G71 U2 R1 X0.5 Z0.5 F120
    G1 X50 Z0
    G1 X25 Z-25
    G1 X50
G80
```

Extra abstraction is allowed only when it gives real UI value:

* keypad presets
* field editing
* preview metadata
* generated helper rows during insertion
* tool/table defaults

Stored program should stay as close to recognizable G-code as possible.

---

# DO NOW

## -3. Big priority: stop caching and pacing where uCNC already does it

Status: mostly done / keep watching.

Problem:

* We are shooting ourselves in the leg by stacking LeanCam caches and pacing
  layers on top of the uCNC stream/planner path.
* uCNC already has the meaningful runtime cache and reasonable execution
  pacing: serial/stream input, parser, planner, motion buffers, and status.
* LeanCam previously added extra program caching, generated stream staging,
  direct stream pacing, step-stream pacing, preview pacing, and execution-controller
  pacing. These layers can disagree, stall display work, hide the real owner of
  runtime state, and make HSTX failures look like graphics bugs.

Rule:

* Do not cache NC/program text unless a screen needs those exact visible lines.
* Do not generate a full expanded run buffer just to run a selected cycle.
* Do not add another pacing layer before `execution_controller`.
* Runtime should emit the next needed source/generated NC line only when uCNC is
  ready to consume it.
* Let uCNC own run buffering and motion pacing. LeanCam should be a source of
  lines, not a second controller.

Target:

* NC viewer reads visible lines from file.
* Preview reads only the small region it needs.
* Selected G71/G72/G76 runtime uses a stateful line generator that emits one
  generated line at a time.
* Full/from-file run streams raw NC file lines through uCNC without full-file
  LeanCam caching.
* Execution controller only orders UI work; it must not become another runtime
  scheduler for G-code execution.

Progress:

* Removed the old `cam_stream` alternate task/queue executor.
* Removed the staged LeanCam direct-stream text buffer path.
* Selected generated runs now use the `leancam_gcode_stepper_*()` demand-fed
  line source.
* Raw NC file runs now read the next source line from the file only when the
  uCNC stream asks for it.
* Bridge runtime now always uses the boring `grbl_stream_readonly()` line
  source. The old target switch and selected-run temp-file fallback were
  removed.
* HSTX health/recovery/torture scaffolding was removed from the live release
  path. The findings remain in the LVDS README as warnings/history only.

Plan B if NC run-view keeps fighting the system:

* Drop the special NC run-view executor entirely.
* File manager `#` means: expand the selected source file into a temporary run
  file, then run that file through the normal uCNC file-run path.
* UI flow is:
  * select source `.nc`
  * press `#`
  * LeanCam expands only as a preparation step
  * uCNC runs the prepared file
* LeanCam does not stream generated runtime lines itself in this mode.
* LeanCam does not own line pacing in this mode.
* The prepared file can be inspected/reused/deleted like any other NC artifact.
* This is less clever and may be the safer machine behavior: one file in, one
  expanded run file out, then uCNC owns execution.

## -2. NC file-manager run view

Status: implemented / needs machine-side use feedback.

Decision:

* File-manager `#` is an NC run action, not a LeanCam generator shortcut.
* `#` opens a read-only NC run/view screen for `.nc`, `.ngc`, and `.gcode`.
* The NC run screen reuses the split preview layout but does not allow code editing.
* `B`/`C` move the selected line.
* Footer run modes are explicit:
  * `1 Single` - run current line, then advance selection.
  * `2 From` - run from current line to EOF.
  * `3 Full` - run the full selected file.
  * `# Run` - execute the selected mode.
* The selected run mode is highlighted with a yellow footer button background.
* Raw NC execution must stay raw NC; do not pass file-manager runs through LeanCam cycle expansion.

Runtime generator note:

* Do not use the file/export generator as a fake stepper by replaying it and
  skipping already emitted lines.
* Runtime run needs a side generator with persistent state: each request emits
  the next NC line and returns.
* The current first pass is `leancam_gcode_stepper_*()` for selected LeanCam
  runs. It keeps the original burst generator available for NC file generation
  and moves selected G71 runtime output to one generated line at a time.

## -1. execution_controller owns LeanCam/LVDS runtime order

Status: done / preserve.

Decision:

* `cnc_io_dotasks` calls one visible runtime owner: `execution_controller_poll()`.
* The controller owns deterministic order:
  * poll keypad input
  * feed keys to LeanCam bridge
  * tick LeanCam bridge
  * build/export UI snapshot
  * render pixels
* `lvds_renderer.c` must remain a pixel consumer.
* `lvds_renderer_state.c` must not secretly poll input or advance LeanCam.
* No module may secretly advance another module.

Progress:

* Added `execution_controller.c/h` under `leanCam`.
* Moved keypad polling and `leancam_bridge_tick()` out of renderer state.
* Changed LVDS boot listener so `cnc_io_dotasks` calls `execution_controller_poll()` only.
* Moved LeanCam runtime snapshot, screen renderer, and LVDS UI helper files from `lvds_renderer` into `leanCam`.
* Collapsed the unused `lvds_hw_renderer` wrapper; `lvds_draw_api` now calls HSTX primitives directly.
* Removed the visual-side generated G-code cache and PSRAM state-save path.
* Fullscreen/generated sim preview now draws only bridge-owned stepper lines
  copied through the snapshot.
* Removed execution-controller HSTX health/autorecover hooks; the controller now
  orders input, bridge tick, snapshot, visual draw, and present only.

Standing rule:

* Keep preview generation incremental and bridge-owned, then let renderer draw
  already prepared snapshot data only.

## 0. NC is the primary file/editor format

Status: mostly done / SETUP replacement still pending.

Decision update:

* NC viewer/editor becomes the main user-facing program view.
* `.lcam` is no longer a supported program format.
* Future files should be normal `.nc` files.
* LeanCam presets still exist, but they insert/expand into NC text.
* The generator should not be "LCAM -> NC" as the final model.
* The generator/preprocessor should prepare an expanded NC run stream from an NC source file.

Meaning:

```text
editable source:      .nc
visible program:      .nc
run preparation:      expanded NC stream
old .lcam support:    none; no fallback/import path
```

Rules:

* Do not add new `.lcam` features or import/fallback behavior.
* Do not design new UI flows around converting one saved `.lcam` file into a separate saved `.nc` file.
* Preset expansion writes real G-code-ish NC rows directly.
* The NC viewer becomes the normal editor surface.
* "Generate" means prepare expanded NC for sim/run, not create a second permanent file by default.

Progress:

* file-list finish/prepare action now loads and preflights the selected `.nc` only; it no longer writes a permanent `_expanded.nc`
* removed the unused `_expanded.nc` output-name builder
* file view is NC-only; the old all-files/expanded-file viewer fallback was removed
* run preparation no longer uses a generic callback/context table; `leancam_run.c` owns the plain line-by-line rules directly

---

## 1. Resource / bus owner

Status: partially done / keep small.

Create:

```text
leancam_resource.c/h
```

Purpose: one central gatekeeper for shared resources.

Owns:

* file IO critical sections
* autosave permission
* PSRAM allocation/regions
* LVDS/HSTX frame access
* snapshot handoff timing
* renderer scratch ownership

Rules:

* no module calls `cnc_set_file_io_critical()` directly except resource owner
* no scattered PSRAM offsets
* no renderer grabbing frame buffers directly
* file save/load goes through resource gate
* bridge/app asks resource owner before expensive or unsafe work

Example API:

```c
bool lc_resource_file_begin(const char *tag);
void lc_resource_file_end(void);

bool lc_resource_can_autosave(void);

void *lc_resource_psram_region(enum lc_psram_region id, size_t size);

bool lc_resource_frame_begin(void);
void lc_resource_frame_end(void);
```

Goal: stop file/PSRAM/LVDS/HSTX/realtime loop fighting.

Progress:

* `leancam_resource.c/h` exists and owns file critical sections, autosave
  permission, and fixed PSRAM regions.
* HSTX frame access is now through draw/present APIs only; no bridge or renderer
  direct-scanout fallback remains.

Still not done:

* Frame begin/end hooks are not a real separate API yet. Keep it that way unless
  a concrete conflict appears.

---

## 2. Split bridge monster

Status: partially done / still the largest remaining architecture debt.

Current `leancam_bridge.c` became the application kernel. It must become thin.

Final bridge responsibility:

* `leancam_bridge_init`
* `leancam_bridge_tick`
* `leancam_bridge_handle_key`
* `leancam_bridge_request_render`
* `leancam_bridge_fill_snapshot`

Move out:

```text
tool catalog      -> leancam_tool_catalog.c/h
preset expansion  -> leancam_presets.c/h
G71/G72 regions   -> leancam_regions.c/h
validation        -> leancam_validate.c/h
app state         -> leancam_bridge.c
file browser      -> merged into leancam_files.c/h
preview model     -> leancam_preview.c/h
```

Bridge must not:

* parse G71
* expand OD presets
* own tool catalog arrays
* own PSRAM offsets
* own file browser logic
* generate G-code paths

Progress:

* resource owner extracted: `leancam_resource.c/h`
* tool catalog extracted: `leancam_tool_catalog.c/h`
* G71/G72 region helpers extracted: `leancam_regions.c/h`
* preset expansion extracted: `leancam_presets.c/h`
* validation extracted: `leancam_validate.c/h`
* app mode/catalog state merged back into bridge; the tiny `leancam_app.c/h` wrapper was removed.
* file browser and prompt state merged back into `leancam_files.c/h`
* NC viewer state extracted: `leancam_nc_viewer.c/h`
* editor field/draft mechanics extracted: `leancam_editor.c/h`
* autosave state merged back into bridge; the tiny `leancam_autosave.c/h` wrapper was removed.
* sim arm state, selected-range emission, whole-program preflight, and expanded NC emission extracted: `leancam_run.c/h`
* path/name helpers merged into `leancam_files.c/h`
* snapshot row/text formatting extracted: `leancam_snapshot.c/h`
* snapshot frame reset/clear defaults moved to `leancam_snapshot.c/h`
* draft field accept/commit resolution started moving from bridge to `leancam_editor.c/h`
* removed the no-op run validation callback path; run preparation now relies on plain setup/tool lookup plus generator errors
* removed a few bridge-only wrapper functions that only forwarded to the shared text/tool helpers
* bridge runtime defaults moved into `leancam_templates.h` so templates/configuration start living in one place.
* removed the generic run context callbacks; bridge now calls the run helpers directly
* preset expansion no longer uses a setup-field callback; bridge passes the current `SETUP` line and presets parse it directly
* bridge run-stream debug scaffolding is compiled out unless bridge serial debug
  is enabled.
* old direct-stream naming was removed from active bridge code.

Still not done:

* `leancam_bridge.c` is still too large and still owns app orchestration.
* Bridge still starts expanded NC preparation from file-browser state, but run preflight/emission now lives in `leancam_run.c/h`.
* menu callbacks still live in bridge.
* snapshot/program display still lives in bridge.
* draft commit still has bridge-side validation, mode changes, autosave, and preset expansion glue.

---

## 3. Menu / editor / preset separation

Status: partially done.

Separate these hard:

```text
schema  = page/button definitions
menu    = key routing and dispatch
editor  = rows/draft editing
presets = OD/ID/FACE/RECESS expansion
```

`leancam_schema.c`:

* declarative page data only
* key labels
* action IDs
* no business logic

`leancam_menu.c`:

* translate key
* lookup schema action
* call callback
* no template construction
* no machining logic

`leancam_editor.c`:

* current row
* draft row
* field cursor
* input buffer
* insert/delete/move/edit
* visual wrapping state if needed

Progress:

* schema is mostly declarative.
* menu routing exists in `leancam_menu.c/h`.
* draft field accept and commit-time default resolution now live in `leancam_editor.c/h`.
* setup/tool/catalog/template/preset ID to text/action mapping now lives with `leancam_templates.h`; bridge only performs draft-vs-preset insertion.
* draft commit no longer runs tool validation side effects before save.
* draft commit resolve/normalize mechanics moved into `leancam_editor.c/h`.
* menu action table is now complete/static instead of rebuilt per key for NC-view callbacks.
* removed stale schema template shortcut; menu now uses normal schema page/key lookup only.
* More/Other template page state has explicit menu accessors and is reset on app init/back-to-files.

Still not done:

* bridge still owns the menu callback table.
* bridge still owns draft commit mode/autosave/preset side effects.

`leancam_presets.c`:

* OD/ID/FACE/RECESS buttons
* inserts real G-code-ish blocks

---

## 4. Stored format becomes real G-code-ish

Status: mostly done.

Replace pipe storage for program core.

Old:

```text
G1|X{50}|Z{0}|C{0}|R{0}
```

New stored form:

```gcode
G1 X50 Z0
```

Old:

```text
G71|U{2}|R{1}|X_ALLOW{0.5}|Z_ALLOW{0.5}|F{120}
```

New:

```gcode
G71 U2 R1 X0.5 Z0.5 F120
```

Meaning:

* `U` = rough DOC for G71
* `W` = rough DOC for G72
* `R` = retract
* `X` = X finish allowance
* `Z` = Z finish allowance
* `F` = rough feed

Use normal words where possible:

* `G1 X.. Z.. C.. R..`
* `G2 X.. Z.. R..`
* `G3 X.. Z.. R..`
* `G33 X.. Z.. K..`
* `G76 X.. Z.. K.. DEPTH.. DOC.. FIN.. SPRING.. ANGLE..`

The editor may internally know editable fields, but storage should look real.

Progress:

* G-code row templates now use `G71 U{} R{} X{} Z{} F{}` / `G1 X{} Z{}` style drafts.
* Preset expansion now inserts stored `G71/G72/G1` rows in G-code-ish form, not pipe rows.
* Draft commit strips temporary braces from program-core `G...` rows before saving.
* G71/G72 region ownership and generator dispatch now match `G71 ...`, `G72 ...`, `G1 ...`, `G2 ...`, `G3 ...` command rows.
* Editor/default expression lookup now treats fields as plain text tokens before `{}`, not pipe records.
* Snapshot display no longer pretty-prints pipe rows; it shows the stored row text directly.
* SETUP, TOOL, TOOLCALL, and preset templates now create plain text rows.
* Tool catalog storage now writes `TOOL T{} R{} ...` rows instead of `TOOL|...`.
* Shared plain text parser no longer accepts pipe-delimited rows or brace-wrapped saved values.
* Tool catalog import rejects old pipe/brace tool rows instead of normalizing them.
* Editor/expression field helpers no longer treat `|` as a draft field separator.
* Active context checks now use plain command matching for SETUP/TOOL/TOOLCALL.
* Plain command/field parsing is centralized in `leancam_text.c/h` and shared by presets, regions, validation, tool catalog, editor/UI, and bridge wrappers.
* G-code generator field lookup now uses the shared plain text parser instead of carrying a second pipe-era parser.
* Draft commit/display no longer blocks or annotates rows with tool validation warnings.
* Tool lookup uses static default rows for T0-T8 when the catalog/program has no single explicit match.
* Tool validation is intentionally minimal: `TOOLCALL T` only needs to parse as a positive integer; tool metadata policy checks were removed for now.
* Run/preflight no longer carries a fake tool validation callback that always passes.
* Preset allowance fields now use real `X`/`Z` words instead of `X_ALLOW`/`Z_ALLOW`.
* Generator dispatch no longer runs old private OD/ID/FACE/CUT/GROOVE/CHAMFER/RADIUS command rows.
* Raw G71/G72 host tests now use plain rows plus explicit `G80`, matching stored NC text.
* Drill/tap/thread templates now dispatch as G-code-ish `G74`, `G84`, direct `G33`, and `G76` rows instead of relying only on private `DRILL`/`TAP`/`THREAD` commands.
* Editor navigation now treats plain tokens like `T1`, `R_FEED120`, and `X50` as editable fields directly. Braces are only needed for preset/template placeholders.
* G1 helper `AUTO` metadata is gone from active rows and host G71/G72 tests.
* `TOOLCALL` draft/default resolution now links through the plain tool table/default rows, so `TOOL.R_FEED`/`TOOL.DOC` style defaults resolve from `TOOL T...` rows.
* Preset insert resolves template defaults to plain words before expansion, so temporary `OD/ID/FACE/RECESS` rows do not leak into the saved program when expansion succeeds.

---

## 5. OD/ID/FACE/RECESS are presets only

Status: mostly done.

Do not store:

```text
OD|...
ID|...
FACE|...
RECESS|...
```

They are keypad/menu presets.

OD button inserts something like:

```gcode
(OD)
G71 U2 R1 X0.5 Z0.5 F120
    G1 X{setup.od} Z0
    G1 X{} Z{}
    G1 X{setup.od}
G80
```

FACE button inserts:

```gcode
(FACE)
G72 W1 R1 X0.2 Z0.2 F100
    G1 X{setup.od} Z0
    G1 X{} Z{}
    G1 X{setup.od}
G80
```

Preset expansion uses:

* current tool/table defaults
* setup OD/ID/CLR
* feed/doc/rpm
* orientation validation metadata if needed

But final visible program is G-code-like.

Progress:

* OD/ID/FACE/RECESS remain preset templates only.
* Preset expansion writes G71/G72 plus visible G1 helper/user rows.
* Presets now insert explicit `G80` after the generated contour region.
* Failed preset expansion removes the temporary preset row instead of leaving `ID ...` / `OD ...` in the program.
* Preset expansion reads setup stock fields directly from the current `SETUP` line instead of using a generic callback.

---

## 6. Explicit G80 regions

Status: done / keep as invariant.

Use explicit `G80`.

Region:

```gcode
G71 ...
    G1 ...
    G1 ...
G80
```

Rules:

* `G71` or `G72` starts canned-cycle contour region
* `G80` ends it
* no hidden EOF ownership
* no “next G71 closes previous” as primary model
* no P/Q ranges for internal LeanCam editor

This makes the contour region visually and structurally clear.

`G80` insertion:

* preset inserts it automatically
* `#` key inside active cycle closes/inserts G80 when sim/run is not already armed.
* More/Special page should also have explicit `G80 End`

Progress:

* `G80` added to raw template list.
* More/Special page has explicit `G80` End insertion.
* `#` on an open G71/G72 region inserts `G80`; a second `#` can still arm/run as before.
* Region ownership recognizes `G80` as the explicit process end.
* Cursor on `G80` can resolve the owning region.
* Generator flushes G71/G72 only on `G80`.
* Program footer reports missing `G80` instead of silently closing an open region at EOF.

---

## 7. One command = one editor object

Status: mostly done / editor ergonomics still evolving.

One stored command:

* one saved line
* one editor object
* one selectable row

Long rows may render over multiple visual rows:

```gcode
G71 U2 R1 X0.5 Z0.5
    F120 RPM800
```

But internally still one command.

Rules:

* continuation line is not separately selectable
* selected highlight covers all wrapped visual rows
* Up/Down moves command-to-command
* field navigation moves within command

No fake twin-row editing.

Progress:

* Stored program rows remain one command per saved line.
* Program display wraps long committed commands into continuation visual rows only.
* Continuation visual rows share the owning command selection state and are not separate editor objects.
* Removed the old UI-level draft field cursor; `leancam_editor.c` is now the single active field cursor for `D`/value editing.
* Removed unused brace-era "next required field" navigation helpers from `leancam_program`.

---

## 8. G1 chamfer/radius rules

Status: implemented enough for current preview/generator.

Use real-world style:

```gcode
G1 X40 Z-20 C1
G1 Z-50
```

or:

```gcode
G1 X40 Z-20 R2
G1 Z-50
```

Rules:

* `C` = chamfer into next element
* `R` = tangent radius/round into next element
* C and R are mutually exclusive
* entering C clears R
* entering R clears C
* C/R belong to current block → next block transition

G2/G3 are explicit arcs:

* real contour geometry
* may be tangent or not
* not same as R shortcut

Progress:

* Editor field lookup now matches exact word names, so `R` no longer aliases `R_FEED`.
* G1 edit normalization keeps `C`/`R` mutually exclusive by clearing the opposite field when a positive value is entered.
* G-code parser rejects `G1` with both positive `C` and positive `R`.
* Host tests cover C/R exclusivity plus emitted chamfer/radius finish geometry.
* G71/G72 now expand simple `G1 ... C...` / `G1 ... R...` corners into an effective contour before rough/finish generation.
* `C` emits a trimmed chamfer line; `R` emits a real tangent fillet arc using the same radius-space math as preview.

---

## 9. G71/G72 scanline roughing only

Status: implemented / deliberately conservative.

Do not implement:

* polygon fill
* recursive clearing
* pocket roughing
* Type-II non-monotonic roughing
* smart CAM islands

Implement simple scanline roughing.

G71:

* scan by X rough levels
* cut in Z direction
* stop before contour according to allowance

G72:

* scan by Z rough levels
* cut in X direction

Basic rule:

* for each roughing level
* find contour intersection
* cut until distance to contour is within allowance
* retract G0
* next pass

No recursion needed.

Task - make G71 roughing scanline-only:

Problem:

G71 must not emit diagonal contour-following roughing moves near taper/profile ends.

Example input:

```gcode
G71 U2.0 R1 X0.5 Z0.5 F120
G1 X25 Z0
G1 X25 Z-15 C0 R0
G1 X30 Z-20 C0 R0
G1 X50 Z-25
G80
```

Forbidden roughing output:

```gcode
G1 X30 Z-20
G1 X50 Z-25
```

Rule:

* G71 roughing uses the contour only as a boundary.
* G71 roughing emits scanline moves only.
* Allowed roughing `G1`: Z changes while X is fixed, or X changes while Z is fixed.
* Forbidden roughing `G1`: X and Z both change from the previous point.

Acceptance guard:

```c
bool x_changed = fabsf(x1 - x0) > 0.001f;
bool z_changed = fabsf(z1 - z0) > 0.001f;
assert(!(x_changed && z_changed));
```

Implementation:

* parsed line segments only
* scan X levels
* find Z boundary at current X level
* emit one fixed-X Z pass
* retract
* no geometry framework, AST, or contour engine

Progress:

* G71 roughing now emits fixed-X scanline passes instead of replaying contour breakpoints.
* G71 roughing has a diagonal `G1` guard before rough feed moves.
* Host tests include the taper/profile example above and fail if any G71 roughing `G1` changes both X and Z.

---

## 10. Arc handling for scanline

Status: implemented for current monotonic roughing; richer cases later only.

For lines:

* linear interpolation

For arcs:

* solve circle intersection

Circle:

```text
(X - CX)^2 + (Z - CZ)^2 = R^2
```

For G71 scanline:

* given X level
* solve Z candidates

```text
dz = sqrt(R^2 - (X - CX)^2)
Z1 = CZ + dz
Z2 = CZ - dz
```

Pick candidate that:

* lies between arc start/end
* lies on correct G2/G3 sweep
* follows monotonic direction

If ambiguous:

* refuse roughing
* show an error; no fallback finish-preview mode

First version may support:

* monotonic simple arcs only

Progress:

* G71 roughing now allows simple `G2/G3 ... R...` contour elements as scanline boundaries.
* Arc roughing support is intentionally limited to plain monotonic G71; G72 arcs still fail clearly.
* Arc boundary lookup is direct circle math only: derive center, solve Z candidates at the current X level, pick the monotonic boundary side.
* Host tests include a simple G71 arc contour and a G72 arc refusal check.
* Arc boundary candidates are now checked against the actual G2/G3 sweep instead of only the endpoint box.
* The raw preview `G1 ... R...` corner shortcut now draws a real tangent fillet using `R / tan(angle / 2)` and center selection tangent to both neighboring lines.

Add rule - deterministic R corner construction:

`R` in `G1` rows is a corner fillet between the previous and next line segments. It is not a magic arc-center hint and it must not be guessed.

For:

```gcode
G1 X25 Z5
G1 X25 Z-15 R5
G1 X50 Z-15
```

`R5` belongs to the corner at `X25 Z-15`:

```text
P0 = X25 Z5
P1 = X25 Z-15
P2 = X50 Z-15
R  = 5
```

Construction:

```text
v_in  = normalize(P0 - P1)
v_out = normalize(P2 - P1)
dot   = dot(v_in, v_out)
theta = acos(clamp(dot, -1, 1))
t     = R / tan(theta / 2)
T1    = P1 + v_in  * t
T2    = P1 + v_out * t
bis   = normalize(v_in + v_out)
C     = P1 + bis * (R / sin(theta / 2))
```

Reject if any segment length is zero, `R <= 0`, `abs(dot)` is near `1.0`, tangent distance does not fit both legs, the center is not exactly radius `R` from both tangency points, or tangency validation fails.

For the example the result must be:

```text
T1 = X25 Z-10
T2 = X30 Z-15
C  = X30 Z-10
R  = 5
```

No vertical/horizontal special cases. The same X/Z vector math must handle all normal 2D profile corners. G71 roughing uses the constructed arc only as boundary geometry.

Progress:

* Generator `G1 ... R...` expansion now uses deterministic tangent-line construction with validation instead of choosing from guessed centers.
* Host tests reject too-large and straight-line `R` corners.

---

## 10A. Fix G71 roughing scanline boundary with R/C profile geometry

Status: implemented in the current test gate.

Problem:

G71 roughing is scanline-only, but roughing must sample the real contour boundary after `C`/`R` profile geometry is expanded.

Example:

```gcode
G71 U2.0 R1 X0.5 Z0.5 F120
G1 X25 Z5 C0 R0
G1 X25 Z-15 C0 R5
G1 X50 Z-15
G80
```

Bad behavior:

* all roughing passes from `X50` down to `X26` cut to about `Z-14.5`
* this ignores the `R5` corner boundary near `X25`

Correct rule:

```text
R/C geometry is ignored as roughing motion, but not ignored as material boundary.
```

Meaning:

* G71 roughing must not replay contour moves as toolpath
* G71 roughing must not emit arc/radius roughing moves
* G71 roughing must not emit diagonal `G1 X... Z...` rough moves
* scanline Z limit must include line, taper, chamfer, and radius geometry

For every roughing X scanline:

1. Intersect scanline X with full profile envelope.
2. Include straight segments.
3. Include chamfers.
4. Include radius/fillet arcs.
5. Get correct Z limit.
6. Apply Z allowance.
7. Emit fixed-X Z cutting move only.

Acceptance:

For every emitted roughing `G1`:

```c
bool x_changed = fabsf(x1 - x0) > 0.001f;
bool z_changed = fabsf(z1 - z0) > 0.001f;

assert(!(x_changed && z_changed));
```

But Z endpoints must change near fillets, chamfers, and tapers.

For the sample above:

* `X50`, `X48`, `X46`, ... far from corner cut to about `Z-14.5`
* near the `R5` corner, smaller X passes must stop earlier
* fail if `X26` still cuts to `Z-14.5`

Approximate expected Z ends:

```text
cut X50 -> test X49.5 -> boundary Z-15.000 -> cut Z-14.500
cut X48 -> test X47.5 -> boundary Z-15.000 -> cut Z-14.500
cut X46 -> test X45.5 -> boundary Z-15.000 -> cut Z-14.500
cut X44 -> test X43.5 -> boundary Z-15.000 -> cut Z-14.500
cut X42 -> test X41.5 -> boundary Z-15.000 -> cut Z-14.500
cut X40 -> test X39.5 -> boundary Z-15.000 -> cut Z-14.500
cut X38 -> test X37.5 -> boundary Z-15.000 -> cut Z-14.500
cut X36 -> test X35.5 -> boundary Z-15.000 -> cut Z-14.500
cut X34 -> test X33.5 -> boundary Z-15.000 -> cut Z-14.500
cut X32 -> test X31.5 -> boundary Z-15.000 -> cut Z-14.500
cut X30 -> test X29.5 -> boundary about Z-14.975 -> cut Z about -14.475
cut X28 -> test X27.5 -> boundary about Z-14.330 -> cut Z about -13.830
cut X26 -> test X25.5 -> boundary about Z-12.179 -> cut Z about -11.679
```

Implementation requirement:

Keep this boring and visible.

* no generic geometry engine
* no AST
* no dynamic allocation
* no framework
* use small fixed structs or the existing fixed effective contour array

Suggested boundary pieces for the sample after `R5`:

```text
line: X25 Z5   -> X25 Z-10
arc:  X25 Z-10 -> X30 Z-15, center X30 Z-10, R5
line: X30 Z-15 -> X50 Z-15
```

Required checks:

* no rejected diagonal rough moves
* no `G1` roughing move changes X and Z together
* passes at `X50..X32` end around `Z-14.5`
* passes near `X30/X28/X26` end earlier because `R5` boundary is active
* fail if roughing emits diagonal contour replay

Progress:

* G71/G72 effective contour expansion now uses direct X/Z math for `C`/`R` corner tangent points, matching scanline boundary coordinates.
* Host gate covers the `R5` boundary sample and verifies `X32`, `X30`, `X28`, and `X26` roughing Z endpoints.
* Host gate keeps the no-diagonal roughing assertion active for the R boundary sample.

Notes:

* diameter-style X values are used, matching current LeanCam profile display
* do not add tool-nose compensation here
* do not implement G70 here
* do not generate finishing contour here
* G71 roughing remains boring scanline roughing only

---

## 11. Validation

Status: implemented for current conservative G71/G72 rules.

G71:

* contour must be monotonic in Z

G72:

* contour must be monotonic in X

If invalid:

* stop generation
* show error
* do not silently fall back to a finish-only preview

Example errors:

```text
G71 contour non-monotonic in Z
G72 contour non-monotonic in X
Arc roughing unsupported
```

Fail clearly.

Progress:

* G71 roughing fails clearly on non-monotonic Z instead of falling back to a finish-only path.
* G72 roughing fails clearly on non-monotonic X.
* Host tests cover both monotonic validation directions plus G72 arc roughing refusal.

---

## 12. G71/G72 test gate

Status: active and passing in host/build checks when run.

Before adding clever logic, create tests.

Current gate:

* host test now uses stored plain NC rows only: `G71/G72`, `G1`, `G80`
* stale private cycle rows (`OD|...`, `ID|...`, `FACE|...`, etc.) are no longer part of the host gate
* fallback finish-preview mode is not accepted; unsupported roughing must fail clearly
* current host check passes: raw G71/G72 roughing and G71 example contours

Use setup:

```gcode
SETUP L50 OD50 ID0 CLR1
```

Test examples:

* simple OD monotonic line
* 4-point contour
* 5-point contour
* 6-point contour
* simple arc
* bad non-monotonic contour
* G72 face case

Each test should check:

* parse region between G71/G72 and G80
* finish pass follows full contour
* roughing does not gouge
* G0/rough/feed/finish classes produced
* invalid roughing refused clearly

Progress:

* Host gate covers plain stored rows only: `G71/G72`, `G1/G2/G3`, and `G80`.
* G71 examples cover 4-point, 5-point, 6-point, edited start/close, scanline-only taper, and simple CW/CCW arcs.
* G72 face case now verifies region marker, rough marker, finish marker, rapid/feed output, and finish contour order.
* Invalid cases cover G71 non-monotonic Z, G72 non-monotonic X, and G72 arc roughing refusal.
* R/C boundary sample verifies near-fillet roughing endpoints and no diagonal roughing moves.

---

## 13. Preview from owning process

Status: mostly done.

Cursor on:

* G71/G72
* G1/G2/G3 inside region
* G80

should preview whole owning process.

Preview should show:

* stock
* final contour
* roughing passes
* rapid moves
* selected source element

Progress:

* preview source rows now use the shared G71/G72/G80 region owner instead of bridge-local header/end scanning
* cursor on `G80` is included in the owning preview source region
* preview region source list includes explicit `G80`, so the visual process boundary is visible
* LVDS preview parser now reads plain NC words (`G71 U...`, `G1 X... Z...`) instead of pipe/brace cycle rows.
* LVDS generated-path preview now passes `G80` to the same G-code generator used for run/sim, so preview path generation no longer fails with an artificial missing-G80 footer error.
* Preview roughing allowance display now reads `X`/`Z` from G71/G72 headers, matching the stored NC format.

---

## 14. Preview path classes

Status: partially done / normal editor generated-path preview remains parked.

Every generated segment must be classified:

```text
RAPID_G0
ROUGH_FEED
FINISH_FEED
AUTO_OR_HELPER
SELECTED_SOURCE
```

Visual rules:

* finish contour strongest
* rough passes dim repeated
* G0 rapid clearly distinct
* selected row highlighted
* labels if fit, otherwise C1/C2/C3

Progress:

* Generated G71/G72 preview path now classifies segments internally as rapid, rough feed, finish feed, or normal feed.
* Rapid moves draw dashed/thin.
* Rough feed draws dim/thin.
* Finish feed draws strongest and visually dominant.
* Generated-region preview inside the normal editor is parked because doing full G-code generation inside the LVDS/HSTX render path can stall the display.
* Normal editor preview currently uses a temporary graphic yellow removal fill plus the final contour.
* Fullscreen sim should show generated G-code path lines, not yellow removal fill.
* Generated-path preview now draws `G2/G3 ... R...` arcs as arcs instead of falling back to a straight line when `I/K` are absent.

---

## 14A. Robust graphic cycle preview fill

Status: parked / still a good future cleanup, not needed for current stable path.

Replace the temporary geometry-buffer hatch/fill with direct scanline drawing into the existing preview VRAM/draw area.

Reason:

* Path lines are stable.
* Roughing hatch/path simulation in the renderer is fragile.
* The editor preview only needs to show material-to-remove, not exact roughing tool motion.
* Do not run full generated G-code preview inside normal LVDS/HSTX drawing.

Target behavior:

* Draw stock and final contour first or into a simple mask.
* Use known stock bounds and contour pixels.
* For each scanline, start from the stock boundary and fill toward the cutting direction.
* Stop when the contour edge is hit.
* Fill only the removal side in yellow.
* Draw final contour on top.
* No separate large contour model/buffer unless a tiny fixed scratch mask is clearly needed.

Direction:

```text
G71: scan/fill in X/diameter direction per horizontal display row
G72: scan/fill in Z direction per vertical display column
```

Implementation preference:

* Use existing LVDS preview area/VRAM like old EGA scanline fill.
* Keep hard pixel/time limits.
* Rebuild only when preview source region changes.
* Renderer draw path should consume the already-prepared graphic, not solve geometry live.

Related sim task:

* Fullscreen sim uses a bridge-owned `leancam_gcode_stepper_*()` instance.
* Do not store generated sim lines in a RAM/PSRAM source-region cache.
* Do not regenerate expanded G-code inside the renderer.
* Do not emit preview debug serial noise by default.
* If generation fails, sim should show a clear message/empty path, not yellow editor fill.

Current parked state:

* Editor preview has temporary yellow fill.
* Stripe lines are disabled.
* Generated G7x preview is disabled for normal editor preview.
* Preview serial debug is off by default.

---

## 15.  Replace LeanCam SETUP block with Eltropilot-style setup/graphics G-codes

Status: open / next real language cleanup.

Goal:
Remove current non-G-code SETUP block completely and replace it with old Eltropilot-inspired CNC-looking setup commands.

Remove completely:
- SETUP block grammar
- SETUP.FIELD references
- key/value setup forms
- exposed JSON/YAML-like metadata
- non-G-code template setup syntax

Allowed human-readable layer:
Only parenthesized CNC-style comments.

Comment behavior:
- optional only
- parser/simulation ignores fully
- truncated in editor UI
- no wrapping by default
- full comment visible only when selected/opened

Adopt Eltropilot-style setup commands:

N70 G970 X-10 X120 Z-150 Z30
N71 G971 X80 Z125 E0
N72 G972 C15
N73 G973 P7

Meaning:
- G970 = graphics/display extents
- G971 = raw stock dimensions
- G972 = clamping length
- G973 = graphics mode/options

Modern LeanCam implementation may use same idea with cleaned parameters:

G970:
- graphics extents / display limits
- chuck/jaw display envelope
- tailstock display envelope if enabled

G971:
- raw stock dimensions
- X = stock OD
- Z = stock length
- optional I = stock ID
- optional E = extra/front allowance

G972:
- clamping length
- C = length inside chuck / chuck-side held length

G973:
- graphics mode bitmask
- 0 = axes only
- 1 = stock
- 3 = stock + chuck
- 7 = stock + chuck + tailstock

Internal behavior:
- these are not machining motion
- they update setup/simulation/planning metadata
- they are visible as CNC-like program commands
- no separate SETUP block remains

Design target:
Old dead Eltropilot-style setup rebuilt with modern parser/editor/simulation.

## 15A Redesign LeanCam G71 roughing around Eltropilot/Heidenhain-style contour ownership - skip for now

Status: skipped for now.

Goal:
Do not force one contour model only.
Support both inline G80-owned contours and reusable referenced contours.

Important correction:
CTR/ENDCTR is only a possible new explicit reusable contour syntax.
It must NOT replace G80.
G80-style inline contour ownership remains first-class.

Variant A: inline cycle-owned contour

N100 G71 P2 I0.5 K0.2
N110 G0 X60 Z0
N120 G1 Z-40
N130 G1 X40
N140 G80

Meaning:
- G71 owns following contour
- G80 ends contour definition
- contour is local/anonymous
- best for quick one-off operations

Variant B: reusable contour reference

N100 CTR
N110 G0 X60 Z0
N120 G1 Z-40
N130 G1 X40
N140 ENDCTR

N200 G71 C=N100 P2 I0.5 K0.2
N210 G70 C=N100

Meaning:
- C=N100 references contour beginning at block N100
- ENDCTR discovered automatically
- contour can be reused by roughing, finishing, threading, grooves, simulation, etc.

Required internal model:
- both variants compile into same internal contour representation
- inline G80 contour becomes temporary anonymous contour object
- CTR/ENDCTR contour becomes persistent reusable contour object
- G71 backend consumes contour object regardless of source

Virtual reference behavior:
- C=N100 is a structural reference, not fragile raw text
- inserting lines inside contour must preserve references
- renumbering must update/preserve references
- deleting contour start invalidates references clearly
- moving contour moves associated contour object

Possible later Eltropilot-inspired cycle remap:
G71 itself may later be replaced or aliased by more Eltropilot-specific roughing G-codes if old manuals confirm exact names/semantics.
For now design parser/backend so cycle number is not hardwired:
- cycle parser maps G-code -> roughing operation
- roughing operation consumes contour object
- syntax layer can support G71 now and Eltropilot code later

Design principle:
Visible program should feel like old lathe CNC / Eltropilot / Heidenhain DIN:
simple CNC-looking text,
G80 contour ownership available,
optional reusable contour references,
modern internals hidden underneath.


## 16. Renderer split

Status: mostly done for current LVDS target.

Split renderer into three layers.

`ui_layout`:

* pages
* tables
* footer
* status line
* preview placement

`draw_api`:

* draw_text
* draw_line
* draw_rect
* draw_polyline
* draw_glyph
* clipping

`hw_renderer`:

* RA8876
* LVDS/HSTX
* ILI9341
* SPI/DMA/page swap details

No hardware renderer should know:

* G71
* menu schema
* tool catalog
* editor state

Progress:

* `draw_api` module started with `lvds_draw_api.c/h`.
* Removed the `lvds_hw_renderer.c/h` pass-through layer; `lvds_draw_api` is now the backend boundary over current LVDS/HSTX primitives.
* Basic draw primitives now have a renderer-facing wrapper over the current LVDS/HSTX backend.
* Clipped text rendering moved behind the draw API; layout code still owns placement for now.
* `lvds_renderer.c` now calls `lvds_draw_*` for text, text measurement, lines, rectangles, and ellipses instead of calling HSTX primitives directly.
* `ui_layout` module started with `lvds_ui_layout.c/h`; tool-editor row/table placement now comes from the layout layer.
* Normal program-list/split-preview placement now also comes from `lvds_ui_layout`.
* Footer menu/status drawing moved to `lvds_ui_footer.c/h`, trimming one self-contained UI renderer chunk out of `lvds_renderer.c`.
* Perf/block meter formatting and drawing moved to `lvds_ui_meters.c/h`.
* Wrapped row text and field-highlight drawing moved to `lvds_ui_text.c/h`.
* Top status/header bar drawing moved to `lvds_ui_header.c/h`; renderer still supplies the interpreted machine state text.
* Live/source file-line label drawing moved to `lvds_ui_live.c/h`.
* Fullscreen/live sim header drawing now uses `lvds_ui_live.c/h`; the old extra fullscreen sim border was removed.
* Removed the hard-disabled fullscreen asset-editor path instead of preserving another inactive renderer layer.
* Main program screen shell drawing moved to `lvds_ui_program.c/h`.
* Removed stale pipe-era fullscreen asset detail/title helpers left behind by the disabled asset editor.
* Moved LeanCam LVDS screen/runtime files from `lvds_renderer` into `leanCam`; the LVDS module now keeps boot, HSTX, palette, fonts, PSRAM, and primitive draw API only.

---

## 17. ATC neighbor collision shadow preview skip for now

Status: skipped for now.

Add conservative 2D collision overlay.

Use:

* turret tool count
* turret PCD/radius
* active station
* previous/next station
* neighbor tool type
* holder/tool diameter
* tool offset

Draw:

* current tool plane
* adjacent-tool shadow triangles

Only default-enable for:

* boring bars
* drills
* long ID tools

Warn on intersection with stock/profile/toolpath.

No full 3D simulation.

---

# NICE TO HAVE / LATER

## A. Named contour blocks - skipped

Status: skipped.

Possible future:

```gcode
CONT N10
    G1 X50 Z0
    G1 X25 Z-25
    G1 X50
G80

G71 C10 U2 R1 X0.5 Z0.5 F120
```

This separates:

* contour definition
* canned cycle use

It is a modern structured replacement for Fanuc P/Q ranges.

Not needed for first stable version.

---

## B. Contour vector editor

Status: later / experimental only.

Task — Experimental ICP / Contour Vector Editor Mode

Status:
Experimental.
Do not treat as final architecture yet.

Goal:
Add lightweight Heidenhain/ICP-style contour teach mode for fast contour entry using keypad vectors.

IMPORTANT:
This mode is editor ergonomics only.
Stored program output must remain normal G-code-ish text.

No private LeanCam syntax may be stored.

------------------------------------------------------------
Concept
------------------------------------------------------------

Add dedicated:
    ICP EDIT

toggle/button/action.

When OFF:
- keypad behaves normally
- menu system unchanged

When ON:
- keypad becomes X/Z vector map
- contour editing becomes directional/interactive

Inspired by:
- Heidenhain ICP/FK
- conversational contour editors

But final stored rows remain:
    G0
    G1
    G2
    G3

No VECTOR(), ICP(), MOVE() or hidden syntax.

------------------------------------------------------------
Keypad Vector Map
------------------------------------------------------------

Keypad:

7 8 9
4 5 6
1 2 3

Lathe interpretation:

Z- = into stock
Z+ = away from stock

X+ = larger diameter
X- = smaller diameter

------------------------------------------------------------
Straight Moves
------------------------------------------------------------

2 = Z- move

Insert:
    G1 X(current) Z{}

Cursor lands on:
    Z

Expected:
- moving into stock
- likely negative Z

Example:

Current point:
    X50 Z0

Press:
    2

Generated draft:
    G1 X50 Z{}

------------------------------------------------------------

8 = Z+ move

Insert:
    G1 X(current) Z{}

Expected:
- retract/back direction
- positive Z

------------------------------------------------------------

4 = X- move

Insert:
    G1 X{} Z(current)

Expected:
- smaller diameter

Example:

Current:
    X50 Z-20

Press:
    4

Generated:
    G1 X{} Z-20

------------------------------------------------------------

6 = X+ move

Insert:
    G1 X{} Z(current)

Expected:
- larger diameter

Useful:
- shoulders
- reliefs
- returns

------------------------------------------------------------
Diagonal / Taper Moves
------------------------------------------------------------

1 = X- Z-

Insert:
    G1 X{} Z{}

Expected:
- smaller diameter
- deeper into stock

Typical:
- OD taper
- lead taper
- chamfer-like transition

------------------------------------------------------------

3 = X+ Z-

Insert:
    G1 X{} Z{}

Expected:
- larger diameter
- deeper into stock

------------------------------------------------------------

7 = X- Z+

Insert:
    G1 X{} Z{}

Expected:
- smaller diameter
- retracting

------------------------------------------------------------

9 = X+ Z+

Insert:
    G1 X{} Z{}

Expected:
- larger diameter
- retracting away

------------------------------------------------------------
Stored Output Rules
------------------------------------------------------------

Stored program must remain real G-code-ish text.

Allowed stored rows:

    G0
    G1
    G2
    G3

Examples:

    G1 X50 Z-20
    G1 X30 Z-40
    G1 X30
    G1 Z0

Forbidden:
    VECTOR(...)
    ICP(...)
    MOVE(...)
    hidden binary metadata syntax

------------------------------------------------------------
Current Point Logic
------------------------------------------------------------

Editor tracks current contour point from previous:
    G0
    G1
    G2
    G3

Inserted row inherits unchanged coordinate automatically.

Examples:

Current:
    G1 X50 Z-20

Press:
    4

Generated:
    G1 X{} Z-20

------------------------------------------------------------
Recommended First Version
------------------------------------------------------------

Only generate:
    G1

No automatic arcs initially.

Reason:
- simpler
- predictable
- easier preview integration
- lower parser complexity

------------------------------------------------------------
ICP EDIT Mode
------------------------------------------------------------

Possible entry:
- dedicated ICP softkey
- menu action
- long press EDIT

Possible exit:
- ICP button again
- ESC/CANCEL
- switching page
- G80 insertion

------------------------------------------------------------
UI Behavior
------------------------------------------------------------

When ICP mode active:
- show vector map overlay
- show current contour point
- show expected move direction hint
- highlight active contour region

Possible overlay:

    ICP MODE

    7 8 9
    4 5 6
    1 2 3

    X- / X+
    Z- / Z+

------------------------------------------------------------
Example Session
------------------------------------------------------------

Initial contour:

    G71 U2 R1 X0.5 Z0.5 F120
        G0 X50 Z0
    G80

Cursor on:
    G0 X50 Z0

Enter:
    ICP EDIT

------------------------------------------------------------

Press:
    2

Generated:
    G1 X50 Z{}

User enters:
    -20

Stored:
    G1 X50 Z-20

------------------------------------------------------------

Press:
    4

Generated:
    G1 X{} Z-20

User enters:
    30

Stored:
    G1 X30 Z-20

------------------------------------------------------------

Press:
    1

Generated:
    G1 X{} Z{}

User enters:
    20
    -40

Stored:
    G1 X20 Z-40

------------------------------------------------------------

Final contour:

    G71 U2 R1 X0.5 Z0.5 F120
        G0 X50 Z0
        G1 X50 Z-20
        G1 X30 Z-20
        G1 X20 Z-40
    G80

------------------------------------------------------------
Future Extensions (NOT NOW)
------------------------------------------------------------

Possible future:
- long press = arc mode
- 5 = edit current point
- 0 = chamfer helper
- # = insert G80/end contour
- tangent arc helpers
- ghost preview while entering values
- automatic chamfer insertion

Not part of first implementation.

------------------------------------------------------------
Critical Rule
------------------------------------------------------------

ICP/vector mode is:
    editor ergonomics only

NOT:
    a new language

Final stored program must remain:
    real G-code-ish text

---

## C. Vector-based process starters

Status: later / experimental only.

Experimental.

Possible:

* 4→2 = OD starter
* 4→8 = ID starter
* 8→6 = FACE starter

Not approved yet. Could become elegant or could become private religion.

---

## D. Richer G71/G72

Status: later only.

Later only:

* non-monotonic paths
* Type-II roughing
* recess/pocket roughing
* smarter stock model

Not now.

First version:

```text
monotonic
scanline
safe
visible
boring
```

---

# Summary

LeanCam becomes:

```text
real G-code-ish storage
+
keypad presets
+
structured editor
+
safe canned-cycle generator
+
live preview/debugger
```

Not:

```text
private CNC language
```

That is the clean line.
