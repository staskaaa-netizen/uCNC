/*
 * ui_snapshot_builder.c
 *
 * Core1 side builder for live UI snapshot.
 *
 * Responsibilities:
 *   - poll CAM keypad on core1
 *   - send decoded key events into LeanCam bridge on core1
 *   - build one immutable snapshot for core0 renderer
 *
 * Core0 renderer must not poll keypad, touch filesystem, or read LeanCam globals.
 */

#include "../../cnc.h"
#include "../system_menu.h"
#include "ui_snapshot.h"
#include "../cam_keyboard/cam_keyboard.h"
#include "../cam_keyboard/ui_input_keypad.h"
#include "../encoder.h"
#include "../file_system.h"
#include "../leanCam/leancam_bridge.h"


#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

ui_snapshot_shared_t g_ui_snapshot = {0};

#ifdef ARDUINO_ARCH_ESP32
SemaphoreHandle_t g_ui_snapshot_mutex = NULL;
#endif

#ifndef UI_SNAPSHOT_UPDATE_MS
#define UI_SNAPSHOT_UPDATE_MS 50
#endif

static char g_ui_popup_text[UI_SNAPSHOT_POPUP_LEN] = {0};

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

static uint8_t ui_builder_detect_screen_kind_simple(void)
{
    if (g_system_menu.flags & SYSTEM_MENU_MODE_MODAL_POPUP)
        return UI_SCREEN_MODAL_POPUP;

    switch (g_system_menu.current_menu)
    {
        case SYSTEM_MENU_ID_STARTUP:
            return UI_SCREEN_STARTUP;

        case SYSTEM_MENU_ID_IDLE:
            return UI_SCREEN_IDLE;

        default:
            return UI_SCREEN_IDLE;
    }
}

static void ui_builder_fill_header_footer(ui_snapshot_frame_t *f)
{
    if (!f)
        return;

    switch (f->screen_kind)
    {
        case UI_SCREEN_STARTUP:     ui_snapshot_set_header(f, "Startup"); break;
        case UI_SCREEN_IDLE:        ui_snapshot_set_header(f, "Idle");    break;
        case UI_SCREEN_MODAL_POPUP: ui_snapshot_set_header(f, "Popup");   break;
        case UI_SCREEN_ALARM:       ui_snapshot_set_header(f, "Alarm");   break;
        case UI_SCREEN_MENU:        ui_snapshot_set_header(f, "Menu");    break;
        case UI_SCREEN_CUSTOM_PAGE: ui_snapshot_set_header(f, "Custom");  break;
        default:                    ui_snapshot_set_header(f, "Runtime"); break;
    }

    ui_snapshot_set_footer(f, "");
}

static void ui_builder_fill_runtime(ui_snapshot_frame_t *f)
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

    if (!f)
        return;

    f->state = cnc_get_exec_state(0xFF);
    itp_get_rt_position(steppos);
    kinematics_steps_to_coordinates(steppos, axis);

    f->axis[0] = axis[0];
    f->axis[1] = axis[1];
    f->axis[2] = axis[2];
    f->axes_valid = true;

    f->feed = itp_get_rt_feed();
    f->feed_valid = true;
    f->spindle = tool_get_speed();
    f->spindle_valid = true;
    f->motion_active = !itp_is_empty() || !planner_buffer_is_empty() || fs_file_run_active();

    f->g33_active = g33_els_is_active();
#ifdef G33_ENCODER
    cpr = (uint32_t)g_settings.encoders_resolution[G33_ENCODER];
    ec = encoder_get_position(G33_ENCODER);
    f->spindle_ec = ec;
    f->spindle_ec_valid = true;
    if (cpr > 0 && encoder_get_index_live_delta(G33_ENCODER, &phase_delta))
    {
        int32_t phase = phase_delta % (int32_t)cpr;
        if (phase < 0)
            phase += (int32_t)cpr;
        f->spindle_phase = (uint32_t)phase;
        f->spindle_cpr = cpr;
        f->spindle_phase_valid = true;
    }
    if (g33_els_get_sync(&sync_ec, &sync_cpr))
    {
        f->g33_sync_ec = sync_ec;
        f->g33_sync_valid = true;
        if (!f->spindle_cpr && sync_cpr)
            f->spindle_cpr = sync_cpr;
    }
#endif
}

static void ui_builder_poll_cam_keyboard(void)
{
    ui_input_keypad_poll();

    while (ui_input_keypad_has_key())
    {
        ui_key_t key = ui_input_keypad_take_key();
        if (key != UI_KEY_NONE)
            leancam_bridge_handle_key(key);
    }
}

void ui_snapshot_set_modal_popup_text(const char *s)
{
    ui_snapshot_strcpy(g_ui_popup_text, s, sizeof(g_ui_popup_text));
}

void ui_snapshot_clear_modal_popup_text(void)
{
    g_ui_popup_text[0] = '\0';
}

void ui_snapshot_build_live(void)
{
    static ui_snapshot_frame_t f;
    char keybuf[32];
    const uint8_t *raw;
    char keych;

    ui_snapshot_prepare_frame(&f);

    f.screen_kind        = ui_builder_detect_screen_kind_simple();
    f.current_menu_id    = g_system_menu.current_menu;
    f.current_index      = g_system_menu.current_index;
    f.menu_flags         = g_system_menu.flags;
    f.total_items        = 0;
    f.visible_items      = 0;
    f.show_nav_back      = false;
    f.nav_back_selected  = false;

    ui_builder_fill_header_footer(&f);
    ui_builder_fill_runtime(&f);
    leancam_bridge_fill_snapshot(&f);
    encoder_get_index_debug_line(ENC0, f.encoder_debug, sizeof(f.encoder_debug), &f.encoder_debug_seq);

    raw = cam_keyboard_raw();
    keych = cam_keyboard_key_char();
    snprintf(keybuf, sizeof(keybuf), "K:%c %02X %02X %02X %02X %02X %02X",
             keych ? keych : '-',
             raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
    ui_snapshot_strcpy(f.leancam_key_debug, keybuf, sizeof(f.leancam_key_debug));

    if (f.screen_kind == UI_SCREEN_MODAL_POPUP)
        ui_snapshot_set_popup(&f, g_ui_popup_text[0] ? g_ui_popup_text : "Popup");

    ui_snapshot_publish(&g_ui_snapshot, &f);
}



static bool ui_snapshot_builder_update(void *args)
{
    static uint32_t last_ms = 0;
    uint32_t now;

    (void)args;

    now = mcu_millis();

    if ((uint32_t)(now - last_ms) < UI_SNAPSHOT_UPDATE_MS)
        return EVENT_CONTINUE;

    last_ms = now;

    ui_builder_poll_cam_keyboard();
    leancam_bridge_tick();
    ui_snapshot_build_live();

    return EVENT_CONTINUE;
}

static bool ui_snapshot_builder_force(void *args)
{
    (void)args;
    ui_snapshot_build_live();
    return EVENT_CONTINUE;
}


CREATE_EVENT_LISTENER(cnc_dotasks, ui_snapshot_builder_update);

CREATE_EVENT_LISTENER(cnc_reset,   ui_snapshot_builder_force);
CREATE_EVENT_LISTENER(cnc_alarm,   ui_snapshot_builder_force);

DECL_MODULE(ui_snapshot_builder)
{
#ifdef ARDUINO_ARCH_ESP32
    if (g_ui_snapshot_mutex == NULL)
        g_ui_snapshot_mutex = xSemaphoreCreateMutex();
#endif

    memset(&g_ui_snapshot, 0, sizeof(g_ui_snapshot));

    ui_input_keypad_init();
    leancam_bridge_init();

    ui_snapshot_build_live();

    ADD_EVENT_LISTENER(cnc_dotasks, ui_snapshot_builder_update);
    /* Keep these disabled if they caused extra churn before. */
    /* ADD_EVENT_LISTENER(cnc_reset, ui_snapshot_builder_force); */
    /* ADD_EVENT_LISTENER(cnc_alarm, ui_snapshot_builder_force); */
}
