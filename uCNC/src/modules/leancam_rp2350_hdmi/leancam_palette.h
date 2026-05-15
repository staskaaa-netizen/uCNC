#ifndef LEANCAM_PALETTE_H
#define LEANCAM_PALETTE_H

#include "leancam_display.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    gray_192 = 0,
    gray_128,
    gray_160,
    gray_96,
    black,
    white_warm,
    yellow,
    yellow_light,
    red,
    green,
    white,
    yellow_pale,
    brown,
    red_bright,
    green_bright,
    LC_COLOR_COUNT
} lc_palette_color_id_t;

typedef enum {
    /* Main LeanCam screen. */
    LC_ELEM_BACKGROUND = 0,
    LC_ELEM_HEADER,
    LC_ELEM_PANEL,
    LC_ELEM_BORDER,
    LC_ELEM_TEXT,
    LC_ELEM_SECONDARY_TEXT,
    LC_ELEM_VALUE_TEXT,
    LC_ELEM_SELECTED_TEXT,
    LC_ELEM_ERROR,
    LC_ELEM_OK,
    LC_ELEM_STOCK,
    LC_ELEM_SELECTED_ROW,
    LC_ELEM_FOOTER_BG,
    LC_ELEM_FOOTER_TEXT,
    LC_ELEM_FOOTER_VALUE,
    LC_ELEM_CUT,
    LC_ELEM_HATCH,
    LC_ELEM_TOOL,

    /* Draft/full sim preview. */
    LC_ELEM_PREVIEW_BG,
    LC_ELEM_PREVIEW_FRAME,
    LC_ELEM_PREVIEW_TEXT,
    LC_ELEM_PREVIEW_DIM_TEXT,
    LC_ELEM_PREVIEW_VALUE_TEXT,
    LC_ELEM_PREVIEW_ACTIVE_TEXT,
    LC_ELEM_PREVIEW_ACTIVE_BG,
    LC_ELEM_PREVIEW_STOCK,
    LC_ELEM_PREVIEW_CHUCK,
    LC_ELEM_PREVIEW_CHUCK_TEXT,
    LC_ELEM_PREVIEW_CUT,
    LC_ELEM_PREVIEW_HATCH,
    LC_ELEM_PREVIEW_PROFILE,
    LC_ELEM_PREVIEW_TOOL,
    LC_ELEM_PREVIEW_TOOL_MARK,
    LC_ELEM_PREVIEW_TOOL_OUTLINE,

    /* Live material-removal screen. */
    LC_ELEM_LIVE_BG,
    LC_ELEM_LIVE_FRAME,
    LC_ELEM_LIVE_TEXT,
    LC_ELEM_LIVE_VALUE_TEXT,
    LC_ELEM_LIVE_DEBUG_TEXT,
    LC_ELEM_LIVE_STOCK,
    LC_ELEM_LIVE_AXIS,
    LC_ELEM_LIVE_TOOL,
    LC_ELEM_LIVE_TOOL_MARK,
    LC_ELEM_LIVE_TOOL_OUTLINE,
    LC_ELEM_LIVE_CHUCK,
    LC_ELEM_LIVE_CHUCK_OUTLINE,
    LC_ELEM_LIVE_COLLISION,
    LC_ELEM_COUNT
} lc_palette_element_id_t;

void lc_palette_init(void);
void lc_palette_reset(void);
lc_color_t lc_palette_color(lc_palette_color_id_t id);
lc_color_t lc_palette_element(lc_palette_element_id_t id);
const char *lc_palette_color_name(lc_palette_color_id_t id);
const char *lc_palette_element_name(lc_palette_element_id_t id);

#ifdef __cplusplus
}
#endif

#endif
