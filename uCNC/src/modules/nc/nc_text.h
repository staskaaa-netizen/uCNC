#ifndef NC_TEXT_H
#define NC_TEXT_H

#include "nc.h"
#include "../lvds_renderer/lvds_draw_api.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void nc_text_draw_line_with_word(const char *line,
                                 int x,
                                 int y,
                                 int cols,
                                 int char_w,
                                 int row_h,
                                 int word_start,
                                 int word_end,
                                 bool selected,
                                 lvds_color_t text_fg,
                                 lvds_color_t dim_fg,
                                 lvds_color_t bg,
                                 lvds_color_t selected_bg,
                                 lvds_color_t word_fg,
                                 lvds_color_t word_bg);

typedef struct {
    bool active;
    char buf[24];
    size_t line;
    int word;
} nc_text_edit_t;

void nc_text_edit_clear(nc_text_edit_t *edit);
bool nc_text_edit_handle_key(nc_document_t *doc,
                             nc_text_edit_t *edit,
                             char key,
                             char *status,
                             size_t status_sz);
const char *nc_text_edit_buffer(const nc_text_edit_t *edit);
bool nc_text_edit_active(const nc_text_edit_t *edit);

#ifdef __cplusplus
}
#endif

#endif
