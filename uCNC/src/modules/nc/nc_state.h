#ifndef NC_STATE_H
#define NC_STATE_H

#include "nc.h"
#include "nc_menu.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_MDI_PATH "mdi.nc"
#define NC_TOOL_PATH "tool.t"

typedef struct {
    uint8_t exec_state;
    float x;
    float z;
    float feed;
    unsigned spindle;
} nc_runtime_state_t;

typedef struct {
    char path[NC_PATH_MAX];
    char lines[NC_MAX_VISIBLE_LINES][NC_MAX_LINE_LEN];
    size_t first_line;
    size_t cursor_line;
    int cursor_visible_index;
    int selected_word_start;
    int selected_word_end;
    char selected_label[32];
    size_t line_count;
    nc_runtime_state_t runtime;
    bool dirty;
} nc_snapshot_t;

void nc_state_init(void);
void nc_state_set_mode(nc_mode_t mode);
nc_mode_t nc_state_mode(void);
void nc_state_remember_path(nc_mode_t mode, const char *path);
const char *nc_state_path(nc_mode_t mode);
bool nc_state_tool_path_supported(const char *path);
void nc_state_save(void);
bool nc_state_load_document(nc_mode_t mode, nc_document_t *doc);
void nc_state_runtime(nc_runtime_state_t *state);
void nc_state_snapshot(const nc_document_t *doc, nc_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif
