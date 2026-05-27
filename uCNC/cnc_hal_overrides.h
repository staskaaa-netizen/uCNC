#ifndef CNC_HAL_OVERRIDES_H
#define CNC_HAL_OVERRIDES_H
#ifdef __cplusplus
extern "C"
{
#endif

#include "cnc_hal_reset.h"

/*
    RP2350 LeanCam LVDS first-stage HAL profile.

    This keeps uCNC small and avoids modules/pins that belong to the old
    ESP32/MKS Tinybee setup: G33, ESP32 PCNT encoder, 74HC595,
    WiFi, full generic SD/filesystem setup, and real machine-runtime extras.
*/

#ifdef UCNC_LEANCAM_LVDS_TARGET
#undef DISABLE_G7_G8
#define ENABLE_SD_CARD_V2
#define ENABLE_LEANCAM_SD_DEBUG
#define ENABLE_UCNC_FILE_SYSTEM
#define LEANCAM_BUILD_FEATURE_BANNER
#define ENABLE_PERSISTENT_SETTINGS
#undef RAM_ONLY_SETTINGS
#define SD_CARD_NO_SYSTEM_MENU
#define LEANCAM_USE_HSTX_PLL 0
#define LEANCAM_USE_PSRAM_FB 0
#define LEANCAM_USE_PSRAM_BACKBUFFER 1
#define LEANCAM_USE_PSRAM_LIVE_SIM 0
#define ENABLE_LVDS_RENDERER
#define RP2350_DISABLE_ARDUINO_CORE1_LOOP
#define LEANCAM_RP2350_DIRECT_STREAM
#define LEANCAM_RP2350_SOFT_CAM_KEYBOARD
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


#define IC74HC595_COUNT 0
#define IC74HC165_COUNT 0

#define ENABLE_MAIN_LOOP_MODULES
#define ENABLE_IO_MODULES
#define ENABLE_PARSER_MODULES

#ifndef ENABLE_PERSISTENT_SETTINGS
/* Keep settings in RAM for minimal bring-up targets. */
#define RAM_ONLY_SETTINGS
#endif

#ifdef ENABLE_SD_CARD_V2
/* Onboard SD wiring is not a valid single RP2350 hardware SPI pin group; use software SPI. */
#define SD_CARD_DETECT_PIN 255
#define SD_CARD_INTERFACE 0
#define SD_CARD_SPI_DMA false
#endif


// ------------------------------------------------------------
// Encoder + G33 modules
// ------------------------------------------------------------

// Enable this from the HAL config, not from platformio.ini build flags.
#define ENABLE_RP2350_PIO_ENCODER

#ifdef ENABLE_RP2350_PIO_ENCODER
#define LEANCAM_FEATURE_BANNER

#define ENCODERS 1
#define ENC0_TYPE ENC_TYPE_CUSTOM

#define ENC0_PULSE_GPIO 20   // A
// B must be GPIO21
#define ENC0_INDEX 255
#define ENC0_INDEX_GPIO 26

#define ENC0_PIO_INDEX 0
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

#define ENC0_INDEX_VIRTUAL_FIRE_HOOK 1
#define ENC0_VIRTUAL_INDEXES_PER_REV 5
#define ENC0_VIRTUAL_MAX_CATCHUP_SLOTS 8
#define ENC0_INDEX_AUTO_ORIGIN 0
#endif

#ifndef LVDS_HSTX_CONFIG_H
#define LVDS_HSTX_CONFIG_H

// Sharp LQ121S1LG44 native single-channel LVDS mode.
#define LVDS_WIDTH 800
#define LVDS_HEIGHT 600
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

/* Single RP2350 LeanCam target: encoder/G33 + SD + LVDS renderer. */
#define LOAD_MODULES_OVERRIDE() ({LOAD_MODULE(rp2350_pio_encoder); LOAD_MODULE(g33); LOAD_MODULE(sd_card_v2); LOAD_MODULE(lvds_renderer); })

#ifdef __cplusplus
}
#endif
#endif
