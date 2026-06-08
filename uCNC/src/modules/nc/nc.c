#include "nc.h"
#include "../file_system.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NC_LOAD_TIMEOUT_MS
#define NC_LOAD_TIMEOUT_MS 1500u
#endif

static int nc_stricmp_ascii(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a++);
        int cb = tolower((unsigned char)*b++);
        if (ca != cb) {
            return ca - cb;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static bool nc_has_suffix_ci(const char *text, const char *suffix)
{
    size_t text_len;
    size_t suffix_len;

    if (!text || !suffix) {
        return false;
    }

    text_len = strlen(text);
    suffix_len = strlen(suffix);
    if (suffix_len > text_len) {
        return false;
    }

    return nc_stricmp_ascii(text + text_len - suffix_len, suffix) == 0;
}

static nc_result_t nc_copy_line_text(char *dst, size_t dst_sz, const char *text)
{
    size_t len;

    if (!dst || dst_sz == 0 || !text) {
        return NC_ERR_BAD_ARG;
    }

    len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        len--;
    }

    if (len >= dst_sz) {
        return NC_ERR_LINE_TOO_LONG;
    }

    memcpy(dst, text, len);
    dst[len] = '\0';
    return nc_line_has_old_pipe_syntax(dst) ? NC_ERR_OLD_PIPE_SYNTAX : NC_OK;
}

static size_t nc_wrapped_chunk_len(const char *text, size_t max_len)
{
    size_t len;
    size_t i;
    size_t split = 0;

    if (!text || max_len == 0) {
        return 0;
    }
    len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        len--;
    }
    if (len < max_len) {
        return len;
    }
    for (i = 1; i < max_len; i++) {
        if (text[i] == ' ' || text[i] == '\t') {
            split = i;
        }
    }
    return split > 8 ? split : max_len - 1;
}

static nc_result_t nc_insert_line_raw(nc_document_t *doc, size_t line_index, const char *text)
{
    size_t i;
    nc_result_t r;

    if (!doc || line_index > doc->line_count) {
        return NC_ERR_BAD_ARG;
    }
    if (doc->line_count >= NC_MAX_LINES) {
        return NC_ERR_TOO_MANY_LINES;
    }

    for (i = doc->line_count; i > line_index; i--) {
        doc->lines[i] = doc->lines[i - 1];
    }

    r = nc_copy_line_text(doc->lines[line_index].text, NC_MAX_LINE_LEN, text ? text : "");
    if (r != NC_OK) {
        return r;
    }
    doc->line_count++;
    return NC_OK;
}

static int nc_current_word_count(const nc_document_t *doc, nc_word_t *words, int max_words)
{
    if (!doc || doc->cursor_line >= doc->line_count) {
        return 0;
    }
    return nc_parse_words(doc->lines[doc->cursor_line].text, words, max_words);
}

const char *nc_result_text(nc_result_t result)
{
    switch (result) {
    case NC_OK: return "ok";
    case NC_ERR_BAD_ARG: return "bad argument";
    case NC_ERR_UNSUPPORTED_FILE: return "unsupported NC file";
    case NC_ERR_OLD_PIPE_SYNTAX: return "old pipe syntax is not supported";
    case NC_ERR_TOO_MANY_LINES: return "too many NC lines";
    case NC_ERR_LINE_TOO_LONG: return "NC line too long";
    case NC_ERR_IO: return "file IO failed";
    case NC_ERR_NO_WORD: return "no word selected";
    default: return "unknown NC error";
    }
}

void nc_document_init(nc_document_t *doc)
{
    if (!doc) {
        return;
    }
    memset(doc, 0, sizeof(*doc));
    doc->selected_word = -1;
}

bool nc_path_supported(const char *path)
{
    if (!path || !*path) {
        return false;
    }
    return nc_has_suffix_ci(path, ".nc") ||
           nc_has_suffix_ci(path, ".ngc") ||
           nc_has_suffix_ci(path, ".gcode") ||
           nc_has_suffix_ci(path, ".t");
}

bool nc_line_has_old_pipe_syntax(const char *line)
{
    return line && strchr(line, '|') != NULL;
}

nc_result_t nc_load_file(nc_document_t *doc, const char *path)
{
    fs_file_t *fp;
    char buf[NC_MAX_LINE_LEN * 4];
    size_t used = 0;
    uint32_t start_ms;
    nc_result_t r;

    if (!doc || !path) {
        return NC_ERR_BAD_ARG;
    }
    if (!nc_path_supported(path)) {
        return NC_ERR_UNSUPPORTED_FILE;
    }

    fp = fs_open(path, "r");
    if (!fp) {
        return NC_ERR_IO;
    }

    nc_document_init(doc);
    start_ms = mcu_millis();
    while (fs_available(fp)) {
        char c;
        if ((uint32_t)(mcu_millis() - start_ms) > NC_LOAD_TIMEOUT_MS) {
            fs_close(fp);
            nc_document_init(doc);
            return NC_ERR_IO;
        }
        if (fs_read(fp, (uint8_t *)&c, 1) != 1) {
            fs_close(fp);
            nc_document_init(doc);
            return NC_ERR_IO;
        }

        if (c == '\n') {
            buf[used] = '\0';
            r = nc_insert_line(doc, doc->line_count, buf);
            if (r != NC_OK) {
                fs_close(fp);
                nc_document_init(doc);
                return r;
            }
            used = 0;
            continue;
        }

        if (used + 1 >= sizeof(buf)) {
            buf[used] = '\0';
            r = nc_insert_line(doc, doc->line_count, buf);
            if (r != NC_OK) {
                fs_close(fp);
                nc_document_init(doc);
                return r;
            }
            used = 0;
        }
        buf[used++] = c;
    }

    if (used > 0 || doc->line_count == 0) {
        buf[used] = '\0';
        r = nc_insert_line(doc, doc->line_count, buf);
        if (r != NC_OK) {
            fs_close(fp);
            nc_document_init(doc);
            return r;
        }
    }
    fs_close(fp);

    strncpy(doc->path, path, sizeof(doc->path) - 1);
    doc->path[sizeof(doc->path) - 1] = '\0';
    doc->dirty = false;
    return NC_OK;
}

nc_result_t nc_save_file(nc_document_t *doc, const char *path)
{
    fs_file_t *fp;
    size_t i;
    const char *save_path;

    if (!doc) {
        return NC_ERR_BAD_ARG;
    }
    save_path = (path && *path) ? path : doc->path;
    if (!save_path || !*save_path) {
        return NC_ERR_BAD_ARG;
    }
    if (!nc_path_supported(save_path)) {
        return NC_ERR_UNSUPPORTED_FILE;
    }

    fp = fs_open(save_path, "w");
    if (!fp) {
        return NC_ERR_IO;
    }
    for (i = 0; i < doc->line_count; i++) {
        size_t len = strlen(doc->lines[i].text);
        if ((len && fs_write(fp, (const uint8_t *)doc->lines[i].text, len) != len) ||
            fs_write(fp, (const uint8_t *)"\n", 1) != 1) {
            fs_close(fp);
            return NC_ERR_IO;
        }
    }
    fs_close(fp);

    strncpy(doc->path, save_path, sizeof(doc->path) - 1);
    doc->path[sizeof(doc->path) - 1] = '\0';
    doc->dirty = false;
    return NC_OK;
}

nc_result_t nc_set_line(nc_document_t *doc, size_t line_index, const char *text)
{
    nc_result_t r;

    if (!doc || line_index >= doc->line_count) {
        return NC_ERR_BAD_ARG;
    }
    r = nc_copy_line_text(doc->lines[line_index].text, NC_MAX_LINE_LEN, text);
    if (r == NC_ERR_LINE_TOO_LONG) {
        r = nc_delete_line(doc, line_index);
        if (r != NC_OK) {
            return r;
        }
        return nc_insert_line(doc, line_index, text);
    }
    if (r != NC_OK) {
        return r;
    }
    doc->dirty = true;
    doc->selected_word = -1;
    return NC_OK;
}

nc_result_t nc_insert_line(nc_document_t *doc, size_t line_index, const char *text)
{
    const char *p;
    size_t at;
    bool continuation = false;
    nc_result_t r;

    if (!doc || line_index > doc->line_count) {
        return NC_ERR_BAD_ARG;
    }
    if (text && nc_line_has_old_pipe_syntax(text)) {
        return NC_ERR_OLD_PIPE_SYNTAX;
    }

    p = text ? text : "";
    at = line_index;
    do {
        char chunk[NC_MAX_LINE_LEN];
        size_t prefix = continuation ? 1u : 0u;
        size_t max_body = NC_WRAP_LINE_LEN - prefix;
        size_t n;

        if (max_body > NC_MAX_LINE_LEN - prefix) {
            max_body = NC_MAX_LINE_LEN - prefix;
        }
        n = nc_wrapped_chunk_len(p, max_body);

        if (prefix) {
            chunk[0] = '\t';
        }
        memcpy(chunk + prefix, p, n);
        chunk[prefix + n] = '\0';

        r = nc_insert_line_raw(doc, at, chunk);
        if (r != NC_OK) {
            return r;
        }
        at++;
        p += n;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        continuation = true;
    } while (*p);

    doc->cursor_line = line_index;
    doc->selected_word = -1;
    doc->dirty = true;
    return NC_OK;
}

nc_result_t nc_delete_line(nc_document_t *doc, size_t line_index)
{
    size_t i;

    if (!doc || line_index >= doc->line_count) {
        return NC_ERR_BAD_ARG;
    }

    for (i = line_index; i + 1 < doc->line_count; i++) {
        doc->lines[i] = doc->lines[i + 1];
    }
    doc->line_count--;
    if (doc->cursor_line >= doc->line_count && doc->line_count > 0) {
        doc->cursor_line = doc->line_count - 1;
    }
    if (doc->line_count == 0) {
        doc->cursor_line = 0;
    }
    doc->selected_word = -1;
    doc->dirty = true;
    return NC_OK;
}

void nc_cursor_up(nc_document_t *doc)
{
    if (doc && doc->cursor_line > 0) {
        doc->cursor_line--;
        doc->selected_word = -1;
    }
}

void nc_cursor_down(nc_document_t *doc)
{
    if (doc && doc->cursor_line + 1 < doc->line_count) {
        doc->cursor_line++;
        doc->selected_word = -1;
    }
}

int nc_parse_words(const char *line, nc_word_t *words, int max_words)
{
    int count = 0;
    int i = 0;

    if (!line || !words || max_words <= 0) {
        return 0;
    }

    while (line[i] != '\0') {
        int start;
        int end;
        char *parse_end;

        if (line[i] == '(') {
            while (line[i] && line[i] != ')') {
                i++;
            }
            if (line[i] == ')') {
                i++;
            }
            continue;
        }

        if (!isalpha((unsigned char)line[i])) {
            i++;
            continue;
        }

        start = i;
        i++;
        while (line[i] == ' ' || line[i] == '\t') {
            i++;
        }
        if (!(line[i] == '+' || line[i] == '-' || line[i] == '.' || isdigit((unsigned char)line[i]))) {
            continue;
        }

        (void)strtod(line + i, &parse_end);
        if (parse_end == line + i) {
            continue;
        }
        end = (int)(parse_end - line);
        words[count].letter = (char)toupper((unsigned char)line[start]);
        words[count].start = start;
        words[count].end = end;
        count++;
        if (count >= max_words) {
            break;
        }
        i = end;
    }

    return count;
}

bool nc_word_value(const char *line, const nc_word_t *word, float *value)
{
    char *parse_end;

    if (!line || !word || !value || word->start < 0 || word->end <= word->start) {
        return false;
    }

    *value = (float)strtod(line + word->start + 1, &parse_end);
    return parse_end == line + word->end;
}

nc_result_t nc_select_next_word(nc_document_t *doc)
{
    nc_word_t words[24];
    int count = nc_current_word_count(doc, words, 24);

    if (!doc || count <= 0) {
        return NC_ERR_NO_WORD;
    }
    if (doc->selected_word < 0 || doc->selected_word + 1 >= count) {
        doc->selected_word = 0;
    } else {
        doc->selected_word++;
    }
    return NC_OK;
}

nc_result_t nc_select_prev_word(nc_document_t *doc)
{
    nc_word_t words[24];
    int count = nc_current_word_count(doc, words, 24);

    if (!doc || count <= 0) {
        return NC_ERR_NO_WORD;
    }
    if (doc->selected_word <= 0 || doc->selected_word >= count) {
        doc->selected_word = count - 1;
    } else {
        doc->selected_word--;
    }
    return NC_OK;
}

nc_result_t nc_get_selected_word(const nc_document_t *doc, nc_word_t *word)
{
    nc_word_t words[24];
    int count;

    if (!doc || !word) {
        return NC_ERR_BAD_ARG;
    }
    count = nc_current_word_count(doc, words, 24);
    if (doc->selected_word < 0 || doc->selected_word >= count) {
        return NC_ERR_NO_WORD;
    }
    *word = words[doc->selected_word];
    return NC_OK;
}

nc_result_t nc_set_selected_word_text(nc_document_t *doc, const char *value_text)
{
    nc_word_t word;
    char new_line[NC_MAX_LINE_LEN];
    const char *old_line;
    int prefix_len;
    int suffix_len;
    int value_len;

    if (!doc || !value_text || doc->cursor_line >= doc->line_count) {
        return NC_ERR_BAD_ARG;
    }
    if (nc_get_selected_word(doc, &word) != NC_OK) {
        return NC_ERR_NO_WORD;
    }

    old_line = doc->lines[doc->cursor_line].text;
    prefix_len = word.start + 1;
    suffix_len = (int)strlen(old_line + word.end);
    value_len = (int)strlen(value_text);
    if (prefix_len + value_len + suffix_len >= NC_MAX_LINE_LEN) {
        return NC_ERR_LINE_TOO_LONG;
    }

    memcpy(new_line, old_line, (size_t)prefix_len);
    memcpy(new_line + prefix_len, value_text, (size_t)value_len);
    memcpy(new_line + prefix_len + value_len, old_line + word.end, (size_t)suffix_len + 1);
    strcpy(doc->lines[doc->cursor_line].text, new_line);
    doc->dirty = true;
    return NC_OK;
}

nc_result_t nc_change_selected_word_value(nc_document_t *doc, float value)
{
    char new_value[20];
    int written = snprintf(new_value, sizeof(new_value), "%.4g", (double)value);

    if (written <= 0 || written >= (int)sizeof(new_value)) {
        return NC_ERR_LINE_TOO_LONG;
    }

    return nc_set_selected_word_text(doc, new_value);
}
