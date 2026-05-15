#ifndef UI_SNAPSHOT_H
#define UI_SNAPSHOT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef ARDUINO_ARCH_ESP32
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#ifndef UI_SNAPSHOT_TITLE_LEN
#define UI_SNAPSHOT_TITLE_LEN 48
#endif

#ifndef UI_SNAPSHOT_POPUP_LEN
#define UI_SNAPSHOT_POPUP_LEN 64
#endif

#ifndef UI_SNAPSHOT_GCODE_LEN
#define UI_SNAPSHOT_GCODE_LEN 96
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

typedef struct
{
    bool valid;

    uint8_t motion_code;   /* 0,1,2,3... */
    bool has_x;
    bool has_y;
    bool has_z;
    bool has_f;

    float x;
    float y;
    float z;
    float f;
} ui_gcode_snapshot_t;

#define UI_GCODE_LINE_LEN RX_BUFFER_CAPACITY

#ifndef UI_LC_MAX_LINES
#define UI_LC_MAX_LINES 22
#endif

#ifndef UI_LC_LINE_LEN
#define UI_LC_LINE_LEN 96
#endif

#ifndef UI_LC_HELPER_LEN
#define UI_LC_HELPER_LEN 129
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

    /* LeanCam preview. Built on core1, rendered on core0. */
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

    /* Optional field-only highlight. Bridge owns these spans.
     * Span is [start, end) in characters inside leancam_lines[row].
     */
    uint8_t leancam_field_hi_start[UI_LC_MAX_LINES];
    uint8_t leancam_field_hi_end[UI_LC_MAX_LINES];

    bool show_nav_back;
    bool nav_back_selected;

    /* live runtime */
    uint8_t state;
    bool axes_valid;
    float axis[3];
    bool spindle_valid;
    uint32_t  spindle;
    bool feed_valid;
    float  feed;
    bool motion_active;

    /* Threading/live-sim hints. Built on core1; renderer must not poll motion
     * modules directly.
     */
    bool g33_active;
    bool spindle_phase_valid;
    uint32_t spindle_phase;
    uint32_t spindle_cpr;
    bool spindle_ec_valid;
    int32_t spindle_ec;
    bool g33_sync_valid;
    int32_t g33_sync_ec;
   
} ui_snapshot_frame_t;

typedef struct
{
    volatile uint32_t publish_seq;
    ui_snapshot_frame_t frame;
} ui_snapshot_shared_t;

extern ui_snapshot_shared_t g_ui_snapshot;

void ui_snapshot_build_live(void);

#ifdef ARDUINO_ARCH_ESP32
extern SemaphoreHandle_t g_ui_snapshot_mutex;
#endif

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
    if (!f) return;
    memset(f, 0, sizeof(*f));
}

static inline void ui_snapshot_set_header(ui_snapshot_frame_t *f, const char *s)
{
    if (!f) return;
    ui_snapshot_strcpy(f->header, s, sizeof(f->header));
}

static inline void ui_snapshot_set_footer(ui_snapshot_frame_t *f, const char *s)
{
    if (!f) return;
    ui_snapshot_strcpy(f->footer, s, sizeof(f->footer));
}

static inline void ui_snapshot_set_popup(ui_snapshot_frame_t *f, const char *s)
{
    if (!f) return;
    ui_snapshot_strcpy(f->popup, s, sizeof(f->popup));
}

static inline void ui_snapshot_set_status(ui_snapshot_frame_t *f, const char *s)
{
    if (!f) return;
    ui_snapshot_strcpy(f->status_line, s, sizeof(f->status_line));
}



static inline void ui_snapshot_publish(ui_snapshot_shared_t *sh, const ui_snapshot_frame_t *src)
{
    if (!sh || !src)
        return;

#ifdef ARDUINO_ARCH_ESP32
    if (g_ui_snapshot_mutex != NULL)
        xSemaphoreTake(g_ui_snapshot_mutex, portMAX_DELAY);
#endif

    sh->frame = *src;
    sh->frame.seq = sh->publish_seq + 1U;
    sh->publish_seq = sh->frame.seq;

#ifdef ARDUINO_ARCH_ESP32
    if (g_ui_snapshot_mutex != NULL)
        xSemaphoreGive(g_ui_snapshot_mutex);
#endif
}

static inline bool ui_snapshot_copy_latest(const ui_snapshot_shared_t *sh,
                                           ui_snapshot_frame_t *dst,
                                           uint32_t *seq_out)
{
    if (!sh || !dst || !seq_out)
        return false;

#ifdef ARDUINO_ARCH_ESP32
    if (g_ui_snapshot_mutex != NULL)
        xSemaphoreTake(g_ui_snapshot_mutex, portMAX_DELAY);
#endif

    *dst = sh->frame;
    *seq_out = sh->publish_seq;

#ifdef ARDUINO_ARCH_ESP32
    if (g_ui_snapshot_mutex != NULL)
        xSemaphoreGive(g_ui_snapshot_mutex);
#endif

    return true;
}

static inline bool ui_snapshot_has_newer_seq(uint32_t current_seq, uint32_t last_seen_seq)
{
    return (current_seq != last_seen_seq);
}



#ifdef __cplusplus
}
#endif

#endif /* UI_SNAPSHOT_H */
