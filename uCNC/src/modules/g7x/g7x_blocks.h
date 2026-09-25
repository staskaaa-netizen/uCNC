#ifndef G7X_BLOCKS_H
#define G7X_BLOCKS_H

#include "g7x.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where the cycles are in a program, read as text.

   The generator answers "what does this program do" for the stream it is fed;
   a screen has to answer the same question about the whole file - which rows are
   a block's profile, which block a row belongs to, and the range a finish cut
   names *above* it - before anything is sent. g7x owns the rules, so the scan
   lives here and the caller only supplies the lines: a program on a card, a
   buffer in a check, a document in a screen - g7x never reads a file or a
   document itself.

   The provider hands over **one line with its leading spaces already skipped**,
   because every reader below treats `  G71 ...` as the cycle it is. */

typedef const char *(*g7x_doc_line_fn)(void *user, size_t index);

typedef struct {
    g7x_doc_line_fn line;
    void *user;
    size_t count;
} g7x_doc_t;

/* G71/G72 header line, ignoring a leading N word. */
bool g7x_doc_line_is_header(const char *line);

/* P/Q range of a header line. False when either word is absent, non-integral,
   below 1 or when Q < P. */
bool g7x_doc_line_range(const char *line, uint32_t *p, uint32_t *q);

/* True when the line carries a P or Q word at all, so a malformed pair can be
   reported instead of silently treated as a G80-terminated cycle. */
bool g7x_doc_line_has_range_words(const char *line);

/* First line of the block that owns `line`. The second line of a Fanuc two-line
   header maps back to its first line; anything else returns `line`. */
size_t g7x_doc_block_start(const g7x_doc_t *doc, size_t line);

/* Last line of the block: the cycle's end mark. A numbered range ends at N(Q) -
   or at the G80 written after it, when the file has one - and a cycle with no
   range ends at its G80. False when the block is incomplete or malformed. */
bool g7x_doc_block_end(const g7x_doc_t *doc, size_t start_line, size_t *end_line);

/* The block that owns `line`: the first header line and the end mark of the
   cycle block whose header sits at or above it. False when the line is outside
   every block. */
bool g7x_doc_block_containing(const g7x_doc_t *doc,
                              size_t line,
                              size_t *start_line,
                              size_t *end_line);

/* True when `index` carries contour geometry inside the block at `start_line`.
   The N(Q) block of a numbered range counts as contour; a G80 terminator does
   not. */
bool g7x_doc_line_is_contour(const g7x_doc_t *doc, size_t start_line, size_t index);

/* True when `index` carries contour geometry of any block in the document. A
   screen marks these rows and a mark that disagreed with the generator's blocks
   would be marking a different program. */
bool g7x_doc_line_is_any_contour(const g7x_doc_t *doc, size_t index);

/* The numbered range that sits *above* `line`: walk back to the N(Q) row, then
   up to the N(P) row. `G70 P Q` names the profile the roughing cycle already
   collected, so its rows are before it, and a screen that marks them asks this.
   False when the range is not there: it is reported, never guessed. */
bool g7x_doc_range_above(const g7x_doc_t *doc,
                         size_t line,
                         uint32_t p,
                         uint32_t q,
                         size_t *first,
                         size_t *last);

/* The path of `line`: the rows that belong with it. A row inside a cycle answers
   with the block it sits in; a `G70 P Q` answers with the numbered range the
   finish cut replays, which is above it. False when the line heads nothing of
   its own - the caller then marks that line alone. */
bool g7x_doc_line_path(const g7x_doc_t *doc,
                       size_t line,
                       size_t *first,
                       size_t *last);

#ifdef __cplusplus
}
#endif

#endif
