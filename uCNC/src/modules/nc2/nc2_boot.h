#ifndef NC2_BOOT_H
#define NC2_BOOT_H

#include <stdbool.h>

/* The first start: the entries the panel ships, written onto a card that has
   never seen one - on its own, with a logo on the screen while it happens.

   This is the only place those entries exist, and it is a *writer*, never a
   fallback: after it has run, every entry is a file like any other, and deleting
   one is how an address stops being an entry. Nothing here overwrites a file, and
   nothing runs on a folder that already holds an entry of its own - so a card the
   operator has touched is theirs, and an operator who wants the shipped set back
   deletes the folder and starts again.

   Small on purpose: one table, one pass over it, one screen. */

/* Look at the card and write the shipped entries if it has none. True when
   anything was written, which is also what raises the logo. */
bool nc2_boot_seed(void);

/* The logo is up: the seed has run and its minimum time has not elapsed yet, so
   the screen a caller draws is this one instead of the work screen. */
bool nc2_boot_active(void);

/* Advance the logo's own clock. Called from the main loop with the elapsed
   milliseconds, the way every other periodic thing in the panel is. */
void nc2_boot_tick(unsigned ms);

/* Draw it over the whole panel. */
void nc2_boot_draw(void);

#endif
