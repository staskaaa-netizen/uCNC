#ifndef LVDS_UI_METERS_H
#define LVDS_UI_METERS_H

#include "lvds_hstx.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void lvds_ui_draw_perf_meter(int x,
                             int y,
                             lvds_color_t fg,
                             lvds_color_t bg,
                             int enabled,
                             uint16_t fps_x10,
                             uint32_t render_us);
void lvds_ui_draw_block_meter(int x,
                              int y,
                              lvds_color_t fg,
                              lvds_color_t bg,
                              int enabled,
                              uint32_t header_us,
                              uint32_t clear_us,
                              uint32_t rows_us,
                              uint32_t preview_us,
                              uint32_t footer_us,
                              uint32_t present_us);

#ifdef __cplusplus
}
#endif

#endif
