#include "ui_input_keypad.h"
#include "cam_keyboard.h"

static char g_ui_input_last_key = 0;
static char g_ui_input_down_key = 0;
static ui_key_t g_ui_input_pending_key = UI_KEY_NONE;



static ui_key_t ui_key_from_char(char k)
{
    switch (k)
    {
        case '0': return UI_KEY_DIGIT_0;
        case '1': return UI_KEY_DIGIT_1;
        case '2': return UI_KEY_DIGIT_2;
        case '3': return UI_KEY_DIGIT_3;
        case '4': return UI_KEY_DIGIT_4;
        case '5': return UI_KEY_DIGIT_5;
        case '6': return UI_KEY_DIGIT_6;
        case '7': return UI_KEY_DIGIT_7;
        case '8': return UI_KEY_DIGIT_8;
        case '9': return UI_KEY_DIGIT_9;

        case '*': return UI_KEY_BACKSPACE;
        case '#': return UI_KEY_FINISH;

        case 'A': return UI_KEY_CANCEL;
        case 'B': return UI_KEY_PREV;
        case 'C': return UI_KEY_NEXT;
        case 'D': return UI_KEY_ACCEPT;

        default:  return UI_KEY_NONE;
    }
}

char ui_key_char(ui_key_t key)
{
    switch (key)
    {
        case UI_KEY_DIGIT_0: return '0';
        case UI_KEY_DIGIT_1: return '1';
        case UI_KEY_DIGIT_2: return '2';
        case UI_KEY_DIGIT_3: return '3';
        case UI_KEY_DIGIT_4: return '4';
        case UI_KEY_DIGIT_5: return '5';
        case UI_KEY_DIGIT_6: return '6';
        case UI_KEY_DIGIT_7: return '7';
        case UI_KEY_DIGIT_8: return '8';
        case UI_KEY_DIGIT_9: return '9';

        case UI_KEY_BACKSPACE: return '*';
        case UI_KEY_FINISH: return '#';

        case UI_KEY_CANCEL: return 'A';
        case UI_KEY_PREV: return 'B';
        case UI_KEY_NEXT: return 'C';
        case UI_KEY_ACCEPT: return 'D';

        default: return 0;
    }
}



void ui_input_keypad_init(void)
{
    cam_keyboard_init();
    g_ui_input_last_key = 0;
    g_ui_input_down_key = 0;
    g_ui_input_pending_key = UI_KEY_NONE;
}

char ui_input_keypad_last_key(void)
{
    return g_ui_input_last_key;
}

char ui_input_keypad_held_key(void)
{
    /* Set on a new press and cleared when the keypad reports the release, so
       this is "down right now", not "seen last". */
    return g_ui_input_down_key;
}

int ui_input_keypad_has_key(void)
{
    return g_ui_input_pending_key != UI_KEY_NONE;
}

ui_key_t ui_input_keypad_take_key(void)
{
    ui_key_t k = g_ui_input_pending_key;

    g_ui_input_pending_key = UI_KEY_NONE;
    return k;
}

void ui_input_keypad_poll(void)
{
    char down;
    char pressed;

    cam_keyboard_update();
    down = cam_keyboard_key_char();
    pressed = cam_keyboard_pressed_char();

    g_ui_input_last_key = down;

    /* A press is a press even when its release came in the same read - a poll
       delayed by a screen change must not swallow the key. A repeat of the key
       that is already down is the keypad re-reporting, not a new press. */
    if (pressed && pressed != g_ui_input_down_key)
    {
        g_ui_input_down_key = pressed;
        g_ui_input_pending_key = ui_key_from_char(pressed);
    }

    /* Not down any more - a tap that came and went in this read included - so
       the next press of the same key counts as a new press again. */
    if (g_ui_input_down_key && down != g_ui_input_down_key)
    {
        g_ui_input_down_key = 0;
    }
}
