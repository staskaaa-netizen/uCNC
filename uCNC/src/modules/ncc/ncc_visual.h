#ifndef NCC_VISUAL_H
#define NCC_VISUAL_H

#include "ncc_state.h"

#include <stdbool.h>
#include <stddef.h>

/* ncc's screen: the program, and the 3x3 pad pinned in the bottom right corner.

   There is no footer strip and no second menu: the pad's nine slots are the
   files at the address the operator has walked to, and the pad writes its name
   into the program as the line the entry will land on. The DRO is not here yet -
   it belongs to the run, and it appears when the machine is doing something. */

void ncc_visual_init(void);

/* Jump straight to a screen (the desktop shell's F keys, and a future mode
   key). The program and the run are built; MANUAL and TOOLS are refused until
   their own modules land. */
void ncc_visual_select_mode(ncc_mode_t mode);
ncc_mode_t ncc_visual_mode(void);

/* Open a program from the card and remember it as the file in play. */
bool ncc_visual_open(const char *path);
/* Write the program back. The caller does this when the screen has been left
   alone, the way the panel writes anything else. */
bool ncc_visual_save(void);
/* The file in play, "" when there is none. */
const char *ncc_visual_path(void);
/* The line the editor's cursor is on, for a shell that reads the screen back. */
size_t ncc_visual_cursor(void);
/* What the screen is showing, for a shell that names it: `EDIT`, and `FILES`
   while the card's list is up. */
const char *ncc_visual_screen_name(void);

/* One machine key, as the keypad sends it: `0`-`9`, `A`-`D`, `#`, `*`. */
void ncc_visual_key(char key);
/* The keypad key that is down right now, or 0 when nothing is held. A held
   direction key in MANUAL feed mode feeds until it comes up, so a shell that
   can hold a key reports both edges; one that cannot reports 0. */
void ncc_visual_hold_key(char key);
/* Main-loop hook: write the program back once the screen has been left alone,
   and flush what the panel remembers. */
void ncc_visual_idle_tasks(void);

/* The screen's periodic work from the main loop: the card's one preset file is
   picked up here, the first loop the drive answers on. */
void ncc_visual_tick(unsigned ms);

/* One pass of the screen's main loop, with the machine's own clock: the preset
   file's pickup, the program written back once the operator has stopped typing,
   and the answer "draw this pass" - dirty, or the period a screen that is
   moving asks for. Both the firmware module and the station call this, so the
   loop that decides what the screen shows is the screen's own and not a copy of
   it beside the module. */
bool ncc_visual_pump(uint32_t now_ms);

bool ncc_visual_dirty(void);
void ncc_visual_clear_dirty(void);
const char *ncc_visual_status(void);
/* The screen's own status line, for a screen that lives beside it (MANUAL). */
void ncc_visual_status_set(const char *text);
/* Ask for the next frame: a screen that changed something on its own. */
void ncc_visual_mark_dirty(void);

/* Draw the whole panel. */
void ncc_visual_draw(void);

/* What the pad is showing, for a shell that labels the machine's keypad: the
   address walked to and the label of one of its nine slots. */
const char *ncc_visual_address(void);
const char *ncc_visual_slot_label(char key);

/* The lines `ncc_visual_usage()` can hand out at most. */
#define NCC_VISUAL_USAGE_MAX 8

/* The screen's own status line: the sentence a key press or a refusal leaves
   behind. It is the longest message the panel can ever have, so it is the
   length the notes' fault block has to be able to show (one place says it). */
#define NCC_STATUS_MAX 64

/* What the active screen is and how its keys drive it, in the screen's own
   words - what a shell draws as the usage notes beside the machine. */
size_t ncc_visual_usage(const char *const **lines);

/* The frame meter's last reading: the frames the screen drew in the last whole
   second. It is a debug reading - the bench asked for it back to test what a
   change costs the panel - and `--fps2test` is the check that counts them. */
uint16_t ncc_visual_fps(void);

/* What one pad key means on the active screen, for a shell that draws the
   keypad itself. */
typedef struct {
    const char *label;
    bool on_menu;
    bool step;
} ncc_visual_key_meaning_t;

bool ncc_visual_key_meaning(char key, ncc_visual_key_meaning_t *meaning);

/* True while the screen has something of its own to redraw (a run in flight, a
   held feed), so a shell keeps the frame coming. */
bool ncc_visual_periodic_needed(void);

#endif
