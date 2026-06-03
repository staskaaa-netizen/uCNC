#ifndef LEANCAM_FILES_H
#define LEANCAM_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "leancam_program.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LC_MAX_FILES      32
#define LC_FILE_NAME_MAX  48
#define LC_FILE_PATH_MAX  96

#define LC_DEFAULT_DIR    "/D/leancam/files"

bool leancam_files_init(void);
bool leancam_files_busy(void);

bool leancam_files_save(const char *path, const program_t *p);
bool leancam_files_save_plain(const char *path, const program_t *p);
bool leancam_files_load(const char *path, program_t *p);

bool leancam_files_refresh(const char *dir);
int  leancam_files_count(void);
const char *leancam_files_name(int index);
bool leancam_files_build_path(const char *dir, int index, char *out, int out_sz);
bool leancam_files_make_new_path(const char *dir, const char *name, char *out, int out_sz);
bool leancam_files_delete_path(const char *path);

const char *lc_path_basename(const char *path);
int lc_path_stricmp(const char *a, const char *b);
bool lc_path_has_suffix_ci(const char *name, const char *suffix);
void lc_path_get_program_stem(const char *current_path, char *out, size_t out_sz);
void lc_path_get_operation_name(const char *line, char *out, size_t out_sz);

void lc_file_browser_init(void);
bool lc_file_browser_refresh(const char *dir, uint32_t retry_ms);
bool lc_file_browser_ready(void);
bool lc_file_browser_should_retry(uint32_t now);
void lc_file_browser_mark_waiting(uint32_t due_ms);
int  lc_file_browser_selected(void);
void lc_file_browser_set_selected(int selected);
void lc_file_browser_clamp_selected(void);
bool lc_file_browser_selected_valid(void);
const char *lc_file_browser_selected_name(void);
bool lc_file_browser_selected_path(const char *dir, char *out, int out_sz);

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

