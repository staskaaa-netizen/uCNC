#include "ncc_state.h"

#include "ncc_files.h"

#include "../../cnc.h"
#include "../../core/interpolator.h"
#include "../../hal/kinematics/kinematic.h"
#include "../file_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCC_STATE_MAX_BYTES 1024u
#define NCC_STATE_SPINDLE_DEFAULT 1000u

static char g_ncc_path[NCC_MODE_COUNT][NCC_PATH_MAX];
static size_t g_ncc_cursor[NCC_MODE_COUNT];
static ncc_mode_t g_ncc_mode;
static unsigned g_ncc_spindle = NCC_STATE_SPINDLE_DEFAULT;
static bool g_ncc_dirty;

/* The keys are nc's, so a card's state file reads the same in both modules. */
static const char *ncc_state_key(ncc_mode_t mode)
{
    switch (mode) {
    case NCC_MODE_MANUAL:
        return "MANUAL";
    case NCC_MODE_TOOLS:
        return "TOOLS";
    case NCC_MODE_RUN:
        return "RUN";
    case NCC_MODE_PROGRAM:
    default:
        return "EDIT";
    }
}

static bool ncc_state_key_matches(const char *key, ncc_mode_t *mode)
{
    ncc_mode_t i;

    if (!key) {
        return false;
    }
    for (i = 0; i < NCC_MODE_COUNT; i++) {
        if (strcmp(key, ncc_state_key(i)) == 0) {
            *mode = i;
            return true;
        }
    }
    return strcmp(key, "PROGRAM") == 0 && (*mode = NCC_MODE_PROGRAM, true);
}

void ncc_state_set_mode(ncc_mode_t mode)
{
    if (mode >= 0 && mode < NCC_MODE_COUNT && g_ncc_mode != mode) {
        g_ncc_mode = mode;
        /* The screen is part of what the card remembers: a power cycle comes
           back to the screen the operator left. Without this the state file
           kept the mode it was last *written* with, which - since only a file
           open or a dirty document writes it - was the first one the panel
           ever showed (bench: "state was better transferred in nc. now it
           always starts in edit"). */
        g_ncc_dirty = true;
    }
}

ncc_mode_t ncc_state_mode(void)
{
    if (g_ncc_mode < 0 || g_ncc_mode >= NCC_MODE_COUNT) {
        return NCC_MODE_PROGRAM;
    }
    return g_ncc_mode;
}

void ncc_state_remember_path(ncc_mode_t mode, const char *path)
{
    int i;

    if (mode < 0 || mode >= NCC_MODE_COUNT || !path) {
        return;
    }
    if (strcmp(g_ncc_path[mode], path) == 0) {
        return;
    }
    g_ncc_cursor[mode] = 0u;
    /* The same file open in two modes keeps one cursor: they are the same text,
       so they are the same place in it. */
    for (i = 0; i < NCC_MODE_COUNT; i++) {
        if (strcmp(g_ncc_path[i], path) == 0) {
            g_ncc_cursor[mode] = g_ncc_cursor[i];
            break;
        }
    }
    snprintf(g_ncc_path[mode], sizeof(g_ncc_path[mode]), "%s", path);
    ncc_state_save();
}

const char *ncc_state_path(ncc_mode_t mode)
{
    if (mode < 0 || mode >= NCC_MODE_COUNT) {
        return "";
    }
    return g_ncc_path[mode];
}

void ncc_state_remember_cursor(const ncc_document_t *doc)
{
    ncc_mode_t mode;
    int i;

    if (!doc || !doc->path[0]) {
        return;
    }
    for (i = 0; i < NCC_MODE_COUNT; i++) {
        if (strcmp(g_ncc_path[i], doc->path) == 0) {
            g_ncc_cursor[i] = doc->cursor;
        }
    }
    mode = ncc_state_mode();
    g_ncc_cursor[mode] = doc->cursor;
}

bool ncc_state_load_document(ncc_mode_t mode, ncc_document_t *doc)
{
    const char *path;

    if (mode < 0 || mode >= NCC_MODE_COUNT || !doc) {
        return false;
    }
    path = ncc_state_path(mode);
    if (!path[0] || !ncc_file_load(doc, path)) {
        return false;
    }
    if (g_ncc_cursor[mode] < doc->line_count) {
        doc->cursor = g_ncc_cursor[mode];
    }
    return true;
}

void ncc_state_save(void)
{
    g_ncc_dirty = true;
}

void ncc_state_flush(void)
{
    fs_file_t *fp;
    int i;

    if (!g_ncc_dirty) {
        return;
    }
    g_ncc_dirty = false;
    fp = fs_open(NCC_STATE_PATH, "w");
    if (!fp) {
        return;
    }
    {
        char line[32];
        int n = snprintf(line, sizeof(line), "MODE=%s\n",
                         ncc_state_key(ncc_state_mode()));

        if (n > 0 && n < (int)sizeof(line)) {
            (void)fs_write(fp, (const uint8_t *)line, (size_t)n);
        }
    }
    {
        char line[32];
        int n = snprintf(line, sizeof(line), "SPINDLE=%u\n", g_ncc_spindle);

        if (n > 0 && n < (int)sizeof(line)) {
            (void)fs_write(fp, (const uint8_t *)line, (size_t)n);
        }
    }
    for (i = 0; i < NCC_MODE_COUNT; i++) {
        char line[NCC_PATH_MAX + 16];
        int n = snprintf(line, sizeof(line), "%s=%s\n",
                         ncc_state_key((ncc_mode_t)i), g_ncc_path[i]);

        if (n > 0 && n < (int)sizeof(line)) {
            (void)fs_write(fp, (const uint8_t *)line, (size_t)n);
        }
    }
    fs_close(fp);
}

unsigned ncc_state_manual_spindle(void)
{
    return g_ncc_spindle;
}

void ncc_state_remember_manual_spindle(unsigned rpm)
{
    if (rpm == 0u || rpm == g_ncc_spindle) {
        return;
    }
    g_ncc_spindle = rpm;
    ncc_state_save();
}

void ncc_state_runtime(ncc_runtime_state_t *state)
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

bool ncc_state_busy(void)
{
    return cnc_get_exec_state(EXEC_RUN | EXEC_HOLD | EXEC_JOG | EXEC_HOMING) != 0u ||
           cnc_has_alarm();
}

void ncc_state_init(void)
{
    fs_file_t *fp;
    char line[NCC_PATH_MAX + 16];
    size_t used = 0u;
    uint32_t read_bytes = 0u;

    memset(g_ncc_path, 0, sizeof(g_ncc_path));
    memset(g_ncc_cursor, 0, sizeof(g_ncc_cursor));
    g_ncc_mode = NCC_MODE_PROGRAM;
    fp = fs_open(NCC_STATE_PATH, "r");
    if (!fp) {
        return;
    }
    /* The file is small and it is read once: a byte at a time, with a cap so a
       card that never ends cannot hold the boot up. */
    while (fs_available(fp) > 0) {
        char c;
        char *eq;

        if (++read_bytes > NCC_STATE_MAX_BYTES) {
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
            ncc_mode_t mode;

            if (ncc_state_key_matches(eq, &mode)) {
                ncc_state_set_mode(mode);
            }
        } else if (strcmp(line, "SPINDLE") == 0) {
            long rpm = strtol(eq, 0, 10);

            if (rpm > 0 && rpm <= 0xffffL) {
                g_ncc_spindle = (unsigned)rpm;
            }
        } else {
            ncc_mode_t mode;

            if (ncc_state_key_matches(line, &mode)) {
                ncc_state_remember_path(mode, eq);
            }
        }
    }
    fs_close(fp);
    g_ncc_dirty = false;
}
