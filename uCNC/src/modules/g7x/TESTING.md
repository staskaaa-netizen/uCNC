# Test G7x without the NC screen

## Automated

With MinGW GCC on PATH:

```text
python tools/test_g7x.py standalone
python tools/test_g7x.py generator
python tools/test_g7x.py all
```

`standalone` links the actual core parser/planner, virtual MCU, G7/G8 and G7x;
no NC source file is linked or initialized. It tests inline collection,
execution ordering, failure cleanup, G76 word ordering, units, work offsets and
one-line P/Q numbered ranges (start block, unnumbered profile rows, `N(Q)`
terminator, retention and error cleanup), the two-line headers for G71 and G72,
the `P` block as an approach, profile `F`/`S`/`T` rows, `G70 P Q` replay and the
cutter-compensation and `G73` refusals - `G70` works in this build, so it is the
module's own feature and not something the panel adds.
G33 is intercepted to inspect targets/pitch; this does not validate spindle
synchronization. `all` also tests the NC adapter and NC stream integration.

## Serial bench test

Use the existing RP2350-LEANCAM-LVDS firmware through its serial terminal; do
not arm an NC stream concurrently. The UI may remain loaded, but G7x commands
are handled by the core parser without an NC document or screen action.

The separate RP2350-G7X-MODULE build omits NC/display/SD/encoder modules. It is
a compilation target for standalone integration, not a drop-in board profile
for the LVDS machine. It does not configure the real G33 spindle feedback.

1. Check controller status and settings first. The examples contain no reset,
   automatic unlock, homing or spindle-start commands.
2. Send `tests/serial/g71.nc` or `g72.nc` one line at a time, waiting for each
   response. Contour rows only collect; G80 submits generated motion.
3. For these examples, expect the first rough pass to approach with a separate
   `G0 X52.500` and then `G0 Z1.500`; every later pass is already clear in X, so
   it retracts with one `G0 X52.500` and - for `G71`, which cuts from the face
   end again - returns with one `G0 Z1.500`, while `G72` rapids straight to the
   next face depth. No rapid is emitted that would not move the tool, the
   automatic finish approaches from the clear point only as far as it has to, and
   the cycle **ends back at the clear point**: `G0 X52.500`, then `G0 Z1.500` -
   the corner a `G0` before the cycle established. The last roughing pass is the
   allowance boundary itself (`X30.500` for this profile, `Z-9.500` for
   `g72.nc`), not a whole depth above it. Both allowances and R are nonzero so
   missing allowance handling is visible.
4. Check hold/resume and Stop/reset while motion is queued. A successful G80
   acknowledgement means blocks were submitted, not that physical motion ended.
5. For a rejection test, change a contour row to reverse both axes; G80 should
   return an error and clear collection. Send an explicit G0 afterwards to
   verify that no stale contour captures it. Do not send the whole invalid
   test blindly: raw serial senders must stop on errors themselves.
6. Numbered range: send `G71 U1 R1 P100 Q200 X0.5 Z0.5 F120` followed by
   `N100 G0 X50 Z0`, an unnumbered contour row and `N200 X40`. The cycle must
   run when the `N200` block is accepted, without a `G80`. Then repeat with a
   `G80` inside the range and with a row numbered beyond `Q`; both must return
   an error and leave no pending cycle.
7. Fanuc two-line header: send `G71 U1 R1`, then `G71 P100 Q200 U0.5 W0.25 F120`
   and the same profile. The cycle must match the one-line spelling above with
   the X clearance at 52.5 and the Z clearance at 1.25. Check that a `G71` block
   sent after the first contour row is still rejected instead of merged.
8. `g76.nc` requires the configured G33 encoder/spindle setup. Expected cut
   diameters are 38, 36.5, 36, then a spring pass at 36; lead is 1.5 mm/rev.
   Check actual phase/pitch separately before treating threading as validated.

## Approach contract and limits

For the existing positive-allowance/outside-X subset, the generator uses:

- X clear = maximum contour X + X allowance + R, in internal stream units.
- Z clear = start Z minus cutting direction times (Z allowance + R).
- Move outward in X first, then to Z clear; feed into the cutting pass. That
  full approach belongs to the first pass of the cycle: the tool's position
  before it is not known, so those two rapids are always emitted.
- Later passes are already out at X clear. `G71` returns in Z to clear (its
  passes cut from the face end again); `G72` moves straight to the next face
  depth in Z, because a return to the start Z would only double the rapid.
- Automatic finishing approaches from the clear point as far as it needs to -
  nothing when the roughing already left the tool there - and the contour itself
  remains unoffset for finishing.
- The roughing's last pass is the allowance boundary: a full depth of cut that
  would cross it is shortened to land on it, so the automatic finish only takes
  what the allowance reserved.
- The cycle ends at the clear point - out in X, then back in Z. That is the
  corner a `G0` before the cycle establishes and where Fanuc leaves the tool; the
  generator never reads the operator's own pre-cycle position.
- A motion that would not move the tool is never emitted at all.

G7 input/output converts X between diameter and internal radius. Consequently
the example X50 + diameter allowance 0.5 + radial clearance R1 gives X52.5.
The operator must establish a clear outward-X path from the starting position.
This is not collision detection or automatic stock/fixture avoidance. Negative
allowance directions and the 45-degree retract remain separate TODOs.

## P/Q ranges, the `P` block and `G70`

Automated in `tools/test_g7x.py all` (generator, parser and NC preview targets);
these are the machine checks that go with them:

- **The `P` block may be the approach.** Write the range's first block as
  `N100 G0 X.. Z..` (Fanuc's spelling, off the stock) and check the cycle
  *rapids* to it before the first cut, instead of feeding in. A program that
  writes the first block as a cut (`N100 G1 X.. Z..`) still works the way it
  did: the block is the profile's start point either way.
- **`P`/`Q` name the rows' `N` numbers, not line numbers.** A program whose rows
  carry no `N` has no range to collect: the rows run as ordinary moves and the
  run ends complaining about an incomplete cycle (the panel's own message). At
  least two rows, numbers rising, `N(P)` first and `N(Q)` last, and no `G80`
  inside the range:

  ```text
  G71 U1 R1 X0.2 Z0.1 F250 P100 Q130
  N100 G1 X20 Z0
  N110 G1 X20 Z-20
  N130 G1 X50 Z-20
  ```
- **A rapid inside the profile is not a rapid on the machine.** Put a `G0` row
  in the middle of a range and check the finish cut *feeds* through it; the
  cycle must never put a rapid through material.
- **Profile `F` is the finish feed.** Give one row its own `F` and check the
  finish cut changes feed at that row (and that the roughing kept the header's
  feed). Profile `S`/`T` are accepted and not acted on - the spindle speed and
  tool must not change in the middle of a cut.
- **`G70 P Q` finishes the range this run collected.** Run `G71 P Q` and then
  `G70 P Q`: the second block must follow the profile, at the profile's feed,
  with no roughing and nothing offset. Starting RUN *at* the `G70` line must
  refuse it (the range never streamed past), not go looking for it.
- **A cycle with cutter compensation active is refused.** `G41` then
  `G71/G72/G70` must stop with "Unsupported command" and no motion; `G40` then
  the same cycle must run. The console says which refusal it was
  (`G41/G42 not supported in a cycle`).
- **`G73` is refused by name.** `G73 U.. W.. R.. P.. Q..` must stop on the line;
  the console says pattern roughing is not implemented.
