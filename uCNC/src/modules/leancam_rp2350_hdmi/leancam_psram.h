#ifndef LEANCAM_PSRAM_H
#define LEANCAM_PSRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LC_PSRAM_BASE ((uintptr_t)0x11000000u)
#define LC_PSRAM_CS_PIN 47u

bool lc_psram_init(void);
bool lc_psram_available(void);
uint32_t lc_psram_clock_hz(void);
void *lc_psram_ptr(size_t offset);

#ifdef __cplusplus
}
#endif

#endif
