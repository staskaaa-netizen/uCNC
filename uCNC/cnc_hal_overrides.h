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
    ESP32/MKS Tinybee setup: G33, ESP32 PCNT encoder, 74HC595,
    WiFi, full generic SD/filesystem setup, and real machine-runtime extras.
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

#define SD_CARD_DETECT_PIN 255
#define SD_CARD_SPI_DMA false

/*
    Load the RP2350 LeanCam UI, display, storage, and parser helper modules.

    Do not load full file_system here yet. It has twice killed early boot on
    RP2350-LEANCAM-HDMI: no HSTX video and no USB serial. The current SD module
    intentionally provides a small fs_* facade for LeanCam until the full uCNC
    filesystem can be staged safely.
*/
#define LOAD_MODULES_OVERRIDE() ({LOAD_MODULE(g7_g8); LOAD_MODULE(ui_snapshot_builder); LOAD_MODULE(leancam_rp2350_hdmi); LOAD_MODULE(leancam_rp2350_sd);})

#ifdef __cplusplus
}
#endif
#endif
