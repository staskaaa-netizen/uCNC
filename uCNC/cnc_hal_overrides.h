#ifndef CNC_HAL_OVERRIDES_H
#define CNC_HAL_OVERRIDES_H
#ifdef __cplusplus
extern "C"
{
#endif

#include "cnc_hal_reset.h"
#define S_CURVE_ACCELERATION_LEVEL 0
#define ESTOP_PULLUP_ENABLE
#define SAFETY_DOOR_PULLUP_ENABLE
#define FHOLD_PULLUP_ENABLE
#define CS_RES_PULLUP_ENABLE
#define LIMIT_X_PULLUP_ENABLE
#define LIMIT_Y_PULLUP_ENABLE
#define LIMIT_Z_PULLUP_ENABLE
#define LIMIT_X2_PULLUP_ENABLE
#define LIMIT_Y2_PULLUP_ENABLE
#define LIMIT_Z2_PULLUP_ENABLE
#define TOOL1 spindle_pwm
#define SPINDLE_PWM PWM0
#define SPINDLE_PWM_DIR DOUT0
#define ENCODERS 0
#define ENABLE_MAIN_LOOP_MODULES
#define ENABLE_IO_MODULES
#define ENABLE_PARSER_MODULES
#define ENABLE_RT_SYNC_MOTIONS
#define ENABLE_ITP_FEED_TASK
#define SD_CARD_INTERFACE SD_CARD_HW_SPI
#define SD_SPI_CS SPI_CS
//#define SD_CARD_DETECT_PIN DIN4
#define FF_USE_LFN 1
#define SD_CARD_SPI_DMA false


//Custom configurations
//#define DISABLE_SYSTEM_MENU

#undef SD_CARD_DETECT_PIN
#define SD_CARD_DETECT_PIN 255

//#define DISABLE_RTC_CODE


#define LOAD_MODULES_OVERRIDE() ({LOAD_MODULE(sd_card_v2);LOAD_MODULE(ui_snapshot_builder);LOAD_MODULE(ra_renderer);LOAD_MODULE(g7_g8);}) 

#ifdef __cplusplus
}
#endif
#endif
