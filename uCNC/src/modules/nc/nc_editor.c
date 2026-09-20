/* The editor's typing half - see nc_editor.h. The draft a value is typed into
   and the floating helper that picks what to type: they own their state, while
   the document, the status line and the repaint flag are the screen's and are
   handed in. */
#include "nc_editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../cnc.h"
#include "nc_draw.h"
#include "nc_files.h"
#include "nc_layout.h"
#include "nc_menu.h"
#include "nc_text.h"
#include "nc_vocab.h"
#include "../lvds_renderer/lvds_draw_api.h"
#include "../lvds_renderer/lvds_hstx.h"

/* What is being typed, and what the helper is showing while it is typed. */
static nc_text_edit_t g_nc_editor_edit;
static char g_nc_editor_gcode_buf[8];
static bool g_nc_editor_modal_active;
static nc_footer_action_t g_nc_editor_modal_parent;
static const nc_footer_item_t *g_nc_editor_modal_items;
static size_t g_nc_editor_modal_count;
static const char *g_nc_editor_modal_title;
static size_t g_nc_editor_modal_line;
static char g_nc_editor_modal_prefix;
static size_t g_nc_editor_modal_origin_line;
static const char *g_nc_editor_modal_label;

static bool nc_editor_handle_selected_word_edit(nc_editor_ctx_t *ctx,
                                                char ch,
                                                nc_visual_key_t key)
{
    if (nc_files_active() ||
        !ctx->editable ||
        ctx->doc->selected_word < 0) {
        return false;
    }

    if (nc_text_edit_handle_key(ctx->doc,
                                &g_nc_editor_edit,
                                ch,
                                ctx->status,
                                ctx->status_size)) {
        return true;
    }
    return false;
}

static void nc_editor_close_modal(nc_editor_ctx_t *ctx)
{
    g_nc_editor_modal_active = false;
    g_nc_editor_modal_items = 0;
    g_nc_editor_modal_count = 0;
    g_nc_editor_modal_title = "";
    g_nc_editor_gcode_buf[0] = '\0';
    g_nc_editor_modal_line = (size_t)-1;
    g_nc_editor_modal_prefix = '\0';
    g_nc_editor_modal_label = 0;
}

static void nc_editor_modal_remove_line(nc_editor_ctx_t *ctx)
{
    if (g_nc_editor_modal_line < ctx->doc->line_count) {
        const char *text = ctx->doc->lines[g_nc_editor_modal_line].text;
        bool ours;

        if (g_nc_editor_modal_prefix) {
            ours = text[0] == g_nc_editor_modal_prefix &&
                   (text[1] == '\0' ||
                    strcmp(text + 1, g_nc_editor_gcode_buf) == 0);
        } else {
            ours = g_nc_editor_modal_label && strcmp(text, g_nc_editor_modal_label) == 0;
        }
        if (ours) {
            (void)nc_delete_line(ctx->doc, g_nc_editor_modal_line);
            if (g_nc_editor_modal_origin_line < ctx->doc->line_count) {
                ctx->doc->cursor_line = g_nc_editor_modal_origin_line;
            } else if (ctx->doc->line_count > 0u) {
                ctx->doc->cursor_line = ctx->doc->line_count - 1u;
            } else {
                ctx->doc->cursor_line = 0u;
            }
        }
    }
    g_nc_editor_modal_line = (size_t)-1;
    g_nc_editor_modal_prefix = '\0';
    g_nc_editor_modal_label = 0;
}

static void nc_editor_modal_cancel(nc_editor_ctx_t *ctx)
{
    nc_editor_modal_remove_line(ctx);
    nc_editor_close_modal(ctx);
}

/* Inserts the helper's own line under the cursor and puts the cursor on it, so
   the panel hangs below the line the user is working with. */
static bool nc_editor_modal_insert_line(nc_editor_ctx_t *ctx, const char *text)
{
    size_t origin = ctx->doc->cursor_line;
    size_t at = origin + 1u;

    if (at > ctx->doc->line_count) {
        at = ctx->doc->line_count;
    }
    if (nc_insert_line(ctx->doc, at, text) != NC_OK) {
        strncpy(ctx->status, "Insert line failed", ctx->status_size - 1);
        nc_editor_close_modal(ctx);
        return false;
    }
    /* nc_insert_line leaves the cursor on the new line; keep the line it came
       from so cancelling returns there. */
    g_nc_editor_modal_origin_line = origin;
    g_nc_editor_modal_line = at;
    ctx->doc->selected_word = -1;
    nc_text_edit_clear(&g_nc_editor_edit);
    return true;
}

static void nc_editor_modal_begin_line(nc_editor_ctx_t *ctx, char prefix)
{
    char line[4];

    snprintf(line, sizeof(line), "%c", prefix);
    g_nc_editor_modal_prefix = '\0';
    g_nc_editor_modal_label = 0;
    g_nc_editor_gcode_buf[0] = '\0';
    if (!nc_editor_modal_insert_line(ctx, line)) {
        return;
    }
    g_nc_editor_modal_prefix = prefix;
    g_nc_editor_modal_active = true;
    g_nc_editor_modal_items = 0;
    g_nc_editor_modal_count = 0;
    g_nc_editor_modal_title = prefix == 'G' ? "GCODE" : "TOOL";
    snprintf(ctx->status, ctx->status_size, "%c", prefix);
}

/* The submenu case: the line carries the action name the panel used to draw as
   its title, and doubles as the place the chosen text will be written. */
static void nc_editor_modal_begin_label(nc_editor_ctx_t *ctx, const char *label)
{
    if (!label || !label[0]) {
        return;
    }
    g_nc_editor_modal_prefix = '\0';
    g_nc_editor_gcode_buf[0] = '\0';
    if (!nc_editor_modal_insert_line(ctx, label)) {
        return;
    }
    g_nc_editor_modal_label = label;
}

static void nc_editor_modal_update_line(nc_editor_ctx_t *ctx)
{
    char text[16];

    if (g_nc_editor_modal_line >= ctx->doc->line_count || !g_nc_editor_modal_prefix) {
        return;
    }
    snprintf(text, sizeof(text), "%c%s", g_nc_editor_modal_prefix, g_nc_editor_gcode_buf);
    (void)nc_set_line(ctx->doc, g_nc_editor_modal_line, text);
}

static void nc_editor_select_first_value(nc_editor_ctx_t *ctx)
{
    nc_word_t words[24];
    int count;

    if (ctx->doc->cursor_line >= ctx->doc->line_count) {
        return;
    }
    count = nc_parse_words(ctx->doc->lines[ctx->doc->cursor_line].text,
                           words,
                           24);
    if (count > 0) {
        ctx->doc->selected_word = count > 1 ? 1 : 0;
    }
}

static void nc_editor_modal_begin(nc_editor_ctx_t *ctx, nc_footer_action_t parent)
{
    nc_editor_modal_cancel(ctx);
    if (parent == NC_FOOTER_ACTION_GCODE) {
        nc_editor_modal_begin_line(ctx, 'G');
        *ctx->dirty = true;
        return;
    }
    g_nc_editor_modal_active = true;
    g_nc_editor_modal_parent = parent;
    g_nc_editor_modal_items = nc_menu_submenu(parent, &g_nc_editor_modal_count);
    switch (parent) {
    case NC_FOOTER_ACTION_OPS: g_nc_editor_modal_title = "OPS"; break;
    case NC_FOOTER_ACTION_TOOL_MENU: g_nc_editor_modal_title = "TOOL"; break;
    case NC_FOOTER_ACTION_G7X_MENU: g_nc_editor_modal_title = "G7X"; break;
    case NC_FOOTER_ACTION_SYNC_MENU: g_nc_editor_modal_title = "THREAD"; break;
    case NC_FOOTER_ACTION_PECK_MENU: g_nc_editor_modal_title = "PECK"; break;
    default: g_nc_editor_modal_title = ""; break;
    }
    /* Give the helper the line it belongs to: the action name sits in that
       line and the panel hangs under it, instead of floating over the line the
       user is reading. Nothing is written into a document that cannot take
       edits (RUN and the like) - the panel still shows. */
    if (ctx->editable) {
        nc_editor_modal_begin_label(ctx, g_nc_editor_modal_title);
    }
    *ctx->dirty = true;
}

static bool nc_editor_modal_handle_key(nc_editor_ctx_t *ctx,
                                       char ch,
                                       nc_visual_key_t key)
{
    if (!g_nc_editor_modal_active) {
        return false;
    }
    if (g_nc_editor_modal_prefix) {
        if (key == NC_VISUAL_KEY_CANCEL || key == NC_VISUAL_KEY_MODE) {
            nc_editor_modal_cancel(ctx);
            *ctx->dirty = true;
            return true;
        }
        if (ch >= '0' && ch <= '9') {
            size_t len = strlen(g_nc_editor_gcode_buf);
            if (len + 1u < sizeof(g_nc_editor_gcode_buf)) {
                g_nc_editor_gcode_buf[len] = ch;
                g_nc_editor_gcode_buf[len + 1u] = '\0';
                nc_editor_modal_update_line(ctx);
                snprintf(ctx->status, ctx->status_size,
                         "%c%s", g_nc_editor_modal_prefix, g_nc_editor_gcode_buf);
            }
            *ctx->dirty = true;
            return true;
        }
        if (key == NC_VISUAL_KEY_BACKSPACE) {
            size_t len = strlen(g_nc_editor_gcode_buf);
            if (len > 0u) {
                g_nc_editor_gcode_buf[len - 1u] = '\0';
                nc_editor_modal_update_line(ctx);
            }
            *ctx->dirty = true;
            return true;
        }
        if (key == NC_VISUAL_KEY_FINISH) {
            nc_editor_modal_cancel(ctx);
            *ctx->dirty = true;
            return true;
        }
        if (key == NC_VISUAL_KEY_ACCEPT) {
            if (g_nc_editor_modal_prefix == 'G' && g_nc_editor_gcode_buf[0]) {
                int code = atoi(g_nc_editor_gcode_buf);
                const char *name = nc_vocab_gcode_name(code);
                const char *params = nc_vocab_gcode_parameters(code);
                const char *template_line = nc_vocab_gcode_template(code);
                const char *param_text = params ? params : "";
                if (name) {
                    (void)nc_set_line(ctx->doc,
                                      g_nc_editor_modal_line,
                                      template_line ? template_line : "");
                    nc_editor_select_first_value(ctx);
                    snprintf(ctx->status, ctx->status_size,
                             "G%d %s: %s", code, name, param_text);
                } else {
                    snprintf(ctx->status, ctx->status_size,
                             "Unknown G%d", code);
                }
            } else if (g_nc_editor_modal_prefix == 'T') {
                nc_editor_select_first_value(ctx);
                snprintf(ctx->status, ctx->status_size,
                         "T%s", g_nc_editor_gcode_buf);
            }
            nc_editor_close_modal(ctx);
            *ctx->dirty = true;
            return true;
        }
        return true;
    }
    if (key == NC_VISUAL_KEY_CANCEL || key == NC_VISUAL_KEY_MODE) {
        nc_editor_modal_cancel(ctx);
        *ctx->dirty = true;
        return true;
    }
    ch = ch;
    if (ch == '0') {
        nc_editor_modal_cancel(ctx);
        *ctx->dirty = true;
        return true;
    }
    if (ch >= '1' && ch <= '9') {
        size_t i;
        for (i = 0; i < g_nc_editor_modal_count; i++) {
            if (g_nc_editor_modal_items[i].key == ch) {
                uint8_t action = g_nc_editor_modal_items[i].action;
                /* The labelled line only marked the spot. Drop it first so the
                   chosen action writes exactly where it stood. */
                nc_editor_modal_cancel(ctx);
                if (action == NC_FOOTER_ACTION_TOOL_SELECT) {
                    nc_editor_modal_begin_line(ctx, 'T');
                } else {
                    /* The screen runs it: the menu's entries are the screen's. */
                    ctx->follow = action;
                }
                *ctx->dirty = true;
                return true;
            }
        }
    }
    return true;
}

/* --- what the screen calls ------------------------------------------------ */

bool nc_editor_modal_key(nc_editor_ctx_t *ctx, nc_visual_key_t key, char ch)
{
    return nc_editor_modal_handle_key(ctx, ch, key);
}

bool nc_editor_selected_word_key(nc_editor_ctx_t *ctx, nc_visual_key_t key, char ch)
{
    return nc_editor_handle_selected_word_edit(ctx, ch, key);
}

void nc_editor_open_modal(nc_editor_ctx_t *ctx, uint8_t action)
{
    nc_editor_modal_begin(ctx, (nc_footer_action_t)action);
}

/* The G/T field: what is picked goes on the line itself, no helper panel. */
void nc_editor_open_field(nc_editor_ctx_t *ctx, char prefix)
{
    nc_editor_modal_begin_line(ctx, prefix);
}

/* The movement keys drop a half-typed value instead of applying it. */
void nc_editor_clear_draft(void)
{
    nc_text_edit_clear(&g_nc_editor_edit);
}

void nc_editor_draw_aids(nc_editor_ctx_t *ctx)
{
    char buf[80];

if (nc_text_edit_active(&g_nc_editor_edit)) {
    snprintf(buf, sizeof(buf), "EDIT %s", nc_text_edit_buffer(&g_nc_editor_edit));
    lvds_draw_fill_rect(20, 526, LVDS_HSTX_WIDTH - 40, 18, NC_VISUAL_BG);
    nc_draw_text_clip(34, 526, buf, 70, NC_VISUAL_ACCENT, NC_VISUAL_BG, LVDS_FONT_NORMAL);
}
if (g_nc_editor_modal_active && !g_nc_editor_modal_prefix) {
    int visible = ctx->snapshot->cursor_visible_index;
    int keypad_y;
    int modal_x;
    if (visible < 0) {
        visible = 0;
    }
    /* The helper belongs to the line under the cursor - the line that
       carries the action name and will take the text. It hangs below that
       line instead of covering the line the user is reading. */
    keypad_y = NC_CODE_Y + (visible + 1) * NC_VISUAL_ROW_H - NC_MODAL_PAD;
    keypad_y = nc_draw_clampi(keypad_y, NC_CODE_Y,
                                NC_PANE_BOTTOM - NC_MODAL_PAD -
                                NC_MODAL_KEY_H * NC_MODAL_ROWS);
    /* Floating on the right, the way the machine's keypad sits beside the
       screen, and pinned so the keys end at the pane bottom when the helper
       line is too low to hang below it. */
    modal_x = NC_RIGHT_PANE_X + NC_RIGHT_PANE_W - NC_MODAL_W - 8;
    /* The G/T field takes its digits on the line itself, so only a submenu
       has a helper to draw. */
    nc_draw_modal_items(modal_x, keypad_y, g_nc_editor_modal_items,
                               g_nc_editor_modal_count, 0u);
}
}
