#ifndef NC_STATE_H
#define NC_STATE_H

#include "nc.h"
#include "nc_menu.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Drive-qualified: a bare name never reaches a driver - uCNC's fs layer
   refuses a path that does not start with a drive, so "tool.t" opened nothing
   on the machine (the desktop bench was more forgiving). */
#define NC_TOOL_PATH "/D/nc/files/tool.t"

typedef struct {
    uint16_t exec_state;
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
    /* RUN marks the line it is on, and the source lines of the cycle that line
       belongs to (`nc_g7x_block_containing()`): the current line, and the block
       it is working in. Set by the screen while a run is going, so the editor's
       pane can draw the weaker mark without knowing why. */
    bool block_mark;
    size_t block_first;
    size_t block_last;
    char selected_label[32];
    size_t line_count;
    nc_runtime_state_t runtime;
    bool dirty;
} nc_snapshot_t;

void nc_state_init(void);
void nc_state_set_mode(nc_mode_t mode);
nc_mode_t nc_state_mode(void);
void nc_state_remember_path(nc_mode_t mode, const char *path);
void nc_state_remember_cursor(const nc_document_t *doc);
const char *nc_state_path(nc_mode_t mode);
/* The cycle block the RUN mark sits in, for the editor pane's weaker mark. The
   screen owns the answer - it is the one that knows the sender's line - and
   sets it (false, with no block) whenever no run is going. */
void nc_state_set_run_block(bool marked, size_t first, size_t last);
bool nc_state_tool_path_supported(const char *path);
void nc_state_save(void);
/* Writes the state file if anything changed since the last flush. */
void nc_state_flush(void);
/* The speed the MANUAL spindle keys start at: the last one the machine had, so
   starting the spindle keeps the operator's speed instead of forcing a default
   (and it survives a reboot with the rest of the remembered state). */
unsigned nc_state_manual_spindle(void);
void nc_state_remember_manual_spindle(unsigned rpm);
bool nc_state_load_document(nc_mode_t mode, nc_document_t *doc);
void nc_state_runtime(nc_runtime_state_t *state);
void nc_state_snapshot(const nc_document_t *doc, nc_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif
