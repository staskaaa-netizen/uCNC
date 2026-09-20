/* The preview's data half: what there is to draw.

   This is the first piece of the preview to move out of the screen - the same
   unit the preview renderer will join when the drawing half follows (`nc_sim.c`
   was the old name; it never simulated anything, it collected the stock, the
   contour extent and the cycle it had to draw). Sources inside the NC module,
   never a module of its own: a preview with no screen to draw in is not a
   thing that can exist. */
#ifndef NC_PREVIEW_H
#define NC_PREVIEW_H

#include "nc.h"
#include "../g7x/g7x_contour.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float stock_x;
    float stock_z;
    float stock_i;
    float stock_e;
    float chuck_c;
    float stock_visible_z;
    float min_x;
    float max_x;
    float min_z;
    float max_z;
    int path_segments;
    int rapid_segments;
    int arc_segments;
    int cycles;
    g7x_cycle_t last_cycle;
} nc_preview_info_t;

bool nc_preview_line_word_float(const char *line, char letter, float *out);
void nc_preview_collect(const nc_document_t *doc, nc_preview_info_t *preview);

#ifdef __cplusplus
}
#endif

#endif
