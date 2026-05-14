#include "../leanCam/leancam_files.h"

#include "ff.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifndef LC_RP2350_FILES_DIR
#define LC_RP2350_FILES_DIR "0:/leancam/files"
#endif

bool leancam_sd_ready(void);

static char g_lc_files[LC_MAX_FILES][LC_FILE_NAME_MAX];
static int g_lc_file_count;
static bool g_lc_busy;

static int lc_stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        ++a;
        ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static bool lc_has_suffix_ci(const char *name, const char *suffix)
{
    size_t ln;
    size_t ls;

    if (!name || !suffix) return false;
    ln = strlen(name);
    ls = strlen(suffix);
    if (ln < ls) return false;
    return lc_stricmp(name + ln - ls, suffix) == 0;
}

static void lc_clear_file_list(void)
{
    int i;

    g_lc_file_count = 0;
    for (i = 0; i < LC_MAX_FILES; ++i) {
        g_lc_files[i][0] = 0;
    }
}

static void lc_sort_files(void)
{
    char tmp[LC_FILE_NAME_MAX];
    int i;
    int j;

    for (i = 0; i < g_lc_file_count - 1; ++i) {
        for (j = i + 1; j < g_lc_file_count; ++j) {
            if (lc_stricmp(g_lc_files[i], g_lc_files[j]) > 0) {
                strcpy(tmp, g_lc_files[i]);
                strcpy(g_lc_files[i], g_lc_files[j]);
                strcpy(g_lc_files[j], tmp);
            }
        }
    }
}

static const char *lc_basename(const char *path)
{
    const char *slash;

    if (!path) return "";
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool lc_make_fat_path(const char *path, char *out, int out_sz)
{
    const char *name;

    if (!out || out_sz <= 0) return false;
    out[0] = 0;

    name = lc_basename(path);
    if (!name[0]) {
        return snprintf(out, (size_t)out_sz, "%s", LC_RP2350_FILES_DIR) < out_sz;
    }

    return snprintf(out, (size_t)out_sz, "%s/%s", LC_RP2350_FILES_DIR, name) < out_sz;
}

static bool lc_ensure_dir(void)
{
    FRESULT fr;

    if (!leancam_sd_ready()) return false;

    fr = f_mkdir("0:/leancam");
    if (fr != FR_OK && fr != FR_EXIST) return false;

    fr = f_mkdir(LC_RP2350_FILES_DIR);
    return fr == FR_OK || fr == FR_EXIST;
}

bool leancam_files_init(void)
{
    lc_clear_file_list();
    return true;
}

bool leancam_files_busy(void)
{
    return g_lc_busy;
}

bool leancam_files_save(const char *path, const program_t *p)
{
    char fat_path[LC_FILE_PATH_MAX + 16];
    FIL file;
    FRESULT fr;
    int i;

    if (!path || !p || !lc_ensure_dir() || !lc_make_fat_path(path, fat_path, sizeof(fat_path))) {
        return false;
    }

    g_lc_busy = true;
    fr = f_open(&file, fat_path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        g_lc_busy = false;
        return false;
    }

    for (i = 0; i < p->count; ++i) {
        UINT bw = 0;
        const char *line = p->lines[i];
        if (line && line[0]) {
            if (f_write(&file, line, (UINT)strlen(line), &bw) != FR_OK || bw != strlen(line)) {
                f_close(&file);
                g_lc_busy = false;
                return false;
            }
        }
        if (f_write(&file, "\n", 1, &bw) != FR_OK || bw != 1) {
            f_close(&file);
            g_lc_busy = false;
            return false;
        }
    }

    f_close(&file);
    g_lc_busy = false;
    return true;
}

bool leancam_files_load(const char *path, program_t *p)
{
    char fat_path[LC_FILE_PATH_MAX + 16];
    FIL file;
    FRESULT fr;
    char line[MAX_LEN];
    int pos = 0;

    if (!path || !p || !leancam_sd_ready() || !lc_make_fat_path(path, fat_path, sizeof(fat_path))) {
        return false;
    }

    g_lc_busy = true;
    fr = f_open(&file, fat_path, FA_READ);
    if (fr != FR_OK) {
        g_lc_busy = false;
        return false;
    }

    prog_init(p);
    while (!f_eof(&file)) {
        char c = 0;
        UINT br = 0;

        fr = f_read(&file, &c, 1, &br);
        if (fr != FR_OK) {
            f_close(&file);
            g_lc_busy = false;
            return false;
        }
        if (br == 0) break;
        if (c == '\r') continue;
        if (c == '\n') {
            line[pos] = 0;
            if (pos > 0 && !prog_add(p, line)) {
                f_close(&file);
                g_lc_busy = false;
                return false;
            }
            pos = 0;
            continue;
        }
        if (pos < MAX_LEN - 1) {
            line[pos++] = c;
        }
    }

    if (pos > 0) {
        line[pos] = 0;
        (void)prog_add(p, line);
    }

    f_close(&file);
    g_lc_busy = false;
    return true;
}

bool leancam_files_refresh(const char *dir)
{
    DIR d;
    FILINFO info;
    FRESULT fr;

    (void)dir;
    lc_clear_file_list();

    if (!lc_ensure_dir()) {
        return false;
    }

    g_lc_busy = true;
    fr = f_opendir(&d, LC_RP2350_FILES_DIR);
    if (fr != FR_OK) {
        g_lc_busy = false;
        return false;
    }

    while (g_lc_file_count < LC_MAX_FILES) {
        fr = f_readdir(&d, &info);
        if (fr != FR_OK || info.fname[0] == 0) break;
        if (info.fattrib & AM_DIR) continue;
        if (!lc_has_suffix_ci(info.fname, ".lcam") && !lc_has_suffix_ci(info.fname, ".nc")) continue;

        strncpy(g_lc_files[g_lc_file_count], info.fname, LC_FILE_NAME_MAX - 1);
        g_lc_files[g_lc_file_count][LC_FILE_NAME_MAX - 1] = 0;
        g_lc_file_count++;
    }

    f_closedir(&d);
    lc_sort_files();
    g_lc_busy = false;
    return true;
}

int leancam_files_count(void)
{
    return g_lc_file_count;
}

const char *leancam_files_name(int index)
{
    if (index < 0 || index >= g_lc_file_count) {
        return NULL;
    }
    return g_lc_files[index];
}

bool leancam_files_build_path(const char *dir, int index, char *out, int out_sz)
{
    const char *name = leancam_files_name(index);

    (void)dir;
    if (!name || !out || out_sz <= 0) {
        return false;
    }
    return snprintf(out, (size_t)out_sz, "%s/%s", LC_DEFAULT_DIR, name) < out_sz;
}

bool leancam_files_make_new_path(const char *dir, const char *name, char *out, int out_sz)
{
    (void)dir;
    if (!name || !name[0] || !out || out_sz <= 0) {
        return false;
    }
    if (lc_has_suffix_ci(name, ".lcam")) {
        return snprintf(out, (size_t)out_sz, "%s/%s", LC_DEFAULT_DIR, name) < out_sz;
    }
    return snprintf(out, (size_t)out_sz, "%s/%s.lcam", LC_DEFAULT_DIR, name) < out_sz;
}

bool leancam_files_delete_path(const char *path)
{
    char fat_path[LC_FILE_PATH_MAX + 16];
    bool ok;

    if (!path || !path[0] || !leancam_sd_ready() || !lc_make_fat_path(path, fat_path, sizeof(fat_path))) {
        return false;
    }

    g_lc_busy = true;
    ok = (f_unlink(fat_path) == FR_OK);
    g_lc_busy = false;
    return ok;
}
