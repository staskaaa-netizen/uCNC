#ifndef NC_UI_HOST_BOARDMAP_H
#define NC_UI_HOST_BOARDMAP_H

/* Board map for the host panel build.

   It is defined here rather than with -DBOARDMAP="..." because cmd.exe strips
   the quotes on the way to gcc, which leaves boarddefs.h with `#include` and no
   filename. The Makefile and the python runner pull this header in with
   -include. */
#define BOARDMAP "src/modules/g7x/tests/virtual_board.h"

#endif
