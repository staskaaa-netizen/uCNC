#ifndef NC2_LAYOUT_H
#define NC2_LAYOUT_H

#include "../lvds_renderer/lvds_hstx.h"

/* The panel's numbers, and they are **the same split nc uses** (`nc_layout.h`):
   the screens have to sit identically, so the code pane, the preview pane and
   the pad's corner are the same places on the glass whichever module is driving
   them. What is gone is the footer and the DRO strip - the pad is the keys and
   the machine's numbers wait for the run - so the two panes are taller here by
   exactly what those bands took. */

#define NC2_CHAR_W 8
#define NC2_ROW_H 24

/* The header line: what the screen is and which file. */
#define NC2_HEADER_H 26

#define NC2_PANE_Y (NC2_HEADER_H + 6)
#define NC2_PANE_BOTTOM (LVDS_HSTX_HEIGHT - 8)
#define NC2_PANE_H (NC2_PANE_BOTTOM - NC2_PANE_Y)

/* The code pane, and the preview beside it: nc's own split. */
#define NC2_LEFT_PANE_X 10
#define NC2_LEFT_PANE_W 350
#define NC2_SPLIT_X 366
#define NC2_RIGHT_PANE_X 374
#define NC2_RIGHT_PANE_W 410
#define NC2_LINE_NO_PAD 4
#define NC2_LINE_TEXT_PAD 32

/* The pad, pinned in the bottom right corner of the preview pane - the corner
   the operator's eye is already in, and out of the code's way. */
#define NC2_PAD_W 234
#define NC2_PAD_H 216
#define NC2_PAD_X (NC2_RIGHT_PANE_X + NC2_RIGHT_PANE_W - NC2_PAD_W - 8)
#define NC2_PAD_Y (NC2_PANE_BOTTOM - NC2_PAD_H)

/* The code rows the pane holds, and the files the list shows. */
#define NC2_CODE_ROWS ((NC2_PANE_H - 4) / NC2_ROW_H)
#define NC2_FILES_MAX 32
#define NC2_NAME_MAX 40

#endif
