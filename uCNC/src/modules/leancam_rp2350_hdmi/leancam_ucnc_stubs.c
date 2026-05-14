#include "../system_menu.h"
#include "../file_system.h"

system_menu_t g_system_menu = {
    .flags = SYSTEM_MENU_MODE_NONE,
    .current_menu = SYSTEM_MENU_ID_IDLE,
    .current_index = 0,
    .current_multiplier = 0,
    .total_items = 0,
    .menu_entry = 0,
    .action_timeout = 0
};

void system_menu_init(void)
{
}

void system_menu_append(system_menu_page_t *newpage)
{
    (void)newpage;
}

void system_menu_append_item(uint8_t menu_id, system_menu_index_t *newitem)
{
    (void)menu_id;
    (void)newitem;
}

void system_menu_go_idle(void)
{
}

void system_menu_show_modal_popup(uint32_t timeout, const char *__s)
{
    (void)timeout;
    (void)__s;
}

void system_menu_render_header(const char *__s)
{
    (void)__s;
}

bool system_menu_render_menu_item_filter(uint8_t item_index)
{
    (void)item_index;
    return false;
}

void system_menu_render_nav_back(bool is_hover)
{
    (void)is_hover;
}

void system_menu_render_footer(void)
{
}

void system_menu_item_render_label(uint8_t render_flags, const char *label)
{
    (void)render_flags;
    (void)label;
}

void system_menu_item_render_arg(uint8_t render_flags, const char *label)
{
    (void)render_flags;
    (void)label;
}

#ifndef ENABLE_LEANCAM_RP2350_SD
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
