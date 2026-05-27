#ifndef LEANCAM_PATHS_H
#define LEANCAM_PATHS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *lc_path_basename(const char *path);
int lc_path_stricmp(const char *a, const char *b);
bool lc_path_has_suffix_ci(const char *name, const char *suffix);
void lc_path_get_program_stem(const char *current_path, char *out, size_t out_sz);
void lc_path_get_operation_name(const char *line, char *out, size_t out_sz);
bool lc_path_make_lrun(const char *dir,
                       const char *current_path,
                       const char *line,
                       int line_no,
                       char *out,
                       int out_sz);

#ifdef __cplusplus
}
#endif

#endif
