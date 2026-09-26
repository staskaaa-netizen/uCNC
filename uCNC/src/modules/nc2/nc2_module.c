/* nc2 as a ÂµCNC module: the panel the machine runs.

   This is the whole of what the firmware needs from the module - the boot, the
   keypad and the main-loop hook - so the module is switched by the build, the
   way nc was: the sources are compiled in (or not) and `LOAD_MODULE(nc2)` runs
   this. Nothing here owns a screen rule: the keys go to the screen's own table
   and the drawing is the screen's. */
#include "../../cnc.h"
#include "nc2_visual.h"
#include "../lvds_renderer/lvds_renderer_boot.h"
#include "../cam_keyboard/ui_input_keypad.h"

static void nc2_module_poll_keyboard(void)
{
    ui_input_keypad_poll();
    while (ui_input_keypad_has_key()) {
        char key = ui_key_char(ui_input_keypad_take_key());

        if (key) {
            nc2_visual_key(key);
        }
    }
    /* The key that is still down is what keeps a MANUAL feed going: the screen
       cancels the jog when the key comes up. */
    nc2_visual_hold_key(ui_key_char(ui_input_keypad_held_key()));
}

static bool nc2_module_update(void *args)
{
    (void)args;
    nc2_module_poll_keyboard();
    /* The screen decides; the module only hands it the clock. */
    if (nc2_visual_pump(mcu_millis())) {
        nc2_visual_draw();
    }
    return EVENT_CONTINUE;
}

CREATE_EVENT_LISTENER(cnc_dotasks, nc2_module_update);

DECL_MODULE(nc2)
{
    if (!lvds_renderer_boot_init()) {
        return;
    }
    ui_input_keypad_init();
    nc2_visual_init();
    ADD_EVENT_LISTENER(cnc_dotasks, nc2_module_update);
}
