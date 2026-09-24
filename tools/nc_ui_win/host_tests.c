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
#include "nc_presets.h"
#include "nc_run.h"
#include "nc_menu.h"
#include "nc_emit.h"
#include "nc_g7x.h"
#include "nc_layout.h"
#include "nc_manual.h"
#include "nc_palette.h"
#include "nc_path_builder.h"
#include "nc_preview.h"
#include "nc_tools.h"
#include "nc_vocab.h"
#include "nc_visual.h"
#include "g7x.h"
#include "host_fs.h"
#include "host_spindle.h"
#include "lvds_host.h"
#include "modules/cam_keyboard/cam_keyboard.h"
#include "host_shell.h"
#include "host_tests.h"
#include "nc_files.h"

/* The checks are a flat list: each one is defined with the helpers it owns, so
   a helper an earlier check uses is stated here. */
static bool host_fs_write_text(const char *path, const char *text);
static void host_press(char key);
static void host_run_to_end(void);

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
       tmp\nc-ui-show\ROOT-list shows `presets.txt` at /D) and the editor can
       open and save them, but only the program extensions are read as G-code -
       no preview parse, no RUN. */
    if (!nc_path_text("presets.txt") || nc_path_supported("presets.txt") ||
        !nc_path_text("facing.nc") || !nc_path_supported("facing.nc")) {
        puts("filetest: FAIL the text/program split is wrong for .txt or .nc");
        failures++;
    } else {
        puts("filetest: a .txt is text (list, edit, save) and not a program");
    }

    /* And the open itself: the list's OPEN used to refuse a `.txt` outright
       ("unsupported NC file"), which left the preset file listed but not
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

/* Headless check of the NC preset-file contract, driven through the same
   fs_* API the firmware uses:

     1. no /D/presets.txt          -> the compiled presets are written out
     2. an edited file             -> the file wins over the compiled default
     3. an unparsable file         -> compiled default stays in use and the
                                      user's text is left alone to be fixed
*/
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

static int host_presettest(void)
{
    static const char *const default_od = "G71 U0 R0 X0 Z0 F0 P0 Q0";
    static const char *const edited_od = "G71 U2 R1 X10 Z-5 F0.2 P100 Q200";
    static const char *const default_finish = "G70 P0 Q0";
    static const char *const default_face = "G72 W0 R0 X0 Z0 F0 P0 Q0";
    static const char *const edited_file =
        "[41]\nname=OD TEST\nline=G71 U2 R1 X10 Z-5 F0.2 P100 Q200\n";
    /* The file owns the insert text, so a section may carry several lines - the
       header and its contour - and they go in in order. */
    static const char *const multi_file =
        "[41]\nname=OD MULTI\n"
        "line=G71 U1 R0.2 X0.5 Z0.5 F450\n"
        "line=G1 X30 Z0\n"
        "line=G80\n";
    /* A section without a name is not usable: the name is what names the entry,
       even while the helper still shows its own labels. */
    static const char *const unnamed_file =
        "[41]\nline=G71 U9 R9 X9 Z9 F9 P9 Q9\n";
    static const char *const broken_file = "this is not a preset file\n";
    char buf[1024];
    fs_file_info_t info;
    fs_file_t *fp;
    int failures = 0;
    size_t read;

    cnc_init();
    cnc_unit_test_start();
    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    printf("nc_ui: fs root %s\n", g_files_root[0] ? g_files_root : "nc-files");

    /* 1. missing file is materialised from the compiled presets */
    (void)fs_remove("/D/presets.txt");
    (void)nc_presets_init();
    if (!fs_finfo("/D/presets.txt", &info)) {
        puts("presettest: FAIL missing preset file was not created");
        failures++;
    } else if (info.size == 0u) {
        puts("presettest: FAIL created preset file is empty");
        failures++;
    } else if (!host_preset_line_is(41, default_od)) {
        puts("presettest: FAIL compiled OD preset did not insert");
        failures++;
    } else {
        puts("presettest: PASS missing file is written from the compiled presets");
    }

    /* 2. an edited file is what the menu inserts */
    if (!host_fs_write_text("/D/presets.txt", edited_file)) {
        puts("presettest: FAIL could not write the edited preset file");
        failures++;
    } else {
        (void)nc_presets_init();
        if (!host_preset_line_is(41, edited_od)) {
            puts("presettest: FAIL edited preset file was not used");
            failures++;
        } else {
            puts("presettest: PASS edited preset file is used");
        }
    }

    /* 2b. the file defines the entries it mentions; ids it does not mention keep
           their compiled text. A card written before `48 FINISH` existed must
           still offer it, which is the case the bench hit. */
    if (!host_fs_write_text("/D/presets.txt",
                            "[41]\nname=OD TEST\n"
                            "line=G71 U2 R1 X10 Z-5 F0.2 P100 Q200\n")) {
        puts("presettest: FAIL could not write the one-section file");
        failures++;
    } else {
        (void)nc_presets_init();
        if (!host_preset_line_is(41, edited_od)) {
            puts("presettest: FAIL the one-section file lost its edited [41]");
            failures++;
        } else if (!host_preset_line_is(48, default_finish)) {
            puts("presettest: FAIL [48] FINISH is not offered beside an old file");
            failures++;
        } else if (!host_preset_line_is(43, default_face)) {
            puts("presettest: FAIL [43] FACE was lost with an old file");
            failures++;
        } else {
            puts("presettest: PASS ids the file omits keep their compiled text");
        }
    }

    /* 2c. an id the menus used to hold still names its entry. The bench reads
           this file and asked the right question about it - "in presets it is set
           as `[10]` but i need to press 16?" - and the answer for a card written
           before the renumbering is that `[10]` *is* the setup entry, so the
           operator's edited lines are not lost to a menu decision. */
    if (!host_fs_write_text("/D/presets.txt",
                            "[10]\nname=MY STOCK\nline=G970 X-10 U120 Z-150 W30\n"
                            "[80]\nname=MY END\nline=G80\n")) {
        puts("presettest: FAIL could not write the old-id file");
        failures++;
    } else {
        (void)nc_presets_init();
        if (!host_preset_line_is(16, "G970 X-10 U120 Z-150 W30")) {
            puts("presettest: FAIL an edited [10] is not the setup entry");
            failures++;
        } else if (!host_preset_line_is(46, "G80")) {
            puts("presettest: FAIL an edited [80] is not the end entry");
            failures++;
        } else if (!host_preset_line_is(48, default_finish)) {
            puts("presettest: FAIL the old-id file lost [48] FINISH");
            failures++;
        } else {
            puts("presettest: PASS an id the menus used to hold names its entry");
        }
    }

    /* 2d. the new-line entry (`1 OPS` then `1`) is a section too, so what that
           key writes is the card's: a blank line by default, a separator or a
           command the operator keeps needing when the file says so. */
    (void)fs_remove("/D/presets.txt");
    (void)nc_presets_init();
    if (!host_preset_line_is(11, "")) {
        puts("presettest: FAIL the new-line entry is not a blank line by default");
        failures++;
    } else if (!host_fs_write_text("/D/presets.txt",
                                   "[11]\nname=SEP\nline=(---)\n")) {
        puts("presettest: FAIL could not write the redefined new-line entry");
        failures++;
    } else {
        (void)nc_presets_init();
        if (!host_preset_line_is(11, "(---)")) {
            puts("presettest: FAIL the card cannot redefine what `1` writes");
            failures++;
        } else if (!host_fs_write_text("/D/presets.txt",
                                       "[11]\nname=SEP\nline=(---)\n"
                                       "[24]\nname=M3\nline=M3 S2000\n")) {
            puts("presettest: FAIL could not write the redefined spindle word");
            failures++;
        } else {
            /* The TOOL pad's machine words are sections for the same reason: the
               speed in `M3` is the operator's choice, not the panel's. */
            (void)nc_presets_init();
            if (!host_preset_line_is(24, "M3 S2000")) {
                puts("presettest: FAIL the card cannot set the M3 speed");
                failures++;
            } else if (!host_preset_line_is(23, "M6")) {
                puts("presettest: FAIL the tool change is not `M6` by default");
                failures++;
            } else {
                puts("presettest: PASS the panel's text entries are the card's");
            }
        }
    }

    /* 2e. a `line=` that starts with a space continues the line above instead of
           starting one - the only way a value that belongs on that line (a `Q`
           on a header, a `C`/`R` on a contour row) gets in without the
           controller ever seeing a line break. The check reads the inserted
           text: one line, the words separated by their own spaces. */
    if (!host_fs_write_text("/D/presets.txt",
                            "[17]\nname=ROW\nline=G1 X0 Z0\nline= C0\n"
                            "line= R0\n"
                            "[18]\nname=NEXT\nline=G1 X10 Z0\n")) {
        puts("presettest: FAIL could not write the inline-row file");
        failures++;
    } else {
        nc_document_t doc;
        bool ok;

        (void)nc_presets_init();
        nc_document_init(&doc);
        (void)nc_insert_line(&doc, 0, "G71 U1 R1 X0.5 Z0.5 F450");
        ok = nc_insert_preset_id(&doc, 17) &&
             doc.line_count == 2u &&
             strcmp(doc.lines[1].text, "G1 X0 Z0 C0 R0") == 0 &&
             nc_insert_preset_id(&doc, 18) &&
             doc.line_count == 3u &&
             strcmp(doc.lines[2].text, "G1 X10 Z0") == 0;
        if (!ok) {
            printf("presettest: FAIL the inline rows read \"%s\" / \"%s\"\n",
                   doc.line_count > 1u ? doc.lines[1].text : "",
                   doc.line_count > 2u ? doc.lines[2].text : "");
            failures++;
        } else {
            puts("presettest: PASS a row that starts with a space continues the "
                 "line above");
        }
    }

    /* 3. a file with no usable section falls back without touching the text */
    if (!host_fs_write_text("/D/presets.txt", broken_file)) {
        puts("presettest: FAIL could not write the unparsable preset file");
        failures++;
    } else {
        (void)nc_presets_init();
        if (!host_preset_line_is(41, default_od)) {
            puts("presettest: FAIL unparsable preset file did not fall back");
            failures++;
        } else {
            read = 0u;
            buf[0] = '\0';
            fp = fs_open("/D/presets.txt", "r");
            if (fp) {
                read = fs_read(fp, (uint8_t *)buf, sizeof(buf) - 1u);
                fs_close(fp);
            }
            buf[read] = '\0';
            if (strcmp(buf, broken_file) != 0) {
                puts("presettest: FAIL the unparsable file was rewritten");
                failures++;
            } else {
                puts("presettest: PASS unparsable file falls back and is left alone");
            }
        }
    }

    /* 4. a section with several lines inserts all of them, in order, through
          the same call the panel's OD entry makes. */
    if (!host_fs_write_text("/D/presets.txt", multi_file)) {
        puts("presettest: FAIL could not write the multi-line preset file");
        failures++;
    } else {
        nc_document_t doc;
        bool ok;

        (void)nc_presets_init();
        nc_document_init(&doc);
        ok = nc_insert_preset_id(&doc, 41) &&
             doc.line_count == 3u &&
             strcmp(doc.lines[0].text, "G71 U1 R0.2 X0.5 Z0.5 F450") == 0 &&
             strcmp(doc.lines[1].text, "G1 X30 Z0") == 0 &&
             strcmp(doc.lines[2].text, "G80") == 0;
        if (!ok) {
            printf("presettest: FAIL the OD section inserted %u lines\n",
                   (unsigned)doc.line_count);
            failures++;
        } else {
            puts("presettest: PASS every line= of a section is inserted, in order");
        }
    }

    /* 5. a section without `name=` is skipped, so the compiled OD stays. */
    if (!host_fs_write_text("/D/presets.txt", unnamed_file)) {
        puts("presettest: FAIL could not write the nameless preset file");
        failures++;
    } else {
        (void)nc_presets_init();
        if (!host_preset_line_is(41, default_od)) {
            puts("presettest: FAIL a section without a name was used");
            failures++;
        } else {
            puts("presettest: PASS a section without a name is skipped");
        }
    }

    printf("presettest: %s (%d failure%s)\n",
           failures ? "FAILED" : "OK",
           failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
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
            size_t submenu_count = 0u;
            const nc_footer_item_t *g7x_items =
                nc_menu_submenu(NC_FOOTER_ACTION_G7X_MENU, &submenu_count);
            bool draw_label = false;

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
            for (k = 0u; k < submenu_count; k++) {
                if (g7x_items[k].action == NC_FOOTER_ACTION_BUILD &&
                    strcmp(g7x_items[k].label, "DRAW") == 0) {
                    draw_label = true;
                }
            }
            if (!trace_label || !draw_label) {
                puts("padtest: FAIL preview TRACE and G7X DRAW labels are ambiguous");
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

/* The cursor has to sit inside the block the builder continues. To the top of
   the program and down onto the G71's first contour row: the fixture's header
   is line 5, so the block starts at the sixth press. */
static void host_builder_into_block(void)
{
    unsigned i;

    for (i = 0u; i < 12u; i++) {
        nc_visual_handle_key(NC_VISUAL_KEY_PREV);
    }
    for (i = 0u; i < 6u; i++) {
        nc_visual_handle_key(NC_VISUAL_KEY_NEXT);
    }
}

/* `4 G7X`, then `7 DRAW`: the pad opens on the block the cursor is in. */
static bool host_builder_open(void)
{
    host_press('4');
    host_press('7');
    return nc_path_builder_active();
}

/* A profile the pad wrote has to be a cycle, not merely rows that look right:
   `g7x_stream_prepare()` refuses a contour that is not monotonic in X and Z,
   which is what a rapid row in the profile (the `G0` the builder used to write
   on `5`) made of the whole block. The header the builder inserts is the OD
   preset - all zeros - so the fixture's own cycle values are put on it first,
   the way the operator types them. */

#define HOST_EMIT_MAX 400

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

/* A profile the pad wrote is a contour only if the generator takes it. The
   check is the expansion itself - the same `nc_emit_stream_*` RUN and the
   preview share - so a row the builder should not have written (the rapid the
   old `5` put inside the profile) turns the whole block down here exactly as it
   does on the machine, instead of leaving rows that merely look right. */
static bool host_builder_emits_as_cycle(const nc_document_t *doc)
{
    static char lines[HOST_EMIT_MAX][NC_MAX_LINE_LEN];
    g7x_result_t err;
    size_t count = 0u;

    return host_emit_lines(doc, lines, HOST_EMIT_MAX, &count, &err) && count != 0u;
}

/* The top of the program, where no cycle block is: the name row is dropped
   again with the one press that takes the cursor back off it. */
static void host_builder_to_top(void)
{
    unsigned i;

    for (i = 0u; i < 12u; i++) {
        nc_visual_handle_key(NC_VISUAL_KEY_PREV);
    }
    nc_visual_handle_key(NC_VISUAL_KEY_NEXT);
}

/* Leaving the screen is what writes the open document out - EDIT's own
   autosave, the same call a screen change always made - so every check can read
   the program back off the card instead of trusting the panel's memory. */
static bool host_builder_flush(const char *program, nc_document_t *doc)
{
    nc_visual_select_mode(NC_MODE_RUN);
    nc_visual_select_mode(NC_MODE_PROGRAM);
    nc_document_init(doc);
    return nc_load_file(doc, program) == NC_OK;
}

/* Put the fixture back, on the card and on the screen: the screen is taken off
   EDIT before the file is written and brought back after it, so the editor
   loads what is on the card rather than holding the last check's document. */
static bool host_builder_restore(const char *program, nc_document_t *fixture)
{
    nc_visual_select_mode(NC_MODE_RUN);
    if (nc_save_file(fixture, program) != NC_OK) {
        return false;
    }
    nc_visual_select_mode(NC_MODE_PROGRAM);
    return true;
}

/* Headless check of the path builder (docs/nc-path-builder.md). The pad has to
   write the contour a lathe program is made of: the axis it does not move
   copied from the current point, the word it does move marked and typed into,
   the block it belongs to continued rather than restarted - and the result has
   to be a block the shared scan (and so RUN, the emitter and the preview) still
   reads.

     1. opened on the fixture's G71 block, the points land before its G80, the
        copied axis is the block's own last point, the diagonal takes both of
        its words one after the other, and `5` writes nothing: the profile is
        the part's geometry and the block's own `G80` ends it;
     2. `*` drops exactly one point (one point is left behind);
     3. `0` cancels the session: the program on the card is the fixture again;
     4. opened where no block is, it makes one - the OD header and the end mark
        the helper's own entries insert, with the cycle-start `G0` in front of
        the header and the contour rows between the header and the end mark;
     5. and cancel there takes the header and the end mark back with the points.
     6. the operator's own profile (the fixture's five contour rows) is rebuilt
        from the pad: `8`, `4`, `2`, `4`, `2` with the values typed at the
        marked word, then `5`. The profile starts at the stock corner, so the
        first press is the X-one (down to the finished diameter at the face) and
        the X moves are `2`, the key that points down; the Z-only moves are `4`.
        The result is expanded as a cycle, so the profile is not merely rows
        that look right: a rapid row inside it (the old `5`) reverses the
        profile's direction and the generator refuses the whole block.
     7. `*` is the delete: while a word is open it takes that one row back, and
        the completed rows stand; `A` is the screen's mode key, so it leaves the
        builder with the written lines kept instead of throwing the path away.
     8. the cycle-start `G0` before the header is inert for the cycle: the same
        program with and without it expands to the same lines plus that one.

   A wrong copy, a word marked on the wrong axis, an undo that takes the whole
   session or a cancel that leaves a line behind are all invisible to a frame
   dump - which is why every step ends on the card. */
static int host_buildertest(void)
{
    static const char *const program = "/D/nc/files/facing.nc";
    nc_document_t base;
    nc_document_t after;
    nc_preview_info_t preview;
    size_t base_lines;
    size_t first;              /* where the first built line lands: over the old G80 */
    size_t end_line = 0u;
    size_t block_start = 0u;
    size_t block_end = 0u;
    int failures = 0;
    int i;

    host_init_core();
    nc_visual_select_mode(NC_MODE_PROGRAM);
    host_pump_idle(64u);

    nc_document_init(&base);
    if (nc_load_file(&base, program) != NC_OK) {
        printf("buildertest: FAIL cannot load %s\n", program);
        return 1;
    }
    base_lines = base.line_count;
    first = base_lines - 1u;

    /* 1. continue the fixture's block: a Z move with its value typed at the
       marked word, an X move that copies the Z the first point left behind, a
       diagonal whose two words are entered one after the other, then `5`. */
    host_builder_into_block();
    if (!host_builder_open()) {
        puts("buildertest: FAIL the G7X submenu's DRAW did not open the builder");
        return 1;
    }
    host_press('6');                              /* Z+: X copied, Z marked */
    host_press('2');
    nc_visual_handle_key(NC_VISUAL_KEY_MINUS);    /* the prefill is negative */
    host_press('2');                              /* 22 */
    nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);   /* D: the word is taken */
    host_press('8');                              /* X+: Z copied, X marked */
    host_press('5');
    host_press('5');                              /* 55 */
    nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);
    host_press('7');                              /* the diagonal: both marked */
    nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);   /* X kept as prefilled */
    host_press('9');                              /* Z entered after it */
    nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);
    host_press('5');                              /* close the drawing */
    if (nc_path_builder_active()) {
        puts("buildertest: FAIL `5` did not leave the builder");
        failures++;
    }

    if (!host_builder_flush(program, &after)) {
        puts("buildertest: FAIL the built program did not reach the card");
        failures++;
    } else {
        static const char *const wanted[] = {
            "G1 X50 Z22", "G1 X55 Z22", "G1 X45 Z9"
        };

        if (after.line_count != base_lines + 3u) {
            printf("buildertest: FAIL the built program has %u lines, wanted %u\n",
                   (unsigned)after.line_count, (unsigned)(base_lines + 3u));
            failures++;
        }
        for (i = 0; i < 3; i++) {
            const char *got = (first + (size_t)i) < after.line_count
                                  ? after.lines[first + (size_t)i].text
                                  : "";

            if (strcmp(got, wanted[i]) != 0) {
                printf("buildertest: FAIL line %u is \"%s\", wanted \"%s\"\n",
                       (unsigned)(first + (size_t)i + 1u), got, wanted[i]);
                failures++;
            }
        }
        if (first + 3u < after.line_count &&
            strcmp(after.lines[first + 3u].text, "G80") != 0) {
            printf("buildertest: FAIL the end mark is not after the last line: \"%s\"\n",
                   after.lines[first + 3u].text);
            failures++;
        }
        /* The built block is a G71 block like any other: the scan finds it and
           the preview reads its points. */
        if (!nc_g7x_block_containing(&after, first, &block_start, &block_end) ||
            block_start != 4u ||
            block_end != first + 3u) {
            printf("buildertest: FAIL the block is %u..%u, wanted 5..%u\n",
                   (unsigned)(block_start + 1u), (unsigned)(block_end + 1u),
                   (unsigned)(first + 4u));
            failures++;
        }
        if (!nc_g7x_block_end(&after, 4u, &end_line) || end_line != first + 3u) {
            puts("buildertest: FAIL the G80 does not close the built block");
            failures++;
        }
        nc_preview_collect(&after, &preview);
        if (preview.max_x < 55.0f || preview.max_z < 20.0f) {
            printf("buildertest: FAIL the preview reads X%.0f Z%.0f\n",
                   (double)preview.max_x, (double)preview.max_z);
            failures++;
        } else {
            printf("buildertest: three points built, "
                   "preview reads X%.0f Z%.0f\n",
                   (double)preview.max_x, (double)preview.max_z);
        }
    }

    /* 2. `*` drops exactly one point: two points in, one undone, one left. */
    if (!host_builder_restore(program, &base)) {
        puts("buildertest: FAIL cannot put the fixture back");
        failures++;
    }
    host_builder_into_block();
    if (!host_builder_open()) {
        puts("buildertest: FAIL the builder did not open for the undo check");
        failures++;
    } else {
        host_press('2');
        nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);
        host_press('2');
        nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);
        nc_visual_handle_key(NC_VISUAL_KEY_BACKSPACE);   /* `*`: undo */
        host_press('5');
        if (!host_builder_flush(program, &after)) {
            puts("buildertest: FAIL the undone program did not reach the card");
            failures++;
        } else if (after.line_count != base_lines + 1u ||
                   strcmp(after.lines[first].text, "G1 X60 Z-25") != 0) {
            printf("buildertest: FAIL undo left %u lines, first \"%s\"\n",
                   (unsigned)after.line_count,
                   first < after.line_count ? after.lines[first].text : "");
            failures++;
        } else {
            puts("buildertest: undo dropped one point and left the rest");
        }
    }

    /* 3. `0` cancels: every line the session inserted goes with it. */
    if (!host_builder_restore(program, &base)) {
        puts("buildertest: FAIL cannot put the fixture back");
        failures++;
    }
    host_builder_into_block();
    if (!host_builder_open()) {
        puts("buildertest: FAIL the builder did not open for the cancel check");
        failures++;
    } else {
        host_press('2');
        nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);
        host_press('0');                                  /* cancel */
        if (nc_path_builder_active()) {
            puts("buildertest: FAIL `0` did not leave the builder");
            failures++;
        }
        if (!host_builder_flush(program, &after)) {
            puts("buildertest: FAIL the cancelled program did not reach the card");
            failures++;
        } else if (after.line_count != base_lines) {
            printf("buildertest: FAIL cancel left %u lines, the fixture has %u\n",
                   (unsigned)after.line_count, (unsigned)base_lines);
            failures++;
        } else {
            unsigned j;

            for (j = 0u; j < base_lines; j++) {
                if (strcmp(after.lines[j].text, base.lines[j].text) != 0) {
                    printf("buildertest: FAIL cancel changed line %u: \"%s\"\n",
                           j + 1u, after.lines[j].text);
                    failures++;
                    break;
                }
            }
            if (j == base_lines) {
                puts("buildertest: cancel took every line the session inserted");
            }
        }
    }

    /* 4. outside a closed cycle the builder refuses to start and leaves the
       document unchanged; cycle templates belong to the G7X vocabulary. */
    if (!host_builder_restore(program, &base)) {
        puts("buildertest: FAIL cannot put the fixture back");
        failures++;
    }
    host_builder_to_top();
    if (!host_builder_open()) {
        if (!host_builder_flush(program, &after)) {
            puts("buildertest: FAIL the unchanged program did not reach the card");
            failures++;
        } else if (after.line_count != base.line_count) {
            puts("buildertest: FAIL refusal changed the document");
            failures++;
        } else {
            unsigned j;

            for (j = 0u; j < base.line_count; j++) {
                if (strcmp(after.lines[j].text, base.lines[j].text) != 0) {
                    printf("buildertest: FAIL refusal changed line %u\n", j + 1u);
                    failures++;
                    break;
                }
            }
            if (j == base.line_count) {
                puts("buildertest: no cycle block, no builder session");
            }
        }
    } else {
        puts("buildertest: FAIL PATH opened outside a cycle block");
        failures++;
    }

    /* The keypad's configured step is metric. Refuse an inch-mode block instead
       of writing a 10-inch prefill while telling the operator it is 10 mm. */
    {
        nc_document_t inch;

        memcpy(&inch, &base, sizeof(inch));
        (void)nc_set_line(&inch, 0u, "N10 G20");
        if (!host_builder_restore(program, &inch)) {
            puts("buildertest: FAIL cannot load the inch-mode fixture");
            failures++;
        }
        host_builder_into_block();
        if (host_builder_open()) {
            puts("buildertest: FAIL PATH accepted an inch-mode block");
            failures++;
        }
        if (!host_builder_restore(program, &base)) {
            puts("buildertest: FAIL cannot restore the metric fixture");
            failures++;
        }
    }

    /* 5. rebuild the operator's profile in a vocabulary-created empty block.

       The fixture's contour is (30,0) (30,-15) (35,-15) (35,-25) (50,-25); the
       builder starts at the stock face (50,0), and one press moves one axis by
       one step (10 mm by default) and copies the other from the current point.
       So the presses are the *directions* and the numbers
       are typed at the marked word:

         8  X-          (50,0) -> type X30
         4  Z-          type 15   -> (30,-15)
         2  X+          type 35   -> (35,-15)
         4  Z-          the prefill is already 25 -> (35,-25)
         2  X+          type 50   -> (50,-25)
         5  close       writes no extra row

       The comparison is on the points: the fixture's corner words (`C0 R0`,
       `C0 R2`, `C0 R5`) are not written by the pad yet, so they are not in the
       built rows. */
    {
        nc_document_t empty;
        size_t i;
        size_t empty_first;

        nc_document_init(&empty);
        for (i = 0u; i < 5u; i++) {
            if (nc_insert_line(&empty, i, base.lines[i].text) != NC_OK) {
                failures++;
            }
        }
        (void)nc_insert_line(&empty, 5u, "G80");
        empty_first = empty.line_count - 1u;
        strncpy(empty.path, program, sizeof(empty.path) - 1u);
        empty.path[sizeof(empty.path) - 1u] = '\0';
        if (!host_builder_restore(program, &empty)) {
            puts("buildertest: FAIL cannot load the empty cycle block");
            failures++;
        }
        host_builder_into_block();
        if (!host_builder_open()) {
            puts("buildertest: FAIL the builder did not open for the profile check");
            failures++;
        } else {
            unsigned row;
            int bad = 0;

            host_press('8');
            host_press('3');
            host_press('0');
            nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);
            host_press('4');
            host_press('1');
            host_press('5');
            nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);
            host_press('2');
            host_press('3');
            host_press('5');
            nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);
            host_press('4');
            nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);
            host_press('2');
            host_press('5');
            host_press('0');
            nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);
            host_press('5');

            if (!host_builder_flush(program, &after)) {
                puts("buildertest: FAIL the rebuilt profile did not reach the card");
                failures++;
            } else if (after.line_count != empty.line_count + 5u) {
                printf("buildertest: FAIL the rebuilt profile has %u lines\n",
                       (unsigned)after.line_count);
                failures++;
            } else {
                for (row = 0u; row < 5u; row++) {
                    float bx = 0.0f;
                    float bz = 0.0f;
                    float gx = 0.0f;
                    float gz = 0.0f;
                    const char *built = after.lines[empty_first + row].text;
                    const char *wanted_row = base.lines[5u + row].text;

                    (void)nc_line_word_float(built, 'X', &bx);
                    (void)nc_line_word_float(built, 'Z', &bz);
                    (void)nc_line_word_float(wanted_row, 'X', &gx);
                    (void)nc_line_word_float(wanted_row, 'Z', &gz);
                    if (fabs((double)bx - (double)gx) > 0.0005 ||
                        fabs((double)bz - (double)gz) > 0.0005) {
                        printf("buildertest: FAIL profile row %u is \"%s\", "
                               "the fixture has \"%s\"\n",
                               row + 1u, built, wanted_row);
                        bad = 1;
                    }
                }
                if (bad) {
                    failures++;
                } else if (strcmp(after.lines[empty_first].text, "G1 X30 Z0") != 0) {
                    printf("buildertest: FAIL the first row is \"%s\", "
                           "wanted \"G1 X30 Z0\"\n", after.lines[empty_first].text);
                    failures++;
                } else if (!host_builder_emits_as_cycle(&after)) {
                    puts("buildertest: FAIL the rebuilt profile does not expand as a cycle");
                    failures++;
                } else {
                    printf("buildertest: the fixture's profile rebuilt from "
                           "\"%s\" to \"%s\"\n",
                           after.lines[empty_first].text,
                           after.lines[empty_first + 4u].text);
                }
            }
        }
    }

    if (!host_builder_restore(program, &base)) {
        puts("buildertest: FAIL cannot restore the fixture");
        failures++;
    }
    /* 6a. `*` while a word is open takes just that row back. */
    host_builder_into_block();
    if (!host_builder_open()) {
        puts("buildertest: FAIL the builder did not open for the delete check");
        failures++;
    } else {
        host_press('2');                                  /* row + X marked */
        host_press('7');                                  /* a value typed */
        nc_visual_handle_key(NC_VISUAL_KEY_BACKSPACE);    /* `*`: delete it */
        if (!nc_path_builder_active()) {
            puts("buildertest: FAIL `*` closed the builder instead of deleting the row");
            failures++;
        }
        host_press('5');
        if (!host_builder_flush(program, &after)) {
            puts("buildertest: FAIL the deleted row did not reach the card");
            failures++;
        } else if (after.line_count != base_lines ||
                   strcmp(after.lines[base_lines - 1u].text, "G80") != 0) {
            puts("buildertest: FAIL deleting the pending point changed the block");
            failures++;
        } else {
            puts("buildertest: `*` took the open row back and left the rest");
        }
    }

    /* 7b. `A` is not the builder's key: it changes screen and the path stays. */
    if (!host_builder_restore(program, &base)) {
        puts("buildertest: FAIL cannot put the fixture back");
        failures++;
    }
    host_builder_into_block();
    if (!host_builder_open()) {
        puts("buildertest: FAIL the builder did not open for the mode-key check");
        failures++;
    } else {
        host_press('2');
        host_press('4');
        host_press('0');                                  /* X40 */
        nc_visual_handle_key(NC_VISUAL_KEY_ACCEPT);
        nc_visual_handle_key(NC_VISUAL_KEY_MODE);         /* `A`: leave */
        if (nc_path_builder_active()) {
            puts("buildertest: FAIL `A` left the builder up");
            failures++;
        }
        if (!host_builder_flush(program, &after)) {
            puts("buildertest: FAIL the kept path did not reach the card");
            failures++;
        } else if (after.line_count != base_lines + 1u ||
                   strcmp(after.lines[base_lines - 1u].text, "G1 X40 Z-25") != 0 ||
                   strcmp(after.lines[base_lines].text, "G80") != 0) {
            puts("buildertest: FAIL `A` did not keep the contour row");
            failures++;
        } else {
            puts("buildertest: `A` changed screen and kept the path");
        }
    }

    if (failures) {
        printf("buildertest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("buildertest: PASS the pad walks the contour, and cancel takes it back");
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

    /* 2. the demo is a program, read the way the panel reads it: the loader,
       the block scan (two numbered ranges) and the shared expansion. */
    host_init_core();
    nc_document_init(&doc);
    nc_document_init(&tool_doc);
    if (nc_load_file(&doc, program) != NC_OK || doc.line_count == 0u) {
        puts("demotest: FAIL the demo program does not load");
        return 1;
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

    /* 3. a card in use is the operator's: the program already on it is not
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

/* The flags this file answers, in the order they are tried: the two dumps
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
        if (strcmp(argv[i], "--buildertest") == 0)
            return host_buildertest();
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
