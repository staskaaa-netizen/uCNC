#include "nc_files.h"
#include "../file_system.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    char name[NC_FILE_NAME_MAX];
    bool is_dir;
} nc_file_entry_t;

static nc_file_entry_t g_nc_files[NC_MAX_FILES];
static int g_nc_file_count;
static int g_nc_file_selected;
static bool g_nc_files_ready;
static bool g_nc_files_active;
static char g_nc_files_cwd[NC_PATH_MAX];

static const char *nc_files_basename(const char *path)
{
    const char *p;

    if (!path) {
        return "";
    }

    p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static bool nc_files_valid_entry_name(const char *name)
{
    const char *p;
    bool has_text = false;

    if (!name || !name[0] || strcmp(name, ".") == 0 || strcmp(name, "/") == 0) {
        return false;
    }
    for (p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 33 || c > 126 || c == '/' || c == '\\') {
            return false;
        }
        has_text = true;
    }
    return has_text;
}

static bool nc_files_parent_path(const char *path, char *out, int out_sz)
{
    const char *slash;
    size_t len;

    if (!path || !out || out_sz <= 0 || strcmp(path, "/D") == 0) {
        return false;
    }

    slash = strrchr(path, '/');
    if (!slash || slash == path || slash == path + 2) {
        strncpy(out, "/D", (size_t)out_sz - 1);
        out[out_sz - 1] = '\0';
        return true;
    }

    len = (size_t)(slash - path);
    if (len >= (size_t)out_sz) {
        len = (size_t)out_sz - 1;
    }
    memcpy(out, path, len);
    out[len] = '\0';
    return true;
}

void nc_files_init(void)
{
    g_nc_file_count = 0;
    g_nc_file_selected = 0;
    g_nc_files_ready = false;
    g_nc_files_active = false;
    strncpy(g_nc_files_cwd, NC_FILES_DIR, sizeof(g_nc_files_cwd) - 1);
    g_nc_files_cwd[sizeof(g_nc_files_cwd) - 1] = '\0';
}

void nc_files_set_active(bool active)
{
    g_nc_files_active = active;
}

bool nc_files_active(void)
{
    return g_nc_files_active;
}

bool nc_files_refresh(const char *dir)
{
    fs_file_t *dp;
    fs_file_info_t info;
    const char *scan_dir = (dir && dir[0]) ? dir : g_nc_files_cwd;

    g_nc_file_count = 0;
    g_nc_file_selected = 0;
    g_nc_files_ready = false;

    dp = fs_opendir(scan_dir);
    if (!dp && strcmp(scan_dir, "/D") != 0) {
        scan_dir = "/D";
        dp = fs_opendir(scan_dir);
    }
    if (!dp) {
        return false;
    }

    strncpy(g_nc_files_cwd, scan_dir, sizeof(g_nc_files_cwd) - 1);
    g_nc_files_cwd[sizeof(g_nc_files_cwd) - 1] = '\0';

    if (strcmp(g_nc_files_cwd, "/D") != 0 && g_nc_file_count < NC_MAX_FILES) {
        strncpy(g_nc_files[g_nc_file_count].name, "..", NC_FILE_NAME_MAX - 1);
        g_nc_files[g_nc_file_count].name[NC_FILE_NAME_MAX - 1] = '\0';
        g_nc_files[g_nc_file_count].is_dir = true;
        g_nc_file_count++;
    }

    while (g_nc_file_count < NC_MAX_FILES) {
        const char *name;

        memset(&info, 0, sizeof(info));
        if (!fs_next_file(dp, &info)) {
            break;
        }
        name = nc_files_basename(info.full_name);
        if (!nc_files_valid_entry_name(name)) {
            continue;
        }
        if (!info.is_dir && !nc_path_supported(name)) {
            continue;
        }

        strncpy(g_nc_files[g_nc_file_count].name, name, NC_FILE_NAME_MAX - 1);
        g_nc_files[g_nc_file_count].name[NC_FILE_NAME_MAX - 1] = '\0';
        g_nc_files[g_nc_file_count].is_dir = info.is_dir;
        g_nc_file_count++;
    }

    fs_close(dp);
    g_nc_files_ready = true;
    return true;
}

int nc_files_count(void)
{
    return g_nc_file_count;
}

const char *nc_files_name(int index)
{
    if (index < 0 || index >= g_nc_file_count) {
        return "";
    }
    return g_nc_files[index].name;
}

const char *nc_files_cwd(void)
{
    return g_nc_files_cwd;
}

bool nc_files_is_dir(int index)
{
    return index >= 0 && index < g_nc_file_count && g_nc_files[index].is_dir;
}

bool nc_files_selected_is_dir(void)
{
    return nc_files_is_dir(g_nc_file_selected);
}

bool nc_files_build_path(int index, char *out, int out_sz)
{
    const char *name = nc_files_name(index);

    if (!out || out_sz <= 0 || !nc_files_valid_entry_name(name)) {
        return false;
    }

    if (strcmp(name, "..") == 0) {
        return nc_files_parent_path(g_nc_files_cwd, out, out_sz);
    }
    if (strchr(name, '/')) {
        return false;
    }

    snprintf(out, (size_t)out_sz, "%s/%s", g_nc_files_cwd, name);
    out[out_sz - 1] = '\0';
    return true;
}

int nc_files_selected(void)
{
    return g_nc_file_selected;
}

void nc_files_select_prev(void)
{
    if (g_nc_file_selected > 0) {
        g_nc_file_selected--;
    }
}

void nc_files_select_next(void)
{
    if (g_nc_file_selected + 1 < g_nc_file_count) {
        g_nc_file_selected++;
    }
}

bool nc_files_selected_path(char *out, int out_sz)
{
    return nc_files_build_path(g_nc_file_selected, out, out_sz);
}

bool nc_files_enter_selected(void)
{
    char path[NC_PATH_MAX];

    if (!nc_files_selected_is_dir() || !nc_files_selected_path(path, sizeof(path))) {
        return false;
    }

    return nc_files_refresh(path);
}

bool nc_files_go_parent(void)
{
    char path[NC_PATH_MAX];

    if (!nc_files_parent_path(g_nc_files_cwd, path, sizeof(path))) {
        return false;
    }

    return nc_files_refresh(path);
}

bool nc_files_delete_selected(void)
{
    char path[NC_PATH_MAX];

    if (nc_files_selected_is_dir() || !nc_files_selected_path(path, sizeof(path))) {
        return false;
    }

    if (!fs_remove(path)) {
        return false;
    }

    return nc_files_refresh(g_nc_files_cwd);
}

bool nc_files_create_named(const char *name, const char *ext, char *out_path, int out_sz)
{
    char path[NC_PATH_MAX];
    const char *suffix = ext && ext[0] ? ext : ".nc";
    fs_file_t *fp;
    int n;

    if (!name || !name[0] || !nc_files_valid_entry_name(name) ||
        strchr(name, '.') || suffix[0] != '.') {
        return false;
    }
    n = snprintf(path, sizeof(path), "%s/%s%s", g_nc_files_cwd, name, suffix);
    if (n < 0 || (size_t)n >= sizeof(path) || fs_finfo(path, &(fs_file_info_t){0})) {
        return false;
    }

    fp = fs_open(path, "w");
    if (!fp) {
        return false;
    }
    (void)fs_write(fp, (const uint8_t *)"(new NC file)\n", 14);
    fs_close(fp);

    if (out_path && out_sz > 0) {
        strncpy(out_path, path, (size_t)out_sz - 1);
        out_path[out_sz - 1] = '\0';
    }
    (void)nc_files_refresh(g_nc_files_cwd);
    return true;
}

bool nc_files_ready(void)
{
    return g_nc_files_ready;
}
