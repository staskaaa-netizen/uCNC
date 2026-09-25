#include "g7x_blocks.h"

#include "g7x_contour.h"

#include <math.h>
#include <stdio.h>

static const char *g7x_doc_at(const g7x_doc_t *doc, size_t index)
{
    const char *line;

    if (!doc || !doc->line || index >= doc->count) {
        return "";
    }
    line = doc->line(doc->user, index);
    return line ? line : "";
}

bool g7x_doc_line_is_header(const char *line)
{
    g7x_cycle_t cycle = g7x_cycle_from_line(line);

    return cycle == G7X_CYCLE_G71 || cycle == G7X_CYCLE_G72;
}

bool g7x_doc_line_range(const char *line, uint32_t *p, uint32_t *q)
{
    float pv;
    float qv;
    bool has_p;
    bool has_q;

    if (!line || !p || !q)
        return false;
    has_p = g7x_get_field_float(line, "P", &pv);
    has_q = g7x_get_field_float(line, "Q", &qv);
    if (!has_p || !has_q ||
        !isfinite(pv) || !isfinite(qv) || pv < 1.0f || qv < 1.0f ||
        floorf(pv) != pv || floorf(qv) != qv || qv < pv)
        return false;
    *p = (uint32_t)pv;
    *q = (uint32_t)qv;
    return true;
}

bool g7x_doc_line_has_range_words(const char *line)
{
    float value;

    if (!line)
        return false;
    return g7x_get_field_float(line, "P", &value) ||
           g7x_get_field_float(line, "Q", &value);
}

/* Index of the first non-blank line after `line`, or doc->count. */
static size_t g7x_doc_next_content_line(const g7x_doc_t *doc, size_t line)
{
    size_t i = line + 1u;

    while (i < doc->count && !*g7x_doc_at(doc, i))
        i++;
    return i;
}

/* Last line of the header: the second block of a Fanuc two-line header. */
static size_t g7x_doc_header_end(const g7x_doc_t *doc, size_t start_line)
{
    size_t next;

    if (!g7x_doc_line_is_header(g7x_doc_at(doc, start_line)))
        return start_line;
    next = g7x_doc_next_content_line(doc, start_line);
    if (next >= doc->count)
        return start_line;
    if (!g7x_doc_line_is_header(g7x_doc_at(doc, next)))
        return start_line;
    /* Same cycle repeated with no contour row between: Fanuc second block. */
    if (g7x_cycle_from_line(g7x_doc_at(doc, next)) !=
        g7x_cycle_from_line(g7x_doc_at(doc, start_line)))
        return start_line;
    return next;
}

/* The line a header belongs to: the *first* block of a Fanuc pair for both of
   its lines, the line itself otherwise. Either line of the pair may carry the
   P/Q range (the usual form puts it on the second, the bench's file on the
   first), and which one does must not change which block a row is in - a cycle
   whose rows answered with two different blocks handed the machine a headerless
   profile when a step was taken from inside the contour. */
static size_t g7x_doc_header_head(const g7x_doc_t *doc, size_t line)
{
    size_t previous = line;

    if (!doc || line >= doc->count)
        return line;
    while (previous > 0u) {
        previous--;
        if (*g7x_doc_at(doc, previous))
            break;
    }
    if (previous != line &&
        g7x_doc_line_is_header(g7x_doc_at(doc, previous)) &&
        g7x_doc_header_end(doc, previous) == line)
        return previous;
    return line;
}

size_t g7x_doc_block_start(const g7x_doc_t *doc, size_t line)
{
    if (!doc || line >= doc->count)
        return line;
    if (!g7x_doc_line_is_header(g7x_doc_at(doc, line)))
        return line;
    return g7x_doc_header_head(doc, line);
}

/* Header line that carries the P/Q selection: the first line of a one-line
   header, or whichever line of a Fanuc two-line header names the range. */
static size_t g7x_doc_range_header(const g7x_doc_t *doc, size_t start_line)
{
    size_t header = g7x_doc_header_end(doc, start_line);
    uint32_t p = 0u;
    uint32_t q = 0u;

    if (g7x_doc_line_range(g7x_doc_at(doc, header), &p, &q))
        return header;
    if (g7x_doc_line_range(g7x_doc_at(doc, start_line), &p, &q))
        return start_line;
    return start_line;
}

bool g7x_doc_block_end(const g7x_doc_t *doc, size_t start_line, size_t *end_line)
{
    size_t header;
    size_t i;
    uint32_t p = 0u;
    uint32_t q = 0u;

    if (!doc || !end_line || start_line >= doc->count)
        return false;
    if (!g7x_doc_line_is_header(g7x_doc_at(doc, start_line)))
        return false;

    header = g7x_doc_range_header(doc, start_line);
    if (g7x_doc_line_range(g7x_doc_at(doc, header), &p, &q)) {
        /* Numbered range: N(Q) closes the profile. */
        uint32_t number = 0u;
        for (i = header + 1u; i < doc->count; i++) {
            if (g7x_line_number(g7x_doc_at(doc, i), &number) && number == q) {
                size_t next = g7x_doc_next_content_line(doc, i);

                /* A `G80` written after the range is the cycle's end mark, and
                   it belongs to the same block. The two ways of writing a cycle
                   have to answer with the same rows: the block is what a step
                   hands the machine, so a range-terminated cycle that left its
                   end mark behind was sent in two pieces and the pane drew a
                   different pale block from the one a `G80`-terminated cycle
                   drew (bench: "now it colorize both g71 and path if any g71 is
                   select. with pq or with g80"). */
                if (next < doc->count &&
                    g7x_contour_cmd_from_line(g7x_doc_at(doc, next)) ==
                        G7X_CONTOUR_END) {
                    i = next;
                }
                *end_line = i;
                return true;
            }
        }
        return false;
    }

    for (i = start_line + 1u; i < doc->count; i++) {
        if (g7x_contour_cmd_from_line(g7x_doc_at(doc, i)) == G7X_CONTOUR_END) {
            *end_line = i;
            return true;
        }
    }
    return false;
}

bool g7x_doc_block_containing(const g7x_doc_t *doc,
                              size_t line,
                              size_t *start_line,
                              size_t *end_line)
{
    size_t i;

    if (start_line)
        *start_line = line;
    if (end_line)
        *end_line = line;
    if (!doc || line >= doc->count)
        return false;

    /* The closest header at or above the line whose block reaches it. Headers
       cannot nest, so the first one that closes over the line is the block it
       belongs to - and a header is asked for the *pair* it heads, so the second
       line of a two-line header answers with the block the first one does. */
    i = line + 1u;
    while (i > 0u) {
        size_t candidate = i - 1u;
        size_t head;
        size_t block_end;

        if (!g7x_doc_line_is_header(g7x_doc_at(doc, candidate))) {
            i--;
            continue;
        }
        head = g7x_doc_header_head(doc, candidate);
        if (g7x_doc_block_end(doc, head, &block_end) && block_end >= line) {
            if (start_line)
                *start_line = head;
            if (end_line)
                *end_line = block_end;
            return true;
        }
        i--;
    }
    return false;
}

bool g7x_doc_range_above(const g7x_doc_t *doc,
                         size_t line,
                         uint32_t p,
                         uint32_t q,
                         size_t *first,
                         size_t *last)
{
    size_t found_first = (size_t)-1;
    size_t found_last = (size_t)-1;
    size_t i;
    uint32_t number = 0u;

    if (!doc || line == 0u || line > doc->count || p == 0u || q < p) {
        return false;
    }
    /* Below the line that names the range, and no further back than the row that
       closes it: N(Q) first, then N(P). */
    for (i = line; i > 0u; i--) {
        const char *text = g7x_doc_at(doc, i - 1u);

        if (!g7x_line_number(text, &number) || number == 0u) {
            continue;
        }
        if (found_last == (size_t)-1) {
            if (number == q) {
                found_last = i - 1u;
            }
            continue;
        }
        if (number == p) {
            found_first = i - 1u;
            break;
        }
        if (number < p) {
            break;              /* walked past the range without meeting N(P) */
        }
    }
    if (found_first == (size_t)-1 || found_last == (size_t)-1 ||
        found_first > found_last) {
        return false;
    }
    if (first) {
        *first = found_first;
    }
    if (last) {
        *last = found_last;
    }
    return true;
}

bool g7x_doc_line_path(const g7x_doc_t *doc,
                       size_t line,
                       size_t *first,
                       size_t *last)
{
    uint32_t p = 0u;
    uint32_t q = 0u;

    if (first) {
        *first = line;
    }
    if (last) {
        *last = line;
    }
    if (!doc || line >= doc->count) {
        return false;
    }
    /* A row inside a cycle belongs with the cycle's block. */
    if (g7x_doc_block_containing(doc, line, first, last)) {
        return true;
    }
    /* A finish cut names a range instead of *being* one: it replays rows the
       roughing cycle collected above it. */
    if (g7x_cycle_from_line(g7x_doc_at(doc, line)) != G7X_CYCLE_G70 ||
        !g7x_doc_line_range(g7x_doc_at(doc, line), &p, &q)) {
        return false;
    }
    return g7x_doc_range_above(doc, line, p, q, first, last);
}

bool g7x_doc_line_is_contour(const g7x_doc_t *doc, size_t start_line, size_t index)
{
    size_t end;
    size_t first;
    uint32_t p = 0u;
    uint32_t q = 0u;

    if (!doc || index >= doc->count)
        return false;
    if (!g7x_doc_block_end(doc, start_line, &end))
        return false;
    first = g7x_doc_header_end(doc, start_line) + 1u;
    if (index < first)
        return false;
    if (g7x_doc_line_range(g7x_doc_at(doc, g7x_doc_range_header(doc, start_line)), &p, &q))
        return index <= end;
    return index < end;
}

bool g7x_doc_line_is_any_contour(const g7x_doc_t *doc, size_t index)
{
    size_t i;

    if (!doc || index >= doc->count)
        return false;
    for (i = 0; i <= index; i++) {
        if (!g7x_doc_line_is_header(g7x_doc_at(doc, i)))
            continue;
        if (g7x_doc_line_is_contour(doc, i, index))
            return true;
    }
    return false;
}
