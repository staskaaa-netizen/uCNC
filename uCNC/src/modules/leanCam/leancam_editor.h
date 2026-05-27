#ifndef LEANCAM_EDITOR_H
#define LEANCAM_EDITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "leancam_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

void lc_editor_reset(void);

uint8_t lc_editor_field_index(void);
void lc_editor_set_field_index(uint8_t index);
uint8_t lc_editor_field_count(const char *line);
bool lc_editor_find_field(const char *line, uint8_t field_index, const char **open_out, const char **close_out);
bool lc_editor_replace_span(char *line, uint32_t line_len, const char *span_start, const char *span_end_exclusive, const char *replacement);
bool lc_editor_line_get_field_text(const char *line, const char *key, char *out, size_t out_sz);
bool lc_editor_line_set_field_text(char *line, uint32_t line_len, const char *key, const char *value);
void lc_editor_apply_accepted_field(leancam_ui_t *ui, const char *field_name, const char *accepted);
void lc_editor_normalize_source_edit(char *line, uint32_t line_len, const char *field_name);

void lc_editor_clear_input(leancam_ui_t *ui);
void lc_editor_toggle_sign(leancam_ui_t *ui);
void lc_editor_add_dot(leancam_ui_t *ui);
void lc_editor_input_digit(leancam_ui_t *ui, char digit);
void lc_editor_field_name(const leancam_ui_t *ui, uint8_t field_index, char *out, size_t out_sz);
void lc_editor_field_name_from_line(const char *line, uint8_t field_index, char *out, size_t out_sz);
void lc_editor_build_preview_line(const leancam_ui_t *ui, char *out, uint32_t out_len);
void lc_editor_advance_field(const char *line);
bool lc_editor_resolve_draft_line_for_commit(leancam_ui_t *ui,
                                             const char *setup_line,
                                             const char *tool_line,
                                             char *out,
                                             uint32_t out_len);
bool lc_editor_prepare_draft_for_commit(leancam_ui_t *ui,
                                        const char *setup_line,
                                        const char *tool_line);
bool lc_editor_accept_active_field(leancam_ui_t *ui,
                                   const char *setup_line,
                                   const char *tool_line,
                                   char *err,
                                   size_t err_sz);

#ifdef __cplusplus
}
#endif

#endif
