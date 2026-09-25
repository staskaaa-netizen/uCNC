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

/* The wording of an error: the module that refused the line knows *why* and
   hands its reason in (`g7x_take_refusal_text()` - reading it takes it, so it
   can never explain a later line); otherwise the status name is all there is. */
static const char *nc_visual_error_text(uint8_t error)
{
    const char *why = g7x_take_refusal_text();

    return (why && why[0]) ? why : nc_feedback_error(error);
}

/* One error line for the operator. `line` is 1-based, or 0 for an error that
   belongs to no source line. */
static void nc_visual_command_error_line(unsigned long line, uint8_t error)
{
    const char *why = nc_visual_error_text(error);

    if (line)
        snprintf(g_nc_visual_command_error, sizeof(g_nc_visual_command_error),
                 "Line %lu error %u: %s", line, error, why);
    else
        snprintf(g_nc_visual_command_error, sizeof(g_nc_visual_command_error),
                 "Error %u: %s", error, why);
}

static bool nc_visual_parse_error(void *args)
{
    uint8_t error = *(uint8_t *)args;

    if (nc_run_error())
        nc_visual_command_error_line(nc_run_error_line() + 1, error);
    else
        nc_visual_command_error_line(0u, error);
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

/* Is the machine in a run? A run that is held is still a run, and the panel's
   own stream counts as one from the moment it starts. One definition, so the
   DRO's green and the state word in its corner (RUN, HOLD) cannot disagree. */
static bool nc_visual_running(const nc_runtime_state_t *runtime)
{
    return nc_visual_runtime_busy(runtime) || nc_run_active() || nc_run_hold();
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

/* MANUAL's pane: the offset its stops read in, which the screen reads once (the
   header above shows the same label). The machine's own figures are the header
   DRO's and do not reach the pane - it draws what the operator is setting up. */
static void nc_visual_manual_pane(void)
{
    nc_manual_view_t view;
    float wco[AXIS_COUNT] = {0};
    char label[16];

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
/* Set once the idle period has been written; a key clears it. */
static bool g_nc_visual_idle_flushed;

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

/* Start a run from `line` to the end of the program.

   `nc_run_arm()` alone only sets the run state: FROM and FULL said "Run from
   line N" and the machine never received a character, because nothing handed
   the reader to the run. The single-line and one-shot paths do that with
   `grbl_stream_readonly()`; a whole-program run is the same stream with no end
   line, which is what `nc_run_start_stream()` opens. */
static void nc_visual_run_start(size_t line, const char *label)
{
    /* A text file is text: it opens in the editor, it is not a program to read
       as G-code or to run. */
    if (!nc_visual_document_is_program()) {
        return;
    }
    /* The same guard as the single step: an armed run must not queue into a
       locked parser. */
    if (cnc_get_exec_state(EXEC_GCODE_LOCKED) || cnc_has_alarm()) {
        snprintf(g_nc_visual_status,
                 sizeof(g_nc_visual_status),
                 "RUN locked: check controller status (?)");
        grbl_stream_printf("[MSG:NC RUN locked state=%u alarm=%u; check ?]\r\n",
                           cnc_get_exec_state(EXEC_ALLACTIVE),
                           (unsigned)cnc_has_alarm());
        return;
    }
    if (!nc_run_start_stream(&g_nc_visual_doc, line)) {
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

/* The line RUN is on - what the pane marks and what `1 SINGLE` acts on. It is
   the line in play, not the sender's position (`nc_run_display_line()`): the
   line a step was taken from, or the unit a run is walking through, with a unit
   that has finished keeping it until the operator takes the cursor. The sender
   walks on without it, so the two are *not* the same answer in general - a pane
   that followed the sender marked the line after a block, which in the bench's
   file was a cycle header that had never run. It is clamped into the program: a
   finished run stands just past its last line, and both the pane's mark and
   `1 SINGLE` need a line that exists - the last one. */
static size_t nc_visual_run_line(const nc_document_t *doc)
{
    size_t line = nc_run_display_line();

    if (doc && doc->line_count && line >= doc->line_count) {
        line = doc->line_count - 1u;
    }
    return line;
}

static void nc_visual_run_step(void)
{
    /* The line the pane marks: one cursor in RUN, so the key acts on what the
       operator sees marked. */
    size_t line = nc_visual_run_line(&g_nc_visual_doc);
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
    nc_manual_screen_t manual;
    const char *message;

    nc_visual_editor_ctx(&editor, 0);
    nc_visual_manual_screen(&manual);
    g_nc_visual_selected_action = action;

    /* Each footer entry has one owner. The screen asks them in turn and keeps
       what is its own below: the mode changes, the RUN keys, the view toggle
       and the entries that are still stubs. */
    if (nc_editor_action(&editor, action)) {
        return;
    }
    message = nc_preview_action(action);
    if (message) {
        strncpy(g_nc_visual_status, message, sizeof(g_nc_visual_status) - 1);
        return;
    }
    if (nc_manual_action(action, &manual)) {
        return;
    }

    switch (action) {
    case NC_FOOTER_ACTION_TOOL_EDIT:
        strncpy(g_nc_visual_status, "Tool edit", sizeof(g_nc_visual_status) - 1);
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
    case NC_FOOTER_ACTION_RESET:
        /* BACK from the file list, or `# RELOAD` in RUN: the operator is
           clearing the screen's state, and the last fault goes with it. Nothing
           else clears the message - a fault stands until it is answered. */
        g_nc_visual_command_error[0] = '\0';
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
    case NC_FOOTER_ACTION_FULL:
        /* A fresh run answers the last fault: the operator has read it and is
           starting something. */
        g_nc_visual_command_error[0] = '\0';
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
                nc_visual_run_start(0, "Run loaded");
            } else {
                snprintf(g_nc_visual_status, sizeof(g_nc_visual_status), "Run open failed: %s", nc_result_text(r));
            }
        } else {
            nc_visual_run_start(0, "Full run");
        }
        break;
    case NC_FOOTER_ACTION_VIEW:
        g_nc_visual_show_code = !g_nc_visual_show_code;
        strncpy(g_nc_visual_status,
                g_nc_visual_show_code ? "Code shown" : "Code hidden - full preview",
                sizeof(g_nc_visual_status) - 1);
        break;
    case NC_FOOTER_ACTION_SINGLE:
        g_nc_visual_command_error[0] = '\0';   /* a fresh attempt answers the last one */
        nc_visual_run_step();
        break;
    case NC_FOOTER_ACTION_FROM:
        g_nc_visual_command_error[0] = '\0';
        nc_visual_run_start(g_nc_visual_doc.cursor_line, "Run from");
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
    default:
        strncpy(g_nc_visual_status, "Action not available yet",
                sizeof(g_nc_visual_status) - 1);
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
    nc_draw_text_clip(38, detail_y + 12, "Tool tip", 12, NC_VISUAL_TOOL_TIP, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    lvds_draw_line(52, detail_y + 76, 142, detail_y + 76, NC_VISUAL_DIM);
    lvds_draw_line(96, detail_y + 34, 96, detail_y + 120, NC_VISUAL_DIM);
    nc_draw_text_clip(102, detail_y + 34, "X0", 4, NC_VISUAL_TOOL_TIP, NC_VISUAL_BG, LVDS_FONT_SMALL);
    nc_draw_text_clip(122, detail_y + 82, "Z0", 4, NC_VISUAL_TOOL_TIP, NC_VISUAL_BG, LVDS_FONT_SMALL);

    if (selected_line >= 0) {
        nc_tool_t tool;
        const char *line = g_nc_visual_doc.lines[selected_line].text;

        (void)nc_tool_from_line(line, &tool);
        nc_draw_tool_glyph(96, detail_y + 76, 44, &tool, NC_VISUAL_BG, false);
        snprintf(buf, sizeof(buf), "Line %d: %.48s", selected_line + 1, line);
        nc_draw_text_clip(170, detail_y + 18, buf, 70, NC_VISUAL_TOOL_TIP, NC_VISUAL_BG, LVDS_FONT_NORMAL);
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
        const char *why = nc_visual_error_text(nc_run_error());

        snprintf(buf,
                 buf_sz,
                 "Line %lu error %u: %s",
                 (unsigned long)(nc_run_error_line() + 1),
                 nc_run_error(),
                 why);
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
   so the strip is a position marker plus that hint, not a menu to click. The
   run state is not repeated here: the DRO's own corner carries it (`uCNC IDLE`
   and the rest), once, on the band the figures are on. */
static void nc_visual_draw_tabs(const char *message,
                                lvds_color_t message_fg)
{
    static const char *const names[NC_MODE_COUNT] = {
        "MANUAL", "EDIT", "TOOLS", "RUN"
    };
    int x = 8;
    int i;

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

    /* The message area, in the empty right side of the strip: it starts where
       the screen names end, and the run state is not written here (the DRO's
       corner has it). */
    if (message && message[0]) {
        int cols = (LVDS_HSTX_WIDTH - 26 - 420) / NC_VISUAL_CHAR_W;
        int len = (int)strlen(message);

        if (len > cols) {
            len = cols;
        }
        if (len > 0) {
            /* A fault is drawn by the header, as a block over this area and the
               DRO below it (`nc_visual_draw_fault()`); everything else is a
               line of the strip's own text. */
            if (message_fg != NC_VISUAL_ERROR) {
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
}

/* The frame counter, in its own corner below the DRO. Drawn at the end of every
   path, so the live path (which repaints only the header and the band) keeps it
   counting too. It is a debug reading: dim, small, and out of everything the
   operator reads. */
static void nc_visual_draw_fps(void)
{
    char fps[16];

    snprintf(fps, sizeof(fps), "%u FPS", (unsigned)g_nc_visual_fps);
    nc_draw_text_clip(LVDS_HSTX_WIDTH - NC_FPS_X_PAD -
                              lvds_draw_text_width(fps, LVDS_FONT_SMALL),
                             NC_FPS_Y,
                             fps,
                             8,
                             NC_VISUAL_DIM,
                             NC_VISUAL_BG,
                             LVDS_FONT_SMALL);
}

/* The controller's own state, short enough for a corner: what the machine is
   doing (or refusing to do) at this moment, on every screen. The tab strip
   still carries the sentence that explains a fault; this is the word an
   operator glances at. */
static const char *nc_visual_state_label(const nc_runtime_state_t *runtime)
{
    uint16_t state = runtime ? runtime->exec_state : 0;

    if (cnc_has_alarm()) return "ALARM";
    if (state & EXEC_KILL) return "KILLED";
    if (state & EXEC_LIMITS) return "LIMITS";
    if (state & EXEC_POSITION_MAYBE_LOST) return "POS LOST";
    if (nc_visual_settings_error()) return "SETTINGS";
    if (state & EXEC_DOOR) return "DOOR";
    if (nc_run_hold() || (state & EXEC_HOLD)) return "HOLD";
    if (nc_visual_running(runtime)) return "RUN";
    if (state & EXEC_JOG) return "JOG";
    if (nc_run_error()) return "ERROR";
    return "IDLE";
}

/* True when that state is a fault: it wears the same red label in the DRO as
   the message area uses, so a machine that needs attention says so twice. */
static bool nc_visual_state_is_fault(const nc_runtime_state_t *runtime)
{
    uint16_t state = runtime ? runtime->exec_state : 0;

    return cnc_has_alarm() || nc_visual_settings_error() ||
           (state & (EXEC_KILL | EXEC_LIMITS | EXEC_POSITION_MAYBE_LOST)) ||
           nc_run_error();
}

/* Wrap a message into the fault block's width, at spaces, from the start: the
   beginning of a message is the part that says what happened. A word longer
   than the width is cut. */
static int nc_visual_fault_lines(const char *message, char lines[][NC_FAULT_COLS + 1])
{
    int count = 0;

    while (message && *message && count < NC_FAULT_LINES) {
        int len = (int)strlen(message);
        int take = len > NC_FAULT_COLS ? NC_FAULT_COLS : len;

        if (len > NC_FAULT_COLS) {
            int i;

            for (i = take; i > 0; i--) {
                if (message[i - 1] == ' ') {
                    take = i - 1;
                    break;
                }
            }
        }
        if (take <= 0) {
            take = NC_FAULT_COLS;      /* one long word: cut it */
        }
        memcpy(lines[count], message, (size_t)take);
        lines[count][take] = '\0';
        count++;
        message += take;
        while (*message == ' ') {
            message++;
        }
    }
    return count;
}

/* The fault block: white letters on red, over the tab strip's message area and
   down over the DRO beside the machine's figures. */
static void nc_visual_draw_fault(const char *message)
{
    char lines[NC_FAULT_LINES][NC_FAULT_COLS + 1];
    int count;
    int i;

    if (!message || !message[0]) {
        return;
    }
    count = nc_visual_fault_lines(message, lines);
    lvds_draw_fill_rect(NC_FAULT_X, NC_TAB_Y, NC_FAULT_W, NC_FAULT_H, NC_VISUAL_ERROR);
    for (i = 0; i < count; i++) {
        nc_draw_text_clip(NC_FAULT_X + 4, NC_TAB_Y + 2 + i * NC_FAULT_ROW_H,
                                 lines[i], NC_FAULT_COLS,
                                 NC_VISUAL_WORD_FG, NC_VISUAL_ERROR,
                                 LVDS_FONT_NORMAL);
    }
}

static void nc_visual_draw_header(const nc_snapshot_t *s,
                                  const char *message,
                                  lvds_color_t message_fg)
{
    char buf[96];
    const nc_runtime_state_t *runtime = s ? &s->runtime : 0;
    const int hy = NC_HEADER_Y;
    /* The snapshot carries the machine position; the operator works in the
       work (nominal) system, so subtract the offsets for the first column and
       keep the machine figures for the third. */
    float work[AXIS_COUNT] = {0};
    /* The DRO wears the panel's green while the machine is in a run and its own
       grey otherwise: one glance at the top says whether the machine is running,
       on every screen. A fault takes the colour away again - a machine stopped
       by a problem must not still say "running", and the red block on the right
       is what carries the alarm. The tab strip above keeps its grey - the screen
       names live there, and the state is what this band carries. The figures are
       dark in every state, so they read on all of them. */
    const lvds_color_t bg = (message_fg == NC_VISUAL_ERROR)
                                ? NC_VISUAL_HEADER
                                : (nc_visual_running(runtime)
                                       ? NC_VISUAL_HEADER_RUN
                                       : NC_VISUAL_HEADER);
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

    lvds_draw_fill_rect(0, hy, LVDS_HSTX_WIDTH, NC_HEADER_H, bg);
    /* No rule along the bottom of the band. It was there to separate the DRO
       from the code when both were plain grey; the band's own colour is the
       boundary now - it wears the panel's green while a run is going, and the
       pane below it is the page's, not the band's - so the line was one more
       thing on the glass that said nothing (bench: "we have one black line under
       dro, now it is obsolete"). The columns keep their own separators: those
       divide readings inside the band, which is what the band is for. */
    /* Every screen carries the DRO, MANUAL included: the header band is the one
       place with room for the work position, the machine figures and F/S, and
       the pane needs its own space for the stops and the jog values (see
       nc_manual.h). It used to be blank on MANUAL, which showed the same numbers
       in the pane instead. */
    {
        lvds_draw_line(182, hy + 6, 182, hy + 61, NC_VISUAL_DIM);
        lvds_draw_line(292, hy + 6, 292, hy + 61, NC_VISUAL_DIM);
    }

    {
        /* Column 1: the work (nominal) position. */
        nc_draw_text_clip(col_work_x, hy + 4, "X", 1, NC_VISUAL_DIM, bg, LVDS_FONT_LARGE);
        snprintf(buf, sizeof(buf), "%9.3f", (double)work[AXIS_X]);
        nc_draw_text_clip(col_work_x + 18, hy + 4, buf, 9, NC_VISUAL_TEXT, bg, LVDS_FONT_LARGE);
        nc_draw_text_clip(col_work_x, hy + 36, "Z", 1, NC_VISUAL_DIM, bg, LVDS_FONT_LARGE);
        snprintf(buf, sizeof(buf), "%9.3f", (double)work[AXIS_Z]);
        nc_draw_text_clip(col_work_x + 18, hy + 36, buf, 9, NC_VISUAL_TEXT, bg, LVDS_FONT_LARGE);

        /* Column 2: the machine figures, in their own cell and one font
           smaller, with the offset they differ by named above them. */
        {
            char offset[16];

            nc_visual_offset_label(offset, sizeof(offset));
            nc_draw_text_clip(col_mach_x, hy + 1, offset, 16,
                                     NC_VISUAL_DIM, bg, LVDS_FONT_SMALL);
            snprintf(buf, sizeof(buf), "%8.3f", (double)(runtime ? runtime->x : 0.0f));
            nc_draw_text_clip(col_mach_x, hy + 11, buf, 8,
                                     NC_VISUAL_DIM, bg, LVDS_FONT_NORMAL);
            snprintf(buf, sizeof(buf), "%8.3f", (double)(runtime ? runtime->z : 0.0f));
            nc_draw_text_clip(col_mach_x, hy + 43, buf, 8,
                                     NC_VISUAL_DIM, bg, LVDS_FONT_NORMAL);
        }

        /* Column 3: feed and spindle. */
        nc_draw_text_clip(col_fs_x, hy + 4, "F", 1, NC_VISUAL_DIM, bg, LVDS_FONT_LARGE);
        snprintf(buf, sizeof(buf), "%8.1f", (double)(runtime ? runtime->feed : 0.0f));
        nc_draw_text_clip(col_fs_x + 18, hy + 4, buf, 8, NC_VISUAL_TEXT, bg, LVDS_FONT_LARGE);
        nc_draw_text_clip(col_fs_x, hy + 36, "S", 1, NC_VISUAL_DIM, bg, LVDS_FONT_LARGE);
        snprintf(buf, sizeof(buf), "%8u", runtime ? runtime->spindle : 0u);
        nc_draw_text_clip(col_fs_x + 18, hy + 36, buf, 8, NC_VISUAL_TEXT, bg, LVDS_FONT_LARGE);
    }

    /* The controller's state, bottom right of the DRO - the one band that is on
       every screen - so "idle, running, held or in a fault" needs no screen
       change to answer. `for start`: more of the machine's own status may move
       here as the panel grows. */
    {
        char state_text[24];
        int state_w;
        int state_x;

        snprintf(state_text, sizeof(state_text), "uCNC %s",
                 nc_visual_state_label(runtime));
        state_w = lvds_draw_text_width(state_text, LVDS_FONT_NORMAL);
        state_x = LVDS_HSTX_WIDTH - state_w - 8;
        if (nc_visual_state_is_fault(runtime)) {
            lvds_draw_fill_rect(state_x - 5, hy + 46, state_w + 10,
                                NC_VISUAL_ROW_H - 4, NC_VISUAL_ERROR);
            nc_draw_text_clip(state_x, hy + 50, state_text, 20,
                                     NC_VISUAL_WORD_FG, NC_VISUAL_ERROR,
                                     LVDS_FONT_NORMAL);
        } else {
            nc_draw_text_clip(state_x, hy + 50, state_text, 20,
                                     NC_VISUAL_DIM, bg, LVDS_FONT_NORMAL);
        }
    }

    /* A fault is a block over the strip's message area and the room this band
       has beside the machine's figures - drawn last, so the DRO cannot paint
       over it. The bottom-right corner and the F/S column are outside it by its
       geometry, not by this call. */
    if (message_fg == NC_VISUAL_ERROR) {
        nc_visual_draw_fault(message);
    }
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
        nc_visual_draw_header(s, notice, notice_fg);
    }
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
        nc_visual_manual_pane();
    }
    /* Nothing else to draw in the body: the screens above cover every mode -
       and in EDIT's full-screen state the preview *is* the body, so the old
       "no controls on this screen" placeholder must not be painted over it. */

    nc_editor_draw_aids(&editor);
    t3 = mcu_micros();

    nc_visual_footer_text(footer_text, sizeof(footer_text));
    nc_draw_footer_status("", footer_text);
    nc_visual_draw_fps();
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
        nc_visual_draw_header(s, notice, notice_fg);
    }
    t1 = mcu_micros();
    nc_visual_preview(s, &g_nc_visual_doc, NC_LEFT_PANE_X, NC_PANE_Y,
                      NC_LEFT_PANE_W, NC_PANE_H, false);
    nc_visual_draw_fps();
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
    /* The SD card is mounted from the main loop, so the preset entries can only
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
    /* A key ends the screen's idle period: whatever was written while it was
       quiet, the operator is working again. */
    g_nc_visual_idle_flushed = false;
    nc_visual_handle_key_impl(key);
    /* Browsing the card may have moved the list's cursor: refresh the preview
       of whatever is pointed at now. A no-op unless the selection changed. */
    if (nc_files_active()) {
        nc_files_preview_sync();
    }
}

/* Called from the main loop: once the operator has stopped pressing keys, write
   what the panel owes the card - the program being edited and the remembered
   state - so no FAT write sits in the middle of a screen change, and an edit
   does not need a save key to survive. One write per idle period: a key starts a
   new one, so a card that cannot be written to is retried when the operator
   works again and not on every pass. */
void nc_visual_idle_tasks(void)
{
    if (g_nc_visual_last_input_ms == 0u ||
        (uint32_t)(mcu_millis() - g_nc_visual_last_input_ms) < NC_VISUAL_IDLE_FLUSH_MS) {
        return;
    }
    if (!g_nc_visual_idle_flushed) {
        nc_editor_ctx_t editor;

        g_nc_visual_idle_flushed = true;
        /* The screen has been left alone and the program is unsaved: this is the
           save. The `*` in the name row is what says the write happened - it is
           the editor's own dirty mark, and it is read in the same place the
           operator typed. */
        nc_visual_editor_ctx(&editor, 0);
        if (editor.doc && editor.doc->dirty && nc_path_text(editor.doc->path)) {
            (void)nc_editor_save_current(&editor);
            g_nc_visual_dirty = true;
        }
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
    if (nc_files_active()) {
        return 0;
    }
    if (g_nc_visual_mode != NC_MODE_MANUAL) {
        return 0;
    }
    /* The pad's meanings are the MANUAL screen's: it states them once, for the
       pad it draws and for the shell that labels its own keypad. */
    return nc_manual_key_hint(key);
}

bool nc_visual_key_meaning(char key, nc_visual_key_meaning_t *meaning)
{
    const nc_footer_item_t *footer;
    const char *hint;
    size_t count = 0u;
    size_t i;

    if (!meaning) {
        return false;
    }
    meaning->label = 0;
    meaning->on_menu = false;
    meaning->step = false;
    /* The footer the strip draws is the one that says a key is on the menu -
       EDIT's own, or the preview's while it has the whole body. */
    footer = nc_visual_footer_items(&count);
    for (i = 0u; i < count; i++) {
        if (footer[i].key == key && footer[i].label[0] != '\0') {
            meaning->label = footer[i].label;
            meaning->on_menu = true;
            break;
        }
    }
    if (!meaning->on_menu) {
        hint = nc_visual_key_hint(key);
        if (hint && hint[0]) {
            meaning->label = hint;
        }
    }
    /* `B`/`C` are the keypad's step keys: the footer names them AXIS-/AXIS+ on
       MANUAL, the editor walks equal fields and the file list steps a row with
       them everywhere else - so a shell draws the arrow the key acts as, not
       the letter. The full-screen preview has nothing to step. */
    if ((key == 'B' || key == 'C') && !nc_visual_full_preview()) {
        meaning->step = true;
    }
    return meaning->label != 0 || meaning->step;
}

const char *nc_visual_screen_name(void)
{
    if (nc_files_active()) {
        return "FILES";
    }
    if (nc_visual_full_preview()) {
        return "PREVIEW";
    }
    return nc_menu_mode_name(g_nc_visual_mode);
}

/* What each screen says it is: the words an operator reads beside the machine.
   The screen that acts on a key names it here, so a shell never states a key
   meaning of its own (and a screen that renames a key renames it here too). */
size_t nc_visual_usage(const char *const **lines)
{
    static const char *const manual[] = {
        "Jog the machine by hand.",
        "Digits jog: 2/8 X, 4/6 Z.",
        "7/9 spindle CCW/CW, 5 stop.",
        "1/3 pick the step or the feed.",
        "B/C (arrows) pick the axis.",
        "* types the stops, # step|feed.",
        "0 zero, D touch-off."
    };
    static const char *const program[] = {
        "The program, one line at a time.",
        "Arrows move, the digits type.",
        "B/C step between equal words.",
        "1 OPS  2 TOOL  3 WORD  4 G7X.",
        "5 THREAD, 6 PECK open a pad each.",
        "U/W are increments of X and Z.",
        "# VIEW  * DEL  0 files."
    };
    static const char *const tools[] = {
        "The tool table, one tool a line.",
        "Arrows move, the digits type.",
        "B/C step between equal words.",
        "1 ADD  7 INS  * DEL.",
        "8 FILES opens the card."
    };
    static const char *const run[] = {
        "Send the program to the machine.",
        "1 SINGLE  2 FROM  3 FULL.",
        "4 HOLD (again resumes)  5 STOP.",
        "6 DIM  # RELOAD  0 files."
    };
    static const char *const files[] = {
        "Pick a program from the card.",
        "B/C or arrows step the list.",
        "4 or D opens the file.",
        "5 new  6 delete  8 refresh.",
        "# runs it, * back."
    };
    static const char *const preview[] = {
        "The part as the program cuts it.",
        "4 STOCK  5 TRACE  6 ROUGH.",
        "7 DIM  # back to the code."
    };
    const char *const *table;
    size_t count;

    if (!lines) {
        return 0u;
    }
    if (nc_files_active()) {
        table = files;
        count = sizeof(files) / sizeof(files[0]);
    } else if (nc_visual_full_preview()) {
        table = preview;
        count = sizeof(preview) / sizeof(preview[0]);
    } else {
        switch (g_nc_visual_mode) {
        case NC_MODE_MANUAL:
            table = manual;
            count = sizeof(manual) / sizeof(manual[0]);
            break;
        case NC_MODE_TOOLS:
            table = tools;
            count = sizeof(tools) / sizeof(tools[0]);
            break;
        case NC_MODE_RUN:
            table = run;
            count = sizeof(run) / sizeof(run[0]);
            break;
        case NC_MODE_PROGRAM:
        default:
            table = program;
            count = sizeof(program) / sizeof(program[0]);
            break;
        }
    }
    *lines = table;
    return count;
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
    /* RUN has one cursor: the line the sender is on. The pane marks that line
       and `1 SINGLE` sends it, so the mark can never be a line away from what
       the key acts on. A streamed run advances the sender's line; the
       document's cursor used to stay where the block started, and SINGLE - which
       reads the document's cursor - then re-sent that block while the pane
       showed the next line (bench: "it was marking next line after current g71,
       but then i pressed run single - it still seems to have marked original one
       with g71. only if i go back/forward it is ok", because the line keys sync
       the two). */
    /* The cycle the marked line belongs to gets the pane's weaker mark - the
       *path* of the block the bright line is the head of. This is the code
       screens' one rule, and it is the same rule in both of them: the bright
       line is the line in play (in RUN the unit the machine is on, in EDIT the
       line the cursor and the keys are on) and the pale rows are the rest of
       the cycle that line belongs to. The answer comes from the same
       `nc_g7x_block_containing()` the sender uses to decide what a line sends,
       so the pale rows are exactly the lines that go with the bright one - no
       extracted path list, and nothing the stream and the mark can drift apart
       on. A line outside every cycle has no path: the mark stays on that line.

       The editor keeps the bright mark on its cursor rather than on the block's
       header: the cursor is what the digits type into and what the legend names,
       and an editor that moves its own cursor mark to another line would be
       lying about where the typing goes. */
    if (nc_visual_is_code_view()) {
        size_t mark = (g_nc_visual_mode == NC_MODE_RUN)
                          ? nc_visual_run_line(&g_nc_visual_doc)
                          : g_nc_visual_doc.cursor_line;
        size_t block_first = mark;
        size_t block_last = mark;
        bool in_block;

        if (g_nc_visual_mode == NC_MODE_RUN) {
            g_nc_visual_doc.cursor_line = mark;
        }
        /* `nc_g7x_line_path()`: the block for a row inside a cycle, and for a
           `G70 P Q` the numbered range it replays - a finish cut's path is the
           profile above it, which is the same answer the preview feeds from. */
        in_block = nc_g7x_line_path(&g_nc_visual_doc, mark,
                                    &block_first, &block_last);
        nc_state_set_run_block(in_block, block_first, block_last);
    } else {
        nc_state_set_run_block(false, 0u, 0u);
    }
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
