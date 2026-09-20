#include "../../cnc.h"
#include "../softi2c.h"
#include "cam_keyboard.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#if (UCNC_MODULE_VERSION < 10800 || UCNC_MODULE_VERSION > 99999)
#error "This module is not compatible with the current version of uCNC"
#endif

#ifndef CAM_KB_I2C_FREQ
#define CAM_KB_I2C_FREQ 100000UL
#endif

#ifndef CAM_KB_I2C_SCL
#define CAM_KB_I2C_SCL DOUT17
#endif

#ifndef CAM_KB_I2C_SDA
#define CAM_KB_I2C_SDA DOUT16
#endif

#ifndef CAM_KB_ADDR
#define CAM_KB_ADDR 0x34
#endif

#define TCA8418_REG_CFG          0x01
#define TCA8418_REG_INT_STAT     0x02
#define TCA8418_REG_KEY_LCK_EC   0x03
#define TCA8418_REG_KEY_EVENT_A  0x04
#define TCA8418_REG_KP_GPIO1     0x1D
#define TCA8418_REG_KP_GPIO2     0x1E
#define TCA8418_REG_KP_GPIO3     0x1F

SOFTI2C(camkbi2c, CAM_KB_I2C_FREQ, CAM_KB_I2C_SCL, CAM_KB_I2C_SDA);
#define CAM_KB_I2C_PORT (&camkbi2c)

static uint8_t   g_cam_kb_raw[6];
static cam_key_t g_cam_kb_key = CAM_KEY_NONE;
static char      g_cam_kb_press;          /* key that went down in this update */
static bool      g_cam_kb_changed = false;
static bool      g_cam_kb_inited = false;
static uint32_t  g_cam_kb_next_init_ms = 0;

static void cam_keyboard_bus_recover(void)
{
    CAM_KB_I2C_PORT->sda(true);
    CAM_KB_I2C_PORT->scl(true);
    mcu_delay_us(10);

    for (uint8_t i = 0; i < 9 && !CAM_KB_I2C_PORT->get_sda(); ++i) {
        CAM_KB_I2C_PORT->scl(false);
        mcu_delay_us(10);
        CAM_KB_I2C_PORT->scl(true);
        mcu_delay_us(10);
    }

    CAM_KB_I2C_PORT->sda(false);
    mcu_delay_us(10);
    CAM_KB_I2C_PORT->scl(true);
    mcu_delay_us(10);
    CAM_KB_I2C_PORT->sda(true);
    mcu_delay_us(10);
}

static bool cam_keyboard_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2];
    data[0] = reg;
    data[1] = value;

#if (UCNC_MODULE_VERSION < 10808)
    return (softi2c_send(CAM_KB_I2C_PORT, CAM_KB_ADDR, data, 2, true) == I2C_OK);
#else
    return (softi2c_send(CAM_KB_I2C_PORT, CAM_KB_ADDR, data, 2, true, 20) == I2C_OK);
#endif
}

static bool cam_keyboard_read_reg(uint8_t reg, uint8_t *value)
{
    if (value == NULL)
        return false;

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
}

static void cam_keyboard_clear_raw(void)
{
    memset(g_cam_kb_raw, 0, sizeof(g_cam_kb_raw));
}

void cam_keyboard_init(void)
{
    bool ok = true;

    if (g_cam_kb_inited)
        return;

    if ((int32_t)(mcu_millis() - g_cam_kb_next_init_ms) < 0)
        return;

    softi2c_config(CAM_KB_I2C_PORT, CAM_KB_I2C_FREQ);
    cam_keyboard_bus_recover();

    ok = cam_keyboard_write_reg(TCA8418_REG_CFG, 0x01) && ok;
    ok = cam_keyboard_write_reg(TCA8418_REG_KP_GPIO1, 0x0F) && ok;
    ok = cam_keyboard_write_reg(TCA8418_REG_KP_GPIO2, 0x0F) && ok;
    ok = cam_keyboard_write_reg(TCA8418_REG_KP_GPIO3, 0x00) && ok;
    ok = cam_keyboard_write_reg(TCA8418_REG_INT_STAT, 0xFF) && ok;

    cam_keyboard_clear_raw();
    g_cam_kb_key = CAM_KEY_NONE;
    g_cam_kb_press = 0;
    g_cam_kb_changed = false;
    g_cam_kb_inited = ok;
    g_cam_kb_next_init_ms = mcu_millis() + (ok ? 0u : 1000u);
}

cam_key_t cam_keyboard_decode_key(const uint8_t raw[6])
{
    /* Bit 7 of the event byte is the release flag; the key is the low seven
       bits either way. Reading a release as "no key" is what left the driver
       believing a key was still down: the next press of that key then looked
       like a repeat and was thrown away, so every key worked once until
       another one was pressed. */
    uint8_t evt = (uint8_t)(raw[0] & 0x7F);

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

    if (!g_cam_kb_inited) {
        cam_keyboard_init();
        return;
    }

    g_cam_kb_changed = false;
    g_cam_kb_press = 0;

    if (!cam_keyboard_read_reg(TCA8418_REG_KEY_LCK_EC, &count)) {
        g_cam_kb_inited = false;
        g_cam_kb_next_init_ms = mcu_millis() + 1000u;
        g_cam_kb_key = CAM_KEY_NONE;
        return;
    }

    count &= 0x0F;
    if (!count)
        return;     /* nothing new: a key that is down stays down */

    while (count--)
    {
        uint8_t evt;
        cam_key_t key;

        if (!cam_keyboard_read_reg(TCA8418_REG_KEY_EVENT_A, &evt))
            break;

        cam_keyboard_clear_raw();
        g_cam_kb_raw[0] = evt;
        key = cam_keyboard_decode_key(g_cam_kb_raw);
        if (key == CAM_KEY_NONE)
            continue;

        if (evt & 0x80)
        {
            /* Release: the key comes up, whether or not its press was in this
               same read. */
            if (key == g_cam_kb_key)
                g_cam_kb_key = CAM_KEY_NONE;
        }
        else
        {
            g_cam_kb_key = key;
            /* The press is kept apart from the key that is down: a tap whose
               release arrives in the same read is still a press. */
            g_cam_kb_press = cam_keyboard_key_to_char(key);
            g_cam_kb_changed = true;
        }
    }

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

char cam_keyboard_pressed_char(void)
{
    return g_cam_kb_press;
}

bool cam_keyboard_changed(void)
{
    return g_cam_kb_changed;
}
