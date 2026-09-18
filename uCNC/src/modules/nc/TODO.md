# NC TODO

## Next UI priorities (proposed; not implemented)

- [ ] Storage status and explicit file-operation errors: distinguish not
  mounted, open/read failure and save failure. Use filesystem state; there is
  no wired card-detect input on the current board.
- [ ] Keep the unsaved editor document available if autosave on screen change
  fails; show retry/cancel instead of replacing it with another loaded file.
- [ ] Enforce/explain short SD filenames in create/rename flows while
  FF_USE_LFN=0. Keep the working SD driver/configuration unchanged.
- [ ] Show active G18/G90/G94, G7/G8 and units near RUN so selected-cycle
  execution prerequisites are visible before submitting a command.
- [ ] Add preview zoom/pan with a visible anchor, then finish chuck/setup layers.

Before changing storage behavior, read the history and bench checklist in
[`docs/sd-card-history.md`](../../../../docs/sd-card-history.md).

## UI feedback completed (2026-09-16)

- [x] Persistent settings-invalid banner with `$RST=*` guidance and explicit
  notice that it resets settings/offsets; storage-write failure has its own
  message. No automatic reset or unlock.
- [x] Distinguish controller alarm, untrusted position, door and jog locks in
  on-screen guidance; refresh when controller/settings state changes.
- [x] Show readable command errors with numeric codes and source lines for NC
  RUN/selected-line sends, including rejected parameters and missing G80.
- [x] Hide UI timing counters by default (`NC_UI_DEBUG_TIMING` enables them).
- [x] Stop repeated corner diagnostics during preview redraws
  (`G7X_DEBUG_CORNERS` enables them).
- [x] Preserve selection when switching screens that open the same file during
  the current session; cursor persistence across reboot remains future work.
- [ ] Bench-check banner readability, error recovery and screen selection on
  the actual display. Software checks and firmware build do not validate layout.

## Dedicated depth-per-pass control

- [ ] Add a dedicated rotary encoder or potentiometer for the G71 U depth of
  cut (material removed per pass, not the F feed-rate override).
- [ ] EDIT: use the knob to set the selected cycle's U value and save it in the
  program through the normal editing flow.
- [ ] RUN: apply a live override relative to programmed U, approximately -80%
  to +80% (20%–180% of the programmed depth). Show programmed U, requested
  override and effective depth separately.
- [ ] During an engaged cutting pass, allow only a decrease in depth. Defer any
  requested increase until the G0/retract/return phase, then apply it to the
  next pass. A later reduction must replace any pending higher request.
- [ ] Define and test the safe transition for a decrease during engagement:
  account for already queued motion, avoid abrupt tool movement, and maintain
  contour, finish allowance and remaining-stock bookkeeping. Do not simply
  change U on motion already queued in the planner.
- [ ] Keep this in the shared G7x generator/runtime; NC provides knob input and
  display. Identify the pass phase explicitly rather than treating every G0 as
  permission to increase depth. Decide later how this maps to G72's W word.
- [ ] Bench tests: knob limits/noise, decrease while cutting, deferred increase,
  changed pending request, hold/resume, Stop/reset, and the final shallow pass.

## Completed software work (2026-09-16)

- [x] Serialize generated G7x blocks before subsequent source commands; preserve
  G7/G8 conversion and propagate errors instead of accepting a failed G80.
- [x] Clear cycle state on parse failure, reset and NC Stop; feed hold/resume
  controls actual motion and remains available after the file reaches EOF.
- [x] Enable native single-line G76 with radial first-cut/decreasing infeed,
  G20/G21 scaling, work offsets, input validation and spring passes.
- [x] Make NC preview return explicit errors for invalid/unsupported cycles.
- [x] Add generator, preview and actual parser/planner regression suites:
  `python tools/test_g7x.py all` (G33 motion is intercepted).
- [ ] Machine-test spindle synchronization, physical hold/resume/Stop and reset.
- [ ] NC: G76 preview and document-source adapter. G7x: G70/PQ (see its checklist).

The remaining design backlog follows. Native G76's exact supported dialect is
documented in `../g7x/README.md`; hardware validation is still required.

- Later: from RUN, jump to EDIT with the same file and line marked.
- Later: when EDIT and SIM are intentionally linked, entering SIM should reopen the edited file without making RUN share that file.
- Tools: TOOLS mode owns the global `.t` table; RUN/SIM/EDIT link to it by `Tn`. Later G7x tool/preset choices should live in G7x commands, not by copying tool rows into programs.
- Pre-alpha visual/test pass:
  - Treat the current UI as stable enough for testing, but not final: missing/rough visuals should be tracked instead of hidden in code.
  - Chuck drawing is still partial. Make it a real lathe chuck/stock holder view, not only one jaw/stock clamp hint.
  - `G970` is already used as stock/setup metadata (`X/U/Z/W`) through `nc_sim_collect_preview`, but the preview does not yet draw the setup region as its own explicit visual layer. Use it for stock origin, stock extents, and setup envelope instead of only fitting stock size.
  - Place small corner feature labels (`R2`, `C5`) by cut direction and operation: for normal G71 OD/x-minus cuts use the left/top side of the corner point; use the opposite side for boring/ID and other reversed directions.
  - Add preview zoom/pan. This needs a visible preview cursor/anchor so zoom has a center and users can inspect corners/clearance.
  - [x] Added TESTING.md checklist for EDIT/SIM/RUN; bench verification remains open: mode switch, file persistence, G970 stock fit, G71/G72 contour labels, live stock removal, run cursor, and reboot recovery.
  - Keep preview-only line labels and UI helpers in NC, but keep all G71/G72 generator/roughing/finish logic in `g7x`.

## Cycle implementation belongs to G7x

The old mixed promotion/completion list is replaced by the audited
[G7x checklist](../g7x/TODO.md). Completed foundations are checked there;
P/Q, G70, allowance-aware approach and directional roughing remain open.
NC supplies document access and UI, not a second cycle generator.

## Remaining NC integration

- [ ] UI review before the next big step (`docs/nc-ui-review.md`). The review's
  organising finding is that most defects are one logical mismatch: a key or an
  entry carrying two meanings (the Up/Down case - `B`/`C`/`D` are footer actions
  while browsing and sign/point/accept while editing; HOLD is a state drawn as
  an action; toggles were highlighted like actions; the cursor is both browsing
  position and armed run line). Fix as one model - explicit kind per entry
  (action/toggle/mode), one meaning per key per context, state shown for state
  entries - then the instances (HOLD, RUN versus SINGLE, action highlight).
  Second P1: apply-the-draft-on-accept in the editor.
- [ ] **Global editor note - steal the editor flow from Heidenhain TNC 415.**
  Field-by-field entry instead of prefilled lines: start a function, the control
  prompts the first required field (letter + description, no need to type the
  letter), Enter accepts and advances, Enter on an empty field skips it, the
  function stays modal until committed. Includes the word validation reported
  from the Windows editor: a `G` word may only be edited to a supported code
  (the cycle family switches inside its own set, e.g. `G71`/`G72`), always
  positive and integer, and unsigned words must reject a sign. Also: Up/Down on
  a value adjusts and flips its sign so the pad's `-` key is unnecessary, End
  and Del get a real job, `.` is used or removed, and the 3x3 becomes a two or
  three level menu. Letter keys (`G`, `X`, `Y`, `Z`, `N`, `Q`, `U`, `R`, `F`)
  are explicitly not planned until the field flow exists - they would only add
  noise. Full spec: `docs/nc-editor-tnc415.md`.
- [x] Desktop sender (`tools/nc_sender`): the NC document, emitter and G7x
  generators compile host-side and stream expanded programs to Grbl or uCNC,
  with a Grbl 1.1 protocol client, Win32 COM transport and host tests. The
  Win32 GUI shell (editor + preview) is the remaining step; see
  `docs/desktop-sender.md`.
- [x] Windows panel shell (`tools/nc_ui_win`): the real NC screen renders on the
  host through a GDI LVDS backend, with the machine key row (F1-F6 modes,
  F7-F12 soft keys, 3x3 pad) beside the emulated 800x600 panel. `nc_visual` grew
  `nc_visual_select_mode()` for direct mode keys. RUN still drives the virtual
  parser; a Grbl transport for real hardware is the next step.
- [x] Supply a document-source adapter for G7x numbered-block lookup
  (`nc_emit_numbered_source`) and preview both one-line and Fanuc two-line
  `G71/G72 ... P Q` ranges from the document, with missing/ambiguous range
  errors surfaced as preview errors.
- [x] Share one G7x block scan between RUN, the preview and the editor
  (`nc_g7x.c`): block start/end for numbered ranges and G80 cycles, two-line
  headers, and which rows count as contour. RUN sends a whole block for a
  selected line, including either header line of a two-line pair.
- [ ] Add G76 preview through the shared G7x threading generator.
- [ ] Keep nc_emit as preview glue; assess removal only if SIM can consume the
  shared stream directly without losing source-line/error information.
- [ ] Implement the depth knob EDIT/RUN interaction described above; G7x owns
  pass-phase enforcement and generated motion changes.
- [ ] Persist cursor position across reboot; current preservation is in-session.
- [x] Hardware checklist exists in TESTING.md; executing it remains open.
