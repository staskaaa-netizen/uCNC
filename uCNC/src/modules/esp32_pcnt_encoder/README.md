# ESP32 PCNT Encoder

ESP32-only uCNC custom encoder module using the ESP32 PCNT peripheral for spindle
quadrature counting. It is intended for lathe spindle feedback and G33/G33 ELS
threading.

## What It Does

- Uses PCNT unit 0 for encoder A/B quadrature.
- Extends the signed 16-bit PCNT value in software with an offset.
- Recenters the hardware PCNT counter before it reaches the 16-bit edge.
- Uses the physical Z/index pulse only to name the spindle phase/tooth.
- Publishes the normal `enc0_index` hook from a PCNT0 modulo virtual index.
- Uses Grbl/µCNC encoder resolution setting `$150` as the modulo CPR.
- Reports index interval statistics through the normal encoder status helpers.

The important design choice is that PCNT0 A/B remains the spindle position truth.
The GPIO index ISR captures the first physical index edge so the tooth is named,
then the module derives future index events from `PCNT0 % $150`. This keeps G33
and the software ELS follower in the same timing world as the step emission loop.

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

#define ENC0_INDEX_GPIO DIN5_BIT

#define SPINDLE_PWM_RPM_ENCODER ENC0
#define G33_ENCODER ENC0
```

`ENC0_PULSE_GPIO` is encoder A. `ENC0_DIR_GPIO` is encoder B.

`ENC0_CPR` is only a compile-time fallback. During normal operation the module
uses `g_settings.encoders_resolution[ENC0]`, which is Grbl/µCNC setting `$150`.
Set `$150` to the encoder count per revolution used by PCNT0.

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

Keep the A/B filter shorter than the real A/B high or low pulse width at maximum
spindle RPM, or real encoder pulses can be filtered out.

## Index Path

The working index path is:

1. GPIO ISR captures the physical Z/index rising edge and snapshots PCNT0.
2. That snapshot becomes the modulo origin.
3. The task loop watches PCNT0 and emits one virtual index at every `$150` counts.
4. The virtual index fires the normal `enc0_index` hook used by G33/G33 ELS.

The standard encoder module fires index hooks on the logical rising edge. This
module follows that convention with `GPIO_INTR_POSEDGE`.

No second PCNT unit is used for index anymore. The index pin is handled only by
the GPIO ISR, and the G33 hook is fired only from the virtual PCNT0 modulo
crossing.

## Status Helpers

The module implements the normal encoder status helpers:

- `encoder_get_index_stats()`
- `encoder_get_index_live_delta()`
- `encoder_get_index_debug_line()`

The debug line is copied into UI snapshots when requested; it is not printed from
the encoder task loop.

Example debug line:

```text
ENCIDX EC:-5269 ECB:0 LAST:-4000 AVG:4000.0 MIN:-4000 MAX:4000 N:34 IGN:0 ISR:35
```

Fields:

- `EC`: current extended encoder count
- `ECB`: live count since the last virtual index
- `LAST`: count between the last two virtual indexes
- `AVG`: average absolute count between virtual indexes
- `MIN`/`MAX`: observed virtual index interval range
- `N`: accepted virtual index intervals
- `IGN`: reserved for ignored index events
- `ISR`: physical index edges captured by GPIO ISR

## Notes

- PCNT0 A/B is the spindle truth.
- The physical index names phase; it does not drive motion directly.
- Virtual modulo index is the active G33/G33 ELS hook source.
- `$150` must match the PCNT0 counts per spindle revolution.
- `ENC0_PCNT_RECENTER_THRESHOLD` defaults to `20000`, keeping the raw PCNT value
  away from the signed 16-bit boundary.
- GPIO34-GPIO39 on classic ESP32 are input-only and have no internal pullups or
  pulldowns. Use proper external biasing or a driven encoder output.
