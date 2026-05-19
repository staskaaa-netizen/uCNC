#ifndef BOADMAP_OVERRIDES_H
#define BOADMAP_OVERRIDES_H
#ifdef __cplusplus
extern "C"
{
#endif

#include "boardmap_reset.h"

/*
    RP2350 Waveshare PiZero LeanCam bring-up map.

    External pins intentionally kept to:
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 20, 21, 22, 23, 24, 25, 26, 27.

    GPIO2/GPIO3 are reserved for the external I2C keyboard.

    Reserved LVDS HSTX pins from the working Sharp 800x600 setup:
    D2 12/13, CLK 14/15, D1 16/17, D0 18/19.

    Onboard SD socket wiring:
    spi1, SCK 30, MOSI/CMD 31, MISO/D0 40, CS/D3 43.
*/

#define MCU MCU_RP2350
#define KINEMATIC KINEMATIC_CARTESIAN
#define AXIS_COUNT 3
#define TOOL_COUNT 1
#define BAUDRATE 115200
#define BOARD_NAME "RP2350 PiZero LeanCam"
#ifndef F_CPU
#define F_CPU 150000000UL
#endif

/* Native USB CDC on the firmware/programming connector. */
#define MCU_HAS_USB

/* GPIO2/GPIO3 reserved for I2C keyboard wiring.
 * Do not define I2C_* here yet: in this target it pulls in the Arduino
 * Wire backend before the keyboard module is ready.
 */
/* #define I2C_CLK_BIT 2 */
/* #define I2C_DATA_BIT 3 */
/* #define I2C_PORT 1 */

#define DIN16_BIT 2
#define DIN17_BIT 3

/* Direct stepper pins. No 74HC595 expansion in this target. */
#define STEP0_BIT 23
#define DIR0_BIT 24
#define STEP0_EN_BIT 27

#define STEP1_BIT 6
#define DIR1_BIT 7
#define STEP1_EN_BIT 11

#define STEP2_BIT 25
#define DIR2_BIT 8
#define STEP2_EN_BIT 20

/* Limit inputs. */
#define LIMIT_X_BIT 5
#define LIMIT_Y_BIT 255
#define LIMIT_Z_BIT 9

#define LIMIT_X_PULLUP
#define LIMIT_Y_PULLUP
#define LIMIT_Z_PULLUP

/* Spare control inputs on available external pins. */
/*#define ESTOP_BIT 23
#define ESTOP_PULLUP*/
/*#define FHOLD_BIT 24
#define FHOLD_PULLUP
#define CS_RES_BIT 27
#define CS_RES_PULLUP*/

/* Simple spindle/tool outputs. */
#define PWM0_BIT 22
#define DOUT0_BIT 255

/* Activity LED. */
#define DOUT31_BIT 24


#define CAM_KB_GPIO_SCL 2

#define CAM_KB_GPIO_SDA 3

/* Onboard SD socket: spi1 / uCNC SPI2. */
#define SPI2_CLK_BIT 30
#define SPI2_SDO_BIT 31
#define SPI2_SDI_BIT 40
#define SPI2_CS_BIT 43
#define SPI2_PORT 1

/* Same onboard SD pins when sd_card_v2 uses software SPI. */
#define DOUT30_BIT 30 /* CLK */
#define DOUT29_BIT 31 /* MOSI */
#define DIN29_BIT 40  /* MISO */
#define SPI_CS_BIT 43 /* CS */

#ifdef ENABLE_SD_CARD_V2
/* SD uses the onboard socket pins above. */
#endif

/* Keep expansion features off for the RP2350 LVDS stage. */
#define IC74HC595_COUNT 0
#define IC74HC165_COUNT 0

#ifdef __cplusplus
}
#endif
#endif
