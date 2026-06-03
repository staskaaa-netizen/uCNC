#ifndef LEANCAM_BRIDGE_H
#define LEANCAM_BRIDGE_H

#include <stdbool.h>
#include "../ui_keys.h"
#include "leancam_snapshot_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

void leancam_bridge_init(void);
void leancam_bridge_tick(void);
void leancam_bridge_handle_key(ui_key_t key);
void leancam_bridge_preview_ack(uint16_t seq);

/* Non-zero when LeanCam program/draft view owns the keypad menu. */
int leancam_bridge_wants_key_menu(void);

/* Core1 snapshot exporter: copies LeanCam view state into a frame. */
void leancam_bridge_fill_snapshot(ui_snapshot_frame_t *f);

#ifdef __cplusplus
}
#endif

#endif

