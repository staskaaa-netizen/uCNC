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
    NC_VISUAL_KEY_FIELD_NEXT
} nc_visual_key_t;

void nc_visual_init(void);
/* Jump straight to a mode (desktop shells and future mode keys). Cycles the
   document exactly like the MODE key does. */
void nc_visual_select_mode(nc_mode_t mode);
void nc_visual_handle_key(nc_visual_key_t key);
/* True while a value/word draft is being typed, so a shell can switch its
   keypad from menu keys to digits. */
bool nc_visual_value_editing(void);
/* Footer (soft key) items of the active mode, same list the panel draws at the
   bottom of the screen. `count` may be NULL. */
const nc_footer_item_t *nc_visual_footer(size_t *count);
/* Perform a footer action, exactly as pressing its soft key would. */
void nc_visual_footer_action(nc_footer_action_t action);
bool nc_visual_dirty(void);
bool nc_visual_periodic_needed(void);
void nc_visual_draw(void);

#ifdef __cplusplus
}
#endif

#endif
