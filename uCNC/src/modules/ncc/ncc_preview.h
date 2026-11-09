#ifndef NCC_PREVIEW_H
#define NCC_PREVIEW_H

#include "ncc.h"
#include "ncc_tools.h"

/* The drawing: the stock, the profile the program cuts, the dimensions in the
   DIN style nc draws them, and the part as the cycles rough it out.

   It draws what the *sender* emits (`../nc_sender/nc_sender.c`), not a second reading of the
   document, so what is on the glass is what the machine will do - and the
   dimension callouts follow the same point rule, so a profile written with
   Fanuc's `U`/`W` increments reads exactly as the same profile written out.

   `nc`'s four layer switches are one key here: `#` turns the DIN layer - the
   rulers and the dimension callouts - on and off, so the drawing is either nc's
   full picture or the stock and the path alone. RUN does not ask: it shows the
   stock while the machine cuts and the path when it is parked. */

/* What the machine is doing, for the live stock: while the tool moves, the
   drawing shows the material it has taken off - the mask of what is still
   there, drawn in the stock's own colour over the pane's background. `x`/`z`
   are the machine's **axis** position, before the work offset and before the
   radius-to-diameter conversion: the drawing owns both (the active `G54...` and
   `G92`, and `G7`/`G8` for the document), so a screen only hands over what the
   runtime says. A run that has parked leaves its mask on the glass (`nc` kept
   it the same way) so the finished part stays until something else is drawn. */
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
    /* The tool the program is using: its `R` is the live brush's radius. The
       caller owns it (it is the loaded table's); a run without one draws no
       cut (the static stock stays). */
    const ncc_tool_t *tool;
} ncc_preview_run_t;

void ncc_preview_draw(const ncc_document_t *doc, const ncc_preview_run_t *run,
                      int x, int y, int w, int h);

/* The `#` selection, owned here because the drawing is what it changes: the
   DIN layer - the rulers and the dimension callouts - on, or the stock and the
   path alone. On is nc's full picture and is what the panel starts with; RUN
   does not ask, it shows the stock while the machine cuts and the path when it
   is parked, wearing whatever DIN state the editor left behind. */
bool ncc_preview_din(void);
bool ncc_preview_toggle_din(void);

/* What the last drawing spent its time in, in microseconds: the stock and the
   mask's scan, then the geometry (dimensions, contour, the emitted part, the
   tool). The screen's frame meter logs them, which is what says where a slow
   frame goes (bench: "12 fps only. why ..."). */
/* The stock's diameter, as the drawing reads it out of the setup rows. The path
   builder starts an axis the program has not given yet at the stock's corner,
   which is where a lathe profile starts (the same answer the drawing uses). */
float ncc_preview_stock_x(const ncc_document_t *doc);

#endif
