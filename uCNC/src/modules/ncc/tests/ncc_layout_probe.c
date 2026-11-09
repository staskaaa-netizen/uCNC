/* Which branch does `ncc_layout.h` take, in the include order the module's own
   files use - that is, before anything has defined the panel's size?

   Nothing is run and nothing is linked: `tools/test_ncc.py` puts this through
   the preprocessor (`-E -dM`) and insists the answer is the machine's own shape.
   It is the check for the bug that kept the 3x3 keys at 64 px on the glass while
   every host frame showed 86 - `LVDS_VIEW_WIDTH` was not defined yet, both sides
   of the old test read zero, and the landscape numbers quietly won. A build's
   include order is not visible in any frame; it is visible here. */
#include "ncc_layout.h"

int ncc_layout_probe_pad_key = NCC_PAD_KEY;
int ncc_layout_probe_split = NCC_SPLIT_RISE;
