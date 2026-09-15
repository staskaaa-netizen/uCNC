#ifndef LEANCAM_PRESETS_H
#define LEANCAM_PRESETS_H

#include "leancam_program.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    LC_PRESET_META_NONE = 0,
    LC_PRESET_META_OD,
    LC_PRESET_META_ID,
    LC_PRESET_META_FACE,
    LC_PRESET_META_RECESS
} lc_preset_meta_kind_t;

typedef struct
{
    lc_preset_meta_kind_t kind;
    int t;
    int o;
    float finish_feed;
    int rpm;
} lc_preset_meta_t;

bool lc_presets_insert_region(program_t *prog,
                              int insert_after,
                              lc_preset_meta_kind_t kind,
                              const char *preset,
                              const char *setup_line,
                              int *target_row,
                              char *err,
                              size_t err_sz);

#ifdef __cplusplus
}
#endif

#endif /* LEANCAM_PRESETS_H */

