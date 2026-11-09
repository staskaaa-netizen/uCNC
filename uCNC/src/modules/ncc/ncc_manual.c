#include "ncc_manual.h"

#include "ncc_draw.h"
#include "ncc_layout.h"
#include "../nc_sender/nc_sender.h"
#include "ncc_state.h"
#include "ncc_visual.h"

#include "../../cnc.h"
#include "../../core/parser.h"
#include "../g7_g8/parser_g7_g8.h"
#include "../lvds_renderer/lvds_hstx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NCC_MIN
#define NCC_MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* MANUAL's own state, and nothing else's: the axis the keys act on, the step
   and the feed, the two stops of each axis, the field being typed and the jog
   in flight. */
static uint8_t g_ncc_manual_axis;                     /* 0 = X, 1 = Z */
static const float g_ncc_manual_steps[] = {
    0.010f, 0.025f, 0.050f, 0.100f, 0.250f, 0.500f, 1.000f
};
static const float g_ncc_manual_feeds[] = {
    50.0f, 100.0f, 200.0f, 300.0f, 500.0f, 800.0f, 1000.0f, 2000.0f
};
static uint8_t g_ncc_manual_step_index = 3u;          /* 0.100 mm per press */
static uint8_t g_ncc_manual_feed_index = 4u;          /* 500 mm/min */
static bool g_ncc_manual_continuous;
static bool g_ncc_manual_stop_set[2][2];
static float g_ncc_manual_stop[2][2];
static char g_ncc_manual_spindle_dir;                 /* 0 off, '3' CW, '4' CCW */
static char g_ncc_manual_feed_key;
static char g_ncc_manual_held_key;
static char g_ncc_manual_flash_key;
static uint32_t g_ncc_manual_flash_ms;
static uint8_t g_ncc_manual_field;                    /* 0 none, 1 minus, 2 plus */
static char g_ncc_manual_field_text[16];
static char g_ncc_manual_touch[16];
static bool g_ncc_manual_touch_active;

/* The nine jog labels, in key order 1..9: the pad *is* the machine's jog panel
   on this screen. The X pair follows the drawing, so the key that points up
   takes the tool toward the centre - the smaller diameter, X-. */
static const char *const g_ncc_manual_labels[9] = {
    "FD-", "X+", "FD+", "Z-", "STOP", "Z+", "CCW", "X-", "CW"
};

/* A feed with no wall in front of it is bounded by the axis travel the machine
   states, or by this cap when it states none, so a key release the panel never
   sees cannot run the axis to the end of the machine. */
#define NCC_MANUAL_FEED_CAP_MM 25.0
#define NCC_MANUAL_FEED_MIN_MM 0.005
#define NCC_MANUAL_FLASH_MS 250u

float ncc_manual_step(void)
{
    return g_ncc_manual_steps[g_ncc_manual_step_index];
}

float ncc_manual_feed(void)
{
    return g_ncc_manual_feeds[g_ncc_manual_feed_index];
}

int ncc_manual_axis(void)
{
    return (int)g_ncc_manual_axis;
}

bool ncc_manual_continuous(void)
{
    return g_ncc_manual_continuous;
}

bool ncc_manual_field_active(void)
{
    return g_ncc_manual_field != 0u;
}

bool ncc_manual_flash(char key)
{
    return g_ncc_manual_flash_key == key &&
           (uint32_t)(mcu_millis() - g_ncc_manual_flash_ms) < NCC_MANUAL_FLASH_MS;
}

const char *ncc_manual_pad_label(char key)
{
    if (key < '1' || key > '9') {
        return "";
    }
    return g_ncc_manual_labels[key - '1'];
}

static char ncc_manual_letter(void)
{
    return g_ncc_manual_axis ? 'Z' : 'X';
}

static const char *ncc_manual_side_name(int side)
{
    return side == 0 ? "-" : "+";
}

static float ncc_manual_machine_value(void)
{
    ncc_runtime_state_t rt;

    ncc_state_runtime(&rt);
    return g_ncc_manual_axis ? rt.z : rt.x;
}

static void ncc_manual_status(const char *text)
{
    ncc_visual_status_set(text);
}

/* The panel works in axis millimetres. A programmed X is a diameter (the lathe's
   own frame), so an axis move of 1 mm has to be written 2 mm or every X jog
   would stop half way to what the readout calls the position. */
static double ncc_manual_program_length(uint8_t axis, double length)
{
    if (axis == AXIS_X && g7_g8_is_diameter_mode()) {
        return length * 2.0;
    }
    return length;
}

static double ncc_manual_feed_limit(uint8_t axis)
{
    double travel = (double)g_settings.max_distance[axis];

    return (travel > 1.0) ? travel : NCC_MANUAL_FEED_CAP_MM;
}

/* The axis limit the setup states: [0, $130] for the axis. */
static bool ncc_manual_setup_limit(uint8_t axis, uint8_t side, float *value)
{
    float travel = g_settings.max_distance[axis];

    if (!value || !(travel > 0.0f)) {
        return false;
    }
    *value = (side == 0u) ? 0.0f : travel;
    return true;
}

bool ncc_manual_stop(int axis, int side, float *value, bool *typed)
{
    if (axis < 0 || axis > 1 || side < 0 || side > 1) {
        return false;
    }
    if (g_ncc_manual_stop_set[axis][side]) {
        if (value) {
            *value = g_ncc_manual_stop[axis][side];
        }
        if (typed) {
            *typed = true;
        }
        return true;
    }
    if (ncc_manual_setup_limit((uint8_t)axis, (uint8_t)side, value)) {
        if (typed) {
            *typed = false;
        }
        return true;
    }
    return false;
}

/* The stop the move is headed for: the plus limit for a positive move, the
   minus one for a negative one. */
static bool ncc_manual_stop_ahead(uint8_t axis, int direction, float *stop)
{
    uint8_t side = (direction > 0) ? 1u : 0u;

    return ncc_manual_stop(axis, side, stop, NULL);
}

/* Panel blocks are refused while RUN holds the reader: a jog lands between two
   program blocks and the G90 it leaves behind would cut into the cycle. */
static bool ncc_manual_send(const char *line)
{
    if (nc_sender_run_streaming()) {
        ncc_manual_status("Program running");
        return false;
    }
    if (cnc_get_exec_state(EXEC_JOG)) {
        ncc_manual_status("Jog busy");
        return false;
    }
    if (cnc_get_exec_state(EXEC_JOG_LOCKED) || cnc_has_alarm()) {
        ncc_manual_status("Controller locked");
        return false;
    }
    if (!nc_sender_run_send_line(line)) {
        ncc_manual_status("Send failed");
        return false;
    }
    return true;
}

void ncc_manual_feed_cancel(void)
{
    if (!g_ncc_manual_feed_key) {
        return;
    }
    g_ncc_manual_feed_key = 0;
    cnc_call_rt_command(CMD_CODE_JOG_CANCEL);
    ncc_manual_status("Feed stop");
}

static bool ncc_manual_axis_standing(const char *status)
{
    if (cnc_get_exec_state(EXEC_JOG | EXEC_RUN)) {
        ncc_manual_status(status);
        return false;
    }
    return true;
}

/* A held direction key feeds: toward the stop the block is the whole distance
   to the wall, so the jog ends on it by itself; away from it (or with no stop
   set) the feed is a bounded move. Letting the key go cancels it where it
   stands. */
static void ncc_manual_feed_start(char key, int direction)
{
    uint8_t axis = g_ncc_manual_axis;
    float stop = 0.0f;
    bool to_stop = false;
    double move;
    char line[48];

    if (!ncc_manual_axis_standing("Wait for stop")) {
        return;
    }
    if (ncc_manual_stop_ahead(axis, direction, &stop)) {
        double room = ((double)stop - (double)ncc_manual_machine_value()) *
                      (double)direction;

        if (room < NCC_MANUAL_FEED_MIN_MM) {
            char text[32];

            snprintf(text, sizeof(text), "At %c%s stop", ncc_manual_letter(),
                     ncc_manual_side_name(direction > 0));
            ncc_manual_status(text);
            return;
        }
        to_stop = true;
        move = (double)stop - (double)ncc_manual_machine_value();
    } else {
        move = ncc_manual_feed_limit(axis) * (double)direction;
    }
    snprintf(line, sizeof(line), "$J=G91 %c%.3f F%.0f", ncc_manual_letter(),
             ncc_manual_program_length(axis, move), (double)ncc_manual_feed());
    if (!ncc_manual_send(line)) {
        return;
    }
    g_ncc_manual_feed_key = key;
    {
        char text[40];

        if (to_stop) {
            snprintf(text, sizeof(text), "Feed to %.3f", (double)stop);
        } else {
            snprintf(text, sizeof(text), "Feed %.3f mm",
                     move < 0.0 ? -move : move);
        }
        ncc_manual_status(text);
    }
}

static void ncc_manual_jog(char key, int direction)
{
    char line[48];
    char sent[48];
    double step = (double)ncc_manual_step();
    uint8_t axis = g_ncc_manual_axis;

    if (g_ncc_manual_continuous) {
        ncc_manual_feed_start(key, direction);
        return;
    }
    ncc_manual_feed_cancel();
    if (!ncc_manual_axis_standing("Jog busy")) {
        return;
    }
    {
        float stop = 0.0f;

        if (ncc_manual_stop_ahead(axis, direction, &stop)) {
            double room = ((double)stop - (double)ncc_manual_machine_value()) *
                          (double)direction;

            step = NCC_MIN(step, room);
            if (step < 0.0005) {
                char text[32];

                snprintf(text, sizeof(text), "At %c%s stop",
                         ncc_manual_letter(),
                         ncc_manual_side_name(direction > 0));
                ncc_manual_status(text);
                return;
            }
        }
    }
    /* Incremental, and back to absolute in the same breath: a jog must not leave
       the machine in G91 for whatever runs next. Both blocks are queued, so the
       pairing survives the trip through the reader. */
    snprintf(line, sizeof(line), "G91 G1 %c%.3f F%.0f", ncc_manual_letter(),
             ncc_manual_program_length(axis, step * (double)direction),
             (double)ncc_manual_feed());
    if (!ncc_manual_send(line)) {
        return;
    }
    snprintf(sent, sizeof(sent), "%s", line);
    ncc_manual_send("G90");
    ncc_manual_status(sent);
}

static void ncc_manual_step_toggle(void)
{
    ncc_manual_feed_cancel();
    g_ncc_manual_continuous = !g_ncc_manual_continuous;
    ncc_manual_status(g_ncc_manual_continuous ? "Continuous feed" : "Step jog");
}

/* `1` and `3` change the value the feed mode is using: the step per press, or
   the feed the continuous jog runs at. */
static void ncc_manual_value_adjust(int direction)
{
    char text[40];

    if (g_ncc_manual_continuous) {
        if (direction < 0) {
            if (g_ncc_manual_feed_index > 0u) {
                g_ncc_manual_feed_index--;
            }
        } else if (g_ncc_manual_feed_index + 1u <
                   sizeof(g_ncc_manual_feeds) / sizeof(g_ncc_manual_feeds[0])) {
            g_ncc_manual_feed_index++;
        }
        snprintf(text, sizeof(text), "Feed %.0f mm/min", (double)ncc_manual_feed());
    } else {
        if (direction < 0) {
            if (g_ncc_manual_step_index > 0u) {
                g_ncc_manual_step_index--;
            }
        } else if (g_ncc_manual_step_index + 1u <
                   sizeof(g_ncc_manual_steps) / sizeof(g_ncc_manual_steps[0])) {
            g_ncc_manual_step_index++;
        }
        snprintf(text, sizeof(text), "Step %.3f mm", (double)ncc_manual_step());
    }
    ncc_manual_status(text);
}

static void ncc_manual_field_status(void)
{
    uint8_t axis = g_ncc_manual_axis;
    uint8_t side = (uint8_t)(g_ncc_manual_field - 1u);
    char limit[24];
    char text[64];
    float setup = 0.0f;

    if (ncc_manual_setup_limit(axis, side, &setup)) {
        snprintf(limit, sizeof(limit), "D=%.3f", (double)setup);
    } else {
        snprintf(limit, sizeof(limit), "%s", "no limit");
    }
    snprintf(text, sizeof(text), "%c%s stop %s *OK A", ncc_manual_letter(),
             ncc_manual_side_name(side), limit);
    ncc_manual_status(text);
}

static void ncc_manual_field_open(uint8_t field)
{
    g_ncc_manual_field = field;
    g_ncc_manual_field_text[0] = '\0';
    ncc_manual_field_status();
}

/* Take what the field holds as the stop of that side - or the axis limit the
   setup states when nothing was typed. */
static void ncc_manual_field_take(void)
{
    uint8_t axis = g_ncc_manual_axis;
    uint8_t side = (uint8_t)(g_ncc_manual_field - 1u);
    float value = 0.0f;
    char text[40];

    if (g_ncc_manual_field_text[0]) {
        value = strtof(g_ncc_manual_field_text, NULL);
    } else if (!ncc_manual_setup_limit(axis, side, &value)) {
        snprintf(text, sizeof(text), "%c%s stop unchanged", ncc_manual_letter(),
                 ncc_manual_side_name(side));
        ncc_manual_status(text);
        return;
    }
    g_ncc_manual_stop[axis][side] = value;
    g_ncc_manual_stop_set[axis][side] = true;
    snprintf(text, sizeof(text), "%c%s stop %.3f", ncc_manual_letter(),
             ncc_manual_side_name(side), (double)value);
    ncc_manual_status(text);
}

static void ncc_manual_zero(void)
{
    char line[48];
    char text[32];

    snprintf(line, sizeof(line), "G10 L20 P0 %c0", ncc_manual_letter());
    ncc_manual_send(line);
    snprintf(text, sizeof(text), "Zero %c", ncc_manual_letter());
    ncc_manual_status(text);
}

static void ncc_manual_touch_apply(void)
{
    char line[48];
    char text[40];

    if (!g_ncc_manual_touch[0]) {
        return;
    }
    snprintf(line, sizeof(line), "G10 L20 P0 %c%s", ncc_manual_letter(),
             g_ncc_manual_touch);
    if (!ncc_manual_send(line)) {
        return;
    }
    snprintf(text, sizeof(text), "Touch %c=%s", ncc_manual_letter(),
             g_ncc_manual_touch);
    g_ncc_manual_touch_active = false;
    g_ncc_manual_touch[0] = '\0';
    ncc_manual_status(text);
}

static void ncc_manual_spindle(char code)
{
    char line[24];
    char text[40];

    if (code == 'S') {
        ncc_manual_send("M5");
        g_ncc_manual_spindle_dir = 0;
        ncc_manual_status("Spindle stop");
        return;
    }
    {
        /* Start at the speed the machine already has - the modal S a program or
           an earlier command left - so the operator's speed is what comes back.
           A spindle that states none leaves the last speed this panel used
           standing, which is remembered with the rest of the state. */
        uint8_t modes[16];
        uint16_t feed = 0u;
        uint16_t modal = 0u;
        unsigned rpm;

        parser_get_modes(modes, &feed, &modal);
        if (modal >= 1u) {
            ncc_state_remember_manual_spindle((unsigned)modal);
        }
        rpm = ncc_state_manual_spindle();
        g_ncc_manual_spindle_dir = code;
        snprintf(line, sizeof(line), "M%c S%u", code, rpm);
        ncc_manual_send(line);
        snprintf(text, sizeof(text), "Spindle %s S%u",
                 code == '3' ? "CW" : "CCW", rpm);
        ncc_manual_status(text);
    }
}

static void ncc_manual_flash_set(char key)
{
    g_ncc_manual_flash_key = key;
    g_ncc_manual_flash_ms = mcu_millis();
}

/* The axis keys (the keypad's `B`/`C`, the shell's arrows). */
static void ncc_manual_pick_axis(int axis)
{
    ncc_manual_feed_cancel();
    g_ncc_manual_axis = (uint8_t)(axis ? 1 : 0);
    ncc_manual_status("Axis");
}

bool ncc_manual_key(char key)
{
    if (g_ncc_manual_touch_active) {
        /* The touch field is open: the keys type into it and nothing else on the
           screen sees them. */
        if (key >= '0' && key <= '9' && strlen(g_ncc_manual_touch) < 12u) {
            size_t n = strlen(g_ncc_manual_touch);

            g_ncc_manual_touch[n] = key;
            g_ncc_manual_touch[n + 1u] = '\0';
            return true;
        }
        if (key == 'B') {
            size_t n = strlen(g_ncc_manual_touch);

            if (n > 0u) {
                g_ncc_manual_touch[n - 1u] = '\0';
            }
            return true;
        }
        if (key == 'C') {
            size_t n = strlen(g_ncc_manual_touch);

            if (n + 1u < 12u) {
                g_ncc_manual_touch[n] = '.';
                g_ncc_manual_touch[n + 1u] = '\0';
            }
            return true;
        }
        if (key == 'D' || key == '#') {
            ncc_manual_touch_apply();
            return true;
        }
        if (key == 'A' || key == '*') {
            g_ncc_manual_touch_active = false;
            g_ncc_manual_touch[0] = '\0';
            ncc_manual_status("Touch cancelled");
            return true;
        }
        return true;
    }
    if (g_ncc_manual_field) {
        /* The stop field is open: the digits type, `*` takes it and opens the
           other one, `D` puts the axis limit in it, `A` leaves it. */
        if (key >= '0' && key <= '9') {
            size_t n = strlen(g_ncc_manual_field_text);

            if (n < 12u) {
                g_ncc_manual_field_text[n] = key;
                g_ncc_manual_field_text[n + 1u] = '\0';
            }
            return true;
        }
        if (key == 'B' || key == 'C') {
            size_t n = strlen(g_ncc_manual_field_text);

            if (key == 'C' && n + 1u < 12u) {
                g_ncc_manual_field_text[n] = '.';
                g_ncc_manual_field_text[n + 1u] = '\0';
            } else if (key == 'B' && n > 0u) {
                g_ncc_manual_field_text[n - 1u] = '\0';
            }
            return true;
        }
        if (key == '*') {
            ncc_manual_field_take();
            if (g_ncc_manual_field == 1u) {
                ncc_manual_field_open(2u);
            } else {
                g_ncc_manual_field = 0u;
            }
            return true;
        }
        if (key == 'D') {
            float setup = 0.0f;

            if (ncc_manual_setup_limit(g_ncc_manual_axis,
                                       (uint8_t)(g_ncc_manual_field - 1u),
                                       &setup)) {
                snprintf(g_ncc_manual_field_text,
                         sizeof(g_ncc_manual_field_text), "%.3f",
                         (double)setup);
            }
            return true;
        }
        if (key == 'A') {
            g_ncc_manual_field = 0u;
            ncc_manual_status("Stop field left");
            return true;
        }
        return true;
    }
    switch (key) {
    case '2': g_ncc_manual_axis = 0; ncc_manual_jog('2', +1); break;
    case '8': g_ncc_manual_axis = 0; ncc_manual_jog('8', -1); break;
    case '4': g_ncc_manual_axis = 1; ncc_manual_jog('4', -1); break;
    case '6': g_ncc_manual_axis = 1; ncc_manual_jog('6', +1); break;
    case '5': ncc_manual_spindle('S'); break;
    case '7': ncc_manual_spindle('4'); break;      /* CCW */
    case '9': ncc_manual_spindle('3'); break;      /* CW */
    case '1': ncc_manual_value_adjust(-1); break;
    case '3': ncc_manual_value_adjust(+1); break;
    case '#': ncc_manual_step_toggle(); break;
    case '*': ncc_manual_field_open(1u); break;
    case 'D':
        /* Touch-off: the next keys type the work offset for the picked axis. */
        g_ncc_manual_touch_active = true;
        g_ncc_manual_touch[0] = '\0';
        ncc_manual_status("Touch: a value, D takes it");
        break;
    case '0': ncc_manual_zero(); break;
    case 'B': ncc_manual_pick_axis(0); break;
    case 'C': ncc_manual_pick_axis(1); break;
    default:
        return false;
    }
    ncc_manual_flash_set(key);
    return true;
}

void ncc_manual_hold(char key)
{
    bool changed = key != g_ncc_manual_held_key;

    g_ncc_manual_held_key = key;
    if (!g_ncc_manual_feed_key) {
        return;                             /* nothing is feeding */
    }
    /* An alarm or a program taking the reader stops the feed here as well as in
       the controller: the panel must not believe a jog is still running. */
    if (cnc_has_alarm() || nc_sender_run_streaming()) {
        ncc_manual_feed_cancel();
        return;
    }
    /* The key that started the feed is the only key that keeps it running.
       Letting go, or reaching for another key, ends it where it is. */
    if (changed && key != g_ncc_manual_feed_key) {
        ncc_manual_feed_cancel();
    }
}

/* --- drawing -------------------------------------------------------------- */

void ncc_manual_draw_pane(int x, int y, int w, int h)
{
    static const char *const axis_name[2] = { "X", "Z" };
    char text[64];
    int axis;
    int row = 0;
    int col_w = ncc_col_width(LVDS_FONT_NORMAL);

    ncc_fill(x, y, w, h, ncc_col_bg());
    for (axis = 0; axis < 2; axis++) {
        int side;

        for (side = 0; side < 2; side++) {
            float value = 0.0f;
            bool typed = false;
            bool hot = axis == (int)g_ncc_manual_axis;
            lvds_color_t fg;

            if (!ncc_manual_stop(axis, side, &value, &typed)) {
                continue;
            }
            snprintf(text, sizeof(text), "%s LIMIT %c  %9.3f",
                     axis_name[axis], side == 0 ? '-' : '+', (double)value);
            fg = typed ? ncc_col_text() : ncc_col_dim();
            if (hot) {
                ncc_fill(x, y + 4 + row * NCC_ROW_H, w, NCC_ROW_H - 2,
                         ncc_col_select());
                ncc_text_clip(x + 4, y + 6 + row * NCC_ROW_H, text,
                              (w - 8) / col_w, ncc_col_pad_text(),
                              ncc_col_select(), LVDS_FONT_NORMAL);
            } else {
                ncc_text_clip(x + 4, y + 6 + row * NCC_ROW_H, text,
                              (w - 8) / col_w, fg, ncc_col_bg(),
                              LVDS_FONT_NORMAL);
            }
            row++;
        }
    }
    if (g_ncc_manual_continuous) {
        snprintf(text, sizeof(text), "FEED  %.0f mm/min", (double)ncc_manual_feed());
    } else {
        snprintf(text, sizeof(text), "STEP  %.3f mm", (double)ncc_manual_step());
    }
    ncc_text_clip(x + 4, y + 6 + row * NCC_ROW_H, text, (w - 8) / col_w,
                  ncc_col_text(), ncc_col_bg(), LVDS_FONT_NORMAL);
    row++;
    snprintf(text, sizeof(text), "AXIS  %c   %s", ncc_manual_letter(),
             g_ncc_manual_continuous ? "feeds while a key is held" : "steps");
    ncc_text_clip(x + 4, y + 6 + row * NCC_ROW_H, text, (w - 8) / col_w,
                  ncc_col_dim(), ncc_col_bg(), LVDS_FONT_SMALL);
}
