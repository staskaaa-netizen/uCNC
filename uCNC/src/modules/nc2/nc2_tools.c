#include "nc2_tools.h"

#include "nc2_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One field of a row, as a number: the same field walk the editor uses. */
static bool nc2_tool_word(const char *line, char letter, float *out)
{
    nc2_field_t fields[NC2_MAX_FIELDS];
    int count = nc2_fields(line, fields, NC2_MAX_FIELDS);
    int i;

    for (i = 0; i < count; i++) {
        if (fields[i].letter != letter || fields[i].value == fields[i].end) {
            continue;
        }
        if (out) {
            *out = (float)strtod(line + fields[i].value, 0);
        }
        return true;
    }
    return false;
}

bool nc2_tool_word_text(const char *line, char letter, char *out, size_t out_sz)
{
    nc2_field_t fields[NC2_MAX_FIELDS];
    int count = nc2_fields(line, fields, NC2_MAX_FIELDS);
    int i;
    size_t len;

    if (!out || out_sz == 0u) {
        return false;
    }
    out[0] = '\0';
    for (i = 0; i < count; i++) {
        if (fields[i].letter != letter || fields[i].value == fields[i].end) {
            continue;
        }
        len = fields[i].end - fields[i].value;
        if (len >= out_sz) {
            len = out_sz - 1u;
        }
        memcpy(out, line + fields[i].value, len);
        out[len] = '\0';
        return true;
    }
    return false;
}

/* A row is a tool when it carries a number for `T` and is not a motion: the
   table's own rows are the only ones the panel writes (`nc`'s rule, kept). */
static bool nc2_tool_line_is_tool(const char *line)
{
    nc2_field_t fields[NC2_MAX_FIELDS];
    int count;
    int i;

    if (!line || !line[0]) {
        return false;
    }
    count = nc2_fields(line, fields, NC2_MAX_FIELDS);
    for (i = 0; i < count; i++) {
        if (fields[i].letter == 'T' || fields[i].letter == 't') {
            return fields[i].value != fields[i].end;
        }
        if (fields[i].letter == 'G' || fields[i].letter == 'M') {
            return false;
        }
    }
    return false;
}

bool nc2_tool_orient_valid(int orient)
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

const char *nc2_tool_default_line(int t)
{
    static char line[NC2_MAX_LINE_LEN];

    if (t < 0 || t > NC2_TOOL_MAX) {
        return NULL;
    }
    if (t == 0) {
        snprintf(line, sizeof(line), "T0 R0 O0 F0 Q0 D0 E0 S800 X0 Z0");
    } else {
        snprintf(line, sizeof(line), "T%d R0.8 O3 F120 Q60 D2.0 E0.5 S800 X0 Z0",
                 t);
    }
    return line;
}

bool nc2_tool_from_line(const char *line, nc2_tool_t *tool)
{
    float v;

    if (!tool) {
        return false;
    }
    memset(tool, 0, sizeof(*tool));
    tool->rpm = 800.0f;
    if (!nc2_tool_line_is_tool(line) || !nc2_tool_word(line, 'T', &v)) {
        return false;
    }
    tool->t = (int)(v + 0.5f);
    (void)nc2_tool_word(line, 'R', &tool->r);
    if (nc2_tool_word(line, 'O', &v)) {
        tool->orient = (int)(v + 0.5f);
    }
    if (!nc2_tool_orient_valid(tool->orient)) {
        tool->orient = 0;
    }
    (void)nc2_tool_word(line, 'F', &tool->feed);
    (void)nc2_tool_word(line, 'Q', &tool->finish_feed);
    (void)nc2_tool_word(line, 'D', &tool->doc);
    (void)nc2_tool_word(line, 'E', &tool->finish_doc);
    (void)nc2_tool_word(line, 'S', &tool->rpm);
    (void)nc2_tool_word(line, 'X', &tool->xoff);
    (void)nc2_tool_word(line, 'Z', &tool->zoff);

    if (tool->finish_feed <= 0.0f) {
        tool->finish_feed = tool->feed;      /* Q absent: the same feed */
    }
    if (tool->doc <= 0.0f) {
        tool->doc = tool->r;                 /* D absent: a nose radius deep */
    }
    tool->valid = true;
    return true;
}

bool nc2_tools_from_row(const char *row, nc2_tool_t *tool)
{
    return nc2_tool_from_line(row, tool);
}

/* --- the table, read once ------------------------------------------------ */

static nc2_tool_t g_nc2_tools[NC2_TOOL_MAX + 1];
static bool g_nc2_tools_loaded;

void nc2_tools_clear(void)
{
    memset(g_nc2_tools, 0, sizeof(g_nc2_tools));
    g_nc2_tools_loaded = false;
}

bool nc2_tools_load(const char *path)
{
    nc2_document_t doc;
    size_t i;
    bool any = false;

    nc2_tools_clear();
    if (!path || !path[0]) {
        return false;
    }
    nc2_document_init(&doc);
    if (!nc2_file_load(&doc, path)) {
        return false;
    }
    for (i = 0u; i < doc.line_count; i++) {
        nc2_tool_t tool;

        if (!nc2_tool_from_line(doc.lines[i], &tool) ||
            tool.t < 0 || tool.t > NC2_TOOL_MAX) {
            continue;
        }
        g_nc2_tools[tool.t] = tool;
        any = true;
    }
    g_nc2_tools_loaded = any;
    return any;
}

bool nc2_tools_active(const nc2_document_t *program, size_t line,
                      nc2_tool_t *tool)
{
    float v;
    int t = 1;
    size_t i;

    if (!tool) {
        return false;
    }
    /* The last `T` word at or above the line in play: what the program has
       chosen by the time it reaches this cut. */
    if (program && program->line_count) {
        if (line >= program->line_count) {
            line = program->line_count - 1u;
        }
        for (i = line + 1u; i > 0u; i--) {
            if (nc2_tool_word(program->lines[i - 1u], 'T', &v)) {
                t = (int)(v + 0.5f);
                break;
            }
        }
    }
    if (t < 0 || t > NC2_TOOL_MAX) {
        t = 1;
    }
    if (g_nc2_tools_loaded && g_nc2_tools[t].valid) {
        *tool = g_nc2_tools[t];
        return true;
    }
    return nc2_tool_from_line(nc2_tool_default_line(t), tool);
}
