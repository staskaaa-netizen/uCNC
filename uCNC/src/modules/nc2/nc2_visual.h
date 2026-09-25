#ifndef NC2_VISUAL_H
#define NC2_VISUAL_H

#include <stdbool.h>

/* nc2's screen: the program, and the 3x3 pad pinned in the bottom right corner.

   There is no footer strip and no second menu: the pad's nine slots are the
   files at the address the operator has walked to, and the pad writes its name
   into the program as the line the entry will land on. The DRO is not here yet -
   it belongs to the run, and it appears when the machine is doing something. */

void nc2_visual_init(void);

/* Open a program from the card and remember it as the file in play. */
bool nc2_visual_open(const char *path);
/* Write the program back. The caller does this when the screen has been left
   alone, the way the panel writes anything else. */
bool nc2_visual_save(void);
/* The file in play, "" when there is none. */
const char *nc2_visual_path(void);
/* What the screen is showing, for a shell that names it: `EDIT`, and `FILES`
   while the card's list is up. */
const char *nc2_visual_screen_name(void);

/* One machine key, as the keypad sends it: `0`-`9`, `A`-`D`, `#`, `*`. */
void nc2_visual_key(char key);

/* The boot logo's own clock and the screen's periodic work, from the main
   loop. */
void nc2_visual_tick(unsigned ms);

bool nc2_visual_dirty(void);
void nc2_visual_clear_dirty(void);
const char *nc2_visual_status(void);

/* Draw the whole panel. */
void nc2_visual_draw(void);

/* What the pad is showing, for a shell that labels the machine's keypad: the
   address walked to and the label of one of its nine slots. */
const char *nc2_visual_address(void);
const char *nc2_visual_slot_label(char key);

#endif
