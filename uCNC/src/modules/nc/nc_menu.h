#ifndef NC_MENU_H
#define NC_MENU_H

#include "nc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NC_MODE_MANUAL = 0,
    NC_MODE_PROGRAM,
    NC_MODE_SIM,
    NC_MODE_MDI,
    NC_MODE_TOOLS,
    NC_MODE_RUN,
    NC_MODE_COUNT
} nc_mode_t;

typedef enum {
    NC_FOOTER_ACTION_NONE = 0,
    NC_FOOTER_ACTION_OPEN,
    NC_FOOTER_ACTION_SAVE,
    NC_FOOTER_ACTION_NEW,
    NC_FOOTER_ACTION_FIELD,
    NC_FOOTER_ACTION_INSERT,
    NC_FOOTER_ACTION_DELETE,
    NC_FOOTER_ACTION_SPECIAL,
    NC_FOOTER_ACTION_STEP,
    NC_FOOTER_ACTION_BACK,
    NC_FOOTER_ACTION_FULL,
    NC_FOOTER_ACTION_STOCK,
    NC_FOOTER_ACTION_PATH,
    NC_FOOTER_ACTION_RESET,
    NC_FOOTER_ACTION_SINGLE,
    NC_FOOTER_ACTION_FROM,
    NC_FOOTER_ACTION_HOLD,
    NC_FOOTER_ACTION_STOP,
    NC_FOOTER_ACTION_SEND,
    NC_FOOTER_ACTION_CLEAR,
    NC_FOOTER_ACTION_HISTORY,
    NC_FOOTER_ACTION_COPY,
    NC_FOOTER_ACTION_TOOL,
    NC_FOOTER_ACTION_JOG_NEG,
    NC_FOOTER_ACTION_JOG_POS,
    NC_FOOTER_ACTION_INCREMENT,
    NC_FOOTER_ACTION_ZERO,
    NC_FOOTER_ACTION_FILE,
    NC_FOOTER_ACTION_REFRESH,
    NC_FOOTER_ACTION_PRESET_OD,
    NC_FOOTER_ACTION_PRESET_ID,
    NC_FOOTER_ACTION_PRESET_FACE,
    NC_FOOTER_ACTION_PRESET_LINE,
    NC_FOOTER_ACTION_PRESET_ARC,
    NC_FOOTER_ACTION_PRESET_SETUP,
    NC_FOOTER_ACTION_PRESET_END,
    NC_FOOTER_ACTION_FILES
} nc_footer_action_t;

typedef struct {
    char key;
    const char *label;
    uint8_t action;
} nc_footer_item_t;

const char *nc_menu_mode_name(nc_mode_t mode);
const nc_footer_item_t *nc_menu_footer(nc_mode_t mode, bool file_view, size_t *count);

#ifdef __cplusplus
}
#endif

#endif
