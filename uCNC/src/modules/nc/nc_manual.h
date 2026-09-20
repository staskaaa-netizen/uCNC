#ifndef NC_MANUAL_H
#define NC_MANUAL_H

/* MANUAL: the readout the jog keys work on.

   The screen is a source of the NC module, not a module of its own - like
   nc_draw.c and nc_preview.c, it is compiled with NC and cannot be switched on
   by itself. What it owns is what only it uses: the axis the keys act on, the
   step and feed tables, the stop the feed may not cross, the spindle direction
   and the touch field. What it may touch outside itself is handed in:

     - the screen's status line and repaint flag (nc_manual_screen_t),
     - the frame's machine figures and the work offset the readout is cut from
       (nc_manual_view_t),

   and nothing else. It never reads the screen's document, its cursor or its
   other state, and it does not draw anything outside its own pane. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nc_state.h"
#include "nc_visual.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The two things MANUAL writes outside itself. Data, not callbacks: the screen
   keeps owning its status line and its dirty flag. */
typedef struct {
    char *status;
    size_t status_size;
    bool *dirty;
} nc_manual_screen_t;

/* What the pane paints. The work offset is read once per frame by the screen
   (the header shows the same label), so there is one cache of it, not two. */
typedef struct {
    const nc_runtime_state_t *runtime;
    bool have_wco;
    float wco_x;
    float wco_z;
    const char *wco_label;
} nc_manual_view_t;

void nc_manual_draw(const nc_manual_view_t *view);

/* A key on the MANUAL screen: the digits jog, feed, run the spindle and move
   the value the pane shows beside them, `#` swaps step for feed, `*` stops a
   feed and otherwise arms the stop. While the touch field is open the keys type
   into it and nothing else on the screen sees them, which is why this answers
   "taken" for every key in that state.

   `ch` is the character the screen read the key from (nc_visual_key_char()), so
   the keypad's character table stays in one place; `key` is the screen's key
   because CANCEL and MODE - which also leave the touch field - are not
   characters on this panel. */
bool nc_manual_key(nc_visual_key_t key,
                   char ch,
                   const nc_manual_screen_t *screen);

/* The footer entries MANUAL acts on itself (axis, zero, touch). */
bool nc_manual_action(uint8_t action, const nc_manual_screen_t *screen);

/* The key that is down right now, or 0: a held direction key feeds. */
void nc_manual_hold(char key, const nc_manual_screen_t *screen);

/* Leaving the screen must not leave a jog running behind the next one. */
void nc_manual_feed_cancel(const nc_manual_screen_t *screen);

/* What a key means here, for a shell that labels its own keypad from the same
   table the pad is drawn with. NULL when the key means nothing on this screen. */
const char *nc_manual_key_hint(char key);

#ifdef __cplusplus
}
#endif

#endif
