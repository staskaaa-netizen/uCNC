/* The editor's typing half - see nc_editor.h. The draft a value is typed into
   and the floating helper that picks what to type: they own their state, while
   the document, the status line and the repaint flag are the screen's and are
   handed in. */
#include "nc_editor.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../cnc.h"
#include "../../interface/grbl_stream.h"
#include "nc_draw.h"
#include "nc_emit.h"
#include "nc_files.h"
#include "nc_g7x.h"
#include "nc_layout.h"
#include "nc_menu.h"
#include "nc_presets.h"
#include "nc_preview.h"
#include "nc_run.h"
#include "nc_state.h"
#include "nc_text.h"
#include "nc_tools.h"
#include "nc_vocab.h"
#include "../g7x/g7x_contour.h"
#include "../lvds_renderer/lvds_draw_api.h"
#include "../lvds_renderer/lvds_hstx.h"

/* What is being typed, and what the helper is showing while it is typed. */
static nc_text_edit_t g_nc_editor_edit;
static char g_nc_editor_gcode_buf[8];
static bool g_nc_editor_modal_active;
static nc_footer_action_t g_nc_editor_modal_parent;
static const nc_footer_item_t *g_nc_editor_modal_items;
static size_t g_nc_editor_modal_count;
/* The pad's own storage: one item and one label per slot, filled when it opens. */
static nc_footer_item_t g_nc_editor_modal_built[NC_MODAL_ROWS * NC_MODAL_COLS];
static char g_nc_editor_modal_names[NC_MODAL_ROWS * NC_MODAL_COLS][16];
static const char *g_nc_editor_modal_title;
static size_t g_nc_editor_modal_line;
static char g_nc_editor_modal_prefix;
static size_t g_nc_editor_modal_origin_line;
static const char *g_nc_editor_modal_label;

static bool nc_editor_handle_selected_word_edit(nc_editor_ctx_t *ctx,
                                                char ch,
                                                nc_visual_key_t key)
{
    (void)key;
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
    (void)ctx;
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

/* The G7X submenu's `Q` and `N` entries: put that word on the line the cursor is
   on and select it, so the number is typed straight into it. This is how a P/Q
   range is written from the panel - `P`/`Q` on the header name block numbers,
   and those numbers are the `N` words on the profile rows.

   `N` numbers a profile block, and the block number comes before the motion word
   (`N100 G1 X20 Z0`), so it goes to the start of the line. `Q` names the end of
   the range and goes after the word the operator is looking at, or at the end of
   the line when nothing is picked. Both arrive as `N0`/`Q0`, the way the
   templates write their words: typing replaces the zero. */
static bool nc_editor_insert_range_word(nc_editor_ctx_t *ctx, char letter)
{
    char new_line[NC_MAX_LINE_LEN];
    nc_word_t words[24];
    const char *old;
    int insert_at;
    int count;
    int i;

    if (!ctx->editable || ctx->doc->line_count == 0 ||
        ctx->doc->cursor_line >= ctx->doc->line_count) {
        strncpy(ctx->status, "No line to put the word on", ctx->status_size - 1);
        return true;
    }
    old = ctx->doc->lines[ctx->doc->cursor_line].text;
    /* The line may already carry the word - a header written from the presets
       has `P0 Q0` on it. Then this key picks that word instead of adding a
       second one, which the parser would refuse as a repeated word. */
    count = nc_parse_words(old, words, 24);
    for (i = 0; i < count; i++) {
        if (words[i].letter == letter) {
            ctx->doc->selected_word = i;
            snprintf(ctx->status, ctx->status_size, "%c %s: type it", letter,
                     nc_vocab_label_for_word(old, &words[i]));
            *ctx->dirty = true;
            return true;
        }
    }
    if (letter == 'N') {
        insert_at = 0;
    } else {
        insert_at = (int)strlen(old);
        if (ctx->doc->selected_word >= 0 &&
            nc_get_selected_word(ctx->doc, &words[0]) == NC_OK) {
            insert_at = (int)words[0].end;
        }
    }

    if (letter == 'N') {
        if (snprintf(new_line, sizeof(new_line), "N0 %s", old) >= (int)sizeof(new_line)) {
            strncpy(ctx->status, "Line too long for N", ctx->status_size - 1);
            return true;
        }
    } else {
        if (insert_at + 4 + (int)strlen(old + insert_at) >= (int)sizeof(new_line)) {
            strncpy(ctx->status, "Line too long for Q", ctx->status_size - 1);
            return true;
        }
        memcpy(new_line, old, (size_t)insert_at);
        new_line[insert_at] = ' ';
        new_line[insert_at + 1] = 'Q';
        new_line[insert_at + 2] = '0';
        strcpy(new_line + insert_at + 3, old + insert_at);
    }

    if (nc_set_line(ctx->doc, ctx->doc->cursor_line, new_line) != NC_OK) {
        strncpy(ctx->status, "Could not write the word", ctx->status_size - 1);
        return true;
    }
    /* Select what was just written: the typed digits replace its zero. */
    ctx->doc->selected_word = -1;
    count = nc_parse_words(new_line, words, 24);
    for (i = 0; i < count; i++) {
        if (words[i].letter == letter) {
            ctx->doc->selected_word = i;
            snprintf(ctx->status, ctx->status_size, "%c %s: type it", letter,
                     nc_vocab_label_for_word(new_line, &words[i]));
            break;
        }
    }
    *ctx->dirty = true;
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

/* --- the contour pad (the G7X submenu's `7`) ------------------------------ */

/* What the pad draws when `7` is pressed: the same three-by-three the editor
   floats, with the nine keys of a turning profile. `5` is the centre and ends
   the contour; the corners move both axes, which is what makes a chamfer or a
   taper one press instead of two. `*` keeps the panel's own delete - it is not
   on the pad, but the key still takes the row just written back. */
static const nc_footer_item_t g_nc_editor_contour_items[] = {
    { '1', "Z- X+", NC_FOOTER_ACTION_NONE },
    { '2', "X+", NC_FOOTER_ACTION_NONE },
    { '3', "Z+ X+", NC_FOOTER_ACTION_NONE },
    { '4', "Z-", NC_FOOTER_ACTION_NONE },
    { '5', "END", NC_FOOTER_ACTION_NONE },
    { '6', "Z+", NC_FOOTER_ACTION_NONE },
    { '7', "Z- X-", NC_FOOTER_ACTION_NONE },
    { '8', "X-", NC_FOOTER_ACTION_NONE },
    { '9', "Z+ X-", NC_FOOTER_ACTION_NONE }
};

/* The distance one press moves: the operator types over the value that lands,
   so this is a starting point and `#` steps it. Millimetres, like the program
   the panel writes - the setup's units are the program's. */
static const float g_nc_editor_contour_steps[] = {
    0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 50.0f
};

static bool g_nc_editor_contour_active;
static uint8_t g_nc_editor_contour_step;
/* The row just written and the word left picked on it: while both are still
   under the cursor the point is being *entered*, and the digits belong to the
   editor's field flow, not to the pad. `D` takes a corner's second word and
   then closes the point, `*` drops it. */
static size_t g_nc_editor_contour_point = (size_t)-1;
static int g_nc_editor_contour_word = -1;
static int g_nc_editor_contour_next = -1;

static float nc_editor_contour_step_mm(void)
{
    return g_nc_editor_contour_steps[g_nc_editor_contour_step];
}

static void nc_editor_contour_settle(void)
{
    g_nc_editor_contour_point = (size_t)-1;
    g_nc_editor_contour_word = -1;
    g_nc_editor_contour_next = -1;
}

/* True while the row just written is still the one being entered: nothing has
   accepted the value and the cursor has not left it. */
static bool nc_editor_contour_pending(const nc_editor_ctx_t *ctx)
{
    return g_nc_editor_contour_point != (size_t)-1 &&
           g_nc_editor_contour_point < ctx->doc->line_count &&
           ctx->doc->cursor_line == g_nc_editor_contour_point &&
           ctx->doc->selected_word == g_nc_editor_contour_word;
}

/* A millimetre value as a program spells it: `G1 X30 Z-15`, not
   `G1 X30.000 Z-15.000`. The value is left picked for typing either way, so
   this is what the row reads when the operator does not change it. */
static void nc_editor_mm_text(float value, char *out, size_t out_sz)
{
    size_t len;

    snprintf(out, out_sz, "%.3f", (double)value);
    len = strlen(out);
    while (len > 0u && out[len - 1u] == '0') {
        out[--len] = '\0';
    }
    if (len > 0u && out[len - 1u] == '.') {
        out[--len] = '\0';
    }
    if (strcmp(out, "-0") == 0) {
        strcpy(out, "0");
    }
}

/* Where the profile is now: the point the row under the cursor leaves the tool
   at, read out of the program with the same rule the sender and the preview
   use. An axis the program has not given yet starts at the stock's own corner,
   which is where a lathe profile starts. */
static void nc_editor_contour_point(nc_editor_ctx_t *ctx, float *x, float *z)
{
    const nc_document_t *doc = ctx->doc;
    bool x_known = false;
    bool z_known = false;
    size_t last = doc->line_count ? doc->cursor_line : (size_t)-1;
    size_t i;

    *x = 0.0f;
    *z = 0.0f;
    for (i = 0u; i < doc->line_count && i <= last; i++) {
        const char *line = doc->lines[i].text;
        uint8_t words = 0u;
        float px = *x;
        float pz = *z;

        if (!nc_emit_line_is_direct(line)) {
            continue;                   /* a header's U is not an increment */
        }
        if (!nc_emit_line_point(line, &px, &pz, 0, 0u, &words)) {
            continue;
        }
        *x = px;
        *z = pz;
        if (words & NC_EMIT_WORD_X_ABS) {
            x_known = true;
        }
        if (words & NC_EMIT_WORD_Z_ABS) {
            z_known = true;
        }
    }
    if (!x_known || !z_known) {
        nc_preview_info_t info;

        nc_preview_collect(doc, &info);
        if (!x_known) {
            *x = info.stock_x;
        }
        if (!z_known) {
            *z = 0.0f;
        }
    }
}

/* One press: a row of its own, below the cursor, and the cursor on it - which
   is the whole of the "walk": the next press reads its point from the row just
   written. The axis that does not move is carried over, the way a program
   written by hand reads. */
static bool nc_editor_contour_move(nc_editor_ctx_t *ctx, int dx, int dz)
{
    char xs[24];
    char zs[24];
    char row[NC_MAX_LINE_LEN];
    nc_word_t words[24];
    size_t at = ctx->doc->cursor_line + 1u;
    float x = 0.0f;
    float z = 0.0f;
    float step = nc_editor_contour_step_mm();
    int count;

    nc_editor_contour_point(ctx, &x, &z);
    x += (float)dx * step;
    z += (float)dz * step;
    nc_editor_mm_text(x, xs, sizeof(xs));
    nc_editor_mm_text(z, zs, sizeof(zs));
    snprintf(row, sizeof(row), "G1 X%s Z%s", xs, zs);
    if (ctx->doc->line_count == 0u) {
        at = 0u;                    /* an empty program takes its first row */
    }
    if (nc_insert_line(ctx->doc, at, row) != NC_OK) {
        return false;
    }
    ctx->doc->cursor_line = at;
    /* The value the operator is most likely to change is picked: the X word
       for an X move, the Z word for a Z move, and for a corner the X one - the
       diameter, the number a lathe hand reads first. */
    count = nc_parse_words(row, words, 24);
    ctx->doc->selected_word = -1;
    g_nc_editor_contour_point = at;
    g_nc_editor_contour_word = -1;
    g_nc_editor_contour_next = -1;
    if (count > 0) {
        int i;

        for (i = 0; i < count; i++) {
            char letter = (char)toupper((unsigned char)words[i].letter);

            if ((dx != 0 && letter == 'X') || (dz != 0 && letter == 'Z')) {
                if (g_nc_editor_contour_word < 0) {
                    g_nc_editor_contour_word = i;
                } else {
                    g_nc_editor_contour_next = i;
                    break;
                }
            }
        }
        ctx->doc->selected_word = g_nc_editor_contour_word;
    }
    /* A fresh draft, so the first digit typed replaces the value the step
       prefilled instead of being appended to it. */
    nc_editor_clear_draft();
    snprintf(ctx->status, ctx->status_size, "%s  step %.1f mm", row,
             (double)step);
    return true;
}

static void nc_editor_contour_begin(nc_editor_ctx_t *ctx)
{
    if (!ctx->editable) {
        strncpy(ctx->status, "Not editable here", ctx->status_size - 1);
        return;
    }
    g_nc_editor_contour_active = true;
    snprintf(ctx->status, ctx->status_size, "Contour: 5 ends, # %.1f mm",
             (double)nc_editor_contour_step_mm());
    *ctx->dirty = true;
}

void nc_editor_contour_leave(void)
{
    g_nc_editor_contour_active = false;
    nc_editor_contour_settle();
}

bool nc_editor_contour_active(void)
{
    return g_nc_editor_contour_active;
}

/* The contour's keys, while its pad is up.

   Two modes, and which one is in force is the editor's own answer: while the
   word just written is still picked, the digits and the sign are the field's
   (one way to type a value, not two), `D` takes a corner's second word and then
   closes the point, `*` drops the row. Once nothing is picked, the digits are
   the pad's: the directions, `5` ends, `#` steps the distance, `*` takes the row
   under the cursor back. */
static bool nc_editor_contour_key(nc_editor_ctx_t *ctx, char ch,
                                  nc_visual_key_t key)
{
    int dx = 0;
    int dz = 0;

    if (!g_nc_editor_contour_active) {
        return false;
    }
    /* The mode key is the screen's: the pad closes and the screen changes,
       which is what keeps the rows already written. */
    if (key == NC_VISUAL_KEY_CANCEL || key == NC_VISUAL_KEY_MODE) {
        nc_editor_contour_leave();
        *ctx->dirty = true;
        return false;
    }
    if (nc_editor_contour_pending(ctx)) {
        if (ch == 'D') {
            if (g_nc_editor_contour_next >= 0) {
                g_nc_editor_contour_word = g_nc_editor_contour_next;
                g_nc_editor_contour_next = -1;
                ctx->doc->selected_word = g_nc_editor_contour_word;
                nc_editor_clear_draft();
                snprintf(ctx->status, ctx->status_size, "%s",
                         ctx->doc->lines[g_nc_editor_contour_point].text);
            } else {
                ctx->doc->selected_word = -1;
                nc_editor_contour_settle();
            }
            *ctx->dirty = true;
            return true;
        }
        if (ch == '*') {
            /* The point being entered: its row goes and the pad is back. */
            (void)nc_delete_line(ctx->doc, ctx->doc->cursor_line);
            nc_editor_contour_settle();
            strncpy(ctx->status, "Point dropped", ctx->status_size - 1);
            *ctx->dirty = true;
            return true;
        }
        /* The digits, the sign, the point and `#` are the editor's field
           flow: what is typed lands in the word the pad just picked. */
        return false;
    }
    if (ch == '5' || ch == '0') {
        g_nc_editor_contour_active = false;
        nc_editor_contour_settle();
        strncpy(ctx->status, "Contour ended", ctx->status_size - 1);
        *ctx->dirty = true;
        return true;
    }
    if (key == NC_VISUAL_KEY_FINISH) {              /* `#`: the step */
        g_nc_editor_contour_step =
            (uint8_t)((g_nc_editor_contour_step + 1u) %
                      (sizeof(g_nc_editor_contour_steps) /
                       sizeof(g_nc_editor_contour_steps[0])));
        snprintf(ctx->status, ctx->status_size, "Contour step %.1f mm",
                 (double)nc_editor_contour_step_mm());
        *ctx->dirty = true;
        return true;
    }
    if (key == NC_VISUAL_KEY_BACKSPACE) {           /* `*`: the delete */
        if (nc_delete_line(ctx->doc, ctx->doc->cursor_line) == NC_OK) {
            strncpy(ctx->status, "Point deleted", ctx->status_size - 1);
        }
        *ctx->dirty = true;
        return true;
    }
    switch (ch) {
    case '1': dx = 1; dz = -1; break;
    case '2': dx = 1; break;
    case '3': dx = 1; dz = 1; break;
    case '4': dz = -1; break;
    case '6': dz = 1; break;
    case '7': dx = -1; dz = -1; break;
    case '8': dx = -1; break;
    case '9': dx = -1; dz = 1; break;
    default: break;
    }
    if (dx == 0 && dz == 0) {
        return true;                                /* a key the pad does not use */
    }
    if (!nc_editor_contour_move(ctx, dx, dz)) {
        strncpy(ctx->status, "Insert failed", ctx->status_size - 1);
    }
    *ctx->dirty = true;
    return true;
}

/* A pad is the panel's own entries - the ones that *do* something rather than
   write text: `Q`/`N`, the `G` field, the `T` field, the tool table - plus the
   card's sections for every other slot. A slot's id is its key path,
   `<footer key><pad key>`, so a section lands where its id says and a card can
   add an entry of its own by writing that id: it is offered as soon as a
   section has it. The label is the section's `name=`, which is the one place the
   file already names the entry - so the panel keeps no second copy of it. */
static void nc_editor_modal_build(nc_footer_action_t parent)
{
    const nc_footer_item_t *table;
    size_t table_count = 0u;
    char digit = nc_menu_submenu_digit(parent);
    int key;

    g_nc_editor_modal_count = 0u;
    if (!digit) {
        return;
    }
    table = nc_menu_submenu(parent, &table_count);
    for (key = 1; key <= (int)(NC_MODAL_ROWS * NC_MODAL_COLS); key++) {
        char ch = (char)('0' + key);
        const nc_footer_item_t *row = 0;
        nc_footer_item_t *item = &g_nc_editor_modal_built[g_nc_editor_modal_count];
        size_t i;

        for (i = 0u; i < table_count; i++) {
            if (table[i].key == ch && table[i].action != NC_FOOTER_ACTION_NONE) {
                row = &table[i];
                break;
            }
        }
        if (row) {
            item->key = ch;
            item->label = row->label;
            item->action = row->action;
        } else if (nc_preset_name_for_id(((int)(digit - '0') * 10) + key,
                                         g_nc_editor_modal_names[g_nc_editor_modal_count],
                                         sizeof(g_nc_editor_modal_names[0]))) {
            item->key = ch;
            item->label = g_nc_editor_modal_names[g_nc_editor_modal_count];
            item->action = NC_FOOTER_ACTION_PRESET_ID;
        } else {
            continue;
        }
        g_nc_editor_modal_count++;
    }
}

static void nc_editor_modal_begin(nc_editor_ctx_t *ctx, nc_footer_action_t parent)
{
    nc_editor_modal_cancel(ctx);
    g_nc_editor_modal_active = true;
    g_nc_editor_modal_parent = parent;
    nc_editor_modal_build(parent);
    g_nc_editor_modal_items = g_nc_editor_modal_built;
    switch (parent) {
    case NC_FOOTER_ACTION_OPS: g_nc_editor_modal_title = "OPS"; break;
    case NC_FOOTER_ACTION_TOOL_MENU: g_nc_editor_modal_title = "TOOL"; break;
    case NC_FOOTER_ACTION_G7X_MENU: g_nc_editor_modal_title = "G7X"; break;
    case NC_FOOTER_ACTION_SYNC_MENU: g_nc_editor_modal_title = "THREAD"; break;
    case NC_FOOTER_ACTION_PECK_MENU: g_nc_editor_modal_title = "PECK"; break;
    case NC_FOOTER_ACTION_GCODE: g_nc_editor_modal_title = "WORD"; break;
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
    /* The contour pad is the modal's own child: while it is up it owns the
       keys, the same way the helper does. */
    if (nc_editor_contour_key(ctx, ch, key)) {
        return true;
    }
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
                } else if (action == NC_FOOTER_ACTION_PRESET_ID) {
                    /* A slot the card fills: the id is the key path, so the
                       section is what this key writes - and its rows do the
                       writing, inline rows and all. */
                    char digit = nc_menu_submenu_digit(g_nc_editor_modal_parent);
                    int id = digit ? ((int)(digit - '0') * 10) + (ch - '0') : 0;

                    if (!digit || !nc_insert_preset_id(ctx->doc, id)) {
                        strncpy(ctx->status, "No such entry", ctx->status_size - 1);
                    } else {
                        nc_preset_name_for_id(id, ctx->status, ctx->status_size);
                    }
                } else if (action == NC_FOOTER_ACTION_CONTOUR) {
                    /* `7` under G7X: the pad becomes the contour pad, in place,
                       and stays until `5`. Nothing is written for the press
                       itself - the profile is the rows the next presses add. */
                    nc_editor_contour_begin(ctx);
                } else if (action == NC_FOOTER_ACTION_GCODE) {
                    /* The word field: type a G-code and its template lands, which
                       is where a single line of any kind is written. */
                    nc_editor_modal_begin_line(ctx, 'G');
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
if (g_nc_editor_contour_active) {
    int visible = ctx->snapshot->cursor_visible_index;
    int keypad_y;
    int modal_x;

    if (visible < 0) {
        visible = 0;
    }
    keypad_y = NC_CODE_Y + (visible + 1) * NC_VISUAL_ROW_H - NC_MODAL_PAD;
    keypad_y = nc_draw_clampi(keypad_y, NC_CODE_Y,
                                NC_PANE_BOTTOM - NC_MODAL_PAD -
                                NC_MODAL_KEY_H * NC_MODAL_ROWS);
    modal_x = NC_RIGHT_PANE_X + NC_RIGHT_PANE_W - NC_MODAL_W - 8;
    nc_draw_modal_items(modal_x, keypad_y, g_nc_editor_contour_items,
                               sizeof(g_nc_editor_contour_items) /
                               sizeof(g_nc_editor_contour_items[0]), 0u);
}
}


/* What the editor owns between frames: the file-name row above line 1, the name
   being typed for a new file, and which action is armed when a save failed. */
static bool g_nc_editor_name_selected;
static bool g_nc_editor_new_file_active;
static char g_nc_editor_new_file_name[NC_FILE_NAME_MAX];
static uint8_t g_nc_editor_unsaved_arm;

/* Called whenever the buffer is about to be reused - a screen change, opening
   another file, creating one. The edits go to the card first. If that write
   fails the caller must not go on: the document is still in memory and is the
   only copy, so it stays open and the operator sees why. */
bool nc_editor_save_current(nc_editor_ctx_t *ctx)
{
    /* Anything the panel can open, it can save and come back to: the operator
       edits the text it showed them - the preset entries included - not only
       programs. `nc_save_file()` has drawn the same line. */
    if (ctx->doc->dirty && nc_path_text(ctx->doc->path) &&
        nc_save_file(ctx->doc, ctx->doc->path) != NC_OK) {
        return false;
    }
    g_nc_editor_unsaved_arm = 0;
    if (nc_path_text(ctx->doc->path)) {
        nc_state_remember_path(ctx->mode, ctx->doc->path);
        nc_state_remember_cursor(ctx->doc);
        nc_state_save();
    }
    return true;
}

bool nc_editor_proceed_without_saving(nc_editor_ctx_t *ctx, uint8_t kind)
{
    (void)ctx;
    if (g_nc_editor_unsaved_arm == kind) {
        g_nc_editor_unsaved_arm = 0;
        return true;
    }
    g_nc_editor_unsaved_arm = kind;
    return false;
}

void nc_editor_seed_demo(nc_editor_ctx_t *ctx)
{
    nc_document_init(ctx->doc);
    nc_insert_line(ctx->doc, 0, "G970 X-10 U120 Z-150 W30");
    nc_insert_line(ctx->doc, 1, "G971 X80 Z125 E0");
    nc_insert_line(ctx->doc, 2, "G972 C15");
    nc_insert_line(ctx->doc, 3, "G973 P7");
    nc_insert_line(ctx->doc, 4, "G71 U2 R1 X0.5 Z0.5 F120");
    nc_insert_line(ctx->doc, 5, "\tG1 X50 Z0");
    nc_insert_line(ctx->doc, 6, "\tG1 X25 Z-25");
    nc_insert_line(ctx->doc, 7, "G80");
    ctx->doc->cursor_line = 4;
    nc_select_next_word(ctx->doc);
    nc_select_next_word(ctx->doc);
    strncpy(ctx->doc->path, "NC module bring-up", sizeof(ctx->doc->path) - 1);
    ctx->doc->dirty = false;
    ctx->status[0] = '\0';
    *ctx->dirty = true;
}

bool nc_editor_selected_tool_word(nc_editor_ctx_t *ctx, char *letter, int *line_index)
{
    nc_word_t word;

    if (letter) {
        *letter = '\0';
    }
    if (line_index) {
        *line_index = -1;
    }
    if (ctx->mode != NC_MODE_TOOLS ||
        nc_get_selected_word(ctx->doc, &word) != NC_OK ||
        ctx->doc->cursor_line >= ctx->doc->line_count ||
        !nc_tool_line_is_tool(ctx->doc->lines[ctx->doc->cursor_line].text)) {
        return false;
    }
    if (letter) {
        *letter = word.letter;
    }
    if (line_index) {
        *line_index = (int)ctx->doc->cursor_line;
    }
    return true;
}

int nc_editor_find_tool_line(nc_editor_ctx_t *ctx, int selected_tool, int *selected_line)
{
    int count = 0;
    size_t i;

    if (selected_line) {
        *selected_line = -1;
    }
    for (i = 0; i < ctx->doc->line_count; i++) {
        if (nc_tool_line_is_tool(ctx->doc->lines[i].text)) {
            if (count == selected_tool && selected_line) {
                *selected_line = (int)i;
            }
            count++;
        }
    }
    return count;
}

int nc_editor_selected_tool_index(nc_editor_ctx_t *ctx)
{
    int count = 0;
    size_t i;

    for (i = 0; i < ctx->doc->line_count; i++) {
        if (nc_tool_line_is_tool(ctx->doc->lines[i].text)) {
            if (i >= ctx->doc->cursor_line) {
                return count;
            }
            count++;
        }
    }
    return count > 0 ? count - 1 : 0;
}

static const char *nc_editor_file_basename(const char *path)
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

static const char *nc_editor_new_file_ext(nc_editor_ctx_t *ctx)
{
    return ctx->mode == NC_MODE_TOOLS ? ".t" : ".nc";
}

static void nc_editor_new_file_status(nc_editor_ctx_t *ctx)
{
    snprintf(ctx->status,
             ctx->status_size,
             "New:%.40s%s #OK *DEL A",
             g_nc_editor_new_file_name[0] ? g_nc_editor_new_file_name : "_",
             nc_editor_new_file_ext(ctx));
}

static bool nc_editor_new_file_handle_key(nc_editor_ctx_t *ctx,
                                          char ch,
                                          nc_visual_key_t key)
{
    size_t len;
    char path[NC_PATH_MAX];
    nc_result_t r;

    if (!g_nc_editor_new_file_active) {
        return false;
    }

    if (key == NC_VISUAL_KEY_CANCEL) {
        g_nc_editor_new_file_active = false;
        strncpy(ctx->status, "New file cancelled", ctx->status_size - 1);
        return true;
    }
    if (key == NC_VISUAL_KEY_FINISH || key == NC_VISUAL_KEY_ACCEPT) {
        /* The new file replaces the open one in the same buffer: save it
           first, the same way a screen change does. */
        if (!nc_editor_save_current(ctx) &&
            !nc_editor_proceed_without_saving(ctx, NC_EDITOR_UNSAVED_NEW)) {
            strncpy(ctx->status, "Save failed - press again to create", ctx->status_size - 1);
            return true;
        }
        if (!g_nc_editor_new_file_name[0] ||
            !nc_files_create_named(g_nc_editor_new_file_name, nc_editor_new_file_ext(ctx), path, sizeof(path))) {
            strncpy(ctx->status, "New file create failed", ctx->status_size - 1);
            return true;
        }
        r = nc_load_file(ctx->doc, path);
        if (r != NC_OK) {
            snprintf(ctx->status, ctx->status_size, "Create open failed: %s", nc_result_text(r));
            return true;
        }
        g_nc_editor_new_file_active = false;
        nc_files_set_active(false);
        nc_state_remember_path(ctx->mode, path);
        nc_state_save();
        snprintf(ctx->status, ctx->status_size, "Created %s", nc_editor_file_basename(path));
        return true;
    }
    if (key == NC_VISUAL_KEY_BACKSPACE) {
        len = strlen(g_nc_editor_new_file_name);
        if (len > 0u) {
            g_nc_editor_new_file_name[len - 1u] = '\0';
        }
        nc_editor_new_file_status(ctx);
        return true;
    }
    if (ch >= '0' && ch <= '9') {
        len = strlen(g_nc_editor_new_file_name);
        if (len + 1u < sizeof(g_nc_editor_new_file_name)) {
            g_nc_editor_new_file_name[len] = ch;
            g_nc_editor_new_file_name[len + 1u] = '\0';
        }
        nc_editor_new_file_status(ctx);
        return true;
    }

    nc_editor_new_file_status(ctx);
    return true;
}

void nc_editor_move_tool_line(nc_editor_ctx_t *ctx, int delta)
{
    int selected_tool = nc_editor_selected_tool_index(ctx);
    int tool_count = nc_editor_find_tool_line(ctx, selected_tool, NULL);
    int selected_line = -1;

    if (tool_count == 0) {
        strncpy(ctx->status, "No tool rows", ctx->status_size - 1);
        return;
    }
    if (delta < 0 && selected_tool > 0) {
        selected_tool--;
    } else if (delta > 0 && selected_tool + 1 < tool_count) {
        selected_tool++;
    }
    (void)nc_editor_find_tool_line(ctx, selected_tool, &selected_line);
    if (selected_line >= 0) {
        ctx->doc->cursor_line = (size_t)selected_line;
        ctx->doc->selected_word = -1;
        nc_editor_clear_draft();
        snprintf(ctx->status,
                 ctx->status_size,
                 "Tool %d/%d",
                 selected_tool + 1,
                 tool_count);
        nc_editor_serial_selected_line(ctx);
    }
}

static void nc_editor_set_code_line(nc_editor_ctx_t *ctx, size_t line)
{
    if (ctx->doc->line_count == 0) {
        ctx->doc->cursor_line = 0;
        nc_run_set_line(ctx->doc, 0);
        return;
    }
    if (line >= ctx->doc->line_count) {
        line = ctx->doc->line_count - 1;
    }
    ctx->doc->cursor_line = line;
    if (ctx->mode == NC_MODE_RUN) {
        nc_run_set_line(ctx->doc, line);
    }
    ctx->doc->selected_word = -1;
    g_nc_editor_name_selected = false;
}

static size_t nc_editor_code_line(nc_editor_ctx_t *ctx)
{
    /* RUN has one cursor, and it is the machine's: the unit being run, or the
       line the sender is on between units (`nc_run_display_line()`). The pane
       marks it, the line keys move it and `1 SINGLE` acts on it. */
    size_t line = ctx->mode == NC_MODE_RUN ? nc_run_display_line()
                                           : ctx->doc->cursor_line;

    return ctx->doc->line_count && line >= ctx->doc->line_count ? 0 : line;
}

void nc_editor_move_line(nc_editor_ctx_t *ctx, int delta)
{
    size_t line = nc_editor_code_line(ctx);

    /* RUN belongs to the machine: while a run is in flight the line is the
       sender's, and a key that moved it would move what the program does next -
       the mark and the stream would part company again, which is the fault this
       whole arrangement exists to prevent. The keys work in RUN when nothing is
       running, which is what they are for there (`2 FROM` starts on the line
       they leave the cursor on). */
    if (ctx->mode == NC_MODE_RUN && nc_run_active()) {
        strncpy(ctx->status, "RUN owns the line", ctx->status_size - 1);
        *ctx->dirty = true;
        return;
    }
    if (!ctx->code_view ||
        ctx->doc->line_count == 0) {
        strncpy(ctx->status, "No code lines", ctx->status_size - 1);
        return;
    }
    /* Above line 1 sits the row naming the file this code belongs to. */
    if (delta < 0 && (g_nc_editor_name_selected || line == 0)) {
        g_nc_editor_name_selected = true;
        /* Drop the value selection: the movement keys must stay movement keys
           while the name row is the cursor. */
        ctx->doc->selected_word = -1;
        nc_editor_clear_draft();
        ctx->status[0] = '\0';
        return;
    }
    if (delta > 0 && g_nc_editor_name_selected) {
        g_nc_editor_name_selected = false;
        ctx->status[0] = '\0';
        return;
    }
    if (delta < 0 && line > 0) {
        line--;
    } else if (delta > 0 && line + 1 < ctx->doc->line_count) {
        line++;
    }

    nc_editor_set_code_line(ctx, line);
    /* No position report: the editor numbers the lines and highlights this
       one. The message line stays for messages. */
    ctx->status[0] = '\0';
}

nc_result_t nc_editor_insert_tool_ref(nc_editor_ctx_t *ctx)
{
    int tool_no = 1;
    char line[16];

    (void)nc_tool_number_for_line(ctx->doc, ctx->doc->cursor_line + 1u, &tool_no);
    if (tool_no <= 0) {
        tool_no = 1;
    }
    snprintf(line, sizeof(line), "T%d", tool_no);
    return nc_insert_line(ctx->doc, ctx->doc->cursor_line + 1u, line);
}

void nc_editor_open_files(nc_editor_ctx_t *ctx, const char *root, bool seed_samples)
{
    int made = seed_samples ? nc_files_seed_samples() : 0;

    nc_files_set_active(true);
    g_nc_editor_new_file_active = false;
    if (nc_files_refresh(root)) {
        /* The pane header already shows the directory. */
        strncpy(ctx->status,
                made ? "Sample files created" : "",
                ctx->status_size - 1);
        /* Land on the file that is open, if it is in this folder. */
        (void)nc_files_select_path(ctx->doc->path);
        /* The screen refreshes the preview of whatever the list points at
           after every key, so the list opening here is picked up there. */
    } else {
        strncpy(ctx->status, "File list unavailable", ctx->status_size - 1);
    }
}

/* Enter on the file-name row shows the folder the open file lives in. */
void nc_editor_open_current_folder(nc_editor_ctx_t *ctx)
{
    char dir[NC_PATH_MAX];
    const char *path = ctx->doc->path;
    const char *slash = 0;
    size_t i;
    size_t len = strlen(path);

    for (i = 0; i < len; i++) {
        if (path[i] == '/' || path[i] == '\\') {
            slash = path + i;
        }
    }
    if (!slash || slash == path) {
        nc_editor_open_files(ctx, NC_FILES_DIR, false);
        return;
    }
    snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
    nc_editor_open_files(ctx, dir, false);
}

void nc_editor_serial_selected_line(nc_editor_ctx_t *ctx)
{
    size_t line = nc_editor_code_line(ctx);
    const char *text = "";

    if (line < ctx->doc->line_count) {
        text = ctx->doc->lines[line].text;
    }
    grbl_stream_printf(__romstr__("[MSG:NC SELECT %s %lu: %.96s]\r\n"),
                       nc_menu_mode_name(ctx->mode),
                       (unsigned long)(line + 1),
                       text);
}

void nc_editor_serial_selected_file(void)
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

static bool nc_editor_line_is_contour_detail(const nc_document_t *doc, size_t index)
{
    if (!doc || index >= doc->line_count) {
        return false;
    }
    if (g7x_contour_cmd_from_line(doc->lines[index].text) == G7X_CONTOUR_NONE) {
        return false;
    }
    return nc_g7x_line_is_any_contour(doc, index);
}

/* The file-name row above line 1 holds the cursor: Enter opens the folder the
   open file lives in, and anything but the movement keys hands the cursor back
   to the code. */
bool nc_editor_key_name(nc_editor_ctx_t *ctx, nc_visual_key_t key)
{
    if (!g_nc_editor_name_selected) {
        return false;
    }
    if (key == NC_VISUAL_KEY_ACCEPT) {
        nc_editor_open_current_folder(ctx);
        ctx->status[0] = '\0';
        *ctx->dirty = true;
        return true;
    }
    if (key != NC_VISUAL_KEY_PREV && key != NC_VISUAL_KEY_NEXT &&
        key != NC_VISUAL_KEY_MODE) {
        /* Anything else hands the cursor back to the code. */
        g_nc_editor_name_selected = false;
    }
    return false;
}


/* --- what the screen calls ------------------------------------------------ */

/* The new-file field, the word keys, the field keys and the selected value,
   in the order the screen offers them. */
bool nc_editor_key_edit(nc_editor_ctx_t *ctx, nc_visual_key_t key, char ch)
{
if (nc_editor_new_file_handle_key(ctx, ch, key)) {
    ctx->status[ctx->status_size - 1] = '\0';
    *ctx->dirty = true;
    return true;
}
if (key == NC_VISUAL_KEY_WORD_PREV || key == NC_VISUAL_KEY_WORD_NEXT) {
    nc_result_t result = key == NC_VISUAL_KEY_WORD_PREV ?
                         nc_select_prev_word(ctx->doc) :
                         nc_select_next_word(ctx->doc);
    snprintf(ctx->status, ctx->status_size, "%s",
             result == NC_OK ? "Word" : "No editable word on this line");
    *ctx->dirty = true;
    return true;
}
if (key == NC_VISUAL_KEY_FIELD_PREV || key == NC_VISUAL_KEY_FIELD_NEXT) {
    bool forward = key == NC_VISUAL_KEY_FIELD_NEXT;
    nc_word_t word;
    nc_result_t result;

    /* TOOLS uses Up/Down to select tool rows. Field walking is for the
       editor screens; on TOOLS the row selection is the cursor. */
    if (!nc_files_active() && ctx->mode == NC_MODE_TOOLS) {
        /* TOOLS' rows are the footer's step/back, which the screen has
           already offered this key to. */
        return false;
    }

    /* Field walking belongs to editable code views only. In the file list,
       a menu, or a view that only runs and simulates (RUN, SIM) these keys
       keep their original path through the footer actions, which also
       reports the newly selected line - moving the cursor directly here
       would move the highlight without registering the selection. */
    if (nc_files_active()) {
        /* The file list's own up/down is what the footer's STEP/BACK do. */
        if (forward) {
            nc_files_select_next();
            strncpy(ctx->status, "File down", ctx->status_size - 1);
        } else {
            nc_files_select_prev();
            strncpy(ctx->status, "File up", ctx->status_size - 1);
        }
        nc_editor_serial_selected_file();
        *ctx->dirty = true;
        return true;
    }
    if (!ctx->code_view || !ctx->editable) {
        /* RUN shows the code without editing it: the key is a line move. The
           move changes what the panel shows, so it asks for the repaint here -
           an unedited screen has no other reason to draw itself, and without
           this the highlight stayed where it was until the next footer key
           happened to mark the screen dirty. */
        nc_editor_move_line(ctx, forward ? 1 : -1);
        nc_editor_serial_selected_line(ctx);
        *ctx->dirty = true;
        return true;
    }
    /* Arrows never edit: drop any draft instead of applying it, so a stray
       decimal point cannot be pushed into the program. */
    nc_editor_clear_draft();
    result = forward ? nc_select_same_word_next(ctx->doc) :
                       nc_select_same_word_prev(ctx->doc);
    if (result == NC_OK &&
        nc_get_selected_word(ctx->doc, &word) == NC_OK) {
        /* The field itself is highlighted; no position report. */
        ctx->status[0] = '\0';
        *ctx->dirty = true;
        return true;
    }
    /* No field of that letter to go to (including "no word selected at
       all"): move one line directly. Going through the key dispatch here
       is what caused the bug, because while a word is selected those
       letters mean sign (B), dot (C) and next word/accept (D). Moving the
       cursor ourselves cannot start an edit and cannot write a value. */
    if (forward) {
        if (g_nc_editor_name_selected) {
            /* Down from the file-name row is line 1. */
            g_nc_editor_name_selected = false;
        } else {
            nc_cursor_down(ctx->doc);
        }
    } else if (ctx->doc->cursor_line == 0) {
        /* Above line 1 sits the row naming the file. */
        g_nc_editor_name_selected = true;
    } else {
        nc_cursor_up(ctx->doc);
    }
    ctx->doc->selected_word = -1;
    nc_editor_clear_draft();
    ctx->status[0] = '\0';
    *ctx->dirty = true;
    return true;
}
    return false;
}

void nc_editor_new_file_begin(nc_editor_ctx_t *ctx)
{
    g_nc_editor_new_file_active = true;
    g_nc_editor_new_file_name[0] = '\0';
    nc_editor_new_file_status(ctx);
}

/* The field is gone: a file was created, or another key took the screen away. */
void nc_editor_new_file_end(nc_editor_ctx_t *ctx)
{
    (void)ctx;
    g_nc_editor_new_file_active = false;
}

/* The plain cursor keys, once the screen has decided this view may be edited. */
void nc_editor_key_cursor(nc_editor_ctx_t *ctx, nc_visual_key_t key)
{
switch (key) {
case NC_VISUAL_KEY_PREV:
    nc_editor_clear_draft();
    nc_cursor_up(ctx->doc);
    ctx->status[0] = '\0';
    break;
case NC_VISUAL_KEY_NEXT:
    nc_editor_clear_draft();
    nc_cursor_down(ctx->doc);
    ctx->status[0] = '\0';
    break;
case NC_VISUAL_KEY_ACCEPT:
case NC_VISUAL_KEY_FINISH:
    if (nc_select_next_word(ctx->doc) == NC_OK) {
        strncpy(ctx->status, "Next word", ctx->status_size - 1);
    } else {
        strncpy(ctx->status, "No editable word here", ctx->status_size - 1);
    }
    break;
case NC_VISUAL_KEY_BACKSPACE:
    if (nc_select_prev_word(ctx->doc) == NC_OK) {
        strncpy(ctx->status, "Previous word", ctx->status_size - 1);
    } else {
        strncpy(ctx->status, "No editable word here", ctx->status_size - 1);
    }
    break;
case NC_VISUAL_KEY_CANCEL:
    nc_editor_clear_draft();
    ctx->doc->selected_word = -1;
    strncpy(ctx->status, "Selection cleared", ctx->status_size - 1);
    break;
default:
    break;
}
}

void nc_editor_draw_files(nc_editor_ctx_t *ctx)
{
    char buf[80];
    int row;
    int line_cols = (NC_RIGHT_PANE_W - NC_LINE_TEXT_X_PAD) / NC_VISUAL_CHAR_W;

    int count = nc_files_count();
    int selected_file = nc_files_selected();
    int first_file = 0;
    const int visible_files = 14;
    if (selected_file >= visible_files) {
        first_file = selected_file - visible_files + 1;
    }
    nc_draw_text_clip(NC_RIGHT_PANE_X + 12, NC_PANE_Y + 10, "NC FILES", 18, NC_VISUAL_ACCENT, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    nc_draw_text_clip(NC_RIGHT_PANE_X + 96, NC_PANE_Y + 10, nc_files_cwd(), 32, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    if (count > 0 && selected_file >= 0) {
        snprintf(buf, sizeof(buf), "%d/%d", selected_file + 1, count);
        nc_draw_text_clip(NC_RIGHT_PANE_X + NC_RIGHT_PANE_W - 70, NC_PANE_Y + 10, buf, 8, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    }
    if (!nc_files_ready()) {
        nc_draw_text_clip(NC_RIGHT_PANE_X + 16, NC_PANE_Y + 48, "File list not ready", 36, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
    } else if (!count) {
        nc_draw_text_clip(NC_RIGHT_PANE_X + 16, NC_PANE_Y + 48, "No NC files found", 36, NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_NORMAL);
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
        nc_draw_text_clip(NC_RIGHT_PANE_X + 16,
                                 y,
                                 buf,
                                 line_cols,
                                 selected ? NC_VISUAL_TEXT : NC_VISUAL_DIM,
                                 bg,
                                 LVDS_FONT_NORMAL);
    }
    if (g_nc_editor_new_file_active) {
        snprintf(buf,
                 sizeof(buf),
                 "NEW: %s%s  # OK  * DEL  A CANCEL",
                 g_nc_editor_new_file_name[0] ? g_nc_editor_new_file_name : "_",
                 nc_editor_new_file_ext(ctx));
        lvds_draw_fill_rect(NC_RIGHT_PANE_X + 8, NC_PANE_BOTTOM - 26,
                            NC_RIGHT_PANE_W - 16, 22, NC_VISUAL_BG);
        nc_draw_text_clip(NC_RIGHT_PANE_X + 16,
                                 NC_PANE_BOTTOM - 24,
                                 buf,
                                 line_cols,
                                 NC_VISUAL_TEXT,
                                 NC_VISUAL_BG,
                                 LVDS_FONT_NORMAL);
    }
}

void nc_editor_draw_pane(nc_editor_ctx_t *ctx)
{
    /* Wide enough for the file path (NC_MAX_PATH-style length) plus the dirty
       mark: the name row is the one place a whole path is formatted. */
    char buf[128];
    int row;
    int line_cols = (NC_RIGHT_PANE_W - NC_LINE_TEXT_X_PAD) / NC_VISUAL_CHAR_W;

    /* The file this code belongs to, as the editor's own first row: the
       cursor can sit on it and Enter opens its folder. */
    {
        bool sel = g_nc_editor_name_selected;
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
                 ctx->snapshot->path[0] ? ctx->snapshot->path : "(no file)",
                 ctx->snapshot->dirty ? " *" : "");
        nc_draw_text_clip(NC_RIGHT_PANE_X + NC_LINE_TEXT_X_PAD,
                                 NC_PANE_Y,
                                 buf,
                                 line_cols,
                                 fg,
                                 text_bg,
                                 LVDS_FONT_NORMAL);
    }
    for (row = 0; row < NC_MAX_VISIBLE_LINES; row++) {
        int y = NC_CODE_Y + row * NC_VISUAL_ROW_H;
        size_t line_index = ctx->snapshot->first_line + (size_t)row;
        bool selected = line_index == nc_editor_code_line(ctx);
        /* The rest of the cycle the marked line belongs to gets the weaker
           mark. It is the cell RUN is on, not the whole document: a plain move
           outside any cycle marks nothing, because the block the screen hands
           over is the marked line itself. Copy, never a scan - which block a
           line is in is the screen's answer (`nc_g7x_block_containing()`), so
           the pane and the sender cannot disagree about it. */
        bool in_block = !selected &&
                        ctx->snapshot->block_mark &&
                        line_index >= ctx->snapshot->block_first &&
                        line_index <= ctx->snapshot->block_last;
        lvds_color_t row_bg;
        char display_line[NC_MAX_LINE_LEN + 2];
        const char *line_text = ctx->snapshot->lines[row];
        int word_start = ctx->editable ? ctx->snapshot->selected_word_start : -1;
        int word_end = ctx->editable ? ctx->snapshot->selected_word_end : -1;
        if (line_text[0] != '\t' &&
            line_text[0] != ' ' &&
            nc_editor_line_is_contour_detail(ctx->doc, line_index)) {
            display_line[0] = '\t';
            strncpy(display_line + 1, line_text, sizeof(display_line) - 2);
            display_line[sizeof(display_line) - 1] = '\0';
            line_text = display_line;
            if (word_start >= 0) word_start++;
            if (word_end >= 0) word_end++;
        }
        if (selected &&
            ctx->editable &&
            ctx->mode != NC_MODE_RUN &&
            ctx->snapshot->selected_label[0]) {
            /* The legend takes the row above the cursor line. For line 1
               that row is the file-name row, so the legend sits over the
               name for as long as it is showing and the name comes back
               when it is not - the same way it covers a code line on every
               other row. */
            int hint_y = (row == 0) ? NC_PANE_Y : (y - NC_VISUAL_ROW_H);
            /* The row it covers keeps the mark that row has: while the cursor
               sits in a cycle, the row above it is part of the pale path, and
               the legend must not punch a hole in it exactly when the operator
               is editing a word of that path. */
            bool hint_pale = row > 0 &&
                             ctx->snapshot->block_mark &&
                             line_index - 1u >= ctx->snapshot->block_first &&
                             line_index - 1u <= ctx->snapshot->block_last;
            lvds_color_t hint_bg = hint_pale ? NC_VISUAL_SELECT_BLOCK
                                             : NC_VISUAL_BG;

            lvds_draw_fill_rect(NC_RIGHT_PANE_X + 2, hint_y - 3, NC_RIGHT_PANE_W - 4, NC_VISUAL_ROW_H, hint_bg);
            snprintf(buf, sizeof(buf), "%c  %s", ctx->doc->selected_word >= 0 ? '>' : ' ', ctx->snapshot->selected_label);
            nc_draw_text_clip(NC_RIGHT_PANE_X + NC_LINE_TEXT_X_PAD,
                                     hint_y,
                                     buf,
                                     line_cols,
                                     NC_VISUAL_ACCENT,
                                     hint_bg,
                                     LVDS_FONT_NORMAL);
        }
        /* One background for the whole row: the bright mark wins over the
           block's pale one, and the row's own grey is what is left. The pane
           draws the mark, so a key that moves the cursor or the sender reaches
           it through the same repaint as the text. */
        row_bg = selected ? NC_VISUAL_SELECT
                          : (in_block ? NC_VISUAL_SELECT_BLOCK : NC_VISUAL_BG);
        snprintf(buf, sizeof(buf), "%3lu", (unsigned long)(ctx->snapshot->first_line + (size_t)row + 1));
        lvds_draw_text(NC_RIGHT_PANE_X + NC_LINE_NO_X_PAD, y, buf, selected ? NC_VISUAL_LINE_NO_SELECTED : NC_VISUAL_DIM,
                       row_bg,
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
                                    row_bg,
                                    NC_VISUAL_SELECT,
                                    NC_VISUAL_WORD_FG,
                                    NC_VISUAL_WORD_BG);
    }
}


/* --- the footer entries the editor owns ----------------------------------- */

/* The actions that change the document or the list it comes from: the helper's
   menus, the one-line inserts (a tool change, a spindle word, a cycle preset),
   the file list with its open/new/delete/refresh, saving, and the cursor steps.

   What a key does is the screen's to route; this answers "mine or not" so the
   screen can offer the action to the next owner. */
bool nc_editor_action(nc_editor_ctx_t *ctx, uint8_t action)
{
    switch (action) {
    case NC_FOOTER_ACTION_OPS:
    case NC_FOOTER_ACTION_TOOL_MENU:
    case NC_FOOTER_ACTION_GCODE:
    case NC_FOOTER_ACTION_G7X_MENU:
    case NC_FOOTER_ACTION_SYNC_MENU:
    case NC_FOOTER_ACTION_PECK_MENU:
        nc_editor_open_modal(ctx, action);
        return true;
    case NC_FOOTER_ACTION_TOOL_SELECT:
        nc_editor_open_field(ctx, 'T');
        return true;
    case NC_FOOTER_ACTION_FILE:
        /* Opens the folder the open file lives in, so the list can land on it;
           falls back to the NC folder when nothing is open. */
        if (ctx->doc->path[0]) {
            nc_editor_open_current_folder(ctx);
        } else {
            nc_editor_open_files(ctx, NC_FILES_DIR, true);
        }
        return true;
    case NC_FOOTER_ACTION_FILES:
        nc_editor_open_files(ctx, NULL, false);
        return true;
    case NC_FOOTER_ACTION_OPEN:
        if (nc_files_active()) {
            char path[NC_PATH_MAX];
            nc_result_t r;
            if (nc_files_selected_is_dir()) {
                if (nc_files_enter_selected()) {
                    snprintf(ctx->status, ctx->status_size, "Dir: %s", nc_files_cwd());
                } else {
                    strncpy(ctx->status, "Directory open failed", ctx->status_size - 1);
                }
                return true;
            }
            if (!nc_files_selected_path(path, sizeof(path))) {
                strncpy(ctx->status, "No NC file selected", ctx->status_size - 1);
                return true;
            }
            if (ctx->mode == NC_MODE_TOOLS &&
                !nc_state_tool_path_supported(path)) {
                strncpy(ctx->status, "TOOLS opens .t files only", ctx->status_size - 1);
                return true;
            }
            /* The buffer is about to be reused for another file: put the edits
               of the one that is open on the card first. */
            if (!nc_editor_save_current(ctx) &&
                !nc_editor_proceed_without_saving(ctx, NC_EDITOR_UNSAVED_OPEN)) {
                strncpy(ctx->status, "Save failed - press again to open", ctx->status_size - 1);
                return true;
            }
            r = nc_load_file(ctx->doc, path);
            if (r == NC_OK) {
                nc_files_set_active(false);
                nc_editor_new_file_end(ctx);
                nc_state_remember_path(ctx->mode, path);
                nc_state_save();
                snprintf(ctx->status, ctx->status_size, "Opened %s", nc_files_name(nc_files_selected()));
            } else {
                snprintf(ctx->status, ctx->status_size, "Open failed: %s", nc_result_text(r));
            }
        } else {
            nc_files_set_active(true);
            nc_editor_new_file_end(ctx);
            if (nc_files_refresh(NULL)) {
                nc_editor_serial_selected_file();
                ctx->status[0] = '\0';
            } else {
                strncpy(ctx->status, "File list unavailable", ctx->status_size - 1);
            }
        }
        return true;
    case NC_FOOTER_ACTION_G7X_Q:
        return nc_editor_insert_range_word(ctx, 'Q');
    case NC_FOOTER_ACTION_G7X_N:
        return nc_editor_insert_range_word(ctx, 'N');
    case NC_FOOTER_ACTION_NEW:
        if (nc_files_active()) {
            nc_editor_new_file_begin(ctx);
            return true;
        }
        nc_document_init(ctx->doc);
        nc_insert_line(ctx->doc, 0, "");
        strncpy(ctx->doc->path, "new.nc", sizeof(ctx->doc->path) - 1);
        nc_state_remember_path(ctx->mode, ctx->doc->path);
        nc_state_save();
        strncpy(ctx->status, "New empty NC program", ctx->status_size - 1);
        return true;
    case NC_FOOTER_ACTION_INSERT:
        if (nc_insert_line(ctx->doc, ctx->doc->cursor_line + 1, "") == NC_OK) {
            nc_cursor_down(ctx->doc);
            strncpy(ctx->status, "Inserted blank line", ctx->status_size - 1);
        } else {
            strncpy(ctx->status, "Insert failed", ctx->status_size - 1);
        }
        return true;
    case NC_FOOTER_ACTION_DELETE:
        if (nc_files_active()) {
            if (nc_files_delete_selected()) {
                strncpy(ctx->status, "File deleted", ctx->status_size - 1);
            } else {
                strncpy(ctx->status, "Delete file failed", ctx->status_size - 1);
            }
            return true;
        }
        if (nc_delete_line(ctx->doc, ctx->doc->cursor_line) == NC_OK) {
            strncpy(ctx->status, "Deleted line", ctx->status_size - 1);
        } else {
            strncpy(ctx->status, "Delete failed", ctx->status_size - 1);
        }
        return true;
    case NC_FOOTER_ACTION_BACK:
        if (nc_files_active()) {
            nc_files_select_prev();
            nc_editor_serial_selected_file();
            strncpy(ctx->status, "File up", ctx->status_size - 1);
        } else if (ctx->mode == NC_MODE_TOOLS) {
            nc_editor_move_tool_line(ctx, -1);
        } else {
            nc_editor_move_line(ctx, -1);
            nc_editor_serial_selected_line(ctx);
        }
        return true;
    case NC_FOOTER_ACTION_STEP:
        if (nc_files_active()) {
            nc_files_select_next();
            nc_editor_serial_selected_file();
            strncpy(ctx->status, "File down", ctx->status_size - 1);
        } else if (ctx->mode == NC_MODE_TOOLS) {
            nc_editor_move_tool_line(ctx, 1);
        } else {
            nc_editor_move_line(ctx, 1);
            nc_editor_serial_selected_line(ctx);
        }
        return true;
    case NC_FOOTER_ACTION_REFRESH:
        if (nc_files_refresh(NULL)) {
            nc_editor_serial_selected_file();
            snprintf(ctx->status, ctx->status_size, "Refreshed %s", nc_files_cwd());
        } else {
            strncpy(ctx->status, "Refresh failed", ctx->status_size - 1);
        }
        if (!nc_files_active()) {
            nc_files_set_active(true);
        }
        return true;
    case NC_FOOTER_ACTION_FIELD:
        if (nc_select_next_word(ctx->doc) == NC_OK) {
            strncpy(ctx->status, "Next word", ctx->status_size - 1);
        } else {
            strncpy(ctx->status, "No editable word here", ctx->status_size - 1);
        }
        return true;
    default:
        return false;
    }
}
