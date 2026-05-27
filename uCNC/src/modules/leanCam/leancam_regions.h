#ifndef LEANCAM_REGIONS_H
#define LEANCAM_REGIONS_H

#include "conv_core.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool lc_region_is_header(const char *line);
bool lc_region_is_contour(const char *line);
bool lc_region_is_end(const char *line);
int lc_region_display_indent(const program_t *prog, int index, const char *line);
bool lc_region_find(const program_t *prog, int index, int *start_out, int *end_out);

#ifdef __cplusplus
}
#endif

#endif /* LEANCAM_REGIONS_H */
