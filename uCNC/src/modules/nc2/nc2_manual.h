#ifndef NC2_MANUAL_H
#define NC2_MANUAL_H

#include "nc2.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MANUAL: the jog screen. The pad is the machine's jog keys - 2/8 X, 4/6 Z,
   7/9 the spindle, 5 its stop, 1/3 the value the pane shows, `#` swaps a step
   for feeding to the stop, `*` types the two stops of the picked axis, `D`
   touches the axis off, `0` zeroes it, `B`/`C` pick the axis - and the pane
   carries what the operator is setting up: the two stops per axis with the
   axis limit the setup states dimmer behind them, and the STEP or FEED value
   the keys change.

   It is a machine panel, not a text editor: each function keeps its own key and
   a value is only typed inside the function whose key opened the field. The
   jog, the feed and the stops are the same questions as `nc`'s MANUAL asks
   (`nc_manual.c`) and answer the same way, because they are the machine's. */

#define NC2_MANUAL_ADDR 0            /* unused: the pad is the jog keys here */

/* MANUAL's own keys, as the keypad sends them (`0`-`9`, `*`, `#`, `A`-`D`).
   False when the key is not this screen's. */
bool nc2_manual_key(char key);
/* The key that is down right now, or 0: a held direction key feeds until it
   comes up. */
void nc2_manual_hold(char key);
/* Leaving the screen must not leave a jog running behind the next one. */
void nc2_manual_feed_cancel(void);
/* True while a stop value is being typed: the pad types into the field then,
   and nothing else on the screen sees those keys. */
bool nc2_manual_field_active(void);

/* The two stops of the picked axis, for the pane: what stands there and whether
   it is the operator's own value or the axis limit the setup states. False when
   the side has neither (the machine states no travel). */
bool nc2_manual_stop(int axis, int side, float *value, bool *typed);
/* The axis the keys act on (0 X, 1 Z), the value the feed mode uses, and the
   two tables' current figures, for the pane. */
int nc2_manual_axis(void);
bool nc2_manual_continuous(void);
float nc2_manual_step(void);
float nc2_manual_feed(void);
bool nc2_manual_flash(char key);

/* Draw MANUAL's pane: the axis stops and the value the keys change. */
void nc2_manual_draw_pane(int x, int y, int w, int h);
/* The nine jog labels of the pad, in key order 1..9. */
const char *nc2_manual_pad_label(char key);

#ifdef __cplusplus
}
#endif

#endif
