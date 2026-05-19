/*
    Name: rp2350_pio_encoder.c
    Description: RP2040/RP2350 PIO-backed custom encoder reader for uCNC.

    ENC0_PULSE_GPIO = encoder A
    encoder B must be ENC0_PULSE_GPIO + 1

    Self-contained:
      - inline PIO program
      - enc_custom_read()
      - optional ENC0_INDEX_GPIO physical index
      - virtual index task
      - index debug helpers
*/

#include "../../cnc.h"
#include "../encoder.h"

#if defined(ENABLE_RP2350_PIO_ENCODER) && (MCU == MCU_RP2040 || MCU == MCU_RP2350 || defined(PICO_RP2040) || defined(PICO_RP2350))

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include <stdio.h>
#include <string.h>

#if (UCNC_MODULE_VERSION < 11501 || UCNC_MODULE_VERSION > 99999)
#error "This module is not compatible with the current version of uCNC"
#endif

#ifndef ENC0_PULSE_GPIO
#error "ENC0_PULSE_GPIO is not defined"
#endif

#ifndef ENC0_PIO_INDEX
#define ENC0_PIO_INDEX 0
#endif

#ifndef ENC0_PIO_SM
#define ENC0_PIO_SM 0
#endif

#ifndef ENC0_MAX_STEP_RATE
#define ENC0_MAX_STEP_RATE 0
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
#define ENC0_VIRTUAL_MAX_CATCHUP_SLOTS 1U
#endif

#ifndef ENC0_INDEX_AUTO_ORIGIN
#ifdef ENC0_INDEX_GPIO
#define ENC0_INDEX_AUTO_ORIGIN 0
#else
#define ENC0_INDEX_AUTO_ORIGIN 1
#endif
#endif

#ifndef ENC0_PIO_PROGRAM_OFFSET
#define ENC0_PIO_PROGRAM_OFFSET 0
#endif

static bool rp_pio_encoder_ready = false;
static bool rp_pio_encoder_banner_printed = false;
static PIO rp_pio_encoder_pio = NULL;

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

static PIO enc0_get_pio(void)
{
#if ENC0_PIO_INDEX == 1
    return pio1;
#else
    return pio0;
#endif
}

static void quadrature_encoder_program_init_inline(PIO pio, uint sm, uint pin, int max_step_rate)
{
    pio_sm_config c;

    pio_sm_set_consecutive_pindirs(pio, sm, pin, 2, false);

    pio_gpio_init(pio, pin);
    pio_gpio_init(pio, pin + 1);

    gpio_pull_up(pin);
    gpio_pull_up(pin + 1);

    c = pio_get_default_sm_config();

    sm_config_set_wrap(&c, 15, 23);
    sm_config_set_in_pins(&c, pin);
    sm_config_set_jmp_pin(&c, pin);
    sm_config_set_in_shift(&c, false, false, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_NONE);

    if (max_step_rate == 0)
    {
        sm_config_set_clkdiv(&c, 1.0f);
    }
    else
    {
        float div = (float)clock_get_hz(clk_sys) / (10.0f * (float)max_step_rate);
        sm_config_set_clkdiv(&c, div);
    }

    pio_sm_init(pio, sm, ENC0_PIO_PROGRAM_OFFSET, &c);
    pio_sm_set_enabled(pio, sm, true);
}

static int32_t quadrature_encoder_get_count_inline(PIO pio, uint sm)
{
    uint32_t ret = 0;
    int n = pio_sm_get_rx_fifo_level(pio, sm) + 1;

    while (n > 0)
    {
        ret = pio_sm_get_blocking(pio, sm);
        n--;
    }

    return (int32_t)ret;
}

static void encoder_rp_pio_init(void)
{
     proto_info("LC_FEATURE:RP2350_PIO_ENCODER init");
    rp_pio_encoder_pio = enc0_get_pio();

    pio_add_program_at_offset(
        rp_pio_encoder_pio,
        &quadrature_encoder_program,
        ENC0_PIO_PROGRAM_OFFSET);

    quadrature_encoder_program_init_inline(
        rp_pio_encoder_pio,
        ENC0_PIO_SM,
        ENC0_PULSE_GPIO,
        ENC0_MAX_STEP_RATE);
}

static int32_t read_encoder_rp_pio(void)
{
    if (!rp_pio_encoder_ready)
    {
        return 0;
    }

    return quadrature_encoder_get_count_inline(
        rp_pio_encoder_pio,
        ENC0_PIO_SM);
}

int32_t enc_custom_read(uint8_t i)
{
    switch (i)
    {
    case ENC0:
        return read_encoder_rp_pio();

    default:
        return 0;
    }
}

static uint32_t rp2350_pio_encoder_index_resolution(void)
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

static void rp2350_pio_encoder_update_index_debug(void)
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
    avg10 = enc0_index_count ? (uint32_t)(((uint64_t)enc0_index_abs_delta_sum * 10ULL + (enc0_index_count / 2U)) / enc0_index_count) : 0;

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

#ifdef ENCODER_DEBUG_PRINT_100MS
    rp2350_pio_encoder_update_index_debug();
#endif
}

void enc0_virtual_index_unarm(void)
{
    enc0_index_have_origin = false;
    enc0_index_have_slot = false;
    enc0_index_last_position = 0;
    enc0_isr_have_ref = false;
}

static void enc0_virtual_index_task(void)
{
    uint32_t enc_res = rp2350_pio_encoder_index_resolution();
    int32_t slot_size;
    int32_t now;
    int32_t rel;
    int32_t slot;
    int32_t slot_delta;
    int32_t direction;
    int32_t emit_count;
    int32_t skipped;

    if (!enc_res)
    {
        return;
    }

    slot_size = (int32_t)((enc_res + (ENC0_VIRTUAL_INDEXES_PER_REV / 2U)) / ENC0_VIRTUAL_INDEXES_PER_REV);

    if (slot_size < 1)
    {
        slot_size = 1;
    }

    now = encoder_get_position(ENC0);

    if (!enc0_index_have_origin)
    {
        if (enc0_isr_have_ref)
        {
            enc0_index_origin = enc0_isr_ref_pcnt;
        }
#if ENC0_INDEX_AUTO_ORIGIN
        else
        {
            enc0_index_origin = now;
        }
#else
        else
        {
            return;
        }
#endif

        enc0_index_last_position = enc0_index_origin;
        enc0_index_have_origin = true;
        enc0_index_have_slot = false;

#ifdef ENCODER_DEBUG_PRINT_100MS
        rp2350_pio_encoder_update_index_debug();
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
    direction = (slot_delta > 0) ? 1 : -1;
    emit_count = (slot_delta > 0) ? slot_delta : -slot_delta;

    if (emit_count > (int32_t)ENC0_VIRTUAL_MAX_CATCHUP_SLOTS)
    {
        skipped = emit_count - (int32_t)ENC0_VIRTUAL_MAX_CATCHUP_SLOTS;
        enc0_index_ignored_count += (uint32_t)skipped;
        enc0_index_last_slot += direction * skipped;
        emit_count = (int32_t)ENC0_VIRTUAL_MAX_CATCHUP_SLOTS;
    }

    while (emit_count--)
    {
        enc0_index_last_slot += direction;
        enc0_accept_virtual_index(enc0_index_origin + enc0_index_last_slot * slot_size);
    }
}

#ifdef ENC0_INDEX_GPIO
static void enc0_index_gpio_isr(uint gpio, uint32_t events)
{
    (void)gpio;
    (void)events;

    enc0_isr_ref_pcnt = encoder_get_position(ENC0);
    enc0_isr_have_ref = 1;
    enc0_isr_count++;
}

static void enc0_index_gpio_isr_init(void)
{
    gpio_init(ENC0_INDEX_GPIO);
    gpio_set_dir(ENC0_INDEX_GPIO, GPIO_IN);
    gpio_pull_up(ENC0_INDEX_GPIO);

    gpio_set_irq_enabled_with_callback(
        ENC0_INDEX_GPIO,
        GPIO_IRQ_EDGE_RISE,
        true,
        &enc0_index_gpio_isr);
}
#endif

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

#ifdef ENCODER_DEBUG_PRINT_100MS
static void rp2350_pio_encoder_print_index_debug(void)
{
    static uint32_t last_seq = 0;
    uint32_t seq = 0;
    char line[128];

    if (encoder_get_index_debug_line(ENC0, line, sizeof(line), &seq) && seq != last_seq)
    {
        last_seq = seq;
        proto_printf("[%s]" MSG_FEEDBACK_END, line);
    }
}
#endif

static bool rp2350_pio_encoder_dotasks(void *args)
{
    (void)args;

#ifdef LEANCAM_BUILD_FEATURE_BANNER
    if (rp_pio_encoder_ready && !rp_pio_encoder_banner_printed && mcu_millis() > 1000)
    {
        rp_pio_encoder_banner_printed = true;
        proto_info("LC_FEATURE:RP2350_PIO_ENCODER ready A=%d B=%d IDX=%d PIO=%d SM=%d CPR=%lu VIDX=%lu AUTO_ORIGIN=%d",
                   (int)ENC0_PULSE_GPIO, (int)(ENC0_PULSE_GPIO + 1), (int)ENC0_INDEX_GPIO,
                   (int)ENC0_PIO_INDEX, (int)ENC0_PIO_SM, (unsigned long)ENC0_CPR,
                   (unsigned long)ENC0_VIRTUAL_INDEXES_PER_REV, (int)ENC0_INDEX_AUTO_ORIGIN);
    }
#endif

    enc0_virtual_index_task();

#ifdef ENCODER_DEBUG_PRINT_100MS
    rp2350_pio_encoder_update_index_debug();
    rp2350_pio_encoder_print_index_debug();
#endif

    return EVENT_CONTINUE;
}

CREATE_EVENT_LISTENER(cnc_io_dotasks, rp2350_pio_encoder_dotasks);

DECL_MODULE(rp2350_pio_encoder)
{
#ifdef LEANCAM_BUILD_FEATURE_BANNER
    proto_info("LC_FEATURE:RP2350_PIO_ENCODER on A=%d B=%d IDX=%d PIO=%d SM=%d CPR=%lu VIDX=%lu AUTO_ORIGIN=%d",
               (int)ENC0_PULSE_GPIO, (int)(ENC0_PULSE_GPIO + 1), (int)ENC0_INDEX_GPIO,
               (int)ENC0_PIO_INDEX, (int)ENC0_PIO_SM, (unsigned long)ENC0_CPR,
               (unsigned long)ENC0_VIRTUAL_INDEXES_PER_REV, (int)ENC0_INDEX_AUTO_ORIGIN);
#endif

    encoder_rp_pio_init();

#ifdef ENC0_INDEX_GPIO
    enc0_index_gpio_isr_init();
#endif

    ADD_EVENT_LISTENER(cnc_io_dotasks, rp2350_pio_encoder_dotasks);

    rp_pio_encoder_ready = true;
}

#else

DECL_MODULE(rp2350_pio_encoder)
{
}

#endif
