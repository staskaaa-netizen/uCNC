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
#define NC_FOOTER_LINES    3
/* The strip is the same eight slots on every screen: the keys are drawn at the
   same places and the same width whatever screen is up, and a screen with
   fewer entries leaves the rest empty rather than stretching its keys. */
#define NC_FOOTER_SLOTS    8
/* EDIT's full-screen preview (`# FULL`): the whole body, with a small margin
   from the panel edges. */
#define NC_FULL_PREVIEW_X  20
#define NC_FULL_PREVIEW_W  (LVDS_HSTX_WIDTH - 40)

/* MANUAL readout row: the axis letter, its position in the offset in use, the
   stop and the machine figure the offset is cut from - three numbers on the
   line, one row per axis. */
#define NC_MANUAL_ROW_H    62
#define NC_MANUAL_COL_X    40
#define NC_MANUAL_COL_POS  76
#define NC_MANUAL_COL_STOP 250
#define NC_MANUAL_COL_MACH 340

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

#endif
