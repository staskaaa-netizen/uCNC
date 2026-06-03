#ifndef LVDS_UI_LAYOUT_H
#define LVDS_UI_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lvds_ui_rect {
    int x;
    int y;
    int w;
    int h;
} lvds_ui_rect_t;

typedef struct lvds_tool_editor_layout {
    int title_x;
    int title_y;
    int row_y;
    int detail_y;
    int active_field_x;
    int active_field_y;
    int glyph_x;
    int glyph_w;
    int text_x;
    int text_cols;
    int row_cell_x;
    int row_cell_w;
    int max_rows;
} lvds_tool_editor_layout_t;

typedef struct lvds_program_layout {
    int row_y;
    int rows_bottom;
    int divider_x;
    int divider_y;
    int divider_h;
    int preview_title_x;
    int preview_title_y;
    int title_x;
    int title_y;
    int text_x;
    int text_cols;
    int row_cell_x;
    int row_cell_w;
    int preview_active_x;
    int preview_active_y;
    int tool_panel_x;
    int tool_panel_y;
    int compact_max_rows;
} lvds_program_layout_t;

void lvds_ui_tool_editor_layout(int screen_w,
                                int left_x,
                                int left_w,
                                int right_x,
                                int right_cols,
                                uint8_t tool_asset,
                                lvds_tool_editor_layout_t *out);
void lvds_ui_program_layout(int screen_w,
                            int left_x,
                            int left_w,
                            int right_pane_x,
                            int right_pane_w,
                            int right_text_cols,
                            lvds_program_layout_t *out);

#ifdef __cplusplus
}
#endif

#endif

