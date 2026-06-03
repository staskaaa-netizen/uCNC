/* LeanCam module contract:
 * Purpose: lightweight NC text viewer state for already-loaded program text.
 * Called by: leancam_bridge while in NC/view mode.
 * Calls into: plain program/text helpers only.
 * Owns: viewer cursor/scroll state; it should not mutate files or generate toolpaths.
 */
#include "leancam_nc_viewer.h"
#include "leancam_resource.h"
#include "../file_system.h"

#include <ctype.h>
#include <string.h>

#ifndef LEANCAM_NC_VIEW_CACHE_PROGRAM
#define LEANCAM_NC_VIEW_CACHE_PROGRAM 0
#endif

static int g_nc_top_line = 0;
static int g_nc_selected_row = 0;
static char g_nc_path[LC_FILE_PATH_MAX];
static char g_nc_setup_line[UI_LC_LINE_LEN];
static char g_nc_lines[UI_LC_MAX_LINES][UI_LC_LINE_LEN];
static uint8_t g_nc_line_count = 0;
static bool g_nc_eof = false;
static program_t g_nc_cached_program;
static bool g_nc_cached_program_valid = false;

static bool lc_nc_line_is_cycle_begin(const char *line)
{
    char c0;
    char c1;
    char c2;

    while (line && (*line == ' ' || *line == '\t'))
        line++;
    if (!line)
        return false;
    c0 = (char)toupper((unsigned char)line[0]);
    c1 = line[1];
    c2 = line[2];
    return line &&
           c0 == 'G' && c1 == '7' && (c2 == '1' || c2 == '2') &&
           (line[3] == 0 || line[3] == ' ' || line[3] == '\t');
}

static bool lc_nc_line_is_cycle_end(const char *line)
{
    char c0;

    while (line && (*line == ' ' || *line == '\t'))
        line++;
    if (!line)
        return false;
    c0 = (char)toupper((unsigned char)line[0]);
    return line &&
           c0 == 'G' && line[1] == '8' && line[2] == '0' &&
           (line[3] == 0 || line[3] == ' ' || line[3] == '\t');
}

static void lc_nc_clear_window(void)
{
    int i;

    for (i = 0; i < UI_LC_MAX_LINES; ++i)
        g_nc_lines[i][0] = 0;

    g_nc_line_count = 0;
    g_nc_eof = false;
}

static bool lc_nc_read_line(fs_file_t *fp, char *out, int out_sz)
{
    int pos = 0;
    bool got = false;

    if (!fp || !out || out_sz <= 0)
        return false;

    while (fs_available(fp))
    {
        char c = 0;

        if (!fs_read(fp, (uint8_t *)&c, 1))
            break;

        got = true;
        if (c == '\r')
            continue;

        if (c == '\n')
            break;

        if (pos < out_sz - 1)
            out[pos++] = c;
    }

    out[pos] = 0;
    return got;
}

static void lc_nc_capture_setup_comment(const char *line)
{
    const char *p;
    const char *end;
    size_t n;

    if (!line || g_nc_setup_line[0])
        return;

    p = strstr(line, "SETUP ");
    if (!p)
        return;

    end = strchr(p, ')');
    if (!end)
        end = p + strlen(p);

    n = (size_t)(end - p);
    if (n >= sizeof(g_nc_setup_line))
        n = sizeof(g_nc_setup_line) - 1u;
    memcpy(g_nc_setup_line, p, n);
    g_nc_setup_line[n] = 0;
}

static bool lc_nc_load_window(void)
{
    fs_file_t *fp;
    char throwaway[UI_LC_LINE_LEN];
    int skipped = 0;
    int row = 0;

    lc_nc_clear_window();
    g_nc_setup_line[0] = 0;

    if (!g_nc_path[0])
        return false;

    if (!lc_resource_file_begin("nc-window"))
        return false;
    fp = fs_open(g_nc_path, "r");
    if (!fp)
    {
        lc_resource_file_end();
        return false;
    }

    while (skipped < g_nc_top_line && lc_nc_read_line(fp, throwaway, sizeof(throwaway)))
    {
        lc_nc_capture_setup_comment(throwaway);
        skipped++;
    }

    while (row < UI_LC_MAX_LINES && lc_nc_read_line(fp, g_nc_lines[row], UI_LC_LINE_LEN))
    {
        lc_nc_capture_setup_comment(g_nc_lines[row]);
        row++;
    }

    g_nc_line_count = (uint8_t)row;
    g_nc_eof = !fs_available(fp);
    if (g_nc_line_count == 0)
        g_nc_selected_row = 0;
    else if (g_nc_selected_row >= g_nc_line_count)
        g_nc_selected_row = g_nc_line_count - 1;

    fs_close(fp);
    lc_resource_file_end();

    return true;
}

static bool lc_nc_find_region_containing(int selected_line, int *start_out, int *end_out)
{
    fs_file_t *fp;
    char line[UI_LC_LINE_LEN];
    int line_no = 0;
    int active_start = -1;
    bool found = false;

    if (start_out) *start_out = -1;
    if (end_out) *end_out = -1;
    if (!g_nc_path[0] || selected_line < 0)
        return false;
    if (!lc_resource_file_begin("nc-region"))
        return false;
    fp = fs_open(g_nc_path, "r");
    if (!fp)
    {
        lc_resource_file_end();
        return false;
    }

    while (lc_nc_read_line(fp, line, sizeof(line)))
    {
        if (lc_nc_line_is_cycle_begin(line))
            active_start = line_no;

        if (active_start >= 0 && selected_line >= active_start && selected_line == line_no)
        {
            int end = line_no;
            while (!lc_nc_line_is_cycle_end(line) && lc_nc_read_line(fp, line, sizeof(line)))
            {
                line_no++;
                end = line_no;
            }
            if (lc_nc_line_is_cycle_end(line))
                end = line_no;
            if (start_out) *start_out = active_start;
            if (end_out) *end_out = end;
            found = true;
            break;
        }

        if (active_start >= 0 && lc_nc_line_is_cycle_end(line))
            active_start = -1;
        line_no++;
    }

    fs_close(fp);
    lc_resource_file_end();
    return found;
}

static void lc_nc_set_selected_line(int line_no)
{
    if (line_no < 0)
        line_no = 0;

    if (line_no < g_nc_top_line)
    {
        g_nc_top_line = line_no;
        g_nc_selected_row = 0;
        (void)lc_nc_load_window();
        return;
    }

    while (line_no >= g_nc_top_line + g_nc_line_count && !g_nc_eof)
    {
        g_nc_top_line++;
        (void)lc_nc_load_window();
    }

    if (line_no >= g_nc_top_line && line_no < g_nc_top_line + g_nc_line_count)
        g_nc_selected_row = line_no - g_nc_top_line;
    else if (g_nc_line_count > 0)
        g_nc_selected_row = g_nc_line_count - 1;
    else
        g_nc_selected_row = 0;
}

void lc_nc_viewer_normalize_selection(void)
{
    int current = g_nc_top_line + g_nc_selected_row;
    int start = -1;
    int end = -1;

    if (lc_nc_find_region_containing(current, &start, &end) && start >= 0 && current != start)
        lc_nc_set_selected_line(start);
}

void lc_nc_viewer_init(void)
{
    g_nc_path[0] = 0;
    g_nc_setup_line[0] = 0;
    g_nc_top_line = 0;
    g_nc_selected_row = 0;
    prog_init(&g_nc_cached_program);
    g_nc_cached_program_valid = false;
    lc_nc_clear_window();
}

bool lc_nc_viewer_open(const char *path)
{
    if (!path || !path[0])
        return false;

    strncpy(g_nc_path, path, sizeof(g_nc_path) - 1);
    g_nc_path[sizeof(g_nc_path) - 1] = 0;
    g_nc_top_line = 0;
    g_nc_selected_row = 0;

    if (!lc_nc_load_window())
    {
        lc_nc_viewer_init();
        return false;
    }
#if LEANCAM_NC_VIEW_CACHE_PROGRAM
    prog_init(&g_nc_cached_program);
    g_nc_cached_program_valid = leancam_files_load(path, &g_nc_cached_program);
#else
    prog_init(&g_nc_cached_program);
    g_nc_cached_program_valid = false;
#endif
    lc_nc_viewer_normalize_selection();

    return true;
}

void lc_nc_viewer_close(void)
{
    lc_nc_viewer_init();
}

void lc_nc_viewer_scroll_prev(void)
{
    int current = lc_nc_viewer_selected_line();
    int start = -1;
    int end = -1;

    if (current > 0 && lc_nc_find_region_containing(current - 1, &start, &end))
    {
        lc_nc_set_selected_line(start);
        lc_nc_viewer_normalize_selection();
        return;
    }

    if (g_nc_selected_row > 0)
    {
        g_nc_selected_row--;
    }
    else if (g_nc_top_line > 0)
    {
        g_nc_top_line--;
        (void)lc_nc_load_window();
    }
    lc_nc_viewer_normalize_selection();
}

void lc_nc_viewer_scroll_next(void)
{
    int current = lc_nc_viewer_selected_line();
    int start = -1;
    int end = -1;

    if (lc_nc_find_region_containing(current, &start, &end))
    {
        lc_nc_set_selected_line(end + 1);
        lc_nc_viewer_normalize_selection();
        return;
    }

    if (g_nc_selected_row + 1 < g_nc_line_count)
    {
        g_nc_selected_row++;
    }
    else if (!g_nc_eof)
    {
        g_nc_top_line++;
        (void)lc_nc_load_window();
    }
    lc_nc_viewer_normalize_selection();
}

const char *lc_nc_viewer_path(void)
{
    return g_nc_path;
}

const program_t *lc_nc_viewer_cached_program(void)
{
    return g_nc_cached_program_valid ? &g_nc_cached_program : NULL;
}

const char *lc_nc_viewer_setup_line(void)
{
    return g_nc_setup_line;
}

int lc_nc_viewer_top_line(void)
{
    return g_nc_top_line;
}

int lc_nc_viewer_selected_line(void)
{
    return g_nc_top_line + g_nc_selected_row;
}

bool lc_nc_viewer_selected_region(int *start_out, int *end_out)
{
    return lc_nc_find_region_containing(lc_nc_viewer_selected_line(), start_out, end_out);
}

uint8_t lc_nc_viewer_line_count(void)
{
    return g_nc_line_count;
}

uint8_t lc_nc_viewer_selected_row(void)
{
    return (uint8_t)g_nc_selected_row;
}

const char *lc_nc_viewer_selected_text(void)
{
    return lc_nc_viewer_line((uint8_t)g_nc_selected_row);
}

const char *lc_nc_viewer_line(uint8_t row)
{
    if (row >= g_nc_line_count)
        return "";
    return g_nc_lines[row];
}


