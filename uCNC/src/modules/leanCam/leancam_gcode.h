#ifndef LEANCAM_GCODE_H
#define LEANCAM_GCODE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    LC_GCODE_OK = 0,
    LC_GCODE_UNSUPPORTED,
    LC_GCODE_NO_SETUP,
    LC_GCODE_BAD_FIELD,
    LC_GCODE_STREAM_REJECT
} lc_gcode_result_t;

#include "leancam_program.h"

typedef int (*lc_gcode_send_fn)(const char *line, void *user);

typedef struct
{
    int emit_modal_header;
    int emit_spindle_stop;
} lc_gcode_line_options_t;

typedef struct
{
    unsigned char bytes[8192];
} lc_gcode_state_snapshot_t;

typedef struct
{
    unsigned char bytes[12288];
} lc_gcode_stepper_t;

typedef enum
{
    LC_GCODE_STEP_LINE = 0,
    LC_GCODE_STEP_DONE,
    LC_GCODE_STEP_ERROR
} lc_gcode_step_result_t;

int leancam_gcode_save_state(lc_gcode_state_snapshot_t *snapshot);
int leancam_gcode_restore_state(const lc_gcode_state_snapshot_t *snapshot);

lc_gcode_result_t leancam_gcode_run_line(const char *line,
                                         const char *setup_line,
                                         const char *tool_line,
                                         lc_gcode_send_fn send,
                                         void *user);
lc_gcode_result_t leancam_gcode_run_line_ex(const char *line,
                                            const char *setup_line,
                                            const char *tool_line,
                                            lc_gcode_send_fn send,
                                            void *user,
                                            char *err,
                                            unsigned err_len);
lc_gcode_result_t leancam_gcode_run_line_with_options(const char *line,
                                                      const char *setup_line,
                                                      const char *tool_line,
                                                      const lc_gcode_line_options_t *options,
                                                      lc_gcode_send_fn send,
                                                      void *user,
                                                      char *err,
                                                      unsigned err_len);
lc_gcode_result_t leancam_gcode_run_program_line_ex(const char *line,
                                                    const char *setup_line,
                                                    const char *tool_line,
                                                    lc_gcode_send_fn send,
                                                    void *user,
                                                    char *err,
                                                    unsigned err_len);
int leancam_gcode_emit_program_header(lc_gcode_send_fn send, void *user);
int leancam_gcode_emit_program_footer_ex(lc_gcode_send_fn send,
                                         void *user,
                                         char *err,
                                         unsigned err_len);
int leancam_gcode_emit_program_footer(lc_gcode_send_fn send, void *user);

void leancam_gcode_stepper_reset(lc_gcode_stepper_t *stepper);
lc_gcode_result_t leancam_gcode_stepper_begin(lc_gcode_stepper_t *stepper,
                                              const program_t *prog,
                                              int start,
                                              int end,
                                              char *err,
                                              unsigned err_len,
                                              int *err_line_out);
lc_gcode_step_result_t leancam_gcode_stepper_next(lc_gcode_stepper_t *stepper,
                                                  char *out,
                                                  unsigned out_len,
                                                  char *err,
                                                  unsigned err_len,
                                                  int *err_line_out);

#ifdef __cplusplus
}
#endif

#endif

