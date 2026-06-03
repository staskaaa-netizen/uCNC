#ifndef G71_G72_H
#define G71_G72_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    G7X_OK = 0,
    G7X_BAD_FIELD,
    G7X_UNSUPPORTED,
    G7X_WRITE_FAILED
} g7x_result_t;

typedef enum {
    G7X_CYCLE_NONE = 0,
    G7X_CYCLE_G71,
    G7X_CYCLE_G72
} g7x_cycle_t;

typedef enum {
    G7X_CONTOUR_NONE = 0,
    G7X_CONTOUR_RAPID,
    G7X_CONTOUR_LINE,
    G7X_CONTOUR_ARC_CW,
    G7X_CONTOUR_ARC_CCW,
    G7X_CONTOUR_END
} g7x_contour_cmd_t;

typedef struct {
    uint8_t units;
    uint8_t distance;
} g7x_modal_t;

enum {
    G7X_DISTANCE_ABSOLUTE = 0,
    G7X_DISTANCE_INCREMENTAL = 1,
    G7X_UNITS_INCH = 0,
    G7X_UNITS_MM = 1
};

void g7x_modal_default(g7x_modal_t *modal);
void g7x_modal_from_ucnc_modes(g7x_modal_t *modal, const uint8_t *modalgroups);
bool g7x_modal_apply_line(g7x_modal_t *modal, const char *line);

bool g7x_command_is(const char *line, const char *cmd);
bool g7x_get_field_text(const char *line, const char *key, char *out, size_t out_sz);
bool g7x_get_field_float(const char *line, const char *key, float *out);

g7x_cycle_t g7x_cycle_from_line(const char *line);
g7x_contour_cmd_t g7x_contour_cmd_from_line(const char *line);

const char *g7x_result_text(g7x_result_t result);

#ifdef __cplusplus
}
#endif

#endif
