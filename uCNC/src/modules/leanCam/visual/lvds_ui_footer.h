#ifndef LVDS_UI_FOOTER_H
#define LVDS_UI_FOOTER_H

#include "../../lvds_renderer/lvds_hstx.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool lvds_ui_footer_draw_menu(int screen_w,
                              int x,
                              int y,
                              const char *text,
                              lvds_color_t button_bg,
                              lvds_color_t button_fg,
                              lvds_color_t button_shadow,
                              lvds_color_t key_fg,
                              lvds_color_t footer_bg,
                              int font);
void lvds_ui_footer_draw_status(int screen_w,
                                int footer_y,
                                int footer_h,
                                int message_x,
                                int message_y,
                                int helper_x,
                                int helper_y,
                                int text_cols,
                                const char *message,
                                const char *helper,
                                const char *helper_fallback,
                                lvds_color_t bg,
                                lvds_color_t message_fg,
                                lvds_color_t helper_fg,
                                lvds_color_t button_bg,
                                lvds_color_t button_fg,
                                lvds_color_t button_shadow,
                                lvds_color_t key_fg,
                                int font);

#ifdef __cplusplus
}
#endif

#endif

