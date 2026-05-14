#ifndef LEANCAM_HDMI_RENDERER_H
#define LEANCAM_HDMI_RENDERER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void leancam_hdmi_renderer_init(void);
void leancam_hdmi_renderer_poll(void);
void leancam_hdmi_renderer_set_keyboard_connected(bool connected);

#ifdef __cplusplus
}
#endif

#endif
