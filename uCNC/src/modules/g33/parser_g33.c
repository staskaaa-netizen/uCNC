/*
	Name: parser_g33.c
	Description: Implements a parser extension for LinuxCNC G33 for ÂµCNC.

	Copyright: Copyright (c) JoÃ£o Martins
	Author: JoÃ£o Martins
	Date: 25/11/2022

	ÂµCNC is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version. Please see <http://www.gnu.org/licenses/>

	ÂµCNC is distributed WITHOUT ANY WARRANTY;
	Also without the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
	See the	GNU General Public License for more details.
*/

#include "../../cnc.h"
#include "../encoder.h"
#include <stdint.h>
#include <stdbool.h>
#include <float.h>
#include <math.h>

#ifdef ENABLE_PARSER_MODULES

#if (UCNC_MODULE_VERSION < 11501 || UCNC_MODULE_VERSION > 99999)
#error "This module is not compatible with the current version of ÂµCNC"
#endif

#ifndef AXIS_DIR_VECTORS
#ifdef ABC_INDEP_FEED_CALC
#define AXIS_DIR_VECTORS MIN(AXIS_COUNT, 3)
#else
#define AXIS_DIR_VECTORS AXIS_COUNT
#endif
#endif

#ifndef G33_ENCODER
#error "G33 requires to have an assigned encoder"
#endif

// enable this to use the encoder pulse as the feedback loop marker/trigger
//  #define G33_FEEDBACK_LOOP_USE_ENC_PULSE

// uncomment to allow data verbose of sync constants
// the message output is
// [MSG:<spindle index counter>:<expected_step_position>:<current_step_position>:<error>:<encoder_rpm>]
// #define G33_DEBUG

#define SYNC_DISABLED 0
#define SYNC_READY 1
#define SYNC_STARTING 2
#define SYNC_RUNNING 4
#define SYNC_UPDATED 8

static volatile int32_t itp_sync_step_counter;		// step distance counter for synched motions
static volatile uint8_t synched_motion_status;		// synched motion status/phase
static volatile int32_t spindle_index_counter;		// spindle index pulse counter
static int32_t spindle_index_counter_start;			// spindle index pulse initial offset when motion starts
static volatile int32_t spindle_index_step_counter; // step distance counter when the spindle index pulses
static volatile int32_t spindle_index_time;			// index pulse timestamp in us
static volatile int32_t spindle_index_last_time;	// index pulse previous timestamp in us
static volatile int32_t spindle_index_encoder_position;
static volatile int32_t spindle_index_last_encoder_position;
static uint32_t steps_per_index;					// motion steps per index pulse
#ifdef G33_FEEDBACK_LOOP_USE_ENC_PULSE
static uint32_t update_loop_index_counter; // keeps the last update loop index counter
#endif
static uint32_t motion_total_steps;
static float motion_total_distance;
static int32_t current_error;
static float rpm_to_stepfeed_constant;
static uint32_t enc_res;
static int32_t spindle_sync_encoder_start;
static int8_t spindle_sync_encoder_dir;
static float steps_per_encoder_count;
static uint32_t spindle_sync_last_update_time;
static volatile int32_t g33_allowed_step_counter;
static volatile bool g33_step_gate_enabled;

#ifndef G33_CONTINUOUS_UPDATE_US
#define G33_CONTINUOUS_UPDATE_US 500UL
#endif

#ifndef G33_DIRECT_STEP_PULSE_US
#define G33_DIRECT_STEP_PULSE_US 2UL
#endif

#ifndef G33_DIRECT_DEBUG_INTERVAL_MS
#define G33_DIRECT_DEBUG_INTERVAL_MS 500UL
#endif

static void g33_reset_index_tracking(void)
{
	ATOMIC_CODEBLOCK
	{
		spindle_index_counter = 0;
		spindle_index_step_counter = 0;
		spindle_index_time = 0;
		spindle_index_last_time = 0;
		spindle_index_encoder_position = 0;
		spindle_index_last_encoder_position = 0;
		synched_motion_status = SYNC_DISABLED;
	}
	spindle_sync_encoder_start = 0;
	spindle_sync_encoder_dir = 1;
	steps_per_encoder_count = 0;
	spindle_sync_last_update_time = 0;
	g33_allowed_step_counter = 0;
	g33_step_gate_enabled = false;
}

static uint8_t g33_stepper_io_mask(uint8_t stepper)
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

static void g33_emit_step(uint8_t stepbits)
{
	io_toggle_steps(stepbits);
	mcu_delay_us(G33_DIRECT_STEP_PULSE_US);
	io_set_steps(g_settings.step_invert_mask);
}

#if (MCU == MCU_VIRTUAL_WIN)
// used with the virtual emulator to simulate pulses
void mcu_stimul_inputs(volatile VIRTUAL_MAP *virtualmap, uint64_t micros)
{
	static uint64_t last_stim = 0, last_stimsync = 0;
	uint64_t next_stim = last_stim + 120000;			 // 120RPM
	uint64_t next_stimsync = last_stimsync + 120000 / 4; // 120RPM

	if (micros >= next_stimsync)
	{
		last_stimsync = next_stimsync;
		virtualmap->inputs ^= 1; // index pin
		mcu_inputs_changed_cb();
	}

	if (micros >= next_stim)
	{
		last_stim = next_stim;
		virtualmap->inputs ^= 2; // index pin
		mcu_inputs_changed_cb();
	}
}
#endif

void itp_rt_stepcount_cb_handler(uint8_t stepbits, uint8_t itp_flags)
{
	if (itp_flags & ITP_SYNC)
	{
		if (itp_flags & ITP_CONST)
		{
			synched_motion_status |= SYNC_RUNNING;
		}
		else
		{
			synched_motion_status &= ~SYNC_RUNNING;
		}
		itp_sync_step_counter++;
	}
}

#ifdef G33_FEEDBACK_LOOP_USE_ENC_PULSE
#define _g33_enc_pulse_(X) enc##X##_pulse(void)
#define _g33_enc_pulse(X) _g33_enc_pulse_(X)
#define g33_enc_pulse(X) _g33_enc_pulse(X)

void g33_enc_pulse(G33_ENCODER)
{
	if (synched_motion_status >= SYNC_RUNNING)
	{
		spindle_index_step_counter = itp_sync_step_counter;
		synched_motion_status |= SYNC_UPDATED;
	}
}
#endif

void spindle_index_cb_handler(void)
{
	// this measures the amount of time it took to do X full turns of the tool
	// allow to measure RPM (using the index pin instead of the encoder)
	uint32_t now = mcu_micros();
	int32_t index = spindle_index_counter;
	spindle_index_last_time = spindle_index_time;
	spindle_index_time = now;
	spindle_index_last_encoder_position = spindle_index_encoder_position;
	spindle_index_encoder_position = encoder_get_position(G33_ENCODER);
	index++;

	switch (synched_motion_status)
	{
	case SYNC_READY:
		// the spindle index starts synchronized motion
		synched_motion_status = SYNC_STARTING;
		index = spindle_index_counter_start;
		spindle_sync_encoder_start = spindle_index_encoder_position;
		spindle_sync_encoder_dir = (spindle_index_encoder_position >= spindle_index_last_encoder_position) ? 1 : -1;
		spindle_sync_last_update_time = now;
		g33_allowed_step_counter = 0;
		g33_step_gate_enabled = true;
		itp_start(false);
#ifdef G33_FEEDBACK_LOOP_USE_ENC_PULSE
		encoder_reset_position(G33_ENCODER, index * enc_res); // syncs the pulse counter with the index counter
#endif
		break;
	default:
#ifndef G33_FEEDBACK_LOOP_USE_ENC_PULSE
		if (index > 0 && synched_motion_status >= SYNC_RUNNING)
		{
			synched_motion_status |= SYNC_UPDATED;
			// store the step position at the time the index pulse happens
			spindle_index_step_counter = itp_sync_step_counter;
		}
#endif
		break;
	}

	spindle_index_counter = index;
	
}

static uint8_t g33_direct_encoder_motion(int32_t *prev_step_pos, int32_t *next_step_pos, uint32_t total_steps)
{
	int32_t step_pos[STEPPER_COUNT];
	int32_t axis_steps[STEPPER_COUNT];
	int32_t emitted_axis_steps[STEPPER_COUNT];
	uint8_t axis_dirs[STEPPER_COUNT];
	uint8_t forward_dirbits = 0;
	uint8_t reverse_dirbits = 0;
	int32_t emitted_master_steps = 0;
	uint32_t min_step_us = G33_DIRECT_STEP_PULSE_US * 2;
	uint32_t last_step_us = 0;
	int32_t encoder_start = 0;
	int8_t encoder_dir = 0;
	uint32_t debug_time = 0;

	if (!total_steps || !enc_res || steps_per_encoder_count <= 0)
	{
		return STATUS_OK;
	}

	if (g_settings.max_step_rate > 1)
	{
		uint32_t max_rate_us = (uint32_t)(1000.0f / g_settings.max_step_rate);
		min_step_us = MAX(min_step_us, max_rate_us);
	}

	memcpy(step_pos, prev_step_pos, sizeof(step_pos));
	memset(emitted_axis_steps, 0, sizeof(emitted_axis_steps));
	memset(axis_dirs, 0, sizeof(axis_dirs));

	for (uint8_t i = 0; i < AXIS_TO_STEPPERS; i++)
	{
		int32_t steps = next_step_pos[i] - prev_step_pos[i];
		uint8_t mask = g33_stepper_io_mask(i);
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

	io_set_dirs(forward_dirbits);
	io_set_steps(g_settings.step_invert_mask);
#ifdef ENABLE_STEPPERS_DISABLE_TIMEOUT
	io_enable_steppers(g_settings.step_enable_invert);
#endif

	// Wait for the next spindle index. From that point EC counts are the thread clock.
	while (!spindle_index_counter)
	{
		if (!cnc_dotasks())
		{
			return STATUS_CRITICAL_FAIL;
		}
	}
	encoder_start = encoder_get_position(G33_ENCODER);
	debug_time = mcu_millis();
	proto_info("MSG:G33 start EC:%ld steps:%lu spr:%lu enc:%lu ratio:%f", encoder_start, total_steps, steps_per_index, enc_res, steps_per_encoder_count);
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

		int32_t wanted_master_steps = (int32_t)lroundf((float)spindle_counts * steps_per_encoder_count);
		wanted_master_steps = CLAMP(0, wanted_master_steps, (int32_t)total_steps);
		if ((uint32_t)(mcu_millis() - debug_time) >= G33_DIRECT_DEBUG_INTERVAL_MS)
		{
			proto_info("MSG:G33 EC:%ld d:%ld c:%ld want:%ld sent:%ld", encoder_get_position(G33_ENCODER), encoder_delta, spindle_counts, wanted_master_steps, emitted_master_steps);
			debug_time = mcu_millis();
		}

		while (emitted_master_steps != wanted_master_steps)
		{
			uint32_t now = mcu_micros();
			if ((uint32_t)(now - last_step_us) < min_step_us)
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
			io_set_dirs(forward ? forward_dirbits : reverse_dirbits);

			for (uint8_t i = 0; i < AXIS_TO_STEPPERS; i++)
			{
				if (!axis_steps[i])
				{
					continue;
				}

				int32_t desired_axis_steps = (int32_t)(((uint64_t)next_master * (uint32_t)axis_steps[i] + (total_steps >> 1)) / total_steps);
				if (forward && desired_axis_steps > emitted_axis_steps[i])
				{
					uint8_t mask = g33_stepper_io_mask(i);
					stepbits |= mask;
					emitted_axis_steps[i]++;
					step_pos[i] += axis_dirs[i] ? -1 : 1;
				}
				else if (!forward && desired_axis_steps < emitted_axis_steps[i])
				{
					uint8_t mask = g33_stepper_io_mask(i);
					stepbits |= mask;
					emitted_axis_steps[i]--;
					step_pos[i] += axis_dirs[i] ? 1 : -1;
				}
			}

			if (stepbits)
			{
				g33_emit_step(stepbits);
				itp_sync_rt_position(step_pos);
				last_step_us = mcu_micros();
			}

			emitted_master_steps = next_master;
		}

		current_error = (int32_t)wanted_master_steps - (int32_t)emitted_master_steps;
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





// this ID must be unique for each code
#define G33 33

bool g33_parse(void *args);
bool g33_exec(void *args);

CREATE_EVENT_LISTENER(gcode_parse, g33_parse);
CREATE_EVENT_LISTENER(gcode_exec, g33_exec);

// this just parses and accepts the code
bool g33_parse(void *args)
{
	gcode_parse_args_t *ptr = (gcode_parse_args_t *)args;
	if (ptr->word == 'G' && ptr->code == 33)
	{
		// stops event propagation
		if (ptr->cmd->group_extended != 0 || CHECKFLAG(ptr->cmd->groups, GCODE_GROUP_MOTION))
		{
			// there is a collision of custom gcode commands (only one per line can be processed)
			*(ptr->error) = STATUS_GCODE_MODAL_GROUP_VIOLATION;
			return EVENT_HANDLED;
		}
		// checks if it's G5 or G5.1
		// check mantissa
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

	// if this is not catched by this parser, just send back the error so other extenders can process it
	return EVENT_CONTINUE;
}

// this actually performs 2 steps in 1 (validation and execution)
bool g33_exec(void *args)
{
	gcode_exec_args_t *ptr = (gcode_exec_args_t *)args;
	if (ptr->cmd->group_extended == EXTENDED_MOTION_GCODE(33))
	{
		if (!CHECKFLAG(ptr->cmd->words, GCODE_XYZ_AXIS))
		{
			// it's an error no axis word is specified
			*(ptr->error) = STATUS_GCODE_NO_AXIS_WORDS;
			return EVENT_HANDLED;
		}

		if (!CHECKFLAG(ptr->cmd->words, GCODE_WORD_K))
		{
			// it's an error no distance per rev word is specified
			*(ptr->error) = STATUS_GCODE_VALUE_WORD_MISSING;
			return EVENT_HANDLED;
		}

		// syncs motions and sets spindle
		if (mc_update_tools(ptr->block_data) != STATUS_OK)
		{
			*(ptr->error) = STATUS_CRITICAL_FAIL;
			return EVENT_HANDLED;
		}

		enc_res = ((uint32_t)g_settings.encoders_resolution[G33_ENCODER]);
		g33_reset_index_tracking();

		// attach the index event callback
#if (G33_ENCODER == ENC0)
		HOOK_ATTACH_CALLBACK(enc0_index, spindle_index_cb_handler);
#elif (G33_ENCODER == ENC1)
		HOOK_ATTACH_CALLBACK(enc1_index, spindle_index_cb_handler);
#elif (G33_ENCODER == ENC2)
		HOOK_ATTACH_CALLBACK(enc2_index, spindle_index_cb_handler);
#elif (G33_ENCODER == ENC3)
		HOOK_ATTACH_CALLBACK(enc3_index, spindle_index_cb_handler);
#elif (G33_ENCODER == ENC4)
		HOOK_ATTACH_CALLBACK(enc4_index, spindle_index_cb_handler);
#elif (G33_ENCODER == ENC5)
		HOOK_ATTACH_CALLBACK(enc5_index, spindle_index_cb_handler);
#elif (G33_ENCODER == ENC6)
		HOOK_ATTACH_CALLBACK(enc6_index, spindle_index_cb_handler);
#elif (G33_ENCODER == ENC7)
		HOOK_ATTACH_CALLBACK(enc7_index, spindle_index_cb_handler);
#endif

		// this code can be removed as the initial reading of the G33 RPM ensures the spindle is running

		// if (!ptr->block_data->motion_flags.bit.spindle_running)
		// {
		// 	*(ptr->error) = STATUS_SPINDLE_RPM_ERROR;
		// 	return EVENT_HANDLED;
		// }

		// // update tool
		// mc_update_tools(ptr->block_data);

#ifdef TOOL_WAIT_FOR_SPEED
		// wait for spindle to reach the desired speed
		uint16_t programmed_speed = ptr->block_data->spindle;
		uint16_t at_speed_threshold = lroundf(TOOL_WAIT_FOR_SPEED_MAX_ERROR * 0.01f * programmed_speed);

		// wait for tool at speed
		uint32_t start_spindle_time = mcu_millis();
		while (ABS(programmed_speed - encoder_get_rpm(G33_ENCODER)) > at_speed_threshold)
		{
			if (!cnc_dotasks() || (mcu_millis() - start_spindle_time) > (DELAY_ON_RESUME_SPINDLE * 1000))
			{
				*(ptr->error) = STATUS_SPINDLE_RPM_ERROR;
				return EVENT_HANDLED;
			}
		}
#endif
		float index_rpm = encoder_get_rpm(G33_ENCODER);
		if (index_rpm < 1)
		{
			index_rpm = 1;
		}

		// gets the starting point
		float prev_target[AXIS_COUNT];
		mc_get_position(prev_target);
		kinematics_apply_transform(prev_target);
		int32_t prev_step_pos[STEPPER_COUNT];
		kinematics_apply_inverse(prev_target, prev_step_pos);

		// gets the exit point (copies to prevent modifying target vector)
		float line_dist = 0;
		float dir_vect[AXIS_COUNT];
		memcpy(dir_vect, ptr->target, sizeof(dir_vect));
		kinematics_apply_transform(dir_vect);
		int32_t next_step_pos[STEPPER_COUNT];
		kinematics_apply_inverse(dir_vect, next_step_pos);

		// calculates amount of motion vector
		for (uint8_t i = AXIS_COUNT; i != 0;)
		{
			i--;
			dir_vect[i] -= prev_target[i];
			line_dist += dir_vect[i] * dir_vect[i];
		}

		line_dist = sqrtf(line_dist);
		motion_total_distance = line_dist;
		float inv_dist = fast_flt_inv(line_dist);

		// determines the normalized direction vector
		// and the maximum acceleration
		float max_feed = FLT_MAX;
		float max_accel = FLT_MAX;

		for (uint8_t i = 0; i < AXIS_DIR_VECTORS; i++)
		{
			float normal_vect = dir_vect[i] * inv_dist;
			dir_vect[i] = normal_vect;
			normal_vect = ABS(normal_vect);
			// denormalize max feed rate for each axis
			float denorm_param = fast_flt_div(g_settings.max_feed_rate[i], normal_vect);
			max_feed = MIN(max_feed, denorm_param);
			max_feed = MIN(max_feed, F_STEP_MAX);
			denorm_param = fast_flt_div(g_settings.acceleration[i], normal_vect);
			max_accel = MIN(max_accel, denorm_param);
		}

		// calculates the total number of steps in the motion
		uint32_t total_steps = 0;
		for (uint8_t i = AXIS_TO_STEPPERS; i != 0;)
		{
			i--;
			int32_t steps = next_step_pos[i] - prev_step_pos[i];

			steps = ABS(steps);
			if (total_steps < (uint32_t)steps)
			{
				total_steps = steps;
			}
		}

		motion_total_steps = total_steps;

		// from this the factor to convert from RPM to step feed can be obtained
		// step rate = rpm_to_stepfeed_constant * RPM
		rpm_to_stepfeed_constant = ptr->words->ijk[2] * total_steps * MIN_SEC_MULT / line_dist;

		// calculates the feedrate based in the K factor and the programmed spindle RPM
		// spindle is in Rev/min and K is in units(mm) per Rev Rev/min * mm/Rev = mm/min
		float total_revs = line_dist / ptr->words->ijk[2];
		float feed = ptr->words->ijk[2] * index_rpm;

		// calculates the expected number of steps per revolution
		float steps_per_rev = (float)total_steps / total_revs;
		steps_per_index = lroundf(steps_per_rev);
		steps_per_encoder_count = (enc_res) ? fast_flt_div(steps_per_rev, (float)enc_res) : 0;

		ptr->block_data->feed = feed;
		ptr->block_data->motion_flags.bit.synched = 1;
		ptr->block_data->max_accel = max_accel;

		// G33 is an electronic-gear move: parser/planner math gives the legal target,
		// but spindle EC counts directly clock the actual step pulses.
		spindle_index_counter = 0;
		itp_sync_step_counter = 0;
		g33_allowed_step_counter = 0;
		g33_step_gate_enabled = false;
		synched_motion_status = SYNC_DISABLED;

		uint8_t exec_error = g33_direct_encoder_motion(prev_step_pos, next_step_pos, total_steps);

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

		if (exec_error != STATUS_OK)
		{
			*(ptr->error) = exec_error;
			return EVENT_HANDLED;
		}

		// Do not leave G33 modal after a completed thread move. Follow-up OD/ID cycle
		// lines may contain only axis words and should return to normal feed motion.
		ptr->new_state->groups.motion = G1;
		ptr->new_state->groups.motion_mantissa = 0;

		*(ptr->error) = STATUS_OK;
		return EVENT_HANDLED;
	}

	return EVENT_CONTINUE;
}

#endif

#ifdef ENABLE_MAIN_LOOP_MODULES
bool g33_proto_status(void *args)
{
	if ((g_settings.status_report_mask & 4))
	{
		if ((synched_motion_status >= SYNC_RUNNING))
		{
			float error = motion_total_distance * current_error;
			error /= (float)motion_total_steps;
			proto_printf("|Se:%f", error);
		}
	}

	return EVENT_CONTINUE;
}
CREATE_EVENT_LISTENER(proto_status, g33_proto_status);

bool spindle_sync_update_loop(void *ptr)
{
	if (synched_motion_status >= SYNC_STARTING && steps_per_encoder_count > 0)
	{
		int32_t error, index_step_counter;
		uint32_t t = 0, delta_t = 0;
		uint32_t now = mcu_micros();
		int32_t encoder_position = encoder_get_position(G33_ENCODER);

		if ((uint32_t)(now - spindle_sync_last_update_time) < G33_CONTINUOUS_UPDATE_US && !(synched_motion_status & SYNC_UPDATED))
		{
			return EVENT_CONTINUE;
		}

		// gets a snapshot of the current spindle/index timing and current motion step position
		ATOMIC_CODEBLOCK
		{
			synched_motion_status &= ~SYNC_UPDATED;
			index_step_counter = itp_sync_step_counter;
			delta_t = spindle_index_time;
			t = spindle_index_last_time;
		}

#ifdef G33_FEEDBACK_LOOP_USE_ENC_PULSE
		delta_t = encoder_get_delta(G33_ENCODER) * g_settings.encoders_resolution[G33_ENCODER];
#else
		delta_t -= t;
#endif
		if (!delta_t)
		{
			return EVENT_CONTINUE;
		}
		float index_rpm = 1000000.0f / ((float)delta_t * MIN_SEC_MULT);
		if (index_rpm < 1)
		{
			cnc_alarm(EXEC_ALARM_SPINDLE_SYNC_FAIL);
			return STATUS_CRITICAL_FAIL;
		}

		// calculate the spindle position from live encoder counts, not only from one index per rev
		int32_t encoder_counts = (encoder_position - spindle_sync_encoder_start) * spindle_sync_encoder_dir;
		int32_t expected_position = (spindle_index_counter_start * (int32_t)steps_per_index) + (int32_t)lroundf((float)encoder_counts * steps_per_encoder_count);
#ifdef G33_FEEDBACK_LOOP_USE_ENC_PULSE
		expected_position /= g_settings.encoders_resolution[G33_ENCODER];
#endif
		if (expected_position < 0)
		{
			expected_position = 0;
		}
		else if ((uint32_t)expected_position > motion_total_steps)
		{
			expected_position = (int32_t)motion_total_steps;
		}

		if (expected_position > g33_allowed_step_counter)
		{
			g33_allowed_step_counter = expected_position;
		}

		// if negative the axis are ahead of spindle and need to slow down
		// if positive the axis are behind the spindle and need to speed up.
		error = g33_allowed_step_counter - index_step_counter;
		current_error = error;

		// #ifdef G33_DEBUG
		//  cnc_call_rt_command(CMD_CODE_REPORT);
		// #endif

		if (error)
		{
			float new_step_rate = rpm_to_stepfeed_constant * index_rpm;
#ifdef G33_FEEDBACK_LOOP_USE_ENC_PULSE
			new_step_rate += error * g_settings.encoders_resolution[G33_ENCODER];
#else
			new_step_rate += error;
#endif
			// this updates the interpolator right on the next step and the current motion in the planner
			itp_update_feed(new_step_rate);
		}
		spindle_sync_last_update_time = now;

#ifdef G33_DEBUG
		int32_t index_counter = spindle_index_counter;
		proto_info("MSG:Spindle turns %ld, expected pos %ld, real pos %ld, error: %ld, rpm %f", index_counter, expected_position, index_step_counter, error, index_rpm);
#endif
	}
	else if (synched_motion_status)
	{
#ifdef G33_DEBUG
		static uint32_t prev_print = 0;
		uint32_t elapsed = mcu_millis() - prev_print;
		if (elapsed > 1000)
		{
			uint32_t t = 0, delta_t = 0;
			ATOMIC_CODEBLOCK
			{
				delta_t = spindle_index_time;
				t = spindle_index_last_time;
			}
			delta_t -= t;
			if (delta_t)
			{
				float index_rpm = 1000000.0f / ((float)delta_t * MIN_SEC_MULT);
				proto_info("MSG:G33 TOOL RPM %f", index_rpm);
			}
			prev_print = mcu_millis();
		}
#endif
	}

	return EVENT_CONTINUE;
}

CREATE_EVENT_LISTENER(cnc_dotasks, spindle_sync_update_loop);
#endif

DECL_MODULE(g33)
{
#ifdef ENABLE_PARSER_MODULES
	ADD_EVENT_LISTENER(gcode_parse, g33_parse);
	ADD_EVENT_LISTENER(gcode_exec, g33_exec);
#else
#error "Parser extensions are not enabled. G33 code extension will not work."
#endif
#ifdef ENABLE_MAIN_LOOP_MODULES
	ADD_EVENT_LISTENER(proto_status, g33_proto_status);
	ADD_EVENT_LISTENER(cnc_dotasks, spindle_sync_update_loop);
#else
#error "Main loop extensions are not enabled. G33 code extension will not work."
#endif
#ifndef ENABLE_RT_SYNC_MOTIONS
#error "ENABLE_RT_SYNC_MOTIONS must be enabled to allow realtime step counting in sync motions."
#endif
}
