/* Windows filesystem driver for the host panel build.

   The NC module addresses everything through uCNC's fs_t API on drive /D
   ("/D/nc/files", "/D/nc_state.txt", ...). The machine mounts sd_card_v2 there;
   on the desktop this driver maps the same paths onto a real directory, so the
   NC file manager, the state store and MDI all work unchanged. */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/* windows.h defines FORCEINLINE with a storage class; the firmware headers
   declare `static FORCEINLINE`, so drop it before including them. */
#undef FORCEINLINE
#endif

#include "host_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file_system.h"

#define HOST_FS_ROOT_MAX 512
#define HOST_FS_PATH_MAX 640

typedef struct {
    FILE *fp;
    HANDLE find;
    WIN32_FIND_DATAA data;
    char pattern[HOST_FS_PATH_MAX];
    bool first;
} host_file_t;

static char g_root[HOST_FS_ROOT_MAX];
static fs_t g_drive;

/* "/D/nc/files" -> "<root>\nc\files". Everything under /D is mapped; other
   drives are rejected. */
static bool host_path(const char *path, char *out, size_t out_sz)
{
    size_t i = 0u;

    if (!path || !out || out_sz == 0u)
        return false;
    if (strncmp(path, "/D", 2u) != 0)
        return false;
    path += 2u;
    if (*path == '/')
        path++;
    out[i++] = '\0';
    i = 0u;
    while (*path && i + 2u < out_sz) {
        out[i++] = (*path == '/') ? '\\' : *path;
        path++;
    }
    out[i] = '\0';
    {
        char joined[HOST_FS_PATH_MAX];
        if (g_root[0] == '\0')
            snprintf(joined, sizeof(joined), "%s", out);
        else
            snprintf(joined, sizeof(joined), "%s\\%s", g_root, out);
        snprintf(out, out_sz, "%s", joined);
    }
    return true;
}

static void host_mkdir_deep(const char *dir)
{
#ifdef _WIN32
    char temp[HOST_FS_PATH_MAX];
    size_t len;

    snprintf(temp, sizeof(temp), "%s", dir);
    len = strlen(temp);
    while (len > 3u) {
        if (temp[len - 1u] == '\\') {
            temp[len - 1u] = '\0';
            CreateDirectoryA(temp, NULL);
        }
        len--;
    }
    CreateDirectoryA(dir, NULL);
#else
    (void)dir;
#endif
}

static fs_file_t *host_open(const char *path, const char *mode)
{
    char real[HOST_FS_PATH_MAX];
    fs_file_t *file;
    host_file_t *host;

    if (!host_path(path, real, sizeof(real)))
        return NULL;
    file = calloc(1, sizeof(*file));
    host = calloc(1, sizeof(*host));
    if (!file || !host) {
        free(file);
        free(host);
        return NULL;
    }
    host->fp = fopen(real, mode && *mode ? mode : "rb");
    if (!host->fp) {
        free(file);
        free(host);
        return NULL;
    }
    file->file_ptr = host;
    file->fs_ptr = &g_drive;
    return file;
}

static size_t host_read(fs_file_t *file, uint8_t *buffer, size_t len)
{
    host_file_t *host = file ? file->file_ptr : NULL;
    if (!host || !host->fp || !buffer)
        return 0u;
    return fread(buffer, 1u, len, host->fp);
}

static size_t host_write(fs_file_t *file, const uint8_t *buffer, size_t len)
{
    host_file_t *host = file ? file->file_ptr : NULL;
    if (!host || !host->fp || !buffer)
        return 0u;
    return fwrite(buffer, 1u, len, host->fp);
}

static bool host_seek(fs_file_t *file, uint32_t position)
{
    host_file_t *host = file ? file->file_ptr : NULL;
    return host && host->fp && fseek(host->fp, (long)position, SEEK_SET) == 0;
}

static int host_available(fs_file_t *file)
{
    host_file_t *host = file ? file->file_ptr : NULL;
    long here;
    long end;

    if (!host || !host->fp)
        return 0;
    here = ftell(host->fp);
    if (here < 0L || fseek(host->fp, 0L, SEEK_END) != 0)
        return 0;
    end = ftell(host->fp);
    fseek(host->fp, here, SEEK_SET);
    return (int)(end - here);
}

static void host_close(fs_file_t *file)
{
    host_file_t *host = file ? file->file_ptr : NULL;

    if (host) {
        if (host->fp)
            fclose(host->fp);
#ifdef _WIN32
        if (host->find && host->find != INVALID_HANDLE_VALUE)
            FindClose(host->find);
#endif
        free(host);
    }
    free(file);
}

static bool host_remove(const char *path)
{
    char real[HOST_FS_PATH_MAX];
#ifdef _WIN32
    return host_path(path, real, sizeof(real)) && DeleteFileA(real);
#else
    (void)real;
    return false;
#endif
}

static fs_file_t *host_opendir(const char *path)
{
    char real[HOST_FS_PATH_MAX];
    fs_file_t *file;
    host_file_t *host;

    if (!host_path(path, real, sizeof(real)))
        return NULL;
    file = calloc(1, sizeof(*file));
    host = calloc(1, sizeof(*host));
    if (!file || !host) {
        free(file);
        free(host);
        return NULL;
    }
#ifdef _WIN32
    snprintf(host->pattern, sizeof(host->pattern), "%s\\*", real);
    host->first = true;
#else
    (void)host->pattern;
#endif
    file->file_ptr = host;
    file->fs_ptr = &g_drive;
    return file;
}

static bool host_next_file(fs_file_t *file, fs_file_info_t *info)
{
    host_file_t *host = file ? file->file_ptr : NULL;

    if (!host || !info)
        return false;
    info->is_dir = false;
    info->size = 0u;
    info->timestamp = 0u;
    info->full_name[0] = '\0';
#ifdef _WIN32
    for (;;) {
        if (host->first) {
            host->find = FindFirstFileA(host->pattern, &host->data);
            host->first = false;
            if (host->find == INVALID_HANDLE_VALUE)
                return false;
        } else if (!FindNextFileA(host->find, &host->data)) {
            return false;
        }
        if (strcmp(host->data.cFileName, ".") == 0 ||
            strcmp(host->data.cFileName, "..") == 0)
            continue;
        snprintf(info->full_name, sizeof(info->full_name), "%s",
                 host->data.cFileName);
        info->is_dir = (host->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        info->size = host->data.nFileSizeLow;
        return true;
    }
#else
    return false;
#endif
}

static bool host_mkdir(const char *path)
{
    char real[HOST_FS_PATH_MAX];

    if (!host_path(path, real, sizeof(real)))
        return false;
    host_mkdir_deep(real);
    return true;
}

static bool host_rmdir(const char *path)
{
    char real[HOST_FS_PATH_MAX];
#ifdef _WIN32
    return host_path(path, real, sizeof(real)) && RemoveDirectoryA(real);
#else
    (void)real;
    return false;
#endif
}

static bool host_finfo(const char *path, fs_file_info_t *info)
{
    char real[HOST_FS_PATH_MAX];
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!info || !host_path(path, real, sizeof(real)))
        return false;
    if (!GetFileAttributesExA(real, GetFileExInfoStandard, &data))
        return false;
    snprintf(info->full_name, sizeof(info->full_name), "%s", path);
    info->is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info->size = data.nFileSizeLow;
    info->timestamp = data.ftLastWriteTime.dwLowDateTime;
    return true;
#else
    (void)path;
    (void)info;
    return false;
#endif
}

bool host_fs_mount(const char *root)
{
    const char *use = (root && *root) ? root : ".\\nc-files";

    snprintf(g_root, sizeof(g_root), "%s", use);
    host_mkdir_deep(g_root);
    {
        char sub[HOST_FS_PATH_MAX];
        snprintf(sub, sizeof(sub), "%s\\nc\\files", g_root);
        host_mkdir_deep(sub);
    }

    g_drive.drive = 'D';
    g_drive.open = host_open;
    g_drive.read = host_read;
    g_drive.write = host_write;
    g_drive.seek = host_seek;
    g_drive.available = host_available;
    g_drive.close = host_close;
    g_drive.remove = host_remove;
    g_drive.opendir = host_opendir;
    g_drive.mkdir = host_mkdir;
    g_drive.rmdir = host_rmdir;
    g_drive.next_file = host_next_file;
    g_drive.finfo = host_finfo;
    g_drive.next = NULL;
    fs_mount(&g_drive);
    return true;
}
