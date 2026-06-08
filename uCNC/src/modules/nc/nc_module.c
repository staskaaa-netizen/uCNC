#include "../../cnc.h"
#include "nc_visual.h"
#include "../lvds_renderer/lvds_renderer_boot.h"
#include "../cam_keyboard/ui_input_keypad.h"

#ifndef NC_MODULE_DRAW_PERIOD_MS
#define NC_MODULE_DRAW_PERIOD_MS 20u
#endif

static uint32_t g_nc_module_last_draw_ms;

static nc_visual_key_t nc_module_map_key(ui_key_t key)
{
    switch (key) {
    case UI_KEY_DIGIT_0: return NC_VISUAL_KEY_DIGIT_0;
    case UI_KEY_DIGIT_1: return NC_VISUAL_KEY_DIGIT_1;
    case UI_KEY_DIGIT_2: return NC_VISUAL_KEY_DIGIT_2;
    case UI_KEY_DIGIT_3: return NC_VISUAL_KEY_DIGIT_3;
    case UI_KEY_DIGIT_4: return NC_VISUAL_KEY_DIGIT_4;
    case UI_KEY_DIGIT_5: return NC_VISUAL_KEY_DIGIT_5;
    case UI_KEY_DIGIT_6: return NC_VISUAL_KEY_DIGIT_6;
    case UI_KEY_DIGIT_7: return NC_VISUAL_KEY_DIGIT_7;
    case UI_KEY_DIGIT_8: return NC_VISUAL_KEY_DIGIT_8;
    case UI_KEY_DIGIT_9: return NC_VISUAL_KEY_DIGIT_9;
    case UI_KEY_BACKSPACE: return NC_VISUAL_KEY_BACKSPACE;
    case UI_KEY_FINISH: return NC_VISUAL_KEY_FINISH;
    case UI_KEY_CANCEL: return NC_VISUAL_KEY_MODE;
    case UI_KEY_PREV: return NC_VISUAL_KEY_PREV;
    case UI_KEY_NEXT: return NC_VISUAL_KEY_NEXT;
    case UI_KEY_ACCEPT: return NC_VISUAL_KEY_ACCEPT;
    default: return NC_VISUAL_KEY_NONE;
    }
}

static void nc_module_poll_keyboard(void)
{
    ui_input_keypad_poll();
    while (ui_input_keypad_has_key()) {
        nc_visual_key_t key = nc_module_map_key(ui_input_keypad_take_key());
        if (key != NC_VISUAL_KEY_NONE) {
            nc_visual_handle_key(key);
        }
    }
}

static bool nc_module_update(void *args)
{
    uint32_t now;

    (void)args;
    nc_module_poll_keyboard();

    now = mcu_millis();
    if (nc_visual_dirty() ||
        (uint32_t)(now - g_nc_module_last_draw_ms) >= NC_MODULE_DRAW_PERIOD_MS) {
        g_nc_module_last_draw_ms = now;
        nc_visual_draw();
    }
    return EVENT_CONTINUE;
}

CREATE_EVENT_LISTENER(cnc_dotasks, nc_module_update);

DECL_MODULE(nc)
{
    if (!lvds_renderer_boot_init()) {
        return;
    }
    ui_input_keypad_init();
    nc_visual_init();
    g_nc_module_last_draw_ms = 0;
    ADD_EVENT_LISTENER(cnc_dotasks, nc_module_update);
}
