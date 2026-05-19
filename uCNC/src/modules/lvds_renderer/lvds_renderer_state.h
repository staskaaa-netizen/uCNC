#ifndef LVDS_RENDERER_STATE_H
#define LVDS_RENDERER_STATE_H

#include "../ui_snapshot/ui_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

void lvds_renderer_state_init(void);
void lvds_renderer_state_poll(void);
const ui_snapshot_frame_t *lvds_renderer_state_frame(void);

#ifdef __cplusplus
}
#endif

#endif
