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
    NC_MODE_TOOLS,
    NC_MODE_RUN,
    NC_MODE_COUNT
} nc_mode_t;

typedef enum {
    NC_FOOTER_ACTION_NONE = 0,
    NC_FOOTER_ACTION_OPEN,
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
    /* MANUAL: pick the axis the readout, zero and touch-off act on. Jog,
       feed override and spindle live on the 3x3 digits there. */
    NC_FOOTER_ACTION_AXIS_PREV,
    NC_FOOTER_ACTION_AXIS_NEXT,
    NC_FOOTER_ACTION_TOUCH,
    NC_FOOTER_ACTION_ZERO,
    NC_FOOTER_ACTION_FILE,
    NC_FOOTER_ACTION_REFRESH,
    NC_FOOTER_ACTION_FILES,
    NC_FOOTER_ACTION_DIMS,
    NC_FOOTER_ACTION_ROUGH,
    /* A pad slot the card's own section fills: the id is the key path, so the
       section lands where it says and a card can add one of its own. */
    NC_FOOTER_ACTION_PRESET_ID,
    /* SIM: show or hide the code pane. Hidden, the preview has the whole body. */
    NC_FOOTER_ACTION_VIEW,
    NC_FOOTER_ACTION_OPS,
    NC_FOOTER_ACTION_TOOL_MENU,
    NC_FOOTER_ACTION_GCODE,
    NC_FOOTER_ACTION_G7X_MENU,
    NC_FOOTER_ACTION_SYNC_MENU,
    NC_FOOTER_ACTION_PECK_MENU,
    NC_FOOTER_ACTION_TOOL_SELECT,
    NC_FOOTER_ACTION_TOOL_EDIT,
    NC_FOOTER_ACTION_G7X_Q,
    NC_FOOTER_ACTION_G7X_N,
    /* The G7X submenu's PATH entry: open the 3x3 path builder on the block the
       cursor is in (nc_path_builder.c). */
    NC_FOOTER_ACTION_BUILD
} nc_footer_action_t;

typedef struct {
    char key;
    const char *label;
    uint8_t action;
} nc_footer_item_t;

const char *nc_menu_mode_name(nc_mode_t mode);
const nc_footer_item_t *nc_menu_footer(nc_mode_t mode, bool file_view, size_t *count);
/* EDIT with the preview on the whole body: the same slots, showing the
   preview's own keys instead of the code-entry ones. */
const nc_footer_item_t *nc_menu_preview_footer(size_t *count);
const nc_footer_item_t *nc_menu_submenu(nc_footer_action_t parent, size_t *count);
/* The footer key a submenu hangs off ('1'..'9', 0 when it hangs off none): the
   first digit of every id inside that pad. */
char nc_menu_submenu_digit(nc_footer_action_t parent);

#ifdef __cplusplus
}
#endif

#endif
