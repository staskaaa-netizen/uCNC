#ifndef NC_LAYOUT_H
#define NC_LAYOUT_H

/* The panel's shared numbers: how wide the panes are, where the
   footer starts, how big a 3x3 key is, how much stock the preview
   holds. They left nc_visual.c with the drawing code that reads
   them and belong to neither half on their own - the screens
   place things with them, nc_draw.c paints with them. */

#include "../lvds_renderer/lvds_hstx.h"

#define NC_PREVIEW_ARC_MAX_STEPS 64
#define NC_PREVIEW_CONTOUR_MAX 48
#ifndef NC_PREVIEW_DIN_STYLE
#define NC_PREVIEW_DIN_STYLE 1
#endif
#ifndef NC_PREVIEW_DIN_POINT_MARKERS
#define NC_PREVIEW_DIN_POINT_MARKERS 1
#endif
#define NC_LIVE_STOCK_MAX_W 680
#define NC_LIVE_STOCK_MAX_H 380
#define NC_LIVE_STOCK_PSRAM_OFFSET (512u * 1024u)
/* Room above the stock inside the preview pane. The caption that used to sit
   there is gone (the tab strip names the screen, the header names the file), so
   this is only the space the top ruler and its labels need; the rest of the
   pane belongs to the drawing. */
#define NC_PREVIEW_TOP_BAND 82
/* The live tool marker's size, in the pane it must stay inside. */
#define NC_LIVE_TOOL_GLYPH 20

#define NC_VISUAL_CHAR_W   8
#define NC_VISUAL_ROW_H    24
#define NC_LEFT_PANE_X     10
#define NC_LEFT_PANE_W     350
#define NC_SPLIT_X         366
#define NC_RIGHT_PANE_X    374
#define NC_RIGHT_PANE_W    410
#define NC_LINE_NO_X_PAD   4
#define NC_LINE_TEXT_X_PAD 32

/* Screen tabs across the very top: the screen name lives here and nowhere
   else, so the header below it is free to carry state and file. */
#define NC_TAB_Y           0
#define NC_TAB_H           24
/* A fault is a panel, not a line of text: the message area of the tab strip
   continued down over the DRO, so it is read before anything behind it. Its
   geometry is what keeps the machine's own readings out of it: it stops left of
   the F/S column and above the DRO's bottom-right corner, which belongs to the
   uCNC state, and runs to the panel's right edge otherwise - the FPS counter is
   a debug reading and lives below the DRO, not in this band. */
#define NC_FAULT_COLS      42
#define NC_FAULT_RIGHT_PAD 0
#define NC_FAULT_X         (LVDS_HSTX_WIDTH - NC_FAULT_RIGHT_PAD - NC_FAULT_W)
#define NC_FAULT_W         (NC_FAULT_COLS * NC_VISUAL_CHAR_W + 8)
#define NC_FAULT_LINES     3
#define NC_FAULT_ROW_H     18
#define NC_FAULT_H         (NC_FAULT_LINES * NC_FAULT_ROW_H + 4)
#define NC_HEADER_Y        (NC_TAB_Y + NC_TAB_H)
#define NC_HEADER_H        68
/* Code/graphic pane. The tab strip is paid for by the pane caption that used
   to repeat the file name above the code rows. */
#define NC_PANE_ROWS       18
#define NC_PANE_Y          (NC_HEADER_Y + NC_HEADER_H + 4)
#define NC_PANE_H          (NC_PANE_ROWS * NC_VISUAL_ROW_H)
#define NC_PANE_BOTTOM     (NC_PANE_Y + NC_PANE_H)
/* First row of the pane is the file the code belongs to; the code rows follow
   it and are one fewer than the pane's row units. */
#define NC_CODE_Y          (NC_PANE_Y + NC_VISUAL_ROW_H)
#define NC_FOOTER_Y        (NC_PANE_BOTTOM + 14)
#define NC_FOOTER_H        (LVDS_HSTX_HEIGHT - NC_FOOTER_Y)
/* Footer keys carry a short label; three lines is the whole button height. */
/* The frame counter is a debug reading: it sits in its own corner *below* the
   DRO, so nothing the operator reads has to make room for it. */
#define NC_FPS_X_PAD       8
#define NC_FPS_Y           (NC_PANE_Y + 4)
#define NC_FOOTER_LINES    3
/* The strip is the same eight slots on every screen: the keys are drawn at the
   same places and the same width whatever screen is up, and a screen with
   fewer entries leaves the rest empty rather than stretching its keys. */
#define NC_FOOTER_SLOTS    8
/* EDIT's full-screen preview (`# FULL`): the whole body, with a small margin
   from the panel edges. */
#define NC_FULL_PREVIEW_X  20
#define NC_FULL_PREVIEW_W  (LVDS_HSTX_WIDTH - 40)

/* MANUAL readout row: the axis and the two limits it works between (minus and
   plus) - one row per axis - and, below the axis rows, the STEP and FEED values
   the value keys change. */
#define NC_MANUAL_ROW_H    62
#define NC_MANUAL_COL_X    40
/* The touch-off value `D` types: after the row's label and before its first
   stop, in the one font the pane's values share, so the row reads left to right
   - what this line is, the position being set, the two limits it works
   between. */
#define NC_MANUAL_COL_TOUCH 104
#define NC_MANUAL_TOUCH_W   (9 * NC_VISUAL_CHAR_W)
/* The two stops sit left of centre: the machine figures and F/S are in the
   header DRO, so nothing needs the middle of the pane, and the values read
   closer to the axis letter they belong to. */
#define NC_MANUAL_COL_STOP 180
#define NC_MANUAL_COL_STOP_PLUS 280
/* The row's text is centred in the band the mark covers. */
#define NC_MANUAL_TEXT_DY  6
/* The picked axis (and the value row in use) is marked across the values it
   owns, not the whole pane: the mark ends past the plus stop column, well short
   of the pad in the corner. */
#define NC_MANUAL_MARK_RIGHT (NC_MANUAL_COL_STOP_PLUS + 9 * NC_VISUAL_CHAR_W + 16)

/* Floating 3x3 helper: the nine keys and nothing else. No panel box and no
   title strip - the editor line above it carries the label - so the keys read
   like the footer's own buttons. */
#define NC_MODAL_ROWS  3
#define NC_MODAL_COLS  3
#define NC_MODAL_KEY_W 72
#define NC_MODAL_KEY_H 72
#define NC_MODAL_PAD   2
#define NC_MODAL_W     (NC_MODAL_KEY_W * NC_MODAL_COLS + NC_MODAL_PAD * 2)
#define NC_MODAL_H     (NC_MODAL_KEY_H * NC_MODAL_ROWS + NC_MODAL_PAD * 2)
/* Glyph height of the bitmap font the helper labels use. */
#define NC_FONT_NORMAL_H 14

/* Every key is cut at its top-right corner: the chamfer is a quarter of the
   key's height, so the shape follows the button instead of being a fixed number
   of pixels, and a short key is not all cut. The cut is page background - the
   key's own colour does not grow a corner - and its edge is drawn like the rest
   of the outline, so the key reads as one machined button. */
#define NC_KEY_CHAMFER_DIVISOR 4

#endif
