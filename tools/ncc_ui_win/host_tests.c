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
#include "ncc.h"
#include "ncc_boot.h"
#include "ncc_boot_image.h"
#include "ncc_draw.h"
#include "nc_sender.h"
#include "ncc_files.h"
#include "ncc_layout.h"
#include "ncc_manual.h"
#include "ncc_preview.h"
#include "ncc_presets.h"
#include "ncc_state.h"
#include "ncc_tools.h"
#include "ncc_visual.h"
#include "ncc_vocab.h"
#include "lvds_palette.h"
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
static uint32_t host_view_at(int x, int y);
static int host_view_ink_in(int x, int y, int w, int h);
static uint32_t host_pane2_row_bg(int row);

static bool host_nc_sender_tool_radius(void *user, int tool, float *radius)
{
    ncc_tool_t row;

    (void)user;
    if (!radius || tool <= 0 || !ncc_tools_number(tool, &row) ||
        row.r <= 0.0f) {
        return false;
    }
    *radius = row.r;
    return true;
}

static g7x_doc_t *host_sender_source(const ncc_document_t *doc)
{
    static g7x_doc_t source;

    source = ncc_document_g7x(doc);
    return &source;
}



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
       into it first, the way a tick does (`ncc_visual_draw()` then the repaint). */
    ncc_visual_select_mode(NCC_MODE_PROGRAM);
    ncc_visual_draw();
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
    ncc_visual_select_mode(NCC_MODE_RUN);
    ncc_visual_draw();
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
    printf("ncc_ui: uCNC programming station (PC), built %s\n", build);
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
    if (!nc_sender_run_send_line("G91 G1 X0.100 F500") ||
        !nc_sender_run_send_line("G90")) {
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
    ncc_visual_key(key);
}

/* Is anything in the region different from its own background (the pixel in its
   top-left corner)? The region is the *picture's*, not the glass's: the panel is
   mounted turned, and a check that asks about a screen coordinate asks it in the
   picture the screen drew. */
static int host_view_ink_in(int x, int y, int w, int h)
{
    uint32_t bg = host_view_at(x, y);
    int r;
    int c;

    for (r = 0; r < h; r++) {
        for (c = 0; c < w; c++) {
            if (host_view_at(x + c, y + r) != bg) {
                return 1;
            }
        }
    }
    return 0;
}

/* Is the tool drawn in the drawing pane? Its two colours are its own: nothing
   else in the preview wears the tool's orange. */
/* Is there ink on the machine's strip - a pixel that is not the strip's own
   colour - in the last 120 columns of it? That is where the state word is
   drawn. */
static int host_strip_has_ink(int x, uint32_t strip_rgb)
{
    int row;
    int col;

    for (row = 0; row < NCC_DRO_H; row++) {
        for (col = 0; col < 120; col++) {
            if (host_view_at(x + col, NCC_DRO_Y + row) != strip_rgb) {
                return 1;
            }
        }
    }
    return 0;
}

/* The raw buffer, for the few readers that work in the glass's own order. */
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

    for (i = 0; i < ncc_file_count(); i++) {
        const ncc_file_entry_t *e = ncc_file_entry(i);

        if (strcmp(e->name, name) != 0) {
            continue;
        }
        while (ncc_file_selected() > i) {
            ncc_file_step(-1);
        }
        while (ncc_file_selected() < i) {
            ncc_file_step(1);
        }
        return true;
    }
    return false;
}

/* ncc's file list: `0` opens the card, and the picker has to do the things an
   operator needs from it - walk, open, make one, delete one, refresh - on the
   folders and the text files the card actually holds, with no pad in the way
   (the digits name a new file there).

     1. `0` opens the folder the *open file* lives in, on that file: the folder
        the operator is working in, not the card's root, and not "the first
        thing that sorts" (bench: "it was better approach in nc - same folder if
        0 pressed not root");
     2. `C` steps and `D` opens: a folder is entered, a program is loaded into the
        editor and the list closes;
     3. `5` then digits then `#` makes a numbered program in the folder being
        listed and opens it;
     4. `6` deletes the selected file, and `8` reads the folder again *without
        moving the pick* (bench: "on refresh button pressed - it select next
        file");
     5. and the screen is part of what the card remembers: the panel comes back
        to the screen the operator left (bench: "state was better transferred in
        nc. now it always starts in edit"). */
static int host_file2test(void)
{
    static const char *const program = "/D/nc/files/one.nc";
    static const char *const text_file = "/D/nc/files/notes.txt";
    char created[NCC_PATH_MAX];
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
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    (void)ncc_visual_open("/D/nc/files/one.nc");

    /* 1. the folder the open file lives in, with the file picked. */
    ncc_visual_key('0');
    if (strcmp(ncc_visual_screen_name(), "FILES") != 0 ||
        strcmp(ncc_file_dir(), "/D/nc/files") != 0) {
        printf("file2test: FAIL `0` is on \"%s\" at \"%s\"\n",
               ncc_visual_screen_name(), ncc_file_dir());
        return 1;
    }
    count = ncc_file_count();
    {
        bool saw_up = false;
        bool saw_file = false;

        for (i = 0; i < count; i++) {
            const ncc_file_entry_t *e = ncc_file_entry(i);

            if (strcmp(e->name, "..") == 0) {
                saw_up = true;
            }
            if (!e->is_dir && strcmp(e->name, "one.nc") == 0) {
                saw_file = true;
            }
        }
        if (!saw_up || !saw_file || count < 2) {
            printf("file2test: FAIL `0` listed %d entries (up %d, file %d)\n",
                   count, saw_up, saw_file);
            failures++;
        }
        if (ncc_file_selected() < 0 ||
            strcmp(ncc_file_entry(ncc_file_selected())->name, "one.nc") != 0) {
            printf("file2test: FAIL `0` did not land on the open file but on "
                   "\"%s\"\n", ncc_file_selected() >= 0
                                    ? ncc_file_entry(ncc_file_selected())->name
                                    : "-");
            failures++;
        }
    }

    /* 2. a folder is entered, and a program opens into the editor. */
    if (!host_file2_select("..")) {
        puts("file2test: FAIL the list has no way up");
        return 1;
    }
    ncc_visual_key('D');
    if (strcmp(ncc_file_dir(), "/D/nc") != 0 ||
        !host_file2_select("files")) {
        printf("file2test: FAIL going up gave \"%s\"\n", ncc_file_dir());
        return 1;
    }
    ncc_visual_key('D');
    if (strcmp(ncc_file_dir(), "/D/nc/files") != 0 ||
        ncc_file_count() < 2) {
        printf("file2test: FAIL entering `nc/files` gave \"%s\" with %d entries\n",
               ncc_file_dir(), ncc_file_count());
        failures++;
    }

    /* 3. and a program opens into the editor. */
    if (!host_file2_select("one.nc")) {
        puts("file2test: FAIL the program is not in its folder");
        return 1;
    }
    ncc_visual_key('D');
    if (strcmp(ncc_visual_screen_name(), "EDIT") != 0 ||
        strcmp(ncc_visual_path(), program) != 0) {
        printf("file2test: FAIL opening gave \"%s\" on \"%s\"\n",
               ncc_visual_screen_name(), ncc_visual_path());
        failures++;
    }

    /* 4. a new one, named with the pad's digits. */
    ncc_visual_key('0');
    ncc_visual_key('5');
    ncc_visual_key('4');
    ncc_visual_key('2');
    ncc_visual_key('#');
    snprintf(created, sizeof(created), "%s", ncc_visual_path());
    if (strcmp(created, "/D/nc/files/42.nc") != 0 ||
        strcmp(ncc_visual_screen_name(), "EDIT") != 0 ||
        !host_fs_read_text("/D/nc/files/42.nc", created, sizeof(created))) {
        printf("file2test: FAIL the new file is \"%s\"\n", created);
        failures++;
    }

    /* 5. delete one: the notes, picked by name - and a refresh must not take
       the pick off it. */
    ncc_visual_key('0');
    if (!host_file2_select("notes.txt")) {
        puts("file2test: FAIL the notes file is not in the folder");
        failures++;
    } else {
        bool gone = false;

        ncc_visual_key('8');            /* refresh: the pick stays put */
        if (ncc_file_selected() < 0 ||
            strcmp(ncc_file_entry(ncc_file_selected())->name, "notes.txt") != 0) {
            printf("file2test: FAIL refresh moved the pick to \"%s\"\n",
                   ncc_file_selected() >= 0
                       ? ncc_file_entry(ncc_file_selected())->name
                       : "-");
            failures++;
        }
        ncc_visual_key('6');
        count = ncc_file_count();
        ncc_visual_key('8');
        for (i = 0; i < ncc_file_count(); i++) {
            const ncc_file_entry_t *e = ncc_file_entry(i);

            if (!e->is_dir && strcmp(e->name, "notes.txt") == 0) {
                gone = false;
                break;
            }
            gone = true;
        }
        if (!gone || ncc_file_count() > count) {
            puts("file2test: FAIL the deleted file is still listed");
            failures++;
        }
    }

    /* And the list draws. */
    ncc_visual_draw();
    if (!host_view_ink_in(NCC_TEXT_X + 4, NCC_TEXT_Y + 30, 300, 200)) {
        puts("file2test: FAIL the list drew nothing");
        failures++;
    }

    /* 6. the screen the operator leaves is the screen the card gives back: the
       state is written when the screen changes, not only when a file is opened
       or a document is dirty (bench: "state was better transferred in nc. now
       it always starts in edit"). A fresh boot on the same card is the panel
       being initialised again. */
    ncc_visual_select_mode(NCC_MODE_MANUAL);
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    if (ncc_visual_mode() != NCC_MODE_MANUAL) {
        printf("file2test: FAIL the panel came back on screen %d, not the one "
               "it was left on\n", (int)ncc_visual_mode());
        failures++;
    }

    if (failures) {
        printf("file2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("file2test: PASS the list opens on the file's own folder, walks, "
         "opens, makes, deletes and refreshes without moving the pick, and the "
         "screen comes back where it was left");
    return 0;
}

/* ncc's screen: the program down the left, the pad's corner on the right, no
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
    ncc_document_t nc_doc;
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
    ncc_visual_init();
    /* There is no first start to wait for here: the entries are in RAM, so the
       screen is ready the moment it comes up (`ncc` has no boot logo). */
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    if (!ncc_visual_open(program)) {
        puts("screen2test: FAIL ncc did not open the fixture");
        return 1;
    }

    /* 1. it draws: the program in the bottom band, the part in the drawing
       above it, the keys in the corner. */
    ncc_visual_draw();
    if (!host_view_ink_in(NCC_TEXT_X, NCC_TEXT_Y, NCC_TEXT_W, 240)) {
        puts("screen2test: FAIL the code pane drew nothing");
        failures++;
    }
    if (!host_view_ink_in(NCC_PAD_X, NCC_PAD_Y, NCC_PAD_W, NCC_PAD_H)) {
        puts("screen2test: FAIL the pad's corner drew nothing");
        failures++;
    }
    /* The drawing pane has the part in it - the stock, the chuck and the DIN
       rulers - and none of that is the pad. */
    if (!host_view_ink_in(NCC_PREVIEW_X + 10, NCC_PREVIEW_Y + 60,
                          NCC_PREVIEW_W - 20, 200)) {
        puts("screen2test: FAIL the drawing pane drew nothing");
        failures++;
    }
    /* The split sits above the screen's middle by the bench's own 25 pixels
       ("move tis all ~ 25 px to the top, its have plenty of space") and not
       where the top of the bottom band would put it, and the notes above the 3x3
       carry what the screen's keys do. */
    if (NCC_DRO_Y + NCC_DRO_H / 2 != LVDS_VIEW_HEIGHT / 2 - NCC_SPLIT_RISE) {
        printf("screen2test: FAIL the strip sits at %d, not %d\n",
               NCC_DRO_Y + NCC_DRO_H / 2,
               LVDS_VIEW_HEIGHT / 2 - NCC_SPLIT_RISE);
        failures++;
    }
    if (!host_view_ink_in(NCC_NOTES_X + 4, NCC_NOTES_Y + 4, NCC_NOTES_W - 8,
                          NCC_NOTES_H - 8)) {
        puts("screen2test: FAIL the space above the pad is empty");
        failures++;
    }

    /* 2. into G7X. */
    ncc_visual_key('4');
    if (strcmp(ncc_visual_address(), "4") != 0 ||
        !ncc_visual_slot_label('1') ||
        strcmp(ncc_visual_slot_label('1'), "G71") != 0) {
        printf("screen2test: FAIL the pad at \"%s\" shows \"%s\"\n",
               ncc_visual_address(),
               ncc_visual_slot_label('1') ? ncc_visual_slot_label('1') : "");
        failures++;
    }
    ncc_visual_draw();
    if (!host_view_ink_in(NCC_TEXT_X, NCC_TEXT_Y, NCC_TEXT_W, 240)) {
        puts("screen2test: FAIL the program does not show the pad's name");
        failures++;
    }
    /* The one line the layout has is the strip between the drawing and the
       keys: it is the machine's own colour, so it is told apart from the page
       on either side of it. */
    if (host_view_at(NCC_DRO_X + 2, NCC_DRO_Y + NCC_DRO_H / 2) !=
        host_panel_rgb(ncc_col_header())) {
        puts("screen2test: FAIL the strip between the halves is not drawn");
        failures++;
    }

    /* 3. the entry lands where the pad's name stood and the press ends the pad,
       the way nc's menu did - the keys are back at the root, so the next digit
       opens a group and not the next slot of the one that just wrote. */
    ncc_visual_key('1');
    if (strcmp(ncc_visual_address(), "") != 0 ||
        !ncc_visual_slot_label('1') ||
        strcmp(ncc_visual_slot_label('1'), "OPS") != 0) {
        printf("screen2test: FAIL the entry left the pad at \"%s\" showing "
               "\"%s\"\n", ncc_visual_address(),
               ncc_visual_slot_label('1') ? ncc_visual_slot_label('1') : "");
        failures++;
    }

    /* 4. the value the entry landed with is picked: a digit replaces it, and
       `#` accepts it - a picked value is the one state where `#` is still the
       editor's and not the preview's. The word picked is the first *value* of
       the row, not the `G71` code it starts with. */
    ncc_visual_key('9');
    ncc_visual_key('#');

    /* 5. and `0` is the exit everywhere: with a pad up it leaves the pad in one
       press, whatever level it is on, instead of opening the card. */
    ncc_visual_key('1');                    /* the OPS pad, one level in */
    if (strcmp(ncc_visual_address(), "1") != 0) {
        printf("screen2test: FAIL the OPS pad left the address at \"%s\"\n",
               ncc_visual_address());
        failures++;
    }
    ncc_visual_key('0');
    if (strcmp(ncc_visual_address(), "") != 0 ||
        strcmp(ncc_visual_screen_name(), "EDIT") != 0) {
        printf("screen2test: FAIL `0` left \"%s\" on \"%s\"\n",
               ncc_visual_address(), ncc_visual_screen_name());
        failures++;
    }

    /* 6. `A` is the step back - in again, then one level out. */
    ncc_visual_key('4');
    if (strcmp(ncc_visual_address(), "4") != 0) {
        puts("screen2test: FAIL the pad did not open again");
        failures++;
    }
    ncc_visual_key('A');
    if (strcmp(ncc_visual_address(), "") != 0) {
        printf("screen2test: FAIL `A` left the address at \"%s\"\n",
               ncc_visual_address());
        failures++;
    }
    ncc_document_init(&nc_doc);
    {
        static char line[NCC_MAX_LINE_LEN];
        bool saw_typed = false;
        bool saw_name = false;
        size_t i;

        /* The screen's own document, read back through ncc's loader: the file
           it saves is the program. */
        if (!ncc_visual_save()) {
            puts("screen2test: FAIL ncc did not save the program");
            failures++;
        } else if (!ncc_file_load(&nc_doc, program)) {
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
    char address[NCC_ADDR_MAX + 1];
    char label[NCC_PRESET_ROW_MAX];
    char rows[NCC_PRESET_ROW_MAX * 3];
    ncc_document_t doc;
    int kind;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    /* The entries are in RAM from the moment the module comes up - there is no
       card to seed and nothing to wait for. */
    if (ncc_preset_read("41", 0, 0u, rows, sizeof(rows)) <= 0) {
        puts("pad2test: FAIL the shipped entries are not in RAM");
        return 1;
    }

    /* 1. the root. */
    ncc_address_reset(address);
    kind = ncc_slot(address, '1', label, sizeof(label));
    if (kind != NCC_SLOT_PAD || strcmp(label, "OPS") != 0) {
        printf("pad2test: FAIL the root's `1` is kind %d, \"%s\"\n", kind, label);
        failures++;
    }
    kind = ncc_slot(address, '4', label, sizeof(label));
    if (kind != NCC_SLOT_PAD || strcmp(label, "G7X") != 0) {
        printf("pad2test: FAIL the root's `4` is kind %d, \"%s\"\n", kind, label);
        failures++;
    }
    /* `8` and `9` at the root are the shipped set's own empty slots (`7` is the
       tool table's group, which the TOOLS screen's pad offers). */
    if (ncc_slot(address, '8', label, sizeof(label)) != NCC_SLOT_EMPTY) {
        puts("pad2test: FAIL the root's `8` is not empty");
        failures++;
    }

    /* 2. into G7X, where the slots are entries, not pads. */
    if (!ncc_address_push(address, '4') || strcmp(address, "4") != 0) {
        puts("pad2test: FAIL the address did not grow");
        failures++;
    }
    kind = ncc_slot(address, '1', label, sizeof(label));
    if (kind != NCC_SLOT_ENTRY || strcmp(label, "G71") != 0) {
        printf("pad2test: FAIL G7X `1` is kind %d, \"%s\"\n", kind, label);
        failures++;
    }
    /* `44 CHMF` and `46 RND`: the corner words belong to a profile, and this is
       the group a profile lives in (bench, 2026-09-28). */
    kind = ncc_slot(address, '4', label, sizeof(label));
    if (kind != NCC_SLOT_ENTRY || strcmp(label, "CHMF") != 0) {
        printf("pad2test: FAIL G7X `4` is kind %d, \"%s\"\n", kind, label);
        failures++;
    }
    kind = ncc_slot(address, '6', label, sizeof(label));
    if (kind != NCC_SLOT_ENTRY || strcmp(label, "RND") != 0) {
        printf("pad2test: FAIL G7X `6` is kind %d, \"%s\"\n", kind, label);
        failures++;
    }
    if (ncc_slot(address, '7', label, sizeof(label)) != NCC_SLOT_EMPTY) {
        puts("pad2test: FAIL `7` under G7X is not empty (nothing ships there)");
        failures++;
    }

    /* 3. pressing it: the entry lands where the pad's name stood, the press ends
       the pad (nc's menu closed on every entry), and opening it again writes
       under the row that landed. */
    ncc_document_init(&doc);
    (void)ncc_insert_line(&doc, 0u, "G0 X52 Z2");
    doc.cursor = 0u;
    if (!ncc_pad_open(&doc, label)) {
        puts("pad2test: FAIL the pad did not open on the slot's name");
        return 1;
    }
    if (ncc_preset_read("41", 0, 0u, rows, sizeof(rows)) <= 0 ||
        !ncc_pad_write(&doc, rows)) {
        puts("pad2test: FAIL the entry's rows were not read and written");
        failures++;
    }
    if (doc.line_count != 2u ||
        strcmp(doc.lines[1], "G71 U0 R0 X0 Z0 F0 P0 Q0") != 0 ||
        doc.cursor != 1u || doc.field != 1 || ncc_pad_active(&doc)) {
        printf("pad2test: FAIL the entry reads \"%s\"\n",
               doc.line_count > 1u ? doc.lines[1] : "");
        failures++;
    }
    (void)ncc_key(&doc, NCC_KEY_ACCEPT, 0);
    if (!ncc_pad_open(&doc, label)) {
        puts("pad2test: FAIL the pad did not open again");
        failures++;
    }
    (void)ncc_pad_write(&doc, "G1 X30 Z0");
    if (doc.line_count != 3u || strcmp(doc.lines[2], "G1 X30 Z0") != 0) {
        printf("pad2test: FAIL the second press wrote \"%s\"\n",
               doc.line_count > 2u ? doc.lines[2] : "");
        failures++;
    }
    ncc_pad_close(&doc);
    if (doc.line_count != 3u) {
        puts("pad2test: FAIL closing the pad took a written row");
        failures++;
    }

    /* 4. back up, and the same digit means something else. */
    ncc_address_pop(address);
    if (strcmp(address, "") != 0) {
        puts("pad2test: FAIL the address did not shrink");
        failures++;
    }
    kind = ncc_slot(address, '4', label, sizeof(label));
    if (kind != NCC_SLOT_PAD) {
        puts("pad2test: FAIL the root's `4` is not a pad again");
        failures++;
    }

    /* 5. and the file tree is the whole story: an address with no file is not
       there, and neither is a level below the pad's depth. */
    if (!ncc_address_push(address, '8') || !ncc_address_push(address, '1') ||
        !ncc_address_push(address, '1') || ncc_address_push(address, '1')) {
        puts("pad2test: FAIL the address grew past its depth");
        failures++;
    }
    ncc_address_reset(address);
    if (ncc_slot(address, '8', label, sizeof(label)) != NCC_SLOT_EMPTY ||
        ncc_slot(address, '9', label, sizeof(label)) != NCC_SLOT_EMPTY ||
        ncc_slot(address, '0', label, sizeof(label)) != NCC_SLOT_EMPTY) {
        puts("pad2test: FAIL a slot nobody wrote answers");
        failures++;
    }

    /* 6. the tool table's group is the TOOLS screen's own pad root, so the root
       of the other screens does not offer it a second time (bench: "push 7
       table to tools? because it seems to be the same"). */
    /* The screen's own pad, on the screen's own screens (the checks above used
       the tree directly). */
    ncc_visual_select_mode(NCC_MODE_PROGRAM);
    if (ncc_visual_slot_label('7') && ncc_visual_slot_label('7')[0]) {
        printf("pad2test: FAIL EDIT's root still offers the table as \"%s\"\n",
               ncc_visual_slot_label('7'));
        failures++;
    }
    ncc_visual_select_mode(NCC_MODE_TOOLS);
    if (!ncc_visual_slot_label('1') ||
        strcmp(ncc_visual_slot_label('1'), "NEW") != 0) {
        printf("pad2test: FAIL TOOLS' pad reads \"%s\", not the table\n",
               ncc_visual_slot_label('1') ? ncc_visual_slot_label('1') : "");
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



/* The same for ncc's. */
static bool host_emit_everything_ncc(const ncc_document_t *doc,
                                     size_t start,
                                     char *out,
                                     size_t out_sz)
{
    nc_sender_emit_stream_t stream;
    g7x_doc_t source;
    size_t line = 0u;
    size_t used = 0u;

    out[0] = '\0';
    source = ncc_document_g7x(doc);
    nc_sender_emit_stream_begin(&stream, &source, start,
                                host_nc_sender_tool_radius, 0);
    nc_sender_emit_stream_set_log(&stream, false);
    while (stream.active) {
        char text[NCC_MAX_LINE_LEN];
        nc_sender_emit_result_t r = nc_sender_emit_stream_next(&stream, text, sizeof(text),
                                                   &line);

        if (r == NC_SENDER_EMIT_ERROR) {
            return false;
        }
        if (r != NC_SENDER_EMIT_LINE) {
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
        ncc_document_t b;
        char out[4096];

        if (!host_fs_write_text(program, texts[pass])) {
            puts("emit2test: FAIL cannot write the fixture");
            return 1;
        }
        ncc_document_init(&b);
        if (!ncc_file_load(&b, program)) {
            puts("emit2test: FAIL the fixture does not load");
            return 1;
        }
        if (!host_emit_everything_ncc(&b, starts[pass], out, sizeof(out))) {
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
           nc_sender_emit_line_is_direct(line);
}

/* Everything ncc's run sends for a program, one line per row, as the machine
   reads it: the expansion of the cycles, joined with newlines. `end_x`/`end_z`
   come back as the point the stream left the tool at, which is where the machine
   has to arrive. */
static bool host_ncc_expand(const ncc_document_t *doc,
                            size_t start,
                            char *out,
                            size_t out_sz,
                            float *end_x,
                            float *end_z)
{
    nc_sender_emit_stream_t stream;
    g7x_doc_t source;
    size_t line = 0u;
    size_t used = 0u;
    unsigned guard = 0u;

    out[0] = '\0';
    source = ncc_document_g7x(doc);
    nc_sender_emit_stream_begin(&stream, &source, start,
                                host_nc_sender_tool_radius, 0);
    nc_sender_emit_stream_set_log(&stream, false);
    while (stream.active && guard++ < 100000u) {
        char text[NCC_MAX_LINE_LEN];
        nc_sender_emit_result_t r = nc_sender_emit_stream_next(&stream, text, sizeof(text),
                                                   &line);
        int n;

        if (r == NC_SENDER_EMIT_ERROR) {
            return false;
        }
        if (r != NC_SENDER_EMIT_LINE || !host_run2_sendable(text)) {
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

/* Read what ncc's run sends until it is over, the way `host_read_run()` reads
   nc's: the machine is pumped between blocks, so the pacing is exercised, not
   bypassed. */
/* Take the sender's own `N<n> ` off every block the run handed over, in place:
   what is left is the program the machine was given. False when a block had no
   number at all - the pane would have nothing to follow. */
static bool host_run2_strip_numbers(char *sent)
{
    char *read = sent;
    char *write = sent;

    while (*read) {
        char *nl = strchr(read, '\n');
        size_t len = nl ? (size_t)(nl - read) : strlen(read);
        size_t skip = 0u;

        if (len < 3u || read[0] != 'N') {
            return false;
        }
        skip = 1u;
        while (skip < len && read[skip] >= '0' && read[skip] <= '9') {
            skip++;
        }
        if (skip == 1u || skip >= len || read[skip] != ' ') {
            return false;
        }
        skip++;
        memmove(write, read + skip, len - skip);
        write += len - skip;
        *write++ = '\n';
        if (!nl) {
            break;
        }
        read = nl + 1;
    }
    *write = '\0';
    return true;
}

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
        if (host_machine_idle() && !nc_sender_run_streaming()) {
            break;
        }
        host_pump(1u);
    }
    if (*sent_len > 0u && sent[*sent_len - 1u] != '\n' && *sent_len + 1u < sent_cap) {
        sent[(*sent_len)++] = '\n';
    }
    sent[*sent_len] = '\0';
}

/* ncc's run: the sender hands the machine what the program means, one unit at a
   time, and the machine really runs it. The check reads the same reader the
   controller reads, compares the lines with the expansion `nc_sender` produces,
   requires the machine to have arrived where the program says, and looks at the
   picture for the machine's strip - which is there on every screen, wears the
   machine's grey while nothing happens and its green while the run is armed. */
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
    ncc_document_t doc;
    ncc_runtime_state_t rt;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, text)) {
        puts("run2test: FAIL cannot write the fixture");
        return 1;
    }
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);                 /* past the first start's logo */
    if (!ncc_visual_open(program)) {
        puts("run2test: FAIL the fixture does not load");
        return 1;
    }
    ncc_visual_select_mode(NCC_MODE_RUN);
    if (strcmp(ncc_visual_screen_name(), "RUN") != 0) {
        printf("run2test: FAIL the screen is \"%s\", not RUN\n",
               ncc_visual_screen_name());
        failures++;
    }
    /* The mode key walks EDIT, TOOLS, RUN, and TOOLS holds the tool table: the
       run has to end up with the program, not with the table the screen before
       it had open. */
    ncc_visual_select_mode(NCC_MODE_TOOLS);
    ncc_visual_key('A');
    if (strcmp(ncc_visual_screen_name(), "RUN") != 0 ||
        strcmp(ncc_visual_path(), program) != 0) {
        printf("run2test: FAIL RUN holds \"%s\" after TOOLS\n",
               ncc_visual_path());
        failures++;
    }

    /* Nothing running: the strip is up - it is the layout's own line between
       the drawing and the keys - and it wears the machine's grey, not the
       run's green. There is no tool on the drawing either: the glyph rides the
       cut, and nothing is cutting. */
    ncc_visual_draw();
    if (host_view_at(NCC_DRO_X + 3, NCC_DRO_Y + 3) !=
        host_panel_rgb(ncc_col_header())) {
        printf("run2test: FAIL the idle strip is 0x%06lX, not the machine's "
               "own colour\n",
               (unsigned long)host_view_at(NCC_DRO_X + 3, NCC_DRO_Y + 3));
        failures++;
    }
    /* `3 FULL`: what the panel hands the controller, line for line. Reading the
       reader here is what takes the lines, so the machine does not run them -
       the next pass runs the same program for the machine's own sake. */
    ncc_visual_key('3');
    host_read_run2(sent, sizeof(sent), &n);

    ncc_document_init(&doc);
    /* Every block the panel hands over carries the sender's own number, and the
       controller answers with the number it is executing - that pair is how the
       pane knows which line of the program is being cut. So the check strips
       the numbers to compare the *program* the machine was given, and insists
       every line had one. */
    if (!host_run2_strip_numbers(sent)) {
        puts("run2test: FAIL a block the run handed over carried no number");
        failures++;
    }
    if (!ncc_file_load(&doc, program) ||
        !host_ncc_expand(&doc, 0u, want, sizeof(want), &want_x, &want_z)) {
        puts("run2test: FAIL the fixture does not expand");
        failures++;
    } else if (strcmp(sent, want) != 0) {
        printf("run2test: FAIL FULL sent\n  \"%s\"\nwanted\n  \"%s\"\n",
               sent, want);
        failures++;
    }

    /* The same program again, but this time the lines are left to the machine:
       the run has to arrive where the expansion left the tool, and the strip has
       to say so - green while it runs, back to the machine's grey when it is
       over. */
    nc_sender_run_reset();
    host_pump_idle(64u);
    ncc_visual_key('3');
    host_pump(1u);
    ncc_visual_draw();
    if (host_view_at(NCC_DRO_X + 3, NCC_DRO_Y + 3) !=
        host_panel_rgb(ncc_col_run())) {
        puts("run2test: FAIL the strip is not green while the run is armed");
        failures++;
    }
    for (n = 0u; n < 200000u; n++) {
        if (host_machine_idle() && !nc_sender_run_streaming()) {
            break;
        }
        host_pump(1u);
    }
    host_pump_idle(64u);
    ncc_state_runtime(&rt);
    /* The program's X is a diameter and the axis works in the radius, so the
       machine's figure is half the program's (Z is the same in both). */
    if (fabsf(rt.x * 2.0f - want_x) > 0.1f || fabsf(rt.z - want_z) > 0.05f) {
        printf("run2test: FAIL the machine ended at X%.3f Z%.3f, not the "
               "program's X%.3f Z%.3f\n", (double)rt.x, (double)rt.z,
               (double)want_x, (double)want_z);
        failures++;
    }
    ncc_visual_draw();
    if (host_view_at(NCC_DRO_X + 3, NCC_DRO_Y + 3) ==
        host_panel_rgb(ncc_col_run())) {
        puts("run2test: FAIL the strip is still green after the run");
        failures++;
    }

    /* `1 SINGLE` sends the unit the mark is on, and the mark stays on the line
       the operator stepped from. */
    nc_sender_run_reset();
    ncc_visual_key('C');
    ncc_visual_key('C');                    /* the mark is on line 3 */
    ncc_visual_key('1');                    /* SINGLE: the block it sits in */
    host_pump_idle(64u);
    if (nc_sender_run_display_line() != 2u) {
        printf("run2test: FAIL SINGLE moved the mark to line %lu\n",
               (unsigned long)(nc_sender_run_display_line() + 1u));
        failures++;
    }

    /* TOOLS is refused while a run is armed: it loads the tool table into the
       document the run is sending, and the pacer would cut the table. The screen
       stays where it is, the run goes on, and the panel says why. */
    nc_sender_run_reset();
    host_pump_idle(64u);
    ncc_visual_key('3');                    /* FULL */
    host_pump(1u);
    ncc_visual_select_mode(NCC_MODE_TOOLS);
    if (strcmp(ncc_visual_screen_name(), "RUN") != 0 ||
        !strstr(ncc_visual_status(), "Program running")) {
        printf("run2test: FAIL TOOLS during a run: screen \"%s\", status "
               "\"%s\"\n", ncc_visual_screen_name(), ncc_visual_status());
        failures++;
    }
    nc_sender_run_stop();
    host_pump_idle(64u);
    nc_sender_run_reset();

    /* A tool the drawing's view does not reach is held on the pane's edge, not
       dropped: the machine is parked far outside the stock's envelope and the
       glyph still has to be on the glass (bench: "tool itself is gone one
       retrun on g0"). */
    if (!nc_sender_run_send_line("G0 X100 Z40")) {
        puts("run2test: FAIL cannot park the tool outside the view");
        failures++;
    } else {
        host_pump(1u);
        for (n = 0u; n < 200000u; n++) {
            if (host_machine_idle()) {
                break;
            }
            host_pump(1u);
        }
        nc_sender_run_reset();
        host_pump_idle(64u);
        ncc_visual_key('3');                /* FULL, so the machine is moving */
        host_pump(1u);
        ncc_visual_draw();
        nc_sender_run_stop();
        host_pump_idle(64u);
        nc_sender_run_reset();

    }

    if (failures) {
        printf("run2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("run2test: PASS the run hands over what the program means, the "
         "machine arrives, and the strip is green only while it runs");
    return 0;
}

/* How many pixels of one colour a rectangle of the picture holds. */
static int host_count_rect(int x0, int y0, int x1, int y1, uint32_t rgb)
{
    int count = 0;
    int x;
    int y;

    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            if (host_view_at(x, y) == rgb) {
                count++;
            }
        }
    }
    return count;
}

/* The stock's own colour down one column of the drawing: how many pixels carry
   it, and the first and last row they sit on. A column with none writes
   nothing into `top`/`bottom`. */
static int host_stock_column(int x, uint32_t rgb, int *top, int *bottom)
{
    int count = 0;
    int y;

    for (y = NCC_PREVIEW_Y; y < NCC_PREVIEW_BOTTOM; y++) {
        if (host_view_at(x, y) != rgb) {
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
   it every turn of its loop. The check turns a taper and reads the picture: the
   material below the tool is gone, its top is where it was, the RUN screen keeps
   the part while the machine is parked, and the editor draws the stock whole.

   And then the demo card's own program, from a tool parked off the stock, with
   the panel standing on TOOLS for a stretch of the run: the material the
   program never cuts has to be whole, where the contour reaches the material
   has to end at it, and a frame that paints the pane whole has to read the same
   material as the one before it - the glass may not lag behind the mask (bench:
   "no remaing is bigger than x/z in g71 command. this was fixed before").

   This is a comparison of the stock's own colour per column of the drawing, not
   a picture: the cut is where nc's mask says it is or the counts say so. */
static int host_live2test(void)
{
    static const char *const program = "/D/nc/files/live2.nc";
    static const char *const text =
        "G0 X50 Z-70\n"
        "G1 X10 Z-18 F200\n"
        "M5\n";
    static int before_col[NCC_PREVIEW_W];
    static int before_top[NCC_PREVIEW_W];
    static int before_bottom[NCC_PREVIEW_W];
    int before_total = 0;
    int cut_columns = 0;
    int cut_top = 0;
    int cut_bottom = 0;
    int lost = 0;
    int stock_left = -1;
    int stock_right = -1;
    int rect_top = NCC_PREVIEW_BOTTOM;
    int rect_bottom = NCC_PREVIEW_Y;
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
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    if (!ncc_visual_open(program)) {
        puts("live2test: FAIL the fixture does not load");
        return 1;
    }
    ncc_visual_select_mode(NCC_MODE_RUN);

    /* The tool is where the operator left it, and that is where the mask starts:
       parked off the stock, not sitting on the axis, or the first frame's own
       position would read as a cut. */
    if (!nc_sender_run_send_line("G0 X60 Z10")) {
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
    ncc_visual_draw();

    /* The stock, off the idle RUN screen: its colour's own columns, and the
       band of rows they sit in - the rectangle every count below is taken in. */
    stock_rgb = host_panel_rgb(ncc_col_prev_stock());
    for (x = NCC_PREVIEW_X; x < NCC_PREVIEW_X + NCC_PREVIEW_W; x++) {
        int col = x - NCC_PREVIEW_X;
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
    nc_sender_run_reset();
    host_pump_idle(64u);
    ncc_visual_key('3');                    /* FULL */
    host_pump(1u);
    ncc_visual_draw();
    for (i = 0u; i < 200000u; i++) {
        if (host_machine_idle() && !nc_sender_run_streaming()) {
            break;
        }
        host_pump(1u);
        ncc_visual_draw();
    }
    host_pump_idle(64u);
    ncc_visual_draw();
    cut_top = NCC_PREVIEW_X + NCC_PREVIEW_W;
    cut_bottom = NCC_PREVIEW_X;
    for (x = NCC_PREVIEW_X; x < NCC_PREVIEW_X + NCC_PREVIEW_W; x++) {
        int col = x - NCC_PREVIEW_X;
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
    ncc_visual_draw();
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
    ncc_visual_select_mode(NCC_MODE_PROGRAM);
    ncc_visual_draw();
    whole = host_count_rect(stock_left, rect_top, stock_right + 1, rect_bottom + 1,
                            stock_rgb);
    if (whole != before_total) {
        printf("live2test: FAIL the editor shows %d stock pixels, wanted %d\n",
               whole, before_total);
        failures++;
    }

    /* And the cut is the *program's* own, in both directions. The run is the
        demo card's program - two `G71`/`G70` pairs - from a tool parked off the
        stock, as the operator is told to park it, and every column is read
        twice: once for the contour the finish draws, once for the material's
        edge.

         - A column the contour reaches may not keep material past it: that
           would be stock left uncut where the command says it is gone (bench:
           "no remaing is bigger than x/z in g71 command").
         - A column the contour never reaches may not lose any material at
           all: the parking move and the cycle's own rapids are at the
           clearance, away from the stock, and a cut is the only thing that
           takes material off.
         - And the glass is the mask. A frame that paints the pane whole is
           asked for at the end and the material has to read the same: the mask
           is what the drawing paints, and a frame that leaves material on the
           glass the mask has already cut is the fault the bench kept seeing.

       The bench's own program, with its own numbers. */
    {
        static const char *const cycle = "/D/nc/files/live2cycle.nc";
        static const char *const cycle_text =
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
            "G1 X35 Z-25\n"
            "N55 G1 X52 Z-25\n"
            "G70 P50 Q55\n"
            "G71 U2 R1 X1 Z1 F500 P100 Q200\n"
            "N100 G1 X35 Z0 R0\n"
            "G1 X35 Z-20 R5\n"
            "N200 G1 X50 Z-20\n"
            "G70 P100 Q200\n"
            "G1 X60 Z5 C0 R0\n"
            "M5\n";
        uint32_t profile_rgb = host_panel_rgb(ncc_col_prev_profile());
        /* The debug line draws all three shifts, and the `G40` one lies exactly
           on the programmed path - so where it is up, the contour on the glass
           is the debug green. Both are the contour the checks below mean. */
        uint32_t debug_rgb = host_panel_rgb(ncc_col_run());
        static bool has_profile[NCC_PREVIEW_W + 2];
        static int stock_rows[NCC_PREVIEW_W];
        int stock_top_row = NCC_PREVIEW_BOTTOM;
        int stock_bottom_row = NCC_PREVIEW_Y;
        int stock_first = NCC_PREVIEW_X + NCC_PREVIEW_W;
        int stock_last = NCC_PREVIEW_X;
        int columns = 0;
        int too_deep = 0;
        int stolen = 0;

        if (!host_fs_write_text(cycle, cycle_text)) {
            puts("live2test: FAIL cannot write the cycle fixture");
            failures++;
        } else {
            ncc_visual_select_mode(NCC_MODE_RUN);
            if (!ncc_visual_open(cycle)) {
                puts("live2test: FAIL the cycle fixture does not load");
                failures++;
            } else {
                /* The stock's own box, off the idle RUN screen: the material is
                   whole there, and the columns the cut reaches are measured
                   against it. */
                ncc_visual_draw();
                for (x = NCC_PREVIEW_X; x < NCC_PREVIEW_X + NCC_PREVIEW_W; x++) {
                    int top = 0;
                    int bottom = 0;
                    int count = host_stock_column(x, stock_rgb, &top, &bottom);

                    stock_rows[x - NCC_PREVIEW_X] = count;
                    if (count <= 0) {
                        continue;
                    }
                    if (top < stock_top_row) stock_top_row = top;
                    if (bottom > stock_bottom_row) stock_bottom_row = bottom;
                    if (x < stock_first) stock_first = x;
                    if (x > stock_last) stock_last = x;
                }
                if (stock_last - stock_first < 100 || stock_bottom_row -
                    stock_top_row < 60) {
                    printf("live2test: FAIL the cycle's stock is %d..%d rows "
                           "%d..%d\n", stock_first, stock_last, stock_top_row,
                           stock_bottom_row);
                    failures++;
                }

                /* Park it the way the operator is told to: off the stock. */
                if (!nc_sender_run_send_line("G0 X60 Z10")) {
                    puts("live2test: FAIL the panel will not park the tool");
                    failures++;
                }
                host_pump_idle(64u);
                ncc_visual_draw();

                nc_sender_run_reset();
                host_pump_idle(64u);
                ncc_visual_draw();
                ncc_visual_key('3');            /* 3 FULL: run the program */
                host_pump(1u);
                ncc_visual_draw();
                /* And the operator stands on the TOOLS screen for a while
                   while it cuts: the pane there is the table's, and the live
                   stock still has to keep up - a frozen mask catches up in one
                   straight sweep across the material the tool really walked. */
                host_pump(1u);
                ncc_visual_select_mode(NCC_MODE_TOOLS);
                for (i = 0u; i < 2000u; i++) {
                    host_pump(1u);
                    ncc_visual_draw();
                }
                ncc_visual_select_mode(NCC_MODE_RUN);
                ncc_visual_draw();
                for (i = 0u; i < 200000u; i++) {
                    if (host_machine_idle() && !nc_sender_run_streaming()) {
                        break;
                    }
                    host_pump(1u);
                    ncc_visual_draw();
                }
                host_pump_idle(64u);
                ncc_visual_draw();

                /* Where the contour is, column by column: what the program
                   cuts, and what it does not. */
                memset(has_profile, 0, sizeof(has_profile));
                for (x = NCC_PREVIEW_X; x < NCC_PREVIEW_X + NCC_PREVIEW_W; x++) {
                    int y;

                    for (y = NCC_PREVIEW_Y; y < NCC_PREVIEW_BOTTOM; y++) {
                        uint32_t c = host_view_at(x, y);

                        if (c == profile_rgb || c == debug_rgb) {
                            has_profile[x - NCC_PREVIEW_X] = true;
                            break;
                        }
                    }
                }
                for (x = NCC_PREVIEW_X; x < NCC_PREVIEW_X + NCC_PREVIEW_W; x++) {
                    int profile_y = -1;
                    int material_y = -1;
                    int col = x - NCC_PREVIEW_X;
                    int y;

                    for (y = NCC_PREVIEW_Y; y < NCC_PREVIEW_BOTTOM; y++) {
                        uint32_t c = host_view_at(x, y);

                        if (c == profile_rgb || c == debug_rgb) {
                            profile_y = y;      /* the finish contour's row */
                        }
                        if (c == stock_rgb) {
                            material_y = y;     /* the material still there */
                        }
                    }
                    /* A column the contour does not reach is the material the
                       program never asks for. Its own edge, minus the couple of
                       pixels the whole-stock reading is off by - and the stock
                       has to be there, not the pane's ground. */
                    if (profile_y < 0) {
                        /* The cut's shape is the row's own *nose radius* now, so
                           its edge reaches that far past the contour's last
                           point - a round tool leaves its radius at the end of
                           a cut, which is the whole point of drawing the brush
                           as a circle (bench, 2026-09-28: *"the shape used in
                           framebuffer to draw remaining of stock ..."*). The
                           fixture's `T2` is `R0.8` and the pane is about seven
                           pixels to the millimetre, so eight columns cover it. */
                        static const int reach = 8;
                        int ok = 0;
                        int k;

                        for (k = -reach; k <= reach && !ok; k++) {
                            int at = col + k;

                            if (at >= 0 && at < (int)(sizeof(has_profile) /
                                                      sizeof(has_profile[0])) &&
                                has_profile[at]) {
                                ok = 1;
                            }
                        }
                        if (ok) {
                            continue;
                        }
                        if (stock_rows[col] > 20 &&
                            (material_y < 0 ||
                             material_y < stock_bottom_row - 2)) {
                            if (!stolen) {
                                printf("live2test: FAIL column %d lost material "
                                       "the program never cut (edge %d, stock "
                                       "ends at %d)\n", x, material_y,
                                       stock_bottom_row);
                            }
                            stolen++;
                        }
                        continue;
                    }
                    columns++;
                    if (material_y > profile_y + 2) {
                        if (!too_deep) {
                            printf("live2test: FAIL column %d keeps material to "
                                   "row %d, the profile is at %d\n", x,
                                   material_y, profile_y);
                        }
                        too_deep++;
                    }
                }
                if (columns < 20) {
                    printf("live2test: FAIL the cycle drew no profile (%d "
                           "columns)\n", columns);
                    failures++;
                }
                if (too_deep) {
                    printf("live2test: FAIL %d of %d columns keep material "
                           "below the profile\n", too_deep, columns);
                    failures++;
                }
                if (stolen) {
                    printf("live2test: FAIL %d columns lost material outside "
                           "the profile\n", stolen);
                    failures++;
                }
                /* And the pane is what the mask says. A frame that paints the
                   pane whole is asked for, and the material has to read the
                   same either side of it: a difference is material the
                   per-band repaint left on the glass - the part drawn short
                   where the program has already cut it. */
                {
                    int before = host_count_rect(NCC_PREVIEW_X,
                                                 NCC_PREVIEW_Y,
                                                 NCC_PREVIEW_X + NCC_PREVIEW_W,
                                                 NCC_PREVIEW_BOTTOM, stock_rgb);
                    int after;

                    ncc_visual_mark_dirty();
                    ncc_visual_draw();
                    after = host_count_rect(NCC_PREVIEW_X, NCC_PREVIEW_Y,
                                            NCC_PREVIEW_X + NCC_PREVIEW_W,
                                            NCC_PREVIEW_BOTTOM, stock_rgb);
                    if (before != after) {
                        printf("live2test: FAIL the glass holds %d material "
                               "pixels where the mask has %d\n", before,
                               after);
                        failures++;
                    }
                }
            }
        }
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

/* The picture the panel shows while it starts.

   The splash takes the renderer's palette for its own sixteen colours (the
   panel's table has no blue and no dark green in it, so a picture reduced onto
   the panel's colours comes out grey - the first attempt at the lathe picture
   did), paints, and gives them back before the work screen draws a pixel. This
   check reads all three off the glass: pixels in colours the panel does *not*
   have (the picture is showing), the build's stamp under it, and the work
   screen's own colours back afterwards - by a key and by the clock both. */
static int host_boot2test(void)
{
    uint32_t panel[LC_COLOR_COUNT];
    uint32_t letterbox;
    int own = 0;
    int stamp = 0;
    int x;
    int y;
    int i;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    /* The machine's own boot, in the machine's own order: an empty palette, and
       then the screen comes up. The picture has to take its colours *before*
       the panel asks for its own - the other way round is what made the glass
       grey while this host, with no cap at the time, showed it in colour. */
    lvds_host_palette_reset();
    ncc_visual_init();
    if (!ncc_boot_active()) {
        puts("boot2test: FAIL no picture at start");
        return 1;
    }
    if (lvds_host_palette_missing() != 0u) {
        printf("boot2test: FAIL %u of the picture's colours have no palette "
               "slot - the panel asked first\n", lvds_host_palette_missing());
        failures++;
    }
    ncc_visual_draw();
    for (i = 0; i < LC_COLOR_COUNT; i++) {
        panel[i] = host_panel_rgb(lvds_palette_color((lvds_palette_color_id_t)i));
    }
    for (y = NCC_BOOT_IMAGE_Y; y < NCC_BOOT_IMAGE_Y + NCC_BOOT_IMAGE_H; y++) {
        for (x = NCC_BOOT_IMAGE_X; x < NCC_BOOT_IMAGE_X + NCC_BOOT_IMAGE_W; x++) {
            uint32_t c = host_view_at(x, y);
            bool known = false;

            for (i = 0; i < LC_COLOR_COUNT; i++) {
                if (c == panel[i]) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                own++;
            }
        }
    }
    if (own < 20000) {
        printf("boot2test: FAIL the picture is on the glass in the panel's "
               "colours (%d pixels of its own)\n", own);
        failures++;
    }
    /* The build's own stamp, under the picture. */
    letterbox = host_panel_rgb(lvds_hstx_rgb(
        ncc_boot_image_colours[NCC_BOOT_IMAGE_BG].r,
        ncc_boot_image_colours[NCC_BOOT_IMAGE_BG].g,
        ncc_boot_image_colours[NCC_BOOT_IMAGE_BG].b));
    for (y = NCC_BOOT_IMAGE_Y + NCC_BOOT_IMAGE_H + 16;
         y < NCC_BOOT_IMAGE_Y + NCC_BOOT_IMAGE_H + 52; y++) {
        for (x = 80; x < LVDS_VIEW_WIDTH - 80; x++) {
            if (host_view_at(x, y) != letterbox) {
                stamp++;
            }
        }
    }
    if (stamp < 200) {
        printf("boot2test: FAIL there is no build stamp under the picture (%d "
               "pixels)\n", stamp);
        failures++;
    }
    /* A key ends it, and the work screen is what is drawn next. */
    {
        unsigned before = lvds_host_palette_reset_count();

    ncc_visual_key('0');
    if (ncc_boot_active()) {
        puts("boot2test: FAIL a key did not end the picture");
        failures++;
    }
    if (lvds_host_palette_reset_count() == before) {
        puts("boot2test: FAIL the picture did not hand the palette back");
        failures++;
    }
    }
    ncc_visual_draw();
    if (!host_view_ink_in(0, 0, 200, NCC_HEADER_H)) {
        puts("boot2test: FAIL the work screen did not take over");
        failures++;
    }
    /* And the panel's own colours fit the palette it now has: the picture's
       slots went back with the picture (the release resets the palette), and
       the work screen takes its own as it draws. */
    if (lvds_host_palette_missing() != 0u) {
        printf("boot2test: FAIL %u of the panel's own colours have no slot "
               "after the picture\n", lvds_host_palette_missing());
        failures++;
    }
    /* And the clock ends it by itself - the operator who is not watching still
       gets a panel. */
    ncc_boot_start();
    if (!ncc_boot_active()) {
        puts("boot2test: FAIL the picture did not come back");
        failures++;
    }
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    if (ncc_boot_active()) {
        puts("boot2test: FAIL the picture never let go");
        failures++;
    }

    if (failures) {
        printf("boot2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("boot2test: PASS the picture is painted in its own colours with the "
         "build's stamp, and it lets go");
    return 0;
}

/* A line the controller refuses has to be said on the screen: the strip's own
   word (`uCNC ERROR`) is a glance, and the sentence names the line, the code and
   what it means (bench: "it says ucnc erro in strip only but not message
   itself?"). The fixture is a program whose third line has a word with no
   number - `G0 X` - which is what the controller answers `error:2` to. */
static int host_fault2test(void)
{
    static const char *const program = "/D/nc/files/fault2.nc";
    static const char *const text =
        "G0 X10 Z2\n"
        "G1 X12 Z2 F100\n"
        "G0 X\n";
    uint32_t err_rgb;
    ncc_document_t doc;
    unsigned guard;
    char said[80];
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, text)) {
        puts("fault2test: FAIL cannot write the fixture");
        return 1;
    }
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    if (!ncc_visual_open(program)) {
        puts("fault2test: FAIL the fixture does not load");
        return 1;
    }
    /* The operator is working on line 2 of the fixture before the run: that is
       the place a fault must not take away from them. */
    ncc_visual_select_mode(NCC_MODE_PROGRAM);
    ncc_visual_key('C');
    ncc_visual_select_mode(NCC_MODE_RUN);
    host_pump_idle(32u);
    ncc_visual_key('3');                    /* FULL */
    for (guard = 0u; guard < 200000u; guard++) {
        if (!nc_sender_run_streaming() && host_machine_idle()) {
            break;
        }
        host_pump(1u);
    }
    host_pump_idle(32u);
    ncc_visual_draw();
    if (!nc_sender_run_error()) {
        puts("fault2test: FAIL the bad line was accepted");
        failures++;
    } else if (!strstr(ncc_visual_status(), "error 2") ||
               !strstr(ncc_visual_status(), "Line 3")) {
        printf("fault2test: FAIL the screen says \"%s\"\n",
               ncc_visual_status());
        failures++;
    }
    snprintf(said, sizeof(said), "%s",
             ncc_visual_status() ? ncc_visual_status() : "");
    /* And the block that was refused is named too: the sentence says which line
       of the program the fault belongs to, and a cycle's line cannot say which
       of the motions it expanded to the machine read (bench, 2026-09-27 - a
       run refused line 8, the `G71`, with "error 2: Invalid number" and nothing
       in the program that could be one). */
    if (strcmp(nc_sender_run_refused_text(), "G0 X") != 0) {
        printf("fault2test: FAIL the refused block reads \"%s\", not the bad "
               "line\n", nc_sender_run_refused_text());
        failures++;
    }
    /* The run's mark stays on the line that failed: the controller answers a
       refusal with a parser reset, and that must not carry the pane back to the
       top of the program (bench: "on error - it still jumps to first line. no i
       should stay here"). Line 3 of the fixture is the bad one. */
    if (nc_sender_run_display_line() != 2u) {
        printf("fault2test: FAIL the mark is on line %lu, not the one that "
               "failed\n", (unsigned long)(nc_sender_run_display_line() + 1u));
        failures++;
    }
    /* And the key the operator answers a fault with - `# RELOAD`, which reads the
       file back and clears the run - leaves the mark where the fault stopped it:
       they are standing on that line to fix it (bench: "on error - it still
       jumps to first line. no i should stay here"). */
    ncc_visual_key('#');
    if (nc_sender_run_display_line() != 2u) {
        printf("fault2test: FAIL RELOAD moved the mark to line %lu\n",
               (unsigned long)(nc_sender_run_display_line() + 1u));
        failures++;
    }
    ncc_visual_select_mode(NCC_MODE_PROGRAM);
    if (ncc_visual_cursor() != 1u) {
        printf("fault2test: FAIL EDIT opens on line %lu, not the operator's "
               "own\n", (unsigned long)(ncc_visual_cursor() + 1u));
        failures++;
    }
    /* And it is said where the operator reads it: nc's fault block, white
       letters on the fault's own red, in the panel's font and in full - a
       sentence cut at the panel's edge is not a message (bench: "error style -
       red background with white letters. same as nc"). */
    err_rgb = host_panel_rgb(ncc_col_error());
    {
        uint32_t white_rgb = host_panel_rgb(ncc_col_field_fg());
        int red = 0;
        int white_on_red = 0;
        int x;
        int y;

        for (y = NCC_NOTES_Y; y < NCC_NOTES_Y + 2 * NCC_FAULT_ROW_H; y++) {
            for (x = NCC_NOTES_X + 2; x < NCC_NOTES_X + NCC_NOTES_W - 6; x++) {
                uint32_t c = host_view_at(x, y);

                if (c == err_rgb) {
                    red++;
                } else if (c == white_rgb &&
                           (host_view_at(x - 2, y) == err_rgb ||
                            host_view_at(x + 2, y) == err_rgb ||
                            host_view_at(x, y + 2) == err_rgb ||
                            host_view_at(x, y - 2) == err_rgb)) {
                    white_on_red++;
                }
            }
        }
        if (red < 200) {
            printf("fault2test: FAIL the fault is not a block of red (%d "
                   "pixels)\n", red);
            failures++;
        } else if (white_on_red < 20) {
            printf("fault2test: FAIL the fault's letters are not white on red "
                   "(%d pixels)\n", white_on_red);
            failures++;
        }
    }
    /* Anything the panel can have to say fits: the status line is
       `NCC_STATUS_MAX` characters, and the block's font is chosen so the longest
       of them fits its columns. An error that cannot be read in full is worse
       than no error at all (bench: "there is nothing more annoying than an error
       you cannot read. So it should fit all possible messages"). */
    {
        char long_msg[NCC_STATUS_MAX];
        int cols_band = (NCC_NOTES_W - 8) / ncc_col_width(LVDS_FONT_NORMAL);
        size_t len;
        int ink = 0;
        int x;
        int y;

        if (cols_band * NCC_FAULT_LINES < NCC_STATUS_MAX - 1) {
            printf("fault2test: FAIL the fault block holds %d characters where "
                   "the panel can say %d\n", cols_band * NCC_FAULT_LINES,
                   NCC_STATUS_MAX - 1);
            failures++;
        }
        snprintf(long_msg, sizeof(long_msg),
                 "Line 12 error 20: no G41/G42 in a cycle here");
        len = strlen(long_msg);
        memset(long_msg + len, 'x', sizeof(long_msg) - 1u - len);
        long_msg[sizeof(long_msg) - 1u] = '\0';
        ncc_visual_status_set(long_msg);
        ncc_visual_draw();
        /* The block's third line carries ink: the sentence was not cut. */
        for (y = NCC_NOTES_Y + 2 + 2 * NCC_FAULT_ROW_H;
             y < NCC_NOTES_Y + 2 + 3 * NCC_FAULT_ROW_H; y++) {
            for (x = NCC_NOTES_X + 4; x < NCC_NOTES_X + NCC_NOTES_W - 6; x++) {
                if (host_view_at(x, y) != err_rgb) {
                    ink++;
                }
            }
        }
        if (ink < 20) {
            printf("fault2test: FAIL the block's third line is empty (%d "
                   "pixels)\n", ink);
            failures++;
        }
    }
    ncc_document_init(&doc);
    if (failures) {
        printf("fault2test: FAILED (%d)\n", failures);
        return 1;
    }
    printf("fault2test: PASS the refusal reads \"%s\" on the screen\n", said);
    return 0;
}

/* The panel's *own* refusal - its generator, not the controller.

   A cycle whose profile range never closed is refused by the panel before a
   block of it is sent. The pane has to keep marking the line the generator gave
   up on: `g_nc_sender_run_line` is the line the pacer would have sent *next*, so
   leaving it alone puts the mark one line past the trouble and `1 SINGLE`
   retries the wrong line - the one that failed never runs (bench, 2026-09-27:
   "on errors - again 1 line is selected and this one will not run"). */
static int host_refuse2test(void)
{
    static const char *const program = "/D/nc/files/refuse.nc";
    unsigned guard;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program,
                            "G0 X10 Z2\n"
                            "G71 U1 R0.5 X0.5 Z0.5 F450 P10 Q20\n"
                            "G80\n")) {
        puts("refuse2test: FAIL cannot write the fixture");
        return 1;
    }
    if (!ncc_visual_open(program)) {
        puts("refuse2test: FAIL the fixture does not load");
        return 1;
    }
    ncc_visual_select_mode(NCC_MODE_RUN);
    ncc_visual_key('3');                    /* FULL */
    for (guard = 0u; guard < 200000u; guard++) {
        if (!nc_sender_run_streaming() && host_machine_idle()) {
            break;
        }
        host_pump(1u);
    }
    host_pump_idle(32u);
    ncc_visual_draw();
    /* The cycle is line 2 (index 1), and that is where the mark has to be. */
    if (nc_sender_run_display_line() != 1u) {
        printf("refuse2test: FAIL the mark is on line %lu, not the cycle the "
               "panel refused\n", (unsigned long)(nc_sender_run_display_line() + 1u));
        failures++;
    }
    if (failures) {
        printf("refuse2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("refuse2test: PASS the mark keeps the line the panel refused");
    return 0;
}

/* The line each thing the run hands over belongs to. A line the panel sends as
   it stands belongs to itself; everything the generator makes while a cycle is
   being expanded belongs to the cycle - its header. That is the line a fault
   has to leave the operator on. The generator's own motions used to answer line
   1 (the block's clear wrote over the `(size_t)-1` that says "no row of the
   program"), so a fault on one of them marked the stock line, and `1 SINGLE`
   there has nothing it can run - bench, 2026-09-27: "on errors - again 1 line
   is selected and this one will not run". */
static int host_blockline2test(void)
{
    static const char *const program = "/D/nc/files/blockline.nc";
    static const char *const text =
        "G971 X50 Z50 I0E0\n"
        "G972 C10\n"
        "T2\n"
        "M3 S450\n"
        "G0 X52 Z2\n"
        "G71 U3 R1 X1 Z1 F500 P50 Q55\n"
        "N50 G1 X30 Z2\n"
        "G1 X30 Z-15 C2\n"
        "G1 X35 Z-15\n"
        "N55 G1 X52 Z-25\n"
        "G80\n";
    static const size_t first = 5u;     /* the G71 header */
    static const size_t last = 10u;     /* its G80 */
    static const size_t standing = 3u;  /* T2, M3 S450, G0 X52 Z2 */
    ncc_document_t doc;
    nc_sender_emit_stream_t stream;
    g7x_doc_t source;
    char out[NCC_MAX_LINE_LEN];
    size_t line = 0u;
    unsigned guard = 0u;
    int block = 0;
    int plain_lines = 0;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, text)) {
        puts("blockline2test: FAIL cannot write the fixture");
        return 1;
    }
    ncc_document_init(&doc);
    if (!ncc_file_load(&doc, program) || doc.line_count != last + 1u) {
        puts("blockline2test: FAIL the fixture does not load");
        return 1;
    }
    source = ncc_document_g7x(&doc);
    nc_sender_emit_stream_begin(&stream, &source, 0u,
                                host_nc_sender_tool_radius, 0);
    nc_sender_emit_stream_set_log(&stream, false);
    while (stream.active && guard++ < 500u) {
        nc_sender_emit_result_t result = nc_sender_emit_stream_next(&stream, out,
                                                        sizeof(out), &line);

        if (result == NC_SENDER_EMIT_ERROR) {
            printf("blockline2test: FAIL the cycle was refused: %s\n",
                   g7x_result_text(stream.error));
            failures++;
            break;
        }
        if (result != NC_SENDER_EMIT_LINE) {
            continue;
        }
        if (stream.block_line < first) {
            plain_lines++;
        } else if (stream.block_line == first) {
            block++;
        } else {
            printf("blockline2test: FAIL \"%.40s\" belongs to line %lu, past "
                   "the cycle it came out of\n",
                   out, (unsigned long)(stream.block_line + 1u));
            failures++;
            break;
        }
    }
    /* The whole expansion - its contours and the motions the generator made
       between them - answers for the one header, and that is more lines than
       the fixture has: this is what a fault marks. */
    if (!failures && block <= (int)(last - first)) {
        printf("blockline2test: FAIL only %d of the cycle's lines answered for "
               "it - the motions the generator makes are not counted\n", block);
        failures++;
    }
    if (!failures && plain_lines != (int)standing) {
        printf("blockline2test: FAIL %d lines answered for themselves, not the "
               "%u the program stands on\n", plain_lines, (unsigned)standing);
        failures++;
    }
    if (failures) {
        printf("blockline2test: FAILED (%d)\n", failures);
        return 1;
    }
    printf("blockline2test: PASS the cycle's own motions answer for it (%d of "
           "the run's lines), and the line the program stands on answers for "
           "itself\n", block);
    return 0;
}

/* The code pane's scroll: the cursor walks down and the *text* moves, so the
   rows after the cursor's stay in view. The bench: "cursor should never reach
   last line. in g code it is always necessary to see next line. so as before
   leave - 6 lines and move text, but not the cursor to the end." */
static int host_scroll2test(void)
{
    static const char *const program = "/D/nc/files/scroll2.nc";
    char text[2048];
    size_t used = 0u;
    uint32_t select_rgb;
    int failures = 0;
    int i;
    int sel_row = -1;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    for (i = 1; i <= 30; i++) {
        int n = snprintf(text + used, sizeof(text) - used, "G1 X%d Z-%d\n", i, i);

        if (n <= 0 || (size_t)n >= sizeof(text) - used) {
            break;
        }
        used += (size_t)n;
    }
    if (!host_fs_write_text(program, text)) {
        puts("scroll2test: FAIL cannot write the fixture");
        return 1;
    }
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    if (!ncc_visual_open(program)) {
        puts("scroll2test: FAIL the fixture does not load");
        return 1;
    }
    /* Twenty lines down: the pane has to have moved, and the cursor has to be
       short of its last row. */
    for (i = 0; i < 20; i++) {
        ncc_visual_key('C');
    }
    ncc_visual_draw();
    select_rgb = host_panel_rgb(ncc_col_select());
    for (i = 0; i < NCC_CODE_ROWS; i++) {
        if (host_pane2_row_bg(i) == select_rgb) {
            sel_row = i;
            break;
        }
    }
    if (sel_row < 0) {
        puts("scroll2test: FAIL the cursor's row is not on the glass");
        failures++;
    } else if (sel_row > NCC_CODE_ROWS - 1 - 6) {
        printf("scroll2test: FAIL the cursor sits on row %d of %d - fewer than "
               "six rows after it\n", sel_row + 1, NCC_CODE_ROWS);
        failures++;
    } else {
        /* And the rows after it are the *next* lines: ink, not empty pane. */
        int below;

        for (below = sel_row + 1; below <= sel_row + 6; below++) {
            int y = NCC_TEXT_Y + 4 + below * NCC_ROW_H;

            if (!host_view_ink_in(NCC_TEXT_X + NCC_LINE_TEXT_PAD, y,
                                  NCC_TEXT_W - NCC_LINE_TEXT_PAD - 4,
                                  NCC_ROW_H)) {
                printf("scroll2test: FAIL row %d after the cursor is empty\n",
                       below + 1);
                failures++;
                break;
            }
        }
    }
    if (failures) {
        printf("scroll2test: FAILED (%d)\n", failures);
        return 1;
    }
    printf("scroll2test: PASS the cursor stops on row %d with six rows of the "
           "program after it\n", sel_row + 1);
    return 0;
}

/* The frame meter, which the bench asked for back to test what a change costs
   the panel ("give me back fps meter it need to be tested"). The screen counts
   the frames it draws and keeps the last whole second's worth; the check draws
   a second of them - one per millisecond of the panel's own clock - and insists
   the meter counted them and that the reading is on the glass where the
   operator does not read. */
static int host_fps2test(void)
{
    unsigned i;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    ncc_visual_draw();
    for (i = 0u; i < 1200u; i++) {
        host_pump(1u);              /* one millisecond of the panel's clock */
        ncc_visual_draw();
    }
    if (ncc_visual_fps() < 100u) {
        printf("fps2test: FAIL the meter reads %u after 1200 frames\n",
               (unsigned)ncc_visual_fps());
        failures++;
    }
    /* The reading is at the *bottom* now: the header's right corner belongs to
       `uCNC` (bench, 2026-09-28: "put uCNC on the rigth side of header. move
       fps to bottom"). */
    if (!host_view_ink_in(NCC_TEXT_X, NCC_BODY_BOTTOM - 12, 56, 14)) {
        puts("fps2test: FAIL the reading is not at the bottom of the glass");
        failures++;
    }
    /* And a frame that changes nothing must not repaint what does not change:
       the check damages a pixel in the pad's corner and draws again - a partial
       frame leaves it (the pad's pixels are the ones already on the glass), and
       a key paints over it, because a key is something the screen shows. */
    {
        uint32_t *px = (uint32_t *)lvds_host_pixels();
        int vx = NCC_PAD_X + 4;
        int vy = NCC_PAD_Y + 4;
        size_t stride = (size_t)(lvds_host_stride() / 4);
        size_t index = (size_t)LVDS_PANEL_Y(vx, vy) * stride +
                       (size_t)LVDS_PANEL_X(vx, vy);
        uint32_t damage = 0x00FF00FFu;      /* a colour nothing draws */

        px[index] = damage;
        ncc_visual_draw();                  /* nothing changed */
        if (px[index] != damage) {
            puts("fps2test: FAIL an unchanged frame repainted the pad");
            failures++;
        }
        ncc_visual_key('C');                /* a key: the text moves */
        ncc_visual_draw();
        if (px[index] == damage) {
            puts("fps2test: FAIL a changed frame left the pad alone");
            failures++;
        }
    }
    if (failures) {
        printf("fps2test: FAILED (%d)\n", failures);
        return 1;
    }
    printf("fps2test: PASS the meter read %u frames a second and the reading "
           "is at the bottom\n", (unsigned)ncc_visual_fps());
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

/* ncc's MANUAL: the machine panel. The digits jog, the spindle runs from the
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
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    ncc_visual_select_mode(NCC_MODE_MANUAL);
    if (strcmp(ncc_visual_screen_name(), "MANUAL") != 0) {
        printf("manual2test: FAIL the screen is \"%s\", not MANUAL\n",
               ncc_visual_screen_name());
        return 1;
    }
    host_pump_idle(32u);

    /* A step jog is the move and the G90 that puts the machine back, in that
       order: X is a diameter, so a 0.100 mm step is written X0.200. */
    ncc_visual_key('B');                      /* the axis is X */
    ncc_visual_key('2');                      /* X+ */
    host_drain_panel(sent, sizeof(sent));
    if (!strstr(sent, "G91 G1 X0.200 F500") || !strstr(sent, "G90")) {
        printf("manual2test: FAIL the X+ step sent \"%s\"\n", sent);
        failures++;
    }
    host_pump_idle(64u);

    /* The value keys change the step the next press uses. */
    ncc_visual_key('3');                      /* one step bigger: 0.250 */
    ncc_visual_key('2');
    host_drain_panel(sent, sizeof(sent));
    if (!strstr(sent, "X0.500")) {
        printf("manual2test: FAIL the bigger step sent \"%s\"\n", sent);
        failures++;
    }
    host_pump_idle(64u);

    /* One more press, this time left to the machine: the axis has to move off
       the stop it is standing on, or the feed below would be refused - which is
       the answer the screen gives, and not what this check is about. */
    ncc_visual_key('2');
    host_pump_idle(64u);
    {
        ncc_runtime_state_t rt;

        ncc_state_runtime(&rt);
        if (!(rt.x > 0.05f)) {
            printf("manual2test: FAIL the X+ jog left the axis at %.3f\n",
                   (double)rt.x);
            failures++;
        }
    }

    /* The spindle keys are the machine's, and the speed is the remembered one. */
    ncc_visual_key('9');                      /* CW */
    host_drain_panel(sent, sizeof(sent));
    if (!strstr(sent, "M3 S")) {
        printf("manual2test: FAIL the CW key sent \"%s\"\n", sent);
        failures++;
    }
    ncc_visual_key('7');                      /* CCW */
    host_drain_panel(sent, sizeof(sent));
    if (!strstr(sent, "M4 S")) {
        printf("manual2test: FAIL the CCW key sent \"%s\"\n", sent);
        failures++;
    }
    ncc_visual_key('5');                      /* stop */
    host_drain_panel(sent, sizeof(sent));
    if (!strstr(sent, "M5")) {
        printf("manual2test: FAIL the stop key sent \"%s\"\n", sent);
        failures++;
    }
    host_pump_idle(64u);

    /* The stops are typed: `*` opens the minus side, `*` takes it and opens the
       plus one, `*` once more takes that and closes. */
    ncc_visual_key('*');
    if (!ncc_manual_field_active()) {
        puts("manual2test: FAIL `*` did not open the minus stop");
        failures++;
    }
    ncc_visual_key('0');
    ncc_visual_key('*');                      /* takes 0, opens the plus side */
    ncc_visual_key('7');
    ncc_visual_key('0');
    ncc_visual_key('*');                      /* takes 70, closes */
    if (ncc_manual_field_active()) {
        puts("manual2test: FAIL the stop field stayed open");
        failures++;
    }
    if (!ncc_manual_stop(0, 0, &value, &typed) || !typed ||
        fabsf(value) > 0.01f) {
        printf("manual2test: FAIL the X- stop is %.3f (typed=%d)\n",
               (double)value, (int)typed);
        failures++;
    }
    if (!ncc_manual_stop(0, 1, &value, &typed) || !typed ||
        fabsf(value - 70.0f) > 0.01f) {
        printf("manual2test: FAIL the X+ stop is %.3f (typed=%d)\n",
               (double)value, (int)typed);
        failures++;
    }

    /* `#` swaps the step for feeding while a key is held. */
    ncc_visual_key('#');
    if (!ncc_manual_continuous()) {
        puts("manual2test: FAIL `#` did not swap to the feed");
        failures++;
    }
    ncc_visual_key('8');                      /* X- toward the minus stop */
    host_drain_panel(sent, sizeof(sent));
    if (!strstr(sent, "$J=G91 X-")) {
        printf("manual2test: FAIL the held X- key sent \"%s\"\n", sent);
        failures++;
    }
    ncc_manual_hold(0);                       /* the key came up */
    host_pump_idle(64u);

    /* Zero writes the work offset for the picked axis. */
    ncc_visual_key('0');
    host_drain_panel(sent, sizeof(sent));
    if (!strstr(sent, "G10 L20 P0 X0")) {
        printf("manual2test: FAIL the zero key sent \"%s\"\n", sent);
        failures++;
    }
    host_pump_idle(64u);

    /* Touch-off types a value and `D` takes it. */
    ncc_visual_key('D');
    ncc_visual_key('1');
    ncc_visual_key('2');
    ncc_visual_key('D');
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

/* ncc's TOOLS: the tool table is a file like any other, so the screen is the
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
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    (void)ncc_visual_open(program);

    /* TOOLS makes the table when the card has none. */
    ncc_visual_select_mode(NCC_MODE_TOOLS);
    if (strcmp(ncc_visual_screen_name(), "TOOLS") != 0 ||
        strcmp(ncc_visual_path(), tool) != 0) {
        printf("tools2test: FAIL TOOLS is \"%s\" at \"%s\"\n",
               ncc_visual_screen_name(), ncc_visual_path());
        return 1;
    }
    if (!ncc_visual_save() || !host_fs_read_text(tool, text, sizeof(text)) ||
        !strstr(text, "T1 ")) {
        printf("tools2test: FAIL the new table reads \"%s\"\n", text);
        failures++;
    }
    /* The screen is two halves: the table's rows in the pane under the header -
       the text the operator edits - and the tool the cursor is on drawn below
       it (bench: "top one is text lines, bottom one is tool view"). */
    ncc_visual_draw();
    if (!host_view_ink_in(NCC_PREVIEW_X + 8, NCC_PREVIEW_Y + 8,
                          NCC_PREVIEW_W - 16, NCC_PREVIEW_PANE_H - 16)) {
        puts("tools2test: FAIL the table is not drawn in the top pane");
        failures++;
    }
    if (!host_view_ink_in(NCC_TEXT_X + 4, NCC_TEXT_Y + 4, NCC_TEXT_W - 8,
                          NCC_TEXT_H - 8)) {
        puts("tools2test: FAIL the tool view is not drawn below the table");
        failures++;
    }

    /* The pad is the *table's* group on this screen, not the program's (bench:
       "on ttols - 3x3 is wrong here"): `1` adds a tool row where the pad's name
       stands, and the keys after it pick a word of the row the cursor is on,
       the key's number being the word's number (bench, 2026-09-28: "insert new
       on 1 and then each next number is edit selected field"). The `O` word is
       gone, so the fields after `R` shift up: `2` is `R`, `3` is `F`. The
       address never leaves the root - `A` still leaves the screen. */
    if (strcmp(ncc_visual_address(), "") != 0) {
        printf("tools2test: FAIL the pad address is \"%s\"\n",
               ncc_visual_address());
        failures++;
    }
    if (!ncc_visual_slot_label('1') ||
        strcmp(ncc_visual_slot_label('1'), "NEW") != 0) {
        printf("tools2test: FAIL the pad's first entry is \"%s\"\n",
               ncc_visual_slot_label('1') ? ncc_visual_slot_label('1') : "");
        failures++;
    }
    if (!ncc_visual_slot_label('2') ||
        strcmp(ncc_visual_slot_label('2'), "R") != 0 ||
        !ncc_visual_slot_label('3') ||
        strcmp(ncc_visual_slot_label('3'), "F") != 0) {
        printf("tools2test: FAIL the pad's field keys read \"%s\" / \"%s\"\n",
               ncc_visual_slot_label('2') ? ncc_visual_slot_label('2') : "",
               ncc_visual_slot_label('3') ? ncc_visual_slot_label('3') : "");
        failures++;
    }
    ncc_visual_key('1');
    if (!ncc_visual_save() || !host_fs_read_text(tool, text, sizeof(text)) ||
        !strstr(text, "T1 R0.8")) {
        printf("tools2test: FAIL the added row is not in \"%s\"\n", text);
        failures++;
    }
    /* The new row's own tool number is picked, so the digits type it; then `2`
       picks the radius and the digits type over it. */
    ncc_visual_key('9');
    ncc_visual_key('#');
    if (!ncc_visual_save() || !host_fs_read_text(tool, text, sizeof(text)) ||
        !strstr(text, "T9 R0.8")) {
        printf("tools2test: FAIL the new row's number typed into \"%s\"\n",
               text);
        failures++;
    }
    ncc_visual_key('2');
    ncc_visual_key('4');
    ncc_visual_key('#');
    if (!ncc_visual_save() || !host_fs_read_text(tool, text, sizeof(text)) ||
        !strstr(text, "R4")) {
        printf("tools2test: FAIL `2` did not pick the radius in \"%s\"\n",
               text);
        failures++;
    }

    /* Back to the program: it is the one that was open. */
    ncc_visual_select_mode(NCC_MODE_PROGRAM);
    if (strcmp(ncc_visual_screen_name(), "EDIT") != 0 ||
        strcmp(ncc_visual_path(), program) != 0) {
        printf("tools2test: FAIL leaving TOOLS left \"%s\" at \"%s\"\n",
               ncc_visual_screen_name(), ncc_visual_path());
        failures++;
    }

    /* The pointer lands on the tool the machine is using: a program that calls
       `T2` and a table that holds it puts the table's cursor on that row when
       TOOLS opens (bench: "we push tool to state of mashine but not jump
       around"). */
    {
        static const char *const two = "/D/nc/files/two.nc";
        static const char *const table =
            "T1 R0.8\n"
            "T2 R0.4\n";
        size_t cursor;
        bool on_t2 = false;

        if (!host_fs_write_text(tool, table) ||
            !host_fs_write_text(two, "G0 X1 Z1\nT2\nG0 X2 Z2\n") ||
            !ncc_visual_open(two)) {
            puts("tools2test: FAIL cannot write the two-tool fixtures");
            failures++;
        } else {
            ncc_visual_key('C');        /* the editor's line is the T2 one */
            ncc_visual_select_mode(NCC_MODE_TOOLS);
            cursor = ncc_visual_cursor();
            if (!host_fs_read_text(tool, text, sizeof(text))) {
                puts("tools2test: FAIL cannot read the table back");
                failures++;
            } else {
                const char *row = text;     /* the row the cursor names */
                size_t seen = 0u;
                size_t i;

                for (i = 0u; text[i] && seen < cursor; i++) {
                    if (text[i] == '\n') {
                        seen++;
                        row = text + i + 1u;
                    }
                }
                on_t2 = strncmp(row, "T2", 2) == 0;
            }
            if (!on_t2) {
                printf("tools2test: FAIL the pointer is on row %u, not the "
                       "program's T2\n", (unsigned)cursor);
                failures++;
            }
            ncc_visual_select_mode(NCC_MODE_PROGRAM);
        }
    }

    /* And once the machine has *run*, the pointer takes the machine's answer -
       the tool the run handed over - before it reads any file. */
    {
        static const char *const two = "/D/nc/files/two.nc";
        size_t cursor;
        bool on_t2 = false;
        unsigned guard;

        if (!ncc_visual_open(two)) {
            puts("tools2test: FAIL cannot open the two-tool program again");
            failures++;
        } else {
            ncc_visual_select_mode(NCC_MODE_RUN);
            host_pump_idle(32u);
            ncc_visual_key('3');            /* FULL: the run sends `T2` */
            for (guard = 0u; guard < 200000u; guard++) {
                if (!nc_sender_run_streaming() && host_machine_idle()) {
                    break;
                }
                host_pump(1u);
            }
            host_pump_idle(32u);
            ncc_visual_select_mode(NCC_MODE_TOOLS);
            cursor = ncc_visual_cursor();
            if (!host_fs_read_text(tool, text, sizeof(text))) {
                puts("tools2test: FAIL cannot read the table back again");
                failures++;
            } else {
                const char *row = text;
                size_t seen = 0u;
                size_t i;

                for (i = 0u; text[i] && seen < cursor; i++) {
                    if (text[i] == '\n') {
                        seen++;
                        row = text + i + 1u;
                    }
                }
                on_t2 = strncmp(row, "T2", 2) == 0;
            }
            if (!on_t2) {
                printf("tools2test: FAIL after a run the pointer is on row %u, "
                       "not the machine's T2\n", (unsigned)cursor);
                failures++;
            }
            ncc_visual_select_mode(NCC_MODE_PROGRAM);
        }
    }

    if (failures) {
        printf("tools2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("tools2test: PASS the tool table is a file the editor writes, and the "
         "program keeps its place");
    return 0;
}

/* The colour most of one *program* row's text band carries - ncc's layout this
   time (`ncc_layout.h`), the same way `host_pane_row_bg()` reads nc's. Row 0 is
   line 1 of the file: the pane's own row above it is the file's name. */
static uint32_t host_pane2_row_bg(int row)
{
    uint32_t seen[8];
    int counts[8];
    int distinct = 0;
    int best = -1;
    int y = NCC_TEXT_Y + 4 + (row + 1) * NCC_ROW_H + NCC_ROW_H / 2;
    int x0 = NCC_TEXT_X + NCC_LINE_NO_PAD +
             4 * ncc_col_width(LVDS_FONT_NORMAL);
    int x;

    for (x = x0; x < NCC_TEXT_X + NCC_TEXT_W - 6; x++) {
        uint32_t c = host_view_at(x, y);
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

/* ncc's marks, read off the glass: in EDIT the cursor's row is the bright
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
    ncc_document_t doc;
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
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    if (!ncc_visual_open(program)) {
        puts("block2test: FAIL the fixture does not load");
        return 1;
    }
    ncc_document_init(&doc);
    (void)ncc_file_load(&doc, program);
    view = ncc_document_g7x(&doc);
    if (!g7x_doc_line_path(&view, 3u, &first, &last)) {
        puts("block2test: FAIL the fixture has no block around line 4");
        return 1;
    }
    select_rgb = host_panel_rgb(ncc_col_select());
    block_rgb = host_panel_rgb(ncc_col_block());
    bg_rgb = host_panel_rgb(ncc_col_bg());
    if (block_rgb == bg_rgb || block_rgb == select_rgb) {
        puts("block2test: FAIL the block colour is not a mark of its own");
        return 1;
    }

    /* EDIT: walk the cursor onto a row inside the cycle and read every row. */
    while (ncc_visual_cursor() < 3u) {
        ncc_visual_key('C');
    }
    ncc_visual_draw();
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
    ncc_visual_select_mode(NCC_MODE_RUN);
    nc_sender_run_reset();
    while (nc_sender_run_display_line() < 3u) {
        ncc_visual_key('C');
    }
    ncc_visual_draw();
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

/* ncc's DRO, read off the picture: the strip across the middle is the machine's
   own - the panel's grey while nothing is happening, the run's green while it
   moves, the fault's red when something is wrong - and the machine's state word
   is on it. The state is said *once*: the header band never carries it (the
   bench's "i do see idle in two places ... only this one should remain"), and
   the strip is a band of the layout, so it never moves. */
static int host_label2test(void)
{
    static const char *const program = "/D/nc/files/label2.nc";
    static const char *const text =
        "G0 X52 Z2\n"
        "G1 X1 Y1 F500\n"
        "M5\n";
    uint32_t run_rgb = host_panel_rgb(ncc_col_run());
    uint32_t err_rgb = host_panel_rgb(ncc_col_error());
    uint32_t machine_rgb = host_panel_rgb(ncc_col_header());
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, text)) {
        puts("label2test: FAIL cannot write the fixture");
        return 1;
    }
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    if (!ncc_visual_open(program)) {
        puts("label2test: FAIL the fixture does not load");
        return 1;
    }
    ncc_visual_select_mode(NCC_MODE_RUN);
    host_pump_idle(32u);

    /* Idle: the strip is up - it is the line between the two halves - and it
       wears the machine's own grey, with the state word on it. */
    ncc_visual_draw();
    if (host_view_at(NCC_DRO_X + 3, NCC_DRO_Y + 3) != machine_rgb) {
        printf("label2test: FAIL an idle strip is 0x%06lX, not the machine's "
               "own colour\n",
               (unsigned long)host_view_at(NCC_DRO_X + 3, NCC_DRO_Y + 3));
        failures++;
    } else if (!host_strip_has_ink(NCC_DRO_X + NCC_DRO_W - 120, machine_rgb)) {
        puts("label2test: FAIL the idle strip has no state word");
        failures++;
    } else {
        puts("label2test: the strip says IDLE in the machine's own colour");
    }

    /* Running: the strip is green, the header band keeps its own colour (the
       state is not said twice), and the word is still on it. */
    ncc_visual_key('3');
    host_pump(1u);
    ncc_visual_draw();
    if (host_view_at(NCC_DRO_X + 3, NCC_DRO_Y + 3) != run_rgb) {
        puts("label2test: FAIL a running strip is not green");
        failures++;
    } else if (host_view_at(2, 2) == run_rgb ||
               host_view_at(2, 2) == err_rgb) {
        puts("label2test: FAIL the header band wears the strip's colour");
        failures++;
    } else {
        puts("label2test: the strip is green while the machine is in a run");
    }

    /* A fault takes the strip over: red with the state word still on it, and the
       green gone - a machine stopped by a problem must not still say "running".
       The alarm is the machine's own, raised here rather than read from a run
       that has to be timed. */
    nc_sender_run_reset();
    host_pump_idle(64u);
    cnc_alarm(EXEC_ALARM_HARD_LIMIT);
    ncc_visual_draw();
    if (!cnc_has_alarm()) {
        puts("label2test: FAIL the alarm did not hold");
        failures++;
    } else if (host_view_at(NCC_DRO_X + 3, NCC_DRO_Y + 3) != err_rgb) {
        printf("label2test: FAIL a faulted strip is 0x%06lX, not the fault "
               "colour\n",
               (unsigned long)host_view_at(NCC_DRO_X + 3, NCC_DRO_Y + 3));
        failures++;
    } else if (host_view_at(NCC_DRO_X + 3, NCC_DRO_Y + 3) == run_rgb) {
        puts("label2test: FAIL the faulted strip is still green");
        failures++;
    } else if (!host_strip_has_ink(NCC_DRO_X + NCC_DRO_W - 120, err_rgb)) {
        puts("label2test: FAIL the faulted strip lost its state word");
        failures++;
    } else {
        puts("label2test: the fault takes the strip over in red");
    }
    cnc_alarm(EXEC_ALARM_NOALARM);

    if (failures) {
        printf("label2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("label2test: PASS the strip is the machine's own, green in a run and "
         "red in a fault, and the state is said once");
    return 0;
}

/* ncc's pacer: a *unit* is what runs as one thing, and a new unit may only be
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
        "G0 X80 Z0\n"
        "M5\n";
    ncc_document_t doc;
    ncc_runtime_state_t rt;
    size_t prev_now = 0u;
    size_t prev_mark = 0u;
    unsigned mark_moves = 0u;
    bool mark_allowed[NCC_MAX_LINES];
    unsigned guard;
    int failures = 0;

    memset(mark_allowed, 0, sizeof(mark_allowed));

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, text)) {
        puts("pace2test: FAIL cannot write the fixture");
        return 1;
    }
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    if (!ncc_visual_open(program)) {
        puts("pace2test: FAIL the fixture does not load");
        return 1;
    }
    ncc_document_init(&doc);
    (void)ncc_file_load(&doc, program);
    ncc_visual_select_mode(NCC_MODE_RUN);
    host_pump_idle(32u);

    ncc_visual_key('3');                    /* FULL */
    prev_now = nc_sender_run_line();
    /* The units of the fixture, so the check knows which lines may be marked:
       a block's own first line, or a line outside every block. It is the run's
       own rule (`g7x_doc_block_containing`, `nc`'s answer), and it has to be
       the run's rule and not `g7x_doc_line_path`: for a `G70` that one answers
       with the range it re-cuts - rows of a block the run has already passed -
       so a check built on it would call the finish cut's own line unmarkable
       and a passed row markable, which is the mark bug itself. */
    {
        g7x_doc_t view = ncc_document_g7x(&doc);
        size_t i;

        for (i = 0u; i < doc.line_count; i++) {
            size_t first = i;
            size_t last = i;

            if (!g7x_doc_block_containing(&view, i, &first, &last) ||
                first == i) {
                mark_allowed[i] = true;
            }
        }
    }
    for (guard = 0u; guard < 200000u; guard++) {
        bool expanding_before = nc_sender_run_expanding();
        size_t now;

        (void)cnc_parse_cmd();
        cnc_dotasks();
        ncc_visual_idle_tasks();
        mcu_unit_test_advance_time(1000u);
        now = nc_sender_run_line();
        if (nc_sender_run_streaming() && now > prev_now) {
            size_t at = now > 0u ? now - 1u : 0u;
            size_t mark;

            if (at >= doc.line_count) {
                at = doc.line_count - 1u;
            }
            /* One unit at a time, as it can be seen from outside the run: a row
               *inside* a block is handed over while that block is being
               expanded - a contour is one cut, and its rows go out back to
               back - and never otherwise. A line that opens a unit, or stands
               on its own, is another matter: the pacer waits for the machine
               for it, by its own rule (`nc_sender_run_machine_idle()` - the planner
               and the interpolator are empty). A check out here samples the
               machine before the pacer runs and cannot see the instant the
               pacer decides in, so it does not re-ask that question; what it
               does pin is that no row of a block ever goes out under a block
               that is not being expanded. */
            if (!mark_allowed[at] && !expanding_before) {
                printf("pace2test: FAIL the sender's line moved onto line %u, "
                       "a row inside a block, while no block was being "
                       "expanded (it was on line %u)\n",
                       (unsigned)(at + 1u), (unsigned)(prev_now + 1u));
                failures++;
                break;
            }
            /* The mark is a line the sender has already handed over. */
            mark = nc_sender_run_display_line();
            if (mark > at) {
                printf("pace2test: FAIL the pane marks line %u while the sender "
                       "has handed over up to %u\n",
                       (unsigned)(mark + 1u), (unsigned)(at + 1u));
                failures++;
                break;
            }
        }
        prev_now = now;
        /* And the mark is the machine's own line, not the sender's: while the
           controller is executing one of the panel's blocks, the line the pane
           marks is the line that number came from (`--pace2test` is the only
           place both are visible at once, because the sender is a step ahead of
           the tool inside a block). */
        {
            size_t executing = 0u;

            if (nc_sender_run_executing_line(&executing) &&
                nc_sender_run_display_line() != executing) {
                printf("pace2test: FAIL the pane marks line %u while the "
                       "machine says it is on line %u\n",
                       (unsigned)(nc_sender_run_display_line() + 1u),
                       (unsigned)(executing + 1u));
                failures++;
                break;
            }
        }
        /* The pane's mark follows the run: it is the line in play, and the
           operator reads the program from it (bench: "on run - it still does not
           moves the cursors it stays at first line"). */
        if (nc_sender_run_display_line() != prev_mark) {
            /* And it only ever goes forward: a `G70` re-feeds the profile's own
               rows, which belong to a block the run has already passed. */
            if (nc_sender_run_display_line() < prev_mark) {
                printf("pace2test: FAIL the mark went back to line %u\n",
                       (unsigned)(nc_sender_run_display_line() + 1u));
                failures++;
                break;
            }
            prev_mark = nc_sender_run_display_line();
            mark_moves++;
            /* ... and it is the *unit* it marks, not the row the expansion is
               on: a cycle stays marked as the block it is (bench: "try to mark
               each one line inside g7x cycle. not stay in whole block as is"). */
            if (prev_mark >= sizeof(mark_allowed) / sizeof(mark_allowed[0]) ||
                !mark_allowed[prev_mark]) {
                printf("pace2test: FAIL the mark moved onto line %u, which "
                       "opens no block\n", (unsigned)(prev_mark + 1u));
                failures++;
                break;
            }
        }
        if (!nc_sender_run_streaming() && host_machine_idle()) {
            break;
        }
    }
    /* Two moves is the fixture's whole run: line 1, the `G71` block it opens,
       and the `G70` after it - the finish cut's own range rows belong to the
       block the run has already passed, and must not count as a move. */
    if (mark_moves < 2u) {
        printf("pace2test: FAIL the pane's mark moved %u times in a full run\n",
               mark_moves);
        failures++;
    }
    if (nc_sender_run_streaming()) {
        puts("pace2test: FAIL the run never ended");
        failures++;
    }
    /* The machine ran the program: the last line sends it to `X80 Z0`, which is
       X40 on the axis - the program's X is a diameter and the axis works in the
       radius. */
    host_pump_idle(64u);
    ncc_state_runtime(&rt);
    printf("pace2test: the run ended at X%.3f Z%.3f, sender line %u\n",
           (double)rt.x, (double)rt.z, (unsigned)(nc_sender_run_line() + 1u));
    if (fabs((double)rt.x - 40.0) > 0.5 || fabs((double)rt.z) > 0.5) {
        printf("pace2test: FAIL the machine did not reach the program's end\n");
        failures++;
    }
    /* And the run ends *on* its last line, with the machine stopped: a program
       whose last row is an `M5` left the mark a line short and the strip saying
       RUN on the bench ("it seems not to mark last m5 line and stays at run"),
       and the half of that the host can see is pinned here. */
    if (nc_sender_run_display_line() != doc.line_count - 1u) {
        printf("pace2test: FAIL the run stopped on line %lu, not its last one "
               "(%lu)\n", (unsigned long)(nc_sender_run_display_line() + 1u),
               (unsigned long)doc.line_count);
        failures++;
    }
    if (nc_sender_run_active() || nc_sender_run_streaming()) {
        puts("pace2test: FAIL the run is still armed after its last line");
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

/* The demo the release carries, read the way ncc reads it: a fresh card gets
   the sample program from `examples\`, and it loads, scans as the two numbered
   `G71` ranges with their `G70` finish cuts, and expands. The entries are the
   module's own - there is nothing about them on the card - so a pad press
   writes the shipped row and the program it lands in is what is saved. */
static int host_demo2test(void)
{
    static const char *const examples = "tools/ncc_ui_win/examples";
    static const char *const program = "/D/nc/files/lathe-demo.nc";
    char expanded[4096];
    ncc_document_t doc;
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
    host_init_core();
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    if (!ncc_visual_open(program)) {
        puts("demo2test: FAIL the demo does not load");
        return 1;
    }
    ncc_document_init(&doc);
    if (!ncc_file_load(&doc, program) || doc.line_count == 0u) {
        printf("demo2test: FAIL the demo does not load (%u lines)\n",
               (unsigned)doc.line_count);
        return 1;
    }
    printf("demo2test: the demo is %u lines\n", (unsigned)doc.line_count);
    {
        g7x_doc_t view = ncc_document_g7x(&doc);

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
    if (!host_ncc_expand(&doc, 0u, expanded, sizeof(expanded), NULL, NULL) ||
        !strstr(expanded, "G1 ")) {
        puts("demo2test: FAIL the demo does not expand");
        failures++;
    }

    /* A pad press writes the shipped entry: G7X `1` is the OD cycle, from RAM. */
    ncc_visual_key('4');
    ncc_visual_key('1');
    if (!ncc_visual_save()) {
        puts("demo2test: FAIL the program could not be written back");
        failures++;
    } else {
        char written[1024];

        if (!host_fs_read_text(program, written, sizeof(written)) ||
            !strstr(written, "G71 U0 R0 X0 Z0 F0 P0 Q0")) {
            printf("demo2test: FAIL the shipped entry is not in \"%s\"\n",
                   written);
            failures++;
        } else {
            puts("demo2test: the key wrote the shipped entry");
        }
    }

    if (failures) {
        printf("demo2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("demo2test: PASS the demo seeds a fresh card, scans as two cycles and "
         "expands, and a key writes the shipped entry");
    return 0;
}

/* Every screen has to hand its frame to the panel.

   On the machine `lvds_hstx_present()` is the copy from the PSRAM draw buffer
   into the SRAM scanout: a screen that draws a perfect frame and never calls it
   shows a black panel. The host draws straight into the frame and its
   `present()` only repaints the window, so the pixels cannot tell - which is
   why the first ncc firmware booted to a black screen with every host check
   green. The host counts the calls instead, and this insists on them: every
   screen, on every draw. */
static int host_present2test(void)
{
    unsigned before;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    /* No first start to get past: the entries are in RAM. */
    before = lvds_host_present_count();
    ncc_visual_draw();
    if (lvds_host_present_count() != before + 1u) {
        printf("present2test: FAIL the screen drew %u frames for the "
               "panel\n", lvds_host_present_count() - before);
        failures++;
    }

    /* And every screen, not only the one that comes up first. */
    ncc_visual_select_mode(NCC_MODE_MANUAL);
    before = lvds_host_present_count();
    ncc_visual_draw();
    if (lvds_host_present_count() != before + 1u) {
        puts("present2test: FAIL the MANUAL screen never reached the panel");
        failures++;
    }
    ncc_visual_select_mode(NCC_MODE_RUN);
    before = lvds_host_present_count();
    ncc_visual_draw();
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
   firmware's own copy of it once forgot to run the first start's clock, which
   left the logo up for ever (there is no logo now, but the loop is still the
   thing the panel really runs); the loop lives in the screen
   (`ncc_visual_pump()`) and this check runs it, with its own clock. */
/* One pass of the loop as both the firmware module and the window run it: ask
   the screen's pump, and draw when it says so. The pump keeps asking until a
   frame has actually been drawn, so a caller that ignored it would see it ask
   for ever - which is the point of it. */
static bool host_pump2(unsigned now_ms)
{
    if (!ncc_visual_pump(now_ms)) {
        return false;
    }
    ncc_visual_draw();
    return true;
}

static int host_pump2test(void)
{
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    /* A screen that has just come up has never been painted: the first pass
       draws, and the next one - nothing moving, nothing pressed - is quiet. */
    if (!host_pump2(1000u)) {
        puts("pump2test: FAIL the screen that came up was not drawn");
        failures++;
    }
    if (host_pump2(1100u)) {
        puts("pump2test: FAIL an idle screen asked for a frame");
        failures++;
    }

    /* A key asks for one. */
    ncc_visual_key('4');
    if (!host_pump2(1500u)) {
        puts("pump2test: FAIL a key did not ask for a frame");
        failures++;
    }

    /* And a screen with something of its own to show keeps the frames coming:
       MANUAL's held feed and the machine's strip need them, at the period and no
       faster. */
    ncc_visual_select_mode(NCC_MODE_MANUAL);
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
    puts("pump2test: PASS the screen's own loop draws when it should, and stays "
         "quiet when nothing has changed");
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
    ncc_document_t doc;
    int failures = 0;

    /* The pad's order, before anything is drawn or pressed: the digits run up
       the way the machine's keypad does, so `7` is the top-left cell. */
    if (ncc_pad_cell_key(0, 0) != '7' || ncc_pad_cell_key(0, 2) != '9' ||
        ncc_pad_cell_key(1, 1) != '5' || ncc_pad_cell_key(2, 0) != '1' ||
        ncc_pad_cell_key(2, 2) != '3') {
        printf("contour2test: FAIL the pad's cells are %c%c%c / %c%c%c / %c%c%c\n",
               ncc_pad_cell_key(0, 0), ncc_pad_cell_key(0, 1),
               ncc_pad_cell_key(0, 2), ncc_pad_cell_key(1, 0),
               ncc_pad_cell_key(1, 1), ncc_pad_cell_key(1, 2),
               ncc_pad_cell_key(2, 0), ncc_pad_cell_key(2, 1),
               ncc_pad_cell_key(2, 2));
        failures++;
    }

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, fixture)) {
        puts("contour2test: FAIL cannot write the fixture");
        return 1;
    }
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    if (!ncc_visual_open(program)) {
        puts("contour2test: FAIL the fixture does not load");
        return 1;
    }
    ncc_visual_select_mode(NCC_MODE_PROGRAM);

    /* 1. the pad opens on G7X's `7`, and that cell says what it is. */
    ncc_visual_key('4');
    if (strcmp(ncc_visual_address(), "4") != 0 ||
        !ncc_visual_slot_label('7') ||
        strcmp(ncc_visual_slot_label('7'), "PATH") != 0) {
        printf("contour2test: FAIL G7X's `7` reads \"%s\" at \"%s\"\n",
               ncc_visual_slot_label('7') ? ncc_visual_slot_label('7') : "",
               ncc_visual_address());
        failures++;
    }
    ncc_visual_key('7');
    if (!ncc_contour_active()) {
        puts("contour2test: FAIL `4` `7` did not open the path builder");
        return 1;
    }
    /* Its pad is the directions, not the card's entries. */
    if (!ncc_visual_slot_label('5') ||
        strcmp(ncc_visual_slot_label('5'), "END") != 0) {
        puts("contour2test: FAIL the builder's pad does not show its directions");
        failures++;
    }

    /* 2. one press, one row - and the point comes from the program, not from a
       memory of the pad: `G0 X52 Z2` is what the first press counts from. */
    ncc_visual_key('2');                        /* X+ */
    /* 3. the value that landed is picked, so the digits type the real number
       over the step's prefill, and `D` gives the pad its digits back. */
    ncc_visual_key('3');
    ncc_visual_key('0');
    ncc_visual_key('D');
    if (!ncc_visual_save()) {
        puts("contour2test: FAIL the program was not written back");
        return 1;
    }
    ncc_document_init(&doc);
    if (!ncc_file_load(&doc, program) || doc.line_count != 2u ||
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
    ncc_visual_key('4');                        /* Z- */
    (void)ncc_visual_save();
    ncc_document_init(&doc);
    if (!ncc_file_load(&doc, program) || doc.line_count != 3u ||
        strcmp(doc.lines[1], "G1 X30 Z2") != 0 ||
        strcmp(doc.lines[2], "G1 X30 Z1.5") != 0) {
        printf("contour2test: FAIL the second point is \"%s\"\n",
               doc.line_count > 2u ? doc.lines[2] : "");
        failures++;
    } else {
        puts("contour2test: the walk carries the axis that does not move");
    }

    /* 5. `#` steps the distance - but only once the point is settled. */
    ncc_visual_key('#');                        /* take the point */
    ncc_visual_key('#');                        /* step the distance */
    ncc_visual_key('2');                        /* X+ by the new step */
    ncc_visual_key('#');                        /* and take that one too */
    (void)ncc_visual_save();
    ncc_document_init(&doc);
    if (!ncc_file_load(&doc, program) || doc.line_count != 4u ||
        strcmp(doc.lines[3], "G1 X31 Z1.5") != 0) {
        printf("contour2test: FAIL the stepped point is \"%s\"\n",
               doc.line_count > 3u ? doc.lines[3] : "");
        failures++;
    } else {
        puts("contour2test: `#` steps the distance");
    }

    /* 6. `*` drops the point being entered - the row goes with it - and the pad
       is still up. */
    ncc_visual_key('2');                        /* a point, still being entered */
    ncc_visual_key('*');
    if (!ncc_contour_active()) {
        puts("contour2test: FAIL `*` closed the builder");
        failures++;
    }
    (void)ncc_visual_save();
    ncc_document_init(&doc);
    if (!ncc_file_load(&doc, program) || doc.line_count != 4u) {
        printf("contour2test: FAIL `*` left %u rows\n",
               (unsigned)doc.line_count);
        failures++;
    } else {
        puts("contour2test: `*` drops the point being entered");
    }

    /* 7. `5` ends the builder, and the rows already written stay - the program
       is an ordinary program, so it also expands. */
    ncc_visual_key('5');
    if (ncc_contour_active()) {
        puts("contour2test: FAIL `5` did not end the builder");
        failures++;
    }
    (void)ncc_visual_save();
    ncc_document_init(&doc);
    if (!ncc_file_load(&doc, program) || doc.line_count != 4u ||
        strcmp(doc.lines[1], "G1 X30 Z2") != 0 ||
        strcmp(doc.lines[3], "G1 X31 Z1.5") != 0) {
        puts("contour2test: FAIL `5` did not leave the rows it wrote");
        failures++;
    }
    {
        g7x_doc_t view = ncc_document_g7x(&doc);
        float x = 0.0f;
        float z = 0.0f;
        size_t i;
        bool any = false;

        for (i = 0u; i < doc.line_count; i++) {
            uint8_t words = 0u;

            if (nc_sender_emit_line_is_direct(doc.lines[i]) &&
                nc_sender_emit_line_point(doc.lines[i], &x, &z, 0, 0u, &words)) {
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
    ncc_visual_key('4');
    ncc_visual_key('7');
    ncc_visual_key('A');
    if (ncc_contour_active()) {
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
   extensions case-insensitively (`nc_has_suffix_ci()`); ncc compared with strcmp
   and so dropped every such file from the list, which is the bench's "it does
   not show their names properly nor does it seem to open them" on a card full of
   old programs. This writes the shapes a real card has and insists they are
   offered: a capital `.NC` and a capital `.TXT`, and a `presets` folder a PC
   left behind, which is the card's text and stays the card's - the entries are
   the module's own and the panel writes nothing back. */
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
    /* The preset folder a PC wrote: the entry names are capitals there. The
       folder is the test's own (the panel makes none), so make it first. */
    (void)fs_mkdir("/D/presets");
    if (!host_fs_write_text("/D/presets/41.TXT",
                            "OD ROUGH\nG71 U0 R0 X0 Z0 F0 P0 Q0\n")) {
        puts("case2test: FAIL cannot write the preset entry");
        return 1;
    }

    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);

    /* 1. the extensions, both ways. */
    if (!ncc_path_is_program("/D/nc/files/UPPER.NC") ||
        !ncc_path_is_program("/D/nc/files/lower.nc") ||
        ncc_path_is_program("/D/nc/files/UPPER.TXT") ||
        !ncc_path_is_text("/D/nc/files/UPPER.TXT")) {
        puts("case2test: FAIL the program and text extensions are not both "
             "cases");
        failures++;
    }

    /* 2. the list offers them, names and all. */
    (void)ncc_file_scan("/D/nc/files");
    for (i = 0; i < ncc_file_count(); i++) {
        const ncc_file_entry_t *e = ncc_file_entry(i);

        if (strcmp(e->name, "UPPER.NC") == 0) {
            saw_upper = true;
        }
        if (strcmp(e->name, "UPPER.TXT") == 0) {
            saw_upper_text = true;
        }
    }
    if (!saw_upper || !saw_upper_text) {
        printf("case2test: FAIL the list shows %d entries and the capital ones "
               "are %s/%s\n", ncc_file_count(),
               saw_upper ? "there" : "missing",
               saw_upper_text ? "there" : "missing");
        failures++;
    }

    /* 3. and one of them opens: a program written on a PC is a program. */
    if (!ncc_visual_open("/D/nc/files/UPPER.NC") ||
        !ncc_path_is_program(ncc_visual_path())) {
        puts("case2test: FAIL a capital .NC does not open as a program");
        failures++;
    }

    /* 4. the entries are the module's own, and the card is left alone: the
       `presets` file a PC wrote is still exactly what it was, and the entry the
       panel answers with comes from RAM. */
    if (!host_fs_read_text("/D/presets/41.TXT", text, sizeof(text)) ||
        strstr(text, "OD ROUGH") == 0) {
        puts("case2test: FAIL the card's own file was touched");
        failures++;
    }
    if (ncc_preset_read("41", 0, 0u, text, sizeof(text)) <= 0 ||
        strstr(text, "G71") == 0) {
        puts("case2test: FAIL the shipped entry is not in RAM");
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

   This is where the second machine-only fault hid. `ncc_file_selected_path()`
   used to *scan* the parent itself when the selection was `..` - and return
   without writing the path it was asked for. The caller then scanned whatever
   its uninitialised buffer held, so leaving a folder handed the driver a
   garbage path: on the bench that is the panel frozen mid-walk ("it is stuck").
   The question is a question again, and the check walks both ways around it. */
static int host_walk2test(void)
{
    char path[NCC_PATH_MAX];
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
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);

    /* In: the picker walks into a folder with `D`. */
    if (!ncc_file_scan("/D")) {
        puts("walk2test: FAIL the card's root does not list");
        return 1;
    }
    if (!ncc_file_scan("/D/nc/files")) {
        puts("walk2test: FAIL the programs folder does not list");
        return 1;
    }
    if (strcmp(ncc_file_dir(), "/D/nc/files") != 0) {
        printf("walk2test: FAIL the list is at \"%s\"\n", ncc_file_dir());
        return 1;
    }

    /* The `..` entry is there, and it names the folder above. */
    saw_up = false;
    for (i = 0; i < ncc_file_count(); i++) {
        if (strcmp(ncc_file_entry(i)->name, "..") == 0) {
            ncc_file_step(-i);            /* select it */
            saw_up = true;
            break;
        }
    }
    if (!saw_up) {
        puts("walk2test: FAIL the list has no `..`");
        return 1;
    }
    path[0] = '\0';
    if (!ncc_file_selected_path(path, sizeof(path)) ||
        strcmp(path, "/D/nc") != 0) {
        printf("walk2test: FAIL `..` answers \"%s\"\n", path);
        failures++;
    } else if (strcmp(ncc_file_dir(), "/D/nc/files") != 0) {
        /* Asking is not walking: the list must not move under the caller. */
        printf("walk2test: FAIL asking for the parent moved the list to \"%s\"\n",
               ncc_file_dir());
        failures++;
    }

    /* Out: the key handler scans what it was handed, and lands there. The walk
       is the operator's - `0` opens the card, the picker selects `nc`, `D`
       enters it - so this is the screen's own path and not a direct scan. */
    ncc_visual_key('0');
    if (!host_file2_select("nc")) {
        puts("walk2test: FAIL the card's root has no `nc` folder");
        return 1;
    }
    ncc_visual_key('D');
    if (strcmp(ncc_visual_screen_name(), "FILES") != 0 ||
        strcmp(ncc_file_dir(), "/D/nc") != 0) {
        printf("walk2test: FAIL walking up lands at \"%s\" on \"%s\"\n",
               ncc_file_dir(), ncc_visual_screen_name());
        failures++;
    }
    /* and back down, to prove the walk is a walk and not one lucky answer */
    if (!host_file2_select("files")) {
        puts("walk2test: FAIL `nc` has no `files` folder");
        return 1;
    }
    ncc_visual_key('D');
    if (strcmp(ncc_file_dir(), "/D/nc/files") != 0) {
        printf("walk2test: FAIL walking back down lands at \"%s\"\n",
               ncc_file_dir());
        failures++;
    }
    /* and up again, which is the path that used to hand out garbage: `..` is
       the first entry of a folder, so `D` on it is the way up. */
    ncc_visual_key('B');                  /* back to the first entry (`..`) */
    ncc_visual_key('D');
    if (strcmp(ncc_file_dir(), "/D/nc") != 0) {
        printf("walk2test: FAIL the second walk up lands at \"%s\"\n",
               ncc_file_dir());
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
   the meaning is a table (`ncc_vocab.c`, the one nc kept). It is what the editor
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
        { "T2 R0.8", 'T', "Tool number" },
        { "M3 S450", 'S', "Spindle speed" },
        { "G76 X0 Z0 P0 Q0 F0 I0 L0 R0", 'L', "Spring passes" }
    };
    int failures = 0;
    unsigned i;

    for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ncc_field_t fields[NCC_MAX_FIELDS];
        int count = ncc_fields(cases[i].line, fields, NCC_MAX_FIELDS);
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
            const char *label = ncc_vocab_label(cases[i].line, &fields[at]);

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

/* ncc's value editor: a line is cut into fields at its letters, the keys walk
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
    ncc_document_t doc;
    ncc_field_t fields[NCC_MAX_FIELDS];
    int count;
    int failures = 0;

    ncc_document_init(&doc);

    /* 1. the fields. */
    count = ncc_fields("N10 G0 X52.5 Z-2", fields, NCC_MAX_FIELDS);
    if (count != 4 || fields[0].letter != 'N' || fields[1].letter != 'G' ||
        fields[2].letter != 'X' || fields[3].letter != 'Z' ||
        fields[2].end - fields[2].value != 4u /* 52.5 */ ||
        fields[3].end - fields[3].value != 2u /* -2 */) {
        printf("edit2test: FAIL the fields of a row are wrong (%d)\n", count);
        failures++;
    }
    count = ncc_fields("(rough) G1 X30 Z-15", fields, NCC_MAX_FIELDS);
    if (count != 3 || fields[0].letter != 'G' || fields[2].letter != 'Z') {
        printf("edit2test: FAIL a comment became a field (%d)\n", count);
        failures++;
    }
    count = ncc_fields("T", fields, NCC_MAX_FIELDS);
    if (count != 1 || fields[0].letter != 'T' ||
        fields[0].value != fields[0].end) {
        puts("edit2test: FAIL a lone letter is not a field waiting for a number");
        failures++;
    }
    count = ncc_fields("G1X30", fields, NCC_MAX_FIELDS);
    if (count != 2 || fields[1].letter != 'X' ||
        fields[1].end - fields[1].value != 2u) {
        puts("edit2test: FAIL letters without spaces do not cut the line");
        failures++;
    }

    /* 2. picking, typing, and the rest of the line standing still. */
    (void)ncc_insert_line(&doc, 0u, "G1 X30 Z-15");
    if (!ncc_key(&doc, NCC_KEY_NEXT, 0) ||
        doc.field != 0 || doc.line_count != 1u) {
        puts("edit2test: FAIL `D` did not pick the line's first field");
        failures++;
    }
    (void)ncc_key(&doc, NCC_KEY_NEXT, 0);               /* to X */
    if (doc.field != 1) {
        puts("edit2test: FAIL `D` did not walk to the next field");
        failures++;
    }
    (void)ncc_key(&doc, NCC_KEY_DIGIT, '4');
    (void)ncc_key(&doc, NCC_KEY_DIGIT, '5');
    if (strcmp(doc.lines[0], "G1 X45 Z-15") != 0) {
        printf("edit2test: FAIL typing gave \"%s\"\n", doc.lines[0]);
        failures++;
    }
    (void)ncc_key(&doc, NCC_KEY_UP, 0);                 /* B: the sign */
    if (strcmp(doc.lines[0], "G1 X-45 Z-15") != 0) {
        printf("edit2test: FAIL the sign gave \"%s\"\n", doc.lines[0]);
        failures++;
    }
    (void)ncc_key(&doc, NCC_KEY_DOWN, 0);               /* C: the point */
    (void)ncc_key(&doc, NCC_KEY_DIGIT, '2');
    if (strcmp(doc.lines[0], "G1 X-45.2 Z-15") != 0) {
        printf("edit2test: FAIL the point gave \"%s\"\n", doc.lines[0]);
        failures++;
    }
    (void)ncc_key(&doc, NCC_KEY_DELETE, 0);             /* backspace a digit */
    if (strcmp(doc.lines[0], "G1 X-45. Z-15") != 0) {
        printf("edit2test: FAIL the backspace gave \"%s\"\n", doc.lines[0]);
        failures++;
    }
    (void)ncc_key(&doc, NCC_KEY_ACCEPT, 0);
    if (doc.field != -1) {
        puts("edit2test: FAIL `#` did not accept the value");
        failures++;
    }

    /* 3. and the fields either side of it were never touched. */
    count = ncc_fields(doc.lines[0], fields, NCC_MAX_FIELDS);
    if (count != 3 || fields[0].end - fields[0].value != 1u ||
        fields[2].end - fields[2].value != 3u /* -15 */) {
        printf("edit2test: FAIL the neighbours changed: \"%s\"\n",
               doc.lines[0]);
        failures++;
    }

    /* 4. nothing picked: the cursor, the line, the way in. */
    (void)ncc_insert_line(&doc, 1u, "G0 X52 Z2");
    {
        (void)ncc_key(&doc, NCC_KEY_DOWN, 0);
        if (doc.cursor != 1u) {
            printf("edit2test: FAIL `C` left the cursor on %u\n",
                   (unsigned)doc.cursor);
            failures++;
        }
        (void)ncc_key(&doc, NCC_KEY_UP, 0);
        if (doc.cursor != 0u) {
            printf("edit2test: FAIL `B` left the cursor on %u\n",
                   (unsigned)doc.cursor);
            failures++;
        }
        if (ncc_key(&doc, NCC_KEY_DIGIT, '4')) {
            puts("edit2test: FAIL a digit was taken with nothing picked "
                 "(it is the pad's)");
            failures++;
        }
    }

    /* 5. the pad. */
    ncc_document_init(&doc);
    (void)ncc_insert_line(&doc, 0u, "G0 X52 Z2");
    doc.cursor = 0u;
    if (!ncc_pad_open(&doc, "G7X") || !ncc_pad_active(&doc) ||
        doc.line_count != 2u || doc.cursor != 1u ||
        strcmp(doc.lines[1], "G7X") != 0) {
        printf("edit2test: FAIL the helper's name is \"%s\"\n",
               doc.line_count > 1u ? doc.lines[1] : "");
        failures++;
    }
    /* The rows a card block hands over: a leading-space row has already been
       merged onto the row above it by the reader (`ncc_block_row`), which is
       what "a row that starts with a space continues the row above" means in
       the file - the pad only ever sees such a row as a block's *first* one. */
    if (!ncc_pad_write(&doc, "G71 U1 R0.5 X0.5 Z0.5 F450 P10 Q20\n"
                                "N10 G1 X30 Z0 C2\n"
                                "N20 G1 X50 Z-15")) {
        puts("edit2test: FAIL the entry was not written");
        failures++;
    }
    if (ncc_pad_active(&doc) || doc.line_count != 4u ||
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
    if (doc.cursor != 1u || doc.field != 1) {
        printf("edit2test: FAIL the cursor is on %u with field %d\n",
               (unsigned)doc.cursor, doc.field);
        failures++;
    }

    /* 6. the press ends the pad, the way nc's menu did, and the open that
       follows - the cursor is on the row that landed - writes the next row
       directly under it. The walk that *does* stay up for every point is the
       path builder's own (`--contour2test`). */
    (void)ncc_key(&doc, NCC_KEY_ACCEPT, 0);             /* let the value go */
    if (!ncc_pad_open(&doc, "G7X")) {
        puts("edit2test: FAIL the pad did not open again");
        failures++;
    }
    if (!ncc_pad_write(&doc, "N30 G1 X60 Z-15") || doc.line_count != 5u ||
        strcmp(doc.lines[2], "N30 G1 X60 Z-15") != 0 || doc.cursor != 2u) {
        printf("edit2test: FAIL the second press wrote \"%s\"\n",
               doc.line_count > 2u ? doc.lines[2] : "");
        failures++;
    }
    ncc_pad_close(&doc);
    if (ncc_pad_active(&doc) || doc.line_count != 5u ||
        strcmp(doc.lines[2], "N30 G1 X60 Z-15") != 0) {
        puts("edit2test: FAIL closing the pad took a written row with it");
        failures++;
    }

    /* 7. and leaving a pad that wrote nothing takes the name line back. */
    ncc_document_init(&doc);
    (void)ncc_insert_line(&doc, 0u, "G0 X52 Z2");
    doc.cursor = 0u;
    (void)ncc_pad_open(&doc, "WORD");
    ncc_pad_close(&doc);
    if (ncc_pad_active(&doc) || doc.line_count != 1u || doc.cursor != 0u ||
        strcmp(doc.lines[0], "G0 X52 Z2") != 0) {
        puts("edit2test: FAIL a pad that wrote nothing left its name behind");
        failures++;
    }

    /* 8. the words that finish a line. A row that starts with a space joins the
       line the press began on - not a line of its own, and not the pad's name
       line - and the word it names is picked, so the digits type its number:
       ` F`, ` C0` and ` R0` land at the end of that line, ` N` at its front,
       where a line number belongs (bench, 2026-09-28: "appends are append for
       all plus prepend for n"). */
    ncc_document_init(&doc);
    (void)ncc_insert_line(&doc, 0u, "G1 X30 Z0");
    doc.cursor = 0u;
    if (!ncc_pad_open(&doc, "WORD")) {
        puts("edit2test: FAIL the WORD pad did not open");
        failures++;
    }
    if (!ncc_pad_write(&doc, " F") || doc.line_count != 1u ||
        strcmp(doc.lines[0], "G1 X30 Z0 F") != 0 || doc.cursor != 0u ||
        doc.field != 3) {
        printf("edit2test: FAIL the feed joined \"%s\" at %u with field %d\n",
               doc.line_count > 0u ? doc.lines[0] : "",
               (unsigned)doc.cursor, doc.field);
        failures++;
    }
    (void)ncc_key(&doc, NCC_KEY_DIGIT, '2');
    if (strcmp(doc.lines[0], "G1 X30 Z0 F2") != 0) {
        printf("edit2test: FAIL the typed feed reads \"%s\"\n", doc.lines[0]);
        failures++;
    }
    /* The pad ended with that press, so the next word opens it again on the
       line the cursor stands on - and ` N` goes to its front. */
    (void)ncc_key(&doc, NCC_KEY_ACCEPT, 0);
    if (!ncc_pad_open(&doc, "WORD")) {
        puts("edit2test: FAIL the WORD pad did not open for the line number");
        failures++;
    }
    if (!ncc_pad_write(&doc, " N50") || doc.line_count != 1u ||
        strcmp(doc.lines[0], "N50 G1 X30 Z0 F2") != 0 || doc.field != 0) {
        printf("edit2test: FAIL the line number made \"%s\" with field %d\n",
               doc.line_count > 0u ? doc.lines[0] : "", doc.field);
        failures++;
    }
    /* And a row that does not start with a space stands on its own line, below
       the one that has just been finished. */
    (void)ncc_key(&doc, NCC_KEY_ACCEPT, 0);
    if (!ncc_pad_open(&doc, "TOOL")) {
        puts("edit2test: FAIL the TOOL pad did not open");
        failures++;
    }
    if (!ncc_pad_write(&doc, "M2") || doc.line_count != 2u ||
        strcmp(doc.lines[1], "M2") != 0 || doc.cursor != 1u) {
        printf("edit2test: FAIL the row after the join stands at %u: \"%s\"\n",
               (unsigned)doc.cursor,
               doc.line_count > 1u ? doc.lines[1] : "");
        failures++;
    }
    ncc_pad_close(&doc);

    /* With no line to join - the program is empty - the word still lands, on
       the line the pad would have written. */
    ncc_document_init(&doc);
    (void)ncc_pad_open(&doc, "WORD");
    if (!ncc_pad_write(&doc, " F") || doc.line_count != 1u ||
        strcmp(doc.lines[0], " F") != 0 || doc.field != 0) {
        printf("edit2test: FAIL the empty program's feed reads \"%s\"\n",
               doc.line_count > 0u ? doc.lines[0] : "");
        failures++;
    }
    ncc_pad_close(&doc);

    /* 9. what a landed row leaves picked. A row that is nothing but its command
       leaves *nothing* picked - there is no value to type on `G41`, and a pick
       would take `B` and `C` from the cursor (bench, 2026-09-28: "if i am over
       g41 - pressing b/c tries to edit it"). A row with values walks into the
       first of them, which is the word after the code; the tool table's row
       walks into the tool number the digits type over. */
    ncc_document_init(&doc);
    (void)ncc_insert_line(&doc, 0u, "G0 X52 Z2");
    doc.cursor = 0u;
    if (!ncc_pad_open(&doc, "TOOL") || !ncc_pad_write(&doc, "G41") ||
        doc.line_count != 2u || strcmp(doc.lines[1], "G41") != 0 ||
        doc.cursor != 1u || doc.field >= 0) {
        printf("edit2test: FAIL `G41` landed as \"%s\" with field %d\n",
               doc.line_count > 1u ? doc.lines[1] : "", doc.field);
        failures++;
    }
    if (!ncc_key(&doc, NCC_KEY_UP, 0) || doc.cursor != 0u ||
        strcmp(doc.lines[1], "G41") != 0) {
        printf("edit2test: FAIL `B` off the command left the cursor on %u\n",
               (unsigned)doc.cursor);
        failures++;
    }
    if (!ncc_key(&doc, NCC_KEY_DOWN, 0) || doc.cursor != 1u) {
        printf("edit2test: FAIL `C` back onto the command left the cursor on "
               "%u\n", (unsigned)doc.cursor);
        failures++;
    }
    ncc_pad_close(&doc);

    ncc_document_init(&doc);
    (void)ncc_insert_line(&doc, 0u, "G0 X52 Z2");
    doc.cursor = 0u;
    if (!ncc_pad_open(&doc, "G7X") || !ncc_pad_write(&doc, "G71 U3 R1") ||
        doc.field != 1 || strcmp(doc.lines[1], "G71 U3 R1") != 0) {
        printf("edit2test: FAIL the cycle left field %d on \"%s\"\n", doc.field,
               doc.line_count > 1u ? doc.lines[1] : "");
        failures++;
    }
    ncc_pad_close(&doc);

    ncc_document_init(&doc);
    (void)ncc_pad_open(&doc, "NEW");
    if (!ncc_pad_write(&doc, "T1 R0.8") || doc.field != 0 ||
        strcmp(doc.lines[0], "T1 R0.8") != 0) {
        printf("edit2test: FAIL the tool row left field %d on \"%s\"\n",
               doc.field, doc.line_count > 0u ? doc.lines[0] : "");
        failures++;
    }
    ncc_pad_close(&doc);

    if (failures) {
        printf("edit2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("edit2test: PASS the fields walk, the value is typed over, and the "
         "pad's words finish the line the press began on");
    return 0;
}

/* ncc's entries are RAM's, and there is no first start.

   The shipped table is copied into a RAM table when the module comes up, and
   the card is never written - so this check is about what the panel answers
   with, and about the one file a card may carry:

     1. the shipped entries are there, whole, before the card is asked anything;
     2. the two shapes that are easy to get wrong read right: a group is a name
        with no rows, and "a new line" is an entry with one empty row;
     3. what the table says is what a key writes;
     4. a card with no file is the shipped table, and nothing is created on it;
     5. `/D/ncc_presets.txt` replaces the addresses it names and leaves the rest
        alone. */
static int host_seedtest(void)
{
    char name[64];
    char rows[NCC_PRESET_ROW_MAX * 4];
    char text[128];
    fs_file_info_t info;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();

    /* 1. the shipped entries, in RAM, with the card out of it entirely. */
    if (ncc_preset_read("41", name, sizeof(name), rows, sizeof(rows)) != 1 ||
        strcmp(name, "G71") != 0 ||
        strcmp(rows, "G71 U0 R0 X0 Z0 F0 P0 Q0") != 0) {
        printf("seedtest: FAIL the shipped OD entry reads \"%s\" / \"%s\"\n",
               name, rows);
        failures++;
    } else {
        puts("seedtest: the shipped entries answer from RAM");
    }

    /* 2. the two shapes that are easy to get wrong: a slot that only holds
       children is its name with no rows, and "a new line" is an entry that
       writes one empty row. */
    if (ncc_preset_read("1", name, sizeof(name), 0, 0u) != 0 ||
        strcmp(name, "OPS") != 0) {
        printf("seedtest: FAIL the group answers \"%s\"\n", name);
        failures++;
    }
    if (ncc_preset_read("45", name, sizeof(name), rows, sizeof(rows)) != 1 ||
        strcmp(name, "PATH") != 0 || rows[0] != '\0') {
        printf("seedtest: FAIL the blank-line entry reads \"%s\"\n", rows);
        failures++;
    }

    /* 3. the round trip: what the RAM table says is what a key will write. */
    if (ncc_preset_read("34", name, sizeof(name), rows, sizeof(rows)) != 1 ||
        strcmp(name, "Mn") != 0 || strcmp(rows, "M") != 0) {
        printf("seedtest: FAIL U INC reads \"%s\" / \"%s\"\n", name, rows);
        failures++;
    }
    /* ...and the two words that *do* join a line carry the leading space: an
       `F` at its end, an `N` at its front. */
    if (ncc_preset_read("37", name, sizeof(name), rows, sizeof(rows)) != 1 ||
        strcmp(name, "Fn") != 0 || strcmp(rows, " F") != 0) {
        printf("seedtest: FAIL the feed reads \"%s\" / \"%s\"\n", name, rows);
        failures++;
    }
    if (ncc_preset_read("36", name, sizeof(name), rows, sizeof(rows)) != 1 ||
        strcmp(name, "Nn") != 0 || strcmp(rows, " N") != 0) {
        printf("seedtest: FAIL the line number reads \"%s\" / \"%s\"\n",
               name, rows);
        failures++;
    }
    if (ncc_preset_read("11", name, sizeof(name), rows, sizeof(rows)) != 4 ||
        strcmp(name, "SETUP") != 0 ||
        strcmp(rows, "G970 X0 U0 Z0 W0\nG971 X0 Z0 I0 E0\nG972 C0\nG973 P0") != 0) {
        printf("seedtest: FAIL the setup entry reads \"%s\"\n", rows);
        failures++;
    }
    if (ncc_preset_read("16", name, sizeof(name), rows, sizeof(rows)) != 1 ||
        strcmp(name, "CLAMP") != 0 || strcmp(rows, "G972 C0") != 0) {
        printf("seedtest: FAIL the clamp entry reads \"%s\" / \"%s\"\n",
               name, rows);
        failures++;
    }
    if (ncc_preset_read("19", name, sizeof(name), rows, sizeof(rows)) != -1) {
        puts("seedtest: FAIL an address nothing lives at answered");
        failures++;
    }

    /* 4. the card is the card's: the panel wrote nothing, and there is no
       presets folder for it to have written into. */
    if (host_fs_read_text("/D/presets/41.txt", text, sizeof(text)) ||
        fs_finfo("/D/presets", &info)) {
        puts("seedtest: FAIL the panel wrote a presets folder or file");
        failures++;
    }

    /* 5. the one card file: it replaces what it names and leaves the rest. */
    if (!host_fs_write_text(NCC_PRESET_FILE,
                            "[41] MINE\nG71 U2 R1 X0 Z0 F0 P0 Q0\n")) {
        puts("seedtest: FAIL cannot write the card's preset file");
        return 1;
    }
    ncc_presets_init();                 /* as a reboot would */
    if (!ncc_presets_sync()) {
        puts("seedtest: FAIL the card with the file did not answer");
        failures++;
    }
    if (ncc_preset_read("41", name, sizeof(name), rows, sizeof(rows)) != 1 ||
        strcmp(name, "MINE") != 0 ||
        strcmp(rows, "G71 U2 R1 X0 Z0 F0 P0 Q0") != 0) {
        printf("seedtest: FAIL the card's entry reads \"%s\" / \"%s\"\n",
               name, rows);
        failures++;
    }
    if (ncc_preset_read("23", name, sizeof(name), rows, sizeof(rows)) != 1 ||
        strcmp(rows, "M6") != 0) {
        puts("seedtest: FAIL an address the file does not name changed");
        failures++;
    }

    if (failures) {
        printf("seedtest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("seedtest: PASS the entries are in RAM, the card is left alone, and "
         "the one file overrides what it names");
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

/* The picture the screen drew, read at one of its own points: the glass is
   mounted turned, so this is where a check looks to see what the operator
   reads (`lvds_hstx.h` owns the turn, and the panel's own drawing puts every
   primitive through it too). */
static uint32_t host_view_at(int x, int y)
{
    return host_frame_at(LVDS_PANEL_X(x, y), LVDS_PANEL_Y(x, y));
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
    printf("ncc_ui: fs root %s\n", g_files_root[0] ? g_files_root : "nc-files");
    dir = fs_opendir("/D");
    if (!dir) {
        puts("ncc_ui: /D is not mounted");
        return 1;
    }
    while (fs_next_file(dir, &info)) {
        printf("  %-4s %8lu  %s\n", info.is_dir ? "dir" : "file",
               (unsigned long)info.size, info.full_name);
        count++;
    }
    fs_close(dir);
    fflush(stdout);
    printf("ncc_ui: %u entries\n", count);
    return 0;
}

/* The boot picture as a .bmp: what the glass holds for the first second and a
   half, the build's stamp included. The host's own boot is instant (see
   `host_init_core()`), so this raises the splash itself. */
static int host_dump_boot(const char *path)
{
    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    ncc_boot_start();
    ncc_visual_draw();
    if (!lvds_host_save_bmp(path)) {
        fprintf(stderr, "nc_ui: cannot write %s\n", path);
        return 1;
    }
    printf("nc_ui: wrote the boot picture to %s\n", path);
    return 0;
}

/* ncc's own screen as a .bmp - the panel alone, like `--dump` for nc's - so the
   new layout can be looked at without the station's window. */
static int host_dump_ncc(const char *path)
{
    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);             /* past the first start's logo */
    /* The card's state decides what is open: the demo is opened only when the
       panel came up with nothing, so a dump of TOOLS or MANUAL shows the file
       that screen really has - not the program the dump wanted to look at. */
    if (!ncc_visual_path()[0]) {
        if (!ncc_visual_open("/D/nc/files/lathe-demo.nc")) {
            (void)ncc_visual_open("/D/nc/files/screen.nc");
        }
    }
    /* `--keys` presses ncc's own keys, one character at a time, so a frame of a
       screen that only appears after input can be looked at. */
    if (g_key_script[0]) {
        const char *p = g_key_script;

        while (*p) {
            while (*p == ',' || *p == ' ') {
                p++;
            }
            if (*p) {
                ncc_visual_key(*p);
            }
            while (*p && *p != ',') {
                p++;
            }
        }
    }
    ncc_visual_draw();
    if (!lvds_host_save_bmp(path)) {
        fprintf(stderr, "ncc_ui: cannot write %s\n", path);
        return 1;
    }
    printf("ncc_ui: wrote ncc's %dx%d screen to %s\n", LVDS_VIEW_WIDTH,
           LVDS_VIEW_HEIGHT, path);
    return 0;
}

/* Typing a G code: the words the code takes come with it.

   The `WORD` pad's `Gn` entry writes a bare `G` on a line of its own with its
   value picked, and nc resolved that: its G field, on accept, looked the code up
   in the vocabulary and wrote the code's own line, then put the cursor on the
   first value, so the operator walked into `G71 U0 R0 X0 Z0 F0 P0 Q0` instead
   of being left with a `G71` and nowhere to go. nc2 dropped the vocabulary table
   and the word stayed bare (bench: "it does not prefill it ... it will not be
   resolved later to x z additional values").

   `Gn` writes a line of its own because that is what a G code is; the words that
   *finish* a line already on the screen are the ones the table writes with a
   leading space, and `37 Fn` is one of them: it adds the `F` to the line the
   cursor is on (bench: "now i do not see how to add feed?"). */
static int host_gcode2test(void)
{
    static const char *const program = "/D/nc/files/gcode.nc";
    ncc_document_t doc;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, "G0 X52 Z2\n")) {
        puts("gcode2test: FAIL cannot write the fixture");
        return 1;
    }
    ncc_visual_open(program);
    ncc_visual_select_mode(NCC_MODE_PROGRAM);

    /* 1. the entry writes the bare word on a line of its own and picks its
       value: the pad's name stood where the line now is. */
    ncc_visual_key('3');                        /* the WORD pad */
    ncc_visual_key('1');                        /* Gn */
    if (!ncc_visual_save()) {
        puts("gcode2test: FAIL the program was not written back");
        return 1;
    }
    ncc_document_init(&doc);
    if (!ncc_file_load(&doc, program) || doc.line_count != 2u ||
        strcmp(doc.lines[0], "G0 X52 Z2") != 0 ||
        strcmp(doc.lines[1], "G") != 0) {
        printf("gcode2test: FAIL the typed code reads \"%s\"\n",
               doc.line_count > 1u ? doc.lines[1] : "");
        failures++;
    }

    /* 2. finishing the field resolves the code into its own line, and the
       operator is told what the code is and what words it takes. */
    ncc_visual_key('7');
    ncc_visual_key('1');
    ncc_visual_key('#');
    if (!ncc_visual_save()) {
        puts("gcode2test: FAIL the resolved program was not written back");
        return 1;
    }
    ncc_document_init(&doc);
    if (!ncc_file_load(&doc, program) || doc.line_count != 2u ||
        strcmp(doc.lines[0], "G0 X52 Z2") != 0 ||
        strcmp(doc.lines[1], "G71 U0 R0 X0 Z0 F0 P0 Q0") != 0) {
        printf("gcode2test: FAIL the resolved line reads \"%s\"\n",
               doc.line_count > 1u ? doc.lines[1] : "");
        failures++;
    }
    if (!ncc_visual_status() || !strstr(ncc_visual_status(), "G71") ||
        !strstr(ncc_visual_status(), "OD roughing")) {
        printf("gcode2test: FAIL the status says \"%s\"\n",
               ncc_visual_status() ? ncc_visual_status() : "");
        failures++;
    }

    /* 3. a code the table holds with no words of its own is named and left
       alone - `G21` is a whole line by itself. The vocabulary is the module's,
       so it is asked directly here; the screen's own sentence is step 2. */
    ncc_document_init(&doc);
    if (!ncc_insert_line(&doc, 0u, "G21") ||
        ncc_gcode_resolve(&doc) != 21 || strcmp(doc.lines[0], "G21") != 0) {
        printf("gcode2test: FAIL G21 reads \"%s\"\n",
               doc.line_count > 0u ? doc.lines[0] : "");
        failures++;
    }
    if (!ncc_vocab_gcode_name(21) || ncc_vocab_gcode_template(21)) {
        puts("gcode2test: FAIL G21's vocabulary entry is wrong");
        failures++;
    }

    /* 4. the feed word: `37 Fn` under WORD puts ` F` **onto the line the cursor
       is on** - the resolved cycle - and picks its value, so a line can be
       given a feed without typing the letter (bench: "now i do not see how to
       add feed?"). */
    ncc_visual_key('#');            /* the resolved first value, let go */
    ncc_visual_key('3');            /* WORD */
    ncc_visual_key('7');            /* Fn */
    ncc_visual_key('2');
    ncc_visual_key('5');
    ncc_visual_key('0');
    if (!ncc_visual_save()) {
        puts("gcode2test: FAIL the feed program was not written back");
        return 1;
    }
    ncc_document_init(&doc);
    if (!ncc_file_load(&doc, program) || doc.line_count != 2u ||
        strcmp(doc.lines[1], "G71 U0 R0 X0 Z0 F0 P0 Q0 F250") != 0) {
        printf("gcode2test: FAIL the feed line reads \"%s\"\n",
               doc.line_count > 1u ? doc.lines[1] : "");
        failures++;
    }

    /* 5. a general `G1` brings the words a linear move takes - and not the
       contour's `C`/`R`, which belong to the G7X path builder, not to every
       linear move (bench: "g1 with c and r is wrong. it does fit only to the
       g7x path but not general code"): the vocabulary's template has neither,
       and the pad's `DRILL` writes it as its own line. */
    {
        const char *template = ncc_vocab_gcode_template(1);

        if (!template || !strstr(template, "G1") ||
            strchr(template, 'C') || strchr(template, 'R')) {
            printf("gcode2test: FAIL the G1 template is \"%s\"\n",
                   template ? template : "");
            failures++;
        }
    }
    ncc_visual_key('#');            /* the feed's value, let go */
    ncc_visual_key('6');            /* the PECK pad */
    ncc_visual_key('1');            /* DRILL */
    if (!ncc_visual_save()) {
        puts("gcode2test: FAIL the drill program was not written back");
        return 1;
    }
    ncc_document_init(&doc);
    if (!ncc_file_load(&doc, program) || doc.line_count != 3u ||
        strcmp(doc.lines[2], "G1 X0 Z0 F0") != 0) {
        printf("gcode2test: FAIL the drill line reads \"%s\"\n",
               doc.line_count > 2u ? doc.lines[2] : "");
        failures++;
    }

    /* 6. and *travelling* over a code leaves it alone. A lone `G` word used to be
       rewritten and picked as the cursor passed it - the resolve ran on every
       key the editor took - while a lone `M` word was left alone, so the two
       behaved differently at the compensation keys (bench, 2026-09-28: "it still
       wants to select this g41 or g42 while i travel along. and not m
       commands"). */
    if (!host_fs_write_text(program, "G0 X52 Z2\nG41\nG71\nM2\n")) {
        puts("gcode2test: FAIL cannot write the travel fixture");
        return 1;
    }
    if (!ncc_visual_open(program)) {
        puts("gcode2test: FAIL the travel fixture did not open");
        return 1;
    }
    ncc_visual_key('C');            /* down onto `G41` */
    ncc_visual_key('C');            /* onto the bare `G71`, which must stay bare */
    ncc_visual_key('C');            /* and on onto `M2`: the travel keeps moving */
    if (ncc_visual_cursor() != 3u) {
        printf("gcode2test: FAIL the cursor stopped at %u over the commands\n",
               (unsigned)ncc_visual_cursor());
        failures++;
    }
    if (!ncc_visual_save()) {
        puts("gcode2test: FAIL the travelled program was not written back");
        return 1;
    }
    ncc_document_init(&doc);
    if (!ncc_file_load(&doc, program) || doc.line_count != 4u ||
        strcmp(doc.lines[0], "G0 X52 Z2") != 0 ||
        strcmp(doc.lines[1], "G41") != 0 ||
        strcmp(doc.lines[2], "G71") != 0 ||
        strcmp(doc.lines[3], "M2") != 0) {
        printf("gcode2test: FAIL travelling left \"%s\" / \"%s\" / \"%s\"\n",
               doc.line_count > 1u ? doc.lines[1] : "",
               doc.line_count > 2u ? doc.lines[2] : "",
               doc.line_count > 3u ? doc.lines[3] : "");
        failures++;
    }

    if (failures) {
        printf("gcode2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("gcode2test: PASS a typed code becomes its own line, with its words");
    return 0;
}


/* The two colours the bench named.

   The word being typed wears nc's own box - white on the panel's black - and
   its legend wears the pale yellow the panel already marks a cycle's rows with,
   black on top of it. Before this the box was black on white and the legend was
   yellow text on the pane's grey (bench: "label is with yellow text - but
   should be same black with yellow background as before. better - pale yellow
   as in g7x group mark. text to be edit - white on black background"). */
/* The ink in a rectangle: every pixel that is not the colour most of it is.
   Glyphs read on their own background, whatever colour that background is. */
static int host_ink_in_rect(int x0, int y0, int w, int h)
{
    uint32_t seen[16];
    int counts[16];
    int distinct = 0;
    int best = 0;
    int total = 0;
    int x;
    int y;

    for (y = y0; y < y0 + h; y++) {
        for (x = x0; x < x0 + w; x++) {
            uint32_t c = host_view_at(x, y);
            int i;

            for (i = 0; i < distinct; i++) {
                if (seen[i] == c) {
                    counts[i]++;
                    break;
                }
            }
            if (i == distinct && distinct < 16) {
                seen[distinct] = c;
                counts[distinct] = 1;
                distinct++;
            }
            total++;
        }
    }
    for (x = 0; x < distinct; x++) {
        if (counts[x] > best) {
            best = counts[x];
        }
    }
    return total - best;
}

static int host_colour2test(void)
{
    static const char *const program = "/D/nc/files/colour.nc";
    uint32_t legend_bg;
    uint32_t box_bg;
    int word_x = NCC_TEXT_X + NCC_LINE_NO_PAD +
                 4 * ncc_col_width(LVDS_FONT_NORMAL);
    int word_y = NCC_TEXT_Y + 4 + 2 * NCC_ROW_H;
    int plain_ink;
    int boxed_ink;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, "G0 X52 Z2\nG970 X-5 U60 Z-60 W5\n")) {
        puts("colour2test: FAIL cannot write the fixture");
        return 1;
    }
    ncc_visual_open(program);
    ncc_visual_select_mode(NCC_MODE_PROGRAM);
    ncc_visual_key('C');            /* the cursor onto line 2 */
    ncc_visual_draw();
    /* The first word with nothing picked: its letters, on the row's own band. */
    plain_ink = host_ink_in_rect(word_x, word_y, 4 * ncc_col_width(LVDS_FONT_NORMAL),
                                 14);
    ncc_visual_key('D');            /* and its first word picked */
    ncc_visual_draw();
    /* The same letters inside the box. The renderer ends a string longer than
       its column count with "...", so a word handed over as a pointer into the
       line came out as "G..." - three dots and a letter where "G970" should be
       (bench: "g970 one edit shows ... still if i select 3 digit number"). The
       ink says which of the two was drawn. */
    boxed_ink = host_ink_in_rect(word_x, word_y, 4 * ncc_col_width(LVDS_FONT_NORMAL),
                                 14);
    if (boxed_ink < plain_ink * 3 / 4) {
        printf("colour2test: FAIL the word in the box carries %d ink pixels "
               "where the word on the row carries %d - it is not its own "
               "letters\n", boxed_ink, plain_ink);
        failures++;
    }

    /* The legend is drawn on the row above the cursor's - line 1's row here. */
    legend_bg = host_view_at(NCC_TEXT_X + 8, NCC_TEXT_Y + 4 + NCC_ROW_H +
                             NCC_ROW_H / 2);
    if (legend_bg != host_panel_rgb(ncc_col_block())) {
        printf("colour2test: FAIL the legend's band is 0x%06lX, not the pale "
               "yellow 0x%06lX\n", (unsigned long)legend_bg,
               (unsigned long)host_panel_rgb(ncc_col_block()));
        failures++;
    }
    /* The picked word's box, on the cursor's own row. */
    box_bg = host_view_at(NCC_TEXT_X + NCC_LINE_NO_PAD +
                          4 * ncc_col_width(LVDS_FONT_NORMAL) + 4,
                          NCC_TEXT_Y + 4 + 2 * NCC_ROW_H + NCC_ROW_H / 2);
    if (box_bg != host_panel_rgb(ncc_col_field_bg())) {
        printf("colour2test: FAIL the word's box is 0x%06lX, not the panel's "
               "black 0x%06lX\n", (unsigned long)box_bg,
               (unsigned long)host_panel_rgb(ncc_col_field_bg()));
        failures++;
    }
    /* And that black is not the white the key faces are. */
    if (ncc_col_field_bg() == ncc_col_pad_key()) {
        puts("colour2test: FAIL the box and the keys are the same colour");
        failures++;
    }

    if (failures) {
        printf("colour2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("colour2test: PASS the word is white on black, and its legend is "
         "black on pale yellow");
    return 0;
}

/* The chuck, and the `G972 C` that sizes it.

   A lathe drawing without the jaws is a stock floating in the air, and the
   `setup` block's `G972 C` is what says how much of the work they hold. The
   port drew the jaws *inside* the stock and then filled the stock over them, so
   the chuck was never on the glass and `C` had nothing to change (bench: "chuck
   g972 stopped working"). nc's own shape is back: a hatched bar along the
   bottom of the pane whose length grows with `C`. The check reads the glass:
   the bar's own colour is counted with a small clamp and with a large one. */
static int host_count_colour_in(int x0, int y0, int w, int h,
                                lvds_color_t colour)
{
    uint32_t want = host_panel_rgb(colour);
    int count = 0;
    int x;
    int y;

    for (y = y0; y < y0 + h; y++) {
        for (x = x0; x < x0 + w; x++) {
            if (host_view_at(x, y) == want) {
                count++;
            }
        }
    }
    return count;
}

static int host_chuck2test(void)
{
    static const char *const program = "/D/nc/files/chuck.nc";
    int small;
    int big;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program,
                            "G970 X-5 U60 Z-60 W5\n"
                            "G971 X50 Z50 I0 E0\n"
                            "G972 C5\n"
                            "G973 P7\n"
                            "G0 X52 Z2\n"
                            "G1 X30 Z0\n")) {
        puts("chuck2test: FAIL cannot write the fixture");
        return 1;
    }
    ncc_visual_open(program);
    ncc_visual_select_mode(NCC_MODE_PROGRAM);
    ncc_visual_draw();
    small = host_count_colour_in(NCC_PREVIEW_X, NCC_PREVIEW_Y,
                                 NCC_PREVIEW_W, NCC_PREVIEW_PANE_H,
                                 ncc_col_prev_chuck());
    if (small <= 0) {
        puts("chuck2test: FAIL the jaws are not on the glass at all");
        failures++;
    }

    /* The same stock, a longer clamp. */
    if (!host_fs_write_text(program,
                            "G970 X-5 U60 Z-60 W5\n"
                            "G971 X50 Z50 I0 E0\n"
                            "G972 C25\n"
                            "G973 P7\n"
                            "G0 X52 Z2\n"
                            "G1 X30 Z0\n")) {
        puts("chuck2test: FAIL cannot rewrite the fixture");
        return 1;
    }
    ncc_visual_open(program);
    ncc_visual_draw();
    big = host_count_colour_in(NCC_PREVIEW_X, NCC_PREVIEW_Y,
                               NCC_PREVIEW_W, NCC_PREVIEW_PANE_H,
                               ncc_col_prev_chuck());
    if (big <= small) {
        printf("chuck2test: FAIL C25 draws %d pixels of jaws, C5 draws %d\n",
               big, small);
        failures++;
    }

    if (failures) {
        printf("chuck2test: FAILED (%d)\n", failures);
        return 1;
    }
    printf("chuck2test: PASS the jaws are on the glass and G972 C sizes them "
           "(%d then %d pixels)\n", small, big);
    return 0;
}

/* The pad's keys: square, with the top-right corner cut at 45 degrees.

   nc's own keys were cut a quarter of their height (`NC_KEY_CHAMFER_DIVISOR`),
   and the bench asked for the squares and the cut back on the 3x3. The check
   reads the glass: the corner pixel is the page's ground and the same column a
   quarter of a key lower is the key's own colour. */
static int host_chamfer2test(void)
{
    uint32_t bg_rgb;
    uint32_t key_rgb;
    int cell_w = NCC_PAD_W / 3;
    int cell_h = NCC_PAD_H / 3;
    int chamfer = cell_h / NCC_KEY_CHAMFER_DIVISOR;
    int failures = 0;
    int col;
    int row;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    ncc_visual_select_mode(NCC_MODE_PROGRAM);
    ncc_visual_draw();
    bg_rgb = host_panel_rgb(ncc_col_bg());
    key_rgb = host_panel_rgb(ncc_col_pad_key());

    if (cell_w != cell_h) {
        printf("chamfer2test: FAIL the keys are %dx%d, not square\n",
               cell_w, cell_h);
        failures++;
    }
    for (row = 0; row < 3; row++) {
        for (col = 0; col < 3; col++) {
            int cx = NCC_PAD_X + col * cell_w;
            int cy = NCC_PAD_Y + row * cell_h;
            uint32_t corner = host_view_at(cx + cell_w - 2, cy);
            uint32_t under = host_view_at(cx + cell_w - 2, cy + chamfer + 2);

            if (corner != bg_rgb) {
                printf("chamfer2test: FAIL cell %d's corner is 0x%06lX, not "
                       "the page's ground\n", row * 3 + col + 1,
                       (unsigned long)corner);
                failures++;
            }
            if (under != key_rgb) {
                printf("chamfer2test: FAIL cell %d has no key under the cut "
                       "(0x%06lX)\n", row * 3 + col + 1, (unsigned long)under);
                failures++;
            }
        }
    }

    if (failures) {
        printf("chamfer2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("chamfer2test: PASS the pad's keys are square and cut a quarter at "
         "the top-right");
    return 0;
}

/* `#`: the preview's key, except where the operator is typing.

   This is ncc's one new screen key, and the order is what makes it work: a
   picked value takes `#` as the editor's accept (exactly as nc2 had it), and
   everywhere else it switches the DIN layer - the drawing's rulers and
   dimension callouts - on and off. The path builder takes it as its step key
   too, which `--contour2test` pins. */
static int host_preview2test(void)
{
    static const char *const program = "/D/nc/files/preview.nc";
    bool before;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program,
                            "G0 X52 Z2\n"
                            "G71 U1 R0.5 X0.5 Z0.5 F450 P10 Q20\n"
                            "G80\n")) {
        puts("preview2test: FAIL cannot write the fixture");
        return 1;
    }
    ncc_visual_open(program);
    ncc_visual_select_mode(NCC_MODE_PROGRAM);

    /* 1. nothing picked: `#` switches the drawing and says which face it is. */
    before = ncc_preview_din();
    ncc_visual_key('#');
    if (ncc_preview_din() == before) {
        puts("preview2test: FAIL `#` did not switch the preview");
        failures++;
    }
    if (!ncc_visual_status() || !strstr(ncc_visual_status(), "Preview")) {
        printf("preview2test: FAIL the switch says \"%s\"\n",
               ncc_visual_status() ? ncc_visual_status() : "");
        failures++;
    }
    ncc_visual_key('#');
    if (ncc_preview_din() != before) {
        puts("preview2test: FAIL `#` did not switch back");
        failures++;
    }

    /* 2. a value that is picked keeps the key: `#` is the editor's accept
       there, and the drawing must not change under the operator's typing. */
    ncc_visual_key('D');
    before = ncc_preview_din();
    ncc_visual_key('#');
    if (ncc_preview_din() != before) {
        puts("preview2test: FAIL the accept key switched the preview");
        failures++;
    }

    if (failures) {
        printf("preview2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("preview2test: PASS `#` switches the drawing, and a picked value keeps "
         "the key");
    return 0;
}

/* Editing the first line, and the file's own row above it.

   The pane's top row is the file the program belongs to (`ncc_draw_file_row()`)
   and the program is drawn under it, so the legend - which is named on the row
   above the cursor's - has a row of its own when the cursor is on line 1. nc
   worked this way because its pane had a file-name row; nc2 took the row out and
   the legend then painted over line 1 itself, which the bench saw as "editing
   the first line does mark it wrongly".

   The row is also the way back to the card: `B` from line 1 steps onto it, `D`
   opens the folder the file lives in *with the file selected*, and `C` steps
   back to the program. */
static int host_firstline2test(void)
{
    static const char *const program = "/D/nc/files/first.nc";
    uint32_t select_rgb;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program,
                            "G0 X52 Z2\n"
                            "G71 U1 R0.5 X0.5 Z0.5 F450 P10 Q20\n"
                            "N10 G1 X30 Z0\n"
                            "G80\n")) {
        puts("firstline2test: FAIL cannot write the fixture");
        return 1;
    }
    ncc_visual_open(program);
    ncc_visual_select_mode(NCC_MODE_PROGRAM);

    /* The cursor is on line 1; `D` picks its first field, which is what raises
       the legend. */
    ncc_visual_key('D');
    ncc_visual_draw();
    select_rgb = host_panel_rgb(ncc_col_select());

    /* Line 1 is marked on its own row - the first one under the file's. */
    if (host_pane2_row_bg(0) != select_rgb) {
        printf("firstline2test: FAIL line 1 is 0x%06lX, not the selection "
               "colour\n", (unsigned long)host_pane2_row_bg(0));
        failures++;
    }
    /* And the legend is not on it: the row above carries the file's name and
       the legend, never the mark. */
    if (host_view_at(NCC_TEXT_X + NCC_TEXT_W - 12, NCC_TEXT_Y + 4 +
                     NCC_ROW_H / 2) == select_rgb) {
        puts("firstline2test: FAIL the file's row carries the cursor's mark");
        failures++;
    }
    /* The legend and the file's name are up there, where the eye is. */
    if (!host_view_ink_in(NCC_TEXT_X + NCC_LINE_TEXT_PAD, NCC_TEXT_Y + 4,
                          NCC_TEXT_W - NCC_LINE_TEXT_PAD - 8, NCC_ROW_H - 2)) {
        puts("firstline2test: FAIL the file's row drew nothing");
        failures++;
    }
    /* The line itself is still readable on its own row. */
    if (!host_view_ink_in(NCC_TEXT_X + NCC_LINE_TEXT_PAD,
                          NCC_TEXT_Y + 4 + NCC_ROW_H,
                          NCC_TEXT_W - NCC_LINE_TEXT_PAD - 8, NCC_ROW_H - 2)) {
        puts("firstline2test: FAIL line 1 is not drawn under the legend");
        failures++;
    }

    /* 2. `B` steps up onto the file's row, and the row lights: it is filled
       with the selection colour and the name is drawn on its field background,
       the way nc's was. The sample is left of the name, where the fill shows. */
    {
        uint32_t before_row = host_view_at(NCC_TEXT_X + 4,
                                           NCC_TEXT_Y + 4 + NCC_ROW_H / 2);

    ncc_visual_key('#');
    ncc_visual_key('B');
    ncc_visual_draw();
    if (host_view_at(NCC_TEXT_X + 4, NCC_TEXT_Y + 4 + NCC_ROW_H / 2) !=
        select_rgb) {
        printf("firstline2test: FAIL the file's row is 0x%06lX when selected "
               "(0x%06lX before), not the selection colour\n",
               (unsigned long)host_view_at(NCC_TEXT_X + 4,
                                           NCC_TEXT_Y + 4 + NCC_ROW_H / 2),
               (unsigned long)before_row);
        failures++;
    }
    }

    /* 3. `D` there opens the folder the file lives in, on the file itself. */
    ncc_visual_key('D');
    if (strcmp(ncc_visual_screen_name(), "FILES") != 0) {
        printf("firstline2test: FAIL `D` left the screen on \"%s\"\n",
               ncc_visual_screen_name());
        failures++;
    } else {
        char selected[NCC_PATH_MAX];
        const char *dir = ncc_file_dir();

        if (strcmp(dir, "/D/nc/files") != 0) {
            printf("firstline2test: FAIL the list opened %s\n", dir);
            failures++;
        }
        if (!ncc_file_selected_path(selected, sizeof(selected)) ||
            strcmp(selected, program) != 0) {
            printf("firstline2test: FAIL the list selected \"%s\", not the "
                   "file it was opened from\n", selected);
            failures++;
        }
    }

    if (failures) {
        printf("firstline2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("firstline2test: PASS line 1 keeps its own row, the file's row takes "
         "the legend, and `D` on it opens its folder on the file");
    return 0;
}

/* Write the shipped entries out as the one card file the panel reads
   (`/D/ncc_presets.txt`, the `[address] name` format): what a key writes, as
   text the operator can read and edit. It is generated from the shipped table
   and nothing else, so the release's copy and the module's table cannot
   drift - regenerate it and compare. */
static int host_dump_presets(const char *path)
{
    char text[8192];
    size_t used = 0u;
    size_t i;
    FILE *fp;

    if (!path || !*path) {
        fprintf(stderr, "ncc_ui: --dump-presets needs a file\n");
        return 1;
    }
    for (i = 0u; i < ncc_preset_builtin_count(); i++) {
        const char *address = 0;
        const char *name = 0;
        const char *rows = 0;
        int n;

        if (!ncc_preset_builtin(i, &address, &name, &rows) || !address) {
            continue;
        }
        n = snprintf(text + used, sizeof(text) - used, "[%s] %s\n",
                     address, name ? name : "");
        if (n <= 0 || (size_t)n >= sizeof(text) - used) {
            fprintf(stderr, "ncc_ui: the entries do not fit one file\n");
            return 1;
        }
        used += (size_t)n;
        {
            const char *row = rows;

            while (row) {
                const char *end = strchr(row, '\n');
                size_t len = end ? (size_t)(end - row) : strlen(row);

                if (used + len + 2u >= sizeof(text)) {
                    fprintf(stderr, "ncc_ui: the entries do not fit one file\n");
                    return 1;
                }
                memcpy(text + used, row, len);
                used += len;
                text[used++] = '\n';
                row = end ? end + 1 : 0;
            }
        }
    }
    fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "ncc_ui: cannot write %s\n", path);
        return 1;
    }
    if (fwrite(text, 1u, used, fp) != used) {
        fclose(fp);
        fprintf(stderr, "ncc_ui: short write on %s\n", path);
        return 1;
    }
    fclose(fp);
    printf("ncc_ui: wrote %lu bytes of entries to %s\n",
           (unsigned long)used, path);
    return 0;
}

/* The editor's offset line, and where its radius comes from: the card's tool
   table, read on the program screen the same way RUN reads it - not a built-in
   row (bench, 2026-09-28: *"only line is seen here? or its takes no tool
   without running?"* - the table was only read on TOOLS and RUN, so the three
   shifts were drawn at whatever `R0.8` the built-in row carries, and the tool
   the operator had measured never reached the drawing).

   The card is read twice with one row changed: `R0` has no nose to shift by and
   the line is not drawn at all, `R3` is drawn - and, on a cycle, off the
   programmed profile, so the profile is still readable under it. The program
   names its tool (`T1`): the drawing reads the radius of the tool **the program
   is on**, so a program that names none gets no line - never the table's first
   row (bench, 2026-10-04: *"still takes first tool radius for g71"*). */
static int host_offset2test(void)
{
    static const char *const program = "/D/nc/files/offset.nc";
    static const char *const tool = "/D/nc/files/tool.t";
    static const char *const cycle =
        "T1\n"
        "G0 X52 Z2\n"
        "G71 U3 R1 X1 Z1 F500 P50 Q55\n"
        "N50 G1 X30 Z2\n"
        "G1 X30 Z-15 C2\n"
        "G1 X35 Z-15\n"
        "G1 X35 Z-25\n"
        "N55 G1 X52 Z-25\n"
        "G70 P50 Q55\n"
        "M2\n";
    int failures = 0;
    int pane_w = NCC_PREVIEW_W - 16;
    int pane_h = NCC_PREVIEW_PANE_H - 16;
    int pane_x = NCC_PREVIEW_X + 8;
    int pane_y = NCC_PREVIEW_Y + 8;
    int green;
    int profile;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, cycle)) {
        puts("offset2test: FAIL cannot write the fixture");
        return 1;
    }
    /* The table first says the tool has no nose radius: there is no shift to
       show, and the pane must carry no offset line at all. */
    if (!host_fs_write_text(tool, "T1 R0\n")) {
        puts("offset2test: FAIL cannot write the table");
        return 1;
    }
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    (void)ncc_visual_open(program);
    ncc_visual_select_mode(NCC_MODE_PROGRAM);
    ncc_visual_draw();
    green = host_count_colour_in(pane_x, pane_y, pane_w, pane_h, ncc_col_run());
    if (green != 0) {
        printf("offset2test: FAIL a tool with R0 still drew an offset line "
               "(%d pixels)\n", green);
        failures++;
    }

    /* Now the table's own radius - the one the operator measured. */
    if (!host_fs_write_text(tool, "T1 R3\n")) {
        puts("offset2test: FAIL cannot write the table");
        return 1;
    }
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    (void)ncc_visual_open(program);
    ncc_visual_select_mode(NCC_MODE_PROGRAM);
    ncc_visual_draw();
    green = host_count_colour_in(pane_x, pane_y, pane_w, pane_h, ncc_col_run());
    profile = host_count_colour_in(pane_x, pane_y, pane_w, pane_h,
                                   ncc_col_prev_profile());
    if (green == 0) {
        puts("offset2test: FAIL the card's R3 drew no offset line");
        failures++;
    }
    if (profile == 0) {
        puts("offset2test: FAIL the offset line covers the profile: the shift "
             "is not drawn off it");
        failures++;
    }

    if (failures) {
        printf("offset2test: FAILED (%d)\n", failures);
        return 1;
    }
    printf("offset2test: PASS the editor's offset line is the card's table "
           "(R0 draws none, R3 draws %d pixels away from the %d of profile)\n",
           green, profile);
    return 0;
}

/* The tool a cycle cuts with is the tool *the program is on*, read from the
   card's table by that number - never the table's own first row - and the nose
   radius belongs to the **contour the cycle builds**, not to the moment the
   `G41`/`G42` word is read: a program may call the tool after the word.

   The card carries two rows (T1 R2, T2 R3) and the program calls T2 *below* its
   `G42` - the bench's own order, `M3 S450` / `G42` / `T2` / `G0` / `G71` - so
   the cycle's finish contour is the tool-centre path X20 - 2*R3 = X14. Read as
   T1 it cuts at X16 (the first row's radius, one millimetre shallow), and read
   at the word itself the tool is not in the spindle *yet* and there is no
   radius at all.

   Recreates the bench's red alert of 2026-10-04: *"still takes first tool radius
   for g71"*. */
static int host_comp2test(void)
{
    static const char *const program = "/D/nc/files/comp2.nc";
    static const char *const tool = "/D/nc/files/tool.t";
    static const char *const table =
        "T1 R2\n"
        "T2 R3\n";
    static const char *const text =
        "G970 X-5 U60 Z-60 W5\n"
        "G971 X50 Z50 I0 E0\n"
        "G973 P7\n"
        "M3 S450\n"
        "G42\n"
        "T2\n"
        "G0 X52 Z2\n"
        "G71 U3 R1 X1 Z1 F500 P50 Q55\n"
        "N50 G1 X20 Z0\n"
        "G1 X20 Z-10\n"
        "N55 G1 X50 Z-10\n"
        "G80\n"
        "G70 P50 Q55\n"
        "M2\n";
    char sent[8192];
    float end_x = 0.0f;
    float end_z = 0.0f;
    ncc_document_t doc;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, text) || !host_fs_write_text(tool, table)) {
        puts("comp2test: FAIL cannot write the fixture");
        return 1;
    }
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    if (!ncc_visual_open(program)) {
        puts("comp2test: FAIL the fixture does not load");
        return 1;
    }
    ncc_document_init(&doc);
    if (!ncc_file_load(&doc, program) ||
        !host_ncc_expand(&doc, 0u, sent, sizeof(sent), &end_x, &end_z)) {
        puts("comp2test: FAIL the fixture does not expand");
        return 1;
    }
    /* The finish contour of the cycle: R3 puts the tool's centre at X14 (the
       tool on the right of the path), R2 would leave it at X16. */
    if (strstr(sent, "X16.000 Z0.000") != NULL ||
        strstr(sent, "X24.000 Z0.000") != NULL) {
        printf("comp2test: FAIL the cycle cut with the table's first row "
               "(R2):\n%s\n", sent);
        failures++;
    }
    if (strstr(sent, "X14.000 Z0.000") == NULL &&
        strstr(sent, "X26.000 Z0.000") == NULL) {
        printf("comp2test: FAIL the cycle emitted no R3 finish contour:\n%s\n",
               sent);
        failures++;
    }
    /* And the run's own record of the tool: the glyph and the brush ride the
       number the program spoke, not the card's first row. */
    nc_sender_run_reset();
    ncc_visual_select_mode(NCC_MODE_RUN);
    ncc_visual_key('3');
    host_pump(4u);
    if (nc_sender_run_tool() != 2) {
        printf("comp2test: FAIL the run's tool is %d, not the program's T2\n",
               nc_sender_run_tool());
        failures++;
    }
    nc_sender_run_stop();
    host_pump_idle(64u);
    nc_sender_run_reset();

    if (failures) {
        printf("comp2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("comp2test: PASS the cycle and the run take the tool the program "
         "called, not the table's first row");
    return 0;
}

/* The tool and the compensation the strip and the drawing wear are the state
   of the **machine's own line**, not of the line the sender has walked to: the
   sender runs ahead by design (it has to, to expand a cycle), so an answer
   taken from its walk showed `G42` while the `G41` part of the program was
   still being cut (bench, 2026-10-04: *"now g42 still appears while g41
   portion was not finished. so make it very state machine in g7x for tool and
   for offset"*).

   The card carries `T1 R2` / `T2 R3` and the program has two cycles, `T2` /
   `G41` then `T1` / `G42`, so both words change between them. Each cycle is
   armed and cut **on its own** (`1 SINGLE`/`2 FROM`'s path, which is where the
   bench saw it) and three things are pinned: the block arms with its own rows'
   words before a move goes out (the old code armed from a global that
   `nc_sender_run_start()` had just zeroed, so the second cycle said `T-1 G40` and cut
   `X20` instead of `X16`), the state stays that block's while the machine cuts
   it - the sender is a block ahead by design, the state is not - and the cut
   really is that block's own tool-centre path.

   What this cannot reach: the host's virtual machine cannot *pace* a program
   that hands over a `T`/`M3` line (the interpreter's `itp_sync()` needs the
   machine's own clock and spindle), so the full-run motion of such a program is
   a bench item - `ncc/TESTING.md`. */
static int host_state2test(void)
{
    static const char *const program = "/D/nc/files/state2.nc";
    static const char *const tool = "/D/nc/files/tool.t";
    static const char *const table =
        "T1 R2\n"
        "T2 R3\n";
    static const char *const text =
        "G971 X50 Z50 I0 E0\n"                                      /* 1 */
        "T2\n"                                                      /* 2 */
        "M3 S1000\n"                                                /* 3 */
        "G0 X52 Z2\n"                                               /* 4 */
        "G41\n"                                                     /* 5 */
        "G71 U3 R1 X1 Z1 F500 P50 Q55\n"                            /* 6 */
        "N50 G1 X20 Z0\n"                                           /* 7 */
        "G1 X20 Z-10\n"                                             /* 8 */
        "N55 G1 X50 Z-10\n"                                         /* 9 */
        "G80\n"                                                     /* 10 */
        "G42\n"                                                     /* 11 */
        "T1\n"                                                      /* 12 */
        "M3 S1000\n"                                                /* 13 */
        "G71 U3 R1 X1 Z1 F500 P100 Q200\n"                          /* 14 */
        "N100 G1 X20 Z0\n"                                          /* 15 */
        "G1 X20 Z-10\n"                                             /* 16 */
        "N200 G1 X50 Z-10\n"                                        /* 17 */
        "G80\n"                                                     /* 18 */
        "M2\n";                                                     /* 19 */
    char sent[8192];
    ncc_document_t doc;
    ncc_runtime_state_t rt;
    unsigned guard;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, text) || !host_fs_write_text(tool, table)) {
        puts("state2test: FAIL cannot write the fixture");
        return 1;
    }
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    if (!ncc_visual_open(program)) {
        puts("state2test: FAIL the fixture does not load");
        return 1;
    }
    ncc_document_init(&doc);
    if (!ncc_file_load(&doc, program) || doc.line_count != 19u) {
        puts("state2test: FAIL the fixture does not load");
        return 1;
    }

    /* One block at a time, the two cycles: `1 SINGLE`'s own path, which arms a
       single cycle and lets the prime pass walk the rows above it. (The host's
       virtual machine cannot *pace* a `T`/`M3` line - the interpreter's sync
       needs the machine's own clock and spindle - so a run that hands those
       lines over is a bench item; a block that carries motion only is not.) */
    ncc_state_runtime(&rt);
    {
        static const size_t cycle[2] = { 5u, 13u };    /* op 1, then op 2 */
        static const int want_tool[2] = { 2, 1 };
        static const g7x_comp_side_t want_comp[2] = { G7X_COMP_LEFT,
                                                      G7X_COMP_RIGHT };
        static const char *const want_cut[2] = { "X26.000 Z0.000",
                                                 "X16.000 Z0.000" };
        float start_x = rt.x;
        float start_z = rt.z;
        int part;

        for (part = 0; part < 2; part++) {
            nc_sender_run_reset();
            host_pump_idle(64u);
            if (!nc_sender_run_send_unit(host_sender_source(&doc),
                                         cycle[part])) {
                printf("state2test: FAIL cycle %d does not arm\n", part + 1);
                failures++;
                continue;
            }
            /* The block's own words, before a single move has gone out. */
            if (nc_sender_run_comp_mode() != want_comp[part] ||
                nc_sender_run_tool() != want_tool[part]) {
                printf("state2test: FAIL cycle %d arms as T%d %s, not T%d %s\n",
                       part + 1, nc_sender_run_tool(),
                       nc_sender_run_comp_mode() == G7X_COMP_LEFT ? "G41" :
                       (nc_sender_run_comp_mode() == G7X_COMP_RIGHT ? "G42" : "G40"),
                       want_tool[part],
                       want_comp[part] == G7X_COMP_LEFT ? "G41" : "G42");
                failures++;
            }
            /* And it stays that block's state while the machine cuts it: the
               sender is ahead by design, the state is not. */
            for (guard = 0u; guard < 200000u; guard++) {
                if (nc_sender_run_streaming() &&
                    (nc_sender_run_comp_mode() != want_comp[part] ||
                     nc_sender_run_tool() != want_tool[part])) {
                    printf("state2test: FAIL cycle %d reads T%d %s while it is "
                           "being cut\n", part + 1, nc_sender_run_tool(),
                           nc_sender_run_comp_mode() == G7X_COMP_LEFT ? "G41" :
                           (nc_sender_run_comp_mode() == G7X_COMP_RIGHT ? "G42"
                                                                  : "G40"));
                    failures++;
                    break;
                }
                if (host_machine_idle() && !nc_sender_run_streaming()) {
                    break;
                }
                host_pump(1u);
            }
            /* What the panel hands over for that block, walked the same way the
               run walks it (`nc_sender_emit_stream_begin()` at the block's line, the
               prime pass over the rows above it): the cycle's finish contour is
               its own tool-centre path - `T2 R3` under `G41` is
               X20 + 2*3 = X26, `T1 R2` under `G42` is X20 - 2*2 = X16.
               Uncompensated both would be X20. */
            if (!host_ncc_expand(&doc, cycle[part], sent, sizeof(sent), 0, 0) ||
                strstr(sent, want_cut[part]) == NULL) {
                printf("state2test: FAIL cycle %d did not cut its own "
                       "compensated path:\n%s\n", part + 1, sent);
                failures++;
            }
            nc_sender_run_stop();
            host_pump_idle(64u);
        }
        ncc_state_runtime(&rt);
        if (fabsf(rt.x - start_x) < 0.5f && fabsf(rt.z - start_z) < 0.5f) {
            printf("state2test: FAIL the machine never moved (X%.3f Z%.3f)\n",
                   (double)rt.x, (double)rt.z);
            failures++;
        }
    }
    nc_sender_run_reset();

    /* And two `G40` shapes, because "no shift" has to be visible in the pane as
       well as in the words: a plain program - no compensation word anywhere -
       runs the tool corner on the programmed point, so the nose's tangent is
       one radius inside it (X14 for a row of X20 with `T2 R3`), and a word
       written *inside* a cycle's contour is not a word of the program at all
       (the generator reads those rows as the profile), so neither the cut nor
       the pane may shift for it. Counted as a word, it took the material off as
       if `G41` while the log said `X20` (bench, 2026-10-04: *"g40 stock removing
       is overcompensated on x axis. log show 20 but in fact is same as g41"*). */
    {
        static const char *const plain_text =
            "G971 X50 Z50 I0 E0\n"
            "T2\n"
            "M3 S1000\n"
            "G0 X52 Z2\n"
            "G71 U3 R1 X1 Z1 F500 P50 Q55\n"
            "N50 G1 X20 Z0\n"
            "G1 X20 Z-10\n"
            "N55 G1 X50 Z-10\n"
            "G80\n"
            "M2\n";
        static const char *const inner_text =
            "G971 X50 Z50 I0 E0\n"
            "T2\n"
            "M3 S1000\n"
            "G0 X52 Z2\n"
            "G71 U3 R1 X1 Z1 F500 P50 Q55\n"
            "N50 G1 X20 Z0\n"
            "G41\n"                       /* among the contour rows */
            "G1 X20 Z-10\n"
            "N55 G1 X50 Z-10\n"
            "G80\n"
            "G1 X60 Z5\n"                 /* motion after the block */
            "M2\n";
        uint32_t stock = host_panel_rgb(ncc_col_prev_stock());
        uint32_t ground = host_panel_rgb(ncc_col_prev_bg());
        int top = 0;
        int bottom = 0;
        int left = 0;
        int right = 0;
        int col;
        int row;
        float mm_x;
        float mm_z;
        float edge = -1.0f;

        host_fs_write_text(program, plain_text);
        ncc_visual_init();
        ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
        ncc_visual_open(program);
        ncc_visual_select_mode(NCC_MODE_RUN);
        host_pump_idle(64u);
        nc_sender_run_reset();
        ncc_document_init(&doc);
        ncc_file_load(&doc, program);
        if (!nc_sender_run_send_unit(host_sender_source(&doc), 4u)) {
            puts("state2test: FAIL the plain cycle does not arm");
            failures++;
        } else {
            if (nc_sender_run_comp_mode() != G7X_COMP_OFF) {
                puts("state2test: FAIL a plain program is not `G40`");
                failures++;
            }
            for (guard = 0u; guard < 200000u; guard++) {
                if (host_machine_idle() && !nc_sender_run_streaming()) {
                    break;
                }
                host_pump(1u);
                ncc_visual_draw();
            }
            ncc_visual_draw();
            /* The stock's own block, then the material's edge over the OD (the
               same read `brush2test` makes). */
            for (row = NCC_PREVIEW_Y; row < NCC_PREVIEW_Y + NCC_PREVIEW_PANE_H;
                 row++) {
                for (col = NCC_PREVIEW_X;
                     col < NCC_PREVIEW_X + NCC_PREVIEW_W; col++) {
                    if (host_view_at(col, row) == stock) {
                        break;
                    }
                }
                if (col < NCC_PREVIEW_X + NCC_PREVIEW_W) {
                    break;
                }
            }
            top = row;
            for (col = NCC_PREVIEW_X; col < NCC_PREVIEW_X + NCC_PREVIEW_W; col++) {
                for (row = top; row < top + NCC_PREVIEW_PANE_H; row++) {
                    if (host_view_at(col, row) == stock) {
                        break;
                    }
                }
                if (row < top + NCC_PREVIEW_PANE_H) {
                    if (!left) {
                        left = col;
                    }
                    right = col;
                }
            }
            for (row = top; row < top + NCC_PREVIEW_PANE_H; row++) {
                if (host_view_at(left + 4, row) == stock) {
                    bottom = row;
                }
            }
            mm_x = 50.0f / (float)(bottom - top);
            mm_z = 50.0f / (float)(right - left);
            {
                int col_at = right - (int)(5.0f / mm_z);

                for (row = top + 2; row <= bottom; row++) {
                    if (host_view_at(col_at, row) == ground) {
                        edge = (float)(row - top) * mm_x;
                        break;
                    }
                }
            }
            if (fabsf(edge - 14.0f) > 0.6f) {
                printf("state2test: FAIL a plain `G40` cut left the material's "
                       "edge at X%.2f, not the tangent's X14 (row X20, R3)\n",
                       (double)edge);
                failures++;
            }
        }
        nc_sender_run_stop();
        host_pump_idle(64u);
        nc_sender_run_reset();

        /* The word written inside the contour: the cut is the programmed path,
           and the words after the block are the program's own - nothing shifted
           for a word the contour owns. */
        {
            host_fs_write_text(program, inner_text);
            ncc_document_init(&doc);
            if (!ncc_file_load(&doc, program)) {
                puts("state2test: FAIL the inner-word fixture does not load");
                failures++;
            } else if (!host_ncc_expand(&doc, 0u, sent, sizeof(sent), 0, 0) ||
                       strstr(sent, "X20.000 Z0.000") == NULL ||
                       strstr(sent, "X26.000 Z0.000") != NULL ||
                       strstr(sent, "X14.000 Z0.000") != NULL) {
                printf("state2test: FAIL a word among the contour rows changed "
                       "the cut:\n%s\n", sent);
                failures++;
            }
            ncc_visual_init();
            ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
            ncc_visual_open(program);
            ncc_visual_select_mode(NCC_MODE_RUN);
            nc_sender_run_reset();
            host_pump_idle(64u);
            /* The line *after* the block: every row up to it has been read, the
               contour's own word included. */
            if (!nc_sender_run_send_unit(host_sender_source(&doc), 10u)) {
                puts("state2test: FAIL the line after the block does not arm");
                failures++;
            } else if (nc_sender_run_comp_mode() != G7X_COMP_OFF ||
                       nc_sender_run_tool() != 2) {
                printf("state2test: FAIL a word among the contour rows shifted "
                       "the program (T%d %s)\n", nc_sender_run_tool(),
                       nc_sender_run_comp_mode() == G7X_COMP_LEFT ? "G41" :
                       (nc_sender_run_comp_mode() == G7X_COMP_RIGHT ? "G42" : "G40"));
                failures++;
            }
            nc_sender_run_stop();
            host_pump_idle(64u);
            nc_sender_run_reset();
        }
    }

    if (failures) {
        printf("state2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("state2test: PASS each cycle arms and cuts with the tool and the "
         "compensation its own rows stand under");
    return 0;
}

/* Leaving RUN for EDIT while the machine is cutting, and the editor's own
   safety: the program in the buffer belongs to the run.

   The check is built so that a mode switch which *re-reads the card* cannot
   hide: the card is given one program behind the panel's back after the run is
   armed, and the machine's own end position then says which of the two was cut
   (bench, 2026-09-28: *"if i go from run while it is runnig to edit i am stuck
   here + its need a safety not to edit [a] running file"*). */
static int host_runlock2test(void)
{
    static const char *const program = "/D/nc/files/runlock.nc";
    static const char *const on_card =    /* what the card is made to say */
        "G0 X52 Z2\n"
        "G1 X40 Z0 F200\n"
        "G1 X40 Z-10\n"
        "M5\n";
    static const char *const in_panel =   /* what the panel is running */
        "G0 X52 Z2\n"
        "G1 X30 Z0 F200\n"
        "G1 X30 Z-10\n"
        "M5\n";
    static const char *const writers = "123456789*D";
    ncc_runtime_state_t rt;
    const char *key;
    unsigned n;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program, in_panel)) {
        puts("runlock2test: FAIL cannot write the fixture");
        return 1;
    }
    ncc_visual_init();
    ncc_visual_tick(NCC_BOOT_HOLD_MS + 1000u);
    (void)ncc_visual_open(program);
    ncc_visual_select_mode(NCC_MODE_RUN);
    host_pump(8u);
    /* Behind the panel's back: the card now holds a different program, so a
       switch that re-reads it can be read off the machine's own end position. */
    if (!host_fs_write_text(program, on_card)) {
        puts("runlock2test: FAIL cannot write the second fixture");
        return 1;
    }
    ncc_visual_key('3');                   /* FULL */
    for (n = 0u; n < 400u; n++) {
        host_pump(1u);
    }
    if (!nc_sender_run_active()) {
        puts("runlock2test: FAIL the run is not in flight");
        return 1;
    }
    /* RUN -> EDIT, the way the mode key walks it. */
    ncc_visual_select_mode(NCC_MODE_PROGRAM);
    if (strcmp(ncc_visual_screen_name(), "EDIT") != 0 ||
        strcmp(ncc_visual_path(), program) != 0) {
        printf("runlock2test: FAIL the screen is \"%s\" on \"%s\"\n",
               ncc_visual_screen_name(), ncc_visual_path());
        failures++;
    }

    /* Every key that would write the text answers, and none of them lands: the
       run's own program is still what the machine is cutting (below). */
    for (key = writers; *key; key++) {
        ncc_visual_key(*key);
        if (strcmp(ncc_visual_status(),
                   "Program running - the file is the run's") != 0) {
            printf("runlock2test: FAIL `%c` was obeyed while the run had the "
                   "file (status \"%s\")\n", *key, ncc_visual_status());
            failures++;
        }
    }
    /* The keys that only look still work. */
    ncc_visual_key('#');
    if (strncmp(ncc_visual_status(), "Preview:", 8) != 0) {
        printf("runlock2test: FAIL `#` no longer switches the drawing "
               "(status \"%s\")\n", ncc_visual_status());
        failures++;
    }
    ncc_visual_key('C');
    if (ncc_visual_cursor() != 1u) {
        printf("runlock2test: FAIL `C` left the cursor on %u\n",
               (unsigned)ncc_visual_cursor());
        failures++;
    }

    /* Let the run finish: it has to end where the *panel's* program ends. */
    for (n = 0u; n < 200000u; n++) {
        if (!nc_sender_run_active() && !nc_sender_run_streaming()) {
            break;
        }
        host_pump(1u);
    }
    ncc_state_runtime(&rt);
    if (nc_sender_run_active()) {
        puts("runlock2test: FAIL the run never ended after the screen changed");
        failures++;
    }
    /* The program's X is a diameter and the machine's axis is the radius. */
    if (fabsf(rt.x * 2.0f - 30.0f) > 0.1f || fabsf(rt.z + 10.0f) > 0.05f) {
        printf("runlock2test: FAIL the machine ended at X%.3f Z%.3f - the card "
               "was re-read under the run (the panel's own program ends at "
               "X30.000 Z-10.000)\n", (double)(rt.x * 2.0f), (double)rt.z);
        failures++;
    }

    if (failures) {
        printf("runlock2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("runlock2test: PASS the editor would not write the run's file, and "
         "the run finished the program it started with");
    return 0;
}

/* The flags this file answers, in the order they are tried: the dumps
   first (they take a path), then one check per flag. -1 means no flag named a
   check, so main() opens the window. */
/* The clearing brush against the coordinates. The brush is the nose's own disc
   and the material goes below its upper arc in both modes. On `G41` the machine
   walks the tool's *centre* path (X26 for an X20 row with `R3`), so the tangent
   lands on X20; on `G40` the corner itself is on X20, so the tangent is one nose
   radius above it, X14. The check reads the glass, parks the run for the final
   frame so the glyph is out of the measurement, and then repeats it with a live
   `G54` offset - bench, 2026-09-04: *"only drawing seems to be wrong. ptah
   preview lines are in place as then need to be. so check position of actual
   clening brush vs sendded coordinates and real mashien coordinates"*. */
static int host_brush2test(void)
{
    static const char *const program = "/D/nc/files/brush.nc";
    ncc_document_t doc;
    ncc_preview_run_t run;
    ncc_tool_t tool;
    uint32_t stock = host_panel_rgb(ncc_col_prev_stock());
    uint32_t ground = host_panel_rgb(ncc_col_prev_bg());
    float mm_x;
    float mm_z;
    int top = 0;
    int bottom = 0;
    int left = 0;
    int right = 0;
    int pass;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program,
                            "G971 X50 Z50 I0 E0\n"
                            "G0 X52 Z2\n"
                            "G1 X20 Z0 F200\n"
                            "G1 X20 Z-20\n"
                            "G1 X50 Z-20\n"
                            "M2\n") ||
        !ncc_tool_from_line("T1 R3", &tool)) {
        puts("brush2test: FAIL cannot build the fixture");
        return 1;
    }
    ncc_document_init(&doc);
    if (!ncc_file_load(&doc, program)) {
        puts("brush2test: FAIL the fixture does not load");
        return 1;
    }

    for (pass = 0; pass < 3; pass++) {
        int step;
        int row;
        int col;
        float edge_x;
        float edge_z;
        float want_x;

        /* Pass 2 moves the machine's frame: the active `G54` is given an offset
           and the machine walks it, so the drawing has to subtract the offset
           before it maps the point. The same part must land on the same rows. */
        if (pass == 2) {
            if (!nc_sender_run_send_line("G10 L2 P1 X20 Z10") ||
                !nc_sender_run_send_line("G54")) {
                puts("brush2test: FAIL cannot set the G54 offset");
                return 1;
            }
            host_pump_idle(64u);
        }
        memset(&run, 0, sizeof(run));
        run.busy = true;
        run.screen_run = true;
        run.paint = true;
        run.tool = &tool;
        /* Pass 0: `G41` - the machine on the tool's centre path: the OD holds
           radius 13 (X26) from the face clearance to the shoulder's Z-17, then
           out to the stock's radius. Pass 1: `G40` - the rows themselves. Pass
           2: `G40` again, with `G54` moving the machine's frame by X20/Z10. */
        left = 0;
        right = 0;
        bottom = 0;
        for (step = 0; step <= 60; step++) {
            float t = (float)step / 60.0f;
            float off = pass == 0 ? 3.0f : 0.0f;
            float wx = pass == 2 ? 10.0f : 0.0f;
            float wz = pass == 2 ? 10.0f : 0.0f;

            if (t < 0.34f) {
                run.x = 10.0f + off + wx;
                run.z = off - (20.0f + off) * (t / 0.34f) + wz;
            } else {
                run.x = 10.0f + off + 15.0f * ((t - 0.34f) / 0.66f) + wx;
                run.z = -20.0f + wz;
            }
            run.full = step == 0;
            ncc_preview_draw(&doc, &run, NCC_PREVIEW_X, NCC_PREVIEW_Y,
                             NCC_PREVIEW_W, NCC_PREVIEW_PANE_H);
        }
        /* The glyph rides the point and covers the rows the cut is read on.
           One more frame with the run parked paints the mask whole and no
           tool, so the number below is the material's own edge. */
        run.busy = false;
        run.full = true;
        ncc_preview_draw(&doc, &run, NCC_PREVIEW_X, NCC_PREVIEW_Y,
                         NCC_PREVIEW_W, NCC_PREVIEW_PANE_H);
        /* The stock's own block is the ruler: X0 at its top row and X50 at its
           bottom, Z0 at its right column (the pane's whole length to the left). */
        for (row = NCC_PREVIEW_Y; row < NCC_PREVIEW_Y + NCC_PREVIEW_PANE_H;
             row++) {
            for (col = NCC_PREVIEW_X; col < NCC_PREVIEW_X + NCC_PREVIEW_W;
                 col++) {
                if (host_view_at(col, row) == stock) {
                    break;
                }
            }
            if (col < NCC_PREVIEW_X + NCC_PREVIEW_W) {
                break;
            }
        }
        top = row;
        for (col = NCC_PREVIEW_X; col < NCC_PREVIEW_X + NCC_PREVIEW_W; col++) {
            for (row = top; row < top + NCC_PREVIEW_PANE_H; row++) {
                if (host_view_at(col, row) == stock) {
                    break;
                }
            }
            if (row < top + NCC_PREVIEW_PANE_H) {
                if (!left) {
                    left = col;
                }
                right = col;
            }
        }
        for (row = top; row < top + NCC_PREVIEW_PANE_H; row++) {
            if (host_view_at(left + 4, row) == stock) {
                bottom = row;
            }
        }
        mm_x = 50.0f / (float)(bottom - top);
        mm_z = 50.0f / (float)(right - left);
        /* `G41` runs the machine on the tool-centre path (X26 for a X20 row
           with R3), so the nose's tangent lands on X20. `G40` runs the corner
           itself on X20, so the tangent is one nose radius above it: X14. */
        want_x = pass == 0 ? 20.0f : 14.0f;

        /* X: the material's edge over the OD's own columns. */
        row = top;
        {
            int col_at = right - (int)(10.0f / mm_z);

            edge_x = -1.0f;
            for (row = top + 2; row <= bottom; row++) {
                if (host_view_at(col_at, row) == ground) {
                    edge_x = (float)(row - top) * mm_x;
                    break;
                }
            }
        }
        /* Z: the material's edge over the shoulder's own rows. */
        edge_z = -1000.0f;
        for (row = top + (int)(14.0f / mm_x); row <= bottom; row++) {
            for (col = left; col <= right; col++) {
                if (host_view_at(col, row) == ground) {
                    float z = (float)(col - right) * mm_z;

                    if (z > edge_z) {
                        edge_z = z;
                    }
                    break;
                }
            }
        }
        if (fabsf(edge_x - want_x) > 0.6f || fabsf(edge_z + 20.0f) > 0.6f) {
            printf("brush2test: FAIL with %s the material's edge is at X%.2f "
                   "Z%.2f, wanted X%.2f Z-20\n",
                   pass == 0 ? "G41 (the machine on its centre path)" :
                   (pass == 1 ? "G40" : "G40 with the G54 offset"),
                   (double)edge_x, (double)edge_z, (double)want_x);
            failures++;
        }
    }

    if (failures) {
        printf("brush2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("brush2test: PASS the cut lands where the nose's tangent is: X20 on "
         "the G41 centre path, X14 under G40's corner (R3), and the same with "
         "a shifted G54 frame");
    return 0;
}

/* The program's frame against the drawing's: a `G8` program writes X as a
   radius, and the same part written in `G7` writes it as a diameter. Both have
   to land on the same row of the stock, because the stock and the drawing are
   in diameters. The check reads the profile line on the glass at one Z, not a
   text label: the labels say what the program wrote, and are meant to differ. */
static int host_frame2test(void)
{
    static const char *const g7_path = "/D/nc/files/frame-g7.nc";
    static const char *const g8_path = "/D/nc/files/frame-g8.nc";
    static const char *const g7_text =
        "G971 X50 Z50 I0 E0\n"
        "G0 X52 Z2\n"
        "G1 X20 Z0\n"
        "G1 X20 Z-20\n"
        "G1 X50 Z-20\n"
        "M2\n";
    static const char *const g8_text =
        "G8\n"
        "G971 X50 Z50 I0 E0\n"
        "G0 X26 Z2\n"
        "G1 X10 Z0\n"
        "G1 X10 Z-20\n"
        "G1 X25 Z-20\n"
        "M2\n";
    ncc_document_t doc;
    uint32_t stock = host_panel_rgb(ncc_col_prev_stock());
    uint32_t profile = host_panel_rgb(ncc_col_prev_profile());
    int row7 = -1;
    int row8 = -1;
    int expected = -1;
    int pass;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(g7_path, g7_text) ||
        !host_fs_write_text(g8_path, g8_text)) {
        puts("frame2test: FAIL cannot write the fixtures");
        return 1;
    }
    for (pass = 0; pass < 2; pass++) {
        const char *path = pass == 0 ? g7_path : g8_path;
        int top = 0;
        int bottom = 0;
        int left = 0;
        int right = 0;
        int row;
        int col;

        ncc_document_init(&doc);
        if (!ncc_file_load(&doc, path)) {
            printf("frame2test: FAIL %s does not load\n", path);
            return 1;
        }
        ncc_preview_draw(&doc, 0, NCC_PREVIEW_X, NCC_PREVIEW_Y,
                         NCC_PREVIEW_W, NCC_PREVIEW_PANE_H);
        /* The stock's own block is the ruler, as in brush2test. */
        for (row = NCC_PREVIEW_Y; row < NCC_PREVIEW_Y + NCC_PREVIEW_PANE_H;
             row++) {
            for (col = NCC_PREVIEW_X; col < NCC_PREVIEW_X + NCC_PREVIEW_W;
                 col++) {
                if (host_view_at(col, row) == stock) {
                    break;
                }
            }
            if (col < NCC_PREVIEW_X + NCC_PREVIEW_W) {
                break;
            }
        }
        top = row;
        for (col = NCC_PREVIEW_X; col < NCC_PREVIEW_X + NCC_PREVIEW_W; col++) {
            for (row = top; row < top + NCC_PREVIEW_PANE_H; row++) {
                if (host_view_at(col, row) == stock) {
                    break;
                }
            }
            if (row < top + NCC_PREVIEW_PANE_H) {
                if (!left) {
                    left = col;
                }
                right = col;
            }
        }
        for (row = top; row < top + NCC_PREVIEW_PANE_H; row++) {
            if (host_view_at(left + 4, row) == stock) {
                bottom = row;
            }
        }
        if (bottom <= top || right <= left) {
            puts("frame2test: FAIL the stock has no block to measure against");
            return 1;
        }
        /* Z-10 is inside the OD's own length: X20 there, in both programs. */
        col = right - (int)(10.0f * (float)(right - left) / 50.0f);
        row = -1;
        {
            int y;

            for (y = top; y <= bottom; y++) {
                if (host_view_at(col, y) == profile) {
                    row = y;
                    break;
                }
            }
        }
        if (pass == 0) {
            row7 = row;
            expected = top + (int)(0.4f * (float)(bottom - top) + 0.5f);
        } else {
            row8 = row;
        }
    }
    if (row7 < 0 || expected < 0) {
        printf("frame2test: FAIL the G7 profile is not on the glass (row %d)\n",
               row7);
        failures++;
    } else if (row7 < expected - 2 || row7 > expected + 2) {
        printf("frame2test: FAIL the G7 X20 profile is on row %d, not the "
               "stock's X20 row %d\n", row7, expected);
        failures++;
    }
    if (row8 < 0) {
        puts("frame2test: FAIL the G8 profile is not on the glass");
        failures++;
    } else if (row7 >= 0 && (row8 < row7 - 1 || row8 > row7 + 1)) {
        printf("frame2test: FAIL G8 X10 draws on row %d while G7 X20 draws on "
               "row %d: the radius was not read as a radius\n", row8, row7);
        failures++;
    }
    if (failures) {
        printf("frame2test: FAILED (%d)\n", failures);
        return 1;
    }
    printf("frame2test: PASS G8 X10 and G7 X20 draw the same part (row %d)\n",
           row7);
    return 0;
}


/* The run's glyph is the nose's circle only: on the machine's point, with no
   body around it. */
static int host_glyph2test(void)
{
    static const char *const program = "/D/nc/files/glyph.nc";
    ncc_document_t doc;
    ncc_preview_run_t run;
    ncc_tool_t tool;
    uint32_t stock = host_panel_rgb(ncc_col_prev_stock());
    uint32_t ink = host_panel_rgb(ncc_col_tool());
    int top = 0;
    int bottom = 0;
    int left = 0;
    int right = 0;
    int r;
    int tx;
    int ty;
    int row;
    int col;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    if (!host_fs_write_text(program,
                            "G971 X50 Z50 I0 E0\n"
                            "G0 X52 Z2\n"
                            "M2\n") ||
        !ncc_tool_from_line("T1 R3 F120 Q60 D2 E0.5 S800 X0 Z0", &tool)) {
        puts("glyph2test: FAIL cannot build the fixture");
        return 1;
    }
    ncc_document_init(&doc);
    if (!ncc_file_load(&doc, program)) {
        puts("glyph2test: FAIL the fixture does not load");
        return 1;
    }
    memset(&run, 0, sizeof(run));
    run.busy = true;
    run.screen_run = true;
    run.paint = true;
    run.full = true;
    run.tool = &tool;
    run.x = 10.0f;                      /* X20, a radius on the axis */
    run.z = -10.0f;
    ncc_preview_draw(&doc, &run, NCC_PREVIEW_X, NCC_PREVIEW_Y,
                     NCC_PREVIEW_W, NCC_PREVIEW_PANE_H);
    for (row = NCC_PREVIEW_Y; row < NCC_PREVIEW_Y + NCC_PREVIEW_PANE_H; row++) {
        for (col = NCC_PREVIEW_X; col < NCC_PREVIEW_X + NCC_PREVIEW_W; col++) {
            if (host_view_at(col, row) == stock) {
                break;
            }
        }
        if (col < NCC_PREVIEW_X + NCC_PREVIEW_W) {
            break;
        }
    }
    top = row;
    for (col = NCC_PREVIEW_X; col < NCC_PREVIEW_X + NCC_PREVIEW_W; col++) {
        for (row = top; row < top + NCC_PREVIEW_PANE_H; row++) {
            if (host_view_at(col, row) == stock) {
                break;
            }
        }
        if (row < top + NCC_PREVIEW_PANE_H) {
            if (!left) {
                left = col;
            }
            right = col;
        }
    }
    for (row = top; row < top + NCC_PREVIEW_PANE_H; row++) {
        if (host_view_at(left + 4, row) == stock) {
            bottom = row;
        }
    }
    if (bottom <= top || right <= left) {
        puts("glyph2test: FAIL the stock has no block to measure against");
        return 1;
    }
    r = (int)(0.5f * (float)(2 * (bottom - top)) * 3.0f / 25.0f + 0.5f);
    tx = right - (int)(10.0f * (float)(right - left) / 50.0f);
    ty = top + (int)(0.4f * (float)(bottom - top) + 0.5f);
    if (host_view_at(tx, ty) != ink) {
        puts("glyph2test: FAIL the nose is not on the machine's point");
        failures++;
    }
    if (host_view_at(tx, ty + r + 2) == ink ||
        host_view_at(tx - r + 2, ty + r) == ink) {
        puts("glyph2test: FAIL tool ink outside the nose circle");
        failures++;
    }
    if (failures) {
        printf("glyph2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("glyph2test: PASS the run's glyph is the nose circle only");
    return 0;
}

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
        if (strcmp(argv[i], "--dump-ncc") == 0)
            return host_dump_ncc(argv[i + 1]);
        if (strcmp(argv[i], "--dump-boot") == 0)
            return host_dump_boot(argv[i + 1]);
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
        if (strcmp(argv[i], "--preview2test") == 0)
            return host_preview2test();
        if (strcmp(argv[i], "--gcode2test") == 0)
            return host_gcode2test();
        if (strcmp(argv[i], "--chamfer2test") == 0)
            return host_chamfer2test();
        if (strcmp(argv[i], "--chuck2test") == 0)
            return host_chuck2test();
        if (strcmp(argv[i], "--colour2test") == 0)
            return host_colour2test();
        if (strcmp(argv[i], "--firstline2test") == 0)
            return host_firstline2test();
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
        if (strcmp(argv[i], "--brush2test") == 0)
            return host_brush2test();
        if (strcmp(argv[i], "--frame2test") == 0)
            return host_frame2test();
        if (strcmp(argv[i], "--glyph2test") == 0)
            return host_glyph2test();
        if (strcmp(argv[i], "--fps2test") == 0)
            return host_fps2test();
        if (strcmp(argv[i], "--scroll2test") == 0)
            return host_scroll2test();
        if (strcmp(argv[i], "--fault2test") == 0)
            return host_fault2test();
        if (strcmp(argv[i], "--boot2test") == 0)
            return host_boot2test();
        if (strcmp(argv[i], "--refuse2test") == 0)
            return host_refuse2test();
        if (strcmp(argv[i], "--blockline2test") == 0)
            return host_blockline2test();
        if (strcmp(argv[i], "--manual2test") == 0)
            return host_manual2test();
        if (strcmp(argv[i], "--tools2test") == 0)
            return host_tools2test();
        if (strcmp(argv[i], "--offset2test") == 0)
            return host_offset2test();
        if (strcmp(argv[i], "--comp2test") == 0)
            return host_comp2test();
        if (strcmp(argv[i], "--state2test") == 0)
            return host_state2test();
        if (strcmp(argv[i], "--runlock2test") == 0)
            return host_runlock2test();
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
