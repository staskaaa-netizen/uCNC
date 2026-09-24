#ifndef NC_UI_HOST_SHELL_H
#define NC_UI_HOST_SHELL_H

#include <stdbool.h>
#include <stddef.h>

/* The emulated machine the station drives, in one place: the panel's own
   geometry, the keypad the machine has in hardware, the virtual card the files
   live on, and the main loop - parse, `cnc_dotasks()`, advance the clock - that
   makes a jog, a feed and a RUN actually execute.

   The window (`main.c`) draws and clicks through it, and the headless checks
   (`host_tests.c`) run it without a window. Nothing here knows about a screen,
   a file or a check; a check that needs a different starting point states it
   through this API (mount a root, press keys, run ticks, take a dump). */

/* The window is the emulated panel - the firmware layout, 800x600, exactly as
   the LVDS panel draws it - with the operator's strip beside it. */
#define PANEL_W 800
#define PANEL_H 600
#define SIDE_W 300
#define WIN_W (PANEL_W + SIDE_W)
#define WIN_H PANEL_H

/* The window's own timer: one tick of the machine's main loop. */
#define TIMER_ID 1
#define TIMER_MS 30

/* The machine's keypad, as cam_keyboard.c decodes it: a 4x4 matrix with the
   first row on top (the characters the driver reports - `*0#D` / `123C` /
   `456B` / `789A`). The window draws and clicks these keys and the checks walk
   them, so the matrix is stated once, where the machine states it. */
#define PAD_COLS 4
#define PAD_ROWS 4
extern const char g_pad_keys[PAD_ROWS][PAD_COLS + 1];

/* The pad key's meaning on the active screen: the footer entry that carries it,
   the screen's own word for a key the footer does not name, and whether the key
   steps a field or the axis. False when the key means nothing here. */
#include "nc_visual.h"

bool host_key_meaning(char key, nc_visual_key_meaning_t *meaning);

/* Where `/D` is mounted: `--files DIR`, else `nc-files` beside the exe. The
   checks set it through `--files` before anything mounts. */
extern char g_files_root[260];

/* The `--keys` script and the `--ticks` count, replayed before a dump. */
extern char g_key_script[160];
extern unsigned g_ticks;

/* Fill `g_files_root` from the exe's own directory when `--files` did not name
   one, so a shortcut or a download mounts the same card wherever it is opened
   from. `argv0` is only a fallback for the rare `GetModuleFileNameA()` miss. */
void host_files_root_beside_exe(const char *argv0);

/* The demo the station ships with: the files in `examples` (the release zip
   carries `examples\` beside the exe) are copied into `<root>\nc\files` while
   that card has no program of its own - a station that was just unpacked opens
   with something to look at. A card that already holds a `.nc` is left alone,
   and nothing is overwritten. Returns how many files were placed. */
int host_seed_card(const char *root, const char *examples);

/* Replays the machine's own key path - the keypad's characters through
   nc_visual_key_for_char(), the mode keys, `HOLD`/`RELEASE` and `WAIT<n>` - so
   what a dump shows is what the panel shows after the same presses. */
void host_play_keys(const char *script);

/* Bring the firmware up the way the machine's own boot does, and mount `/D`. */
void host_init_core(void);

/* One tick of the firmware's main loop: parse what the panel queued, run
   `cnc_dotasks()` and advance the virtual clock by `elapsed_ms`. */
void host_run_machine(unsigned elapsed_ms);

/* The main loop with the clock ticking, `iterations` times. */
void host_pump(unsigned iterations);

/* Let everything the panel queued run out: the parser takes one line per
   iteration and the planner needs the clock to execute what it took. */
void host_pump_idle(unsigned max_iterations);

/* Write the emulated panel (800x600, the firmware layout alone) as a .bmp. */
int host_dump(const char *path);

/* Write the whole bench - the emulated panel and the operator's strip beside it
   - as a .bmp: the picture the window paints, so the key row and the usage
   lines are reviewable (and diffable) without opening the window. */
int host_dump_bench(const char *path);

#endif
