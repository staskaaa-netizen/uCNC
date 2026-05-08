/*
	Name: parser_g33_ELS.c
	Description: G33 electronic leadscrew threading module for uCNC.

	This module keeps the parser side of G33, but does not run the move through
	the normal planner feed path. The spindle encoder count is the master clock:
	after the next index pulse, encoder delta is converted directly to stepper
	position and the module emits the missing step pulses.
*/

#include "../../cnc.h"
#include "../encoder.h"
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>

#ifdef ENABLE_PARSER_MODULES

#if (UCNC_MODULE_VERSION < 11501 || UCNC_MODULE_VERSION > 99999)
#error "This module is not compatible with the current version of uCNC"
#endif

#ifndef G33_ENCODER
#error "g33_ELS requires G33_ENCODER to be assigned to a spindle encoder"
#endif

#ifndef G33_ELS_STEP_PULSE_US
#define G33_ELS_STEP_PULSE_US 5UL
#endif

#ifndef G33_ELS_DIR_SETUP_US
#define G33_ELS_DIR_SETUP_US 5UL
#endif

#ifndef G33_ELS_DEBUG_INTERVAL_MS
#define G33_ELS_DEBUG_INTERVAL_MS 500UL
#endif

#ifndef G33_ELS_RAMP_ENABLE
#define G33_ELS_RAMP_ENABLE 1
#endif

#ifndef G33_ELS_LOCK_REV_MIN
#define G33_ELS_LOCK_REV_MIN 2
#endif

static volatile int32_t g33_els_index_counter;
static volatile int32_t g33_els_index_encoder_position;
static uint32_t g33_els_encoder_cpr;
static uint32_t g33_els_steps_per_rev;
static float g33_els_steps_per_encoder_count;
static volatile bool g33_els_active;
static volatile bool g33_els_sync_valid;
static volatile int32_t g33_els_sync_encoder_start;

bool g33_els_is_active(void)
{
	return g33_els_active;
}

bool g33_els_get_sync(int32_t *start_ec, uint32_t *cpr)
{
	if (!g33_els_active || !g33_els_sync_valid)
	{
		return false;
	}

	if (start_ec)
	{
		*start_ec = g33_els_sync_encoder_start;
	}
	if (cpr)
	{
		*cpr = g33_els_encoder_cpr;
	}
	return true;
}

static void g33_els_reset_index_tracking(void)
{
	ATOMIC_CODEBLOCK
	{
		g33_els_index_counter = 0;
		g33_els_index_encoder_position = 0;
		g33_els_sync_valid = false;
		g33_els_sync_encoder_start = 0;
	}
}

static uint8_t g33_els_stepper_io_mask(uint8_t stepper)
{
	switch (stepper)
	{
#if (STEPPER_COUNT > 0)
	case 0:
		return LINACT0_IO_MASK;
#endif
#if (STEPPER_COUNT > 1)
	case 1:
		return LINACT1_IO_MASK;
#endif
#if (STEPPER_COUNT > 2)
	case 2:
		return LINACT2_IO_MASK;
#endif
#if (STEPPER_COUNT > 3)
	case 3:
		return LINACT3_IO_MASK;
#endif
#if (STEPPER_COUNT > 4)
	case 4:
		return LINACT4_IO_MASK;
#endif
#if (STEPPER_COUNT > 5)
	case 5:
		return LINACT5_IO_MASK;
#endif
	default:
		return 0;
	}
}

static void g33_els_emit_step(uint8_t stepbits)
{
	io_set_steps(g_settings.step_invert_mask ^ stepbits);
	mcu_delay_us(G33_ELS_STEP_PULSE_US);
	io_set_steps(g_settings.step_invert_mask);
}

static void g33_els_set_dirs(uint8_t dirbits)
{
	static uint8_t last_dirbits;

	if (dirbits != last_dirbits)
	{
		io_set_dirs(dirbits);
		mcu_delay_us(G33_ELS_DIR_SETUP_US);
		last_dirbits = dirbits;
	}
}

static void g33_els_index_cb_handler(void)
{
	g33_els_index_encoder_position = encoder_get_position(G33_ENCODER);
	g33_els_index_counter++;
}

static void g33_els_attach_index_hook(void)
{
#if (G33_ENCODER == ENC0)
	HOOK_ATTACH_CALLBACK(enc0_index, g33_els_index_cb_handler);
#elif (G33_ENCODER == ENC1)
	HOOK_ATTACH_CALLBACK(enc1_index, g33_els_index_cb_handler);
#elif (G33_ENCODER == ENC2)
	HOOK_ATTACH_CALLBACK(enc2_index, g33_els_index_cb_handler);
#elif (G33_ENCODER == ENC3)
	HOOK_ATTACH_CALLBACK(enc3_index, g33_els_index_cb_handler);
#elif (G33_ENCODER == ENC4)
	HOOK_ATTACH_CALLBACK(enc4_index, g33_els_index_cb_handler);
#elif (G33_ENCODER == ENC5)
	HOOK_ATTACH_CALLBACK(enc5_index, g33_els_index_cb_handler);
#elif (G33_ENCODER == ENC6)
	HOOK_ATTACH_CALLBACK(enc6_index, g33_els_index_cb_handler);
#elif (G33_ENCODER == ENC7)
	HOOK_ATTACH_CALLBACK(enc7_index, g33_els_index_cb_handler);
#endif
}

static void g33_els_release_index_hook(void)
{
#if (G33_ENCODER == ENC0)
	HOOK_RELEASE(enc0_index);
#elif (G33_ENCODER == ENC1)
	HOOK_RELEASE(enc1_index);
#elif (G33_ENCODER == ENC2)
	HOOK_RELEASE(enc2_index);
#elif (G33_ENCODER == ENC3)
	HOOK_RELEASE(enc3_index);
#elif (G33_ENCODER == ENC4)
	HOOK_RELEASE(enc4_index);
#elif (G33_ENCODER == ENC5)
	HOOK_RELEASE(enc5_index);
#elif (G33_ENCODER == ENC6)
	HOOK_RELEASE(enc6_index);
#elif (G33_ENCODER == ENC7)
	HOOK_RELEASE(enc7_index);
#endif
}

#if G33_ELS_RAMP_ENABLE
static float g33_els_axis_setting_to_step_rate(uint8_t axis, float units_per_min)
{
	if (axis >= STEPPER_COUNT)
	{
		return 0;
	}

	return units_per_min * g_settings.step_per_mm[axis] * MIN_SEC_MULT;
}

static float g33_els_axis_setting_to_step_accel(uint8_t axis, float units_per_sec2)
{
	if (axis >= STEPPER_COUNT)
	{
		return 0;
	}

	return units_per_sec2 * g_settings.step_per_mm[axis];
}
#endif

static uint8_t g33_els_direct_motion(int32_t *prev_step_pos, int32_t *next_step_pos, uint32_t total_steps)
{
	int32_t step_pos[STEPPER_COUNT];
	int32_t axis_steps[STEPPER_COUNT];
	int32_t emitted_axis_steps[STEPPER_COUNT];
	uint8_t axis_dirs[STEPPER_COUNT];
	uint8_t forward_dirbits = 0;
	uint8_t reverse_dirbits = 0;
	int32_t emitted_master_steps = 0;
	uint32_t min_step_us = G33_ELS_STEP_PULSE_US * 2;
	uint32_t last_step_us = 0;
	int32_t encoder_start = 0;
	int8_t encoder_dir = 0;

#ifdef G33_ELS_DEBUG
	uint32_t debug_time = 0;
#endif

#if G33_ELS_RAMP_ENABLE
	float ramp_accel = 0;
	float ramp_max_rate = 0;
	float ramp_rate = 0;
	uint32_t ramp_last_us = 0;
	bool ramp_decel = false;
	bool ramp_locked = false;
	int32_t ramp_lock_counts = 0;
#endif

	if (!total_steps || !g33_els_encoder_cpr || g33_els_steps_per_encoder_count <= 0)
	{
		return STATUS_OK;
	}

	if (g_settings.max_step_rate > 1)
	{
		uint32_t max_rate_us = (uint32_t)(1000.0f / g_settings.max_step_rate);
		min_step_us = MAX(min_step_us, max_rate_us);
	}

#if G33_ELS_RAMP_ENABLE
	ramp_accel = g33_els_axis_setting_to_step_accel(AXIS_Z, g_settings.acceleration[AXIS_Z]);
	ramp_max_rate = g33_els_axis_setting_to_step_rate(AXIS_Z, g_settings.max_feed_rate[AXIS_Z]);
	if (g_settings.max_step_rate > 1)
	{
		ramp_max_rate = MIN(ramp_max_rate, g_settings.max_step_rate * 1000.0f);
	}
	if (ramp_accel <= 0 || ramp_max_rate <= 0)
	{
		ramp_accel = 0;
		ramp_max_rate = 0;
	}
	ramp_lock_counts = (int32_t)(g33_els_encoder_cpr * G33_ELS_LOCK_REV_MIN);
#endif

	memcpy(step_pos, prev_step_pos, sizeof(step_pos));
	memset(axis_steps, 0, sizeof(axis_steps));
	memset(emitted_axis_steps, 0, sizeof(emitted_axis_steps));
	memset(axis_dirs, 0, sizeof(axis_dirs));

	for (uint8_t i = 0; i < AXIS_TO_STEPPERS; i++)
	{
		int32_t steps = next_step_pos[i] - prev_step_pos[i];
		uint8_t mask = g33_els_stepper_io_mask(i);
		if (steps < 0)
		{
			axis_dirs[i] = 1;
			forward_dirbits |= mask;
			steps = -steps;
		}
		else if (steps > 0)
		{
			reverse_dirbits |= mask;
		}
		axis_steps[i] = steps;
	}

	g33_els_set_dirs(forward_dirbits);
	io_set_steps(g_settings.step_invert_mask);
#ifdef ENABLE_STEPPERS_DISABLE_TIMEOUT
	io_enable_steppers(g_settings.step_enable_invert);
#endif

	while (!g33_els_index_counter)
	{
		if (!cnc_dotasks())
		{
			return STATUS_CRITICAL_FAIL;
		}
	}

	ATOMIC_CODEBLOCK
	{
		encoder_start = g33_els_index_encoder_position;
		g33_els_sync_encoder_start = encoder_start;
		g33_els_sync_valid = true;
	}

	// The ramp is an ELS follower envelope, not a planner lead-in move. During
	// acceleration and deceleration the pitch is intentionally not exact, so G33
	// callers must reserve clearance before and after the actual cutting zone.
#ifdef G33_ELS_DEBUG
	debug_time = mcu_millis();
	proto_info("MSG:G33ELS start EC:%ld steps:%lu spr:%lu enc:%lu ratio:%f", encoder_start, total_steps, g33_els_steps_per_rev, g33_els_encoder_cpr, g33_els_steps_per_encoder_count);
#endif
#if G33_ELS_RAMP_ENABLE
	ramp_last_us = mcu_micros();
#endif

	cnc_set_exec_state(EXEC_RUN);

	while ((uint32_t)emitted_master_steps < total_steps)
	{
		int32_t encoder_delta = encoder_get_position(G33_ENCODER) - encoder_start;
		if (!encoder_dir)
		{
			if (encoder_delta > 0)
			{
				encoder_dir = 1;
			}
			else if (encoder_delta < 0)
			{
				encoder_dir = -1;
			}
		}

		int32_t spindle_counts = encoder_delta * encoder_dir;
		if (spindle_counts < 0)
		{
			spindle_counts = 0;
		}

		int32_t wanted_master_steps = (int32_t)lroundf((float)spindle_counts * g33_els_steps_per_encoder_count);
		wanted_master_steps = CLAMP(0, wanted_master_steps, (int32_t)total_steps);

#if G33_ELS_RAMP_ENABLE
		uint32_t ramp_now_us = mcu_micros();
		float ramp_dt = (float)((uint32_t)(ramp_now_us - ramp_last_us)) * 0.000001f;
		ramp_last_us = ramp_now_us;

		uint32_t remaining_steps = total_steps - (uint32_t)emitted_master_steps;
		uint32_t brake_steps = 0;
		if (ramp_accel > 0 && ramp_rate > 0)
		{
			brake_steps = (uint32_t)ceilf((ramp_rate * ramp_rate) / (2.0f * ramp_accel));
		}

		if (!ramp_decel && remaining_steps <= brake_steps)
		{
			ramp_decel = true;
		}

		if (ramp_accel > 0 && ramp_max_rate > 0)
		{
			float rate_delta = ramp_accel * ramp_dt;
			if (ramp_decel)
			{
				ramp_rate = MAX(0, ramp_rate - rate_delta);
			}
			else
			{
				ramp_rate = MIN(ramp_max_rate, ramp_rate + rate_delta);
			}

			if (wanted_master_steps != emitted_master_steps && ramp_rate < 1.0f)
			{
				ramp_rate = 1.0f;
			}

			if (!ramp_locked && spindle_counts >= ramp_lock_counts && ABS(wanted_master_steps - emitted_master_steps) <= 1)
			{
				ramp_locked = true;
			}
		}
#endif

#ifdef G33_ELS_DEBUG
		if ((uint32_t)(mcu_millis() - debug_time) >= G33_ELS_DEBUG_INTERVAL_MS)
		{
#if G33_ELS_RAMP_ENABLE
			int32_t debug_error = wanted_master_steps - emitted_master_steps;
			uint32_t debug_remaining = total_steps - (uint32_t)emitted_master_steps;
			uint32_t debug_brake = (ramp_accel > 0 && ramp_rate > 0) ? (uint32_t)ceilf((ramp_rate * ramp_rate) / (2.0f * ramp_accel)) : 0;
			proto_info("MSG:G33ELS ramp:rate:%f err:%ld brake:%lu rem:%lu phase:%u", ramp_rate, debug_error, debug_brake, debug_remaining, ramp_locked ? 2 : (ramp_decel ? 4 : 3));
#else
			proto_info("MSG:G33ELS EC:%ld d:%ld c:%ld want:%ld sent:%ld", encoder_get_position(G33_ENCODER), encoder_delta, spindle_counts, wanted_master_steps, emitted_master_steps);
#endif
			debug_time = mcu_millis();
		}
#endif

		while (emitted_master_steps != wanted_master_steps)
		{
			uint32_t now = mcu_micros();
			uint32_t step_wait_us = min_step_us;
#if G33_ELS_RAMP_ENABLE
			if (ramp_accel > 0 && ramp_max_rate > 0)
			{
				float rate_delta = ramp_accel * (float)((uint32_t)(now - ramp_last_us)) * 0.000001f;
				ramp_last_us = now;
				if (ramp_decel)
				{
					ramp_rate = MAX(0, ramp_rate - rate_delta);
				}
				else
				{
					ramp_rate = MIN(ramp_max_rate, ramp_rate + rate_delta);
				}
				if (ramp_rate < 1.0f)
				{
					ramp_rate = 1.0f;
				}
			}
			if (ramp_accel > 0 && ramp_max_rate > 0 && ramp_rate > 0)
			{
				uint32_t ramp_step_us = (uint32_t)ceilf(1000000.0f / ramp_rate);
				step_wait_us = MAX(step_wait_us, ramp_step_us);
			}
#endif
			if ((uint32_t)(now - last_step_us) < step_wait_us)
			{
				if (!cnc_dotasks())
				{
					cnc_clear_exec_state(EXEC_RUN);
					return STATUS_CRITICAL_FAIL;
				}
				continue;
			}

			uint8_t stepbits = 0;
			bool forward = (wanted_master_steps > emitted_master_steps);
			int32_t next_master = emitted_master_steps + (forward ? 1 : -1);
			g33_els_set_dirs(forward ? forward_dirbits : reverse_dirbits);

			for (uint8_t i = 0; i < AXIS_TO_STEPPERS; i++)
			{
				if (!axis_steps[i])
				{
					continue;
				}

				int32_t desired_axis_steps = (int32_t)(((uint64_t)next_master * (uint32_t)axis_steps[i] + (total_steps >> 1)) / total_steps);
				if (forward && desired_axis_steps > emitted_axis_steps[i])
				{
					uint8_t mask = g33_els_stepper_io_mask(i);
					stepbits |= mask;
					emitted_axis_steps[i]++;
					step_pos[i] += axis_dirs[i] ? -1 : 1;
				}
				else if (!forward && desired_axis_steps < emitted_axis_steps[i])
				{
					uint8_t mask = g33_els_stepper_io_mask(i);
					stepbits |= mask;
					emitted_axis_steps[i]--;
					step_pos[i] += axis_dirs[i] ? 1 : -1;
				}
			}

			if (stepbits)
			{
				g33_els_emit_step(stepbits);
				itp_sync_rt_position(step_pos);
				last_step_us = mcu_micros();
			}

			emitted_master_steps = next_master;
		}

		if (!cnc_dotasks())
		{
			cnc_clear_exec_state(EXEC_RUN);
			return STATUS_CRITICAL_FAIL;
		}
	}

	itp_sync_rt_position(next_step_pos);
	mc_sync_position();
	cnc_clear_exec_state(EXEC_RUN);
	return STATUS_OK;
}

#define G33 33

static bool g33_els_parse(void *args);
static bool g33_els_exec_modifier(void *args);
static bool g33_els_exec(void *args);

CREATE_EVENT_LISTENER(gcode_parse, g33_els_parse);
CREATE_EVENT_LISTENER(gcode_exec_modifier, g33_els_exec_modifier);
CREATE_EVENT_LISTENER(gcode_exec, g33_els_exec);

static bool g33_els_parse(void *args)
{
	gcode_parse_args_t *ptr = (gcode_parse_args_t *)args;
	if (ptr->word == 'G' && ptr->code == 33)
	{
		if (ptr->cmd->group_extended != 0 || CHECKFLAG(ptr->cmd->groups, GCODE_GROUP_MOTION))
		{
			*(ptr->error) = STATUS_GCODE_MODAL_GROUP_VIOLATION;
			return EVENT_HANDLED;
		}

		uint8_t mantissa = (uint8_t)lroundf(((ptr->value - ptr->code) * 100.0f));
		if (mantissa != 0)
		{
			*(ptr->error) = STATUS_GCODE_UNSUPPORTED_COMMAND;
			return EVENT_HANDLED;
		}

		ptr->new_state->groups.motion = G33;
		ptr->new_state->groups.motion_mantissa = 0;
		SETFLAG(ptr->cmd->groups, GCODE_GROUP_MOTION);
		ptr->cmd->group_extended = EXTENDED_MOTION_GCODE(33);
		*(ptr->error) = STATUS_OK;
		return EVENT_HANDLED;
	}

	return EVENT_CONTINUE;
}

static bool g33_els_exec_modifier(void *args)
{
	gcode_exec_args_t *ptr = (gcode_exec_args_t *)args;
	if (ptr->cmd->group_extended == EXTENDED_MOTION_GCODE(33))
	{
		// G33 ELS is spindle-position driven. Accept F on the command for sender
		// compatibility, but do not let it become the next modal G1 feed.
		CLEARFLAG(ptr->cmd->words, GCODE_WORD_F);
	}

	return EVENT_CONTINUE;
}

static bool g33_els_exec(void *args)
{
	gcode_exec_args_t *ptr = (gcode_exec_args_t *)args;
	if (ptr->cmd->group_extended != EXTENDED_MOTION_GCODE(33))
	{
		return EVENT_CONTINUE;
	}

	if (!CHECKFLAG(ptr->cmd->words, GCODE_XYZ_AXIS))
	{
		*(ptr->error) = STATUS_GCODE_NO_AXIS_WORDS;
		return EVENT_HANDLED;
	}

	if (!CHECKFLAG(ptr->cmd->words, GCODE_WORD_K) || ptr->words->ijk[2] <= 0)
	{
		*(ptr->error) = STATUS_GCODE_VALUE_WORD_MISSING;
		return EVENT_HANDLED;
	}

	if (mc_update_tools(ptr->block_data) != STATUS_OK)
	{
		*(ptr->error) = STATUS_CRITICAL_FAIL;
		return EVENT_HANDLED;
	}

	g33_els_encoder_cpr = (uint32_t)g_settings.encoders_resolution[G33_ENCODER];
	g33_els_reset_index_tracking();
	g33_els_attach_index_hook();

	float prev_target[AXIS_COUNT];
	mc_get_position(prev_target);
	kinematics_apply_transform(prev_target);
	int32_t prev_step_pos[STEPPER_COUNT];
	kinematics_apply_inverse(prev_target, prev_step_pos);

	float target[AXIS_COUNT];
	memcpy(target, ptr->target, sizeof(target));
	kinematics_apply_transform(target);
	int32_t next_step_pos[STEPPER_COUNT];
	kinematics_apply_inverse(target, next_step_pos);

	float line_dist = 0;
	for (uint8_t i = AXIS_COUNT; i != 0;)
	{
		i--;
		float d = target[i] - prev_target[i];
		line_dist += d * d;
	}
	line_dist = sqrtf(line_dist);

	uint32_t total_steps = 0;
	for (uint8_t i = AXIS_TO_STEPPERS; i != 0;)
	{
		i--;
		int32_t steps = ABS(next_step_pos[i] - prev_step_pos[i]);
		total_steps = MAX(total_steps, (uint32_t)steps);
	}

	if (!total_steps || line_dist <= 0)
	{
		g33_els_release_index_hook();
		ptr->new_state->groups.motion = G1;
		ptr->new_state->groups.motion_mantissa = 0;
		*(ptr->error) = STATUS_OK;
		return EVENT_HANDLED;
	}

	float total_revs = line_dist / ptr->words->ijk[2];
	float steps_per_rev = (float)total_steps / total_revs;
	g33_els_steps_per_rev = lroundf(steps_per_rev);
	g33_els_steps_per_encoder_count = g33_els_encoder_cpr ? fast_flt_div(steps_per_rev, (float)g33_els_encoder_cpr) : 0;

	g33_els_active = true;
	uint8_t exec_error = g33_els_direct_motion(prev_step_pos, next_step_pos, total_steps);
	g33_els_active = false;
	g33_els_release_index_hook();

	if (exec_error != STATUS_OK)
	{
		*(ptr->error) = exec_error;
		return EVENT_HANDLED;
	}

	ptr->new_state->groups.motion = G1;
	ptr->new_state->groups.motion_mantissa = 0;
	*(ptr->error) = STATUS_OK;
	return EVENT_HANDLED;
}

#endif

DECL_MODULE(g33_ELS)
{
#ifdef ENABLE_PARSER_MODULES
	ADD_EVENT_LISTENER(gcode_parse, g33_els_parse);
	ADD_EVENT_LISTENER(gcode_exec_modifier, g33_els_exec_modifier);
	ADD_EVENT_LISTENER(gcode_exec, g33_els_exec);
#else
#error "Parser extensions are not enabled. g33_ELS will not work."
#endif
}
