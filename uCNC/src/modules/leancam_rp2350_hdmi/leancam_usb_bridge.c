#include "../../cnc.h"
#include "../leanCam/leancam_bridge.h"
#include "../ui_keys.h"
#include "leancam_hdmi_renderer.h"
#include "leancam_input_usb.h"

static ui_key_t lc_usb_event_to_key(const lc_input_event_t *ev)
{
    if (!ev) {
        return UI_KEY_NONE;
    }

    switch (ev->type) {
        case LC_INPUT_ENTER: return UI_KEY_ACCEPT;
        case LC_INPUT_ESC: return UI_KEY_CANCEL;
        case LC_INPUT_BACKSPACE: return UI_KEY_BACKSPACE;
        case LC_INPUT_UP:
        case LC_INPUT_LEFT:
        case LC_INPUT_SHIFT_TAB: return UI_KEY_PREV;
        case LC_INPUT_DOWN:
        case LC_INPUT_RIGHT:
        case LC_INPUT_TAB: return UI_KEY_NEXT;
        case LC_INPUT_CHAR:
            if (ev->ch >= '0' && ev->ch <= '9') {
                return (ui_key_t)(UI_KEY_DIGIT_0 + (ev->ch - '0'));
            }
            switch (ev->ch) {
                case 'a':
                case 'A': return UI_KEY_CANCEL;
                case 'b':
                case 'B': return UI_KEY_PREV;
                case 'c':
                case 'C': return UI_KEY_NEXT;
                case 'd':
                case 'D': return UI_KEY_ACCEPT;
                case '*': return UI_KEY_BACKSPACE;
                case '#': return UI_KEY_FINISH;
                case '-': return UI_KEY_PREV;
                case '.': return UI_KEY_NEXT;
                default: return UI_KEY_NONE;
            }
        default:
            return UI_KEY_NONE;
    }
}

static bool leancam_rp2350_usb_update(void *args)
{
    lc_input_event_t ev;

    (void)args;
    lc_input_usb_task();
    leancam_hdmi_renderer_set_keyboard_connected(lc_input_usb_keyboard_connected());

    while (lc_input_usb_poll(&ev)) {
        ui_key_t key = lc_usb_event_to_key(&ev);
        if (key != UI_KEY_NONE) {
            leancam_bridge_handle_key(key);
            ui_snapshot_build_live();
        }
    }

    return EVENT_CONTINUE;
}

CREATE_EVENT_LISTENER(cnc_dotasks, leancam_rp2350_usb_update);

DECL_MODULE(leancam_rp2350_usb)
{
    lc_input_usb_init();
    ADD_EVENT_LISTENER(cnc_dotasks, leancam_rp2350_usb_update);
}
