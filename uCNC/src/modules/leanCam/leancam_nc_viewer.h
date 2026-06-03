#ifndef LEANCAM_NC_VIEWER_H
#define LEANCAM_NC_VIEWER_H

#include <stdbool.h>
#include <stdint.h>
#include "leancam_files.h"
#include "leancam_program.h"
#include "leancam_snapshot_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

void lc_nc_viewer_init(void);
bool lc_nc_viewer_open(const char *path);
void lc_nc_viewer_close(void);
void lc_nc_viewer_normalize_selection(void);
void lc_nc_viewer_scroll_prev(void);
void lc_nc_viewer_scroll_next(void);

const char *lc_nc_viewer_path(void);
const program_t *lc_nc_viewer_cached_program(void);
const char *lc_nc_viewer_setup_line(void);
int lc_nc_viewer_top_line(void);
int lc_nc_viewer_selected_line(void);
bool lc_nc_viewer_selected_region(int *start_out, int *end_out);
uint8_t lc_nc_viewer_line_count(void);
uint8_t lc_nc_viewer_selected_row(void);
const char *lc_nc_viewer_selected_text(void);
const char *lc_nc_viewer_line(uint8_t row);

#ifdef __cplusplus
}
#endif

#endif

