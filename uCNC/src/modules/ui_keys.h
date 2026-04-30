#ifndef UI_KEYS_H
#define UI_KEYS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    UI_KEY_NONE = 0,

    UI_KEY_DIGIT_0,
    UI_KEY_DIGIT_1,
    UI_KEY_DIGIT_2,
    UI_KEY_DIGIT_3,
    UI_KEY_DIGIT_4,
    UI_KEY_DIGIT_5,
    UI_KEY_DIGIT_6,
    UI_KEY_DIGIT_7,
    UI_KEY_DIGIT_8,
    UI_KEY_DIGIT_9,

    UI_KEY_BACKSPACE,   /* * */
    UI_KEY_FINISH,      /* # */

    UI_KEY_CANCEL,      /* A */
    UI_KEY_PREV,        /* B */
    UI_KEY_NEXT,        /* C */
    UI_KEY_ACCEPT       /* D */

} ui_key_t;

#ifdef __cplusplus
}
#endif

#endif
