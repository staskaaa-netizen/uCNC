/* Real parser/planner integration; spindle execution is intercepted below.
   This verifies generated G33 targets, not physical spindle synchronization. */
#include "src/cnc.h"
#include "../g7x.h"
#include "../g7x_source.h"
#ifndef G7X_STANDALONE_TEST
#include "../../nc/nc_run.h"
#include "../../nc/nc_feedback.h"
#endif
#include <string.h>
#include <stdio.h>
#include <math.h>

static unsigned threads;
static float thread_x, thread_z, thread_pitch;
static bool fail_thread;
static unsigned motion_count;
static unsigned fail_motion_at;
static unsigned stop_motion_at;

static bool observe_motion(void *args)
{
    gcode_exec_args_t *p = args;
    if (p->cmd->words & GCODE_ALL_AXIS) {
        motion_count++;
#ifndef G7X_STANDALONE_TEST
        if (stop_motion_at && motion_count == stop_motion_at)
            nc_run_stop();
#endif
        if (fail_motion_at && motion_count == fail_motion_at) {
            *p->error = STATUS_SOFT_LIMIT_ERROR;
            return EVENT_HANDLED;
        }
    }
    return EVENT_CONTINUE;
}
CREATE_EVENT_LISTENER(gcode_exec_modifier, observe_motion);

/* Explicit Cartesian identity avoids MinGW's weak-symbol linkage ambiguity. */
void kinematics_apply_transform(float *axis) { (void)axis; }
void kinematics_apply_reverse_transform(float *axis) { (void)axis; }

static bool record_thread(void *args)
{
    gcode_exec_args_t *p = args;
    if (p->cmd->group_extended != EXTENDED_MOTION_GCODE(33))
        return EVENT_CONTINUE;
    threads++;
    thread_x = p->target[AXIS_X];
    thread_z = p->target[AXIS_Z];
    thread_pitch = p->words->ijk[2];
    *p->error = fail_thread ? STATUS_SPINDLE_RPM_ERROR : STATUS_OK;
    return EVENT_HANDLED;
}
CREATE_EVENT_LISTENER(gcode_exec, record_thread);

/* The last F word a generated feed block carried, so a test can see that a
   profile row's feed reached the planner where the program wrote it. */
static float generated_feed;
static unsigned generated_feed_blocks;

static bool record_generated_feed(void *args)
{
    gcode_exec_args_t *p = args;

    if ((p->cmd->words & GCODE_ALL_AXIS) && (p->cmd->words & GCODE_WORD_F)) {
        generated_feed = p->words->f;
        generated_feed_blocks++;
    }
    return EVENT_CONTINUE;
}
CREATE_EVENT_LISTENER(gcode_exec_modifier, record_generated_feed);

static int command(const char *line, uint8_t want)
{
    char input[160];
    snprintf(input, sizeof(input), "%s\n", line);
    if (!mcu_unit_test_inject(input)) return 1;
    uint8_t got = cnc_parse_cmd();
    if (got != want) {
        printf("FAIL `%s`: status %u, expected %u\n", line, got, want);
        return 1;
    }
    return 0;
}

static void ignore_history_block(void *user, uint32_t number, const char *text)
{
    (void)user;
    (void)number;
    (void)text;
}

#ifndef G7X_STANDALONE_TEST
/* The panel's RUN sender is paced, and the pacer lives in the main loop:
   `cnc_dotasks()` is where it hands the machine one block, waits for the machine
   to run it, and only then hands over the next unit. The old reader gave the
   whole program at once, so a test could take a whole program from one
   `grbl_stream_available()`. Now a pass has to be given first, and a program
   only ends when the pacer has run out of lines.

   `nc_parse_block()` takes the block the pacer offered, if any.
   `nc_run_to_end()` drives a program to its end and reports the first parse
   answer that was not OK. */
static bool nc_parse_block(uint8_t *status)
{
    cnc_dotasks();
    if (!grbl_stream_available()) {
        return false;                       /* the pacer is waiting for the
                                               machine, not for the parser */
    }
    if (status) {
        *status = cnc_parse_cmd();
    }
    return true;
}

static uint8_t nc_run_to_end(void)
{
    uint8_t first_error = STATUS_OK;

    for (int i = 0; i < 20000; i++) {
        uint8_t status = STATUS_OK;

        if (nc_parse_block(&status) && status != STATUS_OK && first_error == STATUS_OK) {
            first_error = status;
        }
        mcu_unit_test_advance_time(1000);
        if (!nc_run_streaming() && !grbl_stream_available() &&
            !g7x_parser_busy() && planner_buffer_is_empty() && itp_is_empty()) {
            break;
        }
    }
    return first_error;
}

/* Let the machine finish what it already has. The sections above drive the
   parser directly and leave motion queued without running it; a paced run waits
   for the machine before it hands over its first block, so it has to start from
   an idle machine. */
static void nc_drain_machine(void)
{
    for (int i = 0; i < 20000; i++) {
        if (!g7x_parser_busy() && planner_buffer_is_empty() && itp_is_empty()) {
            break;
        }
        cnc_dotasks();
        mcu_unit_test_advance_time(1000);
    }
}

/* Start a paced run from a machine that has finished everything it was given:
   the pacer waits for the machine before its first block, and the sections above
   leave motion queued without running it. */
static bool nc_start_run(nc_document_t *doc, size_t line)
{
    nc_drain_machine();
    return nc_run_start_stream(doc, line);
}
#endif

int main(void)
{
    int fails = 0;
#ifndef G7X_STANDALONE_TEST
    if (!strstr(nc_feedback_lock(SETTINGS_READ_ERROR, EXEC_POSITION_MAYBE_LOST, false), "$RST=*")) fails++;
    if (!strstr(nc_feedback_lock(SETTINGS_WRITE_ERROR, 0, false), "save failed")) fails++;
    if (!strstr(nc_feedback_lock(0, EXEC_KILL | EXEC_HOLD, false), "alarm")) fails++;
    if (!strstr(nc_feedback_lock(0, EXEC_POSITION_MAYBE_LOST, false), "$H")) fails++;
    if (nc_feedback_lock(0, 0, false)[0]) fails++;
    if (!strstr(nc_feedback_error(STATUS_BAD_NUMBER_FORMAT), "number")) fails++;
#endif
    cnc_init();
    cnc_unit_test_start();
    ADD_EVENT_LISTENER(gcode_exec, record_thread);
    ADD_EVENT_LISTENER(gcode_exec_modifier, observe_motion);
    ADD_EVENT_LISTENER(gcode_exec_modifier, record_generated_feed);
#ifndef G7X_STANDALONE_TEST
    nc_run_init();
#endif
    fails += command("$X", STATUS_OK);
    fails += command("G18 G90 G21", STATUS_OK);
    fails += command("G7", STATUS_OK);
    fails += command("G0 X40 Z0", STATUS_OK);
    fails += command("G76 X36 Z-20 P2 Q1 F1.5 L-1", STATUS_INVALID_STATEMENT);
    fails += command("G76 X36 Z-20 P2 Q1 F1.5 L256", STATUS_INVALID_STATEMENT);
    fails += command("G76 X36 Z-20 P2 Q1 F1.5 L1", STATUS_OK);
    if (threads != 4 || fabsf(thread_x - 18.0f) > .001f ||
        fabsf(thread_z + 20.0f) > .001f || fabsf(thread_pitch - 1.5f) > .001f ||
        g7x_parser_busy()) {
        printf("FAIL G76 completed: threads=%u X=%f Z=%f pitch=%f busy=%d\n",
               threads, thread_x, thread_z, thread_pitch, g7x_parser_busy());
        fails++;
    }
    /* Normal parser words may precede G76, with no spaces between tokens. */
    fails += command("G0 X40 Z0", STATUS_OK);
    threads = 0;
    fails += command("X36Z-20P2Q1F1.5G76", STATUS_OK);
    if (threads != 3 || g7x_parser_busy()) { puts("FAIL word ordering"); fails++; }

    /* Failure propagates on the source command, and no later pass runs. */
    fails += command("G0 X40 Z0", STATUS_OK);
    fail_thread = true;
    threads = 0;
    fails += command("G76 X36 Z-20 P2 Q1 F1.5", STATUS_SPINDLE_RPM_ERROR);
    fail_thread = false;
    if (threads != 1 || g7x_parser_busy()) { puts("FAIL failure cleanup"); fails++; }

    /* G8 radius input and nonzero work offsets must reach the same geometry. */
    fails += command("G8", STATUS_OK);
    fails += command("G10 L2 P1 X10 Z20", STATUS_OK);
    fails += command("G0 X20 Z0", STATUS_OK);
    fails += command("G76 X18 Z-20 P2 Q1 F1.5", STATUS_OK);
    if (fabsf(thread_x - 28) > .001f || fabsf(thread_z) > .001f) {
        puts("FAIL G8/work offsets"); fails++;
    }
    fails += command("G20", STATUS_OK);
    fails += command("G0 X1 Z0", STATUS_OK);
    fails += command("G76 X0.9 Z-1 P0.1 Q0.05 F0.05", STATUS_OK);
    if (fabsf(thread_x - 32.86f) > .002f || fabsf(thread_z + 5.4f) > .002f ||
        fabsf(thread_pitch - 1.27f) > .002f) { puts("FAIL inch scaling"); fails++; }
    fails += command("G21 G90", STATUS_OK);
    fails += command("G0 X20 Z0", STATUS_OK);
    fails += command("G10 L2 P1 X0 Z0", STATUS_OK);
    fails += command("G7", STATUS_OK);
    fails += command("G91", STATUS_OK);
    fails += command("G76 X36 Z-20 P2 Q1 F1.5", STATUS_INVALID_STATEMENT);
    fails += command("G90", STATUS_OK);

    /* Contour rows collect without moving, including modal/one-axis rows. */
    fails += command("G0 X40 Z0", STATUS_OK);
    unsigned before = motion_count;
    fails += command("G71 U1 R1 X0.1 Z0.1 F300", STATUS_OK);
    fails += command("G1 X40 Z0", STATUS_OK);
    fails += command("Z-5", STATUS_OK);
    fails += command("X36", STATUS_OK);
    if (motion_count != before) { puts("FAIL contour executed directly"); fails++; }
    /* One source call consumes the entire cycle; the next line is still pending. */
    mcu_unit_test_inject("G80\nG0 X80 Z0\n");
    if (cnc_parse_cmd() != STATUS_OK || g7x_parser_busy() || motion_count <= before) {
        puts("FAIL synchronous G80"); fails++;
    }
    float pos[AXIS_COUNT];
    mc_get_position(pos);
    if (pos[AXIS_X] >= 39.0f) { puts("FAIL following command overtook cycle"); fails++; }
    if (cnc_parse_cmd() != STATUS_OK) fails++;
    mc_get_position(pos);
    if (fabsf(pos[AXIS_X] - 40) > .001f) { puts("FAIL subsequent command"); fails++; }

    fails += command("G71 U1 F300", STATUS_OK);
    fails += command("G1 X40 Z0", STATUS_OK);
    fails += command("X30 Z-10", STATUS_OK);
    fails += command("X35 Z-5", STATUS_OK);
    fails += command("G80", STATUS_INVALID_STATEMENT);
    if (g7x_parser_busy()) { puts("FAIL invalid contour cleanup"); fails++; }

    fails += command("G71 U1 F300", STATUS_OK);
    fails += command("G1 X40 Z0", STATUS_OK);
    fails += command("X36 Z-5", STATUS_OK);
    fail_motion_at = motion_count + 2;
    fails += command("G80", STATUS_SOFT_LIMIT_ERROR);
    fail_motion_at = 0;
    if (g7x_parser_busy()) { puts("FAIL generated failure cleanup"); fails++; }

    fails += command("G20", STATUS_OK);
    fails += command("G71 U0.05 R0.05 F10", STATUS_OK);
    fails += command("G1 X1 Z0", STATUS_OK);
    fails += command("X0.8 Z-0.1", STATUS_OK);
    fails += command("G80", STATUS_OK);
    uint8_t modes[16]; uint16_t feed, spindle;
    parser_get_modes(modes, &feed, &spindle);
    if (feed != 254) { printf("FAIL inch modal feed: %u\n", feed); fails++; }
    fails += command("G21", STATUS_OK);

    /* Test the actual NC source stream and its new error listener. */
    /* Facing also executes through the real parser with no NC dependency. */
    fails += command("G0 X54 Z2", STATUS_OK);
    fails += command("G72 W2 R1 X0.5 Z0.5 F120", STATUS_OK);
    fails += command("G1 X50 Z0", STATUS_OK);
    fails += command("G1 X30 Z-10", STATUS_OK);
    fails += command("G80", STATUS_OK);
    if (g7x_parser_busy()) { puts("FAIL G72 parser cleanup"); fails++; }

    /* Fanuc/Haas numbered range: N(P)..N(Q) replaces the G80 terminator. */
    fails += command("G0 X54 Z2", STATUS_OK);
    before = motion_count;
    fails += command("G71 U1 R1 P100 Q200 X0.1 Z0.1 F300", STATUS_OK);
    if (!g7x_parser_busy()) { puts("FAIL P/Q range not armed"); fails++; }
    /* Blocks between the header and N(P) are ordinary program text. */
    fails += command("G0 X60 Z2", STATUS_OK);
    if (motion_count != before + 1) { puts("FAIL pre-P block suppressed"); fails++; }
    fails += command("N100 G1 X50 Z0", STATUS_OK);
    fails += command("N150 Z-5", STATUS_OK);
    fails += command("N200 X40", STATUS_OK);
    if (g7x_parser_busy()) { puts("FAIL P/Q range cleanup"); fails++; }
    if (motion_count <= before + 1) { puts("FAIL P/Q cycle did not run"); fails++; }
    const g7x_history_t *history = g7x_parser_numbered_history();
    unsigned visited = 0;
    if (!history || !g7x_history_find(history, 150) ||
        g7x_history_visit_range(history, 100, 200, ignore_history_block, NULL,
                                &visited) != G7X_OK || visited != 3) {
        printf("FAIL P/Q retention: visited=%u\n", visited);
        fails++;
    }
    if (g7x_history_visit_range(history, 100, 300, ignore_history_block, NULL,
                                &visited) != G7X_RANGE_MISSING) {
        puts("FAIL P/Q missing range not reported"); fails++;
    }

    /* P/Q error paths: incomplete header, reversed range, G80 terminator and a
       profile block without a number must all fail and clear collection. */
    fails += command("G71 U1 R1 P100 X0.1 F300", STATUS_INVALID_STATEMENT);
    fails += command("G71 U1 R1 P200 Q100 X0.1 F300", STATUS_INVALID_STATEMENT);
    fails += command("G71 U1 R1 P100 Q200 X0.1 Z0.1 F300", STATUS_OK);
    fails += command("N100 G1 X50 Z0", STATUS_OK);
    fails += command("G80", STATUS_INVALID_STATEMENT);
    if (g7x_parser_busy()) { puts("FAIL P/Q G80 cleanup"); fails++; }
    /* Unnumbered rows inside the range are ordinary contour rows. */
    fails += command("G71 U1 R1 P100 Q200 X0.1 Z0.1 F300", STATUS_OK);
    fails += command("N100 G1 X50 Z0", STATUS_OK);
    fails += command("G1 X45 Z-2", STATUS_OK);
    fails += command("N200 G1 X40 Z-10", STATUS_OK);
    if (g7x_parser_busy()) { puts("FAIL P/Q unnumbered range cleanup"); fails++; }
    /* A number beyond Q cannot belong to the range. */
    fails += command("G71 U1 R1 P100 Q200 X0.1 Z0.1 F300", STATUS_OK);
    fails += command("N100 G1 X50 Z0", STATUS_OK);
    fails += command("N300 G1 X40 Z-10", STATUS_INVALID_STATEMENT);
    if (g7x_parser_busy()) { puts("FAIL P/Q out-of-range cleanup"); fails++; }

    /* Fanuc two-line header completed by a second G71 block. Its U/W finish
       allowances must land exactly where the one-line spelling lands. */
    fails += command("G0 X54 Z2", STATUS_OK);
    fails += command("G71 U1 R1", STATUS_OK);
    fails += command("G71 P100 Q200 U0.5 W0.25 F300", STATUS_OK);
    if (!g7x_parser_busy()) { puts("FAIL two-line header not armed"); fails++; }
    fails += command("N100 G1 X50 Z0", STATUS_OK);
    fails += command("G1 X50 Z-10", STATUS_OK);
    fails += command("N200 G1 X40 Z-10", STATUS_OK);
    if (g7x_parser_busy()) { puts("FAIL two-line range cleanup"); fails++; }
    mc_get_position(pos);
    float two_line_x = pos[AXIS_X];
    float two_line_z = pos[AXIS_Z];
    fails += command("G0 X54 Z2", STATUS_OK);
    fails += command("G71 U1 R1 X0.5 Z0.25 F300", STATUS_OK);
    fails += command("G1 X50 Z0", STATUS_OK);
    fails += command("G1 X50 Z-10", STATUS_OK);
    fails += command("G1 X40 Z-10", STATUS_OK);
    fails += command("G80", STATUS_OK);
    mc_get_position(pos);
    if (fabsf(pos[AXIS_X] - two_line_x) > 0.001f ||
        fabsf(pos[AXIS_Z] - two_line_z) > 0.001f) {
        printf("FAIL two-line allowances X%.3f/%.3f Z%.3f/%.3f\n",
               two_line_x, pos[AXIS_X], two_line_z, pos[AXIS_Z]);
        fails++;
    }

    /* The same two-line form for G72: the facing cycle must take its P/Q range
       and its U/W allowances from the second block exactly as G71 does. */
    fails += command("G0 X54 Z2", STATUS_OK);
    fails += command("G72 W2 R1", STATUS_OK);
    fails += command("G72 P100 Q200 U0.5 W0.25 F300", STATUS_OK);
    fails += command("N100 G1 X10 Z-25", STATUS_OK);
    fails += command("G1 X40 Z-25", STATUS_OK);
    fails += command("N200 G1 X40 Z0", STATUS_OK);
    if (g7x_parser_busy()) { puts("FAIL two-line G72 cleanup"); fails++; }
    mc_get_position(pos);
    float g72_two_line_x = pos[AXIS_X];
    float g72_two_line_z = pos[AXIS_Z];
    fails += command("G0 X54 Z2", STATUS_OK);
    fails += command("G72 W2 R1 X0.5 Z0.25 F300", STATUS_OK);
    fails += command("G1 X10 Z-25", STATUS_OK);
    fails += command("G1 X40 Z-25", STATUS_OK);
    fails += command("G1 X40 Z0", STATUS_OK);
    fails += command("G80", STATUS_OK);
    mc_get_position(pos);
    if (fabsf(pos[AXIS_X] - g72_two_line_x) > 0.001f ||
        fabsf(pos[AXIS_Z] - g72_two_line_z) > 0.001f) {
        printf("FAIL two-line G72 X%.3f/%.3f Z%.3f/%.3f\n",
               g72_two_line_x, pos[AXIS_X], g72_two_line_z, pos[AXIS_Z]);
        fails++;
    }

    /* A profile row may carry the words the finish is to run with: Fanuc reads
       F/S/T from the profile, so the row is taken instead of refused. The
       profile's F must reach the planner at the row that wrote it, and the
       range's `P` block may be the approach - a `G0` that positions the tool.
       Fanuc's own programs are written both ways. */
    fails += command("G0 X54 Z2", STATUS_OK);
    generated_feed = 0.0f;
    generated_feed_blocks = 0u;
    fails += command("G71 U1 R1 P100 Q200 X0.1 Z0.1 F300", STATUS_OK);
    fails += command("N100 G0 X52 Z4", STATUS_OK);        /* the P block */
    fails += command("G1 X50 Z0", STATUS_OK);
    fails += command("N200 G1 X50 Z-6 F200 S800 T2", STATUS_OK);
    if (g7x_parser_busy()) { puts("FAIL profile words cleanup"); fails++; }
    if (generated_feed_blocks == 0u || fabsf(generated_feed - 200.0f) > 0.5f) {
        printf("FAIL profile feed did not reach the planner (last F %.3f)\n",
               (double)generated_feed);
        fails++;
    }

    /* G70 P Q: the finish cut of the range this run collected. It is the
       profile again, nothing offset and no roughing - and it takes the row's
       own feed. A range the run never collected is refused, not guessed. */
    fails += command("G0 X54 Z2", STATUS_OK);
    fails += command("G71 U1 R1 P100 Q200 X0.1 Z0.1 F300", STATUS_OK);
    fails += command("N100 G0 X52 Z4", STATUS_OK);
    fails += command("N110 G1 X50 Z0", STATUS_OK);
    fails += command("N200 G1 X50 Z-6 F200", STATUS_OK);
    if (g7x_parser_busy()) { puts("FAIL G71 before G70 cleanup"); fails++; }
    generated_feed = 0.0f;
    before = motion_count;
    fails += command("G70 P100 Q200", STATUS_OK);
    if (g7x_parser_busy()) { puts("FAIL G70 cleanup"); fails++; }
    if (motion_count <= before) { puts("FAIL G70 did not move"); fails++; }
    if (motion_count - before > 12u) {
        printf("FAIL G70 emitted %u blocks: that is a roughing cycle\n",
               motion_count - before);
        fails++;
    }
    if (fabsf(generated_feed - 200.0f) > 0.5f) {
        printf("FAIL G70 last feed %.3f, expected the profile's 200\n",
               (double)generated_feed);
        fails++;
    }
    fails += command("G70 P300 Q400", STATUS_INVALID_STATEMENT);

    /* A cycle must not run with cutter compensation active: the core takes
       G41/G42 for motion and does nothing with them, so the cycle would cut the
       uncompensated path. Refused, not approximated. */
    fails += command("G41", STATUS_OK);
    fails += command("G71 U1 R1 P100 Q200 X0.1 Z0.1 F300", STATUS_GCODE_UNSUPPORTED_COMMAND);
    fails += command("G72 W1 R1 X0.1 Z0.1 F300", STATUS_GCODE_UNSUPPORTED_COMMAND);
    fails += command("G70 P100 Q200", STATUS_GCODE_UNSUPPORTED_COMMAND);
    fails += command("G40", STATUS_OK);
    if (g7x_parser_busy()) { puts("FAIL compensation refusal left a cycle open"); fails++; }

    /* G73 is a different roughing model and is not implemented: refused by
       name (the console says which cycle), not left to the generic answer. */
    fails += command("G73 U1 W1 R2 P100 Q200", STATUS_GCODE_UNSUPPORTED_COMMAND);

    /* The refusal text is the module's, and a caller *takes* it: a panel with no
       terminal shows it on the glass, and taking it means it can never explain
       a later line's error. */
    if (strstr(g7x_take_refusal_text(), "G73") == NULL) {
        puts("FAIL the G73 refusal is not named");
        fails++;
    }
    if (g7x_take_refusal_text()[0] != '\0') {
        puts("FAIL the refusal text was not taken once");
        fails++;
    }
    /* A cycle that runs leaves nothing behind for a later error to borrow. */
    fails += command("G0 X54 Z2", STATUS_OK);
    fails += command("G71 U1 R1 X0.1 Z0.1 F300", STATUS_OK);
    fails += command("G1 X50 Z0", STATUS_OK);
    fails += command("G1 X50 Z-10", STATUS_OK);
    fails += command("G80", STATUS_OK);
    if (g7x_take_refusal_text()[0] != '\0') {
        puts("FAIL a good cycle left a refusal text behind");
        fails++;
    }

    /* A corner the moves beside it cannot hold is refused, not fitted: the
       operator wrote a number and the machine must cut that number or say so
       (bench: a programmed R35 on a 20 mm move with a 15 mm step came out as
       R3.375 with nothing to explain it - "it should reject it loudly"). R35
       needs 35 mm of room on a 7.5 mm step, so this is the bench's own case. */
    fails += command("G0 X54 Z2", STATUS_OK);
    fails += command("G71 U1 R1 X0.1 Z0.1 F300", STATUS_OK);
    fails += command("G1 X35 Z0", STATUS_OK);
    fails += command("G1 X35 Z-20 R35", STATUS_OK);
    fails += command("G1 X50 Z-20", STATUS_OK);
    fails += command("G80", STATUS_INVALID_STATEMENT);
    if (strstr(g7x_take_refusal_text(), "corner") == NULL) {
        puts("FAIL a corner that cannot fit is not refused by name");
        fails++;
    }
    if (g7x_parser_busy()) {
        puts("FAIL the refused corner left the collector armed");
        fails++;
    }
    /* And one that fits is cut as written. */
    fails += command("G0 X54 Z2", STATUS_OK);
    fails += command("G71 U1 R1 X0.1 Z0.1 F300", STATUS_OK);
    fails += command("G1 X35 Z0", STATUS_OK);
    fails += command("G1 X35 Z-20 R3", STATUS_OK);
    fails += command("G1 X50 Z-20", STATUS_OK);
    fails += command("G80", STATUS_OK);
    if (g7x_take_refusal_text()[0] != '\0') {
        puts("FAIL a corner that fits left a refusal behind");
        fails++;
    }

    /* The bench's Fanuc pair with the range on the **first** line: the first
       pair opened a numbered range and the second carried only the cycle's
       values. The continuation used to override what the first line decided,
       leaving the range armed *and* the region active - the contour then ended
       at the N(Q) row and the G80 was refused, which the bench saw as "line 13
       says invalid parameter or cycle contour" when it pressed `1 SINGLE` on
       line 8 (the whole block is sent, so the module collects all of it). */
    fails += command("G0 X54 Z2", STATUS_OK);
    fails += command("G71 U2 R1 X0 Z0 F50 P50 Q55", STATUS_OK);
    fails += command("G71 U2 R0.2 X0.5 Z0.5 F450", STATUS_OK);
    fails += command("N50 G1 X30 Z2", STATUS_OK);
    fails += command("G1 X30 Z-15 C2", STATUS_OK);
    fails += command("G1 X35 Z-15", STATUS_OK);
    fails += command("G1 X35 Z-25", STATUS_OK);
    fails += command("N55 G1 X52 Z-25", STATUS_OK);
    fails += command("G80", STATUS_OK);
    if (g7x_parser_busy()) {
        puts("FAIL the range-on-first-line pair left the collector armed");
        fails++;
    }
    if (g7x_take_refusal_text()[0] != '\0') {
        puts("FAIL the range-on-first-line pair left a refusal behind");
        fails++;
    }
#ifndef G7X_STANDALONE_TEST
    static nc_document_t doc;
    nc_drain_machine();
    nc_document_init(&doc);
    nc_insert_line(&doc, 0, "G71 U1 F300");
    nc_insert_line(&doc, 1, "G1 X40 Z0");
    nc_insert_line(&doc, 2, "X30 Z-10");
    nc_insert_line(&doc, 3, "X35 Z-5");
    nc_insert_line(&doc, 4, "G80");
    nc_insert_line(&doc, 5, "G0 X999");
    nc_start_run(&doc, 0);
    for (int i = 0; i < 4; i++) {
        uint8_t status = STATUS_OK;

        if (!nc_parse_block(&status) || status != STATUS_OK) fails++;
    }
    {
        uint8_t status = STATUS_OK;
        (void)nc_parse_block(&status);
        if (status != STATUS_INVALID_STATEMENT || nc_run_active()) {
            puts("FAIL NC abort on contour error"); fails++;
        }
    }
    if (nc_run_error() != STATUS_INVALID_STATEMENT || nc_run_error_line() != 4) {
        puts("FAIL NC source error location"); fails++;
    }
    if (grbl_stream_available()) { puts("FAIL NC retained source after error"); fails++; }

    nc_document_init(&doc);
    nc_insert_line(&doc, 0, "G71 U0.1 F300");
    nc_insert_line(&doc, 1, "G1 X40 Z0");
    nc_insert_line(&doc, 2, "X20 Z-20");
    nc_insert_line(&doc, 3, "G80");
    nc_insert_line(&doc, 4, "G0 X999");
    nc_start_run(&doc, 0);
    for (int i = 0; i < 3; i++) {
        uint8_t status = STATUS_OK;

        if (!nc_parse_block(&status) || status != STATUS_OK) fails++;
    }
    stop_motion_at = motion_count + 3;
    {
        uint8_t status = STATUS_OK;

        (void)nc_parse_block(&status);
        if (status != STATUS_SYSTEM_GC_LOCK) { puts("FAIL NC Stop status"); fails++; }
    }
    stop_motion_at = 0;
    for (int i = 0; i < 10000 && cnc_get_exec_state(EXEC_CANCELING); i++) {
        cnc_dotasks(); mcu_unit_test_advance_time(1000);
    }
    if (nc_run_active() || g7x_parser_busy() || !planner_buffer_is_empty() ||
        cnc_get_exec_state(EXEC_CANCELING)) { puts("FAIL NC Stop cleanup"); fails++; }

    /* EOF is not completion: controls must still affect the queued tail. */
    nc_document_init(&doc);
    nc_insert_line(&doc, 0, "G1 X80 Z0 F300");
    nc_start_run(&doc, 0);
    {
        uint8_t status = STATUS_OK;

        if (!nc_parse_block(&status) || status != STATUS_OK) fails++;
    }
    if (!nc_run_active() || nc_run_done()) { puts("FAIL early NC completion"); fails++; }
    if (!nc_run_toggle_hold() || !cnc_get_exec_state(EXEC_HOLD)) {
        puts("FAIL NC hold queued tail"); fails++;
    }
    if (!nc_run_toggle_hold()) { puts("FAIL NC resume queued tail"); fails++; }
    cnc_dotasks();
    nc_run_stop();
    for (int i = 0; i < 10000 && cnc_get_exec_state(EXEC_CANCELING); i++) {
        cnc_dotasks(); mcu_unit_test_advance_time(1000);
    }
    if (!planner_buffer_is_empty() || cnc_get_exec_state(EXEC_CANCELING)) {
        printf("FAIL NC Stop queued tail state=%u planner_empty=%d itp_empty=%d\n",
               cnc_get_exec_state(EXEC_ALLACTIVE), planner_buffer_is_empty(), itp_is_empty()); fails++;
    }
    /* A held Stop must retain the hardware hold and expose it on restart. */
    nc_start_run(&doc, 0);
    {
        uint8_t status = STATUS_OK;

        if (!nc_parse_block(&status) || status != STATUS_OK) fails++;
    }
    if (!nc_run_toggle_hold()) fails++;
    nc_run_stop();
    for (int i = 0; i < 10000 && cnc_get_exec_state(EXEC_CANCELING); i++) {
        cnc_dotasks(); mcu_unit_test_advance_time(1000);
    }
    nc_start_run(&doc, 0);
    if (!nc_run_hold() || !cnc_get_exec_state(EXEC_HOLD)) {
        puts("FAIL held Stop restart state"); fails++;
    }
    if (!nc_run_toggle_hold()) fails++;
    cnc_dotasks();
    if (nc_run_hold() || cnc_get_exec_state(EXEC_HOLD)) {
        puts("FAIL held Stop restart resume"); fails++;
    }
    nc_run_stop();
    cnc_dotasks();

    /* Document EOF cannot leave the modal contour collector alive. */
    nc_document_init(&doc);
    nc_insert_line(&doc, 0, "G71 U1 F300");
    nc_insert_line(&doc, 1, "G1 X40 Z0");
    nc_start_run(&doc, 0);
    for (int i = 0; i < 2; i++) {
        uint8_t status = STATUS_OK;

        if (!nc_parse_block(&status) || status != STATUS_OK) fails++;
    }
    /* The document has no end mark: the pacer runs out of lines and the run has
       to end there, without leaving the contour collector armed. */
    (void)nc_run_to_end();
    if (g7x_parser_busy() || nc_run_active() || nc_run_done()) {
        puts("FAIL unterminated NC contour"); fails++;
    }
    before = motion_count;
    fails += command("G0 X42", STATUS_OK);
    if (motion_count != before + 1) { puts("FAIL stale EOF collector"); fails++; }
    nc_insert_line(&doc, 2, "X38 Z-1");
    nc_insert_line(&doc, 3, "G80");
    nc_start_run(&doc, 0);
    for (int i = 0; i < 4; i++) {
        uint8_t status = STATUS_OK;

        if (!nc_parse_block(&status) || status != STATUS_OK) {
            puts("FAIL complete NC contour"); fails++;
        }
    }
    (void)nc_run_to_end();
    if (g7x_parser_busy()) { puts("FAIL complete NC collector cleanup"); fails++; }

    /* A numbered range that never reaches N(Q) is an incomplete cycle. */
    nc_document_init(&doc);
    nc_insert_line(&doc, 0, "G71 U1 R1 P100 Q200 X0.1 Z0.1 F300");
    nc_insert_line(&doc, 1, "N100 G1 X50 Z0");
    nc_insert_line(&doc, 2, "G1 X45 Z-5");
    nc_start_run(&doc, 0);
    for (int i = 0; i < 3; i++) {
        uint8_t status = STATUS_OK;

        if (!nc_parse_block(&status) || status != STATUS_OK) fails++;
    }
    (void)nc_run_to_end();
    if (g7x_parser_busy() || nc_run_active() || nc_run_done()) {
        puts("FAIL unterminated NC numbered range"); fails++;
    }
    before = motion_count;
    fails += command("G0 X42", STATUS_OK);
    if (motion_count != before + 1) { puts("FAIL stale numbered range"); fails++; }

    /* Selecting the second line of a Fanuc two-line header still sends the whole
       block: both header lines plus the numbered profile. */
    nc_document_init(&doc);
    nc_insert_line(&doc, 0, "G71 U1 R1");
    nc_insert_line(&doc, 1, "G71 P100 Q200 U0.5 W0.25 F300");
    nc_insert_line(&doc, 2, "N100 G1 X50 Z0");
    nc_insert_line(&doc, 3, "G1 X50 Z-10");
    nc_insert_line(&doc, 4, "N200 G1 X40 Z-10");
    nc_insert_line(&doc, 5, "G0 X80 Z0");
    nc_drain_machine();
    if (!nc_run_send_document_line(&doc, 1)) {
        puts("FAIL two-line selected send arm"); fails++;
    }
    {
        uint8_t first = nc_run_to_end();

        if (first != STATUS_OK) {
            printf("FAIL two-line selected send (status %u)\n", (unsigned)first);
            fails++;
        }
    }
    if (g7x_parser_busy()) { puts("FAIL two-line selected send cleanup"); fails++; }

    /* Whole-file RUN through a two-line numbered range and a trailing line. */
    nc_document_init(&doc);
    nc_insert_line(&doc, 0, "G0 X54 Z2");
    nc_insert_line(&doc, 1, "G71 U1 R1");
    nc_insert_line(&doc, 2, "G71 P100 Q200 U0.5 W0.25 F300");
    nc_insert_line(&doc, 3, "N100 G1 X50 Z0");
    nc_insert_line(&doc, 4, "G1 X50 Z-10");
    nc_insert_line(&doc, 5, "N200 G1 X40 Z-10");
    nc_insert_line(&doc, 6, "G0 X80 Z0");
    nc_start_run(&doc, 0);
    if (nc_run_to_end() != STATUS_OK) {
        puts("FAIL two-line RUN stream"); fails++;
    }
    if (g7x_parser_busy() || nc_run_error()) {
        puts("FAIL two-line RUN cleanup"); fails++;
    }
    mc_get_position(pos);
    if (fabsf(pos[AXIS_X] - 40.0f) > 0.001f) {
        printf("FAIL two-line RUN trailing line X%.3f\n", pos[AXIS_X]);
        fails++;
    }

    nc_document_init(&doc);
    nc_insert_line(&doc, 0, "G1 Xbad");
    if (!nc_run_send_document_line(&doc, 0)) fails++;
    if (cnc_parse_cmd() == STATUS_OK || !nc_run_error() || nc_run_error_line() != 0) {
        puts("FAIL selected-line error notification"); fails++;
    }
    nc_document_init(&doc);
    nc_insert_line(&doc, 0, "G0 X42");
    if (!nc_run_send_document_line(&doc, 0)) fails++;
    if (cnc_parse_cmd() != STATUS_OK || nc_run_error()) {
        puts("FAIL selected-line retry"); fails++;
    }
    (void)grbl_stream_available();
#endif
    printf("Parser integration: %d failures\n", fails);
    return fails ? 1 : 0;
}
