#ifndef LEANCAM_SNAPSHOT_H
#define LEANCAM_SNAPSHOT_H

#include "leancam_snapshot_frame.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void lc_snapshot_reset_frame(ui_snapshot_frame_t *f);
void lc_snapshot_put_line_ex(ui_snapshot_frame_t *f,
                             int row,
                             const char *s,
                             bool selected,
                             uint8_t hi_start,
                             uint8_t hi_end);
void lc_snapshot_put_line(ui_snapshot_frame_t *f, int row, const char *s, bool selected);
void lc_snapshot_format_program_line(int line_no,
                                     int indent,
                                     const char *display_line,
                                     char *out,
                                     size_t out_sz);
int lc_snapshot_put_program_command(ui_snapshot_frame_t *f,
                                    int row,
                                    int line_no,
                                    int indent,
                                    const char *display_line,
                                    bool selected);

#ifdef __cplusplus
}
#endif

#endif

