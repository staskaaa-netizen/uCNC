#ifndef CNC_HAL_OVERRIDES_H
#define CNC_HAL_OVERRIDES_H
#ifdef __cplusplus
extern "C"
{
#endif

#include "cnc_hal_reset.h"

/*
    RP2350 LeanCam HDMI first-stage HAL profile.

    This keeps uCNC small and avoids modules/pins that belong to the old
    ESP32/MKS Tinybee setup: G33, ESP32 PCNT encoder, I2C keyboard, 74HC595,
    WiFi, SD/filesystem, and real machine-runtime extras.
*/

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

#define ENCODERS 0

#define IC74HC595_COUNT 0
#define IC74HC165_COUNT 0

#define ENABLE_MAIN_LOOP_MODULES
#define ENABLE_IO_MODULES
#define ENABLE_PARSER_MODULES
#define ENABLE_RT_SYNC_MOTIONS
#define ENABLE_ITP_FEED_TASK

/* Keep settings in RAM for now; flash/SD persistence can return later. */
#define RAM_ONLY_SETTINGS

/*
    SD is tested through a delayed Arduino SDFS probe only. The current uCNC
    SD/file-system layer is parked because linking it breaks video/serial.
*/
#define SD_CARD_DETECT_PIN 255
#define SD_CARD_SPI_DMA false

/* Load only the RP2350 HDMI smoke module for this bring-up target. */
#define LOAD_MODULES_OVERRIDE() ({LOAD_MODULE(ui_snapshot_builder); LOAD_MODULE(leancam_rp2350_hdmi); LOAD_MODULE(leancam_sd_fatfs_probe);})

#ifdef __cplusplus
}
#endif
#endif
