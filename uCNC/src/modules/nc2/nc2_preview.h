#ifndef NC2_PREVIEW_H
#define NC2_PREVIEW_H

#include "nc2.h"

/* The drawing: the stock, the profile the program cuts, the dimensions in the
   DIN style nc draws them, and the part as the cycles rough it out.

   It draws what the *sender* emits (`nc2_emit.c`), not a second reading of the
   document, so what is on the glass is what the machine will do - and the
   dimension callouts follow the same point rule, so a profile written with
   Fanuc's `U`/`W` increments reads exactly as the same profile written out.

   The layer switches `nc` had (`STOCK`/`TRACE`/`ROUGH`/`DIM` on its footer) have
   no keys here: the footer is gone, so the drawing shows what `nc`'s defaults
   showed - all of it - and the switches wait for a home if they are wanted. */

void nc2_preview_draw(const nc2_document_t *doc, int x, int y, int w, int h);

/* The stock's diameter, as the drawing reads it out of the setup rows. The path
   builder starts an axis the program has not given yet at the stock's corner,
   which is where a lathe profile starts (the same answer the drawing uses). */
float nc2_preview_stock_x(const nc2_document_t *doc);

#endif
