/* LeanCam module contract:
 * Purpose: the single visible runtime owner for LeanCam/LVDS UI advancement.
 * Called by: cnc_io_dotasks through its own listener after it boots LVDS hardware.
 * Calls into: keypad polling, leancam_bridge tick/input, snapshot build, and renderer draw in fixed order.
 * Owns: ordering, cadence, and reentry protection; it must not contain editor policy or drawing logic.
 */
#include "../../cnc.h"
#include "execution_controller.h"
#include "visual/leancam_visual_state.h"
#include "leancam_bridge.h"
#include "visual/leancam_visual.h"
#include "../lvds_renderer/lvds_renderer_boot.h"

#ifndef LEANCAM_RP2350_NO_CAM_KEYBOARD
#include "../cam_keyboard/ui_input_keypad.h"
#endif

static bool g_execution_in_poll;
static uint32_t g_execution_snapshot_last_ms;
static uint32_t g_execution_render_last_ms;
static uint32_t g_execution_present_last_ms;

static bool execution_controller_update(void *args)
{
    (void)args;
    execution_controller_poll();
    return EVENT_CONTINUE;
}

CREATE_EVENT_LISTENER(cnc_dotasks, execution_controller_update);

static void execution_controller_step_inputs(void)
{
#ifndef LEANCAM_RP2350_NO_CAM_KEYBOARD
    ui_input_keypad_poll();
    while (ui_input_keypad_has_key()) {
        ui_key_t key = ui_input_keypad_take_key();
        if (key != UI_KEY_NONE) {
            leancam_bridge_handle_key(key);
        }
    }
#endif
}

static void execution_controller_step_bridge(void)
{
    leancam_bridge_tick();
}

static void execution_controller_step_snapshot(uint32_t now)
{
    if ((uint32_t)(now - g_execution_snapshot_last_ms) < EXECUTION_CONTROLLER_SNAPSHOT_MS)
        return;
    g_execution_snapshot_last_ms = now;
    leancam_visual_state_poll();
}

static void execution_controller_step_render(uint32_t now)
{
    if ((uint32_t)(now - g_execution_render_last_ms) < EXECUTION_CONTROLLER_RENDER_MS)
        return;
#if EXECUTION_CONTROLLER_CHUNKED_PRESENT
    if (lvds_hstx_present_chunked_busy())
        return;
#endif
    g_execution_render_last_ms = now;
    leancam_visual_prepare();
    leancam_visual_draw();
}

static void execution_controller_step_present(uint32_t now)
{
#if EXECUTION_CONTROLLER_CHUNKED_PRESENT
    if (EXECUTION_CONTROLLER_PRESENT_MS &&
        (uint32_t)(now - g_execution_present_last_ms) < EXECUTION_CONTROLLER_PRESENT_MS)
        return;
    g_execution_present_last_ms = now;
    (void)lvds_hstx_present_chunked_step();
#else
    (void)now;
#endif
}

void execution_controller_init(void)
{
#ifndef LEANCAM_RP2350_NO_CAM_KEYBOARD
    ui_input_keypad_init();
#endif
    leancam_bridge_init();
    leancam_visual_state_init();
    leancam_visual_init();
    g_execution_snapshot_last_ms = 0;
    g_execution_render_last_ms = 0;
    g_execution_present_last_ms = 0;
}

void execution_controller_poll(void)
{
    uint32_t now_ms;

    if (g_execution_in_poll) {
        return;
    }

    g_execution_in_poll = true;
    now_ms = mcu_millis();

    execution_controller_step_inputs();
    execution_controller_step_bridge();
    execution_controller_step_snapshot(now_ms);
    execution_controller_step_render(now_ms);
    execution_controller_step_present(now_ms);

    g_execution_in_poll = false;
}

DECL_MODULE(leanCam)
{
    if (!lvds_renderer_boot_init())
        return;
    execution_controller_init();
    ADD_EVENT_LISTENER(cnc_dotasks, execution_controller_update);
}
