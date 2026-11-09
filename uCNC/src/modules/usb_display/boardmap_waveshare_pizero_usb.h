/*
    RP2350 board map for the machine whose video goes out over USB to a
    DisplayLink monitor.

    Identical to `boardmap_waveshare_pizero_minimal.h` except in one thing: the
    native USB port is the **display's host**, so the console and the G-code
    protocol cannot be on it. They are on UART0 instead - GP0 transmits, GP1
    receives - which is also the connector the Pico debug probe and any USB-UART
    adapter already speaks. `MCU_HAS_USB` is deliberately absent: it is what
    makes the HAL bring the controller up as a device, and the DisplayLink
    adapter needs it as a host.

    External pins kept in play for this target:
    0 (UART TX), 1 (UART RX), 2..11, 20..27.

    HSTX HDMI, copied from the working Waveshare800x600 DispHSTX project:
    D2 12/13, CLK 14/15, D1 16/17, D0 18/19 - unused here; no LVDS panel.

    Onboard SD from the working frank-doom Waveshare PiZero config:
    spi1, SCK 30, MOSI/CMD 31, MISO/D0 40, CS/D3 43.
*/

#ifndef BOARDMAP_WAVESHARE_PIZERO_USB_H
#define BOARDMAP_WAVESHARE_PIZERO_USB_H

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef MCU
#define MCU MCU_RP2350
#endif

#ifndef BOARD_NAME
#define BOARD_NAME "RP2350 PiZero USB display"
#endif

#ifndef F_CPU
#define F_CPU 150000000UL
#endif

/* The console and the protocol: UART0 on GP0/GP1. The native USB port is the
 * DisplayLink monitor's host, so there is no `MCU_HAS_USB` here. */
#define UART_PORT 0
#define TX_BIT 0
#define RX_BIT 1

/* GPIO2/GPIO3 reserved for I2C keyboard wiring. */
/* #define I2C_CLK_BIT 2 */
/* #define I2C_DATA_BIT 3 */
/* #define I2C_PORT 1 */

#define DIN16_BIT 2
#define DIN17_BIT 3

/* Keep motion pins away from the SD, USB and I2C pins. */
#define STEP0_BIT 23
#define DIR0_BIT 24
#define STEP0_EN_BIT 27

#define STEP1_BIT 6
#define DIR1_BIT 7
#define STEP1_EN_BIT 8

#define STEP2_BIT 10
#define DIR2_BIT 11
#define STEP2_EN_BIT 20

/* No limit inputs on this target while it is a display bring-up bench: the
 * minimal map declared them with pull-ups and nothing wired to the pins, and an
 * unwired input that reads as triggered puts the machine in an alarm it cannot
 * be talked out of from the panel. The pins stay free for the switches; wiring
 * them up means declaring them here again, not changing the display. */
/* #define LIMIT_X_BIT 5 */
/* #define LIMIT_Y_BIT 9 */
/* #define LIMIT_Z_BIT 21 */

/* #define LIMIT_X_PULLUP */
/* #define LIMIT_Y_PULLUP */
/* #define LIMIT_Z_PULLUP */

/* Activity LED. */
#define DOUT31_BIT 25

/* Simple spindle PWM output. */
#define PWM0_BIT 22
#define DOUT0_BIT 26

#ifdef __cplusplus
}
#endif

#endif
