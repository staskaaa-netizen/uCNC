#ifndef LEANCAM_SIM_PREVIEW_H
#define LEANCAM_SIM_PREVIEW_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../ui_snapshot/ui_snapshot.h"

typedef struct
{
    int x0;
    int y0;
    int x1;
    int y1;
    int stock_left;
    int stock_top;
    int stock_right;
    int stock_bottom;
    int z_zero_x;
    float z_scale;
    float d_scale;
    float stock_len;
    float stock_od;
} lcam_sim_view_t;

int sim_z_to_px(const lcam_sim_view_t *view, float z);
int sim_d_to_py(const lcam_sim_view_t *view, float d);

void leancam_sim_preview_draw(const ui_snapshot_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* LEANCAM_SIM_PREVIEW_H */
