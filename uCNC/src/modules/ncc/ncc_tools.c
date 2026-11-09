#include "ncc_tools.h"

#include "ncc_files.h"
#include "../file_system.h"
#include "../g7x/g7x_blocks.h"
#include "../g7x/g7x_contour.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One field of a row, as a number: the same field walk the editor uses. */
static bool ncc_tool_word(const char *line, char letter, float *out)
{
    ncc_field_t fields[NCC_MAX_FIELDS];
    int count = ncc_fields(line, fields, NCC_MAX_FIELDS);
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

bool ncc_tool_word_text(const char *line, char letter, char *out, size_t out_sz)
{
    ncc_field_t fields[NCC_MAX_FIELDS];
    int count = ncc_fields(line, fields, NCC_MAX_FIELDS);
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
static bool ncc_tool_line_is_tool(const char *line)
{
    ncc_field_t fields[NCC_MAX_FIELDS];
    int count;
    int i;

    if (!line || !line[0]) {
        return false;
    }
    count = ncc_fields(line, fields, NCC_MAX_FIELDS);
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

const char *ncc_tool_default_line(int t)
{
    static char line[NCC_MAX_LINE_LEN];

    if (t < 0 || t > NCC_TOOL_MAX) {
        return NULL;
    }
    if (t == 0) {
        snprintf(line, sizeof(line), "T0 R0 F0 Q0 D0 E0 S800 X0 Z0");
    } else {
        snprintf(line, sizeof(line), "T%d R0.8 F120 Q60 D2.0 E0.5 S800 X0 Z0",
                 t);
    }
    return line;
}

bool ncc_tool_from_line(const char *line, ncc_tool_t *tool)
{
    float v;

    if (!tool) {
        return false;
    }
    memset(tool, 0, sizeof(*tool));
    tool->rpm = 800.0f;
    if (!ncc_tool_line_is_tool(line) || !ncc_tool_word(line, 'T', &v)) {
        return false;
    }
    tool->t = (int)(v + 0.5f);
    (void)ncc_tool_word(line, 'R', &tool->r);
    (void)ncc_tool_word(line, 'F', &tool->feed);
    (void)ncc_tool_word(line, 'Q', &tool->finish_feed);
    (void)ncc_tool_word(line, 'D', &tool->doc);
    (void)ncc_tool_word(line, 'E', &tool->finish_doc);
    (void)ncc_tool_word(line, 'S', &tool->rpm);
    (void)ncc_tool_word(line, 'X', &tool->xoff);
    (void)ncc_tool_word(line, 'Z', &tool->zoff);
    if (tool->finish_feed <= 0.0f) {
        tool->finish_feed = tool->feed;
    }
    if (tool->doc <= 0.0f) {
        tool->doc = tool->r;
    }
    tool->valid = true;
    return true;
}

/* --- the table, read once ------------------------------------------------ */

static ncc_tool_t g_ncc_tools[NCC_TOOL_MAX + 1];
static bool g_ncc_tools_loaded;

/* Walk a card's file one line at a time and hand every line to `visit`. A whole
   `ncc_document_t` is 256 lines of 96 bytes - 24 KB - and a local one of those
   is a stack frame no machine has: the table and the program are read as a
   stream instead. */
typedef void (*ncc_tools_line_fn)(const char *line, size_t index, void *ctx);

static bool ncc_tools_walk(const char *path, ncc_tools_line_fn visit, void *ctx)
{
    fs_file_t *fp;
    char line[NCC_MAX_LINE_LEN];
    size_t used = 0u;
    size_t index = 0u;
    uint32_t guard = 0u;

    if (!path || !path[0]) {
        return false;
    }
    fp = fs_open(path, "r");
    if (!fp) {
        return false;
    }
    while (fs_available(fp) > 0 && guard++ < (NCC_MAX_LINES * 8u)) {
        char c;

        if (fs_read(fp, (uint8_t *)&c, 1u) != 1u) {
            break;
        }
        if (c == '\n' || used + 1u >= sizeof(line)) {
            line[used] = '\0';
            if (visit) {
                visit(line, index, ctx);
            }
            used = 0u;
            index++;
        } else if (c != '\r') {
            line[used++] = c;
        }
    }
    if (used > 0u) {
        line[used] = '\0';
        if (visit) {
            visit(line, index, ctx);
        }
    }
    fs_close(fp);
    return true;
}

/* The table's own lines: a row per tool, the number its `T` word says. */
static void ncc_tools_take_line(const char *line, size_t index, void *ctx)
{
    ncc_tool_t tool;
    bool *any = (bool *)ctx;

    (void)index;
    if (!ncc_tool_from_line(line, &tool) || tool.t < 0 ||
        tool.t > NCC_TOOL_MAX) {
        return;
    }
    g_ncc_tools[tool.t] = tool;
    *any = true;
}

/* The tool a program is using by the time it reaches `upto_line`: the last `T`
   word at or above it. The machine's own answer, read from the card without
   holding the program in RAM. */
static void ncc_tools_find_tool(const char *line, size_t index, void *ctx)
{
    struct {
        size_t upto;
        int *tool;
    } *find = ctx;
    int t;

    if (index > find->upto) {
        return;
    }
    /* A program's `Tn` is G7x's word, read by G7x's own reader - the same one
       the state machine a screen wears asks - so the row a program read off the
       card answers with is the row the buffer answers with. */
    if (g7x_line_tool(line, &t)) {
        *find->tool = t;
    }
}

void ncc_tools_clear(void)
{
    memset(g_ncc_tools, 0, sizeof(g_ncc_tools));
    g_ncc_tools_loaded = false;
}

bool ncc_tools_load(const char *path)
{
    bool any = false;

    ncc_tools_clear();
    if (!ncc_tools_walk(path, ncc_tools_take_line, &any)) {
        return false;
    }
    g_ncc_tools_loaded = any;
    return any;
}

bool ncc_tools_program_tool(const char *program_path, size_t upto_line,
                            int *tool)
{
    struct {
        size_t upto;
        int *tool;
    } find;

    if (!tool || !program_path || !program_path[0]) {
        return false;
    }
    *tool = -1;
    find.upto = upto_line;
    find.tool = tool;
    (void)ncc_tools_walk(program_path, ncc_tools_find_tool, &find);
    return *tool >= 0;
}

bool ncc_tools_active(const ncc_document_t *program, size_t line,
                      ncc_tool_t *tool)
{
    g7x_doc_t view;
    g7x_state_t state;

    if (!tool) {
        return false;
    }
    if (!program) {
        return false;
    }
    /* The tool the **program** is on at that line, from G7x's state machine:
       the panel does not read the program's words itself, and a program that
       names none has no tool - it is not the table's first row. Defaulting to
       `T1` is how the drawing (and the offset line) rode whatever row stood at
       the top of the table while the program was cutting with, or asking for,
       another tool (bench, 2026-10-04: *"still takes first tool radius for
       g71"*). */
    view = ncc_document_g7x(program);
    state = g7x_state_at(&view, line);
    if (state.tool < 0) {
        return false;
    }
    return ncc_tools_number(state.tool, tool);
}

/* The table's row for a tool number, with no document in sight - see the
   header: the run's tool is the run's business, not the screen's. A number the
   table cannot hold is **not** tool 1 - answering with the first row is the
   guess that put the wrong nose radius on a cut. */
bool ncc_tools_number(int t, ncc_tool_t *tool)
{
    if (!tool) {
        return false;
    }
    if (t < 0 || t > NCC_TOOL_MAX) {
        return false;
    }
    /* The card's table is *the* table: a number it does not carry has no nose
       radius, and the built-in row is only what a machine with no table at all
       has (the panel's own shipped default, the way nc wrote one when the card
       had none). Answering with a row the operator never wrote is the other
       half of the same guess. */
    if (g_ncc_tools_loaded) {
        if (!g_ncc_tools[t].valid) {
            return false;
        }
        *tool = g_ncc_tools[t];
        return true;
    }
    return ncc_tool_from_line(ncc_tool_default_line(t), tool);
}
