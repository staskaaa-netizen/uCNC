/* LeanCam visual contract:
 * Purpose: draw program rows, preview graphics, tool glyphs, and sim preview from snapshots.
 * Called by: leancam_visual during LeanCam program/tool screens.
 * Calls into: LVDS primitives and visual text/layout helpers; it may decode snapshot text for drawing only.
 * Owns: visual-only preview cache; it must not request generation, file I/O, or editor actions.
 */
#include "lvds_ui_program.h"

#include "../../lvds_renderer/lvds_draw_api.h"

void lvds_ui_program_draw_shell(int screen_w,
                                const lvds_program_layout_t *layout,
                                const char *preview_title,
                                const char *subtitle,
                                bool show_subtitle,
                                lvds_color_t bg,
                                lvds_color_t line,
                                lvds_color_t title_fg,
                                lvds_color_t subtitle_fg,
                                int title_font,
                                int subtitle_font)
{
    if (!layout) {
        return;
    }

    lvds_draw_fill_rect(0, 42, screen_w, 558, bg);
    lvds_draw_fill_rect(layout->divider_x, layout->divider_y, 2, layout->divider_h, line);
    lvds_draw_text_clip(layout->preview_title_x,
                        layout->preview_title_y,
                        preview_title ? preview_title : "",
                        14,
                        title_fg,
                        bg,
                        title_font);
    if (show_subtitle && subtitle && subtitle[0]) {
        lvds_draw_text_clip(layout->title_x,
                            layout->title_y,
                            subtitle,
                            16,
                            subtitle_fg,
                            bg,
                            subtitle_font);
    }
}


