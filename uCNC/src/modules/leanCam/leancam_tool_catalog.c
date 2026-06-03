/* LeanCam module contract:
 * Purpose: parse and cache TOOL rows for lookup by T number.
 * Called by: bridge, preset/default expansion, validation, and generator/tool checks.
 * Calls into: program/text helpers only.
 * Owns: current tool catalog cache, not the editable program itself.
 */
#include "leancam_tool_catalog.h"
#include "leancam_resource.h"
#include "leancam_code.h"
#include "../file_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LC_TOOL_CATALOG_MAX_TOOLS
#define LC_TOOL_CATALOG_MAX_TOOLS 8
#endif

#ifndef LC_TOOL_CATALOG_USE_PSRAM
#define LC_TOOL_CATALOG_USE_PSRAM 1
#endif

typedef struct
{
    char (*lines)[MAX_LEN];
    int count;
    int capacity;
} lc_tool_catalog_t;

static lc_tool_catalog_t g_tools_catalog;
static char g_tools_catalog_fallback[MAX_LINES][MAX_LEN];
static bool g_tools_catalog_in_psram = false;
static bool g_catalogs_loaded = false;

static bool lc_tool_line_is_tool(const char *line)
{
    return lc_code_command_is(line, "TOOL");
}

static bool lc_tool_line_is_plain_storage(const char *line)
{
    return line && !strchr(line, '|') && !strchr(line, '{') && !strchr(line, '}');
}

static void lc_tool_catalog_bind_storage(void)
{
    if (g_tools_catalog.lines && g_tools_catalog_in_psram)
        return;
    if (g_tools_catalog.lines && g_tools_catalog.lines != g_tools_catalog_fallback)
        return;

#if LC_TOOL_CATALOG_USE_PSRAM
    {
        void *psram = lc_resource_psram_region(LC_PSRAM_REGION_TOOL_CATALOG,
                                               MAX_LINES * MAX_LEN);
        if (psram)
        {
            g_tools_catalog.lines = (char (*)[MAX_LEN])psram;
            g_tools_catalog.capacity = MAX_LINES;
            g_tools_catalog_in_psram = true;
            return;
        }
    }
#endif

    g_tools_catalog.lines = g_tools_catalog_fallback;
    g_tools_catalog.capacity = MAX_LINES;
    g_tools_catalog_in_psram = false;
}

static bool lc_tool_line_get_float(const char *line, const char *key, float *out)
{
    return lc_code_get_field_float(line, key, out);
}

void lc_tool_catalog_init(void)
{
    g_tools_catalog.lines = NULL;
    g_tools_catalog.count = 0;
    g_tools_catalog.capacity = 0;
    g_tools_catalog_in_psram = false;
    g_catalogs_loaded = false;
    lc_tool_catalog_bind_storage();
}

void lc_tool_catalog_clear(void)
{
    lc_tool_catalog_bind_storage();
    g_tools_catalog.count = 0;
}

int lc_tool_line_t_value(const char *line)
{
    char *endp;
    char val[16];
    long t;

    if (!lc_tool_line_is_tool(line))
        return -1;
    if (!lc_code_get_field_text(line, "T", val, sizeof(val)))
        return -1;
    t = strtol(val, &endp, 10);
    if (!endp || *endp != 0)
        return -1;

    if (t < 0 || t > LC_TOOL_CATALOG_MAX_TOOLS)
        return -1;

    return (int)t;
}

bool lc_tool_catalog_has_t(int t)
{
    int i;

    if (t < 0)
        return false;

    for (i = 0; i < g_tools_catalog.count; ++i)
    {
        if (lc_tool_line_t_value(g_tools_catalog.lines[i]) == t)
            return true;
    }

    return false;
}

static bool lc_tool_catalog_add(const char *line)
{
    int t;

    lc_tool_catalog_bind_storage();
    t = lc_tool_line_t_value(line);
    if (t < 0 || lc_tool_catalog_has_t(t) || g_tools_catalog.count >= g_tools_catalog.capacity)
        return false;

    strncpy(g_tools_catalog.lines[g_tools_catalog.count], line, MAX_LEN - 1);
    g_tools_catalog.lines[g_tools_catalog.count][MAX_LEN - 1] = 0;
    g_tools_catalog.count++;
    return true;
}

static const char *lc_tool_catalog_default_line(int t)
{
    static char line[MAX_LEN];

    if (t < 0 || t > LC_TOOL_CATALOG_MAX_TOOLS)
        return NULL;

    if (t == 0)
        snprintf(line,
                 sizeof(line),
                 "TOOL T0 R0 ORIENT0 R_FEED0 FIN_FEED0 DOC0 FIN_DOC0 RPM800 XOFF0 ZOFF0");
    else
        snprintf(line,
                 sizeof(line),
                 "TOOL T%d R0.8 ORIENT3 R_FEED120 FIN_FEED60 DOC2.0 FIN_DOC0.5 RPM800 XOFF0 ZOFF0",
                 t);

    return line;
}

bool lc_tool_catalog_add_raw(const char *line)
{
    char t[16] = "0";
    char r[16] = "0";
    char orient[16] = "0";
    char r_feed[16] = "0";
    char fin_feed[16] = "0";
    char doc[16] = "0";
    char fin_doc[16] = "0";
    char rpm[16] = "800";
    char xoff[16] = "0";
    char zoff[16] = "0";
    char normalized[MAX_LEN];

    lc_tool_catalog_bind_storage();
    if (!line || g_tools_catalog.count >= g_tools_catalog.capacity)
        return false;
    if (!lc_tool_line_is_tool(line))
        return false;
    if (!lc_tool_line_is_plain_storage(line))
        return false;
    if (lc_tool_catalog_has_t(lc_tool_line_t_value(line)))
        return false;

    (void)lc_code_get_field_text(line, "T", t, sizeof(t));
    (void)lc_code_get_field_text(line, "R", r, sizeof(r));
    (void)lc_code_get_field_text(line, "ORIENT", orient, sizeof(orient));
    (void)lc_code_get_field_text(line, "R_FEED", r_feed, sizeof(r_feed));
    (void)lc_code_get_field_text(line, "FIN_FEED", fin_feed, sizeof(fin_feed));
    (void)lc_code_get_field_text(line, "DOC", doc, sizeof(doc));
    (void)lc_code_get_field_text(line, "FIN_DOC", fin_doc, sizeof(fin_doc));
    (void)lc_code_get_field_text(line, "RPM", rpm, sizeof(rpm));
    (void)lc_code_get_field_text(line, "XOFF", xoff, sizeof(xoff));
    (void)lc_code_get_field_text(line, "ZOFF", zoff, sizeof(zoff));

    snprintf(normalized,
             sizeof(normalized),
             "TOOL T%.7s R%.7s ORIENT%.7s R_FEED%.7s FIN_FEED%.7s DOC%.7s FIN_DOC%.7s RPM%.7s XOFF%.7s ZOFF%.7s",
             t, r, orient, r_feed, fin_feed, doc, fin_doc, rpm, xoff, zoff);

    strncpy(g_tools_catalog.lines[g_tools_catalog.count], normalized, MAX_LEN - 1);
    g_tools_catalog.lines[g_tools_catalog.count][MAX_LEN - 1] = 0;
    g_tools_catalog.count++;
    return true;
}

bool lc_tool_catalog_add_default(int t)
{
    const char *line;

    line = lc_tool_catalog_default_line(t);
    if (!line)
        return false;
    return lc_tool_catalog_add(line);
}

void lc_tool_catalog_sort_by_t(void)
{
    int i;

    for (i = 0; i < g_tools_catalog.count; ++i)
    {
        int best = i;
        int j;

        for (j = i + 1; j < g_tools_catalog.count; ++j)
        {
            int tj = lc_tool_line_t_value(g_tools_catalog.lines[j]);
            int tb = lc_tool_line_t_value(g_tools_catalog.lines[best]);
            if (tj < 0)
                tj = 9999;
            if (tb < 0)
                tb = 9999;
            if (tj < tb)
                best = j;
        }

        if (best != i)
        {
            char tmp[MAX_LEN];
            memcpy(tmp, g_tools_catalog.lines[i], sizeof(tmp));
            memcpy(g_tools_catalog.lines[i], g_tools_catalog.lines[best], sizeof(tmp));
            memcpy(g_tools_catalog.lines[best], tmp, sizeof(tmp));
        }
    }
}

void lc_tool_catalog_seed_defaults(void)
{
    int t;

    for (t = 0; t <= LC_TOOL_CATALOG_MAX_TOOLS; ++t)
    {
        if (!lc_tool_catalog_has_t(t))
            (void)lc_tool_catalog_add_default(t);
    }
    lc_tool_catalog_sort_by_t();
}

void lc_tool_catalog_copy_from_program(const program_t *prog)
{
    int i;

    lc_tool_catalog_clear();
    if (!prog)
        return;

    for (i = 0; i < prog->count; ++i)
    {
        if (!lc_tool_line_is_tool(prog->lines[i]))
            continue;
        if (!lc_tool_catalog_add_raw(prog->lines[i]))
            continue;
    }
    lc_tool_catalog_seed_defaults();
}

void lc_tool_catalog_copy_to_program(program_t *prog)
{
    int i;

    if (!prog)
        return;

    prog_init(prog);
    for (i = 0; i < g_tools_catalog.count; ++i)
    {
        if (!prog_add(prog, g_tools_catalog.lines[i]))
            break;
    }
}

bool lc_tool_catalog_load_from_file(const char *path)
{
    fs_file_t *fp;
    char line[MAX_LEN];
    int pos = 0;
    bool ok = true;

    if (!path)
        return false;

    lc_tool_catalog_clear();
    if (!lc_resource_file_begin("tool-load"))
        return false;
    fp = fs_open(path, "r");
    if (!fp)
    {
        lc_resource_file_end();
        return false;
    }

    while (fs_available(fp))
    {
        char c = 0;

        if (!fs_read(fp, (uint8_t *)&c, 1))
        {
            ok = false;
            break;
        }

        if (c == '\r')
            continue;

        if (c == '\n')
        {
            line[pos] = 0;
            if (lc_tool_line_is_tool(line))
                (void)lc_tool_catalog_add_raw(line);
            pos = 0;
            continue;
        }

        if (pos < MAX_LEN - 1)
            line[pos++] = c;
    }

    if (ok && pos > 0)
    {
        line[pos] = 0;
        if (lc_tool_line_is_tool(line))
            (void)lc_tool_catalog_add_raw(line);
    }

    fs_close(fp);
    lc_resource_file_end();
    return ok;
}

void lc_tool_catalog_use_loaded(void)
{
    if (g_catalogs_loaded)
        return;

    lc_tool_catalog_seed_defaults();
    g_catalogs_loaded = true;
}

void lc_tool_catalog_load_from_storage(const char *path)
{
    if (g_catalogs_loaded)
        return;

    (void)lc_tool_catalog_load_from_file(path);
    lc_tool_catalog_seed_defaults();
    g_catalogs_loaded = true;
}

const char *lc_tool_catalog_find_in_program_or_catalog(const program_t *prog, int before_or_at, int t)
{
    int i;
    const char *catalog_tool = NULL;
    int catalog_count = 0;

    if (t <= 0)
        return NULL;

    if (prog)
    {
        if (before_or_at >= prog->count)
            before_or_at = prog->count - 1;

        for (i = before_or_at; i >= 0; --i)
        {
            float tv = 0.0f;
            const char *line = prog->lines[i];
            if (!lc_tool_line_is_tool(line))
                continue;
            if (lc_tool_line_get_float(line, "T", &tv) && (int)tv == t)
                return line;
        }

        for (i = 0; i < prog->count; ++i)
        {
            float tv = 0.0f;
            const char *line = prog->lines[i];
            if (!lc_tool_line_is_tool(line))
                continue;
            if (lc_tool_line_get_float(line, "T", &tv) && (int)tv == t)
                return line;
        }
    }

    lc_tool_catalog_use_loaded();
    for (i = 0; i < g_tools_catalog.count; ++i)
    {
        float tv = 0.0f;
        const char *line = g_tools_catalog.lines[i];
        if (!lc_tool_line_is_tool(line))
            continue;
        if (lc_tool_line_get_float(line, "T", &tv) && (int)tv == t)
        {
            if (!catalog_tool)
                catalog_tool = line;
            catalog_count++;
        }
    }

    if (catalog_count == 1)
        return catalog_tool;

    return lc_tool_catalog_default_line(t);
}


