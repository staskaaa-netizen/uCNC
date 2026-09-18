#ifndef GRBL_PORT_WIN32_H
#define GRBL_PORT_WIN32_H

#include "grbl_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *handle;
    unsigned long long opened_ms;
    char device[32];
} grbl_port_win32_t;

/* Win32 COM implementation. Returns NULL on non-Windows builds. */
const grbl_port_t *grbl_port_win32(grbl_port_win32_t *port);

#ifdef __cplusplus
}
#endif

#endif
