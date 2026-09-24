/* MANUAL: the readout the jog keys work on - see nc_manual.h. It keeps the
   state only it uses and is handed the two things it may write outside itself:
   the screen's status line and repaint flag. */
#include "nc_manual.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../cnc.h"
#include "nc_draw.h"
#include "nc_layout.h"
#include "nc_menu.h"
#include "nc_preview.h"
#include "nc_run.h"
#include "nc_state.h"
#include "../g7_g8/parser_g7_g8.h"
#include "../lvds_renderer/lvds_draw_api.h"
#include "../lvds_renderer/lvds_hstx.h"

/* One guard for the jog keys: a move queued while the axis is already moving
   has nowhere to land. It says what it means instead of letting the controller
   answer with a code. */
static bool nc_manual_axis_standing(const nc_manual_screen_t *screen, const char *status)
{
    if (cnc_get_exec_state(EXEC_JOG | EXEC_RUN)) {
        strncpy(screen->status, status, screen->status_size - 1);
        *screen->dirty = true;
        return false;
    }
    return true;
}

/* --- MANUAL: a readout with the axis the keys act on, and the jog keys -----

   B/C pick the axis, the 3x3 digits jog that axis, run the feed override and
   the spindle. Zero and touch-off write the work offset for the picked axis,
   which is what "set it to zero" means to the machine. */
static uint8_t g_nc_manual_axis;                 /* 0 = X, 1 = Z */
static char g_nc_manual_touch[16];
static bool g_nc_manual_touch_active;

/* What the spindle keys do when the machine has no speed of its own: the last
   speed the panel used, from the state store (and 1000 the first time). The
   constant is only the fallback - a spindle that always starts at one number is
   not what the operator left behind. */
#define NC_MANUAL_SPINDLE_RPM  nc_state_manual_spindle()

/* The two values the jog keys use: the step one keypress moves, and the feed
   the continuous jog runs at. `1` and `3` change the one the feed mode is
   using, and the readout beside the pad shows both, so what the keys change is
   always on screen. The screen owns them: a jog block and the readout beside
   it cannot disagree. */
static const float g_nc_manual_steps[] = {
    0.010f, 0.025f, 0.050f, 0.100f, 0.250f, 0.500f, 1.000f
};
static const float g_nc_manual_feeds[] = {
    50.0f, 100.0f, 200.0f, 300.0f, 500.0f, 800.0f, 1000.0f, 2000.0f
};
#define NC_MANUAL_STEP_DEFAULT_INDEX 3u     /* 0.100 mm per press */
#define NC_MANUAL_FEED_DEFAULT_INDEX 4u     /* 500 mm/min */
static uint8_t g_nc_manual_step_index = NC_MANUAL_STEP_DEFAULT_INDEX;
static uint8_t g_nc_manual_feed_index = NC_MANUAL_FEED_DEFAULT_INDEX;

static float nc_manual_step_value(void)
{
    return g_nc_manual_steps[g_nc_manual_step_index];
}

static float nc_manual_feed_value(void)
{
    return g_nc_manual_feeds[g_nc_manual_feed_index];
}

/* Continuous feed and the stops. A stop is what keeps the axis out of the
   chuck or off the end of the travel, and each axis has two of them: side 0 is
   the minus limit, side 1 the plus one. Machine coordinates, the same figures
   the machine column shows, so an offset change cannot move a limit, and a jog
   or feed never crosses the one it is headed for. */
static bool g_nc_manual_continuous;
static bool g_nc_manual_stop_set[2][2];
static float g_nc_manual_stop[2][2];
static char g_nc_manual_spindle_dir;      /* 0 off, '3' CW, '4' CCW */
/* A jog is over as soon as it is sent, so the key that sent it is held lit for
   a moment: the pad has to show which key the machine just acted on. */
#define NC_MANUAL_FLASH_MS 250u
static char g_nc_manual_flash_key;
static uint32_t g_nc_manual_flash_ms;

/* The feed jog in flight: the key that started it, 0 when there is none. The
   controller owns the motion (a `$J=` block covering the whole distance to the
   stop), so the panel only has to notice the key coming up, Stop, an alarm or
   the wall - which is the block's own end. */
#define NC_MANUAL_FEED_MIN_MM  0.005
static char g_nc_manual_feed_key;
static char g_nc_manual_held_key;

/* The stop being typed on the pad: 0 = none, 1 = the minus field, 2 = the plus
   field. `*` accepts the field (and opens the other one), `D` puts the axis
   limit the setup states into it, `A` leaves it, and the first character typed
   replaces what the field holds - `*` is the accept key here, so a wrong value
   is retyped rather than backspaced. */
static uint8_t g_nc_manual_stop_field;
static char g_nc_manual_stop_text[16];
static bool g_nc_manual_stop_fresh;

/* A feed with no wall in front of it has nothing to end on, so it is bounded:
   the axis travel the machine states ($130), or this cap when it states none.
   A key release the panel never sees can then not run the axis to the end of
   the machine; letting go and holding again goes further. */
#define NC_MANUAL_FEED_CAP_MM  25.0

/* What the digits mean here. The panel draws this as the pad in the pane, and
   a shell beside the machine labels its keypad from the same table - the key
   meanings are stated once, where the screen that acts on them lives.

   The X pair follows the drawing, not the word: the preview puts the
   centreline along the top of the stock and the OD at the bottom
   (`nc_preview_map_x()`), so the key pointing up is the one that takes the
   tool toward the centre - the smaller diameter, X- - and the key pointing
   down is X+. Z is not mirrored (Z+ is to the right, as the drawing has it). */
static const nc_footer_item_t g_nc_manual_pad[] = {
    { '7', "CCW", NC_FOOTER_ACTION_NONE },
    { '8', "X-", NC_FOOTER_ACTION_NONE },
    { '9', "CW", NC_FOOTER_ACTION_NONE },
    { '4', "Z-", NC_FOOTER_ACTION_NONE },
    { '5', "STOP", NC_FOOTER_ACTION_NONE },
    { '6', "Z+", NC_FOOTER_ACTION_NONE },
    { '1', "FD-", NC_FOOTER_ACTION_NONE },
    { '2', "X+", NC_FOOTER_ACTION_NONE },
    { '3', "FD+", NC_FOOTER_ACTION_NONE }
};

static float nc_manual_machine_value(void)
{
    nc_runtime_state_t rt;

    nc_state_runtime(&rt);
    return g_nc_manual_axis ? rt.z : rt.x;
}

static char nc_manual_letter(void)
{
    return g_nc_manual_axis ? 'Z' : 'X';
}

static void nc_manual_status(const nc_manual_screen_t *screen, const char *what)
{
    snprintf(screen->status, screen->status_size, "%s %c", what,
             nc_manual_letter());
    *screen->dirty = true;
}

/* The panel works in axis millimetres: the readout, the stop and the room left
   are all the machine figure of the axis. A word in a program is not always the
   same length - the lathe programs X as a diameter (G7, the parser's default),
   so a programmed X of 1 mm moves the axis 0.5 mm. A jog asked for in axis
   millimetres has to be written that much larger, or every X jog would stop
   half way to what the readout calls the position. Z has no such scaling. */
static double nc_manual_program_length(uint8_t axis, double length)
{
    if (axis == AXIS_X && g7_g8_is_diameter_mode()) {
        return length * 2.0;
    }
    return length;
}

/* How far a feed may go when the stop is not in front of it. */
static double nc_manual_feed_limit(uint8_t axis)
{
    double travel = (double)g_settings.max_distance[axis];

    return (travel > 1.0) ? travel : NC_MANUAL_FEED_CAP_MM;
}

/* The axis limit the setup states: the machine frame the kinematics clamp to,
   [0, $130] for the axis. A travel of zero means the axis states none, and then
   that side has no setup limit to fall back on. */
static bool nc_manual_setup_limit(uint8_t axis, uint8_t side, float *value)
{
    float travel = g_settings.max_distance[axis];

    if (!value || !(travel > 0.0f)) {
        return false;
    }
    *value = (side == 0u) ? 0.0f : travel;
    return true;
}

/* The stop of one side as it stands: what the operator typed, or - while that
   side has none - the axis limit the setup states. An axis always has a limit,
   so there is no "no stop" state; `typed` says whether the value is the
   operator's own or the setup's (the pane draws the setup's dimmer). False only
   when the side has neither, which happens when the machine states no travel. */
static bool nc_manual_stop_value(uint8_t axis, uint8_t side, float *value, bool *typed)
{
    if (g_nc_manual_stop_set[axis][side]) {
        if (value) {
            *value = g_nc_manual_stop[axis][side];
        }
        if (typed) {
            *typed = true;
        }
        return true;
    }
    if (nc_manual_setup_limit(axis, side, value)) {
        if (typed) {
            *typed = false;
        }
        return true;
    }
    return false;
}

/* The stop the move is headed for, in the direction it travels: the plus limit
   for a positive move, the minus one for a negative one. False only when that
   side has neither a typed stop nor a stated travel, and then only the feed cap
   bounds the move. */
static bool nc_manual_stop_ahead(uint8_t axis, int direction, float *stop)
{
    uint8_t side = (direction > 0) ? 1u : 0u;

    return nc_manual_stop_value(axis, side, stop, NULL);
}

static const char *nc_manual_stop_side_name(uint8_t side)
{
    return side == 0u ? "-" : "+";
}

/* Panel blocks are refused while RUN holds the reader: a jog lands between two
   program blocks and the G90 it leaves behind would cut into the cycle. */
static bool nc_manual_send(const nc_manual_screen_t *screen, const char *line)
{
    if (nc_run_streaming()) {
        strncpy(screen->status, "Program running", screen->status_size - 1);
        *screen->dirty = true;
        return false;
    }
    /* A running jog locks the parser: anything else queued behind it is
       discarded without an answer, so the panel refuses it instead. */
    if (cnc_get_exec_state(EXEC_JOG)) {
        strncpy(screen->status, "Jog busy", screen->status_size - 1);
        *screen->dirty = true;
        return false;
    }
    if (cnc_get_exec_state(EXEC_JOG_LOCKED) || cnc_has_alarm()) {
        /* Alarm or door: the controller is not taking motion. */
        strncpy(screen->status, "Controller locked", screen->status_size - 1);
        *screen->dirty = true;
        return false;
    }
    if (!nc_run_send_line(line)) {
        strncpy(screen->status, "Send failed", screen->status_size - 1);
        *screen->dirty = true;
        return false;
    }
    return true;
}

/* A feed ends where it stands: the jog is cancelled with a controlled stop, so
   the axis keeps the position it has reached instead of running to the wall. */
void nc_manual_feed_cancel(const nc_manual_screen_t *screen)
{
    if (!g_nc_manual_feed_key) {
        return;
    }
    g_nc_manual_feed_key = 0;
    cnc_call_rt_command(CMD_CODE_JOG_CANCEL);
    strncpy(screen->status, "Feed stop", screen->status_size - 1);
    *screen->dirty = true;
}

/* A held direction key feeds. Toward the stop the block is the whole distance
   to the wall, so the jog ends on it by itself; away from it (or with no stop
   set) the feed is a bounded move, because nothing is there to end it. Letting
   the key go cancels either one where it stands. The distances are measured
   from a standing axis, so none of it depends on a position read taken while
   the machine is moving. */
static void nc_manual_feed(const nc_manual_screen_t *screen, char key, int direction)
{
    uint8_t axis = g_nc_manual_axis;
    float stop = 0.0f;
    bool to_stop = false;
    double move;
    char line[48];

    /* The parser takes a jog only in IDLE, and the distance to the wall is
       measured from a position: an axis that is still moving has neither. */
    if (!nc_manual_axis_standing(screen, "Wait for stop")) {
        return;
    }
    if (nc_manual_stop_ahead(axis, direction, &stop)) {
        double room = ((double)stop - (double)nc_manual_machine_value()) *
                      (double)direction;

        if (room < NC_MANUAL_FEED_MIN_MM) {
            snprintf(screen->status, screen->status_size, "At %c%s stop",
                     nc_manual_letter(), nc_manual_stop_side_name(direction > 0));
            *screen->dirty = true;
            return;
        }
        /* The block covers the whole distance to the stop, so the feed ends on
           it by itself. */
        to_stop = true;
        move = (double)stop - (double)nc_manual_machine_value();
    } else {
        move = nc_manual_feed_limit(axis) * (double)direction;
    }
    /* `$J=` is the controller's jog: it runs the block, and a jog cancel from
       the key release stops it without touching the modal state. */
    snprintf(line, sizeof(line), "$J=G91 %c%.3f F%.0f",
             nc_manual_letter(),
             nc_manual_program_length(axis, move),
             (double)nc_manual_feed_value());
    if (!nc_manual_send(screen, line)) {
        return;
    }
    g_nc_manual_feed_key = key;
    if (to_stop) {
        snprintf(screen->status, screen->status_size, "Feed to %.3f",
                 (double)stop);
    } else {
        snprintf(screen->status, screen->status_size, "Feed %.3f mm",
                 move < 0.0 ? -move : move);
    }
    *screen->dirty = true;
}

static void nc_manual_jog(const nc_manual_screen_t *screen, char key, int direction)
{
    char line[48];
    char sent[48];
    double step = (double)nc_manual_step_value();
    uint8_t axis = g_nc_manual_axis;

    if (g_nc_manual_continuous) {
        nc_manual_feed(screen, key, direction);
        return;
    }
    nc_manual_feed_cancel(screen);     /* step mode never leaves a feed running */
    if (!nc_manual_axis_standing(screen, "Jog busy")) {
        return;
    }
    /* A stop is a wall the axis may not cross: a step into it is shortened to
       end on it, and a step from on it into the same side is refused. The other
       side has its own stop, so standing on one is never a lock. */
    {
        float stop = 0.0f;

        if (nc_manual_stop_ahead(axis, direction, &stop)) {
            double room = ((double)stop - (double)nc_manual_machine_value()) *
                          (double)direction;

            step = MIN(step, room);
            if (step < 0.0005) {
                snprintf(screen->status, screen->status_size, "At %c%s stop",
                         nc_manual_letter(),
                         nc_manual_stop_side_name(direction > 0));
                *screen->dirty = true;
                return;
            }
        }
    }
    /* Incremental, and back to absolute in the same breath: a jog must not
       leave the machine in G91 for whatever runs next. Both blocks are queued,
       so the pairing survives the trip through the reader. */
    snprintf(line, sizeof(line), "G91 G1 %c%.3f F%.0f",
             nc_manual_letter(),
             nc_manual_program_length(axis, step * (double)direction),
             (double)nc_manual_feed_value());
    if (!nc_manual_send(screen, line)) {
        return;
    }
    snprintf(sent, sizeof(sent), "%s", line);
    nc_manual_send(screen, "G90");
    snprintf(screen->status, screen->status_size, "%s", sent);
    *screen->dirty = true;
}

/* '#' swaps a fixed step for feeding to the stop while the key is held. */
static void nc_manual_step_toggle(const nc_manual_screen_t *screen)
{
    nc_manual_feed_cancel(screen);
    g_nc_manual_continuous = !g_nc_manual_continuous;
    strncpy(screen->status,
            g_nc_manual_continuous ? "Continuous feed" : "Step jog",
            screen->status_size - 1);
    *screen->dirty = true;
}

/* '1' and '3' change the value the feed mode is using: the step per press, or
   the feed the continuous jog runs at. Both ends of each table are the end of
   the range - the key is not a mode, it just stops. */
static void nc_manual_value_adjust(const nc_manual_screen_t *screen, int direction)
{
    if (g_nc_manual_continuous) {
        if (direction < 0) {
            if (g_nc_manual_feed_index > 0u) {
                g_nc_manual_feed_index--;
            }
        } else if (g_nc_manual_feed_index + 1u <
                   sizeof(g_nc_manual_feeds) / sizeof(g_nc_manual_feeds[0])) {
            g_nc_manual_feed_index++;
        }
        snprintf(screen->status, screen->status_size, "Feed %.0f mm/min",
                 (double)nc_manual_feed_value());
    } else {
        if (direction < 0) {
            if (g_nc_manual_step_index > 0u) {
                g_nc_manual_step_index--;
            }
        } else if (g_nc_manual_step_index + 1u <
                   sizeof(g_nc_manual_steps) / sizeof(g_nc_manual_steps[0])) {
            g_nc_manual_step_index++;
        }
        snprintf(screen->status, screen->status_size, "Step %.3f mm",
                 (double)nc_manual_step_value());
    }
    *screen->dirty = true;
}

/* '*' arms (or clears) the stop position for the picked axis, here. */
/* --- the stops, typed on the pad -----------------------------------------

   `*` opens the minus stop of the picked axis; `*` again takes what the field
   holds and opens the plus stop; `*` once more takes that and closes. An empty
   field takes the axis limit the setup states, so three presses leave the axis
   with the widest travel the machine has and the operator only ever narrows it.
   `D` puts that limit into the field; `A` leaves the field without changing
   anything. */
static void nc_manual_stop_field_status(const nc_manual_screen_t *screen)
{
    uint8_t axis = g_nc_manual_axis;
    uint8_t side = (uint8_t)(g_nc_manual_stop_field - 1u);
    char limit[24];
    float setup = 0.0f;

    if (nc_manual_setup_limit(axis, side, &setup)) {
        snprintf(limit, sizeof(limit), "D=%.3f", (double)setup);
    } else {
        snprintf(limit, sizeof(limit), "%s", "no limit");
    }
    snprintf(screen->status, screen->status_size, "%c%s stop %s *OK A",
             nc_manual_letter(), nc_manual_stop_side_name(side), limit);
    *screen->dirty = true;
}

static void nc_manual_stop_field_open(const nc_manual_screen_t *screen, uint8_t field)
{
    g_nc_manual_stop_field = field;
    g_nc_manual_stop_text[0] = '\0';
    g_nc_manual_stop_fresh = true;
    nc_manual_stop_field_status(screen);
}

/* Take what the field holds as the stop of that side - or the axis limit the
   setup states when nothing was typed. */
static void nc_manual_stop_field_take(const nc_manual_screen_t *screen)
{
    uint8_t axis = g_nc_manual_axis;
    uint8_t side = (uint8_t)(g_nc_manual_stop_field - 1u);
    float value = 0.0f;

    if (g_nc_manual_stop_text[0]) {
        value = strtof(g_nc_manual_stop_text, NULL);
    } else if (!nc_manual_setup_limit(axis, side, &value)) {
        /* Nothing typed and nothing in the setup: leave this side as it was. */
        snprintf(screen->status, screen->status_size, "%c%s stop unchanged",
                 nc_manual_letter(), nc_manual_stop_side_name(side));
        *screen->dirty = true;
        return;
    }
    g_nc_manual_stop[axis][side] = value;
    g_nc_manual_stop_set[axis][side] = true;
    snprintf(screen->status, screen->status_size, "%c%s stop %.3f",
             nc_manual_letter(), nc_manual_stop_side_name(side), (double)value);
}

static void nc_manual_zero(const nc_manual_screen_t *screen)
{
    char line[48];

    snprintf(line, sizeof(line), "G10 L20 P0 %c0", nc_manual_letter());
    nc_manual_send(screen, line);
    nc_manual_status(screen, "Zero");
}

static void nc_manual_touch_apply(const nc_manual_screen_t *screen)
{
    char line[48];

    if (!g_nc_manual_touch[0]) {
        return;
    }
    snprintf(line, sizeof(line), "G10 L20 P0 %c%s",
             nc_manual_letter(), g_nc_manual_touch);
    if (!nc_manual_send(screen, line)) {
        return;
    }
    snprintf(screen->status, screen->status_size, "Touch %c=%s",
             nc_manual_letter(), g_nc_manual_touch);
    g_nc_manual_touch_active = false;
    g_nc_manual_touch[0] = '\0';
    *screen->dirty = true;
}

static void nc_manual_spindle(const nc_manual_screen_t *screen, char code)
{
    char line[24];
    unsigned rpm;

    if (code == 'S') {
        nc_manual_send(screen, "M5");
        g_nc_manual_spindle_dir = 0;
        strncpy(screen->status, "Spindle stop", screen->status_size - 1);
    } else {
        /* Start at the speed the machine already has - the modal S a program or
           an earlier command left - so the operator's speed is what comes back.
           A spindle that states none leaves the last speed this panel used
           standing, which is remembered with the rest of the state. */
        uint8_t modes[16];
        uint16_t feed = 0u;
        uint16_t modal = 0u;

        parser_get_modes(modes, &feed, &modal);
        if (modal >= 1u) {
            nc_state_remember_manual_spindle((unsigned)modal);
        }
        rpm = nc_state_manual_spindle();
        g_nc_manual_spindle_dir = code;
        snprintf(line, sizeof(line), "M%c S%u", code, rpm);
        nc_manual_send(screen, line);
        snprintf(screen->status, screen->status_size,
                 "Spindle %s S%u", code == '3' ? "CW" : "CCW",
                 rpm);
    }
    *screen->dirty = true;
}

static bool nc_manual_handle_digit(const nc_manual_screen_t *screen, char ch)
{
    bool handled;

    switch (ch) {
    /* The jog keys name their axis, so a jog never lands on the other one by
       accident: 2/8 are X, 4/6 are Z, and the key picks the axis it moves. The
       sign follows the pad's own drawing (see the table above): the key that
       points up takes X down. */
    case '2': g_nc_manual_axis = 0; nc_manual_jog(screen, '2', +1); handled = true; break;
    case '8': g_nc_manual_axis = 0; nc_manual_jog(screen, '8', -1); handled = true; break;
    case '4': g_nc_manual_axis = 1; nc_manual_jog(screen, '4', -1); handled = true; break;
    case '6': g_nc_manual_axis = 1; nc_manual_jog(screen, '6', +1); handled = true; break;
    case '5': nc_manual_spindle(screen, 'S'); handled = true; break;
    case '7': nc_manual_spindle(screen, '4'); handled = true; break;   /* CCW */
    case '9': nc_manual_spindle(screen, '3'); handled = true; break;   /* CW */
    /* The value keys: what they change is the value the pane shows beside them. */
    case '1': nc_manual_value_adjust(screen, -1); handled = true; break;
    case '3': nc_manual_value_adjust(screen, +1); handled = true; break;
    /* '0' is left to the footer's ZERO slot, so the key has one meaning. */
    default: handled = false; break;
    }
    if (handled) {
        g_nc_manual_flash_key = ch;
        g_nc_manual_flash_ms = mcu_millis();
        *screen->dirty = true;
    }
    return handled;
}

/* --- what the screen calls ------------------------------------------------ */

/* A key on this screen. `ch` is the character the screen read the key from
   (nc_visual_key_char()), so the keypad's own table stays in one place; `key`
   is there for the two keys that are not a character on this screen - CANCEL
   and MODE both leave the touch field. */
bool nc_manual_key(nc_visual_key_t key, char ch, const nc_manual_screen_t *screen)
{
    if (!screen || !screen->status || !screen->dirty) {
        return false;
    }
    if (g_nc_manual_stop_field) {
        uint8_t side = (uint8_t)(g_nc_manual_stop_field - 1u);
        size_t len = strlen(g_nc_manual_stop_text);

        /* A stop: type the value, `*` (or `#`) takes it and moves on to the
           other side, `D` puts the axis limit the setup states in the field,
           `A` leaves it alone. The first character typed replaces what the
           field holds, so a wrong value is retyped - `*` is the accept key. */
        if (key == NC_VISUAL_KEY_BACKSPACE || key == NC_VISUAL_KEY_FINISH) {
            nc_manual_stop_field_take(screen);
            if (g_nc_manual_stop_field == 1u) {
                nc_manual_stop_field_open(screen, 2u);
            } else {
                g_nc_manual_stop_field = 0;
                g_nc_manual_stop_text[0] = '\0';
                *screen->dirty = true;
            }
            return true;
        }
        if (key == NC_VISUAL_KEY_ACCEPT) {
            float setup = 0.0f;

            if (nc_manual_setup_limit(g_nc_manual_axis, side, &setup)) {
                snprintf(g_nc_manual_stop_text, sizeof(g_nc_manual_stop_text),
                         "%.3f", (double)setup);
                g_nc_manual_stop_fresh = false;
            } else {
                strncpy(screen->status, "No axis limit in the setup",
                        screen->status_size - 1);
            }
            nc_manual_stop_field_status(screen);
            return true;
        }
        if (key == NC_VISUAL_KEY_CANCEL || key == NC_VISUAL_KEY_MODE) {
            g_nc_manual_stop_field = 0;
            g_nc_manual_stop_text[0] = '\0';
            strncpy(screen->status, "Stop unchanged", screen->status_size - 1);
            *screen->dirty = true;
            return true;
        }
        if (ch >= '0' && ch <= '9') {
            if (g_nc_manual_stop_fresh) {
                g_nc_manual_stop_text[0] = '\0';
                g_nc_manual_stop_fresh = false;
                len = 0u;
            }
            if (len + 1u < sizeof(g_nc_manual_stop_text)) {
                g_nc_manual_stop_text[len] = ch;
                g_nc_manual_stop_text[len + 1u] = '\0';
            }
            nc_manual_stop_field_status(screen);
            return true;
        }
        /* The sign and the point: the keypad has no minus key, so `B` and `C`
           are what the editor's field entry uses for them as well. */
        if (key == NC_VISUAL_KEY_FIELD_PREV || ch == '-') {
            if (g_nc_manual_stop_text[0] == '-') {
                memmove(g_nc_manual_stop_text, g_nc_manual_stop_text + 1, len);
            } else if (len + 1u < sizeof(g_nc_manual_stop_text)) {
                memmove(g_nc_manual_stop_text + 1, g_nc_manual_stop_text, len + 1u);
                g_nc_manual_stop_text[0] = '-';
            }
            g_nc_manual_stop_fresh = false;
            nc_manual_stop_field_status(screen);
            return true;
        }
        if (key == NC_VISUAL_KEY_FIELD_NEXT || ch == '.') {
            if (!strchr(g_nc_manual_stop_text, '.') &&
                len + 2u < sizeof(g_nc_manual_stop_text)) {
                if (g_nc_manual_stop_fresh || len == 0u) {
                    snprintf(g_nc_manual_stop_text, sizeof(g_nc_manual_stop_text),
                             "%s0.", g_nc_manual_stop_text[0] == '-' ? "-" : "");
                } else {
                    g_nc_manual_stop_text[len] = '.';
                    g_nc_manual_stop_text[len + 1u] = '\0';
                }
                g_nc_manual_stop_fresh = false;
            }
            nc_manual_stop_field_status(screen);
            return true;
        }
        /* The field is open: the rest of the keys belong to it. */
        return true;
    }
    if (g_nc_manual_touch_active) {
        size_t len = strlen(g_nc_manual_touch);

        /* Touch-off: type the value, D (or Enter) applies it. */
        if (key == NC_VISUAL_KEY_ACCEPT || key == NC_VISUAL_KEY_FINISH) {
            nc_manual_touch_apply(screen);
            return true;
        }
        if (key == NC_VISUAL_KEY_CANCEL || key == NC_VISUAL_KEY_MODE) {
            g_nc_manual_touch_active = false;
            g_nc_manual_touch[0] = '\0';
            *screen->dirty = true;
            return true;
        }
        if (ch >= '0' && ch <= '9' && len + 1u < sizeof(g_nc_manual_touch)) {
            g_nc_manual_touch[len] = ch;
            g_nc_manual_touch[len + 1u] = '\0';
            *screen->dirty = true;
            return true;
        }
        if ((ch == '-' || ch == '.') && len == 0u) {
            g_nc_manual_touch[len] = ch;
            g_nc_manual_touch[len + 1u] = '\0';
            *screen->dirty = true;
            return true;
        }
        if (key == NC_VISUAL_KEY_BACKSPACE) {
            if (len > 0u) {
                g_nc_manual_touch[len - 1u] = '\0';
            }
            *screen->dirty = true;
        }
        /* The field is open: the rest of the keys belong to it, not to a screen
           the operator cannot see them act on. */
        return true;
    }
    if (ch && nc_manual_handle_digit(screen, ch)) {
        return true;
    }
    if (key == NC_VISUAL_KEY_FINISH) {          /* '#' */
        nc_manual_step_toggle(screen);
        return true;
    }
    if (key == NC_VISUAL_KEY_BACKSPACE) {       /* '*' */
        /* Stop stops the feed; an idle axis opens the stops for entry - the
           minus limit first, then `*` again takes it and opens the plus one. */
        if (g_nc_manual_feed_key) {
            nc_manual_feed_cancel(screen);
            return true;
        }
        nc_manual_stop_field_open(screen, 1u);
        return true;
    }
    /* The axis keys. `B`/`C` are the footer's AXIS-/AXIS+ and reach this screen
       as the field-step keys, because the keypad has no character of its own for
       them - and with a code screen behind the same keys the editor used to take
       them first, so the axis could not be picked at all. The shell's arrows are
       offered here too: on this screen they have nothing else to move. */
    if (key == NC_VISUAL_KEY_FIELD_PREV || key == NC_VISUAL_KEY_WORD_PREV ||
        key == NC_VISUAL_KEY_FIELD_NEXT || key == NC_VISUAL_KEY_WORD_NEXT) {
        g_nc_manual_axis = (uint8_t)((g_nc_manual_axis + 1u) % 2u);
        nc_manual_status(screen, "Axis");
        return true;
    }
    return false;
}

bool nc_manual_action(uint8_t action, const nc_manual_screen_t *screen)
{
    if (!screen || !screen->status || !screen->dirty) {
        return false;
    }
    switch (action) {
    case NC_FOOTER_ACTION_AXIS_PREV:
    case NC_FOOTER_ACTION_AXIS_NEXT:
        g_nc_manual_axis = (uint8_t)((g_nc_manual_axis + 1u) % 2u);
        nc_manual_status(screen, "Axis");
        return true;
    case NC_FOOTER_ACTION_ZERO:
        nc_manual_zero(screen);
        return true;
    case NC_FOOTER_ACTION_TOUCH:
        g_nc_manual_touch_active = true;
        g_nc_manual_touch[0] = '\0';
        snprintf(screen->status, screen->status_size, "Touch %c: value then D",
                 nc_manual_letter());
        return true;
    default:
        return false;
    }
}

void nc_manual_hold(char key, const nc_manual_screen_t *screen)
{
    bool changed;

    if (!screen || !screen->status || !screen->dirty) {
        return;
    }
    changed = key != g_nc_manual_held_key;
    g_nc_manual_held_key = key;
    if (!g_nc_manual_feed_key) {
        return;                             /* nothing is feeding */
    }
    /* An alarm or a program taking the reader stops the feed here as well as
       in the controller: the panel must not believe a jog is still running. */
    if (cnc_has_alarm() || nc_run_streaming()) {
        nc_manual_feed_cancel(screen);
        return;
    }
    /* The key that started the feed is the only key that keeps it running.
       Letting go, or reaching for another key, ends it where it is. */
    if (changed && key != g_nc_manual_feed_key) {
        nc_manual_feed_cancel(screen);
    }
}

const char *nc_manual_key_hint(char key)
{
    size_t i;

    for (i = 0; i < sizeof(g_nc_manual_pad) / sizeof(g_nc_manual_pad[0]); i++) {
        if (g_nc_manual_pad[i].key == key) {
            return g_nc_manual_pad[i].label;
        }
    }
    return 0;
}

bool nc_manual_stop_field_active(void)
{
    return g_nc_manual_stop_field != 0u;
}

void nc_manual_draw(const nc_manual_view_t *view)
{
    /* MANUAL is the readout the jog keys work on. The work position, the
       machine figures and F/S are in the header DRO, as on every screen; the
       pane holds what is MANUAL's own - one line per axis with the position and
       its two stops, the STEP/FEED values the value keys change, and the pad.
       The value being typed is drawn in the cell it belongs to (see the axis
       loop), so there is no field line of its own. */
    int i;

    lvds_draw_fill_rect(0, NC_PANE_Y, LVDS_HSTX_WIDTH, NC_PANE_H, NC_VISUAL_BG);
    {
        /* Column captions, over the columns that are not self-describing. The
           work position, the machine figures and F/S are in the header DRO on
           this screen as on every other, so the pane belongs to the stops - the
           values the jog keys guard - and to the jog values themselves. */
        nc_draw_text_clip(NC_MANUAL_COL_STOP, NC_PANE_Y + 2,
                                 "-STOP", 5, NC_VISUAL_DIM, NC_VISUAL_BG,
                                 LVDS_FONT_SMALL);
        nc_draw_text_clip(NC_MANUAL_COL_STOP_PLUS, NC_PANE_Y + 2,
                                 "+STOP", 5, NC_VISUAL_DIM, NC_VISUAL_BG,
                                 LVDS_FONT_SMALL);
    }
    for (i = 0; i < 2; i++) {
        int y = NC_PANE_Y + 30 + i * NC_MANUAL_ROW_H;
        char value[24];
        char limit[2][24];
        bool stop_set[2] = { false, false };
        const int stop_x[2] = { NC_MANUAL_COL_STOP, NC_MANUAL_COL_STOP_PLUS };
        int side;
        bool picked = (int)g_nc_manual_axis == i;
        bool touch_here = picked && g_nc_manual_touch_active;
        bool typing = picked && g_nc_manual_stop_field != 0u;
        lvds_color_t dim = picked ? NC_VISUAL_LINE_NO_SELECTED : NC_VISUAL_DIM;
        lvds_color_t bg = picked ? NC_VISUAL_SELECT : NC_VISUAL_BG;
        float offset = view->have_wco ? (i == 0 ? view->wco_x : view->wco_z) : 0.0f;

        /* The pane is the *wanted* state - what the operator is setting up -
           while the header DRO above is the current one, so the position is not
           repeated here. The one value that does appear on this row is the one
           being typed: a touch-off writes a work position, so its field sits
           where that position would be, in the editor's word colours with a
           cursor, exactly as the editor shows the word its digits go into. */
        if (touch_here) {
            snprintf(value, sizeof(value), "%s_",
                     g_nc_manual_touch[0] ? g_nc_manual_touch : "");
        } else {
            value[0] = '\0';
        }
        for (side = 0; side < 2; side++) {
            limit[side][0] = '\0';
            if (typing && (uint8_t)side == g_nc_manual_stop_field - 1u) {
                /* The stop being typed. Empty means the axis limit the setup
                   states, which is what `*` or `D` will take. */
                if (g_nc_manual_stop_text[0]) {
                    snprintf(limit[side], sizeof(limit[side]), "%s_",
                             g_nc_manual_stop_text);
                } else {
                    float setup = 0.0f;

                    if (nc_manual_setup_limit((uint8_t)i, (uint8_t)side, &setup)) {
                        snprintf(limit[side], sizeof(limit[side]), "%.3f",
                                 (double)setup);
                    } else {
                        /* Nothing typed and nothing in the setup: the cursor is
                           all there is to show, and the status line says why. */
                        snprintf(limit[side], sizeof(limit[side]), "%s", "_");
                    }
                }
                continue;
            }
            {
                float stop = 0.0f;
                bool typed = false;

                /* The stop of this side: what was typed, or the axis limit the
                   setup states while nothing was. Without either - a machine
                   that states no travel - there is nothing to show. */
                if (!nc_manual_stop_value((uint8_t)i, (uint8_t)side, &stop, &typed)) {
                    snprintf(limit[side], sizeof(limit[side]), "%8s", "--");
                    continue;
                }
                snprintf(limit[side], sizeof(limit[side]), "%8.3f",
                         (double)stop - (double)offset);
                stop_set[side] = typed;
            }
        }
        if (picked) {
            /* The line the keys work on is marked across the values it owns,
               not the whole pane. */
            lvds_draw_fill_rect(16, y - 8, NC_MANUAL_MARK_RIGHT - 16,
                                NC_MANUAL_ROW_H - 20, NC_VISUAL_SELECT);
        }
        /* The row is the axis's limits, not its position - the position is the
           header DRO's - so the label says so. It is written in the same font as
           the values beside it: this row is one table, and a label set larger
           than its own numbers reads as a different field. */
        lvds_draw_text(NC_MANUAL_COL_X, y + NC_MANUAL_TEXT_DY,
                       i == 0 ? "X LIMIT" : "Z LIMIT", dim, bg,
                       LVDS_FONT_NORMAL);
        if (touch_here) {
            lvds_draw_fill_rect(NC_MANUAL_COL_TOUCH - 2,
                                y + NC_MANUAL_TEXT_DY - 2,
                                NC_MANUAL_TOUCH_W + 4,
                                NC_VISUAL_ROW_H - 20,
                                NC_VISUAL_WORD_BG);
            nc_draw_text_clip(NC_MANUAL_COL_TOUCH, y + NC_MANUAL_TEXT_DY, value,
                              9, NC_VISUAL_WORD_FG, NC_VISUAL_WORD_BG,
                              LVDS_FONT_NORMAL);
        }
        for (side = 0; side < 2; side++) {
            /* One line per stop, the value right-aligned in its cell so both
               sides - and the STEP/FEED values below - share a right edge. The
               distance to go is not shown: the DRO has the position, and what
               the stop does is stop the move. A stop the operator typed is
               written in the accent colour; the axis limit the setup states -
               the default while that side has no stop - is dimmer, so it is
               clear which numbers are this job's. */
            if (typing && (uint8_t)side == g_nc_manual_stop_field - 1u) {
                lvds_draw_fill_rect(stop_x[side] - 2, y + NC_MANUAL_TEXT_DY - 2,
                                    9 * NC_VISUAL_CHAR_W, NC_VISUAL_ROW_H - 20,
                                    NC_VISUAL_WORD_BG);
                nc_draw_text_clip(stop_x[side], y + NC_MANUAL_TEXT_DY, limit[side], 9,
                                         NC_VISUAL_WORD_FG, NC_VISUAL_WORD_BG,
                                         LVDS_FONT_NORMAL);
                continue;
            }
            nc_draw_text_clip(stop_x[side], y + NC_MANUAL_TEXT_DY, limit[side], 9,
                                     picked ? NC_VISUAL_LINE_NO_SELECTED
                                            : (stop_set[side] ? NC_VISUAL_ACCENT
                                                              : NC_VISUAL_DIM),
                                     bg, LVDS_FONT_NORMAL);
        }
    }
    {
        /* The two values the jog keys use, lined up with the values above them:
           the name in the axis column, the number right-aligned in the stop
           column (so every number in the pane shares a right edge) with its
           unit after it, and the row in use marked the way the picked axis is -
           `1` and `3` change the marked one, so what they change is on the
           screen at the moment they are pressed. */
        char line[24];
        int row;

        for (row = 0; row < 2; row++) {
            bool continuous = (row == 1);
            bool active = (continuous == g_nc_manual_continuous);
            int y = NC_PANE_Y + 30 + (2 + row) * NC_MANUAL_ROW_H;
            lvds_color_t fg = active ? NC_VISUAL_LINE_NO_SELECTED : NC_VISUAL_DIM;
            lvds_color_t bg = active ? NC_VISUAL_SELECT : NC_VISUAL_BG;

            if (active) {
                lvds_draw_fill_rect(16, y - 8, NC_MANUAL_MARK_RIGHT - 16,
                                    NC_MANUAL_ROW_H - 20, NC_VISUAL_SELECT);
            }
            lvds_draw_text(NC_MANUAL_COL_X, y + NC_MANUAL_TEXT_DY,
                           continuous ? "FEED" : "STEP",
                           fg, bg, LVDS_FONT_NORMAL);
            if (continuous) {
                snprintf(line, sizeof(line), "%8.0f mm/min",
                         (double)nc_manual_feed_value());
            } else {
                snprintf(line, sizeof(line), "%8.3f mm",
                         (double)nc_manual_step_value());
            }
            nc_draw_text_clip(NC_MANUAL_COL_STOP, y + NC_MANUAL_TEXT_DY, line, 16, fg, bg,
                                     LVDS_FONT_NORMAL);
        }
    }
    {
        /* The pad drawn with the same key design the helpers use, so the
           3x3 reads the same everywhere - it just means different things
           here (g_nc_manual_pad). */
        /* Same corner the floating helper uses: right side of the pane,
           sat on the bottom of it and clear of the footer. */
        uint16_t lit = 0u;

        /* The keys that are doing something right now: the spindle
           direction, the key that has a feed running, or - with feed armed
           and nothing feeding - the jog keys of the picked axis. */
        if (g_nc_manual_spindle_dir == '3') {
            lit |= 1u << 9;
        } else if (g_nc_manual_spindle_dir == '4') {
            lit |= 1u << 7;
        }
        if (g_nc_manual_feed_key >= '1' &&
            g_nc_manual_feed_key <= '9') {
            lit |= 1u << (g_nc_manual_feed_key - '0');
        } else if (g_nc_manual_continuous) {
            lit |= (g_nc_manual_axis ? ((1u << 4) | (1u << 6))
                                            : ((1u << 2) | (1u << 8)));
        }
        /* A step jog is over before the next frame, so the key that sent it
           stays lit for a moment: the pad answers the press. */
        if (g_nc_manual_flash_key >= '1' &&
            g_nc_manual_flash_key <= '9' &&
            (uint32_t)(mcu_millis() - g_nc_manual_flash_ms) <
                NC_MANUAL_FLASH_MS) {
            lit |= 1u << (g_nc_manual_flash_key - '0');
        }
        nc_draw_modal_items(NC_RIGHT_PANE_X + NC_RIGHT_PANE_W - NC_MODAL_W - 8,
                                   NC_PANE_BOTTOM - NC_MODAL_H - 6,
                                   g_nc_manual_pad,
                                   sizeof(g_nc_manual_pad) /
                                       sizeof(g_nc_manual_pad[0]),
                                   lit);
    }
}
