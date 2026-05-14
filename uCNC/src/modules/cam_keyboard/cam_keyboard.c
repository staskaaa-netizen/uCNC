#include "../../cnc.h"
#include "../softi2c.h"
#include "cam_keyboard.h"


#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#if defined(LEANCAM_RP2350_SOFT_CAM_KEYBOARD)
#include "hardware/gpio.h"
#include "pico/time.h"
#endif

#if (UCNC_MODULE_VERSION < 10800 || UCNC_MODULE_VERSION > 99999)
#error "This module is not compatible with the current version of µCNC"
#endif

/* ------------------------------------------------------------------------- */
/* config                                                                    */
/* ------------------------------------------------------------------------- */

#if defined(LEANCAM_RP2350_SOFT_CAM_KEYBOARD)
#undef CAM_KEYBOARD_USE_HW_I2C
#elif !defined(CAM_KEYBOARD_USE_HW_I2C)
#define CAM_KEYBOARD_USE_HW_I2C
#endif

#ifndef CAM_KB_I2C_FREQ
#define CAM_KB_I2C_FREQ 100000UL
#endif

#ifndef CAM_KB_I2C_SCL
#define CAM_KB_I2C_SCL DIN16
#endif

#ifndef CAM_KB_I2C_SDA
#define CAM_KB_I2C_SDA DIN17
#endif


#ifndef CAM_KB_ADDR
#define CAM_KB_ADDR 0x34
#endif

#if defined(LEANCAM_RP2350_SOFT_CAM_KEYBOARD)
#ifndef CAM_KB_GPIO_SCL
#define CAM_KB_GPIO_SCL 2
#endif
#ifndef CAM_KB_GPIO_SDA
#define CAM_KB_GPIO_SDA 3
#endif
#endif


#define TCA8418_REG_CFG          0x01
#define TCA8418_REG_INT_STAT     0x02
#define TCA8418_REG_KEY_LCK_EC   0x03
#define TCA8418_REG_KEY_EVENT_A  0x04
#define TCA8418_REG_KP_GPIO1     0x1D
#define TCA8418_REG_KP_GPIO2     0x1E
#define TCA8418_REG_KP_GPIO3     0x1F


/* ------------------------------------------------------------------------- */
/* i2c backend selection                                                     */
/* ------------------------------------------------------------------------- */

#if !defined(LEANCAM_RP2350_SOFT_CAM_KEYBOARD) && (!defined(CAM_KEYBOARD_USE_HW_I2C) || !defined(MCU_HAS_I2C))
SOFTI2C(camkbi2c, CAM_KB_I2C_FREQ, CAM_KB_I2C_SCL, CAM_KB_I2C_SDA);
#endif

#if (defined(CAM_KEYBOARD_USE_HW_I2C) && defined(MCU_HAS_I2C))
#define CAM_KB_I2C_PORT NULL
#elif defined(LEANCAM_RP2350_SOFT_CAM_KEYBOARD)
#define CAM_KB_I2C_PORT NULL
#else
#define CAM_KB_I2C_PORT (&camkbi2c)
#endif

/* ------------------------------------------------------------------------- */
/* local state                                                               */
/* ------------------------------------------------------------------------- */

static uint8_t   g_cam_kb_raw[6];
static cam_key_t g_cam_kb_key = CAM_KEY_NONE;
static bool      g_cam_kb_changed = false;
static bool      g_cam_kb_inited = false;

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

#if defined(LEANCAM_RP2350_SOFT_CAM_KEYBOARD)
static void cam_i2c_delay(void)
{
    busy_wait_us_32(5);
}

static void cam_i2c_scl(bool high)
{
    if (high) {
        gpio_set_dir(CAM_KB_GPIO_SCL, GPIO_IN);
        gpio_pull_up(CAM_KB_GPIO_SCL);
    } else {
        gpio_put(CAM_KB_GPIO_SCL, 0);
        gpio_set_dir(CAM_KB_GPIO_SCL, GPIO_OUT);
    }
    cam_i2c_delay();
}

static void cam_i2c_sda(bool high)
{
    if (high) {
        gpio_set_dir(CAM_KB_GPIO_SDA, GPIO_IN);
        gpio_pull_up(CAM_KB_GPIO_SDA);
    } else {
        gpio_put(CAM_KB_GPIO_SDA, 0);
        gpio_set_dir(CAM_KB_GPIO_SDA, GPIO_OUT);
    }
    cam_i2c_delay();
}

static bool cam_i2c_read_sda(void)
{
    gpio_set_dir(CAM_KB_GPIO_SDA, GPIO_IN);
    gpio_pull_up(CAM_KB_GPIO_SDA);
    cam_i2c_delay();
    return gpio_get(CAM_KB_GPIO_SDA) != 0;
}

static void cam_i2c_start(void)
{
    cam_i2c_sda(true);
    cam_i2c_scl(true);
    cam_i2c_sda(false);
    cam_i2c_scl(false);
}

static void cam_i2c_stop(void)
{
    cam_i2c_sda(false);
    cam_i2c_scl(true);
    cam_i2c_sda(true);
}

static bool cam_i2c_write_byte(uint8_t v)
{
    uint8_t i;
    bool ack;

    for (i = 0; i < 8; ++i) {
        cam_i2c_sda((v & 0x80) != 0);
        cam_i2c_scl(true);
        cam_i2c_scl(false);
        v <<= 1;
    }

    cam_i2c_sda(true);
    cam_i2c_scl(true);
    ack = !cam_i2c_read_sda();
    cam_i2c_scl(false);
    return ack;
}

static uint8_t cam_i2c_read_byte(bool ack)
{
    uint8_t i;
    uint8_t v = 0;

    cam_i2c_sda(true);
    for (i = 0; i < 8; ++i) {
        v <<= 1;
        cam_i2c_scl(true);
        if (cam_i2c_read_sda()) {
            v |= 1;
        }
        cam_i2c_scl(false);
    }
    cam_i2c_sda(!ack);
    cam_i2c_scl(true);
    cam_i2c_scl(false);
    cam_i2c_sda(true);
    return v;
}

static bool cam_i2c_write_reg_gpio(uint8_t reg, uint8_t value)
{
    bool ok;

    cam_i2c_start();
    ok = cam_i2c_write_byte((uint8_t)(CAM_KB_ADDR << 1)) &&
         cam_i2c_write_byte(reg) &&
         cam_i2c_write_byte(value);
    cam_i2c_stop();
    return ok;
}

static bool cam_i2c_read_reg_gpio(uint8_t reg, uint8_t *value)
{
    bool ok;

    cam_i2c_start();
    ok = cam_i2c_write_byte((uint8_t)(CAM_KB_ADDR << 1)) &&
         cam_i2c_write_byte(reg);
    if (!ok) {
        cam_i2c_stop();
        return false;
    }

    cam_i2c_start();
    ok = cam_i2c_write_byte((uint8_t)((CAM_KB_ADDR << 1) | 1));
    if (ok) {
        *value = cam_i2c_read_byte(false);
    }
    cam_i2c_stop();
    return ok;
}
#endif

static bool cam_keyboard_write_reg(uint8_t reg, uint8_t value)
{
#if defined(LEANCAM_RP2350_SOFT_CAM_KEYBOARD)
    return cam_i2c_write_reg_gpio(reg, value);
#else
    uint8_t data[2];
    data[0] = reg;
    data[1] = value;

#if (UCNC_MODULE_VERSION < 10808)
    return (softi2c_send(CAM_KB_I2C_PORT, CAM_KB_ADDR, data, 2, true) == I2C_OK);
#else
    return (softi2c_send(CAM_KB_I2C_PORT, CAM_KB_ADDR, data, 2, true, 20) == I2C_OK);
#endif
#endif
}

static bool cam_keyboard_read_reg(uint8_t reg, uint8_t *value)
{
    if (value == NULL)
        return false;

#if defined(LEANCAM_RP2350_SOFT_CAM_KEYBOARD)
    return cam_i2c_read_reg_gpio(reg, value);
#else
#if (UCNC_MODULE_VERSION < 10808)
    if (softi2c_send(CAM_KB_I2C_PORT, CAM_KB_ADDR, &reg, 1, true) != I2C_OK)
        return false;
    if (softi2c_receive(CAM_KB_I2C_PORT, CAM_KB_ADDR, value, 1) != I2C_OK)
        return false;
#else
    if (softi2c_send(CAM_KB_I2C_PORT, CAM_KB_ADDR, &reg, 1, true, 20) != I2C_OK)
        return false;
    if (softi2c_receive(CAM_KB_I2C_PORT, CAM_KB_ADDR, value, 1, 20) != I2C_OK)
        return false;
#endif

    return true;
#endif
}

static void cam_keyboard_clear_raw(void)
{
    memset(g_cam_kb_raw, 0, sizeof(g_cam_kb_raw));
}

/* ------------------------------------------------------------------------- */
/* public api                                                                */
/* ------------------------------------------------------------------------- */

void cam_keyboard_init(void)
{
    if (g_cam_kb_inited)
        return;

#if defined(LEANCAM_RP2350_SOFT_CAM_KEYBOARD)
    gpio_init(CAM_KB_GPIO_SCL);
    gpio_init(CAM_KB_GPIO_SDA);
    gpio_pull_up(CAM_KB_GPIO_SCL);
    gpio_pull_up(CAM_KB_GPIO_SDA);
    cam_i2c_scl(true);
    cam_i2c_sda(true);
#else
    softi2c_config(CAM_KB_I2C_PORT, CAM_KB_I2C_FREQ);
#endif

    /* TCA8418 keypad mode, auto-increment on */
    (void)cam_keyboard_write_reg(TCA8418_REG_CFG, 0x01);

    /* verified 4x4 keypad scan */
    (void)cam_keyboard_write_reg(TCA8418_REG_KP_GPIO1, 0x0F);
    (void)cam_keyboard_write_reg(TCA8418_REG_KP_GPIO2, 0x0F);
    (void)cam_keyboard_write_reg(TCA8418_REG_KP_GPIO3, 0x00);

    /* clear all pending interrupt flags */
    (void)cam_keyboard_write_reg(TCA8418_REG_INT_STAT, 0xFF);

    cam_keyboard_clear_raw();
    g_cam_kb_key = CAM_KEY_NONE;
    g_cam_kb_changed = false;
    g_cam_kb_inited = true;
}

cam_key_t cam_keyboard_decode_key(const uint8_t raw[6])
{
    uint8_t evt = raw[0];

    if (evt & 0x80)
        return CAM_KEY_NONE;

    switch (evt)
    {
        case  1: return CAM_KEY_STAR;
        case  2: return CAM_KEY_0;
        case  3: return CAM_KEY_HASH;
        case  4: return CAM_KEY_D;

        case 11: return CAM_KEY_1;
        case 12: return CAM_KEY_2;
        case 13: return CAM_KEY_3;
        case 14: return CAM_KEY_C;

        case 21: return CAM_KEY_4;
        case 22: return CAM_KEY_5;
        case 23: return CAM_KEY_6;
        case 24: return CAM_KEY_B;

        case 31: return CAM_KEY_7;
        case 32: return CAM_KEY_8;
        case 33: return CAM_KEY_9;
        case 34: return CAM_KEY_A;

        default: return CAM_KEY_NONE;
    }
}

char cam_keyboard_key_to_char(cam_key_t key)
{
    switch (key)
    {
        case CAM_KEY_0:    return '0';
        case CAM_KEY_1:    return '1';
        case CAM_KEY_2:    return '2';
        case CAM_KEY_3:    return '3';
        case CAM_KEY_4:    return '4';
        case CAM_KEY_5:    return '5';
        case CAM_KEY_6:    return '6';
        case CAM_KEY_7:    return '7';
        case CAM_KEY_8:    return '8';
        case CAM_KEY_9:    return '9';
        case CAM_KEY_STAR: return '*';
        case CAM_KEY_HASH: return '#';
        case CAM_KEY_A:    return 'A';
        case CAM_KEY_B:    return 'B';
        case CAM_KEY_C:    return 'C';
        case CAM_KEY_D:    return 'D';
        default:           return 0;
    }
}

void cam_keyboard_update(void)
{
    uint8_t count;
    uint8_t evt;
    bool got_press = false;

    if (!g_cam_kb_inited)
        return;

    g_cam_kb_changed = false;

    if (!cam_keyboard_read_reg(TCA8418_REG_KEY_LCK_EC, &count))
        return;

    count &= 0x0F;
    if (!count)
        return;

    while (count--)
    {
        if (!cam_keyboard_read_reg(TCA8418_REG_KEY_EVENT_A, &evt))
            break;

        if ((evt & 0x80) == 0)
        {
            cam_keyboard_clear_raw();
            g_cam_kb_raw[0] = evt;
            g_cam_kb_key = cam_keyboard_decode_key(g_cam_kb_raw);
            g_cam_kb_changed = (g_cam_kb_key != CAM_KEY_NONE);
            got_press = g_cam_kb_changed;
        }
    }

    if (!got_press)
        g_cam_kb_key = CAM_KEY_NONE;

    (void)cam_keyboard_write_reg(TCA8418_REG_INT_STAT, 0xFF);
}

const uint8_t *cam_keyboard_raw(void)
{
    return g_cam_kb_raw;
}

cam_key_t cam_keyboard_key(void)
{
    return g_cam_kb_key;
}

char cam_keyboard_key_char(void)
{
    return cam_keyboard_key_to_char(g_cam_kb_key);
}

bool cam_keyboard_changed(void)
{
    return g_cam_kb_changed;
}
