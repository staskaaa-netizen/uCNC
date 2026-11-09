#ifndef NCC_LAYOUT_H
#define NCC_LAYOUT_H

#include "../lvds_renderer/lvds_hstx.h"

/* `ncc` draws five bands - the header, the drawing, the machine's strip, then
   the text with the 3x3 beside it - and it draws them in **both shapes**: the
   panel read turned (600x800) and the panel read upright (800x600). The
   renderer owns the turn (`lvds_hstx.h`), so what this file branches on is the
   *picture's* shape and nothing else: the screens above place things with these
   numbers and never ask how the glass is mounted.

   Only the geometry is per-shape. The drawing, the pad, the text pane and the
   header are one set of functions for both, because nc's own lesson was that a
   second layout is a second thing to keep right - the shape belongs here, the
   rest does not. */

#define NCC_ROW_H 24

/* The header line: the four screens, the file and what the panel has to say. */
#define NCC_HEADER_H 28

/* The text pane's own pads. */
#define NCC_LINE_NO_PAD 4
#define NCC_LINE_TEXT_PAD 32

/* The fault block: white letters on red, and the room it may take. nc's own
   numbers (`NC_FAULT_LINES` / `NC_FAULT_ROW_H`): a fault is a panel, not a line
   of text (`nc_visual_draw_fault()`). */
#define NCC_FAULT_LINES 3
#define NCC_FAULT_ROW_H 16

/* The rows the text pane holds, and the files the list shows. */
#define NCC_FILES_MAX 32
#define NCC_NAME_MAX 40

/* Which shape this picture is, asked of the panel's **turn** rather than its
   view: `lvds_hstx.h` always knows the turn (it is either the build's own
   `-DLVDS_PANEL_TURN` or the header's default), while `LVDS_VIEW_WIDTH` is only
   known once the board's `LVDS_WIDTH`/`LVDS_HEIGHT` are defined - and a module
   file that includes this header *before* `cnc.h` made the old test see zero
   for both, quietly choosing the landscape numbers for a portrait picture.
   That is what kept the 3x3 keys at 64 px on the glass through two flashes
   while the station - which puts the sizes on its own command line - showed the
   86 that had just been asked for (bench, 2026-09-27: "still old size of
   buttons in 3x3"). A quarter turn means the panel is read turned: portrait. */
#if LVDS_PANEL_TURN == 90 || LVDS_PANEL_TURN == 270

/* --- portrait: 600x800, the panel read turned ------------------------------ */

/* The drawing, under the header and over the whole width. */
#define NCC_PREVIEW_X 6
#define NCC_PREVIEW_Y (NCC_HEADER_H + 4)
#define NCC_PREVIEW_W (LVDS_VIEW_WIDTH - 12)

/* The strip in the middle, and a little above it: the split was asked for at
   the middle first, then for it to come up once the drawing showed it had room
   to spare. */
#define NCC_SPLIT_RISE 25
#define NCC_DRO_H 28
#define NCC_DRO_Y (LVDS_VIEW_HEIGHT / 2 - NCC_DRO_H / 2 - NCC_SPLIT_RISE)
#define NCC_DRO_X 0
#define NCC_DRO_W LVDS_VIEW_WIDTH

/* What is left of the top half is the drawing. */
#define NCC_PREVIEW_BOTTOM (NCC_DRO_Y - 4)
/* Not `NCC_PREVIEW_H`: that is the drawing's own header guard, and a macro by
   that name hides the whole of `ncc_preview.h` from whatever includes this
   file first. */
#define NCC_PREVIEW_PANE_H (NCC_PREVIEW_BOTTOM - NCC_PREVIEW_Y)

/* The bottom band: the text pane on the left, the 3x3 in the corner. */
#define NCC_BODY_Y (NCC_DRO_Y + NCC_DRO_H + 6)
#define NCC_BODY_BOTTOM (LVDS_VIEW_HEIGHT - 8)

#define NCC_TEXT_X 8
#define NCC_TEXT_Y NCC_BODY_Y
/* The width the pad leaves, not a number of its own: the pane's edge is the
   pad's, so a key that grows takes its room from here and nowhere else. */
#define NCC_TEXT_W (NCC_PAD_X - 16)
#define NCC_TEXT_H (NCC_BODY_BOTTOM - NCC_BODY_Y)
#define NCC_TEXT_BOTTOM (NCC_TEXT_Y + NCC_TEXT_H)

/* The pad, pinned in the bottom right corner - the corner the operator's hand
   is in, and out of the code's way. Its caption is the line above it. The keys
   are square and the box is three of them, which is what nc's own pad was
   (72x72): the bench asked for the squares back, with the corner cut. */
/* The bench walked this on the glass and landed here (2026-09-27: nc's 72, then
   79, then 86, and finally "now 70 wide" - a shade under nc's own). At 70 a line
   of the panel's font holds eight characters, as 72 did; what the smaller key
   buys is a wider text pane (366 px against 318 at 86), which is the thing the
   operator reads the program in. `PROFILE START` is two lines at either size. */
#define NCC_PAD_KEY 70
#define NCC_PAD_W (NCC_PAD_KEY * 3)
#define NCC_PAD_H (NCC_PAD_KEY * 3)
#define NCC_PAD_X (LVDS_VIEW_WIDTH - NCC_PAD_W - 8)
#define NCC_PAD_Y (NCC_BODY_BOTTOM - NCC_PAD_H)

/* The notes in the space above the 3x3, between the strip and the keys: what
   the machine has to say and, under it, what the screen's keys do. */
#define NCC_NOTES_X NCC_PAD_X
#define NCC_NOTES_Y NCC_BODY_Y
#define NCC_NOTES_W NCC_PAD_W
#define NCC_NOTES_BOTTOM (NCC_PAD_Y - 24)
#define NCC_NOTES_H (NCC_NOTES_BOTTOM - NCC_NOTES_Y)

#else

/* --- landscape: 800x600, the panel read upright ---------------------------- */

/* The drawing keeps the whole width, and it is wider than tall here: the pane
   under the header is what is left once the strip has taken its line. */
#define NCC_PREVIEW_X 6
#define NCC_PREVIEW_Y (NCC_HEADER_H + 4)
#define NCC_PREVIEW_W (LVDS_VIEW_WIDTH - 12)

/* The strip is the screen's centre line, with no rise: in this shape the
   drawing has no room to spare, so the split stays where the middle is. */
#define NCC_SPLIT_RISE 0
#define NCC_DRO_H 28
#define NCC_DRO_Y (LVDS_VIEW_HEIGHT / 2 - NCC_DRO_H / 2 - NCC_SPLIT_RISE)
#define NCC_DRO_X 0
#define NCC_DRO_W LVDS_VIEW_WIDTH

#define NCC_PREVIEW_BOTTOM (NCC_DRO_Y - 4)
#define NCC_PREVIEW_PANE_H (NCC_PREVIEW_BOTTOM - NCC_PREVIEW_Y)

/* The bottom band, the same two things side by side - but here the text pane
   takes the width the portrait shape had to share, because the pad is beside
   it and not under the drawing. */
#define NCC_BODY_Y (NCC_DRO_Y + NCC_DRO_H + 6)
#define NCC_BODY_BOTTOM (LVDS_VIEW_HEIGHT - 8)

/* A smaller square here: the portrait key does not fit under the strip without
   eating the notes, and the machine's own buttons are what the eye compares.
   This shape is not grown with it: 20% more here would leave the fault block
   nowhere to be, and the fault is the one thing that has to fit. */
#define NCC_PAD_KEY 64
#define NCC_PAD_W (NCC_PAD_KEY * 3)
#define NCC_PAD_H (NCC_PAD_KEY * 3)
#define NCC_PAD_X (LVDS_VIEW_WIDTH - NCC_PAD_W - 8)
#define NCC_PAD_Y (NCC_BODY_BOTTOM - NCC_PAD_H)

#define NCC_TEXT_X 8
#define NCC_TEXT_Y NCC_BODY_Y
#define NCC_TEXT_W (NCC_PAD_X - 16)
#define NCC_TEXT_H (NCC_BODY_BOTTOM - NCC_BODY_Y)
#define NCC_TEXT_BOTTOM (NCC_TEXT_Y + NCC_TEXT_H)

/* The notes keep the space above the 3x3, which is taller here: the keys are
   shorter than the portrait pad, so the machine's line and the helpers fit. */
#define NCC_NOTES_X NCC_PAD_X
#define NCC_NOTES_Y NCC_BODY_Y
#define NCC_NOTES_W NCC_PAD_W
#define NCC_NOTES_BOTTOM (NCC_PAD_Y - 24)
#define NCC_NOTES_H (NCC_NOTES_BOTTOM - NCC_NOTES_Y)

#endif

/* The rows the text pane holds, in whichever shape. */
#define NCC_CODE_ROWS (NCC_TEXT_H / NCC_ROW_H)

#endif
