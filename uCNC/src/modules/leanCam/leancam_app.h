#ifndef LEANCAM_APP_H
#define LEANCAM_APP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    LC_MODE_FILES = 0,
    LC_MODE_FILE_NAME,
    LC_MODE_PROGRAM,
    LC_MODE_DRAFT,
    LC_MODE_NC_VIEW
} lc_mode_t;

typedef enum
{
    LC_CATALOG_NONE = 0,
    LC_CATALOG_TOOLS
} lc_catalog_kind_t;

void lc_app_init(void);

lc_mode_t lc_app_mode(void);
void lc_app_set_mode(lc_mode_t mode);
lc_mode_t *lc_app_mode_ptr(void);

lc_catalog_kind_t lc_app_catalog(void);
void lc_app_set_catalog(lc_catalog_kind_t catalog);
lc_catalog_kind_t *lc_app_catalog_ptr(void);

#ifdef __cplusplus
}
#endif

#endif
