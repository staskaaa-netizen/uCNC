/* The 3x3 path builder - see nc_path_builder.h and docs/nc-path-builder.md.
   Where the next point is, which word is waiting for a value, and the contour
   lines that carry them. Everything else is handed to the owners: the pad is
   drawn by nc_draw_modal_items(), the word is marked and typed into by the
   editor, the cycle header and the end mark are inserted from the G7X submenu,
   the block is found by nc_g7x.c and the stock corner read by nc_preview.c. */
#include "nc_path_builder.h"

#include <stdio.h>
#include <string.h>

#include "nc_draw.h"
#include "nc_g7x.h"
#include "nc_layout.h"
#include "nc_preview.h"
#include "../g7x/g7x_contour.h"

/* The step is on `#`, because the digits 1-9 are the directions. */
static const float g_nc_path_steps[] = {
    0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 25.0f, 50.0f
};
#define NC_PATH_STEP_DEFAULT_INDEX 4u    /* 10 mm per press */

/* What the digits mean here: the key points at the direction it moves the tool
   in, the way MANUAL's pad does (`8` X-, `2` X+, `4` Z-, `6` Z+), and the
   corners are the diagonals. The panel draws this as the pad, and a shell
   beside the machine labels its own keypad from the same table.

   The X pair follows the drawing, not the word: the preview puts the centreline
   along the top of the stock and the OD at the bottom
   (`nc_preview_map_x()`), so the key pointing up takes the tool toward the
   centre - X- - and the key pointing down is X+. */
static const nc_footer_item_t g_nc_path_pad[] = {
    { '7', "Z-X-", NC_FOOTER_ACTION_NONE },
    { '8', "X-",   NC_FOOTER_ACTION_NONE },
    { '9', "Z+X-", NC_FOOTER_ACTION_NONE },
    { '4', "Z-",   NC_FOOTER_ACTION_NONE },
    { '5', "END",  NC_FOOTER_ACTION_NONE },
    { '6', "Z+",   NC_FOOTER_ACTION_NONE },
    { '1', "Z-X+", NC_FOOTER_ACTION_NONE },
    { '2', "X+",   NC_FOOTER_ACTION_NONE },
    { '3', "Z+X+", NC_FOOTER_ACTION_NONE }
};

/* The keys the pad does not carry. The strip keeps the eight slots every screen
   keeps, so the physical buttons stay under the same places. */
static const nc_footer_item_t g_nc_path_footer[] = {
    { '5', "END",    NC_FOOTER_ACTION_NONE },
    { '#', "STEP",   NC_FOOTER_ACTION_NONE },
    { '*', "UNDO",   NC_FOOTER_ACTION_NONE },
    { '0', "CANCEL", NC_FOOTER_ACTION_NONE },
    { ' ', "",       NC_FOOTER_ACTION_NONE },
    { ' ', "",       NC_FOOTER_ACTION_NONE },
    { ' ', "",       NC_FOOTER_ACTION_NONE },
    { ' ', "",       NC_FOOTER_ACTION_NONE }
};

#define NC_PATH_NO_LINE ((size_t)-1)

static bool g_nc_path_active;
static uint8_t g_nc_path_step_index = NC_PATH_STEP_DEFAULT_INDEX;

/* The current point, as the program spells it (X is the diameter the parser
   reads). `start` is the block's last point, or its stock corner if empty. */
static float g_nc_path_x;
static float g_nc_path_z;
static float g_nc_path_start_x;
static float g_nc_path_start_z;

/* The word waiting for a value. It counts as waiting only while the editor
   still has it selected, so an accept through the editor's own `#` ends the
   point as surely as `D` does. */
static size_t g_nc_path_point_line = NC_PATH_NO_LINE;
static int g_nc_path_point_word = -1;
static int g_nc_path_point_next = -1;
static char g_nc_path_point_key;

/* The block: the line its end mark sits on (contour lines go just before it),
   the last line the builder wrote, and what this session inserted - the lines
   are contiguous, so cancel takes them back with one count. */
static size_t g_nc_path_end_line = NC_PATH_NO_LINE;
static size_t g_nc_path_last_line = NC_PATH_NO_LINE;
static size_t g_nc_path_session_first = NC_PATH_NO_LINE;
static size_t g_nc_path_session_count;
static size_t g_nc_path_session_origin;
static size_t g_nc_path_written;

static float nc_path_step_value(void)
{
    return g_nc_path_steps[g_nc_path_step_index];
}

/* A program value: the shortest exact spelling, so the line reads the way the
   operator writes it - `X50`, not `X50.000`. */
static void nc_path_float_text(char *out, size_t out_sz, float value)
{
    char buf[24];
    size_t len;

    if (value > -0.0005f && value < 0.0005f) {
        value = 0.0f;                     /* never `-0` in the program */
    }
    snprintf(buf, sizeof(buf), "%.3f", (double)value);
    len = strlen(buf);
    while (len > 0u && buf[len - 1u] == '0') {
        len--;
    }
    if (len > 0u && buf[len - 1u] == '.') {
        len--;
    }
    if (len >= out_sz) {
        len = out_sz - 1u;
    }
    memcpy(out, buf, len);
    out[len] = '\0';
}

/* One program row, written the way the operator's own programs write them - no
   leading tab (the editor indents a contour row on screen) and both words on
   it, so a copied axis is never lost. */
static void nc_path_point_text(char *out, size_t out_sz,
                               const char *cmd, float x, float z)
{
    char xs[24];
    char zs[24];

    nc_path_float_text(xs, sizeof(xs), x);
    nc_path_float_text(zs, sizeof(zs), z);
    snprintf(out, out_sz, "%s X%s Z%s", cmd, xs, zs);
}

static int nc_path_word_index(const char *line, char letter)
{
    nc_word_t words[8];
    int count = nc_parse_words(line, words, 8);
    int i;

    for (i = 0; i < count; i++) {
        if (words[i].letter == letter) {
            return i;
        }
    }
    return -1;
}

/* The point a row leaves the tool at, as the preview reads it: the words that
   are there are applied, the ones that are not keep the previous value. */
static void nc_path_read_line(const char *line, float *x, float *z)
{
    float value;

    if (nc_line_word_float(line, 'X', &value)) {
        *x = value;
    }
    if (nc_line_word_float(line, 'Z', &value)) {
        *z = value;
    }
}

static void nc_path_drop_lines(nc_document_t *doc, size_t first, size_t count)
{
    while (count-- > 0u) {
        if (nc_delete_line(doc, first) != NC_OK) {
            break;
        }
    }
}

/* Where the block's contour leaves the tool, over the rows between its header
   and its end mark; `point_line` comes back as the row that carries it. False
   when the block holds no point at all. */
static bool nc_path_block_last_point(const nc_editor_ctx_t *ctx,
                                     size_t start,
                                     size_t end,
                                     float default_x,
                                     float *x,
                                     float *z,
                                     size_t *point_line)
{
    float px = default_x;
    float pz = 0.0f;
    size_t last = NC_PATH_NO_LINE;
    bool have = false;
    size_t i;

    /* A numbered P/Q block ends on N(Q), which is itself a contour row;
       G80-terminated blocks end on a non-contour row and are skipped below. */
    for (i = start + 1u; i <= end && i < ctx->doc->line_count; i++) {
        const char *line = ctx->doc->lines[i].text;
        g7x_contour_cmd_t cmd = g7x_contour_cmd_from_line(line);
        float vx;
        float vz;
        bool has_x;
        bool has_z;

        if (cmd == G7X_CONTOUR_NONE || cmd == G7X_CONTOUR_END) {
            continue;
        }
        has_x = nc_line_word_float(line, 'X', &vx);
        has_z = nc_line_word_float(line, 'Z', &vz);
        if (!has_x && !has_z) {
            continue;
        }
        if (has_x) {
            px = vx;
        }
        if (has_z) {
            pz = vz;
        }
        last = i;
        have = true;
    }
    if (have) {
        *x = px;
        *z = pz;
    }
    if (point_line) {
        *point_line = have ? last : NC_PATH_NO_LINE;
    }
    return have;
}

static bool nc_path_block_uses_mm(const nc_document_t *doc, size_t through)
{
    g7x_modal_t modal;
    size_t i;

    g7x_modal_default(&modal);
    for (i = 0u; i <= through && i < doc->line_count; i++) {
        (void)g7x_modal_apply_line(&modal,
                                   g7x_skip_line_number(doc->lines[i].text));
    }
    return modal.units == G7X_UNITS_MM;
}

static void nc_path_state_reset(void)
{
    g_nc_path_active = false;
    g_nc_path_point_line = NC_PATH_NO_LINE;
    g_nc_path_point_word = -1;
    g_nc_path_point_next = -1;
    g_nc_path_point_key = 0;
    g_nc_path_end_line = NC_PATH_NO_LINE;
    g_nc_path_last_line = NC_PATH_NO_LINE;
    g_nc_path_session_first = NC_PATH_NO_LINE;
    g_nc_path_session_count = 0u;
    g_nc_path_written = 0u;
}

static void nc_path_status_nav(nc_editor_ctx_t *ctx)
{
    char xs[24];
    char zs[24];

    nc_path_float_text(xs, sizeof(xs), g_nc_path_x);
    nc_path_float_text(zs, sizeof(zs), g_nc_path_z);
    snprintf(ctx->status, ctx->status_size,
             "PATH X%s Z%s  #STEP *UNDO 0CANCEL", xs, zs);
}

static void nc_path_status_point(nc_editor_ctx_t *ctx)
{
    char xs[24];
    char zs[24];
    float x = g_nc_path_x;
    float z = g_nc_path_z;

    /* The point being entered, off its own line: while the word is open it is
       the line that says where the tool is, not the point it moved from. */
    if (g_nc_path_point_line != NC_PATH_NO_LINE &&
        g_nc_path_point_line < ctx->doc->line_count) {
        nc_path_read_line(ctx->doc->lines[g_nc_path_point_line].text, &x, &z);
    }
    nc_path_float_text(xs, sizeof(xs), x);
    nc_path_float_text(zs, sizeof(zs), z);
    snprintf(ctx->status, ctx->status_size, "X%s Z%s  D next", xs, zs);
}

/* The pending word is done: the line is the truth, so the point is read back
   off it instead of being kept from the arithmetic that prefilled it. */
static void nc_path_settle(nc_editor_ctx_t *ctx)
{
    if (g_nc_path_point_line != NC_PATH_NO_LINE &&
        g_nc_path_point_line < ctx->doc->line_count) {
        nc_path_read_line(ctx->doc->lines[g_nc_path_point_line].text,
                          &g_nc_path_x,
                          &g_nc_path_z);
    }
    g_nc_path_point_line = NC_PATH_NO_LINE;
    g_nc_path_point_word = -1;
    g_nc_path_point_next = -1;
    g_nc_path_point_key = 0;
}

static bool nc_path_pending(const nc_editor_ctx_t *ctx)
{
    return g_nc_path_point_line != NC_PATH_NO_LINE &&
           g_nc_path_point_line < ctx->doc->line_count &&
           ctx->doc->cursor_line == g_nc_path_point_line &&
           ctx->doc->selected_word == g_nc_path_point_word;
}

/* Open only on a closed cycle block. The G7X submenu owns creating its header
   and G80; this pad owns only the contour between them. */
static bool nc_path_builder_begin(nc_editor_ctx_t *ctx)
{
    size_t block_start = 0u;
    size_t block_end = 0u;
    size_t point_line = NC_PATH_NO_LINE;
    size_t cursor;
    float profile_x;
    bool inside;
    bool have_point = false;

    nc_path_state_reset();
    if (ctx->doc->line_count == 0u) {
        ctx->doc->cursor_line = 0u;
    }
    cursor = ctx->doc->cursor_line;
    if (cursor >= ctx->doc->line_count && ctx->doc->line_count > 0u) {
        cursor = ctx->doc->line_count - 1u;
    }
    g_nc_path_session_origin = cursor;
    {
        nc_preview_info_t preview;
        nc_preview_collect(ctx->doc, &preview);
        profile_x = preview.stock_x;
    }

    inside = nc_g7x_block_containing(ctx->doc, cursor, &block_start, &block_end);
    if (inside) {
        if (!nc_path_block_uses_mm(ctx->doc, block_end)) {
            strncpy(ctx->status, "PATH steps use mm; switch to G21", ctx->status_size - 1);
            *ctx->dirty = true;
            return false;
        }
        /* Carry on from the last point of that block, not from the stock
           corner, and append at the end of its contour - not where the cursor
           happens to sit inside it. */
        g_nc_path_end_line = block_end;
        g_nc_path_session_first = block_end;
        g_nc_path_session_count = 0u;
        have_point = nc_path_block_last_point(ctx, block_start, block_end,
                                              profile_x,
                                              &g_nc_path_x, &g_nc_path_z,
                                              &point_line);
        g_nc_path_last_line = point_line;
    } else {
        strncpy(ctx->status, "Select a closed G71/G72 block", ctx->status_size - 1);
        *ctx->dirty = true;
        return false;
    }
    if (!have_point) {
        /* The profile's own start: the face at the stock OD. */
        g_nc_path_x = profile_x;
        g_nc_path_z = 0.0f;
    }
    g_nc_path_start_x = g_nc_path_x;
    g_nc_path_start_z = g_nc_path_z;
    g_nc_path_active = true;
    if (g_nc_path_last_line != NC_PATH_NO_LINE) {
        ctx->doc->cursor_line = g_nc_path_last_line;
    }
    ctx->doc->selected_word = -1;
    nc_editor_clear_draft();
    nc_path_status_nav(ctx);
    *ctx->dirty = true;
    return true;
}

/* One pad press: append the contour row the key points at and open the word
   that has to be entered on it. */
static bool nc_path_builder_point(nc_editor_ctx_t *ctx, char digit)
{
    int x_dir = 0;
    int z_dir = 0;
    float step = nc_path_step_value();
    float x;
    float z;
    char text[NC_MAX_LINE_LEN];
    size_t at;
    int x_word;
    int z_word;

    switch (digit) {
    /* The X sign is the pad's own, not the word's: up is the smaller diameter
       (see the table above). */
    case '8': x_dir = -1; break;
    case '2': x_dir = +1; break;
    case '6': z_dir = +1; break;
    case '4': z_dir = -1; break;
    case '7': z_dir = -1; x_dir = -1; break;
    case '9': z_dir = +1; x_dir = -1; break;
    case '1': z_dir = -1; x_dir = +1; break;
    case '3': z_dir = +1; x_dir = +1; break;
    default: return false;
    }

    /* The axis that does not move is copied at its current value; the one that
       does is prefilled one step in the direction the key points. */
    x = x_dir ? g_nc_path_x + (float)x_dir * step : g_nc_path_x;
    z = z_dir ? g_nc_path_z + (float)z_dir * step : g_nc_path_z;
    nc_path_point_text(text, sizeof(text), "G1", x, z);

    at = g_nc_path_end_line;
    if (at == NC_PATH_NO_LINE || at > ctx->doc->line_count) {
        at = ctx->doc->line_count;
    }
    if (nc_insert_line(ctx->doc, at, text) != NC_OK) {
        strncpy(ctx->status, "No room for the contour line", ctx->status_size - 1);
        *ctx->dirty = true;
        return true;
    }
    ctx->doc->cursor_line = at;
    g_nc_path_end_line = at + 1u;
    g_nc_path_last_line = at;
    if (g_nc_path_session_count == 0u) {
        g_nc_path_session_first = at;
    }
    g_nc_path_session_count++;
    g_nc_path_written++;

    /* Mark what the operator has to type: the axis that moves (both of them for
       a diagonal), and nothing for a copied axis. */
    x_word = nc_path_word_index(text, 'X');
    z_word = nc_path_word_index(text, 'Z');
    if (x_dir && z_dir) {
        g_nc_path_point_word = x_word;
        g_nc_path_point_next = z_word;
    } else if (z_dir) {
        g_nc_path_point_word = z_word;
        g_nc_path_point_next = -1;
    } else {
        g_nc_path_point_word = x_word;
        g_nc_path_point_next = -1;
    }
    g_nc_path_point_line = at;
    g_nc_path_point_key = digit;
    ctx->doc->selected_word = g_nc_path_point_word;
    nc_editor_clear_draft();
    nc_path_status_point(ctx);
    *ctx->dirty = true;
    return true;
}

/* The point is refused before it is entered: its line goes, the previous point
   stands, and the builder is back to the pad. */
static void nc_path_point_drop(nc_editor_ctx_t *ctx)
{
    size_t line = g_nc_path_point_line;

    if (line == NC_PATH_NO_LINE || line >= ctx->doc->line_count) {
        return;
    }
    nc_delete_line(ctx->doc, line);
    g_nc_path_end_line--;
    g_nc_path_session_count--;
    g_nc_path_written--;
    g_nc_path_point_line = NC_PATH_NO_LINE;
    g_nc_path_point_word = -1;
    g_nc_path_point_next = -1;
    g_nc_path_point_key = 0;
    if (g_nc_path_written > 0u) {
        g_nc_path_last_line = line - 1u;
        ctx->doc->cursor_line = g_nc_path_last_line;
    } else {
        g_nc_path_last_line = NC_PATH_NO_LINE;
        ctx->doc->cursor_line = g_nc_path_session_origin;
    }
    ctx->doc->selected_word = -1;
    nc_editor_clear_draft();
    strncpy(ctx->status, "Point dropped", ctx->status_size - 1);
    *ctx->dirty = true;
}

/* Undo: drop the last point the builder wrote and stand on the one before it. */
static void nc_path_undo(nc_editor_ctx_t *ctx)
{
    size_t line = g_nc_path_last_line;

    if (g_nc_path_written == 0u ||
        line == NC_PATH_NO_LINE ||
        line >= ctx->doc->line_count) {
        strncpy(ctx->status, "Nothing to undo", ctx->status_size - 1);
        *ctx->dirty = true;
        return;
    }
    nc_delete_line(ctx->doc, line);
    g_nc_path_end_line--;
    g_nc_path_session_count--;
    g_nc_path_written--;
    if (g_nc_path_written > 0u) {
        g_nc_path_last_line = line - 1u;
        ctx->doc->cursor_line = g_nc_path_last_line;
        nc_path_read_line(ctx->doc->lines[g_nc_path_last_line].text,
                          &g_nc_path_x, &g_nc_path_z);
    } else {
        g_nc_path_last_line = NC_PATH_NO_LINE;
        ctx->doc->cursor_line = g_nc_path_session_origin;
        g_nc_path_x = g_nc_path_start_x;
        g_nc_path_z = g_nc_path_start_z;
    }
    ctx->doc->selected_word = -1;
    nc_editor_clear_draft();
    nc_path_status_nav(ctx);
    *ctx->dirty = true;
}

/* `#`: the next value in the builder's table. One key has to reach every value,
   so it wraps where MANUAL's two keys stop. */
static void nc_path_step_next(nc_editor_ctx_t *ctx)
{
    char text[24];

    g_nc_path_step_index = (uint8_t)((g_nc_path_step_index + 1u) %
                                     (sizeof(g_nc_path_steps) /
                                      sizeof(g_nc_path_steps[0])));
    nc_path_float_text(text, sizeof(text), nc_path_step_value());
    snprintf(ctx->status, ctx->status_size, "PATH step %s mm", text);
    *ctx->dirty = true;
}

/* Cancel: every line the session inserted goes, and the document is as it was. */
static void nc_path_cancel(nc_editor_ctx_t *ctx)
{
    if (g_nc_path_session_count > 0u &&
        g_nc_path_session_first != NC_PATH_NO_LINE) {
        nc_path_drop_lines(ctx->doc, g_nc_path_session_first, g_nc_path_session_count);
    }
    nc_path_state_reset();
    if (ctx->doc->line_count == 0u) {
        ctx->doc->cursor_line = 0u;
    } else if (g_nc_path_session_origin >= ctx->doc->line_count) {
        ctx->doc->cursor_line = ctx->doc->line_count - 1u;
    } else {
        ctx->doc->cursor_line = g_nc_path_session_origin;
    }
    ctx->doc->selected_word = -1;
    nc_editor_clear_draft();
    strncpy(ctx->status, "Path cancelled", ctx->status_size - 1);
    *ctx->dirty = true;
}

/* `5`: the drawing is finished. Nothing is written - the contour is the
   geometry of the part and nothing else, and the cycle owns its approach and
   retract (the `R` word and the profile range), so the tool's own position when
   the cycle starts is the program's business, not the builder's. A rapid out to
   the clearance corner used to be written here; it is not part of a finished
   profile, and it reverses the profile's direction, which the generator refuses
   (`g7x_stream_prepare()`: monotonic X and Z - the block emitted
   `unsupported`). The block's own `G80` is already after the last row. */
static void nc_path_close(nc_editor_ctx_t *ctx)
{
    nc_path_state_reset();
    ctx->doc->selected_word = -1;
    nc_editor_clear_draft();
    strncpy(ctx->status, "Path finished - G80 ends it", ctx->status_size - 1);
    *ctx->dirty = true;
}

/* --- what the screen calls ------------------------------------------------ */

bool nc_path_builder_active(void)
{
    return g_nc_path_active;
}

void nc_path_builder_leave(void)
{
    nc_path_state_reset();
}

bool nc_path_builder_key(nc_editor_ctx_t *ctx, nc_visual_key_t key, char ch)
{
    if (!g_nc_path_active || !ctx || !ctx->doc || !ctx->status || !ctx->dirty) {
        return false;
    }
    /* Every key the builder acts on is a character of the keypad; the named
       keys (the mode key, the shell's cancel) belong to the screen. */
    (void)key;
    if (g_nc_path_point_line != NC_PATH_NO_LINE && !nc_path_pending(ctx)) {
        /* An accept through the editor's own keys (`#`, or a field walk that
           left the word) has finished the point already. */
        nc_path_settle(ctx);
        nc_path_status_nav(ctx);
        *ctx->dirty = true;
    }
    if (nc_path_pending(ctx)) {
        if (ch == 'D') {
            if (g_nc_path_point_next >= 0) {
                int next = g_nc_path_point_next;

                ctx->doc->selected_word = next;
                g_nc_path_point_word = next;
                g_nc_path_point_next = -1;
                nc_editor_clear_draft();
                nc_path_status_point(ctx);
                *ctx->dirty = true;
                return true;
            }
            ctx->doc->selected_word = -1;
            nc_editor_clear_draft();
            nc_path_settle(ctx);
            nc_path_status_nav(ctx);
            *ctx->dirty = true;
            return true;
        }
        /* `*` is the panel's delete key, so it is the one that takes this row
           back - the point is refused and its line goes. `A` is not touched:
           it is the screen's cancel/mode key, and it leaves the builder with
           what is written (the screen change saves the document). `0` is a
           digit while a word is open, because `30` and `0` have to be
           enterable. */
        if (ch == '*') {
            nc_path_point_drop(ctx);
            return true;
        }
        /* The digits, the sign, the point and `#` are the editor's: one field
           flow, not two. */
        return false;
    }
    if (ch == '5') {
        /* End of the drawing: the builder is done and nothing is written. The
           other digits are the directions. */
        nc_path_close(ctx);
        return true;
    }
    if (ch >= '1' && ch <= '9') {
        return nc_path_builder_point(ctx, ch);
    }
    if (ch == '#') {
        nc_path_step_next(ctx);
        return true;
    }
    if (ch == '*') {
        nc_path_undo(ctx);
        return true;
    }
    if (ch == '0') {
        nc_path_cancel(ctx);
        return true;
    }
    if (ch == 'D') {
        strncpy(ctx->status, "Nothing waiting here", ctx->status_size - 1);
        *ctx->dirty = true;
        return true;
    }
    /* Anything else - `A` included - is the screen's: the mode key leaves the
       builder and the document keeps the lines, the way leaving any screen
       does. Only `0` takes the session back. */
    return false;
}

bool nc_path_builder_action(nc_editor_ctx_t *ctx, uint8_t action)
{
    if (action != NC_FOOTER_ACTION_BUILD) {
        return false;
    }
    if (!ctx || !ctx->doc || !ctx->status || !ctx->dirty) {
        return true;
    }
    if (!ctx->editable || ctx->mode != NC_MODE_PROGRAM) {
        strncpy(ctx->status, "Path builder needs EDIT", ctx->status_size - 1);
        *ctx->dirty = true;
        return true;
    }
    (void)nc_path_builder_begin(ctx);
    return true;
}

const char *nc_path_builder_key_hint(char key)
{
    size_t i;

    if (!g_nc_path_active) {
        return 0;
    }
    for (i = 0u; i < sizeof(g_nc_path_pad) / sizeof(g_nc_path_pad[0]); i++) {
        if (g_nc_path_pad[i].key == key) {
            return g_nc_path_pad[i].label;
        }
    }
    for (i = 0u; i < sizeof(g_nc_path_footer) / sizeof(g_nc_path_footer[0]); i++) {
        if (g_nc_path_footer[i].key == key) {
            return g_nc_path_footer[i].label;
        }
    }
    return 0;
}

const nc_footer_item_t *nc_path_builder_footer(size_t *count)
{
    if (!count) {
        return 0;
    }
    *count = sizeof(g_nc_path_footer) / sizeof(g_nc_path_footer[0]);
    return g_nc_path_footer;
}

void nc_path_builder_draw(const nc_editor_ctx_t *ctx)
{
    int visible;
    int keypad_y;
    int modal_x;
    uint16_t lit = 0u;

    if (!g_nc_path_active) {
        return;
    }
    visible = (ctx && ctx->snapshot) ? ctx->snapshot->cursor_visible_index : -1;
    if (visible < 0) {
        visible = 0;
    }
    /* The same place the floating helper hangs: below the line the builder is
       working on, on the right, pinned so the keys end at the pane bottom. */
    keypad_y = NC_CODE_Y + (visible + 1) * NC_VISUAL_ROW_H - NC_MODAL_PAD;
    keypad_y = nc_draw_clampi(keypad_y, NC_CODE_Y,
                              NC_PANE_BOTTOM - NC_MODAL_PAD -
                              NC_MODAL_KEY_H * NC_MODAL_ROWS);
    modal_x = NC_RIGHT_PANE_X + NC_RIGHT_PANE_W - NC_MODAL_W - 8;
    if (g_nc_path_point_key >= '1' && g_nc_path_point_key <= '9') {
        lit = (uint16_t)(1u << (g_nc_path_point_key - '0'));
    }
    nc_draw_modal_items(modal_x, keypad_y,
                        g_nc_path_pad,
                        sizeof(g_nc_path_pad) / sizeof(g_nc_path_pad[0]),
                        lit);
}
