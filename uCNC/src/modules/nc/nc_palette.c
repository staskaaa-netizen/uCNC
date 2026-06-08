#include "nc_palette.h"

#include "../lvds_renderer/lvds_palette.h"

void nc_palette_init(void)
{
    lvds_palette_init();
}

lvds_color_t nc_col_bg(void) { return lvds_palette_element(LC_ELEM_BACKGROUND); }
lvds_color_t nc_col_panel(void) { return lvds_palette_element(LC_ELEM_PANEL); }
lvds_color_t nc_col_header(void) { return lvds_palette_element(LC_ELEM_HEADER); }
lvds_color_t nc_col_text(void) { return lvds_palette_element(LC_ELEM_TEXT); }
lvds_color_t nc_col_dim(void) { return lvds_palette_element(LC_ELEM_SECONDARY_TEXT); }
lvds_color_t nc_col_accent(void) { return lvds_palette_element(LC_ELEM_VALUE_TEXT); }
lvds_color_t nc_col_select(void) { return lvds_palette_element(LC_ELEM_SELECTED_ROW); }
lvds_color_t nc_col_line_no_selected(void) { return lvds_palette_color(black); }
lvds_color_t nc_col_word_bg(void) { return lvds_palette_color(black); }
lvds_color_t nc_col_word_fg(void) { return lvds_palette_color(white_warm); }
lvds_color_t nc_col_error(void) { return lvds_palette_element(LC_ELEM_ERROR); }
lvds_color_t nc_col_preview_bg(void) { return lvds_palette_element(LC_ELEM_PREVIEW_BG); }
lvds_color_t nc_col_preview_frame(void) { return lvds_palette_element(LC_ELEM_PREVIEW_FRAME); }
lvds_color_t nc_col_preview_stock(void) { return lvds_palette_element(LC_ELEM_PREVIEW_STOCK); }
lvds_color_t nc_col_preview_cut(void) { return lvds_palette_element(LC_ELEM_PREVIEW_CUT); }
lvds_color_t nc_col_preview_hatch(void) { return lvds_palette_element(LC_ELEM_PREVIEW_HATCH); }
lvds_color_t nc_col_preview_profile(void) { return lvds_palette_element(LC_ELEM_PREVIEW_PROFILE); }
lvds_color_t nc_col_preview_chuck(void) { return lvds_palette_element(LC_ELEM_PREVIEW_CHUCK); }
lvds_color_t nc_col_preview_chuck_text(void) { return lvds_palette_element(LC_ELEM_PREVIEW_CHUCK_TEXT); }
lvds_color_t nc_col_footer_bg(void) { return lvds_palette_element(LC_ELEM_FOOTER_BG); }
lvds_color_t nc_col_footer_text(void) { return lvds_palette_element(LC_ELEM_FOOTER_TEXT); }
lvds_color_t nc_col_footer_value(void) { return lvds_palette_element(LC_ELEM_FOOTER_VALUE); }
lvds_color_t nc_col_footer_button(void) { return lvds_palette_color(white_warm); }
lvds_color_t nc_col_tool_mark(void) { return lvds_palette_element(LC_ELEM_PREVIEW_TOOL_MARK); }
lvds_color_t nc_col_tool_fill(void) { return lvds_palette_color(yellow); }
lvds_color_t nc_col_tool_crosshair(void) { return lvds_palette_element(LC_ELEM_PREVIEW_TOOL); }
