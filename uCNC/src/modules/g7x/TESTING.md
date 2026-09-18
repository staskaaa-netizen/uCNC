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
execution ordering, failure cleanup, G76 word ordering, units and work offsets.
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
3. For these examples, expect separate `G0 X52.500` and `G0 Z1.500` approach
   blocks before feeding into rough passes and before the automatic finish.
   The final retract returns to these same clear coordinates. Both allowances
   and R are nonzero so missing allowance handling is visible.
4. Check hold/resume and Stop/reset while motion is queued. A successful G80
   acknowledgement means blocks were submitted, not that physical motion ended.
5. For a rejection test, change a contour row to reverse both axes; G80 should
   return an error and clear collection. Send an explicit G0 afterwards to
   verify that no stale contour captures it. Do not send the whole invalid
   test blindly: raw serial senders must stop on errors themselves.
6. `g76.nc` requires the configured G33 encoder/spindle setup. Expected cut
   diameters are 38, 36.5, 36, then a spring pass at 36; lead is 1.5 mm/rev.
   Check actual phase/pitch separately before treating threading as validated.

## Approach contract and limits

For the existing positive-allowance/outside-X subset, the generator uses:

- X clear = maximum contour X + X allowance + R, in internal stream units.
- Z clear = start Z minus cutting direction times (Z allowance + R).
- Move outward in X first, then to Z clear; feed into the cutting pass.
- Automatic finishing also approaches from this clear point; the contour
  itself remains unoffset for finishing.

G7 input/output converts X between diameter and internal radius. Consequently
the example X50 + diameter allowance 0.5 + radial clearance R1 gives X52.5.
The operator must establish a clear outward-X path from the starting position.
This is not collision detection or automatic stock/fixture avoidance. Negative
allowance directions, P/Q, G70 and 45-degree retract remain separate TODOs.
