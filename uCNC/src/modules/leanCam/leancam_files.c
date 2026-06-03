/* LeanCam module contract:
 * Purpose: file paths, file browser model, filename prompt, and plain program load/save helpers.
 * Called by: leancam_bridge and resource/autosave flow.
 * Calls into: uCNC file_system APIs and program storage helpers.
 * Owns: file browser/prompt transient state and path utilities; it must not advance editor or renderer state.
 */
#include "leancam_files.h"
#include "leancam_resource.h"
#include "../file_system.h"
#include "../../interface/grbl_stream.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>

static char g_lc_files[LC_MAX_FILES][LC_FILE_NAME_MAX];
static int  g_lc_file_count = 0;
static bool g_lc_busy = false;
static int g_lc_file_selected = 0;
static bool g_lc_files_ready = false;
static uint32_t g_lc_file_retry_due_ms = 0;
static char g_lc_prompt_name[32];
static bool g_lc_prompt_duplicate_pending = false;
static char g_lc_prompt_duplicate_source[LC_FILE_PATH_MAX];

#ifndef LEANCAM_DEBUG
#define LEANCAM_DEBUG 0
#endif

#ifndef LEANCAM_DEBUG_FILES
#define LEANCAM_DEBUG_FILES 0
#endif

#ifndef LC_FILE_SAVE_DBG
#define LC_FILE_SAVE_DBG LEANCAM_DEBUG_FILES
#endif

#if LC_FILE_SAVE_DBG
#define LC_SAVE_LOG(...) grbl_stream_printf(__VA_ARGS__)
#else
#define LC_SAVE_LOG(...) do { } while (0)
#endif

/* ---------- IO GUARD ---------- */

static bool lc_file_io_begin(void)
{
    if (!lc_resource_file_begin("files"))
        return false;

    g_lc_busy = true;
    return true;
}

static void lc_file_io_end(void)
{
    lc_resource_file_end();
    g_lc_busy = false;
}

/* ---------- UTILS ---------- */

static int lc_stricmp(const char *a, const char *b)
{
    while (*a && *b)
    {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        ++a; ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static bool lc_has_suffix_ci(const char *name, const char *suffix)
{
    size_t ln = strlen(name);
    size_t ls = strlen(suffix);
    if (ln < ls) return false;
    return lc_stricmp(name + ln - ls, suffix) == 0;
}

const char *lc_path_basename(const char *path)
{
    const char *p1;
    const char *p2;
    const char *p;

    if (!path || !path[0])
        return "<no file>";

    p1 = strrchr(path, '/');
    p2 = strrchr(path, '\\');
    p = p1 > p2 ? p1 : p2;
    return p ? p + 1 : path;
}

int lc_path_stricmp(const char *a, const char *b)
{
    return lc_stricmp(a ? a : "", b ? b : "");
}

bool lc_path_has_suffix_ci(const char *name, const char *suffix)
{
    if (!name || !suffix)
        return false;
    return lc_has_suffix_ci(name, suffix);
}

static bool lc_path_is_name_char(char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

static void lc_path_sanitize_name_part(const char *in, char *out, size_t out_sz)
{
    size_t i = 0;
    char last = 0;

    if (!out || out_sz == 0)
        return;

    out[0] = 0;
    if (!in)
        return;

    while (*in && i + 1 < out_sz)
    {
        char c = *in++;
        if (!lc_path_is_name_char(c))
            c = '_';
        if (c == '_' && last == '_')
            continue;
        out[i++] = c;
        last = c;
    }

    while (i > 0 && out[i - 1] == '_')
        --i;
    out[i] = 0;
}

void lc_path_get_program_stem(const char *current_path, char *out, size_t out_sz)
{
    const char *base;
    const char *dot;
    size_t len;
    char tmp[40];

    if (!out || out_sz == 0)
        return;

    base = lc_path_basename(current_path);
    dot = strrchr(base, '.');
    len = dot && dot > base ? (size_t)(dot - base) : strlen(base);
    if (len >= sizeof(tmp))
        len = sizeof(tmp) - 1u;

    memcpy(tmp, base, len);
    tmp[len] = 0;

    lc_path_sanitize_name_part(tmp, out, out_sz);
    if (!out[0])
    {
        strncpy(out, "program", out_sz - 1u);
        out[out_sz - 1u] = 0;
    }
}

void lc_path_get_operation_name(const char *line, char *out, size_t out_sz)
{
    char tmp[20];
    size_t n = 0;

    if (!out || out_sz == 0)
        return;

    if (!line)
        line = "";

    while (line[n] && line[n] != '{' && line[n] != ' ' && line[n] != '\t' && n + 1 < sizeof(tmp))
        ++n;

    memcpy(tmp, line, n);
    tmp[n] = 0;

    lc_path_sanitize_name_part(tmp, out, out_sz);
    if (!out[0])
    {
        strncpy(out, "OP", out_sz - 1u);
        out[out_sz - 1u] = 0;
    }
}

static void lc_clear_file_list(void)
{
    g_lc_file_count = 0;
    for (int i = 0; i < LC_MAX_FILES; ++i)
        g_lc_files[i][0] = 0;
}

static void lc_sort_files(void)
{
    char tmp[LC_FILE_NAME_MAX];

    for (int i = 0; i < g_lc_file_count - 1; ++i)
    {
        for (int j = i + 1; j < g_lc_file_count; ++j)
        {
            if (lc_stricmp(g_lc_files[i], g_lc_files[j]) > 0)
            {
                strcpy(tmp, g_lc_files[i]);
                strcpy(g_lc_files[i], g_lc_files[j]);
                strcpy(g_lc_files[j], tmp);
            }
        }
    }
}

static bool lc_write_program_path(const char *path, const program_t *p, uint32_t t0, const char *tag)
{
    fs_file_t *fp;
    int i;
    static char write_buf[(MAX_LINES * (MAX_LEN + 1)) + 1];
    size_t used = 0;

#if !LC_FILE_SAVE_DBG
    (void)t0;
    (void)tag;
#endif

    for (i = 0; i < p->count; ++i)
    {
        const char *line = p->lines[i];
        size_t len = (line && line[0]) ? strlen(line) : 0u;

        if ((used + len + 1u) > sizeof(write_buf))
            return false;

        if (len)
        {
            memcpy(write_buf + used, line, len);
            used += len;
        }

        write_buf[used++] = '\n';
    }

    LC_SAVE_LOG(__romstr__("[MSG:LC file save open begin tag=%s dt=%lu]\r\n"),
                tag,
                (unsigned long)(mcu_millis() - t0));
    fp = fs_open(path, "w");
    if (!fp)
    {
        LC_SAVE_LOG(__romstr__("[MSG:LC file save open fail tag=%s dt=%lu]\r\n"),
                    tag,
                    (unsigned long)(mcu_millis() - t0));
        return false;
    }
    LC_SAVE_LOG(__romstr__("[MSG:LC file save open ok tag=%s dt=%lu]\r\n"),
                tag,
                (unsigned long)(mcu_millis() - t0));

    if (used)
    {
        size_t wrote;

        LC_SAVE_LOG(__romstr__("[MSG:LC file save write begin tag=%s bytes=%lu dt=%lu]\r\n"),
                    tag,
                    (unsigned long)used,
                    (unsigned long)(mcu_millis() - t0));
        wrote = fs_write(fp, (const uint8_t *)write_buf, used);
        if (wrote != used)
        {
            LC_SAVE_LOG(__romstr__("[MSG:LC file save write fail tag=%s wrote=%lu want=%lu dt=%lu]\r\n"),
                        tag,
                        (unsigned long)wrote,
                        (unsigned long)used,
                        (unsigned long)(mcu_millis() - t0));
            fs_close(fp);
            return false;
        }

        LC_SAVE_LOG(__romstr__("[MSG:LC file save write ok tag=%s dt=%lu]\r\n"),
                    tag,
                    (unsigned long)(mcu_millis() - t0));
    }

    LC_SAVE_LOG(__romstr__("[MSG:LC file save close begin tag=%s dt=%lu]\r\n"),
                tag,
                (unsigned long)(mcu_millis() - t0));
    fs_close(fp);
    LC_SAVE_LOG(__romstr__("[MSG:LC file save close ok tag=%s dt=%lu]\r\n"),
                tag,
                (unsigned long)(mcu_millis() - t0));

    return true;
}

static bool lc_load_program_path(const char *path, program_t *p)
{
    fs_file_t *fp;
    char line[MAX_LEN];
    int pos = 0;

    fp = fs_open(path, "r");
    if (!fp)
        return false;

    prog_init(p);

    while (fs_available(fp))
    {
        char c = 0;

        if (!fs_read(fp, (uint8_t *)&c, 1))
        {
            fs_close(fp);
            return false;
        }

        if (c == '\r') continue;

        if (c == '\n')
        {
            line[pos] = 0;

            if (!prog_add(p, line))
            {
                fs_close(fp);
                return false;
            }

            pos = 0;
            continue;
        }

        if (pos < MAX_LEN - 1)
            line[pos++] = c;
    }

    if (pos > 0)
    {
        line[pos] = 0;
        if (!prog_add(p, line))
        {
            fs_close(fp);
            return false;
        }
    }

    fs_close(fp);
    return true;
}

/* ---------- INIT ---------- */

bool leancam_files_init(void)
{
    lc_clear_file_list();
    lc_file_browser_init();
    lc_file_prompt_clear();
    return true;
}

bool leancam_files_busy(void)
{
    return g_lc_busy || lc_resource_file_busy();
}

/* ---------- SAVE ---------- */

bool leancam_files_save(const char *path, const program_t *p)
{
    uint32_t t0;

    if (!path || !p) return false;

    t0 = mcu_millis();
    LC_SAVE_LOG(__romstr__("[MSG:LC file save enter t=%lu path=%s count=%d]\r\n"),
                (unsigned long)t0,
                path,
                p->count);
    if (!lc_file_io_begin())
        return false;
    LC_SAVE_LOG(__romstr__("[MSG:LC file save guard on dt=%lu]\r\n"),
                (unsigned long)(mcu_millis() - t0));

    if (!lc_write_program_path(path, p, t0, "final"))
    {
        lc_file_io_end();
        return false;
    }

    lc_file_io_end();
    LC_SAVE_LOG(__romstr__("[MSG:LC file save guard off dt=%lu]\r\n"),
                (unsigned long)(mcu_millis() - t0));

    return true;
}

bool leancam_files_save_plain(const char *path, const program_t *p)
{
    uint32_t t0;
    bool ok;

    if (!path || !p) return false;

    t0 = mcu_millis();
    if (!lc_file_io_begin())
        return false;
    ok = lc_write_program_path(path, p, t0, "plain");
    lc_file_io_end();
    return ok;
}

/* ---------- LOAD ---------- */

bool leancam_files_load(const char *path, program_t *p)
{
    bool ok;

    if (!path || !p) return false;

    if (!lc_file_io_begin())
        return false;
    ok = lc_load_program_path(path, p);
    lc_file_io_end();
    return ok;
}

/* ---------- REFRESH ---------- */

bool leancam_files_refresh(const char *dir)
{
    fs_file_t *dp;
    fs_file_info_t info;

    if (!dir || !dir[0]) return false;

    lc_clear_file_list();
    if (!lc_file_io_begin())
        return false;

    dp = fs_opendir(dir);
    if (!dp)
    {
        lc_file_io_end();
        return false;
    }

    while (g_lc_file_count < LC_MAX_FILES)
    {
        if (!fs_next_file(dp, &info))
            break;

        if (info.is_dir)
            continue;

        const char *name = strrchr(info.full_name, '/');
        if (!name) continue;
        name++;

        if (!lc_has_suffix_ci(name, ".nc") &&
            !lc_has_suffix_ci(name, ".ngc") &&
            !lc_has_suffix_ci(name, ".gcode") &&
            !lc_has_suffix_ci(name, ".dxf"))
            continue;

        strncpy(g_lc_files[g_lc_file_count], name, LC_FILE_NAME_MAX - 1);
        g_lc_files[g_lc_file_count][LC_FILE_NAME_MAX - 1] = 0;
        ++g_lc_file_count;
    }

    fs_close(dp);
    lc_sort_files();
    lc_file_io_end();

    return true;
}

/* ---------- ACCESS ---------- */

int leancam_files_count(void)
{
    return g_lc_file_count;
}

const char *leancam_files_name(int index)
{
    if (index < 0 || index >= g_lc_file_count)
        return NULL;

    return g_lc_files[index];
}

bool leancam_files_build_path(const char *dir, int index, char *out, int out_sz)
{
    const char *name;

    if (!dir || !out || out_sz <= 0)
        return false;

    name = leancam_files_name(index);
    if (!name)
        return false;

    snprintf(out, out_sz, "%s/%s", dir, name);
    return true;
}

bool leancam_files_make_new_path(const char *dir, const char *name, char *out, int out_sz)
{
    if (!dir || !name || !name[0] || !out)
        return false;

    if (lc_has_suffix_ci(name, ".nc"))
        snprintf(out, out_sz, "%s/%s", dir, name);
    else
        snprintf(out, out_sz, "%s/%s.nc", dir, name);

    return true;
}

bool leancam_files_delete_path(const char *path)
{
    bool ok;

    if (!path || !path[0])
        return false;

    if (!lc_file_io_begin())
        return false;
    ok = fs_remove(path);
    lc_file_io_end();

    return ok;
}

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


