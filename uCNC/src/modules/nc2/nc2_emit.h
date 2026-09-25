#ifndef NC2_EMIT_H
#define NC2_EMIT_H

#include "nc2.h"

#include "../g7x/g7x_contour.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the controller is sent: the program's lines, with the cycles expanded as
   they are met, and the increments `U`/`W` written out as the absolute words
   they mean. The preview draws what this emits, so there is one answer to "what
   does this program do" - the same answer the machine gets. */

typedef enum {
    NC2_EMIT_SKIP = 0,
    NC2_EMIT_LINE,
    NC2_EMIT_ERROR
} nc2_emit_result_t;

typedef struct {
    const nc2_document_t *doc;
    size_t source_line;
    bool active;
    bool log;
    bool g7x_collecting;
    g7x_result_t error;
    g7x_stream_t g7x;
    /* Where the tool is, tracked from what this stream has *emitted* - so a
       cycle's own generated motion counts, and the line after it is resolved
       against where the machine will be. An axis becomes known when the program
       gives it absolutely: a program has to say where it is before it can move
       by increments. */
    float x;
    float z;
    bool x_known;
    bool z_known;
    /* The point the block being collected is at: a contour is relative row to
       row, and the rows themselves do not move the machine yet. */
    float row_x;
    float row_z;
    bool row_valid;
} nc2_emit_stream_t;

/* Which of the four axis words a line carries: an absolute word establishes
   where the tool is, an increment counts from there. */
#define NC2_EMIT_WORD_X_ABS 0x01
#define NC2_EMIT_WORD_Z_ABS 0x02
#define NC2_EMIT_WORD_U_INC 0x04
#define NC2_EMIT_WORD_W_INC 0x08

/* Where a line leaves the tool. `U` and `W` are Fanuc's incremental X and Z:
   always increments, whichever distance mode is active, in the same units as X
   and Z. When `out` is given it receives the line with the increments written as
   the X and Z words they mean - which is what the controller is sent, because
   the machine's parser has no U or W and does not need one. False when the line
   names no axis at all. */
bool nc2_emit_line_point(const char *line, float *x, float *z,
                         char *out, size_t out_sz, uint8_t *words);

/* True when the controller is given this line at all - an ordinary motion or a
   contour row. A cycle header's `U` is a depth of cut, not an increment, so the
   `U` rule only applies to the lines this answers true for. */
bool nc2_emit_line_is_direct(const char *line);

/* One source line as the controller reads it, or SKIP for the lines this layer
   keeps to itself (a cycle header, a stock definition, a `G80`, a comment). */
nc2_emit_result_t nc2_emit_source_line(const nc2_document_t *doc,
                                       size_t line_index,
                                       char *out,
                                       size_t out_sz);

void nc2_emit_stream_begin(nc2_emit_stream_t *stream,
                           const nc2_document_t *doc,
                           size_t start_line);
void nc2_emit_stream_set_log(nc2_emit_stream_t *stream, bool log);
size_t nc2_emit_stream_line(const nc2_emit_stream_t *stream);
nc2_emit_result_t nc2_emit_stream_next(nc2_emit_stream_t *stream,
                                       char *out,
                                       size_t out_sz,
                                       size_t *source_line);

#endif
