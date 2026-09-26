#ifndef NC2_PREVIEW_H
#define NC2_PREVIEW_H

#include "nc2.h"
#include "nc2_tools.h"

/* The drawing: the stock, the profile the program cuts, the dimensions in the
   DIN style nc draws them, and the part as the cycles rough it out.

   It draws what the *sender* emits (`nc2_emit.c`), not a second reading of the
   document, so what is on the glass is what the machine will do - and the
   dimension callouts follow the same point rule, so a profile written with
   Fanuc's `U`/`W` increments reads exactly as the same profile written out.

   The layer switches `nc` had (`STOCK`/`TRACE`/`ROUGH`/`DIM` on its footer) have
   no keys here: the footer is gone, so the drawing shows what `nc`'s defaults
   showed - all of it - and the switches wait for a home if they are wanted. */

/* What the machine is doing, for the live stock: while the tool moves, the
   drawing shows the material it has taken off - the mask of what is still
   there, drawn in the stock's own colour over the pane's background. `x` is the
   machine's X in the axis frame (a **radius**, as the DRO reads it) and `z` the
   same as the program's, so nothing here has to know the diameter mode. A run
   that has parked leaves its mask on the glass (`nc` kept it the same way) so
   the finished part stays until something else is drawn. */
typedef struct {
    bool busy;                      /* it is doing anything at all */
    bool screen_run;                /* the RUN screen is the one being drawn */
    /* The pane is this screen's to paint this frame. The TOOLS screen's rows
       own the pane and paint over it, but the live stock still has to follow
       the machine there - a frozen mask catches up in one straight sweep across
       the material the tool really walked. */
    bool paint;
    /* The screen painted the whole pane this frame (a different band, a mode
       change): the mask has to be drawn whole again, not only where it moved. */
    bool full;
    float x;
    float z;
    /* The tool the program is using, for the glyph that rides the cut: the
       caller owns it (it is the loaded table's), and a run without one is drawn
       without a tool. */
    const nc2_tool_t *tool;
} nc2_preview_run_t;

void nc2_preview_draw(const nc2_document_t *doc, const nc2_preview_run_t *run,
                      int x, int y, int w, int h);

/* What the last drawing spent its time in, in microseconds: the stock and the
   mask's scan, then the geometry (dimensions, contour, the emitted part, the
   tool). The screen's frame meter logs them, which is what says where a slow
   frame goes (bench: "12 fps only. why ..."). */
void nc2_preview_times(uint32_t *stock_us, uint32_t *geom_us);

/* The stock's diameter, as the drawing reads it out of the setup rows. The path
   builder starts an axis the program has not given yet at the stock's corner,
   which is where a lathe profile starts (the same answer the drawing uses). */
float nc2_preview_stock_x(const nc2_document_t *doc);

#endif
