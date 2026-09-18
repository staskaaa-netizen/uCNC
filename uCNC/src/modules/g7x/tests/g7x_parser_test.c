/* Real parser/planner integration; spindle execution is intercepted below.
   This verifies generated G33 targets, not physical spindle synchronization. */
#include "src/cnc.h"
#include "../g7x.h"
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
#ifndef G7X_STANDALONE_TEST
    static nc_document_t doc;
    nc_document_init(&doc);
    nc_insert_line(&doc, 0, "G71 U1 F300");
    nc_insert_line(&doc, 1, "G1 X40 Z0");
    nc_insert_line(&doc, 2, "X30 Z-10");
    nc_insert_line(&doc, 3, "X35 Z-5");
    nc_insert_line(&doc, 4, "G80");
    nc_insert_line(&doc, 5, "G0 X999");
    nc_run_start_stream(&doc, 0);
    for (int i = 0; i < 4; i++) if (cnc_parse_cmd() != STATUS_OK) fails++;
    if (cnc_parse_cmd() != STATUS_INVALID_STATEMENT || nc_run_active()) {
        puts("FAIL NC abort on contour error"); fails++;
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
    nc_run_start_stream(&doc, 0);
    for (int i = 0; i < 3; i++) if (cnc_parse_cmd() != STATUS_OK) fails++;
    stop_motion_at = motion_count + 3;
    if (cnc_parse_cmd() != STATUS_SYSTEM_GC_LOCK) { puts("FAIL NC Stop status"); fails++; }
    stop_motion_at = 0;
    for (int i = 0; i < 10000 && cnc_get_exec_state(EXEC_CANCELING); i++) {
        cnc_dotasks(); mcu_unit_test_advance_time(1000);
    }
    if (nc_run_active() || g7x_parser_busy() || !planner_buffer_is_empty() ||
        cnc_get_exec_state(EXEC_CANCELING)) { puts("FAIL NC Stop cleanup"); fails++; }

    /* EOF is not completion: controls must still affect the queued tail. */
    nc_document_init(&doc);
    nc_insert_line(&doc, 0, "G1 X80 Z0 F300");
    nc_run_start_stream(&doc, 0);
    if (cnc_parse_cmd() != STATUS_OK) fails++;
    (void)grbl_stream_available();
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
    nc_run_start_stream(&doc, 0);
    if (cnc_parse_cmd() != STATUS_OK) fails++;
    if (!nc_run_toggle_hold()) fails++;
    nc_run_stop();
    for (int i = 0; i < 10000 && cnc_get_exec_state(EXEC_CANCELING); i++) {
        cnc_dotasks(); mcu_unit_test_advance_time(1000);
    }
    nc_run_start_stream(&doc, 0);
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
    nc_run_start_stream(&doc, 0);
    if (cnc_parse_cmd() != STATUS_OK || cnc_parse_cmd() != STATUS_OK) fails++;
    (void)grbl_stream_available();
    if (g7x_parser_busy() || nc_run_active() || nc_run_done()) {
        puts("FAIL unterminated NC contour"); fails++;
    }
    before = motion_count;
    fails += command("G0 X42", STATUS_OK);
    if (motion_count != before + 1) { puts("FAIL stale EOF collector"); fails++; }
    nc_insert_line(&doc, 2, "X38 Z-1");
    nc_insert_line(&doc, 3, "G80");
    nc_run_start_stream(&doc, 0);
    for (int i = 0; i < 4; i++) {
        if (cnc_parse_cmd() != STATUS_OK) { puts("FAIL complete NC contour"); fails++; }
    }
    (void)grbl_stream_available();
    if (g7x_parser_busy()) { puts("FAIL complete NC collector cleanup"); fails++; }
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
