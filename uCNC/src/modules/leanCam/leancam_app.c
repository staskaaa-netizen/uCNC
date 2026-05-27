#include "leancam_app.h"

static lc_mode_t g_lc_app_mode = LC_MODE_FILES;
static lc_catalog_kind_t g_lc_app_catalog = LC_CATALOG_NONE;

void lc_app_init(void)
{
    g_lc_app_mode = LC_MODE_FILES;
    g_lc_app_catalog = LC_CATALOG_NONE;
}

lc_mode_t lc_app_mode(void)
{
    return g_lc_app_mode;
}

void lc_app_set_mode(lc_mode_t mode)
{
    g_lc_app_mode = mode;
}

lc_mode_t *lc_app_mode_ptr(void)
{
    return &g_lc_app_mode;
}

lc_catalog_kind_t lc_app_catalog(void)
{
    return g_lc_app_catalog;
}

void lc_app_set_catalog(lc_catalog_kind_t catalog)
{
    g_lc_app_catalog = catalog;
}

lc_catalog_kind_t *lc_app_catalog_ptr(void)
{
    return &g_lc_app_catalog;
}
