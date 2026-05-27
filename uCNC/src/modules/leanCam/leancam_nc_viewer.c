#include "leancam_nc_viewer.h"
#include "leancam_resource.h"
#include "../file_system.h"

#include <string.h>

static int g_nc_top_line = 0;
static int g_nc_selected_row = 0;
static char g_nc_path[LC_FILE_PATH_MAX];
static char g_nc_setup_line[UI_LC_LINE_LEN];
static char g_nc_lines[UI_LC_MAX_LINES][UI_LC_LINE_LEN];
static uint8_t g_nc_line_count = 0;
static bool g_nc_eof = false;

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

void lc_nc_viewer_init(void)
{
    g_nc_path[0] = 0;
    g_nc_setup_line[0] = 0;
    g_nc_top_line = 0;
    g_nc_selected_row = 0;
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

    return true;
}

void lc_nc_viewer_close(void)
{
    lc_nc_viewer_init();
}

void lc_nc_viewer_scroll_prev(void)
{
    if (g_nc_selected_row > 0)
    {
        g_nc_selected_row--;
    }
    else if (g_nc_top_line > 0)
    {
        g_nc_top_line--;
        (void)lc_nc_load_window();
    }
}

void lc_nc_viewer_scroll_next(void)
{
    if (g_nc_selected_row + 1 < g_nc_line_count)
    {
        g_nc_selected_row++;
    }
    else if (!g_nc_eof)
    {
        g_nc_top_line++;
        (void)lc_nc_load_window();
    }
}

const char *lc_nc_viewer_path(void)
{
    return g_nc_path;
}

const char *lc_nc_viewer_setup_line(void)
{
    return g_nc_setup_line;
}

int lc_nc_viewer_top_line(void)
{
    return g_nc_top_line;
}

uint8_t lc_nc_viewer_line_count(void)
{
    return g_nc_line_count;
}

uint8_t lc_nc_viewer_selected_row(void)
{
    return (uint8_t)g_nc_selected_row;
}

const char *lc_nc_viewer_line(uint8_t row)
{
    if (row >= g_nc_line_count)
        return "";
    return g_nc_lines[row];
}
