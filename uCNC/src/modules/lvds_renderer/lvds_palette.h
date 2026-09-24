#ifndef LVDS_PALETTE_H
#define LVDS_PALETTE_H

#include "lvds_hstx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The colours the panel may draw with. **This table is full**: sixteen entries
   against `LVDS_RENDERER_MAX_PALETTE_COLORS` (the HSTX output is paletted, and
   `lvds_palette.c` fails the build above it). A new colour therefore means
   retiring one - reuse an entry, or give the new thing an element below instead
   of a colour. Elements are free: only the colours are capped. */
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
    tool_orange,
    red_bright,
    green_bright,
    /* The NC TOOLS screen's tool tip block (nc_palette.c's NC_VISUAL_TOOL_TIP). */
    tool_tip,
    LC_COLOR_COUNT
} lvds_palette_color_id_t;

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
} lvds_palette_element_id_t;

void lvds_palette_init(void);
void lvds_palette_reset(void);
lvds_color_t lvds_palette_color(lvds_palette_color_id_t id);
lvds_color_t lvds_palette_element(lvds_palette_element_id_t id);
const char *lvds_palette_color_name(lvds_palette_color_id_t id);
const char *lvds_palette_element_name(lvds_palette_element_id_t id);

#ifdef __cplusplus
}
#endif /* LVDS_PALETTE_H */

#endif
