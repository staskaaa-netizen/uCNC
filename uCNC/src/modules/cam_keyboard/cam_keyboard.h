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

/* The key an event byte is about: bit 7 of the byte is the release flag and the
   low seven bits are the key, so a press and its release decode to the same
   key. A release that decoded as "no key" is what used to leave the driver
   believing the key was still down. */
cam_key_t cam_keyboard_decode_key(const uint8_t raw[6]);
/* The character a key sends ('0'-'9', '*', '#', 'A'-'D'), 0 for CAM_KEY_NONE. */
char cam_keyboard_key_to_char(cam_key_t key);
const uint8_t *cam_keyboard_raw(void);
/* The key that is down right now, CAM_KEY_NONE once it is released. Not "the
   last key seen": a held key stays reported until its release event arrives,
   and a caller that wants the edge uses cam_keyboard_changed(). */
cam_key_t cam_keyboard_key(void);
/* The same key as the character the keypad sends ('0'-'9', '*', '#', 'A'-'D'),
   0 when nothing is held. */
char cam_keyboard_key_char(void);
/* The key that went down in this update, 0 when it carried no press. Separate
   from cam_keyboard_key_char() on purpose: a tap whose press and release are
   both in the same read is still a press (a delayed poll must not lose it),
   while the key that is down is already clear by then. */
char cam_keyboard_pressed_char(void);
/* True when this update carried a new press (a release does not set it). */
bool cam_keyboard_changed(void);

#ifdef __cplusplus
}
#endif

#endif
