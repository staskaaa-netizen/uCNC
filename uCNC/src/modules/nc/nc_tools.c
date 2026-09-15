#include "nc_tools.h"

#include "../file_system.h"

#include <stdio.h>
#include <string.h>

#ifndef NC_TOOL_MAX
#define NC_TOOL_MAX 8
#endif

static bool nc_tool_word_float(const char *line, char letter, float *out)
{
    nc_word_t words[16];
    int count;
    int i;

    count = nc_parse_words(line, words, 16);
    for (i = 0; i < count; i++) {
        if (words[i].letter == letter) {
            return nc_word_value(line, &words[i], out);
        }
    }
    return false;
}

const char *nc_tool_default_line(int t)
{
    static char line[NC_MAX_LINE_LEN];

    if (t < 0 || t > NC_TOOL_MAX) {
        return NULL;
    }
    if (t == 0) {
        snprintf(line, sizeof(line), "T0 R0 O0 F0 Q0 D0 E0 S800 X0 Z0");
    } else {
        snprintf(line, sizeof(line), "T%d R0.8 O3 F120 Q60 D2.0 E0.5 S800 X0 Z0", t);
    }
    return line;
}

bool nc_tool_line_is_tool(const char *line)
{
    nc_word_t words[4];
    int count;
    int i;

    if (!line || !line[0]) {
        return false;
    }
    count = nc_parse_words(line, words, 4);
    for (i = 0; i < count; i++) {
        if (words[i].letter == 'T') {
            return true;
        }
        if (words[i].letter == 'G' || words[i].letter == 'M') {
            return false;
        }
    }
    return false;
}

bool nc_tool_orient_valid(int orient)
{
    int digits = 0;

    if (orient == 0) {
        return true;
    }
    if (orient < 0 || orient > 9999) {
        return false;
    }
    while (orient > 0) {
        int d = orient % 10;
        if (d < 1 || d > 9) {
            return false;
        }
        orient /= 10;
        digits++;
    }
    return digits >= 1 && digits <= 4;
}

bool nc_tool_field_text(const char *line, char letter, char *out, size_t out_sz)
{
    nc_word_t words[16];
    int count;
    int i;
    size_t len;

    if (!out || out_sz == 0) {
        return false;
    }
    out[0] = '\0';
    if (!line) {
        return false;
    }

    count = nc_parse_words(line, words, 16);
    for (i = 0; i < count; i++) {
        if (words[i].letter == letter && words[i].end > words[i].start + 1) {
            len = (size_t)(words[i].end - words[i].start - 1);
            if (len >= out_sz) {
                len = out_sz - 1;
            }
            memcpy(out, line + words[i].start + 1, len);
            out[len] = '\0';
            return true;
        }
    }
    return false;
}

bool nc_tool_from_line(const char *line, nc_tool_t *tool)
{
    float v;

    if (!tool) {
        return false;
    }
    memset(tool, 0, sizeof(*tool));
    tool->rpm = 800.0f;
    if (!nc_tool_line_is_tool(line) || !nc_tool_word_float(line, 'T', &v)) {
        return false;
    }

    tool->t = (int)(v + 0.5f);
    (void)nc_tool_word_float(line, 'R', &tool->r);
    if (nc_tool_word_float(line, 'O', &v)) {
        tool->orient = (int)(v + 0.5f);
    }
    if (!nc_tool_orient_valid(tool->orient)) {
        tool->orient = 0;
    }
    (void)nc_tool_word_float(line, 'F', &tool->feed);
    (void)nc_tool_word_float(line, 'Q', &tool->finish_feed);
    (void)nc_tool_word_float(line, 'D', &tool->doc);
    (void)nc_tool_word_float(line, 'E', &tool->finish_doc);
    (void)nc_tool_word_float(line, 'S', &tool->rpm);
    (void)nc_tool_word_float(line, 'X', &tool->xoff);
    (void)nc_tool_word_float(line, 'Z', &tool->zoff);

    if (tool->finish_feed <= 0.0f) {
        tool->finish_feed = tool->feed;
    }
    if (tool->doc <= 0.0f) {
        tool->doc = tool->r;
    }
    tool->valid = true;
    return true;
}

bool nc_tool_number_for_line(const nc_document_t *doc, size_t before_or_at, int *tool_no)
{
    float v;
    size_t i;

    if (!doc || !tool_no || doc->line_count == 0) {
        return false;
    }
    if (before_or_at >= doc->line_count) {
        before_or_at = doc->line_count - 1;
    }

    for (i = before_or_at + 1; i > 0; i--) {
        if (nc_tool_word_float(doc->lines[i - 1].text, 'T', &v)) {
            *tool_no = (int)(v + 0.5f);
            return true;
        }
    }
    return false;
}

bool nc_tool_by_number(const nc_document_t *tool_doc, int tool_no, nc_tool_t *tool)
{
    size_t i;

    if (!tool_doc || !tool) {
        return false;
    }
    for (i = 0; i < tool_doc->line_count; i++) {
        nc_tool_t candidate;
        if (nc_tool_from_line(tool_doc->lines[i].text, &candidate) &&
            candidate.t == tool_no) {
            *tool = candidate;
            return true;
        }
    }
    return false;
}

bool nc_tool_active_for_line(const nc_document_t *doc, size_t before_or_at, nc_tool_t *tool)
{
    int tool_no = 1;

    (void)nc_tool_number_for_line(doc, before_or_at, &tool_no);
    if (nc_tool_by_number(doc, tool_no, tool)) {
        return true;
    }
    return nc_tool_from_line(nc_tool_default_line(tool_no), tool);
}

bool nc_tool_active_from_table(const nc_document_t *program_doc,
                               size_t before_or_at,
                               const nc_document_t *tool_doc,
                               nc_tool_t *tool)
{
    int tool_no = 1;

    (void)nc_tool_number_for_line(program_doc, before_or_at, &tool_no);
    if (nc_tool_by_number(tool_doc, tool_no, tool)) {
        return true;
    }
    return nc_tool_from_line(nc_tool_default_line(tool_no), tool);
}

bool nc_tool_active_from_file(const nc_document_t *program_doc,
                              size_t before_or_at,
                              const char *tool_path,
                              nc_tool_t *tool)
{
    fs_file_t *fp;
    char line[NC_MAX_LINE_LEN];
    size_t used = 0;
    int tool_no = 1;

    if (!tool) {
        return false;
    }
    (void)nc_tool_number_for_line(program_doc, before_or_at, &tool_no);
    if (!tool_path || !tool_path[0]) {
        return nc_tool_from_line(nc_tool_default_line(tool_no), tool);
    }

    fp = fs_open(tool_path, "r");
    if (!fp) {
        return nc_tool_from_line(nc_tool_default_line(tool_no), tool);
    }
    while (fs_available(fp)) {
        char c;
        if (fs_read(fp, (uint8_t *)&c, 1) != 1) {
            break;
        }
        if (c == '\n' || used + 1 >= sizeof(line)) {
            nc_tool_t candidate;
            line[used] = '\0';
            if (nc_tool_from_line(line, &candidate) && candidate.t == tool_no) {
                *tool = candidate;
                fs_close(fp);
                return true;
            }
            used = 0;
        } else if (c != '\r') {
            line[used++] = c;
        }
    }
    if (used > 0) {
        nc_tool_t candidate;
        line[used] = '\0';
        if (nc_tool_from_line(line, &candidate) && candidate.t == tool_no) {
            *tool = candidate;
            fs_close(fp);
            return true;
        }
    }
    fs_close(fp);
    return nc_tool_from_line(nc_tool_default_line(tool_no), tool);
}

nc_result_t nc_insert_tool_default(nc_document_t *doc)
{
    int t = 1;
    size_t i;
    const char *line;

    if (!doc) {
        return NC_ERR_BAD_ARG;
    }

    for (i = 0; i < doc->line_count; i++) {
        nc_tool_t tool;
        if (nc_tool_from_line(doc->lines[i].text, &tool) && tool.t >= t && tool.t < NC_TOOL_MAX) {
            t = tool.t + 1;
        }
    }

    line = nc_tool_default_line(t);
    if (!line) {
        return NC_ERR_BAD_ARG;
    }
    return nc_insert_line(doc, doc->cursor_line + 1, line);
}
