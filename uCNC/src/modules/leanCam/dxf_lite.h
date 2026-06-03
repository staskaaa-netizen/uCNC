#ifndef LEANCAM_DXF_LITE_H
#define LEANCAM_DXF_LITE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DXF_LITE_MAX_POLY_VERTS
#define DXF_LITE_MAX_POLY_VERTS 512u
#endif

#define DXF_LITE_OK 0
#define DXF_LITE_ERR_ARG -1
#define DXF_LITE_ERR_READ -2
#define DXF_LITE_ERR_MALFORMED -3
#define DXF_LITE_ERR_LIMIT -4

typedef int (*dxf_lite_read_line_fn)(void *user, char *buf, size_t len);

typedef struct {
    dxf_lite_read_line_fn read_line;
    void *user;
} dxf_lite_reader_t;

typedef struct {
    void (*rapid)(void *user, double x, double y);
    void (*line)(void *user, double x1, double y1, double x2, double y2);
    void (*arc)(void *user,
                double x1, double y1,
                double x2, double y2,
                double cx, double cy,
                int ccw);
    void (*circle)(void *user, double cx, double cy, double r);
    void (*warning)(void *user, const char *msg);
} dxf_lite_callbacks_t;

int dxf_lite_parse(dxf_lite_reader_t *reader,
                   const dxf_lite_callbacks_t *cb,
                   void *user);

#ifdef __cplusplus
}
#endif

#endif /* LEANCAM_DXF_LITE_H */
