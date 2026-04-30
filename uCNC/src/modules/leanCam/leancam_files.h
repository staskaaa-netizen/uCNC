#ifndef LEANCAM_FILES_H
#define LEANCAM_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include "conv_core.h"

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
bool leancam_files_load(const char *path, program_t *p);

bool leancam_files_refresh(const char *dir);
int  leancam_files_count(void);
const char *leancam_files_name(int index);
bool leancam_files_build_path(const char *dir, int index, char *out, int out_sz);
bool leancam_files_make_new_path(const char *dir, const char *name, char *out, int out_sz);
bool leancam_files_delete_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif
