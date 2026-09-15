# LeanCam / µCNC — Refactor + Real G-code Roadmap
## 2026-06-06 pivot: new NC module, LeanCam becomes reference code

Status: started.

Decision:

* Stop refactoring the LeanCam private language/editor one piece at a time.
* Add a new small `src/modules/nc/` module named **NC**.
* Treat LeanCam as prototype/reference code while NC becomes the boring file
  editor path.

NC owns only:

* `.nc`, `.ngc`, and `.gcode` file open/save/list helpers.
* plain text line storage and line cursor movement.
* temporary one-line word parsing for field stepping/editing.
* small vocabulary labels for display/help.
* a display snapshot that renderers can draw without parsing.

NC must not own:

* G71/G72 roughing generation.
* contour ownership or reusable contour rules.
* SETUP blocks, pipe rows, brace values, or `.lcam` migration.
* hidden stock/tool/program models.
* rendering internals, execution pacing, or uCNC stream buffering.

Milestone 1:

* Open `.nc`.
* Display preserved text lines.
* Move cursor line up/down.
* Edit the current line as text.
* Step through words in the current line.
* Change one word value.
* Save file.
* Show a vocabulary label for the selected word.
* Reject `.lcam` and pipe syntax.

Temporary NC mode key/footer milestone:

* `A` is only a temporary mode access key.
* Pressing `A` cycles `MANUAL -> PROGRAM -> SIM -> MDI -> RUN -> MANUAL`.
* Header must visibly update the mode name.
* Footer labels must change per mode.
* Footer implementation must stay as static tables with `mode + key -> action`.
* Copy the proven LeanCam footer drawing style into NC-local code, but do not
  call LeanCam visual/menu/schema functions and do not reuse LeanCam's menu tree.
* Copy LeanCam keypad convention where possible: `B/C` navigate, `D`
  accept/edit/open, `#` finish/save/run, `*` delete/back, number keys for fixed
  actions. `A` remains the temporary NC mode key until direct mode keys exist.
* PROGRAM, SIM, and RUN should use the old split view idea where useful:
  preview/stock context on one side, NC text/file rows on the other.
* Preview must stay thin: draw stock/setup/path hints from visible NC text only.
  Do not regenerate G-code, build a persistent semantic model, or pull renderer
  work into file/runtime logic.
* File manager is allowed again where a mode needs a file, but only as a small
  fixed NC file list with static footer actions. Do not revive the deep
  LeanCam browser/menu architecture inside NC.
* Do not build a dynamic menu tree, nested page engine, generic UI framework,
  callback maze, or runtime-created menus around this.
* Replace `A` later with direct `MANUAL`, `PROGRAM`, `SIM`, `MDI`, and `RUN`
  mode buttons when real keys exist.

Progress:

* Created the first standalone `src/modules/nc/` files.
* Added fixed text-line storage, extension checks, pipe-syntax rejection,
  current-line editing, one-line word parsing, field stepping, word-value
  replacement, vocabulary lookup, snapshot fill, and simple stdio file IO.

## NC hard rule: text is the program, parsers are disposable

NC history matters here.

Old NC was not designed as an in-memory object database. It came from
block/word-address programs: a line/block contains words like
`G1 X3 Z-10 F120`; NIST RS274/NGC defines a word as a letter followed by a
number/expression, and LinuxCNC still describes G-code as lines/blocks of code
collected in a file.

Therefore the NC module must follow the old machine model:

```text
text line in
scan words
act/display/edit
forget parse result
```

Not:

```text
text line in
build structs
own semantic model
sync model back to text
```

Hard rule:

Do not build persistent command structs.

Forbidden persistent structures:

```c
typedef struct {
    int gcode;
    float x;
    float z;
    float feed;
    float rough_depth;
    float retract;
    int contour_start;
    int contour_end;
} nc_command_t;
```

Forbidden arrays:

```c
nc_command_t program[NC_MAX_LINES];
```

Allowed persistent storage:

```c
char line[NC_MAX_LINE_LEN];
```

or:

```c
typedef struct {
    char text[NC_MAX_LINE_LEN];
} nc_line_t;
```

Temporary scanner only:

Allowed temporary parse while editing one line:

```c
typedef struct {
    char letter;
    int start;
    int end;
} nc_word_span_t;
```

This exists only to find where `X50` or `F120` sits in the text.

It must not become program storage.

Editing rule:

Editing a field edits text directly.

Example:

Input line:

```gcode
G71 U2 R1 X0.5 Z0.5 F120
```

Cursor selects `R1`.

User enters `1.5`.

Result line:

```gcode
G71 U2 R1.5 X0.5 Z0.5 F120
```

No command object is updated.

No regeneration.

No semantic model.

Just replace text span.

Vocabulary rule:

Vocabulary is lookup-only.

Example:

```text
Current command: G71
Current word: R
Label: Retract
```

This label is not stored.

It is calculated from current text context.

File rule:

The file is the database.

RAM holds only:

* visible line window, or
* small editable line buffer, or
* fixed text lines if full file is small enough

Do not mirror the file into semantic structs.

Streaming rule:

Running uses source text.

Raw run:

```text
read next NC line
send to uCNC
```

Generated G7x run:

```text
G7x scanner reads source text lines
emits one generated NC text line
send to uCNC
```

No full expanded object model.

Acceptance test:

Search the new NC module.

Fail if it contains persistent fields named like:

```text
rough_depth
finish_allowance
contour_end
stock_diameter
stock_length
command_type
```

Allowed only in:

* comments
* vocabulary labels
* temporary local scanner variables
* G7x private generator internals, not NC editor storage

Philosophy:

The NC module should work like an old tape reader with a small edit window.

It reads text blocks.

It scans words.

It does not remember more than necessary.

If a struct is not needed to draw/edit the current line, do not create it.

## NC UI rule: fixed Heidenhain-style mode shell, not deep menus

Goal:

Build NC UI around a small fixed set of machine modes, not around a deep
application menu.

The UI should feel closer to old Heidenhain / CNC control logic:

```text
Manual Jog
Programming
Simulation
MDI
Run
```

Each mode has its own screen behavior, but all modes share the same selected NC
file context where useful.

Do not build a modern GUI framework.

Do not build dynamic menu trees.

Do not build generic screens.

Use simple fixed modes and fixed key behavior.

Core idea:

Do not build the UI around a global mode system.

Instead, provide dedicated physical buttons or fixed softkeys for the major
machine functions:

```text
MANUAL
PROGRAM
SIMULATION
MDI
RUN
```

Each function owns its own screen, state, and file selection.

Selecting a file in one function does not automatically select it in another.

Example:

```text
Programming selects PART1.NC
Simulation selects TEST.NC
Run selects JOB42.NC
```

These selections are independent.

A traditional Heidenhain-style MODE key is not required. On older controls it
mainly returned to a previously selected operating area, but for this UI each
major function is directly accessible through its own dedicated button.

Manual remains primarily a DRO/jog screen and does not need to share file
context with the other functions.

Persistent top header:

Create one persistent header visible in all NC modes.

Header shows:

```text
MODE: PROGRAM / SIM / RUN / MDI / MANUAL
FILE: selected file name
TOOL: active tool if known
POS: X / Z DRO if available
STATE: idle / run / hold / alarm
```

Manual Jog may emphasize DRO more strongly, but it should not become a
separate UI universe.

File screen behavior:

File selection is a mode-local screen/action, not a global menu system.

Programming mode:

```text
show files in memory
filter: NC / TOOL / ALL
select file
open for editing
```

Simulation mode:

```text
show files in memory
filter: NC
select file
open for preview/sim
```

Run mode:

```text
show files in memory
filter: NC
select file
prepare/run
```

Special file view may show split storage:

```text
left: internal memory
right: external storage
```

But implement this only as a fixed file browser state, not a generic dual-pane
framework.

File type filters:

Support fixed file filters:

```text
NC files:
  .nc
  .ngc
  .gcode

Tool files:
  .tool
  .tbl

All supported:
  NC + tool files
```

Do not add dynamic plugin filters.

Do not scan unknown extensions as program files.

Tool file loading:

Any relevant mode may allow loading a tool file.

Allowed:

```text
Programming -> load tool file
Simulation  -> load tool file for preview labels
Run         -> load tool file before execution
Manual      -> load tool file if needed
```

Tool file is also plain text.

Do not create hidden tool database first.

If loaded, remember selected tool file path in NC runtime state.

Programming mode:

Programming mode is plain text NC editing.

Responsibilities:

```text
open selected NC file
display text lines
move cursor
edit current line
step word fields
show vocabulary hint for selected word
save file
```

No deep menu.

Use fixed footer keys:

```text
OPEN
SAVE
NEW
FIELD
INSERT
DELETE
SPECIAL
```

SPECIAL may contain rare actions like:

```text
insert G970
insert G971
insert G972
insert G973
load tool file
renumber N blocks
```

Keep SPECIAL as one small fixed list.

No nested tree unless absolutely needed.

Simulation mode:

Simulation mode reads selected NC file.

It does not edit.

Responsibilities:

```text
preview stock
preview contour
preview G7x if supported
single-step simulation lines
show generated path if requested
```

It may call G7x module.

It must not parse NC inside renderer.

It must not build persistent semantic program model.

MDI mode:

MDI mode is one-line command entry.

Responsibilities:

```text
edit one NC line
send line to uCNC
keep small history if easy
```

No program model.

No file save required.

Optional later:

```text
copy MDI line into selected program
```

Not first milestone.

Run mode:

Run mode executes selected NC file.

Responsibilities:

```text
show selected file
show current line
single block
run from current line
full run
hold/stop status
```

Run mode streams source NC lines to uCNC.

It does not create another scheduler.

Generated G7x run uses G7x module as line generator.

Manual Jog mode:

Manual mode is mostly DRO + jog.

Responsibilities:

```text
large X/Z position display
jog keys
feed/jog increment if available
active tool display
machine state/alarm display
```

It may show selected file in header only.

Manual mode should stay simple.

No editor.

No preview.

Special mode / MP-style actions:

Add one fixed SPECIAL screen/action list accessible from Programming.

Inspired by old controls where rare parameters live under special modes.

Allowed actions:

```text
insert G970 graphics extents
insert G971 raw stock
insert G972 clamp length
insert G973 graphics mode
load tool file
renumber N blocks
file filter NC/TOOL/ALL
toggle external storage pane
```

Do not make this a generic settings framework.

Data model:

Persistent state should be tiny:

```c
typedef enum {
    NC_MODE_MANUAL,
    NC_MODE_PROGRAM,
    NC_MODE_SIM,
    NC_MODE_MDI,
    NC_MODE_RUN
} nc_mode_t;

typedef struct {
    nc_mode_t mode;
    char selected_nc_file[NC_PATH_MAX];
    char selected_tool_file[NC_PATH_MAX];
    int selected_line;
    int file_filter;
    bool external_pane_visible;
} nc_state_t;
```

Do not add semantic program structs.

NC source remains text file.

Renderer rule:

NC module prepares a simple snapshot.

Renderer draws snapshot.

Renderer does not own mode logic.

Renderer does not parse NC.

Renderer does not open files.

First milestone:

Implement shell only:

1. Mode enum.
2. Mode switching.
3. Persistent header.
4. File browser with NC/TOOL/ALL filter.
5. Programming mode opens selected NC file as text.
6. Manual mode shows DRO-style placeholder/header.
7. MDI mode has one-line entry placeholder.
8. Sim and Run modes show selected file context but may be stub screens.
9. No deep menu tree.
10. No semantic model.

Acceptance criteria:

A user can understand the UI as:

```text
choose mode
choose file
edit / simulate / run
```

Not:

```text
navigate application tree
manage hidden project state
convert LCAM to NC
```

Mode shell must stay fixed, visible, and boring.

This is a machine control UI, not a desktop application.

## 2026-06-07 audit: why LeanCam grew too large

Status: done now / use this as a regression baseline.

Task:

Find why LeanCam grew too large.

Compare old LeanCam and new NC module.

Do not just count lines.

Classify code by cause:

1. real UI
2. file IO
3. editor mechanics
4. renderer glue
5. old LCAM compatibility
6. semantic model / private language
7. duplicate parser/generator paths
8. stream/cache/pacing layers
9. debug/recovery scaffolding
10. abstractions/callback wrappers

For each large function/file:

* what problem did it solve?
* is that problem still real in NC?
* was it caused by duplicate state?
* can it disappear if NC text is the only source of truth?

Audit inputs checked now:

* LeanCam biggest files:

  * `visual/leancam_visual.c`: about 4327 lines.
  * `leancam_bridge.c`: about 2809 lines.
  * `leancam_gcode.c`: about 2783 lines.
  * `leancam_editor.c`: about 699 lines.
  * `leancam_files.c`: about 572 lines.
  * `leancam_code.c`: about 561 lines.
  * `leancam_menu.c`: about 527 lines.
  * `leancam_tool_catalog.c`: about 387 lines.
  * `leancam_nc_viewer.c`: about 369 lines.

* NC biggest files now:

  * `nc_visual.c`: about 2824 lines.
  * `nc.c`: about 472 lines.
  * `nc_files.c`: about 231 lines.
  * `nc_mode.c`: about 141 lines.
  * `nc_tools.c`: about 114 lines.
  * `nc_vocab.c`: about 100 lines.

Important current finding:

NC avoided the largest LeanCam mistake, the private LCAM program language, but
`nc_visual.c` is already becoming the new pressure point. It now owns:

* mode-local file state
* state save/load
* run state
* footer dispatch
* tool table drawing
* preview geometry
* G7x-like contour scanning for display
* rough area drawing
* arc/chamfer/radius drawing math

That is not yet the old LeanCam semantic model, but it is the same shape of
growth starting in a different place. NC must not let the renderer become a
second parser or a shadow CAM engine.

### Top 10 growth sources

1. Renderer doing preview/simulation geometry.

   LeanCam:

   * `visual/leancam_visual.c` grew by drawing normal screens, split previews,
     fullscreen previews, live material removal, tool glyphs, roughing hatches,
     arc/corner display, and renderer-side preview pacing.

   NC now:

   * `nc_visual.c` already contains preview structs, vector math, arc centers,
     chamfer/radius construction, contour collection, rough-area hatching, and
     G71/G72 context drawing.

   Cause class:

   * real UI
   * renderer glue
   * duplicate parser/generator paths
   * stream/cache/pacing layers if allowed to continue

   Is the problem still real in NC?

   * Preview is real.
   * Renderer-side geometry ownership is not.

   Duplicate state?

   * Yes. Preview is deriving temporary contour/stock/tool state from NC text
     inside the visual file.

   Can it disappear if NC text is source of truth?

   * Partly. The visual can scan visible/current text for a thin preview, but
     anything resembling G7x roughing, reusable contour ownership, or generated
     path must move to G7x/preview helper code that returns simple draw
     primitives or one generated NC line at a time.

2. Bridge/controller becoming application kernel.

   LeanCam:

   * `leancam_bridge.c` owns modes, file browser state, edit state, tool catalog
     sync, preview stepping, run dispatch, messages, snapshots, autosave, and
     keypad routing.

   NC now:

   * `nc_visual.c` is starting to combine mode state, file state, run state,
     footer dispatch, and rendering.

   Cause class:

   * real UI
   * file IO
   * editor mechanics
   * abstractions/callback wrappers
   * stream/cache/pacing layers

   Is the problem still real in NC?

   * A small mode shell is real.
   * A central application kernel is not.

   Duplicate state?

   * Yes, when mode/file/run state lives apart from the file text and must be
     synchronized back.

   Can it disappear if NC text is source of truth?

   * Mostly. Keep only tiny mode/file selection state. Program content remains
     text. Run state should be line index plus uCNC stream status, not a
     private controller.

3. G-code generator mixed with UI dialect translation.

   LeanCam:

   * `leancam_gcode.c` contains direct motion emitters, old drill/tap/thread
     helpers, G71/G72 roughing, G76 stepping, setup validation, tool context,
     state snapshots, and stepper replay paths.

   NC now:

   * `nc_emit.c` is still small, but `nc_visual.c` preview scans G7x-looking
     regions and draws generated-looking roughing context.

   Cause class:

   * semantic model / private language
   * duplicate parser/generator paths
   * stream/cache/pacing layers

   Is the problem still real in NC?

   * Raw line emission is real.
   * G7x generation belongs outside NC editor/visual.

   Duplicate state?

   * Yes, if preview and run each invent their own G7x interpretation.

   Can it disappear if NC text is source of truth?

   * Yes. NC emits source text. G7x module scans source text and emits generated
     text. Renderer draws only snapshots/draw primitives.

4. Private LCAM compatibility and migration paths.

   LeanCam:

   * Pipe rows, braces, SETUP fields, TOOLCALL, old preset rows, and LCAM
     command conversion created parsers, validators, template paths, and
     compatibility shims.

   NC now:

   * Current NC rejects old pipe syntax in core text loading.

   Cause class:

   * old LCAM compatibility
   * semantic model / private language

   Is the problem still real in NC?

   * No.

   Duplicate state?

   * Yes, LCAM made row text and semantic meaning separate.

   Can it disappear if NC text is source of truth?

   * Yes. Delete/bypass LCAM support instead of migrating it.

5. Tool catalog policy becoming hidden database.

   LeanCam:

   * Tool rows, catalog file sync, default tools, validation policy, glyphs,
     offsets, feeds, DOC, and active-tool lookup spread across bridge, visual,
     generator, and catalog files.

   NC now:

   * `nc_tools.c/h` introduces `nc_tool_t`, active tool lookup, default tool
     rows, and tool glyph drawing in `nc_visual.c`.

   Cause class:

   * real UI
   * semantic model / private language
   * duplicate parser/generator paths

   Is the problem still real in NC?

   * Tool display is real.
   * Hidden tool database is not.

   Duplicate state?

   * Potentially. `nc_tool_t` is acceptable only as a temporary local parse
     result, not persistent storage.

   Can it disappear if NC text is source of truth?

   * Mostly. Tool files should be plain text. Active tool data may be scanned
     from text on demand and forgotten.

6. File browser and storage state leaking into UI owner.

   LeanCam:

   * File list, prompt, duplicate, delete, refresh, retry/busy state, and
     current path were wired into bridge/menu logic.

   NC now:

   * `nc_files.c` is reasonably small, but `nc_visual.c` also saves mode paths
     and handles file open/delete/new/refresh.

   Cause class:

   * file IO
   * real UI
   * abstractions/callback wrappers

   Is the problem still real in NC?

   * Fixed file browser is real.
   * Generic file manager behavior is not.

   Duplicate state?

   * Some. Mode path memory is state separate from file content, but acceptable
     if it remains tiny.

   Can it disappear if NC text is source of truth?

   * File content state disappears; selected path state remains.

7. Deep menu/action dispatch tables.

   LeanCam:

   * `leancam_menu.c/h`, schema/templates, bridge actions, draft state, and
     callbacks created a general conversational UI engine.

   NC now:

   * `nc_mode.c/h` uses static footer tables. This is acceptable now, but the
     action enum is already broad and includes stubs.

   Cause class:

   * real UI
   * abstractions/callback wrappers

   Is the problem still real in NC?

   * Fixed footer keys are real.
   * Dynamic/deep menus are not.

   Duplicate state?

   * Not yet, unless actions start carrying their own models.

   Can it disappear if NC text is source of truth?

   * Keep only mode + key -> action. Delete unused/stub actions quickly.

8. Runtime stream/cache/pacing layers.

   LeanCam:

   * Old staged direct stream paths, step-stream pacing, preview ack stepping,
     run view state, generated temp buffers, and execution-controller pacing
     accumulated around uCNC's existing stream/planner.

   NC now:

   * `nc_visual_run_*` is a UI demo/run simulator that emits/skips source lines
     for status. It must not become the real scheduler.

   Cause class:

   * stream/cache/pacing layers
   * duplicate parser/generator paths

   Is the problem still real in NC?

   * Showing current line is real.
   * Scheduling execution is not NC visual's job.

   Duplicate state?

   * Yes if NC tracks run truth apart from uCNC stream/planner state.

   Can it disappear if NC text is source of truth?

   * Yes. Raw run reads next source line only when uCNC asks.

9. Debug/recovery scaffolding left in live files.

   LeanCam:

   * HSTX/PSRAM/debug/recovery/torture findings, serial messages, perf meters,
     and watchdog scaffolding expanded live code paths.

   NC now:

   * `nc_module.c` has watchdog reboot logging; `nc_visual.c` serial-selected
     line/file messages are useful bring-up aids.

   Cause class:

   * debug/recovery scaffolding

   Is the problem still real in NC?

   * Bring-up logging is real.
   * Permanent debug UI is not.

   Duplicate state?

   * Usually no, but it clutters ownership.

   Can it disappear if NC text is source of truth?

   * Not directly. Keep debug in small optional blocks and remove once stable.

10. Wrapper layers introduced before they paid rent.

   LeanCam:

   * Snapshot wrappers, resource gates, menu callbacks, schema helpers, multiple
     command parsers, default resolvers, visual helper layers, and generator
     callbacks each solved a real local problem but multiplied ownership
     boundaries.

   NC now:

   * `nc_editor.c`, `nc_text`, `nc_emit`, `nc_presets`, `nc_tools`, `nc_mode`,
     and `nc_files` are okay only if they stay direct.

   Cause class:

   * abstractions/callback wrappers
   * editor mechanics
   * file IO

   Is the problem still real in NC?

   * Small leaf helpers are real.
   * Generic engines are not.

   Duplicate state?

   * Not by itself, but wrappers make duplicate state easier to hide.

   Can it disappear if NC text is source of truth?

   * Keep helper files leaf-only: parse a line, edit a span, list files, draw a
     fixed screen. No helper owns program meaning.

### Delete candidates

LeanCam delete/bypass candidates after NC is usable:

* `leancam_bridge.c` as live runtime owner.
* `leancam_editor.c/h` and `leancam_program.c/h` private program editing path.
* `leancam_code.c/h` private command/field parser once NC text helpers cover
  visible editing.
* `leancam_schema.c/h`, old menu/schema/template plumbing.
* Pipe/braces/SETUP/TOOLCALL compatibility code in generator/editor/tests.
* `leancam_tool_catalog.c/h` as hidden catalog policy; replace with plain text
  tool files scanned on demand.
* `leancam_nc_viewer.c/h` once NC mode screens own file viewing.
* Renderer-side generated preview and live material-removal caches in
  `visual/leancam_visual.c`.
* Old run/cache/pacing paths that duplicate uCNC stream/planner ownership.
* Debug/recovery scaffolding that is historical rather than live behavior.

NC immediate cleanup candidates before it hardens:

* Split `nc_visual.c` into fixed-screen drawing and thin preview drawing, or
  move preview math into a leaf `nc_preview.c` that returns simple draw data.
* Remove `rough_depth_x` / `rough_depth_z` field names from NC visual preview
  structs; they violate the spirit of the text-only naming rule even if local.
* Keep `nc_tool_t` strictly temporary. Do not store an array of tools.
* Keep `nc_visual_run_*` as UI status only. Real run must stream source text
  through uCNC.
* Delete stubs/actions from `nc_mode` if they are not used soon.
* Keep state file to selected paths/mode only. Do not store parsed stock,
  contour, tool, or preview metadata.

### Lessons to prevent NC from becoming LeanCam again

* Text is the program. Parsed data is disposable.
* Renderer may scan text for a thin current-view hint, but must not become a
  G-code generator or contour owner.
* G7x owns roughing generation and contour rules. NC editor does not.
* Tool files are plain text. Any `nc_tool_t` is a temporary parse result only.
* File browser state is path/index only. File contents remain text.
* Mode shell is fixed. No dynamic menu tree.
* Footer tables are static and small. Stub actions should be deleted or
  implemented directly.
* Run mode shows source lines and asks uCNC to consume source/generator text.
  It does not schedule motion.
* Debug code expires. Keep bring-up prints behind clear optional blocks or
  remove them.
* Every new struct must answer: does this draw/edit the current line, hold a
  selected path, or produce one generated text line? If not, do not add it.

Result now:

* LeanCam's largest growth was not "UI" alone. It was duplicate ownership:
  text plus semantic model, renderer plus preview generator, bridge plus
  scheduler, tool rows plus catalog policy, file browser plus app kernel.
* NC currently obeys the no-private-language rule in `nc.c` and line editing.
* NC is already at risk in `nc_visual.c`; preview/geometry/run/tool logic must
  be kept leaf-local or moved out before it becomes the new bridge/generator.

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

Status: mostly done / SETUP replacement in progress.

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
* SETUP is now rejected by generator/preflight with a clear obsolete-format message.
* G970/G971/G972/G973 are accepted and validated as non-motion NC rows.
* TOOL and preset templates create plain text rows; TOOLCALL generation is removed.
* Tool catalog storage now writes `TOOL T{} R{} ...` rows instead of `TOOL|...`.
* Shared plain text parser no longer accepts pipe-delimited rows or brace-wrapped saved values.
* Tool catalog import rejects old pipe/brace tool rows instead of normalizing them.
* Editor/expression field helpers no longer treat `|` as a draft field separator.
* Active context checks use plain command matching for TOOL and G970-G973 rows.
* Plain command/field parsing is centralized in `leancam_text.c/h` and shared by presets, regions, validation, tool catalog, editor/UI, and bridge wrappers.
* G-code generator field lookup now uses the shared plain text parser instead of carrying a second pipe-era parser.
* Draft commit/display no longer blocks or annotates rows with tool validation warnings.
* Tool lookup uses static default rows for T0-T8 when the catalog/program has no single explicit match.
* Tool selection is by real `T` words on cycles or nearest real `TOOL` row/catalog entry.
* Run/preflight no longer carries a fake tool validation callback that always passes.
* Preset allowance fields now use real `X`/`Z` words instead of `X_ALLOW`/`Z_ALLOW`.
* Generator dispatch no longer runs old private OD/ID/FACE/CUT/GROOVE/CHAMFER/RADIUS command rows.
* Raw G71/G72 host tests now use plain rows plus explicit `G80`, matching stored NC text.
* Drill/tap/thread templates now dispatch as G-code-ish `G74`, `G84`, direct `G33`, and `G76` rows instead of relying only on private `DRILL`/`TAP`/`THREAD` commands.
* Editor navigation now treats plain tokens like `T1`, `R_FEED120`, and `X50` as editable fields directly. Braces are only needed for preset/template placeholders.
* G1 helper `AUTO` metadata is gone from active rows and host G71/G72 tests.
* TOOLCALL draft/default resolution was removed; preset defaults resolve directly from `TOOL T...` rows.
* Preset insert resolves template defaults to plain words and inserts the final NC region directly; `OD/ID/FACE/RECESS` rows are no longer stored even temporarily.
* LVDS preview no longer contains a separate renderer for old private `OD/ID/FACE/DRILL/TAP/CUT/GROOVE/CHAMFER/RADIUS/THR_*` rows. Preview follows raw/grouped NC and current live G-code state.

---

## 5. OD/ID/FACE/RECESS are presets only

Status: done for the live path.

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
* Presets insert the final NC rows directly; there is no temporary `ID ...` / `OD ...` program row to clean up.
* Preset expansion reads stock from G971 words (`X` for OD, `I` for ID), not old `SETUP` fields.
* Old private preset/cycle visual preview support was removed from the live renderer path. The UI may still label buttons as OD/ID/FACE/RECESS, but saved/run text is NC.

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

15. Replace LeanCam SETUP block with Eltropilot-style setup/graphics G-codes

Status: in progress.

Current implementation pass:

* LeanCam no longer emits G20/G21 from runtime, program, or DXF-lite headers.
* G20/G21 are documented as uCNC inch/mm modal words only, not setup metadata.
* G970/G971/G972/G973 validate as normal non-motion NC rows, including N-numbered rows.
* Old SETUP rows are rejected by generator/preflight with `SETUP is obsolete; use G970/G971/G972/G973`.
* Program menu key `0` opens the tool catalog instead of inserting TOOLCALL.
* TOOLCALL template/default resolution was removed from live code.
* Preview stock derives its visual setup from G971/G972 when those NC rows are present.
* Auto "setup first" draft insertion was removed; new files are plain empty NC programs.
* SETUP.FIELD expression resolution was removed from live editor/template code.
* Editor default resolution now only knows literal, `THIS.*`, and `TOOL.*`.
* Preset expansion reads stock from G971 words (`X` for OD, `I` for ID), not SETUP fields.
* NC viewer setup preview captures G971/G972 rows directly.

Goal:
Remove current non-G-code SETUP block completely and replace it with old Eltropilot-inspired CNC-looking setup commands.

Remove completely:

* SETUP block grammar
* SETUP.FIELD references
* key/value setup forms
* exposed JSON/YAML-like metadata
* non-G-code template setup syntax
* any hidden editor-only setup state that is not backed by NC lines

Allowed human-readable layer:
Only parenthesized CNC-style comments.

Comment behavior:

* optional only
* parser/simulation ignores comments fully
* comments are truncated in editor UI
* no wrapping by default
* full comment visible only when selected/opened

Adopt LeanCam private Eltropilot-style setup commands:

Example:

N70 G970 X-10 U120 Z-150 W30   (GRAPHICS EXTENTS)
N71 G971 X80 Z125 E0           (RAW STOCK)
N72 G972 C15                   (CLAMP LENGTH)
N73 G973 P7                    (GRAPHICS MODE)

Meaning:

* G970 = graphics/display extents
* G971 = raw stock dimensions
* G972 = clamping length
* G973 = graphics mode/options

These are inspired by old Eltropilot/Pilot behavior, but LeanCam is not required to be byte-compatible with Eltropilot.

G970: graphics/display extents

* Non-motion command.
* Defines preview/display envelope.
* X = minimum X display value
* U = maximum X display value
* Z = minimum Z display value
* W = maximum Z display value
* Required: X, U, Z, W
* Validation:

  * X < U
  * Z < W
* Updates preview extents only.
* Does not move machine.

G971: raw stock dimensions

* Non-motion command.
* Defines current raw workpiece blank.
* X = stock outside diameter
* Z = stock length
* I = stock inside diameter, optional, default 0
* E = extra/front allowance in Z+ direction, optional, default 0
* Required: X, Z
* Validation:

  * X > 0
  * Z > 0
  * I >= 0
  * I < X
  * E >= 0
* Updates stock/simulation metadata only.
* Does not move machine.

G972: clamping length

* Non-motion command.
* Defines held length inside chuck or clamp.
* C = clamping length
* Required: C
* Validation:

  * C >= 0
* Updates chuck/stock display metadata only.
* Does not move machine.

G973: graphics mode/options

* Non-motion command.
* Defines what preview shows.
* P = graphics mode bitmask
* Supported values:

  * 0 = axes only
  * 1 = stock
  * 3 = stock + chuck
  * 7 = stock + chuck + tailstock
* Required: P
* Validation:

  * P must be one of 0, 1, 3, 7
* Updates preview options only.
* Does not move machine.

Parser rules:

* These are normal NC blocks.
* Store them as normal words:

  * G970
  * X...
  * U...
  * Z...
  * W...
  * etc.
* Do not create parser fields like STOCK_OD, STOCK_LEN, GRAPHICS_MIN_X.
* Semantic meaning is resolved by command table for G970-G973.
* Unknown 3-digit G-code is unsupported.
* G20/G21 remain inch/mm only and must not be reused.

Simulation behavior:

* G970 updates visible extents / simulation viewport basis.
* G971 creates or updates raw stock.
* G972 updates clamping display length.
* G973 selects visible graphics elements.
* None of G970-G973 emit motion, planner moves, or spindle/feed changes.

Editor behavior:

* Editor displays these as normal NC lines.
* Editor may show short labels:

  * G970 GRAPHICS EXTENTS
  * G971 RAW STOCK
  * G972 CLAMP LENGTH
  * G973 GRAPHICS MODE
* Editor must not create a separate SETUP UI model that can diverge from the NC lines.
* NC file is the source of truth.

Round-trip behavior:

* Loading and saving unchanged file must preserve these G970-G973 lines.
* Field edits modify only backing word values.
* Comments remain optional and ignored by simulation.

Tests:

1. Remove old SETUP block parser support.
2. Parse:
   N70 G970 X-10 U120 Z-150 W30
   and identify command as GRAPHICS_EXTENTS.
3. Parse:
   N71 G971 X80 Z125 E0
   and identify command as RAW_STOCK.
4. Parse:
   N72 G972 C15
   and identify command as CLAMP_LENGTH.
5. Parse:
   N73 G973 P7
   and identify command as GRAPHICS_MODE.
6. G970 without X/U/Z/W is an error.
7. G970 with X >= U is an error.
8. G971 without X or Z is an error.
9. G971 with I >= X is an error.
10. G972 without C is an error.
11. G973 with P2 is an error.
12. G20/G21 still parse only as inch/mm.
13. G970-G973 do not generate motion.
14. Saving unchanged file preserves NC lines.
15. Old SETUP block input is rejected with clear message.

Design target:
Old dead Eltropilot-style setup rebuilt as modern LeanCam NC commands. No hidden setup block. No JSON/YAML-like metadata. No editor-private source of truth.

## 15.1

LeanCam Task: Remove Semantic Words From Core And Build Strict Vocabulary Layer

Status: architecture cleanup / language stabilization

Background

Current system started accumulating semantic fields such as:

* R_FEED
* R_DEPTH
* Q_END
* N_START
* CONTOUR_END
* FINISH_ALLOWANCE_X

This creates a second language inside the controller.

The goal is to eliminate this layer completely.

The controller shall have exactly one machine language.

Supported NC words are stored exactly as they appear in the program:

* G
* M
* N
* X
* Z
* U
* W
* I
* K
* R
* P
* Q
* D
* F
* S
* T
* C
* E

Everything else is vocabulary-layer meaning only.

The parser owns syntax.

The vocabulary table owns meaning.

The editor only displays and edits meaning.

The NC file is the source of truth.

---

## RULE 1

Parser owns syntax.

Parser does not own meaning.

Forbidden:

```c
block->contour_end
block->rough_feed
block->rough_depth
block->thread_depth
block->stock_diameter
block->stock_length
```

Allowed:

```c
word('Q',200)
word('F',0.2)
word('R',1.0)
word('X',80)
word('Z',125)
```

Parser stores words only.

---

## RULE 2

Vocabulary table owns meaning.

Meaning depends on command context.

Example:

```gcode
G71 P100 Q200
```

Vocabulary table:

```text
G71 P = contour start N block
G71 Q = contour end N block
```

Editor displays:

```text
Contour Start = N100
Contour End   = N200
```

Storage:

```text
P100
Q200
```

Nothing else exists.

---

## RULE 3

Same letter may mean different things.

Never create a global semantic meaning.

Examples:

```gcode
G71 R1.0
```

Vocabulary:

```text
Retract = 1.0
```

```gcode
G02 X50 Z10 R5
```

Vocabulary:

```text
Arc Radius = 5
```

Storage:

```text
R5
```

Same letter.

Different meaning.

---

## RULE 4

Context determines vocabulary.

Vocabulary lookup key:

```text
command
word letter
position
```

Example:

G71 first line:

```text
U = depth/pass
R = retract
```

G71 second line:

```text
U = finish allowance X
W = finish allowance Z
```

Same letter.

Different meaning.

---

## RULE 5

No automatic dialect guessing.

Forbidden:

```text
Looks like Haas
Looks like Fanuc
Maybe LinuxCNC
Probably Siemens
```

Allowed:

```text
dialect = FANUC_HAAS_LATHE
```

or

```text
dialect = UNKNOWN
```

If dialect = UNKNOWN:

```text
mark unsupported
preserve raw text if possible
do not guess
do not reinterpret
do not auto-convert
```

---

## RULE 6

Editor field modification changes exactly one backing word.

Example:

Input:

```gcode
G71 P100 Q200 U0.5 W0.1
```

User changes:

```text
Contour End = 250
```

Result:

```gcode
G71 P100 Q250 U0.5 W0.1
```

Only Q changes.

Nothing else.

---

## RULE 7

Round-trip requirement.

Input:

```gcode
G71 U2.0 R1.0
G71 P100 Q200 U0.5 W0.1 F0.2
```

Load.

Display.

Save.

Result must remain identical.

Byte-for-byte if practical.

Never regenerate formatting unnecessarily.

Never reorder words unnecessarily.

---

## RULE 8

Supported vocabulary set.

Motion:

```text
G00
G01
G02
G03
```

Lathe cycles:

```text
G70
G71
G72
G73
G80
```

LeanCam / Eltropilot-style setup graphics:

```text
G970
G971
G972
G973
```

Everything else:

```text
UNSUPPORTED
```

Unsupported means:

```text
no semantic editing
no conversion
no guessed dialect
preserve raw text if possible
reject execution if unsafe
```

---

## RULE 9

Referenced contour mode.

Example:

```gcode
G71 P100 Q200

N100 G0 X40 Z2
N110 G1 X30
N120 G1 Z-20
N200 G1 X50
```

Editor:

```text
Contour:
N100 -> N200
```

Storage unchanged.

---

## RULE 10

Inline contour mode.

Example:

```gcode
G71 ...
G0 X40 Z2
G1 X30
G1 Z-20
G1 X50
G80
```

Editor:

```text
Roughing Contour
```

Storage remains normal NC blocks.

Forbidden:

```text
CONTOUR_BEGIN
CONTOUR_END
INLINE_CONTOUR
```

---

## RULE 11

Validation.

Referenced contour mode:

```text
P block must exist
Q block must exist
P <= Q
all referenced contour blocks must exist
```

Missing reference:

```text
ERROR
```

Not warning.

---

## RULE 12

Vocabulary examples.

G71 First Line

Storage:

```gcode
G71 U2.0 R1.0
```

Editor:

```text
Depth per Pass = 2.0
Retract = 1.0
```

---

G71 Second Line

Storage:

```gcode
G71 P100 Q200 U0.5 W0.1 F0.2
```

Editor:

```text
Contour Start = N100
Contour End = N200
Finish Allowance X = 0.5
Finish Allowance Z = 0.1
Feed = 0.2
```

---

G70

Storage:

```gcode
G70 P100 Q200
```

Editor:

```text
Finish Contour
Start = N100
End = N200
```

---

G02

Storage:

```gcode
G02 X50 Z20 R5
```

Editor:

```text
Arc Radius = 5
```

Not:

```text
Retract
```

---

## RULE 13

No editor-only source of truth.

If something affects:

* parser
* preview
* simulation
* generated toolpath
* execution planning

it must exist as NC words in the program.

Allowed:

```gcode
N71 G971 X80 Z125
```

Forbidden:

```text
hidden editor stock model
JSON sidecar
YAML sidecar
SETUP.FIELD
private metadata store
```

NC program remains authoritative.

---

## RULE 14

LeanCam Eltropilot-style setup commands.

G970 Graphics Extents

Example:

```gcode
N70 G970 X-10 U120 Z-150 W30
```

Meaning:

```text
X = graphics minimum X
U = graphics maximum X
Z = graphics minimum Z
W = graphics maximum Z
```

Required:

```text
X U Z W
```

Validation:

```text
X < U
Z < W
```

Non-motion command.

---

G971 Raw Stock

Example:

```gcode
N71 G971 X80 Z125 E0
```

Meaning:

```text
X = stock OD
Z = stock length
I = stock ID optional
E = extra/front allowance optional
```

Required:

```text
X Z
```

Validation:

```text
X > 0
Z > 0
I >= 0
I < X
E >= 0
```

Non-motion command.

---

G972 Clamp Length

Example:

```gcode
N72 G972 C15
```

Meaning:

```text
C = clamped length
```

Required:

```text
C
```

Validation:

```text
C >= 0
```

Non-motion command.

---

G973 Graphics Mode

Example:

```gcode
N73 G973 P7
```

Meaning:

```text
P = graphics mode
```

Supported values:

```text
0 = axes only
1 = stock
3 = stock + chuck
7 = stock + chuck + tailstock
```

Validation:

```text
P must be 0,1,3 or 7
```

Non-motion command.

---

## FINAL GOAL

Parser becomes smaller.

Generator becomes smaller.

Simulation becomes table-driven.

Editor becomes a thin display/edit layer.

NC file remains standard LeanCam language.

No private language.

No semantic fields in controller core.

No hidden setup state.

Only NC words plus vocabulary tables.


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

## 15.2 Architecture Freeze Rules

Status: active.

Purpose:

Prevent LeanCam from reintroducing a second language, hidden state, multiple contour ownership models, or editor-owned semantics.

These rules override future convenience shortcuts.

---

### FREEZE 1

NC file is the only source of truth.

If a value affects:

* simulation
* preview
* execution
* toolpath generation
* stock model
* clamping model
* graphics mode

then it must exist as NC words in the program.

Allowed:

```gcode
G971 X80 Z125
G972 C15
G973 P7
```

Forbidden:

```text
hidden stock structure
runtime-only stock model
editor-only setup object
JSON sidecar
YAML sidecar
SETUP.FIELD
```

Runtime may cache values temporarily.

Runtime cache must always be rebuildable from NC text.

---

### FREEZE 2

Vocabulary owns meaning.

Editor does not own meaning.

Parser does not own meaning.

Generator does not own meaning.

Only vocabulary tables define:

```text
G71 R = retract
G02 R = arc radius
G971 X = stock diameter
```

Editor only displays vocabulary.

Parser only stores words.

Generator only consumes words.

---

### FREEZE 3

No semantic fields in controller core.

Forbidden:

```c
rough_feed
rough_depth
contour_end
stock_diameter
stock_length
graphics_mode
```

Allowed:

```c
word('F',120)
word('Q',200)
word('X',80)
word('P',7)
```

Core stores words.

Vocabulary explains them.

---

### FREEZE 4

G970-G979 reserved namespace.

Reserved for:

```text
LeanCam system/setup/simulation commands
```

Current assignments:

```text
G970 graphics extents
G971 raw stock
G972 clamp length
G973 graphics mode
```

Unassigned:

```text
G974
G975
G976
G977
G978
G979
```

Reserved.

Do not use for machining cycles.

Do not use for roughing cycles.

Do not use for contour operations.

Future use only if clearly setup/simulation related.

---

### FREEZE 5

Single contour ownership model for first stable release.

Supported:

```gcode
G71
...
G80
```

and

```gcode
G72
...
G80
```

Only.

G80 owns contour termination.

No alternative contour ownership systems.

---

### FREEZE 6

Deferred features.

The following concepts are frozen and not part of first stable release:

```text
CTR
ENDCTR
C=N100
named contours
persistent contour objects
reusable contour references
P/Q contour ownership
```

These may return later.

They are not active architecture.

They must not influence current parser/editor/runtime decisions.

---

### FREEZE 7

No dialect guessing.

Forbidden:

```text
looks like Haas
looks like Fanuc
looks like LinuxCNC
looks like Siemens
```

Allowed:

```text
supported
unsupported
```

Unknown commands remain unknown.

No automatic reinterpretation.

---

### FREEZE 8

Semantic round-trip required.

Requirement:

```text
Load
Display
Save
```

must preserve:

* commands
* words
* values
* behavior

Formatting preservation is desirable.

Formatting preservation is not a design goal.

Semantic preservation is mandatory.

Machine behavior preservation is mandatory.

---

### FREEZE 9

No hidden contour model.

Forbidden:

```text
anonymous contour database
editor contour object
secondary contour representation
```

Current contour exists as:

```gcode
G71
...
G1
...
G80
```

The NC lines themselves are the contour.

---

### FREEZE 10

When in doubt:

Prefer:

```text
visible NC line
```

over:

```text
hidden structure
```

Prefer:

```text
simple parser
```

over:

```text
clever architecture
```

Prefer:

```text
boring implementation
```

over:

```text
future-proof abstraction
```

Project rule remains:

Less architecture.
More visible behavior.



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

---

## NC module wiring rule

Status: active rule.

Keep the direction bottom-up and boring:

```text
user actions
-> controller/state
-> nc_visual
-> lvds renderer
```

Runtime facts and selected mode/action state belong to controller/state.

Visual code consumes state and says how to draw it.

LVDS code draws pixels.

Emitter/run code is called from controller/state and may ask text/files for source lines, but it must not be reached sideways from visuals.

Do not add tiny helper files for one screen strip or one small label table unless it removes a real dependency loop. Prefer existing state/menu/visual files over creating another island.
