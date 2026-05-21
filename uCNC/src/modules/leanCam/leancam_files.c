#include "leancam_files.h"
#include "../file_system.h"
#include "../../cnc.h"
#include "../../interface/grbl_stream.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>

static char g_lc_files[LC_MAX_FILES][LC_FILE_NAME_MAX];
static int  g_lc_file_count = 0;
static bool g_lc_busy = false;
static bool g_lc_show_all = false;

#ifndef LC_FILE_SAVE_DBG
#define LC_FILE_SAVE_DBG 1
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

static void lc_file_io_begin(void)
{
    g_lc_busy = true;
    cnc_set_file_io_critical(true);
}

static void lc_file_io_end(void)
{
    cnc_set_file_io_critical(false);
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

/* ---------- INIT ---------- */

bool leancam_files_init(void)
{
    lc_clear_file_list();
    return true;
}

bool leancam_files_busy(void)
{
    return g_lc_busy;
}

/* ---------- SAVE ---------- */

bool leancam_files_save(const char *path, const program_t *p)
{
    fs_file_t *fp;
    int i;
    uint32_t t0;

    if (!path || !p) return false;

    t0 = mcu_millis();
    LC_SAVE_LOG(__romstr__("[MSG:LC file save enter t=%lu path=%s count=%d]\r\n"),
                (unsigned long)t0,
                path,
                p->count);
    leancam_files_debug_probe("save-enter");
    lc_file_io_begin();
    LC_SAVE_LOG(__romstr__("[MSG:LC file save guard on dt=%lu]\r\n"),
                (unsigned long)(mcu_millis() - t0));
    leancam_files_debug_probe("guard-on");

    LC_SAVE_LOG(__romstr__("[MSG:LC file save open begin dt=%lu]\r\n"),
                (unsigned long)(mcu_millis() - t0));
    leancam_files_debug_probe("open-begin");
    fp = fs_open(path, "w");
    if (!fp)
    {
        LC_SAVE_LOG(__romstr__("[MSG:LC file save open fail dt=%lu]\r\n"),
                    (unsigned long)(mcu_millis() - t0));
        lc_file_io_end();
        return false;
    }
    LC_SAVE_LOG(__romstr__("[MSG:LC file save open ok dt=%lu]\r\n"),
                (unsigned long)(mcu_millis() - t0));
    leancam_files_debug_probe("open-ok");

    for (i = 0; i < p->count; ++i)
    {
        const char *line = p->lines[i];
        size_t len = (line && line[0]) ? strlen(line) : 0u;

        LC_SAVE_LOG(__romstr__("[MSG:LC file save line begin i=%d len=%lu dt=%lu]\r\n"),
                    i,
                    (unsigned long)len,
                    (unsigned long)(mcu_millis() - t0));
        leancam_files_debug_probe("line-begin");

        if (len)
        {
            size_t wrote = fs_write(fp, (const uint8_t *)line, len);
            if (wrote != len)
            {
                LC_SAVE_LOG(__romstr__("[MSG:LC file save line fail i=%d wrote=%lu want=%lu dt=%lu]\r\n"),
                            i,
                            (unsigned long)wrote,
                            (unsigned long)len,
                            (unsigned long)(mcu_millis() - t0));
                fs_close(fp);
                lc_file_io_end();
                return false;
            }
        }

        uint8_t nl = '\n';
        if (fs_write(fp, &nl, 1) != 1)
        {
            LC_SAVE_LOG(__romstr__("[MSG:LC file save nl fail i=%d dt=%lu]\r\n"),
                        i,
                        (unsigned long)(mcu_millis() - t0));
            fs_close(fp);
            lc_file_io_end();
            return false;
        }
        LC_SAVE_LOG(__romstr__("[MSG:LC file save line ok i=%d dt=%lu]\r\n"),
                    i,
                    (unsigned long)(mcu_millis() - t0));
        leancam_files_debug_probe("line-ok");
    }

    LC_SAVE_LOG(__romstr__("[MSG:LC file save close begin dt=%lu]\r\n"),
                (unsigned long)(mcu_millis() - t0));
    leancam_files_debug_probe("close-begin");
    fs_close(fp);
    LC_SAVE_LOG(__romstr__("[MSG:LC file save close ok dt=%lu]\r\n"),
                (unsigned long)(mcu_millis() - t0));
    leancam_files_debug_probe("close-ok");
    lc_file_io_end();
    LC_SAVE_LOG(__romstr__("[MSG:LC file save guard off dt=%lu]\r\n"),
                (unsigned long)(mcu_millis() - t0));
    leancam_files_debug_probe("guard-off");

    return true;
}

/* ---------- LOAD ---------- */

bool leancam_files_load(const char *path, program_t *p)
{
    fs_file_t *fp;
    char line[MAX_LEN];
    int pos = 0;

    if (!path || !p) return false;

    lc_file_io_begin();

    fp = fs_open(path, "r");
    if (!fp)
    {
        lc_file_io_end();
        return false;
    }

    prog_init(p);

    while (fs_available(fp))
    {
        char c = 0;

        if (!fs_read(fp, (uint8_t *)&c, 1))
        {
            fs_close(fp);
            lc_file_io_end();
            return false;
        }

        if (c == '\r') continue;

        if (c == '\n')
        {
            line[pos] = 0;

            if (!prog_add(p, line))
            {
                fs_close(fp);
                lc_file_io_end();
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
        prog_add(p, line);
    }

    fs_close(fp);
    lc_file_io_end();

    return true;
}

/* ---------- REFRESH ---------- */

bool leancam_files_refresh(const char *dir)
{
    fs_file_t *dp;
    fs_file_info_t info;

    if (!dir || !dir[0]) return false;

    lc_clear_file_list();
    lc_file_io_begin();

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

        if (!g_lc_show_all && !lc_has_suffix_ci(name, ".lcam") && !lc_has_suffix_ci(name, ".nc"))
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

void leancam_files_set_show_all(bool show_all)
{
    g_lc_show_all = show_all;
}

bool leancam_files_show_all(void)
{
    return g_lc_show_all;
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

    if (lc_has_suffix_ci(name, ".lcam"))
        snprintf(out, out_sz, "%s/%s", dir, name);
    else
        snprintf(out, out_sz, "%s/%s.lcam", dir, name);

    return true;
}

bool leancam_files_delete_path(const char *path)
{
    bool ok;

    if (!path || !path[0])
        return false;

    lc_file_io_begin();
    ok = fs_remove(path);
    lc_file_io_end();

    return ok;
}
