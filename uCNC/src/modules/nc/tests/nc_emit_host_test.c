#ifdef NC_HOST_TEST

#include "../nc.h"
#include "../nc_emit.h"
#include "../nc_g7x.h"
#include "../nc_visual.h"

#include <stdio.h>
#include <string.h>

void grbl_stream_printf(const char *fmt, ...)
{
    (void)fmt;
}

static int expect_line(nc_emit_stream_t *stream, const char *want)
{
    char out[96];
    size_t source_line;
    nc_emit_result_t got;

    do {
        got = nc_emit_stream_next(stream, out, sizeof(out), &source_line);
    } while (got == NC_EMIT_SKIP && stream->active);

    if (got != NC_EMIT_LINE || strcmp(out, want) != 0) {
        printf("FAIL want [%s] got [%s]\n", want, got == NC_EMIT_LINE ? out : "(skip)");
        return 1;
    }
    return 0;
}

static int expect_find_line(nc_emit_stream_t *stream, const char *want)
{
    char out[96];
    size_t source_line;
    nc_emit_result_t got;

    while (stream->active) {
        got = nc_emit_stream_next(stream, out, sizeof(out), &source_line);
        if (got == NC_EMIT_LINE && strcmp(out, want) == 0) {
            return 0;
        }
    }

    printf("FAIL did not find [%s]\n", want);
    return 1;
}

static int expect_find_line_source(nc_emit_stream_t *stream, const char *want, size_t want_source)
{
    char out[96];
    size_t source_line;
    nc_emit_result_t got;

    while (stream->active) {
        got = nc_emit_stream_next(stream, out, sizeof(out), &source_line);
        if (got == NC_EMIT_LINE && strcmp(out, want) == 0) {
            if (source_line != want_source) {
                printf("FAIL [%s] source want %lu got %lu\n",
                       want,
                       (unsigned long)want_source,
                       (unsigned long)source_line);
                return 1;
            }
            return 0;
        }
    }

    printf("FAIL did not find [%s]\n", want);
    return 1;
}

static int expect_rejected(nc_emit_stream_t *stream)
{
    char out[96];
    size_t source;
    nc_emit_result_t r;
    do {
        r = nc_emit_stream_next(stream, out, sizeof(out), &source);
    } while (r == NC_EMIT_SKIP && stream->active);
    if (r != NC_EMIT_ERROR || stream->active || stream->error == G7X_OK) {
        printf("FAIL invalid contour was not stopped: result=%d active=%d error=%d\n",
               r, stream->active, stream->error);
        return 1;
    }
    return 0;
}

static int expect_rejected_with(nc_emit_stream_t *stream, g7x_result_t want)
{
    char out[96];
    size_t source;
    nc_emit_result_t r;
    do {
        r = nc_emit_stream_next(stream, out, sizeof(out), &source);
    } while (r == NC_EMIT_SKIP && stream->active);
    if (r != NC_EMIT_ERROR || stream->active || stream->error != want) {
        printf("FAIL expected error %d, got result=%d error=%d active=%d\n",
               (int)want, r, (int)stream->error, stream->active);
        return 1;
    }
    return 0;
}

/* Fanuc/Haas numbered range in preview: N(P)..N(Q) supplies the contour and
   the source continues with the line after N(Q). */
static int test_g71_numbered_range(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G71 U1 R1 P100 Q200 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 1, "N100 G1 X50 Z0");
    (void)nc_insert_line(&doc, 2, "G1 X50 Z-10");
    (void)nc_insert_line(&doc, 3, "N200 G1 X40 Z-10");
    (void)nc_insert_line(&doc, 4, "G0 X80 Z0");
    nc_emit_stream_begin(&stream, &doc, 0);

    return expect_find_line(&stream, "(G7x finish contour)") ||
           expect_line(&stream, "G0 X52.500") ||
           expect_line(&stream, "G0 Z1.500") ||
           expect_line(&stream, "G1 X50.000 Z0.000 F120.000") ||
           expect_line(&stream, "G1 X50.000 Z-10.000") ||
           expect_line(&stream, "G1 X40.000 Z-10.000") ||
           expect_line(&stream, "G0 X52.500") ||
           expect_line(&stream, "G0 Z1.500") ||
           expect_line(&stream, "G0 X80 Z0");
}

static int test_g71_numbered_range_errors(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;
    int fails = 0;

    /* N(Q) is never reached. */
    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G71 U1 R1 P100 Q200 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 1, "N100 G1 X50 Z0");
    (void)nc_insert_line(&doc, 2, "G1 X50 Z-10");
    nc_emit_stream_begin(&stream, &doc, 0);
    fails += expect_rejected_with(&stream, G7X_RANGE_MISSING);

    /* Profile numbering runs backwards inside the range. */
    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G71 U1 R1 P100 Q200 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 1, "N100 G1 X50 Z0");
    (void)nc_insert_line(&doc, 2, "N150 G1 Z-10");
    (void)nc_insert_line(&doc, 3, "N120 G1 X40 Z-10");
    nc_emit_stream_begin(&stream, &doc, 0);
    fails += expect_rejected_with(&stream, G7X_RANGE_AMBIGUOUS);

    /* Incomplete P/Q header is rejected before any contour is collected. */
    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G71 U1 R1 P100 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 1, "N100 G1 X50 Z0");
    nc_emit_stream_begin(&stream, &doc, 0);
    fails += expect_rejected_with(&stream, G7X_BAD_FIELD);

    return fails;
}

/* Fanuc two-line header in preview: the depth block and the P/Q block form one
   cycle, and the source continues after the N(Q) row. */
static int test_g71_two_line_numbered_range(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G71 U1 R1");
    (void)nc_insert_line(&doc, 1, "G71 P100 Q200 U0.5 W0.25 F120");
    (void)nc_insert_line(&doc, 2, "N100 G1 X50 Z0");
    (void)nc_insert_line(&doc, 3, "G1 X50 Z-10");
    (void)nc_insert_line(&doc, 4, "N200 G1 X40 Z-10");
    (void)nc_insert_line(&doc, 5, "G0 X80 Z0");
    nc_emit_stream_begin(&stream, &doc, 0);

    return expect_find_line(&stream, "(G7x finish contour)") ||
           expect_line(&stream, "G0 X52.500") ||
           expect_line(&stream, "G0 Z1.250") ||
           expect_line(&stream, "G1 X50.000 Z0.000 F120.000") ||
           expect_line(&stream, "G1 X50.000 Z-10.000") ||
           expect_line(&stream, "G1 X40.000 Z-10.000") ||
           expect_line(&stream, "G0 X52.500") ||
           expect_line(&stream, "G0 Z1.250") ||
           expect_line(&stream, "G0 X80 Z0");
}

/* Two-line header that still ends at G80: the second block carries the finish
   allowances and the feed instead of a numbered range. */
static int test_g71_two_line_g80(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G71 U1 R1");
    (void)nc_insert_line(&doc, 1, "G71 U0.5 W0.25 F120");
    (void)nc_insert_line(&doc, 2, "G1 X50 Z0");
    (void)nc_insert_line(&doc, 3, "G1 X50 Z-10");
    (void)nc_insert_line(&doc, 4, "G1 X40 Z-10");
    (void)nc_insert_line(&doc, 5, "G80");
    (void)nc_insert_line(&doc, 6, "G0 X80 Z0");
    nc_emit_stream_begin(&stream, &doc, 0);

    return expect_find_line(&stream, "(G7x finish contour)") ||
           expect_line(&stream, "G0 X52.500") ||
           expect_line(&stream, "G0 Z1.250") ||
           expect_line(&stream, "G1 X50.000 Z0.000 F120.000") ||
           expect_line(&stream, "G1 X50.000 Z-10.000") ||
           expect_line(&stream, "G1 X40.000 Z-10.000") ||
           expect_line(&stream, "G0 X52.500") ||
           expect_line(&stream, "G0 Z1.250") ||
           expect_line(&stream, "G0 X80 Z0");
}

/* RUN, preview and the contour markers share one block scan, so it is tested
   directly: numbered range, two-line header and plain G80 cycle. */
static int test_g7x_block_scan(void)
{
    nc_document_t doc;
    uint32_t p = 0u;
    uint32_t q = 0u;
    size_t end = 0u;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G71 U1 R1");
    (void)nc_insert_line(&doc, 1, "G71 P100 Q200 U0.5 W0.25 F120");
    (void)nc_insert_line(&doc, 2, "N100 G1 X50 Z0");
    (void)nc_insert_line(&doc, 3, "G1 X50 Z-10");
    (void)nc_insert_line(&doc, 4, "N200 G1 X40 Z-10");
    (void)nc_insert_line(&doc, 5, "G0 X80 Z0");

    if (!nc_g7x_line_is_header("G71 U1 R1") ||
        nc_g7x_line_is_header("G0 X80") ||
        !nc_g7x_line_range("G71 P100 Q200", &p, &q) || p != 100u || q != 200u ||
        nc_g7x_line_range("G71 P200 Q100", &p, &q) ||
        nc_g7x_line_range("G71 P100", &p, &q) ||
        !nc_g7x_line_has_range_words("G71 P100") ||
        nc_g7x_line_has_range_words("G71 U1 R1")) {
        printf("FAIL block scan words hdr=%d none=%d range=%d p=%u q=%u\n",
               (int)nc_g7x_line_is_header("G71 U1 R1"),
               (int)nc_g7x_line_is_header("G0 X80"),
               (int)nc_g7x_line_range("G71 P100 Q200", &p, &q), p, q);
        return 1;
    }
    if (nc_g7x_block_start(&doc, 1u) != 0u ||
        nc_g7x_block_start(&doc, 2u) != 2u ||
        !nc_g7x_block_end(&doc, 0u, &end) || end != 4u ||
        !nc_g7x_block_end(&doc, 1u, &end) || end != 4u) {
        printf("FAIL block scan span end=%lu\n", (unsigned long)end);
        return 1;
    }
    if (!nc_g7x_line_is_contour(&doc, 0u, 2u) ||
        nc_g7x_line_is_contour(&doc, 0u, 1u) ||
        nc_g7x_line_is_contour(&doc, 0u, 5u)) {
        puts("FAIL block scan contour range");
        return 1;
    }

    /* Plain G80 cycle: the terminator is the end and is not contour. */
    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G71 U1 R1 X0.5 Z0.25 F120");
    (void)nc_insert_line(&doc, 1, "G1 X50 Z0");
    (void)nc_insert_line(&doc, 2, "G1 X50 Z-10");
    (void)nc_insert_line(&doc, 3, "G80");
    (void)nc_insert_line(&doc, 4, "G0 X80 Z0");
    if (!nc_g7x_block_end(&doc, 0u, &end) || end != 3u ||
        nc_g7x_line_is_contour(&doc, 0u, 3u) ||
        !nc_g7x_line_is_contour(&doc, 0u, 1u) ||
        !nc_g7x_line_is_contour(&doc, 0u, 2u)) {
        printf("FAIL block scan g80 end=%lu\n", (unsigned long)end);
        return 1;
    }

    /* An incomplete block is reported instead of guessed at. */
    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G71 U1 R1 P100 Q200 X0.5 Z0.25 F120");
    (void)nc_insert_line(&doc, 1, "N100 G1 X50 Z0");
    if (nc_g7x_block_end(&doc, 0u, &end)) {
        puts("FAIL block scan incomplete");
        return 1;
    }
    return 0;
}

/* Word editing must respect the letter: a G code may only become a supported
   command (the cycle family switches inside itself), and unsigned words must
   never take a sign. This is the path both the machine UI and the desktop panel
   use, so the rules cannot be bypassed by a caller. */
/* Up/Down walk between words with the same letter and never write a value. */
static int test_same_field_navigation(void)
{
    nc_document_t doc;
    nc_word_t word;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G1 X50 Z0");
    (void)nc_insert_line(&doc, 1, "G1 Z-10");
    (void)nc_insert_line(&doc, 2, "G1 X40 Z-20");

    doc.cursor_line = 0;
    doc.selected_word = 1; /* X50 */
    if (nc_get_selected_word(&doc, &word) != NC_OK || word.letter != 'X') {
        puts("FAIL field navigation start");
        return 1;
    }
    if (nc_select_same_word_next(&doc) != NC_OK || doc.cursor_line != 2u ||
        nc_get_selected_word(&doc, &word) != NC_OK || word.letter != 'X') {
        printf("FAIL next X line=%lu\n", (unsigned long)doc.cursor_line);
        return 1;
    }
    if (nc_select_same_word_next(&doc) != NC_ERR_NO_WORD) {
        puts("FAIL next X past the end");
        return 1;
    }
    if (nc_select_same_word_prev(&doc) != NC_OK || doc.cursor_line != 0u) {
        printf("FAIL previous X line=%lu\n", (unsigned long)doc.cursor_line);
        return 1;
    }
    /* Navigation must not have touched the program text. */
    if (strcmp(doc.lines[0].text, "G1 X50 Z0") != 0 ||
        strcmp(doc.lines[1].text, "G1 Z-10") != 0 ||
        strcmp(doc.lines[2].text, "G1 X40 Z-20") != 0) {
        puts("FAIL field navigation wrote text");
        return 1;
    }
    return 0;
}

static int test_word_value_validation(void)
{
    nc_document_t doc;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G72 U1 R1");
    (void)nc_insert_line(&doc, 1, "G1 X5 F100");

    doc.cursor_line = 0;
    doc.selected_word = 0; /* G72 */
    {
        nc_result_t r = nc_set_selected_word_text(&doc, "99");
        if (r != NC_ERR_BAD_VALUE) {
            printf("FAIL G99 accepted rc=%d text=[%s]\n", (int)r, doc.lines[0].text);
            return 1;
        }
    }
    if (nc_set_selected_word_text(&doc, "99") != NC_ERR_BAD_VALUE ||
        nc_set_selected_word_text(&doc, "-71") != NC_ERR_BAD_VALUE ||
        nc_set_selected_word_text(&doc, "7.1") != NC_ERR_BAD_VALUE ||
        nc_set_selected_word_text(&doc, "") != NC_ERR_BAD_VALUE ||
        strcmp(doc.lines[0].text, "G72 U1 R1") != 0) {
        printf("FAIL G whitelist text=[%s]\n", doc.lines[0].text);
        return 1;
    }
    if (nc_set_selected_word_text(&doc, "71") != NC_OK ||
        strcmp(doc.lines[0].text, "G71 U1 R1") != 0) {
        printf("FAIL G71 switch text=[%s]\n", doc.lines[0].text);
        return 1;
    }

    doc.cursor_line = 1;
    doc.selected_word = 1; /* X5 may be negative */
    if (nc_set_selected_word_text(&doc, "-5.25") != NC_OK ||
        strcmp(doc.lines[1].text, "G1 X-5.25 F100") != 0) {
        printf("FAIL signed X text=[%s]\n", doc.lines[1].text);
        return 1;
    }
    doc.selected_word = 2; /* F100 may not */
    if (nc_set_selected_word_text(&doc, "-100") != NC_ERR_BAD_VALUE) {
        puts("FAIL negative feed accepted");
        return 1;
    }
    return 0;
}

/* Keyboard map sanity.

   The mapping is expected to grow (multi-level menus, field prompts, maybe
   letter keys), so the point of this test is not the current table but the
   obligation to extend it: every key the screen can receive must be classified
   here, and the class counts pin the shape of the map. Adding a key without
   classifying it fails this test. */
typedef enum {
    KEY_CLASS_INVALID = 0,
    KEY_CLASS_NONE,
    KEY_CLASS_DIGIT,
    KEY_CLASS_CONTROL,
    KEY_CLASS_NAV_LINE,
    KEY_CLASS_NAV_WORD,
    KEY_CLASS_NAV_FIELD,
    KEY_CLASS_CHAR
} key_class_t;

static key_class_t key_class_of(nc_visual_key_t key)
{
    switch (key) {
    case NC_VISUAL_KEY_NONE: return KEY_CLASS_NONE;
    case NC_VISUAL_KEY_DIGIT_0:
    case NC_VISUAL_KEY_DIGIT_1:
    case NC_VISUAL_KEY_DIGIT_2:
    case NC_VISUAL_KEY_DIGIT_3:
    case NC_VISUAL_KEY_DIGIT_4:
    case NC_VISUAL_KEY_DIGIT_5:
    case NC_VISUAL_KEY_DIGIT_6:
    case NC_VISUAL_KEY_DIGIT_7:
    case NC_VISUAL_KEY_DIGIT_8:
    case NC_VISUAL_KEY_DIGIT_9: return KEY_CLASS_DIGIT;
    case NC_VISUAL_KEY_BACKSPACE:
    case NC_VISUAL_KEY_FINISH:
    case NC_VISUAL_KEY_CANCEL:
    case NC_VISUAL_KEY_ACCEPT:
    case NC_VISUAL_KEY_MODE: return KEY_CLASS_CONTROL;
    case NC_VISUAL_KEY_PREV:
    case NC_VISUAL_KEY_NEXT: return KEY_CLASS_NAV_LINE;
    case NC_VISUAL_KEY_WORD_PREV:
    case NC_VISUAL_KEY_WORD_NEXT: return KEY_CLASS_NAV_WORD;
    case NC_VISUAL_KEY_FIELD_PREV:
    case NC_VISUAL_KEY_FIELD_NEXT: return KEY_CLASS_NAV_FIELD;
    case NC_VISUAL_KEY_MINUS:
    case NC_VISUAL_KEY_DOT: return KEY_CLASS_CHAR;
    }
    return KEY_CLASS_INVALID;
}

static int test_key_map_sanity(void)
{
    unsigned digits = 0u;
    unsigned controls = 0u;
    unsigned nav_line = 0u;
    unsigned nav_word = 0u;
    unsigned nav_field = 0u;
    unsigned chars = 0u;
    unsigned none = 0u;
    int key;

    for (key = 0; key <= (int)NC_VISUAL_KEY_DOT; key++) {
        switch (key_class_of((nc_visual_key_t)key)) {
        case KEY_CLASS_NONE: none++; break;
        case KEY_CLASS_DIGIT: digits++; break;
        case KEY_CLASS_CONTROL: controls++; break;
        case KEY_CLASS_NAV_LINE: nav_line++; break;
        case KEY_CLASS_NAV_WORD: nav_word++; break;
        case KEY_CLASS_NAV_FIELD: nav_field++; break;
        case KEY_CLASS_CHAR: chars++; break;
        case KEY_CLASS_INVALID:
        default:
            printf("FAIL unclassified key %d - extend the key map table\n", key);
            return 1;
        }
    }
    if (digits != 10u || controls != 5u || nav_line != 2u ||
        nav_word != 2u || nav_field != 2u || chars != 2u || none != 1u) {
        printf("FAIL key classes digits=%u controls=%u line=%u word=%u "
               "field=%u chars=%u none=%u\n",
               digits, controls, nav_line, nav_word, nav_field, chars, none);
        return 1;
    }
    return 0;
}

/* NC supplies program text through the G7x source contract, not the other way
   around: the cursor returns numbered blocks in document order. */
static int test_numbered_source_cursor(void)
{
    nc_document_t doc;
    nc_numbered_source_t holder;
    g7x_source_t source;
    g7x_source_pos_t pos = { 0u, 0u };
    g7x_source_pos_t found = { 0u, 0u };
    char text[96];
    const uint32_t want[3] = { 100u, 150u, 200u };
    unsigned i;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "(profile)");
    (void)nc_insert_line(&doc, 1, "N100 G1 X50 Z0");
    (void)nc_insert_line(&doc, 2, "G1 X50 Z-10");
    (void)nc_insert_line(&doc, 3, "N150 G1 X45");
    (void)nc_insert_line(&doc, 4, "N200 G1 X40");
    source = nc_emit_numbered_source(&holder, &doc);

    for (i = 0; i < 3u; i++) {
        if (g7x_source_next(&source, &pos, &found, text, sizeof(text)) != G7X_SOURCE_OK ||
            found.number != want[i] || !strstr(text, "G1"))
            return 1;
        pos = found;
    }
    if (g7x_source_next(&source, &pos, &found, text, sizeof(text)) != G7X_SOURCE_MISSING)
        return 1;
    if (g7x_source_next(NULL, &pos, &found, text, sizeof(text)) != G7X_SOURCE_ERROR)
        return 1;
    return 0;
}

static int test_g7_g8_passthrough(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G7");
    (void)nc_insert_line(&doc, 1, "G8");
    nc_emit_stream_begin(&stream, &doc, 0);

    return expect_line(&stream, "G7") ||
           expect_line(&stream, "G8");
}

static int test_g71_corner_rounding(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G71 U2 R1 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 1, "G1 X50 Z0");
    (void)nc_insert_line(&doc, 2, "G1 X50 Z-10 R1");
    (void)nc_insert_line(&doc, 3, "G1 X40 Z-10");
    (void)nc_insert_line(&doc, 4, "G1 X40 Z-20");
    (void)nc_insert_line(&doc, 5, "G80");
    nc_emit_stream_begin(&stream, &doc, 0);

    return expect_find_line(&stream, "(G7x finish contour)") ||
           expect_line(&stream, "G0 X52.500") ||
           expect_line(&stream, "G0 Z1.500") ||
           expect_line(&stream, "G1 X50.000 Z0.000 F120.000") ||
           expect_line(&stream, "G1 X50.000 Z-9.000") ||
           expect_line(&stream, "G3 X48.000 Z-10.000 I-1.000 K0.000");
}

static int test_g71_corner_chamfer(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G71 U2 R1 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 1, "G1 X50 Z0");
    (void)nc_insert_line(&doc, 2, "G1 X50 Z-10 C1");
    (void)nc_insert_line(&doc, 3, "G1 X40 Z-10");
    (void)nc_insert_line(&doc, 4, "G1 X40 Z-20");
    (void)nc_insert_line(&doc, 5, "G80");
    nc_emit_stream_begin(&stream, &doc, 0);

    return expect_find_line(&stream, "(G7x finish contour)") ||
           expect_line(&stream, "G0 X52.500") ||
           expect_line(&stream, "G0 Z1.500") ||
           expect_line(&stream, "G1 X50.000 Z0.000 F120.000") ||
           expect_line(&stream, "G1 X50.000 Z-9.000") ||
           expect_line(&stream, "G1 X48.000 Z-10.000");
}

static int test_g71_corner_chamfer_source_line(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G71 U2 R1 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 1, "G1 X50 Z0");
    (void)nc_insert_line(&doc, 2, "G1 X50 Z-10 C1");
    (void)nc_insert_line(&doc, 3, "G1 X40 Z-10");
    (void)nc_insert_line(&doc, 4, "G1 X40 Z-20");
    (void)nc_insert_line(&doc, 5, "G80");
    nc_emit_stream_begin(&stream, &doc, 0);

    return expect_find_line_source(&stream, "G1 X50.000 Z-9.000", 2) ||
           expect_find_line_source(&stream, "G1 X48.000 Z-10.000", 2);
}

static int test_g71_sample_features(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G970 X-5 U60 Z-60 W5");
    (void)nc_insert_line(&doc, 1, "G971 X50 Z50 I0 E0");
    (void)nc_insert_line(&doc, 2, "G972 C12");
    (void)nc_insert_line(&doc, 3, "G973 P7");
    (void)nc_insert_line(&doc, 4, "G71 U2 R1 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 5, "G1 X50 Z0");
    (void)nc_insert_line(&doc, 6, "G1 X35 Z-10 C1.5");
    (void)nc_insert_line(&doc, 7, "G1 X30 Z-20 R2");
    (void)nc_insert_line(&doc, 8, "G2 X42 Z-26 R4");
    (void)nc_insert_line(&doc, 9, "G1 X50 Z-26");
    (void)nc_insert_line(&doc, 10, "G80");
    nc_emit_stream_begin(&stream, &doc, 0);

    /* This profile reverses X and is outside the monotonic subset. */
    return expect_rejected(&stream);
}

static int test_g72_basic(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G72 W2 R1 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 1, "G1 X10 Z-25");
    (void)nc_insert_line(&doc, 2, "G1 X40 Z-25 C0 R0");
    (void)nc_insert_line(&doc, 3, "G1 X40 Z0");
    (void)nc_insert_line(&doc, 4, "G80");
    nc_emit_stream_begin(&stream, &doc, 0);

    return expect_find_line(&stream, "(G72 rough Z-23.000)") ||
           expect_find_line(&stream, "G1 X10.000 Z-25.000 F120.000") ||
           expect_find_line(&stream, "G1 X40.000 Z-25.000") ||
           expect_find_line(&stream, "G1 X40.000 Z0.000");
}

static int test_g72_rectangle(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G72 W1 R1 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 1, "G1 X50 Z0");
    (void)nc_insert_line(&doc, 2, "G1 X50 Z-5 C0 R0");
    (void)nc_insert_line(&doc, 3, "G1 X5 Z-5 C0 R0");
    (void)nc_insert_line(&doc, 4, "G1 X5 Z2");
    (void)nc_insert_line(&doc, 5, "G80");
    nc_emit_stream_begin(&stream, &doc, 0);

    return expect_rejected(&stream);
}

static int test_g72_arc_finish(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G72 W2 R1 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 1, "G1 X10 Z-25");
    (void)nc_insert_line(&doc, 2, "G2 X40 Z-25 R20");
    (void)nc_insert_line(&doc, 3, "G1 X40 Z0");
    (void)nc_insert_line(&doc, 4, "G80");
    nc_emit_stream_begin(&stream, &doc, 0);

    /* Equal endpoint Z hides an interior arc reversal. */
    return expect_rejected(&stream);
}

static int test_g72_corner_rounding(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G72 W1 R1 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 1, "G1 X50 Z2 C0 R0");
    (void)nc_insert_line(&doc, 2, "G1 X50 Z-10");
    (void)nc_insert_line(&doc, 3, "G1 X25 Z-5 C0 R5");
    (void)nc_insert_line(&doc, 4, "G1 X25 Z2");
    (void)nc_insert_line(&doc, 5, "G80");
    nc_emit_stream_begin(&stream, &doc, 0);

    return expect_rejected(&stream);
}

int main(void)
{
    int fails = 0;

    fails += test_word_value_validation();
    fails += test_same_field_navigation();
    fails += test_key_map_sanity();
    fails += test_g7_g8_passthrough();
    fails += test_g71_corner_rounding();
    fails += test_g71_corner_chamfer();
    fails += test_g71_corner_chamfer_source_line();
    fails += test_g71_sample_features();
    fails += test_g71_numbered_range();
    fails += test_g71_numbered_range_errors();
    fails += test_g71_two_line_numbered_range();
    fails += test_g71_two_line_g80();
    fails += test_g7x_block_scan();
    fails += test_numbered_source_cursor();
    fails += test_g72_basic();
    fails += test_g72_rectangle();
    fails += test_g72_arc_finish();
    fails += test_g72_corner_rounding();
    if (fails) {
        printf("NC emit host tests failed: %d\n", fails);
        return 1;
    }
    printf("NC emit host tests passed\n");
    return 0;
}

#endif
