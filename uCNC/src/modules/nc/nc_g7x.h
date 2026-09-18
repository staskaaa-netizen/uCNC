#ifndef NC_G7X_H
#define NC_G7X_H

#include "nc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NC-owned view of one G7x block inside a document.

   G7x owns the cycle semantics; NC only needs to know where a block starts and
   ends so RUN, the preview and the editor agree on what a single block is.
   Keeping that scan in one place avoids a second, drifting copy of the dialect
   in each screen. */

/* G71/G72 header line, ignoring a leading N word. */
bool nc_g7x_line_is_header(const char *line);

/* P/Q range of a header line. False when either word is absent, non-integral,
   below 1 or when Q < P. */
bool nc_g7x_line_range(const char *line, uint32_t *p, uint32_t *q);

/* True when the line carries a P or Q word at all, so a malformed pair can be
   reported instead of silently treated as a G80-terminated cycle. */
bool nc_g7x_line_has_range_words(const char *line);

/* First line of the block that owns `line`. The second line of a Fanuc two-line
   header maps back to its first line; anything else returns `line`. */
size_t nc_g7x_block_start(const nc_document_t *doc, size_t line);

/* Last line of the block: the N(Q) block of a numbered range, otherwise the
   G80 line. False when the block is incomplete or malformed. */
bool nc_g7x_block_end(const nc_document_t *doc,
                      size_t start_line,
                      size_t *end_line);

/* True when `index` carries contour geometry inside the block at `start_line`.
   The N(Q) block of a numbered range counts as contour; a G80 terminator does
   not. */
bool nc_g7x_line_is_contour(const nc_document_t *doc,
                            size_t start_line,
                            size_t index);

#ifdef __cplusplus
}
#endif

#endif
