#include "ncc.h"

#include "../nc_sender/nc_sender.h"
#include "ncc_preview.h"
#include "ncc_vocab.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ncc_document_init(ncc_document_t *doc)
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

int ncc_fields(const char *line, ncc_field_t *out, int max)
{
    return nc_sender_fields(line, out, max);
}

int ncc_document_fields(const ncc_document_t *doc, ncc_field_t *out, int max)
{
    if (!doc || doc->cursor >= doc->line_count) {
        return 0;
    }
    return ncc_fields(doc->lines[doc->cursor], out, max);
}

/* --- the document as the cycle scan sees it ------------------------------- */

/* A line as g7x reads it: leading spaces skipped, because every reader there
   treats `  G71 ...` as the cycle it is. */
static const char *ncc_document_line(void *user, size_t index)
{
    const ncc_document_t *doc = user;
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

g7x_doc_t ncc_document_g7x(const ncc_document_t *doc)
{
    g7x_doc_t view;

    view.line = doc ? ncc_document_line : 0;
    view.user = (void *)doc;
    view.count = doc ? doc->line_count : 0u;
    return view;
}

/* --- lines ---------------------------------------------------------------- */

bool ncc_insert_line(ncc_document_t *doc, size_t at, const char *text)
{
    size_t i;
    bool below;

    if (!doc || !text || strlen(text) >= NCC_MAX_LINE_LEN ||
        doc->line_count >= NCC_MAX_LINES) {
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
        memcpy(doc->lines[i], doc->lines[i - 1u], NCC_MAX_LINE_LEN);
    }
    snprintf(doc->lines[at], NCC_MAX_LINE_LEN, "%s", text);
    doc->line_count++;
    if (below) {
        doc->cursor++;
    }
    doc->dirty = true;
    return true;
}

bool ncc_set_line(ncc_document_t *doc, size_t at, const char *text)
{
    if (!doc || !text || at >= doc->line_count ||
        strlen(text) >= NCC_MAX_LINE_LEN) {
        return false;
    }
    snprintf(doc->lines[at], NCC_MAX_LINE_LEN, "%s", text);
    doc->dirty = true;
    return true;
}

bool ncc_delete_line(ncc_document_t *doc, size_t at)
{
    size_t i;

    if (!doc || at >= doc->line_count) {
        return false;
    }
    for (i = at; i + 1u < doc->line_count; i++) {
        memcpy(doc->lines[i], doc->lines[i + 1u], NCC_MAX_LINE_LEN);
    }
    doc->line_count--;
    doc->lines[doc->line_count][0] = '\0';
    if (doc->cursor >= doc->line_count && doc->cursor > 0u) {
        doc->cursor--;
    }
    ncc_unpick(doc);
    doc->dirty = true;
    return true;
}

void ncc_cursor_move(ncc_document_t *doc, int delta)
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
        ncc_unpick(doc);
    }
}

/* --- the picked field and the value typed at it --------------------------- */

bool ncc_pick_field(ncc_document_t *doc, int index)
{
    ncc_field_t fields[NCC_MAX_FIELDS];
    int count;

    if (!doc || index < 0) {
        return false;
    }
    count = ncc_document_fields(doc, fields, NCC_MAX_FIELDS);
    if (index >= count) {
        return false;
    }
    doc->field = index;
    doc->draft[0] = '\0';
    doc->drafting = false;
    return true;
}

void ncc_unpick(ncc_document_t *doc)
{
    if (doc) {
        doc->field = -1;
        doc->draft[0] = '\0';
        doc->drafting = false;
    }
}

/* The word the operator walks into once a line has landed or a code has been
   resolved: the first word that is not a code already written. A `G`, `M` or `N`
   word carrying a value *is* the command - typing at it would rewrite the code -
   so what follows it is what the digits want (`G71 U0 ...` walks into the `U0`,
   `N10 G1 X30 Z0` into the `X30`, and the tool table's `T1 R0.8 ...` into the
   tool number, which is what the digits type over there).

   A line that is nothing but its command (`G41`, `G40`, `G42`, `M2`, `G80`)
   leaves **nothing** picked: there is no value to type there, and a pick would
   take `B` and `C` from the cursor, which is how the two kinds of line came to
   behave differently (bench, 2026-09-28: *"if i am over g41 - pressing b/c tries
   to edit it"*, and then *"it still wants to select this g41 or g42 while i
   travel along. and not m commands"*). A bare letter waiting for its number
   (`G`, `T`, `M`) is picked: it carries no value yet. */
static void ncc_pick_landed(ncc_document_t *doc)
{
    ncc_field_t fields[NCC_MAX_FIELDS];
    int count;
    int pick = 0;

    if (!doc) {
        return;
    }
    count = ncc_document_fields(doc, fields, NCC_MAX_FIELDS);
    while (pick < count && fields[pick].value != fields[pick].end) {
        char letter = fields[pick].letter;

        if (letter != 'G' && letter != 'M' && letter != 'N') {
            break;                  /* a word that takes a value: this one */
        }
        pick++;
    }
    if (pick < count) {
        (void)ncc_pick_field(doc, pick);
    } else {
        ncc_unpick(doc);            /* nothing on this line is the operator's */
    }
}

/* The word of the cursor's row that one of the TOOLS pad's keys stands for: the
   key's own number is the word's number (so `2` is the row's second word, its
   `R`). The row carries `T` plus its eight fields, so keys `2`..`9` pick them
   in the row's own order.

   A row may carry more words than the pad has keys: the last key then walks
   them, one press each, so no word of the row is out of reach. Answers -1 when
   the key names nothing on this row. The pad's labels and its press ask this, so
   the mapping is stated once. */
int ncc_pad_word_index(const ncc_document_t *doc, char key)
{
    ncc_field_t fields[NCC_MAX_FIELDS];
    const char *line;
    int count;
    int pick;

    if (!doc || key < '1' || key > '9') {
        return -1;
    }
    line = (doc->cursor < doc->line_count) ? doc->lines[doc->cursor] : "";
    count = ncc_fields(line, fields, NCC_MAX_FIELDS);
    /* A tool row may still carry the retired `O` word from an older card. The
       pad's field keys follow the fields that remain, so skip it here rather
       than letting `3` land on a word the screen no longer shows. */
    if (count > 0 && (fields[0].letter == 'T' || fields[0].letter == 't')) {
        int want = key - '1';
        int seen = 0;
        int i;

        for (i = 0; i < count; i++) {
            if (fields[i].letter == 'O' || fields[i].letter == 'o') {
                continue;
            }
            if (seen == want) {
                return i;
            }
            seen++;
        }
        return -1;
    }
    count = ncc_document_fields(doc, 0, 0);
    pick = key - '1';
    if (key == '9' && count > 9) {
        pick = (doc->field >= 8 && doc->field + 1 < count) ? doc->field + 1 : 8;
    }
    return pick < count ? pick : -1;
}

int ncc_gcode_resolve(ncc_document_t *doc)
{
    ncc_field_t fields[NCC_MAX_FIELDS];
    const char *template;
    const char *name;
    const char *text;
    int count;
    int code;

    if (!doc || doc->field >= 0 || doc->cursor >= doc->line_count) {
        return 0;                       /* a field is still being typed */
    }
    text = doc->lines[doc->cursor];
    count = ncc_fields(text, fields, NCC_MAX_FIELDS);
    if (count != 1 || (char)toupper((unsigned char)fields[0].letter) != 'G') {
        return 0;                       /* not a line that is only a G word */
    }
    if (fields[0].value == fields[0].end) {
        return 0;                       /* the letter alone: no number yet */
    }
    code = (int)(strtod(text + fields[0].value, 0) + 0.5);
    name = ncc_vocab_gcode_name(code);
    if (!name) {
        return 0;                       /* a code the panel does not know */
    }
    template = ncc_vocab_gcode_template(code);
    if (template) {
        if (!ncc_set_line(doc, doc->cursor, template)) {
            return 0;
        }
        /* Into the first *value* the code expanded to - and nothing at all when
           the code is a whole line on its own (`G41`, `G40`, `G42`). */
        ncc_pick_landed(doc);
    }
    return code;
}

/* Put a value where the picked field's was: the rest of the line is carried
   over untouched, so what the operator did not type is not rewritten. */
static bool ncc_field_apply(ncc_document_t *doc, const char *value)
{
    ncc_field_t fields[NCC_MAX_FIELDS];
    char line[NCC_MAX_LINE_LEN];
    int count;
    int n;

    if (!doc || doc->field < 0 || doc->cursor >= doc->line_count) {
        return false;
    }
    count = ncc_fields(doc->lines[doc->cursor], fields, NCC_MAX_FIELDS);
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
    return ncc_set_line(doc, doc->cursor, line);
}

static bool ncc_type_digit(ncc_document_t *doc, char digit)
{
    ncc_field_t fields[NCC_MAX_FIELDS];
    int count;

    if (!doc || doc->field < 0) {
        return false;
    }
    count = ncc_document_fields(doc, fields, NCC_MAX_FIELDS);
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
    return ncc_field_apply(doc, doc->draft);
}

/* The sign, on a keypad that has no `-`: it toggles, and it starts from the
   value that is there so `30` becomes `-30` before anything is typed. */
static bool ncc_type_sign(ncc_document_t *doc)
{
    ncc_field_t fields[NCC_MAX_FIELDS];
    int count;

    if (!doc || doc->field < 0) {
        return false;
    }
    count = ncc_document_fields(doc, fields, NCC_MAX_FIELDS);
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
    return ncc_field_apply(doc, doc->draft);
}

/* The point, on the same keypad: one per value. */
static bool ncc_type_point(ncc_document_t *doc)
{
    ncc_field_t fields[NCC_MAX_FIELDS];
    int count;

    if (!doc || doc->field < 0) {
        return false;
    }
    count = ncc_document_fields(doc, fields, NCC_MAX_FIELDS);
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
    return ncc_field_apply(doc, doc->draft);
}

static bool ncc_type_backspace(ncc_document_t *doc)
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
    return ncc_field_apply(doc, doc->draft);
}

/* --- the keys ------------------------------------------------------------- */

bool ncc_key(ncc_document_t *doc, ncc_key_t key, char ch)
{
    ncc_field_t fields[NCC_MAX_FIELDS];
    int count;

    if (!doc) {
        return false;
    }
    if (doc->field >= 0) {
        /* A value is picked: the keys are the value's. */
        switch (key) {
        case NCC_KEY_DIGIT:
            return ncc_type_digit(doc, ch);
        case NCC_KEY_UP:
            return ncc_type_sign(doc);
        case NCC_KEY_DOWN:
            return ncc_type_point(doc);
        case NCC_KEY_ACCEPT:
            ncc_unpick(doc);
            return true;
        case NCC_KEY_DELETE:
            if (ncc_type_backspace(doc)) {
                return true;
            }
            ncc_unpick(doc);
            return true;
        case NCC_KEY_NEXT:
            count = ncc_document_fields(doc, fields, NCC_MAX_FIELDS);
            if (doc->field + 1 < count) {
                return ncc_pick_field(doc, doc->field + 1);
            }
            ncc_unpick(doc);
            return true;
        default:
            return false;
        }
    }
    switch (key) {
    case NCC_KEY_UP:
        ncc_cursor_move(doc, -1);
        return true;
    case NCC_KEY_DOWN:
        ncc_cursor_move(doc, 1);
        return true;
    case NCC_KEY_NEXT:
    case NCC_KEY_ACCEPT:
        /* Into the line: the first field is the one the operator means when
           they press the key with nothing picked. */
        return ncc_pick_field(doc, 0);
    case NCC_KEY_DELETE:
        return ncc_delete_line(doc, doc->cursor);
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

static bool g_ncc_contour;
static uint8_t g_ncc_contour_step;
/* The row just written and the word left picked on it: while both are still
   under the cursor the point is being entered, and the digits belong to the
   editor's field flow rather than to the pad. */
static size_t g_ncc_contour_point = (size_t)-1;
static int g_ncc_contour_word = -1;
static int g_ncc_contour_next = -1;

/* The distance one press moves: the starting point of the value that lands, so
   `#` steps it and the digits type over it. Millimetres, like the program. */
static const float g_ncc_contour_steps[] = {
    0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 50.0f
};

float ncc_contour_step(void)
{
    return g_ncc_contour_steps[g_ncc_contour_step];
}

bool ncc_contour_active(void)
{
    return g_ncc_contour;
}

static void ncc_contour_settle(void)
{
    g_ncc_contour_point = (size_t)-1;
    g_ncc_contour_word = -1;
    g_ncc_contour_next = -1;
}

/* Which way a pad key moves the tool: X is the diameter it cuts, Z the length.
   The corners move both axes, which is what makes a chamfer or a taper one
   press instead of two - nc's own directions, so a profile written with the pad
   is the same profile. */
static bool ncc_contour_direction(char key, int *dx, int *dz)
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

const char *ncc_contour_label(char key)
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

bool ncc_contour_begin(ncc_document_t *doc)
{
    if (!doc) {
        return false;
    }
    g_ncc_contour = true;
    ncc_contour_settle();
    return true;
}

void ncc_contour_leave(void)
{
    g_ncc_contour = false;
    ncc_contour_settle();
}

/* True while the row just written is still the one being entered: nothing has
   taken the value and the cursor has not left it. */
static bool ncc_contour_pending(const ncc_document_t *doc)
{
    return g_ncc_contour_point != (size_t)-1 &&
           g_ncc_contour_point < doc->line_count &&
           doc->cursor == g_ncc_contour_point &&
           doc->field == g_ncc_contour_word;
}

/* A millimetre value as a program spells it: `G1 X30 Z-15`, not `G1 X30.000`.
   The value is left picked for typing either way, so this is what the row reads
   when the operator does not change it. */
static void ncc_contour_mm(float value, char *out, size_t out_sz)
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
static void ncc_contour_point_at(const ncc_document_t *doc, float *x, float *z)
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

        if (!nc_sender_emit_line_is_direct(doc->lines[i])) {
            continue;                   /* a header's U is not an increment */
        }
        if (!nc_sender_emit_line_point(doc->lines[i], &px, &pz, 0, 0u, &words)) {
            continue;
        }
        *x = px;
        *z = pz;
        if (words & NC_SENDER_EMIT_WORD_X_ABS) {
            x_known = true;
        }
        if (words & NC_SENDER_EMIT_WORD_Z_ABS) {
            z_known = true;
        }
    }
    if (!x_known) {
        *x = ncc_preview_stock_x(doc);
    }
    if (!z_known) {
        *z = 0.0f;
    }
}

/* One press: a row of its own, below the cursor, and the cursor on it - which is
   the whole of the "walk", because the next press reads its point from the row
   just written. */
static bool ncc_contour_move(ncc_document_t *doc, int dx, int dz)
{
    char xs[24];
    char zs[24];
    char row[NCC_MAX_LINE_LEN];
    ncc_field_t fields[NCC_MAX_FIELDS];
    size_t at = doc->cursor + 1u;
    float x = 0.0f;
    float z = 0.0f;
    float step = ncc_contour_step();
    int count;
    int i;

    ncc_contour_point_at(doc, &x, &z);
    x += (float)dx * step;
    z += (float)dz * step;
    ncc_contour_mm(x, xs, sizeof(xs));
    ncc_contour_mm(z, zs, sizeof(zs));
    snprintf(row, sizeof(row), "G1 X%s Z%s", xs, zs);
    if (doc->line_count == 0u) {
        at = 0u;                    /* an empty program takes its first row */
    }
    if (!ncc_insert_line(doc, at, row)) {
        return false;
    }
    doc->cursor = at;
    ncc_unpick(doc);
    g_ncc_contour_point = at;
    g_ncc_contour_word = -1;
    g_ncc_contour_next = -1;
    /* The value the operator is most likely to change is picked: the X word for
       an X move, the Z word for a Z move, and for a corner the X one - the
       diameter, the number a lathe hand reads first. */
    count = ncc_fields(row, fields, NCC_MAX_FIELDS);
    for (i = 0; i < count; i++) {
        char letter = (char)toupper((unsigned char)fields[i].letter);

        if ((dx != 0 && letter == 'X') || (dz != 0 && letter == 'Z')) {
            if (g_ncc_contour_word < 0) {
                g_ncc_contour_word = i;
            } else {
                g_ncc_contour_next = i;
                break;
            }
        }
    }
    if (g_ncc_contour_word >= 0) {
        (void)ncc_pick_field(doc, g_ncc_contour_word);
    }
    return true;
}

bool ncc_contour_key(ncc_document_t *doc, char key)
{
    int dx = 0;
    int dz = 0;

    if (!doc || !g_ncc_contour) {
        return false;
    }
    if (ncc_contour_pending(doc)) {
        if (key == 'D') {
            if (g_ncc_contour_next >= 0) {
                g_ncc_contour_word = g_ncc_contour_next;
                g_ncc_contour_next = -1;
                return ncc_pick_field(doc, g_ncc_contour_word);
            }
            ncc_unpick(doc);
            return true;                /* the point is taken: the pad is back */
        }
        if (key == '*') {
            (void)ncc_delete_line(doc, doc->cursor);
            ncc_contour_settle();
            return true;
        }
        /* The digits, the sign, the point and `#` are the editor's field flow:
           what is typed lands in the word the pad just picked. */
        return false;
    }
    if (key == '5') {
        g_ncc_contour = false;
        ncc_contour_settle();
        return true;
    }
    if (key == '#') {                   /* the step */
        g_ncc_contour_step =
            (uint8_t)((g_ncc_contour_step + 1u) %
                      (sizeof(g_ncc_contour_steps) /
                       sizeof(g_ncc_contour_steps[0])));
        return true;
    }
    if (key == '*') {                   /* the point under the cursor */
        return ncc_delete_line(doc, doc->cursor);
    }
    if (!ncc_contour_direction(key, &dx, &dz)) {
        return false;                   /* a key the pad does not use */
    }
    return ncc_contour_move(doc, dx, dz);
}

/* --- the pad -------------------------------------------------------------- */

bool ncc_pad_active(const ncc_document_t *doc)
{
    return doc && doc->pad_open;
}

bool ncc_pad_open(ncc_document_t *doc, const char *name)
{
    size_t origin;
    size_t at;

    if (!doc || !name) {
        return false;
    }
    ncc_pad_close(doc);
    ncc_unpick(doc);
    if (doc->line_count == 0u) {
        origin = 0u;
        at = 0u;
    } else {
        origin = doc->cursor;
        at = doc->cursor + 1u;
    }
    if (!ncc_insert_line(doc, at, name)) {
        return false;
    }
    doc->cursor = at;
    doc->pad_origin = origin;
    doc->pad_label = at;
    doc->pad_next = at;
    doc->pad_open = true;
    return true;
}

void ncc_pad_close(ncc_document_t *doc)
{
    if (!doc || !doc->pad_open) {
        return;
    }
    /* The name line stands where the entry would have landed: if nothing was
       written there it goes, and the cursor is where the pad found it. Once an
       entry has landed the line is the operator's program and stays. */
    if (doc->pad_label < doc->line_count) {
        (void)ncc_delete_line(doc, doc->pad_label);
        if (doc->pad_origin < doc->line_count) {
            doc->cursor = doc->pad_origin;
        }
    }
    doc->pad_label = (size_t)-1;
    doc->pad_open = false;
    ncc_unpick(doc);
}

/* The word a join just added: the first field at or after `from`, which is
   where the row's text landed in the line. */
static int ncc_field_from(const char *line, size_t from)
{
    ncc_field_t fields[NCC_MAX_FIELDS];
    int count = ncc_fields(line, fields, NCC_MAX_FIELDS);
    int i;

    for (i = 0; i < count; i++) {
        if (fields[i].start >= from) {
            return i;
        }
    }
    return count > 0 ? count - 1 : -1;
}

/* A row that starts with a space finishes a line instead of starting one. The
   line meant is the one the press began on: ` F`, ` C0` and ` R0` land at its
   end, and ` N` at its front, where a line number belongs (bench, 2026-09-28:
   "appends are append for all plus prepend for n"). The word is left picked, so
   the digits type its number the way they type any other value.

   False when there is no such line - an empty program, or nothing under the
   pad's own name - and the row goes the ordinary way. */
static bool ncc_pad_join(ncc_document_t *doc, const char *row, size_t len)
{
    size_t text = 0u;
    size_t target;
    size_t old_len;
    size_t from;
    char joined[NCC_MAX_LINE_LEN];
    int n;

    if (!doc || !doc->pad_open || doc->line_count == 0u) {
        return false;
    }
    target = doc->cursor == doc->pad_label ? doc->pad_origin : doc->cursor;
    if (target >= doc->line_count || target == doc->pad_label) {
        return false;               /* the cursor is on the pad's own name */
    }
    while (text < len && row[text] == ' ') {
        text++;
    }
    if (text >= len) {
        return false;               /* spaces and nothing else */
    }
    old_len = strlen(doc->lines[target]);
    if (old_len == 0u) {
        n = snprintf(joined, sizeof(joined), "%.*s",
                     (int)(len - text), row + text);
        from = 0u;
    } else if (toupper((unsigned char)row[text]) == 'N') {
        n = snprintf(joined, sizeof(joined), "%.*s %s",
                     (int)(len - text), row + text, doc->lines[target]);
        from = 0u;
    } else {
        n = snprintf(joined, sizeof(joined), "%s%.*s",
                     doc->lines[target], (int)len, row);
        from = old_len;
    }
    if (n <= 0 || (size_t)n >= sizeof(joined)) {
        return false;
    }
    /* The pad's name line goes with the press: the row finished a line of the
       program, so there is no entry for a name to stand over. */
    if (doc->pad_label != (size_t)-1) {
        bool above = doc->pad_label < target;

        (void)ncc_delete_line(doc, doc->pad_label);
        if (above) {
            target--;
        }
    }
    if (!ncc_set_line(doc, target, joined)) {
        return false;
    }
    doc->pad_label = (size_t)-1;
    /* The row landed, so the press is done: the pad ends the way nc's menu did
       (its `nc_editor_modal_cancel()` before every write), and only what it left
       picked keeps the keys - the digits are the word's from here. */
    doc->pad_open = false;
    doc->cursor = target;
    doc->pad_next = target + 1u;
    (void)ncc_pick_field(doc, ncc_field_from(doc->lines[target], from));
    return true;
}

/* The rows of an entry land where the pad's name stood - and, on the presses
   that follow, one under the other. A row that starts with a space joins the
   line the press began on instead (`ncc_pad_join`); every other row stands on a
   line of its own, and the first row written is where the cursor and the picked
   field go. The pad then ends: the walk that stays up for every point is the
   path builder's own (`ncc_contour_*`), not this one, which is nc's rule - its
   menu closed on any entry and left the `A` key as the way up a level while
   choosing. */

bool ncc_pad_write(ncc_document_t *doc, const char *rows)
{
    const char *row;
    size_t at;                  /* where the entry starts, at the helper's line */
    size_t last = (size_t)-1;   /* the last row written */
    size_t first = (size_t)-1;
    bool wrote = false;         /* the line at `at` has been written */

    if (!doc || !rows) {
        return false;
    }
    row = rows;
    {
        const char *end = strchr(row, '\n');
        size_t len = end ? (size_t)(end - row) : strlen(row);

        if (len > 0u && row[0] == ' ' && ncc_pad_join(doc, row, len)) {
            if (!end) {
                return true;
            }
            row = end + 1;          /* what follows stands under the joined line */
        }
    }
    if (doc->pad_label != (size_t)-1) {
        at = doc->pad_label;
        if (at >= doc->line_count) {
            return false;
        }
    } else {
        at = doc->pad_next;
        if (ncc_insert_line(doc, at, "")) {
            doc->cursor = at;
        } else {
            return false;
        }
    }
    for (;;) {
        const char *end = strchr(row, '\n');
        size_t len = end ? (size_t)(end - row) : strlen(row);
        char line[NCC_MAX_LINE_LEN];
        bool more = end != 0;

        if (len >= sizeof(line)) {
            len = sizeof(line) - 1u;
        }
        memcpy(line, row, len);
        line[len] = '\0';
        if (wrote) {
            if (!ncc_insert_line(doc, at + 1u, line)) {
                return false;
            }
            at++;
        } else if (!ncc_set_line(doc, at, line)) {
            return false;           /* the first row stands where the name was */
        }
        last = at;
        wrote = true;
        if (first == (size_t)-1) {
            first = at;
        }
        if (!more) {
            break;
        }
        row = end + 1;
    }
    doc->pad_label = (size_t)-1;
    doc->pad_open = false;
    if (first != (size_t)-1) {
        doc->cursor = first;
        ncc_pick_landed(doc);
    }
    if (last != (size_t)-1) {
        /* Under the last row written, not under the cursor: the cursor is on the
           first row, where the value to edit is. */
        doc->pad_next = last + 1u;
    }
    return wrote;
}
