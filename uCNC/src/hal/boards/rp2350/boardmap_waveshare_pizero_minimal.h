/*
    Minimal RP2350 board map for first-stage LeanCam/uCNC bring-up on the
    Waveshare RP2350 PiZero-style board.

    This intentionally avoids the HDMI HSTX pins, the onboard SD pins,
    74HC595 expansion and encoders. It is only a small
    uCNC base to prove the RP2350 backend before LeanCam HDMI is layered in.

    External pins kept in play for this target:
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 20, 21, 22, 23, 24, 25, 26, 27.

    HSTX HDMI, copied from the working Waveshare800x600 DispHSTX project:
    D2 12/13, CLK 14/15, D1 16/17, D0 18/19.

    Onboard SD from the working frank-doom Waveshare PiZero config:
    spi1, SCK 30, MOSI/CMD 31, MISO/D0 40, CS/D3 43.
*/

#ifndef BOARDMAP_WAVESHARE_PIZERO_MINIMAL_H
#define BOARDMAP_WAVESHARE_PIZERO_MINIMAL_H

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef MCU
#define MCU MCU_RP2350
#endif

#ifndef BOARD_NAME
#define BOARD_NAME "RP2350 PiZero LeanCam minimal"
#endif

#ifndef F_CPU
#define F_CPU 150000000UL
#endif

/* GPIO2/GPIO3 reserved for I2C keyboard wiring.
 * Do not define I2C_* here yet: in this target it pulls in the Arduino
 * Wire backend before the keyboard module is ready.
 */
/* #define I2C_CLK_BIT 2 */
/* #define I2C_DATA_BIT 3 */
/* #define I2C_PORT 1 */

#define DIN16_BIT 2
#define DIN17_BIT 3

/* Keep motion pins away from the known HDMI, onboard SD, and I2C pins. */
#define STEP0_BIT 23
#define DIR0_BIT 24
#define STEP0_EN_BIT 27

#define STEP1_BIT 6
#define DIR1_BIT 7
#define STEP1_EN_BIT 8

#define STEP2_BIT 10
#define DIR2_BIT 11
#define STEP2_EN_BIT 20

#define LIMIT_X_BIT 5
#define LIMIT_Y_BIT 9
#define LIMIT_Z_BIT 21

#define LIMIT_X_PULLUP
#define LIMIT_Y_PULLUP
#define LIMIT_Z_PULLUP

/* USB serial console on the native/programming connector. */
#define MCU_HAS_USB

/* Activity LED. */
#define DOUT31_BIT 25

/* Simple spindle PWM output, away from HDMI and PIO USB. */
#define PWM0_BIT 22
#define DOUT0_BIT 26

#ifdef __cplusplus
}
#endif

#endif
