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

- [x] **The roughing must leave exactly the finish allowance.** The pass level
  steps by `doc` from the stock end (`max_x - doc` for G71, `start_z + dir*doc`
  for G72) and stops as soon as the *next* step would cross the allowance
  boundary `final_pass` (`min_x + x_allow`, or the Z equivalent), so the last
  rough pass can sit a whole step above it and the automatic finish then has to
  take that step: measured on the bench with `G71 U2 R1 X0.5 Z0.5` over an
  X50 to X30 (diameter) profile, the roughing stopped at X34 while the boundary
  is X30.5, so it left 1.75 mm of radius more than the allowance asks for -
  metal the automatic finish has to take in its own cut (`G72 W2` leaves 1.5 mm
  of Z where `Z0.5` was asked for, the same shape one axis over). A finishing
  allowance is only an allowance if the roughing stops *on* it. **Done:**
  `g7x_rough_step()` is now the one place a pass level moves - it steps onto
  `final_pass` when a full step would cross it, and only from the near side, so
  the boundary pass is emitted once. Both cycles use it for their first pass and
  for every step after it, so the roughing ends on the boundary: the profile
  above emits `(G71 rough X15.250)` / `G1 X30.500 F120.000` and
  `(G72 rough Z-9.500)` as its last roughing passes. `test_allowance_approach()`
  pins both, in both Z directions.
- [ ] **The roughing passes must follow the profile - the leftover has to be the
  X and Z the block was given, not "less than the pass depth".** The pass level
  fix above only puts the *level* on the allowance; the pass itself is still a X
  plunge followed by one Z feed to a single hit point, i.e. a staircase parallel
  to Z. So between two levels the material left is up to a whole pass depth more
  than the allowance asks for, and the automatic finish takes it. Measured off
  the demo card's own expansion (`G971 X50 Z50`, `G71 U3 R1 X1 Z1 ... P50 Q55`
  over X30..X52): passes at X46/40/34/31 stopping at Z-20.471/-16.941/-14.000/
  -12.500, so at Z-20 the boundary is still X46 while the profile there is
  X43.5 and the allowance asks X44.5 - 1.5 mm of diameter the `X1 Z1` never
  asked for. The bench: *"g7x expandned paths are wrong in this way, they should
  stop not then [the] leftover is < [the] U - passdepth, but [when the] leftovers
  is [the] same as asked in x and z inputs ... this one is a regression. it was
  fixed before."*

  **The reference is in the tree**: `remap.py` (the LinuxCNC-style G71 remap this
  module was ported from) does not stair-step at all. It **offsets the collected
  contour** - lines shifted along their normal, arcs by moving the centre and
  adjusting the radius, with each join trimmed to the neighbours' intersection
  (`Find_intersect`) - and then sweeps a tool segment in X by the depth `D`,
  taking the **intersections with that offset contour at each level** as the
  pass. Every pass therefore follows the shape, and what is left is the
  allowance the block asked for, all the way along it.

  To port: the C generator has the collected elements already
  (`region.elements`, radii internally), so this is an offsetting pass over them
  (line normal shift, arc centre shift, join trim) and then the per-level
  intersection walk - plus the pass entry/exit rapids and the end-of-cycle
  clearance point that already exist. It changes the emitted expansions, so the
  goldens in `tools/test_g7x.py`, the station's `--emit2test`, the demo's own
  expected lines and this README's "what the generated cycle moves" section all
  move with it. Do not half-land it: the offset must be right for lines, arcs,
  chamfers and radii, and the pass tests written against `remap.py`'s numbers.
- [x] **Where a cycle leaves the tool.** After the wasted-move report (2026-09-21)
  a block ends on the X retract at the last cut's Z, so it does **not** come back
  to the `G0` the program made before the cycle - which is where Fanuc leaves the
  tool after a stock-removal cycle, and what a program that assumes the cycle
  returned to its start point expects. Today the operator follows the block with
  their own `G0`; decide between that (the program says where the tool goes) and
  returning to the cycle's start point (X, or X and Z) at the end. Applies to
  G71 and G72 both - the report was about G72, where the tail is the last facing
  depth rather than the end of the part.
  **Decided and done:** the cycle returns to the **clearance point** - the X
  retract it always had, then the Z return the wasted-move cleanup had dropped -
  for G71 and G72 both. That is the corner a `G0` before the cycle establishes
  (the operator's `G0 X52 Z2`), and where a program written to the Fanuc
  contract expects the tool. The cycle cannot express the operator's exact
  pre-cycle point because it never reads it: its own rapids are absolute,
  computed from the profile. The per-pass trims stay - a `G72` pass still goes
  straight to the next facing depth, and a rapid that would not move the tool is
  still dropped.
- [x] **Stage 1 of the cutter radius compensation interaction (G40/G41/G42).**
  The core parses the group and ignores it for linear/arc motion, so a cycle run
  with compensation active would cut the uncompensated path and call the result
  a finish. The module now refuses `G70/G71/G72/G76` while
  `groups.cutter_radius_compensation != G40` (status "unsupported command", and
  the console says which it was), tested in `g7x_parser_test.c`. Threading was
  already uncompensated and stays so.
- [ ] **Stage 2:** accept a compensated contour for G71/G72 (threading stays
  uncompensated). See `docs/lathe-cutter-comp.md`.
- [x] Simple explicit approach with BOTH X and Z finish allowances plus R in
  the existing positive-allowance/outside-X subset. Separate X then Z clearance
  moves precede roughing/finishing; final retract uses the same clear point.
  No inferred stock shape or automatic avoidance. Starting-path clearance
  remains the caller's responsibility; see TESTING.md. Bench validation open.
  (The moves around that were tightened later, by bench report: the clearance
  approach belongs to the first pass only, a `G72` pass does not return to the
  start Z, an emitted motion always moves the tool, and the cycle ends on the X
  retract instead of going back to the point it started from.)
- [x] One-line `G71/G72 ... P Q` numbered-range lookup. The parser starts the
  range at `N(P)`, treats unnumbered rows inside as contour rows and closes on
  `N(Q)`; NC preview uses the same rule. G7x owns `g7x_source_t` plus a bounded
  serial history, and reports missing/evicted or ambiguous ranges instead of
  guessing. (Two-line headers and `G70` replay from the retained range were the
  following items and are done - see below.)
- [x] Fanuc one-line and two-line G71/G72 headers with Fanuc word meanings: the
  first block's U/W is the depth of cut and R the retract, the second block's
  U/W are the X/Z finish allowances. A second block of the same cycle completes
  the open header only before any contour row is collected. Haas' D-word depth
  variant stays unsupported because this parser shares the D and Q word slot.
- [x] **First `P` block as approach; profile `F`/`S`/`T` read from the row.**
  A `G0` written as the range's first block is Fanuc's positioning move: the
  cycle rapids to that point and the cut starts with the row after it (a rapid
  *later* in the profile is followed as a cut - a cycle never puts a rapid
  through the material). A profile row's `F` is the **finish** feed and is
  emitted where the program wrote it, on the inline finish and on a `G70`
  replay alike; `S`/`T` are taken off the row so it is no longer refused, and
  are not acted on (see the remaining item below).
- [ ] Finish stock U/W, direction from allowance signs and 45-degree retract.
  Native inline X/Z allowances and U/W depth words are a different contract;
  negative inline allowances currently fail validation.
- [ ] Apply a profile row's `S`/`T` to the finish cut. They are accepted and read
  today (the row is not refused any more), and not acted on: the generator emits
  motion with a feed and has no event for a speed or tool change, and a cycle
  must not change either in the middle of a cut. Fanuc reads them for the
  finish, so this is a generator change (an S/T event) rather than a parser one.
- [ ] A `P` block that names only one axis (`N10 G0 X26`, the common Fanuc
  spelling). The range's first block is the profile's *start point*, and G7x
  never guesses the tool's position, so it must name both X and Z today. Filling
  the missing axis needs a position the module does not have.
- [ ] The refusals are named in the console (`G73 pattern roughing is not
  implemented`, `G41/G42 not supported in a cycle`) but the panel shows the
  status it returns - "Unsupported command". A message the operator sees on the
  glass needs a status of its own (core) or a feedback entry (NC).
- [x] **G70 P/Q replay** with the profile's feed, no rough offsets and no
  roughing: the module keeps the last collected numbered range and re-runs it as
  the finish cut (`g7x_stream_begin_finish()`), refusing any other range instead
  of guessing. Automatic inline finishing is not this feature and still runs, so
  a Fanuc `G71 P Q` + `G70 P Q` pair cuts the finish twice - the second pass is
  air, and the README says so. Keeping the range costs one
  `g7x_contour_region_t` of static RAM: the LEANCAM-LVDS build went from
  379,072 to **382,184 bytes** (72.3% to 72.9% of the RP2350's 512 kB) with the
  kept range and the new refusal paths in. It is cleared on parser reset, so a
  new program does not replay the one before it.
- [x] Inline `G80` mode regression-tested as P/Q support was added: every
  `G80`-terminated cycle in `g7x_host_test.c` and `g7x_parser_test.c` still runs
  through the same generator, and the two-line/P-Q tests compare against the
  inline spelling rather than replacing it.
- [ ] Depth-per-pass override: explicit engaged/return phases, decrease-only
  during engagement and deferred increase for next pass, including queued
  motion and remaining-stock handling. NC owns knob/UI input (see NC TODO).
- [ ] Physical G76/G33 spindle phase/pitch and spindle-loss validation.
- [ ] G73 pattern repeating roughing stays out of scope: it is a different
  roughing model from the scanline passes G71/G72 generate, so it is refused by
  name rather than approximated.

## Optional later G76 dialect work

- [ ] Expose configurable minimum cut in the native parser if required.
- [ ] Thousandths-style P/Q convention and packed two-line Fanuc dialect.
- [ ] Variable/special lead helpers only after a concrete requirement.

## Tests

- [x] Basic native G71/G72, non-monotonic rejection and corner-radius coverage.
- [x] Native G80 sequencing, generated failure/cleanup, units and work offsets.
- [x] G76 invalid pitch/depth, decreasing schedule, taper/ID library cases,
  native spring passes and generated G33 failure propagation.
- [x] P/Q G71 one-line parser and preview regressions, including missing,
  ambiguous and out-of-range numbering.
- [x] P/Q G71 two-line, G72 two-line and `G70` replay regressions: the two-line
  form is compared against the one-line spelling for both cycles, `G70` is
  parsed, refused for a range this run did not collect, and replayed with the
  profile's feed (`g7x_parser_test.c`), and the preview expands it by walking
  back to the range (`nc_emit_host_test.c`).
- [x] A cycle refuses to run with cutter compensation active, and `G73` is
  refused by name.
- [x] Both-axis approach/finish/return clearance for G71/G72, both Z directions:
  the first pass approaches through the clear point, the finish starts there as
  far as it needs to, and the cycle ends there again (X retract, then Z) - the
  per-pass returns are trimmed to what each pass needs.
- [x] Roughing stops **on** the finish allowance (`g7x_rough_step()`), and the
  boundary pass is pinned for both cycles and both Z directions.
- [x] Real parser test target linking no NC sources: `test_g7x.py standalone`.
- [ ] Allowance signs and 45-degree retract.
- [x] `G70` replay (host, parser and NC preview targets). (The complete
  corner-modifier/direction matrix is still open - the corner tests cover the
  radius and chamfer fits on the cycles, not every modifier against every
  direction.)
- [ ] Machine validation; virtual tests intercept G33 and do not test timing.

Run software suites with `python tools/test_g7x.py all`. Native supported
syntax and limitations are in [README.md](README.md).
