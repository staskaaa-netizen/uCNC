#ifndef NC_UI_HOST_SPINDLE_H
#define NC_UI_HOST_SPINDLE_H

#include <stdbool.h>
#include <stddef.h>

/* The station's spindle, read from the machine's own signals.

   The machine wires TOOL1 as `spindle_pwm` (uCNC/cnc_hal_config.h): the speed
   leaves on PWM0 and the direction on DOUT0. The desktop has no spindle
   encoder and must not need one, so the station does not ask for the real
   speed - it reads the two signals the tool drives on the emulated MCU (the
   virtual signal the firmware itself would see) and turns the PWM back into
   the speed the wire carries. The screen keeps showing the commanded speed it
   always showed; this is the panel beside the machine saying what the machine
   is actually being told. */
typedef struct {
    unsigned rpm;   /* what the PWM signal carries, in rpm */
    char dir;       /* 0 = off, '3' = CW, '4' = CCW */
} host_spindle_t;

/* Read the two signals. Always fills `out`. */
void host_spindle_read(host_spindle_t *out);

/* "spindle off", "spindle 1000 rpm CW". False when `out` cannot hold it. */
bool host_spindle_text(char *out, size_t size);

#endif
