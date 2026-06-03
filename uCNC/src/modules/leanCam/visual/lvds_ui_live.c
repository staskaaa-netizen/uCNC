/* LeanCam visual contract:
 * Purpose: draw live machine/runtime panels from snapshot values.
 * Called by: leancam_visual during frame composition.
 * Calls into: LVDS primitive/text helpers only.
 * Owns: no LeanCam application state.
 */
#include "lvds_ui_live.h"

#include "../../lvds_renderer/lvds_draw_api.h"

void lvds_ui_live_draw_source_rows(const ui_snapshot_frame_t *frame,
                                   int x,
                                   int y,
                                   lvds_color_t label_fg,
                                   lvds_color_t value_fg,
                                   lvds_color_t bg,
                                   int font)
{
    const char *file = "-";
    const int left_w = 20;
    const int table_x = x + 12 + (left_w * 2) + 5;
    const int table_cols = 60;

    if (frame && frame->leancam_title[0]) {
        file = frame->leancam_title;
    }

    lvds_draw_text_clip(x, y, "FILE", 5, label_fg, bg, font);
    lvds_draw_text_clip(x, y + 22, file, left_w, value_fg, bg, font);
    lvds_draw_text_clip(table_x, y, "LINE", table_cols, label_fg, bg, font);
    if (!frame || !frame->leancam_preview_line[0]) {
        lvds_draw_text_clip(table_x, y + 22, "full file", table_cols, value_fg, bg, font);
        return;
    }
    lvds_draw_text_clip(table_x, y + 22, frame->leancam_preview_line, table_cols, value_fg, bg, font);
}

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
                              int text_font)
{
    lvds_draw_text_clip(title_x, title_y, title ? title : "", title_cols, title_fg, bg, title_font);
    lvds_ui_live_draw_source_rows(frame, source_x, source_y, label_fg, value_fg, bg, text_font);
    lvds_draw_text_clip(message_x,
                        message_y,
                        frame ? frame->leancam_message : "",
                        message_cols,
                        message_fg,
                        bg,
                        text_font);
}


