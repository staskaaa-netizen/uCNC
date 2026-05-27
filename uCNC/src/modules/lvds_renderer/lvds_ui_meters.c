#include "lvds_ui_meters.h"

#include "lvds_draw_api.h"

#include <stdio.h>

void lvds_ui_draw_perf_meter(int x,
                             int y,
                             lvds_color_t fg,
                             lvds_color_t bg,
                             int enabled,
                             uint16_t fps_x10,
                             uint32_t render_us)
{
    char perf[56];

    if (!enabled) {
        (void)x;
        (void)y;
        (void)fg;
        (void)bg;
        (void)fps_x10;
        (void)render_us;
        return;
    }
    snprintf(perf, sizeof(perf), "%u.%u fps  %lu us",
             (unsigned)(fps_x10 / 10u),
             (unsigned)(fps_x10 % 10u),
             (unsigned long)render_us);
    lvds_draw_text_clip(x, y, perf, 22, fg, bg, LVDS_FONT_SMALL);
}

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
                              uint32_t present_us)
{
    char perf[80];

    if (!enabled) {
        (void)x;
        (void)y;
        (void)fg;
        (void)bg;
        (void)header_us;
        (void)clear_us;
        (void)rows_us;
        (void)preview_us;
        (void)footer_us;
        (void)present_us;
        return;
    }
    snprintf(perf, sizeof(perf), "H%lu C%lu R%lu P%lu F%lu Pr%lu",
             (unsigned long)header_us,
             (unsigned long)clear_us,
             (unsigned long)rows_us,
             (unsigned long)preview_us,
             (unsigned long)footer_us,
             (unsigned long)present_us);
    lvds_draw_text_clip(x, y, perf, 40, fg, bg, LVDS_FONT_SMALL);
}
