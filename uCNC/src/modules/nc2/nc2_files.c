#include "nc2_files.h"

#include "../file_system.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* The end of the path, if it is this suffix - **either case**.

   The card is FAT and the machine's FatFs is built without long filenames
   (`FF_USE_LFN 0`), so what a directory hands back is an 8.3 name: a file
   written on a PC as `lathe-demo.nc` is stored and reported as something like
   `LATHE-~1.NC`, extension in capitals. nc asked the same question the same way
   (`nc_has_suffix_ci()`); nc2 compared with strcmp and so quietly dropped every
   program the operator had - the list showed no `.nc` at all, and RUN would not
   open one (bench: "it does not show their names properly nor does it seem to
   open them"). */
bool nc2_path_has_suffix(const char *path, const char *suffix)
{
    size_t path_len;
    size_t suffix_len;
    size_t i;

    if (!path || !suffix) {
        return false;
    }
    path_len = strlen(path);
    suffix_len = strlen(suffix);
    if (suffix_len == 0u || path_len < suffix_len) {
        return false;
    }
    path += path_len - suffix_len;
    for (i = 0u; i < suffix_len; i++) {
        if (toupper((unsigned char)path[i]) !=
            toupper((unsigned char)suffix[i])) {
            return false;
        }
    }
    return true;
}

bool nc2_path_is_program(const char *path)
{
    return nc2_path_has_suffix(path, ".nc");
}

bool nc2_path_is_text(const char *path)
{
    return nc2_path_is_program(path) || nc2_path_has_suffix(path, ".t") ||
           nc2_path_has_suffix(path, ".txt");
}

/* One line at a time, cut at the panel's own width: a longer line could not be
   shown or typed into, so what the panel cannot work with is not loaded as if it
   could. A carriage return is dropped - the card may have been written on a PC. */
bool nc2_file_load(nc2_document_t *doc, const char *path)
{
    nc2_document_t loaded;
    fs_file_t *fp;
    char line[NC2_MAX_LINE_LEN];
    size_t used = 0u;

    if (!doc || !path || !path[0]) {
        return false;
    }
    fp = fs_open(path, "r");
    if (!fp) {
        return false;
    }
    nc2_document_init(&loaded);
    snprintf(loaded.path, sizeof(loaded.path), "%s", path);
    while (fs_available(fp) > 0) {
        char c;

        if (fs_read(fp, (uint8_t *)&c, 1u) != 1u) {
            fs_close(fp);
            return false;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            line[used] = '\0';
            if (!nc2_insert_line(&loaded, loaded.line_count, line)) {
                fs_close(fp);
                return false;
            }
            used = 0u;
            continue;
        }
        if (used + 1u < sizeof(line)) {
            line[used++] = c;
        }
    }
    fs_close(fp);
    if (used > 0u) {
        line[used] = '\0';
        if (!nc2_insert_line(&loaded, loaded.line_count, line)) {
            return false;
        }
    }
    if (loaded.line_count == 0u) {
        /* A file with nothing in it is a program with nothing in it: the editor
           needs one line to put the cursor on, which is what a new file is. */
        if (!nc2_insert_line(&loaded, 0u, "")) {
            return false;
        }
    }
    loaded.cursor = 0u;
    loaded.dirty = false;
    *doc = loaded;
    return true;
}

bool nc2_file_save(const nc2_document_t *doc)
{
    fs_file_t *fp;
    size_t i;

    if (!doc || !doc->path[0]) {
        return false;
    }
    fp = fs_open(doc->path, "w");
    if (!fp) {
        return false;
    }
    for (i = 0u; i < doc->line_count; i++) {
        size_t len = strlen(doc->lines[i]);

        if ((len && fs_write(fp, (const uint8_t *)doc->lines[i], len) != len) ||
            fs_write(fp, (const uint8_t *)"\n", 1u) != 1u) {
            fs_close(fp);
            return false;
        }
    }
    fs_close(fp);
    return true;
}

/* --- the list ------------------------------------------------------------- */

static nc2_file_entry_t g_nc2_entries[NC2_FILES_MAX];
static int g_nc2_entry_count;
static int g_nc2_selected;
static char g_nc2_dir[NC2_PATH_MAX];
static char g_nc2_new_name[NC2_NAME_MAX];
static bool g_nc2_new_active;

static bool nc2_entry_less(const nc2_file_entry_t *a, const nc2_file_entry_t *b)
{
    if (a->is_dir != b->is_dir) {
        return a->is_dir;           /* folders first */
    }
    if (strcmp(a->name, "..") == 0) {
        return true;                /* and up is always at the top */
    }
    return strcmp(a->name, b->name) < 0;
}

bool nc2_file_scan(const char *dir)
{
    fs_file_t *dp;
    fs_file_info_t info;
    const char *name;

    if (!dir || !dir[0]) {
        return false;
    }
    g_nc2_entry_count = 0;
    g_nc2_selected = 0;
    snprintf(g_nc2_dir, sizeof(g_nc2_dir), "%s", dir);
    /* Up one level, unless this is the card's own root. */
    if (strcmp(dir, "/D") != 0) {
        snprintf(g_nc2_entries[g_nc2_entry_count].name, NC2_NAME_MAX, "%s", "..");
        g_nc2_entries[g_nc2_entry_count].is_dir = true;
        g_nc2_entry_count++;
    }
    dp = fs_opendir(g_nc2_dir);
    if (!dp) {
        return false;
    }
    while (fs_next_file(dp, &info) && g_nc2_entry_count < NC2_FILES_MAX) {
        const char *slash = strrchr(info.full_name, '/');

        name = slash ? slash + 1 : info.full_name;
        if (!name[0] || (!info.is_dir && !nc2_path_is_text(name))) {
            continue;               /* folders, and what the editor can open */
        }
        snprintf(g_nc2_entries[g_nc2_entry_count].name, NC2_NAME_MAX, "%s", name);
        g_nc2_entries[g_nc2_entry_count].is_dir = info.is_dir;
        g_nc2_entry_count++;
    }
    fs_close(dp);
    /* Sorted, because the card's order is whatever the drive feels like and a
       list that reshuffles under a finger is a list that gets things deleted. */
    {
        int i;
        int j;

        for (i = 1; i < g_nc2_entry_count; i++) {
            nc2_file_entry_t hold = g_nc2_entries[i];

            for (j = i; j > 0 && nc2_entry_less(&hold, &g_nc2_entries[j - 1]);
                 j--) {
                g_nc2_entries[j] = g_nc2_entries[j - 1];
            }
            g_nc2_entries[j] = hold;
        }
    }
    return true;
}

const char *nc2_file_dir(void)
{
    return g_nc2_dir;
}

int nc2_file_count(void)
{
    return g_nc2_entry_count;
}

const nc2_file_entry_t *nc2_file_entry(int index)
{
    if (index < 0 || index >= g_nc2_entry_count) {
        return 0;
    }
    return &g_nc2_entries[index];
}

int nc2_file_selected(void)
{
    return g_nc2_selected;
}

void nc2_file_step(int delta)
{
    int at = g_nc2_selected + delta;

    if (g_nc2_entry_count == 0) {
        return;
    }
    if (at < 0) {
        at = 0;
    }
    if (at >= g_nc2_entry_count) {
        at = g_nc2_entry_count - 1;
    }
    g_nc2_selected = at;
}

bool nc2_file_selected_path(char *out, size_t out_sz)
{
    const nc2_file_entry_t *entry = nc2_file_entry(g_nc2_selected);
    int n;

    if (!entry || !out || out_sz == 0u) {
        return false;
    }
    if (strcmp(entry->name, "..") == 0) {
        char parent[NC2_PATH_MAX];
        const char *slash = strrchr(g_nc2_dir, '/');

        if (!slash || slash == g_nc2_dir) {
            return false;
        }
        snprintf(parent, sizeof(parent), "%.*s", (int)(slash - g_nc2_dir),
                 g_nc2_dir);
        return nc2_file_scan(parent);
    }
    n = snprintf(out, out_sz, "%s/%s", g_nc2_dir, entry->name);
    return n > 0 && (size_t)n < out_sz;
}

bool nc2_file_delete_selected(void)
{
    const nc2_file_entry_t *entry = nc2_file_entry(g_nc2_selected);
    char path[NC2_PATH_MAX];

    if (!entry || entry->is_dir || strcmp(entry->name, "..") == 0) {
        return false;
    }
    snprintf(path, sizeof(path), "%s/%s", g_nc2_dir, entry->name);
    if (!fs_remove(path)) {
        return false;
    }
    return nc2_file_scan(g_nc2_dir);
}

bool nc2_file_create(const char *name, const char *ext, char *out, size_t out_sz)
{
    char path[NC2_PATH_MAX];
    fs_file_t *fp;
    int n;

    if (!name || !name[0] || !ext) {
        return false;
    }
    n = snprintf(path, sizeof(path), "%s/%s%s", g_nc2_dir, name, ext);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        return false;
    }
    fp = fs_open(path, "w");
    if (!fp) {
        return false;
    }
    fs_close(fp);
    if (out && out_sz > 0u) {
        snprintf(out, out_sz, "%s", path);
    }
    return true;
}

void nc2_file_new_begin(void)
{
    g_nc2_new_name[0] = '\0';
    g_nc2_new_active = true;
}

void nc2_file_new_end(void)
{
    g_nc2_new_active = false;
    g_nc2_new_name[0] = '\0';
}

bool nc2_file_new_active(void)
{
    return g_nc2_new_active;
}

void nc2_file_new_digit(char digit)
{
    size_t len = strlen(g_nc2_new_name);

    if (len + 1u >= sizeof(g_nc2_new_name) || digit < '0' || digit > '9') {
        return;
    }
    g_nc2_new_name[len] = digit;
    g_nc2_new_name[len + 1u] = '\0';
}

void nc2_file_new_backspace(void)
{
    size_t len = strlen(g_nc2_new_name);

    if (len > 0u) {
        g_nc2_new_name[len - 1u] = '\0';
    }
}

const char *nc2_file_new_name(void)
{
    return g_nc2_new_name;
}
