#ifndef LEANCAM_SNAPSHOT_FRAME_H
#define LEANCAM_SNAPSHOT_FRAME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifndef UI_SNAPSHOT_TITLE_LEN
#define UI_SNAPSHOT_TITLE_LEN 48
#endif

#ifndef UI_SNAPSHOT_POPUP_LEN
#define UI_SNAPSHOT_POPUP_LEN 64
#endif

#ifndef UI_SNAPSHOT_ENCODER_DEBUG_LEN
#define UI_SNAPSHOT_ENCODER_DEBUG_LEN 128
#endif

typedef enum
{
    UI_SCREEN_NONE = 0,
    UI_SCREEN_STARTUP,
    UI_SCREEN_IDLE,
    UI_SCREEN_ALARM,
    UI_SCREEN_MODAL_POPUP,
    UI_SCREEN_MENU,
    UI_SCREEN_CUSTOM_PAGE
} ui_screen_kind_t;

#ifndef UI_LC_MAX_LINES
#define UI_LC_MAX_LINES 22
#endif

#ifndef UI_LC_LINE_LEN
#define UI_LC_LINE_LEN 160
#endif

#ifndef UI_LC_HELPER_LEN
#define UI_LC_HELPER_LEN 129
#endif

#ifndef UI_LC_PREVIEW_REGION_MAX
#define UI_LC_PREVIEW_REGION_MAX 10
#endif

typedef struct
{
    uint32_t seq;

    uint8_t screen_kind;
    uint8_t current_menu_id;
    int16_t current_index;
    uint8_t total_items;
    uint8_t visible_items;
    uint8_t menu_flags;

    char header[UI_SNAPSHOT_TITLE_LEN];
    char footer[UI_SNAPSHOT_TITLE_LEN];
    char popup[UI_SNAPSHOT_POPUP_LEN];
    char status_line[UI_SNAPSHOT_TITLE_LEN];
    uint32_t diag_uptime_s;
    uint32_t diag_build_count;

    bool leancam_active;
    bool leancam_show_menu;
    uint8_t leancam_mode;
    uint8_t leancam_line_count;
    char leancam_title[UI_SNAPSHOT_TITLE_LEN];
    char leancam_message[UI_SNAPSHOT_POPUP_LEN];
    char leancam_helper[UI_LC_HELPER_LEN];
    char leancam_key_debug[32];
    char leancam_lines[UI_LC_MAX_LINES][UI_LC_LINE_LEN];
    uint8_t leancam_line_selected[UI_LC_MAX_LINES];
    char leancam_setup_line[UI_LC_LINE_LEN];
    char leancam_preview_line[UI_LC_LINE_LEN];
    char leancam_preview_region[UI_LC_PREVIEW_REGION_MAX][UI_LC_LINE_LEN];
    uint8_t leancam_preview_region_selected[UI_LC_PREVIEW_REGION_MAX];
    uint8_t leancam_preview_region_count;
    bool leancam_fullscreen_sim;
    bool leancam_sim_preview_active;
    uint16_t leancam_sim_preview_seq;
    uint16_t leancam_sim_preview_index;
    uint16_t leancam_sim_preview_count;
    char leancam_sim_preview_line[UI_LC_LINE_LEN];
    char leancam_tool_line[UI_LC_LINE_LEN];
    char leancam_active_field[12];
    bool leancam_thread_lane_valid;
    float leancam_thread_start_lane;
    float leancam_thread_stop_lane;
    float leancam_thread_ramp_lane;
    float leancam_thread_lock_lane;
    float leancam_thread_z_speed;

    uint32_t encoder_debug_seq;
    char encoder_debug[UI_SNAPSHOT_ENCODER_DEBUG_LEN];

    uint8_t leancam_field_hi_start[UI_LC_MAX_LINES];
    uint8_t leancam_field_hi_end[UI_LC_MAX_LINES];

    bool show_nav_back;
    bool nav_back_selected;

    uint8_t state;
    bool axes_valid;
    float axis[3];
    bool spindle_valid;
    uint32_t spindle;
    bool feed_valid;
    float feed;
    bool motion_active;

    bool g33_active;
    bool spindle_phase_valid;
    uint32_t spindle_phase;
    uint32_t spindle_cpr;
    bool spindle_ec_valid;
    int32_t spindle_ec;
    bool g33_sync_valid;
    int32_t g33_sync_ec;
} ui_snapshot_frame_t;

static inline void ui_snapshot_strcpy(char *dst, const char *src, uint32_t dst_len)
{
    uint32_t i = 0;

    if (!dst || dst_len == 0)
        return;
    if (!src)
    {
        dst[0] = '\0';
        return;
    }
    while (src[i] && i < (dst_len - 1))
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static inline void ui_snapshot_prepare_frame(ui_snapshot_frame_t *f)
{
    if (f)
        memset(f, 0, sizeof(*f));
}

static inline void ui_snapshot_set_status(ui_snapshot_frame_t *f, const char *s)
{
    if (f)
        ui_snapshot_strcpy(f->status_line, s, sizeof(f->status_line));
}

static inline bool ui_snapshot_has_newer_seq(uint32_t current_seq, uint32_t last_seen_seq)
{
    return current_seq != last_seen_seq;
}

#ifdef __cplusplus
}
#endif

#endif /* LEANCAM_SNAPSHOT_FRAME_H */
