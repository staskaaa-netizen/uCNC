#ifndef NC_H
#define NC_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NC_MAX_LINES
#define NC_MAX_LINES 256
#endif

#ifndef NC_MAX_LINE_LEN
#define NC_MAX_LINE_LEN 96
#endif

#ifndef NC_WRAP_LINE_LEN
#define NC_WRAP_LINE_LEN 46
#endif

#ifndef NC_MAX_VISIBLE_LINES
#define NC_MAX_VISIBLE_LINES 18
#endif

#define NC_PATH_MAX 96

typedef enum {
    NC_OK = 0,
    NC_ERR_BAD_ARG,
    NC_ERR_UNSUPPORTED_FILE,
    NC_ERR_OLD_PIPE_SYNTAX,
    NC_ERR_TOO_MANY_LINES,
    NC_ERR_LINE_TOO_LONG,
    NC_ERR_IO,
    NC_ERR_NO_WORD
} nc_result_t;

typedef struct {
    char text[NC_MAX_LINE_LEN];
} nc_line_t;

typedef struct {
    char letter;
    int start;
    int end;
} nc_word_t;

typedef struct {
    nc_line_t lines[NC_MAX_LINES];
    size_t line_count;
    size_t cursor_line;
    int selected_word;
    char path[NC_PATH_MAX];
    bool dirty;
} nc_document_t;

const char *nc_result_text(nc_result_t result);

void nc_document_init(nc_document_t *doc);
bool nc_path_supported(const char *path);
bool nc_line_has_old_pipe_syntax(const char *line);

nc_result_t nc_load_file(nc_document_t *doc, const char *path);
nc_result_t nc_save_file(nc_document_t *doc, const char *path);

nc_result_t nc_set_line(nc_document_t *doc, size_t line_index, const char *text);
nc_result_t nc_insert_line(nc_document_t *doc, size_t line_index, const char *text);
nc_result_t nc_delete_line(nc_document_t *doc, size_t line_index);
void nc_cursor_up(nc_document_t *doc);
void nc_cursor_down(nc_document_t *doc);

int nc_parse_words(const char *line, nc_word_t *words, int max_words);
bool nc_word_value(const char *line, const nc_word_t *word, float *value);
nc_result_t nc_select_next_word(nc_document_t *doc);
nc_result_t nc_select_prev_word(nc_document_t *doc);
nc_result_t nc_get_selected_word(const nc_document_t *doc, nc_word_t *word);
nc_result_t nc_set_selected_word_text(nc_document_t *doc, const char *value_text);
nc_result_t nc_change_selected_word_value(nc_document_t *doc, float value);

#ifdef __cplusplus
}
#endif

#endif
