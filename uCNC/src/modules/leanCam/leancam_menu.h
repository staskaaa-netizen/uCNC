#ifndef LEANCAM_MENU_H
#define LEANCAM_MENU_H

#include <stdbool.h>
#include <stddef.h>
#include "../ui_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    LC_MENU_MODE_FILES = 0,
    LC_MENU_MODE_FILE_NAME,
    LC_MENU_MODE_PROGRAM,
    LC_MENU_MODE_DRAFT,
    LC_MENU_MODE_NC_VIEW
} lc_menu_mode_t;

typedef enum
{
    LC_MENU_CATALOG_NONE = 0,
    LC_MENU_CATALOG_TOOLS
} lc_menu_catalog_kind_t;

typedef enum
{
    LC_MENU_TEMPLATE_TOOLCALL = 0,
    LC_MENU_TEMPLATE_PROCESSCALL,
    LC_MENU_TEMPLATE_OD,
    LC_MENU_TEMPLATE_ID,
    LC_MENU_TEMPLATE_FACE,
    LC_MENU_TEMPLATE_RECESS,
    LC_MENU_TEMPLATE_L,
    LC_MENU_TEMPLATE_C,
    LC_MENU_TEMPLATE_DRILL,
    LC_MENU_TEMPLATE_TAP,
    LC_MENU_TEMPLATE_THREAD,
    LC_MENU_TEMPLATE_END
} lc_menu_template_t;

typedef struct
{
    lc_menu_mode_t (*get_mode)(void *user);
    lc_menu_catalog_kind_t (*get_catalog)(void *user);
    void (*set_mode)(void *user, lc_menu_mode_t mode);
    void (*set_catalog)(void *user, lc_menu_catalog_kind_t catalog);
    void (*set_message)(void *user, const char *message);

    int (*file_count)(void *user);
    int (*file_selected)(void *user);
    void (*file_set_selected)(void *user, int selected);
    void (*files_refresh)(void *user);
    void (*files_delete_selected)(void *user);
    void (*files_duplicate_selected)(void *user);
    void (*files_prepare_run)(void *user);
    void (*files_open_selected)(void *user);

    void (*filename_clear)(void *user);
    size_t (*filename_len)(void *user);
    void (*filename_backspace)(void *user);
    void (*filename_append_digit)(void *user, char digit);
    void (*filename_finish)(void *user);

    void (*catalog_open)(void *user, lc_menu_catalog_kind_t catalog);
    void (*catalog_new_entry)(void *user);
    void (*catalog_duplicate_entry)(void *user);

    void (*program_move_prev)(void *user);
    void (*program_move_next)(void *user);
    bool (*program_begin_edit)(void *user, bool asset);
    void (*program_delete_current)(void *user);
    void (*program_run_selected)(void *user);
    void (*program_begin_template)(void *user, lc_menu_template_t tmpl);
    void (*program_back_to_files)(void *user);

    unsigned (*draft_field_count)(void *user);
    unsigned (*draft_field_index)(void *user);
    bool (*draft_has_input)(void *user);
    void (*draft_cancel)(void *user);
    bool (*draft_accept_field)(void *user);
    bool (*draft_commit)(void *user);
    void (*draft_backspace)(void *user);
    void (*draft_toggle_sign)(void *user);
    void (*draft_add_dot)(void *user);
    void (*draft_input_digit)(void *user, char digit);

    void (*nc_back_to_files)(void *user);
    void (*nc_scroll_prev)(void *user);
    void (*nc_scroll_next)(void *user);
    void (*nc_select_single)(void *user);
    void (*nc_select_from)(void *user);
    void (*nc_select_full)(void *user);
    void (*nc_run_selected_mode)(void *user);
} lc_menu_actions_t;

bool leancam_menu_handle_key(void *user, const lc_menu_actions_t *actions, ui_key_t key);
void leancam_menu_set_program_other_templates(bool enabled);
bool leancam_menu_program_other_templates(void);
void leancam_menu_copy_title(char *out,
                             size_t out_size,
                             lc_menu_mode_t mode,
                             lc_menu_catalog_kind_t catalog);
void leancam_menu_copy_footer(char *out,
                              size_t out_size,
                              lc_menu_mode_t mode,
                              lc_menu_catalog_kind_t catalog,
                              bool draft_active,
                              unsigned draft_field_index,
                              unsigned draft_field_count,
                              const char *draft_input);

#ifdef __cplusplus
}
#endif

#endif

