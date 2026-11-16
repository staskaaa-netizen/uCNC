#include "lvds_psram.h"

#include <string.h>

#include "hardware/address_mapped.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#ifndef LVDS_PSRAM_MAX_SCK_HZ
#define LVDS_PSRAM_MAX_SCK_HZ 109000000u
#endif

#ifndef LVDS_PSRAM_ID
#define LVDS_PSRAM_ID 0x5Du
#endif

static bool g_psram_available;
static uint32_t g_psram_size;
static uint32_t g_psram_clock_hz;

static uint32_t __no_inline_not_in_flash_func(psram_detect)(void)
{
    uint32_t psram_size = 0;

    qmi_hw->direct_csr =
        (30u << QMI_DIRECT_CSR_CLKDIV_LSB) | QMI_DIRECT_CSR_EN_BITS;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
        tight_loop_contents();
    }

    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx =
        QMI_DIRECT_TX_OE_BITS |
        (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) |
        0xF5u;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
        tight_loop_contents();
    }
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint8_t kgd = 0;
    uint8_t eid = 0;
    for (uint32_t i = 0; i < 7u; ++i) {
        qmi_hw->direct_tx = (i == 0u) ? 0x9Fu : 0xFFu;
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) {
            tight_loop_contents();
        }
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
            tight_loop_contents();
        }
        if (i == 5u) {
            kgd = (uint8_t)qmi_hw->direct_rx;
        } else if (i == 6u) {
            eid = (uint8_t)qmi_hw->direct_rx;
        } else {
            (void)qmi_hw->direct_rx;
        }
    }
    qmi_hw->direct_csr &=
        ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    if (kgd == LVDS_PSRAM_ID) {
        psram_size = 1024u * 1024u;
        const uint8_t size_id = eid >> 5u;
        if (size_id == 4u) {
            psram_size *= 16u;
        } else if ((eid == 0x26u) || (size_id == 2u) || (size_id == 3u)) {
            psram_size *= 8u;
        } else if (size_id == 0u) {
            psram_size *= 2u;
        } else if (size_id == 1u) {
            psram_size *= 4u;
        }
    }

    return psram_size;
}

static uint32_t __no_inline_not_in_flash_func(psram_qmi_init)(uint cs_pin)
{
    gpio_set_function(cs_pin, GPIO_FUNC_XIP_CS1);

    const uint32_t intr_stash = save_and_disable_interrupts();
    const uint32_t size = psram_detect();
    if (size == 0u) {
        restore_interrupts(intr_stash);
        return 0u;
    }

    qmi_hw->direct_csr =
        (10u << QMI_DIRECT_CSR_CLKDIV_LSB) |
        QMI_DIRECT_CSR_EN_BITS |
        QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
        tight_loop_contents();
    }

    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x35u;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
        tight_loop_contents();
    }

    const uint32_t clock_hz = clock_get_hz(clk_sys);
    uint32_t divisor =
        (clock_hz + LVDS_PSRAM_MAX_SCK_HZ - 1u) / LVDS_PSRAM_MAX_SCK_HZ;
    if (divisor == 1u && clock_hz > 100000000u) {
        divisor = 2u;
    }
    if (divisor == 0u) {
        divisor = 1u;
    }

    uint32_t rxdelay = divisor;
    if ((clock_hz / divisor) > 100000000u) {
        ++rxdelay;
    }

    const uint32_t clock_period_fs =
        (uint32_t)(1000000000000000ull / clock_hz);
    const uint32_t max_select =
        (125u * 1000000u) / clock_period_fs;
    const uint32_t min_deselect =
        (18u * 1000000u + (clock_period_fs - 1u)) / clock_period_fs -
        (divisor + 1u) / 2u;

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
    restore_interrupts(intr_stash);

    g_psram_clock_hz = clock_hz / divisor;
    return size;
}

static bool psram_test(void)
{
    volatile uint32_t *p = (volatile uint32_t *)LVDS_PSRAM_BASE;
    const uint32_t old0 = p[0];
    const uint32_t old1 = p[1];
    const uint32_t a = 0x13572468u;
    const uint32_t b = 0xDEADBEEFu;

    p[0] = a;
    p[1] = b;
    const bool ok = (p[0] == a && p[1] == b);
    p[0] = old0;
    p[1] = old1;
    return ok;
}

bool lvds_psram_init(void)
{
    g_psram_available = false;
    g_psram_size = 0;
    g_psram_clock_hz = 0;

    g_psram_size = psram_qmi_init(LVDS_PSRAM_CS_PIN);
    if (g_psram_size == 0u || !psram_test()) {
        g_psram_size = 0;
        g_psram_clock_hz = 0;
        return false;
    }

    g_psram_available = true;
    return true;
}

bool lvds_psram_reinit_timing(void)
{
    if (!g_psram_available) {
        return false;
    }

    const uint32_t size = psram_qmi_init(LVDS_PSRAM_CS_PIN);
    if (size == 0u || !psram_test()) {
        g_psram_available = false;
        g_psram_size = 0;
        g_psram_clock_hz = 0;
        return false;
    }

    g_psram_size = size;
    g_psram_available = true;
    return true;
}

bool lvds_psram_available(void)
{
    return g_psram_available;
}

uint32_t lvds_psram_size(void)
{
    return g_psram_size;
}

uint32_t lvds_psram_clock_hz(void)
{
    return g_psram_clock_hz;
}

void *lvds_psram_ptr(size_t offset)
{
    return (void *)(LVDS_PSRAM_BASE + offset);
}
