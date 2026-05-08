# ESP32 PCNT Encoder

ESP32-only uCNC custom encoder module using the ESP32 PCNT peripheral for spindle
quadrature counting. It is intended for lathe spindle feedback and G33 threading.

## What It Does

- Uses one PCNT unit for encoder A/B quadrature.
- Extends the signed 16-bit PCNT value in software with an offset.
- Recenters the hardware PCNT counter before it reaches the 16-bit edge.
- Uses an optional second PCNT unit as a hardware mailbox for the Z/index pulse.
- Reports index interval statistics through the normal encoder status helpers.
- Can publish a virtual `enc0_index` hook from the PCNT index mailbox for G33.

With the index PCNT enabled, the Z pulse does not need the normal uCNC DIN ISR
path. The Z pin is counted by PCNT hardware and drained from the module task
loop. This avoids the ESP32 GPIO ISR path for the index pulse while still giving
G33 the same hook it already expects.

## Basic Configuration

Example for encoder 0:

```c
#define ENC0_TYPE ENC_TYPE_CUSTOM

#define ENC0_PULSE_GPIO 15
#define ENC0_DIR_GPIO 17
#define ENC0_PCNT_UNIT PCNT_UNIT_0

#define ENC0_IS_INCREMENTAL
#define ENC0_READ_WRAP 65536UL
#define ENC0_CPR 4000

#define SPINDLE_PWM_RPM_ENCODER ENC0
#define G33_ENCODER ENC0
```

`ENC0_PULSE_GPIO` is encoder A. `ENC0_DIR_GPIO` is encoder B.

## Filters

`ENCODER_PCNT_FILTER` is in PCNT APB clock ticks, not microseconds. On classic
ESP32 the APB clock is normally 80 MHz:

```text
80 ticks ~= 1 us
```

Example:

```c
#define ENCODER_PCNT_FILTER 160  // about 2 us
```

Keep this shorter than the real A/B high or low pulse width at maximum spindle
RPM, or real encoder pulses can be filtered out.

## Index Without GPIO ISR

The index pulse can be captured by a separate PCNT unit:

```c
#define ENC0_INDEX_GPIO DIN5_BIT
#define ENC0_INDEX_PCNT_UNIT PCNT_UNIT_1
#define ENC0_INDEX_PCNT_FILTER 10
```

This config makes PCNT count rising Z/index edges. The index pulse is not gated
by encoder A/B, so it behaves symmetrically in both rotation directions.

The task loop reads and clears only the index PCNT counter. It does not reset the
main encoder counter.

Do not also enable the normal DIN ISR for the same index input when using index
PCNT. For example, if the index is on `DIN5`, leave `DIN5_ISR` undefined. The
module will invoke the virtual `enc0_index` hook after the PCNT mailbox reports a
Z pulse.

If `ENC0_INDEX_PCNT_UNIT` is not defined, the module falls back to the normal
uCNC `enc0_index` hook. With `DIN5_ISR` enabled on ESP32, that hook is driven by
the GPIO interrupt path, not by slow main-loop level polling. The same min/max
cap and UI debug line are still used. This fallback was useful for comparison,
but the PCNT index path is the preferred ESP32 solution.

By default, accepted index-to-index intervals are checked against the configured
encoder resolution from GRBL setting `$150` for encoder 0. The default window is:

```text
min = $150 * 0.75
max = $150 * 1.25
```

For `$150=4000`, this accepts `3000..5000`.

The window can still be overridden at compile time:

```c
#define ENC0_INDEX_MIN_DELTA 3000
#define ENC0_INDEX_MAX_DELTA 5000
```

The module keeps the encoder position at the last accepted index and calculates:

- live delta since the last accepted index
- last accepted revolution count
- min/max accepted revolution count
- accepted index count

If the `ui_snapshot` integration from this modules branch is used, the debug line
is copied into the snapshot and rendered by `ra8876_display` only when it changes.
It is not printed directly from the encoder task loop.

Example debug line:

```text
ENCIDX EC:-5269 ECB:0 LAST:-4001 AVG:4000.1 MIN:-4002 MAX:4003 N:34 HW:35 IGN:0 BAD:0 MISS:0
```

Fields:

- `EC`: current extended encoder count
- `ECB`: live count since the last accepted index
- `LAST`: count between the last two accepted indexes
- `AVG`: average absolute count between accepted indexes
- `MIN`/`MAX`: accepted interval range
- `N`: accepted index intervals
- `HW`: raw index pulses counted by the index PCNT unit
- `IGN`: ignored too-small deltas, usually same-slot recrossing near reverse
- `BAD`: reserved for impossible intervals that are not classified as missed
- `MISS`: extra or over-cap Z interval, usually software did not process an index
  interval in time

## G33 Compatibility

G33 listens to the normal uCNC encoder index hook. When index PCNT is enabled,
this module invokes that same hook from the PCNT index mailbox. So G33 does not
need a special ISR-capable DIN pin for this ESP32 PCNT solution.

For repeat G33 commands, the bundled `g33` module clears its internal index
timestamps/counters at the start of each G33 move. This avoids stale index timing
from a previous thread move being reused by the next command.

If index PCNT is not configured, the module falls back to the normal uCNC
`enc0_index` hook path. In that fallback mode the selected index input still
needs to work through the normal uCNC DIN polling/ISR mechanism.

## Experimental Index Hunt

This module also contains a diagnostic-only section marked:

```c
// ============================================================
// EXPERIMENTAL INDEX POSITION HUNT
// ============================================================
```

It is a playground for comparing index-position methods. It does not change G33
or motion behavior. The normal PCNT encoder count remains the spindle truth.

Modes:

```c
#define ENC0_INDEX_MODE_LEGACY_ISR 0
#define ENC0_INDEX_MODE_PCNT_INDEX 1
#define ENC0_INDEX_MODE_RMT_MARKER 2
#define ENC0_INDEX_MODE_DFF_LATCH 3
#define ENC0_INDEX_MODE_SOFTWARE_POLL 4
#define ENC0_INDEX_MODE_GPIO_ISR 5
#define ENC0_INDEX_MODE_VIRTUAL_MOD 6
```

`ENC0_INDEX_MODE` defaults to `ENC0_INDEX_MODE_LEGACY_ISR`. For the current
RMT test setup, use:

```c
#define ENC0_INDEX_MODE ENC0_INDEX_MODE_RMT_MARKER
#define ENC0_INDEX_HUNT_DEBUG
#define ENC0_RMT_MARKER_SINGLE_PULSE 1
```

In single-pulse mode, RMT watches one Z/index pulse per revolution and compares
accepted RMT markers against the existing PCNT index mailbox reference. Debug
fields include:

- `md`: marker-to-marker encoder count delta
- `cmp`: RMT marker position minus the last PCNT index position
- `cmin`/`cmax`: observed compare range
- `cn`: accepted compare count
- `near`/`far`: rejected too-close or outside-window marker candidates

Optional A/B/Z capture can also be enabled:

```c
#define ENC0_RMT_ABZ_CAPTURE 1
```

Default RMT channels are:

```c
#define ENC0_INDEX_RMT_CHANNEL   RMT_CHANNEL_0  // Z/index
#define ENC0_INDEX_RMT_A_CHANNEL RMT_CHANNEL_1  // encoder A
#define ENC0_INDEX_RMT_B_CHANNEL RMT_CHANNEL_2  // encoder B
```

The ABZ debug stream reports packet, symbol, edge, duration, timestamp, and PCNT
snapshots for A and B. This is intended to inspect signal timing and compare RMT
capture behavior against the existing PCNT path. It is not yet a motion sync
source.

A fifth diagnostic path can poll the normal `ENC0_INDEX` input directly from
`cnc_io_dotasks`:

```c
#define ENC0_INDEX_SOFT_POLL_ENABLE 1
```

This can run alongside RMT marker testing. It reports:

```text
[IDXHUNT SOFT] pcnt=... edges=... near=... far=... md=... cmp=... cmin=... cmax=... cn=... crj=...
```

This is intentionally the simplest possible baseline: no hardware capture, just
software edge polling in the module task loop. It is useful for comparing loop
latency against the PCNT index mailbox and RMT marker diagnostics.

An additional GPIO ISR diagnostic can snapshot the main PCNT count from a direct
ESP32 GPIO ISR:

```c
#define ENC0_INDEX_ISR_HUNT_ENABLE 1
```

It reports:

```text
[IDXHUNT ISR] pcnt=... raw=... edges=... pend=... ovf=... lvl=... us=... near=... far=... md=... cmp=... cmin=... cmax=... cn=... crj=... ring=...
```

This is still experimental. The ISR only captures a small mailbox: raw event
count, pin level, timestamp, and the main PCNT count as close to the GPIO edge as
possible. The normal task loop performs filtering and debug output. The mailbox
is a small ring so the diagnostic can classify multiple ISR edges between task
passes; `ovf` reports if that ring overflows.

For comparison, the PCNT index mailbox can be configured to count both edges:

```c
pos_mode = PCNT_COUNT_INC
neg_mode = PCNT_COUNT_INC
```

That confirms whether the mailbox sees one or both edges of the Z pulse, but it
still does not timestamp the first edge. The mailbox is drained later from the
module task loop, so a consistent one-to-two-edge region in the debug output is
expected when the spindle moves during that drain latency.

The current test findings are:

- PCNT unit 0 A/B quadrature is the main spindle position source and remains the
  count truth.
- The physical index should name the first/main pulse for phase, not drive motion
  by itself.
- PCNT unit 1 is a good hardware mailbox for "an index edge happened", but its
  reported position is the PCNT0 value when the mailbox is drained later.
- Counting both index edges on PCNT unit 1 confirms the pulse edges, but it still
  cannot tell where the first edge was. In hand tests this showed a visible
  one-to-two-edge/pulse region caused by drain latency.
- Direct GPIO ISR capture is much better than software polling. In tests it saw
  the index events reliably, rejected the opposite edge as `near`, and kept
  compare values mostly within tens of encoder counts after startup.
- Software polling from the task loop is only a baseline. It missed most short
  index pulses unless rotation was very slow.
- RMT Z single-pulse capture worked as an index detector, but the legacy RMT
  ring buffer path still reports through task drain. Separate A/B/Z RMT streams
  are useful to inspect signal activity, but they are not yet a unified absolute
  timestamp reconstruction.

One more diagnostic can derive a virtual index from PCNT unit 0 modulo the
configured encoder CPR:

```c
#define ENC0_INDEX_VIRTUAL_MOD_ENABLE 1
#define ENC0_INDEX_VIRTUAL_FIRE_HOOK 0
```

The virtual index locks its origin to the first accepted physical reference
(PCNT index first, ISR reference second) and then reports every `$150`/`ENC0_CPR`
counts from the main A/B counter. It can optionally fire the normal `enc0_index`
hook, but this is disabled by default because it would affect G33 behavior.

Debug line:

```text
[IDXHUNT VIRT] pcnt=... origin=... ev=... boundary=... md=... pcntcmp=... pcmin=... pcmax=... pcn=... pcrj=... isrcmp=... isrmin=... isrmax=... isrn=... isrrj=... hook=... fire=...
```

This answers a different question from the physical index tests: "If the main
PCNT0 quadrature count is truth, how stable is a modulo-derived index once its
phase is named by the real index pulse?"

## Notes

- ESP32 PCNT counters are still limited internally. The extended position is
  software-maintained by this module.
- `ENC0_PCNT_RECENTER_THRESHOLD` defaults to `20000`, keeping the raw PCNT value
  away from the signed 16-bit boundary.
- `ENCODER_PCNT_FILTER` and `ENC0_INDEX_PCNT_FILTER` are PCNT filter ticks, not
  microseconds. On classic ESP32, `80` ticks is about `1 us`.
- GPIO34-GPIO39 on classic ESP32 are input-only and have no internal pullups or
  pulldowns. Use proper external biasing or a driven encoder output.
