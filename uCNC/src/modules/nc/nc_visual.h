#ifndef NC_VISUAL_H
#define NC_VISUAL_H

#include <stdbool.h>

#include "nc_menu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    NC_VISUAL_KEY_NONE = 0,
    NC_VISUAL_KEY_MODE,
    NC_VISUAL_KEY_DIGIT_0,
    NC_VISUAL_KEY_DIGIT_1,
    NC_VISUAL_KEY_DIGIT_2,
    NC_VISUAL_KEY_DIGIT_3,
    NC_VISUAL_KEY_DIGIT_4,
    NC_VISUAL_KEY_DIGIT_5,
    NC_VISUAL_KEY_DIGIT_6,
    NC_VISUAL_KEY_DIGIT_7,
    NC_VISUAL_KEY_DIGIT_8,
    NC_VISUAL_KEY_DIGIT_9,
    NC_VISUAL_KEY_BACKSPACE,
    NC_VISUAL_KEY_FINISH,
    NC_VISUAL_KEY_CANCEL,
    NC_VISUAL_KEY_PREV,
    NC_VISUAL_KEY_NEXT,
    NC_VISUAL_KEY_ACCEPT,
    /* Arrow-style editing: move between the words (arguments) of one line.
       PREV/NEXT stay on lines, so a keyboard can map Up/Down to lines and
       Left/Right to arguments. */
    NC_VISUAL_KEY_WORD_PREV,
    NC_VISUAL_KEY_WORD_NEXT,
    /* Field-typed navigation: jump to the next/previous word with the same
       letter, like a control that steps between equal fields. Never edits. */
    NC_VISUAL_KEY_FIELD_PREV,
    NC_VISUAL_KEY_FIELD_NEXT,
    /* Dedicated sign and decimal point. These exist so value entry does not
       have to overload the footer letters (UP was sign, DOWN was the point). */
    NC_VISUAL_KEY_MINUS,
    NC_VISUAL_KEY_DOT
} nc_visual_key_t;

void nc_visual_init(void);
/* Jump straight to a mode (desktop shells and future mode keys). Cycles the
   document exactly like the MODE key does. */
void nc_visual_select_mode(nc_mode_t mode);
void nc_visual_handle_key(nc_visual_key_t key);
/* The keypad key that is down right now (the characters
   nc_visual_key_for_char() takes), or 0 when nothing is held. A held MANUAL
   direction key in feed mode feeds until it comes up, so a shell that can hold
   a key reports both edges; a shell that cannot simply reports 0. */
void nc_visual_hold_key(char key);
/* Main-loop hook: flush the remembered state once the screen is idle. */
void nc_visual_idle_tasks(void);
/* Footer (soft key) items of the active mode, same list the panel draws at the
   bottom of the screen. `count` may be NULL. */
const nc_footer_item_t *nc_visual_footer(size_t *count);
/* The machine keypad's key characters, as cam_keyboard.c reports them (digits,
   `*`, `#`, and `A`-`D`), to the key the screen acts on. The panel shell on the
   host presses through the same table, so the bench presses what the machine
   presses. Unknown characters give NC_VISUAL_KEY_NONE. */
nc_visual_key_t nc_visual_key_for_char(char key);
/* The key's meaning on the active screen when it is not a footer entry - the
   jog keys the MANUAL pad draws. NULL when the footer label or the bare key is
   all there is to say. */
const char *nc_visual_key_hint(char key);
bool nc_visual_dirty(void);
bool nc_visual_periodic_needed(void);
void nc_visual_draw(void);

#ifdef __cplusplus
}
#endif

#endif
