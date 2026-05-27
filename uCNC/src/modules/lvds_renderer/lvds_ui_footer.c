#include "lvds_ui_footer.h"

#include "lvds_draw_api.h"

#include <string.h>

static void lvds_ui_strcpy(char *dst, const char *src, size_t dst_sz)
{
    if (!dst || dst_sz == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    strncpy(dst, src, dst_sz - 1u);
    dst[dst_sz - 1u] = 0;
}

bool lvds_ui_footer_draw_menu(int screen_w,
                              int x,
                              int y,
                              const char *text,
                              lvds_color_t button_bg,
                              lvds_color_t button_fg,
                              lvds_color_t button_shadow,
                              lvds_color_t key_fg,
                              lvds_color_t footer_bg,
                              int font)
{
    char field[24];
    char key[8];
    char label[24];
    const char *p;
    int i;
    int fields = 1;
    int field_w;
    int key_w;
    int label_x;
    int button_x;
    int button_w;

    if (!text || !text[0]) {
        return false;
    }

    for (p = text; *p; ++p) {
        if (*p == '|') {
            fields++;
        }
    }
    if (fields < 2 || fields > 10) {
        return false;
    }
    field_w = (screen_w - (x * 2)) / fields;

    p = text;
    for (i = 0; i < fields; ++i) {
        const char *bar = strchr(p, '|');
        const char *space;
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (!p[0] || len == 0 || len >= sizeof(field)) {
            return false;
        }
        if (i < fields - 1 && !bar) {
            return false;
        }
        memcpy(field, p, len);
        field[len] = 0;

        key[0] = 0;
        label[0] = 0;
        space = strchr(field, ' ');
        if (space && space > field) {
            size_t key_len = (size_t)(space - field);
            size_t label_len;
            if (key_len >= sizeof(key)) {
                key_len = sizeof(key) - 1u;
            }
            memcpy(key, field, key_len);
            key[key_len] = 0;
            while (*space == ' ') {
                space++;
            }
            label_len = strlen(space);
            if (label_len >= sizeof(label)) {
                label_len = sizeof(label) - 1u;
            }
            memcpy(label, space, label_len);
            label[label_len] = 0;
        } else {
            lvds_ui_strcpy(label, field, sizeof(label));
        }

        label_x = x + (i * field_w);
        if (key[0]) {
            lvds_draw_text_clip(label_x, y + 3, key, 3, key_fg, footer_bg, font);
            key_w = lvds_draw_text_width(key, font) + 4;
        } else {
            key_w = 0;
        }

        button_x = label_x + key_w;
        button_w = field_w - key_w - 3;
        if (button_w < 16) {
            button_w = field_w - 3;
            button_x = label_x;
        }
        lvds_draw_fill_rect(button_x, y, button_w, 22, button_bg);
        lvds_draw_line(button_x, y + 21, button_x + button_w - 1, y + 21, button_shadow);
        lvds_draw_line(button_x + button_w - 1, y, button_x + button_w - 1, y + 21, button_shadow);
        lvds_draw_text_clip(button_x + 4, y + 3, label, (button_w - 8) / 8,
                            button_fg, button_bg, font);

        p = bar ? (bar + 1) : "";
    }

    return true;
}

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
                                int font)
{
    if (!helper_fallback) {
        helper_fallback = helper;
    }
    lvds_draw_fill_rect(0, footer_y, screen_w, footer_h, bg);
    lvds_draw_text_clip(message_x, message_y, message, text_cols, message_fg, bg, font);
    if (!lvds_ui_footer_draw_menu(screen_w,
                                  helper_x,
                                  helper_y,
                                  helper,
                                  button_bg,
                                  button_fg,
                                  button_shadow,
                                  key_fg,
                                  bg,
                                  font)) {
        lvds_draw_text_clip(helper_x, helper_y, helper_fallback, text_cols, helper_fg, bg, font);
    }
}
