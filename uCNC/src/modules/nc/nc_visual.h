#ifndef NC_VISUAL_H
#define NC_VISUAL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    NC_VISUAL_KEY_NONE = 0,
    NC_VISUAL_KEY_MODE,
    NC_VISUAL_KEY_DIGIT_0,
    NC_VISUAL_KEY_DIGIT_1,
    NC_VISUAL_KEY_DIGIT_2,
    NC_VISUAL_KEY_DIGIT_3,
    NC_VISUAL_KEY_DIGIT_4,
    NC_VISUAL_KEY_DIGIT_5,
    NC_VISUAL_KEY_DIGIT_6,
    NC_VISUAL_KEY_DIGIT_7,
    NC_VISUAL_KEY_DIGIT_8,
    NC_VISUAL_KEY_DIGIT_9,
    NC_VISUAL_KEY_BACKSPACE,
    NC_VISUAL_KEY_FINISH,
    NC_VISUAL_KEY_CANCEL,
    NC_VISUAL_KEY_PREV,
    NC_VISUAL_KEY_NEXT,
    NC_VISUAL_KEY_ACCEPT
} nc_visual_key_t;

void nc_visual_init(void);
void nc_visual_handle_key(nc_visual_key_t key);
bool nc_visual_dirty(void);
void nc_visual_draw(void);

#ifdef __cplusplus
}
#endif

#endif
