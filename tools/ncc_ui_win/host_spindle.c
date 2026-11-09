/* The station's spindle - see host_spindle.h. One owner for "what is the
   spindle being told", so the side panel, the checks and any future sender all
   read the same two signals instead of each inventing a spindle. */
#include "host_spindle.h"

#include <stdio.h>

#include "cnc.h"

/* The boardmap's spindle wiring, the same pins `spindle_pwm` drives: PWM0 the
   speed, DOUT0 the direction. Read through the IO HAL, so what comes back is
   what the firmware set - on the emulated MCU the virtual map, on real
   hardware the pin. */
#ifndef HOST_SPINDLE_PWM
#define HOST_SPINDLE_PWM PWM0
#endif
#ifndef HOST_SPINDLE_DIR
#define HOST_SPINDLE_DIR DOUT0
#endif

void host_spindle_read(host_spindle_t *out)
{
    unsigned max_rpm;
    unsigned pwm;

    if (!out) {
        return;
    }
    out->rpm = 0u;
    out->dir = 0;
    /* The speed leaving the tool is already ranged to the PWM's 0..255 (the
       planner hands `spindle_pwm` the ranged value), so the way back is the
       tool's own range: pwm = 255 * rpm / max. A signal of zero is a stopped
       spindle whatever the direction pin says. */
    pwm = (unsigned)io_get_pwm(HOST_SPINDLE_PWM);
    if (pwm == 0u) {
        return;
    }
    max_rpm = (unsigned)g_settings.spindle_max_rpm;
    out->rpm = (pwm * max_rpm + 127u) / 255u;
    /* The direction pin is set for a speed the tool was given as positive
       (M3, and M4 is the negative one), so its state is the direction. */
    out->dir = io_get_output(HOST_SPINDLE_DIR) ? '3' : '4';
}

bool host_spindle_text(char *out, size_t size)
{
    host_spindle_t spindle;

    if (!out || size == 0u) {
        return false;
    }
    host_spindle_read(&spindle);
    if (spindle.dir == 0) {
        return snprintf(out, size, "spindle off") > 0;
    }
    return snprintf(out, size, "spindle %u rpm %s", spindle.rpm,
                    spindle.dir == '3' ? "CW" : "CCW") > 0;
}
