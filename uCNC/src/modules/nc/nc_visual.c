#include "nc_visual.h"

#include "../../cnc.h"
#include "../../interface/grbl_stream.h"
#include "nc.h"
#include "nc_emit.h"
#include "nc_feedback.h"
#include "nc_files.h"
#include "nc_menu.h"
#include "nc_palette.h"
#include "nc_presets.h"
#include "nc_run.h"
#include "nc_preview.h"
#include "nc_state.h"
#include "nc_text.h"
#include "nc_tools.h"
#include "nc_vocab.h"
#include "../g7x/g7x_contour.h"
#include "../g7_g8/parser_g7_g8.h"
#include "nc_g7x.h"
#include "../lvds_renderer/lvds_draw_api.h"
#include "../lvds_renderer/lvds_hstx.h"
#if __has_include("../lvds_renderer/lvds_psram.h")
#include "../lvds_renderer/lvds_psram.h"
#define NC_VISUAL_HAVE_PSRAM 1
#else
#define NC_VISUAL_HAVE_PSRAM 0
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#define NC_PREVIEW_ARC_MAX_STEPS 64
#define NC_PREVIEW_CONTOUR_MAX 48
#ifndef NC_PREVIEW_DIN_STYLE
#define NC_PREVIEW_DIN_STYLE 1
#endif
#ifndef NC_PREVIEW_DIN_POINT_MARKERS
#define NC_PREVIEW_DIN_POINT_MARKERS 1
#endif
#define NC_LIVE_STOCK_MAX_W 680
#define NC_LIVE_STOCK_MAX_H 380
#define NC_LIVE_STOCK_PSRAM_OFFSET (512u * 1024u)
/* Room above the stock inside the preview pane. The caption that used to sit
   there is gone (the tab strip names the screen, the header names the file), so
   this is only the space the top ruler and its labels need; the rest of the
   pane belongs to the drawing. */
#define NC_PREVIEW_TOP_BAND 82
/* The live tool marker's size, in the pane it must stay inside. */
#define NC_LIVE_TOOL_GLYPH 20

#define NC_VISUAL_CHAR_W   8
#define NC_VISUAL_ROW_H    24
#define NC_LEFT_PANE_X     10
#define NC_LEFT_PANE_W     350
#define NC_SPLIT_X         366
#define NC_RIGHT_PANE_X    374
#define NC_RIGHT_PANE_W    410
#define NC_LINE_NO_X_PAD   4
#define NC_LINE_TEXT_X_PAD 32

/* Screen tabs across the very top: the screen name lives here and nowhere
   else, so the header below it is free to carry state and file. */
#define NC_TAB_Y           0
#define NC_TAB_H           24
#define NC_HEADER_Y        (NC_TAB_Y + NC_TAB_H)
#define NC_HEADER_H        68
/* Code/graphic pane. The tab strip is paid for by the pane caption that used
   to repeat the file name above the code rows. */
#define NC_PANE_ROWS       18
#define NC_PANE_Y          (NC_HEADER_Y + NC_HEADER_H + 4)
#define NC_PANE_H          (NC_PANE_ROWS * NC_VISUAL_ROW_H)
#define NC_PANE_BOTTOM     (NC_PANE_Y + NC_PANE_H)
/* First row of the pane is the file the code belongs to; the code rows follow
   it and are one fewer than the pane's row units. */
#define NC_CODE_Y          (NC_PANE_Y + NC_VISUAL_ROW_H)
#define NC_FOOTER_Y        (NC_PANE_BOTTOM + 14)
#define NC_FOOTER_H        (LVDS_HSTX_HEIGHT - NC_FOOTER_Y)
/* Footer keys carry a short label; three lines is the whole button height. */
#define NC_FOOTER_LINES    3
/* The strip is the same eight slots on every screen: the keys are drawn at the
   same places and the same width whatever screen is up, and a screen with
   fewer entries leaves the rest empty rather than stretching its keys. */
#define NC_FOOTER_SLOTS    8
/* EDIT's full-screen preview (`# FULL`): the whole body, with a small margin
   from the panel edges. */
#define NC_FULL_PREVIEW_X  20
#define NC_FULL_PREVIEW_W  (LVDS_HSTX_WIDTH - 40)

/* MANUAL readout row: the axis letter, its position in the offset in use, the
   stop and the machine figure the offset is cut from - three numbers on the
   line, one row per axis. */
#define NC_MANUAL_ROW_H    62
#define NC_MANUAL_COL_X    40
#define NC_MANUAL_COL_POS  76
#define NC_MANUAL_COL_STOP 250
#define NC_MANUAL_COL_MACH 340

/* Floating 3x3 helper: the nine keys and nothing else. No panel box and no
   title strip - the editor line above it carries the label - so the keys read
   like the footer's own buttons. */
#define NC_MODAL_ROWS  3
#define NC_MODAL_COLS  3
#define NC_MODAL_KEY_W 72
#define NC_MODAL_KEY_H 72
#define NC_MODAL_PAD   2
#define NC_MODAL_W     (NC_MODAL_KEY_W * NC_MODAL_COLS + NC_MODAL_PAD * 2)
#define NC_MODAL_H     (NC_MODAL_KEY_H * NC_MODAL_ROWS + NC_MODAL_PAD * 2)
/* Glyph height of the bitmap font the helper labels use. */
#define NC_FONT_NORMAL_H 14
static bool g_nc_visual_in_draw;
static nc_document_t g_nc_visual_doc;
static char g_nc_visual_status[64];
static bool g_nc_visual_dirty;
static char g_nc_visual_command_error[96];
static uint32_t g_nc_visual_notice_signature;

static uint8_t nc_visual_settings_error(void)
{
#ifndef DISABLE_SAFE_SETTINGS
    return g_settings_error;
#else
    return 0;
#endif
}

static bool nc_visual_parse_error(void *args)
{
    uint8_t error = *(uint8_t *)args;
    if (nc_run_error())
        snprintf(g_nc_visual_command_error, sizeof(g_nc_visual_command_error),
                 "Line %lu error %u: %s", (unsigned long)(nc_run_error_line() + 1),
                 error, nc_feedback_error(error));
    else
        snprintf(g_nc_visual_command_error, sizeof(g_nc_visual_command_error),
                 "Error %u: %s", error, nc_feedback_error(error));
    g_nc_visual_dirty = true;
    return EVENT_CONTINUE;
}
CREATE_EVENT_LISTENER(cnc_parse_cmd_error, nc_visual_parse_error);

static bool nc_visual_parser_reset(void *args)
{
    (void)args;
    g_nc_visual_command_error[0] = '\0';
    g_nc_visual_dirty = true;
    return EVENT_CONTINUE;
}
CREATE_EVENT_LISTENER(parser_reset, nc_visual_parser_reset);
/* Preview view options: whole stock or its outline, the path and the roughing
   pass, and the dimension layer. They are EDIT's, used while the preview has
   the whole body. */
static bool g_nc_visual_show_stock = true;
static bool g_nc_visual_show_path = true;
static bool g_nc_visual_show_rough = true;
static bool g_nc_visual_show_dims = true;
/* EDIT's code pane; hidden (`# FULL`), the preview has the whole body. */
static bool g_nc_visual_show_code = true;
static uint16_t g_nc_visual_fps;
static uint16_t g_nc_visual_fps_frames;
static uint32_t g_nc_visual_fps_last_ms;
static uint16_t g_nc_visual_stat_snapshot_ms;
static uint16_t g_nc_visual_stat_draw_ms;
static uint16_t g_nc_visual_stat_present_ms;
static uint16_t g_nc_visual_stat_total_ms;
static uint16_t g_nc_visual_stat_header_ms;
static uint16_t g_nc_visual_stat_preview_ms;
static uint16_t g_nc_visual_stat_body_ms;
static uint16_t g_nc_visual_stat_footer_ms;
static uint16_t g_nc_visual_stat_preview_collect_ms;
static uint16_t g_nc_visual_stat_preview_clear_ms;
static uint16_t g_nc_visual_stat_preview_stock_ms;
static uint16_t g_nc_visual_stat_preview_geom_ms;
static uint16_t g_nc_visual_stat_preview_tool_ms;
static uint32_t g_nc_visual_acc_snapshot_us;
static uint32_t g_nc_visual_acc_draw_us;
static uint32_t g_nc_visual_acc_present_us;
static uint32_t g_nc_visual_acc_total_us;
static uint32_t g_nc_visual_acc_header_us;
static uint32_t g_nc_visual_acc_preview_us;
static uint32_t g_nc_visual_acc_body_us;
static uint32_t g_nc_visual_acc_footer_us;
static uint32_t g_nc_visual_acc_preview_collect_us;
static uint32_t g_nc_visual_acc_preview_clear_us;
static uint32_t g_nc_visual_acc_preview_stock_us;
static uint32_t g_nc_visual_acc_preview_geom_us;
static uint32_t g_nc_visual_acc_preview_tool_us;
static uint32_t g_nc_visual_frame_header_us;
static uint32_t g_nc_visual_frame_preview_us;
static uint32_t g_nc_visual_frame_body_us;
static uint32_t g_nc_visual_frame_footer_us;
static uint32_t g_nc_visual_frame_preview_collect_us;
static uint32_t g_nc_visual_frame_preview_clear_us;
static uint32_t g_nc_visual_frame_preview_stock_us;
static uint32_t g_nc_visual_frame_preview_geom_us;
static uint32_t g_nc_visual_frame_preview_tool_us;
static size_t g_nc_visual_last_draw_run_line = (size_t)-1;
static bool g_nc_visual_last_runtime_busy;
static nc_text_edit_t g_nc_visual_edit;
static bool g_nc_visual_new_file_active;
static char g_nc_visual_new_file_name[NC_FILE_NAME_MAX];
static uint8_t *g_nc_live_stock_mask;
static bool g_nc_live_stock_ready;
static bool g_nc_live_stock_was_active;
static bool g_nc_live_stock_has_last;
static int g_nc_live_stock_w;
static int g_nc_live_stock_h;
static float g_nc_live_stock_setup_x;
static float g_nc_live_stock_setup_z;
static float g_nc_live_stock_setup_i;
static float g_nc_live_stock_last_x;
static float g_nc_live_stock_last_z;
static bool g_nc_live_tool_rect_valid;
static int g_nc_live_tool_rect_x;
static int g_nc_live_tool_rect_y;
static int g_nc_live_tool_rect_w;
static int g_nc_live_tool_rect_h;
static bool g_nc_live_preview_cache_valid;
static nc_preview_info_t g_nc_live_preview_cache;
static nc_tool_t g_nc_live_tool_cache;
static bool g_nc_live_have_tool_cache;
static int g_nc_live_stock_w_cache;
static int g_nc_live_stock_h_cache;
static int g_nc_live_stock_left_cache;
static int g_nc_live_stock_top_cache;
static int g_nc_live_z0_x_cache;

typedef enum {
    NC_PREVIEW_SEG_FEED = 0,
    NC_PREVIEW_SEG_ROUGH,
    NC_PREVIEW_SEG_FINISH
} nc_preview_segment_t;

static void nc_visual_draw_text_clip(int x,
                                     int y,
                                     const char *text,
                                     int cols,
                                     lvds_color_t fg,
                                     lvds_color_t bg,
                                     int font);
static void nc_visual_serial_selected_line(void);
static void nc_visual_serial_selected_file(void);
static void nc_files_preview_sync(void);
static size_t nc_visual_code_line(void);
static bool nc_visual_can_edit_code(void);
static const char *nc_visual_file_basename(const char *path);
static void nc_visual_dispatch_footer_action(uint8_t action);
static void nc_visual_manual_feed_cancel(void);
static bool nc_visual_full_preview(void);

static void nc_visual_fps_tick(uint32_t snapshot_us,
                               uint32_t draw_us,
                               uint32_t present_us,
                               uint32_t total_us);
static void nc_visual_draw_tool_glyph(int tip_x,
                                      int tip_y,
                                      int size,
                                      const nc_tool_t *tool,
                                      lvds_color_t bg,
                                      bool selected);

static nc_mode_t g_nc_visual_mode = NC_MODE_MANUAL;
static uint8_t g_nc_visual_selected_action = NC_FOOTER_ACTION_NONE;
static bool g_nc_modal_active;
static nc_footer_action_t g_nc_modal_parent;
static const nc_footer_item_t *g_nc_modal_items;
static size_t g_nc_modal_count;
static const char *g_nc_modal_title;
static char g_nc_gcode_buf[8];
static size_t g_nc_modal_line;
static char g_nc_modal_prefix;
/* The helper is anchored to a line of its own, inserted under the cursor: the
   line the choice will fill. Remember where it came from so cancelling puts
   the cursor back and choosing writes exactly where the line stood. */
static size_t g_nc_modal_origin_line;
static const char *g_nc_modal_label;
/* The editor's file-name row above line 1 can hold the cursor. */
static bool g_nc_visual_name_selected;
/* Screen idle time is tracked so the state store is written when the operator
   stops moving, never in the middle of a screen change. */
#define NC_VISUAL_IDLE_FLUSH_MS 400u
static uint32_t g_nc_visual_last_input_ms;

static const char *nc_visual_tool_path(void)
{
    const char *path = nc_state_path(NC_MODE_TOOLS);

    return (path && path[0] && nc_state_tool_path_supported(path)) ? path : NC_TOOL_PATH;
}

/* The footer of what the screen is showing: EDIT's own, or the preview's while
   the code is hidden and the preview has the whole body. The key lookup and the
   strip have to agree, or a key would be labelled one thing and do another. */
static const nc_footer_item_t *nc_visual_footer_items(size_t *count)
{
    if (nc_visual_full_preview()) {
        return nc_menu_preview_footer(count);
    }
    return nc_menu_footer(g_nc_visual_mode, nc_files_active(), count);
}

/* One place for the two guards the MANUAL jog and RUN share, so a screen cannot
   answer "wait for stop" while another quietly queues the same move. Both say
   what they mean instead of letting the controller answer with a code. */
static bool nc_visual_axis_standing(const char *status)
{
    if (cnc_get_exec_state(EXEC_JOG | EXEC_RUN)) {
        strncpy(g_nc_visual_status, status, sizeof(g_nc_visual_status) - 1);
        g_nc_visual_dirty = true;
        return false;
    }
    return true;
}

static bool nc_visual_document_is_program(void)
{
    if (g_nc_visual_doc.path[0] && !nc_path_supported(g_nc_visual_doc.path)) {
        strncpy(g_nc_visual_status, "Not a program file", sizeof(g_nc_visual_status) - 1);
        return false;
    }
    return true;
}

static void nc_visual_set_mode(nc_mode_t mode)
{
    if (mode < 0 || mode >= NC_MODE_COUNT) {
        return;
    }
    /* Leaving MANUAL must not leave a jog running behind the next screen. */
    nc_visual_manual_feed_cancel();
    g_nc_visual_mode = mode;
    nc_state_set_mode(mode);
    nc_state_save();
}

/* What the operator was trying to do when an autosave failed, so the same
   action twice can mean "go on and drop the edits". A failed card write must
   not wall the machine off: the first press reports, the second insists. */
enum {
    NC_VISUAL_UNSAVED_NONE = 0,
    NC_VISUAL_UNSAVED_MODE,
    NC_VISUAL_UNSAVED_OPEN,
    NC_VISUAL_UNSAVED_NEW
};
static uint8_t g_nc_visual_unsaved_arm;

/* Called whenever the buffer is about to be reused - a screen change, opening
   another file, creating one. The edits go to the card first. If that write
   fails the caller must not go on: the document is still in memory and is the
   only copy, so it stays open and the operator sees why. */
static bool nc_visual_save_current_if_file(void)
{
    if (g_nc_visual_doc.dirty && nc_path_supported(g_nc_visual_doc.path) &&
        nc_save_file(&g_nc_visual_doc, g_nc_visual_doc.path) != NC_OK) {
        return false;
    }
    g_nc_visual_unsaved_arm = 0;
    if (nc_path_supported(g_nc_visual_doc.path)) {
        nc_state_remember_path(g_nc_visual_mode, g_nc_visual_doc.path);
        nc_state_remember_cursor(&g_nc_visual_doc);
        nc_state_save();
    }
    return true;
}

static bool nc_visual_proceed_without_saving(uint8_t kind)
{
    if (g_nc_visual_unsaved_arm == kind) {
        g_nc_visual_unsaved_arm = 0;
        return true;
    }
    g_nc_visual_unsaved_arm = kind;
    return false;
}

/* --- MANUAL: a readout with the axis the keys act on, and the jog keys -----

   B/C pick the axis, the 3x3 digits jog that axis, run the feed override and
   the spindle. Zero and touch-off write the work offset for the picked axis,
   which is what "set it to zero" means to the machine. */
static uint8_t g_nc_visual_manual_axis;                 /* 0 = X, 1 = Z */
static char g_nc_visual_manual_touch[16];
static bool g_nc_visual_manual_touch_active;

#define NC_MANUAL_SPINDLE_RPM  1000u

/* The two values the jog keys use: the step one keypress moves, and the feed
   the continuous jog runs at. `1` and `3` change the one the feed mode is
   using, and the readout beside the pad shows both, so what the keys change is
   always on screen. The screen owns them: a jog block and the readout beside
   it cannot disagree. */
static const float g_nc_visual_manual_steps[] = {
    0.010f, 0.025f, 0.050f, 0.100f, 0.250f, 0.500f, 1.000f
};
static const float g_nc_visual_manual_feeds[] = {
    50.0f, 100.0f, 200.0f, 300.0f, 500.0f, 800.0f, 1000.0f, 2000.0f
};
#define NC_MANUAL_STEP_DEFAULT_INDEX 3u     /* 0.100 mm per press */
#define NC_MANUAL_FEED_DEFAULT_INDEX 4u     /* 500 mm/min */
static uint8_t g_nc_visual_manual_step_index = NC_MANUAL_STEP_DEFAULT_INDEX;
static uint8_t g_nc_visual_manual_feed_index = NC_MANUAL_FEED_DEFAULT_INDEX;

static float nc_visual_manual_step_value(void)
{
    return g_nc_visual_manual_steps[g_nc_visual_manual_step_index];
}

static float nc_visual_manual_feed_value(void)
{
    return g_nc_visual_manual_feeds[g_nc_visual_manual_feed_index];
}

/* Continuous feed and the stop position. The stop is what keeps the axis out
   of the chuck: it is set at the current point of the picked axis and a jog
   never crosses it. Machine coordinates, the same figures the machine column
   shows, so an offset change cannot move the limit. */
static bool g_nc_visual_manual_continuous;
static bool g_nc_visual_manual_stop_set[2];
static float g_nc_visual_manual_stop[2];
static char g_nc_visual_manual_spindle_dir;      /* 0 off, '3' CW, '4' CCW */
/* A jog is over as soon as it is sent, so the key that sent it is held lit for
   a moment: the pad has to show which key the machine just acted on. */
#define NC_MANUAL_FLASH_MS 250u
static char g_nc_visual_manual_flash_key;
static uint32_t g_nc_visual_manual_flash_ms;

/* The feed jog in flight: the key that started it, 0 when there is none. The
   controller owns the motion (a `$J=` block covering the whole distance to the
   stop), so the panel only has to notice the key coming up, Stop, an alarm or
   the wall - which is the block's own end. */
#define NC_MANUAL_FEED_MIN_MM  0.005
static char g_nc_visual_manual_feed_key;
static char g_nc_visual_manual_held_key;
/* Which side of the stop the axis works on: +1 above it, -1 below, 0 while it
   is not known yet (the stop was just armed at the axis). */
static int8_t g_nc_visual_manual_stop_side[2];

/* A feed with no wall in front of it has nothing to end on, so it is bounded:
   the axis travel the machine states ($130), or this cap when it states none.
   A key release the panel never sees can then not run the axis to the end of
   the machine; letting go and holding again goes further. */
#define NC_MANUAL_FEED_CAP_MM  25.0

/* What the digits mean here. The panel draws this as the pad in the pane, and
   a shell beside the machine labels its keypad from the same table - the key
   meanings are stated once, where the screen that acts on them lives. */
static const nc_footer_item_t g_nc_visual_manual_pad[] = {
    { '7', "CCW", NC_FOOTER_ACTION_NONE },
    { '8', "X+", NC_FOOTER_ACTION_NONE },
    { '9', "CW", NC_FOOTER_ACTION_NONE },
    { '4', "Z-", NC_FOOTER_ACTION_NONE },
    { '5', "STOP", NC_FOOTER_ACTION_NONE },
    { '6', "Z+", NC_FOOTER_ACTION_NONE },
    { '1', "FD-", NC_FOOTER_ACTION_NONE },
    { '2', "X-", NC_FOOTER_ACTION_NONE },
    { '3', "FD+", NC_FOOTER_ACTION_NONE }
};

static float nc_visual_manual_machine_value(void)
{
    nc_runtime_state_t rt;

    nc_state_runtime(&rt);
    return g_nc_visual_manual_axis ? rt.z : rt.x;
}

static char nc_visual_manual_letter(void)
{
    return g_nc_visual_manual_axis ? 'Z' : 'X';
}

/* The work coordinate offset the parser is applying. parser_get_wco() reports
   at a limited rate and leaves the array untouched when it declines, so the
   last answer is kept: a caption must not flicker between two meanings. */
static bool nc_visual_wco(float *out)
{
    static float cached[AXIS_COUNT];
    static bool valid;
    float wco[AXIS_COUNT];

    if (parser_get_wco(wco)) {
        memcpy(cached, wco, sizeof(cached));
        valid = true;
    }
    if (!valid) {
        return false;
    }
    memcpy(out, cached, sizeof(cached));
    return true;
}

/* The offset the work column is derived with - the work coordinate offset the
   parser is applying - shown over the machine column so the difference between
   the two columns has a name. */
static void nc_visual_offset_label(char *out, size_t out_sz)
{
    float wco[AXIS_COUNT] = {0};

    if (!nc_visual_wco(wco)) {
        snprintf(out, out_sz, "MACHINE");
        return;
    }
    snprintf(out, out_sz, "OFF X%.2f Z%.2f",
             (double)wco[AXIS_X], (double)wco[AXIS_Z]);
}

static void nc_visual_manual_status(const char *what)
{
    snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "%s %c", what,
             nc_visual_manual_letter());
    g_nc_visual_dirty = true;
}

/* The panel works in axis millimetres: the readout, the stop and the room left
   are all the machine figure of the axis. A word in a program is not always the
   same length - the lathe programs X as a diameter (G7, the parser's default),
   so a programmed X of 1 mm moves the axis 0.5 mm. A jog asked for in axis
   millimetres has to be written that much larger, or every X jog would stop
   half way to what the readout calls the position. Z has no such scaling. */
static double nc_visual_manual_program_length(uint8_t axis, double length)
{
    if (axis == AXIS_X && g7_g8_is_diameter_mode()) {
        return length * 2.0;
    }
    return length;
}

/* How far a feed may go when the stop is not in front of it. */
static double nc_visual_manual_feed_limit(uint8_t axis)
{
    double travel = (double)g_settings.max_distance[axis];

    return (travel > 1.0) ? travel : NC_MANUAL_FEED_CAP_MM;
}

/* Is this move headed into the wall? The wall is a statement about the *move*,
   not about the sign of the distance: standing exactly on the stop leaves the
   operator free to go either way, and the way they go is the side they work
   on. Remember it, so the panel only guards the direction that would cross -
   and so a step into the wall from on it is refused instead of walking through
   it, while the axis is never trapped on the stop. `dist` is stop - here. */
static bool nc_visual_manual_toward_stop(uint8_t axis, double dist, int direction)
{
    int8_t side = g_nc_visual_manual_stop_side[axis];

    if (dist > NC_MANUAL_FEED_MIN_MM) {
        side = -1;                      /* the axis is below the stop */
    } else if (dist < -NC_MANUAL_FEED_MIN_MM) {
        side = 1;                       /* above it */
    }
    g_nc_visual_manual_stop_side[axis] = side;
    return side != 0 && direction == -side;
}

/* Panel blocks are refused while RUN holds the reader: a jog lands between two
   program blocks and the G90 it leaves behind would cut into the cycle. */
static bool nc_visual_manual_send(const char *line)
{
    if (nc_run_streaming()) {
        strncpy(g_nc_visual_status, "Program running", sizeof(g_nc_visual_status) - 1);
        g_nc_visual_dirty = true;
        return false;
    }
    /* A running jog locks the parser: anything else queued behind it is
       discarded without an answer, so the panel refuses it instead. */
    if (cnc_get_exec_state(EXEC_JOG)) {
        strncpy(g_nc_visual_status, "Jog busy", sizeof(g_nc_visual_status) - 1);
        g_nc_visual_dirty = true;
        return false;
    }
    if (cnc_get_exec_state(EXEC_JOG_LOCKED) || cnc_has_alarm()) {
        /* Alarm or door: the controller is not taking motion. */
        strncpy(g_nc_visual_status, "Controller locked", sizeof(g_nc_visual_status) - 1);
        g_nc_visual_dirty = true;
        return false;
    }
    if (!nc_run_send_line(line)) {
        strncpy(g_nc_visual_status, "Send failed", sizeof(g_nc_visual_status) - 1);
        g_nc_visual_dirty = true;
        return false;
    }
    return true;
}

/* A feed ends where it stands: the jog is cancelled with a controlled stop, so
   the axis keeps the position it has reached instead of running to the wall. */
static void nc_visual_manual_feed_cancel(void)
{
    if (!g_nc_visual_manual_feed_key) {
        return;
    }
    g_nc_visual_manual_feed_key = 0;
    cnc_call_rt_command(CMD_CODE_JOG_CANCEL);
    strncpy(g_nc_visual_status, "Feed stop", sizeof(g_nc_visual_status) - 1);
    g_nc_visual_dirty = true;
}

/* A held direction key feeds. Toward the stop the block is the whole distance
   to the wall, so the jog ends on it by itself; away from it (or with no stop
   set) the feed is a bounded move, because nothing is there to end it. Letting
   the key go cancels either one where it stands. The distances are measured
   from a standing axis, so none of it depends on a position read taken while
   the machine is moving. */
static void nc_visual_manual_feed(char key, int direction)
{
    uint8_t axis = g_nc_visual_manual_axis;
    bool to_stop = false;
    double move;
    char line[48];

    /* The parser takes a jog only in IDLE, and the distance to the wall is
       measured from a position: an axis that is still moving has neither. */
    if (!nc_visual_axis_standing("Wait for stop")) {
        return;
    }
    if (g_nc_visual_manual_stop_set[axis]) {
        double dist = (double)g_nc_visual_manual_stop[axis] -
                      (double)nc_visual_manual_machine_value();

        to_stop = nc_visual_manual_toward_stop(axis, dist, direction);
        if (to_stop) {
            if (dist * (double)direction < NC_MANUAL_FEED_MIN_MM) {
                strncpy(g_nc_visual_status, "At stop", sizeof(g_nc_visual_status) - 1);
                g_nc_visual_dirty = true;
                return;
            }
            move = dist;                /* the block ends on the wall */
        } else {
            move = nc_visual_manual_feed_limit(axis) * (double)direction;
        }
    } else {
        move = nc_visual_manual_feed_limit(axis) * (double)direction;
    }
    /* `$J=` is the controller's jog: it runs the block, and a jog cancel from
       the key release stops it without touching the modal state. */
    snprintf(line, sizeof(line), "$J=G91 %c%.3f F%.0f",
             nc_visual_manual_letter(),
             nc_visual_manual_program_length(axis, move),
             (double)nc_visual_manual_feed_value());
    if (!nc_visual_manual_send(line)) {
        return;
    }
    g_nc_visual_manual_feed_key = key;
    if (to_stop) {
        snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Feed to %.3f",
                 (double)g_nc_visual_manual_stop[axis]);
    } else {
        snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Feed %.3f mm",
                 move < 0.0 ? -move : move);
    }
    g_nc_visual_dirty = true;
}

static void nc_visual_manual_jog(char key, int direction)
{
    char line[48];
    char sent[48];
    double step = (double)nc_visual_manual_step_value();
    uint8_t axis = g_nc_visual_manual_axis;

    if (g_nc_visual_manual_continuous) {
        nc_visual_manual_feed(key, direction);
        return;
    }
    nc_visual_manual_feed_cancel();     /* step mode never leaves a feed running */
    if (!nc_visual_axis_standing("Jog busy")) {
        return;
    }
    /* The stop is a wall the axis may not cross: a step into it is shortened to
       end on it. A step away is free, and standing exactly on the wall is not a
       lock - '*' at the current point is the normal way to set it, so the axis
       has to be able to leave. The first way it leaves is the side it works on
       (see nc_visual_manual_toward_stop). */
    if (g_nc_visual_manual_stop_set[axis]) {
        double here = (double)nc_visual_manual_machine_value();
        double dist = (double)g_nc_visual_manual_stop[axis] - here;

        if (nc_visual_manual_toward_stop(axis, dist, direction)) {
            double room = dist * (double)direction;

            step = MIN(step, room);
            if (step < 0.0005) {
                strncpy(g_nc_visual_status, "At stop", sizeof(g_nc_visual_status) - 1);
                g_nc_visual_dirty = true;
                return;
            }
        }
    }
    /* Incremental, and back to absolute in the same breath: a jog must not
       leave the machine in G91 for whatever runs next. Both blocks are queued,
       so the pairing survives the trip through the reader. */
    snprintf(line, sizeof(line), "G91 G1 %c%.3f F%.0f",
             nc_visual_manual_letter(),
             nc_visual_manual_program_length(axis, step * (double)direction),
             (double)nc_visual_manual_feed_value());
    if (!nc_visual_manual_send(line)) {
        return;
    }
    snprintf(sent, sizeof(sent), "%s", line);
    nc_visual_manual_send("G90");
    snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "%s", sent);
    g_nc_visual_dirty = true;
}

/* '#' swaps a fixed step for feeding to the stop while the key is held. */
static void nc_visual_manual_step_toggle(void)
{
    nc_visual_manual_feed_cancel();
    g_nc_visual_manual_continuous = !g_nc_visual_manual_continuous;
    strncpy(g_nc_visual_status,
            g_nc_visual_manual_continuous ? "Continuous feed" : "Step jog",
            sizeof(g_nc_visual_status) - 1);
    g_nc_visual_dirty = true;
}

/* '1' and '3' change the value the feed mode is using: the step per press, or
   the feed the continuous jog runs at. Both ends of each table are the end of
   the range - the key is not a mode, it just stops. */
static void nc_visual_manual_value_adjust(int direction)
{
    if (g_nc_visual_manual_continuous) {
        if (direction < 0) {
            if (g_nc_visual_manual_feed_index > 0u) {
                g_nc_visual_manual_feed_index--;
            }
        } else if (g_nc_visual_manual_feed_index + 1u <
                   sizeof(g_nc_visual_manual_feeds) / sizeof(g_nc_visual_manual_feeds[0])) {
            g_nc_visual_manual_feed_index++;
        }
        snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Feed %.0f mm/min",
                 (double)nc_visual_manual_feed_value());
    } else {
        if (direction < 0) {
            if (g_nc_visual_manual_step_index > 0u) {
                g_nc_visual_manual_step_index--;
            }
        } else if (g_nc_visual_manual_step_index + 1u <
                   sizeof(g_nc_visual_manual_steps) / sizeof(g_nc_visual_manual_steps[0])) {
            g_nc_visual_manual_step_index++;
        }
        snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Step %.3f mm",
                 (double)nc_visual_manual_step_value());
    }
    g_nc_visual_dirty = true;
}

/* '*' arms (or clears) the stop position for the picked axis, here. */
static void nc_visual_manual_stop_toggle(void)
{
    uint8_t axis = g_nc_visual_manual_axis;

    if (g_nc_visual_manual_stop_set[axis]) {
        g_nc_visual_manual_stop_set[axis] = false;
        g_nc_visual_manual_stop_side[axis] = 0;
        strncpy(g_nc_visual_status, "Stop cleared", sizeof(g_nc_visual_status) - 1);
    } else {
        g_nc_visual_manual_stop[axis] = nc_visual_manual_machine_value();
        g_nc_visual_manual_stop_set[axis] = true;
        g_nc_visual_manual_stop_side[axis] = 0;     /* the side is not known yet */
        snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Stop %c at %.3f",
                 nc_visual_manual_letter(), (double)g_nc_visual_manual_stop[axis]);
    }
    g_nc_visual_dirty = true;
}

static void nc_visual_manual_zero(void)
{
    char line[48];

    snprintf(line, sizeof(line), "G10 L20 P0 %c0", nc_visual_manual_letter());
    nc_visual_manual_send(line);
    nc_visual_manual_status("Zero");
}

static void nc_visual_manual_touch_apply(void)
{
    char line[48];

    if (!g_nc_visual_manual_touch[0]) {
        return;
    }
    snprintf(line, sizeof(line), "G10 L20 P0 %c%s",
             nc_visual_manual_letter(), g_nc_visual_manual_touch);
    if (!nc_visual_manual_send(line)) {
        return;
    }
    snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Touch %c=%s",
             nc_visual_manual_letter(), g_nc_visual_manual_touch);
    g_nc_visual_manual_touch_active = false;
    g_nc_visual_manual_touch[0] = '\0';
    g_nc_visual_dirty = true;
}

static void nc_visual_manual_spindle(char code)
{
    char line[24];

    if (code == 'S') {
        nc_visual_manual_send("M5");
        g_nc_visual_manual_spindle_dir = 0;
        strncpy(g_nc_visual_status, "Spindle stop", sizeof(g_nc_visual_status) - 1);
    } else {
        g_nc_visual_manual_spindle_dir = code;
        snprintf(line, sizeof(line), "M%c S%u", code, NC_MANUAL_SPINDLE_RPM);
        nc_visual_manual_send(line);
        snprintf(g_nc_visual_status, sizeof(g_nc_visual_status),
                 "Spindle %s S%u", code == '3' ? "CW" : "CCW",
                 (unsigned)NC_MANUAL_SPINDLE_RPM);
    }
    g_nc_visual_dirty = true;
}

static bool nc_visual_manual_handle_digit(char ch)
{
    bool handled;

    switch (ch) {
    /* The jog keys name their axis, so a jog never lands on the other one by
       accident: 2/8 are X, 4/6 are Z, and the key picks the axis it moves. */
    case '2': g_nc_visual_manual_axis = 0; nc_visual_manual_jog('2', -1); handled = true; break;
    case '8': g_nc_visual_manual_axis = 0; nc_visual_manual_jog('8', +1); handled = true; break;
    case '4': g_nc_visual_manual_axis = 1; nc_visual_manual_jog('4', -1); handled = true; break;
    case '6': g_nc_visual_manual_axis = 1; nc_visual_manual_jog('6', +1); handled = true; break;
    case '5': nc_visual_manual_spindle('S'); handled = true; break;
    case '7': nc_visual_manual_spindle('4'); handled = true; break;   /* CCW */
    case '9': nc_visual_manual_spindle('3'); handled = true; break;   /* CW */
    /* The value keys: what they change is the value the pane shows beside them. */
    case '1': nc_visual_manual_value_adjust(-1); handled = true; break;
    case '3': nc_visual_manual_value_adjust(+1); handled = true; break;
    /* '0' is left to the footer's ZERO slot, so the key has one meaning. */
    default: handled = false; break;
    }
    if (handled) {
        g_nc_visual_manual_flash_key = ch;
        g_nc_visual_manual_flash_ms = mcu_millis();
        g_nc_visual_dirty = true;
    }
    return handled;
}

static void nc_visual_seed_demo(void)
{
    nc_document_init(&g_nc_visual_doc);
    nc_insert_line(&g_nc_visual_doc, 0, "G970 X-10 U120 Z-150 W30");
    nc_insert_line(&g_nc_visual_doc, 1, "G971 X80 Z125 E0");
    nc_insert_line(&g_nc_visual_doc, 2, "G972 C15");
    nc_insert_line(&g_nc_visual_doc, 3, "G973 P7");
    nc_insert_line(&g_nc_visual_doc, 4, "G71 U2 R1 X0.5 Z0.5 F120");
    nc_insert_line(&g_nc_visual_doc, 5, "\tG1 X50 Z0");
    nc_insert_line(&g_nc_visual_doc, 6, "\tG1 X25 Z-25");
    nc_insert_line(&g_nc_visual_doc, 7, "G80");
    g_nc_visual_doc.cursor_line = 4;
    nc_select_next_word(&g_nc_visual_doc);
    nc_select_next_word(&g_nc_visual_doc);
    strncpy(g_nc_visual_doc.path, "NC module bring-up", sizeof(g_nc_visual_doc.path) - 1);
    g_nc_visual_doc.dirty = false;
    g_nc_visual_status[0] = '\0';
    g_nc_visual_dirty = true;
}

typedef struct {
    float x;
    float z;
} nc_preview_v2_t;

static float nc_visual_absf(float v)
{
    return v < 0.0f ? -v : v;
}

static int nc_visual_clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float nc_visual_directed_arc_sweep(float a0, float a1, bool cw)
{
    float sweep = a1 - a0;

    if (cw) {
        while (sweep >= 0.0f) {
            sweep -= 6.2831853f;
        }
    } else {
        while (sweep <= 0.0f) {
            sweep += 6.2831853f;
        }
    }
    return sweep;
}

static bool nc_visual_r_arc_center(float start_z,
                                   float start_x,
                                   float end_z,
                                   float end_x,
                                   float r,
                                   bool cw,
                                   nc_preview_v2_t *center)
{
    float sx = start_x * 0.5f;
    float ex = end_x * 0.5f;
    float dz = end_z - start_z;
    float dx = ex - sx;
    float chord = sqrtf(dz * dz + dx * dx);
    float abs_r = nc_visual_absf(r);
    float mid_z = (start_z + end_z) * 0.5f;
    float mid_x = (sx + ex) * 0.5f;
    float h;
    float nz;
    float nx;
    nc_preview_v2_t c1;
    nc_preview_v2_t c2;
    float s1;
    float s2;

    if (!center || chord < 0.0001f || abs_r < chord * 0.5f) {
        return false;
    }

    h = sqrtf((abs_r * abs_r) - ((chord * 0.5f) * (chord * 0.5f)));
    nz = -dx / chord;
    nx = dz / chord;
    c1.z = mid_z + nz * h;
    c1.x = (mid_x + nx * h) * 2.0f;
    c2.z = mid_z - nz * h;
    c2.x = (mid_x - nx * h) * 2.0f;

    s1 = nc_visual_directed_arc_sweep(atan2f(sx - c1.x * 0.5f, start_z - c1.z),
                                      atan2f(ex - c1.x * 0.5f, end_z - c1.z),
                                      cw);
    s2 = nc_visual_directed_arc_sweep(atan2f(sx - c2.x * 0.5f, start_z - c2.z),
                                      atan2f(ex - c2.x * 0.5f, end_z - c2.z),
                                      cw);
    if (r >= 0.0f) {
        *center = nc_visual_absf(s1) <= nc_visual_absf(s2) ? c1 : c2;
    } else {
        *center = nc_visual_absf(s1) > nc_visual_absf(s2) ? c1 : c2;
    }
    return true;
}

static int nc_visual_preview_z(const nc_preview_info_t *p, int z0_x, int stock_w, float z)
{
    if (!p || p->stock_visible_z <= 0.0f) {
        return z0_x;
    }
    return z0_x + (int)((z / p->stock_visible_z) * (float)stock_w);
}

static int nc_visual_preview_x(const nc_preview_info_t *p, int stock_top, int stock_h, float x)
{
    if (!p || p->stock_x <= 0.0f) {
        return stock_top;
    }
    return stock_top + (int)(((x * 0.5f) / (p->stock_x * 0.5f)) * (float)stock_h);
}

static void nc_visual_draw_preview_tool_panel(int x,
                                              int y,
                                              int w,
                                              int h,
                                              const nc_tool_t *tool)
{
    int panel_w = 112;
    int panel_h = 48;
    int panel_x = x + w - panel_w - 10;
    int panel_y = y + 8;
    char buf[32];

    if (!tool || !tool->valid) {
        return;
    }

    lvds_draw_fill_rect(panel_x, panel_y, panel_w, panel_h, NC_VISUAL_PREVIEW_BG);
    lvds_draw_line(panel_x + 8, panel_y + 26, panel_x + 48, panel_y + 26, NC_VISUAL_DIM);
    lvds_draw_line(panel_x + 28, panel_y + 8, panel_x + 28, panel_y + 42, NC_VISUAL_DIM);
    nc_visual_draw_tool_glyph(panel_x + 28, panel_y + 26, 20, tool, NC_VISUAL_PREVIEW_BG, false);
    snprintf(buf, sizeof(buf), "T%d O%d", tool->t, tool->orient);
    nc_visual_draw_text_clip(panel_x + 54, panel_y + 10, buf, 8, NC_VISUAL_TEXT, NC_VISUAL_PREVIEW_BG, LVDS_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "R %.2g", (double)tool->r);
    nc_visual_draw_text_clip(panel_x + 54, panel_y + 28, buf, 8, NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_NORMAL);
}

static int nc_visual_tool_tip_digit(int orient)
{
    int digits[4];
    int n = 0;
    int tmp = orient;

    if (orient >= 1 && orient <= 9 && orient != 5) {
        return orient;
    }
    if (orient > 9) {
        while (tmp > 0 && n < (int)(sizeof(digits) / sizeof(digits[0]))) {
            digits[n++] = tmp % 10;
            tmp /= 10;
        }
        if (tmp > 0 || n <= 0) {
            return 3;
        }
        if (n == 3 || n == 4) {
            return digits[n - 2];
        }
        return digits[n - 1];
    }
    return 3;
}

static int nc_visual_tool_orient_digits(int orient, int *digits, int max_digits)
{
    int tmp[4];
    int n = 0;
    int i;

    while (orient > 0 && n < (int)(sizeof(tmp) / sizeof(tmp[0]))) {
        tmp[n++] = orient % 10;
        orient /= 10;
    }
    if (orient > 0 || n <= 0 || n > max_digits) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        digits[i] = tmp[n - 1 - i];
    }
    return n;
}

static bool nc_visual_tool_keypad_point(int digit, int ox, int oy, int step, int *x, int *y)
{
    static const int kx[10] = {0, -1, 0, 1, -1, 0, 1, -1, 0, 1};
    static const int ky[10] = {0,  1, 1, 1,  0, 0, 0, -1,-1,-1};

    if (digit < 1 || digit > 9) {
        return false;
    }
    if (x) *x = ox + (kx[digit] * step);
    if (y) *y = oy + (ky[digit] * step);
    return true;
}

static void nc_visual_tool_edges(int orient, bool *left, bool *top, bool *right, bool *bottom)
{
    int o = nc_visual_tool_tip_digit(orient);

    if (left) *left = (o == 1 || o == 4 || o == 7 || o == 2 || o == 5 || o == 8);
    if (top) *top = (o == 7 || o == 8 || o == 9 || o == 4 || o == 5 || o == 6);
    if (right) *right = (o == 3 || o == 6 || o == 9 || o == 2 || o == 5 || o == 8);
    if (bottom) *bottom = (o == 1 || o == 2 || o == 3 || o == 4 || o == 5 || o == 6);
}

static void nc_visual_fill_triangle(int x1, int y1,
                                    int x2, int y2,
                                    int x3, int y3,
                                    lvds_color_t color)
{
    int min_y = y1;
    int max_y = y1;
    int y;

    if (y2 < min_y) min_y = y2;
    if (y3 < min_y) min_y = y3;
    if (y2 > max_y) max_y = y2;
    if (y3 > max_y) max_y = y3;

    if (min_y < 0) min_y = 0;
    if (max_y >= LVDS_HSTX_HEIGHT) max_y = LVDS_HSTX_HEIGHT - 1;

    for (y = min_y; y <= max_y; y++) {
        int xs[3];
        int n = 0;
        if ((y1 <= y && y < y2) || (y2 <= y && y < y1)) {
            xs[n++] = x1 + ((x2 - x1) * (y - y1)) / (y2 - y1);
        }
        if ((y2 <= y && y < y3) || (y3 <= y && y < y2)) {
            xs[n++] = x2 + ((x3 - x2) * (y - y2)) / (y3 - y2);
        }
        if ((y3 <= y && y < y1) || (y1 <= y && y < y3)) {
            xs[n++] = x3 + ((x1 - x3) * (y - y3)) / (y1 - y3);
        }
        if (n >= 2) {
            int xa = xs[0];
            int xb = xs[1];
            if (xa > xb) {
                int t = xa;
                xa = xb;
                xb = t;
            }
            if (xa < 0) xa = 0;
            if (xb >= LVDS_HSTX_WIDTH) xb = LVDS_HSTX_WIDTH - 1;
            if (xb >= xa) {
                lvds_draw_fill_rect(xa, y, xb - xa + 1, 1, color);
            }
        }
    }
}

static void nc_visual_tool_marker_line(int x1,
                                       int y1,
                                       int x2,
                                       int y2,
                                       lvds_color_t color,
                                       int thick)
{
    x1 = nc_visual_clampi(x1, 0, LVDS_HSTX_WIDTH - 1);
    y1 = nc_visual_clampi(y1, 0, LVDS_HSTX_HEIGHT - 1);
    x2 = nc_visual_clampi(x2, 0, LVDS_HSTX_WIDTH - 1);
    y2 = nc_visual_clampi(y2, 0, LVDS_HSTX_HEIGHT - 1);

    if (thick > 1) {
        lvds_draw_line_w(x1, y1, x2, y2, color, thick);
    } else {
        lvds_draw_line(x1, y1, x2, y2, color);
    }
}

static int nc_visual_tool_polygon_points(int tip_x,
                                         int tip_y,
                                         int orient,
                                         int size,
                                         int *px,
                                         int *py)
{
    int digits[4];
    int tip_grid_x;
    int tip_grid_y;
    int step = nc_visual_clampi(size / 2, 6, 56);
    int n = nc_visual_tool_orient_digits(orient, digits, 4);
    int i;

    if (n < 3) {
        return 0;
    }
    if (n == 4) {
        int cut_x;
        int cut_y;
        int z_x;
        int z_y;
        if (!nc_visual_tool_keypad_point(digits[1], 0, 0, step, &cut_x, &cut_y) ||
            !nc_visual_tool_keypad_point(digits[2], 0, 0, step, &z_x, &z_y)) {
            return 0;
        }
        tip_grid_x = cut_x;
        tip_grid_y = z_y;
    } else {
        int tip_digit = nc_visual_tool_tip_digit(orient);
        if (!nc_visual_tool_keypad_point(tip_digit, 0, 0, step, &tip_grid_x, &tip_grid_y)) {
            return 0;
        }
    }

    for (i = 0; i < n; i++) {
        int gx;
        int gy;
        if (!nc_visual_tool_keypad_point(digits[i], 0, 0, step, &gx, &gy)) {
            return 0;
        }
        px[i] = tip_x + gx - tip_grid_x;
        py[i] = tip_y + gy - tip_grid_y;
    }
    return n;
}

static void nc_visual_draw_tool_polygon(int tip_x,
                                        int tip_y,
                                        int orient,
                                        int size,
                                        int thick,
                                        lvds_color_t fill,
                                        lvds_color_t edge,
                                        lvds_color_t mount)
{
    int px[4];
    int py[4];
    int n = nc_visual_tool_polygon_points(tip_x, tip_y, orient, size, px, py);
    int i;

    if (n < 3) {
        return;
    }
    for (i = 1; i + 1 < n; i++) {
        nc_visual_fill_triangle(px[0], py[0], px[i], py[i], px[i + 1], py[i + 1], fill);
    }
    if (n == 3) {
        nc_visual_tool_marker_line(px[1], py[1], px[0], py[0], edge, thick);
        nc_visual_tool_marker_line(px[1], py[1], px[2], py[2], edge, thick);
        nc_visual_tool_marker_line(px[0], py[0], px[2], py[2], mount, 1);
    } else {
        nc_visual_tool_marker_line(px[1], py[1], px[2], py[2], edge, thick > 1 ? thick : 2);
        nc_visual_tool_marker_line(px[3], py[3], px[0], py[0], mount, 1);
    }
}

static void nc_visual_draw_tool_glyph(int tip_x,
                                      int tip_y,
                                      int size,
                                      const nc_tool_t *tool,
                                      lvds_color_t bg,
                                      bool selected)
{
    int rr;
    int corner;
    int sx;
    int sy;
    bool left;
    bool top;
    bool right;
    bool bottom;
    lvds_color_t edge = selected ? NC_VISUAL_LINE_NO_SELECTED : NC_VISUAL_TOOL_MARK;
    lvds_color_t fill = NC_VISUAL_TOOL_FILL;

    if (!tool || !tool->valid) {
        return;
    }

    rr = tool->r > 0.0f ? (int)(tool->r * 8.0f) : 2;
    rr = nc_visual_clampi(rr, 1, size / 3);
    corner = nc_visual_tool_tip_digit(tool->orient);
    switch (corner) {
    case 7:
        sx = tip_x;
        sy = tip_y;
        break;
    case 9:
        sx = tip_x - size;
        sy = tip_y;
        break;
    case 1:
        sx = tip_x;
        sy = tip_y - size;
        break;
    case 3:
    default:
        sx = tip_x - size;
        sy = tip_y - size;
        break;
    }

    if (tool->orient == 0) {
        lvds_draw_fill_ellipse(tip_x, tip_y, 4, 4, fill);
        lvds_draw_ellipse(tip_x, tip_y, 4, 4, edge);
    } else if (tool->orient == 5) {
        lvds_draw_fill_rect(tip_x - size / 2, tip_y - size / 2, size, size, fill);
        lvds_draw_rect(tip_x - size / 2, tip_y - size / 2, size, size, edge);
        lvds_draw_line(tip_x - size / 2, tip_y, tip_x + size / 2, tip_y, edge);
        lvds_draw_line(tip_x, tip_y - size / 2, tip_x, tip_y + size / 2, edge);
    } else if (tool->orient > 9) {
        nc_visual_draw_tool_polygon(tip_x,
                                    tip_y,
                                    tool->orient,
                                    size,
                                    selected ? 2 : 1,
                                    fill,
                                    edge,
                                    NC_VISUAL_DIM);
    } else {
        nc_visual_tool_edges(tool->orient, &left, &top, &right, &bottom);
        lvds_draw_fill_rect(sx + 1, sy + 1, size - 1, size - 1, fill);
        if (left) lvds_draw_line(sx, sy, sx, sy + size, edge);
        if (top) lvds_draw_line(sx, sy, sx + size, sy, edge);
        if (right) lvds_draw_line(sx + size, sy, sx + size, sy + size, edge);
        if (bottom) lvds_draw_line(sx, sy + size, sx + size, sy + size, edge);
        lvds_draw_fill_ellipse(tip_x, tip_y, rr, rr, edge);
        lvds_draw_rect(tip_x - rr, tip_y - rr, rr * 2, rr * 2, bg);
    }
    lvds_draw_fill_ellipse(tip_x, tip_y, 2, 2, edge);
}

static void nc_visual_draw_tool_glyph_centered(int x,
                                               int y,
                                               int box_size,
                                               int marker_size,
                                               const nc_tool_t *tool,
                                               lvds_color_t bg,
                                               bool selected)
{
    int sx;
    int sy;
    int tip_x;
    int tip_y;
    int corner;

    if (!tool || !tool->valid) {
        return;
    }

    sx = x + ((box_size - marker_size) / 2);
    sy = y + ((box_size - marker_size) / 2);
    tip_x = x + (box_size / 2);
    tip_y = y + (box_size / 2);

    if (tool->orient > 9) {
        int px[4];
        int py[4];
        int n = nc_visual_tool_polygon_points(tip_x, tip_y, tool->orient, marker_size, px, py);
        if (n >= 3) {
            int min_x = px[0];
            int max_x = px[0];
            int min_y = py[0];
            int max_y = py[0];
            int i;
            for (i = 1; i < n; i++) {
                if (px[i] < min_x) min_x = px[i];
                if (px[i] > max_x) max_x = px[i];
                if (py[i] < min_y) min_y = py[i];
                if (py[i] > max_y) max_y = py[i];
            }
            tip_x += (x + (box_size / 2)) - ((min_x + max_x) / 2);
            tip_y += (y + (box_size / 2)) - ((min_y + max_y) / 2);
        }
    } else if (tool->orient > 0 && tool->orient <= 9 && tool->orient != 5) {
        corner = nc_visual_tool_tip_digit(tool->orient);
        switch (corner) {
        case 7:
            tip_x = sx;
            tip_y = sy;
            break;
        case 9:
            tip_x = sx + marker_size;
            tip_y = sy;
            break;
        case 1:
            tip_x = sx;
            tip_y = sy + marker_size;
            break;
        case 3:
        default:
            tip_x = sx + marker_size;
            tip_y = sy + marker_size;
            break;
        }
    }

    nc_visual_draw_tool_glyph(tip_x, tip_y, marker_size, tool, bg, selected);
}

static bool nc_visual_selected_tool_word(char *letter, int *line_index)
{
    nc_word_t word;

    if (letter) {
        *letter = '\0';
    }
    if (line_index) {
        *line_index = -1;
    }
    if (g_nc_visual_mode != NC_MODE_TOOLS ||
        nc_get_selected_word(&g_nc_visual_doc, &word) != NC_OK ||
        g_nc_visual_doc.cursor_line >= g_nc_visual_doc.line_count ||
        !nc_tool_line_is_tool(g_nc_visual_doc.lines[g_nc_visual_doc.cursor_line].text)) {
        return false;
    }
    if (letter) {
        *letter = word.letter;
    }
    if (line_index) {
        *line_index = (int)g_nc_visual_doc.cursor_line;
    }
    return true;
}

static void nc_visual_draw_tool_cell(const char *line,
                                     char letter,
                                     int x,
                                     int y,
                                     int cols,
                                     lvds_color_t fg,
                                     lvds_color_t bg,
                                     bool active)
{
    char value[24];
    lvds_color_t cell_fg = active ? NC_VISUAL_WORD_FG : fg;
    lvds_color_t cell_bg = active ? NC_VISUAL_WORD_BG : bg;

    if (!nc_tool_field_text(line, letter, value, sizeof(value))) {
        value[0] = '-';
        value[1] = '\0';
    }
    if (active) {
        lvds_draw_fill_rect(x - 2, y - 2, (cols * NC_VISUAL_CHAR_W) + 4, 20, cell_bg);
    }
    nc_visual_draw_text_clip(x, y, value, cols, cell_fg, cell_bg, LVDS_FONT_NORMAL);
}

static void nc_visual_draw_tool_param(const char *line,
                                      char letter,
                                      const char *label,
                                      int x,
                                      int y,
                                      int value_cols,
                                      bool active)
{
    char value[24];
    char buf[24];
    lvds_color_t value_fg = active ? NC_VISUAL_WORD_FG : NC_VISUAL_TEXT;
    lvds_color_t value_bg = active ? NC_VISUAL_WORD_BG : NC_VISUAL_BG;

    snprintf(buf, sizeof(buf), "%-7s", label ? label : "");
    nc_visual_draw_text_clip(x, y, buf, 7, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    if (!nc_tool_field_text(line, letter, value, sizeof(value))) {
        value[0] = '-';
        value[1] = '\0';
    }
    if (active) {
        lvds_draw_fill_rect(x + 68, y - 2, (value_cols * NC_VISUAL_CHAR_W) + 4, 20, value_bg);
    }
    nc_visual_draw_text_clip(x + 70, y, value, value_cols, value_fg, value_bg, LVDS_FONT_NORMAL);
}

static int nc_visual_find_tool_line(int selected_tool, int *selected_line)
{
    int count = 0;
    size_t i;

    if (selected_line) {
        *selected_line = -1;
    }
    for (i = 0; i < g_nc_visual_doc.line_count; i++) {
        if (nc_tool_line_is_tool(g_nc_visual_doc.lines[i].text)) {
            if (count == selected_tool && selected_line) {
                *selected_line = (int)i;
            }
            count++;
        }
    }
    return count;
}

static int nc_visual_selected_tool_index(void)
{
    int count = 0;
    size_t i;

    for (i = 0; i < g_nc_visual_doc.line_count; i++) {
        if (nc_tool_line_is_tool(g_nc_visual_doc.lines[i].text)) {
            if (i >= g_nc_visual_doc.cursor_line) {
                return count;
            }
            count++;
        }
    }
    return count > 0 ? count - 1 : 0;
}

#if NC_PREVIEW_DIN_STYLE
static void nc_visual_draw_dashdot_line(int x0,
                                        int y0,
                                        int x1,
                                        int y1,
                                        lvds_color_t color)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int len2 = dx * dx + dy * dy;
    float len;
    int pos = 0;
    static const uint8_t pattern[] = {18, 5, 3, 5};
    int pat = 0;

    if (len2 <= 0) {
        return;
    }
    len = sqrtf((float)len2);
    while (pos < (int)len) {
        int seg = pattern[pat & 3];
        int a = pos;
        int b = pos + seg;

        if (b > (int)len) {
            b = (int)len;
        }
        if ((pat & 1) == 0 && b > a) {
            int xa = x0 + (int)((float)dx * ((float)a / len));
            int ya = y0 + (int)((float)dy * ((float)a / len));
            int xb = x0 + (int)((float)dx * ((float)b / len));
            int yb = y0 + (int)((float)dy * ((float)b / len));
            lvds_draw_line(xa, ya, xb, yb, color);
        }
        pos += seg;
        pat++;
    }
}

static void nc_visual_draw_centerline(int x0, int y0, int x1, int y1)
{
    nc_visual_draw_dashdot_line(x0, y0, x1, y1, NC_VISUAL_DIM);
}

static void nc_visual_draw_origin_marker(int x, int y)
{
    lvds_draw_ellipse(x, y, 11, 11, NC_VISUAL_TEXT);
    lvds_draw_ellipse(x, y, 6, 6, NC_VISUAL_TEXT);
    lvds_draw_line(x - 15, y, x - 8, y, NC_VISUAL_TEXT);
    lvds_draw_line(x + 8, y, x + 15, y, NC_VISUAL_TEXT);
    lvds_draw_line(x, y - 15, x, y - 8, NC_VISUAL_TEXT);
    lvds_draw_line(x, y + 8, x, y + 15, NC_VISUAL_TEXT);
}

static void nc_visual_draw_chuck_hatching(int x, int y, int w, int h, lvds_color_t color)
{
    int s;

    if (w <= 0 || h <= 0) {
        return;
    }
    for (s = -h; s < w; s += 8) {
        int x0 = s > 0 ? s : 0;
        int y0 = s > 0 ? 0 : -s;
        int x1 = (s + h) < w ? s + h : w;
        int y1 = (s + h) < w ? h : w - s;

        y1 = nc_visual_clampi(y1, 0, h);
        lvds_draw_line(x + x0, y + y0, x + x1, y + y1, color);
    }
    for (s = 0; s < w + h; s += 8) {
        int x0 = s < w ? s : w;
        int y0 = s < w ? 0 : s - w;
        int x1 = s < h ? 0 : s - h;
        int y1 = s < h ? s : h;

        x1 = nc_visual_clampi(x1, 0, w);
        y0 = nc_visual_clampi(y0, 0, h);
        lvds_draw_line(x + x0, y + y0, x + x1, y + y1, color);
    }
}

static void nc_visual_draw_arrowhead(int x, int y, int dir_x, int dir_y, lvds_color_t color)
{
    int px = -dir_y;
    int py = dir_x;

    lvds_draw_line(x, y, x - dir_x * 7 + px * 3, y - dir_y * 7 + py * 3, color);
    lvds_draw_line(x, y, x - dir_x * 7 - px * 3, y - dir_y * 7 - py * 3, color);
}

static void nc_visual_draw_diameter_dimension(int x,
                                              int y0,
                                              int y1,
                                              const char *label)
{
    lvds_draw_line(x, y0, x, y1, NC_VISUAL_DIM);
    nc_visual_draw_arrowhead(x, y1, 0, 1, NC_VISUAL_DIM);
    if (label && label[0]) {
        lvds_draw_text(x + 6,
                       y1 - 8,
                       label,
                       NC_VISUAL_DIM,
                       NC_VISUAL_PREVIEW_BG,
                       LVDS_FONT_NORMAL);
    }
}

static void nc_visual_draw_z_point_dimension(int start_x,
                                             int point_x,
                                             int zero_x,
                                             int center_y,
                                             int point_y,
                                             const char *label)
{
    int dim_y = center_y - 15;
    int ext_top = center_y - 20;
    int ext_bottom = point_y + 3;
    int text_x;

    if (ext_bottom < ext_top) {
        int t = ext_bottom;
        ext_bottom = ext_top;
        ext_top = t;
    }
    lvds_draw_line(point_x, ext_top, point_x, ext_bottom, NC_VISUAL_DIM);
    lvds_draw_line(start_x, dim_y, point_x, dim_y, NC_VISUAL_DIM);
    if (start_x == zero_x) {
        lvds_draw_fill_rect(start_x - 1, dim_y - 1, 3, 3, NC_VISUAL_DIM);
    } else {
        nc_visual_draw_arrowhead(point_x, dim_y, -1, 0, NC_VISUAL_DIM);
    }
    if (!label || !label[0]) {
        return;
    }
    text_x = point_x - lvds_draw_text_width(label, LVDS_FONT_SMALL) / 2;
    lvds_draw_text(text_x,
                   center_y - 26,
                   label,
                   NC_VISUAL_DIM,
                   NC_VISUAL_PREVIEW_BG,
                   LVDS_FONT_SMALL);
}

static void nc_visual_draw_x_point_dimension(int dim_x,
                                             int start_y,
                                             int point_y,
                                             int zero_y,
                                             int point_x,
                                             const char *label)
{
    int text_y;

    lvds_draw_line(dim_x, start_y, dim_x, point_y, NC_VISUAL_DIM);
    lvds_draw_line(point_x + 3, point_y, dim_x, point_y, NC_VISUAL_DIM);
    if (start_y == zero_y) {
        lvds_draw_fill_rect(dim_x - 1, start_y - 1, 3, 3, NC_VISUAL_DIM);
    }
    nc_visual_draw_arrowhead(dim_x, point_y, 0, 1, NC_VISUAL_DIM);
    if (!label || !label[0]) {
        return;
    }
    text_y = point_y - 6;
    lvds_draw_text(dim_x + 4,
                   text_y,
                   label,
                   NC_VISUAL_DIM,
                   NC_VISUAL_PREVIEW_BG,
                   LVDS_FONT_SMALL);
}

static void nc_visual_draw_contour_point_marker(int x, int y, bool filled)
{
    if (filled) {
        lvds_draw_fill_ellipse(x, y, 3, 3, NC_VISUAL_TEXT);
    } else {
        lvds_draw_ellipse(x, y, 5, 5, NC_VISUAL_TEXT);
    }
}

#endif

static void nc_visual_draw_chuck(const nc_preview_info_t *preview,
                                 int stock_left,
                                 int stock_top,
                                 int stock_w,
                                 int stock_h)
{
    float c = preview && preview->chuck_c > 0.0f ? preview->chuck_c : 15.0f;
    int c_w = preview && preview->stock_visible_z > 0.0f ?
              (int)((c / preview->stock_visible_z) * (float)stock_w + 0.5f) :
              24;
    int c_h = preview && preview->stock_x > 0.0f ?
              (int)((c / preview->stock_x) * (float)stock_h + 0.5f) :
              24;
    int block_x = 0;
    int block_w;
    int block_y;
    lvds_color_t fill = NC_VISUAL_FOOTER_BUTTON;
    lvds_color_t ink = NC_VISUAL_LINE_NO_SELECTED;

    if (c_w < 8) c_w = 8;
    if (c_h < 8) c_h = 8;
    block_w = stock_left + c_w;
    if (block_w > stock_left + stock_w) {
        block_w = stock_left + stock_w;
    }
    block_y = stock_top + stock_h - (c_h / 2);
    if (block_y < 44) {
        block_y = 44;
    }
    if (block_y + c_h > NC_FOOTER_Y) {
        c_h = NC_FOOTER_Y - block_y;
    }
    if (block_w <= 0 || c_h <= 0) {
        return;
    }

    lvds_draw_fill_rect(block_x, block_y, block_w, c_h, fill);
    lvds_draw_rect(block_x, block_y, block_w, c_h, ink);
#if NC_PREVIEW_DIN_STYLE
    nc_visual_draw_chuck_hatching(block_x, block_y, block_w, c_h, ink);
#endif
}

static void nc_visual_draw_chuck_relief(const nc_preview_info_t *preview,
                                        int stock_left,
                                        int stock_top,
                                        int stock_h)
{
    float c = preview && preview->chuck_c > 0.0f ? preview->chuck_c : 15.0f;
    int c_h = preview && preview->stock_x > 0.0f ?
              (int)((c / preview->stock_x) * (float)stock_h + 0.5f) :
              24;
    int r = c_h / 8;

    if (r < 2) {
        r = 2;
    }
    lvds_draw_fill_ellipse(stock_left,
                           stock_top + stock_h,
                           r,
                           r,
                           NC_VISUAL_PREVIEW_BG);
    lvds_draw_ellipse(stock_left,
                      stock_top + stock_h,
                      r,
                      r,
                      NC_VISUAL_LINE_NO_SELECTED);
}

#if NC_PREVIEW_DIN_STYLE
static void nc_visual_draw_din_layer(const nc_preview_info_t *preview,
                                     int stock_left,
                                     int stock_top,
                                     int stock_w,
                                     int stock_h,
                                     int z0_x)
{
    char label[16];
    int stock_right;
    int stock_bottom;
    int dim_x;
    int id_y;

    if (!preview) {
        return;
    }

    stock_right = stock_left + stock_w;
    stock_bottom = stock_top + stock_h;
    dim_x = stock_right + 14;
    if (dim_x > LVDS_HSTX_WIDTH - 24) {
        dim_x = stock_right - 18;
    }

    nc_visual_draw_centerline(stock_left - 34,
                              stock_top,
                              stock_right + 18,
                              stock_top);
    nc_visual_draw_centerline(z0_x,
                              stock_top - 26,
                              z0_x,
                              stock_bottom + 20);
    nc_visual_draw_origin_marker(z0_x, stock_top);

    /*lvds_draw_line(z0_x, stock_top, z0_x + 24, stock_top - 22, NC_VISUAL_DIM);
    lvds_draw_text(z0_x + 27,
                   stock_top - 29,
                   "+Z",
                   NC_VISUAL_DIM,
                   NC_VISUAL_PREVIEW_BG,
                   LVDS_FONT_NORMAL);
    lvds_draw_line(z0_x, stock_top, z0_x - 18, stock_top - 24, NC_VISUAL_DIM);
    lvds_draw_text(z0_x - 35,
                   stock_top - 39,
                   "+X",
                   NC_VISUAL_DIM,
                   NC_VISUAL_PREVIEW_BG,
                   LVDS_FONT_NORMAL);*/

    if (g_nc_visual_mode != NC_MODE_RUN) {
        snprintf(label, sizeof(label), "%.0f", preview->stock_x);
        nc_visual_draw_diameter_dimension(dim_x,
                                          stock_top,
                                          stock_bottom,
                                          label);
    }
    if (g_nc_visual_mode != NC_MODE_RUN &&
        preview->stock_i > 0.0f && preview->stock_i < preview->stock_x) {
        id_y = nc_visual_preview_x(preview, stock_top, stock_h, preview->stock_i);
        nc_visual_draw_centerline(stock_left - 18, id_y, stock_right + 8, id_y);
        snprintf(label, sizeof(label), "%.0f", preview->stock_i);
        nc_visual_draw_diameter_dimension(dim_x - 18,
                                          stock_top,
                                          id_y,
                                          label);
    }
}
#endif

/* True when `index` carries contour geometry of any G7x block, whether the
   block ends at G80 or at a numbered range's N(Q) row. */
static bool nc_visual_line_is_g7x_contour(const nc_document_t *doc, size_t index)
{
    size_t i;

    if (!doc || index >= doc->line_count) {
        return false;
    }
    for (i = 0; i <= index; i++) {
        if (!nc_g7x_line_is_header(doc->lines[i].text)) {
            continue;
        }
        if (nc_g7x_line_is_contour(doc, i, index)) {
            return true;
        }
    }
    return false;
}

static void nc_visual_draw_contour_points(const nc_document_t *doc,
                                              const nc_preview_info_t *preview,
                                              int z0_x,
                                              int stock_left,
                                              int stock_right,
                                              int stock_w,
                                              int stock_top,
                                              int stock_h)
{
#if NC_PREVIEW_DIN_POINT_MARKERS
    size_t i;
    float x = preview ? preview->stock_x : 0.0f;
    float z = 0.0f;
    int prev_z_px = z0_x;
    int prev_x_py = stock_top;
    int x_dim = stock_right + 15;
    char label[16];

    /* The dimension callouts follow the same switch as the rest of the layer:
       EDIT shows them in the pane and on the whole body alike, so turning `DIM`
       off on the full body is what hides them in the split view too. */
    if (!doc || !preview || g_nc_visual_mode == NC_MODE_MANUAL ||
        g_nc_visual_mode == NC_MODE_TOOLS) {
        return;
    }

    for (i = 0; i < doc->line_count; i++) {
        const char *line = doc->lines[i].text;
        g7x_contour_cmd_t cmd;
        bool has_x;
        bool has_z;

        if (!nc_visual_line_is_g7x_contour(doc, i)) {
            continue;
        }
        cmd = g7x_contour_cmd_from_line(line);
        if (cmd == G7X_CONTOUR_NONE || cmd == G7X_CONTOUR_RAPID) {
            continue;
        }

        has_x = nc_preview_line_word_float(line, 'X', &x);
        has_z = nc_preview_line_word_float(line, 'Z', &z);
        if (has_x || has_z) {
            int px = nc_visual_preview_z(preview, z0_x, stock_w, z);
            int py = nc_visual_preview_x(preview, stock_top, stock_h, x);
            float feature = 0.0f;
            if (nc_visual_full_preview() ||
                (g_nc_visual_mode == NC_MODE_RUN &&
                 !nc_run_active() &&
                 !nc_run_hold() &&
                 !cnc_get_exec_state(EXEC_RUN | EXEC_HOLD))) {
                nc_visual_draw_contour_point_marker(px, py, i == doc->cursor_line);
            }
            snprintf(label, sizeof(label), "%.0f", x);
            nc_visual_draw_x_point_dimension(x_dim, prev_x_py, py, stock_top, px, label);
            prev_x_py = py;
            snprintf(label, sizeof(label), "%.0f", z);
            nc_visual_draw_z_point_dimension(prev_z_px, px, z0_x, stock_top, py, label);
            prev_z_px = px;
            if (nc_preview_line_word_float(line, 'R', &feature) && feature > 0.0001f) {
                snprintf(label, sizeof(label), "R%.0f", feature);
                lvds_draw_text(px + 4, py - 16, label, NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_SMALL);
            } else if (nc_preview_line_word_float(line, 'C', &feature) && feature > 0.0001f) {
                snprintf(label, sizeof(label), "C%.0f", feature);
                lvds_draw_text(px + 4, py - 16, label, NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_SMALL);
            }
        }
    }
    snprintf(label, sizeof(label), "%.0f", -preview->stock_visible_z);
    nc_visual_draw_z_point_dimension(prev_z_px,
                                     stock_left,
                                     z0_x,
                                     stock_top,
                                     stock_top,
                                     label);
#else
    (void)doc;
    (void)preview;
    (void)z0_x;
    (void)stock_left;
    (void)stock_right;
    (void)stock_w;
    (void)stock_top;
    (void)stock_h;
#endif
}

static bool nc_visual_draw_explicit_arc(const nc_preview_info_t *preview,
                                        int z0_x,
                                        int stock_w,
                                        int stock_top,
                                        int stock_h,
                                        float start_x,
                                        float start_z,
                                        float end_x,
                                        float end_z,
                                        float r,
                                        bool cw,
                                        lvds_color_t color,
                                        int width)
{
    nc_preview_v2_t center;
    float a0;
    float sweep;
    float abs_r = nc_visual_absf(r);
    float screen_r;
    int steps;
    int last_px;
    int last_py;
    int i;

    if (!preview || !nc_visual_r_arc_center(start_z, start_x, end_z, end_x, r, cw, &center)) {
        return false;
    }

    a0 = atan2f((start_x * 0.5f) - (center.x * 0.5f), start_z - center.z);
    sweep = nc_visual_directed_arc_sweep(a0,
                                         atan2f((end_x * 0.5f) - (center.x * 0.5f),
                                                end_z - center.z),
                                         cw);
    screen_r = abs_r * (float)stock_w / (preview->stock_z > 0.0001f ? preview->stock_z : 1.0f);
    steps = nc_visual_clampi((int)(nc_visual_absf(sweep) * screen_r * 0.35f) + 8,
                             10,
                             NC_PREVIEW_ARC_MAX_STEPS);
    last_px = nc_visual_preview_z(preview, z0_x, stock_w, start_z);
    last_py = nc_visual_preview_x(preview, stock_top, stock_h, start_x);
    for (i = 1; i <= steps; i++) {
        float a = a0 + sweep * ((float)i / (float)steps);
        float z = center.z + cosf(a) * abs_r;
        float x = ((center.x * 0.5f) + sinf(a) * abs_r) * 2.0f;
        int px = nc_visual_preview_z(preview, z0_x, stock_w, z);
        int py = nc_visual_preview_x(preview, stock_top, stock_h, x);
        lvds_draw_line_w(last_px, last_py, px, py, color, width);
        last_px = px;
        last_py = py;
    }
    return true;
}

static bool nc_visual_draw_center_arc(const nc_preview_info_t *preview,
                                      int z0_x,
                                      int stock_w,
                                      int stock_top,
                                      int stock_h,
                                      float start_x,
                                      float start_z,
                                      float end_x,
                                      float end_z,
                                      float i_off,
                                      float k_off,
                                      bool cw,
                                      lvds_color_t color,
                                      int width)
{
    float center_z = start_z + k_off;
    float center_xr = (start_x * 0.5f) + i_off;
    float start_xr = start_x * 0.5f;
    float end_xr = end_x * 0.5f;
    float dz = start_z - center_z;
    float dx = start_xr - center_xr;
    float radius = sqrtf(dz * dz + dx * dx);
    float screen_r;
    float a0;
    float sweep;
    int steps;
    int last_px;
    int last_py;
    int n;

    if (!preview || radius < 0.0001f) {
        return false;
    }

    a0 = atan2f(start_xr - center_xr, start_z - center_z);
    sweep = nc_visual_directed_arc_sweep(a0,
                                         atan2f(end_xr - center_xr,
                                                end_z - center_z),
                                         cw);
    screen_r = radius * (float)stock_w / (preview->stock_z > 0.0001f ? preview->stock_z : 1.0f);
    steps = nc_visual_clampi((int)(nc_visual_absf(sweep) * screen_r * 0.35f) + 8,
                             10,
                             NC_PREVIEW_ARC_MAX_STEPS);
    last_px = nc_visual_preview_z(preview, z0_x, stock_w, start_z);
    last_py = nc_visual_preview_x(preview, stock_top, stock_h, start_x);
    for (n = 1; n <= steps; n++) {
        float a = a0 + sweep * ((float)n / (float)steps);
        float z = center_z + cosf(a) * radius;
        float x = (center_xr + sinf(a) * radius) * 2.0f;
        int px = nc_visual_preview_z(preview, z0_x, stock_w, z);
        int py = nc_visual_preview_x(preview, stock_top, stock_h, x);
        lvds_draw_line_w(last_px, last_py, px, py, color, width);
        last_px = px;
        last_py = py;
    }
    return true;
}

static void nc_visual_draw_dashed_segment(const nc_preview_info_t *preview,
                                          int z0_x,
                                          int stock_w,
                                          int stock_top,
                                          int stock_h,
                                          float z0,
                                          float x0,
                                          float z1,
                                          float x1,
                                          lvds_color_t color)
{
    float dz = z1 - z0;
    float dx = x1 - x0;
    int px0;
    int py0;
    int px1;
    int py1;
    float plen;
    int pieces;
    int p;

    if (!preview) {
        return;
    }

    px0 = nc_visual_preview_z(preview, z0_x, stock_w, z0);
    py0 = nc_visual_preview_x(preview, stock_top, stock_h, x0);
    px1 = nc_visual_preview_z(preview, z0_x, stock_w, z1);
    py1 = nc_visual_preview_x(preview, stock_top, stock_h, x1);
    plen = sqrtf((float)((px1 - px0) * (px1 - px0) + (py1 - py0) * (py1 - py0)));
    pieces = nc_visual_clampi((int)(plen / 8.0f), 1, 80);
    for (p = 0; p < pieces; p += 2) {
        float a = (float)p / (float)pieces;
        float b = (float)(p + 1) / (float)pieces;
        int xa;
        int ya;
        int xb;
        int yb;

        if (b > 1.0f) {
            b = 1.0f;
        }
        xa = nc_visual_preview_z(preview, z0_x, stock_w, z0 + dz * a);
        ya = nc_visual_preview_x(preview, stock_top, stock_h, x0 + dx * a);
        xb = nc_visual_preview_z(preview, z0_x, stock_w, z0 + dz * b);
        yb = nc_visual_preview_x(preview, stock_top, stock_h, x0 + dx * b);
        lvds_draw_line(xa, ya, xb, yb, color);
    }
}

static bool nc_visual_draw_emitted_motion_line(const nc_preview_info_t *preview,
                                               int z0_x,
                                               int stock_w,
                                               int stock_top,
                                               int stock_h,
                                               const char *line,
                                               nc_preview_segment_t segment,
                                               bool selected,
                                               float *last_x,
                                               float *last_z,
                                               bool *have_last)
{
    g7x_contour_cmd_t cmd;
    float x;
    float z;
    bool has_x;
    bool has_z;
    int x0;
    int y0;
    int x1;
    int y1;
    lvds_color_t color = NC_VISUAL_TEXT;
    int width = selected ? 3 : 1;

    if (!preview || !line || !last_x || !last_z || !have_last) {
        return false;
    }
    cmd = g7x_contour_cmd_from_line(line);
    if (cmd == G7X_CONTOUR_NONE || cmd == G7X_CONTOUR_END) {
        return false;
    }

    x = *last_x;
    z = *last_z;
    has_x = nc_preview_line_word_float(line, 'X', &x);
    has_z = nc_preview_line_word_float(line, 'Z', &z);
    if (!*have_last) {
        *last_x = has_x ? x : preview->stock_x;
        *last_z = has_z ? z : 0.0f;
        *have_last = true;
        return false;
    }
    if (!has_x && !has_z) {
        return false;
    }

    x0 = nc_visual_preview_z(preview, z0_x, stock_w, *last_z);
    y0 = nc_visual_preview_x(preview, stock_top, stock_h, *last_x);
    x1 = nc_visual_preview_z(preview, z0_x, stock_w, z);
    y1 = nc_visual_preview_x(preview, stock_top, stock_h, x);
    if (segment == NC_PREVIEW_SEG_ROUGH) {
        color = NC_VISUAL_TEXT;
    } else if (segment == NC_PREVIEW_SEG_FINISH) {
        color = NC_VISUAL_TEXT;
    }

    if (cmd == G7X_CONTOUR_ARC_CW || cmd == G7X_CONTOUR_ARC_CCW) {
        float r = 0.0f;
        float i_off = 0.0f;
        float k_off = 0.0f;
        if ((nc_preview_line_word_float(line, 'I', &i_off) &&
             nc_preview_line_word_float(line, 'K', &k_off) &&
             nc_visual_draw_center_arc(preview,
                                       z0_x,
                                       stock_w,
                                       stock_top,
                                       stock_h,
                                       *last_x,
                                       *last_z,
                                       x,
                                       z,
                                       i_off,
                                       k_off,
                                       cmd == G7X_CONTOUR_ARC_CW,
                                       color,
                                       width)) ||
            (nc_preview_line_word_float(line, 'R', &r) &&
             nc_visual_draw_explicit_arc(preview,
                                         z0_x,
                                         stock_w,
                                         stock_top,
                                         stock_h,
                                         *last_x,
                                         *last_z,
                                         x,
                                         z,
                                         r,
                                         cmd == G7X_CONTOUR_ARC_CW,
                                         color,
                                         width))) {
            /* Arc drawn above. */
        } else {
            lvds_draw_line_w(x0, y0, x1, y1, color, width);
        }
    } else if (cmd == G7X_CONTOUR_RAPID) {
        nc_visual_draw_dashed_segment(preview,
                                      z0_x,
                                      stock_w,
                                      stock_top,
                                      stock_h,
                                      *last_z,
                                      *last_x,
                                      z,
                                      x,
                                      NC_VISUAL_ERROR);
    } else {
        nc_visual_draw_dashed_segment(preview,
                                      z0_x,
                                      stock_w,
                                      stock_top,
                                      stock_h,
                                      *last_z,
                                      *last_x,
                                      z,
                                      x,
                                      color);
    }

    *last_x = x;
    *last_z = z;
    return true;
}

static void nc_visual_draw_emitted_preview(const nc_document_t *doc,
                                           const nc_preview_info_t *preview,
                                           int z0_x,
                                           int stock_w,
                                           int stock_top,
                                           int stock_h,
                                           size_t max_lines)
{
    nc_emit_stream_t stream;
    float last_x = 0.0f;
    float last_z = 0.0f;
    bool have_last = false;
    size_t emitted_line = 0;
    size_t drawn_lines = 0;
    size_t guard = 0;
    nc_preview_segment_t segment = NC_PREVIEW_SEG_FEED;

    if (!doc || !preview) {
        return;
    }

    nc_emit_stream_begin(&stream, doc, 0);
    nc_emit_stream_set_log(&stream, false);
    while (stream.active && guard++ < max_lines * 8u + 64u) {
        char line[NC_MAX_LINE_LEN];
        nc_emit_result_t result = nc_emit_stream_next(&stream,
                                                      line,
                                                      sizeof(line),
                                                      &emitted_line);
        if (result == NC_EMIT_ERROR) {
            snprintf(g_nc_visual_status, sizeof(g_nc_visual_status),
                     "Preview: %s at line %lu", g7x_result_text(stream.error),
                     (unsigned long)(emitted_line + 1u));
            break;
        }
        if (result == NC_EMIT_LINE) {
            if (line[0] == '(') {
                if (strstr(line, "rough")) {
                    segment = NC_PREVIEW_SEG_ROUGH;
                } else if (strstr(line, "finish")) {
                    segment = NC_PREVIEW_SEG_FINISH;
                }
                continue;
            }
            if (drawn_lines >= max_lines) {
                break;
            }
            if (g_nc_visual_mode == NC_MODE_PROGRAM &&
                ((segment == NC_PREVIEW_SEG_ROUGH && !g_nc_visual_show_rough) ||
                 (segment != NC_PREVIEW_SEG_ROUGH && !g_nc_visual_show_path))) {
                drawn_lines++;
                continue;
            }
            (void)nc_visual_draw_emitted_motion_line(preview,
                                                     z0_x,
                                                     stock_w,
                                                     stock_top,
                                                     stock_h,
                                                     line,
                                                     segment,
                                                     emitted_line == doc->cursor_line,
                                                     &last_x,
                                                     &last_z,
                                                     &have_last);
            drawn_lines++;
        } else if (emitted_line >= doc->line_count &&
                   nc_emit_stream_line(&stream) >= doc->line_count) {
            break;
        }
    }
}

static bool nc_visual_runtime_busy(const nc_runtime_state_t *runtime)
{
    return runtime && (runtime->exec_state & (EXEC_RUN | EXEC_HOLD));
}

static const char *nc_visual_run_state_text(const nc_runtime_state_t *runtime)
{
    if (g_nc_visual_mode != NC_MODE_RUN) {
        return nc_menu_mode_name(g_nc_visual_mode);
    }
    if (nc_run_hold() || (runtime && (runtime->exec_state & EXEC_HOLD))) {
        return "RUN HOLD";
    }
    if (nc_visual_runtime_busy(runtime) || nc_run_active()) {
        return "RUN ACTIVE";
    }
    if (nc_run_done()) {
        return "RUN IDLE";
    }
    return "RUN";
}

static const char *nc_visual_file_basename(const char *path)
{
    const char *slash;
    const char *backslash;

    if (!path || !path[0]) {
        return "(no file)";
    }
    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash)) {
        slash = backslash;
    }
    return slash ? slash + 1 : path;
}

static float nc_live_runtime_x_to_diam(float runtime_x)
{
    return nc_visual_absf(runtime_x) * 2.0f;
}

static bool nc_visual_live_tool_rect(const nc_preview_info_t *preview,
                                     const nc_runtime_state_t *runtime,
                                     int z0_x,
                                     int stock_w,
                                     int stock_top,
                                     int stock_h,
                                     int *rx,
                                     int *ry,
                                     int *rw,
                                     int *rh)
{
    int sx;
    int sy;
    int pad = 16;
    int min_x;
    int max_x;
    int min_y;
    int max_y;
    int x0;
    int y0;
    int x1;
    int y1;

    if (!preview || !runtime) {
        return false;
    }

    sx = nc_visual_preview_z(preview, z0_x, stock_w, runtime->z);
    sy = nc_visual_preview_x(preview, stock_top, stock_h, nc_live_runtime_x_to_diam(runtime->x));
    sx = nc_visual_clampi(sx, 0, LVDS_HSTX_WIDTH - 1);
    sy = nc_visual_clampi(sy, 44, LVDS_HSTX_HEIGHT - 1);

    min_x = 0;
    max_x = nc_visual_clampi(z0_x + stock_w + pad, 0, LVDS_HSTX_WIDTH - 1);
    min_y = nc_visual_clampi(stock_top - pad, 44, LVDS_HSTX_HEIGHT - 1);
    max_y = nc_visual_clampi(stock_top + stock_h + pad, 44, LVDS_HSTX_HEIGHT - 1);
    x0 = nc_visual_clampi(sx - pad, min_x, max_x);
    y0 = nc_visual_clampi(sy - pad, min_y, max_y);
    x1 = nc_visual_clampi(sx + pad, min_x, max_x);
    y1 = nc_visual_clampi(sy + pad, min_y, max_y);

    if (rx) *rx = x0;
    if (ry) *ry = y0;
    if (rw) *rw = x1 - x0 + 1;
    if (rh) *rh = y1 - y0 + 1;
    return true;
}

static void nc_visual_draw_live_tool(const nc_preview_info_t *preview,
                                     const nc_runtime_state_t *runtime,
                                     int z0_x,
                                     int stock_w,
                                     int stock_top,
                                     int stock_h,
                                     int pane_x,
                                     int pane_y,
                                     int pane_w,
                                     int pane_h,
                                     const nc_tool_t *tool)
{
    int sx;
    int sy;
    int rx;
    int ry;
    int rw;
    int rh;

    if (!preview || !runtime || !tool || !tool->valid) {
        return;
    }

    sx = nc_visual_preview_z(preview, z0_x, stock_w, runtime->z);
    sy = nc_visual_preview_x(preview, stock_top, stock_h, nc_live_runtime_x_to_diam(runtime->x));
    /* Inside the pane or not at all. A marker clamped to the whole screen is
       drawn over the header, the code pane or the footer when the axis is out
       of view - and because only the pane is redrawn every frame, that marker
       stayed there: the "cursor drawn but not cleared". Out of view, the DRO
       says where the axis is; the preview does not guess. */
    if (sx < pane_x || sx + NC_LIVE_TOOL_GLYPH > pane_x + pane_w ||
        sy < pane_y || sy + NC_LIVE_TOOL_GLYPH > pane_y + pane_h) {
        return;
    }

    if (nc_visual_live_tool_rect(preview, runtime, z0_x, stock_w, stock_top, stock_h, &rx, &ry, &rw, &rh)) {
        g_nc_live_tool_rect_x = rx;
        g_nc_live_tool_rect_y = ry;
        g_nc_live_tool_rect_w = rw;
        g_nc_live_tool_rect_h = rh;
        g_nc_live_tool_rect_valid = true;
    }
    nc_visual_draw_tool_glyph(sx, sy, NC_LIVE_TOOL_GLYPH, tool, NC_VISUAL_PREVIEW_BG, false);
}

static bool nc_live_stock_alloc(void)
{
    if (g_nc_live_stock_mask) {
        return true;
    }
#if NC_VISUAL_HAVE_PSRAM
    if (!lvds_psram_available()) {
        (void)lvds_psram_init();
    }
    if (lvds_psram_available()) {
        g_nc_live_stock_mask = (uint8_t *)lvds_psram_ptr(NC_LIVE_STOCK_PSRAM_OFFSET);
    }
#endif
    return g_nc_live_stock_mask != NULL;
}

static bool nc_live_stock_context_changed(const nc_preview_info_t *preview,
                                          int stock_w,
                                          int stock_h)
{
    if (!preview) {
        return true;
    }
    return !g_nc_live_stock_ready ||
           g_nc_live_stock_w != stock_w ||
           g_nc_live_stock_h != stock_h ||
           g_nc_live_stock_setup_x != preview->stock_x ||
           g_nc_live_stock_setup_z != preview->stock_visible_z ||
           g_nc_live_stock_setup_i != preview->stock_i;
}

static void nc_live_stock_reset(const nc_preview_info_t *preview,
                                int stock_w,
                                int stock_h)
{
    int material_top = 0;
    int y;

    g_nc_live_stock_ready = false;
    g_nc_live_stock_has_last = false;
    if (!preview || !nc_live_stock_alloc()) {
        return;
    }

    stock_w = nc_visual_clampi(stock_w, 1, NC_LIVE_STOCK_MAX_W);
    stock_h = nc_visual_clampi(stock_h, 1, NC_LIVE_STOCK_MAX_H);
    memset(g_nc_live_stock_mask, 0, (size_t)NC_LIVE_STOCK_MAX_W * NC_LIVE_STOCK_MAX_H);
    if (preview->stock_i > 0.0f && preview->stock_x > 0.0f) {
        material_top = nc_visual_clampi((int)((preview->stock_i / preview->stock_x) * (float)stock_h),
                                        0,
                                        stock_h - 1);
    }
    for (y = material_top; y < stock_h; y++) {
        memset(g_nc_live_stock_mask + ((size_t)y * NC_LIVE_STOCK_MAX_W), 1, (size_t)stock_w);
    }

    g_nc_live_stock_w = stock_w;
    g_nc_live_stock_h = stock_h;
    g_nc_live_stock_setup_x = preview->stock_x;
    g_nc_live_stock_setup_z = preview->stock_visible_z;
    g_nc_live_stock_setup_i = preview->stock_i;
    g_nc_live_stock_ready = true;
}

static void nc_live_stock_remove_rect(int x0, int y0, int x1, int y1)
{
    int y;

    if (!g_nc_live_stock_mask || !g_nc_live_stock_ready) {
        return;
    }
    x0 = nc_visual_clampi(x0, 0, g_nc_live_stock_w - 1);
    x1 = nc_visual_clampi(x1, 0, g_nc_live_stock_w - 1);
    y0 = nc_visual_clampi(y0, 0, g_nc_live_stock_h - 1);
    y1 = nc_visual_clampi(y1, 0, g_nc_live_stock_h - 1);
    if (x1 < x0) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y1 < y0) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    for (y = y0; y <= y1; y++) {
        memset(g_nc_live_stock_mask + ((size_t)y * NC_LIVE_STOCK_MAX_W) + x0,
               0,
               (size_t)(x1 - x0 + 1));
    }
}

static void nc_live_stock_cut_sweep(const nc_preview_info_t *preview,
                                    int z0_x,
                                    int stock_left,
                                    int stock_w,
                                    int stock_top,
                                    int stock_h,
                                    float x0,
                                    float z0,
                                    float x1,
                                    float z1)
{
    int sx0;
    int sx1;
    int sy0;
    int sy1;
    int samples;
    int i;

    if (!preview || !g_nc_live_stock_ready) {
        return;
    }
    sx0 = nc_visual_preview_z(preview, z0_x, stock_w, z0) - stock_left;
    sx1 = nc_visual_preview_z(preview, z0_x, stock_w, z1) - stock_left;
    sy0 = nc_visual_preview_x(preview, stock_top, stock_h, x0) - stock_top;
    sy1 = nc_visual_preview_x(preview, stock_top, stock_h, x1) - stock_top;
    samples = nc_visual_absf((float)(sx1 - sx0)) > nc_visual_absf((float)(sy1 - sy0)) ?
              nc_visual_absf((float)(sx1 - sx0)) :
              nc_visual_absf((float)(sy1 - sy0));
    samples = nc_visual_clampi(samples, 1, 80);
    for (i = 0; i <= samples; i++) {
        float t = (float)i / (float)samples;
        int sx = sx0 + (int)((float)(sx1 - sx0) * t);
        int sy = sy0 + (int)((float)(sy1 - sy0) * t);
        nc_live_stock_remove_rect(sx - 1, sy, sx + 1, g_nc_live_stock_h - 1);
    }
}

static void nc_live_stock_update(const nc_preview_info_t *preview,
                                 const nc_runtime_state_t *runtime,
                                 int z0_x,
                                 int stock_left,
                                 int stock_w,
                                 int stock_top,
                                 int stock_h)
{
    float diam_x;

    if (!preview || !runtime || !g_nc_live_stock_ready) {
        return;
    }
    diam_x = nc_live_runtime_x_to_diam(runtime->x);
    if (g_nc_live_stock_has_last) {
        nc_live_stock_cut_sweep(preview,
                                z0_x,
                                stock_left,
                                stock_w,
                                stock_top,
                                stock_h,
                                g_nc_live_stock_last_x,
                                g_nc_live_stock_last_z,
                                diam_x,
                                runtime->z);
    } else {
        nc_live_stock_cut_sweep(preview,
                                z0_x,
                                stock_left,
                                stock_w,
                                stock_top,
                                stock_h,
                                diam_x,
                                runtime->z,
                                diam_x,
                                runtime->z);
    }
    g_nc_live_stock_last_x = diam_x;
    g_nc_live_stock_last_z = runtime->z;
    g_nc_live_stock_has_last = true;
}

static void nc_live_stock_draw(int stock_left,
                               int stock_top,
                               int clip_x,
                               int clip_y,
                               int clip_w,
                               int clip_h)
{
    int y0;
    int y1;
    int y;
    int clip_left = clip_x - stock_left;
    int clip_right = clip_left + clip_w - 1;

    if (!g_nc_live_stock_mask || !g_nc_live_stock_ready) {
        return;
    }
    clip_left = nc_visual_clampi(clip_left, 0, g_nc_live_stock_w - 1);
    clip_right = nc_visual_clampi(clip_right, 0, g_nc_live_stock_w - 1);
    y0 = nc_visual_clampi(clip_y - stock_top, 0, g_nc_live_stock_h - 1);
    y1 = nc_visual_clampi(clip_y + clip_h - stock_top - 1, 0, g_nc_live_stock_h - 1);
    if (clip_right < clip_left || y1 < y0) {
        return;
    }
    lvds_draw_fill_rect(stock_left + clip_left,
                        stock_top + y0,
                        clip_right - clip_left + 1,
                        y1 - y0 + 1,
                        NC_VISUAL_PREVIEW_BG);
    for (y = y0; y <= y1; y++) {
        const uint8_t *row = g_nc_live_stock_mask + ((size_t)y * NC_LIVE_STOCK_MAX_W);
        int x = clip_left;
        while (x <= clip_right) {
            int start;
            while (x <= clip_right && !row[x]) {
                x++;
            }
            start = x;
            while (x <= clip_right && row[x]) {
                x++;
            }
            if (x > start) {
                lvds_draw_fill_rect(stock_left + start,
                                    stock_top + y,
                                    x - start,
                                    1,
                                    NC_VISUAL_PREVIEW_STOCK);
            }
        }
    }
}

static bool nc_visual_draw_live_stock(const nc_preview_info_t *preview,
                                      const nc_runtime_state_t *runtime,
                                      const nc_tool_t *tool,
                                      int stock_left,
                                      int stock_top,
                                      int stock_w,
                                      int stock_h,
                                      int z0_x,
                                      int tool_panel_x,
                                      int tool_panel_y,
                                      int tool_panel_w,
                                      int pane_h,
                                      bool draw_static_panel)
{
    bool live_run;
    bool context_changed;
    bool retain_stock;
    int clear_y;
    int clear_h;
    uint32_t t0;
    uint32_t t1;
    uint32_t t2;
    uint32_t t3;
    uint32_t t4;

    live_run = nc_visual_runtime_busy(runtime) || nc_run_active();

    if (!preview || !runtime) {
        g_nc_live_stock_was_active = false;
        g_nc_live_tool_rect_valid = false;
        return false;
    }
    context_changed = nc_live_stock_context_changed(preview, stock_w, stock_h);
    retain_stock = g_nc_visual_mode == NC_MODE_RUN &&
                   g_nc_live_stock_ready &&
                   !context_changed;
    if (!live_run && !retain_stock) {
        g_nc_live_stock_was_active = false;
        g_nc_live_tool_rect_valid = false;
        return false;
    }
    if (live_run && (!g_nc_live_stock_was_active || context_changed)) {
        nc_live_stock_reset(preview, stock_w, stock_h);
        g_nc_live_tool_rect_valid = false;
    }
    g_nc_live_stock_was_active = live_run || retain_stock;
    if (!g_nc_live_stock_ready) {
        nc_visual_draw_text_clip(stock_left,
                                 stock_top + 16,
                                 "Live stock needs PSRAM",
                                 28,
                                 NC_VISUAL_ERROR,
                                 NC_VISUAL_PREVIEW_BG,
                                 LVDS_FONT_NORMAL);
        return true;
    }
    t0 = mcu_micros();
    clear_y = nc_visual_clampi(stock_top - 24, 44, NC_FOOTER_Y - 1);
    clear_h = nc_visual_clampi(stock_top + stock_h + 74 - clear_y, 1, NC_FOOTER_Y - clear_y);
    lvds_draw_fill_rect(tool_panel_x, clear_y, tool_panel_w, clear_h, NC_VISUAL_PREVIEW_BG);
    t1 = mcu_micros();
    if (live_run) {
        nc_live_stock_update(preview, runtime, z0_x, stock_left, stock_w, stock_top, stock_h);
    }
    nc_visual_draw_chuck(preview, stock_left, stock_top, stock_w, stock_h);
    nc_visual_draw_chuck_relief(preview, stock_left, stock_top, stock_h);
    nc_live_stock_draw(stock_left, stock_top, stock_left, stock_top, stock_w, stock_h);
    t2 = mcu_micros();
#if NC_PREVIEW_DIN_STYLE
    if (g_nc_visual_show_dims) {
        nc_visual_draw_din_layer(preview, stock_left, stock_top, stock_w, stock_h, z0_x);
    }
#endif
    if (g_nc_visual_show_dims) {
        nc_visual_draw_contour_points(&g_nc_visual_doc,
                                          preview,
                                          z0_x,
                                          stock_left,
                                          stock_left + stock_w,
                                          stock_w,
                                          stock_top,
                                          stock_h);
    }
#if !NC_PREVIEW_DIN_STYLE
    lvds_draw_line(z0_x, stock_top - 18, z0_x, stock_top + stock_h + 18, NC_VISUAL_DIM);
    lvds_draw_text(z0_x - 12, stock_top - 34, "Z0", NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_NORMAL);
#endif
    t3 = mcu_micros();
    nc_visual_draw_live_tool(preview, runtime, z0_x, stock_w, stock_top, stock_h,
                             tool_panel_x, tool_panel_y, tool_panel_w, pane_h, tool);
    if (draw_static_panel) {
        nc_visual_draw_preview_tool_panel(tool_panel_x,
                                          tool_panel_y,
                                          tool_panel_w,
                                          58,
                                          tool);
    }
    t4 = mcu_micros();
    g_nc_visual_frame_preview_clear_us += t1 - t0;
    g_nc_visual_frame_preview_stock_us += t2 - t1;
    g_nc_visual_frame_preview_geom_us += t3 - t2;
    g_nc_visual_frame_preview_tool_us += t4 - t3;
    return true;
}

static void nc_visual_draw_thin_preview(const nc_document_t *doc,
                                        const nc_runtime_state_t *runtime,
                                        int x,
                                        int y,
                                        int w,
                                        int h,
                                        bool clear_bg)
{
    nc_preview_info_t preview;
    int stock_w;
    int stock_h;
    int stock_left;
    int stock_top;
    int z0_x;
    float usable_w;
    float usable_h;
    float z_scale;
    float x_scale;
    float scale;
    nc_tool_t tool;
    bool have_tool = false;
    size_t tool_line = doc && doc->line_count ? doc->cursor_line : 0;
    uint32_t t0 = mcu_micros();
    uint32_t t1;
    uint32_t t2;
    uint32_t t3;
    uint32_t t5;
    bool use_live_cache = !clear_bg &&
                          g_nc_visual_mode == NC_MODE_RUN &&
                          g_nc_live_preview_cache_valid;

    if (doc && doc->path[0] && !nc_path_supported(doc->path)) {
        /* Text is not a program. The editor shows it; the preview must not read
           it as G-code - the preset file is the file this is for. */
        if (clear_bg) {
            lvds_draw_fill_rect(x, y, w, h, NC_VISUAL_PREVIEW_BG);
        }
        nc_visual_draw_text_clip(x + 12, y + 12, "Text file - no preview", 24,
                                 NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG,
                                 LVDS_FONT_NORMAL);
        return;
    }

    if (doc && g_nc_visual_mode == NC_MODE_RUN && nc_run_line() < doc->line_count) {
        tool_line = nc_run_line();
    }

    if (use_live_cache) {
        preview = g_nc_live_preview_cache;
        tool = g_nc_live_tool_cache;
        have_tool = g_nc_live_have_tool_cache;
        stock_w = g_nc_live_stock_w_cache;
        stock_h = g_nc_live_stock_h_cache;
        stock_left = g_nc_live_stock_left_cache;
        stock_top = g_nc_live_stock_top_cache;
        z0_x = g_nc_live_z0_x_cache;
        t1 = t0;
        t2 = t0;
        t3 = t0;
    } else {
        nc_preview_collect(doc, &preview);
        t1 = mcu_micros();
        if (clear_bg) {
            lvds_draw_fill_rect(x, y, w, h, NC_VISUAL_PREVIEW_BG);
            /* No caption: the tab strip carries the screen name and the header
               carries the file or the run state. */
        }
        t2 = mcu_micros();

        /* Content box inside the pane: the stock starts below a short band at
           the top (the caption that used to live there is gone - the tab strip
           says the screen and the header says the file) and the drawing stops
           short of the pane's own bottom edge, so no part of the preview -
           stock, chuck or a dimension label - can be drawn over the footer
           strip below it. SIM keeps a little more room under the stock for its
           labels. */
        usable_w = (float)(w - 72);
        usable_h = (float)(h - NC_PREVIEW_TOP_BAND -
                           (nc_visual_full_preview() ? 28 : 8));
        if (usable_w < 40.0f) usable_w = 40.0f;
        if (usable_h < 40.0f) usable_h = 40.0f;
        z_scale = usable_w / preview.stock_visible_z;
        x_scale = usable_h / (preview.stock_x * 0.5f);
        scale = z_scale < x_scale ? z_scale : x_scale;
        if (scale <= 0.0f) scale = 1.0f;
        stock_w = (int)(preview.stock_visible_z * scale + 0.5f);
        stock_h = (int)((preview.stock_x * 0.5f) * scale + 0.5f);
        if (stock_w < 24) stock_w = 24;
        if (stock_h < 24) stock_h = 24;
        if (stock_w > (int)usable_w) stock_w = (int)usable_w;
        if (stock_h > (int)usable_h) stock_h = (int)usable_h;

        stock_left = x + 20;
        stock_top = y + NC_PREVIEW_TOP_BAND;
        z0_x = stock_left + (int)(preview.stock_z * scale + 0.5f);
        if (z0_x < stock_left) z0_x = stock_left;
        if (z0_x > stock_left + stock_w) z0_x = stock_left + stock_w;
        memset(&tool, 0, sizeof(tool));
        if (doc && g_nc_visual_mode == NC_MODE_TOOLS &&
            nc_tool_active_from_table(doc, tool_line, &g_nc_visual_doc, &tool)) {
            have_tool = true;
        } else if (doc && nc_tool_active_from_file(doc,
                                                   tool_line,
                                                   nc_visual_tool_path(),
                                                   &tool)) {
            have_tool = true;
        }
        t3 = mcu_micros();
        if (g_nc_visual_mode == NC_MODE_RUN) {
            g_nc_live_preview_cache = preview;
            g_nc_live_tool_cache = tool;
            g_nc_live_have_tool_cache = have_tool;
            g_nc_live_stock_w_cache = stock_w;
            g_nc_live_stock_h_cache = stock_h;
            g_nc_live_stock_left_cache = stock_left;
            g_nc_live_stock_top_cache = stock_top;
            g_nc_live_z0_x_cache = z0_x;
            g_nc_live_preview_cache_valid = true;
        }
    }
    g_nc_visual_frame_preview_collect_us += (t1 - t0) + (t3 - t2);
    g_nc_visual_frame_preview_clear_us += t2 - t1;
    if (g_nc_visual_mode == NC_MODE_RUN && nc_visual_draw_live_stock(&preview,
                                                                      runtime,
                                                                      have_tool ? &tool : NULL,
                                                                      stock_left,
                                                                      stock_top,
                                                                      stock_w,
                                                                      stock_h,
                                                                      z0_x,
                                                                      x,
                                                                      y,
                                                                      w,
                                                                      h,
                                                                      clear_bg)) {
        return;
    }

    t0 = mcu_micros();
    nc_visual_draw_chuck(&preview, stock_left, stock_top, stock_w, stock_h);
    nc_visual_draw_chuck_relief(&preview, stock_left, stock_top, stock_h);
    if (g_nc_visual_show_stock || !nc_visual_full_preview()) {
        lvds_draw_fill_rect(stock_left, stock_top, stock_w, stock_h, NC_VISUAL_PREVIEW_STOCK);
        if (preview.stock_i > 0.0f) {
            int id_h = nc_visual_preview_x(&preview, stock_top, stock_h, preview.stock_i) - stock_top;
            if (id_h > 0 && id_h < stock_h) {
                lvds_draw_fill_rect(stock_left, stock_top, stock_w, id_h, NC_VISUAL_PREVIEW_BG);
            }
        }
    }
    t1 = mcu_micros();
#if NC_PREVIEW_DIN_STYLE
    if (g_nc_visual_show_dims) {
        nc_visual_draw_din_layer(&preview, stock_left, stock_top, stock_w, stock_h, z0_x);
    }
#endif
    if (g_nc_visual_show_dims) {
        nc_visual_draw_contour_points(doc,
                                          &preview,
                                          z0_x,
                                          stock_left,
                                          stock_left + stock_w,
                                          stock_w,
                                          stock_top,
                                          stock_h);
    }
#if !NC_PREVIEW_DIN_STYLE
    lvds_draw_line(z0_x, stock_top - 18, z0_x, stock_top + stock_h + 18, NC_VISUAL_DIM);
    lvds_draw_text(z0_x - 12, stock_top - 34, "Z0", NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_NORMAL);
#endif
    t2 = mcu_micros();
    if (!nc_visual_full_preview()) {
        nc_visual_draw_preview_tool_panel(x, y, w, h, have_tool ? &tool : NULL);
    }
    t3 = mcu_micros();
    nc_visual_draw_emitted_preview(doc,
                                   &preview,
                                   z0_x,
                                   stock_w,
                                   stock_top,
                                   stock_h,
                                   96u);
    t5 = mcu_micros();
    if (have_tool && !nc_visual_full_preview()) {
        nc_visual_draw_live_tool(&preview, runtime, z0_x, stock_w, stock_top,
                                 stock_h, x, y, w, h, &tool);
        nc_visual_draw_preview_tool_panel(x, y, w, h, &tool);
    }
    g_nc_visual_frame_preview_stock_us += t1 - t0;
    g_nc_visual_frame_preview_geom_us += (t2 - t1) + (t5 - t3);
    g_nc_visual_frame_preview_tool_us += (t3 - t2) + (mcu_micros() - t5);
}

/* Footer entries that are switches (not actions). Their highlight must show the
   state, otherwise "last key pressed" and "feature on" look identical and the
   entry seems permanently lit. */
static bool nc_visual_footer_item_is_toggle(nc_footer_action_t action)
{
    return action == NC_FOOTER_ACTION_DIMS ||
           action == NC_FOOTER_ACTION_STOCK ||
           action == NC_FOOTER_ACTION_PATH ||
           action == NC_FOOTER_ACTION_ROUGH ||
           action == NC_FOOTER_ACTION_VIEW;
}

static bool nc_visual_footer_item_on(nc_footer_action_t action)
{
    switch (action) {
    case NC_FOOTER_ACTION_DIMS: return g_nc_visual_show_dims;
    case NC_FOOTER_ACTION_STOCK: return g_nc_visual_show_stock;
    case NC_FOOTER_ACTION_PATH: return g_nc_visual_show_path;
    case NC_FOOTER_ACTION_ROUGH: return g_nc_visual_show_rough;
    case NC_FOOTER_ACTION_VIEW: return g_nc_visual_show_code;
    default: return false;
    }
}

/* Greedy word wrap for the footer keys: `text` onto at most NC_FOOTER_LINES
   lines of `cols` columns. A word longer than the line is split. */
static int nc_visual_wrap_lines(const char *text,
                                int cols,
                                char lines[NC_FOOTER_LINES][24])
{
    int count = 0;
    const char *p = text;

    if (!text || cols < 1) {
        return 0;
    }
    while (*p && count < NC_FOOTER_LINES) {
        const char *start = p;
        const char *wrap_at = 0;
        int len = 0;

        while (p[len] && len < cols) {
            if (p[len] == ' ') {
                wrap_at = p + len;
            }
            len++;
        }
        if (p[len] && wrap_at) {
            len = (int)(wrap_at - start);
            p = wrap_at + 1;
        } else {
            p = start + len;
        }
        if (len > 23) {
            len = 23;
        }
        memcpy(lines[count], start, (size_t)len);
        lines[count][len] = '\0';
        count++;
        while (*p == ' ') {
            p++;
        }
    }
    return count;
}

static void nc_visual_footer_text(char *out, size_t out_sz)
{
    size_t count;
    size_t i;
    size_t used = 0;
    int fields = 0;
    const nc_footer_item_t *footer = nc_visual_footer_items(&count);

    if (!out || out_sz == 0) {
        return;
    }

    out[0] = '\0';
    if (!nc_files_active() &&
        nc_visual_can_edit_code() &&
        g_nc_visual_doc.selected_word >= 0) {
        /* A word is selected: the keys work on the word, not on the screen - in
           EDIT too, where `#` is the accept key while a value is open and the
           full-screen toggle when nothing is being edited. The strip says which
           of the two it is at the moment the key is pressed. */
        snprintf(out, out_sz, "B UP|C DOWN|D NEXT|# OK|* DEL");
        used = strlen(out);
        fields = 5;
    }

    if (fields == 0) {
        for (i = 0; i < count && fields < NC_FOOTER_SLOTS; i++) {
            int n;

            if (!footer[i].label[0]) {
                /* An empty slot: the separator is still written so the keys
                   around it keep the place the physical buttons have. */
                n = snprintf(out + used, out_sz - used, "%s", fields ? "|" : "");
            } else {
                n = snprintf(out + used,
                             out_sz - used,
                             "%s%s%c %s",
                             fields ? "|" : "",
                             (nc_visual_footer_item_is_toggle(footer[i].action) ?
                                  nc_visual_footer_item_on(footer[i].action) :
                                  (footer[i].action != NC_FOOTER_ACTION_NONE &&
                                   footer[i].action == g_nc_visual_selected_action)) ? "!" : "",
                             footer[i].key,
                             footer[i].label);
            }
            if (n < 0 || (size_t)n >= out_sz - used) {
                out[out_sz - 1] = '\0';
                return;
            }
            used += (size_t)n;
            fields++;
        }
    }
    /* Keep the strip the same width on every screen: the entries a screen does
       not use are empty slots, not a wider key. */
    while (fields < NC_FOOTER_SLOTS) {
        int n = snprintf(out + used, out_sz - used, "%s", fields ? "|" : "");

        if (n < 0 || (size_t)n >= out_sz - used) {
            out[out_sz - 1] = '\0';
            return;
        }
        used += (size_t)n;
        fields++;
    }
}

static void nc_visual_draw_footer_status(const char *message, const char *footer_text)
{
    char field[24];
    const char *p = footer_text ? footer_text : "";
    int fields = 1;
    int field_w;
    int i;

    (void)message;

    if (!p[0]) {
        return;
    }

    for (i = 0; p[i]; i++) {
        if (p[i] == '|') {
            fields++;
        }
    }
    if (fields < 1) {
        fields = 1;
    }
    field_w = (LVDS_HSTX_WIDTH - 12) / fields;

    for (i = 0; i < fields; i++) {
        const char *bar = strchr(p, '|');
        const char *space;
        char key[8];
        const char *label;
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        bool active = false;
        /* Keys keep a small gap between them. The bottom few rows of the panel
           are left as page background: the close of the frame does not come out
           clean on the glass there (see nc/TODO.md), and content in those rows
           shows as a broken edge. */
        int bx = 6 + i * field_w + 1;
        int bw = field_w - 3;
        int bh = NC_FOOTER_H - 4;
        int by = NC_FOOTER_Y;
        lvds_color_t button_bg = NC_VISUAL_FOOTER_BUTTON;
        lvds_color_t button_fg = NC_VISUAL_FOOTER_TEXT;

        if (len >= sizeof(field)) {
            len = sizeof(field) - 1;
        }
        memcpy(field, p, len);
        field[len] = '\0';
        if (field[0] == '!') {
            active = true;
            memmove(field, field + 1, strlen(field));
        }
        if (active) {
            button_bg = NC_VISUAL_FOOTER_VALUE;
        }

        space = strchr(field, ' ');
        if (space) {
            size_t key_len = (size_t)(space - field);
            if (key_len >= sizeof(key)) {
                key_len = sizeof(key) - 1;
            }
            memcpy(key, field, key_len);
            key[key_len] = '\0';
            label = space + 1;
        } else {
            key[0] = '\0';
            label = field;
        }

        if (!label[0]) {
            p = bar ? (bar + 1) : "";
            continue;
        }

        lvds_draw_fill_rect(bx, by, bw, bh, button_bg);
        lvds_draw_rect(bx, by, bw, bh, NC_VISUAL_DIM);

        {
            /* Footer keys are the same white keys as the 3x3 helper: the
               number sits in the top-left corner and the label has room for up
               to three wrapped lines. */
            char lines[NC_FOOTER_LINES][24];
            int indent = 4;
            int cols;
            int line_h = NC_FONT_NORMAL_H + 2;
            int block_y;
            int n;
            int j;

            if (key[0]) {
                lvds_draw_text(bx + 4, by + 3, key, NC_VISUAL_ACCENT, button_bg,
                               LVDS_FONT_NORMAL);
                indent += lvds_draw_text_width(key, LVDS_FONT_NORMAL) + 6;
            }
            cols = (bw - indent - 4) / NC_VISUAL_CHAR_W;
            n = nc_visual_wrap_lines(label, cols, lines);
            if (n > 0) {
                block_y = by + (bh - n * line_h) / 2;
                for (j = 0; j < n; j++) {
                    nc_visual_draw_text_clip(bx + indent,
                                             block_y + j * line_h,
                                             lines[j],
                                             (int)strlen(lines[j]),
                                             button_fg,
                                             button_bg,
                                             LVDS_FONT_NORMAL);
                }
            }
        }

        p = bar ? (bar + 1) : "";
    }
}

static char nc_visual_key_char(nc_visual_key_t key)
{
    if (key >= NC_VISUAL_KEY_DIGIT_0 && key <= NC_VISUAL_KEY_DIGIT_9) {
        return (char)('0' + (key - NC_VISUAL_KEY_DIGIT_0));
    }

    switch (key) {
    case NC_VISUAL_KEY_BACKSPACE: return '*';
    case NC_VISUAL_KEY_FINISH: return '#';
    case NC_VISUAL_KEY_CANCEL: return 'A';
    case NC_VISUAL_KEY_PREV: return 'B';
    case NC_VISUAL_KEY_NEXT: return 'C';
    case NC_VISUAL_KEY_ACCEPT: return 'D';
    case NC_VISUAL_KEY_MINUS: return '-';
    case NC_VISUAL_KEY_DOT: return '.';
    default: return '\0';
    }
}

static const char *nc_visual_new_file_ext(void)
{
    return g_nc_visual_mode == NC_MODE_TOOLS ? ".t" : ".nc";
}

static void nc_visual_new_file_status(void)
{
    snprintf(g_nc_visual_status,
             sizeof(g_nc_visual_status),
             "New:%.40s%s #OK *DEL A",
             g_nc_visual_new_file_name[0] ? g_nc_visual_new_file_name : "_",
             nc_visual_new_file_ext());
}

static bool nc_visual_new_file_handle_key(nc_visual_key_t key)
{
    char ch = nc_visual_key_char(key);
    size_t len;
    char path[NC_PATH_MAX];
    nc_result_t r;

    if (!g_nc_visual_new_file_active) {
        return false;
    }

    if (key == NC_VISUAL_KEY_CANCEL) {
        g_nc_visual_new_file_active = false;
        strncpy(g_nc_visual_status, "New file cancelled", sizeof(g_nc_visual_status) - 1);
        return true;
    }
    if (key == NC_VISUAL_KEY_FINISH || key == NC_VISUAL_KEY_ACCEPT) {
        /* The new file replaces the open one in the same buffer: save it
           first, the same way a screen change does. */
        if (!nc_visual_save_current_if_file() &&
            !nc_visual_proceed_without_saving(NC_VISUAL_UNSAVED_NEW)) {
            strncpy(g_nc_visual_status, "Save failed - press again to create", sizeof(g_nc_visual_status) - 1);
            return true;
        }
        if (!g_nc_visual_new_file_name[0] ||
            !nc_files_create_named(g_nc_visual_new_file_name, nc_visual_new_file_ext(), path, sizeof(path))) {
            strncpy(g_nc_visual_status, "New file create failed", sizeof(g_nc_visual_status) - 1);
            return true;
        }
        r = nc_load_file(&g_nc_visual_doc, path);
        if (r != NC_OK) {
            snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Create open failed: %s", nc_result_text(r));
            return true;
        }
        g_nc_visual_new_file_active = false;
        nc_files_set_active(false);
        nc_state_remember_path(g_nc_visual_mode, path);
        nc_state_save();
        snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Created %s", nc_visual_file_basename(path));
        return true;
    }
    if (key == NC_VISUAL_KEY_BACKSPACE) {
        len = strlen(g_nc_visual_new_file_name);
        if (len > 0u) {
            g_nc_visual_new_file_name[len - 1u] = '\0';
        }
        nc_visual_new_file_status();
        return true;
    }
    if (ch >= '0' && ch <= '9') {
        len = strlen(g_nc_visual_new_file_name);
        if (len + 1u < sizeof(g_nc_visual_new_file_name)) {
            g_nc_visual_new_file_name[len] = ch;
            g_nc_visual_new_file_name[len + 1u] = '\0';
        }
        nc_visual_new_file_status();
        return true;
    }

    nc_visual_new_file_status();
    return true;
}

static uint8_t nc_visual_footer_action_for_key(nc_visual_key_t key)
{
    size_t count;
    size_t i;
    char key_char = nc_visual_key_char(key);
    const nc_footer_item_t *footer = nc_visual_footer_items(&count);

    if (!key_char) {
        return NC_FOOTER_ACTION_NONE;
    }

    for (i = 0; i < count; i++) {
        if (footer[i].key == key_char) {
            return footer[i].action;
        }
    }

    return NC_FOOTER_ACTION_NONE;
}

static void nc_visual_run_arm(size_t line, const char *label)
{
    /* A text file is text: it opens in the editor, it is not a program to read
       as G-code or to run. */
    if (!nc_visual_document_is_program()) {
        return;
    }
    if (!nc_run_arm(&g_nc_visual_doc, line)) {
        strncpy(g_nc_visual_status, "RUN needs a program", sizeof(g_nc_visual_status) - 1);
        return;
    }

    g_nc_visual_doc.cursor_line = nc_run_line();
    g_nc_visual_doc.selected_word = -1;
    nc_visual_set_mode(NC_MODE_RUN);
    snprintf(g_nc_visual_status,
             sizeof(g_nc_visual_status),
             "%s line %lu",
             label,
             (unsigned long)(nc_run_line() + 1));
}

static void nc_visual_run_step(void)
{
    size_t line = g_nc_visual_doc.cursor_line;
    g_nc_visual_command_error[0] = '\0';

    if (!nc_visual_document_is_program()) {
        return;
    }
    if (cnc_get_exec_state(EXEC_GCODE_LOCKED) || cnc_has_alarm()) {
        snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "RUN locked: check controller status (?)");
        grbl_stream_printf("[MSG:NC RUN locked state=%u alarm=%u; check ?]\r\n",
                           cnc_get_exec_state(EXEC_ALLACTIVE), (unsigned)cnc_has_alarm());
        return;
    }

    if (!nc_run_send_document_line(&g_nc_visual_doc, line)) {
        strncpy(g_nc_visual_status, "RUN line skipped", sizeof(g_nc_visual_status) - 1);
        return;
    }
    g_nc_visual_doc.cursor_line = nc_run_line();
    g_nc_visual_doc.selected_word = -1;
    snprintf(g_nc_visual_status,
             sizeof(g_nc_visual_status),
             "RUN sent line %lu",
             (unsigned long)(line + 1u));
}

static bool nc_visual_is_code_view(void)
{
    return g_nc_visual_mode == NC_MODE_PROGRAM ||
           g_nc_visual_mode == NC_MODE_RUN;
}

/* EDIT with the code hidden (`# FULL`): the preview has the whole body. This is
   what the SIM screen used to be - the same screen with the engine behind it,
   full size - so the preview's own keys and layers belong to this state. */
static bool nc_visual_full_preview(void)
{
    return g_nc_visual_mode == NC_MODE_PROGRAM &&
           !g_nc_visual_show_code &&
           !nc_files_active();
}

static bool nc_visual_uses_file(void)
{
    return nc_visual_is_code_view() ||
           g_nc_visual_mode == NC_MODE_TOOLS;
}

static bool nc_visual_can_edit_code(void)
{
    if (nc_visual_full_preview()) {
        /* The preview is the whole screen: the program is not being edited, so
           the editing keys are not live. Otherwise a stray digit or cursor key
           would move the selection - and even write a field - behind a drawing
           the operator cannot see it happen in. */
        return false;
    }
    return g_nc_visual_mode == NC_MODE_PROGRAM ||
           g_nc_visual_mode == NC_MODE_TOOLS;
}

static void nc_visual_move_tool_line(int delta)
{
    int selected_tool = nc_visual_selected_tool_index();
    int tool_count = nc_visual_find_tool_line(selected_tool, NULL);
    int selected_line = -1;

    if (tool_count == 0) {
        strncpy(g_nc_visual_status, "No tool rows", sizeof(g_nc_visual_status) - 1);
        return;
    }
    if (delta < 0 && selected_tool > 0) {
        selected_tool--;
    } else if (delta > 0 && selected_tool + 1 < tool_count) {
        selected_tool++;
    }
    (void)nc_visual_find_tool_line(selected_tool, &selected_line);
    if (selected_line >= 0) {
        g_nc_visual_doc.cursor_line = (size_t)selected_line;
        g_nc_visual_doc.selected_word = -1;
        nc_text_edit_clear(&g_nc_visual_edit);
        snprintf(g_nc_visual_status,
                 sizeof(g_nc_visual_status),
                 "Tool %d/%d",
                 selected_tool + 1,
                 tool_count);
        nc_visual_serial_selected_line();
    }
}

static void nc_visual_set_code_line(size_t line)
{
    if (g_nc_visual_doc.line_count == 0) {
        g_nc_visual_doc.cursor_line = 0;
        nc_run_set_line(&g_nc_visual_doc, 0);
        return;
    }
    if (line >= g_nc_visual_doc.line_count) {
        line = g_nc_visual_doc.line_count - 1;
    }
    g_nc_visual_doc.cursor_line = line;
    if (g_nc_visual_mode == NC_MODE_RUN) {
        nc_run_set_line(&g_nc_visual_doc, line);
    }
    g_nc_visual_doc.selected_word = -1;
    g_nc_visual_name_selected = false;
}

static size_t nc_visual_code_line(void)
{
    size_t line = g_nc_visual_mode == NC_MODE_RUN ? nc_run_line() : g_nc_visual_doc.cursor_line;

    return g_nc_visual_doc.line_count && line >= g_nc_visual_doc.line_count ? 0 : line;
}

static void nc_visual_move_code_line(int delta)
{
    size_t line = nc_visual_code_line();

    if (!nc_visual_is_code_view() ||
        g_nc_visual_doc.line_count == 0) {
        strncpy(g_nc_visual_status, "No code lines", sizeof(g_nc_visual_status) - 1);
        return;
    }
    /* Above line 1 sits the row naming the file this code belongs to. */
    if (delta < 0 && (g_nc_visual_name_selected || line == 0)) {
        g_nc_visual_name_selected = true;
        /* Drop the value selection: the movement keys must stay movement keys
           while the name row is the cursor. */
        g_nc_visual_doc.selected_word = -1;
        nc_text_edit_clear(&g_nc_visual_edit);
        g_nc_visual_status[0] = '\0';
        return;
    }
    if (delta > 0 && g_nc_visual_name_selected) {
        g_nc_visual_name_selected = false;
        g_nc_visual_status[0] = '\0';
        return;
    }
    if (delta < 0 && line > 0) {
        line--;
    } else if (delta > 0 && line + 1 < g_nc_visual_doc.line_count) {
        line++;
    }

    nc_visual_set_code_line(line);
    /* No position report: the editor numbers the lines and highlights this
       one. The message line stays for messages. */
    g_nc_visual_status[0] = '\0';
}

static nc_result_t nc_visual_insert_tool_ref(void)
{
    int tool_no = 1;
    char line[16];

    (void)nc_tool_number_for_line(&g_nc_visual_doc, g_nc_visual_doc.cursor_line + 1u, &tool_no);
    if (tool_no <= 0) {
        tool_no = 1;
    }
    snprintf(line, sizeof(line), "T%d", tool_no);
    return nc_insert_line(&g_nc_visual_doc, g_nc_visual_doc.cursor_line + 1u, line);
}

static void nc_visual_open_file_view(const char *root, bool seed_samples)
{
    int made = seed_samples ? nc_files_seed_samples() : 0;

    nc_files_set_active(true);
    g_nc_visual_new_file_active = false;
    if (nc_files_refresh(root)) {
        /* The pane header already shows the directory. */
        strncpy(g_nc_visual_status,
                made ? "Sample files created" : "",
                sizeof(g_nc_visual_status) - 1);
        /* Land on the file that is open, if it is in this folder. */
        (void)nc_files_select_path(g_nc_visual_doc.path);
        nc_files_preview_sync();
    } else {
        strncpy(g_nc_visual_status, "File list unavailable", sizeof(g_nc_visual_status) - 1);
    }
}

/* Enter on the file-name row shows the folder the open file lives in. */
static void nc_visual_open_current_folder(void)
{
    char dir[NC_PATH_MAX];
    const char *path = g_nc_visual_doc.path;
    const char *slash = 0;
    size_t i;
    size_t len = strlen(path);

    for (i = 0; i < len; i++) {
        if (path[i] == '/' || path[i] == '\\') {
            slash = path + i;
        }
    }
    if (!slash || slash == path) {
        nc_visual_open_file_view(NC_FILES_DIR, false);
        return;
    }
    snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
    nc_visual_open_file_view(dir, false);
}

/* Live preview of the file pointed at in the file list.

   Safety: the selected file is read into a scratch document, never the open
   one, and only when the selection changes - so browsing cannot disturb the
   program being edited and the card is not read on every frame. Directories and
   files that fail to load simply have no preview and the open program's picture
   stays. Nothing here is prepared for execution: the collector only reads
   geometry, and no machine state, arming or motion is touched. */
static nc_document_t g_nc_files_preview_doc;
static char g_nc_files_preview_path[NC_PATH_MAX];
static bool g_nc_files_preview_ok;

static void nc_files_preview_sync(void)
{
    char path[NC_PATH_MAX];

    if (!nc_files_active() || nc_files_selected_is_dir() ||
        !nc_files_selected_path(path, sizeof(path))) {
        g_nc_files_preview_path[0] = '\0';
        g_nc_files_preview_ok = false;
        return;
    }
    if (strcmp(path, g_nc_files_preview_path) == 0) {
        return;                       /* this file is already shown, or already
                                         failed - do not keep re-reading it */
    }
    nc_document_init(&g_nc_files_preview_doc);
    g_nc_files_preview_ok = nc_load_file(&g_nc_files_preview_doc, path) == NC_OK;
    strncpy(g_nc_files_preview_path, path, sizeof(g_nc_files_preview_path) - 1);
    g_nc_files_preview_path[sizeof(g_nc_files_preview_path) - 1] = '\0';
    g_nc_visual_dirty = true;
}

static void nc_visual_insert_preset_action(nc_preset_t preset,
                                           const char *ok,
                                           const char *fail)
{
    if (nc_insert_preset(&g_nc_visual_doc, preset) == NC_OK) {
        strncpy(g_nc_visual_status, ok, sizeof(g_nc_visual_status) - 1);
    } else {
        strncpy(g_nc_visual_status, fail, sizeof(g_nc_visual_status) - 1);
    }
}

static bool nc_visual_handle_selected_word_edit(nc_visual_key_t key)
{
    char key_char = nc_visual_key_char(key);

    if (nc_files_active() ||
        !nc_visual_can_edit_code() ||
        g_nc_visual_doc.selected_word < 0) {
        return false;
    }

    if (nc_text_edit_handle_key(&g_nc_visual_doc,
                                &g_nc_visual_edit,
                                key_char,
                                g_nc_visual_status,
                                sizeof(g_nc_visual_status))) {
        return true;
    }
    return false;
}

static void nc_visual_close_modal(void)
{
    g_nc_modal_active = false;
    g_nc_modal_items = 0;
    g_nc_modal_count = 0;
    g_nc_modal_title = "";
    g_nc_gcode_buf[0] = '\0';
    g_nc_modal_line = (size_t)-1;
    g_nc_modal_prefix = '\0';
    g_nc_modal_label = 0;
}

static void nc_visual_modal_remove_line(void)
{
    if (g_nc_modal_line < g_nc_visual_doc.line_count) {
        const char *text = g_nc_visual_doc.lines[g_nc_modal_line].text;
        bool ours;

        if (g_nc_modal_prefix) {
            ours = text[0] == g_nc_modal_prefix &&
                   (text[1] == '\0' ||
                    strcmp(text + 1, g_nc_gcode_buf) == 0);
        } else {
            ours = g_nc_modal_label && strcmp(text, g_nc_modal_label) == 0;
        }
        if (ours) {
            (void)nc_delete_line(&g_nc_visual_doc, g_nc_modal_line);
            if (g_nc_modal_origin_line < g_nc_visual_doc.line_count) {
                g_nc_visual_doc.cursor_line = g_nc_modal_origin_line;
            } else if (g_nc_visual_doc.line_count > 0u) {
                g_nc_visual_doc.cursor_line = g_nc_visual_doc.line_count - 1u;
            } else {
                g_nc_visual_doc.cursor_line = 0u;
            }
        }
    }
    g_nc_modal_line = (size_t)-1;
    g_nc_modal_prefix = '\0';
    g_nc_modal_label = 0;
}

static void nc_visual_modal_cancel(void)
{
    nc_visual_modal_remove_line();
    nc_visual_close_modal();
}

/* Inserts the helper's own line under the cursor and puts the cursor on it, so
   the panel hangs below the line the user is working with. */
static bool nc_visual_modal_insert_line(const char *text)
{
    size_t origin = g_nc_visual_doc.cursor_line;
    size_t at = origin + 1u;

    if (at > g_nc_visual_doc.line_count) {
        at = g_nc_visual_doc.line_count;
    }
    if (nc_insert_line(&g_nc_visual_doc, at, text) != NC_OK) {
        strncpy(g_nc_visual_status, "Insert line failed", sizeof(g_nc_visual_status) - 1);
        nc_visual_close_modal();
        return false;
    }
    /* nc_insert_line leaves the cursor on the new line; keep the line it came
       from so cancelling returns there. */
    g_nc_modal_origin_line = origin;
    g_nc_modal_line = at;
    g_nc_visual_doc.selected_word = -1;
    nc_text_edit_clear(&g_nc_visual_edit);
    return true;
}

static void nc_visual_modal_begin_line(char prefix)
{
    char line[4];

    snprintf(line, sizeof(line), "%c", prefix);
    g_nc_modal_prefix = '\0';
    g_nc_modal_label = 0;
    g_nc_gcode_buf[0] = '\0';
    if (!nc_visual_modal_insert_line(line)) {
        return;
    }
    g_nc_modal_prefix = prefix;
    g_nc_modal_active = true;
    g_nc_modal_items = 0;
    g_nc_modal_count = 0;
    g_nc_modal_title = prefix == 'G' ? "GCODE" : "TOOL";
    snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "%c", prefix);
}

/* The submenu case: the line carries the action name the panel used to draw as
   its title, and doubles as the place the chosen text will be written. */
static void nc_visual_modal_begin_label(const char *label)
{
    if (!label || !label[0]) {
        return;
    }
    g_nc_modal_prefix = '\0';
    g_nc_gcode_buf[0] = '\0';
    if (!nc_visual_modal_insert_line(label)) {
        return;
    }
    g_nc_modal_label = label;
}

static void nc_visual_modal_update_line(void)
{
    char text[16];

    if (g_nc_modal_line >= g_nc_visual_doc.line_count || !g_nc_modal_prefix) {
        return;
    }
    snprintf(text, sizeof(text), "%c%s", g_nc_modal_prefix, g_nc_gcode_buf);
    (void)nc_set_line(&g_nc_visual_doc, g_nc_modal_line, text);
}

static void nc_visual_select_first_value(void)
{
    nc_word_t words[24];
    int count;

    if (g_nc_visual_doc.cursor_line >= g_nc_visual_doc.line_count) {
        return;
    }
    count = nc_parse_words(g_nc_visual_doc.lines[g_nc_visual_doc.cursor_line].text,
                           words,
                           24);
    if (count > 0) {
        g_nc_visual_doc.selected_word = count > 1 ? 1 : 0;
    }
}

static void nc_visual_open_modal(nc_footer_action_t parent)
{
    nc_visual_modal_cancel();
    if (parent == NC_FOOTER_ACTION_GCODE) {
        nc_visual_modal_begin_line('G');
        g_nc_visual_dirty = true;
        return;
    }
    g_nc_modal_active = true;
    g_nc_modal_parent = parent;
    g_nc_modal_items = nc_menu_submenu(parent, &g_nc_modal_count);
    switch (parent) {
    case NC_FOOTER_ACTION_OPS: g_nc_modal_title = "OPS"; break;
    case NC_FOOTER_ACTION_TOOL_MENU: g_nc_modal_title = "TOOL"; break;
    case NC_FOOTER_ACTION_G7X_MENU: g_nc_modal_title = "G7X"; break;
    case NC_FOOTER_ACTION_SYNC_MENU: g_nc_modal_title = "THREAD"; break;
    case NC_FOOTER_ACTION_PECK_MENU: g_nc_modal_title = "PECK"; break;
    default: g_nc_modal_title = ""; break;
    }
    /* Give the helper the line it belongs to: the action name sits in that
       line and the panel hangs under it, instead of floating over the line the
       user is reading. Nothing is written into a document that cannot take
       edits (RUN and the like) - the panel still shows. */
    if (nc_visual_can_edit_code()) {
        nc_visual_modal_begin_label(g_nc_modal_title);
    }
    g_nc_visual_dirty = true;
}

static bool nc_visual_modal_handle_key(nc_visual_key_t key)
{
    char ch;

    if (!g_nc_modal_active) {
        return false;
    }
    if (g_nc_modal_prefix) {
        if (key == NC_VISUAL_KEY_CANCEL || key == NC_VISUAL_KEY_MODE) {
            nc_visual_modal_cancel();
            g_nc_visual_dirty = true;
            return true;
        }
        ch = nc_visual_key_char(key);
        if (ch >= '0' && ch <= '9') {
            size_t len = strlen(g_nc_gcode_buf);
            if (len + 1u < sizeof(g_nc_gcode_buf)) {
                g_nc_gcode_buf[len] = ch;
                g_nc_gcode_buf[len + 1u] = '\0';
                nc_visual_modal_update_line();
                snprintf(g_nc_visual_status, sizeof(g_nc_visual_status),
                         "%c%s", g_nc_modal_prefix, g_nc_gcode_buf);
            }
            g_nc_visual_dirty = true;
            return true;
        }
        if (key == NC_VISUAL_KEY_BACKSPACE) {
            size_t len = strlen(g_nc_gcode_buf);
            if (len > 0u) {
                g_nc_gcode_buf[len - 1u] = '\0';
                nc_visual_modal_update_line();
            }
            g_nc_visual_dirty = true;
            return true;
        }
        if (key == NC_VISUAL_KEY_FINISH) {
            nc_visual_modal_cancel();
            g_nc_visual_dirty = true;
            return true;
        }
        if (key == NC_VISUAL_KEY_ACCEPT) {
            if (g_nc_modal_prefix == 'G' && g_nc_gcode_buf[0]) {
                int code = atoi(g_nc_gcode_buf);
                const char *name = nc_vocab_gcode_name(code);
                const char *params = nc_vocab_gcode_parameters(code);
                const char *template_line = nc_vocab_gcode_template(code);
                const char *param_text = params ? params : "";
                if (name) {
                    (void)nc_set_line(&g_nc_visual_doc,
                                      g_nc_modal_line,
                                      template_line ? template_line : "");
                    nc_visual_select_first_value();
                    snprintf(g_nc_visual_status, sizeof(g_nc_visual_status),
                             "G%d %s: %s", code, name, param_text);
                } else {
                    snprintf(g_nc_visual_status, sizeof(g_nc_visual_status),
                             "Unknown G%d", code);
                }
            } else if (g_nc_modal_prefix == 'T') {
                nc_visual_select_first_value();
                snprintf(g_nc_visual_status, sizeof(g_nc_visual_status),
                         "T%s", g_nc_gcode_buf);
            }
            nc_visual_close_modal();
            g_nc_visual_dirty = true;
            return true;
        }
        return true;
    }
    if (key == NC_VISUAL_KEY_CANCEL || key == NC_VISUAL_KEY_MODE) {
        nc_visual_modal_cancel();
        g_nc_visual_dirty = true;
        return true;
    }
    ch = nc_visual_key_char(key);
    if (ch == '0') {
        nc_visual_modal_cancel();
        g_nc_visual_dirty = true;
        return true;
    }
    if (ch >= '1' && ch <= '9') {
        size_t i;
        for (i = 0; i < g_nc_modal_count; i++) {
            if (g_nc_modal_items[i].key == ch) {
                uint8_t action = g_nc_modal_items[i].action;
                /* The labelled line only marked the spot. Drop it first so the
                   chosen action writes exactly where it stood. */
                nc_visual_modal_cancel();
                if (action == NC_FOOTER_ACTION_TOOL_SELECT) {
                    nc_visual_modal_begin_line('T');
                } else {
                    nc_visual_dispatch_footer_action(action);
                }
                g_nc_visual_dirty = true;
                return true;
            }
        }
    }
    return true;
}

static void nc_visual_dispatch_footer_action(uint8_t action)
{
    g_nc_visual_selected_action = action;

    switch (action) {
    case NC_FOOTER_ACTION_OPS:
    case NC_FOOTER_ACTION_TOOL_MENU:
    case NC_FOOTER_ACTION_GCODE:
    case NC_FOOTER_ACTION_G7X_MENU:
    case NC_FOOTER_ACTION_SYNC_MENU:
    case NC_FOOTER_ACTION_PECK_MENU:
        nc_visual_open_modal((nc_footer_action_t)action);
        return;
    case NC_FOOTER_ACTION_TOOL_SELECT:
        nc_visual_modal_begin_line('T');
        break;
    case NC_FOOTER_ACTION_TOOL_EDIT:
        strncpy(g_nc_visual_status, "Tool edit", sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_TOOL_CHANGE:
        if (nc_insert_line(&g_nc_visual_doc, g_nc_visual_doc.cursor_line + 1u, "M6") == NC_OK) {
            nc_cursor_down(&g_nc_visual_doc);
            strncpy(g_nc_visual_status, "Inserted M6 tool change", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_SPINDLE_ON:
        if (nc_insert_line(&g_nc_visual_doc, g_nc_visual_doc.cursor_line + 1u, "M3 S1000") == NC_OK) {
            nc_cursor_down(&g_nc_visual_doc);
            strncpy(g_nc_visual_status, "Inserted M3 spindle on", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_SPINDLE_STOP:
        if (nc_insert_line(&g_nc_visual_doc, g_nc_visual_doc.cursor_line + 1u, "M5") == NC_OK) {
            nc_cursor_down(&g_nc_visual_doc);
            strncpy(g_nc_visual_status, "Inserted M5 spindle stop", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_SPINDLE_CCW:
        if (nc_insert_line(&g_nc_visual_doc, g_nc_visual_doc.cursor_line + 1u, "M4 S1000") == NC_OK) {
            nc_cursor_down(&g_nc_visual_doc);
            strncpy(g_nc_visual_status, "Inserted M4 spindle CCW", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_G7X_Q:
        strncpy(g_nc_visual_status, "G71 Q: numbered contour end", sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_G7X_N:
        strncpy(g_nc_visual_status, "G71 N: numbered block", sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_TAP:
        if (nc_insert_preset_id(&g_nc_visual_doc, 53)) {
            nc_cursor_down(&g_nc_visual_doc);
            strncpy(g_nc_visual_status, "Inserted tap preset", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "Tap preset unavailable", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_THREAD_OD:
        if (nc_insert_preset_id(&g_nc_visual_doc, 51)) {
            nc_cursor_down(&g_nc_visual_doc);
            strncpy(g_nc_visual_status, "Inserted G76 OD thread", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_THREAD_ID:
        if (nc_insert_preset_id(&g_nc_visual_doc, 52)) {
            nc_cursor_down(&g_nc_visual_doc);
            strncpy(g_nc_visual_status, "Inserted G76 ID thread", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_PECK_DRILL:
        if (nc_insert_preset_id(&g_nc_visual_doc, 61)) {
            nc_cursor_down(&g_nc_visual_doc);
            strncpy(g_nc_visual_status, "Inserted drill preset", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_PECK_PECK:
        if (nc_insert_preset_id(&g_nc_visual_doc, 62)) {
            nc_cursor_down(&g_nc_visual_doc);
            strncpy(g_nc_visual_status, "Inserted peck preset", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_PECK_DWELL:
        if (nc_insert_preset_id(&g_nc_visual_doc, 63)) {
            nc_cursor_down(&g_nc_visual_doc);
            strncpy(g_nc_visual_status, "Inserted dwell preset", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_FILE:
        /* Opens the folder the open file lives in, so the list can land on it;
           falls back to the NC folder when nothing is open. */
        if (g_nc_visual_doc.path[0]) {
            nc_visual_open_current_folder();
        } else {
            nc_visual_open_file_view(NC_FILES_DIR, true);
        }
        break;
    case NC_FOOTER_ACTION_FILES:
        nc_visual_open_file_view(NULL, false);
        break;
    case NC_FOOTER_ACTION_OPEN:
        if (nc_files_active()) {
            char path[NC_PATH_MAX];
            nc_result_t r;
            if (nc_files_selected_is_dir()) {
                if (nc_files_enter_selected()) {
                    snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Dir: %s", nc_files_cwd());
                } else {
                    strncpy(g_nc_visual_status, "Directory open failed", sizeof(g_nc_visual_status) - 1);
                }
                break;
            }
            if (!nc_files_selected_path(path, sizeof(path))) {
                strncpy(g_nc_visual_status, "No NC file selected", sizeof(g_nc_visual_status) - 1);
                break;
            }
            if (g_nc_visual_mode == NC_MODE_TOOLS &&
                !nc_state_tool_path_supported(path)) {
                strncpy(g_nc_visual_status, "TOOLS opens .t files only", sizeof(g_nc_visual_status) - 1);
                break;
            }
            /* The buffer is about to be reused for another file: put the edits
               of the one that is open on the card first. */
            if (!nc_visual_save_current_if_file() &&
                !nc_visual_proceed_without_saving(NC_VISUAL_UNSAVED_OPEN)) {
                strncpy(g_nc_visual_status, "Save failed - press again to open", sizeof(g_nc_visual_status) - 1);
                break;
            }
            r = nc_load_file(&g_nc_visual_doc, path);
            if (r == NC_OK) {
                nc_files_set_active(false);
                g_nc_visual_new_file_active = false;
                nc_state_remember_path(g_nc_visual_mode, path);
                nc_state_save();
                snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Opened %s", nc_files_name(nc_files_selected()));
            } else {
                snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Open failed: %s", nc_result_text(r));
            }
        } else {
            nc_files_set_active(true);
            g_nc_visual_new_file_active = false;
            if (nc_files_refresh(NULL)) {
                nc_visual_serial_selected_file();
                g_nc_visual_status[0] = '\0';
            } else {
                strncpy(g_nc_visual_status, "File list unavailable", sizeof(g_nc_visual_status) - 1);
            }
        }
        break;
    case NC_FOOTER_ACTION_PRESET_OD:
        nc_visual_insert_preset_action(NC_PRESET_OD, "Inserted OD preset", "OD preset failed");
        break;
    case NC_FOOTER_ACTION_PRESET_ID:
        nc_visual_insert_preset_action(NC_PRESET_ID, "Inserted ID preset", "ID preset failed");
        break;
    case NC_FOOTER_ACTION_PRESET_FACE:
        nc_visual_insert_preset_action(NC_PRESET_FACE, "Inserted FACE preset", "FACE preset failed");
        break;
    case NC_FOOTER_ACTION_PRESET_LINE:
        nc_visual_insert_preset_action(NC_PRESET_LINE, "Inserted line preset", "Line preset failed");
        break;
    case NC_FOOTER_ACTION_PRESET_ARC:
        nc_visual_insert_preset_action(NC_PRESET_ARC, "Inserted arc preset", "Arc preset failed");
        break;
    case NC_FOOTER_ACTION_PRESET_SETUP:
        nc_visual_insert_preset_action(NC_PRESET_SETUP, "Inserted setup preset", "Setup preset failed");
        break;
    case NC_FOOTER_ACTION_PRESET_END:
        nc_visual_insert_preset_action(NC_PRESET_END, "Inserted G80", "G80 preset failed");
        break;
    case NC_FOOTER_ACTION_TOOL:
        if (g_nc_visual_mode == NC_MODE_PROGRAM) {
            if (nc_visual_insert_tool_ref() == NC_OK) {
                nc_cursor_down(&g_nc_visual_doc);
                g_nc_visual_doc.selected_word = -1;
                snprintf(g_nc_visual_status,
                         sizeof(g_nc_visual_status),
                         "Inserted tool ref line %lu",
                         (unsigned long)(g_nc_visual_doc.cursor_line + 1u));
            } else {
                strncpy(g_nc_visual_status, "Tool ref insert failed", sizeof(g_nc_visual_status) - 1);
            }
        } else if (g_nc_visual_mode != NC_MODE_TOOLS) {
            nc_visual_set_mode(NC_MODE_TOOLS);
            nc_files_set_active(false);
            nc_text_edit_clear(&g_nc_visual_edit);
            if (nc_state_load_document(g_nc_visual_mode, &g_nc_visual_doc)) {
                if (g_nc_visual_doc.line_count && !nc_tool_line_is_tool(g_nc_visual_doc.lines[g_nc_visual_doc.cursor_line].text)) {
                    int first_line = -1;
                    (void)nc_visual_find_tool_line(0, &first_line);
                    if (first_line >= 0) {
                        g_nc_visual_doc.cursor_line = (size_t)first_line;
                    }
                }
                g_nc_visual_doc.selected_word = -1;
                (void)nc_select_next_word(&g_nc_visual_doc);
                snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "TOOLS: %.48s", g_nc_visual_doc.path);
            } else {
                nc_document_init(&g_nc_visual_doc);
                strncpy(g_nc_visual_status, "TOOLS has no file", sizeof(g_nc_visual_status) - 1);
            }
        } else if (nc_insert_tool_default(&g_nc_visual_doc) == NC_OK) {
            nc_cursor_down(&g_nc_visual_doc);
            g_nc_visual_doc.selected_word = -1;
            (void)nc_select_next_word(&g_nc_visual_doc);
            strncpy(g_nc_visual_status, "Inserted TOOL row", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "Tool action stub", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_SAVE:
        if (g_nc_visual_doc.path[0] && nc_save_file(&g_nc_visual_doc, g_nc_visual_doc.path) == NC_OK) {
            nc_state_remember_path(g_nc_visual_mode, g_nc_visual_doc.path);
            nc_state_save();
            strncpy(g_nc_visual_status, "Saved", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "Save needs an opened NC file", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_NEW:
        if (nc_files_active()) {
            g_nc_visual_new_file_active = true;
            g_nc_visual_new_file_name[0] = '\0';
            nc_visual_new_file_status();
            break;
        }
        nc_document_init(&g_nc_visual_doc);
        nc_insert_line(&g_nc_visual_doc, 0, "");
        strncpy(g_nc_visual_doc.path, "new.nc", sizeof(g_nc_visual_doc.path) - 1);
        nc_state_remember_path(g_nc_visual_mode, g_nc_visual_doc.path);
        nc_state_save();
        strncpy(g_nc_visual_status, "New empty NC program", sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_INSERT:
        if (nc_insert_line(&g_nc_visual_doc, g_nc_visual_doc.cursor_line + 1, "") == NC_OK) {
            nc_cursor_down(&g_nc_visual_doc);
            strncpy(g_nc_visual_status, "Inserted blank line", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "Insert failed", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_DELETE:
        if (nc_files_active()) {
            if (nc_files_delete_selected()) {
                strncpy(g_nc_visual_status, "File deleted", sizeof(g_nc_visual_status) - 1);
            } else {
                strncpy(g_nc_visual_status, "Delete file failed", sizeof(g_nc_visual_status) - 1);
            }
            break;
        }
        if (nc_delete_line(&g_nc_visual_doc, g_nc_visual_doc.cursor_line) == NC_OK) {
            strncpy(g_nc_visual_status, "Deleted line", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "Delete failed", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_BACK:
        if (nc_files_active()) {
            nc_files_select_prev();
            nc_visual_serial_selected_file();
            strncpy(g_nc_visual_status, "File up", sizeof(g_nc_visual_status) - 1);
        } else if (g_nc_visual_mode == NC_MODE_TOOLS) {
            nc_visual_move_tool_line(-1);
        } else {
            nc_visual_move_code_line(-1);
            nc_visual_serial_selected_line();
        }
        break;
    case NC_FOOTER_ACTION_STEP:
        if (nc_files_active()) {
            nc_files_select_next();
            nc_visual_serial_selected_file();
            strncpy(g_nc_visual_status, "File down", sizeof(g_nc_visual_status) - 1);
        } else if (g_nc_visual_mode == NC_MODE_TOOLS) {
            nc_visual_move_tool_line(1);
        } else {
            nc_visual_move_code_line(1);
            nc_visual_serial_selected_line();
        }
        break;
    case NC_FOOTER_ACTION_RESET:
        if (nc_files_active()) {
            /* BACK leaves the list in one press and puts the screen back the
               way it was - the file that is open is still the open file. Going
               up a folder is the `..` entry's job, so a browse that ends in
               "nothing picked" does not have to walk back down again. */
            nc_files_set_active(false);
            strncpy(g_nc_visual_status, "Back to NC", sizeof(g_nc_visual_status) - 1);
        } else if (g_nc_visual_mode == NC_MODE_RUN) {
            /* Reset the run and read the program back off the card, so a file
               (or a G970 setup block in it) edited since it was opened is what
               runs next. */
            nc_run_reset();
            if (nc_state_load_document(NC_MODE_RUN, &g_nc_visual_doc)) {
                strncpy(g_nc_visual_status, "RUN reset, file reloaded",
                        sizeof(g_nc_visual_status) - 1);
            } else {
                g_nc_visual_doc.cursor_line = 0;
                strncpy(g_nc_visual_status, "RUN reset", sizeof(g_nc_visual_status) - 1);
            }
        } else {
            strncpy(g_nc_visual_status, "Reset is stubbed", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_REFRESH:
        if (nc_files_refresh(NULL)) {
            nc_visual_serial_selected_file();
            snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Refreshed %s", nc_files_cwd());
        } else {
            strncpy(g_nc_visual_status, "Refresh failed", sizeof(g_nc_visual_status) - 1);
        }
        if (!nc_files_active()) {
            nc_files_set_active(true);
        }
        break;
    case NC_FOOTER_ACTION_FULL:
        if (nc_files_active()) {
            char path[NC_PATH_MAX];
            nc_result_t r;
            if (nc_files_selected_is_dir()) {
                if (nc_files_enter_selected()) {
                    snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Dir: %s", nc_files_cwd());
                } else {
                    strncpy(g_nc_visual_status, "Directory open failed", sizeof(g_nc_visual_status) - 1);
                }
                break;
            }
            if (!nc_files_selected_path(path, sizeof(path))) {
                strncpy(g_nc_visual_status, "No NC file selected", sizeof(g_nc_visual_status) - 1);
                break;
            }
            /* Loading for RUN reuses the same buffer, so save first. */
            if (!nc_visual_save_current_if_file() &&
                !nc_visual_proceed_without_saving(NC_VISUAL_UNSAVED_OPEN)) {
                strncpy(g_nc_visual_status, "Save failed - press again to load", sizeof(g_nc_visual_status) - 1);
                break;
            }
            r = nc_load_file(&g_nc_visual_doc, path);
            if (r == NC_OK) {
                nc_files_set_active(false);
                nc_visual_set_mode(NC_MODE_RUN);
                nc_state_remember_path(NC_MODE_RUN, path);
                nc_state_save();
                nc_visual_run_arm(0, "Run loaded");
            } else {
                snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Run open failed: %s", nc_result_text(r));
            }
        } else {
            nc_visual_run_arm(0, "Full run armed");
        }
        break;
    case NC_FOOTER_ACTION_STOCK:
        g_nc_visual_show_stock = !g_nc_visual_show_stock;
        strncpy(g_nc_visual_status,
                g_nc_visual_show_stock ? "Stock on" : "Stock outline",
                sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_PATH:
        g_nc_visual_show_path = !g_nc_visual_show_path;
        strncpy(g_nc_visual_status,
                g_nc_visual_show_path ? "Path on" : "Path hidden",
                sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_ROUGH:
        g_nc_visual_show_rough = !g_nc_visual_show_rough;
        strncpy(g_nc_visual_status,
                g_nc_visual_show_rough ? "Rough on" : "Rough hidden",
                sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_DIMS:
        g_nc_visual_show_dims = !g_nc_visual_show_dims;
        strncpy(g_nc_visual_status,
                g_nc_visual_show_dims ? "Dimensions on" : "Dimensions hidden",
                sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_VIEW:
        g_nc_visual_show_code = !g_nc_visual_show_code;
        strncpy(g_nc_visual_status,
                g_nc_visual_show_code ? "Code shown" : "Code hidden - full preview",
                sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_SINGLE:
        nc_visual_run_step();
        break;
    case NC_FOOTER_ACTION_FROM:
        nc_visual_run_arm(g_nc_visual_doc.cursor_line, "Run from");
        break;
    case NC_FOOTER_ACTION_HOLD:
        if (nc_run_toggle_hold()) {
            strncpy(g_nc_visual_status,
                    nc_run_hold() ? "RUN hold" : "RUN resumed",
                    sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "No active RUN", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_STOP:
        nc_run_stop();
        strncpy(g_nc_visual_status, "RUN stopped", sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_SEND:
        /* Sending a line without a program is what RUN does; nothing here has a
           document of its own to send. */
        strncpy(g_nc_visual_status, "Send is stubbed", sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_CLEAR:
        strncpy(g_nc_visual_status, "Clear is stubbed", sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_FIELD:
        if (nc_select_next_word(&g_nc_visual_doc) == NC_OK) {
            strncpy(g_nc_visual_status, "Next word", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "No editable word here", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_FOOTER_ACTION_AXIS_PREV:
        g_nc_visual_manual_axis = (uint8_t)((g_nc_visual_manual_axis + 1u) % 2u);
        nc_visual_manual_status("Axis");
        break;
    case NC_FOOTER_ACTION_AXIS_NEXT:
        g_nc_visual_manual_axis = (uint8_t)((g_nc_visual_manual_axis + 1u) % 2u);
        nc_visual_manual_status("Axis");
        break;
    case NC_FOOTER_ACTION_ZERO:
        nc_visual_manual_zero();
        break;
    case NC_FOOTER_ACTION_TOUCH:
        g_nc_visual_manual_touch_active = true;
        g_nc_visual_manual_touch[0] = '\0';
        snprintf(g_nc_visual_status, sizeof(g_nc_visual_status),
                 "Touch %c: value then D", nc_visual_manual_letter());
        break;
    default:
        strncpy(g_nc_visual_status, "Action not available yet", sizeof(g_nc_visual_status) - 1);
        break;
    }
}

static void nc_visual_cycle_mode(void)
{
    nc_visual_select_mode((nc_mode_t)((g_nc_visual_mode + 1) % NC_MODE_COUNT));
}

void nc_visual_select_mode(nc_mode_t mode)
{
    if (mode < 0 || mode >= NC_MODE_COUNT || mode == g_nc_visual_mode) {
        return;
    }
    if (!nc_visual_save_current_if_file() &&
        !nc_visual_proceed_without_saving(NC_VISUAL_UNSAVED_MODE)) {
        strncpy(g_nc_visual_status,
                "Save failed - press again to leave",
                sizeof(g_nc_visual_status) - 1);
        g_nc_visual_dirty = true;
        return;
    }
    nc_visual_set_mode(mode);
    g_nc_visual_selected_action = NC_FOOTER_ACTION_NONE;
    if (nc_visual_uses_file()) {
        nc_files_set_active(false);
        nc_text_edit_clear(&g_nc_visual_edit);
        if (nc_state_load_document(g_nc_visual_mode, &g_nc_visual_doc)) {
            if (g_nc_visual_mode == NC_MODE_RUN && !nc_run_active())
                nc_run_set_line(&g_nc_visual_doc, g_nc_visual_doc.cursor_line);
            if (g_nc_visual_mode == NC_MODE_TOOLS) {
                int first_tool = -1;
                if (nc_visual_find_tool_line(0, &first_tool) > 0 && first_tool >= 0) {
                    g_nc_visual_doc.cursor_line = (size_t)first_tool;
                    g_nc_visual_doc.selected_word = -1;
                }
            }
            /* The header and the tab strip already name the screen and the
               file, so the status line stays free for messages. */
            g_nc_visual_status[0] = '\0';
        } else {
            nc_document_init(&g_nc_visual_doc);
            strncpy(g_nc_visual_status, "No file selected", sizeof(g_nc_visual_status) - 1);
        }
    } else {
        g_nc_visual_status[0] = '\0';
    }
    g_nc_visual_dirty = true;
}

static void nc_visual_draw_text_clip(int x,
                                     int y,
                                     const char *text,
                                     int cols,
                                     lvds_color_t fg,
                                     lvds_color_t bg,
                                     int font)
{
    lvds_draw_text_clip(x, y, text ? text : "", cols, fg, bg, font);
}

static void nc_visual_serial_selected_line(void)
{
    size_t line = nc_visual_code_line();
    const char *text = "";

    if (line < g_nc_visual_doc.line_count) {
        text = g_nc_visual_doc.lines[line].text;
    }
    grbl_stream_printf(__romstr__("[MSG:NC SELECT %s %lu: %.96s]\r\n"),
                       nc_menu_mode_name(g_nc_visual_mode),
                       (unsigned long)(line + 1),
                       text);
}

static void nc_visual_serial_selected_file(void)
{
    int selected = nc_files_selected();
    const char *name = nc_files_name(selected);
    char path[NC_PATH_MAX];

    if (nc_files_selected_path(path, sizeof(path))) {
        grbl_stream_printf(__romstr__("[MSG:NC FILE %d: %.96s]\r\n"), selected + 1, path);
    } else {
        grbl_stream_printf(__romstr__("[MSG:NC FILE %d: %.96s]\r\n"), selected + 1, name);
    }
}

static bool nc_visual_line_is_contour_detail(const nc_document_t *doc, size_t index)
{
    if (!doc || index >= doc->line_count) {
        return false;
    }
    if (g7x_contour_cmd_from_line(doc->lines[index].text) == G7X_CONTOUR_NONE) {
        return false;
    }
    return nc_visual_line_is_g7x_contour(doc, index);
}

static void nc_visual_draw_tool_screen(void)
{
    int tool_count;
    int selected_tool;
    int selected_line;
    int first_tool = 0;
    int row;
    /* The table clears the header's bottom edge, and the details sit at the
       bottom of the pane: the free space belongs between them, not below. */
    const int table_y = NC_PANE_Y + 6;
    const int detail_y = 398;
    char buf[80];
    char active_letter = '\0';
    int active_line = -1;

    tool_count = nc_visual_find_tool_line(nc_visual_selected_tool_index(), NULL);
    selected_tool = nc_visual_selected_tool_index();
    (void)nc_visual_find_tool_line(selected_tool, &selected_line);
    (void)nc_visual_selected_tool_word(&active_letter, &active_line);
    if (selected_tool >= 8) {
        first_tool = selected_tool - 7;
    }

    lvds_draw_fill_rect(18, table_y - 10, LVDS_HSTX_WIDTH - 36,
                        NC_PANE_BOTTOM - (table_y - 10), NC_VISUAL_BG);
    nc_visual_draw_text_clip(28, table_y, "TOOL TABLE", 16, NC_VISUAL_TEXT, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    /* The header carries the file name; the table only counts its rows. */
    snprintf(buf, sizeof(buf), "%d tools", tool_count);
    nc_visual_draw_text_clip(674, table_y, buf, 12, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);

    nc_visual_draw_text_clip(72, table_y + 28, "T", 4, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(120, table_y + 28, "RADIUS", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(190, table_y + 28, "ORIENT", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(260, table_y + 28, "FEED", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(326, table_y + 28, "FF", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(392, table_y + 28, "DOC", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(458, table_y + 28, "FDOC", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(524, table_y + 28, "RPM", 7, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(604, table_y + 28, "XOFF", 7, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_visual_draw_text_clip(684, table_y + 28, "ZOFF", 7, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    lvds_draw_line(28, table_y + 50, LVDS_HSTX_WIDTH - 28, table_y + 50, NC_VISUAL_DIM);

    if (tool_count == 0) {
        nc_visual_draw_text_clip(44, table_y + 78, "No tool rows in this NC file. Press 1 to add T1.", 64,
                                 NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    }

    for (row = 0; row < 8 && first_tool + row < tool_count; row++) {
        int tool_line = -1;
        int y = table_y + 64 + row * 26;
        bool selected = (first_tool + row) == selected_tool;
        lvds_color_t bg = selected ? NC_VISUAL_SELECT : NC_VISUAL_BG;
        lvds_color_t fg = selected ? NC_VISUAL_LINE_NO_SELECTED : NC_VISUAL_TEXT;
        nc_tool_t tool;
        const char *line;

        (void)nc_visual_find_tool_line(first_tool + row, &tool_line);
        if (tool_line < 0) {
            continue;
        }
        line = g_nc_visual_doc.lines[tool_line].text;
        (void)nc_tool_from_line(line, &tool);
        if (selected) {
            lvds_draw_fill_rect(24, y - 4, LVDS_HSTX_WIDTH - 48, 24, bg);
        }
        nc_visual_draw_tool_glyph_centered(30, y - 3, 32, 18, &tool, bg, selected);
        nc_visual_draw_tool_cell(line, 'T', 72, y, 4, fg, bg, tool_line == active_line && active_letter == 'T');
        nc_visual_draw_tool_cell(line, 'R', 120, y, 6, fg, bg, tool_line == active_line && active_letter == 'R');
        nc_visual_draw_tool_cell(line, 'O', 190, y, 6, fg, bg, tool_line == active_line && active_letter == 'O');
        nc_visual_draw_tool_cell(line, 'F', 260, y, 6, fg, bg, tool_line == active_line && active_letter == 'F');
        nc_visual_draw_tool_cell(line, 'Q', 326, y, 6, fg, bg, tool_line == active_line && active_letter == 'Q');
        nc_visual_draw_tool_cell(line, 'D', 392, y, 6, fg, bg, tool_line == active_line && active_letter == 'D');
        nc_visual_draw_tool_cell(line, 'E', 458, y, 6, fg, bg, tool_line == active_line && active_letter == 'E');
        nc_visual_draw_tool_cell(line, 'S', 524, y, 7, fg, bg, tool_line == active_line && active_letter == 'S');
        nc_visual_draw_tool_cell(line, 'X', 604, y, 7, fg, bg, tool_line == active_line && active_letter == 'X');
        nc_visual_draw_tool_cell(line, 'Z', 684, y, 7, fg, bg, tool_line == active_line && active_letter == 'Z');
    }

    lvds_draw_line(18, detail_y, LVDS_HSTX_WIDTH - 18, detail_y, NC_VISUAL_DIM);
    nc_visual_draw_text_clip(38, detail_y + 12, "Tool tip", 12, NC_VISUAL_TEXT, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    lvds_draw_line(52, detail_y + 76, 142, detail_y + 76, NC_VISUAL_DIM);
    lvds_draw_line(96, detail_y + 34, 96, detail_y + 120, NC_VISUAL_DIM);
    nc_visual_draw_text_clip(102, detail_y + 34, "X0", 4, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_SMALL);
    nc_visual_draw_text_clip(122, detail_y + 82, "Z0", 4, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_SMALL);

    if (selected_line >= 0) {
        nc_tool_t tool;
        const char *line = g_nc_visual_doc.lines[selected_line].text;

        (void)nc_tool_from_line(line, &tool);
        nc_visual_draw_tool_glyph(96, detail_y + 76, 44, &tool, NC_VISUAL_BG, false);
        snprintf(buf, sizeof(buf), "Line %d: %.48s", selected_line + 1, line);
        nc_visual_draw_text_clip(170, detail_y + 18, buf, 70, NC_VISUAL_TEXT, NC_VISUAL_BG, LVDS_FONT_NORMAL);
        nc_visual_draw_tool_param(line, 'T', "T", 170, detail_y + 44, 8, selected_line == active_line && active_letter == 'T');
        nc_visual_draw_tool_param(line, 'O', "Orient", 170, detail_y + 64, 8, selected_line == active_line && active_letter == 'O');
        nc_visual_draw_tool_param(line, 'R', "Radius", 170, detail_y + 84, 8, selected_line == active_line && active_letter == 'R');
        nc_visual_draw_tool_param(line, 'D', "DOC", 170, detail_y + 104, 8, selected_line == active_line && active_letter == 'D');
        nc_visual_draw_tool_param(line, 'E', "FDOC", 400, detail_y + 44, 8, selected_line == active_line && active_letter == 'E');
        nc_visual_draw_tool_param(line, 'F', "FEED", 400, detail_y + 64, 8, selected_line == active_line && active_letter == 'F');
        nc_visual_draw_tool_param(line, 'Q', "F_FEED", 400, detail_y + 84, 8, selected_line == active_line && active_letter == 'Q');
        nc_visual_draw_tool_param(line, 'S', "RPM", 400, detail_y + 104, 8, selected_line == active_line && active_letter == 'S');
        nc_visual_draw_tool_param(line, 'X', "XOFF", 618, detail_y + 44, 7, selected_line == active_line && active_letter == 'X');
        nc_visual_draw_tool_param(line, 'Z', "ZOFF", 618, detail_y + 64, 7, selected_line == active_line && active_letter == 'Z');
    }
}

/* The operator message: the lock/settings/alarm line first, then the last
   command error, run error, transient message and finally the screen's own
   status. It is drawn at the right end of the tab strip, which leaves the DRO
   block to the numbers alone. */
static const char *nc_visual_notice(char *buf,
                                    size_t buf_sz,
                                    lvds_color_t *fg,
                                    const nc_runtime_state_t *runtime)
{
    const char *notice = nc_feedback_lock(nc_visual_settings_error(),
                                          runtime ? runtime->exec_state : 0,
                                          cnc_has_alarm());

    *fg = NC_VISUAL_TEXT;
    if (!notice[0]) {
        notice = g_nc_visual_command_error;
    }
    if (!notice[0] && nc_run_error()) {
        snprintf(buf,
                 buf_sz,
                 "Line %lu error %u: %s",
                 (unsigned long)(nc_run_error_line() + 1),
                 nc_run_error(),
                 nc_feedback_error(nc_run_error()));
        notice = buf;
    }
    if (!notice[0] && nc_message_kind() != NC_MSG_NONE && nc_message_text()[0]) {
        notice = nc_message_text();
    }
    if (!notice[0]) {
        notice = g_nc_visual_status[0] ? g_nc_visual_status : "";
    }
    if (nc_visual_settings_error() ||
        cnc_has_alarm() ||
        (runtime && (runtime->exec_state & (EXEC_KILL | EXEC_LIMITS | EXEC_POSITION_MAYBE_LOST))) ||
        g_nc_visual_command_error[0] ||
        nc_run_error() ||
        nc_message_kind() == NC_MSG_ERROR) {
        *fg = NC_VISUAL_ERROR;
    }
    return notice;
}

/* The screen names across the top. Every screen is reachable with the MODE key,
   so the strip is a position marker plus that hint, not a menu to click. */
static void nc_visual_draw_tabs(const char *message,
                                lvds_color_t message_fg)
{
    static const char *const names[NC_MODE_COUNT] = {
        "MANUAL", "EDIT", "TOOLS", "RUN"
    };
    const char *state = "";
    int x = 8;
    int i;

    if (g_nc_visual_mode == NC_MODE_RUN) {
        /* "RUN IDLE" -> "IDLE": the tab already says RUN. */
        nc_runtime_state_t rt;
        const char *text;

        nc_state_runtime(&rt);
        text = nc_visual_run_state_text(&rt);
        if (strncmp(text, "RUN ", 4u) == 0) {
            state = text + 4u;
        }
    }

    lvds_draw_fill_rect(0, NC_TAB_Y, LVDS_HSTX_WIDTH, NC_TAB_H, NC_VISUAL_HEADER);

    for (i = 0; i < NC_MODE_COUNT; i++) {
        int w = lvds_draw_text_width(names[i], LVDS_FONT_NORMAL) + 14;
        bool active = (nc_mode_t)i == g_nc_visual_mode;

        /* No divider under the strip; the current screen keeps the block it
           always had. */
        if (active) {
            lvds_draw_fill_rect(x, NC_TAB_Y + 2, w, NC_TAB_H - 5, NC_VISUAL_SELECT);
        }
        nc_visual_draw_text_clip(x + 7,
                                 NC_TAB_Y + 4,
                                 names[i],
                                 (w - 12) / NC_VISUAL_CHAR_W,
                                 active ? NC_VISUAL_LINE_NO_SELECTED : NC_VISUAL_DIM,
                                 active ? NC_VISUAL_SELECT : NC_VISUAL_HEADER,
                                 LVDS_FONT_NORMAL);
        x += w;
    }

    /* Scroll hint: the MODE key walks the strip. */
    nc_visual_draw_text_clip(LVDS_HSTX_WIDTH - 22,
                             NC_TAB_Y + 4,
                             ">",
                             1,
                             NC_VISUAL_DIM,
                             NC_VISUAL_HEADER,
                             LVDS_FONT_NORMAL);

    /* The run state sits at the left of the message area; the message uses
       whatever is left before the hint. */
    if (state && state[0]) {
        nc_visual_draw_text_clip(420, NC_TAB_Y + 4, state, 12,
                                 NC_VISUAL_TEXT, NC_VISUAL_HEADER, LVDS_FONT_NORMAL);
    }

    /* The message area, in the empty right side of the strip. */
    if (message && message[0]) {
        int cols = (LVDS_HSTX_WIDTH - 26 - 420 -
                    lvds_draw_text_width(state ? state : "", LVDS_FONT_NORMAL)) /
                   NC_VISUAL_CHAR_W;
        int len = (int)strlen(message);

        if (len > cols) {
            len = cols;
        }
        if (len > 0) {
            nc_visual_draw_text_clip(LVDS_HSTX_WIDTH - 26 - len * NC_VISUAL_CHAR_W,
                                     NC_TAB_Y + 4,
                                     message,
                                     len,
                                     message_fg,
                                     NC_VISUAL_HEADER,
                                     LVDS_FONT_NORMAL);
        }
    }
}

static void nc_visual_draw_header(const nc_snapshot_t *s)
{
    char buf[96];
    char fps[16];
    const nc_runtime_state_t *runtime = s ? &s->runtime : 0;
    const int hy = NC_HEADER_Y;
    /* The snapshot carries the machine position; the operator works in the
       work (nominal) system, so subtract the offsets for the first column and
       keep the machine figures for the third. */
    float work[AXIS_COUNT] = {0};
    /* Columns, each with room for what it holds: the position in the offset in
       use (large), the machine figure it is cut from (normal, one cell of its
       own so the two never meet), then feed and spindle (large). */
    const int col_work_x = 10;
    const int col_mach_x = 190;
    const int col_fs_x = 300;

    if (runtime) {
        work[AXIS_X] = runtime->x;
        work[AXIS_Z] = runtime->z;
        parser_machine_to_work(work);
    }

    lvds_draw_fill_rect(0, hy, LVDS_HSTX_WIDTH, NC_HEADER_H, NC_VISUAL_HEADER);
    lvds_draw_line(0, hy + NC_HEADER_H - 1, LVDS_HSTX_WIDTH, hy + NC_HEADER_H - 1, NC_VISUAL_DIM);
    /* MANUAL carries no DRO at all - its pane is the readout. */
    if (g_nc_visual_mode != NC_MODE_MANUAL) {
        lvds_draw_line(182, hy + 6, 182, hy + 61, NC_VISUAL_DIM);
        lvds_draw_line(292, hy + 6, 292, hy + 61, NC_VISUAL_DIM);
    }

    /* MANUAL shows the same numbers big in its own pane, so the strip copies
       are left out there. */
    if (g_nc_visual_mode != NC_MODE_MANUAL) {
        /* Column 1: the work (nominal) position. */
        nc_visual_draw_text_clip(col_work_x, hy + 4, "X", 1, NC_VISUAL_DIM, NC_VISUAL_HEADER, LVDS_FONT_LARGE);
        snprintf(buf, sizeof(buf), "%9.3f", (double)work[AXIS_X]);
        nc_visual_draw_text_clip(col_work_x + 18, hy + 4, buf, 9, NC_VISUAL_TEXT, NC_VISUAL_HEADER, LVDS_FONT_LARGE);
        nc_visual_draw_text_clip(col_work_x, hy + 36, "Z", 1, NC_VISUAL_DIM, NC_VISUAL_HEADER, LVDS_FONT_LARGE);
        snprintf(buf, sizeof(buf), "%9.3f", (double)work[AXIS_Z]);
        nc_visual_draw_text_clip(col_work_x + 18, hy + 36, buf, 9, NC_VISUAL_TEXT, NC_VISUAL_HEADER, LVDS_FONT_LARGE);

        /* Column 2: the machine figures, in their own cell and one font
           smaller, with the offset they differ by named above them. */
        {
            char offset[16];

            nc_visual_offset_label(offset, sizeof(offset));
            nc_visual_draw_text_clip(col_mach_x, hy + 1, offset, 16,
                                     NC_VISUAL_DIM, NC_VISUAL_HEADER, LVDS_FONT_SMALL);
            snprintf(buf, sizeof(buf), "%8.3f", (double)(runtime ? runtime->x : 0.0f));
            nc_visual_draw_text_clip(col_mach_x, hy + 11, buf, 8,
                                     NC_VISUAL_DIM, NC_VISUAL_HEADER, LVDS_FONT_NORMAL);
            snprintf(buf, sizeof(buf), "%8.3f", (double)(runtime ? runtime->z : 0.0f));
            nc_visual_draw_text_clip(col_mach_x, hy + 43, buf, 8,
                                     NC_VISUAL_DIM, NC_VISUAL_HEADER, LVDS_FONT_NORMAL);
        }

        /* Column 3: feed and spindle. */
        nc_visual_draw_text_clip(col_fs_x, hy + 4, "F", 1, NC_VISUAL_DIM, NC_VISUAL_HEADER, LVDS_FONT_LARGE);
        snprintf(buf, sizeof(buf), "%8.1f", (double)(runtime ? runtime->feed : 0.0f));
        nc_visual_draw_text_clip(col_fs_x + 18, hy + 4, buf, 8, NC_VISUAL_TEXT, NC_VISUAL_HEADER, LVDS_FONT_LARGE);
        nc_visual_draw_text_clip(col_fs_x, hy + 36, "S", 1, NC_VISUAL_DIM, NC_VISUAL_HEADER, LVDS_FONT_LARGE);
        snprintf(buf, sizeof(buf), "%8u", runtime ? runtime->spindle : 0u);
        nc_visual_draw_text_clip(col_fs_x + 18, hy + 36, buf, 8, NC_VISUAL_TEXT, NC_VISUAL_HEADER, LVDS_FONT_LARGE);
    }

    snprintf(fps, sizeof(fps), "%u FPS", (unsigned)g_nc_visual_fps);
    nc_visual_draw_text_clip(LVDS_HSTX_WIDTH - lvds_draw_text_width(fps, LVDS_FONT_SMALL) - 8,
                             hy + 6,
                             fps,
                             8,
                             NC_VISUAL_DIM,
                             NC_VISUAL_HEADER,
                             LVDS_FONT_SMALL);
}

static void nc_visual_draw_modal_items(int x,
                                       int y,
                                       const nc_footer_item_t *items,
                                       size_t count,
                                       uint16_t active_mask)
{
    int row;
    int col;

    /* Only the keys are drawn, with the program still visible above and below
       them, so the helper stays attached to the line it belongs to. */
    for (row = 0; row < NC_MODAL_ROWS; row++) {
        for (col = 0; col < NC_MODAL_COLS; col++) {
            int cx = x + NC_MODAL_PAD + col * NC_MODAL_KEY_W;
            int cy = y + NC_MODAL_PAD + row * NC_MODAL_KEY_H;
            int digit = (NC_MODAL_ROWS - 1 - row) * NC_MODAL_COLS + col + 1;
            const nc_footer_item_t *item = 0;
            lvds_color_t key_bg = (active_mask & (1u << digit))
                                      ? NC_VISUAL_FOOTER_VALUE
                                      : NC_VISUAL_FOOTER_BUTTON;
            size_t i;

            for (i = 0; i < count; i++) {
                if ((int)items[i].key - '0' == digit) {
                    item = &items[i];
                    break;
                }
            }

            lvds_draw_fill_rect(cx, cy, NC_MODAL_KEY_W, NC_MODAL_KEY_H, key_bg);
            lvds_draw_rect(cx, cy, NC_MODAL_KEY_W, NC_MODAL_KEY_H,
                           NC_VISUAL_DIM);
            if (!item) {
                continue;
            }
            {
                /* One font for every key, as on the footer, with the digit as
                   the corner marker. The label is clipped to its own length:
                   the draw helper fills the whole column count it is given. */
                char key[4];
                int cols = (NC_MODAL_KEY_W - 8) / NC_VISUAL_CHAR_W;
                int label_len = (int)strlen(item->label);
                int label_x;

                if (label_len > cols) {
                    label_len = cols;
                }
                label_x = cx + (NC_MODAL_KEY_W - label_len * NC_VISUAL_CHAR_W) / 2;
                if (label_x < cx + 4) {
                    label_x = cx + 4;
                }
                snprintf(key, sizeof(key), "%d", digit);
                lvds_draw_text(cx + 5, cy + 4, key, NC_VISUAL_ACCENT,
                               key_bg, LVDS_FONT_NORMAL);
                nc_visual_draw_text_clip(label_x,
                                         cy + (NC_MODAL_KEY_H - NC_FONT_NORMAL_H) / 2,
                                         item->label,
                                         label_len,
                                         NC_VISUAL_FOOTER_TEXT,
                                         key_bg,
                                         LVDS_FONT_NORMAL);
            }
        }
    }
}

static void nc_visual_draw_snapshot(const nc_snapshot_t *s)
{
    int row;
    int line_cols = (NC_RIGHT_PANE_W - NC_LINE_TEXT_X_PAD) / NC_VISUAL_CHAR_W;
    char buf[80];
    char footer_text[160];
    bool full_preview = nc_visual_full_preview();
    bool split = nc_files_active() || (nc_visual_is_code_view() && !full_preview);
    uint32_t t0;
    uint32_t t1;
    uint32_t t2;
    uint32_t t3;
    uint32_t t4;

    lvds_draw_fill_rect(0, 0, LVDS_HSTX_WIDTH, LVDS_HSTX_HEIGHT, NC_VISUAL_BG);
    t0 = mcu_micros();
    {
        char notice_buf[96];
        lvds_color_t notice_fg;
        const char *notice = nc_visual_notice(notice_buf, sizeof(notice_buf),
                                              &notice_fg, &s->runtime);
        nc_visual_draw_tabs(notice, notice_fg);
    }
    nc_visual_draw_header(s);
    t1 = mcu_micros();

    /* Body first, as page background: a frame can be a partial redraw, and how
       much of the body the preview covers changes with the code pane (and while
       browsing, where it follows another file). Without this the corner the
       larger drawing used keeps its old pixels. */
    lvds_draw_fill_rect(0, NC_PANE_Y, LVDS_HSTX_WIDTH, NC_PANE_H, NC_VISUAL_BG);
    if (full_preview) {
        /* The whole body is the preview. Nothing else is drawn in it - no code
           pane, no floating box - so the drawing is as big as the panel allows. */
        nc_visual_draw_thin_preview(&g_nc_visual_doc, &s->runtime, NC_FULL_PREVIEW_X,
                                    NC_PANE_Y, NC_FULL_PREVIEW_W, NC_PANE_H, true);
    } else if (split) {
        /* While the file list is up, the preview follows the file being pointed
           at rather than the one that is open. */
        const nc_document_t *preview_doc =
            (nc_files_active() && g_nc_files_preview_ok) ? &g_nc_files_preview_doc
                                                         : &g_nc_visual_doc;
        nc_visual_draw_thin_preview(preview_doc, &s->runtime, NC_LEFT_PANE_X, NC_PANE_Y,
                                    NC_LEFT_PANE_W, NC_PANE_H, true);
        if (nc_files_active()) {
            int selected = nc_files_selected();
            const char *label = selected >= 0 ? nc_files_name(selected) : "no file picked";

            snprintf(buf, sizeof(buf), "PREVIEW %.28s", label);
            nc_visual_draw_text_clip(NC_LEFT_PANE_X + 12,
                                     NC_PANE_Y + 4,
                                     buf,
                                     34,
                                     g_nc_files_preview_ok ? NC_VISUAL_DIM : NC_VISUAL_ERROR,
                                     NC_VISUAL_PREVIEW_BG,
                                     LVDS_FONT_NORMAL);
        }
        lvds_draw_line(NC_SPLIT_X, NC_PANE_Y, NC_SPLIT_X, NC_PANE_BOTTOM, NC_VISUAL_DIM);
        lvds_draw_fill_rect(NC_RIGHT_PANE_X, NC_PANE_Y, NC_RIGHT_PANE_W, NC_PANE_H, NC_VISUAL_BG);
    } else {
        lvds_draw_fill_rect(20, NC_PANE_Y + 4, LVDS_HSTX_WIDTH - 40, NC_PANE_H - 52, NC_VISUAL_PANEL);
        lvds_draw_rect(20, NC_PANE_Y + 4, LVDS_HSTX_WIDTH - 40, NC_PANE_H - 52, NC_VISUAL_DIM);
    }
    t2 = mcu_micros();

    if (nc_files_active()) {
        int count = nc_files_count();
        int selected_file = nc_files_selected();
        int first_file = 0;
        const int visible_files = 14;
        if (selected_file >= visible_files) {
            first_file = selected_file - visible_files + 1;
        }
        nc_visual_draw_text_clip(NC_RIGHT_PANE_X + 12, NC_PANE_Y + 10, "NC FILES", 18, NC_VISUAL_ACCENT, NC_VISUAL_BG, LVDS_FONT_NORMAL);
        nc_visual_draw_text_clip(NC_RIGHT_PANE_X + 96, NC_PANE_Y + 10, nc_files_cwd(), 32, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
        if (count > 0 && selected_file >= 0) {
            snprintf(buf, sizeof(buf), "%d/%d", selected_file + 1, count);
            nc_visual_draw_text_clip(NC_RIGHT_PANE_X + NC_RIGHT_PANE_W - 70, NC_PANE_Y + 10, buf, 8, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
        }
        if (!nc_files_ready()) {
            nc_visual_draw_text_clip(NC_RIGHT_PANE_X + 16, NC_PANE_Y + 48, "File list not ready", 36, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
        } else if (!count) {
            nc_visual_draw_text_clip(NC_RIGHT_PANE_X + 16, NC_PANE_Y + 48, "No NC files found", 36, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
        }
        for (row = 0; row < count - first_file && row < visible_files; row++) {
            int file_index = first_file + row;
            int y = NC_PANE_Y + 60 + row * 26;
            bool selected = (file_index == selected_file);
            lvds_color_t bg = selected ? NC_VISUAL_SELECT : NC_VISUAL_BG;
            const char *name = nc_files_name(file_index);
            bool is_dir = nc_files_is_dir(file_index);
            snprintf(buf, sizeof(buf), "%c %s", is_dir ? '/' : ' ', name);
            if (selected) {
                lvds_draw_fill_rect(NC_RIGHT_PANE_X + 4, y - 4, NC_RIGHT_PANE_W - 8, 26, bg);
            }
            nc_visual_draw_text_clip(NC_RIGHT_PANE_X + 16,
                                     y,
                                     buf,
                                     line_cols,
                                     selected ? NC_VISUAL_TEXT : NC_VISUAL_DIM,
                                     bg,
                                     LVDS_FONT_NORMAL);
        }
        if (g_nc_visual_new_file_active) {
            snprintf(buf,
                     sizeof(buf),
                     "NEW: %s%s  # OK  * DEL  A CANCEL",
                     g_nc_visual_new_file_name[0] ? g_nc_visual_new_file_name : "_",
                     nc_visual_new_file_ext());
            lvds_draw_fill_rect(NC_RIGHT_PANE_X + 8, NC_PANE_BOTTOM - 26,
                                NC_RIGHT_PANE_W - 16, 22, NC_VISUAL_BG);
            nc_visual_draw_text_clip(NC_RIGHT_PANE_X + 16,
                                     NC_PANE_BOTTOM - 24,
                                     buf,
                                     line_cols,
                                     NC_VISUAL_TEXT,
                                     NC_VISUAL_BG,
                                     LVDS_FONT_NORMAL);
        }
    } else if (g_nc_visual_mode == NC_MODE_TOOLS) {
        nc_visual_draw_tool_screen();
    } else if (nc_visual_is_code_view() && !full_preview) {
        /* The file this code belongs to, as the editor's own first row: the
           cursor can sit on it and Enter opens its folder. */
        {
            bool sel = g_nc_visual_name_selected;
            lvds_color_t bg = sel ? NC_VISUAL_SELECT : NC_VISUAL_BG;
            /* Selected, the name reads as a field, the way a selected word does
               on a code line. */
            lvds_color_t fg = sel ? NC_VISUAL_WORD_FG : NC_VISUAL_DIM;
            lvds_color_t text_bg = sel ? NC_VISUAL_WORD_BG : NC_VISUAL_BG;

            if (sel) {
                lvds_draw_fill_rect(NC_RIGHT_PANE_X, NC_PANE_Y,
                                    NC_RIGHT_PANE_W, NC_VISUAL_ROW_H, bg);
            }
            snprintf(buf,
                     sizeof(buf),
                     "%s%s",
                     s->path[0] ? s->path : "(no file)",
                     s->dirty ? " *" : "");
            nc_visual_draw_text_clip(NC_RIGHT_PANE_X + NC_LINE_TEXT_X_PAD,
                                     NC_PANE_Y,
                                     buf,
                                     line_cols,
                                     fg,
                                     text_bg,
                                     LVDS_FONT_NORMAL);
        }
        for (row = 0; row < NC_MAX_VISIBLE_LINES; row++) {
            int y = NC_CODE_Y + row * NC_VISUAL_ROW_H;
            size_t line_index = s->first_line + (size_t)row;
            bool selected = line_index == nc_visual_code_line();
            char display_line[NC_MAX_LINE_LEN + 2];
            const char *line_text = s->lines[row];
            int word_start = nc_visual_can_edit_code() ? s->selected_word_start : -1;
            int word_end = nc_visual_can_edit_code() ? s->selected_word_end : -1;
            if (line_text[0] != '\t' &&
                line_text[0] != ' ' &&
                nc_visual_line_is_contour_detail(&g_nc_visual_doc, line_index)) {
                display_line[0] = '\t';
                strncpy(display_line + 1, line_text, sizeof(display_line) - 2);
                display_line[sizeof(display_line) - 1] = '\0';
                line_text = display_line;
                if (word_start >= 0) word_start++;
                if (word_end >= 0) word_end++;
            }
            if (selected &&
                nc_visual_can_edit_code() &&
                g_nc_visual_mode != NC_MODE_RUN &&
                s->selected_label[0]) {
                /* The legend takes the row above the cursor line. For line 1
                   that row is the file-name row, so the legend sits over the
                   name for as long as it is showing and the name comes back
                   when it is not - the same way it covers a code line on every
                   other row. */
                int hint_y = (row == 0) ? NC_PANE_Y : (y - NC_VISUAL_ROW_H);

                lvds_draw_fill_rect(NC_RIGHT_PANE_X + 2, hint_y - 3, NC_RIGHT_PANE_W - 4, NC_VISUAL_ROW_H, NC_VISUAL_BG);
                snprintf(buf, sizeof(buf), "%c  %s", g_nc_visual_doc.selected_word >= 0 ? '>' : ' ', s->selected_label);
                nc_visual_draw_text_clip(NC_RIGHT_PANE_X + NC_LINE_TEXT_X_PAD,
                                         hint_y,
                                         buf,
                                         line_cols,
                                         NC_VISUAL_ACCENT,
                                         NC_VISUAL_BG,
                                         LVDS_FONT_NORMAL);
            }
            snprintf(buf, sizeof(buf), "%3lu", (unsigned long)(s->first_line + (size_t)row + 1));
            lvds_draw_text(NC_RIGHT_PANE_X + NC_LINE_NO_X_PAD, y, buf, selected ? NC_VISUAL_LINE_NO_SELECTED : NC_VISUAL_DIM,
                           selected ? NC_VISUAL_SELECT : NC_VISUAL_BG,
                           LVDS_FONT_NORMAL);
            nc_text_draw_line_with_word(line_text,
                                        NC_RIGHT_PANE_X + NC_LINE_TEXT_X_PAD,
                                        y,
                                        line_cols,
                                        NC_VISUAL_CHAR_W,
                                        NC_VISUAL_ROW_H,
                                        word_start,
                                        word_end,
                                        selected,
                                        NC_VISUAL_TEXT,
                                        NC_VISUAL_DIM,
                                        NC_VISUAL_BG,
                                        NC_VISUAL_SELECT,
                                        NC_VISUAL_WORD_FG,
                                        NC_VISUAL_WORD_BG);
        }
    } else if (g_nc_visual_mode == NC_MODE_MANUAL) {
        /* MANUAL is the readout the jog keys work on. Every axis keeps its
           three numbers on one line - where the axis is in the offset in use,
           the stop the feed must not cross, and the machine figure the offset
           is cut from - so nothing has to be looked up on another screen. */
        static const char *const letters[4] = { "X", "Z", "F", "S" };
        const nc_runtime_state_t *runtime = &s->runtime;
        float wco[AXIS_COUNT] = {0};
        bool have_wco = nc_visual_wco(wco);
        int i;

        lvds_draw_fill_rect(0, NC_PANE_Y, LVDS_HSTX_WIDTH, NC_PANE_H, NC_VISUAL_BG);
        {
            /* Column captions, over the columns that are not self-describing. */
            char offset[16];

            nc_visual_draw_text_clip(NC_MANUAL_COL_STOP, NC_PANE_Y + 2,
                                     "STOP", 4, NC_VISUAL_DIM, NC_VISUAL_BG,
                                     LVDS_FONT_SMALL);
            nc_visual_draw_text_clip(NC_MANUAL_COL_MACH, NC_PANE_Y + 2,
                                     "MACHINE", 7, NC_VISUAL_DIM, NC_VISUAL_BG,
                                     LVDS_FONT_SMALL);
            nc_visual_offset_label(offset, sizeof(offset));
            nc_visual_draw_text_clip(NC_MANUAL_COL_MACH + 54, NC_PANE_Y + 2,
                                     offset, 16, NC_VISUAL_ACCENT, NC_VISUAL_BG,
                                     LVDS_FONT_SMALL);
        }
        for (i = 0; i < 4; i++) {
            int y = NC_PANE_Y + 30 + i * NC_MANUAL_ROW_H;
            char value[24];
            char machine[24];
            char stop[24];
            char room[24];
            bool picked = (i < 2) && ((int)g_nc_visual_manual_axis == i);
            lvds_color_t fg = picked ? NC_VISUAL_LINE_NO_SELECTED : NC_VISUAL_TEXT;
            lvds_color_t dim = picked ? NC_VISUAL_LINE_NO_SELECTED : NC_VISUAL_DIM;
            lvds_color_t bg = picked ? NC_VISUAL_SELECT : NC_VISUAL_BG;

            switch (i) {
            case 0: snprintf(value, sizeof(value), "%9.3f", (double)runtime->x); break;
            case 1: snprintf(value, sizeof(value), "%9.3f", (double)runtime->z); break;
            case 2: snprintf(value, sizeof(value), "%8.1f", (double)runtime->feed); break;
            default: snprintf(value, sizeof(value), "%8u", runtime->spindle); break;
            }
            machine[0] = '\0';
            stop[0] = '\0';
            room[0] = '\0';
            if (i < 2) {
                float offset = have_wco ? wco[i == 0 ? AXIS_X : AXIS_Z] : 0.0f;
                double pos = (i == 0) ? (double)runtime->x : (double)runtime->z;

                /* The row reads in the offset in use; the machine figure the
                   offset was cut from keeps its own column, one font smaller
                   and without the caret that only ever restated the column
                   heading. */
                snprintf(value, sizeof(value), "%9.3f", pos - (double)offset);
                snprintf(machine, sizeof(machine), "%9.3f", pos);
                if (g_nc_visual_manual_stop_set[i]) {
                    double limit = (double)g_nc_visual_manual_stop[i];

                    /* The limit '*' set for this axis, in the same frame as the
                       row, with the room left before the feed stops on it. */
                    snprintf(stop, sizeof(stop), "%8.3f", limit - (double)offset);
                    snprintf(room, sizeof(room), "to%7.3f",
                             limit - pos < 0.0 ? pos - limit : limit - pos);
                } else {
                    snprintf(stop, sizeof(stop), "%8s", "--");
                    snprintf(room, sizeof(room), "%s", "press *");
                }
            }
            if (picked) {
                /* The whole line is the selection, the way the editor marks
                   the line its keys are working on. */
                lvds_draw_fill_rect(16, y - 8, LVDS_HSTX_WIDTH - 32,
                                    NC_MANUAL_ROW_H - 20, NC_VISUAL_SELECT);
            }
            /* Every cell is written with its own width so a shorter number
               cannot leave the tail of the last one behind. */
            lvds_draw_text(NC_MANUAL_COL_X, y, letters[i], dim, bg,
                           LVDS_FONT_LARGE);
            nc_visual_draw_text_clip(NC_MANUAL_COL_POS, y, value, 9, fg, bg,
                                     LVDS_FONT_LARGE);
            if (machine[0]) {
                nc_visual_draw_text_clip(NC_MANUAL_COL_MACH, y + 7, machine, 9,
                                         picked ? NC_VISUAL_LINE_NO_SELECTED
                                                : NC_VISUAL_TEXT,
                                         bg, LVDS_FONT_NORMAL);
            }
            if (i < 2) {
                /* Two small rows in the same line: the stop itself, and how far
                   the axis still is from it. */
                nc_visual_draw_text_clip(NC_MANUAL_COL_STOP, y + 1, stop, 9,
                                         picked ? NC_VISUAL_LINE_NO_SELECTED
                                                : NC_VISUAL_ACCENT,
                                         bg, LVDS_FONT_SMALL);
                nc_visual_draw_text_clip(NC_MANUAL_COL_STOP, y + 12, room, 9,
                                         dim, bg, LVDS_FONT_SMALL);
            }
        }
        if (g_nc_visual_manual_touch_active) {
            char field[24];

            snprintf(field, sizeof(field), "TOUCH %c = %s_",
                     nc_visual_manual_letter(),
                     g_nc_visual_manual_touch[0] ? g_nc_visual_manual_touch : "");
            nc_visual_draw_text_clip(60, NC_PANE_Y + 268, field, 24,
                                     NC_VISUAL_ACCENT, NC_VISUAL_BG, LVDS_FONT_LARGE);
        }
        {
            /* The two values the jog keys use, beside the pad they belong to.
               The one the feed mode is on is filled, the way the picked axis
               line is: `1` and `3` change the filled one, so what they change
               is on the screen at the moment they are pressed. */
            const int value_x = NC_RIGHT_PANE_X + 20;
            const int value_w = 150;
            char line[28];
            int row;

            nc_visual_draw_text_clip(value_x, NC_PANE_BOTTOM - 124, "1-    3+", 8,
                                     NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_SMALL);
            for (row = 0; row < 2; row++) {
                bool continuous = (row == 1);
                bool active = (continuous == g_nc_visual_manual_continuous);
                int y = NC_PANE_BOTTOM - 100 + row * 30;

                if (active) {
                    lvds_draw_fill_rect(value_x - 6, y - 4, value_w, 24,
                                        NC_VISUAL_SELECT);
                }
                if (continuous) {
                    snprintf(line, sizeof(line), "FEED %.0f mm/min",
                             (double)nc_visual_manual_feed_value());
                } else {
                    snprintf(line, sizeof(line), "STEP %.3f mm",
                             (double)nc_visual_manual_step_value());
                }
                nc_visual_draw_text_clip(value_x, y,
                                         line, 18,
                                         active ? NC_VISUAL_LINE_NO_SELECTED
                                                : NC_VISUAL_DIM,
                                         active ? NC_VISUAL_SELECT : NC_VISUAL_BG,
                                         LVDS_FONT_NORMAL);
            }
        }
        {
            /* The pad drawn with the same key design the helpers use, so the
               3x3 reads the same everywhere - it just means different things
               here (g_nc_visual_manual_pad). */
            /* Same corner the floating helper uses: right side of the pane,
               sat on the bottom of it and clear of the footer. */
            uint16_t lit = 0u;

            /* The keys that are doing something right now: the spindle
               direction, the key that has a feed running, or - with feed armed
               and nothing feeding - the jog keys of the picked axis. */
            if (g_nc_visual_manual_spindle_dir == '3') {
                lit |= 1u << 9;
            } else if (g_nc_visual_manual_spindle_dir == '4') {
                lit |= 1u << 7;
            }
            if (g_nc_visual_manual_feed_key >= '1' &&
                g_nc_visual_manual_feed_key <= '9') {
                lit |= 1u << (g_nc_visual_manual_feed_key - '0');
            } else if (g_nc_visual_manual_continuous) {
                lit |= (g_nc_visual_manual_axis ? ((1u << 4) | (1u << 6))
                                                : ((1u << 2) | (1u << 8)));
            }
            /* A step jog is over before the next frame, so the key that sent it
               stays lit for a moment: the pad answers the press. */
            if (g_nc_visual_manual_flash_key >= '1' &&
                g_nc_visual_manual_flash_key <= '9' &&
                (uint32_t)(mcu_millis() - g_nc_visual_manual_flash_ms) <
                    NC_MANUAL_FLASH_MS) {
                lit |= 1u << (g_nc_visual_manual_flash_key - '0');
            }
            nc_visual_draw_modal_items(NC_RIGHT_PANE_X + NC_RIGHT_PANE_W - NC_MODAL_W - 8,
                                       NC_PANE_BOTTOM - NC_MODAL_H - 6,
                                       g_nc_visual_manual_pad,
                                       sizeof(g_nc_visual_manual_pad) /
                                           sizeof(g_nc_visual_manual_pad[0]),
                                       lit);
        }
    }
    /* Nothing else to draw in the body: the screens above cover every mode -
       and in EDIT's full-screen state the preview *is* the body, so the old
       "no controls on this screen" placeholder must not be painted over it. */

    if (nc_text_edit_active(&g_nc_visual_edit)) {
        snprintf(buf, sizeof(buf), "EDIT %s", nc_text_edit_buffer(&g_nc_visual_edit));
        lvds_draw_fill_rect(20, 526, LVDS_HSTX_WIDTH - 40, 18, NC_VISUAL_BG);
        nc_visual_draw_text_clip(34, 526, buf, 70, NC_VISUAL_ACCENT, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    }
    if (g_nc_modal_active && !g_nc_modal_prefix) {
        int visible = s->cursor_visible_index;
        int keypad_y;
        int modal_x;
        if (visible < 0) {
            visible = 0;
        }
        /* The helper belongs to the line under the cursor - the line that
           carries the action name and will take the text. It hangs below that
           line instead of covering the line the user is reading. */
        keypad_y = NC_CODE_Y + (visible + 1) * NC_VISUAL_ROW_H - NC_MODAL_PAD;
        keypad_y = nc_visual_clampi(keypad_y, NC_CODE_Y,
                                    NC_PANE_BOTTOM - NC_MODAL_PAD -
                                    NC_MODAL_KEY_H * NC_MODAL_ROWS);
        /* Floating on the right, the way the machine's keypad sits beside the
           screen, and pinned so the keys end at the pane bottom when the helper
           line is too low to hang below it. */
        modal_x = NC_RIGHT_PANE_X + NC_RIGHT_PANE_W - NC_MODAL_W - 8;
        /* The G/T field takes its digits on the line itself, so only a submenu
           has a helper to draw. */
        nc_visual_draw_modal_items(modal_x, keypad_y, g_nc_modal_items,
                                   g_nc_modal_count, 0u);
    }
    t3 = mcu_micros();

    nc_visual_footer_text(footer_text, sizeof(footer_text));
    nc_visual_draw_footer_status("", footer_text);
    t4 = mcu_micros();

    g_nc_visual_frame_header_us += t1 - t0;
    g_nc_visual_frame_preview_us += t2 - t1;
    g_nc_visual_frame_body_us += t3 - t2;
    g_nc_visual_frame_footer_us += t4 - t3;
}

static void nc_visual_draw_live_snapshot(const nc_snapshot_t *s)
{
    uint32_t t0;
    uint32_t t1;
    uint32_t t2;

    if (!s) {
        return;
    }
    t0 = mcu_micros();
    {
        char notice_buf[96];
        lvds_color_t notice_fg;
        const char *notice = nc_visual_notice(notice_buf, sizeof(notice_buf),
                                              &notice_fg, &s->runtime);
        nc_visual_draw_tabs(notice, notice_fg);
    }
    nc_visual_draw_header(s);
    t1 = mcu_micros();
    nc_visual_draw_thin_preview(&g_nc_visual_doc, &s->runtime, NC_LEFT_PANE_X, NC_PANE_Y,
                                NC_LEFT_PANE_W, NC_PANE_H, false);
    t2 = mcu_micros();
    g_nc_visual_frame_header_us += t1 - t0;
    g_nc_visual_frame_preview_us += t2 - t1;
}

void nc_visual_init(void)
{
    nc_palette_init();
    nc_files_init();
    nc_presets_init();
    nc_run_init();
    ADD_EVENT_LISTENER(cnc_parse_cmd_error, nc_visual_parse_error);
    ADD_EVENT_LISTENER(parser_reset, nc_visual_parser_reset);
    nc_state_init();
    g_nc_visual_mode = nc_state_mode();
    if (g_nc_visual_mode < 0 || g_nc_visual_mode >= NC_MODE_COUNT) {
        g_nc_visual_mode = NC_MODE_PROGRAM;
    }
    if (nc_visual_uses_file() &&
        nc_state_load_document(g_nc_visual_mode, &g_nc_visual_doc)) {
        snprintf(g_nc_visual_status,
                 sizeof(g_nc_visual_status),
                 "%s: %.48s",
                 nc_menu_mode_name(g_nc_visual_mode),
                 g_nc_visual_doc.path);
        g_nc_visual_dirty = true;
    } else if (!nc_state_load_document(NC_MODE_PROGRAM, &g_nc_visual_doc)) {
        nc_visual_seed_demo();
        nc_state_remember_path(NC_MODE_PROGRAM, "");
        nc_state_save();
    } else {
        snprintf(g_nc_visual_status,
                 sizeof(g_nc_visual_status),
                 "%s: %.48s",
                 nc_menu_mode_name(g_nc_visual_mode),
                 g_nc_visual_doc.path);
        g_nc_visual_dirty = true;
    }
    lvds_hstx_clear(NC_VISUAL_BG);
    lvds_draw_text(24, NC_PANE_Y + 10, "Waiting for NC snapshot", NC_VISUAL_TEXT, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    lvds_hstx_present();
}

static void nc_visual_handle_key_impl(nc_visual_key_t key)
{
    /* A message is transient: the next key press hands the line back to the
       normal status text, so nothing has to be dismissed. */
    nc_message_clear();
    /* The SD card is mounted from the main loop, so the preset file can only
       be read or seeded once the machine has been running. This is the first
       point where the drive is expected to answer. */
    (void)nc_presets_sync();
    uint8_t footer_action;

    if (nc_visual_modal_handle_key(key)) {
        return;
    }
    if (!nc_files_active() && g_nc_visual_mode == NC_MODE_MANUAL) {
        char manual_ch = nc_visual_key_char(key);

        if (g_nc_visual_manual_touch_active) {
            size_t len = strlen(g_nc_visual_manual_touch);

            /* Touch-off: type the value, D (or Enter) applies it. */
            if (key == NC_VISUAL_KEY_ACCEPT || key == NC_VISUAL_KEY_FINISH) {
                nc_visual_manual_touch_apply();
                return;
            }
            if (key == NC_VISUAL_KEY_CANCEL || key == NC_VISUAL_KEY_MODE) {
                g_nc_visual_manual_touch_active = false;
                g_nc_visual_manual_touch[0] = '\0';
                g_nc_visual_dirty = true;
                return;
            }
            if (manual_ch >= '0' && manual_ch <= '9' &&
                len + 1u < sizeof(g_nc_visual_manual_touch)) {
                g_nc_visual_manual_touch[len] = manual_ch;
                g_nc_visual_manual_touch[len + 1u] = '\0';
                g_nc_visual_dirty = true;
                return;
            }
            if ((manual_ch == '-' || manual_ch == '.') && len == 0u) {
                g_nc_visual_manual_touch[len] = manual_ch;
                g_nc_visual_manual_touch[len + 1u] = '\0';
                g_nc_visual_dirty = true;
                return;
            }
            if (key == NC_VISUAL_KEY_BACKSPACE) {
                if (len > 0u) {
                    g_nc_visual_manual_touch[len - 1u] = '\0';
                }
                g_nc_visual_dirty = true;
                return;
            }
            return;                 /* swallow the rest while the field is open */
        }
        if (manual_ch && nc_visual_manual_handle_digit(manual_ch)) {
            return;
        }
        if (key == NC_VISUAL_KEY_FINISH) {          /* '#' */
            nc_visual_manual_step_toggle();
            return;
        }
        if (key == NC_VISUAL_KEY_BACKSPACE) {       /* '*' */
            /* Stop stops the feed; only an idle axis arms or clears the wall. */
            if (g_nc_visual_manual_feed_key) {
                nc_visual_manual_feed_cancel();
                return;
            }
            nc_visual_manual_stop_toggle();
            return;
        }
    }
    if (g_nc_visual_name_selected) {
        if (key == NC_VISUAL_KEY_ACCEPT) {
            /* The file-name row is the way into its folder. */
            nc_visual_open_current_folder();
            g_nc_visual_status[0] = '\0';
            g_nc_visual_dirty = true;
            return;
        }
        if (key != NC_VISUAL_KEY_PREV && key != NC_VISUAL_KEY_NEXT &&
            key != NC_VISUAL_KEY_MODE) {
            /* Anything else hands the cursor back to the code. */
            g_nc_visual_name_selected = false;
        }
    }
    if (!nc_files_active() &&
        g_nc_visual_mode == NC_MODE_TOOLS &&
        (key == NC_VISUAL_KEY_PREV ||
         key == NC_VISUAL_KEY_NEXT ||
         key == NC_VISUAL_KEY_FIELD_PREV ||
         key == NC_VISUAL_KEY_FIELD_NEXT ||
         key == NC_VISUAL_KEY_WORD_PREV ||
         key == NC_VISUAL_KEY_WORD_NEXT)) {
        bool forward = key == NC_VISUAL_KEY_NEXT ||
                       key == NC_VISUAL_KEY_FIELD_NEXT ||
                       key == NC_VISUAL_KEY_WORD_NEXT;
        nc_visual_dispatch_footer_action(forward ? NC_FOOTER_ACTION_STEP :
                                                   NC_FOOTER_ACTION_BACK);
        g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
        g_nc_visual_dirty = true;
        return;
    }

    if (nc_visual_new_file_handle_key(key)) {
        g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
        g_nc_visual_dirty = true;
        return;
    }
    if (key == NC_VISUAL_KEY_WORD_PREV || key == NC_VISUAL_KEY_WORD_NEXT) {
        nc_result_t result = key == NC_VISUAL_KEY_WORD_PREV ?
                             nc_select_prev_word(&g_nc_visual_doc) :
                             nc_select_next_word(&g_nc_visual_doc);
        snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "%s",
                 result == NC_OK ? "Word" : "No editable word on this line");
        g_nc_visual_dirty = true;
        return;
    }
    if (key == NC_VISUAL_KEY_FIELD_PREV || key == NC_VISUAL_KEY_FIELD_NEXT) {
        bool forward = key == NC_VISUAL_KEY_FIELD_NEXT;
        nc_word_t word;
        nc_result_t result;

        /* TOOLS uses Up/Down to select tool rows. Field walking is for the
           editor screens; on TOOLS the row selection is the cursor. */
        if (!nc_files_active() && g_nc_visual_mode == NC_MODE_TOOLS) {
            nc_visual_dispatch_footer_action(forward ? NC_FOOTER_ACTION_STEP :
                                                       NC_FOOTER_ACTION_BACK);
            g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
            g_nc_visual_dirty = true;
            return;
        }

        /* Field walking belongs to editable code views only. In the file list,
           a menu, or a view that only runs and simulates (RUN, SIM) these keys
           keep their original path through the footer actions, which also
           reports the newly selected line - moving the cursor directly here
           would move the highlight without registering the selection. */
        if (nc_files_active() || !nc_visual_is_code_view() ||
            !nc_visual_can_edit_code()) {
            nc_visual_handle_key(forward ? NC_VISUAL_KEY_NEXT : NC_VISUAL_KEY_PREV);
            return;
        }
        /* Arrows never edit: drop any draft instead of applying it, so a stray
           decimal point cannot be pushed into the program. */
        nc_text_edit_clear(&g_nc_visual_edit);
        result = forward ? nc_select_same_word_next(&g_nc_visual_doc) :
                           nc_select_same_word_prev(&g_nc_visual_doc);
        if (result == NC_OK &&
            nc_get_selected_word(&g_nc_visual_doc, &word) == NC_OK) {
            /* The field itself is highlighted; no position report. */
            g_nc_visual_status[0] = '\0';
            g_nc_visual_dirty = true;
            return;
        }
        /* No field of that letter to go to (including "no word selected at
           all"): move one line directly. Going through the key dispatch here
           is what caused the bug, because while a word is selected those
           letters mean sign (B), dot (C) and next word/accept (D). Moving the
           cursor ourselves cannot start an edit and cannot write a value. */
        if (forward) {
            if (g_nc_visual_name_selected) {
                /* Down from the file-name row is line 1. */
                g_nc_visual_name_selected = false;
            } else {
                nc_cursor_down(&g_nc_visual_doc);
            }
        } else if (g_nc_visual_doc.cursor_line == 0) {
            /* Above line 1 sits the row naming the file. */
            g_nc_visual_name_selected = true;
        } else {
            nc_cursor_up(&g_nc_visual_doc);
        }
        g_nc_visual_doc.selected_word = -1;
        nc_text_edit_clear(&g_nc_visual_edit);
        g_nc_visual_status[0] = '\0';
        g_nc_visual_dirty = true;
        return;
    }

    if (key == NC_VISUAL_KEY_MODE) {
        nc_text_edit_clear(&g_nc_visual_edit);
        g_nc_visual_new_file_active = false;
        nc_visual_cycle_mode();
        g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
        g_nc_visual_dirty = true;
        return;
    }

    if (nc_visual_handle_selected_word_edit(key)) {
        g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
        g_nc_visual_dirty = true;
        return;
    }

    if ((key == NC_VISUAL_KEY_PREV || key == NC_VISUAL_KEY_NEXT) &&
        (nc_files_active() ||
         nc_visual_is_code_view() ||
         g_nc_visual_mode == NC_MODE_TOOLS)) {
        nc_text_edit_clear(&g_nc_visual_edit);
        nc_visual_dispatch_footer_action(key == NC_VISUAL_KEY_PREV ? NC_FOOTER_ACTION_BACK : NC_FOOTER_ACTION_STEP);
        g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
        g_nc_visual_dirty = true;
        return;
    }

    footer_action = nc_visual_footer_action_for_key(key);
    if (footer_action != NC_FOOTER_ACTION_NONE) {
        nc_visual_dispatch_footer_action(footer_action);
        g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
        g_nc_visual_dirty = true;
        return;
    }

    /* `0` opens the file list on every screen that works on a document, as a
       key of the screen rather than a slot in the strip: the footer carries the
       keys that mean something special here, and opening a file is the same
       meaning everywhere. MANUAL's `0 ZERO` is a footer entry and wins above. */
    if (key == NC_VISUAL_KEY_DIGIT_0 &&
        (nc_visual_is_code_view() || g_nc_visual_mode == NC_MODE_TOOLS) &&
        !nc_files_active()) {
        nc_visual_dispatch_footer_action(NC_FOOTER_ACTION_FILES);
        g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
        g_nc_visual_dirty = true;
        return;
    }

    if (!nc_visual_can_edit_code()) {
        /* A view screen (RUN, SIM) or the file list: the keys it takes are
           named in its footer, and the rest have no meaning here. Say that
           plainly instead of the old stub-screen text, which pointed at a mode
           key that is not what the operator is looking for. */
        strncpy(g_nc_visual_status, "No action here", sizeof(g_nc_visual_status) - 1);
        g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
        g_nc_visual_dirty = true;
        return;
    }

    switch (key) {
    case NC_VISUAL_KEY_PREV:
        nc_text_edit_clear(&g_nc_visual_edit);
        nc_cursor_up(&g_nc_visual_doc);
        g_nc_visual_status[0] = '\0';
        break;
    case NC_VISUAL_KEY_NEXT:
        nc_text_edit_clear(&g_nc_visual_edit);
        nc_cursor_down(&g_nc_visual_doc);
        g_nc_visual_status[0] = '\0';
        break;
    case NC_VISUAL_KEY_ACCEPT:
    case NC_VISUAL_KEY_FINISH:
        if (nc_select_next_word(&g_nc_visual_doc) == NC_OK) {
            strncpy(g_nc_visual_status, "Next word", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "No editable word here", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_VISUAL_KEY_BACKSPACE:
        if (nc_select_prev_word(&g_nc_visual_doc) == NC_OK) {
            strncpy(g_nc_visual_status, "Previous word", sizeof(g_nc_visual_status) - 1);
        } else {
            strncpy(g_nc_visual_status, "No editable word here", sizeof(g_nc_visual_status) - 1);
        }
        break;
    case NC_VISUAL_KEY_CANCEL:
        nc_text_edit_clear(&g_nc_visual_edit);
        g_nc_visual_doc.selected_word = -1;
        strncpy(g_nc_visual_status, "Selection cleared", sizeof(g_nc_visual_status) - 1);
        break;
    default:
        break;
    }

    g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
    g_nc_visual_dirty = true;
}

void nc_visual_handle_key(nc_visual_key_t key)
{
    g_nc_visual_last_input_ms = mcu_millis();
    nc_visual_handle_key_impl(key);
    /* Browsing the card may have moved the list's cursor: refresh the preview
       of whatever is pointed at now. A no-op unless the selection changed. */
    if (nc_files_active()) {
        nc_files_preview_sync();
    }
}

/* Called from the main loop: write the remembered state once the operator has
   stopped pressing keys, so no FAT write sits in the middle of a screen
   change. */
void nc_visual_idle_tasks(void)
{
    if (g_nc_visual_last_input_ms == 0u ||
        (uint32_t)(mcu_millis() - g_nc_visual_last_input_ms) < NC_VISUAL_IDLE_FLUSH_MS) {
        return;
    }
    nc_state_flush();
}

bool nc_visual_dirty(void)
{
    return g_nc_visual_dirty;
}

const nc_footer_item_t *nc_visual_footer(size_t *count)
{
    return nc_visual_footer_items(count);
}

void nc_visual_hold_key(char key)
{
    bool changed = key != g_nc_visual_manual_held_key;

    g_nc_visual_manual_held_key = key;
    if (!g_nc_visual_manual_feed_key) {
        return;                             /* nothing is feeding */
    }
    /* An alarm or a program taking the reader stops the feed here as well as
       in the controller: the panel must not believe a jog is still running. */
    if (cnc_has_alarm() || nc_run_streaming()) {
        nc_visual_manual_feed_cancel();
        return;
    }
    /* The key that started the feed is the only key that keeps it running.
       Letting go, or reaching for another key, ends it where it is. */
    if (changed && key != g_nc_visual_manual_feed_key) {
        nc_visual_manual_feed_cancel();
    }
}

nc_visual_key_t nc_visual_key_for_char(char key)
{
    if (key >= '0' && key <= '9') {
        return (nc_visual_key_t)(NC_VISUAL_KEY_DIGIT_0 + (key - '0'));
    }

    switch (key) {
    case '*': return NC_VISUAL_KEY_BACKSPACE;
    case '#': return NC_VISUAL_KEY_FINISH;
    /* The keypad's cancel key is the mode key: the same key that leaves a
       half-typed word alone and the one that cycles the screens. */
    case 'A': return NC_VISUAL_KEY_MODE;
    /* B and C step between equal fields, which is what the footer's B/C
       entries mean on a screen that has no fields: the screen decides. */
    case 'B': return NC_VISUAL_KEY_FIELD_PREV;
    case 'C': return NC_VISUAL_KEY_FIELD_NEXT;
    case 'D': return NC_VISUAL_KEY_ACCEPT;
    default: return NC_VISUAL_KEY_NONE;
    }
}

const char *nc_visual_key_hint(char key)
{
    size_t i;

    if (g_nc_visual_mode != NC_MODE_MANUAL || nc_files_active()) {
        return 0;
    }
    for (i = 0; i < sizeof(g_nc_visual_manual_pad) / sizeof(g_nc_visual_manual_pad[0]); i++) {
        if (g_nc_visual_manual_pad[i].key == key) {
            return g_nc_visual_manual_pad[i].label;
        }
    }
    return 0;
}

bool nc_visual_periodic_needed(void)
{
    uint32_t signature = cnc_get_exec_state(EXEC_ALLACTIVE) |
                         ((uint32_t)nc_visual_settings_error() << 16) |
                         ((uint32_t)cnc_has_alarm() << 24) |
                         ((uint32_t)nc_run_error() << 25);
    if (signature != g_nc_visual_notice_signature) {
        g_nc_visual_notice_signature = signature;
        g_nc_visual_dirty = true;
        return true;
    }
    return g_nc_visual_mode == NC_MODE_MANUAL ||
           (g_nc_visual_mode == NC_MODE_RUN &&
            (nc_run_active() ||
             nc_run_hold() ||
             cnc_get_exec_state(EXEC_RUN | EXEC_HOLD) ||
             g_nc_visual_last_runtime_busy));
}

void nc_visual_draw(void)
{
    nc_snapshot_t snapshot;
    uint32_t t0;
    uint32_t t1;
    uint32_t t2;
    uint32_t t3;
    bool full_draw;
    bool runtime_busy;
    size_t run_line;

    if (g_nc_visual_in_draw) {
        return;
    }

    g_nc_visual_in_draw = true;

    g_nc_visual_frame_header_us = 0;
    g_nc_visual_frame_preview_us = 0;
    g_nc_visual_frame_body_us = 0;
    g_nc_visual_frame_footer_us = 0;
    g_nc_visual_frame_preview_collect_us = 0;
    g_nc_visual_frame_preview_clear_us = 0;
    g_nc_visual_frame_preview_stock_us = 0;
    g_nc_visual_frame_preview_geom_us = 0;
    g_nc_visual_frame_preview_tool_us = 0;
    t0 = mcu_micros();
    nc_state_snapshot(&g_nc_visual_doc, &snapshot);
    t1 = mcu_micros();
    run_line = nc_run_line();
    runtime_busy = nc_visual_runtime_busy(&snapshot.runtime);
    full_draw = g_nc_visual_dirty ||
                g_nc_visual_mode != NC_MODE_RUN ||
                (g_nc_visual_last_runtime_busy && !runtime_busy) ||
                (!runtime_busy && !nc_run_active() && !nc_run_hold()) ||
                run_line != g_nc_visual_last_draw_run_line;
    if (full_draw) {
        g_nc_live_tool_rect_valid = false;
        g_nc_live_preview_cache_valid = false;
        nc_visual_draw_snapshot(&snapshot);
    } else {
        nc_visual_draw_live_snapshot(&snapshot);
    }
    t2 = mcu_micros();
    lvds_hstx_present();
    t3 = mcu_micros();
    nc_visual_fps_tick(t1 - t0, t2 - t1, t3 - t2, t3 - t0);

    g_nc_visual_last_draw_run_line = g_nc_visual_mode == NC_MODE_RUN ? run_line : (size_t)-1;
    g_nc_visual_last_runtime_busy = g_nc_visual_mode == NC_MODE_RUN && runtime_busy;
    g_nc_visual_dirty = false;
    g_nc_visual_in_draw = false;
}

static void nc_visual_fps_tick(uint32_t snapshot_us,
                               uint32_t draw_us,
                               uint32_t present_us,
                               uint32_t total_us)
{
    uint32_t now = mcu_millis();
    uint32_t elapsed;

    if (!g_nc_visual_fps_last_ms) {
        g_nc_visual_fps_last_ms = now;
    }
    g_nc_visual_fps_frames++;
    g_nc_visual_acc_snapshot_us += snapshot_us;
    g_nc_visual_acc_draw_us += draw_us;
    g_nc_visual_acc_present_us += present_us;
    g_nc_visual_acc_total_us += total_us;
    g_nc_visual_acc_header_us += g_nc_visual_frame_header_us;
    g_nc_visual_acc_preview_us += g_nc_visual_frame_preview_us;
    g_nc_visual_acc_body_us += g_nc_visual_frame_body_us;
    g_nc_visual_acc_footer_us += g_nc_visual_frame_footer_us;
    g_nc_visual_acc_preview_collect_us += g_nc_visual_frame_preview_collect_us;
    g_nc_visual_acc_preview_clear_us += g_nc_visual_frame_preview_clear_us;
    g_nc_visual_acc_preview_stock_us += g_nc_visual_frame_preview_stock_us;
    g_nc_visual_acc_preview_geom_us += g_nc_visual_frame_preview_geom_us;
    g_nc_visual_acc_preview_tool_us += g_nc_visual_frame_preview_tool_us;
    elapsed = now - g_nc_visual_fps_last_ms;
    if (elapsed >= 1000u) {
        g_nc_visual_fps = (uint16_t)(((uint32_t)g_nc_visual_fps_frames * 1000u) / elapsed);
        if (g_nc_visual_fps_frames) {
            g_nc_visual_stat_snapshot_ms = (uint16_t)((g_nc_visual_acc_snapshot_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_draw_ms = (uint16_t)((g_nc_visual_acc_draw_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_present_ms = (uint16_t)((g_nc_visual_acc_present_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_total_ms = (uint16_t)((g_nc_visual_acc_total_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_header_ms = (uint16_t)((g_nc_visual_acc_header_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_preview_ms = (uint16_t)((g_nc_visual_acc_preview_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_body_ms = (uint16_t)((g_nc_visual_acc_body_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_footer_ms = (uint16_t)((g_nc_visual_acc_footer_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_preview_collect_ms = (uint16_t)((g_nc_visual_acc_preview_collect_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_preview_clear_ms = (uint16_t)((g_nc_visual_acc_preview_clear_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_preview_stock_ms = (uint16_t)((g_nc_visual_acc_preview_stock_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_preview_geom_ms = (uint16_t)((g_nc_visual_acc_preview_geom_us / g_nc_visual_fps_frames + 500u) / 1000u);
            g_nc_visual_stat_preview_tool_ms = (uint16_t)((g_nc_visual_acc_preview_tool_us / g_nc_visual_fps_frames + 500u) / 1000u);
        }
        g_nc_visual_fps_frames = 0;
        g_nc_visual_acc_snapshot_us = 0;
        g_nc_visual_acc_draw_us = 0;
        g_nc_visual_acc_present_us = 0;
        g_nc_visual_acc_total_us = 0;
        g_nc_visual_acc_header_us = 0;
        g_nc_visual_acc_preview_us = 0;
        g_nc_visual_acc_body_us = 0;
        g_nc_visual_acc_footer_us = 0;
        g_nc_visual_acc_preview_collect_us = 0;
        g_nc_visual_acc_preview_clear_us = 0;
        g_nc_visual_acc_preview_stock_us = 0;
        g_nc_visual_acc_preview_geom_us = 0;
        g_nc_visual_acc_preview_tool_us = 0;
        g_nc_visual_fps_last_ms = now;
    }
}
