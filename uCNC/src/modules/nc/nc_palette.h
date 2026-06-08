#ifndef NC_PALETTE_H
#define NC_PALETTE_H

#include "../lvds_renderer/lvds_draw_api.h"

#ifdef __cplusplus
extern "C" {
#endif

void nc_palette_init(void);

lvds_color_t nc_col_bg(void);
lvds_color_t nc_col_panel(void);
lvds_color_t nc_col_header(void);
lvds_color_t nc_col_text(void);
lvds_color_t nc_col_dim(void);
lvds_color_t nc_col_accent(void);
lvds_color_t nc_col_select(void);
lvds_color_t nc_col_line_no_selected(void);
lvds_color_t nc_col_word_bg(void);
lvds_color_t nc_col_word_fg(void);
lvds_color_t nc_col_error(void);
lvds_color_t nc_col_preview_bg(void);
lvds_color_t nc_col_preview_frame(void);
lvds_color_t nc_col_preview_stock(void);
lvds_color_t nc_col_preview_cut(void);
lvds_color_t nc_col_preview_hatch(void);
lvds_color_t nc_col_preview_profile(void);
lvds_color_t nc_col_preview_chuck(void);
lvds_color_t nc_col_preview_chuck_text(void);
lvds_color_t nc_col_footer_bg(void);
lvds_color_t nc_col_footer_text(void);
lvds_color_t nc_col_footer_value(void);
lvds_color_t nc_col_footer_button(void);
lvds_color_t nc_col_tool_mark(void);
lvds_color_t nc_col_tool_fill(void);
lvds_color_t nc_col_tool_crosshair(void);

#define NC_VISUAL_BG                  nc_col_bg()
#define NC_VISUAL_PANEL               nc_col_panel()
#define NC_VISUAL_HEADER              nc_col_header()
#define NC_VISUAL_TEXT                nc_col_text()
#define NC_VISUAL_DIM                 nc_col_dim()
#define NC_VISUAL_ACCENT              nc_col_accent()
#define NC_VISUAL_SELECT              nc_col_select()
#define NC_VISUAL_LINE_NO_SELECTED    nc_col_line_no_selected()
#define NC_VISUAL_WORD_BG             nc_col_word_bg()
#define NC_VISUAL_WORD_FG             nc_col_word_fg()
#define NC_VISUAL_ERROR               nc_col_error()
#define NC_VISUAL_PREVIEW_BG          nc_col_preview_bg()
#define NC_VISUAL_PREVIEW_FRAME       nc_col_preview_frame()
#define NC_VISUAL_PREVIEW_STOCK       nc_col_preview_stock()
#define NC_VISUAL_PREVIEW_CUT         nc_col_preview_cut()
#define NC_VISUAL_PREVIEW_HATCH       nc_col_preview_hatch()
#define NC_VISUAL_PREVIEW_PROFILE     nc_col_preview_profile()
#define NC_VISUAL_PREVIEW_CHUCK       nc_col_preview_chuck()
#define NC_VISUAL_PREVIEW_CHUCK_TEXT  nc_col_preview_chuck_text()
#define NC_VISUAL_FOOTER_BG           nc_col_footer_bg()
#define NC_VISUAL_FOOTER_TEXT         nc_col_footer_text()
#define NC_VISUAL_FOOTER_VALUE        nc_col_footer_value()
#define NC_VISUAL_FOOTER_BUTTON       nc_col_footer_button()
#define NC_VISUAL_TOOL_MARK           nc_col_tool_mark()
#define NC_VISUAL_TOOL_FILL           nc_col_tool_fill()
#define NC_VISUAL_TOOL_CROSSHAIR      nc_col_tool_crosshair()

#ifdef __cplusplus
}
#endif

#endif
