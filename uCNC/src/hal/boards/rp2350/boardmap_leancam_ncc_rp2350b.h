/*
    uCNC board map for the LeanCam NCC RP2350B motherboard.

    The normative pin contract is:
    uCNC/LeanCam-NCC-RP2350B/documentation/hal-pinout-contract.md

    The carrier exposes motion through the combined four-register
    74AHCT595/74HC165 chain. GPIO0 is reserved for the WEACT module PSRAM and
    must never be reassigned here.
*/

#ifndef BOARDMAP_LEANCAM_NCC_RP2350B_H
#define BOARDMAP_LEANCAM_NCC_RP2350B_H

#ifdef __cplusplus
extern "C"
{
#endif

#define LEANCAM_WEACT_RP2350B

#ifndef MCU
#define MCU MCU_RP2350
#endif

#ifndef BOARD_NAME
#define BOARD_NAME "uCNC LeanCam NCC RP2350B"
#endif

#ifndef F_CPU
#define F_CPU 150000000UL
#endif

#ifndef AXIS_COUNT
#define AXIS_COUNT 3
#endif
#define TOOL_COUNT 1
#define BAUDRATE 115200

/* Four 74AHCT595 outputs and four 74HC165 inputs share one full-duplex PIO
   bus. GPIO20/21/22 are DATA/CLK/LATCH and GPIO23 is the 165 serial return.
   GPIO24 remains the 165 parallel-load output and is pulsed by the MCU HAL.
   PIO0 belongs to the spindle encoder at fixed instruction offset 0; the
   extender therefore owns PIO1 exclusively. */
#define IC74HC595_CUSTOM_SHIFT_IO
#define IC74HC595_PIO_DATA 20
#define IC74HC595_PIO_CLK 21
#define IC74HC595_PIO_LATCH 22
#define IC74HC595_PIO_DATA_IN 23
#define IC74HC595_PIO_INDEX 1
#define IC74HC595_PIO_SM 0
#define IC74HC595_PIO_FREQ 20000000UL
#define IC74HC595_COUNT 4
#define IC74HC165_COUNT 4

/* Physical control pins of the serial chains. These names stay outside the
   shifted DOUT0-DOUT15 range. */
#define SHIFT_REGISTER_SDO DOUT20
#define SHIFT_REGISTER_CLK DOUT21
#define IC74HC595_LATCH DOUT22
#define SHIFT_REGISTER_SDI DIN31
#define IC74HC165_LOAD DOUT23

#define DOUT20_BIT 20
#define DOUT21_BIT 21
#define DOUT22_BIT 22
#define DOUT23_BIT 24

/* Output chain order: GPIO20 -> U13 -> U12 -> U11 -> U10.
   The carrier exposes seven physical axes, while the machine build uses A0-A2.
   The remaining mappings stay documented for the full board contract. Physical
   axes are not in numeric order: A4 and A6 use the documented non-sequential
   bit positions. */
#ifdef LEANCAM_TEST_MOTION_ON_GENERAL_OUTPUTS
/* Temporary bench mapping: visible general-output LEDs stand in for A0-A2.
   The 595 bytes are emitted U10 first and U13 last. D01 is U12.QB, so its
   logical offset is byte 1 bit 1, not the first general-output byte. */
#define STEP0_IO_OFFSET 9
#define DIR0_IO_OFFSET 10
#define STEP1_IO_OFFSET 11
#define DIR1_IO_OFFSET 12
#define STEP2_IO_OFFSET 13
#define DIR2_IO_OFFSET 14
#else
#define STEP0_IO_OFFSET 0
#define DIR0_IO_OFFSET 1
#define STEP1_IO_OFFSET 2
#define DIR1_IO_OFFSET 3
#define STEP2_IO_OFFSET 4
#define DIR2_IO_OFFSET 5
#endif
#define STEP3_IO_OFFSET 6
#define DIR3_IO_OFFSET 7
#define STEP6_IO_OFFSET 8
#define DIR6_IO_OFFSET 9
#define STEP5_IO_OFFSET 10
#define DIR5_IO_OFFSET 11
#define STEP4_IO_OFFSET 12
#define DIR4_IO_OFFSET 13

/* EN3 is shared by A0-A2 and EN4 by A3-A6. Giving every axis the same offset
   keeps the physical shared-enable contract visible to the core. */
#ifdef LEANCAM_TEST_MOTION_ON_GENERAL_OUTPUTS
#define STEP0_EN_IO_OFFSET 15
#define STEP1_EN_IO_OFFSET 15
#define STEP2_EN_IO_OFFSET 15
#else
#define STEP0_EN_IO_OFFSET 14
#define STEP1_EN_IO_OFFSET 14
#define STEP2_EN_IO_OFFSET 14
#endif
#define STEP3_EN_IO_OFFSET 15
#define STEP4_EN_IO_OFFSET 15
#define STEP5_EN_IO_OFFSET 15
#define STEP6_EN_IO_OFFSET 15

/* D01-D16 on J12/J32. */
#define DOUT0_IO_OFFSET 16
#define DOUT1_IO_OFFSET 17
#define DOUT2_IO_OFFSET 18
#define DOUT3_IO_OFFSET 19
#define DOUT4_IO_OFFSET 20
#define DOUT5_IO_OFFSET 21
#define DOUT6_IO_OFFSET 22
#define DOUT7_IO_OFFSET 23
#define DOUT8_IO_OFFSET 24
#define DOUT9_IO_OFFSET 25
#define DOUT10_IO_OFFSET 26
#define DOUT11_IO_OFFSET 27
#define DOUT12_IO_OFFSET 28
#define DOUT13_IO_OFFSET 29
#define DOUT14_IO_OFFSET 30
#define DOUT15_IO_OFFSET 31

/* Input chain U6 -> U7 -> U8 -> U9 -> GPIO23. Map the documented stream
   positions onto monotonic logical inputs where possible. */
#define DIN0_IO_OFFSET 15  /* I01 */
#define DIN1_IO_OFFSET 14  /* I02 */
#define DIN2_IO_OFFSET 13  /* I03 */
#define DIN3_IO_OFFSET 12  /* I04 */
#define DIN4_IO_OFFSET 11  /* I05 */
#define DIN5_IO_OFFSET 10  /* I06 */
#define DIN6_IO_OFFSET 9   /* I07 */
#define DIN7_IO_OFFSET 8   /* I08 */
#define DIN8_IO_OFFSET 7   /* I09 */
#define DIN9_IO_OFFSET 6   /* I10 */
#define DIN10_IO_OFFSET 5  /* I11 */
#define DIN11_IO_OFFSET 4  /* I12 */
#define DIN12_IO_OFFSET 3  /* I13 */
#define DIN13_IO_OFFSET 2  /* I14 */
#define DIN14_IO_OFFSET 1  /* I15 */
#define DIN15_IO_OFFSET 0  /* I16 */

#define DIN16_IO_OFFSET 30 /* A0 ERROR */
#define DIN17_IO_OFFSET 31 /* A0 ZERO */
#define DIN18_IO_OFFSET 18 /* A1 ERROR */
#define DIN19_IO_OFFSET 19 /* A1 ZERO */
#define DIN20_IO_OFFSET 16 /* A2 ERROR */
#define DIN21_IO_OFFSET 17 /* A2 ZERO */
#define DIN22_IO_OFFSET 22 /* A3 ERROR */
#define DIN23_IO_OFFSET 23 /* A3 ZERO */
#define DIN24_IO_OFFSET 28 /* A4 ERROR */
#define DIN25_IO_OFFSET 29 /* A4 ZERO */
#define DIN26_IO_OFFSET 25 /* A5 ERROR */
#define DIN27_IO_OFFSET 24 /* A5 ZERO */
#define DIN28_IO_OFFSET 27 /* A6 ERROR */
#define DIN29_IO_OFFSET 26 /* A6 ZERO */

/* Direct pins outside the extended chains. */
#define DIN30_BIT 2
#define DIN31_BIT 23
#define DIN30_PULLUP

#define ESTOP_BIT 9
/* U35 pulls GPIO9 low when the isolated E-stop loop is energized. This is the
   name uCNC actually consumes; `ESTOP_ACTIVE_LOW` is documentation only. */
#define INVERT_EMERGENCY_STOP

#define SPI_CLK_BIT 34
#define SPI_SDO_BIT 35
#define SPI_SDI_BIT 36
#define SPI_CS_BIT 37
#define SPI_PORT 0

/* The camera keypad uses the software-I2C backend, so it needs the two direct
   pins without pulling the Arduino Wire backend into the LVDS target. */
#define DOUT24_BIT 38
#define DOUT25_BIT 39
#define CAM_KB_I2C_SDA DOUT24
#define CAM_KB_I2C_SCL DOUT25

/* uCNC's own ACTIVITY_LED heartbeat is DOUT31. On this carrier that logical
   output is the WeAct module's onboard LED on GPIO25. ENC2 index also reaches
   GPIO25 through U28, but this build configures ENC0 only. */
#define DOUT31_BIT 25

/* AO0 is the first 0-10 V output and is the only analog output currently used
   as a configured PWM source. AI0-AI3 remain available for later modules. */
#define PWM0_BIT 46
#define PWM1_BIT 44
#define PWM2_BIT 45
#define PWM3_BIT 47

#define ANALOG0_BIT 40
#define ANALOG1_BIT 42
#define ANALOG2_BIT 41
#define ANALOG3_BIT 43

#define MCU_HAS_USB

#ifdef __cplusplus
}
#endif

#endif
