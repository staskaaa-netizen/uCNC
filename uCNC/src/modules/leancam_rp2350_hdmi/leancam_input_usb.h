#ifndef LEANCAM_INPUT_USB_H
#define LEANCAM_INPUT_USB_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LC_INPUT_NONE = 0,
    LC_INPUT_CHAR,
    LC_INPUT_ENTER,
    LC_INPUT_ESC,
    LC_INPUT_BACKSPACE,
    LC_INPUT_UP,
    LC_INPUT_DOWN,
    LC_INPUT_LEFT,
    LC_INPUT_RIGHT,
    LC_INPUT_TAB,
    LC_INPUT_SHIFT_TAB
} lc_input_type_t;

typedef struct {
    lc_input_type_t type;
    char ch;
} lc_input_event_t;

void lc_input_usb_init(void);
void lc_input_usb_task(void);
bool lc_input_usb_poll(lc_input_event_t *event);
bool lc_input_usb_keyboard_connected(void);

#ifdef __cplusplus
}
#endif

#endif
