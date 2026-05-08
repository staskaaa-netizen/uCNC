# g33_ELS

`g33_ELS` is an experimental electronic leadscrew implementation for `G33`.

It keeps uCNC's normal parser handling for the command, target coordinates,
offsets, and modal state, but it does not run the thread pass through the
normal feed planner. After the next spindle index pulse, the spindle encoder
count becomes the master position source and the module emits step/dir pulses
directly to follow the commanded pitch.

Do not load this module together with the regular `g33` module. Both implement
the same `G33` command.

## Requirements

- `ENABLE_PARSER_MODULES`
- An encoder configured as `G33_ENCODER`
- A working spindle index hook for that encoder, for example:

```c
#define G33_ENCODER ENC0
#define G33_INDEX_PIN DIN5
```

On ESP32 the `esp32_pcnt_encoder` module can provide this index hook from a
PCNT index counter, without a GPIO ISR.

## Loading

Add the module to your module loader:

```c
LOAD_MODULE(g33_ELS);
```

Example:

```c
#define LOAD_MODULES_OVERRIDE() ({ \
    LOAD_MODULE(esp32_pcnt_encoder); \
    LOAD_MODULE(g33_ELS); \
})
```

## Command

```gcode
G33 Z-10 K1
```

`K` is pitch in machine units per spindle revolution. With millimetres active,
`K1` means 1 mm per spindle revolution.

For example, with:

```text
$102 = 200    ; Z steps/mm
$150 = 4000   ; encoder counts/rev for ENC0
```

`G33 Z-10 K1` produces:

```text
Z steps per rev = 1 * 200 = 200
EC counts per rev = 4000
Ratio = 200 / 4000 = 0.05 Z steps per encoder count
```

So every 20 encoder counts releases one Z step. A 10 mm pass takes 2000 Z
steps, or 40000 encoder counts.

## Behaviour

1. The parser resolves the target just like a normal motion command.
2. The module waits for the next spindle index pulse.
3. The encoder count at that index becomes the thread phase zero.
4. Live encoder delta is converted to desired motion steps.
5. Missing steps are emitted directly as acceleration-limited step/dir pulses.
6. uCNC realtime position is updated as pulses are emitted.
7. Before the target is reached, the follower decelerates and stops at the
   final commanded step position.
8. When the target is reached, G33 exits and restores modal motion to `G1`.

If the spindle stops before the target, motion stops. If the spindle reverses
before the target, the axis moves backward along the same thread path. After the
target is reached, further spindle rotation does not move the axis.

The acceleration and deceleration portions are not pitch-correct cutting
motion. They are only clearance/lead-in and lead-out envelopes around the ELS
follower. Reserve enough room before the real thread start and after the thread
end for the configured Z acceleration, Z max rate, spindle speed, and pitch.

## Step generator logic

`g33_ELS` does not produce a timed feed like `G1`. There is no "move at
F speed" planner segment during the thread pass. Instead, the main G33 loop
keeps comparing spindle position against the number of motion steps already
emitted.

The core idea is:

```text
encoder_delta = current_encoder_count - encoder_count_at_index
wanted_steps = encoder_delta * steps_per_encoder_count
missing_steps = wanted_steps - sent_steps
```

`steps_per_encoder_count` comes from:

```text
steps_per_rev = total_motion_steps / total_spindle_revolutions
steps_per_encoder_count = steps_per_rev / encoder_counts_per_rev
```

Example with `$102 = 200`, `$150 = 4000`, and `G33 Z-10 K1`:

```text
total_motion_steps = 10 mm * 200 steps/mm = 2000
total_spindle_revolutions = 10 mm / 1 mm per rev = 10
steps_per_rev = 2000 / 10 = 200
steps_per_encoder_count = 200 / 4000 = 0.05
```

So 20 encoder counts release one Z step.

The loop behaves like this:

```text
while target is not reached:
    read encoder count
    calculate wanted_steps

    while sent_steps is behind wanted_steps:
        wait until acceleration and direct step-rate caps allow another pulse
        set direction pins
        emit one step pulse
        update uCNC realtime step position
        sent_steps++

    if spindle moved backward:
        wanted_steps becomes smaller
        emit reverse pulses until sent_steps matches wanted_steps
```

The module therefore only emits a pulse when the encoder says the axis should
be farther along the thread. If the spindle is not moving, `wanted_steps` does
not change and no new pulses are emitted.

With ramping enabled, `sent_steps` is still never allowed to run ahead of the
spindle-derived `wanted_steps` during ramp-in and locked following. Near the
end, the module computes:

```text
brake_steps = current_step_rate^2 / (2 * z_accel_steps_per_sec2)
```

When the remaining commanded motion is within that distance, it enters the
lead-out deceleration envelope and stops at the final Z step.

Each emitted step is still a normal step/dir driver pulse:

```text
idle step level -> active pulse -> idle step level
```

With `$2 = 0`, the step pin rests low and each step is a short high pulse. The
machine can be in `EXEC_RUN` while the electrical step pin is low between
pulses; that is expected. Holding the pin active would not create additional
steps because step/dir drivers count edges/pulses, not "time held high".

### Native GPIO vs buffered outputs

The current ELS generator emits pulses directly with:

```c
io_set_steps(active);
mcu_delay_us(G33_ELS_STEP_PULSE_US);
io_set_steps(idle);
```

This works best when step pins are native MCU GPIO pins, where `io_set_steps()`
changes the physical pin immediately.

On buffered or expanded outputs, such as ESP32 I2S step output or shift-register
based outputs, `io_set_steps()` may only update an intermediate software/output
buffer. The physical pin changes later when that backend flushes its buffer. If
the ELS pulse is shorter than that backend update cycle, the active pulse can be
overwritten by the idle state before it ever appears on the physical pin. The
symptom is a step pin that appears to stay at idle even though the ELS loop is
emitting pulses.

For those machines, prefer one of these approaches:

- put the ELS-controlled step pin on native GPIO,
- route ELS pulse generation through uCNC's normal step ISR/backend,
- or increase `G33_ELS_STEP_PULSE_US`, accepting a lower maximum step rate.

The current module is the simple direct-pulse version. It is not yet an
I2S-buffer-aware realtime step backend.

## `$0` / max step rate

This module uses:

```c
g_settings.max_step_rate
```

as a safety cap for direct step pulse emission. In the current tested uCNC
configuration this is reported as `$0` and behaves as kHz. For example:

```text
$0 = 8.000
```

caps direct G33 output at about 8 kHz, or roughly 125 us between emitted step
pulses.

This cap only limits the direct ELS pulse generator. Normal planner moves and
jogs still use uCNC's normal planner/interpolator path.

If your uCNC build or branch documents `$0` differently, confirm the units for
`g_settings.max_step_rate` before using high spindle speeds.

## Options

```c
#define G33_ELS_STEP_PULSE_US 5
```

Sets the direct step pulse width in microseconds.

```c
#define G33_ELS_DIR_SETUP_US 5
```

Sets the delay after changing direction before emitting the next step pulse.
This matters when the spindle is reversed during a pass.

```c
#define G33_ELS_RAMP_ENABLE 1
```

Enables acceleration-limited ELS following. The ramp uses the existing Z
settings: `$122` / `g_settings.acceleration[AXIS_Z]` for acceleration and
`$112` / `g_settings.max_feed_rate[AXIS_Z]` for maximum Z velocity, converted
through `$102` / `g_settings.step_per_mm[AXIS_Z]`.

```c
#define G33_ELS_LOCK_REV_MIN 2
```

Minimum spindle revolutions before debug reports the follower as phase-locked.
The spindle encoder remains the master position source; this only labels the
debug phase once the emitted steps have caught up to the encoder-derived target.

```c
#define G33_ELS_DEBUG
#define G33_ELS_DEBUG_INTERVAL_MS 500
```

Enables serial debug messages:

```text
[MSG:G33ELS start EC:... steps:... spr:... enc:... ratio:...]
[MSG:G33ELS ramp:rate:... err:... brake:... rem:... phase:...]
```

`rate` is the current acceleration-limited step rate, `err` is
encoder-commanded steps minus emitted steps, `brake` is the current calculated
stopping distance in steps, and `rem` is the remaining commanded motion.

## Notes

This module intentionally does not use feed rate `F` for the thread pass. The
spindle encoder is the clock. `K`, target distance, step/mm, and encoder CPR
define the position relationship.

`F` is accepted on a `G33` line for sender/post compatibility, but it is ignored
and is not stored as the next modal `G1` feed. This prevents a command such as
`G33 Z-10 K1 F100` from changing the feed used by the following retract or
return moves.
