#ifndef G7X_SOURCE_H
#define G7X_SOURCE_H

#include "g7x.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Program text lookup for P/Q numbered-block cycles.

   G7x never reads files or NC documents itself. A caller that owns program
   text (NC file, preview adapter, bench harness) supplies this cursor. The
   parser run path does not need a source: numbered profile blocks are
   collected from the live stream as they arrive. The source is for callers
   that must resume a range out of stream order, currently the NC preview. */

typedef enum {
    G7X_SOURCE_OK = 0,
    G7X_SOURCE_MISSING,
    G7X_SOURCE_ERROR
} g7x_source_status_t;

/* Position inside an external source. `line_index` is source defined and may
   stay 0 for sources that have no line array. */
typedef struct {
    uint32_t number;
    size_t line_index;
} g7x_source_pos_t;

/* Return the next numbered block after `from`. On the first call the caller
   passes {0, 0}; afterwards it feeds the previous result back in. The source
   scans from `from->line_index` and stores the resume index in `out`, so a
   caller may also start in the middle of a document. Copies the block text
   into `text`. */
typedef g7x_source_status_t (*g7x_source_next_fn)(void *user,
                                                  const g7x_source_pos_t *from,
                                                  g7x_source_pos_t *out,
                                                  char *text,
                                                  size_t text_sz);

typedef struct {
    g7x_source_next_fn next;
    void *user;
} g7x_source_t;

g7x_source_status_t g7x_source_next(const g7x_source_t *source,
                                    const g7x_source_pos_t *from,
                                    g7x_source_pos_t *out,
                                    char *text,
                                    size_t text_sz);

/* Bounded serial history of numbered blocks in stream order. It is the
   fallback when the caller has no random-access source, and the retention
   buffer for replaying a range that has already streamed past. Blocks that
   were evicted are reported as missing instead of silently resolved. */
#ifndef G7X_MAX_RETAINED_BLOCKS
#define G7X_MAX_RETAINED_BLOCKS 48
#endif
#ifndef G7X_RETAINED_TEXT_LEN
#define G7X_RETAINED_TEXT_LEN 48
#endif

typedef struct {
    uint32_t number;
    bool used;
    char text[G7X_RETAINED_TEXT_LEN];
} g7x_retained_block_t;

typedef struct {
    g7x_retained_block_t blocks[G7X_MAX_RETAINED_BLOCKS];
    unsigned count;
    unsigned next;
    bool evicted;
} g7x_history_t;

void g7x_history_reset(g7x_history_t *history);
g7x_result_t g7x_history_add(g7x_history_t *history,
                             uint32_t number,
                             const char *text);
const char *g7x_history_find(const g7x_history_t *history, uint32_t number);
bool g7x_history_evicted(const g7x_history_t *history);

/* Walk the retained blocks that form the inclusive [p, q] range in stream
   order. A missing block, a duplicate number and a reversed range are reported
   instead of guessing, because a wrong profile cuts the wrong part. */
typedef void (*g7x_history_visit_fn)(void *user,
                                     uint32_t number,
                                     const char *text);

g7x_result_t g7x_history_visit_range(const g7x_history_t *history,
                                     uint32_t p,
                                     uint32_t q,
                                     g7x_history_visit_fn visit,
                                     void *user,
                                     unsigned *visited);

/* Parser-owned retention of numbered profile blocks. NULL when the parser
   module is not built. Reset by parser_reset (new program or reset). */
const g7x_history_t *g7x_parser_numbered_history(void);
void g7x_parser_numbered_history_reset(void);

#ifdef __cplusplus
}
#endif

#endif
