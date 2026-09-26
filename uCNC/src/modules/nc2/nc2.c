#include "nc2.h"

#include "nc2_emit.h"
#include "nc2_preview.h"

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
    doc->pad_label = (size_t)-1;
    doc->pad_origin = 0u;
    doc->pad_next = 0u;
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

/* --- the document as the cycle scan sees it ------------------------------- */

/* A line as g7x reads it: leading spaces skipped, because every reader there
   treats `  G71 ...` as the cycle it is. */
static const char *nc2_document_line(void *user, size_t index)
{
    const nc2_document_t *doc = user;
    const char *line;

    if (!doc || index >= doc->line_count) {
        return "";
    }
    line = doc->lines[index];
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    return line;
}

g7x_doc_t nc2_document_g7x(const nc2_document_t *doc)
{
    g7x_doc_t view;

    view.line = doc ? nc2_document_line : 0;
    view.user = (void *)doc;
    view.count = doc ? doc->line_count : 0u;
    return view;
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
        /* Moving by hand is saying "I am working here": the pad's next entry
           belongs under where the operator put the cursor. */
        doc->pad_next = doc->cursor + 1u;
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

/* --- the path builder: G7X's `7`, the address 47 --------------------------

   nc's contour pad, kept as it behaved ("same result as before"): each press
   writes one `G1` row below the cursor, the axes that move at the step and the
   others carried over from the point the row above reaches. The pad stays until
   `5`, the row just written keeps its value picked so the digits type the real
   number over it, `D` takes a corner's second word and closes the point, and `*`
   takes the point back. */

static bool g_nc2_contour;
static uint8_t g_nc2_contour_step;
/* The row just written and the word left picked on it: while both are still
   under the cursor the point is being entered, and the digits belong to the
   editor's field flow rather than to the pad. */
static size_t g_nc2_contour_point = (size_t)-1;
static int g_nc2_contour_word = -1;
static int g_nc2_contour_next = -1;

/* The distance one press moves: the starting point of the value that lands, so
   `#` steps it and the digits type over it. Millimetres, like the program. */
static const float g_nc2_contour_steps[] = {
    0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 50.0f
};

float nc2_contour_step(void)
{
    return g_nc2_contour_steps[g_nc2_contour_step];
}

bool nc2_contour_active(void)
{
    return g_nc2_contour;
}

static void nc2_contour_settle(void)
{
    g_nc2_contour_point = (size_t)-1;
    g_nc2_contour_word = -1;
    g_nc2_contour_next = -1;
}

/* Which way a pad key moves the tool: X is the diameter it cuts, Z the length.
   The corners move both axes, which is what makes a chamfer or a taper one
   press instead of two - nc's own directions, so a profile written with the pad
   is the same profile. */
static bool nc2_contour_direction(char key, int *dx, int *dz)
{
    switch (key) {
    case '1': *dx = 1;  *dz = -1; return true;
    case '2': *dx = 1;  *dz = 0;  return true;
    case '3': *dx = 1;  *dz = 1;  return true;
    case '4': *dx = 0;  *dz = -1; return true;
    case '6': *dx = 0;  *dz = 1;  return true;
    case '7': *dx = -1; *dz = -1; return true;
    case '8': *dx = -1; *dz = 0;  return true;
    case '9': *dx = -1; *dz = 1;  return true;
    default: return false;
    }
}

const char *nc2_contour_label(char key)
{
    switch (key) {
    case '1': return "Z- X+";
    case '2': return "X+";
    case '3': return "Z+ X+";
    case '4': return "Z-";
    case '5': return "END";
    case '6': return "Z+";
    case '7': return "Z- X-";
    case '8': return "X-";
    case '9': return "Z+ X-";
    default: return "";
    }
}

bool nc2_contour_begin(nc2_document_t *doc)
{
    if (!doc) {
        return false;
    }
    g_nc2_contour = true;
    nc2_contour_settle();
    return true;
}

void nc2_contour_leave(void)
{
    g_nc2_contour = false;
    nc2_contour_settle();
}

/* True while the row just written is still the one being entered: nothing has
   taken the value and the cursor has not left it. */
static bool nc2_contour_pending(const nc2_document_t *doc)
{
    return g_nc2_contour_point != (size_t)-1 &&
           g_nc2_contour_point < doc->line_count &&
           doc->cursor == g_nc2_contour_point &&
           doc->field == g_nc2_contour_word;
}

/* A millimetre value as a program spells it: `G1 X30 Z-15`, not `G1 X30.000`.
   The value is left picked for typing either way, so this is what the row reads
   when the operator does not change it. */
static void nc2_contour_mm(float value, char *out, size_t out_sz)
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

/* Where the profile is now: the point the rows up to the cursor leave the tool
   at, read with the one rule the sender and the preview use. An axis the program
   has not given yet starts at the stock's own corner, which is where a lathe
   profile starts. */
static void nc2_contour_point_at(const nc2_document_t *doc, float *x, float *z)
{
    bool x_known = false;
    bool z_known = false;
    size_t i;

    *x = 0.0f;
    *z = 0.0f;
    for (i = 0u; i < doc->line_count && i <= doc->cursor; i++) {
        uint8_t words = 0u;
        float px = *x;
        float pz = *z;

        if (!nc2_emit_line_is_direct(doc->lines[i])) {
            continue;                   /* a header's U is not an increment */
        }
        if (!nc2_emit_line_point(doc->lines[i], &px, &pz, 0, 0u, &words)) {
            continue;
        }
        *x = px;
        *z = pz;
        if (words & NC2_EMIT_WORD_X_ABS) {
            x_known = true;
        }
        if (words & NC2_EMIT_WORD_Z_ABS) {
            z_known = true;
        }
    }
    if (!x_known) {
        *x = nc2_preview_stock_x(doc);
    }
    if (!z_known) {
        *z = 0.0f;
    }
}

/* One press: a row of its own, below the cursor, and the cursor on it - which is
   the whole of the "walk", because the next press reads its point from the row
   just written. */
static bool nc2_contour_move(nc2_document_t *doc, int dx, int dz)
{
    char xs[24];
    char zs[24];
    char row[NC2_MAX_LINE_LEN];
    nc2_field_t fields[NC2_MAX_FIELDS];
    size_t at = doc->cursor + 1u;
    float x = 0.0f;
    float z = 0.0f;
    float step = nc2_contour_step();
    int count;
    int i;

    nc2_contour_point_at(doc, &x, &z);
    x += (float)dx * step;
    z += (float)dz * step;
    nc2_contour_mm(x, xs, sizeof(xs));
    nc2_contour_mm(z, zs, sizeof(zs));
    snprintf(row, sizeof(row), "G1 X%s Z%s", xs, zs);
    if (doc->line_count == 0u) {
        at = 0u;                    /* an empty program takes its first row */
    }
    if (!nc2_insert_line(doc, at, row)) {
        return false;
    }
    doc->cursor = at;
    nc2_unpick(doc);
    g_nc2_contour_point = at;
    g_nc2_contour_word = -1;
    g_nc2_contour_next = -1;
    /* The value the operator is most likely to change is picked: the X word for
       an X move, the Z word for a Z move, and for a corner the X one - the
       diameter, the number a lathe hand reads first. */
    count = nc2_fields(row, fields, NC2_MAX_FIELDS);
    for (i = 0; i < count; i++) {
        char letter = (char)toupper((unsigned char)fields[i].letter);

        if ((dx != 0 && letter == 'X') || (dz != 0 && letter == 'Z')) {
            if (g_nc2_contour_word < 0) {
                g_nc2_contour_word = i;
            } else {
                g_nc2_contour_next = i;
                break;
            }
        }
    }
    if (g_nc2_contour_word >= 0) {
        (void)nc2_pick_field(doc, g_nc2_contour_word);
    }
    return true;
}

bool nc2_contour_key(nc2_document_t *doc, char key)
{
    int dx = 0;
    int dz = 0;

    if (!doc || !g_nc2_contour) {
        return false;
    }
    if (nc2_contour_pending(doc)) {
        if (key == 'D') {
            if (g_nc2_contour_next >= 0) {
                g_nc2_contour_word = g_nc2_contour_next;
                g_nc2_contour_next = -1;
                return nc2_pick_field(doc, g_nc2_contour_word);
            }
            nc2_unpick(doc);
            return true;                /* the point is taken: the pad is back */
        }
        if (key == '*') {
            (void)nc2_delete_line(doc, doc->cursor);
            nc2_contour_settle();
            return true;
        }
        /* The digits, the sign, the point and `#` are the editor's field flow:
           what is typed lands in the word the pad just picked. */
        return false;
    }
    if (key == '5') {
        g_nc2_contour = false;
        nc2_contour_settle();
        return true;
    }
    if (key == '#') {                   /* the step */
        g_nc2_contour_step =
            (uint8_t)((g_nc2_contour_step + 1u) %
                      (sizeof(g_nc2_contour_steps) /
                       sizeof(g_nc2_contour_steps[0])));
        return true;
    }
    if (key == '*') {                   /* the point under the cursor */
        return nc2_delete_line(doc, doc->cursor);
    }
    if (!nc2_contour_direction(key, &dx, &dz)) {
        return false;                   /* a key the pad does not use */
    }
    return nc2_contour_move(doc, dx, dz);
}

/* --- the pad -------------------------------------------------------------- */

bool nc2_pad_active(const nc2_document_t *doc)
{
    return doc && doc->pad_open;
}

bool nc2_pad_open(nc2_document_t *doc, const char *name)
{
    size_t origin;
    size_t at;

    if (!doc || !name) {
        return false;
    }
    nc2_pad_close(doc);
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
    doc->pad_origin = origin;
    doc->pad_label = at;
    doc->pad_next = at;
    doc->pad_open = true;
    return true;
}

void nc2_pad_close(nc2_document_t *doc)
{
    if (!doc || !doc->pad_open) {
        return;
    }
    /* The name line stands where the entry would have landed: if nothing was
       written there it goes, and the cursor is where the pad found it. Once an
       entry has landed the line is the operator's program and stays. */
    if (doc->pad_label < doc->line_count) {
        (void)nc2_delete_line(doc, doc->pad_label);
        if (doc->pad_origin < doc->line_count) {
            doc->cursor = doc->pad_origin;
        }
    }
    doc->pad_label = (size_t)-1;
    doc->pad_open = false;
    nc2_unpick(doc);
}

/* The rows of an entry land where the pad's name stood - and, on the presses
   after the first, one under the other, so a contour is written by pressing the
   same pad again and again. A row that starts with a space continues the row
   above it instead of starting a new one, which is how a value joins a line
   already written; the first row written is where the cursor and the picked
   field go. */
bool nc2_pad_write(nc2_document_t *doc, const char *rows)
{
    const char *row;
    size_t at;                  /* where the entry starts, at the helper's line */
    size_t last = (size_t)-1;   /* the last row written: a continuation joins it */
    size_t first = (size_t)-1;
    bool wrote = false;         /* the line at `at` has been written */

    if (!doc || !rows) {
        return false;
    }
    if (doc->pad_label != (size_t)-1) {
        at = doc->pad_label;
        if (at >= doc->line_count) {
            return false;
        }
    } else {
        at = doc->pad_next;
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
    doc->pad_label = (size_t)-1;
    if (first != (size_t)-1) {
        doc->cursor = first;
        (void)nc2_pick_field(doc, 0);
    }
    if (last != (size_t)-1) {
        /* Under the last row written, not under the cursor: the cursor is on the
           first row, where the value to edit is. */
        doc->pad_next = last + 1u;
    }
    return wrote;
}
