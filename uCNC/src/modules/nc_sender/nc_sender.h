#ifndef NC_SENDER_H
#define NC_SENDER_H

#include "../g7x/g7x_contour.h"
#include "../g7x/g7x.h"
#include "../g7x/g7x_blocks.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the controller is sent: program text, with G7x cycles expanded as they
   are met and the increments `U`/`W` written as the absolute words they mean.

   This module owns the program-to-block answer, not a screen or a run. It reads
   lines through `g7x_doc_t` and asks the caller for a tool radius only when a
   compensated cycle resolves one. The pacer that decides *when* to hand these
   blocks to the machine stays with its panel. */

#ifndef NC_SENDER_MAX_LINE_LEN
#define NC_SENDER_MAX_LINE_LEN 96
#endif
#define NC_SENDER_MAX_FIELDS 24

typedef bool (*nc_sender_tool_radius_fn)(void *user, int tool, float *radius);

/* One programme word: its letter and the text span of its value. */
typedef struct {
    char letter;
    size_t start;
    size_t value;
    size_t end;
} nc_sender_field_t;

int nc_sender_fields(const char *line, nc_sender_field_t *out, int max);

typedef enum {
    NC_SENDER_EMIT_SKIP = 0,
    NC_SENDER_EMIT_LINE,
    NC_SENDER_EMIT_ERROR
} nc_sender_emit_result_t;

typedef struct {
    g7x_doc_t doc;
    size_t source_line;
    /* The line the block in play belongs to: a cycle's header while its
       expansion walks, or the line itself for a line the panel hands over as it
       stands. It is where a run's fault is marked. `source_line` cannot answer
       that - the stream walks past the rows of a cycle before the generator can
       answer for the whole of it, so it is already past the trouble when a
       cycle is refused, and the motions the generator makes carry no source
       line at all. The bench saw both: the mark landed on the line after the
       one that would not expand, and on line 1 through a run (2026-09-27: "on
       errors - again 1 line is selected and this one will not run"). */
    size_t block_line;
    bool active;
    bool log;
    bool g7x_collecting;
    g7x_result_t error;
    /* The compensation the program has asked for (`G41`/`G42`/`G40`), and the
       nose radius of the tool in play. The panel keeps the modal word for
       itself: the controller has no compensation and would cut the programmed
       path while the operator believed otherwise (`docs/lathe-cutter-comp.md`),
       so the cycle is handed a *shifted* contour instead - `lathe_comp`'s
       geometry, applied by G7x when the contour is prepared. */
    g7x_comp_side_t comp_side;
    float comp_radius;
    /* The tool the *sender* is on: the number of the last `T` line this stream
       has handed over, kept for the whole run. The compensation's radius is
       resolved from this number through the caller's callback - never by
       scanning a document for a `T` word, and never from the machine. The
       sending side knows which insert it is cutting with, and that is the whole
       answer (bench, 2026-10-04: *"make it sort of hard store of t1 and
       compensation"*, after a `T1 R2` / `T2 R3` table let a `G41` cut with the
       wrong radius). 0 means "none spoken yet". */
    int tool;
    nc_sender_tool_radius_fn tool_radius;
    void *tool_radius_user;
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
} nc_sender_emit_stream_t;

/* Which of the four axis words a line carries: an absolute word establishes
   where the tool is, an increment counts from there. */
#define NC_SENDER_EMIT_WORD_X_ABS 0x01
#define NC_SENDER_EMIT_WORD_Z_ABS 0x02
#define NC_SENDER_EMIT_WORD_U_INC 0x04
#define NC_SENDER_EMIT_WORD_W_INC 0x08

/* Where a line leaves the tool. `U` and `W` are Fanuc's incremental X and Z:
   always increments, whichever distance mode is active, in the same units as X
   and Z. When `out` is given it receives the line with the increments written as
   the X and Z words they mean - which is what the controller is sent, because
   the machine's parser has no U or W and does not need one. False when the line
   names no axis at all. */
bool nc_sender_emit_line_point(const char *line, float *x, float *z,
                               char *out, size_t out_sz, uint8_t *words);

/* True when the controller is given this line at all - an ordinary motion or a
   contour row. A cycle header's `U` is a depth of cut, not an increment, so the
   `U` rule only applies to the lines this answers true for. */
bool nc_sender_emit_line_is_direct(const char *line);

/* One source line as the controller reads it, or SKIP for the lines this layer
   keeps to itself (a cycle header, a stock definition, a `G80`, a comment). */
nc_sender_emit_result_t nc_sender_emit_source_line(const g7x_doc_t *doc,
                                                   size_t line_index,
                                                   char *out,
                                                   size_t out_sz);

void nc_sender_emit_stream_begin(nc_sender_emit_stream_t *stream,
                                 const g7x_doc_t *doc,
                                 size_t start_line,
                                 nc_sender_tool_radius_fn tool_radius,
                                 void *tool_radius_user);
void nc_sender_emit_stream_set_log(nc_sender_emit_stream_t *stream, bool log);

/* Whether the stream stopped on a fault rather than on the end of the program:
   a contour that never closed, a range the rows do not carry, a generated block
   the parser refused. A run that ends on one of these is not a finished
   program, and the pacer has to say so instead of "stream done". */
bool nc_sender_emit_stream_failed(const nc_sender_emit_stream_t *stream);
size_t nc_sender_emit_stream_line(const nc_sender_emit_stream_t *stream);
/* The line of the *program* the stream is working on: a cycle's header while
   its expansion walks, or the line itself for a line handed over as it stands.
   A sender that wants to say "this block came from that line" asks this, not
   the line each single output reports - a cycle's own motions and the rows it
   re-cuts answer differently there. */
size_t nc_sender_emit_stream_block_line(const nc_sender_emit_stream_t *stream);
nc_sender_emit_result_t nc_sender_emit_stream_next(nc_sender_emit_stream_t *stream,
                                                   char *out,
                                                   size_t out_sz,
                                                   size_t *source_line);

/* --- pacing and run state ------------------------------------------------- */

#ifndef NC_SENDER_STATUS_MAX
#define NC_SENDER_STATUS_MAX 64
#endif

typedef void (*nc_sender_status_fn)(void *user, const char *text);

void nc_sender_run_init(nc_sender_status_fn status, void *status_user,
                        nc_sender_tool_radius_fn tool_radius,
                        void *tool_radius_user);
void nc_sender_run_pace(void);

/* The firmware normally lets a G7x cycle's emitted lines follow back-to-back as
   one cut. A desktop station can ask for strict pacing, which waits for the
   virtual machine to become idle between every emitted line as well. */
void nc_sender_run_set_strict_block_pacing(bool enabled);

bool nc_sender_run_start(const g7x_doc_t *doc, size_t line);
bool nc_sender_run_send_unit(const g7x_doc_t *doc, size_t line);

void nc_sender_run_reset(void);
void nc_sender_run_stop(void);
bool nc_sender_run_toggle_hold(void);
void nc_sender_run_set_line(const g7x_doc_t *doc, size_t line);

bool nc_sender_run_active(void);
bool nc_sender_run_hold(void);
bool nc_sender_run_streaming(void);
int nc_sender_run_tool(void);
g7x_comp_side_t nc_sender_run_comp_mode(void);
bool nc_sender_run_expanding(void);
size_t nc_sender_run_line(void);
size_t nc_sender_run_display_line(void);
bool nc_sender_run_executing_line(size_t *line);

uint8_t nc_sender_run_error(void);
size_t nc_sender_run_error_line(void);
const char *nc_sender_run_refused_text(void);
bool nc_sender_run_send_line(const char *line);

#endif
