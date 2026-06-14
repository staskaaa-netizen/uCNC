#ifndef G7X_CONTOUR_H
#define G7X_CONTOUR_H

#include "g7x.h"

#ifdef __cplusplus
extern "C" {
#endif

bool g7x_command_is(const char *line, const char *cmd);
bool g7x_get_field_text(const char *line, const char *key, char *out, size_t out_sz);
bool g7x_get_field_float(const char *line, const char *key, float *out);
bool g7x_modal_apply_line(g7x_modal_t *modal, const char *line);

g7x_cycle_t g7x_cycle_from_line(const char *line);
g7x_contour_cmd_t g7x_contour_cmd_from_line(const char *line);

g7x_result_t g7x_stream_begin(g7x_stream_t *stream, const char *cycle_line);
g7x_result_t g7x_stream_add_line(g7x_stream_t *stream, const char *line, bool *done);
g7x_result_t g7x_thread_begin(g7x_thread_stream_t *stream,
                              const char *line,
                              float default_start_diameter,
                              float default_clearance);

#ifdef __cplusplus
}
#endif

#endif
