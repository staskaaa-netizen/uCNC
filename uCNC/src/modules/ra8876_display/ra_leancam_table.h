#ifndef RA_LEANCAM_TABLE_H
#define RA_LEANCAM_TABLE_H

#include "../ui_snapshot/ui_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    char header[UI_LC_LINE_LEN];
    char value[UI_LC_LINE_LEN];
    uint8_t hi_start;
    uint8_t hi_end;
    bool has_hi;
    bool table_like;
} ra_lc_table_line_t;

void ra_lc_build_table_line(const ui_snapshot_frame_t *frame, int row, ra_lc_table_line_t *out);

#ifdef __cplusplus
}
#endif

#endif
