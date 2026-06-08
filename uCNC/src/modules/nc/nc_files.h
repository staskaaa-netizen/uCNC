#ifndef NC_FILES_H
#define NC_FILES_H

#include "nc.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NC_FILES_DIR
#define NC_FILES_DIR "/D/nc/files"
#endif

#ifndef NC_MAX_FILES
#define NC_MAX_FILES 32
#endif

#ifndef NC_FILE_NAME_MAX
#define NC_FILE_NAME_MAX 48
#endif

void nc_files_init(void);
void nc_files_set_active(bool active);
bool nc_files_active(void);
bool nc_files_refresh(const char *dir);
int nc_files_count(void);
const char *nc_files_name(int index);
const char *nc_files_cwd(void);
bool nc_files_is_dir(int index);
bool nc_files_selected_is_dir(void);
bool nc_files_build_path(int index, char *out, int out_sz);
int nc_files_selected(void);
void nc_files_select_prev(void);
void nc_files_select_next(void);
bool nc_files_selected_path(char *out, int out_sz);
bool nc_files_enter_selected(void);
bool nc_files_go_parent(void);
bool nc_files_delete_selected(void);
bool nc_files_create_named(const char *name, const char *ext, char *out_path, int out_sz);
bool nc_files_ready(void);

#ifdef __cplusplus
}
#endif

#endif
