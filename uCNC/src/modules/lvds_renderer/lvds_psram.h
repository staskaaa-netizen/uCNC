#ifndef LVDS_PSRAM_H
#define LVDS_PSRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LVDS_PSRAM_BASE ((uintptr_t)0x11000000u)
#define LVDS_PSRAM_CS_PIN 47u

bool lvds_psram_init(void);
bool lvds_psram_available(void);
uint32_t lvds_psram_clock_hz(void);
void *lvds_psram_ptr(size_t offset);

#ifdef __cplusplus
}
#endif

#endif /* LVDS_PSRAM_H */
