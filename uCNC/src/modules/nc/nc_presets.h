#ifndef NC_PRESETS_H
#define NC_PRESETS_H

#include "nc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NC_PRESET_OD = 0,
    NC_PRESET_ID,
    NC_PRESET_FACE,
    NC_PRESET_LINE,
    NC_PRESET_ARC,
    NC_PRESET_SETUP,
    NC_PRESET_END
} nc_preset_t;

nc_result_t nc_insert_preset(nc_document_t *doc, nc_preset_t preset);

#ifdef __cplusplus
}
#endif

#endif
