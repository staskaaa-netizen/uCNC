/*
	Name: esp32_pcnt_encoder.c
	Description: ESP32 PCNT-backed custom encoder reader for uCNC.
*/

#include "../../cnc.h"
#include "../encoder.h"

#if (MCU == MCU_ESP32 || MCU == MCU_ESP32S3 || MCU == MCU_ESP32C3)

#include "driver/pcnt.h"
#include <stdio.h>
#include <string.h>

#if (UCNC_MODULE_VERSION < 11501 || UCNC_MODULE_VERSION > 99999)
#error "This module is not compatible with the current version of uCNC"
#endif

#ifndef ENC0_PCNT_UNIT
#define ENC0_PCNT_UNIT PCNT_UNIT_0
#endif

#ifndef ENC0_PULSE_GPIO
#error "ENC0_PULSE_GPIO is not defined"
#endif

#ifndef ENC0_DIR_GPIO
#error "ENC0_DIR_GPIO is not defined"
#endif

#if defined(ENC0_INDEX_PCNT_UNIT) && defined(ENC0_INDEX_GPIO)
#define ENC0_INDEX_PCNT_ENABLED 1
#endif

#if defined(ENC0_INDEX_PCNT_FILTER_US) && !defined(ENC0_INDEX_PCNT_FILTER)
#define ENC0_INDEX_PCNT_FILTER ((ENC0_INDEX_PCNT_FILTER_US) * 80)
#endif

#if defined(ENC0_INDEX_PCNT_FILTER) && (ENC0_INDEX_PCNT_FILTER > 1023)
#undef ENC0_INDEX_PCNT_FILTER
#define ENC0_INDEX_PCNT_FILTER 1023
#endif

#ifndef ENC0_PCNT_RECENTER_THRESHOLD
#define ENC0_PCNT_RECENTER_THRESHOLD 20000
#endif

#define ENC0_INDEX_MODE_LEGACY_ISR 0
#define ENC0_INDEX_MODE_PCNT_INDEX 1
#define ENC0_INDEX_MODE_RMT_MARKER 2
#define ENC0_INDEX_MODE_DFF_LATCH 3
#define ENC0_INDEX_MODE_SOFTWARE_POLL 4

#ifndef ENC0_INDEX_MODE
#define ENC0_INDEX_MODE ENC0_INDEX_MODE_LEGACY_ISR
#endif

#ifndef ENC0_INDEX_SOFT_POLL_ENABLE
#define ENC0_INDEX_SOFT_POLL_ENABLE (ENC0_INDEX_MODE == ENC0_INDEX_MODE_SOFTWARE_POLL)
#endif

#ifndef ENC0_INDEX_SOFT_POLL_STATUS_MS
#define ENC0_INDEX_SOFT_POLL_STATUS_MS 1000
#endif

#ifndef ENC0_INDEX_HUNT_COMPARE_MAX_DELTA
#define ENC0_INDEX_HUNT_COMPARE_MAX_DELTA 500
#endif

#ifndef ENC0_RMT_SHORT_LIMIT_PCT
#define ENC0_RMT_SHORT_LIMIT_PCT 65
#endif

#ifndef ENC0_RMT_LONG_LIMIT_PCT
#define ENC0_RMT_LONG_LIMIT_PCT 135
#endif

#ifndef ENC0_RMT_SUM_TOLERANCE_PCT
#define ENC0_RMT_SUM_TOLERANCE_PCT 20
#endif

#ifndef ENC0_INDEX_RMT_GPIO
#define ENC0_INDEX_RMT_GPIO ENC0_INDEX_GPIO
#endif

#ifndef ENC0_READ_WRAP
#define ENC0_READ_WRAP 65536UL
#endif

static bool esp32_pcnt_encoder_ready;
static int32_t esp32_pcnt_encoder_offset;
static volatile uint8_t esp32_pcnt_index_pending;
static bool esp32_pcnt_index_have_origin;
static bool esp32_pcnt_index_have_stats;
static int32_t esp32_pcnt_index_last_position;
static int32_t esp32_pcnt_index_last_delta;
static int32_t esp32_pcnt_index_min_delta;
static int32_t esp32_pcnt_index_max_delta;
static uint32_t esp32_pcnt_index_count;
static uint32_t esp32_pcnt_index_abs_delta_sum;
static uint32_t esp32_pcnt_index_hw_count;
static uint32_t esp32_pcnt_index_ignored_count;
static uint32_t esp32_pcnt_index_bad_count;
static uint32_t esp32_pcnt_index_missed_count;
static bool esp32_pcnt_index_resync_after_miss;
static char esp32_pcnt_index_debug_line[128];
static uint32_t esp32_pcnt_index_debug_seq;
static uint32_t esp32_pcnt_index_debug_reported_count;
static uint32_t esp32_pcnt_index_debug_reported_hw_count;
static uint32_t esp32_pcnt_index_debug_reported_ignored_count;
static uint32_t esp32_pcnt_index_debug_reported_bad_count;
static uint32_t esp32_pcnt_index_debug_reported_missed_count;

static uint32_t esp32_pcnt_encoder_index_min_delta(void);
static uint32_t esp32_pcnt_encoder_index_max_delta(void);

// ============================================================
// EXPERIMENTAL INDEX POSITION HUNT
// ============================================================
// Diagnostic-only playground for comparing true-index-position methods.
// Existing PCNT/GPIO index handling remains the source for current G33/motion
// behavior; this section only records and optionally reports observations.
typedef struct
{
	uint8_t valid;
	uint8_t mode;
	uint8_t direction;
	int32_t pcnt_count_now;
	int32_t index_ref_count;
	int32_t wrapped_delta;
	int32_t marker_edge_offset;
	uint32_t avg_ticks;
	uint32_t short_ticks;
	uint32_t long_ticks;
	float phase_fraction;
	uint32_t timestamp_us;
} enc0_index_sample_t;

static enc0_index_sample_t enc0_index_hunt_latest;

static int32_t enc0_wrap_delta(int32_t now, int32_t ref, int32_t wrap)
{
	int32_t d = now - ref;

	// Expected checks: ref=65530 now=5 -> +11, ref=5 now=65530 -> -11.
	if (d > wrap / 2)
	{
		d -= wrap;
	}
	if (d < -wrap / 2)
	{
		d += wrap;
	}

	return d;
}

static int32_t enc0_wrap_add(int32_t base, int32_t add, int32_t wrap)
{
	int32_t v = base + add;

	while (v >= wrap)
	{
		v -= wrap;
	}
	while (v < 0)
	{
		v += wrap;
	}

	return v;
}

#if ENC0_INDEX_MODE != ENC0_INDEX_MODE_LEGACY_ISR
static float enc0_index_hunt_clampf(float value, float min_value, float max_value)
{
	if (value < min_value)
	{
		return min_value;
	}
	if (value > max_value)
	{
		return max_value;
	}
	return value;
}

static const char *enc0_index_hunt_mode_name(uint8_t mode)
{
	switch (mode)
	{
	case ENC0_INDEX_MODE_PCNT_INDEX:
		return "PCNT_INDEX";
	case ENC0_INDEX_MODE_RMT_MARKER:
		return "RMT_MARKER";
	case ENC0_INDEX_MODE_DFF_LATCH:
		return "DFF_LATCH";
	case ENC0_INDEX_MODE_SOFTWARE_POLL:
		return "SOFTWARE_POLL";
	default:
		return "LEGACY_ISR";
	}
}

static void enc0_index_hunt_store_sample(uint8_t mode, int32_t ref, int32_t marker_edge_offset, uint32_t avg_ticks, uint32_t short_ticks, uint32_t long_ticks, bool short_before_long)
{
	int32_t now = encoder_get_position(ENC0);
	int32_t now_wrapped = enc0_wrap_add(0, now, (int32_t)ENC0_READ_WRAP);
	int32_t ref_wrapped = enc0_wrap_add(0, ref, (int32_t)ENC0_READ_WRAP);
	int32_t adjusted_ref = enc0_wrap_add(ref_wrapped, marker_edge_offset, (int32_t)ENC0_READ_WRAP);
	int32_t wrapped_delta = enc0_wrap_delta(now_wrapped, adjusted_ref, (int32_t)ENC0_READ_WRAP);
	float phase = 0.0f;

	if (avg_ticks)
	{
		phase = (float)short_ticks / (float)avg_ticks;
		if (!short_before_long)
		{
			phase = 1.0f - phase;
		}
		phase = enc0_index_hunt_clampf(phase, 0.0f, 1.0f);
	}

	enc0_index_hunt_latest.valid = 1;
	enc0_index_hunt_latest.mode = mode;
	enc0_index_hunt_latest.direction = (wrapped_delta >= 0) ? 1 : 2;
	enc0_index_hunt_latest.pcnt_count_now = now;
	enc0_index_hunt_latest.index_ref_count = adjusted_ref;
	enc0_index_hunt_latest.wrapped_delta = wrapped_delta;
	enc0_index_hunt_latest.marker_edge_offset = marker_edge_offset;
	enc0_index_hunt_latest.avg_ticks = avg_ticks;
	enc0_index_hunt_latest.short_ticks = short_ticks;
	enc0_index_hunt_latest.long_ticks = long_ticks;
	enc0_index_hunt_latest.phase_fraction = phase;
	enc0_index_hunt_latest.timestamp_us = mcu_micros();

#ifdef ENC0_INDEX_HUNT_DEBUG
	proto_info("MSG:[IDXHUNT] mode=%s dir=%c pcnt=%ld ref=%ld delta=%ld avg=%lu short=%lu long=%lu phase=%f valid=%u",
			   enc0_index_hunt_mode_name(mode),
			   (enc0_index_hunt_latest.direction == 2) ? 'R' : 'F',
			   (long)enc0_index_hunt_latest.pcnt_count_now,
			   (long)enc0_index_hunt_latest.index_ref_count,
			   (long)enc0_index_hunt_latest.wrapped_delta,
			   (unsigned long)enc0_index_hunt_latest.avg_ticks,
			   (unsigned long)enc0_index_hunt_latest.short_ticks,
			   (unsigned long)enc0_index_hunt_latest.long_ticks,
			   enc0_index_hunt_latest.phase_fraction,
			   enc0_index_hunt_latest.valid);
#endif
}
#endif

#if ENC0_INDEX_SOFT_POLL_ENABLE
static uint8_t enc0_soft_poll_last_state;
static uint8_t enc0_soft_poll_have_state;
static bool enc0_soft_poll_have_ref;
static int32_t enc0_soft_poll_last_pcnt;
static int32_t enc0_soft_poll_last_delta;
static int32_t enc0_soft_poll_last_pcnt_compare_delta;
static int32_t enc0_soft_poll_min_pcnt_compare_delta;
static int32_t enc0_soft_poll_max_pcnt_compare_delta;
static uint32_t enc0_soft_poll_edge_count;
static uint32_t enc0_soft_poll_near_count;
static uint32_t enc0_soft_poll_far_count;
static uint32_t enc0_soft_poll_compare_count;
static uint32_t enc0_soft_poll_compare_reject_count;
static uint32_t enc0_soft_poll_last_status_ms;

static void enc0_index_hunt_soft_poll_compare_to_pcnt_index(int32_t pcnt_now)
{
	int32_t compare_delta;

	if (!esp32_pcnt_index_have_origin)
	{
		return;
	}

	compare_delta = enc0_wrap_delta(enc0_wrap_add(0, pcnt_now, (int32_t)ENC0_READ_WRAP),
									enc0_wrap_add(0, esp32_pcnt_index_last_position, (int32_t)ENC0_READ_WRAP),
									(int32_t)ENC0_READ_WRAP);
	enc0_soft_poll_last_pcnt_compare_delta = compare_delta;
	if ((uint32_t)ABS(compare_delta) > ENC0_INDEX_HUNT_COMPARE_MAX_DELTA)
	{
		enc0_soft_poll_compare_reject_count++;
		return;
	}

	if (!enc0_soft_poll_compare_count)
	{
		enc0_soft_poll_min_pcnt_compare_delta = compare_delta;
		enc0_soft_poll_max_pcnt_compare_delta = compare_delta;
	}
	else
	{
		if (compare_delta < enc0_soft_poll_min_pcnt_compare_delta)
		{
			enc0_soft_poll_min_pcnt_compare_delta = compare_delta;
		}
		if (compare_delta > enc0_soft_poll_max_pcnt_compare_delta)
		{
			enc0_soft_poll_max_pcnt_compare_delta = compare_delta;
		}
	}
	enc0_soft_poll_compare_count++;
}

static void enc0_index_hunt_soft_poll(void)
{
	uint8_t state = io_get_input(ENC0_INDEX) ? 1U : 0U;
	int32_t pcnt_now;
	int32_t marker_delta = 0;
	uint32_t abs_marker_delta;
	uint32_t min_delta;
	uint32_t max_delta;

	if (!enc0_soft_poll_have_state)
	{
		enc0_soft_poll_last_state = state;
		enc0_soft_poll_have_state = 1;
		return;
	}

	if (!state || enc0_soft_poll_last_state)
	{
		enc0_soft_poll_last_state = state;
		return;
	}
	enc0_soft_poll_last_state = state;

	pcnt_now = encoder_get_position(ENC0);
	if (enc0_soft_poll_have_ref)
	{
		marker_delta = enc0_wrap_delta(enc0_wrap_add(0, pcnt_now, (int32_t)ENC0_READ_WRAP),
									   enc0_wrap_add(0, enc0_soft_poll_last_pcnt, (int32_t)ENC0_READ_WRAP),
									   (int32_t)ENC0_READ_WRAP);
	}

	abs_marker_delta = (uint32_t)ABS(marker_delta);
	min_delta = esp32_pcnt_encoder_index_min_delta();
	max_delta = esp32_pcnt_encoder_index_max_delta();
	if (enc0_soft_poll_have_ref && abs_marker_delta < ENC0_INDEX_HUNT_MIN_MARKER_DELTA)
	{
		enc0_soft_poll_near_count++;
		return;
	}
	if (enc0_soft_poll_have_ref && (abs_marker_delta < min_delta || abs_marker_delta > max_delta))
	{
		enc0_soft_poll_far_count++;
		enc0_soft_poll_last_pcnt = pcnt_now;
		enc0_soft_poll_last_delta = marker_delta;
		return;
	}

	enc0_soft_poll_have_ref = true;
	enc0_soft_poll_last_pcnt = pcnt_now;
	enc0_soft_poll_last_delta = marker_delta;
	enc0_soft_poll_edge_count++;
	enc0_index_hunt_soft_poll_compare_to_pcnt_index(pcnt_now);
	enc0_index_hunt_store_sample(ENC0_INDEX_MODE_SOFTWARE_POLL, pcnt_now - marker_delta, 0, 0, 0, 0, true);
}

static void enc0_index_hunt_soft_poll_status(void)
{
#ifdef ENC0_INDEX_HUNT_DEBUG
	uint32_t now_ms = mcu_millis();

	if ((uint32_t)(now_ms - enc0_soft_poll_last_status_ms) < ENC0_INDEX_SOFT_POLL_STATUS_MS)
	{
		return;
	}
	enc0_soft_poll_last_status_ms = now_ms;

	proto_info("MSG:[IDXHUNT SOFT] pcnt=%ld edges=%lu near=%lu far=%lu md=%ld cmp=%ld cmin=%ld cmax=%ld cn=%lu crj=%lu cwin=%lu pin=%u",
			   (long)encoder_get_position(ENC0),
			   (unsigned long)enc0_soft_poll_edge_count,
			   (unsigned long)enc0_soft_poll_near_count,
			   (unsigned long)enc0_soft_poll_far_count,
			   (long)enc0_soft_poll_last_delta,
			   (long)enc0_soft_poll_last_pcnt_compare_delta,
			   (long)enc0_soft_poll_min_pcnt_compare_delta,
			   (long)enc0_soft_poll_max_pcnt_compare_delta,
			   (unsigned long)enc0_soft_poll_compare_count,
			   (unsigned long)enc0_soft_poll_compare_reject_count,
			   (unsigned long)ENC0_INDEX_HUNT_COMPARE_MAX_DELTA,
			   (unsigned)enc0_soft_poll_last_state);
#endif
}
#endif

#if ENC0_INDEX_MODE == ENC0_INDEX_MODE_RMT_MARKER
#include "driver/rmt.h"
#include "freertos/ringbuf.h"

#ifndef ENC0_INDEX_RMT_CHANNEL
#define ENC0_INDEX_RMT_CHANNEL RMT_CHANNEL_0
#endif

#ifndef ENC0_INDEX_RMT_A_CHANNEL
#define ENC0_INDEX_RMT_A_CHANNEL RMT_CHANNEL_1
#endif

#ifndef ENC0_INDEX_RMT_B_CHANNEL
#define ENC0_INDEX_RMT_B_CHANNEL RMT_CHANNEL_2
#endif

#ifndef ENC0_INDEX_RMT_CLK_DIV
#define ENC0_INDEX_RMT_CLK_DIV 80
#endif

#ifndef ENC0_INDEX_RMT_IDLE_THRESHOLD
#define ENC0_INDEX_RMT_IDLE_THRESHOLD 2000
#endif

#ifndef ENC0_INDEX_RMT_SYMBOL_BUF
#define ENC0_INDEX_RMT_SYMBOL_BUF 256
#endif

#ifndef ENC0_INDEX_HUNT_STATUS_MS
#define ENC0_INDEX_HUNT_STATUS_MS 1000
#endif

#ifndef ENC0_INDEX_HUNT_MIN_MARKER_DELTA
#define ENC0_INDEX_HUNT_MIN_MARKER_DELTA 1000
#endif

#ifndef ENC0_RMT_LOCAL_AVG_SIBLINGS
#define ENC0_RMT_LOCAL_AVG_SIBLINGS 2
#endif

#ifndef ENC0_INDEX_HUNT_RESYNC_ON_FAR
#define ENC0_INDEX_HUNT_RESYNC_ON_FAR 1
#endif

#ifndef ENC0_RMT_MARKER_SINGLE_PULSE
#define ENC0_RMT_MARKER_SINGLE_PULSE 0
#endif

#ifndef ENC0_RMT_ABZ_CAPTURE
#define ENC0_RMT_ABZ_CAPTURE 0
#endif

#ifndef ENC0_INDEX_HUNT_COMPARE_MAX_DELTA
#define ENC0_INDEX_HUNT_COMPARE_MAX_DELTA 500
#endif

#define ENC0_RMT_INTERVAL_RING 16

static volatile uint8_t enc0_rmt_intervals_pending;
static volatile uint32_t enc0_rmt_event_count;
static volatile uint32_t enc0_rmt_symbol_count;
static volatile uint32_t enc0_rmt_empty_poll_count;
static volatile uint32_t enc0_rmt_packet_count;
static volatile uint32_t enc0_rmt_candidate_count;
static volatile uint32_t enc0_rmt_accept_count;
static volatile uint32_t enc0_rmt_reject_near_count;
static volatile uint32_t enc0_rmt_reject_far_count;
static int32_t enc0_rmt_last_near_delta;
static int32_t enc0_rmt_last_far_delta;
static int32_t enc0_rmt_last_pcnt_compare_delta;
static int32_t enc0_rmt_min_pcnt_compare_delta;
static int32_t enc0_rmt_max_pcnt_compare_delta;
static uint32_t enc0_rmt_compare_count;
static uint32_t enc0_rmt_compare_reject_count;
static bool enc0_rmt_have_marker_ref;
static bool enc0_rmt_have_compare_stats;
static int32_t enc0_rmt_last_marker_pcnt;
static int32_t enc0_rmt_last_marker_delta;
static uint32_t enc0_rmt_last_min_delta;
static uint32_t enc0_rmt_last_max_delta;
static uint32_t enc0_rmt_last_avg_ticks;
static uint32_t enc0_rmt_last_short_ticks;
static uint32_t enc0_rmt_last_long_ticks;
static uint32_t enc0_rmt_last_status_ms;
static uint32_t enc0_rmt_intervals[ENC0_RMT_INTERVAL_RING];
static uint8_t enc0_rmt_interval_head;
static uint8_t enc0_rmt_interval_count;
static RingbufHandle_t enc0_rmt_ringbuf;

#if ENC0_RMT_ABZ_CAPTURE
typedef struct
{
	RingbufHandle_t ringbuf;
	rmt_channel_t channel;
	int gpio;
	uint32_t packet_count;
	uint32_t symbol_count;
	uint32_t edge_count;
	uint32_t last_duration0;
	uint32_t last_duration1;
	int32_t last_pcnt;
	uint32_t last_us;
} enc0_rmt_edge_stream_t;

static enc0_rmt_edge_stream_t enc0_rmt_a_stream;
static enc0_rmt_edge_stream_t enc0_rmt_b_stream;

static void enc0_index_hunt_rmt_stream_init(enc0_rmt_edge_stream_t *stream, rmt_channel_t channel, int gpio)
{
	rmt_config_t config = {
		.rmt_mode = RMT_MODE_RX,
		.channel = channel,
		.gpio_num = gpio,
		.clk_div = ENC0_INDEX_RMT_CLK_DIV,
		.mem_block_num = 1,
		.rx_config = {
			.filter_en = false,
			.filter_ticks_thresh = 0,
			.idle_threshold = ENC0_INDEX_RMT_IDLE_THRESHOLD,
		},
	};

	stream->channel = channel;
	stream->gpio = gpio;
	rmt_config(&config);
	rmt_driver_install(config.channel, ENC0_INDEX_RMT_SYMBOL_BUF * sizeof(rmt_item32_t), 0);
	rmt_get_ringbuf_handle(config.channel, &stream->ringbuf);
	rmt_rx_start(config.channel, true);
}

static void enc0_index_hunt_rmt_stream_drain(enc0_rmt_edge_stream_t *stream)
{
	size_t rx_size = 0;
	rmt_item32_t *items;
	uint32_t count;
	uint32_t i;

	if (!stream->ringbuf)
	{
		return;
	}

	for (;;)
	{
		items = (rmt_item32_t *)xRingbufferReceive(stream->ringbuf, &rx_size, 0);
		if (!items)
		{
			break;
		}

		count = rx_size / sizeof(rmt_item32_t);
		stream->packet_count++;
		stream->symbol_count += count;
		stream->last_pcnt = encoder_get_position(ENC0);
		stream->last_us = mcu_micros();

		for (i = 0; i < count; i++)
		{
			if (items[i].duration0)
			{
				stream->edge_count++;
				stream->last_duration0 = items[i].duration0;
			}
			if (items[i].duration1)
			{
				stream->edge_count++;
				stream->last_duration1 = items[i].duration1;
			}
		}
		vRingbufferReturnItem(stream->ringbuf, (void *)items);
	}
}

static void enc0_index_hunt_rmt_abz_init(void)
{
	enc0_index_hunt_rmt_stream_init(&enc0_rmt_a_stream, ENC0_INDEX_RMT_A_CHANNEL, ENC0_PULSE_GPIO);
	enc0_index_hunt_rmt_stream_init(&enc0_rmt_b_stream, ENC0_INDEX_RMT_B_CHANNEL, ENC0_DIR_GPIO);

#ifdef ENC0_INDEX_HUNT_DEBUG
	proto_info("MSG:[IDXHUNT ABZ init] A:gpio=%d ch=%d B:gpio=%d ch=%d clkdiv=%u idle=%lu buf=%u",
			   (int)ENC0_PULSE_GPIO,
			   (int)ENC0_INDEX_RMT_A_CHANNEL,
			   (int)ENC0_DIR_GPIO,
			   (int)ENC0_INDEX_RMT_B_CHANNEL,
			   (unsigned)ENC0_INDEX_RMT_CLK_DIV,
			   (unsigned long)ENC0_INDEX_RMT_IDLE_THRESHOLD,
			   (unsigned)ENC0_INDEX_RMT_SYMBOL_BUF);
#endif
}

static void enc0_index_hunt_rmt_abz_drain(void)
{
	enc0_index_hunt_rmt_stream_drain(&enc0_rmt_a_stream);
	enc0_index_hunt_rmt_stream_drain(&enc0_rmt_b_stream);
}
#endif

static void enc0_index_hunt_rmt_compare_to_pcnt_index(int32_t pcnt_now)
{
	if (!esp32_pcnt_index_have_origin)
	{
		return;
	}

	enc0_rmt_last_pcnt_compare_delta = enc0_wrap_delta(enc0_wrap_add(0, pcnt_now, (int32_t)ENC0_READ_WRAP),
													   enc0_wrap_add(0, esp32_pcnt_index_last_position, (int32_t)ENC0_READ_WRAP),
													   (int32_t)ENC0_READ_WRAP);
	if ((uint32_t)ABS(enc0_rmt_last_pcnt_compare_delta) > ENC0_INDEX_HUNT_COMPARE_MAX_DELTA)
	{
		enc0_rmt_compare_reject_count++;
		return;
	}

	if (!enc0_rmt_have_compare_stats)
	{
		enc0_rmt_min_pcnt_compare_delta = enc0_rmt_last_pcnt_compare_delta;
		enc0_rmt_max_pcnt_compare_delta = enc0_rmt_last_pcnt_compare_delta;
		enc0_rmt_have_compare_stats = true;
	}
	else
	{
		if (enc0_rmt_last_pcnt_compare_delta < enc0_rmt_min_pcnt_compare_delta)
		{
			enc0_rmt_min_pcnt_compare_delta = enc0_rmt_last_pcnt_compare_delta;
		}
		if (enc0_rmt_last_pcnt_compare_delta > enc0_rmt_max_pcnt_compare_delta)
		{
			enc0_rmt_max_pcnt_compare_delta = enc0_rmt_last_pcnt_compare_delta;
		}
	}
	enc0_rmt_compare_count++;
}

static void enc0_index_hunt_rmt_push_interval(uint32_t ticks)
{
	if (!ticks)
	{
		return;
	}
	enc0_rmt_intervals[enc0_rmt_interval_head] = ticks;
	enc0_rmt_interval_head = (uint8_t)((enc0_rmt_interval_head + 1U) % ENC0_RMT_INTERVAL_RING);
	if (enc0_rmt_interval_count < ENC0_RMT_INTERVAL_RING)
	{
		enc0_rmt_interval_count++;
	}
}

static bool enc0_index_hunt_rmt_is_marker(uint32_t a, uint32_t b, uint32_t local_avg, bool *short_before_long)
{
	uint32_t short_ticks = (a < b) ? a : b;
	uint32_t long_ticks = (a < b) ? b : a;
	uint32_t sum = a + b;
	uint32_t expected = local_avg * 2U;
	uint32_t tolerance = (expected * ENC0_RMT_SUM_TOLERANCE_PCT) / 100U;

	if (!local_avg || short_ticks >= ((local_avg * ENC0_RMT_SHORT_LIMIT_PCT) / 100U))
	{
		return false;
	}
	if (long_ticks <= ((local_avg * ENC0_RMT_LONG_LIMIT_PCT) / 100U))
	{
		return false;
	}
	if (sum + tolerance < expected || sum > expected + tolerance)
	{
		return false;
	}

	*short_before_long = (a < b);
	return true;
}

static uint32_t enc0_index_hunt_rmt_local_avg(uint32_t *intervals, uint8_t count, uint8_t pair_index)
{
	uint32_t sum = 0;
	uint8_t samples = 0;
	uint8_t n;

	for (n = 1; n <= ENC0_RMT_LOCAL_AVG_SIBLINGS; n++)
	{
		if (pair_index >= n)
		{
			sum += intervals[pair_index - n];
			samples++;
		}
		if ((uint8_t)(pair_index + 1U + n) < count)
		{
			sum += intervals[pair_index + 1U + n];
			samples++;
		}
	}

	return samples ? ((sum + (samples / 2U)) / samples) : 0;
}

static void enc0_index_hunt_rmt_detect(void)
{
	uint32_t local[ENC0_RMT_INTERVAL_RING];
	uint8_t i;

#if ENC0_RMT_MARKER_SINGLE_PULSE
	int32_t pcnt_now;
	int32_t pcnt_wrapped;
	int32_t ref_wrapped;
	int32_t marker_delta;
	uint32_t abs_marker_delta;
	uint32_t min_delta;
	uint32_t max_delta;

	if (!enc0_rmt_intervals_pending)
	{
		return;
	}
	enc0_rmt_intervals_pending = 0;
	pcnt_now = encoder_get_position(ENC0);
	pcnt_wrapped = enc0_wrap_add(0, pcnt_now, (int32_t)ENC0_READ_WRAP);
	ref_wrapped = enc0_wrap_add(0, enc0_rmt_last_marker_pcnt, (int32_t)ENC0_READ_WRAP);
	marker_delta = enc0_rmt_have_marker_ref ? enc0_wrap_delta(pcnt_wrapped, ref_wrapped, (int32_t)ENC0_READ_WRAP) : 0;
	abs_marker_delta = (uint32_t)ABS(marker_delta);
	min_delta = esp32_pcnt_encoder_index_min_delta();
	max_delta = esp32_pcnt_encoder_index_max_delta();
	enc0_rmt_candidate_count++;
	enc0_rmt_last_min_delta = min_delta;
	enc0_rmt_last_max_delta = max_delta;

	if (enc0_rmt_have_marker_ref && abs_marker_delta < ENC0_INDEX_HUNT_MIN_MARKER_DELTA)
	{
		enc0_rmt_last_near_delta = marker_delta;
		enc0_rmt_reject_near_count++;
		return;
	}
	if (enc0_rmt_have_marker_ref && (abs_marker_delta < min_delta || abs_marker_delta > max_delta))
	{
		enc0_rmt_last_far_delta = marker_delta;
		enc0_rmt_reject_far_count++;
#if ENC0_INDEX_HUNT_RESYNC_ON_FAR
		if (abs_marker_delta > max_delta)
		{
			enc0_rmt_last_marker_pcnt = pcnt_now;
			enc0_rmt_last_marker_delta = marker_delta;
		}
#endif
		return;
	}

	enc0_rmt_have_marker_ref = true;
	enc0_rmt_last_marker_pcnt = pcnt_now;
	enc0_rmt_last_marker_delta = marker_delta;
	enc0_rmt_accept_count++;
	enc0_index_hunt_rmt_compare_to_pcnt_index(pcnt_now);
	enc0_index_hunt_store_sample(ENC0_INDEX_MODE_RMT_MARKER, pcnt_now - marker_delta, 0, 0, 0, 0, true);
	return;
#endif

	if (!enc0_rmt_intervals_pending)
	{
		return;
	}
	enc0_rmt_intervals_pending = 0;

	if (enc0_rmt_interval_count < 4)
	{
		return;
	}

	for (i = 0; i < enc0_rmt_interval_count; i++)
	{
		uint8_t oldest = (enc0_rmt_interval_count == ENC0_RMT_INTERVAL_RING) ? enc0_rmt_interval_head : 0;
		local[i] = enc0_rmt_intervals[(uint8_t)((oldest + i) % ENC0_RMT_INTERVAL_RING)];
	}

	for (i = 0; i + 1U < enc0_rmt_interval_count; i++)
	{
		uint32_t a = local[i];
		uint32_t b = local[i + 1U];
		uint32_t local_avg = enc0_index_hunt_rmt_local_avg(local, enc0_rmt_interval_count, i);
		bool short_before_long = false;

		if (enc0_index_hunt_rmt_is_marker(a, b, local_avg, &short_before_long))
		{
			uint32_t short_ticks = short_before_long ? a : b;
			uint32_t long_ticks = short_before_long ? b : a;
			int32_t pcnt_now = encoder_get_position(ENC0);
			int32_t pcnt_wrapped = enc0_wrap_add(0, pcnt_now, (int32_t)ENC0_READ_WRAP);
			int32_t ref_wrapped = enc0_wrap_add(0, enc0_rmt_last_marker_pcnt, (int32_t)ENC0_READ_WRAP);
			int32_t marker_delta = enc0_rmt_have_marker_ref ? enc0_wrap_delta(pcnt_wrapped, ref_wrapped, (int32_t)ENC0_READ_WRAP) : 0;
			uint32_t abs_marker_delta = (uint32_t)ABS(marker_delta);
			uint32_t min_delta = esp32_pcnt_encoder_index_min_delta();
			uint32_t max_delta = esp32_pcnt_encoder_index_max_delta();

			enc0_rmt_candidate_count++;
			enc0_rmt_last_min_delta = min_delta;
			enc0_rmt_last_max_delta = max_delta;

			if (enc0_rmt_have_marker_ref && abs_marker_delta < ENC0_INDEX_HUNT_MIN_MARKER_DELTA)
			{
				enc0_rmt_last_near_delta = marker_delta;
				enc0_rmt_reject_near_count++;
				return;
			}
			if (enc0_rmt_have_marker_ref && (abs_marker_delta < min_delta || abs_marker_delta > max_delta))
			{
				enc0_rmt_last_far_delta = marker_delta;
				enc0_rmt_reject_far_count++;
#if ENC0_INDEX_HUNT_RESYNC_ON_FAR
				if (abs_marker_delta > max_delta)
				{
					enc0_rmt_last_marker_pcnt = pcnt_now;
					enc0_rmt_last_marker_delta = marker_delta;
				}
#endif
				return;
			}

			enc0_rmt_have_marker_ref = true;
			enc0_rmt_last_marker_pcnt = pcnt_now;
			enc0_rmt_last_marker_delta = marker_delta;
			enc0_rmt_accept_count++;
			enc0_rmt_last_avg_ticks = local_avg;
			enc0_rmt_last_short_ticks = short_ticks;
			enc0_rmt_last_long_ticks = long_ticks;
			enc0_index_hunt_rmt_compare_to_pcnt_index(pcnt_now);
			enc0_index_hunt_store_sample(ENC0_INDEX_MODE_RMT_MARKER, pcnt_now - marker_delta, 0, local_avg, short_ticks, long_ticks, short_before_long);
			return;
		}
	}
}

static void enc0_index_hunt_rmt_drain(void)
{
	size_t rx_size = 0;
	rmt_item32_t *items;
	uint32_t count;
	uint32_t i;
	uint8_t got_packet = 0;

	// ESP-IDF legacy RMT RX hands symbols to a ring buffer; this keeps the
	// ISR/callback side equivalent to "copy symbols and flag work" only.
	// Marker detection and PCNT reads stay here in the normal encoder task.
	if (!enc0_rmt_ringbuf)
	{
		return;
	}

	for (;;)
	{
		items = (rmt_item32_t *)xRingbufferReceive(enc0_rmt_ringbuf, &rx_size, 0);
		if (!items)
		{
			break;
		}

		got_packet = 1;
		count = rx_size / sizeof(rmt_item32_t);
		for (i = 0; i < count; i++)
		{
			enc0_index_hunt_rmt_push_interval(items[i].duration0);
			enc0_index_hunt_rmt_push_interval(items[i].duration1);
		}
		vRingbufferReturnItem(enc0_rmt_ringbuf, (void *)items);

		if (count)
		{
			enc0_rmt_event_count++;
			enc0_rmt_packet_count++;
			enc0_rmt_symbol_count += count;
			enc0_rmt_intervals_pending = 1;
		}
	}

	if (!got_packet)
	{
		enc0_rmt_empty_poll_count++;
	}
}

static void enc0_index_hunt_rmt_status(void)
{
#ifdef ENC0_INDEX_HUNT_DEBUG
	uint32_t now_ms = mcu_millis();

	if ((uint32_t)(now_ms - enc0_rmt_last_status_ms) < ENC0_INDEX_HUNT_STATUS_MS)
	{
		return;
	}
	enc0_rmt_last_status_ms = now_ms;

	proto_info("MSG:[IDXHUNT RMT] pcnt=%ld pkts=%lu syms=%lu cand=%lu ok=%lu near=%lu nd=%ld far=%lu fd=%ld empty=%lu ring=%u md=%ld min=%lu max=%lu cmp=%ld cmin=%ld cmax=%ld cn=%lu crj=%lu cwin=%lu avg=%lu sib=%u sp=%u abz=%u short=%lu long=%lu idle=%lu buf=%u mind=%lu rsync=%u",
			   (long)encoder_get_position(ENC0),
			   (unsigned long)enc0_rmt_packet_count,
			   (unsigned long)enc0_rmt_symbol_count,
			   (unsigned long)enc0_rmt_candidate_count,
			   (unsigned long)enc0_rmt_accept_count,
			   (unsigned long)enc0_rmt_reject_near_count,
			   (long)enc0_rmt_last_near_delta,
			   (unsigned long)enc0_rmt_reject_far_count,
			   (long)enc0_rmt_last_far_delta,
			   (unsigned long)enc0_rmt_empty_poll_count,
			   enc0_rmt_interval_count,
			   (long)enc0_rmt_last_marker_delta,
			   (unsigned long)enc0_rmt_last_min_delta,
			   (unsigned long)enc0_rmt_last_max_delta,
			   (long)enc0_rmt_last_pcnt_compare_delta,
			   (long)enc0_rmt_min_pcnt_compare_delta,
			   (long)enc0_rmt_max_pcnt_compare_delta,
			   (unsigned long)enc0_rmt_compare_count,
			   (unsigned long)enc0_rmt_compare_reject_count,
			   (unsigned long)ENC0_INDEX_HUNT_COMPARE_MAX_DELTA,
			   (unsigned long)enc0_rmt_last_avg_ticks,
			   (unsigned)ENC0_RMT_LOCAL_AVG_SIBLINGS,
			   (unsigned)ENC0_RMT_MARKER_SINGLE_PULSE,
			   (unsigned)ENC0_RMT_ABZ_CAPTURE,
			   (unsigned long)enc0_rmt_last_short_ticks,
			   (unsigned long)enc0_rmt_last_long_ticks,
			   (unsigned long)ENC0_INDEX_RMT_IDLE_THRESHOLD,
			   (unsigned)ENC0_INDEX_RMT_SYMBOL_BUF,
			   (unsigned long)ENC0_INDEX_HUNT_MIN_MARKER_DELTA,
			   (unsigned)ENC0_INDEX_HUNT_RESYNC_ON_FAR);
#if ENC0_RMT_ABZ_CAPTURE
	proto_info("MSG:[IDXHUNT ABZ] A:pkts=%lu syms=%lu edges=%lu pcnt=%ld d0=%lu d1=%lu us=%lu B:pkts=%lu syms=%lu edges=%lu pcnt=%ld d0=%lu d1=%lu us=%lu",
			   (unsigned long)enc0_rmt_a_stream.packet_count,
			   (unsigned long)enc0_rmt_a_stream.symbol_count,
			   (unsigned long)enc0_rmt_a_stream.edge_count,
			   (long)enc0_rmt_a_stream.last_pcnt,
			   (unsigned long)enc0_rmt_a_stream.last_duration0,
			   (unsigned long)enc0_rmt_a_stream.last_duration1,
			   (unsigned long)enc0_rmt_a_stream.last_us,
			   (unsigned long)enc0_rmt_b_stream.packet_count,
			   (unsigned long)enc0_rmt_b_stream.symbol_count,
			   (unsigned long)enc0_rmt_b_stream.edge_count,
			   (long)enc0_rmt_b_stream.last_pcnt,
			   (unsigned long)enc0_rmt_b_stream.last_duration0,
			   (unsigned long)enc0_rmt_b_stream.last_duration1,
			   (unsigned long)enc0_rmt_b_stream.last_us);
#endif
#endif
}

static void enc0_index_hunt_rmt_init(void)
{
	rmt_config_t config = {
		.rmt_mode = RMT_MODE_RX,
		.channel = ENC0_INDEX_RMT_CHANNEL,
		.gpio_num = ENC0_INDEX_RMT_GPIO,
		.clk_div = ENC0_INDEX_RMT_CLK_DIV,
		.mem_block_num = 1,
		.rx_config = {
			.filter_en = false,
			.filter_ticks_thresh = 0,
			.idle_threshold = ENC0_INDEX_RMT_IDLE_THRESHOLD,
		},
	};

	rmt_config(&config);
	rmt_driver_install(config.channel, ENC0_INDEX_RMT_SYMBOL_BUF * sizeof(rmt_item32_t), 0);
	rmt_get_ringbuf_handle(config.channel, &enc0_rmt_ringbuf);
	rmt_rx_start(config.channel, true);

#ifdef ENC0_INDEX_HUNT_DEBUG
	proto_info("MSG:[IDXHUNT RMT init] gpio=%d ch=%d clkdiv=%u idle=%lu buf=%u",
			   (int)ENC0_INDEX_RMT_GPIO,
			   (int)ENC0_INDEX_RMT_CHANNEL,
			   (unsigned)ENC0_INDEX_RMT_CLK_DIV,
			   (unsigned long)ENC0_INDEX_RMT_IDLE_THRESHOLD,
			   (unsigned)ENC0_INDEX_RMT_SYMBOL_BUF);
#endif
}
#endif

#if ENC0_INDEX_MODE == ENC0_INDEX_MODE_DFF_LATCH
#ifndef ENC0_INDEX_DFF_Q_PIN
#define ENC0_INDEX_DFF_Q_PIN ENC0_INDEX
#endif

#ifdef ENC0_INDEX_DFF_CLEAR_PIN
static void enc0_index_hunt_dff_clear(void)
{
	io_set_output(ENC0_INDEX_DFF_CLEAR_PIN);
	io_clear_output(ENC0_INDEX_DFF_CLEAR_PIN);
}
#endif

static void enc0_index_hunt_dff_poll(void)
{
	if (!io_get_input(ENC0_INDEX_DFF_Q_PIN))
	{
		return;
	}
	enc0_index_hunt_store_sample(ENC0_INDEX_MODE_DFF_LATCH, encoder_get_position(ENC0), 0, 0, 0, 0, true);
#ifdef ENC0_INDEX_DFF_CLEAR_PIN
	enc0_index_hunt_dff_clear();
#endif
}
#endif

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

static uint32_t esp32_pcnt_encoder_index_min_delta(void)
{
#ifdef ENC0_INDEX_MIN_DELTA
	return (uint32_t)ENC0_INDEX_MIN_DELTA;
#else
	uint32_t resolution = esp32_pcnt_encoder_index_resolution();
	return (resolution) ? ((resolution * 3U) / 4U) : 1U;
#endif
}

static uint32_t esp32_pcnt_encoder_index_max_delta(void)
{
#ifdef ENC0_INDEX_MAX_DELTA
	return (uint32_t)ENC0_INDEX_MAX_DELTA;
#else
	uint32_t resolution = esp32_pcnt_encoder_index_resolution();
	return (resolution) ? ((resolution * 5U) / 4U) : 2147483647UL;
#endif
}

static void esp32_pcnt_encoder_update_index_debug(void)
{
	int32_t live_delta;
	uint32_t avg10;

	if (!esp32_pcnt_index_have_origin && !esp32_pcnt_index_hw_count)
	{
		return;
	}

	if (esp32_pcnt_index_debug_reported_count == esp32_pcnt_index_count &&
		esp32_pcnt_index_debug_reported_hw_count == esp32_pcnt_index_hw_count &&
		esp32_pcnt_index_debug_reported_ignored_count == esp32_pcnt_index_ignored_count &&
		esp32_pcnt_index_debug_reported_bad_count == esp32_pcnt_index_bad_count &&
		esp32_pcnt_index_debug_reported_missed_count == esp32_pcnt_index_missed_count)
	{
		return;
	}

	esp32_pcnt_index_debug_reported_count = esp32_pcnt_index_count;
	esp32_pcnt_index_debug_reported_hw_count = esp32_pcnt_index_hw_count;
	esp32_pcnt_index_debug_reported_ignored_count = esp32_pcnt_index_ignored_count;
	esp32_pcnt_index_debug_reported_bad_count = esp32_pcnt_index_bad_count;
	esp32_pcnt_index_debug_reported_missed_count = esp32_pcnt_index_missed_count;

	live_delta = encoder_get_position(ENC0) - esp32_pcnt_index_last_position;
	avg10 = (esp32_pcnt_index_count) ? (uint32_t)(((uint64_t)esp32_pcnt_index_abs_delta_sum * 10ULL + (esp32_pcnt_index_count / 2U)) / esp32_pcnt_index_count) : 0;

	snprintf(esp32_pcnt_index_debug_line,
			 sizeof(esp32_pcnt_index_debug_line),
			 "ENCIDX EC:%ld ECB:%ld LAST:%ld AVG:%lu.%lu MIN:%ld MAX:%ld N:%lu HW:%lu IGN:%lu BAD:%lu MISS:%lu",
			 (long)encoder_get_position(ENC0),
			 (long)live_delta,
			 (long)esp32_pcnt_index_last_delta,
			 (unsigned long)(avg10 / 10U),
			 (unsigned long)(avg10 % 10U),
			 (long)esp32_pcnt_index_min_delta,
			 (long)esp32_pcnt_index_max_delta,
			 (unsigned long)esp32_pcnt_index_count,
			 (unsigned long)esp32_pcnt_index_hw_count,
			 (unsigned long)esp32_pcnt_index_ignored_count,
			 (unsigned long)esp32_pcnt_index_bad_count,
			 (unsigned long)esp32_pcnt_index_missed_count);

	esp32_pcnt_index_debug_seq++;
}

static void esp32_pcnt_encoder_invoke_virtual_index(void)
{
#ifdef ENC0_INDEX_PCNT_ENABLED
	HOOK_INVOKE(enc0_index);
#endif
}

static void esp32_pcnt_encoder_process_index(void)
{
	uint8_t pending;
	int32_t position;
	int32_t delta;
	uint32_t abs_delta;
	uint32_t min_delta;
	uint32_t max_delta;

	pending = esp32_pcnt_index_pending;
	if (!pending)
	{
		return;
	}

	esp32_pcnt_index_pending = 0;
	position = encoder_get_position(ENC0);

	if (!esp32_pcnt_index_have_origin)
	{
		esp32_pcnt_index_last_position = position;
		esp32_pcnt_index_have_origin = true;
#if ENC0_INDEX_MODE == ENC0_INDEX_MODE_PCNT_INDEX
		enc0_index_hunt_store_sample(ENC0_INDEX_MODE_PCNT_INDEX, position, 0, 0, 0, 0, true);
#elif ENC0_INDEX_MODE == ENC0_INDEX_MODE_DFF_LATCH
		enc0_index_hunt_store_sample(ENC0_INDEX_MODE_DFF_LATCH, position, 0, 0, 0, 0, true);
#endif
		esp32_pcnt_encoder_update_index_debug();
		return;
	}

	delta = position - esp32_pcnt_index_last_position;
	abs_delta = (uint32_t)ABS(delta);
	min_delta = esp32_pcnt_encoder_index_min_delta();
	max_delta = esp32_pcnt_encoder_index_max_delta();
	if (abs_delta < min_delta)
	{
		esp32_pcnt_index_ignored_count++;
		esp32_pcnt_encoder_update_index_debug();
		return;
	}
	if (abs_delta > max_delta)
	{
		esp32_pcnt_index_missed_count++;
		esp32_pcnt_index_resync_after_miss = false;
		esp32_pcnt_index_last_position = position;
		esp32_pcnt_encoder_update_index_debug();
		return;
	}

	esp32_pcnt_index_resync_after_miss = false;
	esp32_pcnt_index_last_position = position;
	esp32_pcnt_index_last_delta = delta;
#if ENC0_INDEX_MODE == ENC0_INDEX_MODE_PCNT_INDEX
	enc0_index_hunt_store_sample(ENC0_INDEX_MODE_PCNT_INDEX, position, 0, 0, 0, 0, true);
#elif ENC0_INDEX_MODE == ENC0_INDEX_MODE_DFF_LATCH
	enc0_index_hunt_store_sample(ENC0_INDEX_MODE_DFF_LATCH, position, 0, 0, 0, 0, true);
#endif
	esp32_pcnt_index_count++;
	esp32_pcnt_index_abs_delta_sum += (uint32_t)ABS(delta);

	if (!esp32_pcnt_index_have_stats)
	{
		esp32_pcnt_index_min_delta = delta;
		esp32_pcnt_index_max_delta = delta;
		esp32_pcnt_index_have_stats = true;
	}
	else
	{
		if (delta < esp32_pcnt_index_min_delta)
		{
			esp32_pcnt_index_min_delta = delta;
		}
		if (delta > esp32_pcnt_index_max_delta)
		{
			esp32_pcnt_index_max_delta = delta;
		}
	}

	esp32_pcnt_encoder_update_index_debug();
}

static void esp32_pcnt_encoder_latch_index(void)
{
	if (esp32_pcnt_index_pending < 255)
	{
		esp32_pcnt_index_pending++;
	}
}

#ifndef ENC0_INDEX_PCNT_ENABLED
static void esp32_pcnt_encoder_on_index(void)
{
	esp32_pcnt_index_hw_count++;
	esp32_pcnt_encoder_latch_index();
}
#endif

#ifdef ENC0_INDEX_PCNT_ENABLED
static void encoder_esp32_index_pcnt_init(uint8_t unit, int index_gpio)
{
	pcnt_config_t index_ch = {
		.pulse_gpio_num = index_gpio,
		.ctrl_gpio_num = PCNT_PIN_NOT_USED,
		.lctrl_mode = PCNT_MODE_KEEP,
		.hctrl_mode = PCNT_MODE_KEEP,
		.pos_mode = PCNT_COUNT_INC,
		.neg_mode = PCNT_COUNT_DIS,
		.counter_h_lim = 32767,
		.counter_l_lim = 0,
		.unit = (pcnt_unit_t)unit,
		.channel = PCNT_CHANNEL_0,
	};

	pcnt_unit_config(&index_ch);

#ifdef ENC0_INDEX_PCNT_FILTER
	pcnt_set_filter_value((pcnt_unit_t)unit, ENC0_INDEX_PCNT_FILTER);
	pcnt_filter_enable((pcnt_unit_t)unit);
#elif defined(ENCODER_PCNT_FILTER)
	pcnt_set_filter_value((pcnt_unit_t)unit, ENCODER_PCNT_FILTER);
	pcnt_filter_enable((pcnt_unit_t)unit);
#else
	pcnt_filter_disable((pcnt_unit_t)unit);
#endif

	pcnt_counter_pause((pcnt_unit_t)unit);
	pcnt_counter_clear((pcnt_unit_t)unit);
	pcnt_counter_resume((pcnt_unit_t)unit);
}

static void esp32_pcnt_encoder_drain_index_pcnt(void)
{
	int16_t index_count = 0;

	if (!esp32_pcnt_encoder_ready)
	{
		return;
	}

	pcnt_get_counter_value((pcnt_unit_t)ENC0_INDEX_PCNT_UNIT, &index_count);
	if (index_count <= 0)
	{
		return;
	}

	pcnt_counter_pause((pcnt_unit_t)ENC0_INDEX_PCNT_UNIT);
	pcnt_counter_clear((pcnt_unit_t)ENC0_INDEX_PCNT_UNIT);
	pcnt_counter_resume((pcnt_unit_t)ENC0_INDEX_PCNT_UNIT);

	esp32_pcnt_index_hw_count += (uint32_t)index_count;
	if (index_count > 1)
	{
		esp32_pcnt_index_missed_count += (uint32_t)(index_count - 1);
		esp32_pcnt_index_resync_after_miss = true;
	}
	esp32_pcnt_encoder_latch_index();
	esp32_pcnt_encoder_update_index_debug();
	esp32_pcnt_encoder_invoke_virtual_index();
}
#endif

static void encoder_esp32_pcnt_init(uint8_t unit, int pulse_gpio, int dir_gpio)
{
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
	if (i != ENC0 || !esp32_pcnt_index_have_stats)
	{
		return false;
	}

	if (last)
	{
		*last = esp32_pcnt_index_last_delta;
	}
	if (min)
	{
		*min = esp32_pcnt_index_min_delta;
	}
	if (max)
	{
		*max = esp32_pcnt_index_max_delta;
	}
	if (count)
	{
		*count = esp32_pcnt_index_count;
	}

	return true;
}

bool encoder_get_index_live_delta(uint8_t i, int32_t *delta)
{
	if (i != ENC0 || !esp32_pcnt_index_have_origin || !delta)
	{
		return false;
	}

	*delta = encoder_get_position(ENC0) - esp32_pcnt_index_last_position;
	return true;
}

bool encoder_get_index_debug_line(uint8_t i, char *line, uint32_t line_len, uint32_t *seq)
{
	if (i != ENC0 || !line || line_len == 0 || !esp32_pcnt_index_debug_seq)
	{
		return false;
	}

	strncpy(line, esp32_pcnt_index_debug_line, line_len - 1);
	line[line_len - 1] = '\0';
	if (seq)
	{
		*seq = esp32_pcnt_index_debug_seq;
	}
	return true;
}

static bool esp32_pcnt_encoder_dotasks(void *args)
{
	(void)args;
#ifdef ENC0_INDEX_PCNT_ENABLED
	esp32_pcnt_encoder_drain_index_pcnt();
#endif
	esp32_pcnt_encoder_process_index();
#if ENC0_INDEX_SOFT_POLL_ENABLE
	enc0_index_hunt_soft_poll();
	enc0_index_hunt_soft_poll_status();
#endif
#if ENC0_INDEX_MODE == ENC0_INDEX_MODE_RMT_MARKER
#if ENC0_RMT_ABZ_CAPTURE
	enc0_index_hunt_rmt_abz_drain();
#endif
	enc0_index_hunt_rmt_drain();
	enc0_index_hunt_rmt_detect();
	enc0_index_hunt_rmt_status();
#elif ENC0_INDEX_MODE == ENC0_INDEX_MODE_DFF_LATCH
	enc0_index_hunt_dff_poll();
#endif
	esp32_pcnt_encoder_update_index_debug();
	return EVENT_CONTINUE;
}

CREATE_EVENT_LISTENER(cnc_io_dotasks, esp32_pcnt_encoder_dotasks);

DECL_MODULE(esp32_pcnt_encoder)
{
	encoder_esp32_pcnt_init(ENC0_PCNT_UNIT, ENC0_PULSE_GPIO, ENC0_DIR_GPIO);
#ifdef ENC0_INDEX_PCNT_ENABLED
	encoder_esp32_index_pcnt_init(ENC0_INDEX_PCNT_UNIT, ENC0_INDEX_GPIO);
#else
	HOOK_ATTACH_CALLBACK(enc0_index, esp32_pcnt_encoder_on_index);
#endif
#if ENC0_INDEX_MODE == ENC0_INDEX_MODE_RMT_MARKER
	enc0_index_hunt_rmt_init();
#if ENC0_RMT_ABZ_CAPTURE
	enc0_index_hunt_rmt_abz_init();
#endif
#endif
	ADD_EVENT_LISTENER(cnc_io_dotasks, esp32_pcnt_encoder_dotasks);
	esp32_pcnt_encoder_ready = true;
}

#else

DECL_MODULE(esp32_pcnt_encoder)
{
}

#endif
