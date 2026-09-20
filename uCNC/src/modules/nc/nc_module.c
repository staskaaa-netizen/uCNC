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
    /* The screen owns what a key means, so the keypad only reports which key
       was pressed; the screen's own table turns the character into an action.
       B and C walk between words with the same letter (an X finds the next X)
       and never edit; outside a code view the screen falls back to stepping. */
    return nc_visual_key_for_char(ui_key_char(key));
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
    /* The key that is still down is what keeps a MANUAL feed going: the panel
       cancels the jog when the key comes up. */
    nc_visual_hold_key(ui_input_keypad_held_key());
}

static bool nc_module_update(void *args)
{
    uint32_t now;
    bool periodic;

    (void)args;
    nc_module_poll_keyboard();
    nc_visual_idle_tasks();

    now = mcu_millis();
    periodic = nc_visual_periodic_needed() &&
               (uint32_t)(now - g_nc_module_last_draw_ms) >= NC_MODULE_DRAW_PERIOD_MS;
    if (nc_visual_dirty() || periodic) {
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
