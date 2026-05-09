#ifndef BOADMAP_OVERRIDES_H
#define BOADMAP_OVERRIDES_H
#ifdef __cplusplus
extern "C"
{
#endif

#include "boardmap_reset.h"
#define MCU MCU_ESP32
#define KINEMATIC KINEMATIC_CARTESIAN
#define AXIS_COUNT 3
#define TOOL_COUNT 1
#define BAUDRATE 115200
#define BOARD_NAME "MKS Tinybee"
#define UART_PORT 0
#define UART2_PORT 0
//#define SPI_PORT 1
//#define SPI2_PORT 1
#define I2C_PORT 0
#define ITP_TIMER 3
#define SERVO_TIMER 1
#define ONESHOT_TIMER 2
#define STEP0_IO_OFFSET 1
#define STEP1_IO_OFFSET 4
#define STEP2_IO_OFFSET 7
#define STEP3_IO_OFFSET 10
#define STEP4_IO_OFFSET 13
#define DIR0_IO_OFFSET 2
#define DIR1_IO_OFFSET 5
#define DIR2_IO_OFFSET 8
#define DIR3_IO_OFFSET 11
#define DIR4_IO_OFFSET 14
#define STEP0_EN_IO_OFFSET 0
#define STEP1_EN_IO_OFFSET 3
#define STEP2_EN_IO_OFFSET 6
#define STEP3_EN_IO_OFFSET 9
#define STEP4_EN_IO_OFFSET 12
#define PWM0_IO_OFFSET 16
#define PWM1_IO_OFFSET 17
#define PWM2_IO_OFFSET 18
#define PWM3_IO_OFFSET 19
#define PWM4_IO_OFFSET 20
#define DOUT0_IO_OFFSET 22
#define DOUT2_IO_OFFSET 23
#define DOUT4_BIT 4
#define DOUT5_BIT 21
#define DOUT6_BIT 0
#define DOUT7_IO_OFFSET 21
#define DOUT25_BIT 25
#define DOUT26_BIT 26
#define DOUT27_BIT 27
#define LIMIT_X_BIT 33
#define LIMIT_Y_BIT 32
#define LIMIT_Z_BIT 22

#define DIN4_BIT 34
#define DIN4_ISR
#define DIN5_BIT 2
#define DIN5_ISR


#define I2C_CLK_BIT 14
#define I2C_DATA_BIT 12
//#define PROBE_BIT 2
//#define PROBE_PULLUP
/*#define ESTOP_BIT 13
#define ESTOP_PULLUP*/

/*#define FHOLD_BIT 36 //red (black connector)
#define FHOLD_ISR
#define CS_RES_BIT 39 //green (white connector)
#define CS_RES_ISR*/


/*#define DIN16_BIT 13
#define DIN16_PULLUP
#define DIN17_BIT 12
#define DIN17_PULLUP
#define DIN18_BIT 14
#define DIN18_PULLUP*/

//#define ENABLE_ENCODER_MODULE



#define TX_BIT 1
#define RX_BIT 3
#define SPI_CLK_BIT 18
#define SPI_SDI_BIT 19
#define SPI_SDO_BIT 23
#define SPI_CS_BIT 5

#define SPI2_CLK_BIT 21
#define SPI2_SDI_BIT 13  //15
#define SPI2_SDO_BIT 16
#define SPI2_CS_BIT 0 //17
#define IC74HC595_CUSTOM_SHIFT_IO
//Custom configurations
#define RX_PULLUP 
#define IC74HC595_I2S_WS 26
#define IC74HC595_I2S_CLK 25
#define IC74HC595_I2S_DATA 27
#define IC74HC595_COUNT 4
#define SPI2_FREQ  25000000UL

#define DEFAULT_CONTROL_INV_MASK 12


#ifdef __cplusplus
}
#endif
#endif
