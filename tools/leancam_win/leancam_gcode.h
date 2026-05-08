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

typedef int (*lc_gcode_send_fn)(const char *line, void *user);

typedef struct
{
    int emit_modal_header;
    int emit_spindle_stop;
} lc_gcode_line_options_t;

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
int leancam_gcode_emit_program_footer(lc_gcode_send_fn send, void *user);

#ifdef __cplusplus
}
#endif

#endif
