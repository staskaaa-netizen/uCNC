#include "nc_text.h"

#include <stdio.h>
#include <string.h>

void nc_text_draw_line_with_word(const char *line,
                                 int x,
                                 int y,
                                 int cols,
                                 int char_w,
                                 int row_h,
                                 int word_start,
                                 int word_end,
                                 bool selected,
                                 lvds_color_t text_fg,
                                 lvds_color_t dim_fg,
                                 lvds_color_t bg,
                                 lvds_color_t selected_bg,
                                 lvds_color_t word_fg,
                                 lvds_color_t word_bg)
{
    lvds_color_t line_bg = selected ? selected_bg : bg;
    char expanded[NC_MAX_LINE_LEN + 16];
    char selected_text[NC_MAX_LINE_LEN + 16];
    int src = 0;
    int dst = 0;
    int display_word_start = word_start;
    int display_word_end = word_end;
    int len;
    int clipped_start;
    int clipped_end;
    int clipped_cols;

    if (!line) {
        line = "";
    }
    while (line[src] && dst + 1 < (int)sizeof(expanded)) {
        if (line[src] == '\t') {
            int spaces = 4 - (dst & 3);
            int extra = spaces - 1;
            int i;
            if (word_start > src) display_word_start += extra;
            if (word_end > src) display_word_end += extra;
            for (i = 0; i < spaces && dst + 1 < (int)sizeof(expanded); i++) {
                expanded[dst++] = ' ';
            }
            src++;
        } else {
            expanded[dst++] = line[src++];
        }
    }
    expanded[dst] = '\0';
    len = dst;

    lvds_draw_text_clip(x, y, expanded, cols, selected ? text_fg : dim_fg, line_bg, LVDS_FONT_NORMAL);

    if (!selected || display_word_start < 0 || display_word_end <= display_word_start || display_word_start >= cols) {
        return;
    }

    clipped_start = display_word_start;
    clipped_end = display_word_end;
    if (clipped_end > cols) {
        clipped_end = cols;
    }
    if (clipped_start >= len) {
        return;
    }
    clipped_cols = clipped_end - clipped_start;
    if (clipped_cols <= 0) {
        return;
    }
    if (clipped_cols >= (int)sizeof(selected_text)) {
        clipped_cols = (int)sizeof(selected_text) - 1;
    }
    memcpy(selected_text, expanded + clipped_start, (size_t)clipped_cols);
    selected_text[clipped_cols] = '\0';

    lvds_draw_fill_rect(x + clipped_start * char_w,
                        y - 2,
                        clipped_cols * char_w,
                        row_h - 4,
                        word_bg);
    lvds_draw_text_clip(x + clipped_start * char_w,
                        y,
                        selected_text,
                        clipped_cols,
                        word_fg,
                        word_bg,
                        LVDS_FONT_NORMAL);
}

void nc_text_edit_clear(nc_text_edit_t *edit)
{
    if (!edit) {
        return;
    }
    edit->active = false;
    edit->buf[0] = '\0';
    edit->line = 0;
    edit->word = -1;
}

static bool nc_text_selected_value_text(const nc_document_t *doc, char *out, size_t out_sz)
{
    nc_word_t word;
    const char *line;
    size_t len;

    if (!doc || !out || out_sz == 0) {
        return false;
    }
    out[0] = '\0';
    if (nc_get_selected_word(doc, &word) != NC_OK ||
        doc->cursor_line >= doc->line_count) {
        return false;
    }

    line = doc->lines[doc->cursor_line].text;
    len = (size_t)(word.end - (word.start + 1));
    if (len >= out_sz) {
        len = out_sz - 1;
    }
    memcpy(out, line + word.start + 1, len);
    out[len] = '\0';
    return true;
}

static void nc_text_edit_begin(nc_document_t *doc, nc_text_edit_t *edit, bool preserve_negative)
{
    char current[24];

    if (!doc || !edit) {
        return;
    }
    if (edit->active &&
        edit->line == doc->cursor_line &&
        edit->word == doc->selected_word) {
        return;
    }

    nc_text_edit_clear(edit);
    edit->active = true;
    edit->line = doc->cursor_line;
    edit->word = doc->selected_word;

    if (preserve_negative &&
        nc_text_selected_value_text(doc, current, sizeof(current)) &&
        current[0] == '-') {
        strncpy(edit->buf, "-", sizeof(edit->buf) - 1);
        edit->buf[sizeof(edit->buf) - 1] = '\0';
    }
}

static void nc_text_edit_apply(nc_document_t *doc, nc_text_edit_t *edit)
{
    char apply[24];

    if (!doc || !edit || !edit->active) {
        return;
    }

    if (edit->buf[0] == '\0') {
        strncpy(apply, "0", sizeof(apply) - 1);
    } else if (strcmp(edit->buf, "-") == 0) {
        strncpy(apply, "-0", sizeof(apply) - 1);
    } else if (strcmp(edit->buf, ".") == 0) {
        strncpy(apply, "0.", sizeof(apply) - 1);
    } else if (strcmp(edit->buf, "-.") == 0) {
        strncpy(apply, "-0.", sizeof(apply) - 1);
    } else {
        strncpy(apply, edit->buf, sizeof(apply) - 1);
    }
    apply[sizeof(apply) - 1] = '\0';
    (void)nc_set_selected_word_text(doc, apply);
}

static void nc_text_edit_digit(nc_document_t *doc, nc_text_edit_t *edit, char digit)
{
    size_t len;

    nc_text_edit_begin(doc, edit, true);
    len = strlen(edit->buf);
    if (len + 1u >= sizeof(edit->buf)) {
        return;
    }
    edit->buf[len] = digit;
    edit->buf[len + 1u] = '\0';
    nc_text_edit_apply(doc, edit);
}

static void nc_text_edit_toggle_sign(nc_document_t *doc, nc_text_edit_t *edit)
{
    size_t len;

    nc_text_edit_begin(doc, edit, false);
    if (edit->buf[0] == '-') {
        memmove(edit->buf, edit->buf + 1, strlen(edit->buf));
    } else {
        len = strlen(edit->buf);
        if (len + 1u >= sizeof(edit->buf)) {
            return;
        }
        memmove(edit->buf + 1, edit->buf, len + 1u);
        edit->buf[0] = '-';
    }
    nc_text_edit_apply(doc, edit);
}

static void nc_text_edit_dot(nc_document_t *doc, nc_text_edit_t *edit)
{
    size_t len;

    nc_text_edit_begin(doc, edit, false);
    if (strchr(edit->buf, '.')) {
        return;
    }
    len = strlen(edit->buf);
    if (len + 1u >= sizeof(edit->buf)) {
        return;
    }
    if (len == 0u) {
        strncpy(edit->buf, "0.", sizeof(edit->buf) - 1);
    } else if (strcmp(edit->buf, "-") == 0) {
        strncpy(edit->buf, "-0.", sizeof(edit->buf) - 1);
    } else {
        edit->buf[len] = '.';
        edit->buf[len + 1u] = '\0';
    }
    edit->buf[sizeof(edit->buf) - 1] = '\0';
    nc_text_edit_apply(doc, edit);
}

static void nc_text_edit_backspace(nc_document_t *doc, nc_text_edit_t *edit)
{
    size_t len;

    nc_text_edit_begin(doc, edit, false);
    len = strlen(edit->buf);
    if (len > 0u) {
        edit->buf[len - 1u] = '\0';
        nc_text_edit_apply(doc, edit);
    }
}

bool nc_text_edit_handle_key(nc_document_t *doc,
                             nc_text_edit_t *edit,
                             char key,
                             char *status,
                             size_t status_sz)
{
    nc_word_t word;

    if (!doc || !edit || doc->selected_word < 0) {
        return false;
    }

    if (key >= '0' && key <= '9') {
        nc_text_edit_digit(doc, edit, key);
    } else if (key == 'B') {
        nc_text_edit_toggle_sign(doc, edit);
    } else if (key == 'C') {
        nc_text_edit_dot(doc, edit);
    } else if (key == '*') {
        nc_text_edit_backspace(doc, edit);
    } else if (key == 'D') {
        int old_word = doc->selected_word;
        nc_text_edit_clear(edit);
        if (nc_select_next_word(doc) == NC_OK && doc->selected_word > old_word) {
            if (status && status_sz) {
                strncpy(status, "Next word", status_sz - 1);
                status[status_sz - 1] = '\0';
            }
        } else {
            doc->selected_word = -1;
            if (status && status_sz) {
                strncpy(status, "Edit accepted", status_sz - 1);
                status[status_sz - 1] = '\0';
            }
        }
        return true;
    } else if (key == '#') {
        nc_text_edit_clear(edit);
        doc->selected_word = -1;
        if (status && status_sz) {
            strncpy(status, "Edit accepted", status_sz - 1);
            status[status_sz - 1] = '\0';
        }
        return true;
    } else {
        return false;
    }

    if (status && status_sz) {
        if (nc_get_selected_word(doc, &word) == NC_OK) {
            snprintf(status,
                     status_sz,
                     "%c=%s",
                     word.letter,
                     edit->buf[0] ? edit->buf : "0");
        } else {
            strncpy(status, "Edit failed", status_sz - 1);
            status[status_sz - 1] = '\0';
        }
    }
    return true;
}

const char *nc_text_edit_buffer(const nc_text_edit_t *edit)
{
    return (edit && edit->buf[0]) ? edit->buf : "0";
}

bool nc_text_edit_active(const nc_text_edit_t *edit)
{
    return edit && edit->active;
}
