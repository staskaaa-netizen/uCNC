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

/* Last line of the block: the cycle's end mark. A numbered range ends at N(Q) -
   or at the G80 written after it, when the file has one - and a cycle with no
   range ends at its G80. False when the block is incomplete or malformed. */
bool nc_g7x_block_end(const nc_document_t *doc,
                      size_t start_line,
                      size_t *end_line);

/* The block that owns `line`: the first header line and the end mark of the
   cycle block whose header sits at or above it. False when the line is outside
   every block - which is how the path builder tells "nothing to continue here"
   from "a block whose header never closes", the two cases it answers
   differently. */
bool nc_g7x_block_containing(const nc_document_t *doc,
                             size_t line,
                             size_t *start_line,
                             size_t *end_line);

/* True when `index` carries contour geometry inside the block at `start_line`.
   The N(Q) block of a numbered range counts as contour; a G80 terminator does
   not. */
bool nc_g7x_line_is_contour(const nc_document_t *doc,
                            size_t start_line,
                            size_t index);

/* The numbered range that sits *above* `line`: walk back to the N(Q) row, then
   up to the N(P) row. `G70 P Q` names the profile the roughing cycle already
   collected, so its rows are before it - the preview feeds them
   (`nc_emit_feed_g7x_range_above()`) and the pane marks them, from this one
   answer, so the two cannot disagree about which lines a G70 owns. False when
   the range is not there: it is reported, never guessed. */
bool nc_g7x_range_above(const nc_document_t *doc,
                        size_t line,
                        uint32_t p,
                        uint32_t q,
                        size_t *first,
                        size_t *last);

/* The path of `line`: the rows that belong with it. A row inside a cycle answers
   with the block it sits in; a `G70 P Q` answers with the numbered range the
   finish cut replays, which is above it. False when the line heads nothing of
   its own - the pane then marks that line alone. */
bool nc_g7x_line_path(const nc_document_t *doc,
                      size_t line,
                      size_t *first,
                      size_t *last);

/* True when `index` carries contour geometry of any block in the document.
   The preview marks these lines and the editor highlights them: one scan, so
   the two cannot disagree about what a contour line is. */
bool nc_g7x_line_is_any_contour(const nc_document_t *doc, size_t index);

#ifdef __cplusplus
}
#endif

#endif
