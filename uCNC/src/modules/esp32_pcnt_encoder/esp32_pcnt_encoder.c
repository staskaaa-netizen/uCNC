/*
	Name: esp32_pcnt_encoder.c
	Description: ESP32 PCNT-backed custom encoder reader for uCNC.
*/

#include "../../cnc.h"
#include "../encoder.h"

#if (MCU == MCU_ESP32 || MCU == MCU_ESP32S3 || MCU == MCU_ESP32C3)

#include "driver/gpio.h"
#include "driver/pcnt.h"
#include <stdio.h>
#include <string.h>

#if (UCNC_MODULE_VERSION < 11501 || UCNC_MODULE_VERSION > 99999)
#error "This module is not compatible with the current version of uCNC"
#endif

#ifndef ENC0_PCNT_UNIT
#define ENC0_PCNT_UNIT PCNT_UNIT_0
#endif

#ifndef ENC0_PCNT_COUNT_MODE
#define ENC0_PCNT_COUNT_MODE 4
#endif

#ifndef ENC0_PULSE_GPIO
#error "ENC0_PULSE_GPIO is not defined"
#endif

#if (ENC0_PCNT_COUNT_MODE == 4) && !defined(ENC0_DIR_GPIO)
#error "ENC0_DIR_GPIO is required for ENC0_PCNT_COUNT_MODE == 4"
#endif

#if (ENC0_PCNT_COUNT_MODE != 4) && !defined(ENC0_DIR_GPIO)
#define ENC0_DIR_GPIO PCNT_PIN_NOT_USED
#endif

#ifndef ENC0_PCNT_RECENTER_THRESHOLD
#define ENC0_PCNT_RECENTER_THRESHOLD 20000
#endif

#ifndef ENC0_READ_WRAP
#define ENC0_READ_WRAP 65536UL
#endif

#ifndef ENC0_INDEX_ISR_EDGE
// The standard encoder module fires index hooks on the logical rising edge.
#define ENC0_INDEX_ISR_EDGE GPIO_INTR_POSEDGE
#endif

#ifndef ENC0_INDEX_VIRTUAL_FIRE_HOOK
#define ENC0_INDEX_VIRTUAL_FIRE_HOOK 1
#endif

#ifndef ENC0_VIRTUAL_INDEXES_PER_REV
#define ENC0_VIRTUAL_INDEXES_PER_REV 1U
#endif

#if (ENC0_VIRTUAL_INDEXES_PER_REV < 1)
#error "ENC0_VIRTUAL_INDEXES_PER_REV must be >= 1"
#endif

#ifndef ENC0_VIRTUAL_MAX_CATCHUP_SLOTS
// Do not emit many hooks in one cnc_io_dotasks pass. G33 uses actual PCNT
// phase, so one current-position hook is enough and avoids dt=0 storms.
#define ENC0_VIRTUAL_MAX_CATCHUP_SLOTS 1U
#endif

static bool esp32_pcnt_encoder_ready;
static int32_t esp32_pcnt_encoder_offset;

static bool enc0_index_have_origin;
static bool enc0_index_have_slot;
static bool enc0_index_have_stats;
static int32_t enc0_index_origin;
static int32_t enc0_index_last_slot;
static int32_t enc0_index_last_position;
static int32_t enc0_index_last_delta;
static int32_t enc0_index_min_delta;
static int32_t enc0_index_max_delta;
static uint32_t enc0_index_count;
static uint32_t enc0_index_abs_delta_sum;
static uint32_t enc0_index_ignored_count;

static char enc0_index_debug_line[128];
static uint32_t enc0_index_debug_seq;
static uint32_t enc0_index_debug_reported_count;
static uint32_t enc0_index_debug_reported_ignored_count;

static volatile uint8_t enc0_isr_have_ref;
static volatile int32_t enc0_isr_ref_pcnt;
static volatile uint32_t enc0_isr_count;

static uint32_t esp32_pcnt_encoder_index_resolution(void)
{
	float resolution = g_settings.encoders_resolution[ENC0];
	if (resolution >= 1.0f)
	{
		return (uint32_t)(resolution + 0.5f);
	}
#ifdef ENC0_CPR
	return (uint32_t)ENC0_CPR;
#else
	return 0;
#endif
}

static int32_t enc0_floor_div_i32(int32_t value, int32_t divisor)
{
	int32_t q = value / divisor;
	int32_t r = value % divisor;

	if (r && ((r < 0) != (divisor < 0)))
	{
		q--;
	}
	return q;
}

static void esp32_pcnt_encoder_update_index_debug(void)
{
	int32_t live_delta;
	uint32_t avg10;

	if (!enc0_index_have_origin)
	{
		return;
	}

	if (enc0_index_debug_reported_count == enc0_index_count &&
		enc0_index_debug_reported_ignored_count == enc0_index_ignored_count)
	{
		return;
	}

	enc0_index_debug_reported_count = enc0_index_count;
	enc0_index_debug_reported_ignored_count = enc0_index_ignored_count;

	live_delta = encoder_get_position(ENC0) - enc0_index_last_position;
	avg10 = (enc0_index_count) ? (uint32_t)(((uint64_t)enc0_index_abs_delta_sum * 10ULL + (enc0_index_count / 2U)) / enc0_index_count) : 0;

	snprintf(enc0_index_debug_line,
			 sizeof(enc0_index_debug_line),
			 "ENCIDX EC:%ld ECB:%ld LAST:%ld AVG:%lu.%lu MIN:%ld MAX:%ld N:%lu IGN:%lu ISR:%lu",
			 (long)encoder_get_position(ENC0),
			 (long)live_delta,
			 (long)enc0_index_last_delta,
			 (unsigned long)(avg10 / 10U),
			 (unsigned long)(avg10 % 10U),
			 (long)enc0_index_min_delta,
			 (long)enc0_index_max_delta,
			 (unsigned long)enc0_index_count,
			 (unsigned long)enc0_index_ignored_count,
			 (unsigned long)enc0_isr_count);

	enc0_index_debug_seq++;
}

static void enc0_accept_virtual_index(int32_t boundary)
{
	int32_t delta = boundary - enc0_index_last_position;

	enc0_index_last_position = boundary;
	enc0_index_last_delta = delta;
	enc0_index_count++;
	enc0_index_abs_delta_sum += (uint32_t)ABS(delta);

	if (!enc0_index_have_stats)
	{
		enc0_index_min_delta = delta;
		enc0_index_max_delta = delta;
		enc0_index_have_stats = true;
	}
	else
	{
		if (delta < enc0_index_min_delta)
		{
			enc0_index_min_delta = delta;
		}
		if (delta > enc0_index_max_delta)
		{
			enc0_index_max_delta = delta;
		}
	}

#if ENC0_INDEX_VIRTUAL_FIRE_HOOK
	HOOK_INVOKE(enc0_index);
#endif
#if ENCODER_DEBUG_PRINT_100MS
	esp32_pcnt_encoder_update_index_debug();
#endif
}

void enc0_virtual_index_unarm(void)
{
    enc0_index_have_origin = false;
    enc0_index_have_slot = false;
    enc0_index_last_position = 0;

    // Force next G33 to wait for fresh physical index.
    enc0_isr_have_ref = false;
}


static void enc0_virtual_index_task(void)
{
	uint32_t enc_res = esp32_pcnt_encoder_index_resolution();
	int32_t slot_size;
	int32_t now;
	int32_t rel;
	int32_t slot;
	int32_t slot_delta;
	int32_t boundary;

	if (!enc_res)
	{
		return;
	}

	// Slot size is encoder counts per *virtual* index, not per physical rev.
	// Example: 4000 counts/rev and 10 virtual indexes => 400 counts/slot.
	slot_size = (int32_t)((enc_res + (ENC0_VIRTUAL_INDEXES_PER_REV / 2U)) / ENC0_VIRTUAL_INDEXES_PER_REV);
	if (slot_size < 1)
	{
		slot_size = 1;
	}

	now = encoder_get_position(ENC0);
	if (!enc0_index_have_origin)
	{
		// Physical index is the phase starter. Do not auto-origin here, because then
		// G33 may start at any arbitrary virtual slot.
		if (enc0_isr_have_ref)
		{
			enc0_index_origin = enc0_isr_ref_pcnt;
		}
		else
		{
			return;
		}
		enc0_index_last_position = enc0_index_origin;
		enc0_index_have_origin = true;
		enc0_index_have_slot = false;
		#if ENCODER_DEBUG_PRINT_100MS
	esp32_pcnt_encoder_update_index_debug();
#endif
	}

	rel = now - enc0_index_origin;
	slot = enc0_floor_div_i32(rel, slot_size);
	if (!enc0_index_have_slot)
	{
		enc0_index_last_slot = slot;
		enc0_index_have_slot = true;
		return;
	}
	if (slot == enc0_index_last_slot)
	{
		return;
	}

	slot_delta = slot - enc0_index_last_slot;

	// If cnc_io_dotasks was late and several virtual slots were crossed, do not
	// call the G33 hook multiple times with nearly identical timestamps. Emit one
	// hook at the latest crossed boundary. PCNT-mode G33 reads actual PCNT count
	// for phase, so it only needs a fresh update trigger.
	if (slot_delta > 0)
	{
		if (slot_delta > (int32_t)ENC0_VIRTUAL_MAX_CATCHUP_SLOTS)
		{
			enc0_index_ignored_count += (uint32_t)(slot_delta - 1);
		}
		boundary = enc0_index_origin + slot * slot_size;
	}
	else
	{
		if ((-slot_delta) > (int32_t)ENC0_VIRTUAL_MAX_CATCHUP_SLOTS)
		{
			enc0_index_ignored_count += (uint32_t)((-slot_delta) - 1);
		}
		boundary = enc0_index_origin + slot * slot_size;
	}

	enc0_index_last_slot = slot;
	enc0_accept_virtual_index(boundary);
}

#ifdef ENC0_INDEX_GPIO
static void IRAM_ATTR enc0_index_gpio_isr(void *arg)
{
	int16_t raw = 0;

	(void)arg;
	pcnt_get_counter_value((pcnt_unit_t)ENC0_PCNT_UNIT, &raw);
	enc0_isr_ref_pcnt = esp32_pcnt_encoder_offset + (int32_t)raw;
	enc0_isr_have_ref = 1;
	enc0_isr_count++;
}

static void enc0_index_gpio_isr_init(void)
{
	gpio_set_intr_type((gpio_num_t)ENC0_INDEX_GPIO, ENC0_INDEX_ISR_EDGE);
	gpio_isr_handler_add((gpio_num_t)ENC0_INDEX_GPIO, enc0_index_gpio_isr, NULL);
}
#endif

static void encoder_esp32_pcnt_init(uint8_t unit, int pulse_gpio, int dir_gpio)
{
	#if ENC0_PCNT_COUNT_MODE == 1


	pcnt_config_t ch0 = {
		.pulse_gpio_num = pulse_gpio,
		.ctrl_gpio_num  = PCNT_PIN_NOT_USED,

		.lctrl_mode = PCNT_MODE_KEEP,
		.hctrl_mode = PCNT_MODE_KEEP,

		.pos_mode = PCNT_COUNT_INC,
		.neg_mode = PCNT_COUNT_DIS,

		.counter_h_lim = 32767,
		.counter_l_lim = -32768,

		.unit = (pcnt_unit_t)unit,
		.channel = PCNT_CHANNEL_0,
	};

	
	pcnt_unit_config(&ch0);

	#elif ENC0_PCNT_COUNT_MODE == 2

	// ------------------------------------------------------------
	// 2x A-only mode (AZ wiring)
	// Count rising + falling edges
	// Example:
	//   1000 PPR encoder -> $150 = 2000
	// ------------------------------------------------------------

	pcnt_config_t ch0 = {
		.pulse_gpio_num = pulse_gpio,
		.ctrl_gpio_num  = PCNT_PIN_NOT_USED,

		.lctrl_mode = PCNT_MODE_KEEP,
		.hctrl_mode = PCNT_MODE_KEEP,

		.pos_mode = PCNT_COUNT_INC,
		.neg_mode = PCNT_COUNT_INC,

		.counter_h_lim = 32767,
		.counter_l_lim = -32768,

		.unit = (pcnt_unit_t)unit,
		.channel = PCNT_CHANNEL_0,
	};

	pcnt_unit_config(&ch0);

#elif ENC0_PCNT_COUNT_MODE == 4

   pcnt_config_t ch0 = {
		.pulse_gpio_num = pulse_gpio,
		.ctrl_gpio_num = dir_gpio,
		.lctrl_mode = PCNT_MODE_REVERSE,
		.hctrl_mode = PCNT_MODE_KEEP,
		.pos_mode = PCNT_COUNT_INC,
		.neg_mode = PCNT_COUNT_DEC,
		.counter_h_lim = 32767,
		.counter_l_lim = -32768,
		.unit = (pcnt_unit_t)unit,
		.channel = PCNT_CHANNEL_0,
	};

	pcnt_config_t ch1 = {
		.pulse_gpio_num = dir_gpio,
		.ctrl_gpio_num = pulse_gpio,
		.lctrl_mode = PCNT_MODE_KEEP,
		.hctrl_mode = PCNT_MODE_REVERSE,
		.pos_mode = PCNT_COUNT_INC,
		.neg_mode = PCNT_COUNT_DEC,
		.counter_h_lim = 32767,
		.counter_l_lim = -32768,
		.unit = (pcnt_unit_t)unit,
		.channel = PCNT_CHANNEL_1,
	};

	pcnt_unit_config(&ch0);
	pcnt_unit_config(&ch1);

#endif
	


#ifdef ENCODER_PCNT_FILTER
	pcnt_set_filter_value((pcnt_unit_t)unit, ENCODER_PCNT_FILTER);
	pcnt_filter_enable((pcnt_unit_t)unit);
#else
	pcnt_filter_disable((pcnt_unit_t)unit);
#endif

	pcnt_counter_pause((pcnt_unit_t)unit);
	pcnt_counter_clear((pcnt_unit_t)unit);
	pcnt_counter_resume((pcnt_unit_t)unit);
}

int32_t read_encoder_esp32_pcnt(uint8_t unit)
{
	int16_t value = 0;
	int32_t out;

	if (!esp32_pcnt_encoder_ready)
	{
		return 0;
	}

	pcnt_get_counter_value((pcnt_unit_t)unit, &value);

	out = esp32_pcnt_encoder_offset + (int32_t)value;
	if (value >= ENC0_PCNT_RECENTER_THRESHOLD || value <= -ENC0_PCNT_RECENTER_THRESHOLD)
	{
		pcnt_counter_pause((pcnt_unit_t)unit);
		pcnt_counter_clear((pcnt_unit_t)unit);
		pcnt_counter_resume((pcnt_unit_t)unit);
		esp32_pcnt_encoder_offset = out;
	}

	return out;
}

int32_t enc_custom_read(uint8_t i)
{
	switch (i)
	{
	case ENC0:
		return read_encoder_esp32_pcnt(ENC0_PCNT_UNIT);
	default:
		return 0;
	}
}

bool encoder_get_index_stats(uint8_t i, int32_t *last, int32_t *min, int32_t *max, uint32_t *count)
{
	if (i != ENC0 || !enc0_index_have_stats)
	{
		return false;
	}

	if (last)
	{
		*last = enc0_index_last_delta;
	}
	if (min)
	{
		*min = enc0_index_min_delta;
	}
	if (max)
	{
		*max = enc0_index_max_delta;
	}
	if (count)
	{
		*count = enc0_index_count;
	}

	return true;
}

bool encoder_get_index_live_delta(uint8_t i, int32_t *delta)
{
	if (i != ENC0 || !enc0_index_have_origin || !delta)
	{
		return false;
	}

	*delta = encoder_get_position(ENC0) - enc0_index_last_position;
	return true;
}

bool encoder_get_index_debug_line(uint8_t i, char *line, uint32_t line_len, uint32_t *seq)
{
	if (i != ENC0 || !line || line_len == 0 || !enc0_index_debug_seq)
	{
		return false;
	}

	strncpy(line, enc0_index_debug_line, line_len - 1);
	line[line_len - 1] = '\0';
	if (seq)
	{
		*seq = enc0_index_debug_seq;
	}
	return true;
}

static bool esp32_pcnt_encoder_dotasks(void *args)
{
	(void)args;
	enc0_virtual_index_task();
	#if ENCODER_DEBUG_PRINT_100MS
		esp32_pcnt_encoder_update_index_debug();
	#endif
	return EVENT_CONTINUE;
}

CREATE_EVENT_LISTENER(cnc_io_dotasks, esp32_pcnt_encoder_dotasks);

DECL_MODULE(esp32_pcnt_encoder)
{
	encoder_esp32_pcnt_init(ENC0_PCNT_UNIT, ENC0_PULSE_GPIO, ENC0_DIR_GPIO);
#ifdef ENC0_INDEX_GPIO
	gpio_install_isr_service(0);
	enc0_index_gpio_isr_init();
#endif
	ADD_EVENT_LISTENER(cnc_io_dotasks, esp32_pcnt_encoder_dotasks);
	esp32_pcnt_encoder_ready = true;
}

#else

DECL_MODULE(esp32_pcnt_encoder)
{
}

#endif
