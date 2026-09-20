#include "nc_visual.h"

#include "../../cnc.h"
#include "../../interface/grbl_stream.h"
#include "nc.h"
#include "nc_draw.h"
#include "nc_editor.h"
#include "nc_feedback.h"
#include "nc_files.h"
#include "nc_layout.h"
#include "nc_manual.h"
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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

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

/* The parser is running or held. The preview is told the answer instead of
   asking: the screen owns what the machine is doing. */
static bool nc_visual_runtime_busy(const nc_runtime_state_t *runtime)
{
    return runtime && (runtime->exec_state & (EXEC_RUN | EXEC_HOLD));
}

/* The work coordinate offset the parser is applying. parser_get_wco() reports
   at a limited rate and leaves the array untouched when it declines, so the
   last answer is kept: a caption must not flicker between two meanings. The
   header and MANUAL's readout both name the same offset, so it is read once,
   here, and the pane is given the answer. */
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

/* MANUAL's pane: this frame's figures, and the offset the readout is cut from.
   The screen reads both once - the header above shows the same label. */
static void nc_visual_manual_pane(const nc_runtime_state_t *runtime)
{
    nc_manual_view_t view;
    float wco[AXIS_COUNT] = {0};
    char label[16];

    view.runtime = runtime;
    view.have_wco = nc_visual_wco(wco);
    view.wco_x = wco[AXIS_X];
    view.wco_z = wco[AXIS_Z];
    nc_visual_offset_label(label, sizeof(label));
    view.wco_label = label;
    nc_manual_draw(&view);
}

/* What MANUAL may write outside itself: this screen's status line and the flag
   that asks for a repaint. The screen keeps owning both. */
static void nc_visual_manual_screen(nc_manual_screen_t *screen)
{
    screen->status = g_nc_visual_status;
    screen->status_size = sizeof(g_nc_visual_status);
    screen->dirty = &g_nc_visual_dirty;
}

static void nc_files_preview_sync(void);
static bool nc_visual_can_edit_code(void);
static bool nc_visual_is_code_view(void);
static void nc_visual_dispatch_footer_action(uint8_t action);
static bool nc_visual_full_preview(void);

static void nc_visual_fps_tick(uint32_t snapshot_us,
                               uint32_t draw_us,
                               uint32_t present_us,
                               uint32_t total_us);

static nc_mode_t g_nc_visual_mode = NC_MODE_MANUAL;
static uint8_t g_nc_visual_selected_action = NC_FOOTER_ACTION_NONE;
/* The editor's file-name row above line 1 can hold the cursor. */
/* Screen idle time is tracked so the state store is written when the operator
   stops moving, never in the middle of a screen change. */
#define NC_VISUAL_IDLE_FLUSH_MS 400u
static uint32_t g_nc_visual_last_input_ms;

/* The editor's view of this screen: the document is the screen's, and so are the
   status line and the repaint flag. `s` is the frame being drawn, which only the
   editor's panels need. */
static void nc_visual_editor_ctx(nc_editor_ctx_t *ctx, const nc_snapshot_t *s)
{
    ctx->doc = &g_nc_visual_doc;
    ctx->snapshot = s;
    ctx->mode = g_nc_visual_mode;
    ctx->editable = nc_visual_can_edit_code();
    ctx->code_view = nc_visual_is_code_view();
    ctx->status = g_nc_visual_status;
    ctx->status_size = sizeof(g_nc_visual_status);
    ctx->dirty = &g_nc_visual_dirty;
    ctx->follow = NC_FOOTER_ACTION_NONE;
}

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

/* RUN arms and steps only from a file it can read as a program. */
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
    nc_manual_screen_t manual;

    if (mode < 0 || mode >= NC_MODE_COUNT) {
        return;
    }
    /* Leaving MANUAL must not leave a jog running behind the next screen. */
    nc_visual_manual_screen(&manual);
    nc_manual_feed_cancel(&manual);
    g_nc_visual_mode = mode;
    nc_state_set_mode(mode);
    nc_state_save();
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
    /* The preview lights its own keys: the screen reads the layer back. */
    case NC_FOOTER_ACTION_DIMS: return nc_preview_layer(NC_PREVIEW_LAYER_DIMS);
    case NC_FOOTER_ACTION_STOCK: return nc_preview_layer(NC_PREVIEW_LAYER_STOCK);
    case NC_FOOTER_ACTION_PATH: return nc_preview_layer(NC_PREVIEW_LAYER_PATH);
    case NC_FOOTER_ACTION_ROUGH: return nc_preview_layer(NC_PREVIEW_LAYER_ROUGH);
    case NC_FOOTER_ACTION_VIEW: return g_nc_visual_show_code;
    default: return false;
    }
}

/* Greedy word wrap for the footer keys: `text` onto at most NC_FOOTER_LINES
   lines of `cols` columns. A word longer than the line is split. */

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

/* One frame of the preview: the request is what the screen knows, the times
   come back so the frame counter can report what the drawing cost. */
static void nc_visual_preview(const nc_snapshot_t *s,
                              const nc_document_t *doc,
                              int x,
                              int y,
                              int w,
                              int h,
                              bool clear_bg)
{
    nc_preview_ctx_t ctx;
    nc_preview_times_t times = { 0 };

    ctx.doc = doc;
    ctx.screen_doc = &g_nc_visual_doc;
    ctx.runtime = &s->runtime;
    ctx.tool_path = nc_visual_tool_path();
    ctx.run_line = nc_run_line();
    ctx.mode = g_nc_visual_mode;
    ctx.status = g_nc_visual_status;
    ctx.status_size = sizeof(g_nc_visual_status);
    ctx.full = nc_visual_full_preview();
    ctx.runtime_busy = nc_visual_runtime_busy(&s->runtime);
    ctx.streaming = nc_run_active();
    ctx.hold = nc_run_hold();
    nc_preview_draw(&ctx, x, y, w, h, clear_bg, &times);
    g_nc_visual_frame_preview_collect_us += times.collect;
    g_nc_visual_frame_preview_clear_us += times.clear;
    g_nc_visual_frame_preview_stock_us += times.stock;
    g_nc_visual_frame_preview_geom_us += times.geom;
    g_nc_visual_frame_preview_tool_us += times.tool;
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













static void nc_visual_dispatch_footer_action(uint8_t action)
{
    nc_editor_ctx_t editor;

    nc_visual_editor_ctx(&editor, 0);
    g_nc_visual_selected_action = action;

    switch (action) {
    case NC_FOOTER_ACTION_OPS:
    case NC_FOOTER_ACTION_TOOL_MENU:
    case NC_FOOTER_ACTION_GCODE:
    case NC_FOOTER_ACTION_G7X_MENU:
    case NC_FOOTER_ACTION_SYNC_MENU:
    case NC_FOOTER_ACTION_PECK_MENU:
        nc_editor_open_modal(&editor, action);
        return;
    case NC_FOOTER_ACTION_TOOL_SELECT:
        nc_editor_open_field(&editor, 'T');
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
            nc_editor_open_current_folder(&editor);
        } else {
            nc_editor_open_files(&editor, NC_FILES_DIR, true);
        }
        break;
    case NC_FOOTER_ACTION_FILES:
        nc_editor_open_files(&editor, NULL, false);
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
            if (!nc_editor_save_current(&editor) &&
                !nc_editor_proceed_without_saving(&editor, NC_EDITOR_UNSAVED_OPEN)) {
                strncpy(g_nc_visual_status, "Save failed - press again to open", sizeof(g_nc_visual_status) - 1);
                break;
            }
            r = nc_load_file(&g_nc_visual_doc, path);
            if (r == NC_OK) {
                nc_files_set_active(false);
                nc_editor_new_file_end(&editor);
                nc_state_remember_path(g_nc_visual_mode, path);
                nc_state_save();
                snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Opened %s", nc_files_name(nc_files_selected()));
            } else {
                snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Open failed: %s", nc_result_text(r));
            }
        } else {
            nc_files_set_active(true);
            nc_editor_new_file_end(&editor);
            if (nc_files_refresh(NULL)) {
                nc_editor_serial_selected_file();
                g_nc_visual_status[0] = '\0';
            } else {
                strncpy(g_nc_visual_status, "File list unavailable", sizeof(g_nc_visual_status) - 1);
            }
        }
        break;
    case NC_FOOTER_ACTION_PRESET_OD:
        nc_editor_insert_preset(&editor, NC_PRESET_OD, "Inserted OD preset", "OD preset failed");
        break;
    case NC_FOOTER_ACTION_PRESET_ID:
        nc_editor_insert_preset(&editor, NC_PRESET_ID, "Inserted ID preset", "ID preset failed");
        break;
    case NC_FOOTER_ACTION_PRESET_FACE:
        nc_editor_insert_preset(&editor, NC_PRESET_FACE, "Inserted FACE preset", "FACE preset failed");
        break;
    case NC_FOOTER_ACTION_PRESET_LINE:
        nc_editor_insert_preset(&editor, NC_PRESET_LINE, "Inserted line preset", "Line preset failed");
        break;
    case NC_FOOTER_ACTION_PRESET_ARC:
        nc_editor_insert_preset(&editor, NC_PRESET_ARC, "Inserted arc preset", "Arc preset failed");
        break;
    case NC_FOOTER_ACTION_PRESET_SETUP:
        nc_editor_insert_preset(&editor, NC_PRESET_SETUP, "Inserted setup preset", "Setup preset failed");
        break;
    case NC_FOOTER_ACTION_PRESET_END:
        nc_editor_insert_preset(&editor, NC_PRESET_END, "Inserted G80", "G80 preset failed");
        break;
    case NC_FOOTER_ACTION_TOOL:
        if (g_nc_visual_mode == NC_MODE_PROGRAM) {
            if (nc_editor_insert_tool_ref(&editor) == NC_OK) {
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
            nc_editor_clear_draft();
            if (nc_state_load_document(g_nc_visual_mode, &g_nc_visual_doc)) {
                if (g_nc_visual_doc.line_count && !nc_tool_line_is_tool(g_nc_visual_doc.lines[g_nc_visual_doc.cursor_line].text)) {
                    int first_line = -1;
                    (void)nc_editor_find_tool_line(&editor, 0, &first_line);
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
            nc_editor_new_file_begin(&editor);
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
            nc_editor_serial_selected_file();
            strncpy(g_nc_visual_status, "File up", sizeof(g_nc_visual_status) - 1);
        } else if (g_nc_visual_mode == NC_MODE_TOOLS) {
            nc_editor_move_tool_line(&editor, -1);
        } else {
            nc_editor_move_line(&editor, -1);
            nc_editor_serial_selected_line(&editor);
        }
        break;
    case NC_FOOTER_ACTION_STEP:
        if (nc_files_active()) {
            nc_files_select_next();
            nc_editor_serial_selected_file();
            strncpy(g_nc_visual_status, "File down", sizeof(g_nc_visual_status) - 1);
        } else if (g_nc_visual_mode == NC_MODE_TOOLS) {
            nc_editor_move_tool_line(&editor, 1);
        } else {
            nc_editor_move_line(&editor, 1);
            nc_editor_serial_selected_line(&editor);
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
            nc_editor_serial_selected_file();
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
            if (!nc_editor_save_current(&editor) &&
                !nc_editor_proceed_without_saving(&editor, NC_EDITOR_UNSAVED_OPEN)) {
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
        strncpy(g_nc_visual_status,
                nc_preview_toggle_layer(NC_PREVIEW_LAYER_STOCK) ? "Stock on"
                                                                : "Stock outline",
                sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_PATH:
        strncpy(g_nc_visual_status,
                nc_preview_toggle_layer(NC_PREVIEW_LAYER_PATH) ? "Path on"
                                                               : "Path hidden",
                sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_ROUGH:
        strncpy(g_nc_visual_status,
                nc_preview_toggle_layer(NC_PREVIEW_LAYER_ROUGH) ? "Rough on"
                                                                : "Rough hidden",
                sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_DIMS:
        strncpy(g_nc_visual_status,
                nc_preview_toggle_layer(NC_PREVIEW_LAYER_DIMS) ? "Dimensions on"
                                                               : "Dimensions hidden",
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
    default:
        /* The screen's own actions - the axis keys, ZERO and TOUCH - belong to
           the screen that acts on them. */
        {
            nc_manual_screen_t manual;

            nc_visual_manual_screen(&manual);
            if (!nc_manual_action(action, &manual)) {
                strncpy(g_nc_visual_status, "Action not available yet",
                        sizeof(g_nc_visual_status) - 1);
            }
        }
        break;
    }
}

static void nc_visual_cycle_mode(void)
{
    nc_visual_select_mode((nc_mode_t)((g_nc_visual_mode + 1) % NC_MODE_COUNT));
}

void nc_visual_select_mode(nc_mode_t mode)
{
    nc_editor_ctx_t editor;

    if (mode < 0 || mode >= NC_MODE_COUNT || mode == g_nc_visual_mode) {
        return;
    }
    nc_visual_editor_ctx(&editor, 0);
    if (!nc_editor_save_current(&editor) &&
        !nc_editor_proceed_without_saving(&editor, NC_EDITOR_UNSAVED_MODE)) {
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
        nc_editor_clear_draft();
        if (nc_state_load_document(g_nc_visual_mode, &g_nc_visual_doc)) {
            if (g_nc_visual_mode == NC_MODE_RUN && !nc_run_active())
                nc_run_set_line(&g_nc_visual_doc, g_nc_visual_doc.cursor_line);
            if (g_nc_visual_mode == NC_MODE_TOOLS) {
                int first_tool = -1;
                if (nc_editor_find_tool_line(&editor, 0, &first_tool) > 0 && first_tool >= 0) {
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





static void nc_visual_draw_tool_screen(void)
{
    nc_editor_ctx_t editor;
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

    nc_visual_editor_ctx(&editor, 0);
    tool_count = nc_editor_find_tool_line(&editor, nc_editor_selected_tool_index(&editor), NULL);
    selected_tool = nc_editor_selected_tool_index(&editor);
    (void)nc_editor_find_tool_line(&editor, selected_tool, &selected_line);
    (void)nc_editor_selected_tool_word(&editor, &active_letter, &active_line);
    if (selected_tool >= 8) {
        first_tool = selected_tool - 7;
    }

    lvds_draw_fill_rect(18, table_y - 10, LVDS_HSTX_WIDTH - 36,
                        NC_PANE_BOTTOM - (table_y - 10), NC_VISUAL_BG);
    nc_draw_text_clip(28, table_y, "TOOL TABLE", 16, NC_VISUAL_TEXT, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    /* The header carries the file name; the table only counts its rows. */
    snprintf(buf, sizeof(buf), "%d tools", tool_count);
    nc_draw_text_clip(674, table_y, buf, 12, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);

    nc_draw_text_clip(72, table_y + 28, "T", 4, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_draw_text_clip(120, table_y + 28, "RADIUS", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_draw_text_clip(190, table_y + 28, "ORIENT", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_draw_text_clip(260, table_y + 28, "FEED", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_draw_text_clip(326, table_y + 28, "FF", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_draw_text_clip(392, table_y + 28, "DOC", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_draw_text_clip(458, table_y + 28, "FDOC", 6, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_draw_text_clip(524, table_y + 28, "RPM", 7, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_draw_text_clip(604, table_y + 28, "XOFF", 7, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_draw_text_clip(684, table_y + 28, "ZOFF", 7, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    lvds_draw_line(28, table_y + 50, LVDS_HSTX_WIDTH - 28, table_y + 50, NC_VISUAL_DIM);

    if (tool_count == 0) {
        nc_draw_text_clip(44, table_y + 78, "No tool rows in this NC file. Press 1 to add T1.", 64,
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

        (void)nc_editor_find_tool_line(&editor, first_tool + row, &tool_line);
        if (tool_line < 0) {
            continue;
        }
        line = g_nc_visual_doc.lines[tool_line].text;
        (void)nc_tool_from_line(line, &tool);
        if (selected) {
            lvds_draw_fill_rect(24, y - 4, LVDS_HSTX_WIDTH - 48, 24, bg);
        }
        nc_draw_tool_glyph_centered(30, y - 3, 32, 18, &tool, bg, selected);
        nc_draw_tool_cell(line, 'T', 72, y, 4, fg, bg, tool_line == active_line && active_letter == 'T');
        nc_draw_tool_cell(line, 'R', 120, y, 6, fg, bg, tool_line == active_line && active_letter == 'R');
        nc_draw_tool_cell(line, 'O', 190, y, 6, fg, bg, tool_line == active_line && active_letter == 'O');
        nc_draw_tool_cell(line, 'F', 260, y, 6, fg, bg, tool_line == active_line && active_letter == 'F');
        nc_draw_tool_cell(line, 'Q', 326, y, 6, fg, bg, tool_line == active_line && active_letter == 'Q');
        nc_draw_tool_cell(line, 'D', 392, y, 6, fg, bg, tool_line == active_line && active_letter == 'D');
        nc_draw_tool_cell(line, 'E', 458, y, 6, fg, bg, tool_line == active_line && active_letter == 'E');
        nc_draw_tool_cell(line, 'S', 524, y, 7, fg, bg, tool_line == active_line && active_letter == 'S');
        nc_draw_tool_cell(line, 'X', 604, y, 7, fg, bg, tool_line == active_line && active_letter == 'X');
        nc_draw_tool_cell(line, 'Z', 684, y, 7, fg, bg, tool_line == active_line && active_letter == 'Z');
    }

    lvds_draw_line(18, detail_y, LVDS_HSTX_WIDTH - 18, detail_y, NC_VISUAL_DIM);
    nc_draw_text_clip(38, detail_y + 12, "Tool tip", 12, NC_VISUAL_TEXT, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    lvds_draw_line(52, detail_y + 76, 142, detail_y + 76, NC_VISUAL_DIM);
    lvds_draw_line(96, detail_y + 34, 96, detail_y + 120, NC_VISUAL_DIM);
    nc_draw_text_clip(102, detail_y + 34, "X0", 4, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_SMALL);
    nc_draw_text_clip(122, detail_y + 82, "Z0", 4, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_SMALL);

    if (selected_line >= 0) {
        nc_tool_t tool;
        const char *line = g_nc_visual_doc.lines[selected_line].text;

        (void)nc_tool_from_line(line, &tool);
        nc_draw_tool_glyph(96, detail_y + 76, 44, &tool, NC_VISUAL_BG, false);
        snprintf(buf, sizeof(buf), "Line %d: %.48s", selected_line + 1, line);
        nc_draw_text_clip(170, detail_y + 18, buf, 70, NC_VISUAL_TEXT, NC_VISUAL_BG, LVDS_FONT_NORMAL);
        nc_draw_tool_param(line, 'T', "T", 170, detail_y + 44, 8, selected_line == active_line && active_letter == 'T');
        nc_draw_tool_param(line, 'O', "Orient", 170, detail_y + 64, 8, selected_line == active_line && active_letter == 'O');
        nc_draw_tool_param(line, 'R', "Radius", 170, detail_y + 84, 8, selected_line == active_line && active_letter == 'R');
        nc_draw_tool_param(line, 'D', "DOC", 170, detail_y + 104, 8, selected_line == active_line && active_letter == 'D');
        nc_draw_tool_param(line, 'E', "FDOC", 400, detail_y + 44, 8, selected_line == active_line && active_letter == 'E');
        nc_draw_tool_param(line, 'F', "FEED", 400, detail_y + 64, 8, selected_line == active_line && active_letter == 'F');
        nc_draw_tool_param(line, 'Q', "F_FEED", 400, detail_y + 84, 8, selected_line == active_line && active_letter == 'Q');
        nc_draw_tool_param(line, 'S', "RPM", 400, detail_y + 104, 8, selected_line == active_line && active_letter == 'S');
        nc_draw_tool_param(line, 'X', "XOFF", 618, detail_y + 44, 7, selected_line == active_line && active_letter == 'X');
        nc_draw_tool_param(line, 'Z', "ZOFF", 618, detail_y + 64, 7, selected_line == active_line && active_letter == 'Z');
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
        nc_draw_text_clip(x + 7,
                                 NC_TAB_Y + 4,
                                 names[i],
                                 (w - 12) / NC_VISUAL_CHAR_W,
                                 active ? NC_VISUAL_LINE_NO_SELECTED : NC_VISUAL_DIM,
                                 active ? NC_VISUAL_SELECT : NC_VISUAL_HEADER,
                                 LVDS_FONT_NORMAL);
        x += w;
    }

    /* Scroll hint: the MODE key walks the strip. */
    nc_draw_text_clip(LVDS_HSTX_WIDTH - 22,
                             NC_TAB_Y + 4,
                             ">",
                             1,
                             NC_VISUAL_DIM,
                             NC_VISUAL_HEADER,
                             LVDS_FONT_NORMAL);

    /* The run state sits at the left of the message area; the message uses
       whatever is left before the hint. */
    if (state && state[0]) {
        nc_draw_text_clip(420, NC_TAB_Y + 4, state, 12,
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
            nc_draw_text_clip(LVDS_HSTX_WIDTH - 26 - len * NC_VISUAL_CHAR_W,
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
        nc_draw_text_clip(col_work_x, hy + 4, "X", 1, NC_VISUAL_DIM, NC_VISUAL_HEADER, LVDS_FONT_LARGE);
        snprintf(buf, sizeof(buf), "%9.3f", (double)work[AXIS_X]);
        nc_draw_text_clip(col_work_x + 18, hy + 4, buf, 9, NC_VISUAL_TEXT, NC_VISUAL_HEADER, LVDS_FONT_LARGE);
        nc_draw_text_clip(col_work_x, hy + 36, "Z", 1, NC_VISUAL_DIM, NC_VISUAL_HEADER, LVDS_FONT_LARGE);
        snprintf(buf, sizeof(buf), "%9.3f", (double)work[AXIS_Z]);
        nc_draw_text_clip(col_work_x + 18, hy + 36, buf, 9, NC_VISUAL_TEXT, NC_VISUAL_HEADER, LVDS_FONT_LARGE);

        /* Column 2: the machine figures, in their own cell and one font
           smaller, with the offset they differ by named above them. */
        {
            char offset[16];

            nc_visual_offset_label(offset, sizeof(offset));
            nc_draw_text_clip(col_mach_x, hy + 1, offset, 16,
                                     NC_VISUAL_DIM, NC_VISUAL_HEADER, LVDS_FONT_SMALL);
            snprintf(buf, sizeof(buf), "%8.3f", (double)(runtime ? runtime->x : 0.0f));
            nc_draw_text_clip(col_mach_x, hy + 11, buf, 8,
                                     NC_VISUAL_DIM, NC_VISUAL_HEADER, LVDS_FONT_NORMAL);
            snprintf(buf, sizeof(buf), "%8.3f", (double)(runtime ? runtime->z : 0.0f));
            nc_draw_text_clip(col_mach_x, hy + 43, buf, 8,
                                     NC_VISUAL_DIM, NC_VISUAL_HEADER, LVDS_FONT_NORMAL);
        }

        /* Column 3: feed and spindle. */
        nc_draw_text_clip(col_fs_x, hy + 4, "F", 1, NC_VISUAL_DIM, NC_VISUAL_HEADER, LVDS_FONT_LARGE);
        snprintf(buf, sizeof(buf), "%8.1f", (double)(runtime ? runtime->feed : 0.0f));
        nc_draw_text_clip(col_fs_x + 18, hy + 4, buf, 8, NC_VISUAL_TEXT, NC_VISUAL_HEADER, LVDS_FONT_LARGE);
        nc_draw_text_clip(col_fs_x, hy + 36, "S", 1, NC_VISUAL_DIM, NC_VISUAL_HEADER, LVDS_FONT_LARGE);
        snprintf(buf, sizeof(buf), "%8u", runtime ? runtime->spindle : 0u);
        nc_draw_text_clip(col_fs_x + 18, hy + 36, buf, 8, NC_VISUAL_TEXT, NC_VISUAL_HEADER, LVDS_FONT_LARGE);
    }

    snprintf(fps, sizeof(fps), "%u FPS", (unsigned)g_nc_visual_fps);
    nc_draw_text_clip(LVDS_HSTX_WIDTH - lvds_draw_text_width(fps, LVDS_FONT_SMALL) - 8,
                             hy + 6,
                             fps,
                             8,
                             NC_VISUAL_DIM,
                             NC_VISUAL_HEADER,
                             LVDS_FONT_SMALL);
}


static void nc_visual_draw_snapshot(const nc_snapshot_t *s)
{
    char buf[80];
    char footer_text[160];
    bool full_preview = nc_visual_full_preview();
    bool split = nc_files_active() || (nc_visual_is_code_view() && !full_preview);
    uint32_t t0;
    uint32_t t1;
    uint32_t t2;
    uint32_t t3;
    uint32_t t4;
    nc_editor_ctx_t editor;

    nc_visual_editor_ctx(&editor, s);

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
        nc_visual_preview(s, &g_nc_visual_doc, NC_FULL_PREVIEW_X,
                          NC_PANE_Y, NC_FULL_PREVIEW_W, NC_PANE_H, true);
    } else if (split) {
        /* While the file list is up, the preview follows the file being pointed
           at rather than the one that is open. */
        const nc_document_t *preview_doc =
            (nc_files_active() && g_nc_files_preview_ok) ? &g_nc_files_preview_doc
                                                         : &g_nc_visual_doc;
        nc_visual_preview(s, preview_doc, NC_LEFT_PANE_X, NC_PANE_Y,
                          NC_LEFT_PANE_W, NC_PANE_H, true);
        if (nc_files_active()) {
            int selected = nc_files_selected();
            const char *label = selected >= 0 ? nc_files_name(selected) : "no file picked";

            snprintf(buf, sizeof(buf), "PREVIEW %.28s", label);
            nc_draw_text_clip(NC_LEFT_PANE_X + 12,
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
        nc_editor_draw_files(&editor);
    } else if (g_nc_visual_mode == NC_MODE_TOOLS) {
        nc_visual_draw_tool_screen();
    } else if (nc_visual_is_code_view() && !full_preview) {
        nc_editor_draw_pane(&editor);
    } else if (g_nc_visual_mode == NC_MODE_MANUAL) {
        nc_visual_manual_pane(&s->runtime);
    }
    /* Nothing else to draw in the body: the screens above cover every mode -
       and in EDIT's full-screen state the preview *is* the body, so the old
       "no controls on this screen" placeholder must not be painted over it. */

    nc_editor_draw_aids(&editor);
    t3 = mcu_micros();

    nc_visual_footer_text(footer_text, sizeof(footer_text));
    nc_draw_footer_status("", footer_text);
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
    nc_visual_preview(s, &g_nc_visual_doc, NC_LEFT_PANE_X, NC_PANE_Y,
                      NC_LEFT_PANE_W, NC_PANE_H, false);
    t2 = mcu_micros();
    g_nc_visual_frame_header_us += t1 - t0;
    g_nc_visual_frame_preview_us += t2 - t1;
}

void nc_visual_init(void)
{
    nc_editor_ctx_t editor;

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
    nc_visual_editor_ctx(&editor, 0);
    if (nc_visual_uses_file() &&
        nc_state_load_document(g_nc_visual_mode, &g_nc_visual_doc)) {
        snprintf(g_nc_visual_status,
                 sizeof(g_nc_visual_status),
                 "%s: %.48s",
                 nc_menu_mode_name(g_nc_visual_mode),
                 g_nc_visual_doc.path);
        g_nc_visual_dirty = true;
    } else if (!nc_state_load_document(NC_MODE_PROGRAM, &g_nc_visual_doc)) {
        nc_editor_seed_demo(&editor);
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
    nc_editor_ctx_t editor;
    char key_ch = nc_visual_key_char(key);
    uint8_t footer_action;

    nc_visual_editor_ctx(&editor, 0);
    if (nc_editor_modal_key(&editor, key, key_ch)) {
        if (editor.follow != NC_FOOTER_ACTION_NONE) {
            nc_visual_dispatch_footer_action(editor.follow);
        }
        return;
    }
    if (!nc_files_active() && g_nc_visual_mode == NC_MODE_MANUAL) {
        nc_manual_screen_t manual;

        nc_visual_manual_screen(&manual);
        if (nc_manual_key(key, nc_visual_key_char(key), &manual)) {
            return;
        }
    }
    if (nc_editor_key_name(&editor, key)) {
        return;
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

    if (nc_editor_key_edit(&editor, key, key_ch)) {
        return;
    }
    if (key == NC_VISUAL_KEY_MODE) {
        nc_editor_clear_draft();
        nc_editor_new_file_end(&editor);
        nc_visual_cycle_mode();
        g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
        g_nc_visual_dirty = true;
        return;
    }

    if (nc_editor_selected_word_key(&editor, key, key_ch)) {
        g_nc_visual_status[sizeof(g_nc_visual_status) - 1] = '\0';
        g_nc_visual_dirty = true;
        return;
    }

    if ((key == NC_VISUAL_KEY_PREV || key == NC_VISUAL_KEY_NEXT) &&
        (nc_files_active() ||
         nc_visual_is_code_view() ||
         g_nc_visual_mode == NC_MODE_TOOLS)) {
        nc_editor_clear_draft();
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

    nc_editor_key_cursor(&editor, key);

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
    nc_manual_screen_t manual;

    nc_visual_manual_screen(&manual);
    nc_manual_hold(key, &manual);
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
    if (g_nc_visual_mode != NC_MODE_MANUAL || nc_files_active()) {
        return 0;
    }
    /* The pad's meanings are the MANUAL screen's: it states them once, for the
       pad it draws and for the shell that labels its own keypad. */
    return nc_manual_key_hint(key);
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
        nc_preview_invalidate();
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
