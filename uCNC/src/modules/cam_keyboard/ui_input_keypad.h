#ifndef UI_INPUT_KEYPAD_H
#define UI_INPUT_KEYPAD_H

#include "../ui_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_input_keypad_init(void);
void ui_input_keypad_poll(void);

/* last seen raw key, for debug/footer */
char ui_input_keypad_last_key(void);

/* The key that is down right now, 0 when none is. The NC MANUAL feed reads it:
   a held direction key feeds until it comes up. */
char ui_input_keypad_held_key(void);

/* The character a key code stands for on the keypad ('0'-'9', '*', '#',
   'A'-'D'), the inverse of the table this file decodes with. */
char ui_key_char(ui_key_t key);

/* pending key interface */
ui_key_t ui_input_keypad_take_key(void);
int ui_input_keypad_has_key(void);

#ifdef __cplusplus
}
#endif

#endif
