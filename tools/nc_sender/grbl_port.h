#ifndef GRBL_PORT_H
#define GRBL_PORT_H

/* Serial port abstraction. The sender never calls Win32 directly, so the same
   protocol code can run against a real COM port, a pipe, or a scripted fake
   controller in the host tests. */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Open `device` (for example "COM5") at `baud`. */
    bool (*open)(void *ctx, const char *device, unsigned baud);
    void (*close)(void *ctx);
    /* Return bytes written, or -1. */
    int (*write)(void *ctx, const char *data, size_t len);
    /* Return bytes read, 0 when nothing is waiting, or -1 on error. */
    int (*read)(void *ctx, char *buf, size_t len);
    /* Monotonic milliseconds. */
    unsigned (*elapsed_ms)(void *ctx);
    void (*sleep_ms)(void *ctx, unsigned ms);
} grbl_port_t;

#ifdef __cplusplus
}
#endif

#endif
