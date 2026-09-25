/* NC's view of the G7x blocks inside a document - see nc_g7x.h.

   The scan itself is g7x's (`g7x_blocks.c`): the rules for what a block is, what
   a numbered range is and which rows carry contour belong to the module that
   owns the cycles, and a screen asking the same question a second way is how the
   two answers drift. So this file is only the adapter - it hands g7x the lines
   of the document and nothing else. */
#include "nc_g7x.h"

#include "../g7x/g7x_blocks.h"

/* A line as the scan reads it: leading spaces skipped, because every reader
   there treats `  G71 ...` as the cycle it is. */
static const char *nc_g7x_provide(void *user, size_t index)
{
    const nc_document_t *doc = user;
    const char *line;

    if (!doc || index >= doc->line_count) {
        return "";
    }
    line = doc->lines[index].text;
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    return line;
}

static g7x_doc_t nc_g7x_doc(const nc_document_t *doc)
{
    g7x_doc_t d;

    d.line = doc ? nc_g7x_provide : 0;
    d.user = (void *)doc;
    d.count = doc ? doc->line_count : 0u;
    return d;
}

bool nc_g7x_line_is_header(const char *line)
{
    return g7x_doc_line_is_header(line);
}

bool nc_g7x_line_range(const char *line, uint32_t *p, uint32_t *q)
{
    return g7x_doc_line_range(line, p, q);
}

bool nc_g7x_line_has_range_words(const char *line)
{
    return g7x_doc_line_has_range_words(line);
}

size_t nc_g7x_block_start(const nc_document_t *doc, size_t line)
{
    g7x_doc_t d = nc_g7x_doc(doc);

    return g7x_doc_block_start(&d, line);
}

bool nc_g7x_block_end(const nc_document_t *doc,
                      size_t start_line,
                      size_t *end_line)
{
    g7x_doc_t d = nc_g7x_doc(doc);

    return g7x_doc_block_end(&d, start_line, end_line);
}

bool nc_g7x_block_containing(const nc_document_t *doc,
                             size_t line,
                             size_t *start_line,
                             size_t *end_line)
{
    g7x_doc_t d = nc_g7x_doc(doc);

    return g7x_doc_block_containing(&d, line, start_line, end_line);
}

bool nc_g7x_line_is_contour(const nc_document_t *doc,
                            size_t start_line,
                            size_t index)
{
    g7x_doc_t d = nc_g7x_doc(doc);

    return g7x_doc_line_is_contour(&d, start_line, index);
}

bool nc_g7x_line_is_any_contour(const nc_document_t *doc, size_t index)
{
    g7x_doc_t d = nc_g7x_doc(doc);

    return g7x_doc_line_is_any_contour(&d, index);
}

bool nc_g7x_range_above(const nc_document_t *doc,
                        size_t line,
                        uint32_t p,
                        uint32_t q,
                        size_t *first,
                        size_t *last)
{
    g7x_doc_t d = nc_g7x_doc(doc);

    return g7x_doc_range_above(&d, line, p, q, first, last);
}

bool nc_g7x_line_path(const nc_document_t *doc,
                      size_t line,
                      size_t *first,
                      size_t *last)
{
    g7x_doc_t d = nc_g7x_doc(doc);

    return g7x_doc_line_path(&d, line, first, last);
}
