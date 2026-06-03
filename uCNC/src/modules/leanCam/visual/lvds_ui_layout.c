/* LeanCam visual contract:
 * Purpose: shared screen rectangles and layout constants for LeanCam LVDS views.
 * Called by: visual drawing modules.
 * Calls into: no application logic.
 * Owns: static layout math only.
 */
#include "lvds_ui_layout.h"

void lvds_ui_tool_editor_layout(int screen_w,
                                int left_x,
                                int left_w,
                                int right_x,
                                int right_cols,
                                uint8_t tool_asset,
                                lvds_tool_editor_layout_t *out)
{
    int glyph_gap = tool_asset ? 14 : 0;

    (void)left_w;
    if (!out) {
        return;
    }

    out->title_x = left_x;
    out->title_y = 64;
    out->row_y = 100;
    out->detail_y = 354;
    out->active_field_x = left_x + 16;
    out->active_field_y = 522;
    out->glyph_x = left_x + 8;
    out->glyph_w = tool_asset ? 30 : 0;
    out->text_x = tool_asset ? (left_x + out->glyph_w + glyph_gap) : right_x;
    out->text_cols = tool_asset ? ((screen_w - out->text_x - 18) / 8) : right_cols;
    out->row_cell_x = tool_asset ? 14 : (right_x - 2);
    out->row_cell_w = tool_asset ? (screen_w - 28) : (screen_w - right_x - 16);
    out->max_rows = tool_asset ? 9 : 7;
}

void lvds_ui_program_layout(int screen_w,
                            int left_x,
                            int left_w,
                            int right_pane_x,
                            int right_pane_w,
                            int right_text_cols,
                            lvds_program_layout_t *out)
{
    (void)left_w;
    if (!out) {
        return;
    }

    out->row_y = 86;
    out->rows_bottom = 532;
    out->divider_x = (screen_w / 2) - 1;
    out->divider_y = 58;
    out->divider_h = 474;
    out->preview_title_x = left_x + 14;
    out->preview_title_y = 64;
    out->title_x = left_x + 204;
    out->title_y = 68;
    out->text_x = right_pane_x + 4;
    out->text_cols = right_text_cols;
    out->row_cell_x = right_pane_x - 2;
    out->row_cell_w = right_pane_w + 6;
    out->preview_active_x = left_x + 14;
    out->preview_active_y = 522;
    out->tool_panel_x = left_x + 294;
    out->tool_panel_y = 466;
    out->compact_max_rows = 23;
}


