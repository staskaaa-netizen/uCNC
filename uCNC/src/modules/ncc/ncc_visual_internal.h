#ifndef NCC_VISUAL_INTERNAL_H
#define NCC_VISUAL_INTERNAL_H

#include "ncc.h"

#include <stdbool.h>
#include <stddef.h>

/* Read-only answers the screen's view needs from its controller.

   `ncc_visual.c` owns input, the document, screen selection and the status
   line. `ncc_view.c` owns framebuffer layout and frame invalidation. These are
   the only facts crossing that boundary; neither side reaches into the other's
   state. */

const ncc_document_t *ncc_visual_document(void);
bool ncc_visual_list(void);
bool ncc_visual_name_selected(void);
const char *ncc_visual_refused(void);
const char *ncc_visual_label(char key);
uint32_t ncc_visual_raw_inputs(void);

/* The active pad's own answers, shared by the controller's key meaning and the
   view's key faces. */
const char *ncc_pad_label(char key);
void ncc_pad_name(char *out, size_t out_sz);
size_t ncc_visual_run_line(void);

#endif
