#ifndef G7X_CONTOUR_H
#define G7X_CONTOUR_H

#include "g7x.h"

#ifdef __cplusplus
extern "C" {
#endif

bool g7x_command_is(const char *line, const char *cmd);
bool g7x_get_field_text(const char *line, const char *key, char *out, size_t out_sz);
bool g7x_get_field_float(const char *line, const char *key, float *out);
/* N word of a source row, ignoring comment text. Unlike the generic field
   reader this also accepts N as the first token of the line. */
bool g7x_line_number(const char *line, uint32_t *out);
/* Source row with an optional leading N word removed, so command and field
   readers see the same text for `N100 G1 X50` and `G1 X50`. */
const char *g7x_skip_line_number(const char *line);
bool g7x_modal_apply_line(g7x_modal_t *modal, const char *line);

g7x_cycle_t g7x_cycle_from_line(const char *line);
g7x_contour_cmd_t g7x_contour_cmd_from_line(const char *line);

g7x_result_t g7x_stream_begin(g7x_stream_t *stream, const char *cycle_line);
/* Fanuc two-line header: the first block carries the depth of cut (U for G71,
   W for G72) and R, the second carries P/Q plus the X/Z finish allowances in
   U/W. Both X/Z and U/W spellings are accepted for the allowances. */
g7x_result_t g7x_stream_begin_linked(g7x_stream_t *stream,
                                     const char *first_line,
                                     const char *second_line);
g7x_result_t g7x_stream_add_line(g7x_stream_t *stream, const char *line, bool *done);
g7x_result_t g7x_thread_begin(g7x_thread_stream_t *stream,
                              const char *line,
                              float default_start_diameter,
                              float default_clearance);

#ifdef __cplusplus
}
#endif

#endif
