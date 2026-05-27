#ifndef LEANCAM_NC_VIEWER_H
#define LEANCAM_NC_VIEWER_H

#include <stdbool.h>
#include <stdint.h>
#include "leancam_files.h"
#include "../ui_snapshot/ui_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

void lc_nc_viewer_init(void);
bool lc_nc_viewer_open(const char *path);
void lc_nc_viewer_close(void);
void lc_nc_viewer_scroll_prev(void);
void lc_nc_viewer_scroll_next(void);

const char *lc_nc_viewer_path(void);
const char *lc_nc_viewer_setup_line(void);
int lc_nc_viewer_top_line(void);
uint8_t lc_nc_viewer_line_count(void);
uint8_t lc_nc_viewer_selected_row(void);
const char *lc_nc_viewer_line(uint8_t row);

#ifdef __cplusplus
}
#endif

#endif
