#ifndef LEANCAM_CODE_H
#define LEANCAM_CODE_H

#include "leancam_program.h"
#include "leancam_snapshot_frame.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool lc_code_command_is(const char *line, const char *cmd);
bool lc_code_get_field_text(const char *line, const char *key, char *out, size_t out_sz);
bool lc_code_get_field_float(const char *line, const char *key, float *out);

void lc_code_build_draft_display(char *dst,
                                 uint32_t dst_len,
                                 const char *draft,
                                 const char *input,
                                 uint8_t active_index,
                                 const char *setup_line,
                                 const char *tool_line,
                                 const char *this_line,
                                 uint8_t *hi_start,
                                 uint8_t *hi_end);
int lc_code_resolve_field_value(const char *raw,
                                const char *setup_line,
                                const char *tool_line,
                                const char *this_line,
                                char *out,
                                uint32_t out_len);

bool lc_code_region_is_header(const char *line);
bool lc_code_region_is_contour(const char *line);
bool lc_code_region_is_end(const char *line);
int lc_code_region_display_indent(const program_t *prog, int index, const char *line);
bool lc_code_region_find(const program_t *prog, int index, int *start_out, int *end_out);

const char *lc_code_effective_tool_for_cycle(const program_t *prog, int before_or_at, const char *cycle);
bool lc_code_validate_tool_call(const program_t *prog,
                                int before_or_at,
                                const char *line,
                                char *err,
                                size_t err_sz);

#ifdef __cplusplus
}
#endif

#endif

