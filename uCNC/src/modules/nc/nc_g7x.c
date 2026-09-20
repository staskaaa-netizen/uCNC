/* Document scanning for G7x blocks: numbered ranges, Fanuc two-line headers and
   plain G80-terminated cycles. Text only; no machine state. */
#include "nc_g7x.h"

#include "../g7x/g7x_contour.h"

#include <math.h>

static const char *nc_g7x_trim(const char *line)
{
    while (line && (*line == ' ' || *line == '\t'))
        line++;
    return line ? line : "";
}

static const char *nc_g7x_line(const nc_document_t *doc, size_t index)
{
    if (!doc || index >= doc->line_count)
        return "";
    return nc_g7x_trim(doc->lines[index].text);
}

bool nc_g7x_line_is_header(const char *line)
{
    g7x_cycle_t cycle = g7x_cycle_from_line(line);

    return cycle == G7X_CYCLE_G71 || cycle == G7X_CYCLE_G72;
}

bool nc_g7x_line_range(const char *line, uint32_t *p, uint32_t *q)
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

bool nc_g7x_line_has_range_words(const char *line)
{
    float value;

    if (!line)
        return false;
    return g7x_get_field_float(line, "P", &value) ||
           g7x_get_field_float(line, "Q", &value);
}

/* Index of the first non-blank line after `line`, or doc->line_count. */
static size_t nc_g7x_next_content_line(const nc_document_t *doc, size_t line)
{
    size_t i = line + 1u;

    while (i < doc->line_count && !*nc_g7x_line(doc, i))
        i++;
    return i;
}

/* Last line of the header: the second block of a Fanuc two-line header. */
static size_t nc_g7x_header_end(const nc_document_t *doc, size_t start_line)
{
    size_t next;

    if (!nc_g7x_line_is_header(nc_g7x_line(doc, start_line)))
        return start_line;
    next = nc_g7x_next_content_line(doc, start_line);
    if (next >= doc->line_count)
        return start_line;
    if (!nc_g7x_line_is_header(nc_g7x_line(doc, next)))
        return start_line;
    /* Same cycle repeated with no contour row between: Fanuc second block. */
    if (g7x_cycle_from_line(nc_g7x_line(doc, next)) !=
        g7x_cycle_from_line(nc_g7x_line(doc, start_line)))
        return start_line;
    return next;
}

size_t nc_g7x_block_start(const nc_document_t *doc, size_t line)
{
    uint32_t p = 0u;
    uint32_t q = 0u;
    size_t previous;

    if (!doc || line >= doc->line_count)
        return line;
    if (!nc_g7x_line_is_header(nc_g7x_line(doc, line)) ||
        !nc_g7x_line_range(nc_g7x_line(doc, line), &p, &q))
        return line;

    /* The P/Q block of a two-line header belongs to the line above it. */
    previous = line;
    while (previous > 0u) {
        previous--;
        if (*nc_g7x_line(doc, previous))
            break;
    }
    if (previous != line &&
        nc_g7x_line_is_header(nc_g7x_line(doc, previous)) &&
        nc_g7x_header_end(doc, previous) == line)
        return previous;
    return line;
}

/* Header line that carries the P/Q selection: the first line of a one-line
   header, or the second line of a Fanuc two-line header. */
static size_t nc_g7x_range_header(const nc_document_t *doc, size_t start_line)
{
    size_t header = nc_g7x_header_end(doc, start_line);
    uint32_t p = 0u;
    uint32_t q = 0u;

    if (nc_g7x_line_range(nc_g7x_line(doc, header), &p, &q))
        return header;
    if (nc_g7x_line_range(nc_g7x_line(doc, start_line), &p, &q))
        return start_line;
    return start_line;
}

bool nc_g7x_block_end(const nc_document_t *doc,
                      size_t start_line,
                      size_t *end_line)
{
    size_t header;
    size_t i;
    uint32_t p = 0u;
    uint32_t q = 0u;

    if (!doc || !end_line || start_line >= doc->line_count)
        return false;
    if (!nc_g7x_line_is_header(nc_g7x_line(doc, start_line)))
        return false;

    header = nc_g7x_range_header(doc, start_line);
    if (nc_g7x_line_range(nc_g7x_line(doc, header), &p, &q)) {
        /* Numbered range: N(Q) closes the profile. */
        uint32_t number = 0u;
        for (i = header + 1u; i < doc->line_count; i++) {
            if (g7x_line_number(nc_g7x_line(doc, i), &number) && number == q) {
                *end_line = i;
                return true;
            }
        }
        return false;
    }

    for (i = start_line + 1u; i < doc->line_count; i++) {
        if (g7x_contour_cmd_from_line(nc_g7x_line(doc, i)) == G7X_CONTOUR_END) {
            *end_line = i;
            return true;
        }
    }
    return false;
}

bool nc_g7x_line_is_contour(const nc_document_t *doc,
                            size_t start_line,
                            size_t index)
{
    size_t end;
    size_t first;
    uint32_t p = 0u;
    uint32_t q = 0u;

    if (!doc || index >= doc->line_count)
        return false;
    if (!nc_g7x_block_end(doc, start_line, &end))
        return false;
    first = nc_g7x_header_end(doc, start_line) + 1u;
    if (index < first)
        return false;
    if (nc_g7x_line_range(nc_g7x_line(doc, nc_g7x_range_header(doc, start_line)), &p, &q))
        return index <= end;
    return index < end;
}

bool nc_g7x_line_is_any_contour(const nc_document_t *doc, size_t index)
{
    size_t i;

    if (!doc || index >= doc->line_count)
        return false;
    for (i = 0; i <= index; i++) {
        if (!nc_g7x_line_is_header(doc->lines[i].text))
            continue;
        if (nc_g7x_line_is_contour(doc, i, index))
            return true;
    }
    return false;
}
