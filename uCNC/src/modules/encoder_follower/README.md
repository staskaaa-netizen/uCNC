# Encoder Follower

ENC1 and ENC2 can follow hand-wheel encoders by submitting their count deltas to
the normal uCNC jog/interpolator path. The module never toggles STEP pins or
owns a second motion loop.

Defaults in this machine:

| Encoder | Axis | Steps per encoder count |
| --- | --- | ---: |
| ENC1 | X | 1 |
| ENC2 | Z | 1 |

Configure with:

```c
#define ENABLE_ENCODER_FOLLOWER
#define ENC1_FOLLOWER_AXIS AXIS_X
#define ENC1_FOLLOWER_STEPS_PER_COUNT 1.0f
#define ENC2_FOLLOWER_AXIS AXIS_Z
#define ENC2_FOLLOWER_STEPS_PER_COUNT 1.0f
```

`g_settings.step_per_mm[axis]` converts requested motor steps into the machine
coordinate offset expected by `mc_incremental_jog()`.

The module samples at a fixed period, combines both axes into one motion when
both change, and issues only that latest encoder delta as a normal incremental
jog. It does not maintain a target, accumulate a backlog, or retry a refused
move. An executing jog reports both `EXEC_JOG` and `EXEC_RUN`; the follower
continues sampling in that state so consecutive deltas can be queued. A program
run without `EXEC_JOG`, alarm, hold, homing, probing and every other forbidden
state reset the encoder baselines, so movement made while motion is forbidden
cannot be replayed later.
