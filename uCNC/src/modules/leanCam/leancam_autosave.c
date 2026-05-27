#include "leancam_autosave.h"

static bool g_lc_autosave_pending = false;
static uint32_t g_lc_autosave_due_ms = 0;
static uint8_t g_lc_autosave_busy_tries = 0;

void lc_autosave_init(void)
{
    g_lc_autosave_pending = false;
    g_lc_autosave_due_ms = 0;
    g_lc_autosave_busy_tries = 0;
}

void lc_autosave_schedule(uint32_t now, uint32_t delay_ms)
{
    g_lc_autosave_pending = true;
    g_lc_autosave_busy_tries = 0;
    g_lc_autosave_due_ms = now + delay_ms;
}

bool lc_autosave_due(uint32_t now)
{
    return g_lc_autosave_pending &&
           (int32_t)(now - g_lc_autosave_due_ms) >= 0;
}

void lc_autosave_clear(void)
{
    g_lc_autosave_pending = false;
    g_lc_autosave_due_ms = 0;
    g_lc_autosave_busy_tries = 0;
}

bool lc_autosave_defer_busy(uint32_t now, uint32_t retry_ms, uint8_t max_tries)
{
    g_lc_autosave_busy_tries++;
    if (g_lc_autosave_busy_tries >= max_tries)
    {
        lc_autosave_clear();
        return false;
    }

    g_lc_autosave_pending = true;
    g_lc_autosave_due_ms = now + retry_ms;
    return true;
}

uint8_t lc_autosave_busy_tries(void)
{
    return g_lc_autosave_busy_tries;
}
