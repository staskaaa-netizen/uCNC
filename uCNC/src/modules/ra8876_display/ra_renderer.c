/*
 * ra_renderer.c
 *
 * RA8876 renderer task and drawing helpers.
 *
 * Clean renderer version for LeanCam text-preview integration.
 */

#ifdef ARDUINO_ARCH_ESP32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#include "../../cnc.h"
#include "../ui_snapshot/ui_snapshot.h"

#include "ra8876_ll.h"
#include "ra_leancam_table.h"
#include "leancam_sim_preview.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#ifndef RA_RENDERER_UPDATE_MS
#define RA_RENDERER_UPDATE_MS 50
#endif

#ifndef RA_TOP_STATUS_UPDATE_MS
#define RA_TOP_STATUS_UPDATE_MS 250
#endif

#define UI_SCREEN_W               1024
#define UI_SCREEN_H                600

#define UI_TOP_BAR_H                40
#define UI_BOTTOM_BAR_H             40

#define UI_STATUS_MSG_X              8
#define UI_STATUS_MSG_Y              8

#define UI_AXIS_X                  700
#define UI_AXIS_Y0                  30
#define UI_AXIS_ROW_H               20

#define UI_FS_X                    820
#define UI_FS_Y0                    30

#define UI_FOOTER_X                 10
#define UI_FOOTER_Y                558

#define UI_PREVIEW_X                10
#define UI_PREVIEW_Y               100
#define UI_PREVIEW_ROW_H            30
#define UI_LC_PHYSICAL_ROWS         16

#define UI_CAM_MENU_W              300
#define UI_CAM_MENU_AREA_H         300
#define UI_CAM_MENU_BOTTOM_GAP      60
#define UI_CAM_MENU_X             (UI_SCREEN_W - UI_CAM_MENU_W)
#define UI_CAM_MENU_Y             (UI_SCREEN_H - (UI_CAM_MENU_AREA_H + UI_CAM_MENU_BOTTOM_GAP))

#define UI_CAM_MENU_GAP             10
#define UI_CAM_CELL_W             ((UI_CAM_MENU_W - (2 * UI_CAM_MENU_GAP)) / 3)
#define UI_CAM_CELL_H             ((UI_CAM_MENU_AREA_H - (2 * UI_CAM_MENU_GAP)) / 3)
#define UI_CAM_LABEL_PAD_X          10
#define UI_CAM_LABEL_PAD_Y           8

#define UI_LEANCAM_TEXT_X           10
#define UI_LEANCAM_TEXT_Y           60
#define UI_LEANCAM_TEXT_W          480
#define UI_LEANCAM_TEXT_H          480

#define UI_LEANCAM_HELPER_X         10
#define UI_LEANCAM_HELPER_Y        560
#define UI_LEANCAM_HELPER_W        850
#define UI_LEANCAM_HELPER_H        260

#define UI_LEANCAM_CLEAR_COLS       58
#define UI_LEANCAM_WIDE_COLS        90
#define UI_LEANCAM_WIDE_TEXT_W     (UI_LEANCAM_WIDE_COLS * UI_TEXT_CHAR_W)
#define UI_LEANCAM_PREVIEW_TOP     180
#define UI_LEANCAM_FILE_MARK_COLS   48

#ifndef UI_TEXT_CHAR_W
#define UI_TEXT_CHAR_W                8
#endif

#define UI_COL_BG               RA_BLACK
#define UI_COL_TOP              RA_BLUE
#define UI_COL_BOTTOM           RA_DGRAY
#define UI_COL_FRAME            RA_GRAY
#define UI_COL_TEXT             RA_WHITE
#define UI_COL_WARN             RA_YELLOW
#define UI_COL_VALUE            RA_AMBER_SOFT
#define UI_COL_CAM_FRAME        RA_GRAY
#define UI_COL_CAM_FILL         RA_BLACK
#define UI_COL_CAM_TITLE        RA_WHITE
#define UI_COL_CAM_INDEX        RA_YELLOW
#define UI_COL_CAM_LABEL        RA_AMBER_SOFT

#define UI_TEXT_CACHE_SLOTS       64
#define UI_TEXT_CACHE_LEN        129

static uint32_t g_ra_last_snapshot_seq = 0;
static bool g_ra_renderer_inited = false;
static uint8_t g_ra_last_leancam_mode = 0xff;
#ifdef ARDUINO_ARCH_ESP32
static TaskHandle_t g_ra_renderer_task = NULL;
#endif

typedef struct
{
    bool valid;
    uint16_t x;
    uint16_t y;
    uint16_t fg;
    uint16_t bg;
    char text[UI_TEXT_CACHE_LEN];
} ui_text_cache_entry_t;

static ui_text_cache_entry_t g_ui_text_cache[UI_TEXT_CACHE_SLOTS];

static void ui_text_cache_invalidate_all(void)
{
    memset(g_ui_text_cache, 0, sizeof(g_ui_text_cache));
}

static void ui_textf(int x, int y, uint16_t fg, uint16_t bg, const char *fmt, ...)
{
    char buf[129];
    va_list ap;
    ui_text_cache_entry_t *free_slot = NULL;
    ui_text_cache_entry_t *oldest = &g_ui_text_cache[0];
    int i;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    for (i = 0; i < UI_TEXT_CACHE_SLOTS; ++i)
    {
        ui_text_cache_entry_t *e = &g_ui_text_cache[i];

        if (!e->valid)
        {
            if (!free_slot)
                free_slot = e;
            continue;
        }

        if (e->x == (uint16_t)x && e->y == (uint16_t)y)
        {
            if (e->fg == fg &&
                e->bg == bg &&
                strcmp(e->text, buf) == 0)
                return;

            e->fg = fg;
            e->bg = bg;
            ui_snapshot_strcpy(e->text, buf, sizeof(e->text));
            ra_text((uint16_t)x, (uint16_t)y, fg, bg, buf);
            return;
        }
    }

    oldest = free_slot ? free_slot : oldest;
    oldest->valid = true;
    oldest->x = (uint16_t)x;
    oldest->y = (uint16_t)y;
    oldest->fg = fg;
    oldest->bg = bg;
    ui_snapshot_strcpy(oldest->text, buf, sizeof(oldest->text));

    ra_text((uint16_t)x, (uint16_t)y, fg, bg, buf);
}

static void ui_draw_static_frame(void)
{
    ui_text_cache_invalidate_all();
    ra_fill_rect(0, 0, UI_SCREEN_W - 1, UI_SCREEN_H - 1, UI_COL_BG);

    ra_fill_rect(0, 0, UI_SCREEN_W - 1, UI_TOP_BAR_H - 1, UI_COL_TOP);
    ra_fill_rect(0,
                 UI_SCREEN_H - UI_BOTTOM_BAR_H,
                 UI_SCREEN_W - 1,
                 UI_SCREEN_H - 1,
                 UI_COL_BOTTOM);

    ra_fill_rect(0, UI_TOP_BAR_H, UI_SCREEN_W - 1, UI_TOP_BAR_H, UI_COL_FRAME);
    ra_fill_rect(0,
                 UI_SCREEN_H - UI_BOTTOM_BAR_H - 1,
                 UI_SCREEN_W - 1,
                 UI_SCREEN_H - UI_BOTTOM_BAR_H - 1,
                 UI_COL_FRAME);
}

static const char *ui_exec_state_text(uint8_t state)
{
    switch (state)
    {
        case EXEC_POSITION_MAYBE_LOST:
        case EXEC_LIMITS:
            return cnc_get_exec_state(EXEC_HOMING) ? MSG_STATUS_HOME : MSG_STATUS_ALARM;

        case EXEC_HOLD:
            return MSG_STATUS_HOLD;

        case EXEC_HOMING:
            return MSG_STATUS_HOME;

        case EXEC_JOG:
            return MSG_STATUS_JOG;

        case EXEC_RUN:
            return MSG_STATUS_RUN;

        default:
            return MSG_STATUS_IDLE;
    }
}

static void ui_draw_top_status(const ui_snapshot_frame_t *frame)
{
    static char last_left[32] = {0};
    static char last_right[96] = {0};
    float x = 0.0f;
    float z = 0.0f;
    float feed = 0.0f;
    unsigned spindle = 0;
    char left[32];
    char right[96];

    if (!frame)
        return;

    if (frame->axes_valid)
    {
        x = frame->axis[0];
        z = frame->axis[2];
    }
    if (frame->feed_valid)
        feed = frame->feed;
    if (frame->spindle_valid)
        spindle = (unsigned)frame->spindle;
    if (!cnc_get_exec_state(EXEC_RUN))
    {
        feed = 0.0f;
        spindle = 0;
    }

    snprintf(left, sizeof(left), "%-10s", ui_exec_state_text(frame->state));
    snprintf(right,
             sizeof(right),
             "X:%8.3f Z:%8.3f F:%8.1f S:%6u",
             (double)x,
             (double)z,
             (double)feed,
             spindle);

    if (strcmp(last_left, left) == 0 && strcmp(last_right, right) == 0)
        return;

    ui_snapshot_strcpy(last_left, left, sizeof(last_left));
    ui_snapshot_strcpy(last_right, right, sizeof(last_right));

    ra_set_font_size(RA_FONT_MEDIUM);
    ra_fill_rect(0, 0, UI_SCREEN_W - 1, UI_TOP_BAR_H - 1, UI_COL_TOP);

    ra_text(UI_STATUS_MSG_X, UI_STATUS_MSG_Y, UI_COL_TEXT, UI_COL_TOP, left);
    ra_text(344, UI_STATUS_MSG_Y, UI_COL_TEXT, UI_COL_TOP, right);

    ra_set_font_size(RA_FONT_SMALL);
}

static void ui_draw_runtime_status(const ui_snapshot_frame_t *frame)
{
    (void)frame;
}

static int ui_lc_cols_for_y(int y)
{
    return (y < UI_LEANCAM_PREVIEW_TOP) ? UI_LEANCAM_WIDE_COLS : UI_LEANCAM_CLEAR_COLS;
}

static void ui_clear_leancam_text_area(void)
{
    ui_text_cache_invalidate_all();
    ra_fill_rect(UI_LEANCAM_TEXT_X,
                 UI_LEANCAM_TEXT_Y,
                 UI_LEANCAM_TEXT_X + UI_LEANCAM_TEXT_W - 1,
                 UI_LEANCAM_TEXT_Y + UI_LEANCAM_TEXT_H - 1,
                 UI_COL_BG);

    ra_fill_rect(UI_LEANCAM_TEXT_X,
                 UI_LEANCAM_TEXT_Y,
                 UI_LEANCAM_TEXT_X + UI_LEANCAM_WIDE_TEXT_W - 1,
                 UI_LEANCAM_PREVIEW_TOP - 1,
                 UI_COL_BG);
}

static void ui_draw_lc_value_line(int x, int y, const char *line, bool has_hi, uint8_t start, uint8_t end, int clear_cols)
{
    int len;
    char span[UI_LC_LINE_LEN];

    if (!line)
        line = "";

    len = (int)strlen(line);
    if (len > clear_cols)
        len = clear_cols;

    if (!has_hi || end <= start || start >= (uint8_t)len)
    {
        ui_textf(x, y, UI_COL_VALUE, UI_COL_BG, "%-*.*s", clear_cols, clear_cols, line);
        return;
    }

    if (end > (uint8_t)len)
        end = (uint8_t)len;

    ui_textf(x, y, UI_COL_VALUE, UI_COL_BG, "%.*s", (int)start, line);

    len = (int)(end - start);
    if (len >= (int)sizeof(span))
        len = (int)sizeof(span) - 1;
    memcpy(span, line + start, (size_t)len);
    span[len] = 0;

    ui_textf(x + ((int)start * UI_TEXT_CHAR_W),
             y,
             UI_COL_BG,
             UI_COL_WARN,
             "%s",
             span);

    ui_textf(x + ((int)end * UI_TEXT_CHAR_W),
             y,
             UI_COL_VALUE,
             UI_COL_BG,
             "%-*.*s",
             clear_cols - (int)end,
             clear_cols - (int)end,
             line + end);
}

static void ui_draw_leancam_preview(const ui_snapshot_frame_t *frame)
{
    int i;
    int y = UI_LEANCAM_TEXT_Y;
    int physical_rows = 0;

    if (!frame || !frame->leancam_active)
        return;

    if (g_ra_last_leancam_mode != frame->leancam_mode)
    {
        ui_clear_leancam_text_area();
        g_ra_last_leancam_mode = frame->leancam_mode;
    }

    for (i = 0; i < frame->leancam_line_count && i < UI_LC_MAX_LINES; ++i)
    {
        static ra_lc_table_line_t tl;
        uint16_t fg;
        uint16_t bg;

        if (physical_rows >= UI_LC_PHYSICAL_ROWS)
            break;

        ra_lc_build_table_line(frame, i, &tl);

        fg = frame->leancam_line_selected[i] ? UI_COL_WARN : UI_COL_TEXT;
        bg = UI_COL_BG;

        if (tl.table_like && physical_rows + 1 < UI_LC_PHYSICAL_ROWS)
        {
            int cols = ui_lc_cols_for_y(y);

            ui_textf(UI_LEANCAM_TEXT_X, y, fg, bg, "%-*.*s", cols, cols, tl.header);

            y += UI_PREVIEW_ROW_H;
            physical_rows++;

            cols = ui_lc_cols_for_y(y);
            ui_draw_lc_value_line(UI_LEANCAM_TEXT_X, y, tl.value, tl.has_hi, tl.hi_start, tl.hi_end, cols);

            y += UI_PREVIEW_ROW_H;
            physical_rows++;
        }
        else
        {
            if (frame->leancam_line_selected[i])
            {
                if (frame->leancam_mode == 0 || frame->leancam_mode == 1)
                {
                    ui_textf(UI_LEANCAM_TEXT_X,
                             y,
                             UI_COL_BG,
                             UI_COL_WARN,
                             "%-*.*s",
                             UI_LEANCAM_FILE_MARK_COLS,
                             UI_LEANCAM_FILE_MARK_COLS,
                             frame->leancam_lines[i]);
                }
                else
                {
                    int cols = ui_lc_cols_for_y(y);
                    ui_textf(UI_LEANCAM_TEXT_X,
                             y,
                             UI_COL_BG,
                             UI_COL_WARN,
                             "%-*.*s",
                             cols,
                             cols,
                             frame->leancam_lines[i]);
                }
            }
            else
            {
                int cols = ui_lc_cols_for_y(y);
                ui_textf(UI_LEANCAM_TEXT_X,
                         y,
                         UI_COL_TEXT,
                         UI_COL_BG,
                         "%-*.*s",
                         cols,
                         cols,
                         frame->leancam_lines[i]);
            }

            y += UI_PREVIEW_ROW_H;
            physical_rows++;
        }
    }

    while (physical_rows < UI_LC_PHYSICAL_ROWS)
    {
        int cols = ui_lc_cols_for_y(y);
        ui_textf(UI_LEANCAM_TEXT_X, y, UI_COL_TEXT, UI_COL_BG, "%-*.*s", cols, cols, "");
        y += UI_PREVIEW_ROW_H;
        physical_rows++;
    }

    ui_textf(UI_LEANCAM_HELPER_X,
             UI_LEANCAM_HELPER_Y,
             UI_COL_TEXT,
             UI_COL_BOTTOM,
             "%-120.120s",
             frame->leancam_helper);

    leancam_sim_preview_draw(frame);

    /*if (frame->leancam_mode == 0)
    {
        ra_fill_rect(520, 170, 819, 469, 0xF81F);
        ra_fill_rect(840, 170, 1023, 529, 0x07E0);
    }*/
}

static void ra_renderer_draw_frame(const ui_snapshot_frame_t *frame)
{
    static bool live_sim_visible = false;
    bool live_run_state;

    if (!frame)
        return;

    live_run_state = cnc_get_exec_state(EXEC_RUN) ||
                     (live_sim_visible && cnc_get_exec_state(EXEC_HOLD));

    if (frame->leancam_active && live_run_state)
    {
        if (!live_sim_visible)
            ra_set_font_size(RA_FONT_MEDIUM);

        leancam_sim_live_draw(frame);
        live_sim_visible = true;
        return;
    }

    if (live_sim_visible)
    {
        ra_set_font_size(RA_FONT_SMALL);
        ui_draw_static_frame();
        g_ra_last_leancam_mode = 0xff;
        live_sim_visible = false;
    }

    ui_draw_top_status(frame);
    ui_draw_runtime_status(frame);
    ui_draw_leancam_preview(frame);
}

static void ra_renderer_hw_init(void)
{
    ra_init();

    g_ra_last_snapshot_seq = 0;
    g_ra_last_leancam_mode = 0xff;
    g_ra_renderer_inited = true;

    ui_draw_static_frame();

   // ra_text(20, UI_TOP_BAR_H + 10, UI_COL_WARN, UI_COL_BG, "waiting for first snapshot...");
}

void ra_renderer_poll(void)
{
    static ui_snapshot_frame_t local_frame;
    uint32_t seq = 0;

    if (!g_ra_renderer_inited)
        return;

    if (!ui_snapshot_copy_latest(&g_ui_snapshot, &local_frame, &seq))
        return;

    if (!ui_snapshot_has_newer_seq(seq, g_ra_last_snapshot_seq))
        return;

    ra_renderer_draw_frame(&local_frame);
    g_ra_last_snapshot_seq = seq;
}

#ifdef ARDUINO_ARCH_ESP32
static void ra_renderer_task(void *arg)
{
    uint32_t last_ms = 0;
    uint32_t now;

    (void)arg;
     vTaskDelay(2000);
    ra_renderer_hw_init();
    /* Core0 renderer init only. LeanCam/keypad live on core1 snapshot builder. */

    while (1)
    {
        now = mcu_millis();

        if ((uint32_t)(now - last_ms) >= RA_RENDERER_UPDATE_MS)
        {
            last_ms = now;
            ra_renderer_poll();
        }

        vTaskDelay(50);
    }
}
#endif

DECL_MODULE(ra_renderer)
{
#ifdef ARDUINO_ARCH_ESP32
    if (g_ra_renderer_task == NULL)
    {
        xTaskCreatePinnedToCore(
            ra_renderer_task,
            "ra_ui",
            4096,
            NULL,
            1,
            &g_ra_renderer_task,
            0
        );
    }
#endif
}
