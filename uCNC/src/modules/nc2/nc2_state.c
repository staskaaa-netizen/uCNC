#include "nc2_state.h"

#include "nc2_files.h"

#include "../../cnc.h"
#include "../../core/interpolator.h"
#include "../../hal/kinematics/kinematic.h"
#include "../file_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NC2_STATE_MAX_BYTES 1024u
#define NC2_STATE_SPINDLE_DEFAULT 1000u

static char g_nc2_path[NC2_MODE_COUNT][NC2_PATH_MAX];
static size_t g_nc2_cursor[NC2_MODE_COUNT];
static nc2_mode_t g_nc2_mode;
static unsigned g_nc2_spindle = NC2_STATE_SPINDLE_DEFAULT;
static bool g_nc2_dirty;

/* The keys are nc's, so a card's state file reads the same in both modules. */
static const char *nc2_state_key(nc2_mode_t mode)
{
    switch (mode) {
    case NC2_MODE_MANUAL:
        return "MANUAL";
    case NC2_MODE_TOOLS:
        return "TOOLS";
    case NC2_MODE_RUN:
        return "RUN";
    case NC2_MODE_PROGRAM:
    default:
        return "EDIT";
    }
}

static bool nc2_state_key_matches(const char *key, nc2_mode_t *mode)
{
    nc2_mode_t i;

    if (!key) {
        return false;
    }
    for (i = 0; i < NC2_MODE_COUNT; i++) {
        if (strcmp(key, nc2_state_key(i)) == 0) {
            *mode = i;
            return true;
        }
    }
    return strcmp(key, "PROGRAM") == 0 && (*mode = NC2_MODE_PROGRAM, true);
}

void nc2_state_set_mode(nc2_mode_t mode)
{
    if (mode >= 0 && mode < NC2_MODE_COUNT) {
        g_nc2_mode = mode;
    }
}

nc2_mode_t nc2_state_mode(void)
{
    if (g_nc2_mode < 0 || g_nc2_mode >= NC2_MODE_COUNT) {
        return NC2_MODE_PROGRAM;
    }
    return g_nc2_mode;
}

void nc2_state_remember_path(nc2_mode_t mode, const char *path)
{
    int i;

    if (mode < 0 || mode >= NC2_MODE_COUNT || !path) {
        return;
    }
    if (strcmp(g_nc2_path[mode], path) == 0) {
        return;
    }
    g_nc2_cursor[mode] = 0u;
    /* The same file open in two modes keeps one cursor: they are the same text,
       so they are the same place in it. */
    for (i = 0; i < NC2_MODE_COUNT; i++) {
        if (strcmp(g_nc2_path[i], path) == 0) {
            g_nc2_cursor[mode] = g_nc2_cursor[i];
            break;
        }
    }
    snprintf(g_nc2_path[mode], sizeof(g_nc2_path[mode]), "%s", path);
    nc2_state_save();
}

const char *nc2_state_path(nc2_mode_t mode)
{
    if (mode < 0 || mode >= NC2_MODE_COUNT) {
        return "";
    }
    return g_nc2_path[mode];
}

size_t nc2_state_cursor(nc2_mode_t mode)
{
    if (mode < 0 || mode >= NC2_MODE_COUNT) {
        return 0u;
    }
    return g_nc2_cursor[mode];
}

void nc2_state_remember_cursor(const nc2_document_t *doc)
{
    nc2_mode_t mode;
    int i;

    if (!doc || !doc->path[0]) {
        return;
    }
    for (i = 0; i < NC2_MODE_COUNT; i++) {
        if (strcmp(g_nc2_path[i], doc->path) == 0) {
            g_nc2_cursor[i] = doc->cursor;
        }
    }
    mode = nc2_state_mode();
    g_nc2_cursor[mode] = doc->cursor;
}

bool nc2_state_load_document(nc2_mode_t mode, nc2_document_t *doc)
{
    const char *path;

    if (mode < 0 || mode >= NC2_MODE_COUNT || !doc) {
        return false;
    }
    path = nc2_state_path(mode);
    if (!path[0] || !nc2_file_load(doc, path)) {
        return false;
    }
    if (g_nc2_cursor[mode] < doc->line_count) {
        doc->cursor = g_nc2_cursor[mode];
    }
    return true;
}

void nc2_state_save(void)
{
    g_nc2_dirty = true;
}

void nc2_state_flush(void)
{
    fs_file_t *fp;
    int i;

    if (!g_nc2_dirty) {
        return;
    }
    g_nc2_dirty = false;
    fp = fs_open(NC2_STATE_PATH, "w");
    if (!fp) {
        return;
    }
    {
        char line[32];
        int n = snprintf(line, sizeof(line), "MODE=%s\n",
                         nc2_state_key(nc2_state_mode()));

        if (n > 0 && n < (int)sizeof(line)) {
            (void)fs_write(fp, (const uint8_t *)line, (size_t)n);
        }
    }
    {
        char line[32];
        int n = snprintf(line, sizeof(line), "SPINDLE=%u\n", g_nc2_spindle);

        if (n > 0 && n < (int)sizeof(line)) {
            (void)fs_write(fp, (const uint8_t *)line, (size_t)n);
        }
    }
    for (i = 0; i < NC2_MODE_COUNT; i++) {
        char line[NC2_PATH_MAX + 16];
        int n = snprintf(line, sizeof(line), "%s=%s\n",
                         nc2_state_key((nc2_mode_t)i), g_nc2_path[i]);

        if (n > 0 && n < (int)sizeof(line)) {
            (void)fs_write(fp, (const uint8_t *)line, (size_t)n);
        }
    }
    fs_close(fp);
}

unsigned nc2_state_manual_spindle(void)
{
    return g_nc2_spindle;
}

void nc2_state_remember_manual_spindle(unsigned rpm)
{
    if (rpm == 0u || rpm == g_nc2_spindle) {
        return;
    }
    g_nc2_spindle = rpm;
    nc2_state_save();
}

void nc2_state_runtime(nc2_runtime_state_t *state)
{
    int32_t steppos[STEPPER_COUNT] = { 0 };
    float axis[AXIS_COUNT] = { 0 };

    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->exec_state = cnc_get_exec_state(EXEC_ALLACTIVE);
    itp_get_rt_position(steppos);
    kinematics_steps_to_coordinates(steppos, axis);
    state->x = axis[AXIS_X];
    state->z = axis[AXIS_Z];
    state->feed = itp_get_rt_feed();
#if TOOL_COUNT > 0
    state->spindle = (unsigned)tool_get_speed();
#endif
}

bool nc2_state_busy(void)
{
    return cnc_get_exec_state(EXEC_RUN | EXEC_HOLD | EXEC_JOG | EXEC_HOMING) != 0u ||
           cnc_has_alarm();
}

void nc2_state_init(void)
{
    fs_file_t *fp;
    char line[NC2_PATH_MAX + 16];
    size_t used = 0u;
    uint32_t read_bytes = 0u;

    memset(g_nc2_path, 0, sizeof(g_nc2_path));
    memset(g_nc2_cursor, 0, sizeof(g_nc2_cursor));
    g_nc2_mode = NC2_MODE_PROGRAM;
    fp = fs_open(NC2_STATE_PATH, "r");
    if (!fp) {
        return;
    }
    /* The file is small and it is read once: a byte at a time, with a cap so a
       card that never ends cannot hold the boot up. */
    while (fs_available(fp) > 0) {
        char c;
        char *eq;

        if (++read_bytes > NC2_STATE_MAX_BYTES) {
            break;
        }
        if (fs_read(fp, (uint8_t *)&c, 1u) != 1u) {
            break;
        }
        if (c != '\n' && used + 1u < sizeof(line)) {
            if (c != '\r') {
                line[used++] = c;
            }
            continue;
        }
        line[used] = '\0';
        used = 0u;
        eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq++ = '\0';
        if (strcmp(line, "MODE") == 0) {
            nc2_mode_t mode;

            if (nc2_state_key_matches(eq, &mode)) {
                nc2_state_set_mode(mode);
            }
        } else if (strcmp(line, "SPINDLE") == 0) {
            long rpm = strtol(eq, 0, 10);

            if (rpm > 0 && rpm <= 0xffffL) {
                g_nc2_spindle = (unsigned)rpm;
            }
        } else {
            nc2_mode_t mode;

            if (nc2_state_key_matches(line, &mode)) {
                nc2_state_remember_path(mode, eq);
            }
        }
    }
    fs_close(fp);
    g_nc2_dirty = false;
}
