#include "leancam_schema.h"

#include <stdio.h>
#include <string.h>

#define LC_SCHEMA_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const lc_schema_menu_item_t g_files_items[] = {
    {'1', "Tools", LC_SCHEMA_ACT_OPEN_CATALOG, LC_MENU_CATALOG_TOOLS, 0},
    {'4', "Open", LC_SCHEMA_ACT_FILE_OPEN, 0, 0},
    {'5', "New", LC_SCHEMA_ACT_FILE_NEW, 0, 0},
    {'6', "Del", LC_SCHEMA_ACT_FILE_DELETE, 0, 0},
    {'8', "Ref", LC_SCHEMA_ACT_FILE_REFRESH, 0, 0},
    {'9', "All", LC_SCHEMA_ACT_FILE_TOGGLE_ALL, 0, 0}
};

static const lc_schema_menu_item_t g_file_name_items[] = {
    {'0', "Name", LC_SCHEMA_ACT_NAME_DIGIT, 0, 0},
    {'*', "Back", LC_SCHEMA_ACT_NAME_BACK, 0, 0},
    {'D', "Create", LC_SCHEMA_ACT_NAME_CREATE, 0, 0},
    {'#', "Create", LC_SCHEMA_ACT_NAME_CREATE, 0, 0},
    {'A', "Cancel", LC_SCHEMA_ACT_NAME_CANCEL, 0, 0}
};

static const lc_schema_menu_item_t g_program_items[] = {
    {'0', "Tool", LC_SCHEMA_ACT_TEMPLATE, LC_MENU_TEMPLATE_TOOL, 0},
    {'1', "OD", LC_SCHEMA_ACT_TEMPLATE, LC_MENU_TEMPLATE_OD, 0},
    {'2', "ID", LC_SCHEMA_ACT_TEMPLATE, LC_MENU_TEMPLATE_ID, 0},
    {'3', "Face", LC_SCHEMA_ACT_TEMPLATE, LC_MENU_TEMPLATE_FACE, 0},
    {'4', "Drill", LC_SCHEMA_ACT_TEMPLATE, LC_MENU_TEMPLATE_DRILL, 0},
    {'5', "Tap", LC_SCHEMA_ACT_TEMPLATE, LC_MENU_TEMPLATE_TAP, 0},
    {'6', "Cut", LC_SCHEMA_ACT_TEMPLATE, LC_MENU_TEMPLATE_CUT, 0},
    {'7', "Chmf", LC_SCHEMA_ACT_TEMPLATE, LC_MENU_TEMPLATE_CHAMFER, 0},
    {'8', "ThrO", LC_SCHEMA_ACT_TEMPLATE, LC_MENU_TEMPLATE_THR_OD, 0},
    {'9', "ThrI", LC_SCHEMA_ACT_TEMPLATE, LC_MENU_TEMPLATE_THR_ID, 0},
    {'D', "Edit", LC_SCHEMA_ACT_PROGRAM_EDIT, 0, LC_SCHEMA_ITEM_HIDDEN},
    {'#', "Run", LC_SCHEMA_ACT_PROGRAM_RUN, 0, LC_SCHEMA_ITEM_HIDDEN},
    {'*', "Del", LC_SCHEMA_ACT_PROGRAM_DELETE, 0, LC_SCHEMA_ITEM_HIDDEN},
    {'A', "Files", LC_SCHEMA_ACT_PROGRAM_BACK, 0, LC_SCHEMA_ITEM_HIDDEN}
};

static const lc_schema_menu_item_t g_catalog_items[] = {
    {'0', "New", LC_SCHEMA_ACT_CATALOG_NEW, 0, 0},
    {'1', "Tools", LC_SCHEMA_ACT_OPEN_CATALOG, LC_MENU_CATALOG_TOOLS, 0},
    {'4', "Edit", LC_SCHEMA_ACT_PROGRAM_EDIT, 0, 0},
    {'5', "Copy", LC_SCHEMA_ACT_PROGRAM_COPY, 0, 0},
    {'6', "Del", LC_SCHEMA_ACT_PROGRAM_DELETE, 0, 0},
    {'8', "Files", LC_SCHEMA_ACT_PROGRAM_BACK, 0, 0}
};

static const lc_schema_menu_item_t g_draft_items[] = {
    {'F', "", LC_SCHEMA_ACT_DRAFT_FIELD, 0, 0},
    {'I', "", LC_SCHEMA_ACT_DRAFT_DIGIT, 0, 0},
    {'B', "+/-", LC_SCHEMA_ACT_DRAFT_SIGN, 0, 0},
    {'C', ".", LC_SCHEMA_ACT_DRAFT_DOT, 0, 0},
    {'D', "Next", LC_SCHEMA_ACT_DRAFT_NEXT, 0, 0},
    {'#', "Save", LC_SCHEMA_ACT_DRAFT_SAVE, 0, 0},
    {'*', "Back", LC_SCHEMA_ACT_DRAFT_BACK, 0, 0},
    {'A', "Cancel", LC_SCHEMA_ACT_DRAFT_CANCEL, 0, 0}
};

static const lc_schema_menu_item_t g_nc_items[] = {
    {'B', "Up", LC_SCHEMA_ACT_NC_UP, 0, 0},
    {'C', "Down", LC_SCHEMA_ACT_NC_DOWN, 0, 0},
    {'A', "Files", LC_SCHEMA_ACT_NC_BACK, 0, 0},
    {'#', "View", LC_SCHEMA_ACT_NC_VIEW, 0, 0}
};

static const lc_schema_page_t g_pages[] = {
    {LC_SCHEMA_PAGE_FILES, LC_SCHEMA_PAGE_FILES, "LeanCam Files", g_files_items, (unsigned)LC_SCHEMA_COUNT(g_files_items)},
    {LC_SCHEMA_PAGE_FILE_NAME, LC_SCHEMA_PAGE_FILES, "New LeanCam File", g_file_name_items, (unsigned)LC_SCHEMA_COUNT(g_file_name_items)},
    {LC_SCHEMA_PAGE_PROGRAM, LC_SCHEMA_PAGE_FILES, "LeanCam Program", g_program_items, (unsigned)LC_SCHEMA_COUNT(g_program_items)},
    {LC_SCHEMA_PAGE_CATALOG, LC_SCHEMA_PAGE_FILES, "Catalog", g_catalog_items, (unsigned)LC_SCHEMA_COUNT(g_catalog_items)},
    {LC_SCHEMA_PAGE_DRAFT, LC_SCHEMA_PAGE_PROGRAM, "LeanCam Draft", g_draft_items, (unsigned)LC_SCHEMA_COUNT(g_draft_items)},
    {LC_SCHEMA_PAGE_NC_VIEW, LC_SCHEMA_PAGE_FILES, "NC Viewer", g_nc_items, (unsigned)LC_SCHEMA_COUNT(g_nc_items)}
};

const lc_schema_page_t *leancam_schema_page(lc_schema_page_id_t id)
{
    unsigned i;

    for (i = 0; i < LC_SCHEMA_COUNT(g_pages); ++i) {
        if (g_pages[i].id == id) {
            return &g_pages[i];
        }
    }
    return NULL;
}

lc_schema_page_id_t leancam_schema_page_for(lc_menu_mode_t mode, lc_menu_catalog_kind_t catalog)
{
    switch (mode) {
        case LC_MENU_MODE_FILES:
            return LC_SCHEMA_PAGE_FILES;
        case LC_MENU_MODE_FILE_NAME:
            return LC_SCHEMA_PAGE_FILE_NAME;
        case LC_MENU_MODE_PROGRAM:
            return catalog == LC_MENU_CATALOG_NONE ? LC_SCHEMA_PAGE_PROGRAM : LC_SCHEMA_PAGE_CATALOG;
        case LC_MENU_MODE_DRAFT:
            return LC_SCHEMA_PAGE_DRAFT;
        case LC_MENU_MODE_NC_VIEW:
            return LC_SCHEMA_PAGE_NC_VIEW;
        default:
            return LC_SCHEMA_PAGE_FILES;
    }
}

const lc_schema_menu_item_t *leancam_schema_find_key(const lc_schema_page_t *page, char key)
{
    unsigned i;

    if (!page) {
        return NULL;
    }
    for (i = 0; i < page->item_count; ++i) {
        if (page->items[i].key == key) {
            return &page->items[i];
        }
    }
    return NULL;
}

const lc_schema_menu_item_t *leancam_schema_find_key_for(lc_menu_mode_t mode,
                                                         lc_menu_catalog_kind_t catalog,
                                                         char key)
{
    return leancam_schema_find_key(leancam_schema_page(leancam_schema_page_for(mode, catalog)), key);
}

bool leancam_schema_template_for_key(char key, lc_menu_template_t *out)
{
    const lc_schema_menu_item_t *item = leancam_schema_find_key(leancam_schema_page(LC_SCHEMA_PAGE_PROGRAM), key);

    if (!item || item->action != LC_SCHEMA_ACT_TEMPLATE || !out) {
        return false;
    }
    *out = (lc_menu_template_t)item->value;
    return true;
}

void leancam_schema_format_footer(char *out, size_t out_size, const lc_schema_page_t *page)
{
    unsigned i;
    size_t used = 0;

    if (!out || out_size == 0) {
        return;
    }
    out[0] = 0;
    if (!page) {
        return;
    }

    for (i = 0; i < page->item_count; ++i) {
        int wrote;
        const lc_schema_menu_item_t *item = &page->items[i];

        if (item->flags & LC_SCHEMA_ITEM_HIDDEN) {
            continue;
        }
        wrote = snprintf(out + used,
                         out_size - used,
                         "%s%c %s",
                         used ? "|" : "",
                         item->key,
                         item->label ? item->label : "");
        if (wrote < 0) {
            out[used] = 0;
            return;
        }
        if ((size_t)wrote >= out_size - used) {
            out[out_size - 1] = 0;
            return;
        }
        used += (size_t)wrote;
    }
}

void leancam_schema_format_title(char *out, size_t out_size, const lc_schema_page_t *page)
{
    if (!out || out_size == 0) {
        return;
    }
    snprintf(out, out_size, "%s", (page && page->title) ? page->title : "LeanCam");
}
