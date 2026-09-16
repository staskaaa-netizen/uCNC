#ifndef NC_FEEDBACK_H
#define NC_FEEDBACK_H
#include <stdint.h>
#include <stdbool.h>
const char *nc_feedback_lock(uint8_t settings_error, uint16_t state, bool alarm);
const char *nc_feedback_error(uint8_t error);
#endif
