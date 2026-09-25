#include "nc2_files.h"

#include "../file_system.h"

#include <stdio.h>
#include <string.h>

static const char *nc2_extension(const char *path)
{
    const char *dot;

    if (!path) {
        return "";
    }
    dot = strrchr(path, '.');
    return dot ? dot : "";
}

bool nc2_path_is_program(const char *path)
{
    return strcmp(nc2_extension(path), ".nc") == 0;
}

bool nc2_path_is_text(const char *path)
{
    const char *ext = nc2_extension(path);

    return strcmp(ext, ".nc") == 0 || strcmp(ext, ".t") == 0 ||
           strcmp(ext, ".txt") == 0;
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
        return false;
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
