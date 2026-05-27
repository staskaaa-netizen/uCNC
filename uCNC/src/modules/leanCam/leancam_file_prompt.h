#ifndef LEANCAM_FILE_PROMPT_H
#define LEANCAM_FILE_PROMPT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void lc_file_prompt_clear(void);
void lc_file_prompt_begin_new(void);
void lc_file_prompt_begin_duplicate(const char *source_path);

bool lc_file_prompt_duplicate_pending(void);
const char *lc_file_prompt_duplicate_source(void);
const char *lc_file_prompt_name(void);
bool lc_file_prompt_name_empty(void);
size_t lc_file_prompt_name_len(void);
void lc_file_prompt_backspace(void);
void lc_file_prompt_append_digit(char digit);
void lc_file_prompt_finish_duplicate(void);

#ifdef __cplusplus
}
#endif

#endif
