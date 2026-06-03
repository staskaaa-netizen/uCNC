#ifndef LEANCAM_VISUAL_H
#define LEANCAM_VISUAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void leancam_visual_init(void);
void leancam_visual_prepare(void);
void leancam_visual_draw(void);
uint32_t leancam_visual_reentry_count(void);
uint32_t leancam_visual_max_draw_us(void);


#ifdef __cplusplus
}
#endif

#endif

