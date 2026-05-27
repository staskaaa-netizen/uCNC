#ifndef LVDS_UI_PROGRAM_H
#define LVDS_UI_PROGRAM_H

#include "lvds_hstx.h"
#include "lvds_ui_layout.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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
                                int subtitle_font);

#ifdef __cplusplus
}
#endif

#endif
