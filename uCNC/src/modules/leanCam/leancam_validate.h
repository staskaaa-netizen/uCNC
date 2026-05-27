#ifndef LEANCAM_VALIDATE_H
#define LEANCAM_VALIDATE_H

#include "conv_core.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *lc_validate_effective_tool_for_cycle(const program_t *prog, int before_or_at, const char *cycle);
bool lc_validate_tool_call(const program_t *prog,
                           int before_or_at,
                           const char *line,
                           char *err,
                           size_t err_sz);

#ifdef __cplusplus
}
#endif

#endif /* LEANCAM_VALIDATE_H */
