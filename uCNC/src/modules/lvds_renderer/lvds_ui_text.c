#include "lvds_ui_text.h"

#include "lvds_draw_api.h"

#include <stdio.h>
#include <string.h>

#define LVDS_UI_TEXT_BUF 160
#define LVDS_UI_TEXT_CHAR_W 8

int lvds_ui_text_visible_len(const char *text)
{
    int len;

    if (!text) {
        return 0;
    }
    len = (int)strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) {
        len--;
    }
    return len;
}

int lvds_ui_text_wrapped_height(const char *text, int cols)
{
    int len;

    if (!text) {
        text = "";
    }
    if (cols < 1) {
        return 22;
    }
    len = lvds_ui_text_visible_len(text);
    return (len > cols) ? 42 : 22;
}

static int lvds_ui_text_wrap_break(const char *text, int cols)
{
    int i;
    int last_space = -1;
    int len;

    if (!text) {
        return 0;
    }
    len = lvds_ui_text_visible_len(text);
    if (len <= cols) {
        return len;
    }
    for (i = 0; i < cols && text[i]; ++i) {
        if (text[i] == ' ') {
            last_space = i;
        }
    }
    if (last_space > 0) {
        return last_space;
    }
    return cols;
}

static void lvds_ui_text_draw_value_with_highlight(int x,
                                                   int y,
                                                   const char *value,
                                                   bool has_hi,
                                                   uint8_t hi_start,
                                                   uint8_t hi_end,
                                                   int clear_cols,
                                                   lvds_color_t value_fg,
                                                   lvds_color_t bg,
                                                   lvds_color_t hi_fg,
                                                   lvds_color_t hi_bg,
                                                   int font)
{
    char left[LVDS_UI_TEXT_BUF];
    char mid[LVDS_UI_TEXT_BUF];
    char right[LVDS_UI_TEXT_BUF];
    int len;

    if (!value) {
        value = "";
    }
    len = (int)strlen(value);
    if (len > clear_cols) {
        len = clear_cols;
    }

    if (!has_hi || hi_end <= hi_start || hi_start >= (uint8_t)len) {
        char line[LVDS_UI_TEXT_BUF];
        snprintf(line, sizeof(line), "%-*.*s", clear_cols, clear_cols, value);
        lvds_draw_text(x, y, line, value_fg, bg, font);
        return;
    }

    if (hi_end > (uint8_t)len) {
        hi_end = (uint8_t)len;
    }

    snprintf(left, sizeof(left), "%.*s", (int)hi_start, value);
    snprintf(mid, sizeof(mid), "%.*s", (int)(hi_end - hi_start), value + hi_start);
    snprintf(right, sizeof(right), "%-*.*s",
             clear_cols - (int)hi_end, clear_cols - (int)hi_end, value + hi_end);

    lvds_draw_text(x, y, left, value_fg, bg, font);
    lvds_draw_text(x + ((int)hi_start * LVDS_UI_TEXT_CHAR_W), y, mid, hi_fg, hi_bg, font);
    lvds_draw_text(x + ((int)hi_end * LVDS_UI_TEXT_CHAR_W), y, right, value_fg, bg, font);
}

static void lvds_ui_text_draw_plain_segment(int x,
                                            int y,
                                            const char *text,
                                            int start,
                                            int cols,
                                            lvds_color_t fg,
                                            lvds_color_t bg,
                                            int font)
{
    char buf[LVDS_UI_TEXT_BUF];
    int len;

    if (!text) {
        text = "";
    }
    if (start < 0) {
        start = 0;
    }
    len = lvds_ui_text_visible_len(text);
    if (start > len) {
        start = len;
    }
    snprintf(buf, sizeof(buf), "%-*.*s", cols, cols, text + start);
    lvds_draw_text(x, y, buf, fg, bg, font);
}

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
                               uint8_t hi_end)
{
    int len;
    int second_cols;
    int break_at;
    int second_start;

    if (!text) {
        text = "";
    }
    if (cols < 1) {
        return;
    }

    len = lvds_ui_text_visible_len(text);
    if (len <= cols) {
        if (has_hi) {
            lvds_ui_text_draw_value_with_highlight(x, y, text, true, hi_start, hi_end,
                                                   cols, value_fg, bg, hi_fg, hi_bg, font);
        } else {
            lvds_ui_text_draw_plain_segment(x, y, text, 0, cols, fg, bg, font);
        }
        return;
    }

    break_at = has_hi ? cols : lvds_ui_text_wrap_break(text, cols);
    second_start = break_at;
    while (text[second_start] == ' ') {
        second_start++;
    }

    if (has_hi && hi_start < (uint8_t)break_at) {
        lvds_ui_text_draw_value_with_highlight(x, y, text, true, hi_start, hi_end,
                                               break_at, value_fg, bg, hi_fg, hi_bg, font);
    } else {
        lvds_ui_text_draw_plain_segment(x, y, text, 0, break_at, fg, bg, font);
    }
    if (break_at < cols) {
        lvds_ui_text_draw_plain_segment(x + (break_at * LVDS_UI_TEXT_CHAR_W),
                                        y, "", 0, cols - break_at, fg, bg, font);
    }

    second_cols = cols - 4;
    if (second_cols < 1) {
        second_cols = cols;
    }
    lvds_draw_text_clip(x, y + 20, "    ", 4, fg, bg, font);
    if (has_hi && hi_start >= (uint8_t)second_start) {
        lvds_ui_text_draw_value_with_highlight(x + 32,
                                               y + 20,
                                               text + second_start,
                                               true,
                                               (uint8_t)(hi_start - second_start),
                                               (hi_end > (uint8_t)second_start) ? (uint8_t)(hi_end - second_start) : 0,
                                               second_cols,
                                               value_fg,
                                               bg,
                                               hi_fg,
                                               hi_bg,
                                               font);
    } else {
        lvds_ui_text_draw_plain_segment(x + 32, y + 20, text, second_start,
                                        second_cols, fg, bg, font);
    }
}
