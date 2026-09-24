#include "nc_palette.h"

#include "../lvds_renderer/lvds_palette.h"

void nc_palette_init(void)
{
    lvds_palette_init();
}

lvds_color_t nc_col_bg(void) { return lvds_palette_element(LC_ELEM_BACKGROUND); }
lvds_color_t nc_col_panel(void) { return lvds_palette_element(LC_ELEM_PANEL); }
lvds_color_t nc_col_header(void) { return lvds_palette_element(LC_ELEM_HEADER); }
/* The DRO's background while the machine is in a run: the panel's own green -
   a colour it already owns (`gray_192`..`tool_tip` is the whole 16-colour table
   the HSTX palette holds, so this cannot be a new one). The loud colour is spent
   only while the machine is actually running - a state the panel is not in must
   not shout - and a fault takes it away again rather than sharing it with the
   red alert. The strip above keeps its grey, so the band is the one thing that
   changes. */
lvds_color_t nc_col_header_run(void) { return lvds_palette_color(green); }
lvds_color_t nc_col_text(void) { return lvds_palette_element(LC_ELEM_TEXT); }
lvds_color_t nc_col_dim(void) { return lvds_palette_element(LC_ELEM_SECONDARY_TEXT); }
lvds_color_t nc_col_accent(void) { return lvds_palette_element(LC_ELEM_TEXT); }
lvds_color_t nc_col_select(void) { return lvds_palette_element(LC_ELEM_SELECTED_ROW); }
/* The weaker mark beside the line RUN is on: the source lines of the cycle that
   line belongs to. The colour table is full (sixteen entries, the renderer's
   cap), so this is the palette's own `yellow_light` - the pale yellow it already
   owns and no NC screen used - rather than a new colour. */
lvds_color_t nc_col_select_block(void) { return lvds_palette_color(yellow_light); }
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
lvds_color_t nc_col_tool_fill(void) { return lvds_palette_color(tool_orange); }
lvds_color_t nc_col_tool_crosshair(void) { return lvds_palette_element(LC_ELEM_PREVIEW_TOOL); }
/* The tool tip block of the TOOLS screen: its colour is its own, because the
   block is a reading of the tipped tool, not another line of the table. */
lvds_color_t nc_col_tool_tip(void) { return lvds_palette_color(tool_tip); }
