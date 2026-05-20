#include "FS.h"
#include <LittleFS.h>
#include <new>

extern "C" {
#include "src/cnc.h"
#include "src/modules/file_system.h"
}

#ifndef RP2350_FLASH_FS_DRIVE
#define RP2350_FLASH_FS_DRIVE 'I'
#endif

static fs_t g_rp2350_flash_fs;

static File *flash_file(fs_file_t *fp)
{
    return fp ? static_cast<File *>(fp->file_ptr) : nullptr;
}

static void flash_fill_info(fs_file_info_t *info, File &file)
{
    if (!info) {
        return;
    }
    memset(info, 0, sizeof(*info));
    strncpy(info->full_name, file.name(), FS_PATH_NAME_MAX_LEN - 1);
    info->is_dir = file.isDirectory();
    info->size = file.size();
    info->timestamp = (uint32_t)file.getLastWrite();
}

static fs_file_t *flash_open_file(const char *path, const char *mode)
{
    fs_file_t *fp = (fs_file_t *)calloc(1, sizeof(fs_file_t));
    if (!fp) {
        return nullptr;
    }

    void *file_mem = calloc(1, sizeof(File));
    if (!file_mem) {
        fs_safe_free(fp);
        return nullptr;
    }

    File *file = new (file_mem) File(LittleFS.open(path, mode));
    if (!file || !(*file)) {
        if (file) {
            file->~File();
        }
        fs_safe_free(file_mem);
        fs_safe_free(fp);
        return nullptr;
    }

    fp->file_ptr = file;
    fp->fs_ptr = &g_rp2350_flash_fs;
    flash_fill_info(&fp->file_info, *file);
    return fp;
}

static size_t flash_read(fs_file_t *fp, uint8_t *buffer, size_t len)
{
    File *file = flash_file(fp);
    return file ? file->read(buffer, len) : 0;
}

static size_t flash_write(fs_file_t *fp, const uint8_t *buffer, size_t len)
{
    File *file = flash_file(fp);
    return file ? file->write(buffer, len) : 0;
}

static bool flash_seek(fs_file_t *fp, uint32_t position)
{
    File *file = flash_file(fp);
    return file ? file->seek(position) : false;
}

static int flash_available(fs_file_t *fp)
{
    File *file = flash_file(fp);
    return file ? file->available() : 0;
}

static void flash_close(fs_file_t *fp)
{
    File *file = flash_file(fp);
    if (file) {
        file->flush();
        file->close();
        file->~File();
        fs_safe_free(file);
        fp->file_ptr = nullptr;
    }
}

static bool flash_remove(const char *path)
{
    return LittleFS.remove(path);
}

static fs_file_t *flash_opendir(const char *path)
{
    return flash_open_file(path, "r");
}

static bool flash_mkdir(const char *path)
{
    return LittleFS.mkdir(path);
}

static bool flash_rmdir(const char *path)
{
    return LittleFS.rmdir(path);
}

static bool flash_next_file(fs_file_t *fp, fs_file_info_t *info)
{
    File *dir = flash_file(fp);
    if (!dir || !info) {
        return false;
    }

    File file = dir->openNextFile();
    if (!file) {
        return false;
    }

    flash_fill_info(info, file);
    file.close();
    return true;
}

static bool flash_info(const char *path, fs_file_info_t *info)
{
    File file = LittleFS.open(path, "r");
    if (!file) {
        return false;
    }
    flash_fill_info(info, file);
    file.close();
    return true;
}

extern "C" void rp2350_flash_fs_init(void)
{
#ifdef ENABLE_UCNC_FILE_SYSTEM
    if (!LittleFS.begin()) {
        proto_info("MSG:Internal flash FS error");
        return;
    }

    g_rp2350_flash_fs = {
        .drive = RP2350_FLASH_FS_DRIVE,
        .open = flash_open_file,
        .read = flash_read,
        .write = flash_write,
        .seek = flash_seek,
        .available = flash_available,
        .close = flash_close,
        .remove = flash_remove,
        .opendir = flash_opendir,
        .mkdir = flash_mkdir,
        .rmdir = flash_rmdir,
        .next_file = flash_next_file,
        .finfo = flash_info,
        .next = NULL};
    fs_mount(&g_rp2350_flash_fs);
    proto_info("MSG:Internal flash FS mounted /%c", RP2350_FLASH_FS_DRIVE);
#endif
}
