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
    char key;

    cam_keyboard_update();
    key = cam_keyboard_key_char();

    g_ui_input_last_key = key;

    /* key released: arm next press, even if same key */
    if (!key)
    {
        g_ui_input_down_key = 0;
        return;
    }

    /* same physical press still held */
    if (key == g_ui_input_down_key)
        return;

    /* new press */
    g_ui_input_down_key = key;
    g_ui_input_pending_key = ui_key_from_char(key);
}