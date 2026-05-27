#ifndef LEANCAM_RUN_H
#define LEANCAM_RUN_H

#include "conv_core.h"
#include "leancam_gcode.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void lc_run_init(void);
bool lc_run_sim_armed(void);
void lc_run_sim_set_armed(bool armed);
bool lc_run_sim_toggle_arm(void);
bool lc_run_emit_banner(int run_line_no, const char *tool, lc_gcode_send_fn send, void *user);
const char *lc_run_gcode_result_name(lc_gcode_result_t r);
lc_gcode_result_t lc_run_preflight_program(const program_t *prog,
                                           char *err,
                                           unsigned err_len,
                                           int *err_line_out,
                                           int *made_out);
lc_gcode_result_t lc_run_emit_program(const program_t *prog,
                                      lc_gcode_send_fn send,
                                      void *send_user,
                                      char *err,
                                      unsigned err_len,
                                      int *err_line_out,
                                      int *made_out);
lc_gcode_result_t lc_run_emit_selected_range(const program_t *prog,
                                             int start,
                                             int end,
                                             lc_gcode_send_fn send,
                                             void *send_user,
                                             char *err,
                                             unsigned err_len,
                                             int *err_line_out,
                                             int *made_out);

#ifdef __cplusplus
}
#endif

#endif
