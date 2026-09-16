# NC TODO

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
- [ ] Add G76 preview, G70 finishing replay and P/Q contour extraction.

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
  - Add a small test checklist for EDIT/SIM/RUN: mode switch, file persistence, G970 stock fit, G71/G72 contour labels, live stock removal, run cursor, and reboot recovery.
  - Keep preview-only line labels and UI helpers in NC, but keep all G71/G72 generator/roughing/finish logic in `g7x`.
- G7x parser/runtime promotion:
  - `g7x` is the owner and must work without the NC module loaded.
  - NC may use G7x for preview/SIM and file streaming glue, but must not grow a second roughing/finish generator.
  - Treat `G71/G72` as a modal parser-owned region, not as an NC UI feature.
  - `G71/G72` opens the collector; parsed contour `G0/G1/G2/G3` rows are stored and suppressed; `G80` closes/prepares the region.
  - Reuse the same `g7x_stream_t` contour/generator for parser RUN and NC SIM/preview.
  - Generated rough/finish rows now execute as parsed blocks through the parser generated-block helper, like canned cycles.
  - Next: machine-test bad contour, generated block failure, and modal cleanup paths; improve status mapping if needed.
  - After that: remove or shrink `nc_emit` down to a preview-only adapter, then delete it if SIM can consume the shared G7x stream directly.
- G7x lathe-cycle completion task:
  - Final target: self-contained `g7x` interpreter module for `G71/G72` Type I roughing, `G70` finish replay, and `G76` threading. Main parser routes blocks; G7x collects/interprets contour or thread parameters; generated output returns as ordinary motion/threading blocks.
  - Add Fanuc/Haas-style P/Q contour support for `G71/G72`: one-line and two-line headers, P first N-block, Q last N-block, first P block as approach only, profile `F/S/T` ignored, Type I monotonic validation, finish stock `U/W`, 45-degree retract, and direction from allowance signs.
  - Preserve current native inline `G71/G72 ... G80` mode as a serial-friendly fallback/regression path while adding P/Q extraction from loaded program or serial history.
  - Add `G70 P.. Q..` finish cycle that replays the stored/extracted contour using current finishing feed/tool/spindle, with no roughing offsets.
  - Keep current `G76` source semantics Fanuc-like by letters: `X/Z` end point, `P` thread height, `Q` first cut, `F` lead/pitch, optional `MIN_Q/QMIN`, optional finish allowance, optional taper, and optional spring passes. Two-line Fanuc packed `P(m)(r)(a)` can be added later only if it is a real parser feature, not unused scaffolding.
  - Remaining G76 semantic gaps: hardware validation of spindle/threading availability, optional thousandths-style P/Q dialect and optional packed two-line dialect. Native P/Q use active length units; first-cut/min-cut/decreasing infeed is implemented.
  - G76 pitch/lead note: current code assumes one constant pitch and emits `G33 ... Kpitch`; final parser must leave room for special pitch/lead handling and related helpers later, but do not add those helpers until the base letter semantics are settled.
  - Tests to add from the attached task: G71 two-line, G71 one-line, G71 allowance direction signs, G71 non-monotonic rejection, G71 corner modifiers, G70 finish replay, G72 facing two-line, G76 invalid pitch/depth, serial P/Q regression, and native inline `G80` regression.
