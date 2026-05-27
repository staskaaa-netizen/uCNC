#ifndef LVDS_UI_TEXT_H
#define LVDS_UI_TEXT_H

#include "lvds_hstx.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int lvds_ui_text_visible_len(const char *text);
int lvds_ui_text_wrapped_height(const char *text, int cols);
void lvds_ui_text_draw_wrapped(int x,
                               int y,
                               const char *text,
                               int cols,
                               lvds_color_t fg,
                               lvds_color_t bg,
                               lvds_color_t value_fg,
                               lvds_color_t hi_fg,
                               lvds_color_t hi_bg,
                               int font,
                               bool has_hi,
                               uint8_t hi_start,
                               uint8_t hi_end);

#ifdef __cplusplus
}
#endif

#endif
