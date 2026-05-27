#ifndef LVDS_RENDERER_H
#define LVDS_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void lvds_renderer_draw_init(void);
void lvds_renderer_prepare_poll(void);
void lvds_renderer_draw_poll(void);
uint32_t lvds_renderer_reentry_count(void);
uint32_t lvds_renderer_max_draw_us(void);
void lvds_renderer_trace_next_frames(uint8_t count);


#ifdef __cplusplus
}
#endif

#endif
