#ifndef CNC_HAL_OVERRIDES_H
#define CNC_HAL_OVERRIDES_H
#ifdef __cplusplus
extern "C"
{
#endif

#include "cnc_hal_reset.h"

/* Module/runtime configuration for this HAL profile. Keep these in overrides,
   not in PlatformIO target flags or core helper headers. */
#ifndef UCNC_MINIMAL_RP2350
#undef DISABLE_G7_G8

#define ENABLE_SD_CARD_V2
#define ENABLE_UCNC_FILE_SYSTEM
#define ENABLE_PERSISTENT_SETTINGS
#undef RAM_ONLY_SETTINGS
#define SD_CARD_NO_SYSTEM_MENU

#define LEANCAM_BUILD_FEATURE_BANNER
#ifndef LEANCAM_USE_PSRAM_BACKBUFFER
#define LEANCAM_USE_PSRAM_BACKBUFFER 1
#endif
#define LEANCAM_NC_VIEW_CACHE_PROGRAM 1

#define RP2350_DISABLE_ARDUINO_CORE1_LOOP
#endif

#define S_CURVE_ACCELERATION_LEVEL 0

#define LIMIT_X_PULLUP_ENABLE
#define LIMIT_Y_PULLUP_ENABLE
#define LIMIT_Z_PULLUP_ENABLE

#define ESTOP_PULLUP_ENABLE
#define FHOLD_PULLUP_ENABLE
#define CS_RES_PULLUP_ENABLE

#define TOOL1 spindle_pwm
#define SPINDLE_PWM PWM0
#define SPINDLE_PWM_DIR DOUT0


#ifndef IC74HC595_COUNT
#define IC74HC595_COUNT 0
#endif
#ifndef IC74HC165_COUNT
#define IC74HC165_COUNT 0
#endif

#define ENABLE_MAIN_LOOP_MODULES
#define ENABLE_IO_MODULES
#define ENABLE_PARSER_MODULES

#ifndef ENABLE_PERSISTENT_SETTINGS
/* Keep settings in RAM for minimal bring-up targets. */
#define RAM_ONLY_SETTINGS
#endif

#ifdef ENABLE_SD_CARD_V2
#ifdef LEANCAM_WEACT_RP2350B
#define SD_CARD_DETECT_PIN DIN30
#define SD_CARD_INTERFACE 1
#else
#define SD_CARD_DETECT_PIN 255
#define SD_CARD_INTERFACE 0
#endif
#define SD_CARD_SPI_DMA false
/* Bench trial: upstream disk initialization without our boot/retry workaround.
   Rollback reference: checkpoint/sd-working-20260916. */
#define SD_CARD_BOOT_DELAY_MS 0
#define SD_CARD_MOUNT_RETRIES 1
#define SD_CARD_MOUNT_RETRY_DELAY_MS 0
#define ENABLE_LEANCAM_SD_DEBUG
#endif


// ------------------------------------------------------------
// Encoder + G33 modules
// ------------------------------------------------------------

#ifndef UCNC_MINIMAL_RP2350
// Enable this from the HAL config, not from platformio.ini build flags.
#define ENABLE_RP2350_PIO_ENCODER

#ifdef ENABLE_RP2350_PIO_ENCODER
#define LEANCAM_FEATURE_BANNER
#define LEANCAM_DEBUG_BRIDGE 1
#define CAM_KB_DEBUG 1

#define ENC0_TYPE ENC_TYPE_CUSTOM

#ifdef LEANCAM_WEACT_RP2350B
#define ENCODERS 3
#define RP2350_PIO_ENCODER_GPIO_BASE 16
/* The board pairs encoder A=GPIO33 and B=GPIO32. The PIO backend consumes an
   ascending adjacent pair, so start on B and invert the counted direction. */
#define ENC0_PULSE_GPIO 32
#define ENC0_RP2350_ENCODER_INVERT 1
#define ENC0_INDEX_GPIO 31

#define ENC1_TYPE ENC_TYPE_CUSTOM
#define ENC1_PULSE_GPIO 29
#define ENC1_RP2350_ENCODER_INVERT 1
#define ENC1_INDEX_GPIO 255
#define ENC1_PIO_INDEX 2
#define ENC1_PIO_SM 0
#define ENC1_MAX_STEP_RATE 0
#define ENC1_PIO_PROGRAM_OFFSET 0
#define ENC1_IS_INCREMENTAL
#define ENC1_NO_WRAP_CORRECTION
#define ENC1_CPR 4000
#define ENC1_INDEX 255

#define ENC2_TYPE ENC_TYPE_CUSTOM
#define ENC2_PULSE_GPIO 26
#define ENC2_RP2350_ENCODER_INVERT 1
#define ENC2_INDEX_GPIO 25
#define ENC2_PIO_INDEX 2
#define ENC2_PIO_SM 1
#define ENC2_MAX_STEP_RATE 0
#define ENC2_PIO_PROGRAM_OFFSET 0
#define ENC2_IS_INCREMENTAL
#define ENC2_NO_WRAP_CORRECTION
#define ENC2_CPR 4000
#define ENC2_INDEX 255
#else
#define ENCODERS 1
#define ENC0_PULSE_GPIO 20   // A
// B must be GPIO21
#define ENC0_INDEX_GPIO 26
#endif
#define ENC0_INDEX 255

#define ENC0_PIO_INDEX 1
#define ENC0_PIO_SM    0
#define ENC0_MAX_STEP_RATE 0
#define ENC0_PIO_PROGRAM_OFFSET 0

#define ENC0_IS_INCREMENTAL
#define ENC0_CPR 4000


//#define ENCODER_DEBUG_PRINT_100MS  1 // optional test only

#define SPINDLE_PWM_RPM_ENCODER ENC0

#define G33_ENCODER ENC0
#define G33_FEEDBACK_LOOP_USE_HW_COUNTER
#define G33_CORRECTION_GAIN 1.0f

#define G33_DEBUG
#define G33_DEBUG_EVERY_N 5

#define ENC0_VIRTUAL_INDEX 1
#define ENC0_VIRTUAL_INDEX_CPR (ENC0_CPR / 10)
#define ENC0_VIRTUAL_INDEX_OFFSET 0
#define ENC0_VIRTUAL_INDEX_HYSTERESIS 1
#endif

#ifndef LVDS_HSTX_CONFIG_H
#define LVDS_HSTX_CONFIG_H

// Sharp LQ121S1LG44 native single-channel LVDS mode.
#define LVDS_WIDTH 800
#define LVDS_HEIGHT 600
/* The glass is 800x600, but the frame carries four more **active** lines than
   the glass shows. The bench's marker probe (2026-10-04) put five known
   colours on the frame's last five lines and one came out **twice**: the close
   of the active area repeats a line, whatever content is there - our content,
   our blanking (an extra-blanking-line probe changed nothing) and the clock all
   ruled out. The bench then walked the active height down: **602** shows an
   extra black line, **603** and **604** are clean, **600** puts the repeat back
   on the picture - so the close needs at least three tail lines, and four keeps
   one line of margin. (Root cause still unknown: the panel/link repeats or eats
   a line at the close of the active area.) The view, the layout and the station
   stay the glass's own 600x800. */
#define LVDS_PANEL_ACTIVE_HEIGHT 604
#define LVDS_SYS_CLOCK_KHZ 280000
#define LVDS_HSTX_PLL_KHZ 280000
#define LVDS_HSTX_CLOCK_DIV 2

// Ascending HSTX GPIO pinout, matching lvds_hstx.c.

#define LVDS_D0P 12
#define LVDS_D0M 13
#define LVDS_D1P 14
#define LVDS_D1M 15
#define LVDS_D2P 16
#define LVDS_D2M 17
#define LVDS_CLKP 18
#define LVDS_CLKM 19

#endif

/* Single RP2350 NC target: encoder/G33 + SD + LVDS renderer. The panel is nc2
   (nc is retired) or ncc, and the env picks it: `ENABLE_NCC_MODULE` is the
   clean rebuild, anything else keeps nc2. This override is where the machine's
   module set is written, so it is the line the switch really turns on. */
#ifdef ENABLE_NCC_MODULE
#define UCNC_PANEL_LOAD() LOAD_MODULE(ncc)
#else
#define UCNC_PANEL_LOAD() LOAD_MODULE(nc2)
#endif
#define LOAD_MODULES_OVERRIDE() ({LOAD_MODULE(shift_register); LOAD_MODULE(rp2350_pio_encoder); LOAD_MODULE(g33); LOAD_MODULE(sd_card_v2); UCNC_PANEL_LOAD(); LOAD_MODULE(g7x); })
#endif

#ifdef __cplusplus
}
#endif
#endif
