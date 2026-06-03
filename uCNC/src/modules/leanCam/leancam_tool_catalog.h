#ifndef LEANCAM_TOOL_CATALOG_H
#define LEANCAM_TOOL_CATALOG_H

#include "leancam_program.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void lc_tool_catalog_init(void);
void lc_tool_catalog_clear(void);
bool lc_tool_catalog_add_raw(const char *line);
bool lc_tool_catalog_has_t(int t);
bool lc_tool_catalog_add_default(int t);
void lc_tool_catalog_seed_defaults(void);
void lc_tool_catalog_sort_by_t(void);
void lc_tool_catalog_copy_from_program(const program_t *prog);
void lc_tool_catalog_copy_to_program(program_t *prog);
bool lc_tool_catalog_load_from_file(const char *path);
void lc_tool_catalog_use_loaded(void);
void lc_tool_catalog_load_from_storage(const char *path);
int lc_tool_line_t_value(const char *line);
const char *lc_tool_catalog_find_in_program_or_catalog(const program_t *prog, int before_or_at, int t);

#ifdef __cplusplus
}
#endif

#endif /* LEANCAM_TOOL_CATALOG_H */

