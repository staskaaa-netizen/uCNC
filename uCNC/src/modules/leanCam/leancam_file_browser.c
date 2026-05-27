#include "leancam_file_browser.h"
#include "../../cnc.h"

static int g_lc_file_selected = 0;
static bool g_lc_files_ready = false;
static uint32_t g_lc_file_retry_due_ms = 0;

void lc_file_browser_init(void)
{
    g_lc_file_selected = 0;
    g_lc_files_ready = false;
    g_lc_file_retry_due_ms = 0;
}

bool lc_file_browser_refresh(const char *dir, uint32_t retry_ms)
{
    int cnt;

    if (!leancam_files_refresh(dir))
    {
        g_lc_files_ready = false;
        g_lc_file_retry_due_ms = mcu_millis() + retry_ms;
        g_lc_file_selected = 0;
        return false;
    }

    g_lc_files_ready = true;
    g_lc_file_retry_due_ms = 0;
    cnt = leancam_files_count();

    if (g_lc_file_selected < 0)
        g_lc_file_selected = 0;
    if (g_lc_file_selected > cnt)
        g_lc_file_selected = cnt;

    return true;
}

bool lc_file_browser_ready(void)
{
    return g_lc_files_ready;
}

bool lc_file_browser_should_retry(uint32_t now)
{
    if (g_lc_files_ready)
        return false;
    if (g_lc_file_retry_due_ms != 0 &&
        (int32_t)(now - g_lc_file_retry_due_ms) < 0)
        return false;
    return true;
}

void lc_file_browser_mark_waiting(uint32_t due_ms)
{
    g_lc_files_ready = false;
    g_lc_file_retry_due_ms = due_ms;
}

int lc_file_browser_selected(void)
{
    return g_lc_file_selected;
}

void lc_file_browser_set_selected(int selected)
{
    g_lc_file_selected = selected;
    lc_file_browser_clamp_selected();
}

void lc_file_browser_clamp_selected(void)
{
    int cnt = leancam_files_count();

    if (g_lc_file_selected < 0)
        g_lc_file_selected = 0;
    if (g_lc_file_selected > cnt)
        g_lc_file_selected = cnt;
}

bool lc_file_browser_selected_valid(void)
{
    int cnt = leancam_files_count();
    return g_lc_file_selected >= 0 && g_lc_file_selected < cnt;
}

const char *lc_file_browser_selected_name(void)
{
    if (!lc_file_browser_selected_valid())
        return "";
    return leancam_files_name(g_lc_file_selected);
}

bool lc_file_browser_selected_path(const char *dir, char *out, int out_sz)
{
    if (!lc_file_browser_selected_valid())
        return false;
    return leancam_files_build_path(dir, g_lc_file_selected, out, out_sz);
}
