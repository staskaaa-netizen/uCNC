#ifndef NC_EMIT_H
#define NC_EMIT_H

#include "nc.h"
#include "../g7x/g7x_contour.h"
#include "../g7x/g7x_source.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NC_EMIT_SKIP = 0,
    NC_EMIT_LINE,
    NC_EMIT_ERROR
} nc_emit_result_t;

typedef struct {
    const nc_document_t *doc;
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
       by increments (`nc_emit_line_point()`). */
    float x;
    float z;
    bool x_known;
    bool z_known;
    /* The point the block being collected is at: a contour is relative row to
       row, and the rows themselves do not move the machine yet - the generated
       motion of the block does that. */
    float row_x;
    float row_z;
    bool row_valid;
} nc_emit_stream_t;

/* Document-backed numbered-block source. G7x owns the lookup contract; NC only
   supplies the program text it already holds. `holder` must outlive the
   returned source. */
typedef struct {
    const nc_document_t *doc;
} nc_numbered_source_t;

g7x_source_t nc_emit_numbered_source(nc_numbered_source_t *holder,
                                     const nc_document_t *doc);

nc_emit_result_t nc_emit_source_line(const nc_document_t *doc,
                                     size_t line_index,
                                     char *out,
                                     size_t out_sz);

/* Which of the four axis words `nc_emit_line_point()` found: an absolute word
   establishes where the tool is, an increment counts from there. */
#define NC_EMIT_WORD_X_ABS 0x01
#define NC_EMIT_WORD_Z_ABS 0x02
#define NC_EMIT_WORD_U_INC 0x04
#define NC_EMIT_WORD_W_INC 0x08

bool nc_emit_line_point(const char *line, float *x, float *z,
                        char *out, size_t out_sz, uint8_t *words);

/* True when the controller is given this line - an ordinary motion or a contour
   row. It is the question "does this line move the tool", and the reason the
   preview asks it is that a cycle header's `U` is a depth of cut, not an
   increment. */
bool nc_emit_line_is_direct(const char *line);
void nc_emit_stream_begin(nc_emit_stream_t *stream,
                          const nc_document_t *doc,
                          size_t start_line);
void nc_emit_stream_set_log(nc_emit_stream_t *stream, bool log);
size_t nc_emit_stream_line(const nc_emit_stream_t *stream);
nc_emit_result_t nc_emit_stream_next(nc_emit_stream_t *stream,
                                     char *out,
                                     size_t out_sz,
                                     size_t *source_line);

#ifdef __cplusplus
}
#endif

#endif
