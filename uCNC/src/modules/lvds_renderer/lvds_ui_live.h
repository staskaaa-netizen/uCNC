#ifndef LVDS_UI_LIVE_H
#define LVDS_UI_LIVE_H

#include "../ui_snapshot/ui_snapshot.h"
#include "lvds_hstx.h"

#ifdef __cplusplus
extern "C" {
#endif

void lvds_ui_live_draw_source_rows(const ui_snapshot_frame_t *frame,
                                   int x,
                                   int y,
                                   lvds_color_t label_fg,
                                   lvds_color_t value_fg,
                                   lvds_color_t bg,
                                   int font);
void lvds_ui_live_draw_header(const ui_snapshot_frame_t *frame,
                              int title_x,
                              int title_y,
                              const char *title,
                              int title_cols,
                              int source_x,
                              int source_y,
                              int message_x,
                              int message_y,
                              int message_cols,
                              lvds_color_t title_fg,
                              lvds_color_t label_fg,
                              lvds_color_t value_fg,
                              lvds_color_t message_fg,
                              lvds_color_t bg,
                              int title_font,
                              int text_font);

#ifdef __cplusplus
}
#endif

#endif
