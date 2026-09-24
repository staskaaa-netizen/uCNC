#ifndef NC_MANUAL_H
#define NC_MANUAL_H

/* MANUAL: the readout the jog keys work on, with a minus and a plus stop per
   axis that the operator types on the pad (`*` opens the minus stop, `*` again
   takes it and opens the plus one, `*` once more takes that; `D` puts the axis
   limit the setup states in the field, `A` leaves it).

   This screen is a machine panel, not a text editor: each function keeps its
   own key (`0` zero, `D` touch, `*` stop, `#` step/feed) and a value is only
   typed inside the function whose key opened the field. The field-by-field walk
   of `D` belongs to the EDIT screen (`docs/nc-editor-tnc415.md`); it must not be
   brought here, where the operator is watching the machine rather than reading
   a program.

   The screen is split current versus wanted: the header DRO carries the *current*
   state (work position, machine figures, F/S) on this screen as on every other,
   and the pane below carries what the operator is *setting up* - the axis lines
   with their two stops, the STEP/FEED values the `1`/`3` keys change, and the jog
   pad. Nothing current is repeated in the pane, and no distance-to-go is shown
   beside a stop (the DRO has the position): the row's label says so - `X LIMIT`,
   `Z LIMIT` - because the numbers on it are the limits, not a position. Numbers
   are right-aligned within their column and centred in their row, so the stops
   and the STEP/FEED values share a right edge, and the row's label is written in
   the same font those values use - the pane is one table, and a label set larger
   than its own numbers reads as a different field. The line the keys act on is
   marked across the values it owns, not the whole pane. The spindle keys start
   at the speed the machine
   has - the modal `S`, remembered in the state store - rather than at a fixed
   default. A stop that was never typed reads as the **axis limit the setup states**
   (drawn dimmer), and that is what stops a move until the operator narrows it -
   so an axis always has a limit. A value being typed is drawn in the cell it
   belongs to - the stop cell for a limit, and the touch-off's own column between
   the label and the first stop for a value `D` types - in the editor's word
   colours, so there is no separate field line.

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
   (the header shows the same label), so there is one cache of it, not two. The
   pane shows the values the operator is setting up, so it needs no machine
   figures at all - those are the header DRO's, and they reach the pane nowhere
   else (that is the current/wanted split this screen is built on). */
typedef struct {
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
   because the keys that are not characters on this panel are not: CANCEL and
   MODE leave a field, and the axis keys `B`/`C` (with the shell's arrows) arrive
   as the field-step keys, since the keypad has no character of its own for
   them. */
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

/* True while a stop value is being typed: the pad types into the field, and
   nothing else sees those keys until the field is taken or left. */
bool nc_manual_stop_field_active(void);

#ifdef __cplusplus
}
#endif

#endif
