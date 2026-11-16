#ifndef ENCODER_FOLLOWER_H
#define ENCODER_FOLLOWER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
	int32_t encoder_delta;
	int32_t encoder_position;
	int32_t motor_steps;
	uint32_t tick_count;
	uint32_t sample_count;
	uint32_t accepted_moves;
	uint32_t rejected_moves;
	uint16_t state;
	uint8_t status;
} encoder_follower_debug_t;

bool encoder_follower_get_debug(encoder_follower_debug_t *debug);

#endif
