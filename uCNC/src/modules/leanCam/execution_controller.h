#ifndef EXECUTION_CONTROLLER_H
#define EXECUTION_CONTROLLER_H

#include <stdint.h>

#ifndef EXECUTION_CONTROLLER_SNAPSHOT_MS
#define EXECUTION_CONTROLLER_SNAPSHOT_MS 20u
#endif

#ifndef EXECUTION_CONTROLLER_RENDER_MS
#define EXECUTION_CONTROLLER_RENDER_MS 30u
#endif

#ifndef EXECUTION_CONTROLLER_PRESENT_MS
#define EXECUTION_CONTROLLER_PRESENT_MS 0u
#endif

#ifndef EXECUTION_CONTROLLER_CHUNKED_PRESENT
#define EXECUTION_CONTROLLER_CHUNKED_PRESENT 0
#endif

#ifndef EXECUTION_CONTROLLER_PRESENT_CHUNKS
#define EXECUTION_CONTROLLER_PRESENT_CHUNKS 8u
#endif

#ifdef __cplusplus
extern "C" {
#endif

void execution_controller_init(void);
void execution_controller_poll(void);

#ifdef __cplusplus
}
#endif

#endif
