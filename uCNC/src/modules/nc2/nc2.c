#include "nc2.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void nc2_document_init(nc2_document_t *doc)
{
    if (!doc) {
        return;
    }
    memset(doc, 0, sizeof(*doc));
    doc->cursor = 0u;
    doc->field = -1;
    doc->helper_line = (size_t)-1;
    doc->helper_origin = 0u;
    doc->lines[0][0] = '\0';
}

/* --- fields --------------------------------------------------------------- */

/* One letter and the number written after it. The value may be empty, and the
   letters are taken as they come: `G1X30` is three fields and so is `X45.2`,
   because that is all a field has to be for a value to be typed at it. Text in
   `( ... )` is a comment - the operator's words, not a field - and is skipped
   whole, closing bracket or not. */
int nc2_fields(const char *line, nc2_field_t *out, int max)
{
    int count = 0;
    const char *p = line ? line : "";

    while (*p) {
        if (*p == '(') {
            while (*p && *p != ')') {
                p++;
            }
            if (*p) {
                p++;
            }
            continue;
        }
        if (isalpha((unsigned char)*p)) {
            const char *letter = p;

            p++;
            if (count < max && out) {
                out[count].letter = (char)toupper((unsigned char)*letter);
                out[count].start = (size_t)(letter - line);
                out[count].value = (size_t)(p - line);
            }
            while (*p && (isdigit((unsigned char)*p) || *p == '-' ||
                          *p == '+' || *p == '.')) {
                p++;
            }
            if (count < max && out) {
                out[count].end = (size_t)(p - line);
            }
            count++;
            continue;
        }
        p++;
    }
    return count;
}

int nc2_document_fields(const nc2_document_t *doc, nc2_field_t *out, int max)
{
    if (!doc || doc->cursor >= doc->line_count) {
        return 0;
    }
    return nc2_fields(doc->lines[doc->cursor], out, max);
}

/* --- lines ---------------------------------------------------------------- */

bool nc2_insert_line(nc2_document_t *doc, size_t at, const char *text)
{
    size_t i;
    bool below;

    if (!doc || !text || strlen(text) >= NC2_MAX_LINE_LEN ||
        doc->line_count >= NC2_MAX_LINES) {
        return false;
    }
    /* A line inserted at or above the cursor pushes it down, so the operator
       stays on the line they were reading. The first line of an empty document
       is the exception: there is nothing to stay on. */
    below = doc->line_count > 0u && doc->cursor >= at;
    if (at > doc->line_count) {
        at = doc->line_count;
    }
    for (i = doc->line_count; i > at; i--) {
        memcpy(doc->lines[i], doc->lines[i - 1u], NC2_MAX_LINE_LEN);
    }
    snprintf(doc->lines[at], NC2_MAX_LINE_LEN, "%s", text);
    doc->line_count++;
    if (below) {
        doc->cursor++;
    }
    doc->dirty = true;
    return true;
}

bool nc2_set_line(nc2_document_t *doc, size_t at, const char *text)
{
    if (!doc || !text || at >= doc->line_count ||
        strlen(text) >= NC2_MAX_LINE_LEN) {
        return false;
    }
    snprintf(doc->lines[at], NC2_MAX_LINE_LEN, "%s", text);
    doc->dirty = true;
    return true;
}

bool nc2_delete_line(nc2_document_t *doc, size_t at)
{
    size_t i;

    if (!doc || at >= doc->line_count) {
        return false;
    }
    for (i = at; i + 1u < doc->line_count; i++) {
        memcpy(doc->lines[i], doc->lines[i + 1u], NC2_MAX_LINE_LEN);
    }
    doc->line_count--;
    doc->lines[doc->line_count][0] = '\0';
    if (doc->cursor >= doc->line_count && doc->cursor > 0u) {
        doc->cursor--;
    }
    nc2_unpick(doc);
    doc->dirty = true;
    return true;
}

void nc2_cursor_move(nc2_document_t *doc, int delta)
{
    long at;

    if (!doc || doc->line_count == 0u) {
        return;
    }
    at = (long)doc->cursor + delta;
    if (at < 0) {
        at = 0;
    }
    if (at >= (long)doc->line_count) {
        at = (long)doc->line_count - 1;
    }
    if ((size_t)at != doc->cursor) {
        doc->cursor = (size_t)at;
        nc2_unpick(doc);
    }
}

/* --- the picked field and the value typed at it --------------------------- */

bool nc2_pick_field(nc2_document_t *doc, int index)
{
    nc2_field_t fields[NC2_MAX_FIELDS];
    int count;

    if (!doc || index < 0) {
        return false;
    }
    count = nc2_document_fields(doc, fields, NC2_MAX_FIELDS);
    if (index >= count) {
        return false;
    }
    doc->field = index;
    doc->draft[0] = '\0';
    doc->drafting = false;
    return true;
}

void nc2_unpick(nc2_document_t *doc)
{
    if (doc) {
        doc->field = -1;
        doc->draft[0] = '\0';
        doc->drafting = false;
    }
}

/* Put a value where the picked field's was: the rest of the line is carried
   over untouched, so what the operator did not type is not rewritten. */
static bool nc2_field_apply(nc2_document_t *doc, const char *value)
{
    nc2_field_t fields[NC2_MAX_FIELDS];
    char line[NC2_MAX_LINE_LEN];
    int count;
    int n;

    if (!doc || doc->field < 0 || doc->cursor >= doc->line_count) {
        return false;
    }
    count = nc2_fields(doc->lines[doc->cursor], fields, NC2_MAX_FIELDS);
    if (doc->field >= count) {
        return false;
    }
    n = snprintf(line, sizeof(line), "%.*s%s%s",
                 (int)fields[doc->field].value,
                 doc->lines[doc->cursor],
                 value,
                 doc->lines[doc->cursor] + fields[doc->field].end);
    if (n <= 0 || (size_t)n >= sizeof(line)) {
        return false;
    }
    return nc2_set_line(doc, doc->cursor, line);
}

static bool nc2_type_digit(nc2_document_t *doc, char digit)
{
    nc2_field_t fields[NC2_MAX_FIELDS];
    int count;

    if (!doc || doc->field < 0) {
        return false;
    }
    count = nc2_document_fields(doc, fields, NC2_MAX_FIELDS);
    if (doc->field >= count) {
        return false;
    }
    if (!doc->drafting) {
        /* The first digit typed replaces the value that was there - a prefilled
           number is a starting point, not a prefix to be extended. */
        doc->draft[0] = '\0';
        if (fields[doc->field].value < fields[doc->field].end &&
            doc->lines[doc->cursor][fields[doc->field].value] == '-') {
            doc->draft[0] = '-';
            doc->draft[1] = '\0';
        }
        doc->drafting = true;
    }
    if (strlen(doc->draft) + 1u >= sizeof(doc->draft)) {
        return true;                /* full: the keystroke is simply not added */
    }
    strncat(doc->draft, &digit, 1u);
    return nc2_field_apply(doc, doc->draft);
}

/* The sign, on a keypad that has no `-`: it toggles, and it starts from the
   value that is there so `30` becomes `-30` before anything is typed. */
static bool nc2_type_sign(nc2_document_t *doc)
{
    nc2_field_t fields[NC2_MAX_FIELDS];
    int count;

    if (!doc || doc->field < 0) {
        return false;
    }
    count = nc2_document_fields(doc, fields, NC2_MAX_FIELDS);
    if (doc->field >= count) {
        return false;
    }
    if (!doc->drafting) {
        size_t i;
        size_t n = fields[doc->field].end - fields[doc->field].value;

        if (n >= sizeof(doc->draft)) {
            n = sizeof(doc->draft) - 1u;
        }
        for (i = 0u; i < n; i++) {
            doc->draft[i] = doc->lines[doc->cursor][fields[doc->field].value + i];
        }
        doc->draft[n] = '\0';
        doc->drafting = true;
    }
    if (doc->draft[0] == '-') {
        memmove(doc->draft, doc->draft + 1, strlen(doc->draft));
    } else {
        size_t len = strlen(doc->draft);

        if (len + 1u >= sizeof(doc->draft)) {
            return true;
        }
        memmove(doc->draft + 1, doc->draft, len + 1u);
        doc->draft[0] = '-';
    }
    return nc2_field_apply(doc, doc->draft);
}

/* The point, on the same keypad: one per value. */
static bool nc2_type_point(nc2_document_t *doc)
{
    nc2_field_t fields[NC2_MAX_FIELDS];
    int count;

    if (!doc || doc->field < 0) {
        return false;
    }
    count = nc2_document_fields(doc, fields, NC2_MAX_FIELDS);
    if (doc->field >= count) {
        return false;
    }
    if (!doc->drafting) {
        size_t i;
        size_t n = fields[doc->field].end - fields[doc->field].value;

        if (n >= sizeof(doc->draft)) {
            n = sizeof(doc->draft) - 1u;
        }
        for (i = 0u; i < n; i++) {
            doc->draft[i] = doc->lines[doc->cursor][fields[doc->field].value + i];
        }
        doc->draft[n] = '\0';
        doc->drafting = true;
    }
    if (!strchr(doc->draft, '.') &&
        strlen(doc->draft) + 1u < sizeof(doc->draft)) {
        strcat(doc->draft, ".");
    }
    return nc2_field_apply(doc, doc->draft);
}

static bool nc2_type_backspace(nc2_document_t *doc)
{
    size_t len;

    if (!doc || doc->field < 0 || !doc->drafting) {
        return false;
    }
    len = strlen(doc->draft);
    if (len == 0u) {
        return false;
    }
    doc->draft[len - 1u] = '\0';
    return nc2_field_apply(doc, doc->draft);
}

/* --- the keys ------------------------------------------------------------- */

bool nc2_key(nc2_document_t *doc, nc2_key_t key, char ch)
{
    nc2_field_t fields[NC2_MAX_FIELDS];
    int count;

    if (!doc) {
        return false;
    }
    if (doc->field >= 0) {
        /* A value is picked: the keys are the value's. */
        switch (key) {
        case NC2_KEY_DIGIT:
            return nc2_type_digit(doc, ch);
        case NC2_KEY_UP:
            return nc2_type_sign(doc);
        case NC2_KEY_DOWN:
            return nc2_type_point(doc);
        case NC2_KEY_ACCEPT:
            nc2_unpick(doc);
            return true;
        case NC2_KEY_DELETE:
            if (nc2_type_backspace(doc)) {
                return true;
            }
            nc2_unpick(doc);
            return true;
        case NC2_KEY_NEXT:
            count = nc2_document_fields(doc, fields, NC2_MAX_FIELDS);
            if (doc->field + 1 < count) {
                return nc2_pick_field(doc, doc->field + 1);
            }
            nc2_unpick(doc);
            return true;
        default:
            return false;
        }
    }
    switch (key) {
    case NC2_KEY_UP:
        nc2_cursor_move(doc, -1);
        return true;
    case NC2_KEY_DOWN:
        nc2_cursor_move(doc, 1);
        return true;
    case NC2_KEY_NEXT:
    case NC2_KEY_ACCEPT:
        /* Into the line: the first field is the one the operator means when
           they press the key with nothing picked. */
        return nc2_pick_field(doc, 0);
    case NC2_KEY_DELETE:
        return nc2_delete_line(doc, doc->cursor);
    default:
        /* The digits are the pad's while nothing is picked, and `0`/`A`/the
           modes belong to the screen. */
        return false;
    }
}

/* --- the pad's helper ----------------------------------------------------- */

bool nc2_helper_active(const nc2_document_t *doc)
{
    return doc && doc->helper_open;
}

bool nc2_helper_open(nc2_document_t *doc, const char *name)
{
    size_t origin;
    size_t at;

    if (!doc || !name) {
        return false;
    }
    nc2_helper_cancel(doc);
    nc2_unpick(doc);
    if (doc->line_count == 0u) {
        origin = 0u;
        at = 0u;
    } else {
        origin = doc->cursor;
        at = doc->cursor + 1u;
    }
    if (!nc2_insert_line(doc, at, name)) {
        return false;
    }
    doc->cursor = at;
    doc->helper_origin = origin;
    doc->helper_line = at;
    doc->helper_open = true;
    return true;
}

void nc2_helper_cancel(nc2_document_t *doc)
{
    if (!doc || !doc->helper_open) {
        return;
    }
    if (doc->helper_line < doc->line_count) {
        (void)nc2_delete_line(doc, doc->helper_line);
    }
    if (doc->helper_origin < doc->line_count) {
        doc->cursor = doc->helper_origin;
    }
    doc->helper_line = (size_t)-1;
    doc->helper_open = false;
    nc2_unpick(doc);
}

/* The rows of an entry land where the helper's name stood. A row that starts
   with a space continues the row above it instead of starting a new one, which
   is how a value joins a line already written; the first row that is written is
   where the cursor and the picked field go. */
bool nc2_helper_write(nc2_document_t *doc, const char *rows)
{
    const char *row;
    size_t at;                  /* where the entry starts, at the helper's line */
    size_t last = (size_t)-1;   /* the last row written: a continuation joins it */
    size_t first = (size_t)-1;
    bool wrote = false;         /* the line at `at` has been written */

    if (!doc || !rows) {
        return false;
    }
    if (doc->helper_open) {
        at = doc->helper_line;
        if (at >= doc->line_count) {
            return false;
        }
    } else {
        at = doc->cursor + 1u;
        if (nc2_insert_line(doc, at, "")) {
            doc->cursor = at;
        } else {
            return false;
        }
    }
    row = rows;
    for (;;) {
        const char *end = strchr(row, '\n');
        size_t len = end ? (size_t)(end - row) : strlen(row);
        char line[NC2_MAX_LINE_LEN];
        bool more = end != 0;

        if (len >= sizeof(line)) {
            len = sizeof(line) - 1u;
        }
        memcpy(line, row, len);
        line[len] = '\0';
        if (line[0] == ' ' && last != (size_t)-1) {
            char joined[NC2_MAX_LINE_LEN];
            int n = snprintf(joined, sizeof(joined), "%s%s",
                             doc->lines[last], line);

            if (n <= 0 || (size_t)n >= sizeof(joined) ||
                !nc2_set_line(doc, last, joined)) {
                return false;
            }
        } else {
            if (wrote) {
                if (!nc2_insert_line(doc, at + 1u, line)) {
                    return false;
                }
                at++;
            } else if (!nc2_set_line(doc, at, line)) {
                return false;       /* the first row stands where the name was */
            }
            last = at;
            wrote = true;
            if (first == (size_t)-1) {
                first = at;
            }
        }
        if (!more) {
            break;
        }
        row = end + 1;
    }
    doc->helper_line = (size_t)-1;
    doc->helper_open = false;
    if (first != (size_t)-1) {
        doc->cursor = first;
        (void)nc2_pick_field(doc, 0);
    }
    return wrote;
}
