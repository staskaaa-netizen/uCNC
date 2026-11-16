/*
	Name: rp2350_pio_encoder.c
	Description: RP2040/RP2350 PIO hardware-assisted encoder backend for uCNC.
*/

#include "../../cnc.h"
#include "../encoder.h"

#ifndef ENCODER_HW_PIO
#define ENCODER_HW_PIO 2
#endif

#define RP2350_ENCODER_MODE_QUADRATURE 0
#define RP2350_ENCODER_MODE_PULSE 1

#ifndef ENC0_RP2350_ENCODER_MODE
#define ENC0_RP2350_ENCODER_MODE RP2350_ENCODER_MODE_QUADRATURE
#endif
#ifndef ENC1_RP2350_ENCODER_MODE
#define ENC1_RP2350_ENCODER_MODE RP2350_ENCODER_MODE_QUADRATURE
#endif
#ifndef ENC2_RP2350_ENCODER_MODE
#define ENC2_RP2350_ENCODER_MODE RP2350_ENCODER_MODE_QUADRATURE
#endif

#if defined(ENABLE_RP2350_PIO_ENCODER) && !defined(ENC0_USE_HARDWARE_COUNTER)
#define ENC0_USE_HARDWARE_COUNTER 1
#endif

#if defined(ENABLE_RP2350_PIO_ENCODER) && !defined(ENC0_HW_COUNTER_TYPE)
#define ENC0_HW_COUNTER_TYPE ENCODER_HW_PIO
#endif

#if defined(ENCODER0_USE_HARDWARE_COUNTER) && !defined(ENC0_USE_HARDWARE_COUNTER)
#define ENC0_USE_HARDWARE_COUNTER ENCODER0_USE_HARDWARE_COUNTER
#endif

#if defined(ENCODER0_HW_COUNTER_TYPE) && !defined(ENC0_HW_COUNTER_TYPE)
#define ENC0_HW_COUNTER_TYPE ENCODER0_HW_COUNTER_TYPE
#endif

#if defined(ENC0_USE_HARDWARE_COUNTER) && ENC0_USE_HARDWARE_COUNTER && !defined(ENC0_TYPE)
#define ENC0_TYPE ENC_TYPE_CUSTOM
#define ENC0_IS_INCREMENTAL
#define ENC0_NO_WRAP_CORRECTION
#endif

#if (ENCODERS > 0) && (ENC0_TYPE == ENC_TYPE_CUSTOM) && defined(ENC0_USE_HARDWARE_COUNTER) && (ENC0_USE_HARDWARE_COUNTER) && (ENC0_HW_COUNTER_TYPE == ENCODER_HW_PIO) && (MCU == MCU_RP2040 || MCU == MCU_RP2350 || defined(PICO_RP2040) || defined(PICO_RP2350))

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#ifndef ENC0_PULSE_GPIO
#error "ENC0_PULSE_GPIO must be defined for RP PIO encoder"
#endif

#ifndef ENC0_PIO_INDEX
#define ENC0_PIO_INDEX 0
#endif
#ifndef ENC0_PIO_SM
#define ENC0_PIO_SM 0
#endif
#ifndef ENC0_PIO_PROGRAM_OFFSET
#define ENC0_PIO_PROGRAM_OFFSET 0
#endif
#ifndef ENC0_MAX_STEP_RATE
#define ENC0_MAX_STEP_RATE 0
#endif
#ifndef ENC0_RP2350_ENCODER_INVERT
#define ENC0_RP2350_ENCODER_INVERT 0
#endif
#ifndef ENC0_INDEX_GPIO
#define ENC0_INDEX_GPIO 255
#endif

#if ENCODERS > 1
#ifndef ENC1_PIO_INDEX
#define ENC1_PIO_INDEX ENC0_PIO_INDEX
#endif
#ifndef ENC1_PIO_SM
#define ENC1_PIO_SM 1
#endif
#ifndef ENC1_PIO_PROGRAM_OFFSET
#define ENC1_PIO_PROGRAM_OFFSET ENC0_PIO_PROGRAM_OFFSET
#endif
#ifndef ENC1_MAX_STEP_RATE
#define ENC1_MAX_STEP_RATE 0
#endif
#ifndef ENC1_RP2350_ENCODER_INVERT
#define ENC1_RP2350_ENCODER_INVERT 0
#endif
#ifndef ENC1_INDEX_GPIO
#define ENC1_INDEX_GPIO 255
#endif
#endif

#if ENCODERS > 2
#ifndef ENC2_PIO_INDEX
#define ENC2_PIO_INDEX ENC0_PIO_INDEX
#endif
#ifndef ENC2_PIO_SM
#define ENC2_PIO_SM 2
#endif
#ifndef ENC2_PIO_PROGRAM_OFFSET
#define ENC2_PIO_PROGRAM_OFFSET ENC0_PIO_PROGRAM_OFFSET
#endif
#ifndef ENC2_MAX_STEP_RATE
#define ENC2_MAX_STEP_RATE 0
#endif
#ifndef ENC2_RP2350_ENCODER_INVERT
#define ENC2_RP2350_ENCODER_INVERT 0
#endif
#ifndef ENC2_INDEX_GPIO
#define ENC2_INDEX_GPIO 255
#endif
#endif

/* RP2350 PIO addresses either GPIO0-31 or GPIO16-47. */
#ifndef RP2350_PIO_ENCODER_GPIO_BASE
#if (ENC0_PULSE_GPIO >= 32)
#define RP2350_PIO_ENCODER_GPIO_BASE 16
#else
#define RP2350_PIO_ENCODER_GPIO_BASE 0
#endif
#endif

typedef struct
{
	uint8_t mode;
	uint8_t sm;
	uint8_t pin;
	uint8_t invert;
	int16_t index_gpio;
	int max_step_rate;
	PIO pio;
	bool ready;
	uint32_t hw_count;
	int32_t pulse_position;
	uint8_t pulse_last;
} rp_pio_encoder_t;

#if ENCODERS > 0
static rp_pio_encoder_t rp_enc0 = {
	.mode = ENC0_RP2350_ENCODER_MODE,
	.sm = ENC0_PIO_SM,
	.pin = ENC0_PULSE_GPIO,
	.invert = ENC0_RP2350_ENCODER_INVERT,
	.index_gpio = ENC0_INDEX_GPIO,
	.max_step_rate = ENC0_MAX_STEP_RATE,
};
#endif
#if ENCODERS > 1
static rp_pio_encoder_t rp_enc1 = {
	.mode = ENC1_RP2350_ENCODER_MODE,
	.sm = ENC1_PIO_SM,
	.pin = ENC1_PULSE_GPIO,
	.invert = ENC1_RP2350_ENCODER_INVERT,
	.index_gpio = ENC1_INDEX_GPIO,
	.max_step_rate = ENC1_MAX_STEP_RATE,
};
#endif
#if ENCODERS > 2
static rp_pio_encoder_t rp_enc2 = {
	.mode = ENC2_RP2350_ENCODER_MODE,
	.sm = ENC2_PIO_SM,
	.pin = ENC2_PULSE_GPIO,
	.invert = ENC2_RP2350_ENCODER_INVERT,
	.index_gpio = ENC2_INDEX_GPIO,
	.max_step_rate = ENC2_MAX_STEP_RATE,
};
#endif

static bool rp_pio_encoder_initialized;

static const uint16_t quadrature_encoder_program_instructions[] = {
	0x000f, 0x000e, 0x0015, 0x000f,
	0x0015, 0x000f, 0x000f, 0x000e,
	0x000e, 0x000f, 0x000f, 0x0015,
	0x000f, 0x0015,
	0x008f,
	0xa0c2,
	0x8000,
	0x60c2,
	0x4002,
	0xa0e6,
	0xa0a6,
	0xa04a,
	0x0097,
	0xa04a,
};

static const struct pio_program quadrature_encoder_program = {
	.instructions = quadrature_encoder_program_instructions,
	.length = 24,
	.origin = ENC0_PIO_PROGRAM_OFFSET,
};

static PIO rp_pio_get(uint8_t index)
{
#if NUM_PIOS > 2
	if (index == 2)
	{
		return pio2;
	}
#endif
#if NUM_PIOS > 1
	if (index == 1)
	{
		return pio1;
	}
#endif
	return pio0;
}

static void raster_load_quadrature_program(PIO pio)
{
#if PICO_PIO_USE_GPIO_BASE
	pio_set_gpio_base(pio, RP2350_PIO_ENCODER_GPIO_BASE);
#endif
	pio_add_program_at_offset(pio, &quadrature_encoder_program,
							  ENC0_PIO_PROGRAM_OFFSET);
}

static void quadrature_encoder_program_init_inline(rp_pio_encoder_t *encoder)
{
	PIO pio = encoder->pio;
	uint sm = encoder->sm;
	uint pin = encoder->pin;
	pio_sm_config c;

	pio_sm_set_consecutive_pindirs(pio, sm, pin, 2, false);
	pio_gpio_init(pio, pin);
	pio_gpio_init(pio, pin + 1);
	gpio_pull_up(pin);
	gpio_pull_up(pin + 1);

	c = pio_get_default_sm_config();
	sm_config_set_wrap(&c, ENC0_PIO_PROGRAM_OFFSET + 15,
					   ENC0_PIO_PROGRAM_OFFSET + 23);
	sm_config_set_in_pins(&c, pin);
	sm_config_set_jmp_pin(&c, pin);
	sm_config_set_in_shift(&c, false, false, 32);
	sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_NONE);

	if (encoder->max_step_rate == 0)
	{
		sm_config_set_clkdiv(&c, 1.0f);
	}
	else
	{
		sm_config_set_clkdiv(
			&c, (float)clock_get_hz(clk_sys) /
					(10.0f * (float)encoder->max_step_rate));
	}

	pio_sm_init(pio, sm, ENC0_PIO_PROGRAM_OFFSET, &c);
	pio_sm_set_enabled(pio, sm, true);
}

static void pulse_encoder_init(rp_pio_encoder_t *encoder)
{
	gpio_init(encoder->pin);
	gpio_set_dir(encoder->pin, GPIO_IN);
	gpio_pull_up(encoder->pin);
	encoder->pulse_last = gpio_get(encoder->pin) ? 1U : 0U;
}

static void encoder_index_init(rp_pio_encoder_t *encoder)
{
	int16_t index_gpio = encoder->index_gpio;

	if (index_gpio < 0 || index_gpio == 255)
	{
		return;
	}
	gpio_init((uint)index_gpio);
	gpio_set_dir((uint)index_gpio, GPIO_IN);
	gpio_pull_up((uint)index_gpio);
}

static int32_t quadrature_encoder_get_count(rp_pio_encoder_t *encoder)
{
	uint32_t value = encoder->hw_count;
	uint8_t count = pio_sm_get_rx_fifo_level(encoder->pio, encoder->sm);

	while (count--)
	{
		value = pio_sm_get(encoder->pio, encoder->sm);
	}
	encoder->hw_count = value;
	return (int32_t)value;
}

static int32_t encoder_read(rp_pio_encoder_t *encoder)
{
	int32_t count;

	if (!encoder->ready)
	{
		return 0;
	}

	if (encoder->mode == RP2350_ENCODER_MODE_PULSE)
	{
		uint8_t now = gpio_get(encoder->pin) ? 1U : 0U;
		if (now && !encoder->pulse_last)
		{
			encoder->pulse_position++;
		}
		encoder->pulse_last = now;
		return encoder->pulse_position;
	}

	count = quadrature_encoder_get_count(encoder);
	return encoder->invert ? (int32_t)(0u - (uint32_t)count) : count;
}

static void encoder_index_isr(uint gpio, uint32_t events)
{
	(void)events;
#if ENCODERS > 0
	if ((int16_t)gpio == rp_enc0.index_gpio)
	{
		encoder_record_index_reference(ENC0, encoder_read(&rp_enc0));
	}
#endif
#if ENCODERS > 1
	else if ((int16_t)gpio == rp_enc1.index_gpio)
	{
		encoder_record_index_reference(ENC1, encoder_read(&rp_enc1));
	}
#endif
#if ENCODERS > 2
	else if ((int16_t)gpio == rp_enc2.index_gpio)
	{
		encoder_record_index_reference(ENC2, encoder_read(&rp_enc2));
	}
#endif
}

static void encoder_index_irq_enable(rp_pio_encoder_t *encoder)
{
	if (encoder->index_gpio < 0 || encoder->index_gpio == 255)
	{
		return;
	}
	gpio_set_irq_enabled_with_callback((uint)encoder->index_gpio,
									   GPIO_IRQ_EDGE_RISE, true,
									   &encoder_index_isr);
}

static void encoder_rp_pio_init(void)
{
#if ENCODERS > 0
	rp_enc0.pio = rp_pio_get(ENC0_PIO_INDEX);
	if (rp_enc0.mode == RP2350_ENCODER_MODE_PULSE)
	{
		pulse_encoder_init(&rp_enc0);
	}
	else
	{
		raster_load_quadrature_program(rp_enc0.pio);
		quadrature_encoder_program_init_inline(&rp_enc0);
	}
	encoder_index_init(&rp_enc0);
	rp_enc0.ready = true;
#endif
#if ENCODERS > 1
	rp_enc1.pio = rp_pio_get(ENC1_PIO_INDEX);
	if (rp_enc1.mode == RP2350_ENCODER_MODE_PULSE)
	{
		pulse_encoder_init(&rp_enc1);
	}
	else
	{
		raster_load_quadrature_program(rp_enc1.pio);
		quadrature_encoder_program_init_inline(&rp_enc1);
	}
	encoder_index_init(&rp_enc1);
	rp_enc1.ready = true;
#endif
#if ENCODERS > 2
	rp_enc2.pio = rp_pio_get(ENC2_PIO_INDEX);
	if (rp_enc2.mode == RP2350_ENCODER_MODE_PULSE)
	{
		pulse_encoder_init(&rp_enc2);
	}
	else
	{
		quadrature_encoder_program_init_inline(&rp_enc2);
	}
	encoder_index_init(&rp_enc2);
	rp_enc2.ready = true;
#endif

#if ENCODERS > 0
	encoder_index_irq_enable(&rp_enc0);
#endif
#if ENCODERS > 1
	encoder_index_irq_enable(&rp_enc1);
#endif
#if ENCODERS > 2
	encoder_index_irq_enable(&rp_enc2);
#endif
}

int32_t enc_custom_read(uint8_t i)
{
	switch (i)
	{
#if ENCODERS > 0
	case ENC0:
		return encoder_read(&rp_enc0);
#endif
#if ENCODERS > 1
	case ENC1:
		return encoder_read(&rp_enc1);
#endif
#if ENCODERS > 2
	case ENC2:
		return encoder_read(&rp_enc2);
#endif
	default:
		return 0;
	}
}

DECL_MODULE(rp2350_pio_encoder)
{
	if (rp_pio_encoder_initialized)
	{
		return;
	}

	encoder_rp_pio_init();
	rp_pio_encoder_initialized = true;
}

#else

DECL_MODULE(rp2350_pio_encoder)
{
}

#endif
