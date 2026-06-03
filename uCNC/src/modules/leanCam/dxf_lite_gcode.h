#ifndef LEANCAM_DXF_LITE_GCODE_H
#define LEANCAM_DXF_LITE_GCODE_H

#include "dxf_lite.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*dxf_lite_gcode_write_fn)(void *user, const char *line);

typedef struct {
    dxf_lite_gcode_write_fn write;
    void *user;
    double x_scale;
    double y_scale;
    double x_offset;
    double y_offset;
    int header_emitted;
    int have_pos;
    double x;
    double y;
} dxf_lite_gcode_emitter_t;

void dxf_lite_gcode_init(dxf_lite_gcode_emitter_t *emitter,
                         dxf_lite_gcode_write_fn write,
                         void *user);
const dxf_lite_callbacks_t *dxf_lite_gcode_callbacks(void);
int dxf_lite_gcode_finish(dxf_lite_gcode_emitter_t *emitter);

#ifdef __cplusplus
}
#endif

#endif /* LEANCAM_DXF_LITE_GCODE_H */
