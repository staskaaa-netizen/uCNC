#ifndef LEANCAM_RESOURCE_H
#define LEANCAM_RESOURCE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    LC_PSRAM_REGION_LIVE_SIM = 0,
    LC_PSRAM_REGION_TOOL_CATALOG
} lc_psram_region_t;

bool lc_resource_file_begin(const char *tag);
void lc_resource_file_end(void);
bool lc_resource_file_busy(void);
bool lc_resource_can_autosave(void);

void *lc_resource_psram_region(lc_psram_region_t id, size_t size);

bool lc_resource_frame_begin(void);
void lc_resource_frame_end(void);

#ifdef __cplusplus
}
#endif

#endif /* LEANCAM_RESOURCE_H */

