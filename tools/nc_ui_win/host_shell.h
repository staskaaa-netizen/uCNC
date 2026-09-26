#ifndef NC_UI_HOST_SHELL_H
#define NC_UI_HOST_SHELL_H

#include <stdbool.h>
#include <stddef.h>

#include <windows.h>

#include "nc2_visual.h"     /* the screen's key meaning, for the pad's labels */

/* The emulated machine the station drives, in one place: the panel's own
   geometry, the keypad the machine has in hardware, the virtual card the files
   live on, and the main loop - parse, `cnc_dotasks()`, advance the clock - that
   makes a jog, a feed and a RUN actually execute.

   The window (`main.c`) draws and clicks through it, and the headless checks
   (`host_tests.c`) run it without a window. Nothing here knows about a screen,
   a file or a check; a check that needs a different starting point states it
   through this API (mount a root, press keys, run ticks, take a dump). */

/* This header is the station's own shell layer, so it is Windows: `HDC` below
   is a device context. The two files that include it take <windows.h> first
   (with the FORCEINLINE dance) before the firmware headers, so reaching for it
   here again costs nothing. */

/* The window is the emulated panel - the picture the screen draws, exactly as
   the operator reads it - with the operator's own strip beside it. The glass is
   800x600 and is mounted turned, so the panel half of the window is the turned
   picture and `lvds_hstx.h` owns the turn between the two. */
#define PANEL_W LVDS_VIEW_WIDTH
#define PANEL_H LVDS_VIEW_HEIGHT
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

/* The pad key's meaning on the active screen: the pad entry that carries it, the
   screen's own word for a key the pad does not name, and whether the key steps a
   field or the axis. False when the key means nothing here. */
#include "nc2_visual.h"

bool host_key_meaning(char key, nc2_visual_key_meaning_t *meaning);

/* The machine keypad key a PC key stands for when that does not depend on the
   keyboard layout (`W` and Delete are the keypad's `#`, Enter is `D`, Esc is
   `A`, Backspace is `*`); 0 for every other key. The window completes the map
   with the digits and, through Windows, the letters and symbols the layout
   decides. */
char host_pc_machine_key(unsigned vk);

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

/* The preset entries the release ships: `examples\presets\*.txt` - one file per
   address, generated from the compiled table by `--dump-presets` - are copied
   into `<root>\presets` while that folder holds no entry file of its own, so a
   station opens with the words its keys write as files the operator can read and
   edit. Independent of `host_seed_card()`: a card that already has programs is
   exactly the card whose `presets` folder is empty. Nothing is overwritten.
   Returns how many files were placed. */
int host_seed_presets(const char *root, const char *examples);

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

/* Which build this exe is: its own file's timestamp and size, what Windows
   shows in Properties. A station that was just built and one left over from an
   earlier run are told apart by the window title or by `--version` - nothing of
   it is drawn on the emulated panel, which stays the machine's. */
void host_build_text(char *out, size_t out_sz);

/* Write the whole bench - the emulated panel and the operator's strip beside it
   - as a .bmp: the picture the window paints, so the key row and the usage
   lines are reviewable (and diffable) without opening the window. */
int host_dump_bench(const char *path);

/* Compose the whole bench - the emulated panel every call, the operator's strip
   when what it shows has changed - into the station's own memory bitmap, and
   hand back that DC and its 32bpp top-down pixels. The window blits the result
   in one go (which is why the strip does not blink); `--painttest` reads it.
   False when there is no memory bitmap, and then the caller draws straight. */
bool host_compose_bench(HDC window, HDC *dc_out, void **pixels_out);

#endif
