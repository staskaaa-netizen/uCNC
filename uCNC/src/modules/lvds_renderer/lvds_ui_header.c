#include "lvds_ui_header.h"

#include "lvds_draw_api.h"

#include <stdbool.h>
#include <stdio.h>

void lvds_ui_header_draw(int screen_w,
                         const char *state_text,
                         float x,
                         float z,
                         float feed,
                         unsigned spindle,
                         lvds_color_t bg,
                         lvds_color_t fg,
                         int font)
{
    static bool header_bg_ready;
    char buf[96];

    if (!state_text) {
        state_text = "";
    }
    if (!header_bg_ready) {
        lvds_draw_fill_rect(0, 0, screen_w, 42, bg);
        header_bg_ready = true;
    }
    snprintf(buf, sizeof(buf), "%-5s X:%7.3f Z:%7.3f F:%5.1f S:%-6u   ",
             state_text, (double)x, (double)z, (double)feed, spindle);
    lvds_draw_text(12, 10, buf, fg, bg, font);
}
