#ifndef NC_EMIT_H
#define NC_EMIT_H

#include "nc.h"
#include "../g71_g72/g71_g72.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NC_EMIT_SKIP = 0,
    NC_EMIT_LINE
} nc_emit_result_t;

typedef struct {
    const nc_document_t *doc;
    size_t source_line;
    bool active;
    bool log;
    bool g7x_collecting;
    g7x_stream_t g7x;
} nc_emit_stream_t;

nc_emit_result_t nc_emit_source_line(const nc_document_t *doc,
                                     size_t line_index,
                                     char *out,
                                     size_t out_sz);
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
