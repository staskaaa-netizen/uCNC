#ifndef LEANCAM_VISUAL_STATE_H
#define LEANCAM_VISUAL_STATE_H

#include "../leancam_snapshot_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

void leancam_visual_state_init(void);
void leancam_visual_state_poll(void);
const ui_snapshot_frame_t *leancam_visual_state_frame(void);

#ifdef __cplusplus
}
#endif

#endif

