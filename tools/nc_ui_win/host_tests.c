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
#include "nc.h"
#include "nc_state.h"
#include "nc_editor.h"
#include "nc_presets.h"
#include "nc_run.h"
#include "nc_menu.h"
#include "nc_emit.h"
#include "nc_g7x.h"
#include "nc_layout.h"
#include "nc_manual.h"
#include "nc_palette.h"
#include "nc_preview.h"
#include "nc_tools.h"
#include "nc_vocab.h"
#include "nc_visual.h"
#include "nc2_boot.h"
#include "nc2.h"
#include "nc2_draw.h"
#include "nc2_emit.h"
#include "nc2_files.h"
#include "nc2_layout.h"
#include "nc2_presets.h"
#include "nc2_run.h"
#include "nc2_state.h"
#include "nc2_visual.h"
#include "g7x.h"
#include "host_fs.h"
#include "host_spindle.h"
#include "lvds_host.h"
#include "modules/cam_keyboard/cam_keyboard.h"
#include "host_shell.h"
#include "host_tests.h"
#include "nc_files.h"

#include <stdlib.h>

/* The checks are a flat list: each one is defined with the helpers it owns, so
   a helper an earlier check uses is stated here. */
static bool host_fs_write_text(const char *path, const char *text);
static bool host_fs_read_text(const char *path, char *out, size_t out_sz);
static void host_press(char key);
static void host_run_to_end(void);
static uint32_t host_frame_at(int x, int y);
static uint32_t host_panel_rgb(lvds_color_t color);

/* Headless check of the held feed, on the real parser, planner and virtual MCU.

     1. the value keys change what the next block carries;
     2. a held direction key toward the stop sends one jog block covering the
        whole distance to it - the feed is the controller's jog, not a stream
        of short moves - and nothing else while the key stays down;
     3. the axis lands on the stop and does not cross it;
     4. on the stop the direction that would cross it is refused, and the other
        one feeds away (the wall is one-sided, so the axis is never locked);
     5. letting the key go cancels that feed where it stands.

   This proves the blocks the panel sends and the state it leaves behind. That
   the machine moves, and that it decelerates where the controller says, are
   bench items. */
static int host_feedtest(void)
{
    char expected[48];
    char reader[128];
    nc_runtime_state_t rt;
    double before_feed;
    double travel = (double)g_settings.max_distance[AXIS_X];
    /* The panel bounds a feed with no wall in front of it by the axis travel
       the machine states, or its own cap when it states none. */
    double limit = (travel > 1.0) ? travel : 25.0;
    size_t n = 0u;
    int failures = 0;
    int step;

    host_init_core();
    nc_visual_select_mode(NC_MODE_MANUAL);
    host_pump_idle(64u);
    nc_state_runtime(&rt);
    printf("feedtest: X starts at %.3f, the feed limit is %.1f mm\n",
           (double)rt.x, limit);

    /* Type the stops first: `*` opens the minus stop, `*` again takes it and
       opens the plus one, and `A` leaves that one unset - so the minus side has
       a wall at -1 mm and the plus side has none, which is what the last check
       needs. */
    host_press('*');
    host_press('B');                                      /* `-` the sign */
    host_press('1');
    host_press('*');
    host_press('A');

    /* Then step X+ in step mode (the default) - the key that points down, `2`,
       because the preview draws X downward from the top of the stock - one step
       per press. */
    for (step = 0; step < 16; step++) {
        nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_2);
        host_pump_idle(512u);
    }
    host_pump_idle(512u);
    nc_state_runtime(&rt);
    printf("feedtest: after sixteen X+ steps X is %.3f\n", (double)rt.x);
    if (rt.x <= 0.0f) {
        puts("feedtest: FAIL the step jogs did not move the virtual axis");
        failures++;
    }

    /* `3` and `1` change the value the mode is using, and the block follows it:
       one step of the next size up, then back. */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_3);          /* step 0.100 -> 0.250 */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_2);          /* X+ */
    for (n = 0u; n + 1u < sizeof(reader) && grbl_stream_available(); ) {
        char c = grbl_stream_getc();

        reader[n++] = c ? c : '\n';
    }
    reader[n] = '\0';
    printf("feedtest: bigger step queued \"%s\"\n", reader);
    /* X is programmed as a diameter (G7 is the parser's default), so a step of
       0.250 axis millimetres is written as 0.500: without that the axis would
       move half of what the readout says. */
    if (strcmp(reader, "G91 G1 X0.500 F500\nG90\n") != 0) {
        puts("feedtest: FAIL '3' did not change the step the jog uses");
        failures++;
    }
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_1);          /* step back to 0.100 */
    host_pump_idle(512u);
    nc_state_runtime(&rt);

    /* Feed back toward the wall, holding the key: one block, as far as the wall,
       and nothing else while the key stays down. Reading the reader is what
       takes the block away from the parser, so this pass only checks the block;
       the next one lets the controller run it. */
    nc_visual_handle_key(NC_VISUAL_KEY_FINISH);           /* feed mode again */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_1);          /* feed 500 -> 300 */
    snprintf(expected, sizeof(expected), "$J=G91 X-%.3f F300\n",
             ((double)rt.x + 1.0) * 2.0);                 /* room to X-1, as a diameter */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_8);          /* X-: toward the wall */
    nc_visual_hold_key('8');
    for (n = 0u; n + 1u < sizeof(reader) && grbl_stream_available(); ) {
        char c = grbl_stream_getc();

        reader[n++] = c ? c : '\n';
    }
    reader[n] = '\0';
    printf("feedtest: feed queued \"%s\" (wanted \"%s\")\n", reader, expected);
    if (strcmp(reader, expected) != 0) {
        puts("feedtest: FAIL the held feed is not one jog block covering the stop");
        failures++;
    }
    if (grbl_stream_available()) {
        puts("feedtest: FAIL the held feed kept queueing blocks while it was held");
        failures++;
    }
    nc_visual_hold_key(0);
    host_pump_idle(64u);

    /* For real now: the controller takes the block and the axis lands on the
       wall. This is also what proves the distance was written in the units the
       parser reads - half of it and the axis would stop in the middle. */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_8);
    nc_visual_hold_key('8');
    host_pump_idle(4000u);
    nc_state_runtime(&rt);
    printf("feedtest: fed to the stop, X is %.3f\n", (double)rt.x);
    if (rt.x < -1.02f || rt.x > -0.98f) {
        printf("feedtest: FAIL the feed did not end on the stop (X %.3f)\n", (double)rt.x);
        failures++;
    }
    nc_visual_hold_key(0);
    host_pump_idle(64u);

    /* On the wall: the way that would cross it is refused, so an irrelevant
       press queues nothing at all. */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_8);
    nc_visual_hold_key('8');
    host_pump(20u);
    if (grbl_stream_available() || cnc_get_exec_state(EXEC_JOG)) {
        puts("feedtest: FAIL a feed crossed the stop");
        failures++;
    }
    nc_visual_hold_key(0);

    /* The other way has no stop in front of it, so the feed is a bounded move -
       the travel the machine states, or the panel's cap when it states none -
       and the key release stops it. The operator is never left with a key that
       does nothing. */
    before_feed = (double)rt.x;
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_2);          /* X+ */
    nc_visual_hold_key('2');
    host_pump(20u);
    if (!cnc_get_exec_state(EXEC_JOG)) {
        puts("feedtest: FAIL the axis could not feed away from the stop");
        failures++;
    }
    host_pump(20u);
    nc_state_runtime(&rt);
    printf("feedtest: backing off, X is %.3f (jog=%u)\n", (double)rt.x,
           (unsigned)cnc_get_exec_state(EXEC_JOG));
    nc_visual_hold_key(0);
    host_pump_idle(64u);
    nc_state_runtime(&rt);
    printf("feedtest: after release X is %.3f (jog=%u)\n", (double)rt.x,
           (unsigned)cnc_get_exec_state(EXEC_JOG));
    if (cnc_get_exec_state(EXEC_JOG)) {
        puts("feedtest: FAIL the feed kept jogging after the key came up");
        failures++;
    }
    if ((double)rt.x > limit) {
        printf("feedtest: FAIL the feed away ran past its limit (X %.3f)\n", (double)rt.x);
        failures++;
    }
    if ((double)rt.x <= before_feed + 0.0005) {
        puts("feedtest: FAIL the feed with no stop on that side did nothing");
        failures++;
    }
    if (failures) {
        printf("feedtest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("feedtest: PASS held key feeds to the stop, release cancels it");
    return 0;
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
/* Is this exact colour anywhere in the region? Used for the label checks: the
   error label is the palette's red with white_warm letters on it. */
static int host_color_in(const uint32_t *px, int x, int y, int w, int h,
                         uint32_t want)
{
    int r;
    int c;

    for (r = 0; r < h; r++) {
        for (c = 0; c < w; c++) {
            if (px[(size_t)(y + r) * (size_t)lvds_host_width() + (size_t)(x + c)] == want) {
                return 1;
            }
        }
    }
    return 0;
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

/* The two labels the bench asked for:
     - a fault in the message area is white letters on red, not red text;
     - the controller's state sits in the DRO's bottom-right corner, and wears
       the same red label when the state is a fault.
   The frame is read back through the host pixels, so this is the drawing the
   panel makes, not the strings it meant to make. */
static int host_labeltest(void)
{
    static const char *const program = "/D/nc/files/labels.nc";

    /* Long enough that the machine is still in the run when the frame is drawn:
       the running colour is what this checks first. */
    static const char *const slow_program =
        "G970 X-5 U60 Z-60 W5\nG971 X50 Z50 I0 E0\nT2\nG0 X52 Z2\n"
        "G1 X50 Z0 F20\nG1 X50 Z-40 F20\nG1 X20 Z-40 F20\n";
    /* The panel's palette is 5-6-5 on the wire, so the frame holds the rounded
       values: `red` #E60000 arrives as (231,0,0) and `white_warm` #F0F0F0 as
       (247,243,247). Measured from the drawing, not from the hex. `green`
       #24c863 arrives as (33,203,99) - the DRO's running colour, which has to be
       one of the sixteen colours the table already holds. */
    const uint32_t red = 0x00E70000u;
    const uint32_t white = 0x00F7F3F7u;
    const uint32_t green = 0x0021CB63u;
    const uint32_t *px;
    int failures = 0;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    if (!host_fs_write_text(program, slow_program) ||
        !host_fs_write_text("/D/nc_state.txt",
                            "MODE=RUN\nRUN=/D/nc/files/labels.nc\n")) {
        puts("labeltest: FAIL cannot set up the fixture");
        return 1;
    }
    host_init_core();
    host_pump_idle(64u);

    /* Idle first: the DRO corner carries the state, and nothing is wearing the
       red label yet. */
    nc_visual_draw();
    px = (const uint32_t *)lvds_host_pixels();
    if (!host_ink_in(px, 520, 58, 270, 22)) {
        puts("labeltest: FAIL the DRO corner is blank (no uCNC state)");
        failures++;
    }
    if (host_color_in(px, 400, 0, 390, 22, red)) {
        puts("labeltest: FAIL an idle panel is showing the red label");
        failures++;
    }
    if (host_color_in(px, 0, 26, 800, 56, green)) {
        puts("labeltest: FAIL an idle DRO is wearing the running colour");
        failures++;
    }

    /* Running: the DRO wears the panel's green, and only while the machine is
       actually in a run - the strip above it stays grey. Armed and drawn before
       the machine can finish, so this is the state the operator reads mid-cut. */
    host_press('3');
    nc_visual_draw();
    px = (const uint32_t *)lvds_host_pixels();
    if (!host_color_in(px, 0, 26, 800, 56, green)) {
        puts("labeltest: FAIL a running DRO is not green");
        failures++;
    } else if (host_color_in(px, 0, 0, 800, 24, green)) {
        puts("labeltest: FAIL the tab strip turned green with the DRO");
        failures++;
    } else {
        puts("labeltest: the DRO is green while the machine is in a run");
    }
    /* The run has to be *over* before the fault goes in: its errors land in the
       same listener, and a run that is still armed would set its own error and
       race this frame. A paced program is not drained by `host_pump_idle()` -
       that returns as soon as the machine is idle between blocks - so it is run
       to its end the way the reader does. */
    host_run_to_end();

    /* A refused command: the parser rejects the line (the module says why) and
       the panel shows the reason as the label. This is the listener RUN's
       errors land in too, without the run's own reset racing the frame - a
       parser reset clears the error, so a run's stop is a moving target for a
       single frame. */
    if (!mcu_unit_test_inject("G73 U1 W1 R2 P100 Q200\n") ||
        cnc_parse_cmd() != STATUS_GCODE_UNSUPPORTED_COMMAND) {
        puts("labeltest: FAIL the G73 line was not refused");
        failures++;
    }
    nc_visual_draw();
    px = (const uint32_t *)lvds_host_pixels();
    if (!host_color_in(px, 400, 0, 390, 22, red)) {
        puts("labeltest: FAIL the fault is not a red label");
        failures++;
    } else if (host_color_in(px, 0, 26, 800, 56, green)) {
        /* A fault takes the running colour away: a machine stopped by a problem
           must not still say "running". */
        puts("labeltest: FAIL the DRO is still green with a fault up");
        failures++;
    } else if (!host_color_in(px, 400, 0, 390, 22, white)) {
        puts("labeltest: FAIL the red label has no white letters");
        failures++;
    } else if (!host_ink_in(px, 520, 58, 270, 22)) {
        puts("labeltest: FAIL the DRO corner lost its state");
        failures++;
    } else if (host_color_in(px, 280, 24, 110, 68, red)) {
        /* The block stops left of the F/S column: the machine's own figures are
           never covered by a message. */
        puts("labeltest: FAIL the fault block covers the F/S column");
        failures++;
    } else if (host_color_in(px, 500, 60, 300, 31, red)) {
        /* And above the DRO's bottom-right corner, where the state is. */
        puts("labeltest: FAIL the fault block covers the uCNC state");
        failures++;
    } else {
        /* It has to clear too. `#` is RELOAD in RUN (the RESET action): the
           operator's own answer to a fault, and the only thing that clears it -
           a fault stands until it is answered. */
        nc_visual_select_mode(NC_MODE_RUN);
        nc_visual_handle_key(NC_VISUAL_KEY_FINISH);      /* `#` */
        nc_visual_draw();
        px = (const uint32_t *)lvds_host_pixels();
        if (host_color_in(px, 0, 0, 800, 92, red)) {
            puts("labeltest: FAIL the fault block did not clear on RELOAD");
            failures++;
        } else {
            puts("labeltest: PASS white-on-red faults that clear, and the uCNC "
                 "state in the DRO");
        }
    }

    return failures ? 1 : 0;
}

static int host_filetest(void)
{
    static const char *const program = "/D/nc/files/facing.nc";
    static const char *const tools = "/D/nc/files/tool.t";
    nc_document_t doc;
    nc_document_t tool_doc;
    nc_emit_stream_t stream;
    nc_tool_t tool;
    char line[NC_MAX_LINE_LEN + 2];
    size_t source_line;
    size_t end_line = 0u;
    unsigned emitted = 0u;
    unsigned tool_lines = 0u;
    bool saw_deep_z = false;
    int failures = 0;
    nc_result_t r;
    size_t i;

    host_init_core();
    nc_document_init(&doc);
    nc_document_init(&tool_doc);

    r = nc_load_file(&doc, program);
    if (r != NC_OK) {
        printf("filetest: FAIL cannot load %s (%s)\n", program, nc_result_text(r));
        failures++;
    } else if (doc.line_count != 11u ||
               strcmp(doc.lines[0].text, "G970 X-5 U60 Z-60 W5") != 0 ||
               strcmp(doc.lines[4].text, "G71 U1 R0.2 X0.5 Z0.5 F450") != 0 ||
               strcmp(doc.lines[10].text, "G80") != 0) {
        printf("filetest: FAIL the program did not load as written (%u lines)\n",
               (unsigned)doc.line_count);
        failures++;
    } else {
        printf("filetest: program %s: %u lines\n", program, (unsigned)doc.line_count);
    }

    r = nc_load_file(&tool_doc, tools);
    if (r != NC_OK) {
        printf("filetest: FAIL cannot load %s (%s)\n", tools, nc_result_text(r));
        failures++;
    } else {
        for (i = 0u; i < tool_doc.line_count; i++) {
            if (nc_tool_line_is_tool(tool_doc.lines[i].text)) {
                tool_lines++;
            }
        }
        if (tool_lines != 7u) {
            printf("filetest: FAIL the tool table has %u tools, not 7\n", tool_lines);
            failures++;
        } else if (!nc_tool_by_number(&tool_doc, 2, &tool) ||
                   tool.r != 3.0f || tool.orient != 176) {
            puts("filetest: FAIL T2 is not the R3 O176 tool");
            failures++;
        } else {
            printf("filetest: tools %s: %u tools\n", tools, tool_lines);
        }
    }

    /* The block the operator sees as one: the G71 line and its G80 range. */
    if (doc.line_count == 11u) {
        if (nc_g7x_block_start(&doc, 4u) != 4u ||
            !nc_g7x_block_end(&doc, 4u, &end_line) || end_line != 10u ||
            !nc_g7x_line_is_contour(&doc, 4u, 5u) ||
            nc_g7x_line_is_contour(&doc, 4u, 10u)) {
            puts("filetest: FAIL the G71 block does not start at line 5 and end at G80");
            failures++;
        } else {
            printf("filetest: G71 block is lines 5..%u\n", (unsigned)(end_line + 1u));
        }

        /* Expanded through the emitter RUN and the preview share. */
        nc_emit_stream_begin(&stream, &doc, 4u);
        while (emitted < 400u) {
            nc_emit_result_t er = nc_emit_stream_next(&stream, line, sizeof(line),
                                                      &source_line);

            if (er == NC_EMIT_SKIP) {
                break;
            }
            if (er == NC_EMIT_ERROR) {
                printf("filetest: FAIL the emitter stopped at line %u\n",
                       (unsigned)(source_line + 1u));
                failures++;
                break;
            }
            if (strstr(line, "Z-25")) {
                saw_deep_z = true;
            }
            emitted++;
        }
        if (emitted < 10u) {
            printf("filetest: FAIL the G71 block expanded to only %u lines\n", emitted);
            failures++;
        } else if (!saw_deep_z) {
            puts("filetest: FAIL the expansion never reaches the Z-25 finish");
            failures++;
        } else {
            printf("filetest: G71 expanded to %u lines\n", emitted);
        }

        /* And the tool line above it resolves through the table to T2. */
        if (nc_tool_active_from_table(&doc, 4u, &tool_doc, &tool) &&
            tool.t == 2 && tool.r == 3.0f && tool.orient == 176) {
            printf("filetest: the G71 block cuts with T%d R%.1f O%d\n", tool.t,
                   (double)tool.r, tool.orient);
        } else {
            puts("filetest: FAIL the T2 line above the G71 block did not resolve");
            failures++;
        }
    }

    /* Text files are text: the list carries them (the frame in
       tmp\nc-ui-show\ROOT-list shows the card's text files at /D) and the
       editor can open and save them, but only the program extensions are read
       as G-code - no preview parse, no RUN. */
    if (!nc_path_text("notes.txt") || nc_path_supported("notes.txt") ||
        !nc_path_text("facing.nc") || !nc_path_supported("facing.nc")) {
        puts("filetest: FAIL the text/program split is wrong for .txt or .nc");
        failures++;
    } else {
        puts("filetest: a .txt is text (list, edit, save) and not a program");
    }

    /* And the open itself: the list's OPEN used to refuse a `.txt` outright
       ("unsupported NC file"), which left a text file listed but not
       reachable. The editor opens it, saves it, and the preview draws its text
       rather than one "no preview" caption - one ink row would be that caption. */
    {
        static const char *const text_path = "/D/nc/files/notes.txt";
        static const char *const text_body =
            "preset notes, line one\nsecond line here\nthird line here\n"
            "fourth line\nfifth line\n";
        nc_document_t text_doc;
        nc_preview_ctx_t ctx;
        nc_preview_times_t times;
        char status[64];
        const uint32_t *px;
        int ink_rows = 0;
        int low_ink_rows = 0;
        int row;

        nc_document_init(&text_doc);
        if (!host_fs_write_text(text_path, text_body)) {
            puts("filetest: FAIL cannot write the text file");
            failures++;
        } else if (nc_load_file(&text_doc, text_path) != NC_OK ||
                   text_doc.line_count != 5u ||
                   strcmp(text_doc.lines[1].text, "second line here") != 0) {
            puts("filetest: FAIL a text file cannot be opened (the list's OPEN)");
            failures++;
        } else if (nc_save_file(&text_doc, text_path) != NC_OK) {
            puts("filetest: FAIL a text file cannot be saved after editing");
            failures++;
        } else {
            printf("filetest: text %s: %u lines, openable and saveable\n",
                   text_path, (unsigned)text_doc.line_count);
        }

        memset(&ctx, 0, sizeof(ctx));
        memset(&times, 0, sizeof(times));
        status[0] = '\0';
        ctx.doc = &text_doc;
        ctx.screen_doc = &text_doc;
        ctx.status = status;
        ctx.status_size = sizeof(status);
        ctx.mode = NC_MODE_PROGRAM;
        nc_preview_draw(&ctx, 10, 96, 350, 432, true, &times);
        px = (const uint32_t *)lvds_host_pixels();
        for (row = 0; row < 432; row++) {
            int col;

            for (col = 0; col < 350; col++) {
                size_t at = (size_t)(96 + row) * (size_t)lvds_host_width() +
                            (size_t)(10 + col);

                if (px[at] != px[(size_t)104u * (size_t)lvds_host_width() + 12u]) {
                    ink_rows++;
                    if (row > 100) {
                        /* The fifth line of the file sits around row 120 of the
                           pane. A single "no preview" caption (the old drawing)
                           could not reach this low, so this is what separates
                           the text from it. */
                        low_ink_rows++;
                    }
                    break;
                }
            }
        }
        if (ink_rows < 3 || low_ink_rows < 3) {
            printf("filetest: FAIL the text preview drew %d rows (%d low)\n",
                   ink_rows, low_ink_rows);
            failures++;
        } else {
            printf("filetest: the text preview drew %d rows (%d low)\n",
                   ink_rows, low_ink_rows);
        }
    }

    if (failures) {
        printf("filetest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("filetest: PASS program, tool table, block scan, expansion and T link");
    return 0;
}

/* Rewrite a card file with Windows line endings, the way a PC editor (or a git
   checkout on Windows) leaves a program. False when it cannot be read or
   written. */
static bool host_crlf_file(const char *path)
{
    char text[NC_MAX_LINE_LEN * 32];
    fs_file_t *fp;
    size_t used = 0u;
    size_t i;

    fp = fs_open(path, "r");
    if (!fp) {
        return false;
    }
    while (used + 1u < sizeof(text) && fs_available(fp)) {
        char c;

        if (fs_read(fp, (uint8_t *)&c, 1u) != 1u) {
            break;
        }
        if (c != '\r') {
            text[used++] = c;
        }
    }
    fs_close(fp);
    text[used] = '\0';

    fp = fs_open(path, "w");
    if (!fp) {
        return false;
    }
    for (i = 0u; i < used; i++) {
        if (text[i] == '\n' &&
            fs_write(fp, (const uint8_t *)"\r\n", 2u) != 2u) {
            fs_close(fp);
            return false;
        }
        if (text[i] != '\n' &&
            fs_write(fp, (const uint8_t *)&text[i], 1u) != 1u) {
            fs_close(fp);
            return false;
        }
    }
    fs_close(fp);
    return true;
}

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
       into it first, the way a tick does (`nc_visual_draw()` then the repaint). */
    nc_visual_select_mode(NC_MODE_PROGRAM);
    nc_visual_draw();
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
    nc_visual_select_mode(NC_MODE_RUN);
    nc_visual_draw();
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

/* Print what the machine thinks it is doing after the keys and ticks have run:
   the states a scripted feed depends on, and the figures the panel reads. */
static int host_state(void)
{
    nc_runtime_state_t rt;
    unsigned i;

    host_init_core();
    if (g_key_script[0]) {
        host_play_keys(g_key_script);
    }
    for (i = 0u; i < g_ticks; i++) {
        host_run_machine(TIMER_MS);
    }
    nc_state_runtime(&rt);
    printf("state: exec=0x%04x run=%u jog=%u hold=%u alarm=%u canceling=%u\n",
           cnc_get_exec_state(EXEC_ALLACTIVE),
           (unsigned)!!cnc_get_exec_state(EXEC_RUN),
           (unsigned)!!cnc_get_exec_state(EXEC_JOG),
           (unsigned)!!cnc_get_exec_state(EXEC_HOLD),
           (unsigned)cnc_has_alarm(),
           (unsigned)!!cnc_get_exec_state(EXEC_CANCELING));
    printf("state: X=%.3f Z=%.3f feed=%.1f spindle=%u "
           "planner_empty=%u itp_empty=%u reader=%u\n",
           (double)rt.x, (double)rt.z, (double)rt.feed, (unsigned)rt.spindle,
           (unsigned)planner_buffer_is_empty(),
           (unsigned)itp_is_empty(),
           (unsigned)grbl_stream_available());
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

static bool host_preset_line_is(int id, const char *expected)
{
    nc_document_t doc;
    bool ok;

    nc_document_init(&doc);
    ok = nc_insert_preset_id(&doc, id) &&
         doc.line_count == 1u &&
         strcmp(doc.lines[0].text, expected) == 0;
    return ok;
}

/* The preset entries, which are the card's: `/D/presets/<address>.txt`, the
   first row the name, the rest the rows to write (docs/nc-preset-file.md).

     1. a card with no folder answers with the compiled entries, and the folder
        is the one thing the panel creates by itself;
     2. an entry file replaces its address - name and rows, both halves;
     3. an empty first row keeps the compiled name (the name may be empty, the
        rows may not);
     4. an address no compiled entry uses appears on its pad when the card has
        the file: this is how a word the pads do not offer is added;
     5. every row of the file is written, in order;
     6. a row that starts with a space continues the row above instead of
        starting one;
     7. a file with a name and no rows is not an entry;
     8. an address outside the pads' space is not an entry either. */
static int host_presettest(void)
{
    static const char *const default_od = "G71 U0 R0 X0 Z0 F0 P0 Q0";
    static const char *const edited_od = "G71 U2 R1 X10 Z-5 F0.2 P100 Q200";
    static const char *const default_finish = "G70 P0 Q0";
    static const char *const entry_41 =
        "OD TEST\nG71 U2 R1 X10 Z-5 F0.2 P100 Q200\n";
    char name[32];
    fs_file_info_t info;
    int failures = 0;

    cnc_init();
    cnc_unit_test_start();
    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    printf("nc_ui: fs root %s\n", g_files_root[0] ? g_files_root : "nc-files");

    /* 1. no folder: the compiled entries answer, and the folder appears so the
          operator has somewhere to put their own. */
    (void)fs_rmdir("/D/presets");
    (void)nc_presets_init();
    (void)nc_presets_sync();
    if (!fs_finfo("/D/presets", &info) || !info.is_dir) {
        puts("presettest: FAIL the presets folder was not created");
        failures++;
    } else if (!host_preset_line_is(41, default_od)) {
        puts("presettest: FAIL the compiled OD entry did not insert");
        failures++;
    } else if (!nc_preset_name_for_id(41, name, sizeof(name)) ||
               strcmp(name, "OD ROUGH") != 0) {
        puts("presettest: FAIL the compiled OD entry has no name");
        failures++;
    } else {
        puts("presettest: PASS a card with no folder answers with the compiled "
             "entries");
    }

    /* 2. an entry file is what the key writes: both halves of it. */
    if (!host_fs_write_text("/D/presets/41.txt", entry_41)) {
        puts("presettest: FAIL could not write /D/presets/41.txt");
        failures++;
    } else {
        (void)nc_presets_init();
        if (!host_preset_line_is(41, edited_od)) {
            puts("presettest: FAIL the entry file's rows are not what `41` "
                 "writes");
            failures++;
        } else if (!nc_preset_name_for_id(41, name, sizeof(name)) ||
                   strcmp(name, "OD TEST") != 0) {
            printf("presettest: FAIL `41` is named \"%s\", not \"OD TEST\"\n",
                   name);
            failures++;
        } else if (!host_preset_line_is(48, default_finish)) {
            puts("presettest: FAIL an address with no file lost its compiled "
                 "entry");
            failures++;
        } else {
            puts("presettest: PASS an entry file is the name and the rows");
        }
    }

    /* 3. the name may be empty - and then the compiled name stands. */
    if (!host_fs_write_text("/D/presets/16.txt", "\nG970 X-10 U120 Z-150 W30\n")) {
        puts("presettest: FAIL could not write the nameless entry");
        failures++;
    } else {
        (void)nc_presets_init();
        if (!host_preset_line_is(16, "G970 X-10 U120 Z-150 W30")) {
            puts("presettest: FAIL the nameless entry did not write its row");
            failures++;
        } else if (!nc_preset_name_for_id(16, name, sizeof(name)) ||
                   strcmp(name, "SETUP") != 0) {
            printf("presettest: FAIL an empty first row did not keep the "
                   "compiled name (\"%s\")\n", name);
            failures++;
        } else {
            puts("presettest: PASS an empty first row keeps the compiled name");
        }
    }

    /* 4. an address no compiled entry uses, with a file: the word the pads do
          not offer today. */
    if (!host_fs_write_text("/D/presets/12.txt", "COOLANT\nM8\n")) {
        puts("presettest: FAIL could not write the added entry");
        failures++;
    } else {
        (void)nc_presets_init();
        if (!host_preset_line_is(12, "M8")) {
            puts("presettest: FAIL the added entry did not write");
            failures++;
        } else if (!nc_preset_name_for_id(12, name, sizeof(name)) ||
                   strcmp(name, "COOLANT") != 0) {
            puts("presettest: FAIL the added entry has no name");
            failures++;
        } else {
            puts("presettest: PASS a free address with a file is an entry");
        }
    }

    /* 5. every row, in order - a header and its contour are one entry. */
    if (!host_fs_write_text("/D/presets/13.txt",
                            "ROUGH\nG71 U1 R0.2 X0.5 Z0.5 F450\nG1 X30 Z0\n"
                            "G80\n")) {
        puts("presettest: FAIL could not write the multi-row entry");
        failures++;
    } else {
        nc_document_t doc;
        bool ok;

        (void)nc_presets_init();
        nc_document_init(&doc);
        ok = nc_insert_preset_id(&doc, 13) && doc.line_count == 3u &&
             strcmp(doc.lines[0].text, "G71 U1 R0.2 X0.5 Z0.5 F450") == 0 &&
             strcmp(doc.lines[1].text, "G1 X30 Z0") == 0 &&
             strcmp(doc.lines[2].text, "G80") == 0 &&
             doc.cursor_line == 0u && doc.selected_word == -1;
        if (!ok) {
            printf("presettest: FAIL the multi-row entry wrote %u lines: \"%s\" "
                   "\"%s\" \"%s\"\n", (unsigned)doc.line_count,
                   doc.line_count > 0u ? doc.lines[0].text : "",
                   doc.line_count > 1u ? doc.lines[1].text : "",
                   doc.line_count > 2u ? doc.lines[2].text : "");
            failures++;
        } else {
            puts("presettest: PASS every row of an entry is written, in order");
        }
    }

    /* 6. a row that starts with a space continues the row above: the only way a
          value that belongs on a line already written gets in without the
          controller seeing a line break inside the block - and the word it wrote
          is left picked, so the number is typed straight into it. The cursor
          stays on the line the operator was on, because the entry wrote no line
          of its own. */
    if (!host_fs_write_text("/D/presets/36.txt",
                            "ROW\nG1 X0 Z0\n C0\n R0\n")) {
        puts("presettest: FAIL could not write the inline-row entry");
        failures++;
    } else {
        nc_document_t doc;
        bool ok;

        (void)nc_presets_init();
        nc_document_init(&doc);
        (void)nc_insert_line(&doc, 0, "G71 U1 R1 X0.5 Z0.5 F450");
        ok = nc_insert_preset_id(&doc, 36) && doc.line_count == 2u &&
             strcmp(doc.lines[1].text, "G1 X0 Z0 C0 R0") == 0 &&
             doc.cursor_line == 1u && doc.selected_word == 4;
        if (!ok) {
            printf("presettest: FAIL the inline rows read \"%s\", cursor %u, "
                   "word %d\n",
                   doc.line_count > 1u ? doc.lines[1].text : "",
                   (unsigned)doc.cursor_line, doc.selected_word);
            failures++;
        } else {
            puts("presettest: PASS a row that starts with a space continues the "
                 "row above and leaves its word picked");
        }
    }

    /* 7. the rows are the mandatory half: a file with a name and nothing else is
          not an entry, and the address stays empty. */
    if (!host_fs_write_text("/D/presets/37.txt", "NOTHING\n")) {
        puts("presettest: FAIL could not write the empty entry");
        failures++;
    } else {
        (void)nc_presets_init();
        if (nc_preset_name_for_id(37, name, sizeof(name)) ||
            host_preset_line_is(37, "NOTHING")) {
            puts("presettest: FAIL an entry with no rows is offered");
            failures++;
        } else {
            puts("presettest: PASS a file with no rows is not an entry");
        }
    }

    /* 8. an address outside the pads' space is not an entry either. */
    if (!host_fs_write_text("/D/presets/99.txt", "OUTSIDE\nM8\n")) {
        puts("presettest: FAIL could not write the out-of-range entry");
        failures++;
    } else {
        (void)nc_presets_init();
        if (nc_preset_name_for_id(99, name, sizeof(name)) ||
            host_preset_line_is(99, "M8")) {
            puts("presettest: FAIL an address outside the pads' space is an "
                 "entry");
            failures++;
        } else {
            puts("presettest: PASS an address outside the pads' space is not an "
                 "entry");
        }
    }

    /* The one entry that writes nothing at all: `1 OPS` then `1` is a blank
       line, which is an entry like any other. */
    if (!host_preset_line_is(11, "")) {
        puts("presettest: FAIL the new-line entry is not a blank line");
        failures++;
    } else {
        puts("presettest: PASS the compiled new-line entry is one blank line");
    }

    if (failures) {
        printf("presettest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("presettest: PASS the card's entries are address files");
    return 0;
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
    if (!nc_run_send_line("G91 G1 X0.100 F500") ||
        !nc_run_send_line("G90")) {
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

/* Headless check of the key model the shell presents. Three things can go
   wrong and all of them are silent on screen:

     1. a key character does not reach the key the machine sends
        (nc_visual_key_for_char() is that table, and nc_module.c maps the
        hardware keypad through it);
     2. the pad is not the machine's keypad - the shell would press a key the
        machine does not have, or draw one where the hardware has another;
     3. a screen offers a footer key the pad cannot press, so the bench - and
        the machine - cannot reach that entry at all.

   The third one is what the old pad did wrong: it mapped its cells to footer
   positions, so MANUAL's B/C/D and the skipped slots sent the wrong key.

   It also pins the PC keys that stand for a keypad key without asking the
   keyboard layout (`host_pc_machine_key()`), because the machine's finish key
   is `#`: a key that needs Shift+3 on most layouts and AltGr on the rest is not
   reachable, so `W` carries it. */
/* The footer is a strip of fixed slots: more than NC_FOOTER_SLOTS and the keys
   shrink and move between screens, so a screen that needs fewer entries leaves
   them empty instead. `D` (accept) and `0` (file) keep their slot because the
   screen names what they do there; `*` is the delete/back key. */
#define HOST_FOOTER_SLOTS 8

static bool host_pad_has_key(char key)
{
    int row;
    int col;

    for (row = 0; row < PAD_ROWS; row++) {
        for (col = 0; col < PAD_COLS; col++) {
            if (g_pad_keys[row][col] == key)
                return true;
        }
    }
    return false;
}

static int host_padtest(void)
{
    /* The matrix cam_keyboard.c decodes: key event 1*row+col, as the driver
       numbers them. Row 1 on top. */
    static const char *const machine[PAD_ROWS] = {
        "*0#D",
        "123C",
        "456B",
        "789A"
    };
    static const nc_mode_t modes[] = {
        NC_MODE_MANUAL, NC_MODE_PROGRAM, NC_MODE_TOOLS, NC_MODE_RUN
    };
    static const struct {
        char key;
        nc_visual_key_t want;
    } map[] = {
        { '0', NC_VISUAL_KEY_DIGIT_0 },
        { '1', NC_VISUAL_KEY_DIGIT_1 },
        { '2', NC_VISUAL_KEY_DIGIT_2 },
        { '3', NC_VISUAL_KEY_DIGIT_3 },
        { '4', NC_VISUAL_KEY_DIGIT_4 },
        { '5', NC_VISUAL_KEY_DIGIT_5 },
        { '6', NC_VISUAL_KEY_DIGIT_6 },
        { '7', NC_VISUAL_KEY_DIGIT_7 },
        { '8', NC_VISUAL_KEY_DIGIT_8 },
        { '9', NC_VISUAL_KEY_DIGIT_9 },
        { '*', NC_VISUAL_KEY_BACKSPACE },
        { '#', NC_VISUAL_KEY_FINISH },
        { 'A', NC_VISUAL_KEY_MODE },
        { 'B', NC_VISUAL_KEY_FIELD_PREV },
        { 'C', NC_VISUAL_KEY_FIELD_NEXT },
        { 'D', NC_VISUAL_KEY_ACCEPT }
    };
    unsigned m;
    unsigned k;
    int row;
    int failures = 0;

    host_init_core();
    for (k = 0u; k < sizeof(map) / sizeof(map[0]); k++) {
        if (nc_visual_key_for_char(map[k].key) != map[k].want) {
            printf("padtest: FAIL keypad '%c' is not the key the machine sends\n",
                   map[k].key);
            failures++;
        }
    }
    if (nc_visual_key_for_char('a') != NC_VISUAL_KEY_NONE ||
        nc_visual_key_for_char('X') != NC_VISUAL_KEY_NONE ||
        nc_visual_key_for_char('\0') != NC_VISUAL_KEY_NONE) {
        puts("padtest: FAIL a key outside the keypad was accepted");
        failures++;
    }
    /* The PC keys that stand for a keypad key without asking the layout. `W`
       carries the machine's `#`, because `#` needs Shift+3 on most layouts and
       AltGr on the rest: the key has to be reachable, and it stays the same key
       the machine sends - `nc_visual_key_for_char()` is what turns it into the
       screen's own key, so on EDIT `W` is VIEW (the footer's `#`), in a value
       field it is the accept key, and on MANUAL it is step/feed. */
    {
        static const struct {
            unsigned vk;
            char key;
            nc_visual_key_t want;
        } pc[] = {
            { 'W', '#', NC_VISUAL_KEY_FINISH },
            { VK_DELETE, '#', NC_VISUAL_KEY_FINISH },
            { VK_RETURN, 'D', NC_VISUAL_KEY_ACCEPT },
            { VK_ESCAPE, 'A', NC_VISUAL_KEY_MODE },
            { VK_BACK, '*', NC_VISUAL_KEY_BACKSPACE },
            /* A digit is the layout's and a letter like Q is nobody's: the
               fixed map must not answer for them. */
            { '9', 0, NC_VISUAL_KEY_NONE },
            { 'Q', 0, NC_VISUAL_KEY_NONE }
        };
        size_t k;

        (void)pc;
        for (k = 0u; k < sizeof(pc) / sizeof(pc[0]); k++) {
            char got = host_pc_machine_key(pc[k].vk);

            if (got != pc[k].key) {
                printf("padtest: FAIL PC key 0x%02x sends '%c', not '%c'\n",
                       pc[k].vk, got ? got : '?', pc[k].key);
                failures++;
            } else if (got && nc_visual_key_for_char(got) != pc[k].want) {
                printf("padtest: FAIL PC key '%c' is not the key the machine "
                       "sends for '%c'\n", pc[k].vk, got);
                failures++;
            }
        }
    }
    for (row = 0; row < PAD_ROWS; row++) {
        if (strcmp(g_pad_keys[row], machine[row]) != 0) {
            printf("padtest: FAIL row %d is \"%s\", the keypad has \"%s\"\n",
                   row, g_pad_keys[row], machine[row]);
            failures++;
        }
    }
    for (m = 0u; m < sizeof(modes) / sizeof(modes[0]); m++) {
        size_t count = 0u;
        const nc_footer_item_t *footer;
        size_t i;

        nc_visual_select_mode(modes[m]);
        footer = nc_visual_footer(&count);
        for (i = 0u; i < count; i++) {
            if (footer[i].key == ' ' || footer[i].key == '\0')
                continue; /* empty slot: it keeps its place, it is not a key */
            if (!host_pad_has_key(footer[i].key)) {
                printf("padtest: FAIL %s offers key '%c' (%s) the keypad has not\n",
                       nc_menu_mode_name(modes[m]), footer[i].key, footer[i].label);
                failures++;
            }
            {
                nc_visual_key_meaning_t meaning;

                /* Every key the footer offers has to read as a menu key on the
                   pad beside the machine, or the strip and the keypad disagree
                   about the same key. */
                if (!host_key_meaning(footer[i].key, &meaning) ||
                    !meaning.on_menu || !meaning.label) {
                    printf("padtest: FAIL %s key '%c' (%s) is drawn with no "
                           "menu label\n", nc_menu_mode_name(modes[m]),
                           footer[i].key, footer[i].label);
                    failures++;
                }
            }
        }
        /* `B`/`C` are the keypad's step keys - the arrows - on every screen
           that steps a field or the axis, so the shell draws them as arrows. */
        {
            char step_key;

            for (step_key = 'B'; step_key <= 'C'; step_key++) {
                nc_visual_key_meaning_t meaning;

                if (!host_key_meaning(step_key, &meaning) || !meaning.step) {
                    printf("padtest: FAIL %s does not draw '%c' as a step key\n",
                           nc_menu_mode_name(modes[m]), step_key);
                    failures++;
                }
            }
        }
        /* Every screen names itself and says how its keys drive it: a shell
           beside the machine has to have something to show for each of them,
           and the screen is the one that owns those words. */
        {
            const char *const *usage = 0;
            size_t lines = nc_visual_usage(&usage);

            if (lines == 0u || lines > NC_VISUAL_USAGE_MAX || !usage ||
                !usage[0] || !usage[0][0]) {
                printf("padtest: FAIL %s has no usage lines\n",
                       nc_menu_mode_name(modes[m]));
                failures++;
            }
            if (!nc_visual_screen_name() || !nc_visual_screen_name()[0]) {
                printf("padtest: FAIL %s has no screen name\n",
                       nc_menu_mode_name(modes[m]));
                failures++;
            }
        }
        /* Both strips the screen can show - its own and the file list's - have
           to fit the fixed slots. */
        {
            unsigned view;

            for (view = 0u; view < 2u; view++) {
                size_t fcount = 0u;
                const nc_footer_item_t *items =
                    nc_menu_footer(modes[m], view != 0u, &fcount);

                if (fcount > HOST_FOOTER_SLOTS) {
                    printf("padtest: FAIL %s footer has %u slots (max %u)\n",
                           nc_menu_mode_name(modes[m]), (unsigned)fcount,
                           (unsigned)HOST_FOOTER_SLOTS);
                    failures++;
                }
                /* Only a screen that edits a document may delete with a key:
                   RUN carries no delete, because `*` used to delete a line of
                   the program being run. */
                if (modes[m] != NC_MODE_PROGRAM && modes[m] != NC_MODE_TOOLS &&
                    view == 0u) {
                    size_t k;

                    for (k = 0u; k < fcount; k++) {
                        if (items[k].action == NC_FOOTER_ACTION_DELETE) {
                            printf("padtest: FAIL %s footer deletes with '%c' (%s)\n",
                                   nc_menu_mode_name(modes[m]), items[k].key,
                                   items[k].label);
                            failures++;
                        }
                    }
                }
            }
        }
        /* EDIT's full-screen strip is a view too: it must fit the slots and it
           must not offer a delete while the preview is the whole screen. */
        {
            size_t pcount = 0u;
            const nc_footer_item_t *preview_items = nc_menu_preview_footer(&pcount);
            size_t k;
            bool trace_label = false;

            if (pcount > HOST_FOOTER_SLOTS) {
                printf("padtest: FAIL full-screen footer has %u slots (max %u)\n",
                       (unsigned)pcount, (unsigned)HOST_FOOTER_SLOTS);
                failures++;
            }
            for (k = 0u; k < pcount; k++) {
                if (preview_items[k].action == NC_FOOTER_ACTION_PATH &&
                    strcmp(preview_items[k].label, "TRACE") == 0) {
                    trace_label = true;
                }
                if (preview_items[k].action == NC_FOOTER_ACTION_DELETE) {
                    printf("padtest: FAIL full screen deletes with '%c' (%s)\n",
                           preview_items[k].key, preview_items[k].label);
                    failures++;
                }
            }
            if (!trace_label) {
                puts("padtest: FAIL the preview's TRACE key has no label");
                failures++;
            }
        }
        /* MANUAL's jog digits are not footer entries: the screen's own hint is
           the only thing that can label them, and the panel draws them too. */
        if (modes[m] == NC_MODE_MANUAL) {
            int digit;

            for (digit = 1; digit <= 9; digit++) {
                if (!nc_visual_key_hint((char)('0' + digit))) {
                    printf("padtest: FAIL MANUAL digit %d has no meaning to show\n",
                           digit);
                    failures++;
                }
            }
        }
    }
    if (failures) {
        printf("padtest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("padtest: PASS the keypad is the machine's matrix, its keys are the keys "
         "the machine sends, and every footer key is on it");
    return 0;
}

/* The legend the editor shows for the word under the cursor: the first word with
   this letter on the line. */
static const char *host_label_of(const char *line, char letter)
{
    nc_word_t words[16];
    int count = nc_parse_words(line, words, 16);
    int i;

    for (i = 0; i < count; i++) {
        if (words[i].letter == letter)
            return nc_vocab_label_for_word(line, &words[i]);
    }
    return "(no such word)";
}

/* Every word the panel writes into a line has to read as something: a template
   word that falls through to the legend's "NC word" tells the operator nothing,
   which is what P, Q and N did in the G71/G72 headers. The set of templates is
   the field-entry set (`nc_vocab_gcode_template()`, the same words the presets
   carry), so this walks all of it rather than trusting a list kept by hand. */
static int host_vocabtest(void)
{
    static const int templates[] = { 0, 1, 2, 3, 4, 28, 30, 33, 71, 72, 76,
                                     90, 91, 95, 96, 97, 970, 971, 972, 973 };
    int failures = 0;
    size_t t;

    for (t = 0u; t < sizeof(templates) / sizeof(templates[0]); t++) {
        const char *tmpl = nc_vocab_gcode_template(templates[t]);
        nc_word_t words[16];
        int count;
        int i;

        if (!tmpl) {
            printf("vocabtest: FAIL gcode %d has no template\n", templates[t]);
            failures++;
            continue;
        }
        count = nc_parse_words(tmpl, words, 16);
        for (i = 0; i < count; i++) {
            const char *label = nc_vocab_label_for_word(tmpl, &words[i]);

            if (!label || !label[0] || strcmp(label, "NC word") == 0) {
                printf("vocabtest: FAIL %c of [%s] has no legend\n",
                       words[i].letter, tmpl);
                failures++;
            }
        }
    }

    /* The words that are not in a template still say what they are. */
    if (strcmp(host_label_of("G71 U1 R1 P100 Q200 F120", 'P'),
               "Profile start block") != 0 ||
        strcmp(host_label_of("G71 U1 R1 P100 Q200 F120", 'Q'),
               "Profile end block") != 0 ||
        strcmp(host_label_of("G70 P100 Q200", 'P'), "Profile start block") != 0 ||
        strcmp(host_label_of("G70 P100 Q200", 'Q'), "Profile end block") != 0 ||
        strcmp(host_label_of("N100 G1 X50 Z0", 'N'), "Block number") != 0 ||
        strcmp(host_label_of("G4 P1", 'P'), "Dwell time") != 0 ||
        strcmp(host_label_of("G33 X0 Z0 K2 F1.5", 'K'), "Thread pitch") != 0) {
        puts("vocabtest: FAIL a cycle word is not named for what it is");
        failures++;
    }

    if (failures) {
        printf("vocabtest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("vocabtest: PASS every word the panel writes has a legend");
    return 0;
}

/* Press a keypad character the way the machine sends it. */
static void host_press(char key)
{
    nc_visual_handle_key(nc_visual_key_for_char(key));
}

/* Nothing queued, nothing stepping, no cycle left to emit. */
static bool host_machine_idle(void)
{
    return !g7x_parser_busy() &&
           planner_buffer_is_empty() &&
           itp_is_empty();
}

/* Read what the RUN stream sends until the run is over. A program is *paced*
   (`nc_run_pace()`): the panel hands the machine one unit, waits for it to run,
   and only then hands over the next one. So a reader that does not pump the
   machine would only ever see the first block - and the point of the pacing is
   that the reader *can* look between blocks. `sent` gets the lines joined by
   newlines, `raw` keeps the block breaks as `|` for the log. */
static void host_read_run(char *sent,
                          size_t sent_cap,
                          size_t *sent_len,
                          char *raw,
                          size_t raw_cap,
                          size_t *raw_len)
{
    unsigned guard;

    *sent_len = 0u;
    *raw_len = 0u;
    for (guard = 0u; guard < 200000u; guard++) {
        bool read_any = false;

        while (grbl_stream_available()) {
            char c = grbl_stream_getc();

            read_any = true;
            if (*raw_len + 2u < raw_cap) {
                raw[(*raw_len)++] = c ? c : '|';
            }
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
            continue;                 /* drain what the last block offered */
        }
        /* The run is over when nothing is armed and the machine has finished
           everything it was given. While a program is armed the pacer is the
           one that decides, and it does so inside the pump. */
        if (host_machine_idle() && !nc_run_streaming()) {
            break;
        }
        host_pump(1u);
    }
    if (*sent_len > 0u && sent[*sent_len - 1u] != '\n' && *sent_len + 1u < sent_cap) {
        sent[(*sent_len)++] = '\n';
    }
    sent[*sent_len] = '\0';
    raw[*raw_len] = '\0';
}

/* Let a paced program run to its end, throwing away what it sends. */
static void host_run_to_end(void)
{
    char sent[64];
    char raw[64];
    size_t n = 0u;
    size_t r = 0u;

    host_read_run(sent, sizeof(sent), &n, raw, sizeof(raw), &r);
}

/* The panel saves the program itself: `nc_editor_save_current()` runs from the
   idle task once the screen has been left alone, and before the buffer is reused
   on a screen change. So a check "saves" the way the operator does - it stops
   touching the keyboard and lets the clock pass the idle threshold (a word
   selection would swallow digits anyway, so the selection goes first). */
static void host_editor_save(void)
{
    nc_visual_handle_key(NC_VISUAL_KEY_CANCEL);
    mcu_unit_test_advance_time(2000000u);      /* 2 s of emulated clock */
    host_pump(40u);
}

/* The same, without the cancel: the contour pad has to *stay* up while the
   program is written, so the flush is reached the way the panel reaches it -
   by leaving the screen alone. */
static void host_editor_flush(void)
{
    mcu_unit_test_advance_time(2000000u);
    host_pump(40u);
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
    nc_document_t nc_doc;
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
    nc_document_init(&nc_doc);
    {
        static char line[NC2_MAX_LINE_LEN];
        bool saw_typed = false;
        bool saw_name = false;
        size_t i;

        /* The document the screen holds is nc2's, not nc's, so the check reads
           the file it saves through nc2's own writer. */
        if (!nc2_visual_save()) {
            puts("screen2test: FAIL nc2 did not save the program");
            failures++;
        } else if (nc_load_file(&nc_doc, program) != NC_OK) {
            puts("screen2test: FAIL the saved program does not load as a program");
            failures++;
        } else {
            for (i = 0u; i < nc_doc.line_count; i++) {
                snprintf(line, sizeof(line), "%s", nc_doc.lines[i].text);
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
    if (!nc2_boot_seed()) {
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

/* Everything nc's sender emits for a document, as one string to compare. */
static bool host_emit_everything_nc(const nc_document_t *doc,
                                    size_t start,
                                    char *out,
                                    size_t out_sz)
{
    nc_emit_stream_t stream;
    size_t line = 0u;
    size_t used = 0u;

    out[0] = '\0';
    nc_emit_stream_begin(&stream, doc, start);
    nc_emit_stream_set_log(&stream, false);
    while (stream.active) {
        char text[NC_MAX_LINE_LEN];
        nc_emit_result_t r = nc_emit_stream_next(&stream, text, sizeof(text),
                                                 &line);

        if (r == NC_EMIT_ERROR) {
            return false;
        }
        if (r != NC_EMIT_LINE) {
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

/* The two senders, line for line. What the machine is told is the one thing the
   new module may not change on the way in, and the surest way to say so is to
   feed both the same program and compare what comes out:

     1. a program with two roughing cycles, a finish cut each and a plain row
        after them;
     2. the same program started mid-file (`RUN FROM`), where the new sender has
        to prime its point from the lines above before it can resolve anything;
     3. a contour written with Fanuc's `U`/`W` increments, which both senders have
        to leave as the absolute lines the controller reads.

   Every case runs through both `nc_emit_stream_*` and `nc2_emit_stream_*` and the
   joined output has to be identical, so a difference in either one fails here
   rather than on the machine. */
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
    const char *text = absolute_text;
    size_t starts[2];
    int failures = 0;
    int pass;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    host_init_core();
    /* Line 11 in the file above is the `G70 P50 Q55` - a run started there has
       to know where the first cycle left the tool. */
    starts[0] = 0u;
    starts[1] = 11u;
    for (pass = 0; pass < 3; pass++) {
        nc_document_t a;
        nc2_document_t b;
        char out_nc[4096];
        char out_nc2[4096];
        size_t start;
        size_t s;

        if (pass == 2) {
            text = increments_text;
            starts[1] = 0u;
        }
        if (!host_fs_write_text(program, text)) {
            puts("emit2test: FAIL cannot write the fixture");
            return 1;
        }
        nc_document_init(&a);
        nc2_document_init(&b);
        if (nc_load_file(&a, program) != NC_OK ||
            !nc2_file_load(&b, program)) {
            puts("emit2test: FAIL the fixture does not load");
            return 1;
        }
        for (s = 0u; s < 2u; s++) {
            start = starts[s];
            if (!host_emit_everything_nc(&a, start, out_nc, sizeof(out_nc)) ||
                !host_emit_everything_nc2(&b, start, out_nc2, sizeof(out_nc2))) {
                printf("emit2test: FAIL pass %d from line %u does not expand\n",
                       pass, (unsigned)start);
                failures++;
                continue;
            }
            if (strcmp(out_nc, out_nc2) != 0) {
                printf("emit2test: FAIL pass %d from line %u differs:\n"
                       "  nc : %s\n  nc2: %s\n", pass, (unsigned)start,
                       out_nc, out_nc2);
                failures++;
            }
        }
    }

    if (failures) {
        printf("emit2test: FAILED (%d)\n", failures);
        return 1;
    }
    puts("emit2test: PASS nc2 sends exactly what nc sends, from the top and "
         "from the middle");
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

    /* 1. the first start. */
    if (!nc2_boot_seed()) {
        puts("seedtest: FAIL an empty card was not seeded");
        return 1;
    }
    if (!nc2_boot_active()) {
        puts("seedtest: FAIL the logo did not come up");
        failures++;
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

/* Headless check of the editor's typed-key paths - the ones a frame dump cannot
   see. A typed character has to reach the thing that acts on it:

     1. a selected word takes the digits typed at it;
     2. the floating helper takes the digit that picks an entry from a submenu;
     3. the helper's field takes a digit and then the G-code template for it;
     4. the footer keys reach the editor's insert and delete.

   This is where the split can go wrong silently: a handler that keeps its own
   copy of the key, or an entry that is handed the key instead of the character,
   looks exactly like a working one until a key is pressed. Each step ends by
   saving through the helper and reading the program back off the card, so the
   check is on the document, not on what the panel remembers. */
static int host_editortest(void)
{
    static const char *const program = "/D/nc/files/facing.nc";
    nc_document_t before;
    nc_document_t after;
    size_t base_lines;
    int failures = 0;
    unsigned changed = 0u;
    unsigned i;

    host_init_core();
    nc_visual_select_mode(NC_MODE_PROGRAM);
    host_pump_idle(64u);

    /* What the panel holds is the fixture: EDIT was pointed at it by the state
       file the test runner writes. */
    nc_document_init(&before);
    if (nc_load_file(&before, program) != NC_OK) {
        printf("editortest: FAIL cannot load %s\n", program);
        return 1;
    }
    base_lines = before.line_count;

    /* 1. a selected word takes the digits typed at it */
    nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);     /* `D`: select the next word */
    host_press('7');
    host_editor_save();
    nc_document_init(&after);
    if (nc_load_file(&after, program) != NC_OK) {
        puts("editortest: FAIL the edited program did not save");
        failures++;
    } else {
        for (i = 0u; i < after.line_count && i < before.line_count; i++) {
            if (strcmp(after.lines[i].text, before.lines[i].text) != 0) {
                changed++;
                if (!strchr(after.lines[i].text, '7')) {
                    printf("editortest: FAIL the typed digit is not in line %u: \"%s\"\n",
                           (unsigned)(i + 1u), after.lines[i].text);
                    failures++;
                }
            }
        }
        if (!changed) {
            puts("editortest: FAIL a digit typed at a selected word changed nothing");
            failures++;
        } else {
            puts("editortest: a selected word took the typed digit, and the idle "
                 "task wrote it to the card");
        }
    }

    /* 2. the helper takes the digit that picks an entry: OPS `1` is INS */
    nc_visual_handle_key(NC_VISUAL_KEY_CANCEL);
    host_press('1');
    host_press('1');
    host_editor_save();
    nc_document_init(&after);
    if (nc_load_file(&after, program) != NC_OK) {
        puts("editortest: FAIL the program did not save after the helper ran");
        failures++;
    } else if (after.line_count != base_lines + 1u) {
        printf("editortest: FAIL OPS `1 INS` left %u lines, wanted %u\n",
               (unsigned)after.line_count, (unsigned)(base_lines + 1u));
        failures++;
    } else {
        printf("editortest: the helper ran the entry its digit picked\n");
    }

    /* 2b. OPS `6` is SETUP: the stock dimensions, four lines at once (`G970`
       extents, `G971` stock, `G972` chuck clamp, `G973` preview mode). Before
       this the block could only be typed one G-code at a time (`3 G`), and the
       bench asked for the way to insert it: "it seems it lacks ability to insert
       stock dimensions?". */
    nc_visual_handle_key(NC_VISUAL_KEY_CANCEL);
    host_press('1');                                 /* `1 OPS` */
    host_press('6');                                 /* `6 SETUP`: the stock */
    host_editor_save();
    nc_document_init(&after);
    if (nc_load_file(&after, program) != NC_OK) {
        puts("editortest: FAIL the program did not save after the setup block");
        failures++;
    } else {
        static const char *const setup[] = { "G970 ", "G971 ", "G972 ", "G973 " };
        unsigned wanted = base_lines + 1u + 4u;   /* the blank line, then four */
        size_t at = (size_t)-1;

        if (after.line_count != wanted) {
            printf("editortest: FAIL OPS `6 SETUP` left %u lines, wanted %u\n",
                   (unsigned)after.line_count, wanted);
            failures++;
        } else {
            /* The block lands where the cursor is, so it is looked for as a run
               of four lines rather than at a fixed index. */
            for (i = 0u; i + 3u < after.line_count; i++) {
                unsigned k;

                for (k = 0u; k < sizeof(setup) / sizeof(setup[0]); k++) {
                    if (strncmp(after.lines[i + k].text, setup[k],
                                strlen(setup[k])) != 0) {
                        break;
                    }
                }
                if (k == sizeof(setup) / sizeof(setup[0])) {
                    at = i;
                    break;
                }
            }
            if (at == (size_t)-1) {
                puts("editortest: FAIL the four setup lines are not in the "
                     "program, in order");
                failures++;
            } else {
                puts("editortest: OPS `6 SETUP` inserted the stock dimensions");
            }
        }
    }

    /* 2c. OPS holds only what has no other key: the new line and the stock block.
       The file list is `0`, delete is `*`, the end mark and the line/arc rows are
       the G7X menu's and the `3 G` field's jobs, and the save is the panel's own
       idle task (bench: "except save - we do not need any extras... and save is
       basically not needed too. it is fine with auto save. and it should be").
       The pad's nine slots are checked as drawn by `--padtest`; what matters here
       is that the pad is not a second copy of the keyboard. */
    nc_visual_handle_key(NC_VISUAL_KEY_CANCEL);
    host_press('1');                                 /* `1 OPS` */
    host_press('2');                                 /* FILES: not there any more */
    host_press('1');                                 /* `1 OPS` */
    host_press('4');                                 /* SAVE: not there any more */
    if (nc_files_active()) {
        puts("editortest: FAIL an OPS key that has no entry still acts");
        failures++;
    } else if (nc_load_file(&after, program) != NC_OK ||
               after.line_count != base_lines + 1u + 4u) {
        printf("editortest: FAIL OPS `2`/`4` wrote something (%u lines)\n",
               (unsigned)after.line_count);
        failures++;
    } else {
        puts("editortest: OPS holds the entries with no other key");
    }

    /* 2d. the TOOL pad's machine words are sections too, so their text is the
       card's (`[23] M6`, `[24] M3`, `[25] STOP`, `[26] M4`). `2` opens the pad,
       `3` writes the tool change. */
    nc_visual_handle_key(NC_VISUAL_KEY_CANCEL);
    host_press('2');                                 /* `2 TOOL` */
    host_press('3');                                 /* `3 M6` */
    host_editor_save();
    nc_document_init(&after);
    if (nc_load_file(&after, program) != NC_OK) {
        puts("editortest: FAIL the program did not save after the tool word");
        failures++;
    } else {
        bool saw_m6 = false;

        for (i = 0u; i < after.line_count; i++) {
            if (strcmp(after.lines[i].text, "M6") == 0) {
                saw_m6 = true;
            }
        }
        if (!saw_m6) {
            puts("editortest: FAIL TOOL `3` did not write the tool change");
            failures++;
        } else {
            puts("editortest: TOOL `3 M6` writes the card's tool word");
        }
    }

    /* 3. the word pad: `3 WORD` holds the field that writes one line by its
       number (`1 G`), and the card's own single-line entries beside it. Type a
       G-code and its template lands; the check reads the *exact* row, because
       the fixture has `G1` rows of its own and "some line starts with G1" would
       pass without the key doing anything. */
    nc_visual_handle_key(NC_VISUAL_KEY_CANCEL);
    host_press('3');                                /* `3 WORD` */
    host_press('1');                                /* `1 G`: the field */
    host_press('1');                                /* the code: G1 */
    nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);     /* `D`: take it */
    host_editor_save();
    nc_document_init(&after);
    if (nc_load_file(&after, program) != NC_OK) {
        puts("editortest: FAIL the program did not save after the G field");
        failures++;
    } else {
        bool saw_template = false;

        for (i = 0u; i < after.line_count; i++) {
            if (strcmp(after.lines[i].text, "G1 X0 Z0 C0 R0") == 0) {
                saw_template = true;
            }
        }
        if (!saw_template) {
            puts("editortest: FAIL the typed G code did not reach a template line");
            failures++;
        } else {
            puts("editortest: the G field took G1 and wrote its template");
        }
    }

    /* 3b. and the card's single-line entries beside it: `3 WORD` `2` is `[32]
       CHMF`, a row that starts with a space - so it continues the line the
       cursor is on instead of starting one. */
    nc_visual_handle_key(NC_VISUAL_KEY_CANCEL);
    host_press('3');                                /* `3 WORD` */
    host_press('2');                                /* `2 CHMF` */
    host_editor_save();
    nc_document_init(&after);
    if (nc_load_file(&after, program) != NC_OK) {
        puts("editortest: FAIL the program did not save after CHMF");
        failures++;
    } else {
        bool saw_chamfer = false;

        for (i = 0u; i < after.line_count; i++) {
            if (strstr(after.lines[i].text, " C0") != 0) {
                saw_chamfer = true;
            }
        }
        if (!saw_chamfer) {
            puts("editortest: FAIL WORD `2 CHMF` did not add its word in line");
            failures++;
        } else {
            puts("editortest: WORD `2 CHMF` adds the card's corner word in line");
        }
    }

    /* 4. the G7X range words: `4 N` numbers the line the cursor is on (the block
       number goes first), and `4 Q` picks the Q already on a header instead of
       writing a second one. This is how a P/Q range is written on the panel. */
    nc_visual_handle_key(NC_VISUAL_KEY_CANCEL);
    host_press('4');                                /* `4 G7X` */
    host_press('5');                                /* `5 N`: number the line */
    host_editor_save();
    nc_document_init(&after);
    if (nc_load_file(&after, program) != NC_OK) {
        puts("editortest: FAIL the program did not save after the N word");
        failures++;
    } else {
        bool saw_numbered_row = false;
        bool saw_second_q = false;
        int q_count = 0;

        for (i = 0u; i < after.line_count; i++) {
            if (strncmp(after.lines[i].text, "N0 ", 3) == 0) {
                saw_numbered_row = true;
            }
        }
        /* `4 Q` on the fanuc header: the Q it already carries is picked, so the
           program still has exactly one Q word on that line. */
        host_press('4');
        host_press('4');
        host_editor_save();
        nc_document_init(&after);
        if (nc_load_file(&after, program) != NC_OK) {
            puts("editortest: FAIL the program did not save after the Q word");
            failures++;
        } else {
            for (i = 0u; i < after.line_count; i++) {
                const char *q = after.lines[i].text;

                while ((q = strchr(q, 'Q')) != NULL) {
                    q_count++;
                    q++;
                    if (q_count > 1 && strncmp(after.lines[i].text, "G7", 2) == 0) {
                        saw_second_q = true;
                    }
                }
            }
        }
        if (!saw_numbered_row) {
            puts("editortest: FAIL `4 N` did not number the row");
            failures++;
        } else if (saw_second_q) {
            puts("editortest: FAIL `4 Q` wrote a second Q on the header");
            failures++;
        } else {
            printf("editortest: the range words are written on the line (%d Q)\n",
                   q_count);
        }
    }

    /* 4. the footer's own keys reach the editor: `*` deletes the line the
       cursor is on (the blank the helper inserted above), so the program is
       back to two lines more than the fixture - the blank from the helper and
       the template line. */
    nc_visual_handle_key(NC_VISUAL_KEY_CANCEL);
    host_press('*');
    host_editor_save();
    nc_document_init(&after);
    if (nc_load_file(&after, program) != NC_OK) {
        puts("editortest: FAIL the program did not save after delete");
        failures++;
    } else if (after.line_count < base_lines) {
        printf("editortest: FAIL delete left %u lines, the fixture has %u\n",
               (unsigned)after.line_count, (unsigned)base_lines);
        failures++;
    } else {
        printf("editortest: the footer key deleted through the editor\n");
    }

    if (failures) {
        printf("editortest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("editortest: PASS typed keys reach the word, the helper, the field and "
         "the footer actions");
    return 0;
}

/* The expansion, declared here because the pad's check reads it: the
   definitions sit with the other emit helpers below. */
#define HOST_EMIT_MAX 400
static bool host_emit_lines(const nc_document_t *doc,
                            char lines[][NC_MAX_LINE_LEN],
                            size_t max,
                            size_t *count,
                            g7x_result_t *err);

/* The contour pad: `4 G7X`, then `7`, and every press writes one row of the
   profile - the axis that moves at the step, the other carried over from the
   point the row above reached - with the pad still up for the next press until
   `5`. Every check reads the program back off the card, because the rows are
   the whole point of the pad:

     1. `4` `7` opens it, and the first press continues from the program's own
        point (the `G0 X52 Z2` above it) instead of inventing one;
     2. a press, then another, write one row each, in order, and the axis that
        does not move is carried over;
     3. while the word the pad picked is still picked, the digits are the
        editor's - the value is typed over the prefill - and `D` hands the pad
        back its digits;
     4. `#` steps the distance, so the next row moves by the new step;
     5. `*` drops the point: the row being entered goes, and the pad keeps no
        undo of its own;
     6. `5` ends the contour;
     7. with no point anywhere in the program, the first row starts at the
        stock's corner, which is where a lathe profile starts;
     8. leaving the screen closes the pad, and the rows already written stay.

   What the pad draws is the editor's own three by three, so a frame is not the
   check here; where the rows land is what the machine would cut, and the
   expansion is asked for the same reason. */
static int host_contourtest(void)
{
    static const char *const program = "/D/nc/files/contour.nc";
    static const char *const fixture = "G0 X52 Z2\n";
    char lines[HOST_EMIT_MAX][NC_MAX_LINE_LEN];
    nc_document_t doc;
    g7x_result_t err = G7X_OK;
    size_t emitted = 0u;
    int failures = 0;
    unsigned i;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    if (!host_fs_write_text(program, fixture) ||
        !host_fs_write_text("/D/nc_state.txt",
                            "MODE=EDIT\nEDIT=/D/nc/files/contour.nc\n")) {
        puts("contourtest: FAIL cannot set up the fixture");
        return 1;
    }
    host_init_core();
    nc_visual_select_mode(NC_MODE_PROGRAM);
    host_pump_idle(64u);

    /* 1. the pad opens on the G7X submenu's `7`, from the program's own point. */
    host_press('4');
    host_press('7');
    if (!nc_editor_contour_active()) {
        puts("contourtest: FAIL `4` `7` did not open the contour pad");
        return 1;
    }
    host_press('2');                            /* X+ */
    /* 3. the word the pad picked is the editor's while it is picked: type the
       real value over the step's prefill, then `D` gives the pad its digits
       back. */
    host_press('3');
    host_press('0');
    nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);
    host_editor_flush();
    nc_document_init(&doc);
    if (nc_load_file(&doc, program) != NC_OK || doc.line_count != 2u ||
        strcmp(doc.lines[1].text, "G1 X30 Z2") != 0) {
        printf("contourtest: FAIL the first point is \"%s\"\n",
               doc.line_count > 1u ? doc.lines[1].text : "");
        failures++;
    } else {
        puts("contourtest: the point continues from the program, and its value is "
             "typed over the prefill");
    }

    /* 2. the next press reads the point from the row just written, and carries
       the axis that does not move. */
    host_press('4');                            /* Z- */
    host_editor_flush();
    nc_document_init(&doc);
    if (nc_load_file(&doc, program) != NC_OK || doc.line_count != 3u ||
        strcmp(doc.lines[1].text, "G1 X30 Z2") != 0 ||
        strcmp(doc.lines[2].text, "G1 X30 Z1.5") != 0) {
        printf("contourtest: FAIL the second point is \"%s\"\n",
               doc.line_count > 2u ? doc.lines[2].text : "");
        failures++;
    } else {
        puts("contourtest: the walk carries the axis that does not move");
    }

    /* 4. `#` steps the distance - but only once the point is settled. */
    nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);
    nc_visual_handle_key(NC_VISUAL_KEY_FINISH);
    host_press('2');                            /* X+ by the new step */
    nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);
    host_editor_flush();
    nc_document_init(&doc);
    if (nc_load_file(&doc, program) != NC_OK || doc.line_count != 4u ||
        strcmp(doc.lines[3].text, "G1 X31 Z1.5") != 0) {
        printf("contourtest: FAIL the stepped point is \"%s\"\n",
               doc.line_count > 3u ? doc.lines[3].text : "");
        failures++;
    } else {
        puts("contourtest: `#` steps the distance");
    }

    /* 5. `*` drops the point being entered - the row goes with it - and the pad
       is still up. */
    host_press('2');                            /* a point, still being entered */
    nc_visual_handle_key(NC_VISUAL_KEY_BACKSPACE);
    if (!nc_editor_contour_active()) {
        puts("contourtest: FAIL `*` closed the pad");
        failures++;
    }
    host_editor_flush();
    nc_document_init(&doc);
    if (nc_load_file(&doc, program) != NC_OK || doc.line_count != 4u) {
        printf("contourtest: FAIL `*` left %u rows\n", (unsigned)doc.line_count);
        failures++;
    } else {
        puts("contourtest: `*` drops the point being entered");
    }

    /* 6. `5` ends the contour, and the rows already written stay. */
    host_press('5');
    if (nc_editor_contour_active()) {
        puts("contourtest: FAIL `5` did not end the contour");
        failures++;
    }
    host_editor_flush();
    nc_document_init(&doc);
    if (nc_load_file(&doc, program) != NC_OK || doc.line_count != 4u) {
        printf("contourtest: FAIL `5` left %u rows\n",
               (unsigned)doc.line_count);
        failures++;
    } else {
        puts("contourtest: `5` ends the contour and keeps what it wrote");
    }

    /* 7. and the program the pad wrote is a program: the sender reads the rows
       as they stand, which is what makes a walked profile an ordinary one. */
    if (!host_emit_lines(&doc, lines, HOST_EMIT_MAX, &emitted, &err) ||
        emitted != doc.line_count) {
        printf("contourtest: FAIL the walked rows do not expand (%d)\n",
               (int)err);
        failures++;
    } else {
        for (i = 0u; i < emitted; i++) {
            if (strcmp(lines[i], doc.lines[i].text) != 0) {
                printf("contourtest: FAIL row %u leaves as \"%s\"\n",
                       i + 1u, lines[i]);
                failures++;
                break;
            }
        }
        if (i == emitted) {
            puts("contourtest: the walked rows are the lines the sender reads");
        }
    }

    /* 7. no point in the program at all: the first row starts at the stock's
       corner, moved by the step `#` left behind (1 mm here, not the 0.5 it
       started at - the distance is the operator's last choice, not a session's).
       Everything is deleted first, so there is nothing to continue from. */
    for (i = 0u; i < 8u; i++) {
        nc_visual_handle_key(NC_VISUAL_KEY_PREV);
    }
    for (i = 0u; i < 5u; i++) {
        nc_visual_handle_key(NC_VISUAL_KEY_BACKSPACE);
    }
    host_press('4');
    host_press('7');
    host_press('2');                            /* X+ */
    host_editor_flush();
    nc_document_init(&doc);
    if (nc_load_file(&doc, program) != NC_OK || doc.line_count != 1u ||
        strcmp(doc.lines[0].text, "G1 X51 Z0") != 0) {
        printf("contourtest: FAIL the empty program starts at \"%s\"\n",
               doc.line_count ? doc.lines[0].text : "");
        failures++;
    } else {
        puts("contourtest: an empty program starts at the stock's corner");
    }

    /* 8. leaving the screen closes the pad, and what it wrote stays. */
    if (!nc_editor_contour_active()) {
        puts("contourtest: FAIL the pad did not stay up");
        failures++;
    }
    nc_visual_select_mode(NC_MODE_RUN);
    if (nc_editor_contour_active()) {
        puts("contourtest: FAIL the pad outlived the screen");
        failures++;
    } else {
        puts("contourtest: leaving the screen closes the pad");
    }

    if (failures) {
        printf("contourtest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("contourtest: PASS the pad writes the profile, one row per press");
    return 0;
}

/* Expand a whole document and keep the lines, so two documents can be compared
   line for line. False with `err` set when the expansion stops. */
static bool host_emit_lines(const nc_document_t *doc,
                            char lines[][NC_MAX_LINE_LEN],
                            size_t max,
                            size_t *count,
                            g7x_result_t *err)
{
    nc_emit_stream_t stream;
    size_t src = 0u;

    *count = 0u;
    *err = G7X_OK;
    nc_emit_stream_begin(&stream, doc, 0);
    while (stream.active && *count < max) {
        nc_emit_result_t r = nc_emit_stream_next(&stream,
                                                 lines[*count],
                                                 NC_MAX_LINE_LEN,
                                                 &src);
        if (r == NC_EMIT_ERROR) {
            *err = stream.error;
            return false;
        }
        if (r == NC_EMIT_LINE) {
            (*count)++;
        }
    }
    return true;
}

/* Build a document out of a fixture. */
static void host_fill_lines(nc_document_t *doc,
                            const char *const *lines,
                            size_t count)
{
    size_t i;

    nc_document_init(doc);
    for (i = 0u; i < count; i++) {
        (void)nc_insert_line(doc, doc->line_count, lines[i]);
    }
}

/* Fanuc's increments: `U` and `W` are X and Z as distances from where the tool
   is, and this panel resolves them into the absolute line the controller reads
   (`nc_emit_line_point()` is the one place that rule lives). The check is an
   *equality* - the same geometry written absolutely and written in increments
   has to leave the sender as the same lines, inside a cycle as well as outside
   one - because that is what "collapse to normal G1 lines" means:

     1. a plain contour, both spellings: the emitted lines are equal;
     2. the same contour inside a `G71` block: the generated motion is equal,
        which also says the rows reached the generator as the points they mean;
     3. an increment whose axis was never given absolutely is left as written,
        so the controller refuses it by name instead of the sender inventing a
        position from nothing;
     4. and the document keeps what the operator wrote: the spelling is the
        program's, and only the wire is absolute. */
static int host_uwtest(void)
{
    static const char *const plain_absolute[] = {
        "G0 X50 Z2",
        "G1 Z-8.000 F0.2",
        "G1 X45.000"
    };
    static const char *const plain_increments[] = {
        "G0 X50 Z2",
        "G1 W-10 F0.2",
        "G1 U-5"
    };
    static const char *const cycle_absolute[] = {
        "G0 X52 Z2",
        "G71 U1 R0.5 X0.5 Z0.5 F450 P10 Q20",
        "N10 G0 X30 Z0",
        "G1 Z-15",
        "N20 G1 X50 Z-15",
        "G80"
    };
    static const char *const cycle_increments[] = {
        "G0 X52 Z2",
        "G71 U1 R0.5 X0.5 Z0.5 F450 P10 Q20",
        "N10 G0 U-22 W-2",
        "G1 W-15",
        "N20 G1 U20",
        "G80"
    };
    static const char *const orphan[] = {
        "G1 W-10 F0.2",
        "G1 X50"
    };
    static char lines_a[HOST_EMIT_MAX][NC_MAX_LINE_LEN];
    static char lines_b[HOST_EMIT_MAX][NC_MAX_LINE_LEN];
    nc_document_t doc;
    g7x_result_t err;
    size_t count_a = 0u;
    size_t count_b = 0u;
    size_t i;
    int failures = 0;

    host_init_core();

    /* 1. a plain contour, written both ways. */
    host_fill_lines(&doc, plain_absolute,
                    sizeof(plain_absolute) / sizeof(plain_absolute[0]));
    if (!host_emit_lines(&doc, lines_a, HOST_EMIT_MAX, &count_a, &err)) {
        printf("uwtest: FAIL the absolute contour does not expand (%d)\n",
               (int)err);
        return 1;
    }
    host_fill_lines(&doc, plain_increments,
                    sizeof(plain_increments) / sizeof(plain_increments[0]));
    if (!host_emit_lines(&doc, lines_b, HOST_EMIT_MAX, &count_b, &err)) {
        printf("uwtest: FAIL the incremental contour does not expand (%d)\n",
               (int)err);
        return 1;
    }
    if (count_a != count_b) {
        printf("uwtest: FAIL %u lines against %u\n",
               (unsigned)count_a, (unsigned)count_b);
        failures++;
    } else {
        for (i = 0u; i < count_a; i++) {
            if (strcmp(lines_a[i], lines_b[i]) != 0) {
                printf("uwtest: FAIL line %u is \"%s\", the absolute spelling "
                       "gives \"%s\"\n", (unsigned)(i + 1u), lines_b[i],
                       lines_a[i]);
                failures++;
                break;
            }
        }
        if (i == count_a) {
            printf("uwtest: %u lines, the increments and the absolutes agree\n",
                   (unsigned)count_a);
        }
    }

    /* 2. and inside a cycle, where the rows are the generator's input. */
    host_fill_lines(&doc, cycle_absolute,
                    sizeof(cycle_absolute) / sizeof(cycle_absolute[0]));
    if (!host_emit_lines(&doc, lines_a, HOST_EMIT_MAX, &count_a, &err)) {
        printf("uwtest: FAIL the absolute cycle does not expand (%d)\n",
               (int)err);
        return 1;
    }
    host_fill_lines(&doc, cycle_increments,
                    sizeof(cycle_increments) / sizeof(cycle_increments[0]));
    if (!host_emit_lines(&doc, lines_b, HOST_EMIT_MAX, &count_b, &err)) {
        printf("uwtest: FAIL the incremental cycle does not expand (%d)\n",
               (int)err);
        return 1;
    }
    if (count_a < 20u || count_a != count_b) {
        printf("uwtest: FAIL the cycle expanded to %u lines against %u\n",
               (unsigned)count_a, (unsigned)count_b);
        failures++;
    } else {
        for (i = 0u; i < count_a; i++) {
            if (strcmp(lines_a[i], lines_b[i]) != 0) {
                printf("uwtest: FAIL cycle line %u is \"%s\", the absolute "
                       "spelling gives \"%s\"\n", (unsigned)(i + 1u),
                       lines_b[i], lines_a[i]);
                failures++;
                break;
            }
        }
        if (i == count_a) {
            printf("uwtest: the cycle expands to the same %u moves either way\n",
                   (unsigned)count_a);
        }
    }

    /* 3. an increment with nothing to count from is left as written. */
    host_fill_lines(&doc, orphan, sizeof(orphan) / sizeof(orphan[0]));
    if (!host_emit_lines(&doc, lines_a, HOST_EMIT_MAX, &count_a, &err) ||
        count_a == 0u) {
        puts("uwtest: FAIL the orphan-increment program does not expand");
        failures++;
    } else if (strcmp(lines_a[0], "G1 W-10 F0.2") != 0) {
        printf("uwtest: FAIL an unresolved increment was sent as \"%s\"\n",
               lines_a[0]);
        failures++;
    } else {
        puts("uwtest: an increment with nothing to count from is left as "
             "written, for the controller to refuse");
    }

    /* 4. the program keeps the spelling the operator wrote. */
    host_fill_lines(&doc, plain_increments,
                    sizeof(plain_increments) / sizeof(plain_increments[0]));
    if (strcmp(doc.lines[1].text, "G1 W-10 F0.2") != 0 ||
        strcmp(doc.lines[2].text, "G1 U-5") != 0) {
        printf("uwtest: FAIL the document was rewritten to \"%s\" / \"%s\"\n",
               doc.lines[1].text, doc.lines[2].text);
        failures++;
    } else {
        puts("uwtest: the program keeps the increments it was written with");
    }

    if (failures) {
        printf("uwtest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("uwtest: PASS the increments collapse to the lines they mean");
    return 0;
}

/* What the RUN keys that start a program send. `1 SINGLE` sends the line the
   cursor is on (or the whole block it belongs to); `2 FROM` runs from the
   cursor to the end; `3 FULL` runs the whole program. FROM and FULL used to
   only *arm* the run - the status said "Run from line N" and the machine never
   received a character, because nothing handed the reader to the run. The
   check reads the same reader the controller reads: the source lines the panel
   sends, in order, with the setup rows (G970-G973) skipped, and the run done
   when the program ends. */
static int host_runtest(void)
{
    static const char *const full_wanted =
        "T2\n"
        "G71 U1 R0.2 X0.5 Z0.5 F450\n"
        "G1 X30 Z0\n"
        "G1 X30 Z-15 C0 R0\n"
        "G1 X35 Z-15 C0 R2\n"
        "G1 X35 Z-25 C0 R5\n"
        "G1 X50.000 Z-25\n"
        "G80\n";
    static const char *const from_wanted =
        "G1 X30 Z0\n"
        "G1 X30 Z-15 C0 R0\n"
        "G1 X35 Z-15 C0 R2\n"
        "G1 X35 Z-25 C0 R5\n"
        "G1 X50.000 Z-25\n"
        "G80\n";
    char sent[512];
    char raw[1024];
    int failures = 0;
    unsigned i;
    size_t n;
    size_t r;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    if (!host_fs_write_text("/D/nc_state.txt",
                            "MODE=RUN\nRUN=/D/nc/files/facing.nc\n")) {
        puts("runtest: FAIL cannot point RUN at the fixture");
        return 1;
    }
    host_init_core();
    host_pump_idle(64u);

    /* `3 FULL`: the whole program, from line 1. */
    host_press('3');
    host_read_run(sent, sizeof(sent), &n, raw, sizeof(raw), &r);
    printf("runtest: FULL sent %s\n", raw);
    if (strcmp(sent, full_wanted) != 0) {
        printf("runtest: FAIL FULL sent \"%s\"\n", sent);
        failures++;
    }
    if (nc_run_active()) {
        puts("runtest: FAIL FULL left the run active after the last line");
        failures++;
    }

    /* `2 FROM`: the same program from the line the cursor is on. */
    host_press('#');
    for (i = 0u; i < 12u; i++) {
        nc_visual_handle_key(NC_VISUAL_KEY_PREV);
    }
    for (i = 0u; i < 6u; i++) {
        nc_visual_handle_key(NC_VISUAL_KEY_NEXT);
    }
    host_press('2');
    host_read_run(sent, sizeof(sent), &n, raw, sizeof(raw), &r);
    printf("runtest: FROM sent %s\n", raw);
    if (strcmp(sent, from_wanted) != 0) {
        printf("runtest: FAIL FROM sent \"%s\"\n", sent);
        failures++;
    }

    /* One cursor in RUN: the pane marks the line the sender is on, and SINGLE
       acts on what is marked - the line itself, or the G7x block it sits in.
       A streamed run advances the sender's line, while SINGLE used to send the
       document's cursor - which stayed where the block started, so the mark was
       a line ahead and the key acted somewhere else (bench: "it was marking next
       line after current g71, but then i pressed run single - it still seems to
       have marked original one with g71. only if i go back/forward it is ok" -
       the line keys sync the two, which is why moving fixed it by hand). */
    {
        nc_document_t doc;
        size_t start = 0u;
        size_t end = 0u;
        char want[NC_MAX_LINE_LEN * 8];
        size_t at;

        char discard[1024];
        size_t discard_len = 0u;

        /* A whole-program run again, so the sender ends past the block it ran. */
        host_press('#');
        host_press('3');
        host_read_run(sent, sizeof(sent), &n, discard, sizeof(discard), &discard_len);
        at = nc_run_line();
        want[0] = '\0';
        if (nc_load_file(&doc, "/D/nc/files/facing.nc") == NC_OK && doc.line_count > 0u) {
            if (at >= doc.line_count) {
                at = doc.line_count - 1u;
            }
            /* The mark is inside the cycle block, so the step is the block. */
            if (nc_g7x_block_containing(&doc, at, &start, &end)) {
                size_t k;
                size_t used = 0u;

                for (k = start; k <= end && used + NC_MAX_LINE_LEN < sizeof(want); k++) {
                    used += (size_t)snprintf(want + used, sizeof(want) - used,
                                             "%s\n", doc.lines[k].text);
                }
            } else {
                snprintf(want, sizeof(want), "%s\n", doc.lines[at].text);
            }
        }
        host_press('1');                     /* SINGLE: the line the run is on */
        host_read_run(sent, sizeof(sent), &n, discard, sizeof(discard), &discard_len);
        if (want[0] == '\0' || strcmp(sent, want) != 0) {
            printf("runtest: FAIL SINGLE after the run sent \"%s\", wanted the "
                   "block the mark is in \"%s\"\n", sent, want);
            failures++;
        } else {
            printf("runtest: SINGLE acts on what the run marks (%u lines from the "
                   "block)\n", (unsigned)(end - start + 1u));
        }

        /* And a line outside every block is its own step: `T2` above the block. */
        host_press('#');
        for (i = 0u; i < 12u; i++) {
            nc_visual_handle_key(NC_VISUAL_KEY_PREV);
        }
        for (i = 0u; i < 4u; i++) {        /* the name row, then `T2` at line 4 */
            nc_visual_handle_key(NC_VISUAL_KEY_NEXT);
        }
        host_press('1');
        host_read_run(sent, sizeof(sent), &n, discard, sizeof(discard), &discard_len);
        if (strcmp(sent, "T2\n") != 0) {
            printf("runtest: FAIL SINGLE on a line outside a block sent \"%s\"\n", sent);
            failures++;
        } else {
            puts("runtest: a line outside every block is its own step");
        }
    }

    if (failures) {
        printf("runtest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("runtest: PASS FROM runs from the cursor and FULL runs the program");
    return 0;
}

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

/* What one editor pane row is painted with: the colour most of its text band
   carries. The band is a row high and its glyphs never cover it, so the mode of
   the pixels across it is the row's own background. */
static uint32_t host_pane_row_bg(int row)
{
    uint32_t seen[8];
    int counts[8];
    int distinct = 0;
    int best = -1;
    int y = NC_CODE_Y + row * NC_VISUAL_ROW_H + NC_VISUAL_ROW_H / 2;
    int x;

    for (x = NC_RIGHT_PANE_X + NC_LINE_TEXT_X_PAD;
         x < NC_RIGHT_PANE_X + NC_RIGHT_PANE_W - 3; x++) {
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

/* Every code-pane row has to carry the mark its line deserves: bright on the
   line in play, pale on the rest of the cycle that line heads, and the pane's own
   ground everywhere else. */
static int host_check_pane_rows(const nc_document_t *doc,
                                size_t mark,
                                size_t block_first,
                                size_t block_last,
                                bool in_block,
                                uint32_t select_rgb,
                                uint32_t block_rgb,
                                uint32_t bg_rgb)
{
    int failures = 0;
    int row;

    for (row = 0; row < (int)doc->line_count; row++) {
        bool pale = in_block && (size_t)row >= block_first &&
                    (size_t)row <= block_last;
        uint32_t want = ((size_t)row == mark) ? select_rgb
                                              : (pale ? block_rgb : bg_rgb);
        uint32_t got = host_pane_row_bg(row);

        if (got != want) {
            printf("blocktest: FAIL row %d is %06lX, wanted %06lX (%s)\n",
                   row + 1, (unsigned long)got, (unsigned long)want,
                   (size_t)row == mark ? "the line in play"
                                       : (pale ? "a line of its path"
                                               : "an unmarked line"));
            failures++;
        }
    }
    return failures;
}

/* What the code screens mark: the line in play, and the cycle it heads. Two
   colours, one meaning, wherever the code is read - in RUN the line in play is
   the unit the machine is on, in EDIT it is the cursor, the line the digits type
   into, and the pale path behind it is the cycle the operator is working on. A
   line outside every cycle has no path and marks only itself.

   Both the mark and the path come from the same `nc_g7x_block_containing()` the
   sender uses, so neither screen can name a different set of lines than the
   block, and nothing has to keep an extracted path list in step with it. The
   check reads the frame - a snapshot carrying the block while the pane paints
   every row flat is exactly what it is for - and it walks both screens and both
   cases, because the two publish the mark from different places (`nc_visual_draw()`
   takes RUN's line from the sender and EDIT's from the document's cursor).

   RUN's mark is the *unit the machine is on*, which is not the same answer as the
   sender's position: once a unit has run, `nc_run_display_line()` holds it until
   the operator takes the cursor, while `nc_run_line()` has already walked on to
   the line the next step comes from. Both are checked here, because a pane that
   read the sender's line marked a cycle header that had never been run. */
static int host_blocktest(void)
{
    static const char *const program = "/D/nc/files/path.nc";
    /* A numbered cycle in the middle, with the finish cut that replays it below:

         1  G0 X54 Z2
         2  G71 U1 R1 P100 Q200 X0.1 Z0.1 F300     (the block: 2..5)
         3  N100 G1 X50 Z0
        4  G1 X50 Z-10
        5  N200 G1 X40 Z-10
        6  G70 P100 Q200                            (its path: 3..5)
        7  G1 Xbad                                  (refused: the run stops here)
        8  G0 X80 Z0

       The G70 sits *below* the range it names, which is the case the bench
       reported as unmarked: a finish cut owns rows that are above it. */
    static const char *const text =
        "G0 X54 Z2\n"
        "G71 U1 R1 P100 Q200 X0.1 Z0.1 F300\n"
        "N100 G1 X50 Z0\n"
        "G1 X50 Z-10\n"
        "N200 G1 X40 Z-10\n"
        "G70 P100 Q200\n"
        "G1 Xbad\n"
        "G0 X80 Z0\n";
    const size_t in_cycle = 3u;    /* `G1 X50 Z-10`, inside the P/Q range */
    const size_t finish = 5u;      /* `G70 P100 Q200`, the finish cut */
    const size_t outside = 0u;     /* the rapid before the cycle */
    const size_t bad_line = 6u;    /* `G1 Xbad`: the run must stop here */
    nc_document_t doc;
    size_t start = 0u;
    size_t end = 0u;
    size_t range_first = 0u;
    size_t range_last = 0u;
    uint32_t select_rgb;
    uint32_t block_rgb;
    uint32_t bg_rgb;
    int failures = 0;
    size_t i;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    if (!host_fs_write_text(program, text) ||
        !host_fs_write_text("/D/nc_state.txt",
                            "MODE=EDIT\nEDIT=/D/nc/files/path.nc\n"
                            "RUN=/D/nc/files/path.nc\n")) {
        puts("blocktest: FAIL cannot point EDIT and RUN at the fixture");
        return 1;
    }
    host_init_core();
    host_pump_idle(64u);

    nc_document_init(&doc);
    if (nc_load_file(&doc, program) != NC_OK || doc.line_count == 0u) {
        puts("blocktest: FAIL the fixture did not load");
        return 1;
    }
    if (doc.line_count > NC_MAX_VISIBLE_LINES) {
        printf("blocktest: FAIL the fixture is %u lines, longer than the pane's "
               "%u (the rows would scroll)\n",
            (unsigned)doc.line_count, (unsigned)NC_MAX_VISIBLE_LINES);
        return 1;
    }
    /* The two answers the finder has to give: the block a row sits in, and the
       range a finish cut names above it. */
    if (!nc_g7x_block_containing(&doc, in_cycle, &start, &end) ||
        !nc_g7x_line_path(&doc, finish, &range_first, &range_last) ||
        range_first == start) {
        puts("blocktest: FAIL the fixture does not read as a cycle plus a "
             "finish cut");
        return 1;
    }
    printf("blocktest: line %u is in the block at %u..%u, the finish cut names "
           "%u..%u\n",
           (unsigned)(in_cycle + 1u), (unsigned)(start + 1u), (unsigned)(end + 1u),
           (unsigned)(range_first + 1u), (unsigned)(range_last + 1u));

    select_rgb = host_panel_rgb(NC_VISUAL_SELECT);
    block_rgb = host_panel_rgb(NC_VISUAL_SELECT_BLOCK);
    bg_rgb = host_panel_rgb(NC_VISUAL_BG);
    printf("blocktest: drawn colours - marked %06lX, block %06lX, plain %06lX\n",
           (unsigned long)select_rgb, (unsigned long)block_rgb,
           (unsigned long)bg_rgb);
    if (block_rgb == bg_rgb || block_rgb == select_rgb) {
        puts("blocktest: FAIL the block colour is not a mark of its own");
        return 1;
    }

    /* EDIT first: the rows are read and written there, so this is where the path
       has to be visible. The cursor keeps the bright mark because it is the line
       the digits go into. */
    for (i = 0u; i < in_cycle; i++) {
        nc_visual_handle_key(NC_VISUAL_KEY_NEXT);
    }
    nc_visual_draw();
    failures += host_check_pane_rows(&doc, in_cycle, start, end, true,
                                     select_rgb, block_rgb, bg_rgb);

    /* The finish cut: its path is the range above it, not the block it sits
       after - the bench's "it does not mark path region if g70 is after g71". */
    for (i = in_cycle; i < finish; i++) {
        nc_visual_handle_key(NC_VISUAL_KEY_NEXT);
    }
    nc_visual_draw();
    failures += host_check_pane_rows(&doc, finish, range_first, range_last, true,
                                     select_rgb, block_rgb, bg_rgb);

    /* Out of every cycle: the rapid above the header is a step of its own, and
       nothing keeps the pale path. */
    for (i = 0u; i < finish - outside; i++) {
        nc_visual_handle_key(NC_VISUAL_KEY_PREV);
    }
    nc_visual_draw();
    failures += host_check_pane_rows(&doc, outside, outside, outside, false,
                                     select_rgb, block_rgb, bg_rgb);

    /* RUN: the same two colours, with the line in play taken from the sender. `C`
       is a line step in RUN, the key the bench used, and it marks the screen for
       repaint. */
    nc_visual_select_mode(NC_MODE_RUN);
    /* The screens share the cursor: RUN opens on the line EDIT was left on. */
    if (nc_run_line() != outside) {
        printf("blocktest: FAIL RUN opened on line %u, wanted %u\n",
               (unsigned)(nc_run_line() + 1u), (unsigned)(outside + 1u));
        return 1;
    }
    for (i = outside; i < in_cycle; i++) {
        nc_visual_handle_key(NC_VISUAL_KEY_FIELD_NEXT);
    }
    if (nc_run_line() != in_cycle) {
        printf("blocktest: FAIL the line keys left RUN on line %u, wanted %u\n",
               (unsigned)(nc_run_line() + 1u), (unsigned)(in_cycle + 1u));
        return 1;
    }
    nc_visual_draw();
    /* Idle RUN is the EDIT rule again - the line in play is the cursor's, its
       block is the path. (While a *run* is in flight the bright line is the
       unit's head instead, which `--pacetest` watches from the machine's side.) */
    failures += host_check_pane_rows(&doc, in_cycle, start, end, true,
                                     select_rgb, block_rgb, bg_rgb);

    /* And on the finish cut: one line in play, the range it replays pale. */
    for (i = in_cycle; i < finish; i++) {
        nc_visual_handle_key(NC_VISUAL_KEY_FIELD_NEXT);
    }
    if (nc_run_line() != finish) {
        printf("blocktest: FAIL the line keys left RUN on line %u, wanted %u\n",
               (unsigned)(nc_run_line() + 1u), (unsigned)(finish + 1u));
        return 1;
    }
    nc_visual_draw();
    failures += host_check_pane_rows(&doc, finish, range_first, range_last, true,
                                     select_rgb, block_rgb, bg_rgb);

    /* A `1 SINGLE` from inside a block: the whole block is what goes out, but
       the mark stays on the line the step was taken from - the line in play -
       with the block's other rows pale around it, and it is still there when
       the machine has finished. It must not jump up to the block's header
       (bench: "it still marks next row with g71, not the one starting with
       N50") and it must not walk on to the next line either (bench: "now it
       runs but it marks also next g71. which it should not mark"). The *sender*
       does walk on - it is the line the next step of a run comes from - so the
       two answers are checked separately below. */
    for (i = 0u; i < 12u; i++) {
        nc_visual_handle_key(NC_VISUAL_KEY_FIELD_PREV);
    }
    for (i = 0u; i < in_cycle; i++) {
        nc_visual_handle_key(NC_VISUAL_KEY_FIELD_NEXT);
    }
    if (nc_run_line() != in_cycle) {
        printf("blocktest: FAIL cannot leave RUN on line %u\n",
               (unsigned)(in_cycle + 1u));
        return 1;
    }
    host_press('1');                                  /* SINGLE: the block */
    for (i = 0u; i < 40u; i++) {
        host_pump(1u);
    }
    nc_visual_draw();
    failures += host_check_pane_rows(&doc, in_cycle, start, end, true,
                                     select_rgb, block_rgb, bg_rgb);
    for (i = 0u; i < 40000u; i++) {
        host_pump(1u);
        if (!nc_run_streaming() && host_machine_idle()) {
            break;
        }
    }
    host_pump_idle(64u);
    nc_visual_draw();
    failures += host_check_pane_rows(&doc, in_cycle, start, end, true,
                                     select_rgb, block_rgb, bg_rgb);
    if (nc_run_line() != end + 1u) {
        printf("blocktest: FAIL the step left the sender on line %u, wanted %u "
               "(the mark must not follow it)\n",
               (unsigned)(nc_run_line() + 1u), (unsigned)(end + 2u));
        failures++;
    }

    /* A refused line stops the run **on that line**: the mark goes back to it and
       the sender's position with it, so the next `1 SINGLE` acts on what has to
       be fixed (bench: "after error it still goes to next line"). The fixture's
       `G1 Xbad` has a line after it on purpose - if the run stopped on the *next*
       line the check would see the mark there. */
    for (i = 0u; i < 12u; i++) {
        nc_visual_handle_key(NC_VISUAL_KEY_FIELD_PREV);
    }
    for (i = 0u; i < bad_line; i++) {
        nc_visual_handle_key(NC_VISUAL_KEY_FIELD_NEXT);
    }
    host_press('1');                                  /* SINGLE: the bad line */
    host_pump_idle(256u);
    if (nc_run_error() == STATUS_OK) {
        puts("blocktest: FAIL the refused line was not reported");
        failures++;
    }
    nc_visual_draw();
    failures += host_check_pane_rows(&doc, bad_line, bad_line, bad_line, false,
                                     select_rgb, block_rgb, bg_rgb);
    if (nc_run_line() != bad_line) {
        printf("blocktest: FAIL the run stands on line %u after an error, wanted "
               "%u\n", (unsigned)(nc_run_line() + 1u), (unsigned)(bad_line + 1u));
        failures++;
    }

    /* RUN owns the cursor: once a run is in flight the line keys must not move
       the line the sender is on (bench: "i can move cursor in run mode which
       should be non - if i do run ucnc is owner of the cursor"). They work
       again when nothing is running, which is what `2 FROM` needs. */
    host_press('3');                                  /* FULL */
    {
        size_t before = nc_run_line();

        nc_visual_handle_key(NC_VISUAL_KEY_FIELD_NEXT);
        nc_visual_handle_key(NC_VISUAL_KEY_FIELD_PREV);
        nc_visual_handle_key(NC_VISUAL_KEY_NEXT);
        if (nc_run_line() != before) {
            printf("blocktest: FAIL a key moved the line to %u while a run was "
                   "armed (it was on %u)\n",
                   (unsigned)(nc_run_line() + 1u), (unsigned)(before + 1u));
            failures++;
        }
    }
    nc_visual_handle_key(NC_VISUAL_KEY_FINISH);       /* `#` RELOAD: run reset */
    {
        size_t before = nc_run_line();

        nc_visual_handle_key(NC_VISUAL_KEY_FIELD_NEXT);
        if (nc_run_line() != before + 1u) {
            printf("blocktest: FAIL the line keys stopped working with the run "
                   "over (line %u, wanted %u)\n",
                   (unsigned)(nc_run_line() + 1u), (unsigned)(before + 2u));
            failures++;
        }
    }

    /* A single step is not a program run: when the machine has run it the panel
       has to be idle again - the DRO's colour and its state word follow this one
       answer - and the mark stands on the line that ran, not on the one the
       sender moved on to. */
    nc_visual_handle_key(NC_VISUAL_KEY_FIELD_PREV);   /* back onto the rapid */
    if (nc_run_line() != outside) {
        printf("blocktest: FAIL the line keys left RUN on line %u, wanted %u\n",
               (unsigned)(nc_run_line() + 1u), (unsigned)(outside + 1u));
        return 1;
    }
    host_press('1');                                  /* SINGLE on the rapid */
    /* Pump past the step: the parser has to take the block and the machine has
       to run it, and the pacer needs a pass with everything idle to notice that
       the step is over. */
    for (i = 0u; i < 20000u; i++) {
        host_pump(1u);
        if (!nc_run_active() && host_machine_idle() && !grbl_stream_available()) {
            break;
        }
    }
    host_pump_idle(64u);
    if (nc_run_active()) {
        puts("blocktest: FAIL a single step left the panel believing a run is "
             "in flight");
        failures++;
    }
    if (nc_run_line() != outside + 1u) {
        printf("blocktest: FAIL the step left the sender on line %u, wanted %u\n",
               (unsigned)(nc_run_line() + 1u), (unsigned)(outside + 2u));
        failures++;
    }
    nc_visual_draw();
    failures += host_check_pane_rows(&doc, outside, outside, outside, false,
                                     select_rgb, block_rgb, bg_rgb);

    if (failures) {
        printf("blocktest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("blocktest: PASS EDIT and RUN mark the line, its cycle and the range a "
         "finish cut names, and RUN owns the cursor");
    return 0;
}

/* The RUN sender is *paced*: the panel hands the machine one unit, waits until
   it has run, and only then hands over the next one. That is what keeps the
   pane's mark on the code that is cutting. The old stream gave the reader the
   whole program at once, so the sender - and with it the mark - ran ahead by the
   depth of the controller's look-ahead, which the bench read as "it is marking
   next line while running previous one".

   The check drives a program with one plain move, a G7x cycle and one more plain
   move, and watches the sender between pumps of the real parser, planner and
   virtual MCU: the sender may only move forward while the machine has finished
   what it was given, the mark may never be ahead of the sender, and while the
   cycle is cutting the mark is the block's header - the line that names the code
   that is running. */
static int host_pacetest(void)
{
    static const char *const program = "/D/nc/files/pace.nc";
    static const char *const text =
        "G0 X52 Z2\n"
        "G71 U1 R0.2 X0.5 Z0.5 F450\n"
        "G1 X30 Z0\n"
        "G1 X30 Z-15 C0 R0\n"
        "G1 X35 Z-15 C0 R2\n"
        "G1 X35 Z-25 C0 R5\n"
        "G1 X50.000 Z-25\n"
        "G80\n"
        "G0 X80 Z0\n";
    nc_runtime_state_t rt;
    nc_document_t doc;
    size_t prev;
    size_t now;
    bool saw_cycle_wait = false;
    int failures = 0;
    unsigned guard;

    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    if (!host_fs_write_text(program, text) ||
        !host_fs_write_text("/D/nc_state.txt",
                            "MODE=RUN\nRUN=/D/nc/files/pace.nc\n")) {
        puts("pacetest: FAIL cannot set up the fixture");
        return 1;
    }
    host_init_core();
    host_pump_idle(64u);

    nc_document_init(&doc);
    if (nc_load_file(&doc, program) != NC_OK || doc.line_count == 0u) {
        puts("pacetest: FAIL the fixture did not load");
        return 1;
    }

    host_press('3');                                    /* FULL */
    prev = nc_run_line();
    for (guard = 0u; guard < 20000u; guard++) {
        bool idle;

        host_pump(1u);
        idle = host_machine_idle();
        now = nc_run_line();

        /* A block may only be handed over while the machine is still busy when
           the machine is *collecting* a contour - those rows have to arrive or
           the cycle is never completed. Handing one over while a unit is running
           is the fault: that is what let the mark run ahead of the tool. */
        if (nc_run_streaming() && now > prev && !idle) {
            if (!g7x_parser_collecting()) {
                printf("pacetest: FAIL the sender started a new block at line %u "
                       "while the machine was still running\n",
                       (unsigned)(now + 1u));
                failures++;
                break;
            }
        }
        /* The mark may never be ahead of what the sender has handed over. When
           the run is over the mark stays on the unit that ran, which is still
           behind the sender - only a unit the machine has actually been given
           may be marked. */
        if (nc_run_streaming() && !idle) {
            size_t mark = nc_run_display_line();
            size_t running = 0u;

            /* The mark is a line the sender has already handed over. */
            if (mark >= now) {
                printf("pacetest: FAIL the pane marks line %u while the sender "
                       "has handed over %u lines\n",
                       (unsigned)(mark + 1u), (unsigned)now);
                failures++;
                break;
            }
            /* Past the cycle's end mark the sender is waiting for the cycle to
               cut, and the mark is the header that names it. */
            if (now == 8u) {
                if (mark != 1u || !nc_run_running(&running) || running != 1u) {
                    printf("pacetest: FAIL while the cycle cuts the mark is line "
                           "%u (wanted the header, line 2)\n",
                           (unsigned)(mark + 1u));
                    failures++;
                    break;
                }
                saw_cycle_wait = true;
            }
        }
        prev = now;
        if (!nc_run_streaming() && idle) {
            /* Nothing more is armed and the machine has run everything - the
               last block may still have been stepping when the pacer ended. */
            break;
        }
    }

    if (!saw_cycle_wait) {
        puts("pacetest: FAIL the cycle was never waited for");
        failures++;
    }
    if (nc_run_streaming()) {
        puts("pacetest: FAIL the run never ended");
        failures++;
    }
    /* The machine ran the program: the last line sends it to `X80 Z0`, which is
       X40 on the axis (the program's X is a diameter - the lathe's own frame -
       and the axis works in the radius; Z is the same in both). Nothing
       captured the characters on the way (`--runtest` does that), because a
       reader that swallows them would starve the parser - and the pacing is
       only real when the machine is the one consuming the blocks. */
    nc_state_runtime(&rt);
    printf("pacetest: the run ended at X%.3f Z%.3f, sender line %u\n",
           (double)rt.x, (double)rt.z, (unsigned)(nc_run_line() + 1u));
    if (fabs((double)rt.x - 40.0) > 0.05 || fabs((double)rt.z) > 0.05) {
        puts("pacetest: FAIL the program did not run through the machine");
        failures++;
    }
    if (failures) {
        printf("pacetest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("pacetest: PASS the sender gives one block and waits for the machine");
    return 0;
}

/* Headless check of the MANUAL stops, which are typed rather than armed at the
   current point:

     1. `*` opens the minus stop of the picked axis, `*` again takes what the
        field holds and opens the plus stop, `*` once more takes that and closes
        - so the numbers come from the pad, not from where the axis stands;
     2. an empty field takes the axis limit the setup states (the machine frame
        [0, $130] the kinematics clamp to), so three presses leave the machine's
        own travel as the stops and the operator only narrows it;
     3. `D` puts that same limit in the field, `A` leaves it alone, and typing
        again replaces what the field held;
     4. a step stops on the stop it is headed for and is refused from on it,
        while the other side still moves - neither side is a lock;
     5. a held feed covers the room to the stop it is headed for, not the whole
        travel, and the panel says which stop it feeds to.

   The virtual machine starts at X0 with no travel stated, so the setup limit
   test uses whatever `$130` the host board map gives it. */
static int host_stoptest(void)
{
    nc_runtime_state_t rt;
    char reader[128];
    double travel = (double)g_settings.max_distance[AXIS_X];
    int failures = 0;
    size_t n;
    int step;

    host_init_core();
    nc_visual_select_mode(NC_MODE_MANUAL);
    host_pump_idle(64u);
    nc_state_runtime(&rt);
    printf("stoptest: X starts at %.3f, the machine states %.1f mm of travel\n",
           (double)rt.x, travel);

    /* 1. type the stops: X from -1 to 5 machine millimetres. */
    host_press('*');
    host_press('B');                                     /* `-` the sign */
    host_press('1');
    host_press('*');                                     /* take it, open plus */
    host_press('5');
    host_press('*');                                     /* take it, done */
    if (nc_manual_stop_field_active()) {
        puts("stoptest: FAIL the third `*` did not close the stop field");
        failures++;
    }

    /* 2. a step into each stop stops on it, and the side is not a lock. */
    for (step = 0; step < 60; step++) {
        nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_2);      /* X+ */
        host_pump_idle(512u);
    }
    nc_state_runtime(&rt);
    printf("stoptest: sixty X+ steps land at %.3f\n", (double)rt.x);
    if (rt.x < 4.99f || rt.x > 5.01f) {
        printf("stoptest: FAIL the axis crossed the plus stop (X %.3f)\n",
               (double)rt.x);
        failures++;
    }
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_2);          /* into it again */
    host_pump_idle(512u);
    nc_state_runtime(&rt);
    if (rt.x > 5.01f) {
        puts("stoptest: FAIL a step from the plus stop crossed it");
        failures++;
    }
    for (step = 0; step < 80; step++) {
        nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_8);      /* X- */
        host_pump_idle(512u);
    }
    nc_state_runtime(&rt);
    printf("stoptest: eighty X- steps land at %.3f\n", (double)rt.x);
    if (rt.x < -1.01f || rt.x > -0.99f) {
        printf("stoptest: FAIL the axis crossed the minus stop (X %.3f)\n",
               (double)rt.x);
        failures++;
    }
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_8);          /* into it again */
    host_pump_idle(512u);
    nc_state_runtime(&rt);
    if (rt.x < -1.01f) {
        puts("stoptest: FAIL a step from the minus stop crossed it");
        failures++;
    }

    /* 3. a feed covers the room to the stop it is headed for. */
    nc_visual_handle_key(NC_VISUAL_KEY_FINISH);           /* `#`: feed mode */
    host_press('2');                                      /* X+, toward +5 */
    nc_visual_hold_key('2');
    n = 0u;
    while (n + 1u < sizeof(reader) && grbl_stream_available()) {
        char c = grbl_stream_getc();

        reader[n++] = c ? c : '|';
    }
    reader[n] = '\0';
    printf("stoptest: feed toward the plus stop queued \"%s\"\n", reader);
    if (strncmp(reader, "$J=G91 X12.000", 14) != 0) {
        puts("stoptest: FAIL the feed did not cover the room to the plus stop");
        failures++;
    }
    nc_visual_hold_key(0);
    host_pump_idle(64u);

    /* 4. `D` puts the axis limit the setup states in the field. The virtual
       board states no travel of its own, so the test states one: the machine
       frame is [0, $130], and the two stops are its ends. */
    g_settings.max_distance[AXIS_X] = 60.0f;
    nc_visual_handle_key(NC_VISUAL_KEY_FINISH);           /* back to step mode */
    host_press('*');                                      /* the minus field */
    nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);           /* `D`: 0.000 */
    host_press('*');                                      /* take it, open plus */
    nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);           /* `D`: 60.000 */
    host_press('*');                                      /* take it, done */
    for (step = 0; step < 30; step++) {                   /* above the low end */
        nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_2);      /* X+ */
        host_pump_idle(512u);
    }
    for (step = 0; step < 60; step++) {
        nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_8);      /* X- */
        host_pump_idle(512u);
    }
    nc_state_runtime(&rt);
    printf("stoptest: X- with the setup limits lands at %.3f (low end 0.000)\n",
           (double)rt.x);
    if (rt.x < -0.01f || rt.x > 0.01f) {
        printf("stoptest: FAIL the setup limit did not stop the axis on 0 "
               "(X %.3f)\n", (double)rt.x);
        failures++;
    }

    /* And `A` leaves a field without touching the stop it holds. */
    /* 7. The spindle starts at the speed the machine has, not at a constant.
       A program's `S` is what the panel remembers and what `M3` uses - the
       operator's speed is not forced back to a default on every start. */
    /* The spindle speed is modal and rides on a motion block, so the machine
       gets it the way a program gives it: an `S` with a move. */
    if (!nc_run_send_line("G91 G1 X0.500 S1500")) {
        puts("stoptest: FAIL cannot hand the machine a spindle speed");
        failures++;
    }
    host_pump_idle(512u);                                 /* the machine reads it */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_9);          /* M3: spindle CW */
    n = 0u;
    while (n + 1u < sizeof(reader) && grbl_stream_available()) {
        char c = grbl_stream_getc();

        reader[n++] = c ? c : '|';
    }
    reader[n] = '\0';
    printf("stoptest: spindle start queued \"%s\"\n", reader);
    if (strncmp(reader, "M3 S1500", 8) != 0) {
        puts("stoptest: FAIL the spindle did not keep the speed the machine had");
        failures++;
    }
    /* And the panel keeps it for the next boot: the state file carries it with
       the rest of what is remembered. */
    host_pump(1024u);
    nc_state_flush();
    {
        fs_file_t *fp = fs_open("/D/nc_state.txt", "r");
        char text[256];
        size_t used = 0u;

        text[0] = '\0';
        if (fp) {
            while (used + 1u < sizeof(text) && fs_available(fp)) {
                char c;

                if (fs_read(fp, (uint8_t *)&c, 1u) != 1u) {
                    break;
                }
                if (c != '\r') {
                    text[used++] = c;
                }
            }
            fs_close(fp);
            text[used] = '\0';
        }
        if (!strstr(text, "SPINDLE=1500")) {
            printf("stoptest: FAIL the remembered spindle speed is not in the "
                   "state file: \"%s\"\n", text);
            failures++;
        } else {
            puts("stoptest: the machine's speed is remembered for the next boot");
        }
    }

    /* 5. `B`/`C` pick the axis. The keypad reports them as the field-step keys,
       and the editor used to take them before this screen saw them, so the
       readout and the zero key acted on X whatever the footer said. The offset
       `0` writes names the picked axis, which is the proof. */
    host_press('B');                                      /* AXIS- */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_0);          /* ZERO */
    /* Read the block before the machine does: the parser takes the reader's
       characters as soon as the loop runs. */
    n = 0u;
    while (n + 1u < sizeof(reader) && grbl_stream_available()) {
        char c = grbl_stream_getc();

        reader[n++] = c ? c : '|';
    }
    reader[n] = '\0';
    printf("stoptest: zero after `B` wrote \"%s\"\n", reader);
    if (strncmp(reader, "G10 L20 P0 Z0", 13) != 0) {
        puts("stoptest: FAIL `B` did not pick the Z axis");
        failures++;
    }

    /* And `A` leaves a field without touching the stop it holds. */
    /* 6. A side with no typed stop falls back to the axis limit the setup
       states, so there is no "no stop" state: on Z, whose stops were never
       typed, the low end of the stated travel is already a wall. */
    g_settings.max_distance[AXIS_Z] = 40.0f;
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_4);          /* Z-: into the low end */
    /* Nothing may reach the reader: the panel is what refuses the move, not
       the controller's own clamp on the same limit. */
    n = 0u;
    while (n + 1u < sizeof(reader) && grbl_stream_available()) {
        char c = grbl_stream_getc();

        reader[n++] = c ? c : '|';
    }
    reader[n] = '\0';
    if (n != 0u) {
        printf("stoptest: FAIL the default stop let a move through: \"%s\"\n",
               reader);
        failures++;
    }
    host_pump_idle(512u);
    nc_state_runtime(&rt);
    printf("stoptest: Z- with the default limit leaves Z at %.3f\n", (double)rt.z);
    if (rt.z < -0.01f || rt.z > 0.01f) {
        puts("stoptest: FAIL the setup limit did not hold Z at its low end");
        failures++;
    }
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_6);          /* Z+: away from it */
    host_pump_idle(512u);
    nc_state_runtime(&rt);
    if (rt.z <= 0.0005f) {
        puts("stoptest: FAIL the default stop blocked a move away from it");
        failures++;
    }

    host_press('*');
    host_press('A');
    if (nc_manual_stop_field_active()) {
        puts("stoptest: FAIL `A` left the stop field open");
        failures++;
    }

    if (failures) {
        printf("stoptest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("stoptest: PASS the stops are typed, taken from the setup and respected");
    return 0;
}

/* The station's spindle, read from the machine's own signals rather than from
   a spindle encoder (the desktop has none). The tool is `spindle_pwm`: the
   planner hands it a speed already ranged to the PWM's 0..255, the tool writes
   that to PWM0 and its sign to DOUT0. The check drives the MANUAL spindle keys
   - what an operator does - and reads those two signals back through the IO
   HAL, so it is the wire that is checked, not the number the panel meant.

     1. `9` (M3) sets the direction signal and puts a speed on the PWM;
     2. `7` (M4) sets the other direction at the same speed;
     3. `5` (M5) leaves no speed on the wire;
     4. and the panel's own words follow the signal, so a spindle that is not
        running cannot be described as running.

   Both the entries and the signals are software: spindle phase, real rpm and
   the spindle being on at all are bench items. */
static int host_spindletest(void)
{
    host_spindle_t spindle;
    char text[32];
    int failures = 0;
    unsigned asked;

    host_init_core();
    nc_visual_select_mode(NC_MODE_MANUAL);
    host_pump_idle(64u);

    /* The speed the MANUAL keys use is the machine's own - the modal `S`, kept
       in the state store - so the check asks the store what is about to be
       sent rather than fixing a number of its own. */
    asked = nc_state_manual_spindle();

    /* 1. `9`: M3, spindle CW. */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_9);
    host_pump_idle(512u);
    host_spindle_read(&spindle);
    printf("spindletest: M3 asked %u, the signal carries %u rpm, dir %c\n",
           asked, spindle.rpm, spindle.dir ? spindle.dir : '-');
    if (spindle.dir != '3') {
        puts("spindletest: FAIL M3 did not set the direction signal");
        failures++;
    }
    /* The signal has to carry the speed the rest of the panel reads back: the
       DRO draws `tool_get_speed()`, and that number is the same wire, so a
       spindle no tool ever drove - an encoder-only one - shows here as zero
       against a running DRO. */
    if (spindle.rpm != (unsigned)tool_get_speed()) {
        printf("spindletest: FAIL the signal reads %u rpm, the panel reads %u\n",
               spindle.rpm, (unsigned)tool_get_speed());
        failures++;
    }
    if (spindle.rpm == 0u) {
        puts("spindletest: FAIL the spindle signal is standing after M3");
        failures++;
    }

    /* 2. `7`: M4, the other direction, same speed. */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_7);
    host_pump_idle(512u);
    host_spindle_read(&spindle);
    printf("spindletest: M4 the signal carries %u rpm, dir %c\n",
           spindle.rpm, spindle.dir ? spindle.dir : '-');
    if (spindle.dir != '4' || spindle.rpm == 0u) {
        puts("spindletest: FAIL M4 did not put the spindle on CCW");
        failures++;
    }

    /* 3. `5`: M5, nothing on the wire. */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_5);
    host_pump_idle(512u);
    host_spindle_read(&spindle);
    host_spindle_text(text, sizeof(text));
    printf("spindletest: M5 the panel reads \"%s\"\n", text);
    if (spindle.rpm != 0u || spindle.dir != 0) {
        puts("spindletest: FAIL M5 left the spindle signal standing");
        failures++;
    }
    if (strcmp(text, "spindle off") != 0) {
        puts("spindletest: FAIL the panel does not follow the stopped signal");
        failures++;
    }

    if (failures) {
        printf("spindletest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("spindletest: PASS the spindle runs from the machine's own signals");
    return 0;
}

/* The demo the station ships with: `examples\` beside the exe, which the
   release zip unpacks with the station (`lathe-demo.nc`, the program an
   operator ran on the machine; and `tool.t`, the table it calls T2 from).

     1. a card with no program of its own is seeded from the examples;
     2. a card that already has one is left alone - the operator's program, and
        the edits in it, are not an upgrade's business;
     3. and the demo program itself has to be a program: the panel's own loader,
        block scan and expansion - not a file that merely looks like G-code. It
        carries two numbered G71 ranges, each finished by its own `G70 P Q`, and
        the expansion has to take both.

   The examples folder is read where the checks are run from (the repository
   root), so the file this checks is the file the zip carries. */
static int host_demotest(void)
{
    static const char *const examples = "tools/nc_ui_win/examples";
    static const char *const program = "/D/nc/files/lathe-demo.nc";
    static const char *const tool_file = "/D/nc/files/tool.t";
    static char lines[HOST_EMIT_MAX][NC_MAX_LINE_LEN];
    nc_document_t doc;
    nc_document_t tool_doc;
    nc_tool_t tool;
    g7x_result_t err = G7X_OK;
    size_t emitted = 0u;
    size_t blocks = 0u;
    size_t finishes = 0u;
    size_t i;
    int copied;
    int failures = 0;

    /* 1. a fresh card gets the examples that ship beside the station. */
    copied = host_seed_card(g_files_root, examples);
    printf("demotest: seeded the empty card with %d files\n", copied);
    if (copied != 2) {
        puts("demotest: FAIL the card was not seeded from the examples");
        return 1;
    }

    /* 1b. and the entries themselves are files the operator can read and edit.
       The release ships one per address (`--dump-presets` writes them out of
       the compiled table), and this is seeded into a folder of its own: an
       operator whose card already holds programs is exactly the one whose
       `presets` folder is empty, so it is not gated by the rule above. */
    copied = host_seed_presets(g_files_root, examples);
    printf("demotest: seeded %d preset files\n", copied);
    if (copied < 10) {
        puts("demotest: FAIL the entries were not seeded as files");
        return 1;
    }

    /* 2. the demo is a program, read the way the panel reads it: the loader,
       the block scan (two numbered ranges) and the shared expansion. */
    host_init_core();

    /* The card's own file is what a key writes, not the compiled table: give
       the U INC entry a value of its own and press it. */
    if (!host_fs_write_text("/D/presets/34.txt", "U INC\n U7\n")) {
        puts("demotest: FAIL cannot write the card's own entry");
        return 1;
    }
    {
        nc_document_t entry;

        nc_document_init(&entry);
        (void)nc_insert_line(&entry, 0u, "G1 X30 Z-15");
        entry.cursor_line = 0u;
        if (!nc_insert_preset_id(&entry, 34) || entry.line_count != 1u ||
            strcmp(entry.lines[0].text, "G1 X30 Z-15 U7") != 0) {
            printf("demotest: FAIL U INC wrote \"%s\"\n",
                   entry.line_count ? entry.lines[0].text : "");
            failures++;
        } else {
            puts("demotest: U INC writes the card's own entry file");
        }
    }

    nc_document_init(&doc);
    nc_document_init(&tool_doc);
    {
        nc_result_t loaded = nc_load_file(&doc, program);

        if (loaded != NC_OK || doc.line_count == 0u) {
            printf("demotest: FAIL the demo program does not load (%s, %u lines)\n",
                   nc_result_text(loaded), (unsigned)doc.line_count);
            return 1;
        }
    }
    printf("demotest: the demo is %u lines\n", (unsigned)doc.line_count);

    for (i = 0u; i < doc.line_count; i++) {
        uint32_t p = 0u;
        uint32_t q = 0u;
        float g = 0.0f;

        if (nc_g7x_line_is_header(doc.lines[i].text)) {
            size_t start = 0u;
            size_t end = 0u;

            blocks++;
            if (!nc_g7x_block_containing(&doc, i, &start, &end) || start != i ||
                end <= i) {
                printf("demotest: FAIL the cycle on line %u has no block\n",
                       (unsigned)(i + 1u));
                failures++;
                continue;
            }
            if (!nc_g7x_line_range(doc.lines[i].text, &p, &q)) {
                printf("demotest: FAIL the cycle on line %u names no range\n",
                       (unsigned)(i + 1u));
                failures++;
            }
            i = end;                          /* the block owns its rows */
            continue;
        }
        if (nc_line_word_float(doc.lines[i].text, 'G', &g) && g == 70.0f) {
            finishes++;
        }
    }
    if (blocks != 2u || finishes != 2u) {
        printf("demotest: FAIL the demo has %u cycles and %u finish cuts\n",
               (unsigned)blocks, (unsigned)finishes);
        failures++;
    }
    if (nc_load_file(&tool_doc, tool_file) != NC_OK ||
        !nc_tool_by_number(&tool_doc, 2, &tool) ||
        tool.r != 3.0f || tool.orient != 176) {
        printf("demotest: FAIL the demo's T2 is not the R3 O176 tool "
               "(r=%.2f o=%d)\n", (double)tool.r, tool.orient);
        failures++;
    }

    /* The expansion RUN and the preview share: a row the generator refuses
       fails here, not on the glass. */
    if (!host_emit_lines(&doc, lines, HOST_EMIT_MAX, &emitted, &err)) {
        printf("demotest: FAIL the demo does not expand (g7x error %d)\n",
               (int)err);
        failures++;
    } else {
        printf("demotest: the demo expands to %u moves\n", (unsigned)emitted);
        if (emitted < 40u) {
            puts("demotest: FAIL the expansion is too short to be both cycles");
            failures++;
        }
    }

    /* 3. the same program with Windows line endings - a PC editor, or a git
       checkout on Windows, which is where this very file comes from - has to
       load as the same document. The loader used to carry the CR into the line,
       and the wrap loop could not consume it: it inserted a tab line per pass
       until the document hit its line limit, so a CRLF program could not be
       opened at all. */
    if (!host_crlf_file(program)) {
        puts("demotest: FAIL cannot put the demo back with CRLF endings");
        return 1;
    }
    {
        nc_document_t crlf;
        nc_result_t loaded;

        nc_document_init(&crlf);
        loaded = nc_load_file(&crlf, program);
        if (loaded != NC_OK || crlf.line_count != doc.line_count) {
            printf("demotest: FAIL a CRLF program loads as %u lines (%s), "
                   "not %u\n", (unsigned)crlf.line_count,
                   nc_result_text(loaded), (unsigned)doc.line_count);
            failures++;
        } else {
            printf("demotest: the same demo with CRLF endings loads as %u "
                   "lines\n", (unsigned)crlf.line_count);
        }
    }

    /* 4. a card in use is the operator's: the program already on it is not
       replaced, and an upgrade does not rewrite it. */
    if (!host_fs_write_text(program, "(my program)\nG0 X1\n")) {
        puts("demotest: FAIL cannot write to the seeded card");
        return 1;
    }
    copied = host_seed_card(g_files_root, examples);
    if (copied != 0) {
        printf("demotest: FAIL the station overwrote a card in use (%d files)\n",
               copied);
        failures++;
    } else if (host_seed_presets(g_files_root, examples) != 0) {
        /* The entries follow the same rule the programs do: a folder with an
           entry file of its own - here, the one written above - is the
           operator's, and an upgrade never rewrites it. */
        puts("demotest: FAIL the station overwrote the card's own entries");
        failures++;
    } else {
        fs_file_t *fp = fs_open(program, "r");
        char text[64];
        size_t used = 0u;

        text[0] = '\0';
        if (!fp) {
            puts("demotest: FAIL the operator's program left the card");
            failures++;
        } else {
            while (used + 1u < sizeof(text) && fs_available(fp)) {
                char c;

                if (fs_read(fp, (uint8_t *)&c, 1u) != 1u) {
                    break;
                }
                if (c != '\r') {
                    text[used++] = c;
                }
            }
            fs_close(fp);
            text[used] = '\0';
            if (strncmp(text, "(my program)", 12) != 0) {
                printf("demotest: FAIL the card in use reads \"%s\"\n", text);
                failures++;
            } else {
                puts("demotest: the card in use was left alone");
            }
        }
    }

    if (failures) {
        printf("demotest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("demotest: PASS the demo seeds a fresh card and expands as a program");
    return 0;
}

/* A key that changes what the panel shows has to ask for a repaint - the
   screen's dirty flag is what the module's update hook watches.

   RUN's line keys did not: the branch that moves the cursor of a screen that
   cannot be edited (RUN and the view screens) moved the run line and returned
   without touching the flag, so nothing was drawn until some other key - the
   next footer key - happened to set it. On the machine that reads as "the
   cursor only moves when I press something else". */
static int host_dirtytest(void)
{
    size_t first;
    int failures = 0;

    /* RUN, pointed at the fixture: this is about the repaint a key asks for,
       not about opening a file. */
    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    if (!host_fs_write_text("/D/nc_state.txt",
                            "MODE=RUN\nRUN=/D/nc/files/facing.nc\n")) {
        puts("dirtytest: FAIL cannot point RUN at the fixture");
        return 1;
    }
    host_init_core();
    host_pump_idle(64u);

    nc_visual_draw();
    if (nc_visual_dirty()) {
        puts("dirtytest: FAIL drawing a frame left the screen asking for one");
        failures++;
    }
    if (nc_run_line() != 0u) {
        printf("dirtytest: FAIL RUN opened on line %u, wanted 1\n",
               (unsigned)(nc_run_line() + 1u));
        failures++;
    }

    /* `C` (the keypad's step-down key) is a line move in RUN: the panel has to
       mark itself for repaint or the highlight stays on the old line. */
    first = nc_run_line();
    nc_visual_handle_key(NC_VISUAL_KEY_FIELD_NEXT);
    if (nc_run_line() != first + 1u) {
        printf("dirtytest: FAIL `C` left RUN on line %u, wanted %u\n",
               (unsigned)(nc_run_line() + 1u), (unsigned)(first + 2u));
        failures++;
    }
    if (!nc_visual_dirty()) {
        puts("dirtytest: FAIL the RUN line move did not ask for a repaint");
        failures++;
    }

    nc_visual_draw();
    nc_visual_handle_key(NC_VISUAL_KEY_FIELD_PREV);
    if (nc_run_line() != first) {
        printf("dirtytest: FAIL `B` left RUN on line %u, wanted %u\n",
               (unsigned)(nc_run_line() + 1u), (unsigned)(first + 1u));
        failures++;
    }
    if (!nc_visual_dirty()) {
        puts("dirtytest: FAIL the RUN line move back did not ask for a repaint");
        failures++;
    }

    /* The fixture also carries a G71 block: a line move is a line move, and the
       same key has to ask for the repaint there. */
    nc_visual_draw();
    nc_visual_handle_key(NC_VISUAL_KEY_FIELD_NEXT);
    nc_visual_handle_key(NC_VISUAL_KEY_FIELD_NEXT);
    nc_visual_handle_key(NC_VISUAL_KEY_FIELD_NEXT);
    nc_visual_handle_key(NC_VISUAL_KEY_FIELD_NEXT);
    if (!nc_visual_dirty()) {
        puts("dirtytest: FAIL a run through the block did not ask for a repaint");
        failures++;
    }

    /* The other half of the same contract: while a run is in flight the panel
       has to keep asking to draw by itself. That periodic frame is what carries
       the cursor and the live figures through a stream of emitted lines, where
       no key is pressed at all. */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_2);        /* `2 FROM`: arm */
    nc_visual_draw();
    if (!nc_visual_periodic_needed()) {
        puts("dirtytest: FAIL an armed RUN does not keep asking to draw");
        failures++;
    }

    if (failures) {
        printf("dirtytest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("dirtytest: PASS a key that moves the RUN cursor asks for the repaint");
    return 0;
}

/* Headless check of the editor's new-file field: the keys have to reach the
   name, the name has to reach the card, and the file that is created has to be
   the one the editor then holds.

   This path is why the check exists: the extraction of the editor passed the
   key character to the field handler and the handler kept a local copy of it,
   so every digit was dropped and the file was created with no name at all -
   something no frame dump shows, because the field is only drawn in the file
   list. */
static int host_newfiletest(void)
{
    static const char *const created = "/D/nc/files/12.nc";
    nc_document_t doc;
    int failures = 0;
    nc_result_t r;

    host_init_core();
    nc_visual_select_mode(NC_MODE_PROGRAM);
    host_pump_idle(64u);

    nc_visual_handle_key(NC_VISUAL_KEY_CANCEL);             /* drop the selection:
                                                               `0` opens the list
                                                               only without one */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_0);            /* open the file list */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_5);            /* 5 NEW */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_1);            /* type the name */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_2);
    nc_visual_handle_key(NC_VISUAL_KEY_FINISH);             /* '#' creates it */
    host_pump_idle(64u);

    nc_document_init(&doc);
    r = nc_load_file(&doc, created);
    if (r != NC_OK) {
        printf("newfiletest: FAIL the typed name did not reach the card (%s)\n",
               nc_result_text(r));
        failures++;
    } else {
        printf("newfiletest: created %s with %u lines\n", created,
               (unsigned)doc.line_count);
    }

    /* The next digits must land in the next field: a field that keeps its own
       copy of the key is exactly what this checks against. */
    nc_visual_handle_key(NC_VISUAL_KEY_CANCEL);
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_0);
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_5);
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_3);
    nc_visual_handle_key(NC_VISUAL_KEY_CANCEL);             /* 'A' cancels */
    host_pump_idle(64u);

    {
        nc_document_t other;

        nc_document_init(&other);
        if (nc_load_file(&other, "/D/nc/files/3.nc") == NC_OK) {
            puts("newfiletest: FAIL a cancelled new-file field still created a file");
            failures++;
        }
    }

    if (failures) {
        printf("newfiletest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("newfiletest: PASS the new-file field takes the typed name and creates it");
    return 0;
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
    int id;
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
    for (id = NC_PRESET_ADDR_FIRST; id <= NC_PRESET_ADDR_LAST; id++) {
        FILE *fp;
        size_t len;

        if (!nc_preset_file_for_id(id, text, sizeof(text))) {
            continue;               /* no compiled entry at this address */
        }
        len = strlen(text);
        snprintf(path, sizeof(path), "%s\\%d.txt", dir, id);
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
        if (strcmp(argv[i], "--presettest") == 0)
            return host_presettest();
        if (strcmp(argv[i], "--streamtest") == 0)
            return host_streamtest();
        if (strcmp(argv[i], "--padtest") == 0)
            return host_padtest();
        if (strcmp(argv[i], "--vocabtest") == 0)
            return host_vocabtest();
        if (strcmp(argv[i], "--labeltest") == 0)
            return host_labeltest();
        if (strcmp(argv[i], "--feedtest") == 0)
            return host_feedtest();
        if (strcmp(argv[i], "--spindletest") == 0)
            return host_spindletest();
        if (strcmp(argv[i], "--demotest") == 0)
            return host_demotest();
        if (strcmp(argv[i], "--uwtest") == 0)
            return host_uwtest();
        if (strcmp(argv[i], "--painttest") == 0)
            return host_painttest();
        if (strcmp(argv[i], "--state") == 0)
            return host_state();
        if (strcmp(argv[i], "--version") == 0)
            return host_version();
        if (strcmp(argv[i], "--keytest") == 0)
            return host_keytest();
        if (strcmp(argv[i], "--filetest") == 0)
            return host_filetest();
        if (strcmp(argv[i], "--newfiletest") == 0)
            return host_newfiletest();
        if (strcmp(argv[i], "--editortest") == 0)
            return host_editortest();
        if (strcmp(argv[i], "--contourtest") == 0)
            return host_contourtest();
        if (strcmp(argv[i], "--seedtest") == 0)
            return host_seedtest();
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
        if (strcmp(argv[i], "--dirtytest") == 0)
            return host_dirtytest();
        if (strcmp(argv[i], "--runtest") == 0)
            return host_runtest();
        if (strcmp(argv[i], "--blocktest") == 0)
            return host_blocktest();
        if (strcmp(argv[i], "--pacetest") == 0)
            return host_pacetest();
        if (strcmp(argv[i], "--stoptest") == 0)
            return host_stoptest();
    }
    return -1;
}
