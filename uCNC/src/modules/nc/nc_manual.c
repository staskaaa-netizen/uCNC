/* MANUAL: the readout the jog keys work on - see nc_manual.h. It keeps the
   state only it uses and is handed the two things it may write outside itself:
   the screen's status line and repaint flag. */
#include "nc_manual.h"

#include <stdio.h>
#include <string.h>

#include "../../cnc.h"
#include "nc_draw.h"
#include "nc_layout.h"
#include "nc_menu.h"
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

#define NC_MANUAL_SPINDLE_RPM  1000u

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

/* Continuous feed and the stop position. The stop is what keeps the axis out
   of the chuck: it is set at the current point of the picked axis and a jog
   never crosses it. Machine coordinates, the same figures the machine column
   shows, so an offset change cannot move the limit. */
static bool g_nc_manual_continuous;
static bool g_nc_manual_stop_set[2];
static float g_nc_manual_stop[2];
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
/* Which side of the stop the axis works on: +1 above it, -1 below, 0 while it
   is not known yet (the stop was just armed at the axis). */
static int8_t g_nc_manual_stop_side[2];

/* A feed with no wall in front of it has nothing to end on, so it is bounded:
   the axis travel the machine states ($130), or this cap when it states none.
   A key release the panel never sees can then not run the axis to the end of
   the machine; letting go and holding again goes further. */
#define NC_MANUAL_FEED_CAP_MM  25.0

/* What the digits mean here. The panel draws this as the pad in the pane, and
   a shell beside the machine labels its keypad from the same table - the key
   meanings are stated once, where the screen that acts on them lives. */
static const nc_footer_item_t g_nc_manual_pad[] = {
    { '7', "CCW", NC_FOOTER_ACTION_NONE },
    { '8', "X+", NC_FOOTER_ACTION_NONE },
    { '9', "CW", NC_FOOTER_ACTION_NONE },
    { '4', "Z-", NC_FOOTER_ACTION_NONE },
    { '5', "STOP", NC_FOOTER_ACTION_NONE },
    { '6', "Z+", NC_FOOTER_ACTION_NONE },
    { '1', "FD-", NC_FOOTER_ACTION_NONE },
    { '2', "X-", NC_FOOTER_ACTION_NONE },
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

/* Is this move headed into the wall? The wall is a statement about the *move*,
   not about the sign of the distance: standing exactly on the stop leaves the
   operator free to go either way, and the way they go is the side they work
   on. Remember it, so the panel only guards the direction that would cross -
   and so a step into the wall from on it is refused instead of walking through
   it, while the axis is never trapped on the stop. `dist` is stop - here. */
static bool nc_manual_toward_stop(uint8_t axis, double dist, int direction)
{
    int8_t side = g_nc_manual_stop_side[axis];

    if (dist > NC_MANUAL_FEED_MIN_MM) {
        side = -1;                      /* the axis is below the stop */
    } else if (dist < -NC_MANUAL_FEED_MIN_MM) {
        side = 1;                       /* above it */
    }
    g_nc_manual_stop_side[axis] = side;
    return side != 0 && direction == -side;
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
    bool to_stop = false;
    double move;
    char line[48];

    /* The parser takes a jog only in IDLE, and the distance to the wall is
       measured from a position: an axis that is still moving has neither. */
    if (!nc_manual_axis_standing(screen, "Wait for stop")) {
        return;
    }
    if (g_nc_manual_stop_set[axis]) {
        double dist = (double)g_nc_manual_stop[axis] -
                      (double)nc_manual_machine_value();

        to_stop = nc_manual_toward_stop(axis, dist, direction);
        if (to_stop) {
            if (dist * (double)direction < NC_MANUAL_FEED_MIN_MM) {
                strncpy(screen->status, "At stop", screen->status_size - 1);
                *screen->dirty = true;
                return;
            }
            move = dist;                /* the block ends on the wall */
        } else {
            move = nc_manual_feed_limit(axis) * (double)direction;
        }
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
                 (double)g_nc_manual_stop[axis]);
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
    /* The stop is a wall the axis may not cross: a step into it is shortened to
       end on it. A step away is free, and standing exactly on the wall is not a
       lock - '*' at the current point is the normal way to set it, so the axis
       has to be able to leave. The first way it leaves is the side it works on
       (see nc_manual_toward_stop). */
    if (g_nc_manual_stop_set[axis]) {
        double here = (double)nc_manual_machine_value();
        double dist = (double)g_nc_manual_stop[axis] - here;

        if (nc_manual_toward_stop(axis, dist, direction)) {
            double room = dist * (double)direction;

            step = MIN(step, room);
            if (step < 0.0005) {
                strncpy(screen->status, "At stop", screen->status_size - 1);
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
static void nc_manual_stop_toggle(const nc_manual_screen_t *screen)
{
    uint8_t axis = g_nc_manual_axis;

    if (g_nc_manual_stop_set[axis]) {
        g_nc_manual_stop_set[axis] = false;
        g_nc_manual_stop_side[axis] = 0;
        strncpy(screen->status, "Stop cleared", screen->status_size - 1);
    } else {
        g_nc_manual_stop[axis] = nc_manual_machine_value();
        g_nc_manual_stop_set[axis] = true;
        g_nc_manual_stop_side[axis] = 0;     /* the side is not known yet */
        snprintf(screen->status, screen->status_size, "Stop %c at %.3f",
                 nc_manual_letter(), (double)g_nc_manual_stop[axis]);
    }
    *screen->dirty = true;
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

    if (code == 'S') {
        nc_manual_send(screen, "M5");
        g_nc_manual_spindle_dir = 0;
        strncpy(screen->status, "Spindle stop", screen->status_size - 1);
    } else {
        g_nc_manual_spindle_dir = code;
        snprintf(line, sizeof(line), "M%c S%u", code, NC_MANUAL_SPINDLE_RPM);
        nc_manual_send(screen, line);
        snprintf(screen->status, screen->status_size,
                 "Spindle %s S%u", code == '3' ? "CW" : "CCW",
                 (unsigned)NC_MANUAL_SPINDLE_RPM);
    }
    *screen->dirty = true;
}

static bool nc_manual_handle_digit(const nc_manual_screen_t *screen, char ch)
{
    bool handled;

    switch (ch) {
    /* The jog keys name their axis, so a jog never lands on the other one by
       accident: 2/8 are X, 4/6 are Z, and the key picks the axis it moves. */
    case '2': g_nc_manual_axis = 0; nc_manual_jog(screen, '2', -1); handled = true; break;
    case '8': g_nc_manual_axis = 0; nc_manual_jog(screen, '8', +1); handled = true; break;
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
        /* Stop stops the feed; only an idle axis arms or clears the wall. */
        if (g_nc_manual_feed_key) {
            nc_manual_feed_cancel(screen);
            return true;
        }
        nc_manual_stop_toggle(screen);
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

void nc_manual_draw(const nc_manual_view_t *view)
{
    /* MANUAL is the readout the jog keys work on. Every axis keeps its
       three numbers on one line - where the axis is in the offset in use,
       the stop the feed must not cross, and the machine figure the offset
       is cut from - so nothing has to be looked up on another screen. */
    static const char *const letters[4] = { "X", "Z", "F", "S" };
    const nc_runtime_state_t *runtime = view->runtime;
    int i;

    lvds_draw_fill_rect(0, NC_PANE_Y, LVDS_HSTX_WIDTH, NC_PANE_H, NC_VISUAL_BG);
    {
        /* Column captions, over the columns that are not self-describing. */
        char offset[16];

        nc_draw_text_clip(NC_MANUAL_COL_STOP, NC_PANE_Y + 2,
                                 "STOP", 4, NC_VISUAL_DIM, NC_VISUAL_BG,
                                 LVDS_FONT_SMALL);
        nc_draw_text_clip(NC_MANUAL_COL_MACH, NC_PANE_Y + 2,
                                 "MACHINE", 7, NC_VISUAL_DIM, NC_VISUAL_BG,
                                 LVDS_FONT_SMALL);
        snprintf(offset, sizeof(offset), "%s", view->wco_label);
        nc_draw_text_clip(NC_MANUAL_COL_MACH + 54, NC_PANE_Y + 2,
                                 offset, 16, NC_VISUAL_ACCENT, NC_VISUAL_BG,
                                 LVDS_FONT_SMALL);
    }
    for (i = 0; i < 4; i++) {
        int y = NC_PANE_Y + 30 + i * NC_MANUAL_ROW_H;
        char value[24];
        char machine[24];
        char stop[24];
        char room[24];
        bool picked = (i < 2) && ((int)g_nc_manual_axis == i);
        lvds_color_t fg = picked ? NC_VISUAL_LINE_NO_SELECTED : NC_VISUAL_TEXT;
        lvds_color_t dim = picked ? NC_VISUAL_LINE_NO_SELECTED : NC_VISUAL_DIM;
        lvds_color_t bg = picked ? NC_VISUAL_SELECT : NC_VISUAL_BG;

        switch (i) {
        case 0: snprintf(value, sizeof(value), "%9.3f", (double)runtime->x); break;
        case 1: snprintf(value, sizeof(value), "%9.3f", (double)runtime->z); break;
        case 2: snprintf(value, sizeof(value), "%8.1f", (double)runtime->feed); break;
        default: snprintf(value, sizeof(value), "%8u", runtime->spindle); break;
        }
        machine[0] = '\0';
        stop[0] = '\0';
        room[0] = '\0';
        if (i < 2) {
            float offset = view->have_wco
                               ? (i == 0 ? view->wco_x
                                        : view->wco_z)
                               : 0.0f;
            double pos = (i == 0) ? (double)runtime->x : (double)runtime->z;

            /* The row reads in the offset in use; the machine figure the
               offset was cut from keeps its own column, one font smaller
               and without the caret that only ever restated the column
               heading. */
            snprintf(value, sizeof(value), "%9.3f", pos - (double)offset);
            snprintf(machine, sizeof(machine), "%9.3f", pos);
            if (g_nc_manual_stop_set[i]) {
                double limit = (double)g_nc_manual_stop[i];

                /* The limit '*' set for this axis, in the same frame as the
                   row, with the room left before the feed stops on it. */
                snprintf(stop, sizeof(stop), "%8.3f", limit - (double)offset);
                snprintf(room, sizeof(room), "to%7.3f",
                         limit - pos < 0.0 ? pos - limit : limit - pos);
            } else {
                snprintf(stop, sizeof(stop), "%8s", "--");
                snprintf(room, sizeof(room), "%s", "press *");
            }
        }
        if (picked) {
            /* The whole line is the selection, the way the editor marks
               the line its keys are working on. */
            lvds_draw_fill_rect(16, y - 8, LVDS_HSTX_WIDTH - 32,
                                NC_MANUAL_ROW_H - 20, NC_VISUAL_SELECT);
        }
        /* Every cell is written with its own width so a shorter number
           cannot leave the tail of the last one behind. */
        lvds_draw_text(NC_MANUAL_COL_X, y, letters[i], dim, bg,
                       LVDS_FONT_LARGE);
        nc_draw_text_clip(NC_MANUAL_COL_POS, y, value, 9, fg, bg,
                                 LVDS_FONT_LARGE);
        if (machine[0]) {
            nc_draw_text_clip(NC_MANUAL_COL_MACH, y + 7, machine, 9,
                                     picked ? NC_VISUAL_LINE_NO_SELECTED
                                            : NC_VISUAL_TEXT,
                                     bg, LVDS_FONT_NORMAL);
        }
        if (i < 2) {
            /* Two small rows in the same line: the stop itself, and how far
               the axis still is from it. */
            nc_draw_text_clip(NC_MANUAL_COL_STOP, y + 1, stop, 9,
                                     picked ? NC_VISUAL_LINE_NO_SELECTED
                                            : NC_VISUAL_ACCENT,
                                     bg, LVDS_FONT_SMALL);
            nc_draw_text_clip(NC_MANUAL_COL_STOP, y + 12, room, 9,
                                     dim, bg, LVDS_FONT_SMALL);
        }
    }
    if (g_nc_manual_touch_active) {
        char field[24];

        snprintf(field, sizeof(field), "TOUCH %c = %s_",
                 nc_manual_letter(),
                 g_nc_manual_touch[0] ? g_nc_manual_touch : "");
        nc_draw_text_clip(60, NC_PANE_Y + 268, field, 24,
                                 NC_VISUAL_ACCENT, NC_VISUAL_BG, LVDS_FONT_LARGE);
    }
    {
        /* The two values the jog keys use, beside the pad they belong to.
           The one the feed mode is on is filled, the way the picked axis
           line is: `1` and `3` change the filled one, so what they change
           is on the screen at the moment they are pressed. */
        const int value_x = NC_RIGHT_PANE_X + 20;
        const int value_w = 150;
        char line[28];
        int row;

        nc_draw_text_clip(value_x, NC_PANE_BOTTOM - 124, "1-    3+", 8,
                                 NC_VISUAL_DIM, NC_VISUAL_BG, LVDS_FONT_SMALL);
        for (row = 0; row < 2; row++) {
            bool continuous = (row == 1);
            bool active = (continuous == g_nc_manual_continuous);
            int y = NC_PANE_BOTTOM - 100 + row * 30;

            if (active) {
                lvds_draw_fill_rect(value_x - 6, y - 4, value_w, 24,
                                    NC_VISUAL_SELECT);
            }
            if (continuous) {
                snprintf(line, sizeof(line), "FEED %.0f mm/min",
                         (double)nc_manual_feed_value());
            } else {
                snprintf(line, sizeof(line), "STEP %.3f mm",
                         (double)nc_manual_step_value());
            }
            nc_draw_text_clip(value_x, y,
                                     line, 18,
                                     active ? NC_VISUAL_LINE_NO_SELECTED
                                            : NC_VISUAL_DIM,
                                     active ? NC_VISUAL_SELECT : NC_VISUAL_BG,
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
