#ifndef LEANCAM_EXPR_H
#define LEANCAM_EXPR_H

#include <stdint.h>
#include "../ui_snapshot/ui_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

void leancam_expr_build_draft_display(char *dst,
                                      uint32_t dst_len,
                                      const char *draft,
                                      const char *input,
                                      uint8_t active_index,
                                      const char *setup_line,
                                      const char *this_line,
                                      uint8_t *hi_start,
                                      uint8_t *hi_end);

#ifdef __cplusplus
}
#endif

#endif
