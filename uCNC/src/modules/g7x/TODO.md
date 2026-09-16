# G7x status and remaining work

Audited against source on 2026-09-16. Checked means implemented and
software-tested, not physically validated on a machine.

## Ownership

- G7x: contour storage/validation, cycle semantics, allowances, approach/retract,
  pass generation, retained-contour replay and depth-override application.
- Core parser: parse/route commands and execute generated blocks.
- NC: files/editor, program-source adapter, preview and operator controls.
- G33/encoder: spindle-synchronized motion. G76 requests this through G33.

## Implemented foundation

- [x] Standalone module; RUN does not depend on NC screen code.
- [x] Native inline G71/G72 ... G80 collection with bounded contour storage.
- [x] Shared stepped generator for execution and NC preview.
- [x] Generated blocks submitted through parser, with ordering/error cleanup.
- [x] Conservative X/Z monotonic validation, including arc interiors.
- [x] Existing corner expansion and automatic inline finishing pass.
- [x] Single-line G76: X/Z/P/Q/F and native R finish, I taper, L spring passes.
- [x] Radial first cut, decreasing cuts, minimum Q/4 in native G76, units and
  work offsets. Library source adapter additionally supports MIN_Q/QMIN aliases.
- [x] Constant-lead G33 output and error propagation from generated commands.

## Required cycle work still open

- [ ] Simple explicit approach with BOTH X and Z finish allowances plus
  clearance. Current roughing endpoints use allowances, but the approach does
  not implement this complete rule: G71 starts at start_z and pass+retract;
  G72 starts at max_x+retract and the current Z pass.
  Keep the approach separate from profile cutting. No inferred stock shape,
  automatic entry selection or avoidance planning; reject unsupported cases.
- [ ] P/Q numbered-block lookup through a source interface independent of NC.
  NC supplies document access; define bounded serial-history behavior and
  errors for unavailable/ambiguous ranges. Existing inline collection is not
  retained P/Q lookup or replay storage.
- [ ] Fanuc/Haas one-line and two-line G71/G72 headers and their word meanings.
- [ ] First P block as approach only; profile F/S/T ignored for roughing.
- [ ] Finish stock U/W, direction from allowance signs and 45-degree retract.
  Native inline X/Z allowances and U/W depth words are a different contract;
  negative inline allowances currently fail validation.
- [ ] G70 P/Q replay with current finish feed/tool/spindle and no rough offsets.
  Automatic inline finishing is not this feature.
- [ ] Retain/regression-test inline G80 mode as P/Q support is added.
- [ ] Depth-per-pass override: explicit engaged/return phases, decrease-only
  during engagement and deferred increase for next pass, including queued
  motion and remaining-stock handling. NC owns knob/UI input (see NC TODO).
- [ ] Physical G76/G33 spindle phase/pitch and spindle-loss validation.

## Optional later G76 dialect work

- [ ] Expose configurable minimum cut in the native parser if required.
- [ ] Thousandths-style P/Q convention and packed two-line Fanuc dialect.
- [ ] Variable/special lead helpers only after a concrete requirement.

## Tests

- [x] Basic native G71/G72, non-monotonic rejection and corner-radius coverage.
- [x] Native G80 sequencing, generated failure/cleanup, units and work offsets.
- [x] G76 invalid pitch/depth, decreasing schedule, taper/ID library cases,
  native spring passes and generated G33 failure propagation.
- [ ] P/Q G71 one-/two-line, G72 two-line and serial range regressions.
- [ ] Both-axis approach clearance, allowance signs and 45-degree retract.
- [ ] G70 replay and complete corner-modifier/direction matrix.
- [ ] Machine validation; virtual tests intercept G33 and do not test timing.

Run software suites with `python tools/test_g7x.py all`. Native supported
syntax and limitations are in [README.md](README.md).
