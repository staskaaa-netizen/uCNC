#include "lvds_psram.h"

#include <string.h>

#include "../../cnc.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

static bool g_psram_available;
static uint32_t g_psram_clock_hz;

#ifndef LVDS_PSRAM_INIT_ATTEMPTS
#define LVDS_PSRAM_INIT_ATTEMPTS 3
#endif

#ifndef LVDS_PSRAM_POST_INIT_DELAY_MS
#define LVDS_PSRAM_POST_INIT_DELAY_MS 0
#endif

#ifndef LVDS_PSRAM_RETRY_DELAY_MS
#define LVDS_PSRAM_RETRY_DELAY_MS 0
#endif

#if LVDS_PSRAM_POST_INIT_DELAY_MS > 0 || LVDS_PSRAM_RETRY_DELAY_MS > 0
static void lvds_psram_delay_ms(uint32_t ms)
{
    cnc_delay_ms(ms);
}
#endif

static void __no_inline_not_in_flash_func(psram_qmi_init)(uint cs_pin)
{
    gpio_set_function(cs_pin, GPIO_FUNC_XIP_CS1);

    qmi_hw->direct_csr = (10 << QMI_DIRECT_CSR_CLKDIV_LSB) |
                         QMI_DIRECT_CSR_EN_BITS |
                         QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) tight_loop_contents();

    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x35u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) tight_loop_contents();

    const uint32_t clock_hz = clock_get_hz(clk_sys);
    const uint32_t max_psram_freq = 50000000u;
    uint32_t divisor = (clock_hz + max_psram_freq - 1u) / max_psram_freq;
    if (divisor == 1u && clock_hz > 100000000u) {
        divisor = 2u;
    }
    if (divisor == 0u) {
        divisor = 1u;
    }

    uint32_t rxdelay = divisor;
    if (clock_hz / divisor > 100000000u) {
        rxdelay += 1u;
    }

    const uint32_t clock_period_fs = (uint32_t)(1000000000000000ull / clock_hz);
    const uint32_t max_select = (125u * 1000000u) / clock_period_fs;
    const uint32_t min_deselect =
        (18u * 1000000u + (clock_period_fs - 1u)) / clock_period_fs - (divisor + 1u) / 2u;

    qmi_hw->m[1].timing =
        (1u << QMI_M1_TIMING_COOLDOWN_LSB) |
        (QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB) |
        (max_select << QMI_M1_TIMING_MAX_SELECT_LSB) |
        (min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB) |
        (rxdelay << QMI_M1_TIMING_RXDELAY_LSB) |
        (divisor << QMI_M1_TIMING_CLKDIV_LSB);

    qmi_hw->m[1].rfmt =
        (QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_RFMT_ADDR_WIDTH_LSB) |
        (QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_RFMT_DUMMY_WIDTH_LSB) |
        (QMI_M0_RFMT_DATA_WIDTH_VALUE_Q << QMI_M0_RFMT_DATA_WIDTH_LSB) |
        (QMI_M0_RFMT_PREFIX_LEN_VALUE_8 << QMI_M0_RFMT_PREFIX_LEN_LSB) |
        (6u << QMI_M0_RFMT_DUMMY_LEN_LSB);
    qmi_hw->m[1].rcmd = 0xEB;

    qmi_hw->m[1].wfmt =
        (QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB) |
        (QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_WFMT_ADDR_WIDTH_LSB) |
        (QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_WFMT_DUMMY_WIDTH_LSB) |
        (QMI_M0_WFMT_DATA_WIDTH_VALUE_Q << QMI_M0_WFMT_DATA_WIDTH_LSB) |
        (QMI_M0_WFMT_PREFIX_LEN_VALUE_8 << QMI_M0_WFMT_PREFIX_LEN_LSB);
    qmi_hw->m[1].wcmd = 0x38;

    qmi_hw->direct_csr = 0;
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
    g_psram_clock_hz = clock_hz / divisor;
}

static bool psram_test(void)
{
    volatile uint32_t *p = (volatile uint32_t *)LVDS_PSRAM_BASE;
    const uint32_t old0 = p[0];
    const uint32_t old1 = p[1];
    const uint32_t a = 0x13572468u;
    const uint32_t b = 0xdeadbeefu;
    bool ok;

    p[0] = a;
    p[1] = b;
    ok = (p[0] == a && p[1] == b);
    p[0] = old0;
    p[1] = old1;
    return ok;
}

bool lvds_psram_init(void)
{
    g_psram_available = false;
    g_psram_clock_hz = 0;

    for (int attempt = 0; attempt < LVDS_PSRAM_INIT_ATTEMPTS; ++attempt) {
        psram_qmi_init(LVDS_PSRAM_CS_PIN);
#if LVDS_PSRAM_POST_INIT_DELAY_MS > 0
        lvds_psram_delay_ms((uint32_t)LVDS_PSRAM_POST_INIT_DELAY_MS);
#endif
        if (psram_test()) {
            g_psram_available = true;
            return true;
        }
#if LVDS_PSRAM_RETRY_DELAY_MS > 0
        lvds_psram_delay_ms((uint32_t)LVDS_PSRAM_RETRY_DELAY_MS);
#endif
    }

    return false;
}

bool lvds_psram_available(void)
{
    return g_psram_available;
}

uint32_t lvds_psram_clock_hz(void)
{
    return g_psram_clock_hz;
}

void *lvds_psram_ptr(size_t offset)
{
    return (void *)(LVDS_PSRAM_BASE + offset);
}
