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

#ifdef __cplusplus
}
#endif

#endif
