#include "lvds_renderer_state.h"

#include "../../cnc.h"
#include "../encoder.h"
#include "../file_system.h"
#include "../leanCam/leancam_bridge.h"

#ifndef LEANCAM_RP2350_NO_CAM_KEYBOARD
#include "../cam_keyboard/cam_keyboard.h"
#include "../cam_keyboard/ui_input_keypad.h"
#endif

#include <stdio.h>

#ifndef LVDS_RENDERER_STATE_MS
#define LVDS_RENDERER_STATE_MS 10
#endif

/* Do not call fs_file_run_active() from this renderer state poll. In the RP2350
 * LVDS + sd_card_v2 build it deadlocks the display path after SD mount;
 * planner/interpolator state is enough for the UI badge.
 */

static ui_snapshot_frame_t g_lvds_frame;
static uint32_t g_lvds_seq;
static volatile bool g_lvds_frame_dirty;

bool __attribute__((weak)) encoder_get_index_debug_line(uint8_t i, char *line, uint32_t line_len, uint32_t *seq)
{
    (void)i;
    (void)line;
    (void)line_len;
    (void)seq;
    return false;
}

bool __attribute__((weak)) g33_els_is_active(void)
{
    return false;
}

bool __attribute__((weak)) g33_els_get_sync(int32_t *start_ec, uint32_t *cpr)
{
    (void)start_ec;
    (void)cpr;
    return false;
}

static void lc_poll_inputs(void)
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

static void lc_fill_runtime(ui_snapshot_frame_t *frame)
{
    int32_t steppos[STEPPER_COUNT] = {0};
    float axis[MAX(AXIS_COUNT, 3)] = {0};
#ifdef G33_ENCODER
    int32_t phase_delta = 0;
    int32_t sync_ec = 0;
    int32_t ec = 0;
    uint32_t cpr = 0;
    uint32_t sync_cpr = 0;
#endif

    frame->state = cnc_get_exec_state(0xff);
    itp_get_rt_position(steppos);
    kinematics_steps_to_coordinates(steppos, axis);
    frame->axis[0] = axis[0];
    frame->axis[1] = axis[1];
    frame->axis[2] = axis[2];
    frame->axes_valid = true;
    frame->feed = itp_get_rt_feed();
    frame->feed_valid = true;
    frame->spindle = tool_get_speed();
    frame->spindle_valid = true;
    frame->motion_active = !itp_is_empty() || !planner_buffer_is_empty();
    frame->g33_active = g33_els_is_active();

#ifdef G33_ENCODER
    cpr = (uint32_t)g_settings.encoders_resolution[G33_ENCODER];
    ec = encoder_get_position(G33_ENCODER);
    frame->spindle_ec = ec;
    frame->spindle_ec_valid = true;
    if (cpr > 0 && encoder_get_index_live_delta(G33_ENCODER, &phase_delta)) {
        int32_t phase = phase_delta % (int32_t)cpr;
        if (phase < 0) {
            phase += (int32_t)cpr;
        }
        frame->spindle_phase = (uint32_t)phase;
        frame->spindle_cpr = cpr;
        frame->spindle_phase_valid = true;
    }
    if (g33_els_get_sync(&sync_ec, &sync_cpr)) {
        frame->g33_sync_ec = sync_ec;
        frame->g33_sync_valid = true;
        if (!frame->spindle_cpr && sync_cpr) {
            frame->spindle_cpr = sync_cpr;
        }
    }
#endif
}

static void lc_build_frame(ui_snapshot_frame_t *frame, uint32_t seq)
{
#ifndef LEANCAM_RP2350_NO_CAM_KEYBOARD
    char keybuf[32];
    const uint8_t *raw;
    char keych;
#endif

    ui_snapshot_prepare_frame(frame);
    frame->seq = seq;
    frame->screen_kind = UI_SCREEN_IDLE;

    lc_fill_runtime(frame);
    leancam_bridge_fill_snapshot(frame);
    encoder_get_index_debug_line(ENC0, frame->encoder_debug, sizeof(frame->encoder_debug), &frame->encoder_debug_seq);

#ifndef LEANCAM_RP2350_NO_CAM_KEYBOARD
    raw = cam_keyboard_raw();
    keych = cam_keyboard_key_char();
    snprintf(keybuf, sizeof(keybuf), "K:%c %02X %02X %02X %02X %02X %02X",
             keych ? keych : '-',
             raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
    ui_snapshot_strcpy(frame->leancam_key_debug, keybuf, sizeof(frame->leancam_key_debug));
#endif
}

void leancam_bridge_request_render(void)
{
    g_lvds_frame_dirty = true;
}

void lvds_renderer_state_init(void)
{
#ifndef LEANCAM_RP2350_NO_CAM_KEYBOARD
    ui_input_keypad_init();
#endif
    leancam_bridge_init();
    g_lvds_seq = 1;
    lc_build_frame(&g_lvds_frame, g_lvds_seq);
    g_lvds_frame_dirty = false;
}

void lvds_renderer_state_poll(void)
{
    static uint32_t last_ms;
    uint32_t now = mcu_millis();

    lc_poll_inputs();
    leancam_bridge_tick();

    if (!g_lvds_frame_dirty && (uint32_t)(now - last_ms) < LVDS_RENDERER_STATE_MS) {
        return;
    }
    last_ms = now;
    g_lvds_frame_dirty = false;
    lc_build_frame(&g_lvds_frame, ++g_lvds_seq);
}

const ui_snapshot_frame_t *lvds_renderer_state_frame(void)
{
    return &g_lvds_frame;
}
