/* ncc as a ÂµCNC module: the panel the machine runs.

   This is the whole of what the firmware needs from the module - the boot, the
   keypad and the main-loop hook - so the module is switched by the build, the
   way nc was: the sources are compiled in (or not) and `LOAD_MODULE(ncc)` runs
   this. Nothing here owns a screen rule: the keys go to the screen's own table
   and the drawing is the screen's. */
#include "../../cnc.h"
#include "ncc_visual.h"
#include "../lvds_renderer/lvds_renderer_boot.h"
#include "../cam_keyboard/ui_input_keypad.h"
#if ENABLE_SHIFT_REGISTER_DEBUG
#include "hardware/gpio.h"
#include "hardware/spi.h"
#endif

#if ENABLE_SHIFT_REGISTER_DEBUG
extern volatile uint8_t ic74hc595_io_pins[];

static bool ncc_shift_register_status(void *args)
{
    (void)args;
    proto_printf("|SR:%x,%x,%x,%x",
                 ic74hc595_io_pins[0], ic74hc595_io_pins[1],
                 ic74hc595_io_pins[2], ic74hc595_io_pins[3]);
    proto_printf("|GF:%u,%u,%u,%u,%u,%u,%u,%u,%u",
                 (unsigned)gpio_get_function(20), (unsigned)gpio_get_function(21),
                 (unsigned)gpio_get_function(22), (unsigned)gpio_get_function(23),
                 (unsigned)gpio_get_function(24), (unsigned)gpio_get_function(34),
                 (unsigned)gpio_get_function(35), (unsigned)gpio_get_function(36),
                 (unsigned)gpio_get_function(37));
    proto_printf("|GD:%u,%u,%u,%u,%u,%u,%u,%u,%u",
                 (unsigned)gpio_get_dir(20), (unsigned)gpio_get_dir(21),
                 (unsigned)gpio_get_dir(22), (unsigned)gpio_get_dir(23),
                 (unsigned)gpio_get_dir(24), (unsigned)gpio_get_dir(34),
                 (unsigned)gpio_get_dir(35), (unsigned)gpio_get_dir(36),
                 (unsigned)gpio_get_dir(37));
    proto_printf("|GV:%u,%u,%u,%u,%u,%u,%u,%u,%u",
                 (unsigned)gpio_get(20), (unsigned)gpio_get(21),
                 (unsigned)gpio_get(22), (unsigned)gpio_get(23),
                 (unsigned)gpio_get(24), (unsigned)gpio_get(34),
                 (unsigned)gpio_get(35), (unsigned)gpio_get(36),
                 (unsigned)gpio_get(37));
    proto_printf("|SP:%lx,%lx,%lx,%lu",
                 (unsigned long)spi_get_hw(SPI_HW)->cr0,
                 (unsigned long)spi_get_hw(SPI_HW)->cr1,
                 (unsigned long)spi_get_hw(SPI_HW)->cpsr,
                 (unsigned long)spi_get_baudrate(SPI_HW));
    return EVENT_CONTINUE;
}
CREATE_EVENT_LISTENER(proto_status, ncc_shift_register_status);
#endif

static void ncc_module_poll_keyboard(void)
{
    ui_input_keypad_poll();
    while (ui_input_keypad_has_key()) {
        char key = ui_key_char(ui_input_keypad_take_key());

        if (key) {
            ncc_visual_key(key);
        }
    }
    /* The key that is still down is what keeps a MANUAL feed going: the screen
       cancels the jog when the key comes up. */
    ncc_visual_hold_key(ui_key_char(ui_input_keypad_held_key()));
}

static bool ncc_module_update(void *args)
{
    (void)args;
    ncc_module_poll_keyboard();
    /* The screen decides; the module only hands it the clock. */
    if (ncc_visual_pump(mcu_millis())) {
        ncc_visual_draw();
    }
    return EVENT_CONTINUE;
}

/* The panel must keep drawing while the controller is in its reset/alarm
   wait loop. `cnc_dotasks` stops before module tasks in that state;
   `cnc_io_dotasks` still runs, so the operator sees the fault screen rather
   than a black panel. */
CREATE_EVENT_LISTENER(cnc_io_dotasks, ncc_module_update);

DECL_MODULE(ncc)
{
    if (!lvds_renderer_boot_init()) {
        return;
    }
    ui_input_keypad_init();
    ncc_visual_init();
    ADD_EVENT_LISTENER(cnc_io_dotasks, ncc_module_update);
#if ENABLE_SHIFT_REGISTER_DEBUG
    ADD_EVENT_LISTENER(proto_status, ncc_shift_register_status);
#endif
}
