/* Host shim for the LeanCam tool catalog.

   The tool catalog lives in the firmware file/tool world. The LeanCam module's
   own host test stubs the same lookup and the generator then falls back to
   scanning the program text, so this keeps the desktop tool on the same host
   contract instead of compiling firmware code that expects the board headers.
   See uCNC/src/modules/leanCam/tests/leancam_gcode_host_test.c. */

#include "leancam_gcode.h"
#include "leancam_program.h"

#include <stddef.h>

const char *lc_tool_catalog_find_in_program_or_catalog(const program_t *prog,
                                                       int before_or_at,
                                                       int t)
{
    (void)prog;
    (void)before_or_at;
    (void)t;
    return NULL;
}
