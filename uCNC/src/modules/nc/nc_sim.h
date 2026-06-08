#ifndef NC_SIM_H
#define NC_SIM_H

#include "nc.h"
#include "../g71_g72/g71_g72.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float stock_x;
    float stock_z;
    float stock_i;
    float stock_e;
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

bool nc_sim_line_word_float(const char *line, char letter, float *out);
void nc_sim_collect_preview(const nc_document_t *doc, nc_preview_info_t *preview);

#ifdef __cplusplus
}
#endif

#endif
