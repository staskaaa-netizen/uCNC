#include "leancam_menu.h"
#include "leancam_schema.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static bool g_program_other_templates;

void leancam_menu_set_program_other_templates(bool enabled)
{
    g_program_other_templates = enabled;
}

bool leancam_menu_program_other_templates(void)
{
    return g_program_other_templates;
}

static bool lc_menu_is_digit(ui_key_t key)
{
    return key >= UI_KEY_DIGIT_0 && key <= UI_KEY_DIGIT_9;
}

static char lc_menu_digit_char(ui_key_t key)
{
    return (char)('0' + (key - UI_KEY_DIGIT_0));
}

static void lc_menu_start_filename(void *user, const lc_menu_actions_t *a)
{
    if (!a) return;
    if (a->filename_clear) a->filename_clear(user);
    if (a->set_mode) a->set_mode(user, LC_MENU_MODE_FILE_NAME);
    if (a->set_message) a->set_message(user, "LC: new name");
}

static bool lc_menu_handle_files(void *user, const lc_menu_actions_t *a, ui_key_t key)
{
    const lc_schema_menu_item_t *item;
    int cnt;
    int sel;

    if (!a) return false;
    cnt = a->file_count ? a->file_count(user) : 0;
    sel = a->file_selected ? a->file_selected(user) : 0;

    if (key == UI_KEY_PREV) {
        if (sel > 0 && a->file_set_selected) a->file_set_selected(user, sel - 1);
        return true;
    }
    if (key == UI_KEY_NEXT) {
        if (sel < cnt && a->file_set_selected) a->file_set_selected(user, sel + 1);
        return true;
    }
    if (key == UI_KEY_ACCEPT) {
        if (sel == cnt) {
            lc_menu_start_filename(user, a);
        } else if (a->files_open_selected) {
            a->files_open_selected(user);
        }
        return true;
    }
    if (key == UI_KEY_CANCEL) {
        if (a->files_refresh) a->files_refresh(user);
        return true;
    }
    if (key == UI_KEY_FINISH) {
        if (a->files_prepare_run) a->files_prepare_run(user);
        return true;
    }
    if (key == UI_KEY_BACKSPACE) {
        if (a->files_delete_selected) a->files_delete_selected(user);
        return true;
    }
    if (!lc_menu_is_digit(key)) {
        return false;
    }

    item = leancam_schema_find_key_for(LC_MENU_MODE_FILES,
                                       LC_MENU_CATALOG_NONE,
                                       lc_menu_digit_char(key));
    if (!item) {
        return false;
    }

    switch (item->action)
    {
        case LC_SCHEMA_ACT_OPEN_CATALOG:
            if (a->catalog_open) a->catalog_open(user, (lc_menu_catalog_kind_t)item->value);
            return true;

        case LC_SCHEMA_ACT_FILE_OPEN:
            if (sel == cnt) {
                lc_menu_start_filename(user, a);
            } else if (a->files_open_selected) {
                a->files_open_selected(user);
            }
            return true;

        case LC_SCHEMA_ACT_FILE_NEW:
            lc_menu_start_filename(user, a);
            return true;

        case LC_SCHEMA_ACT_FILE_DELETE:
            if (a->files_delete_selected) a->files_delete_selected(user);
            return true;

        case LC_SCHEMA_ACT_FILE_DUPLICATE:
            if (a->files_duplicate_selected) a->files_duplicate_selected(user);
            return true;

        case LC_SCHEMA_ACT_FILE_REFRESH:
            if (a->files_refresh) a->files_refresh(user);
            return true;

        case LC_SCHEMA_ACT_FILE_PREPARE:
            if (a->files_prepare_run) a->files_prepare_run(user);
            return true;

        default:
            return false;
    }

}

static bool lc_menu_handle_filename(void *user, const lc_menu_actions_t *a, ui_key_t key)
{
    const lc_schema_menu_item_t *item;
    char keych;

    if (!a) return false;

    if (key == UI_KEY_CANCEL) {
        keych = 'A';
    } else if (key == UI_KEY_BACKSPACE) {
        keych = '*';
    } else if (key == UI_KEY_ACCEPT) {
        keych = 'D';
    } else if (key == UI_KEY_FINISH) {
        keych = '#';
    } else if (lc_menu_is_digit(key)) {
        keych = '0';
    } else {
        return false;
    }

    item = leancam_schema_find_key_for(LC_MENU_MODE_FILE_NAME, LC_MENU_CATALOG_NONE, keych);
    if (!item) {
        return false;
    }

    switch (item->action)
    {
        case LC_SCHEMA_ACT_NAME_DIGIT:
            if (lc_menu_is_digit(key) && a->filename_append_digit) {
                a->filename_append_digit(user, lc_menu_digit_char(key));
            }
            return true;

        case LC_SCHEMA_ACT_NAME_BACK:
            if (a->filename_backspace) a->filename_backspace(user);
            return true;

        case LC_SCHEMA_ACT_NAME_CREATE:
            if (a->filename_finish) a->filename_finish(user);
            return true;

        case LC_SCHEMA_ACT_NAME_CANCEL:
            if (a->set_mode) a->set_mode(user, LC_MENU_MODE_FILES);
            if (a->set_message) a->set_message(user, "LC: cancel new");
            return true;

        default:
            return false;
    }
}

static bool lc_menu_handle_catalog_program(void *user, const lc_menu_actions_t *a, ui_key_t key)
{
    const lc_schema_menu_item_t *item;

    if (!a) return false;

    if (key == UI_KEY_CANCEL) {
        if (a->program_back_to_files) a->program_back_to_files(user);
        return true;
    }
    if (key == UI_KEY_PREV) {
        if (a->program_move_prev) a->program_move_prev(user);
        return true;
    }
    if (key == UI_KEY_NEXT) {
        if (a->program_move_next) a->program_move_next(user);
        return true;
    }
    if (key == UI_KEY_ACCEPT) {
        if (a->program_begin_edit && !a->program_begin_edit(user, true) && a->set_message) {
            a->set_message(user, "LC: nothing to edit");
        }
        return true;
    }
    if (key == UI_KEY_FINISH) {
        return true;
    }
    if (key == UI_KEY_BACKSPACE) {
        if (a->program_delete_current) a->program_delete_current(user);
        return true;
    }
    if (!lc_menu_is_digit(key)) {
        return false;
    }

    item = leancam_schema_find_key_for(LC_MENU_MODE_PROGRAM,
                                       LC_MENU_CATALOG_TOOLS,
                                       lc_menu_digit_char(key));
    if (!item) {
        return false;
    }

    switch (item->action)
    {
        case LC_SCHEMA_ACT_CATALOG_NEW:
            if (a->catalog_new_entry) a->catalog_new_entry(user);
            return true;

        case LC_SCHEMA_ACT_OPEN_CATALOG:
            if (a->catalog_open) a->catalog_open(user, (lc_menu_catalog_kind_t)item->value);
            return true;

        case LC_SCHEMA_ACT_PROGRAM_EDIT:
            if (a->program_begin_edit && !a->program_begin_edit(user, true) && a->set_message) {
                a->set_message(user, "LC: nothing to edit");
            }
            return true;

        case LC_SCHEMA_ACT_PROGRAM_COPY:
            if (a->catalog_duplicate_entry) a->catalog_duplicate_entry(user);
            return true;

        case LC_SCHEMA_ACT_PROGRAM_DELETE:
            if (a->program_delete_current) a->program_delete_current(user);
            return true;

        case LC_SCHEMA_ACT_PROGRAM_BACK:
            if (a->program_back_to_files) a->program_back_to_files(user);
            return true;

        default:
            return false;
    }
}

static bool lc_menu_handle_program(void *user, const lc_menu_actions_t *a, ui_key_t key)
{
    const lc_schema_menu_item_t *item;
    char keych;
    lc_schema_page_id_t page_id;

    if (!a) return false;

    if (a->get_catalog && a->get_catalog(user) != LC_MENU_CATALOG_NONE) {
        return lc_menu_handle_catalog_program(user, a, key);
    }

    if (key == UI_KEY_PREV) {
        if (a->program_move_prev) a->program_move_prev(user);
        return true;
    }
    if (key == UI_KEY_NEXT) {
        if (a->program_move_next) a->program_move_next(user);
        return true;
    }
    if (key == UI_KEY_CANCEL) {
        keych = 'A';
    } else if (key == UI_KEY_ACCEPT) {
        keych = 'D';
    } else if (key == UI_KEY_FINISH) {
        keych = '#';
    } else if (key == UI_KEY_BACKSPACE) {
        keych = '*';
    } else if (lc_menu_is_digit(key)) {
        keych = lc_menu_digit_char(key);
    } else {
        return false;
    }

    page_id = g_program_other_templates ? LC_SCHEMA_PAGE_PROGRAM_OTHER : LC_SCHEMA_PAGE_PROGRAM;
    item = leancam_schema_find_key(leancam_schema_page(page_id), keych);
    if (!item) {
        return false;
    }

    switch (item->action)
    {
        case LC_SCHEMA_ACT_TEMPLATE_MORE:
            g_program_other_templates = !g_program_other_templates;
            if (a->set_message) {
                a->set_message(user, g_program_other_templates ? "LC: other cycles" : "LC: main cycles");
            }
            return true;

        case LC_SCHEMA_ACT_TEMPLATE:
            if (a->program_begin_template) a->program_begin_template(user, (lc_menu_template_t)item->value);
            return true;

        case LC_SCHEMA_ACT_PROGRAM_EDIT:
            if (a->program_begin_edit && !a->program_begin_edit(user, false) && a->set_message) {
                a->set_message(user, "LC: nothing to edit");
            }
            return true;

        case LC_SCHEMA_ACT_PROGRAM_RUN:
            if (a->program_run_selected) a->program_run_selected(user);
            return true;

        case LC_SCHEMA_ACT_PROGRAM_DELETE:
            if (a->program_delete_current) a->program_delete_current(user);
            return true;

        case LC_SCHEMA_ACT_PROGRAM_BACK:
            if (a->program_back_to_files) a->program_back_to_files(user);
            return true;

        default:
            return false;
    }
}

static bool lc_menu_handle_draft(void *user, const lc_menu_actions_t *a, ui_key_t key)
{
    const lc_schema_menu_item_t *item;
    char keych;

    if (!a) return false;

    if (key == UI_KEY_CANCEL) {
        keych = 'A';
    } else if (key == UI_KEY_ACCEPT) {
        keych = 'D';
    } else if (key == UI_KEY_BACKSPACE) {
        keych = '*';
    } else if (key == UI_KEY_FINISH) {
        keych = '#';
    } else if (key == UI_KEY_PREV) {
        keych = 'B';
    } else if (key == UI_KEY_NEXT) {
        keych = 'C';
    } else if (lc_menu_is_digit(key)) {
        keych = 'I';
    } else {
        return false;
    }

    item = leancam_schema_find_key_for(LC_MENU_MODE_DRAFT, LC_MENU_CATALOG_NONE, keych);
    if (!item) {
        return false;
    }

    switch (item->action)
    {
        case LC_SCHEMA_ACT_DRAFT_CANCEL:
            if (a->draft_cancel) a->draft_cancel(user);
            return true;

        case LC_SCHEMA_ACT_DRAFT_NEXT:
            if (a->draft_field_index && a->draft_field_count &&
                a->draft_field_index(user) >= a->draft_field_count(user)) {
                if (a->draft_commit) (void)a->draft_commit(user);
                return true;
            }
            if (a->draft_accept_field && !a->draft_accept_field(user)) {
                if (a->set_message) a->set_message(user, "LC: no active field");
                return true;
            }
            if (a->draft_field_index && a->draft_field_count &&
                a->draft_field_index(user) >= a->draft_field_count(user)) {
                if (a->draft_commit) (void)a->draft_commit(user);
            } else if (a->set_message) {
                a->set_message(user, "LC: field accepted");
            }
            return true;

        case LC_SCHEMA_ACT_DRAFT_BACK:
            if (a->draft_has_input && a->draft_has_input(user)) {
                if (a->draft_backspace) a->draft_backspace(user);
            } else if (a->draft_cancel) {
                a->draft_cancel(user);
            }
            return true;

        case LC_SCHEMA_ACT_DRAFT_SAVE:
            if (a->draft_commit) (void)a->draft_commit(user);
            return true;

        case LC_SCHEMA_ACT_DRAFT_SIGN:
            if (a->draft_toggle_sign) a->draft_toggle_sign(user);
            return true;

        case LC_SCHEMA_ACT_DRAFT_DOT:
            if (a->draft_add_dot) a->draft_add_dot(user);
            return true;

        case LC_SCHEMA_ACT_DRAFT_DIGIT:
            if (lc_menu_is_digit(key)) {
                if (a->draft_input_digit) a->draft_input_digit(user, lc_menu_digit_char(key));
                return true;
            }
            return false;

        default:
            return false;
    }
}

static bool lc_menu_handle_nc(void *user, const lc_menu_actions_t *a, ui_key_t key)
{
    const lc_schema_menu_item_t *item;
    char keych;

    if (!a) return false;

    if (key == UI_KEY_CANCEL) {
        keych = 'A';
    } else if (key == UI_KEY_PREV) {
        keych = 'B';
    } else if (key == UI_KEY_NEXT) {
        keych = 'C';
    } else if (key == UI_KEY_FINISH) {
        keych = '#';
    } else {
        return false;
    }

    item = leancam_schema_find_key_for(LC_MENU_MODE_NC_VIEW, LC_MENU_CATALOG_NONE, keych);
    if (!item) {
        return false;
    }

    switch (item->action)
    {
        case LC_SCHEMA_ACT_NC_UP:
            if (a->nc_scroll_prev) a->nc_scroll_prev(user);
            return true;

        case LC_SCHEMA_ACT_NC_DOWN:
            if (a->nc_scroll_next) a->nc_scroll_next(user);
            return true;

        case LC_SCHEMA_ACT_NC_BACK:
            if (a->nc_back_to_files) a->nc_back_to_files(user);
            return true;

        case LC_SCHEMA_ACT_NC_VIEW:
            if (a->set_message) a->set_message(user, "LC: nc view only");
            return true;

        default:
            return false;
    }
}

bool leancam_menu_handle_key(void *user, const lc_menu_actions_t *actions, ui_key_t key)
{
    lc_menu_mode_t mode;

    if (!actions || !actions->get_mode || key == UI_KEY_NONE) {
        return false;
    }

    mode = actions->get_mode(user);
    switch (mode)
    {
        case LC_MENU_MODE_FILES:
            return lc_menu_handle_files(user, actions, key);
        case LC_MENU_MODE_FILE_NAME:
            return lc_menu_handle_filename(user, actions, key);
        case LC_MENU_MODE_PROGRAM:
            return lc_menu_handle_program(user, actions, key);
        case LC_MENU_MODE_DRAFT:
            return lc_menu_handle_draft(user, actions, key);
        case LC_MENU_MODE_NC_VIEW:
            return lc_menu_handle_nc(user, actions, key);
        default:
            return false;
    }
}

static void lc_menu_copy(char *out, size_t out_size, const char *text)
{
    if (!out || out_size == 0) {
        return;
    }
    if (!text) {
        text = "";
    }
    snprintf(out, out_size, "%s", text);
}

void leancam_menu_copy_title(char *out,
                             size_t out_size,
                             lc_menu_mode_t mode,
                             lc_menu_catalog_kind_t catalog)
{
    const char *title = "LeanCam";
    const lc_schema_page_t *page = leancam_schema_page(leancam_schema_page_for(mode, catalog));

    switch (mode)
    {
        case LC_MENU_MODE_FILES:
        case LC_MENU_MODE_FILE_NAME:
        case LC_MENU_MODE_NC_VIEW:
            leancam_schema_format_title(out, out_size, page);
            return;

        case LC_MENU_MODE_PROGRAM:
            if (catalog == LC_MENU_CATALOG_TOOLS)
                title = "Tool Catalog";
            else {
                leancam_schema_format_title(out, out_size, page);
                return;
            }
            break;

        case LC_MENU_MODE_DRAFT:
            if (catalog == LC_MENU_CATALOG_TOOLS)
                title = mode == LC_MENU_MODE_DRAFT ? "Tool Glyph Editor" : "Tool Catalog";
            else
                leancam_schema_format_title(out, out_size, page);
            break;

        default:
            break;
    }

    lc_menu_copy(out, out_size, title);
}

void leancam_menu_copy_footer(char *out,
                              size_t out_size,
                              lc_menu_mode_t mode,
                              lc_menu_catalog_kind_t catalog,
                              bool draft_active,
                              unsigned draft_field_index,
                              unsigned draft_field_count,
                              const char *draft_input)
{
    const lc_schema_page_t *page;
    (void)draft_input;

    if (!out || out_size == 0) {
        return;
    }

    if (draft_active) {
        if (draft_field_index < draft_field_count) {
            snprintf(out,
                     out_size,
                     "F %u/%u|0-9 Value|B +/-|C .|D Next|# Save|* Back|A Cancel",
                     draft_field_index + 1u,
                     draft_field_count);
        } else {
            lc_menu_copy(out, out_size, "F Done|# Save|* Back|A Cancel");
        }
        return;
    }

    switch (mode)
    {
        case LC_MENU_MODE_FILES:
            page = leancam_schema_page(LC_SCHEMA_PAGE_FILES);
            leancam_schema_format_footer(out, out_size, page);
            break;

        case LC_MENU_MODE_FILE_NAME:
            leancam_schema_format_footer(out, out_size, leancam_schema_page(LC_SCHEMA_PAGE_FILE_NAME));
            break;

        case LC_MENU_MODE_PROGRAM:
            leancam_schema_format_footer(out,
                                         out_size,
                                         leancam_schema_page(catalog == LC_MENU_CATALOG_NONE ?
                                                            (g_program_other_templates ? LC_SCHEMA_PAGE_PROGRAM_OTHER : LC_SCHEMA_PAGE_PROGRAM) :
                                                            LC_SCHEMA_PAGE_CATALOG));
            break;

        case LC_MENU_MODE_DRAFT:
            lc_menu_copy(out, out_size, "");
            break;

        case LC_MENU_MODE_NC_VIEW:
            leancam_schema_format_footer(out, out_size, leancam_schema_page(LC_SCHEMA_PAGE_NC_VIEW));
            break;

        default:
            lc_menu_copy(out, out_size, "");
            break;
    }
}
