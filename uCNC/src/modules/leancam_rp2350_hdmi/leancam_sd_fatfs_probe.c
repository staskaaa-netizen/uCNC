#include "../../cnc.h"

#ifdef LEANCAM_ENABLE_FATFS_SD_PROBE

#include "ff.h"
#include "f_util.h"
#include "hw_config.h"
#include "sd_card.h"

#ifdef ENABLE_LEANCAM_RP2350_SD
#include "../file_system.h"
#endif

#ifndef LEANCAM_SD_PROBE_DELAY_MS
#define LEANCAM_SD_PROBE_DELAY_MS 8000UL
#endif

#ifndef LEANCAM_SD_CAT_MAX_BYTES
#define LEANCAM_SD_CAT_MAX_BYTES 4096UL
#endif

static FATFS g_lc_sd_fs;
#ifdef ENABLE_LEANCAM_RP2350_SD
static fs_t g_lc_sd_drive;
#endif
static bool g_lc_sd_armed;
static bool g_lc_sd_done;
static bool g_lc_sd_mounted;
static uint32_t g_lc_sd_start_ms;

bool leancam_sd_ready(void)
{
    return g_lc_sd_mounted;
}

static bool lc_sd_make_fat_path(char *out, size_t out_len, const char *arg)
{
    const char *path = arg;

    if (!out || out_len == 0) {
        return false;
    }

    while (path && *path == ' ') {
        path++;
    }

    if (!path || !path[0]) {
        return snprintf(out, out_len, "0:/") < (int)out_len;
    }

    if (path[0] == '/') {
        return snprintf(out, out_len, "0:%s", path) < (int)out_len;
    }

    return snprintf(out, out_len, "0:/%s", path) < (int)out_len;
}

static void lc_sd_list_path(const char *arg)
{
    char path[160];
    DIR dir;
    FILINFO info;
    FRESULT fr;

    if (!g_lc_sd_mounted) {
        proto_info("LC_SD:not mounted");
        return;
    }

    if (!lc_sd_make_fat_path(path, sizeof(path), arg)) {
        proto_info("LC_SD:path too long");
        return;
    }

    fr = f_opendir(&dir, path);
    proto_info("LC_SD:ls %s fr=%d", path, (int)fr);
    if (fr != FR_OK) {
        return;
    }

    uint16_t count = 0;
    while (true) {
        fr = f_readdir(&dir, &info);
        if (fr != FR_OK || info.fname[0] == 0) {
            break;
        }
        if (info.fattrib & AM_DIR) {
            proto_info("LC_SD:dir %s", info.fname);
        } else {
            proto_info("LC_SD:file %s size=%lu", info.fname, (uint32_t)info.fsize);
        }
        count++;
    }
    f_closedir(&dir);
    proto_info("LC_SD:ls count=%u fr=%d", count, (int)fr);
}

static void lc_sd_cat_path(const char *arg)
{
    char path[160];
    FIL file;
    FRESULT fr;
    UINT br = 0;
    uint32_t total = 0;
    uint8_t buf[64];

    if (!g_lc_sd_mounted) {
        proto_info("LC_SD:not mounted");
        return;
    }

    if (!arg || !arg[0]) {
        proto_info("LC_SD:usage $SDCAT filename");
        return;
    }

    if (!lc_sd_make_fat_path(path, sizeof(path), arg)) {
        proto_info("LC_SD:path too long");
        return;
    }

    fr = f_open(&file, path, FA_READ);
    proto_info("LC_SD:cat %s fr=%d", path, (int)fr);
    if (fr != FR_OK) {
        return;
    }

    do {
        fr = f_read(&file, buf, sizeof(buf), &br);
        if (fr != FR_OK || br == 0) {
            break;
        }
        for (UINT i = 0; i < br; i++) {
            uint8_t c = buf[i];
            proto_putc((c == '\0') ? '.' : (char)c);
        }
        total += br;
    } while (br == sizeof(buf) && total < LEANCAM_SD_CAT_MAX_BYTES);

    f_close(&file);
    proto_print(MSG_EOL);
    proto_info("LC_SD:cat bytes=%lu", total);
}

static bool leancam_sd_cmd_parser(void *args)
{
    grbl_cmd_args_t *cmd = (grbl_cmd_args_t *)args;
    char params[160];

    if (!cmd || !cmd->cmd || !cmd->error) {
        return EVENT_CONTINUE;
    }

    if (!strcmp("SL", (char *)cmd->cmd) || !strcmp("SDLS", (char *)cmd->cmd)) {
        memset(params, 0, sizeof(params));
        if (cmd->next_char != EOL) {
            if (parser_get_grbl_cmd_arg(params, sizeof(params)) < 0) {
                *(cmd->error) = STATUS_INVALID_STATEMENT;
                return EVENT_HANDLED;
            }
        }
        lc_sd_list_path(params);
        *(cmd->error) = STATUS_OK;
        return EVENT_HANDLED;
    }

    if (!strcmp("SC", (char *)cmd->cmd) || !strcmp("SDCAT", (char *)cmd->cmd)) {
        memset(params, 0, sizeof(params));
        if (parser_get_grbl_cmd_arg(params, sizeof(params)) < 0) {
            *(cmd->error) = STATUS_INVALID_STATEMENT;
            return EVENT_HANDLED;
        }
        lc_sd_cat_path(params);
        *(cmd->error) = STATUS_OK;
        return EVENT_HANDLED;
    }

    return EVENT_CONTINUE;
}

#ifdef ENABLE_LEANCAM_RP2350_SD
static bool lc_sd_path(char *out, size_t out_len, const char *path)
{
    if (!out || !out_len || !path) {
        return false;
    }

    memset(out, 0, out_len);
    if (strcmp(path, "/") == 0 || path[0] == 0) {
        return snprintf(out, out_len, "0:/") < (int)out_len;
    }

    if (path[0] == '/') {
        return snprintf(out, out_len, "0:%s", path) < (int)out_len;
    }

    return snprintf(out, out_len, "0:/%s", path) < (int)out_len;
}

static bool lc_sd_finfo_from_fatfs(const char *ucnc_path, const FILINFO *info, fs_file_info_t *finfo)
{
    if (!ucnc_path || !info || !finfo) {
        return false;
    }

    memset(finfo, 0, sizeof(fs_file_info_t));
    strncpy(finfo->full_name, ucnc_path, FS_PATH_NAME_MAX_LEN - 1);
    finfo->is_dir = ((info->fattrib & AM_DIR) != 0);
    finfo->size = info->fsize;
    finfo->timestamp = ((uint32_t)info->fdate << 16) | info->ftime;
    return true;
}

static fs_file_t *lc_sd_open(const char *path, const char *mode)
{
    char fat_path[FS_PATH_NAME_MAX_LEN + 4];
    BYTE flags = 0;
    fs_file_t *fp = NULL;
    FIL *fil = NULL;

    if (!g_lc_sd_mounted || !lc_sd_path(fat_path, sizeof(fat_path), path)) {
        return NULL;
    }

    if (strchr(mode, 'r')) flags |= FA_READ;
    if (strchr(mode, 'w')) flags |= FA_CREATE_ALWAYS | FA_WRITE;
    if (strchr(mode, 'a')) flags |= FA_OPEN_APPEND | FA_WRITE;
    if (strchr(mode, '+')) flags |= FA_READ | FA_WRITE;
    if (strchr(mode, 'x')) flags |= FA_CREATE_NEW;

    fp = calloc(1, sizeof(fs_file_t));
    fil = calloc(1, sizeof(FIL));
    if (!fp || !fil) {
        free(fil);
        free(fp);
        return NULL;
    }

    if (f_open(fil, fat_path, flags) != FR_OK) {
        free(fil);
        free(fp);
        return NULL;
    }

    fp->file_ptr = fil;
    (void)g_lc_sd_drive.finfo(path, &fp->file_info);
    return fp;
}

static size_t lc_sd_read(fs_file_t *fp, uint8_t *buffer, size_t len)
{
    UINT br = 0;
    if (!fp || !fp->file_ptr || !buffer) {
        return 0;
    }
    return (f_read((FIL *)fp->file_ptr, buffer, (UINT)len, &br) == FR_OK) ? br : 0;
}

static size_t lc_sd_write(fs_file_t *fp, const uint8_t *buffer, size_t len)
{
    UINT bw = 0;
    if (!fp || !fp->file_ptr || !buffer) {
        return 0;
    }
    if (f_write((FIL *)fp->file_ptr, buffer, (UINT)len, &bw) != FR_OK) {
        return 0;
    }
    (void)f_sync((FIL *)fp->file_ptr);
    return bw;
}

static bool lc_sd_seek(fs_file_t *fp, uint32_t position)
{
    return fp && fp->file_ptr && f_lseek((FIL *)fp->file_ptr, position) == FR_OK;
}

static int lc_sd_available(fs_file_t *fp)
{
    FIL *fil;
    if (!fp || !fp->file_ptr) {
        return 0;
    }
    fil = (FIL *)fp->file_ptr;
    return (int)(f_size(fil) - f_tell(fil));
}

static void lc_sd_close(fs_file_t *fp)
{
    if (!fp || !fp->file_ptr) {
        return;
    }
    if (fp->file_info.is_dir) {
        (void)f_closedir((DIR *)fp->file_ptr);
    } else {
        (void)f_close((FIL *)fp->file_ptr);
    }
}

static bool lc_sd_remove(const char *path)
{
    char fat_path[FS_PATH_NAME_MAX_LEN + 4];
    return g_lc_sd_mounted && lc_sd_path(fat_path, sizeof(fat_path), path) && f_unlink(fat_path) == FR_OK;
}

static fs_file_t *lc_sd_opendir(const char *path)
{
    char fat_path[FS_PATH_NAME_MAX_LEN + 4];
    fs_file_t *fp = NULL;
    DIR *dir = NULL;

    if (!g_lc_sd_mounted || !lc_sd_path(fat_path, sizeof(fat_path), path)) {
        return NULL;
    }

    fp = calloc(1, sizeof(fs_file_t));
    dir = calloc(1, sizeof(DIR));
    if (!fp || !dir) {
        free(dir);
        free(fp);
        return NULL;
    }

    if (f_opendir(dir, fat_path) != FR_OK) {
        free(dir);
        free(fp);
        return NULL;
    }

    fp->file_ptr = dir;
    strncpy(fp->file_info.full_name, path, FS_PATH_NAME_MAX_LEN - 1);
    fp->file_info.is_dir = true;
    return fp;
}

static bool lc_sd_mkdir(const char *path)
{
    char fat_path[FS_PATH_NAME_MAX_LEN + 4];
    return g_lc_sd_mounted && lc_sd_path(fat_path, sizeof(fat_path), path) && f_mkdir(fat_path) == FR_OK;
}

static bool lc_sd_rmdir(const char *path)
{
    return lc_sd_remove(path);
}

static bool lc_sd_next_file(fs_file_t *fp, fs_file_info_t *finfo)
{
    FILINFO info;
    FRESULT fr;
    if (!fp || !fp->file_ptr || !finfo) {
        return false;
    }

    fr = f_readdir((DIR *)fp->file_ptr, &info);
    if (fr != FR_OK || info.fname[0] == 0) {
        return false;
    }

    memset(finfo, 0, sizeof(fs_file_info_t));
    if (strcmp(fp->file_info.full_name, "/") == 0) {
        snprintf(finfo->full_name, sizeof(finfo->full_name), "/%s", info.fname);
    } else {
        snprintf(finfo->full_name, sizeof(finfo->full_name), "%s/%s", fp->file_info.full_name, info.fname);
    }
    finfo->is_dir = ((info.fattrib & AM_DIR) != 0);
    finfo->size = info.fsize;
    finfo->timestamp = ((uint32_t)info.fdate << 16) | info.ftime;
    return true;
}

static bool lc_sd_finfo(const char *path, fs_file_info_t *finfo)
{
    char fat_path[FS_PATH_NAME_MAX_LEN + 4];
    FILINFO info;

    if (!g_lc_sd_mounted || !finfo || !lc_sd_path(fat_path, sizeof(fat_path), path)) {
        return false;
    }

    if (strcmp(path, "/") == 0 || path[0] == 0) {
        memset(finfo, 0, sizeof(fs_file_info_t));
        strcpy(finfo->full_name, "/");
        finfo->is_dir = true;
        return true;
    }

    if (f_stat(fat_path, &info) != FR_OK) {
        return false;
    }

    return lc_sd_finfo_from_fatfs(path, &info, finfo);
}

static void lc_sd_mount_ucnc_drive(void)
{
    memset(&g_lc_sd_drive, 0, sizeof(g_lc_sd_drive));
    g_lc_sd_drive.drive = 'S';
    g_lc_sd_drive.open = lc_sd_open;
    g_lc_sd_drive.read = lc_sd_read;
    g_lc_sd_drive.write = lc_sd_write;
    g_lc_sd_drive.seek = lc_sd_seek;
    g_lc_sd_drive.available = lc_sd_available;
    g_lc_sd_drive.close = lc_sd_close;
    g_lc_sd_drive.remove = lc_sd_remove;
    g_lc_sd_drive.opendir = lc_sd_opendir;
    g_lc_sd_drive.mkdir = lc_sd_mkdir;
    g_lc_sd_drive.rmdir = lc_sd_rmdir;
    g_lc_sd_drive.next_file = lc_sd_next_file;
    g_lc_sd_drive.finfo = lc_sd_finfo;
    g_lc_sd_drive.next = NULL;
    fs_mount(&g_lc_sd_drive);
}
#endif

static bool leancam_sd_fatfs_probe_update(void *args)
{
    (void)args;

    if (g_lc_sd_done) {
        return EVENT_CONTINUE;
    }

    if (!g_lc_sd_armed) {
        g_lc_sd_armed = true;
        g_lc_sd_start_ms = mcu_millis();
        proto_info("LC_SD:FatFs probe armed delay=%lu", (uint32_t)LEANCAM_SD_PROBE_DELAY_MS);
        return EVENT_CONTINUE;
    }

    if ((uint32_t)(mcu_millis() - g_lc_sd_start_ms) < LEANCAM_SD_PROBE_DELAY_MS) {
        return EVENT_CONTINUE;
    }

    g_lc_sd_done = true;
    proto_info("LC_SD:FatFs init spi1 sck=30 mosi=31 miso=40 cs=43");

    sd_init_driver();
    sd_card_t *card = sd_get_by_num(0);
    if (!card) {
        proto_info("LC_SD:no card config");
        return EVENT_CONTINUE;
    }

    const char *drive = sd_get_drive_prefix(card);
    FRESULT fr = f_mount(&g_lc_sd_fs, drive, 1);
    proto_info("LC_SD:f_mount drive=%s fr=%d", drive, (int)fr);
    if (fr != FR_OK) {
        return EVENT_CONTINUE;
    }
    g_lc_sd_mounted = true;
#ifdef ENABLE_LEANCAM_RP2350_SD
    lc_sd_mount_ucnc_drive();
    proto_info("LC_SD:uCNC drive mounted as /S");
#endif

    char root_path[12];
    snprintf(root_path, sizeof(root_path), "%s/", drive);

    DIR dir;
    FILINFO info;
    fr = f_opendir(&dir, root_path);
    proto_info("LC_SD:f_opendir %s fr=%d", root_path, (int)fr);
    if (fr != FR_OK) {
        return EVENT_CONTINUE;
    }

    uint8_t shown = 0;
    while (shown < 3) {
        fr = f_readdir(&dir, &info);
        if (fr != FR_OK || info.fname[0] == 0) {
            break;
        }
        proto_info("LC_SD:entry %s%s", info.fname, (info.fattrib & AM_DIR) ? "/" : "");
        shown++;
    }
    f_closedir(&dir);
    return EVENT_CONTINUE;
}

CREATE_EVENT_LISTENER(cnc_dotasks, leancam_sd_fatfs_probe_update);
CREATE_EVENT_LISTENER(grbl_cmd, leancam_sd_cmd_parser);

DECL_MODULE(leancam_sd_fatfs_probe)
{
    ADD_EVENT_LISTENER(cnc_dotasks, leancam_sd_fatfs_probe_update);
    ADD_EVENT_LISTENER(grbl_cmd, leancam_sd_cmd_parser);
}

#else

DECL_MODULE(leancam_sd_fatfs_probe)
{
}

#endif
