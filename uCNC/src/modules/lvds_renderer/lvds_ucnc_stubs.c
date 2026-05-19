#include "../file_system.h"

#ifndef ENABLE_UCNC_FILE_SYSTEM
bool fs_file_run_active(void)
{
    return false;
}

fs_file_t *fs_open(const char *path, const char *mode)
{
    (void)path;
    (void)mode;
    return NULL;
}

fs_file_t *fs_opendir(const char *path)
{
    (void)path;
    return NULL;
}

bool fs_next_file(fs_file_t *fp, fs_file_info_t *finfo)
{
    (void)fp;
    (void)finfo;
    return false;
}

size_t fs_read(fs_file_t *fp, uint8_t *buffer, size_t len)
{
    (void)fp;
    (void)buffer;
    (void)len;
    return 0;
}

size_t fs_write(fs_file_t *fp, const uint8_t *buffer, size_t len)
{
    (void)fp;
    (void)buffer;
    (void)len;
    return 0;
}

int fs_available(fs_file_t *fp)
{
    (void)fp;
    return 0;
}

void fs_close(fs_file_t *fp)
{
    (void)fp;
}

bool fs_remove(const char *path)
{
    (void)path;
    return false;
}

void fs_file_run(char *params)
{
    (void)params;
}
#endif
