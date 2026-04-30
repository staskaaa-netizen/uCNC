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

/* pending key interface */
ui_key_t ui_input_keypad_take_key(void);
int ui_input_keypad_has_key(void);

#ifdef __cplusplus
}
#endif

#endif
