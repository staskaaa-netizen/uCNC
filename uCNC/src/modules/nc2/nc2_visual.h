#ifndef NC2_VISUAL_H
#define NC2_VISUAL_H

#include "nc2_state.h"

#include <stdbool.h>
#include <stddef.h>

/* nc2's screen: the program, and the 3x3 pad pinned in the bottom right corner.

   There is no footer strip and no second menu: the pad's nine slots are the
   files at the address the operator has walked to, and the pad writes its name
   into the program as the line the entry will land on. The DRO is not here yet -
   it belongs to the run, and it appears when the machine is doing something. */

void nc2_visual_init(void);

/* Jump straight to a screen (the desktop shell's F keys, and a future mode
   key). The program and the run are built; MANUAL and TOOLS are refused until
   their own modules land. */
void nc2_visual_select_mode(nc2_mode_t mode);
nc2_mode_t nc2_visual_mode(void);

/* Open a program from the card and remember it as the file in play. */
bool nc2_visual_open(const char *path);
/* Write the program back. The caller does this when the screen has been left
   alone, the way the panel writes anything else. */
bool nc2_visual_save(void);
/* The file in play, "" when there is none. */
const char *nc2_visual_path(void);
/* The line the editor's cursor is on, for a shell that reads the screen back. */
size_t nc2_visual_cursor(void);
/* What the screen is showing, for a shell that names it: `EDIT`, and `FILES`
   while the card's list is up. */
const char *nc2_visual_screen_name(void);

/* One machine key, as the keypad sends it: `0`-`9`, `A`-`D`, `#`, `*`. */
void nc2_visual_key(char key);
/* The keypad key that is down right now, or 0 when nothing is held. A held
   direction key in MANUAL feed mode feeds until it comes up, so a shell that
   can hold a key reports both edges; one that cannot reports 0. */
void nc2_visual_hold_key(char key);
/* Main-loop hook: write the program back once the screen has been left alone,
   and flush what the panel remembers. */
void nc2_visual_idle_tasks(void);

/* The boot logo's own clock and the screen's periodic work, from the main
   loop. */
void nc2_visual_tick(unsigned ms);

/* One pass of the screen's main loop, with the machine's own clock: the logo's
   countdown, the program written back once the operator has stopped typing, and
   the answer "draw this pass" - dirty, or the period a screen that is moving
   asks for. Both the firmware module and the station call this, so the loop
   that decides what the screen shows is the screen's own and not a copy of it
   beside the module (a copy that once left the first start's logo up for
   ever). */
bool nc2_visual_pump(uint32_t now_ms);

bool nc2_visual_dirty(void);
void nc2_visual_clear_dirty(void);
const char *nc2_visual_status(void);
/* The screen's own status line, for a screen that lives beside it (MANUAL). */
void nc2_visual_status_set(const char *text);
/* Ask for the next frame: a screen that changed something on its own. */
void nc2_visual_mark_dirty(void);

/* Draw the whole panel. */
void nc2_visual_draw(void);

/* What the pad is showing, for a shell that labels the machine's keypad: the
   address walked to and the label of one of its nine slots. */
const char *nc2_visual_address(void);
const char *nc2_visual_slot_label(char key);

/* The lines `nc2_visual_usage()` can hand out at most. */
#define NC2_VISUAL_USAGE_MAX 8

/* What the active screen is and how its keys drive it, in the screen's own
   words - what a shell draws as the usage notes beside the machine. */
size_t nc2_visual_usage(const char *const **lines);

/* What one pad key means on the active screen, for a shell that draws the
   keypad itself. */
typedef struct {
    const char *label;
    bool on_menu;
    bool step;
} nc2_visual_key_meaning_t;

bool nc2_visual_key_meaning(char key, nc2_visual_key_meaning_t *meaning);

/* True while the screen has something of its own to redraw (a run in flight,
   the boot logo), so a shell keeps the frame coming. */
bool nc2_visual_periodic_needed(void);

#endif
