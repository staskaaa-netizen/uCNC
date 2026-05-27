#ifndef LEANCAM_FILE_BROWSER_H
#define LEANCAM_FILE_BROWSER_H

#include <stdbool.h>
#include <stdint.h>
#include "leancam_files.h"

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif
