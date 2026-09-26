/* The station's headless checks: the dumps (`--dump`, `--dump-bench`) and
   every `--*test` the CI and a human run. They drive the same emulated machine
   the window does (host_shell.h) and read the firmware's own answers back -
   the state, the card, and the frame the panel drew - so a screen or a cycle
   that only looks right fails here.

   Each check's own header comment says what it pins and what it cannot: what
   is software-verified is here, what needs the machine is bench work and
   belongs in uCNC/src/modules/nc/TESTING.md and g7x/TESTING.md.
   */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* windows.h defines FORCEINLINE with a storage class, which clashes with the
   core's `static FORCEINLINE` declarations. Parse windows.h first, then let the
   firmware headers define it their own way. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef FORCEINLINE

#include "cnc.h"
#include "core/interpolator.h"
#include "core/planner.h"
#include "file_system.h"
#include "nc2_boot.h"
#include "nc2.h"
#include "nc2_draw.h"
#include "nc2_emit.h"
#include "nc2_files.h"
#include "nc2_layout.h"
#include "nc2_manual.h"
#include "nc2_presets.h"
#include "nc2_run.h"
#include "nc2_state.h"
#include "nc2_visual.h"
#include "nc2_vocab.h"
#include "g7x.h"
#include "host_fs.h"
#include "host_spindle.h"
#include "lvds_host.h"
#include "modules/cam_keyboard/cam_keyboard.h"
#include "host_shell.h"
#include "host_tests.h"

#include <stdlib.h>

/* The checks are a flat list: each one is defined with the helpers it owns, so
   a helper an earlier check uses is stated here. */
static bool host_fs_write_text(const char *path, const char *text);
static bool host_fs_read_text(const char *path, char *out, size_t out_sz);
static void host_press(char key);

static uint32_t host_frame_at(int x, int y);
static uint32_t host_panel_rgb(lvds_color_t color);



/* The keypad's event byte: bit 7 is the release flag and the low seven bits are
   the key. Every key the machine has must decode on both edges - a release that
   decodes as "no key" leaves the driver believing the key is still down, and
   then the panel takes the next press of that key for a repeat and drops it
   ("A works once until B is pressed"). */
static int host_keytest(void)
{
    static const struct {
        uint8_t event;      /* as the TCA8418 numbers them, 1*row+col */
        cam_key_t key;
        const char *name;
    } map[] = {
        {  1, CAM_KEY_STAR, "*" }, {  2, CAM_KEY_0, "0" },
        {  3, CAM_KEY_HASH, "#" }, {  4, CAM_KEY_D, "D" },
        { 11, CAM_KEY_1, "1" },    { 12, CAM_KEY_2, "2" },
        { 13, CAM_KEY_3, "3" },    { 14, CAM_KEY_C, "C" },
        { 21, CAM_KEY_4, "4" },    { 22, CAM_KEY_5, "5" },
        { 23, CAM_KEY_6, "6" },    { 24, CAM_KEY_B, "B" },
        { 31, CAM_KEY_7, "7" },    { 32, CAM_KEY_8, "8" },
        { 33, CAM_KEY_9, "9" },    { 34, CAM_KEY_A, "A" }
    };
    uint8_t raw[6] = {0};
    unsigned i;
    int failures = 0;

    for (i = 0u; i < sizeof(map) / sizeof(map[0]); i++) {
        raw[0] = map[i].event;
        if (cam_keyboard_decode_key(raw) != map[i].key) {
            printf("keytest: FAIL event %u is not '%s' pressed\n",
                   map[i].event, map[i].name);
            failures++;
        }
        raw[0] = (uint8_t)(map[i].event | 0x80);
        if (cam_keyboard_decode_key(raw) != map[i].key) {
            printf("keytest: FAIL event %u is not '%s' released\n",
                   map[i].event, map[i].name);
            failures++;
        }
        if (cam_keyboard_key_to_char(map[i].key) != map[i].name[0]) {
            printf("keytest: FAIL '%s' is not the character it sends\n", map[i].name);
            failures++;
        }
    }
    raw[0] = 0x00;
    if (cam_keyboard_decode_key(raw) != CAM_KEY_NONE) {
        puts("keytest: FAIL a zero event is a key");
        failures++;
    }
    if (failures) {
        printf("keytest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("keytest: PASS every keypad key decodes on both edges");
    return 0;
}

/* The real thing, loaded through the same code the machine uses: one NC program
   and the tool table that belongs to it (`uCNC/src/modules/nc/tests/fixtures`,
   copied into the `--files` root by the test runner).

   The program is the operator's facing job: a G971/G973 setup, a T2 tool line
   and a G80-terminated G71 contour with the corner round/chamfer words. The
   checks are the ones that would catch a regression the eye might miss - the
   block scan finding the block a line inside it, the emitter expanding it, and
   the T word resolving through the tool table to the tool that cuts it. */










/* Compare one band of two composed benches: the rows of `bytes` bytes starting
   at `from` in each row (the composite is 32bpp and top-down, so a column band
   is a per-row run). */
static bool host_band_same(const unsigned char *a, const unsigned char *b,
                           size_t from, size_t bytes)
{
    size_t stride = (size_t)WIN_W * 4u;
    int y;

    for (y = 0; y < WIN_H; y++) {
        if (memcmp(a + (size_t)y * stride + from,
                   b + (size_t)y * stride + from, bytes) != 0) {
            return false;
        }
    }
    return true;
}

/* The window composes the whole bench in one memory bitmap and blits it in one
   go, and it only recomposes the strip beside the panel when what the strip
   shows has changed (host_compose_bench()). The panel is one blit of the
   firmware's own buffer and cannot tear; the strip is two dozen GDI calls, and
   drawing those straight onto the window every 20 ms is what made it blink.

   Two failures are invisible in a dump of one screen and both are ugly on the
   desk:

     1. a strip left in the buffer - a screen change would keep showing the
        mode, the key meanings and the spindle of the screen before it;
     2. a strip that is not stable - composing one screen twice has to give the
        same picture, or something in it is being built differently each frame.

   The check composes into its own memory DC and reads the pixels back. */
static int host_painttest(void)
{
    HDC mem = CreateCompatibleDC(NULL);
    HDC bench = NULL;
    void *pixels = NULL;
    unsigned char *first;
    unsigned char *second;
    const size_t stride = (size_t)WIN_W * 4u;
    const size_t size = (size_t)WIN_W * (size_t)WIN_H * 4u;
    const size_t strip_from = (size_t)PANEL_W * 4u;
    const size_t strip_bytes = (size_t)SIDE_W * 4u;
    int failures = 0;

    if (!mem) {
        puts("painttest: FAIL no memory DC");
        return 1;
    }
    first = malloc(size);
    second = malloc(size);
    if (!first || !second) {
        puts("painttest: FAIL no room for the composed benches");
        return 1;
    }

    /* The panel half is the firmware's own buffer: the screen has to be drawn
       into it first, the way a tick does (`nc2_visual_draw()` then the repaint). */
    nc2_visual_select_mode(NC2_MODE_PROGRAM);
    nc2_visual_draw();
    if (!host_compose_bench(mem, &bench, &pixels)) {
        puts("painttest: FAIL the station has no bench bitmap");
        return 1;
    }
    memcpy(first, pixels, size);
    if (!host_compose_bench(mem, &bench, &pixels)) {
        puts("painttest: FAIL the second compose failed");
        return 1;
    }
    if (memcmp(first, pixels, size) != 0) {
        puts("painttest: FAIL composing the same screen twice changed it");
        failures++;
    } else {
        puts("painttest: the same screen composes the same picture");
    }

    /* 1. a screen change redraws the strip (and the panel with it). */
    nc2_visual_select_mode(NC2_MODE_RUN);
    nc2_visual_draw();
    if (!host_compose_bench(mem, &bench, &pixels)) {
        puts("painttest: FAIL the compose after the screen change failed");
        return 1;
    }
    memcpy(second, pixels, size);
    if (host_band_same(first, second, strip_from, strip_bytes)) {
        puts("painttest: FAIL a screen change left the old strip in the buffer");
        failures++;
    } else if (host_band_same(first, second, 0u, strip_from)) {
        puts("painttest: FAIL a screen change left the old panel in the buffer");
        failures++;
    } else {
        puts("painttest: a screen change redraws the panel and the strip");
    }

    /* 2. and that screen is stable too. */
    if (!host_compose_bench(mem, &bench, &pixels)) {
        puts("painttest: FAIL the third compose failed");
        return 1;
    }
    if (memcmp(second, pixels, size) != 0) {
        puts("painttest: FAIL the composed bench changes between frames");
        failures++;
    } else {
        puts("painttest: the strip is stable across frames");
    }
    (void)stride;

    free(second);
    free(first);
    DeleteDC(mem);
    if (failures) {
        printf("painttest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("painttest: PASS the bench is composed once and blitted whole");
    return 0;
}

/* Which build this is. The station has no version resource, so the answer is
   its own file's timestamp and size (host_build_text()) - the same figures
   Explorer shows, which is what tells the exe that was just built from one
   that was copied, zipped or left over from an earlier run. The window title
   carries the same line. */
static int host_version(void)
{
    char build[80];

    host_build_text(build, sizeof(build));
    printf("nc_ui: uCNC programming station (PC), built %s\n", build);
    return 0;
}



/* Write a file on the card through the same fs_* API the firmware uses. */
static bool host_fs_write_text(const char *path, const char *text)
{
    size_t len = strlen(text);
    fs_file_t *fp = fs_open(path, "w");

    if (!fp) {
        return false;
    }
    if (fs_write(fp, (const uint8_t *)text, len) != len) {
        fs_close(fp);
        return false;
    }
    fs_close(fp);
    return true;
}

/* The whole of a card file, as text: the checks that read an entry back compare
   the file itself rather than what the module remembers of it. The `\r` is
   dropped where it is found, because a card may have been written on a PC. */
static bool host_fs_read_text(const char *path, char *out, size_t out_sz)
{
    fs_file_t *fp;
    size_t used = 0u;

    if (!out || out_sz == 0u) {
        return false;
    }
    out[0] = '\0';
    fp = fs_open(path, "r");
    if (!fp) {
        return false;
    }
    while (fs_available(fp) > 0 && used + 1u < out_sz) {
        char c;

        if (fs_read(fp, (uint8_t *)&c, 1u) != 1u) {
            fs_close(fp);
            return false;
        }
        if (c != '\r') {
            out[used++] = c;
        }
    }
    out[used] = '\0';
    fs_close(fp);
    return true;
}





/* Headless check of the panel's one-shot blocks on the reader the RUN stream
   also shares: the two blocks a jog sends are both delivered, in order, and the
   reader goes back to the console when they are done. A jog used to lose the
   first block because the second one was written over it before the parser had
   read it. */
static int host_streamtest(void)
{
    char got[128];
    size_t n = 0u;
    unsigned guard;

    host_init_core();
    if (!nc2_run_send_line("G91 G1 X0.100 F500") ||
        !nc2_run_send_line("G90")) {
        puts("streamtest: FAIL the panel blocks were not queued");
        return 1;
    }
    for (guard = 0u; guard < 256u && grbl_stream_available(); guard++) {
        char c = grbl_stream_getc();

        if (n + 2u < sizeof(got)) {
            got[n++] = c ? c : '|';
        }
    }
    got[n] = '\0';
    if (strcmp(got, "G91 G1 X0.100 F500|G90|") != 0) {
        printf("streamtest: FAIL the reader saw \"%s\"\n", got);
        return 1;
    }
    if (grbl_stream_available()) {
        puts("streamtest: FAIL the reader did not go back to the console");
        return 1;
    }
    puts("streamtest: PASS both jog blocks delivered, reader handed back");
    return 0;
}

/* Press a keypad character the way the machine sends it. */
static void host_press(char key)
{
    nc2_visual_key(key);
}

/* Is anything in the region different from its own background (the pixel in its
   top-left corner)? */
static int host_ink_in(const uint32_t *px, int x, int y, int w, int h)
{
    uint32_t bg = px[(size_t)y * (size_t)lvds_host_width() + (size_t)x];
    int r;
    int c;

    for (r = 0; r < h; r++) {
        for (c = 0; c < w; c++) {
            if (px[(size_t)(y + r) * (size_t)lvds_host_width() + (size_t)(x + c)] != bg) {
                return 1;
            }
        }
    }
    return 0;
}

/* Nothing queued, nothing stepping, no cycle left to emit. */
static bool host_machine_idle(void)
{
    return !g7x_parser_busy() &&
           planner_buffer_is_empty() &&
           itp_is_empty();
}









/* Put the list's cursor on an entry by name: what a picker is for, and what the
   check has to do for itself (the screen's keys only step). */
static bool host_file2_select(const char *name)
{
    int i;

    for (i = 0; i < nc2_file_count(); i++) {
        const nc2_file_entry_t *e = nc2_file_entry(i);

        if (strcmp(e->name, name) != 0) {
            continue;
        }
        while (nc2_file_selected() > i) {
            nc2_file_step(-1);
        }
        while (nc2_file_selected() < i) {
            nc2_file_step(1);
        }
        return true;
    }
    return false;
}

/* nc2's file list: `0` opens the card, and the picker has to do the four things
   an operator needs from it - walk, open, make one, delete one - on the folders
   and the text files the card actually holds, with no pad in the way (the digits
   name a new file there).

     1. `0` lists the card's own root: the folders are there as well as the files,
        and `..` is not, because the root is as far up as it goes;
     2. `C` steps and `D` opens: a folder is entered, a program is loaded into the
        editor and the list closes;
     3. `5` then digits then `#` makes a numbered program in the folder being
        listed and opens it;
     4. `6` deletes the selected file, and `8` reads the folder again. */
static int host_file2test(void)
{
    static const char *const program = "/D/nc/files/one.nc";
    static const char *const text_file = "/D/nc/files/notes.txt";
    char created[NC2_PATH_MAX];
    int failures = 0;
    int count;
    int i;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, "G0 X1 Z1\n") ||
        !host_fs_write_text(text_file, "notes\n")) {
        puts("file2test: FAIL cannot write the fixture");
        return 1;
    }
    nc2_visual_init();
    nc2_visual_tick(4000u);
    (void)nc2_visual_open("/D/nc/files/one.nc");

    /* 1. the card's root. */
    nc2_visual_key('0');
    if (strcmp(nc2_visual_screen_name(), "FILES") != 0 ||
        strcmp(nc2_file_dir(), "/D") != 0) {
        printf("file2test: FAIL `0` is on \"%s\" at \"%s\"\n",
               nc2_visual_screen_name(), nc2_file_dir());
        return 1;
    }
    count = nc2_file_count();
    {
        bool saw_dir = false;
        bool saw_up = false;

        for (i = 0; i < count; i++) {
            const nc2_file_entry_t *e = nc2_file_entry(i);

            if (e->is_dir && strcmp(e->name, "nc") == 0) {
                saw_dir = true;
            }
            if (strcmp(e->name, "..") == 0) {
                saw_up = true;
            }
        }
        if (!saw_dir || saw_up) {
            printf("file2test: FAIL the root lists %d entries (dir %d, up %d)\n",
                   count, saw_dir, saw_up);
            failures++;
        }
    }

    /* 2. into the folders, and open a program. The card root holds `nc`, and the
       programs are one level under it. */
    if (!host_file2_select("nc")) {
        puts("file2test: FAIL the card's own folders are not listed");
        return 1;
    }
    nc2_visual_key('D');
    if (!host_file2_select("files")) {
        puts("file2test: FAIL the programs folder is not listed");
        return 1;
    }
    nc2_visual_key('D');
    if (strcmp(nc2_file_dir(), "/D/nc/files") != 0 ||
        nc2_file_count() < 2) {
        printf("file2test: FAIL entering `nc/files` gave \"%s\" with %d entries\n",
               nc2_file_dir(), nc2_file_count());
        failures++;
    }

    /* 3. and a program opens into the editor. */
    if (!host_file2_select("one.nc")) {
        puts("file2test: FAIL the program is not in its folder");
        return 1;
    }
    nc2_visual_key('D');
    if (strcmp(nc2_visual_screen_name(), "EDIT") != 0 ||
        strcmp(nc2_visual_path(), program) != 0) {
        printf("file2test: FAIL opening gave \"%s\" on \"%s\"\n",
               nc2_visual_screen_name(), nc2_visual_path());
        failures++;
    }

    /* 4. a new one, named with the pad's digits. */
    nc2_visual_key('0');
    nc2_visual_key('5');
    nc2_visual_key('4');
    nc2_visual_key('2');
    nc2_visual_key('#');
    snprintf(created, sizeof(created), "%s", nc2_visual_path());
    if (strcmp(created, "/D/42.nc") != 0 ||
        strcmp(nc2_visual_screen_name(), "EDIT") != 0 ||
        !host_fs_read_text("/D/42.nc", created, sizeof(created))) {
        printf("file2test: FAIL the new file is \"%s\"\n", created);
        failures++;
    }

    /* 5. and delete one: the notes, picked by name. */
    nc2_visual_key('0');
    if (!host_file2_select("nc")) {
        puts("file2test: FAIL the card's folders are gone");
        return 1;
    }
    nc2_visual_key('D');
    if (!host_file2_select("files")) {
        puts("file2test: FAIL the programs folder is gone");
        return 1;
    }
    nc2_visual_key('D');
    if (!host_file2_select("notes.txt")) {
        puts("file2test: FAIL the notes file is not in the folder");
        failures++;
    } else {
        bool gone = false;

        nc2_visual_key('6');
        count = nc2_file_count();
        nc2_visual_key('8');
        for (i = 0; i < nc2_file_count(); i++) {
            const nc2_file_entry_t *e = nc2_file_entry(i);

            if (!e->is_dir && strcmp(e->name, "notes.txt") == 0) {
                gone = false;
                break;
            }
            gone = true;
        }
        if (!gone || nc2_file_count() > count) {
            puts("file2test: FAIL the deleted file is still listed");
            failures++;
        }
    }

    /* And the list draws. */
    nc2_visual_draw();
    {
        const uint32_t *px = (const uint32_t *)lvds_host_pixels();

        if (!host_ink_in(px, NC2_LEFT_PANE_X + 4, NC2_PANE_Y + 30, 300, 200)) {
            puts("file2test: FAIL the list drew nothing");
            failures++;
        }
    }

    if (failures) {
        printf("file2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("file2test: PASS the card's list walks, opens, makes and deletes");
    return 0;
}

/* nc2's screen: the program down the left, the pad's corner on the right, no
   footer. The pad's slots are the files at the address walked to, which is the
   whole menu, and this is what walking it does to the program:

     1. the screen draws a program and the pad - ink where the pane is and where
        the pad is, nothing where a footer used to be;
     2. `4` walks into G7X: the pad's name is written as the line under the cursor
        (the title, and the place the entry will land), and the pad now shows
        G7X's own slots;
     3. `1` writes that entry where the name stood, and the pad stays;
     4. `D` then a digit types at the field the pad just picked - the value the
        entry landed with is replaced, not extended;
     5. `A` goes back up a level, and the program keeps what was written. */
static int host_screen2test(void)
{
    static const char *const program = "/D/nc/files/screen.nc";
    nc2_document_t nc_doc;
    const uint32_t *px;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program,
                            "G0 X52 Z2\n"
                            "G71 U1 R0.5 X0.5 Z0.5 F450 P10 Q20\n"
                            "N10 G1 X30 Z0\n"
                            "G80\n")) {
        puts("screen2test: FAIL cannot write the fixture");
        return 1;
    }
    nc2_visual_init();
    /* A card with no entries gets them on the first start, and the logo stands
       while that happens: let it go before looking at the screen. */
    nc2_visual_tick(4000u);
    if (nc2_boot_active()) {
        puts("screen2test: FAIL the boot logo stayed up");
        return 1;
    }
    if (!nc2_visual_open(program)) {
        puts("screen2test: FAIL nc2 did not open the fixture");
        return 1;
    }

    /* 1. it draws, and where the footer used to be there is program. */
    nc2_visual_draw();
    px = (const uint32_t *)lvds_host_pixels();
    if (!host_ink_in(px, 20, 40, 480, 400)) {
        puts("screen2test: FAIL the code pane drew nothing");
        failures++;
    }
    if (!host_ink_in(px, LVDS_HSTX_WIDTH - 240, LVDS_HSTX_HEIGHT - 240, 230, 230)) {
        puts("screen2test: FAIL the pad's corner drew nothing");
        failures++;
    }
    /* The drawing pane has the part in it - the stock, the chuck and the DIN
       rulers - and none of that is the pad. */
    if (!host_ink_in(px, NC2_RIGHT_PANE_X + 20, NC2_PANE_Y + 90, 320, 160)) {
        puts("screen2test: FAIL the drawing pane drew nothing");
        failures++;
    }

    /* 2. into G7X. */
    nc2_visual_key('4');
    if (strcmp(nc2_visual_address(), "4") != 0 ||
        !nc2_visual_slot_label('1') ||
        strcmp(nc2_visual_slot_label('1'), "OD ROUGH") != 0) {
        printf("screen2test: FAIL the pad at \"%s\" shows \"%s\"\n",
               nc2_visual_address(),
               nc2_visual_slot_label('1') ? nc2_visual_slot_label('1') : "");
        failures++;
    }
    nc2_visual_draw();
    px = (const uint32_t *)lvds_host_pixels();
    if (!host_ink_in(px, 20, 40, 480, 400)) {
        puts("screen2test: FAIL the program does not show the pad's name");
        failures++;
    }
    /* The two panes are told apart by the line between them and nothing else: no
       box around either, so the split is the one line the layout draws. */
    if (!host_ink_in(px, NC2_SPLIT_X - 1, NC2_PANE_Y + 8, 3, 40)) {
        puts("screen2test: FAIL the line between the panes is not drawn");
        failures++;
    }

    /* 3. the entry lands where the pad's name stood, and the pad stays. */
    nc2_visual_key('1');
    if (strcmp(nc2_visual_address(), "4") != 0 ||
        nc2_visual_slot_label('1') == 0) {
        puts("screen2test: FAIL the pad closed when the entry landed");
        failures++;
    }

    /* 4. the field the entry landed with is picked: a digit replaces it. */
    nc2_visual_key('D');
    nc2_visual_key('9');
    nc2_visual_key('#');

    /* 5. and `0` is the exit everywhere: with a pad up it leaves the pad in one
       press, whatever level it is on, instead of opening the card. */
    nc2_visual_key('0');
    if (strcmp(nc2_visual_address(), "") != 0 ||
        strcmp(nc2_visual_screen_name(), "EDIT") != 0) {
        printf("screen2test: FAIL `0` left \"%s\" on \"%s\"\n",
               nc2_visual_address(), nc2_visual_screen_name());
        failures++;
    }

    /* 6. `A` is the step back - in again, then one level out. */
    nc2_visual_key('4');
    if (strcmp(nc2_visual_address(), "4") != 0) {
        puts("screen2test: FAIL the pad did not open again");
        failures++;
    }
    nc2_visual_key('A');
    if (strcmp(nc2_visual_address(), "") != 0) {
        printf("screen2test: FAIL `A` left the address at \"%s\"\n",
               nc2_visual_address());
        failures++;
    }
    nc2_document_init(&nc_doc);
    {
        static char line[NC2_MAX_LINE_LEN];
        bool saw_typed = false;
        bool saw_name = false;
        size_t i;

        /* The screen's own document, read back through nc2's loader: the file
           it saves is the program. */
        if (!nc2_visual_save()) {
            puts("screen2test: FAIL nc2 did not save the program");
            failures++;
        } else if (!nc2_file_load(&nc_doc, program)) {
            puts("screen2test: FAIL the saved program does not load as a program");
            failures++;
        } else {
            for (i = 0u; i < nc_doc.line_count; i++) {
                snprintf(line, sizeof(line), "%s", nc_doc.lines[i]);
                if (strcmp(line, "G7X") == 0) {
                    saw_name = true;
                }
                if (strstr(line, "U9") != 0 && strstr(line, "G71") != 0) {
                    saw_typed = true;
                }
            }
            if (saw_name) {
                puts("screen2test: FAIL the pad's name stayed in the program");
                failures++;
            }
            if (!saw_typed) {
                puts("screen2test: FAIL the typed value is not in the program");
                failures++;
            }
        }
    }

    if (failures) {
        printf("screen2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("screen2test: PASS the pad is the menu, and the program is what it "
         "wrote");
    return 0;
}

/* The pad is the file tree: the digits walked are the address, and the slot at
   that address is the file with that name. There is no menu table anywhere -
   the only names are the files' own first rows - and this is what walking it
   looks like on the card the panel ships:

     1. the root holds the groups and nothing else, and a group's label is its
        file's name;
     2. a slot that is a pad opens (its address grows a digit), a slot that is an
        entry does not;
     3. pressing an entry writes it where the pad's name stood, the pad stays for
        the next press, and the next press lands under it;
     4. `A` steps back up one level, and the tree answers for the level it is
        at: the same digit means a different thing under a different address;
     5. an address nobody wrote a file for is simply not there. */
static int host_pad2test(void)
{
    char address[NC2_ADDR_MAX + 1];
    char label[NC2_PRESET_ROW_MAX];
    char rows[NC2_PRESET_ROW_MAX * 3];
    nc2_document_t doc;
    int kind;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    /* The screen seeds a card with no entries as it comes up (that is the first
       start), so the entries are there by now. */
    if (!nc2_preset_exists("1") || !nc2_preset_exists("41")) {
        puts("pad2test: FAIL the card was not seeded");
        return 1;
    }

    /* 1. the root. */
    nc2_address_reset(address);
    kind = nc2_slot(address, '1', label, sizeof(label));
    if (kind != NC2_SLOT_PAD || strcmp(label, "OPS") != 0) {
        printf("pad2test: FAIL the root's `1` is kind %d, \"%s\"\n", kind, label);
        failures++;
    }
    kind = nc2_slot(address, '4', label, sizeof(label));
    if (kind != NC2_SLOT_PAD || strcmp(label, "G7X") != 0) {
        printf("pad2test: FAIL the root's `4` is kind %d, \"%s\"\n", kind, label);
        failures++;
    }
    if (nc2_slot(address, '7', label, sizeof(label)) != NC2_SLOT_EMPTY) {
        puts("pad2test: FAIL the root's `7` is not empty");
        failures++;
    }

    /* 2. into G7X, where the slots are entries, not pads. */
    if (!nc2_address_push(address, '4') || strcmp(address, "4") != 0) {
        puts("pad2test: FAIL the address did not grow");
        failures++;
    }
    kind = nc2_slot(address, '1', label, sizeof(label));
    if (kind != NC2_SLOT_ENTRY || strcmp(label, "OD ROUGH") != 0) {
        printf("pad2test: FAIL G7X `1` is kind %d, \"%s\"\n", kind, label);
        failures++;
    }
    if (nc2_slot(address, '7', label, sizeof(label)) != NC2_SLOT_EMPTY) {
        puts("pad2test: FAIL `7` under G7X is not empty (nothing ships there)");
        failures++;
    }

    /* 3. pressing it: the entry lands where the pad's name stood, and the pad is
       still there for the next press. */
    nc2_document_init(&doc);
    (void)nc2_insert_line(&doc, 0u, "G0 X52 Z2");
    doc.cursor = 0u;
    if (!nc2_pad_open(&doc, label)) {
        puts("pad2test: FAIL the pad did not open on the slot's name");
        return 1;
    }
    if (nc2_preset_read("41", 0, 0u, rows, sizeof(rows)) <= 0 ||
        !nc2_pad_write(&doc, rows)) {
        puts("pad2test: FAIL the entry's rows were not read and written");
        failures++;
    }
    if (doc.line_count != 2u ||
        strcmp(doc.lines[1], "G71 U0 R0 X0 Z0 F0 P0 Q0") != 0 ||
        doc.cursor != 1u || doc.field != 0 || !nc2_pad_active(&doc)) {
        printf("pad2test: FAIL the entry reads \"%s\"\n",
               doc.line_count > 1u ? doc.lines[1] : "");
        failures++;
    }
    (void)nc2_key(&doc, NC2_KEY_ACCEPT, 0);
    (void)nc2_pad_write(&doc, "G1 X30 Z0");
    if (doc.line_count != 3u || strcmp(doc.lines[2], "G1 X30 Z0") != 0) {
        printf("pad2test: FAIL the second press wrote \"%s\"\n",
               doc.line_count > 2u ? doc.lines[2] : "");
        failures++;
    }
    nc2_pad_close(&doc);
    if (doc.line_count != 3u) {
        puts("pad2test: FAIL closing the pad took a written row");
        failures++;
    }

    /* 4. back up, and the same digit means something else. */
    nc2_address_pop(address);
    if (strcmp(address, "") != 0) {
        puts("pad2test: FAIL the address did not shrink");
        failures++;
    }
    kind = nc2_slot(address, '4', label, sizeof(label));
    if (kind != NC2_SLOT_PAD) {
        puts("pad2test: FAIL the root's `4` is not a pad again");
        failures++;
    }

    /* 5. and the file tree is the whole story: an address with no file is not
       there, and neither is a level below the pad's depth. */
    if (!nc2_address_push(address, '7') || !nc2_address_push(address, '1') ||
        !nc2_address_push(address, '1') || nc2_address_push(address, '1')) {
        puts("pad2test: FAIL the address grew past its depth");
        failures++;
    }
    nc2_address_reset(address);
    if (nc2_slot(address, '7', label, sizeof(label)) != NC2_SLOT_EMPTY ||
        nc2_slot(address, '9', label, sizeof(label)) != NC2_SLOT_EMPTY ||
        nc2_slot(address, '0', label, sizeof(label)) != NC2_SLOT_EMPTY) {
        puts("pad2test: FAIL a slot nobody wrote answers");
        failures++;
    }

    if (failures) {
        printf("pad2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("pad2test: PASS the pad is the file tree, and every press writes where "
         "the pad's name stood");
    return 0;
}



/* The same for nc2's. */
static bool host_emit_everything_nc2(const nc2_document_t *doc,
                                     size_t start,
                                     char *out,
                                     size_t out_sz)
{
    nc2_emit_stream_t stream;
    size_t line = 0u;
    size_t used = 0u;

    out[0] = '\0';
    nc2_emit_stream_begin(&stream, doc, start);
    nc2_emit_stream_set_log(&stream, false);
    while (stream.active) {
        char text[NC2_MAX_LINE_LEN];
        nc2_emit_result_t r = nc2_emit_stream_next(&stream, text, sizeof(text),
                                                   &line);

        if (r == NC2_EMIT_ERROR) {
            return false;
        }
        if (r != NC2_EMIT_LINE) {
            continue;
        }
        if (used + strlen(text) + 2u > out_sz) {
            return false;
        }
        memcpy(out + used, text, strlen(text));
        used += strlen(text);
        out[used++] = '|';
        out[used] = '\0';
    }
    return true;
}

/* What the machine is told, line for line. The sender is the one thing the
   module may not change on the way in, so the lines it makes are pinned against
   the answer nc gave for the same programs (`EMIT_GOLDEN_*` below):

     1. a program with two roughing cycles, a finish cut each and a plain row
        after them;
     2. the same program started mid-file (`RUN FROM`), where the sender has to
        prime its point from the lines above before it can resolve anything;
     3. a contour written with Fanuc's `U`/`W` increments, which the sender has
        to leave as the absolute lines the controller reads.

   A difference fails here rather than on the machine. */

/* What the sender has to keep making of those programs. nc's own
   expansion is the reference these were taken from, line for line,
   while both modules were still built: the wire content may not drift,
   and nc is retired, so the answer is written down here. */
#define EMIT_GOLDEN_TOP \
    "T2|" \
    "M3 S450|" \
    "G0 X52 Z2|" \
    "(NC G71 generated)|" \
    "(G71 rough X23.000)|" \
    "G0 X55.000|" \
    "G0 Z4.000|" \
    "G1 X46.000 F500.000|" \
    "G1 Z-20.471 F500.000|" \
    "G0 X55.000|" \
    "G0 Z4.000|" \
    "(G71 rough X20.000)|" \
    "G1 X40.000 F500.000|" \
    "G1 Z-16.941 F500.000|" \
    "G0 X55.000|" \
    "G0 Z4.000|" \
    "(G71 rough X17.000)|" \
    "G1 X34.000 F500.000|" \
    "G1 Z-14.000 F500.000|" \
    "G0 X55.000|" \
    "G0 Z4.000|" \
    "(G71 rough X15.500)|" \
    "G1 X31.000 F500.000|" \
    "G1 Z-12.500 F500.000|" \
    "G0 X55.000|" \
    "G0 Z4.000|" \
    "(G7x finish contour)|" \
    "G1 X30.000 Z2.000 F500.000|" \
    "G1 X30.000 Z-13.000|" \
    "G1 X34.000 Z-15.000|" \
    "G1 X35.000 Z-15.000|" \
    "G1 X52.000 Z-25.000|" \
    "G0 X55.000|" \
    "G0 Z4.000|" \
    "(NC G70 generated)|" \
    "(G7x finish contour)|" \
    "G0 X54.000|" \
    "G0 Z3.000|" \
    "G1 X30.000 Z2.000 F120.000|" \
    "G1 X30.000 Z-13.000|" \
    "G1 X34.000 Z-15.000|" \
    "G1 X35.000 Z-15.000|" \
    "G1 X52.000 Z-25.000|" \
    "G0 X54.000|" \
    "G0 Z3.000|" \
    "(NC G71 generated)|" \
    "(G71 rough X23.000)|" \
    "G0 X53.000|" \
    "G0 Z2.000|" \
    "G1 X46.000 F500.000|" \
    "G1 Z-19.000 F500.000|" \
    "G0 X53.000|" \
    "G0 Z2.000|" \
    "(G71 rough X21.000)|" \
    "G1 X42.000 F500.000|" \
    "G1 Z-18.770 F500.000|" \
    "G0 X53.000|" \
    "G0 Z2.000|" \
    "(G71 rough X19.000)|" \
    "G1 X38.000 F500.000|" \
    "G1 Z-17.571 F500.000|" \
    "G0 X53.000|" \
    "G0 Z2.000|" \
    "(G71 rough X18.000)|" \
    "G1 X36.000 F500.000|" \
    "G1 Z-16.179 F500.000|" \
    "G0 X53.000|" \
    "G0 Z2.000|" \
    "(G7x finish contour)|" \
    "G1 X35.000 Z0.000 F500.000|" \
    "G1 X35.000 Z-15.000|" \
    "G2 X45.000 Z-20.000 I5.000 K0.000|" \
    "G1 X50.000 Z-20.000|" \
    "G0 X53.000|" \
    "G0 Z2.000|" \
    "(NC G70 generated)|" \
    "(G7x finish contour)|" \
    "G0 X52.000|" \
    "G0 Z1.000|" \
    "G1 X35.000 Z0.000 F120.000|" \
    "G1 X35.000 Z-15.000|" \
    "G2 X45.000 Z-20.000 I5.000 K0.000|" \
    "G1 X50.000 Z-20.000|" \
    "G0 X52.000|" \
    "G0 Z1.000|" \
    "G1 X60 Z5 C0 R0|" \
    "M5|"

#define EMIT_GOLDEN_MID \
    "(NC G70 generated)|" \
    "(G7x finish contour)|" \
    "G0 X54.000|" \
    "G0 Z3.000|" \
    "G1 X30.000 Z2.000 F120.000|" \
    "G1 X30.000 Z-13.000|" \
    "G1 X34.000 Z-15.000|" \
    "G1 X35.000 Z-15.000|" \
    "G1 X52.000 Z-25.000|" \
    "G0 X54.000|" \
    "G0 Z3.000|" \
    "(NC G71 generated)|" \
    "(G71 rough X23.000)|" \
    "G0 X53.000|" \
    "G0 Z2.000|" \
    "G1 X46.000 F500.000|" \
    "G1 Z-19.000 F500.000|" \
    "G0 X53.000|" \
    "G0 Z2.000|" \
    "(G71 rough X21.000)|" \
    "G1 X42.000 F500.000|" \
    "G1 Z-18.770 F500.000|" \
    "G0 X53.000|" \
    "G0 Z2.000|" \
    "(G71 rough X19.000)|" \
    "G1 X38.000 F500.000|" \
    "G1 Z-17.571 F500.000|" \
    "G0 X53.000|" \
    "G0 Z2.000|" \
    "(G71 rough X18.000)|" \
    "G1 X36.000 F500.000|" \
    "G1 Z-16.179 F500.000|" \
    "G0 X53.000|" \
    "G0 Z2.000|" \
    "(G7x finish contour)|" \
    "G1 X35.000 Z0.000 F500.000|" \
    "G1 X35.000 Z-15.000|" \
    "G2 X45.000 Z-20.000 I5.000 K0.000|" \
    "G1 X50.000 Z-20.000|" \
    "G0 X53.000|" \
    "G0 Z2.000|" \
    "(NC G70 generated)|" \
    "(G7x finish contour)|" \
    "G0 X52.000|" \
    "G0 Z1.000|" \
    "G1 X35.000 Z0.000 F120.000|" \
    "G1 X35.000 Z-15.000|" \
    "G2 X45.000 Z-20.000 I5.000 K0.000|" \
    "G1 X50.000 Z-20.000|" \
    "G0 X52.000|" \
    "G0 Z1.000|" \
    "G1 X60 Z5 C0 R0|" \
    "M5|"

#define EMIT_GOLDEN_INC \
    "G0 X52 Z2|" \
    "G1 Z-23.000|" \
    "G1 X30.000|" \
    "G1 Z-38.000|" \
    "G1 X50.000|" \
    "M5|"

static int host_emit2test(void)
{
    static const char *const program = "/D/nc/files/emit.nc";
    static const char *const absolute_text =
        "G970 X-5 U60 Z-60 W5\n"
        "G971 X50 Z50 I0 E0\n"
        "G973 P7\n"
        "T2\n"
        "M3 S450\n"
        "G0 X52 Z2\n"
        "G71 U3 R1 X1 Z1 F500 P50 Q55\n"
        "N50 G1 X30 Z2\n"
        "G1 X30 Z-15 C2\n"
        "G1 X35 Z-15\n"
        "N55 G1 X52 Z-25\n"
        "G70 P50 Q55\n"
        "G71 U2 R1 X1 Z1 F500 P100 Q200\n"
        "N100 G1 X35 Z0 R0\n"
        "G1 X35 Z-20 R5\n"
        "N200 G1 X50 Z-20\n"
        "G70 P100 Q200\n"
        "G1 X60 Z5 C0 R0\n"
        "M5\n";
    static const char *const increments_text =
        "G971 X50 Z50 I0 E0\n"
        "G0 X52 Z2\n"
        "G1 W-25\n"
        "G1 U-22\n"
        "G1 W-15\n"
        "G1 U20\n"
        "M5\n";
        static const char *const golden[3] = {
        /* absolute_text from line 0 */
        EMIT_GOLDEN_TOP,
        /* absolute_text from line 11 - the `G70 P50 Q55`, whose tool is where
           the first cycle left it */
        EMIT_GOLDEN_MID,
        /* the same contour written with the `U`/`W` increments */
        EMIT_GOLDEN_INC
    };
    const char *const texts[3] = { absolute_text, absolute_text,
                                   increments_text };
    const size_t starts[3] = { 0u, 11u, 0u };
    int failures = 0;
    int pass;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    for (pass = 0; pass < 3; pass++) {
        nc2_document_t b;
        char out[4096];

        if (!host_fs_write_text(program, texts[pass])) {
            puts("emit2test: FAIL cannot write the fixture");
            return 1;
        }
        nc2_document_init(&b);
        if (!nc2_file_load(&b, program)) {
            puts("emit2test: FAIL the fixture does not load");
            return 1;
        }
        if (!host_emit_everything_nc2(&b, starts[pass], out, sizeof(out))) {
            printf("emit2test: FAIL pass %d from line %u does not expand\n",
                   pass, (unsigned)starts[pass]);
            failures++;
            continue;
        }
        if (strcmp(out, golden[pass]) != 0) {
            printf("emit2test: FAIL pass %d from line %u sends\n  \"%s\"\n"
                   "wanted\n  \"%s\"\n", pass, (unsigned)starts[pass], out,
                   golden[pass]);
            failures++;
        }
    }

    if (failures) {
        printf("emit2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("emit2test: PASS the sender still makes what nc made, from the top of "
         "a program and from the middle");
    return 0;
}

/* True when the run hands this line to the controller: the generator's own
   notes (`(G71 rough X23.000)`) are a line of the stream but not a line of the
   program. Mirrors the run's own question, so the check and the sender agree. */
static bool host_run2_sendable(const char *line)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (!*line || *line == '(') {
        return false;
    }
    return toupper((unsigned char)line[0]) == 'G' ||
           toupper((unsigned char)line[0]) == 'M' ||
           toupper((unsigned char)line[0]) == 'S' ||
           toupper((unsigned char)line[0]) == 'T' ||
           nc2_emit_line_is_direct(line);
}

/* Everything nc2's run sends for a program, one line per row, as the machine
   reads it: the expansion of the cycles, joined with newlines. `end_x`/`end_z`
   come back as the point the stream left the tool at, which is where the machine
   has to arrive. */
static bool host_nc2_expand(const nc2_document_t *doc,
                            size_t start,
                            char *out,
                            size_t out_sz,
                            float *end_x,
                            float *end_z)
{
    nc2_emit_stream_t stream;
    size_t line = 0u;
    size_t used = 0u;
    unsigned guard = 0u;

    out[0] = '\0';
    nc2_emit_stream_begin(&stream, doc, start);
    nc2_emit_stream_set_log(&stream, false);
    while (stream.active && guard++ < 100000u) {
        char text[NC2_MAX_LINE_LEN];
        nc2_emit_result_t r = nc2_emit_stream_next(&stream, text, sizeof(text),
                                                   &line);
        int n;

        if (r == NC2_EMIT_ERROR) {
            return false;
        }
        if (r != NC2_EMIT_LINE || !host_run2_sendable(text)) {
            continue;
        }
        n = snprintf(out + used, out_sz - used, "%s\n", text);
        if (n < 0 || (size_t)n >= out_sz - used) {
            return false;
        }
        used += (size_t)n;
    }
    if (end_x) {
        *end_x = stream.x;
    }
    if (end_z) {
        *end_z = stream.z;
    }
    return true;
}

/* Read what nc2's run sends until it is over, the way `host_read_run()` reads
   nc's: the machine is pumped between blocks, so the pacing is exercised, not
   bypassed. */
static void host_read_run2(char *sent, size_t sent_cap, size_t *sent_len)
{
    unsigned guard;

    *sent_len = 0u;
    for (guard = 0u; guard < 200000u; guard++) {
        bool read_any = false;

        while (grbl_stream_available()) {
            char c = grbl_stream_getc();

            read_any = true;
            if (c) {
                if (*sent_len + 1u < sent_cap) {
                    sent[(*sent_len)++] = c;
                }
            } else if (*sent_len > 0u && sent[*sent_len - 1u] != '\n' &&
                       *sent_len + 1u < sent_cap) {
                sent[(*sent_len)++] = '\n';
            }
        }
        if (read_any) {
            continue;
        }
        if (host_machine_idle() && !nc2_run_streaming()) {
            break;
        }
        host_pump(1u);
    }
    if (*sent_len > 0u && sent[*sent_len - 1u] != '\n' && *sent_len + 1u < sent_cap) {
        sent[(*sent_len)++] = '\n';
    }
    sent[*sent_len] = '\0';
}

/* nc2's run: the sender hands the machine what the program means, one unit at a
   time, and the machine really runs it. The check reads the same reader the
   controller reads, compares the lines with the expansion `nc2_emit` produces,
   requires the machine to have arrived where the program says, and looks at the
   glass for the floating DRO - which is there while the machine is busy and gone
   when it is not. */
static int host_run2test(void)
{
    static const char *const program = "/D/nc/files/run2.nc";
    static const char *const text =
        "G0 X52 Z2\n"
        "G71 U2 R1 X1 Z1 F500 P10 Q20\n"
        "N10 G1 X50 Z2\n"
        "G1 X40 Z2 C2\n"
        "N20 G1 X40 Z-20\n"
        "G70 P10 Q20\n"
        "M5\n";
    char sent[4096];
    char want[4096];
    size_t n = 0u;
    float want_x = 0.0f;
    float want_z = 0.0f;
    nc2_document_t doc;
    nc2_runtime_state_t rt;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, text)) {
        puts("run2test: FAIL cannot write the fixture");
        return 1;
    }
    nc2_visual_init();
    nc2_visual_tick(4000u);                 /* past the first start's logo */
    if (!nc2_visual_open(program)) {
        puts("run2test: FAIL the fixture does not load");
        return 1;
    }
    nc2_visual_select_mode(NC2_MODE_RUN);
    if (strcmp(nc2_visual_screen_name(), "RUN") != 0) {
        printf("run2test: FAIL the screen is \"%s\", not RUN\n",
               nc2_visual_screen_name());
        failures++;
    }

    /* Nothing running: the DRO is not there, so the whole drawing is the
       screen. */
    nc2_visual_draw();
    if (host_frame_at(NC2_DRO_X + 3, NC2_DRO_Y + 3) ==
        host_panel_rgb(nc2_col_run())) {
        puts("run2test: FAIL the DRO is up on an idle machine");
        failures++;
    }

    /* `3 FULL`: what the panel hands the controller, line for line. Reading the
       reader here is what takes the lines, so the machine does not run them -
       the next pass runs the same program for the machine's own sake. */
    nc2_visual_key('3');
    host_read_run2(sent, sizeof(sent), &n);

    nc2_document_init(&doc);
    if (!nc2_file_load(&doc, program) ||
        !host_nc2_expand(&doc, 0u, want, sizeof(want), &want_x, &want_z)) {
        puts("run2test: FAIL the fixture does not expand");
        failures++;
    } else if (strcmp(sent, want) != 0) {
        printf("run2test: FAIL FULL sent\n  \"%s\"\nwanted\n  \"%s\"\n",
               sent, want);
        failures++;
    }

    /* The same program again, but this time the lines are left to the machine:
       the run has to arrive where the expansion left the tool, the DRO has to be
       on the glass while it is busy, and gone when it is over. */
    nc2_run_reset();
    host_pump_idle(64u);
    nc2_visual_key('3');
    host_pump(1u);
    nc2_visual_draw();
    if (host_frame_at(NC2_DRO_X + 3, NC2_DRO_Y + 3) !=
        host_panel_rgb(nc2_col_run())) {
        puts("run2test: FAIL no DRO while the run is armed");
        failures++;
    }
    for (n = 0u; n < 200000u; n++) {
        if (host_machine_idle() && !nc2_run_streaming()) {
            break;
        }
        host_pump(1u);
    }
    host_pump_idle(64u);
    nc2_state_runtime(&rt);
    /* The program's X is a diameter and the axis works in the radius, so the
       machine's figure is half the program's (Z is the same in both). */
    if (fabsf(rt.x * 2.0f - want_x) > 0.1f || fabsf(rt.z - want_z) > 0.05f) {
        printf("run2test: FAIL the machine ended at X%.3f Z%.3f, not the "
               "program's X%.3f Z%.3f\n", (double)rt.x, (double)rt.z,
               (double)want_x, (double)want_z);
        failures++;
    }
    nc2_visual_draw();
    if (host_frame_at(NC2_DRO_X + 3, NC2_DRO_Y + 3) ==
        host_panel_rgb(nc2_col_run())) {
        puts("run2test: FAIL the DRO stayed after the run");
        failures++;
    }

    /* `1 SINGLE` sends the unit the mark is on, and the mark stays on the line
       the operator stepped from. */
    nc2_run_reset();
    nc2_visual_key('C');
    nc2_visual_key('C');                    /* the mark is on line 3 */
    nc2_visual_key('1');                    /* SINGLE: the block it sits in */
    host_pump_idle(64u);
    if (nc2_run_display_line() != 2u) {
        printf("run2test: FAIL SINGLE moved the mark to line %lu\n",
               (unsigned long)(nc2_run_display_line() + 1u));
        failures++;
    }

    if (failures) {
        printf("run2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("run2test: PASS the run hands over what the program means, the "
         "machine arrives, and the DRO floats only while it is busy");
    return 0;
}

/* How many pixels of one colour a rectangle of the frame holds. */
static int host_count_rect(int x0, int y0, int x1, int y1, uint32_t rgb)
{
    int count = 0;
    int x;
    int y;

    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            if (host_frame_at(x, y) == rgb) {
                count++;
            }
        }
    }
    return count;
}

/* The stock's own colour down one column of the preview: how many pixels carry
   it, and the first and last row they sit on. A column with none writes
   nothing into `top`/`bottom`. */
static int host_stock_column(int x, uint32_t rgb, int *top, int *bottom)
{
    int count = 0;
    int y;

    for (y = NC2_PANE_Y; y < NC2_PANE_BOTTOM; y++) {
        if (host_frame_at(x, y) != rgb) {
            continue;
        }
        if (count == 0 && top) {
            *top = y;
        }
        if (bottom) {
            *bottom = y;
        }
        count++;
    }
    return count;
}

/* The live stock: while the machine cuts, the drawing shows the material the
   tool has taken off, made from the machine's own position as the panel reads
   it every turn of its loop. The check turns a taper and reads the glass: the
   material below the tool is gone, its top is where it was, the RUN screen keeps
   the part while the machine is parked, and the editor draws the stock whole.

   This is a comparison of the stock's own colour per column, not a picture: the
   cut is where nc's mask says it is or the counts say so. */
static int host_live2test(void)
{
    static const char *const program = "/D/nc/files/live2.nc";
    static const char *const text =
        "G0 X50 Z-70\n"
        "G1 X10 Z-18 F200\n"
        "M5\n";
    static int before_col[NC2_RIGHT_PANE_W];
    static int before_top[NC2_RIGHT_PANE_W];
    static int before_bottom[NC2_RIGHT_PANE_W];
    int before_total = 0;
    int cut_columns = 0;
    int cut_top = 0;
    int cut_bottom = 0;
    int lost = 0;
    int stock_left = -1;
    int stock_right = -1;
    int rect_top = NC2_PANE_BOTTOM;
    int rect_bottom = NC2_PANE_Y;
    int kept;
    int whole;
    uint32_t stock_rgb;
    int x;
    unsigned i;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, text)) {
        puts("live2test: FAIL cannot write the fixture");
        return 1;
    }
    nc2_visual_init();
    nc2_visual_tick(4000u);
    if (!nc2_visual_open(program)) {
        puts("live2test: FAIL the fixture does not load");
        return 1;
    }
    nc2_visual_select_mode(NC2_MODE_RUN);

    /* The tool is where the operator left it, and that is where the mask starts:
       parked off the stock, not sitting on the axis, or the first frame's own
       position would read as a cut. */
    if (!nc2_run_send_line("G0 X60 Z10")) {
        puts("live2test: FAIL the panel will not park the tool");
        return 1;
    }
    host_pump(1u);
    for (i = 0u; i < 200000u; i++) {
        if (host_machine_idle()) {
            break;
        }
        host_pump(1u);
    }
    nc2_visual_draw();

    /* The stock, off the idle RUN screen: its colour's own columns, and the
       band of rows they sit in - the rectangle every count below is taken in. */
    stock_rgb = host_panel_rgb(nc2_col_prev_stock());
    for (x = NC2_RIGHT_PANE_X; x < NC2_RIGHT_PANE_X + NC2_RIGHT_PANE_W; x++) {
        int col = x - NC2_RIGHT_PANE_X;
        int top = 0;
        int bottom = 0;

        before_top[col] = 0;
        before_bottom[col] = 0;
        before_col[col] = host_stock_column(x, stock_rgb, &top, &bottom);
        if (before_col[col] <= 0) {
            continue;
        }
        before_top[col] = top;
        before_bottom[col] = bottom;
        before_total += before_col[col];
        if (stock_left < 0) {
            stock_left = x;
        }
        stock_right = x;
        if (top < rect_top) {
            rect_top = top;
        }
        if (bottom > rect_bottom) {
            rect_bottom = bottom;
        }
    }
    if (stock_left < 0 || stock_right - stock_left < 100 ||
        rect_bottom - rect_top < 60) {
        printf("live2test: FAIL the idle RUN screen shows no stock "
               "(columns %d..%d, rows %d..%d)\n", stock_left, stock_right,
               rect_top, rect_bottom);
        return 1;
    }
    /* The run, drawn the way the panel draws it - once per turn of its loop, so
       the mask comes off as the tool moves and not in one bite at the end. */
    nc2_run_reset();
    host_pump_idle(64u);
    nc2_visual_key('3');                    /* FULL */
    host_pump(1u);
    nc2_visual_draw();
    for (i = 0u; i < 200000u; i++) {
        if (host_machine_idle() && !nc2_run_streaming()) {
            break;
        }
        host_pump(1u);
        nc2_visual_draw();
    }
    host_pump_idle(64u);
    nc2_visual_draw();

    cut_top = NC2_PANE_BOTTOM;
    cut_bottom = NC2_PANE_Y;
    for (x = NC2_RIGHT_PANE_X; x < NC2_RIGHT_PANE_X + NC2_RIGHT_PANE_W; x++) {
        int col = x - NC2_RIGHT_PANE_X;
        int top = 0;
        int bottom = 0;
        int count = host_stock_column(x, stock_rgb, &top, &bottom);

        if (count >= before_col[col]) {
            continue;
        }
        cut_columns++;
        lost += before_col[col] - count;
        if (x < cut_top) {
            cut_top = x;
        }
        cut_bottom = x;
        /* The tool takes the material off from its own X down to the axis, so a
           column the cut reached is either shorter or empty - never one that
           lost its top and still holds material, which would be an erase, not a
           cut. A column the dimension layer already covered when the stock was
           whole can come out empty, which is why empty passes here. */
        if (count == 0) {
            continue;
        }
        if (top != before_top[col] || bottom >= before_bottom[col]) {
            printf("live2test: FAIL column %d is rows %d..%d, was %d..%d\n", x,
                   top, bottom, before_top[col], before_bottom[col]);
            failures++;
        }
    }
    if (cut_columns < 10 || lost < 500) {
        printf("live2test: FAIL the run took nothing off the stock "
               "(%d columns, %d pixels)\n", cut_columns, lost);
        failures++;
    }
    if (cut_bottom <= cut_top) {
        printf("live2test: FAIL the cut is a single column (%d)\n", cut_top);
        failures++;
    }
    if (cut_top < stock_left + 4 || cut_bottom > stock_right) {
        printf("live2test: FAIL the cut runs %d..%d, the stock %d..%d\n",
               cut_top, cut_bottom, stock_left, stock_right);
        failures++;
    }

    /* The RUN screen keeps the part: the machine is parked, and the second frame
       is the one the operator left on the glass. */
    kept = host_count_rect(stock_left, rect_top, stock_right + 1, rect_bottom + 1,
                           stock_rgb);
    nc2_visual_draw();
    if (host_count_rect(stock_left, rect_top, stock_right + 1, rect_bottom + 1,
                        stock_rgb) != kept) {
        puts("live2test: FAIL the part moves under the parked machine");
        failures++;
    }
    if (kept >= before_total) {
        printf("live2test: FAIL the parked RUN screen shows the stock whole "
               "(%d of %d)\n", kept, before_total);
        failures++;
    }

    /* The editor draws the stock whole again: the mask is the RUN screen's. */
    nc2_visual_select_mode(NC2_MODE_PROGRAM);
    nc2_visual_draw();
    whole = host_count_rect(stock_left, rect_top, stock_right + 1, rect_bottom + 1,
                            stock_rgb);
    if (whole != before_total) {
        printf("live2test: FAIL the editor shows %d stock pixels, wanted %d\n",
               whole, before_total);
        failures++;
    }

    if (failures) {
        printf("live2test: FAILED (%d)\n", failures);
        return 1;
    }
    printf("live2test: PASS the run takes the stock off (%d columns, %d "
           "pixels), the top stays, and the editor draws it whole\n",
           cut_columns, lost);
    return 0;
}

/* Drain what the panel queued for the controller, one line after another, into
   one string separated by `|`. Nothing here pumps the machine: these are the
   blocks the panel sends on its own (a jog, a zero, a spindle start). */
static void host_drain_panel(char *out, size_t out_sz)
{
    size_t used = 0u;
    unsigned guard;

    out[0] = '\0';
    for (guard = 0u; guard < 20000u; guard++) {
        bool any = false;

        while (grbl_stream_available()) {
            char c = grbl_stream_getc();

            any = true;
            if (c == '\n' || c == '\r' || c == 0) {
                if (used > 0u && out[used - 1u] != '|' && used + 1u < out_sz) {
                    out[used++] = '|';
                }
            } else if (used + 1u < out_sz) {
                out[used++] = c;
            }
        }
        if (!any) {
            break;
        }
    }
    out[used] = '\0';
}

/* nc2's MANUAL: the machine panel. The digits jog, the spindle runs from the
   keys, `#` swaps a step for feeding, `*` types the two stops of the picked
   axis, `D` touches it off and `0` zeroes it - and a jog is always the pair
   `G91 G1 ...` then `G90`, so nothing after it runs in the wrong distance mode.
   What each key sends is read off the same reader the controller reads. */
static int host_manual2test(void)
{
    char sent[512];
    float value = 0.0f;
    bool typed = false;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    nc2_visual_init();
    nc2_visual_tick(4000u);
    nc2_visual_select_mode(NC2_MODE_MANUAL);
    if (strcmp(nc2_visual_screen_name(), "MANUAL") != 0) {
        printf("manual2test: FAIL the screen is \"%s\", not MANUAL\n",
               nc2_visual_screen_name());
        return 1;
    }
    host_pump_idle(32u);

    /* A step jog is the move and the G90 that puts the machine back, in that
       order: X is a diameter, so a 0.100 mm step is written X0.200. */
    nc2_visual_key('B');                      /* the axis is X */
    nc2_visual_key('2');                      /* X+ */
    host_drain_panel(sent, sizeof(sent));
    if (!strstr(sent, "G91 G1 X0.200 F500") || !strstr(sent, "G90")) {
        printf("manual2test: FAIL the X+ step sent \"%s\"\n", sent);
        failures++;
    }
    host_pump_idle(64u);

    /* The value keys change the step the next press uses. */
    nc2_visual_key('3');                      /* one step bigger: 0.250 */
    nc2_visual_key('2');
    host_drain_panel(sent, sizeof(sent));
    if (!strstr(sent, "X0.500")) {
        printf("manual2test: FAIL the bigger step sent \"%s\"\n", sent);
        failures++;
    }
    host_pump_idle(64u);

    /* One more press, this time left to the machine: the axis has to move off
       the stop it is standing on, or the feed below would be refused - which is
       the answer the screen gives, and not what this check is about. */
    nc2_visual_key('2');
    host_pump_idle(64u);
    {
        nc2_runtime_state_t rt;

        nc2_state_runtime(&rt);
        if (!(rt.x > 0.05f)) {
            printf("manual2test: FAIL the X+ jog left the axis at %.3f\n",
                   (double)rt.x);
            failures++;
        }
    }

    /* The spindle keys are the machine's, and the speed is the remembered one. */
    nc2_visual_key('9');                      /* CW */
    host_drain_panel(sent, sizeof(sent));
    if (!strstr(sent, "M3 S")) {
        printf("manual2test: FAIL the CW key sent \"%s\"\n", sent);
        failures++;
    }
    nc2_visual_key('7');                      /* CCW */
    host_drain_panel(sent, sizeof(sent));
    if (!strstr(sent, "M4 S")) {
        printf("manual2test: FAIL the CCW key sent \"%s\"\n", sent);
        failures++;
    }
    nc2_visual_key('5');                      /* stop */
    host_drain_panel(sent, sizeof(sent));
    if (!strstr(sent, "M5")) {
        printf("manual2test: FAIL the stop key sent \"%s\"\n", sent);
        failures++;
    }
    host_pump_idle(64u);

    /* The stops are typed: `*` opens the minus side, `*` takes it and opens the
       plus one, `*` once more takes that and closes. */
    nc2_visual_key('*');
    if (!nc2_manual_field_active()) {
        puts("manual2test: FAIL `*` did not open the minus stop");
        failures++;
    }
    nc2_visual_key('0');
    nc2_visual_key('*');                      /* takes 0, opens the plus side */
    nc2_visual_key('7');
    nc2_visual_key('0');
    nc2_visual_key('*');                      /* takes 70, closes */
    if (nc2_manual_field_active()) {
        puts("manual2test: FAIL the stop field stayed open");
        failures++;
    }
    if (!nc2_manual_stop(0, 0, &value, &typed) || !typed ||
        fabsf(value) > 0.01f) {
        printf("manual2test: FAIL the X- stop is %.3f (typed=%d)\n",
               (double)value, (int)typed);
        failures++;
    }
    if (!nc2_manual_stop(0, 1, &value, &typed) || !typed ||
        fabsf(value - 70.0f) > 0.01f) {
        printf("manual2test: FAIL the X+ stop is %.3f (typed=%d)\n",
               (double)value, (int)typed);
        failures++;
    }

    /* `#` swaps the step for feeding while a key is held. */
    nc2_visual_key('#');
    if (!nc2_manual_continuous()) {
        puts("manual2test: FAIL `#` did not swap to the feed");
        failures++;
    }
    nc2_visual_key('8');                      /* X- toward the minus stop */
    host_drain_panel(sent, sizeof(sent));
    if (!strstr(sent, "$J=G91 X-")) {
        printf("manual2test: FAIL the held X- key sent \"%s\"\n", sent);
        failures++;
    }
    nc2_manual_hold(0);                       /* the key came up */
    host_pump_idle(64u);

    /* Zero writes the work offset for the picked axis. */
    nc2_visual_key('0');
    host_drain_panel(sent, sizeof(sent));
    if (!strstr(sent, "G10 L20 P0 X0")) {
        printf("manual2test: FAIL the zero key sent \"%s\"\n", sent);
        failures++;
    }
    host_pump_idle(64u);

    /* Touch-off types a value and `D` takes it. */
    nc2_visual_key('D');
    nc2_visual_key('1');
    nc2_visual_key('2');
    nc2_visual_key('D');
    host_drain_panel(sent, sizeof(sent));
    if (!strstr(sent, "G10 L20 P0 X12")) {
        printf("manual2test: FAIL the touch-off sent \"%s\"\n", sent);
        failures++;
    }

    if (failures) {
        printf("manual2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("manual2test: PASS the jog keys move the machine, the stops are typed, "
         "the spindle runs from the keys and the axis is zeroed and touched off");
    return 0;
}

/* nc2's TOOLS: the tool table is a file like any other, so the screen is the
   editor on it. A card with no table gets the shipped row and the file is
   written; the pad inserts into the table the same way it inserts into a
   program; and a look at the tools does not lose the program's place. */
static int host_tools2test(void)
{
    static const char *const program = "/D/nc/files/one.nc";
    static const char *const tool = "/D/nc/files/tool.t";
    char text[512];
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, "G0 X1 Z1\n")) {
        puts("tools2test: FAIL cannot write the fixture");
        return 1;
    }
    nc2_visual_init();
    nc2_visual_tick(4000u);
    (void)nc2_visual_open(program);

    /* TOOLS makes the table when the card has none. */
    nc2_visual_select_mode(NC2_MODE_TOOLS);
    if (strcmp(nc2_visual_screen_name(), "TOOLS") != 0 ||
        strcmp(nc2_visual_path(), tool) != 0) {
        printf("tools2test: FAIL TOOLS is \"%s\" at \"%s\"\n",
               nc2_visual_screen_name(), nc2_visual_path());
        return 1;
    }
    if (!nc2_visual_save() || !host_fs_read_text(tool, text, sizeof(text)) ||
        !strstr(text, "T1 ")) {
        printf("tools2test: FAIL the new table reads \"%s\"\n", text);
        failures++;
    }

    /* The pad inserts into the table like it inserts into a program: `4` walks
       into the G7X group and `1` writes its entry where the pad's name stands. */
    nc2_visual_key('4');
    if (strcmp(nc2_visual_address(), "4") != 0) {
        printf("tools2test: FAIL the pad address is \"%s\"\n",
               nc2_visual_address());
        failures++;
    }
    nc2_visual_key('1');
    if (!nc2_visual_save() || !host_fs_read_text(tool, text, sizeof(text)) ||
        !strstr(text, "G71")) {
        printf("tools2test: FAIL the inserted row is not in \"%s\"\n", text);
        failures++;
    }

    /* Back to the program: it is the one that was open. */
    nc2_visual_select_mode(NC2_MODE_PROGRAM);
    if (strcmp(nc2_visual_screen_name(), "EDIT") != 0 ||
        strcmp(nc2_visual_path(), program) != 0) {
        printf("tools2test: FAIL leaving TOOLS left \"%s\" at \"%s\"\n",
               nc2_visual_screen_name(), nc2_visual_path());
        failures++;
    }

    if (failures) {
        printf("tools2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("tools2test: PASS the tool table is a file the editor writes, and the "
         "program keeps its place");
    return 0;
}

/* The colour most of one code-pane row's text band carries - nc2's layout this
   time (`nc2_layout.h`), the same way `host_pane_row_bg()` reads nc's. */
static uint32_t host_pane2_row_bg(int row)
{
    uint32_t seen[8];
    int counts[8];
    int distinct = 0;
    int best = -1;
    int y = NC2_PANE_Y + 4 + row * NC2_ROW_H + NC2_ROW_H / 2;
    int x0 = NC2_LEFT_PANE_X + NC2_LINE_NO_PAD +
             4 * nc2_col_width(LVDS_FONT_NORMAL);
    int x;

    for (x = x0; x < NC2_LEFT_PANE_X + NC2_LEFT_PANE_W - 6; x++) {
        uint32_t c = host_frame_at(x, y);
        int i;

        for (i = 0; i < distinct; i++) {
            if (seen[i] == c) {
                counts[i]++;
                break;
            }
        }
        if (i != distinct) {
            continue;
        }
        if (distinct < (int)(sizeof(seen) / sizeof(seen[0]))) {
            seen[distinct] = c;
            counts[distinct] = 1;
            distinct++;
        }
    }
    for (x = 0; x < distinct; x++) {
        if (best < 0 || counts[x] > counts[best]) {
            best = x;
        }
    }
    return best < 0 ? 0u : seen[best];
}

/* nc2's marks, read off the glass: in EDIT the cursor's row is the bright
   selection and the block it heads is pale around it; in RUN the line in play is
   bright and its block pale, and everything outside both is the pane's own
   ground. A mark the snapshot carries but the pane never paints would pass a
   check on the document and still not be there, so this reads the frame. */
static int host_block2test(void)
{
    static const char *const program = "/D/nc/files/block2.nc";
    static const char *const text =
        "G0 X52 Z2\n"
        "G71 U2 R1 X1 Z1 F500 P10 Q20\n"
        "N10 G1 X50 Z2\n"
        "G1 X40 Z2 C2\n"
        "N20 G1 X40 Z-20\n"
        "G70 P10 Q20\n"
        "G1 X60 Z5\n";
    nc2_document_t doc;
    g7x_doc_t view;
    size_t first = 0u;
    size_t last = 0u;
    uint32_t select_rgb;
    uint32_t block_rgb;
    uint32_t bg_rgb;
    int failures = 0;
    int row;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, text)) {
        puts("block2test: FAIL cannot write the fixture");
        return 1;
    }
    nc2_visual_init();
    nc2_visual_tick(4000u);
    if (!nc2_visual_open(program)) {
        puts("block2test: FAIL the fixture does not load");
        return 1;
    }
    nc2_document_init(&doc);
    (void)nc2_file_load(&doc, program);
    view = nc2_document_g7x(&doc);
    if (!g7x_doc_line_path(&view, 3u, &first, &last)) {
        puts("block2test: FAIL the fixture has no block around line 4");
        return 1;
    }
    select_rgb = host_panel_rgb(nc2_col_select());
    block_rgb = host_panel_rgb(nc2_col_block());
    bg_rgb = host_panel_rgb(nc2_col_bg());
    if (block_rgb == bg_rgb || block_rgb == select_rgb) {
        puts("block2test: FAIL the block colour is not a mark of its own");
        return 1;
    }

    /* EDIT: walk the cursor onto a row inside the cycle and read every row. */
    while (nc2_visual_cursor() < 3u) {
        nc2_visual_key('C');
    }
    nc2_visual_draw();
    for (row = 0; row <= (int)last; row++) {
        uint32_t got = host_pane2_row_bg(row);
        bool want_select = (size_t)row == 3u;
        bool want_block = !want_select && (size_t)row >= first && (size_t)row <= last;

        if (want_select && got != select_rgb) {
            printf("block2test: FAIL EDIT row %d is 0x%06lX, not the "
                   "selection colour\n", row + 1, (unsigned long)got);
            failures++;
        } else if (want_block && got != block_rgb) {
            printf("block2test: FAIL EDIT row %d is 0x%06lX, not the block "
                   "colour\n", row + 1, (unsigned long)got);
            failures++;
        } else if (!want_select && !want_block && got != bg_rgb) {
            printf("block2test: FAIL EDIT row %d is 0x%06lX, not the pane's "
                   "ground\n", row + 1, (unsigned long)got);
            failures++;
        }
    }

    /* RUN: the same two colours, with the line in play taken from the sender -
       the row the mark names - and nothing outside the block carrying the pale
       one. */
    nc2_visual_select_mode(NC2_MODE_RUN);
    nc2_run_reset();
    while (nc2_run_display_line() < 3u) {
        nc2_visual_key('C');
    }
    nc2_visual_draw();
    for (row = 0; row <= (int)last; row++) {
        uint32_t got = host_pane2_row_bg(row);
        bool want_select = (size_t)row == 3u;
        bool want_block = !want_select && (size_t)row >= first && (size_t)row <= last;

        if (want_select && got != select_rgb) {
            printf("block2test: FAIL RUN row %d is 0x%06lX, not the "
                   "selection colour\n", row + 1, (unsigned long)got);
            failures++;
        } else if (want_block && got != block_rgb) {
            printf("block2test: FAIL RUN row %d is 0x%06lX, not the block "
                   "colour\n", row + 1, (unsigned long)got);
            failures++;
        } else if (!want_select && !want_block && got != bg_rgb) {
            printf("block2test: FAIL RUN row %d is 0x%06lX, not the pane's "
                   "ground\n", row + 1, (unsigned long)got);
            failures++;
        }
    }

    if (failures) {
        printf("block2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("block2test: PASS the bright line and the pale block around it are on "
         "the glass, on both code screens");
    return 0;
}

/* nc2's DRO, read off the glass: it is a band floating over the preview that is
   there only while the machine has something to say, it wears the panel's green
   while it runs and its red for a fault, and the machine's state word is on it.
   The state is said *once* - the header band never carries it (the bench's "i do
   see idle in two places ... only this one should remain"). */
static int host_label2test(void)
{
    static const char *const program = "/D/nc/files/label2.nc";
    static const char *const text =
        "G0 X52 Z2\n"
        "G1 X1 Y1 F500\n"
        "M5\n";
    uint32_t run_rgb = host_panel_rgb(nc2_col_run());
    uint32_t err_rgb = host_panel_rgb(nc2_col_error());
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, text)) {
        puts("label2test: FAIL cannot write the fixture");
        return 1;
    }
    nc2_visual_init();
    nc2_visual_tick(4000u);
    if (!nc2_visual_open(program)) {
        puts("label2test: FAIL the fixture does not load");
        return 1;
    }
    nc2_visual_select_mode(NC2_MODE_RUN);
    host_pump_idle(32u);

    /* Idle: there is no DRO at all, so the drawing keeps the whole preview. */
    nc2_visual_draw();
    if (host_frame_at(NC2_DRO_X + 3, NC2_DRO_Y + 3) == run_rgb ||
        host_frame_at(NC2_DRO_X + 3, NC2_DRO_Y + 3) == err_rgb) {
        puts("label2test: FAIL the DRO is up on an idle machine");
        failures++;
    }

    /* Running: the band is green, it is inside the preview pane (the header band
       keeps its own colour - the state is not said twice), and the word is
       drawn on it. */
    nc2_visual_key('3');
    host_pump(1u);
    nc2_visual_draw();
    if (host_frame_at(NC2_DRO_X + 3, NC2_DRO_Y + 3) != run_rgb) {
        puts("label2test: FAIL a running DRO is not green");
        failures++;
    } else if (host_frame_at(2, 2) == run_rgb ||
               host_frame_at(2, 2) == err_rgb) {
        puts("label2test: FAIL the header band wears the DRO's colour");
        failures++;
    } else {
        puts("label2test: the DRO is green while the machine is in a run");
    }

    /* A fault takes the band over: red with the state word still on it, and the
       green gone - a machine stopped by a problem must not still say "running".
       The alarm is the machine's own, raised here rather than read from a run
       that has to be timed. */
    nc2_run_reset();
    host_pump_idle(64u);
    cnc_alarm(EXEC_ALARM_HARD_LIMIT);
    nc2_visual_draw();
    if (!cnc_has_alarm()) {
        puts("label2test: FAIL the alarm did not hold");
        failures++;
    } else if (host_frame_at(NC2_DRO_X + 3, NC2_DRO_Y + 3) != err_rgb) {
        printf("label2test: FAIL a faulted DRO is 0x%06lX, not the fault "
               "colour\n",
               (unsigned long)host_frame_at(NC2_DRO_X + 3, NC2_DRO_Y + 3));
        failures++;
    } else if (host_frame_at(NC2_DRO_X + 3, NC2_DRO_Y + 3) == run_rgb) {
        puts("label2test: FAIL the faulted DRO is still green");
        failures++;
    } else {
        int x;
        bool ink = false;

        /* The state word is drawn on the red: any pixel in the band that is not
           the fault colour is the word's ink - the DRO has no other content on
           its last row. */
        for (x = NC2_DRO_X + 6; x < NC2_DRO_X + NC2_DRO_W - 6; x++) {
            if (host_frame_at(x, NC2_DRO_Y + 50) != err_rgb ||
                host_frame_at(x, NC2_DRO_Y + 54) != err_rgb) {
                ink = true;
                break;
            }
        }
        if (!ink) {
            puts("label2test: FAIL the faulted DRO lost its state word");
            failures++;
        } else {
            puts("label2test: the fault takes the DRO over in red");
        }
    }
    cnc_alarm(EXEC_ALARM_NOALARM);

    if (failures) {
        printf("label2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("label2test: PASS the DRO is up only while the machine is busy, and "
         "the state is said once");
    return 0;
}

/* nc2's pacer: a *unit* is what runs as one thing, and a new unit may only be
   handed over when the machine has finished the last one. A contour's own lines
   are the exception - they are one cut - and the mark may never name a line the
   sender has not handed over. What it proves is ordering; the motion is the
   machine's. */
static int host_pace2test(void)
{
    static const char *const program = "/D/nc/files/pace2.nc";
    static const char *const text =
        "G0 X52 Z2\n"
        "G71 U1 R0.2 X0.5 Z0.5 F450 P10 Q20\n"
        "N10 G1 X30 Z0\n"
        "G1 X30 Z-15 C0 R0\n"
        "N20 G1 X35 Z-25\n"
        "G70 P10 Q20\n"
        "G0 X80 Z0\n";
    nc2_document_t doc;
    nc2_runtime_state_t rt;
    size_t prev_now = 0u;
    unsigned guard;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, text)) {
        puts("pace2test: FAIL cannot write the fixture");
        return 1;
    }
    nc2_visual_init();
    nc2_visual_tick(4000u);
    if (!nc2_visual_open(program)) {
        puts("pace2test: FAIL the fixture does not load");
        return 1;
    }
    nc2_document_init(&doc);
    (void)nc2_file_load(&doc, program);
    nc2_visual_select_mode(NC2_MODE_RUN);
    host_pump_idle(32u);

    nc2_visual_key('3');                    /* FULL */
    prev_now = nc2_run_line();
    for (guard = 0u; guard < 200000u; guard++) {
        bool expanding_before = nc2_run_expanding();
        bool idle_at_pacer;
        size_t now;

        /* The main loop's two halves, so the machine's state can be read where
           the pacer reads it - between the parse and the tasks - instead of once
           per pump, where a unit that ended inside the pump would look like one
           that had not. */
        (void)cnc_parse_cmd();
        idle_at_pacer = host_machine_idle() && !grbl_stream_available();
        cnc_dotasks();
        nc2_visual_idle_tasks();
        mcu_unit_test_advance_time(1000u);
        now = nc2_run_line();
        if (nc2_run_streaming() && now > prev_now) {
            size_t at = now > 0u ? now - 1u : 0u;
            size_t mark;

            if (at >= doc.line_count) {
                at = doc.line_count - 1u;
            }
            /* A line may only be handed over while the machine is still running
               when it belongs to a block the sender is expanding - a contour is
               one cut. A plain line is a unit of its own and waits. */
            if (!idle_at_pacer && !expanding_before) {
                printf("pace2test: FAIL line %u was handed over while the "
                       "machine was still running\n", (unsigned)(at + 1u));
                failures++;
                break;
            }
            /* The mark is a line the sender has already handed over. */
            mark = nc2_run_display_line();
            if (mark > at) {
                printf("pace2test: FAIL the pane marks line %u while the sender "
                       "has handed over up to %u\n",
                       (unsigned)(mark + 1u), (unsigned)(at + 1u));
                failures++;
                break;
            }
        }
        prev_now = now;
        if (!nc2_run_streaming() && host_machine_idle()) {
            break;
        }
    }
    if (nc2_run_streaming()) {
        puts("pace2test: FAIL the run never ended");
        failures++;
    }
    /* The machine ran the program: the last line sends it to `X80 Z0`, which is
       X40 on the axis - the program's X is a diameter and the axis works in the
       radius. */
    host_pump_idle(64u);
    nc2_state_runtime(&rt);
    printf("pace2test: the run ended at X%.3f Z%.3f, sender line %u\n",
           (double)rt.x, (double)rt.z, (unsigned)(nc2_run_line() + 1u));
    if (fabs((double)rt.x - 40.0) > 0.5 || fabs((double)rt.z) > 0.5) {
        printf("pace2test: FAIL the machine did not reach the program's end\n");
        failures++;
    }

    if (failures) {
        printf("pace2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("pace2test: PASS one unit at a time, and the mark never runs ahead of "
         "what was handed over");
    return 0;
}

/* The demo the release carries, read the way nc2 reads it: a fresh card is
   seeded from `examples\` exactly once, the sample loads, scans as the two
   numbered `G71` ranges with their `G70` finish cuts, and expands; and the
   entries are files, so what a key writes is the card's own row. */
static int host_demo2test(void)
{
    static const char *const examples = "tools/nc_ui_win/examples";
    static const char *const program = "/D/nc/files/lathe-demo.nc";
    char expanded[4096];
    nc2_document_t doc;
    size_t blocks = 0u;
    size_t finishes = 0u;
    size_t i;
    int copied;
    int failures = 0;

    copied = host_seed_card(g_files_root, examples);
    printf("demo2test: seeded the empty card with %d files\n", copied);
    if (copied != 2) {
        puts("demo2test: FAIL the card was not seeded from the examples");
        return 1;
    }
    copied = host_seed_presets(g_files_root, examples);
    printf("demo2test: seeded %d preset files\n", copied);
    if (copied < 10) {
        puts("demo2test: FAIL the entries were not seeded as files");
        return 1;
    }
    host_init_core();
    /* The card's own entry is what a key writes, not a compiled table. */
    if (!host_fs_write_text("/D/presets/41.txt", "OD ROUGH\n G1 X50 Z2\n")) {
        puts("demo2test: FAIL cannot write the card's own entry");
        return 1;
    }
    nc2_visual_init();
    nc2_visual_tick(4000u);
    if (!nc2_visual_open(program)) {
        puts("demo2test: FAIL the demo does not load");
        return 1;
    }
    nc2_document_init(&doc);
    if (!nc2_file_load(&doc, program) || doc.line_count == 0u) {
        printf("demo2test: FAIL the demo does not load (%u lines)\n",
               (unsigned)doc.line_count);
        return 1;
    }
    printf("demo2test: the demo is %u lines\n", (unsigned)doc.line_count);
    {
        g7x_doc_t view = nc2_document_g7x(&doc);

        for (i = 0u; i < doc.line_count; i++) {
            uint32_t p = 0u;
            uint32_t q = 0u;
            size_t first = 0u;
            size_t last = 0u;
            char upper[8];
            const char *line = doc.lines[i];

            while (*line == ' ') {
                line++;
            }
            if (*line == 'N') {
                line++;
                while (*line >= '0' && *line <= '9') {
                    line++;
                }
                while (*line == ' ') {
                    line++;
                }
            }
            upper[0] = (char)toupper((unsigned char)line[0]);
            upper[1] = (char)toupper((unsigned char)line[1]);
            upper[2] = (char)toupper((unsigned char)line[2]);
            upper[3] = '\0';
            if (g7x_doc_line_is_header(line)) {
                blocks++;
                if (!g7x_doc_block_containing(&view, i, &first, &last) ||
                    first != i || last <= i) {
                    printf("demo2test: FAIL the cycle on line %u has no block\n",
                           (unsigned)(i + 1u));
                    failures++;
                    continue;
                }
                if (!g7x_doc_line_range(line, &p, &q)) {
                    printf("demo2test: FAIL the cycle on line %u names no range\n",
                           (unsigned)(i + 1u));
                    failures++;
                }
                i = last;                 /* the block owns its rows */
                continue;
            }
            if (strcmp(upper, "G70") == 0) {
                finishes++;
            }
        }
    }
    if (blocks != 2u || finishes != 2u) {
        printf("demo2test: FAIL the demo has %u cycles and %u finish cuts\n",
               (unsigned)blocks, (unsigned)finishes);
        failures++;
    }
    if (!host_nc2_expand(&doc, 0u, expanded, sizeof(expanded), NULL, NULL) ||
        !strstr(expanded, "G1 ")) {
        puts("demo2test: FAIL the demo does not expand");
        failures++;
    }

    /* A pad press writes the card's own entry, not a table compiled in. */
    nc2_visual_key('4');
    nc2_visual_key('1');
    if (!nc2_visual_save()) {
        puts("demo2test: FAIL the program could not be written back");
        failures++;
    } else {
        char written[1024];

        if (!host_fs_read_text(program, written, sizeof(written)) ||
            !strstr(written, "G1 X50 Z2")) {
            printf("demo2test: FAIL the card's entry is not in \"%s\"\n",
                   written);
            failures++;
        } else {
            puts("demo2test: the key wrote the card's own entry");
        }
    }

    if (failures) {
        printf("demo2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("demo2test: PASS the demo seeds a fresh card, scans as two cycles and "
         "expands, and the entries are files");
    return 0;
}

/* Every screen has to hand its frame to the panel.

   On the machine `lvds_hstx_present()` is the copy from the PSRAM draw buffer
   into the SRAM scanout: a screen that draws a perfect frame and never calls it
   shows a black panel. The host draws straight into the frame and its
   `present()` only repaints the window, so the pixels cannot tell - which is
   why the first nc2 firmware booted to a black screen with every host check
   green. The host counts the calls instead, and this insists on them: the first
   start's logo, and the work screen it hands over to. */
static int host_present2test(void)
{
    unsigned before;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!nc2_boot_active()) {
        puts("present2test: FAIL a fresh card did not raise the logo");
        return 1;
    }
    before = lvds_host_present_count();
    nc2_visual_draw();
    if (lvds_host_present_count() != before + 1u) {
        printf("present2test: FAIL the logo drew %u frames for the panel\n",
               lvds_host_present_count() - before);
        failures++;
    }

    /* Past the logo: the screen the operator works on, and the same one frame
       per draw. */
    nc2_visual_tick(4000u);
    if (nc2_boot_active()) {
        puts("present2test: FAIL the logo stayed up");
        failures++;
    }
    before = lvds_host_present_count();
    nc2_visual_draw();
    if (lvds_host_present_count() != before + 1u) {
        printf("present2test: FAIL the work screen drew %u frames for the "
               "panel\n", lvds_host_present_count() - before);
        failures++;
    }

    /* And every screen, not only the one that comes up first. */
    nc2_visual_select_mode(NC2_MODE_MANUAL);
    before = lvds_host_present_count();
    nc2_visual_draw();
    if (lvds_host_present_count() != before + 1u) {
        puts("present2test: FAIL the MANUAL screen never reached the panel");
        failures++;
    }
    nc2_visual_select_mode(NC2_MODE_RUN);
    before = lvds_host_present_count();
    nc2_visual_draw();
    if (lvds_host_present_count() != before + 1u) {
        puts("present2test: FAIL the RUN screen never reached the panel");
        failures++;
    }

    if (failures) {
        printf("present2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("present2test: PASS every screen hands its frame to the panel");
    return 0;
}

/* The screen's own main loop - the one the firmware module and the station
   window both run.

   This is the second machine-only blind spot: the host checks drive the screen
   themselves, so the loop that decides *when* to draw was never theirs. The
   firmware's own copy of it forgot to run the first start's clock, which left
   the logo up for ever; the loop lives in the screen now (`nc2_visual_pump()`)
   and this check runs it, with its own clock. */
/* One pass of the loop as both the firmware module and the window run it: ask
   the screen's pump, and draw when it says so. The pump keeps asking until a
   frame has actually been drawn, so a caller that ignored it would see it ask
   for ever - which is the point of it. */
static bool host_pump2(unsigned now_ms)
{
    if (!nc2_visual_pump(now_ms)) {
        return false;
    }
    nc2_visual_draw();
    return true;
}

static int host_pump2test(void)
{
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!nc2_boot_active()) {
        puts("pump2test: FAIL a fresh card did not raise the logo");
        return 1;
    }

    /* The logo is up: it is drawn, and it is still up while its time has not
       passed. */
    if (!host_pump2(1000u)) {
        puts("pump2test: FAIL the logo did not ask for a frame");
        failures++;
    }
    if (!nc2_boot_active()) {
        puts("pump2test: FAIL the logo left before its time");
        failures++;
    }

    /* Past its time it leaves *and says so*: the pass that ends it has to be a
       pass that draws, or the panel keeps the logo on the glass while the
       screen believes it is gone. */
    if (!host_pump2(1300u)) {
        puts("pump2test: FAIL the pass that ended the logo did not draw");
        failures++;
    }
    if (nc2_boot_active()) {
        puts("pump2test: FAIL the logo never left");
        failures++;
    }

    /* Nothing moving, nothing pressed: the loop stays quiet. */
    if (host_pump2(1400u)) {
        puts("pump2test: FAIL an idle screen asked for a frame");
        failures++;
    }

    /* A key asks for one. */
    nc2_visual_key('4');
    if (!host_pump2(1500u)) {
        puts("pump2test: FAIL a key did not ask for a frame");
        failures++;
    }

    /* And a screen with something of its own to show keeps the frames coming:
       MANUAL's held feed and the floating DRO need them, at the period and no
       faster. */
    nc2_visual_select_mode(NC2_MODE_MANUAL);
    if (!host_pump2(2000u)) {
        puts("pump2test: FAIL the screen change did not ask for a frame");
        failures++;
    }
    if (host_pump2(2010u)) {
        puts("pump2test: FAIL the screen drew faster than its own period");
        failures++;
    }
    if (!host_pump2(2030u)) {
        puts("pump2test: FAIL a screen that is working did not keep drawing");
        failures++;
    }

    if (failures) {
        printf("pump2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("pump2test: PASS the screen's own loop draws when it should, and the "
         "first start's logo leaves");
    return 0;
}

/* The path builder - G7X's `7`, the address 47.

   nc's contour pad, so this is nc's `--contourtest` carried over: the pad's
   nine directions, one `G1` row per press, the axis that does not move carried
   over from the point the row above reaches, the value that lands picked so the
   digits type the real number over it, `#` stepping the distance, `*` taking the
   point back, and `5` ending it with the rows still in the program. It also pins
   the pad's own order, which the bench reads first: 7 8 9 on the top row.

   The check reads the program back off the card after every press, the way the
   operator would see it. */
static int host_contour2test(void)
{
    static const char *const program = "/D/nc/files/contour.nc";
    static const char *const fixture = "G0 X52 Z2\n";
    nc2_document_t doc;
    int failures = 0;

    /* The pad's order, before anything is drawn or pressed: the digits run up
       the way the machine's keypad does, so `7` is the top-left cell. */
    if (nc2_pad_cell_key(0, 0) != '7' || nc2_pad_cell_key(0, 2) != '9' ||
        nc2_pad_cell_key(1, 1) != '5' || nc2_pad_cell_key(2, 0) != '1' ||
        nc2_pad_cell_key(2, 2) != '3') {
        printf("contour2test: FAIL the pad's cells are %c%c%c / %c%c%c / %c%c%c\n",
               nc2_pad_cell_key(0, 0), nc2_pad_cell_key(0, 1),
               nc2_pad_cell_key(0, 2), nc2_pad_cell_key(1, 0),
               nc2_pad_cell_key(1, 1), nc2_pad_cell_key(1, 2),
               nc2_pad_cell_key(2, 0), nc2_pad_cell_key(2, 1),
               nc2_pad_cell_key(2, 2));
        failures++;
    }

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, fixture)) {
        puts("contour2test: FAIL cannot write the fixture");
        return 1;
    }
    nc2_visual_init();
    nc2_visual_tick(4000u);
    if (!nc2_visual_open(program)) {
        puts("contour2test: FAIL the fixture does not load");
        return 1;
    }
    nc2_visual_select_mode(NC2_MODE_PROGRAM);

    /* 1. the pad opens on G7X's `7`, and that cell says what it is. */
    nc2_visual_key('4');
    if (strcmp(nc2_visual_address(), "4") != 0 ||
        !nc2_visual_slot_label('7') ||
        strcmp(nc2_visual_slot_label('7'), "PATH") != 0) {
        printf("contour2test: FAIL G7X's `7` reads \"%s\" at \"%s\"\n",
               nc2_visual_slot_label('7') ? nc2_visual_slot_label('7') : "",
               nc2_visual_address());
        failures++;
    }
    nc2_visual_key('7');
    if (!nc2_contour_active()) {
        puts("contour2test: FAIL `4` `7` did not open the path builder");
        return 1;
    }
    /* Its pad is the directions, not the card's entries. */
    if (!nc2_visual_slot_label('5') ||
        strcmp(nc2_visual_slot_label('5'), "END") != 0) {
        puts("contour2test: FAIL the builder's pad does not show its directions");
        failures++;
    }

    /* 2. one press, one row - and the point comes from the program, not from a
       memory of the pad: `G0 X52 Z2` is what the first press counts from. */
    nc2_visual_key('2');                        /* X+ */
    /* 3. the value that landed is picked, so the digits type the real number
       over the step's prefill, and `D` gives the pad its digits back. */
    nc2_visual_key('3');
    nc2_visual_key('0');
    nc2_visual_key('D');
    if (!nc2_visual_save()) {
        puts("contour2test: FAIL the program was not written back");
        return 1;
    }
    nc2_document_init(&doc);
    if (!nc2_file_load(&doc, program) || doc.line_count != 2u ||
        strcmp(doc.lines[1], "G1 X30 Z2") != 0) {
        printf("contour2test: FAIL the first point is \"%s\"\n",
               doc.line_count > 1u ? doc.lines[1] : "");
        failures++;
    } else {
        puts("contour2test: the point continues from the program, and its value "
             "is typed over the prefill");
    }

    /* 4. the next press reads the point from the row just written and carries
       the axis that does not move. */
    nc2_visual_key('4');                        /* Z- */
    (void)nc2_visual_save();
    nc2_document_init(&doc);
    if (!nc2_file_load(&doc, program) || doc.line_count != 3u ||
        strcmp(doc.lines[1], "G1 X30 Z2") != 0 ||
        strcmp(doc.lines[2], "G1 X30 Z1.5") != 0) {
        printf("contour2test: FAIL the second point is \"%s\"\n",
               doc.line_count > 2u ? doc.lines[2] : "");
        failures++;
    } else {
        puts("contour2test: the walk carries the axis that does not move");
    }

    /* 5. `#` steps the distance - but only once the point is settled. */
    nc2_visual_key('#');                        /* take the point */
    nc2_visual_key('#');                        /* step the distance */
    nc2_visual_key('2');                        /* X+ by the new step */
    nc2_visual_key('#');                        /* and take that one too */
    (void)nc2_visual_save();
    nc2_document_init(&doc);
    if (!nc2_file_load(&doc, program) || doc.line_count != 4u ||
        strcmp(doc.lines[3], "G1 X31 Z1.5") != 0) {
        printf("contour2test: FAIL the stepped point is \"%s\"\n",
               doc.line_count > 3u ? doc.lines[3] : "");
        failures++;
    } else {
        puts("contour2test: `#` steps the distance");
    }

    /* 6. `*` drops the point being entered - the row goes with it - and the pad
       is still up. */
    nc2_visual_key('2');                        /* a point, still being entered */
    nc2_visual_key('*');
    if (!nc2_contour_active()) {
        puts("contour2test: FAIL `*` closed the builder");
        failures++;
    }
    (void)nc2_visual_save();
    nc2_document_init(&doc);
    if (!nc2_file_load(&doc, program) || doc.line_count != 4u) {
        printf("contour2test: FAIL `*` left %u rows\n",
               (unsigned)doc.line_count);
        failures++;
    } else {
        puts("contour2test: `*` drops the point being entered");
    }

    /* 7. `5` ends the builder, and the rows already written stay - the program
       is an ordinary program, so it also expands. */
    nc2_visual_key('5');
    if (nc2_contour_active()) {
        puts("contour2test: FAIL `5` did not end the builder");
        failures++;
    }
    (void)nc2_visual_save();
    nc2_document_init(&doc);
    if (!nc2_file_load(&doc, program) || doc.line_count != 4u ||
        strcmp(doc.lines[1], "G1 X30 Z2") != 0 ||
        strcmp(doc.lines[3], "G1 X31 Z1.5") != 0) {
        puts("contour2test: FAIL `5` did not leave the rows it wrote");
        failures++;
    }
    {
        g7x_doc_t view = nc2_document_g7x(&doc);
        float x = 0.0f;
        float z = 0.0f;
        size_t i;
        bool any = false;

        for (i = 0u; i < doc.line_count; i++) {
            uint8_t words = 0u;

            if (nc2_emit_line_is_direct(doc.lines[i]) &&
                nc2_emit_line_point(doc.lines[i], &x, &z, 0, 0u, &words)) {
                any = true;
            }
        }
        (void)view;
        if (!any) {
            puts("contour2test: FAIL the rows it wrote move nothing");
            failures++;
        }
    }

    /* 8. the mode key leaves it, and the rows stay: a builder is a view of the
       program, not a mode the program is in. */
    nc2_visual_key('4');
    nc2_visual_key('7');
    nc2_visual_key('A');
    if (nc2_contour_active()) {
        puts("contour2test: FAIL the mode key left the builder up");
        failures++;
    }

    if (failures) {
        printf("contour2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("contour2test: PASS the path builder writes the profile it walks, one "
         "G1 row per press, and the pad stays until 5");
    return 0;
}

/* What the card really hands back, and what the panel therefore has to accept.

   A card is FAT, and the machine's FatFs is built without long filenames
   (`FF_USE_LFN 0`): a program written on a PC as `lathe-demo.nc` is stored and
   read back as `LATHE-~1.NC` - 8.3, extension in capitals. nc matched its
   extensions case-insensitively (`nc_has_suffix_ci()`); nc2 compared with strcmp
   and so dropped every such file from the list, which is the bench's "it does
   not show their names properly nor does it seem to open them" on a card full of
   old programs. This writes the shapes a real card has and insists they are
   offered: a capital `.NC`, a capital `.TXT`, and the preset entries a card
   written on a PC holds (`41.TXT`), which are what `nc2_presets_any()` has to
   see or a card in use is seeded again on every boot. */
static int host_case2test(void)
{
    char text[512];
    int failures = 0;
    int i;
    bool saw_upper = false;
    bool saw_upper_text = false;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text("/D/nc/files/UPPER.NC", "G0 X1 Z1\n") ||
        !host_fs_write_text("/D/nc/files/UPPER.TXT", "notes\n") ||
        !host_fs_write_text("/D/nc/files/lower.nc", "G0 X2 Z2\n")) {
        puts("case2test: FAIL cannot write the fixture");
        return 1;
    }
    /* The preset folder a PC wrote: the entry names are capitals there. */
    if (!host_fs_write_text("/D/presets/41.TXT",
                            "OD ROUGH\nG71 U0 R0 X0 Z0 F0 P0 Q0\n")) {
        puts("case2test: FAIL cannot write the preset entry");
        return 1;
    }

    nc2_visual_init();
    nc2_visual_tick(4000u);

    /* 1. the extensions, both ways. */
    if (!nc2_path_is_program("/D/nc/files/UPPER.NC") ||
        !nc2_path_is_program("/D/nc/files/lower.nc") ||
        nc2_path_is_program("/D/nc/files/UPPER.TXT") ||
        !nc2_path_is_text("/D/nc/files/UPPER.TXT")) {
        puts("case2test: FAIL the program and text extensions are not both "
             "cases");
        failures++;
    }

    /* 2. the list offers them, names and all. */
    (void)nc2_file_scan("/D/nc/files");
    for (i = 0; i < nc2_file_count(); i++) {
        const nc2_file_entry_t *e = nc2_file_entry(i);

        if (strcmp(e->name, "UPPER.NC") == 0) {
            saw_upper = true;
        }
        if (strcmp(e->name, "UPPER.TXT") == 0) {
            saw_upper_text = true;
        }
    }
    if (!saw_upper || !saw_upper_text) {
        printf("case2test: FAIL the list shows %d entries and the capital ones "
               "are %s/%s\n", nc2_file_count(),
               saw_upper ? "there" : "missing",
               saw_upper_text ? "there" : "missing");
        failures++;
    }

    /* 3. and one of them opens: a program written on a PC is a program. */
    if (!nc2_visual_open("/D/nc/files/UPPER.NC") ||
        !nc2_path_is_program(nc2_visual_path())) {
        puts("case2test: FAIL a capital .NC does not open as a program");
        failures++;
    }

    /* 4. a card whose entries are capitals is a card in use: the first start
       must not write over it (and its logo must not come back every boot). */
    if (nc2_presets_any()) {
        /* Written after the seed ran, so the answer is the direct question. */
        if (!host_fs_read_text("/D/presets/41.TXT", text, sizeof(text)) ||
            strstr(text, "OD ROUGH") == 0) {
            puts("case2test: FAIL the operator's capital entry was rewritten");
            failures++;
        }
    } else {
        puts("case2test: FAIL a card holding 41.TXT reads as having no entry");
        failures++;
    }

    if (failures) {
        printf("case2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("case2test: PASS a card's own names - capitals and 8.3 - are the "
         "panel's names");
    return 0;
}

/* Walking the card, the way the operator does: into a folder and back out with
   `..`.

   This is where the second machine-only fault hid. `nc2_file_selected_path()`
   used to *scan* the parent itself when the selection was `..` - and return
   without writing the path it was asked for. The caller then scanned whatever
   its uninitialised buffer held, so leaving a folder handed the driver a
   garbage path: on the bench that is the panel frozen mid-walk ("it is stuck").
   The question is a question again, and the check walks both ways around it. */
static int host_walk2test(void)
{
    char path[NC2_PATH_MAX];
    int failures = 0;
    int i;
    bool saw_up;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    /* The driver makes the folders a path needs, but a *new* folder is the
       operator's to make: `mkdir` first, then the file inside it. */
    (void)fs_mkdir("/D/nc/files/deep");
    if (!host_fs_write_text("/D/nc/files/one.nc", "G0 X1 Z1\n") ||
        !host_fs_write_text("/D/nc/files/deep/two.nc", "G0 X2 Z2\n")) {
        puts("walk2test: FAIL cannot write the fixture");
        return 1;
    }
    nc2_visual_init();
    nc2_visual_tick(4000u);

    /* In: the picker walks into a folder with `D`. */
    if (!nc2_file_scan("/D")) {
        puts("walk2test: FAIL the card's root does not list");
        return 1;
    }
    if (!nc2_file_scan("/D/nc/files")) {
        puts("walk2test: FAIL the programs folder does not list");
        return 1;
    }
    if (strcmp(nc2_file_dir(), "/D/nc/files") != 0) {
        printf("walk2test: FAIL the list is at \"%s\"\n", nc2_file_dir());
        return 1;
    }

    /* The `..` entry is there, and it names the folder above. */
    saw_up = false;
    for (i = 0; i < nc2_file_count(); i++) {
        if (strcmp(nc2_file_entry(i)->name, "..") == 0) {
            nc2_file_step(-i);            /* select it */
            saw_up = true;
            break;
        }
    }
    if (!saw_up) {
        puts("walk2test: FAIL the list has no `..`");
        return 1;
    }
    path[0] = '\0';
    if (!nc2_file_selected_path(path, sizeof(path)) ||
        strcmp(path, "/D/nc") != 0) {
        printf("walk2test: FAIL `..` answers \"%s\"\n", path);
        failures++;
    } else if (strcmp(nc2_file_dir(), "/D/nc/files") != 0) {
        /* Asking is not walking: the list must not move under the caller. */
        printf("walk2test: FAIL asking for the parent moved the list to \"%s\"\n",
               nc2_file_dir());
        failures++;
    }

    /* Out: the key handler scans what it was handed, and lands there. The walk
       is the operator's - `0` opens the card, the picker selects `nc`, `D`
       enters it - so this is the screen's own path and not a direct scan. */
    nc2_visual_key('0');
    if (!host_file2_select("nc")) {
        puts("walk2test: FAIL the card's root has no `nc` folder");
        return 1;
    }
    nc2_visual_key('D');
    if (strcmp(nc2_visual_screen_name(), "FILES") != 0 ||
        strcmp(nc2_file_dir(), "/D/nc") != 0) {
        printf("walk2test: FAIL walking up lands at \"%s\" on \"%s\"\n",
               nc2_file_dir(), nc2_visual_screen_name());
        failures++;
    }
    /* and back down, to prove the walk is a walk and not one lucky answer */
    if (!host_file2_select("files")) {
        puts("walk2test: FAIL `nc` has no `files` folder");
        return 1;
    }
    nc2_visual_key('D');
    if (strcmp(nc2_file_dir(), "/D/nc/files") != 0) {
        printf("walk2test: FAIL walking back down lands at \"%s\"\n",
               nc2_file_dir());
        failures++;
    }
    /* and up again, which is the path that used to hand out garbage: `..` is
       the first entry of a folder, so `D` on it is the way up. */
    nc2_visual_key('B');                  /* back to the first entry (`..`) */
    nc2_visual_key('D');
    if (strcmp(nc2_file_dir(), "/D/nc") != 0) {
        printf("walk2test: FAIL the second walk up lands at \"%s\"\n",
               nc2_file_dir());
        failures++;
    }

    if (failures) {
        printf("walk2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("walk2test: PASS the card is walked both ways, and asking for the "
         "folder above does not move the list");
    return 0;
}

/* The legend: what the word the cursor is on means.

   The editor cuts a line into fields at its letters and knows nothing more, so
   the meaning is a table (`nc2_vocab.c`, the one nc kept). It is what the editor
   shows on the row above the cursor, and a word the panel's own entries write
   must not read as "NC word". */
static int host_vocab2test(void)
{
    static const struct {
        const char *line;
        char letter;
        const char *want;
    } cases[] = {
        { "G71 U3 R1 X1 Z1 F500 P50 Q55", 'U', "Depth/pass" },
        { "G71 U3 R1 X1 Z1 F500 P50 Q55", 'R', "Retract" },
        { "G71 U3 R1 X1 Z1 F500 P50 Q55", 'P', "Profile start block" },
        { "G70 P50 Q55", 'Q', "Profile end block" },
        { "G1 X30 Z2", 'X', "X position" },
        { "G1 X30 Z2", 'Z', "Z position" },
        { "G1 W-25", 'W', "Z increment" },
        { "N50 G1 X30 Z2", 'N', "Block number" },
        { "G970 X-5 U60 Z-60 W5", 'U', "Preview max X" },
        { "G971 X50 Z50 I0 E0", 'X', "Stock OD" },
        { "T2 R0.8 O3 F120", 'T', "Tool number" },
        { "M3 S450", 'S', "Spindle speed" },
        { "G76 X0 Z0 P0 Q0 F0 I0 L0 R0", 'L', "Spring passes" }
    };
    int failures = 0;
    unsigned i;

    for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        nc2_field_t fields[NC2_MAX_FIELDS];
        int count = nc2_fields(cases[i].line, fields, NC2_MAX_FIELDS);
        int at;

        for (at = 0; at < count; at++) {
            if (fields[at].letter == cases[i].letter) {
                break;
            }
        }
        if (at == count) {
            printf("vocab2test: FAIL \"%s\" has no %c\n", cases[i].line,
                   cases[i].letter);
            failures++;
            continue;
        }
        {
            const char *label = nc2_vocab_label(cases[i].line, &fields[at]);

            if (!label || strcmp(label, cases[i].want)) {
                printf("vocab2test: FAIL %c of \"%s\" is \"%s\", not \"%s\"\n",
                       cases[i].letter, cases[i].line,
                       label ? label : "(nothing)", cases[i].want);
                failures++;
            }
        }
    }

    if (failures) {
        printf("vocab2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("vocab2test: PASS every word the panel writes is named for what it is");
    return 0;
}

/* nc2's value editor: a line is cut into fields at its letters, the keys walk
   them, and what is typed replaces the value that was there. The dumb editor the
   bench asked for, so the checks are about its two rules - where a field begins
   and ends, and what a keystroke does to it:

     1. a line cuts into one field per letter, comments are not fields, and a
        letter with no number is a field with an empty value (that is how a file
        writes `T` and ` Q` for the operator to complete);
     2. `D` walks the fields; the first digit typed *replaces* the value, later
        digits extend it, and the rest of the line is carried over untouched;
     3. `B` is the sign and `C` the point while a value is picked - the keypad has
        neither key, and this is how it types `-45.2`;
     4. with nothing picked the keys are the screen's: `B`/`C` move the cursor,
        `D`/`#` pick the line's first field, `*` deletes the line;
     5. the pad's helper writes its name as a line under the cursor, the entry's
        rows land where that name stood (a row starting with a space continues the
        row above), the first field of the first row is picked, and leaving the
        pad without a pick takes the name back and returns the cursor. */
static int host_edit2test(void)
{
    nc2_document_t doc;
    nc2_field_t fields[NC2_MAX_FIELDS];
    int count;
    int failures = 0;

    nc2_document_init(&doc);

    /* 1. the fields. */
    count = nc2_fields("N10 G0 X52.5 Z-2", fields, NC2_MAX_FIELDS);
    if (count != 4 || fields[0].letter != 'N' || fields[1].letter != 'G' ||
        fields[2].letter != 'X' || fields[3].letter != 'Z' ||
        fields[2].end - fields[2].value != 4u /* 52.5 */ ||
        fields[3].end - fields[3].value != 2u /* -2 */) {
        printf("edit2test: FAIL the fields of a row are wrong (%d)\n", count);
        failures++;
    }
    count = nc2_fields("(rough) G1 X30 Z-15", fields, NC2_MAX_FIELDS);
    if (count != 3 || fields[0].letter != 'G' || fields[2].letter != 'Z') {
        printf("edit2test: FAIL a comment became a field (%d)\n", count);
        failures++;
    }
    count = nc2_fields("T", fields, NC2_MAX_FIELDS);
    if (count != 1 || fields[0].letter != 'T' ||
        fields[0].value != fields[0].end) {
        puts("edit2test: FAIL a lone letter is not a field waiting for a number");
        failures++;
    }
    count = nc2_fields("G1X30", fields, NC2_MAX_FIELDS);
    if (count != 2 || fields[1].letter != 'X' ||
        fields[1].end - fields[1].value != 2u) {
        puts("edit2test: FAIL letters without spaces do not cut the line");
        failures++;
    }

    /* 2. picking, typing, and the rest of the line standing still. */
    (void)nc2_insert_line(&doc, 0u, "G1 X30 Z-15");
    if (!nc2_key(&doc, NC2_KEY_NEXT, 0) ||
        doc.field != 0 || doc.line_count != 1u) {
        puts("edit2test: FAIL `D` did not pick the line's first field");
        failures++;
    }
    (void)nc2_key(&doc, NC2_KEY_NEXT, 0);               /* to X */
    if (doc.field != 1) {
        puts("edit2test: FAIL `D` did not walk to the next field");
        failures++;
    }
    (void)nc2_key(&doc, NC2_KEY_DIGIT, '4');
    (void)nc2_key(&doc, NC2_KEY_DIGIT, '5');
    if (strcmp(doc.lines[0], "G1 X45 Z-15") != 0) {
        printf("edit2test: FAIL typing gave \"%s\"\n", doc.lines[0]);
        failures++;
    }
    (void)nc2_key(&doc, NC2_KEY_UP, 0);                 /* B: the sign */
    if (strcmp(doc.lines[0], "G1 X-45 Z-15") != 0) {
        printf("edit2test: FAIL the sign gave \"%s\"\n", doc.lines[0]);
        failures++;
    }
    (void)nc2_key(&doc, NC2_KEY_DOWN, 0);               /* C: the point */
    (void)nc2_key(&doc, NC2_KEY_DIGIT, '2');
    if (strcmp(doc.lines[0], "G1 X-45.2 Z-15") != 0) {
        printf("edit2test: FAIL the point gave \"%s\"\n", doc.lines[0]);
        failures++;
    }
    (void)nc2_key(&doc, NC2_KEY_DELETE, 0);             /* backspace a digit */
    if (strcmp(doc.lines[0], "G1 X-45. Z-15") != 0) {
        printf("edit2test: FAIL the backspace gave \"%s\"\n", doc.lines[0]);
        failures++;
    }
    (void)nc2_key(&doc, NC2_KEY_ACCEPT, 0);
    if (doc.field != -1) {
        puts("edit2test: FAIL `#` did not accept the value");
        failures++;
    }

    /* 3. and the fields either side of it were never touched. */
    count = nc2_fields(doc.lines[0], fields, NC2_MAX_FIELDS);
    if (count != 3 || fields[0].end - fields[0].value != 1u ||
        fields[2].end - fields[2].value != 3u /* -15 */) {
        printf("edit2test: FAIL the neighbours changed: \"%s\"\n",
               doc.lines[0]);
        failures++;
    }

    /* 4. nothing picked: the cursor, the line, the way in. */
    (void)nc2_insert_line(&doc, 1u, "G0 X52 Z2");
    {
        (void)nc2_key(&doc, NC2_KEY_DOWN, 0);
        if (doc.cursor != 1u) {
            printf("edit2test: FAIL `C` left the cursor on %u\n",
                   (unsigned)doc.cursor);
            failures++;
        }
        (void)nc2_key(&doc, NC2_KEY_UP, 0);
        if (doc.cursor != 0u) {
            printf("edit2test: FAIL `B` left the cursor on %u\n",
                   (unsigned)doc.cursor);
            failures++;
        }
        if (nc2_key(&doc, NC2_KEY_DIGIT, '4')) {
            puts("edit2test: FAIL a digit was taken with nothing picked "
                 "(it is the pad's)");
            failures++;
        }
    }

    /* 5. the pad. */
    nc2_document_init(&doc);
    (void)nc2_insert_line(&doc, 0u, "G0 X52 Z2");
    doc.cursor = 0u;
    if (!nc2_pad_open(&doc, "G7X") || !nc2_pad_active(&doc) ||
        doc.line_count != 2u || doc.cursor != 1u ||
        strcmp(doc.lines[1], "G7X") != 0) {
        printf("edit2test: FAIL the helper's name is \"%s\"\n",
               doc.line_count > 1u ? doc.lines[1] : "");
        failures++;
    }
    if (!nc2_pad_write(&doc, "G71 U1 R0.5 X0.5 Z0.5 F450 P10 Q20\n"
                                "N10 G1 X30 Z0\n"
                                " C2\n"
                                "N20 G1 X50 Z-15")) {
        puts("edit2test: FAIL the entry was not written");
        failures++;
    }
    if (!nc2_pad_active(&doc) || doc.line_count != 4u ||
        strcmp(doc.lines[0], "G0 X52 Z2") != 0 ||
        strcmp(doc.lines[1], "G71 U1 R0.5 X0.5 Z0.5 F450 P10 Q20") != 0 ||
        strcmp(doc.lines[2], "N10 G1 X30 Z0 C2") != 0 ||
        strcmp(doc.lines[3], "N20 G1 X50 Z-15") != 0) {
        printf("edit2test: FAIL the entry's rows are: \"%s\" \"%s\" \"%s\"\n",
               doc.line_count > 1u ? doc.lines[1] : "",
               doc.line_count > 2u ? doc.lines[2] : "",
               doc.line_count > 3u ? doc.lines[3] : "");
        failures++;
    }
    if (doc.cursor != 1u || doc.field != 0) {
        printf("edit2test: FAIL the cursor is on %u with field %d\n",
               (unsigned)doc.cursor, doc.field);
        failures++;
    }

    /* 6. the pad stays: a second press writes under the first, which is what
       makes a profile walk one press per point. */
    (void)nc2_key(&doc, NC2_KEY_ACCEPT, 0);             /* let the value go */
    if (!nc2_pad_write(&doc, "N30 G1 X60 Z-15") || doc.line_count != 5u ||
        strcmp(doc.lines[4], "N30 G1 X60 Z-15") != 0 || doc.cursor != 4u) {
        printf("edit2test: FAIL the second press wrote \"%s\"\n",
               doc.line_count > 4u ? doc.lines[4] : "");
        failures++;
    }
    nc2_pad_close(&doc);
    if (nc2_pad_active(&doc) || doc.line_count != 5u ||
        strcmp(doc.lines[4], "N30 G1 X60 Z-15") != 0) {
        puts("edit2test: FAIL closing the pad took a written row with it");
        failures++;
    }

    /* 7. and leaving a pad that wrote nothing takes the name line back. */
    nc2_document_init(&doc);
    (void)nc2_insert_line(&doc, 0u, "G0 X52 Z2");
    doc.cursor = 0u;
    (void)nc2_pad_open(&doc, "WORD");
    nc2_pad_close(&doc);
    if (nc2_pad_active(&doc) || doc.line_count != 1u || doc.cursor != 0u ||
        strcmp(doc.lines[0], "G0 X52 Z2") != 0) {
        puts("edit2test: FAIL a pad that wrote nothing left its name behind");
        failures++;
    }

    if (failures) {
        printf("edit2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("edit2test: PASS the fields walk, the value is typed over, and the "
         "pad writes one entry per press");
    return 0;
}

/* nc2's first start: a card that has never seen an entry gets the ones the panel
   ships, written once, and the logo stands while it happens. The entries live in
   `nc2/default.c`'s table and nowhere else, and after this has run they are files
   like every other - so the checks are about the card:

     1. an empty folder gets every shipped entry, with the format the reader
        reads (name row, then the rows, a continuing row keeping its space);
     2. the two rows that only exist to be written - the group's name and the
        entry with one blank line - come out as they must;
     3. what was written reads back as what was written;
     4. a folder that holds an entry of its own is never touched, and a file that
        is there is never replaced;
     5. an address whose file is deleted is *not* an entry - there is no table
        behind the files to fall back on - and the seed does not put it back;
     6. the logo names what happened and goes away on its own. */
static int host_seedtest(void)
{
    char name[64];
    char rows[NC2_PRESET_ROW_MAX * 4];
    char text[128];
    fs_file_t *dir;
    fs_file_info_t info;
    int files = 0;
    int failures = 0;
    const uint32_t *px;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();

    /* 1. the first start. `host_init_core()` brings the screen up the way the
       machine does, and the screen is what seeds the card - so the seed has
       already run here, and what is checked is what it left behind. */
    if (!nc2_boot_active()) {
        puts("seedtest: FAIL an empty card was not seeded (no logo)");
        return 1;
    }
    /* It is the screen for a moment, says what happened, and leaves on its own -
       and the seconds it stands are the seed's, not the screen's, so a station
       that had nothing to write never shows it at all. */
    nc2_boot_draw();
    px = (const uint32_t *)lvds_host_pixels();
    if (!host_ink_in(px, 300, 240, 200, 120)) {
        puts("seedtest: FAIL the logo drew nothing");
        failures++;
    }
    nc2_boot_tick(600u);
    if (!nc2_boot_active()) {
        puts("seedtest: FAIL the logo left before its time");
        failures++;
    }
    nc2_boot_tick(600u);
    if (nc2_boot_active()) {
        puts("seedtest: FAIL the logo stayed up");
        failures++;
    }
    dir = fs_opendir(NC2_PRESET_ROOT);
    if (!dir) {
        puts("seedtest: FAIL the presets folder is not there");
        return 1;
    }
    while (fs_next_file(dir, &info)) {
        if (!info.is_dir) {
            files++;
        }
    }
    fs_close(dir);
    if (files != 27) {
        printf("seedtest: FAIL %d files were written, wanted 27\n", files);
        failures++;
    } else {
        puts("seedtest: the empty card got the shipped entries");
    }

    /* 2. the two shapes that are easy to get wrong: a slot that only holds
       children is its name alone, and an entry with one blank row is a name and
       an empty line - which is what "a new line" is. */
    if (!host_fs_read_text("/D/presets/1.txt", text, sizeof(text)) ||
        strcmp(text, "OPS\n") != 0) {
        printf("seedtest: FAIL the group file reads \"%s\"\n", text);
        failures++;
    }
    if (!host_fs_read_text("/D/presets/11.txt", text, sizeof(text)) ||
        strcmp(text, "INS\n\n") != 0) {
        printf("seedtest: FAIL the blank-line entry reads \"%s\"\n", text);
        failures++;
    }

    /* 3. the round trip: what the file says is what a key will write. */
    if (nc2_preset_read("34", name, sizeof(name), rows, sizeof(rows)) != 1 ||
        strcmp(name, "U INC") != 0 || strcmp(rows, " U") != 0) {
        printf("seedtest: FAIL U INC reads \"%s\" / \"%s\"\n", name, rows);
        failures++;
    }
    if (nc2_preset_read("16", name, sizeof(name), rows, sizeof(rows)) != 4 ||
        strcmp(name, "SETUP") != 0 ||
        strcmp(rows, "G970 X0 U0 Z0 W0\nG971 X0 Z0 I0 E0\nG972 C0\nG973 P0") != 0) {
        printf("seedtest: FAIL the setup entry reads \"%s\"\n", rows);
        failures++;
    }
    if (nc2_preset_read("34", name, sizeof(name), rows, sizeof(rows)) >= 0 &&
        !nc2_preset_exists("34")) {
        puts("seedtest: FAIL a read answered for a file that is not there");
        failures++;
    }

    /* 4. the operator's folder, and their files. */
    if (nc2_preset_write("34", "MINE", " U9") != 0) {
        puts("seedtest: FAIL a file that is there was written over");
        failures++;
    }
    if (!host_fs_read_text("/D/presets/34.txt", text, sizeof(text)) ||
        strcmp(text, "U INC\n U\n") != 0) {
        printf("seedtest: FAIL the existing file changed to \"%s\"\n", text);
        failures++;
    }
    if (nc2_boot_seed()) {
        puts("seedtest: FAIL a folder with entries was seeded again");
        failures++;
    }

    /* 5. a deleted file means the address is not an entry - and it stays that
       way, because the seed only ever fills a folder with nothing in it. */
    if (!fs_remove("/D/presets/34.txt") || nc2_preset_exists("34") ||
        nc2_preset_read("34", name, sizeof(name), rows, sizeof(rows)) >= 0) {
        puts("seedtest: FAIL a deleted entry still answers");
        failures++;
    }
    if (nc2_boot_seed()) {
        puts("seedtest: FAIL the seed put a deleted file back");
        failures++;
    } else {
        puts("seedtest: deleting an entry is how an address stops being one");
    }

    if (failures) {
        printf("seedtest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("seedtest: PASS a card with no entries gets them once, and the logo "
         "says so");
    return 0;
}



/* The expansion, declared here because the pad's check reads it: the
   definitions sit with the other emit helpers below. */
#define HOST_EMIT_MAX 400












/* The frame the host backend last drew, as 0x00RRGGBB. Reading the glass is
   the point of the check below: a mark the snapshot carries but the pane never
   paints would pass a check on the snapshot and still not be there. */
static uint32_t host_frame_at(int x, int y)
{
    const uint32_t *px = (const uint32_t *)lvds_host_pixels();
    int stride;

    if (!px || x < 0 || y < 0 || x >= lvds_host_width() || y >= lvds_host_height()) {
        return 0u;
    }
    stride = lvds_host_stride() / 4;
    return px[(size_t)y * (size_t)stride + (size_t)x] & 0xFFFFFFu;
}

/* A panel colour as the frame holds it: the renderer keeps 16 bits per colour
   and the backend expands them back to eight per channel, so what is drawn is
   the quantised value, not the hex the palette was written with. */
static uint32_t host_panel_rgb(lvds_color_t color)
{
    uint32_t r = (uint32_t)((color >> 11) & 0x1Fu);
    uint32_t g = (uint32_t)((color >> 5) & 0x3Fu);
    uint32_t b = (uint32_t)(color & 0x1Fu);

    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}



















/* Headless check that the desktop filesystem is mounted where the NC module
   expects it: list "/D" through the same fs_* API the file manager uses. */
static int host_fstest(void)
{
    fs_file_t *dir;
    fs_file_info_t info;
    unsigned count = 0u;

    cnc_init();
    cnc_unit_test_start();
    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    printf("nc_ui: fs root %s\n", g_files_root[0] ? g_files_root : "nc-files");
    dir = fs_opendir("/D");
    if (!dir) {
        puts("nc_ui: /D is not mounted");
        return 1;
    }
    while (fs_next_file(dir, &info)) {
        printf("  %-4s %8lu  %s\n", info.is_dir ? "dir" : "file",
               (unsigned long)info.size, info.full_name);
        count++;
    }
    fs_close(dir);
    fflush(stdout);
    printf("nc_ui: %u entries\n", count);
    return 0;
}

/* nc2's own screen as a .bmp - the panel alone, like `--dump` for nc's - so the
   new layout can be looked at without the station's window. */
static int host_dump_nc2(const char *path)
{
    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    nc2_visual_init();
    nc2_visual_tick(4000u);             /* past the first start's logo */
    if (!nc2_visual_open("/D/nc/files/lathe-demo.nc")) {
        (void)nc2_visual_open("/D/nc/files/screen.nc");
    }
    /* `--keys` presses nc2's own keys, one character at a time, so a frame of a
       screen that only appears after input can be looked at. */
    if (g_key_script[0]) {
        const char *p = g_key_script;

        while (*p) {
            while (*p == ',' || *p == ' ') {
                p++;
            }
            if (*p) {
                nc2_visual_key(*p);
            }
            while (*p && *p != ',') {
                p++;
            }
        }
    }
    nc2_visual_draw();
    if (!lvds_host_save_bmp(path)) {
        fprintf(stderr, "nc_ui: cannot write %s\n", path);
        return 1;
    }
    printf("nc_ui: wrote nc2's %dx%d screen to %s\n", lvds_host_width(),
           lvds_host_height(), path);
    return 0;
}

/* Write the compiled entries out as the card's own files: one per address,
   named after it, in the format `/D/presets` reads. This is where
   `tools/nc_ui_win/examples/presets` - what a fresh station's card is seeded
   from - comes from, so the words a key writes are visible and editable as
   files, and because they are generated from the compiled table and
   `tools/test_nc_ui.py` regenerates and compares them, the two cannot drift. */
static int host_dump_presets(const char *dir)
{
    char path[260];
    char text[1024];
    size_t i;
    int written = 0;

    if (!dir || !*dir) {
        fprintf(stderr, "nc_ui: --dump-presets needs a folder\n");
        return 1;
    }
    if (!CreateDirectoryA(dir, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "nc_ui: cannot make %s\n", dir);
        return 1;
    }
    for (i = 0u; i < nc2_boot_count(); i++) {
        const char *address = 0;
        const char *name = 0;
        const char *rows = 0;
        FILE *fp;
        size_t len;

        if (!nc2_boot_entry(i, &address, &name, &rows) || !address) {
            continue;
        }
        /* The file the panel writes for that address: the name, then every row
           under it - byte for byte what `nc2_preset_write()` writes, so the
           shipped files and the module's own first start cannot drift. */
        len = (size_t)snprintf(text, sizeof(text), "%s\n", name ? name : "");
        {
            const char *row = rows;

            while (row && len + 2u < sizeof(text)) {
                const char *end = strchr(row, '\n');
                size_t row_len = end ? (size_t)(end - row) : strlen(row);

                if (row_len > sizeof(text) - len - 2u) {
                    row_len = sizeof(text) - len - 2u;
                }
                memcpy(text + len, row, row_len);
                len += row_len;
                text[len++] = '\n';
                row = end ? end + 1 : 0;
            }
        }
        text[len] = '\0';
        snprintf(path, sizeof(path), "%s\\%s.txt", dir, address);
        fp = fopen(path, "wb");
        if (!fp) {
            fprintf(stderr, "nc_ui: cannot write %s\n", path);
            return 1;
        }
        if (fwrite(text, 1u, len, fp) != len) {
            fclose(fp);
            fprintf(stderr, "nc_ui: short write on %s\n", path);
            return 1;
        }
        fclose(fp);
        written++;
    }
    printf("nc_ui: wrote %d preset files to %s\n", written, dir);
    return 0;
}

/* The flags this file answers, in the order they are tried: the dumps
   first (they take a path), then one check per flag. -1 means no flag named a
   check, so main() opens the window. */
int host_tests_run(int argc, char **argv)
{
    int i;

    for (i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0)
            return host_dump(argv[i + 1]);
        if (strcmp(argv[i], "--dump-bench") == 0)
            return host_dump_bench(argv[i + 1]);
        if (strcmp(argv[i], "--dump-presets") == 0)
            return host_dump_presets(argv[i + 1]);
        if (strcmp(argv[i], "--dump-nc2") == 0)
            return host_dump_nc2(argv[i + 1]);
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fstest") == 0)
            return host_fstest();
        if (strcmp(argv[i], "--streamtest") == 0)
            return host_streamtest();
        if (strcmp(argv[i], "--painttest") == 0)
            return host_painttest();
        if (strcmp(argv[i], "--version") == 0)
            return host_version();
        if (strcmp(argv[i], "--keytest") == 0)
            return host_keytest();
        if (strcmp(argv[i], "--seedtest") == 0)
            return host_seedtest();
        if (strcmp(argv[i], "--present2test") == 0)
            return host_present2test();
        if (strcmp(argv[i], "--pump2test") == 0)
            return host_pump2test();
        if (strcmp(argv[i], "--edit2test") == 0)
            return host_edit2test();
        if (strcmp(argv[i], "--pad2test") == 0)
            return host_pad2test();
        if (strcmp(argv[i], "--screen2test") == 0)
            return host_screen2test();
        if (strcmp(argv[i], "--file2test") == 0)
            return host_file2test();
        if (strcmp(argv[i], "--emit2test") == 0)
            return host_emit2test();
        if (strcmp(argv[i], "--run2test") == 0)
            return host_run2test();
        if (strcmp(argv[i], "--live2test") == 0)
            return host_live2test();
        if (strcmp(argv[i], "--manual2test") == 0)
            return host_manual2test();
        if (strcmp(argv[i], "--tools2test") == 0)
            return host_tools2test();
        if (strcmp(argv[i], "--contour2test") == 0)
            return host_contour2test();
        if (strcmp(argv[i], "--case2test") == 0)
            return host_case2test();
        if (strcmp(argv[i], "--walk2test") == 0)
            return host_walk2test();
        if (strcmp(argv[i], "--vocab2test") == 0)
            return host_vocab2test();
        if (strcmp(argv[i], "--block2test") == 0)
            return host_block2test();
        if (strcmp(argv[i], "--label2test") == 0)
            return host_label2test();
        if (strcmp(argv[i], "--pace2test") == 0)
            return host_pace2test();
        if (strcmp(argv[i], "--demo2test") == 0)
            return host_demo2test();
    }
    return -1;
}
