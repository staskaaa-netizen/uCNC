#include "nc2_presets.h"

#include "../file_system.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* The address is the file's name, and the digits are checked here rather than
   trusted: everything else in the module hands addresses in, and a stray path
   would write outside the folder. */
static bool nc2_addr_ok(const char *address)
{
    size_t len;

    if (!address || !address[0]) {
        return false;
    }
    len = strlen(address);
    if (len > NC2_PRESET_ADDR_MAX) {
        return false;
    }
    while (*address) {
        if (*address < '0' || *address > '9') {
            return false;
        }
        address++;
    }
    return true;
}

bool nc2_preset_path(const char *address, char *out, size_t out_sz)
{
    int n;

    if (!out || out_sz == 0u || !nc2_addr_ok(address)) {
        return false;
    }
    n = snprintf(out, out_sz, "%s%s%s", NC2_PRESET_DIR, address,
                 NC2_PRESET_SUFFIX);
    return n > 0 && (size_t)n < out_sz;
}

bool nc2_preset_exists(const char *address)
{
    fs_file_info_t info;
    char path[64];

    return nc2_preset_path(address, path, sizeof(path)) && fs_finfo(path, &info);
}

/* Append one row and its newline. */
static bool nc2_join_row(char *out, size_t out_sz, size_t *used,
                         const char *row, size_t len)
{
    size_t sep = *used > 0u ? 1u : 0u;

    if (*used + sep + len + 1u > out_sz) {
        return false;
    }
    if (sep) {
        out[(*used)++] = '\n';
    }
    memcpy(out + *used, row, len);
    *used += len;
    out[*used] = '\0';
    return true;
}

bool nc2_preset_read(const char *address, char *name, size_t name_sz,
                     char *rows, size_t rows_sz)
{
    char path[64];
    char row[NC2_PRESET_ROW_MAX];
    fs_file_t *fp;
    size_t used = 0u;
    size_t row_used = 0u;
    bool first = true;
    bool ok = true;

    if (name && name_sz > 0u) {
        name[0] = '\0';
    }
    if (rows && rows_sz > 0u) {
        rows[0] = '\0';
    }
    if (!nc2_preset_path(address, path, sizeof(path))) {
        return false;
    }
    fp = fs_open(path, "r");
    if (!fp) {
        return false;
    }
    while (fs_available(fp) > 0) {
        char c;

        if (fs_read(fp, (uint8_t *)&c, 1u) != 1u) {
            ok = false;
            break;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n' || row_used + 1u >= sizeof(row)) {
            row[row_used] = '\0';
            if (first) {
                first = false;
                if (name && name_sz > 0u) {
                    strncpy(name, row, name_sz - 1u);
                    name[name_sz - 1u] = '\0';
                }
            } else if (rows && rows_sz > 0u) {
                ok = nc2_join_row(rows, rows_sz, &used, row, row_used);
                if (!ok) {
                    break;
                }
            }
            row_used = 0u;
            continue;               /* a row longer than the buffer is cut */
        }
        row[row_used++] = c;
    }
    if (ok && row_used > 0u) {
        row[row_used] = '\0';
        if (first) {
            if (name && name_sz > 0u) {
                strncpy(name, row, name_sz - 1u);
                name[name_sz - 1u] = '\0';
            }
        } else if (rows && rows_sz > 0u) {
            ok = nc2_join_row(rows, rows_sz, &used, row, row_used);
        }
    }
    fs_close(fp);
    return ok;
}

int nc2_preset_write(const char *address, const char *name, const char *rows)
{
    fs_file_info_t info;
    fs_file_t *fp;
    char path[64];
    const char *row;

    if (!nc2_preset_path(address, path, sizeof(path)) || !name) {
        return -1;
    }
    if (fs_finfo(path, &info)) {
        return 0;                   /* the operator's file is never replaced */
    }
    fp = fs_open(path, "w");
    if (!fp) {
        return -1;
    }
    if (fs_write(fp, (const uint8_t *)name, strlen(name)) != strlen(name) ||
        fs_write(fp, (const uint8_t *)"\n", 1u) != 1u) {
        fs_close(fp);
        return -1;
    }
    row = rows;
    while (row) {
        const char *end = strchr(row, '\n');
        size_t len = end ? (size_t)(end - row) : strlen(row);
        bool more = end != 0;

        if ((len && fs_write(fp, (const uint8_t *)row, len) != len) ||
            fs_write(fp, (const uint8_t *)"\n", 1u) != 1u) {
            fs_close(fp);
            return -1;
        }
        row = more ? end + 1 : 0;
    }
    fs_close(fp);
    return 1;
}

bool nc2_presets_any(void)
{
    fs_file_t *dir;
    fs_file_info_t info;
    const char *suffix = NC2_PRESET_SUFFIX;
    size_t suffix_len = strlen(suffix);

    dir = fs_opendir(NC2_PRESET_ROOT);
    if (!dir) {
        return false;
    }
    while (fs_next_file(dir, &info)) {
        size_t len = strlen(info.full_name);

        if (info.is_dir || len < suffix_len) {
            continue;
        }
        if (strcmp(info.full_name + len - suffix_len, suffix) == 0) {
            fs_close(dir);
            return true;
        }
    }
    fs_close(dir);
    return false;
}
