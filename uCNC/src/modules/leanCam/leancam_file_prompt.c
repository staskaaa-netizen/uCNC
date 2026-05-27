#include "leancam_file_prompt.h"
#include "leancam_files.h"

#include <string.h>

static char g_lc_prompt_name[32];
static bool g_lc_prompt_duplicate_pending = false;
static char g_lc_prompt_duplicate_source[LC_FILE_PATH_MAX];

void lc_file_prompt_clear(void)
{
    g_lc_prompt_name[0] = 0;
    g_lc_prompt_duplicate_pending = false;
    g_lc_prompt_duplicate_source[0] = 0;
}

void lc_file_prompt_begin_new(void)
{
    lc_file_prompt_clear();
}

void lc_file_prompt_begin_duplicate(const char *source_path)
{
    g_lc_prompt_name[0] = 0;
    g_lc_prompt_duplicate_pending = true;
    if (!source_path)
        source_path = "";
    strncpy(g_lc_prompt_duplicate_source, source_path, sizeof(g_lc_prompt_duplicate_source) - 1);
    g_lc_prompt_duplicate_source[sizeof(g_lc_prompt_duplicate_source) - 1] = 0;
}

bool lc_file_prompt_duplicate_pending(void)
{
    return g_lc_prompt_duplicate_pending;
}

const char *lc_file_prompt_duplicate_source(void)
{
    return g_lc_prompt_duplicate_source;
}

const char *lc_file_prompt_name(void)
{
    return g_lc_prompt_name;
}

bool lc_file_prompt_name_empty(void)
{
    return g_lc_prompt_name[0] == 0;
}

size_t lc_file_prompt_name_len(void)
{
    return strlen(g_lc_prompt_name);
}

void lc_file_prompt_backspace(void)
{
    size_t len = strlen(g_lc_prompt_name);
    if (len > 0)
        g_lc_prompt_name[len - 1] = 0;
}

void lc_file_prompt_append_digit(char digit)
{
    size_t len = strlen(g_lc_prompt_name);
    if (len + 1 < sizeof(g_lc_prompt_name))
    {
        g_lc_prompt_name[len] = digit;
        g_lc_prompt_name[len + 1] = 0;
    }
}

void lc_file_prompt_finish_duplicate(void)
{
    g_lc_prompt_duplicate_pending = false;
    g_lc_prompt_duplicate_source[0] = 0;
}
