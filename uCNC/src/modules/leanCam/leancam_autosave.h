#ifndef LEANCAM_AUTOSAVE_H
#define LEANCAM_AUTOSAVE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void lc_autosave_init(void);
void lc_autosave_schedule(uint32_t now, uint32_t delay_ms);
bool lc_autosave_due(uint32_t now);
void lc_autosave_clear(void);

bool lc_autosave_defer_busy(uint32_t now, uint32_t retry_ms, uint8_t max_tries);
uint8_t lc_autosave_busy_tries(void);

#ifdef __cplusplus
}
#endif

#endif
