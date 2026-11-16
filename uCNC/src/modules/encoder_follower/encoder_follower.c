/*
   ENC1/ENC2 position follower.

   Encoder counts are converted to motor-step deltas and submitted through the
   normal jog/interpolator path. The module never drives STEP pins directly.
*/

#include "../../cnc.h"
#include "../encoder.h"
#include "encoder_follower.h"
#include <math.h>

#if defined(ENABLE_ENCODER_FOLLOWER) && (ENCODERS > 1)

#ifndef ENC1_FOLLOWER_AXIS
#define ENC1_FOLLOWER_AXIS AXIS_X
#endif
#ifndef ENC1_FOLLOWER_STEPS_PER_COUNT
#define ENC1_FOLLOWER_STEPS_PER_COUNT 1.0f
#endif

#ifndef ENC2_FOLLOWER_AXIS
#define ENC2_FOLLOWER_AXIS AXIS_Z
#endif
#ifndef ENC2_FOLLOWER_STEPS_PER_COUNT
#define ENC2_FOLLOWER_STEPS_PER_COUNT 1.0f
#endif

#ifndef ENCODER_FOLLOWER_FEED_MM_MIN
#define ENCODER_FOLLOWER_FEED_MM_MIN 500.0f
#endif
#ifndef ENCODER_FOLLOWER_PERIOD_US
#define ENCODER_FOLLOWER_PERIOD_US 5000u
#endif

#if ENC1_FOLLOWER_AXIS >= AXIS_COUNT
#error "ENC1_FOLLOWER_AXIS is outside AXIS_COUNT"
#endif
#if (ENCODERS > 2) && (ENC2_FOLLOWER_AXIS >= AXIS_COUNT)
#error "ENC2_FOLLOWER_AXIS is outside AXIS_COUNT"
#endif

typedef struct
{
	uint8_t encoder;
	uint8_t axis;
	float steps_per_count;
	int32_t last_count;
	bool primed;
} encoder_follower_t;

static encoder_follower_t g_followers[] = {
	{ENC1, ENC1_FOLLOWER_AXIS, ENC1_FOLLOWER_STEPS_PER_COUNT,
	 0, false},
#if ENCODERS > 2
	{ENC2, ENC2_FOLLOWER_AXIS, ENC2_FOLLOWER_STEPS_PER_COUNT,
	 0, false},
#endif
};

static uint32_t g_follower_last_us;
static encoder_follower_debug_t g_follower_debug;

static void follower_reset_baseline(void)
{
	for (uint8_t i = 0; i < sizeof(g_followers) / sizeof(g_followers[0]); i++)
	{
		g_followers[i].last_count = encoder_get_position(g_followers[i].encoder);
		g_followers[i].primed = true;
	}
}

static bool encoder_follower_tick(void *args)
{
	float target_offset[AXIS_COUNT] = {0};
	uint32_t now = mcu_micros();
	uint16_t state = cnc_get_exec_state(EXEC_ALLACTIVE);
	bool any_steps = false;
	motion_data_t block_data = {0};

	(void)args;
	g_follower_debug.tick_count++;
	g_follower_debug.state = state;

	/* EXEC_RUN is set alongside EXEC_JOG while a jog block is executing.
	   Keep sampling through that state so the hand-wheel can queue its next
	   delta; program-run and every other state still reset the baseline. */
	if ((state & ~(EXEC_JOG | EXEC_RUN)) ||
		((state & EXEC_RUN) && !(state & EXEC_JOG)) ||
		cnc_has_alarm())
	{
		follower_reset_baseline();
		return EVENT_CONTINUE;
	}
	if (!g_follower_last_us ||
		(uint32_t)(now - g_follower_last_us) < ENCODER_FOLLOWER_PERIOD_US)
	{
		return EVENT_CONTINUE;
	}
	g_follower_last_us = now;
	g_follower_debug.sample_count++;

	for (uint8_t i = 0; i < sizeof(g_followers) / sizeof(g_followers[0]); i++)
	{
		encoder_follower_t *follower = &g_followers[i];
		int32_t count = encoder_get_position(follower->encoder);

		if (!follower->primed)
		{
			follower->last_count = count;
			follower->primed = true;
			continue;
		}

		int32_t encoder_delta =
			(int32_t)((uint32_t)count - (uint32_t)follower->last_count);
		int32_t motor_steps =
			(int32_t)truncf((float)encoder_delta * follower->steps_per_count);
		float steps_per_mm = g_settings.step_per_mm[follower->axis];

		if (i == 0u)
		{
			g_follower_debug.encoder_delta = encoder_delta;
			g_follower_debug.encoder_position = count;
			g_follower_debug.motor_steps = motor_steps;
		}
		follower->last_count = count;

		if (motor_steps && steps_per_mm > 0.0f)
		{
			target_offset[follower->axis] +=
				(float)motor_steps / steps_per_mm;
			any_steps = true;
		}
	}

	if (!any_steps)
	{
		return EVENT_CONTINUE;
	}

	block_data.feed = ENCODER_FOLLOWER_FEED_MM_MIN;
	block_data.motion_mode = MOTIONCONTROL_MODE_FEED;
	g_follower_debug.status = mc_incremental_jog(target_offset, &block_data);
	if (g_follower_debug.status == STATUS_OK)
	{
		g_follower_debug.accepted_moves++;
	}
	else
	{
		g_follower_debug.rejected_moves++;
	}

	return EVENT_CONTINUE;
}

CREATE_EVENT_LISTENER(cnc_io_dotasks, encoder_follower_tick);

bool encoder_follower_get_debug(encoder_follower_debug_t *debug)
{
	if (!debug)
	{
		return false;
	}
	*debug = g_follower_debug;
	return true;
}

DECL_MODULE(encoder_follower)
{
	follower_reset_baseline();
	g_follower_last_us = 0u;
	ADD_EVENT_LISTENER(cnc_io_dotasks, encoder_follower_tick);
}

#else

DECL_MODULE(encoder_follower)
{
}

bool encoder_follower_get_debug(encoder_follower_debug_t *debug)
{
	(void)debug;
	return false;
}

#endif
