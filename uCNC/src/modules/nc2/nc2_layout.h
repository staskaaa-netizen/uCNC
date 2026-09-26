#ifndef NC2_LAYOUT_H
#define NC2_LAYOUT_H

#include "../lvds_renderer/lvds_hstx.h"

/* `nc2` draws a **600x800 picture**, top to bottom: the header, the drawing,
   the machine's own strip, and then the bottom band - the text on the left
   with the 3x3 pinned in the corner beside it. That is the whole layout, and
   the middle strip is the only line the screen needs: the top is what the
   machine is making, the bottom is what the operator types. The glass is
   800x600 and mounted turned, so these numbers are the picture's, not the
   panel's (`lvds_hstx.h` owns the turn, and `LVDS_VIEW_*` is the picture's own
   size). */
#define NC2_ROW_H 24

/* The header line: the four screens, the file and what the panel has to say. */
#define NC2_HEADER_H 28

/* The drawing, under the header and over the whole width: the stock, the
   contour and the dimensions, as nc drew them. */
#define NC2_PREVIEW_X 6
#define NC2_PREVIEW_Y (NC2_HEADER_H + 4)
#define NC2_PREVIEW_W (LVDS_VIEW_WIDTH - 12)

/* The strip in the middle, and a little above it: the bench first asked for the
   centre line (*"make it at the middle as asked"*) and then, with the drawing
   sitting in the top half with room to spare, for the split to come up - *"move
   tis all ~ 25 px to the top, its have plenty of space"*. What the top half
   gives up is a row and a half of the bottom band's height. */
#define NC2_SPLIT_RISE 25
#define NC2_DRO_H 28
#define NC2_DRO_Y (LVDS_VIEW_HEIGHT / 2 - NC2_DRO_H / 2 - NC2_SPLIT_RISE)
#define NC2_DRO_X 0
#define NC2_DRO_W LVDS_VIEW_WIDTH

/* What is left of the top half is the drawing. */
#define NC2_PREVIEW_BOTTOM (NC2_DRO_Y - 4)
/* Not `NC2_PREVIEW_H`: that is the drawing's own header guard, and a macro by
   that name hides the whole of `nc2_preview.h` from whatever includes this
   file first. */
#define NC2_PREVIEW_PANE_H (NC2_PREVIEW_BOTTOM - NC2_PREVIEW_Y)

/* The bottom band: the text pane on the left, the 3x3 in the corner. */
#define NC2_BODY_Y (NC2_DRO_Y + NC2_DRO_H + 6)
#define NC2_BODY_BOTTOM (LVDS_VIEW_HEIGHT - 8)

#define NC2_TEXT_X 8
#define NC2_TEXT_Y NC2_BODY_Y
#define NC2_TEXT_W 342
#define NC2_TEXT_H (NC2_BODY_BOTTOM - NC2_BODY_Y)
#define NC2_TEXT_BOTTOM (NC2_TEXT_Y + NC2_TEXT_H)
#define NC2_LINE_NO_PAD 4
#define NC2_LINE_TEXT_PAD 32

/* The pad, pinned in the bottom right corner - the corner the operator's hand
   is in, and out of the code's way. Its caption is the line above it. */
#define NC2_PAD_W 234
#define NC2_PAD_H 216
#define NC2_PAD_X (LVDS_VIEW_WIDTH - NC2_PAD_W - 8)
#define NC2_PAD_Y (NC2_BODY_BOTTOM - NC2_PAD_H)

/* The notes in the space above the 3x3, between the strip and the keys: what
   the machine has to say (an error, the panel's own message) and, under it,
   what the screen's keys do - the helpers the station used to keep in its side
   strip. The pad's caption is the line directly above the keys, so the notes
   stop short of it. */
#define NC2_NOTES_X NC2_PAD_X
#define NC2_NOTES_Y NC2_BODY_Y
#define NC2_NOTES_W NC2_PAD_W
#define NC2_NOTES_BOTTOM (NC2_PAD_Y - 24)
#define NC2_NOTES_H (NC2_NOTES_BOTTOM - NC2_NOTES_Y)

/* The rows the text pane holds, and the files the list shows. */
#define NC2_CODE_ROWS (NC2_TEXT_H / NC2_ROW_H)
#define NC2_FILES_MAX 32
#define NC2_NAME_MAX 40

#endif
