#ifndef LVDS_UI_HEADER_H
#define LVDS_UI_HEADER_H

#include "lvds_hstx.h"

#ifdef __cplusplus
extern "C" {
#endif

void lvds_ui_header_draw(int screen_w,
                         const char *state_text,
                         float x,
                         float z,
                         float feed,
                         unsigned spindle,
                         lvds_color_t bg,
                         lvds_color_t fg,
                         int font);

#ifdef __cplusplus
}
#endif

#endif
