#ifndef CAM_KEYBOARD_H
#define CAM_KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    CAM_KEY_NONE = 0,
    CAM_KEY_0,
    CAM_KEY_1,
    CAM_KEY_2,
    CAM_KEY_3,
    CAM_KEY_4,
    CAM_KEY_5,
    CAM_KEY_6,
    CAM_KEY_7,
    CAM_KEY_8,
    CAM_KEY_9,
    CAM_KEY_STAR,
    CAM_KEY_HASH,
    CAM_KEY_A,
    CAM_KEY_B,
    CAM_KEY_C,
    CAM_KEY_D
} cam_key_t;

void cam_keyboard_init(void);
void cam_keyboard_update(void);

const uint8_t *cam_keyboard_raw(void);
cam_key_t cam_keyboard_key(void);
char cam_keyboard_key_char(void);
bool cam_keyboard_changed(void);

#ifdef __cplusplus
}
#endif

#endif
