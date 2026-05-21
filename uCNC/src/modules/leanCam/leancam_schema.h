#ifndef LEANCAM_SCHEMA_H
#define LEANCAM_SCHEMA_H

#include "leancam_menu.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    LC_SCHEMA_PAGE_FILES = 0,
    LC_SCHEMA_PAGE_FILE_NAME,
    LC_SCHEMA_PAGE_PROGRAM,
    LC_SCHEMA_PAGE_CATALOG,
    LC_SCHEMA_PAGE_DRAFT,
    LC_SCHEMA_PAGE_NC_VIEW
} lc_schema_page_id_t;

typedef enum
{
    LC_SCHEMA_ACT_NONE = 0,
    LC_SCHEMA_ACT_OPEN_CATALOG,
    LC_SCHEMA_ACT_FILE_OPEN,
    LC_SCHEMA_ACT_FILE_NEW,
    LC_SCHEMA_ACT_FILE_DELETE,
    LC_SCHEMA_ACT_FILE_REFRESH,
    LC_SCHEMA_ACT_FILE_TOGGLE_ALL,
    LC_SCHEMA_ACT_FILE_GENERATE,
    LC_SCHEMA_ACT_TEMPLATE,
    LC_SCHEMA_ACT_PROGRAM_EDIT,
    LC_SCHEMA_ACT_CATALOG_NEW,
    LC_SCHEMA_ACT_PROGRAM_COPY,
    LC_SCHEMA_ACT_PROGRAM_DELETE,
    LC_SCHEMA_ACT_PROGRAM_BACK,
    LC_SCHEMA_ACT_PROGRAM_RUN,
    LC_SCHEMA_ACT_DRAFT_FIELD,
    LC_SCHEMA_ACT_DRAFT_NEXT,
    LC_SCHEMA_ACT_DRAFT_SIGN,
    LC_SCHEMA_ACT_DRAFT_DOT,
    LC_SCHEMA_ACT_DRAFT_DIGIT,
    LC_SCHEMA_ACT_DRAFT_SAVE,
    LC_SCHEMA_ACT_DRAFT_BACK,
    LC_SCHEMA_ACT_DRAFT_CANCEL,
    LC_SCHEMA_ACT_NAME_DIGIT,
    LC_SCHEMA_ACT_NAME_BACK,
    LC_SCHEMA_ACT_NAME_CREATE,
    LC_SCHEMA_ACT_NAME_CANCEL,
    LC_SCHEMA_ACT_NC_UP,
    LC_SCHEMA_ACT_NC_DOWN,
    LC_SCHEMA_ACT_NC_BACK,
    LC_SCHEMA_ACT_NC_VIEW
} lc_schema_action_t;

typedef struct
{
    char key;
    const char *label;
    lc_schema_action_t action;
    int value;
    unsigned flags;
} lc_schema_menu_item_t;

#define LC_SCHEMA_ITEM_HIDDEN 0x01u

typedef struct
{
    lc_schema_page_id_t id;
    lc_schema_page_id_t parent;
    const char *title;
    const lc_schema_menu_item_t *items;
    unsigned item_count;
} lc_schema_page_t;

const lc_schema_page_t *leancam_schema_page(lc_schema_page_id_t id);
lc_schema_page_id_t leancam_schema_page_for(lc_menu_mode_t mode, lc_menu_catalog_kind_t catalog);
const lc_schema_menu_item_t *leancam_schema_find_key(const lc_schema_page_t *page, char key);
const lc_schema_menu_item_t *leancam_schema_find_key_for(lc_menu_mode_t mode,
                                                         lc_menu_catalog_kind_t catalog,
                                                         char key);
bool leancam_schema_template_for_key(char key, lc_menu_template_t *out);
void leancam_schema_format_footer(char *out, size_t out_size, const lc_schema_page_t *page);
void leancam_schema_format_title(char *out, size_t out_size, const lc_schema_page_t *page);

#ifdef __cplusplus
}
#endif

#endif
