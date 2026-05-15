#include <assert.h>
#include "hw_config.h"

static spi_t spis[] = {
    {
        .hw_inst = spi1,
        .sck_gpio = 30,
        .mosi_gpio = 31,
        .miso_gpio = 40,
        .set_drive_strength = true,
        .mosi_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
        .sck_gpio_drive_strength = GPIO_DRIVE_STRENGTH_12MA,
        .no_miso_gpio_pull_up = true,
        .baud_rate = 20 * 1000 * 1000,
        .spi_mode = 0,
    },
};

static sd_spi_if_t spi_ifs[] = {
    {
        .spi = &spis[0],
        .ss_gpio = 43,
        .set_drive_strength = true,
        .ss_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
    },
};

static sd_card_t sd_cards[] = {
    {
        .type = SD_IF_SPI,
        .spi_if_p = &spi_ifs[0],
        .use_card_detect = false,
        .card_detect_gpio = 24,
        .card_detected_true = 0,
        .card_detect_use_pull = true,
        .card_detect_pull_hi = true,
    },
};

size_t sd_get_num(void)
{
    return count_of(sd_cards);
}

sd_card_t *sd_get_by_num(size_t num)
{
    assert(num < sd_get_num());
    return (num < sd_get_num()) ? &sd_cards[num] : NULL;
}
