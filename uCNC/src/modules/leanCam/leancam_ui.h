#ifndef LEANCAM_UI_H
#define LEANCAM_UI_H

#include <stdbool.h>
#include "conv_core.h"

#define LEANCAM_INPUT_MAX 64
#define LEANCAM_PATH_MAX  96

typedef struct {
    program_t prog;
    int cur_line;

    bool draft_active;
    int draft_fs;
    int draft_fe;
    int draft_insert_after;
    int draft_replace_index;
    char draft_line[MAX_LEN];
    char input_buf[LEANCAM_INPUT_MAX];

    char current_path[LEANCAM_PATH_MAX];
    bool dirty;
} leancam_ui_t;

void leancam_ui_init(leancam_ui_t *ui);
void leancam_ui_new(leancam_ui_t *ui);

bool leancam_ui_begin_template(leancam_ui_t *ui, const char *tmpl);
bool leancam_ui_begin_edit_current(leancam_ui_t *ui);
bool leancam_ui_accept_field(leancam_ui_t *ui);
void leancam_ui_cancel_draft(leancam_ui_t *ui);
bool leancam_ui_commit_draft(leancam_ui_t *ui);

void leancam_ui_input_char(leancam_ui_t *ui, int ch);
void leancam_ui_backspace(leancam_ui_t *ui);

void leancam_ui_move_up(leancam_ui_t *ui);
void leancam_ui_move_down(leancam_ui_t *ui);

/* Draft field navigation: press-brake style column selection. */
void leancam_ui_move_field_prev(leancam_ui_t *ui);
void leancam_ui_move_field_next(leancam_ui_t *ui);
void leancam_ui_delete_line(leancam_ui_t *ui);

bool leancam_ui_save(leancam_ui_t *ui, const char *path);
bool leancam_ui_load(leancam_ui_t *ui, const char *path);

void leancam_get_module_name(const char *line, char *out, int out_sz);

#endif