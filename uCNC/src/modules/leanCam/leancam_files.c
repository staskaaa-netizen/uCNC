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

#ifndef LC_FILE_SAVE_DBG
#define LC_FILE_SAVE_DBG 0
#endif

#if LC_FILE_SAVE_DBG
#define LC_SAVE_LOG(...) grbl_stream_printf(__VA_ARGS__)
#else
#define LC_SAVE_LOG(...) do { } while (0)
#endif

void __attribute__((weak)) leancam_files_debug_probe(const char *stage)
{
    (void)stage;
}

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
    leancam_files_debug_probe("open-begin");
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
    leancam_files_debug_probe("open-ok");

    if (used)
    {
        size_t wrote;

        LC_SAVE_LOG(__romstr__("[MSG:LC file save write begin tag=%s bytes=%lu dt=%lu]\r\n"),
                    tag,
                    (unsigned long)used,
                    (unsigned long)(mcu_millis() - t0));
        leancam_files_debug_probe("write-begin");

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
        leancam_files_debug_probe("write-ok");
    }

    LC_SAVE_LOG(__romstr__("[MSG:LC file save close begin tag=%s dt=%lu]\r\n"),
                tag,
                (unsigned long)(mcu_millis() - t0));
    leancam_files_debug_probe("close-begin");
    fs_close(fp);
    LC_SAVE_LOG(__romstr__("[MSG:LC file save close ok tag=%s dt=%lu]\r\n"),
                tag,
                (unsigned long)(mcu_millis() - t0));
    leancam_files_debug_probe("close-ok");

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
    leancam_files_debug_probe("save-enter");
    if (!lc_file_io_begin())
        return false;
    LC_SAVE_LOG(__romstr__("[MSG:LC file save guard on dt=%lu]\r\n"),
                (unsigned long)(mcu_millis() - t0));
    leancam_files_debug_probe("guard-on");

    if (!lc_write_program_path(path, p, t0, "final"))
    {
        lc_file_io_end();
        return false;
    }

    lc_file_io_end();
    LC_SAVE_LOG(__romstr__("[MSG:LC file save guard off dt=%lu]\r\n"),
                (unsigned long)(mcu_millis() - t0));
    leancam_files_debug_probe("guard-off");

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

        if (!lc_has_suffix_ci(name, ".nc"))
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
