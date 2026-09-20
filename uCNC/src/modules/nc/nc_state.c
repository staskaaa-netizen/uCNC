#include "nc_state.h"

#include "../../cnc.h"
#include "../../core/interpolator.h"
#include "../../hal/kinematics/kinematic.h"
#if TOOL_COUNT > 0
#include "../../hal/tools/tool.h"
#endif
#include "../file_system.h"
#include "nc_tools.h"
#include "nc_vocab.h"

#include <stdio.h>
#include <string.h>

#define NC_STATE_PATH      "/D/nc_state.txt"
#define NC_STATE_OLD_PATH  "nc_state.txt"
#define NC_STATE_MAX_BYTES 1024u
#define NC_STATE_TIMEOUT_MS 500u

static char g_nc_state_path[NC_MODE_COUNT][NC_PATH_MAX];
static size_t g_nc_state_cursor[NC_MODE_COUNT];
static nc_mode_t g_nc_state_mode;

bool nc_state_tool_path_supported(const char *path)
{
    size_t len;

    if (!path || !path[0]) {
        return false;
    }
    len = strlen(path);
    return len >= 2 &&
           (path[len - 2] == '.') &&
           (path[len - 1] == 't' || path[len - 1] == 'T');
}

static const char *nc_state_key(nc_mode_t mode)
{
    switch (mode) {
    case NC_MODE_PROGRAM: return "EDIT";
    case NC_MODE_TOOLS: return "TOOLS";
    case NC_MODE_RUN: return "RUN";
    case NC_MODE_MANUAL:
    default: return "MANUAL";
    }
}

static bool nc_state_key_matches(const char *key, nc_mode_t mode)
{
    if (!key) {
        return false;
    }
    if (strcmp(key, nc_state_key(mode)) == 0) {
        return true;
    }
    return mode == NC_MODE_PROGRAM && strcmp(key, "PROGRAM") == 0;
}

static bool nc_state_mode_from_text(const char *text, nc_mode_t *mode)
{
    int i;

    if (!text || !mode) {
        return false;
    }
    for (i = 0; i < NC_MODE_COUNT; i++) {
        if (nc_state_key_matches(text, (nc_mode_t)i)) {
            *mode = (nc_mode_t)i;
            return true;
        }
    }
    return false;
}

void nc_state_set_mode(nc_mode_t mode)
{
    if (mode < 0 || mode >= NC_MODE_COUNT) {
        return;
    }
    g_nc_state_mode = mode;
}

nc_mode_t nc_state_mode(void)
{
    if (g_nc_state_mode < 0 || g_nc_state_mode >= NC_MODE_COUNT) {
        return NC_MODE_PROGRAM;
    }
    return g_nc_state_mode;
}

void nc_state_remember_path(nc_mode_t mode, const char *path)
{
    if (mode < 0 || mode >= NC_MODE_COUNT || !path) {
        return;
    }
    if (strcmp(g_nc_state_path[mode], path) != 0) {
        g_nc_state_cursor[mode] = 0;
        for (int i = 0; i < NC_MODE_COUNT; i++) {
            if (strcmp(g_nc_state_path[i], path) == 0) {
                g_nc_state_cursor[mode] = g_nc_state_cursor[i];
                break;
            }
        }
    }
    strncpy(g_nc_state_path[mode], path, NC_PATH_MAX - 1);
    g_nc_state_path[mode][NC_PATH_MAX - 1] = '\0';
}

void nc_state_remember_cursor(const nc_document_t *doc)
{
    if (!doc || !doc->path[0]) return;
    for (int i = 0; i < NC_MODE_COUNT; i++) {
        if (strcmp(g_nc_state_path[i], doc->path) == 0)
            g_nc_state_cursor[i] = doc->cursor_line;
    }
}

const char *nc_state_path(nc_mode_t mode)
{
    if (mode < 0 || mode >= NC_MODE_COUNT) {
        return "";
    }
    return g_nc_state_path[mode];
}

/* The state file is written when the screen goes idle, not while the operator
   is moving between screens: a FAT write costs a good part of a second on the
   card, and a screen change used to pay it two or three times over. Everything
   that changes the remembered state only marks it stale. */
static bool g_nc_state_dirty;

static void nc_state_write(void)
{
    fs_file_t *fp = fs_open(NC_STATE_PATH, "w");
    int i;

    if (!fp) {
        return;
    }
    {
        char line[24];
        int n = snprintf(line,
                         sizeof(line),
                         "MODE=%s\n",
                         nc_state_key(nc_state_mode()));
        if (n > 0 && n < (int)sizeof(line)) {
            (void)fs_write(fp, (const uint8_t *)line, (size_t)n);
        }
    }
    for (i = 0; i < NC_MODE_COUNT; i++) {
        char line[NC_PATH_MAX + 16];
        int n = snprintf(line,
                         sizeof(line),
                         "%s=%s\n",
                         nc_state_key((nc_mode_t)i),
                         g_nc_state_path[i]);
        if (n > 0 && n < (int)sizeof(line)) {
            (void)fs_write(fp, (const uint8_t *)line, (size_t)n);
        }
    }
    fs_close(fp);
}

void nc_state_save(void)
{
    g_nc_state_dirty = true;
}

void nc_state_flush(void)
{
    if (!g_nc_state_dirty) {
        return;
    }
    g_nc_state_dirty = false;
    nc_state_write();
}

static fs_file_t *nc_state_open_read(void)
{
    fs_file_t *fp = fs_open(NC_STATE_PATH, "r");

    if (!fp) {
        fp = fs_open(NC_STATE_OLD_PATH, "r");
    }
    return fp;
}

void nc_state_init(void)
{
    fs_file_t *fp;
    char line[NC_PATH_MAX + 16];
    size_t used = 0;
    uint32_t read_bytes = 0;
    uint32_t start_ms;

    memset(g_nc_state_path, 0, sizeof(g_nc_state_path));
    memset(g_nc_state_cursor, 0, sizeof(g_nc_state_cursor));
    g_nc_state_mode = NC_MODE_PROGRAM;
    strncpy(g_nc_state_path[NC_MODE_TOOLS], NC_TOOL_PATH, NC_PATH_MAX - 1);

    fp = nc_state_open_read();
    if (!fp) {
        return;
    }

    start_ms = mcu_millis();
    while (fs_available(fp)) {
        char c;
        if (++read_bytes > NC_STATE_MAX_BYTES ||
            (uint32_t)(mcu_millis() - start_ms) > NC_STATE_TIMEOUT_MS) {
            break;
        }
        if (fs_read(fp, (uint8_t *)&c, 1) != 1) {
            break;
        }
        if (c == '\n' || used + 1 >= sizeof(line)) {
            char *eq;
            line[used] = '\0';
            eq = strchr(line, '=');
            if (eq) {
                int i;
                *eq++ = '\0';
                if (strcmp(line, "MODE") == 0) {
                    nc_mode_t mode;
                    if (nc_state_mode_from_text(eq, &mode)) {
                        nc_state_set_mode(mode);
                    }
                } else {
                    for (i = 0; i < NC_MODE_COUNT; i++) {
                        if (nc_state_key_matches(line, (nc_mode_t)i)) {
                            nc_state_remember_path((nc_mode_t)i, eq);
                            break;
                        }
                    }
                }
            }
            used = 0;
        } else if (c != '\r') {
            line[used++] = c;
        }
    }
    if (used > 0) {
        char *eq;
        line[used] = '\0';
        eq = strchr(line, '=');
        if (eq) {
            int i;
            *eq++ = '\0';
            if (strcmp(line, "MODE") == 0) {
                nc_mode_t mode;
                if (nc_state_mode_from_text(eq, &mode)) {
                    nc_state_set_mode(mode);
                }
            } else {
                for (i = 0; i < NC_MODE_COUNT; i++) {
                    if (nc_state_key_matches(line, (nc_mode_t)i)) {
                        nc_state_remember_path((nc_mode_t)i, eq);
                        break;
                    }
                }
            }
        }
    }
    fs_close(fp);
}

bool nc_state_load_document(nc_mode_t mode, nc_document_t *doc)
{
    const char *path;

    if (!doc || mode < 0 || mode >= NC_MODE_COUNT) {
        return false;
    }

    path = g_nc_state_path[mode];
    if (mode == NC_MODE_TOOLS && !nc_state_tool_path_supported(path)) {
        path = NC_TOOL_PATH;
    }
    if (path && path[0] && nc_load_file(doc, path) == NC_OK) {
        nc_state_remember_path(mode, path);
        doc->cursor_line = doc->line_count ? MIN(g_nc_state_cursor[mode], doc->line_count - 1) : 0;
        return true;
    } else if (path && path[0] && mode != NC_MODE_TOOLS) {
        nc_state_remember_path(mode, "");
        nc_state_save();
    }
    if (mode == NC_MODE_TOOLS) {
        nc_document_init(doc);
        (void)nc_insert_line(doc, 0, nc_tool_default_line(1));
        strncpy(doc->path, NC_TOOL_PATH, sizeof(doc->path) - 1);
        doc->path[sizeof(doc->path) - 1] = '\0';
        (void)nc_save_file(doc, NC_TOOL_PATH);
        nc_state_remember_path(mode, NC_TOOL_PATH);
        nc_state_save();
        return true;
    }
    return false;
}

void nc_state_runtime(nc_runtime_state_t *state)
{
    int32_t steppos[STEPPER_COUNT] = {0};
    float axis[AXIS_COUNT] = {0};

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

void nc_state_snapshot(const nc_document_t *doc, nc_snapshot_t *snapshot)
{
    size_t first = 0;
    size_t cursor;
    size_t i;
    nc_word_t word;

    if (!snapshot) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->cursor_visible_index = -1;
    snapshot->selected_word_start = -1;
    snapshot->selected_word_end = -1;
    nc_state_runtime(&snapshot->runtime);

    if (!doc) {
        return;
    }

    cursor = doc->cursor_line;
    if (doc->line_count > 0 && cursor >= doc->line_count) {
        cursor = 0;
    }

    if (cursor >= NC_MAX_VISIBLE_LINES / 2) {
        first = cursor - (NC_MAX_VISIBLE_LINES / 2);
    }
    if (first + NC_MAX_VISIBLE_LINES > doc->line_count && doc->line_count > NC_MAX_VISIBLE_LINES) {
        first = doc->line_count - NC_MAX_VISIBLE_LINES;
    }

    strncpy(snapshot->path, doc->path, sizeof(snapshot->path) - 1);
    snapshot->first_line = first;
    snapshot->cursor_line = cursor;
    snapshot->line_count = doc->line_count;
    snapshot->dirty = doc->dirty;

    for (i = 0; i < NC_MAX_VISIBLE_LINES && first + i < doc->line_count; i++) {
        strncpy(snapshot->lines[i], doc->lines[first + i].text, NC_MAX_LINE_LEN - 1);
        if (first + i == cursor) {
            snapshot->cursor_visible_index = (int)i;
        }
    }

    if (cursor == doc->cursor_line &&
        nc_get_selected_word(doc, &word) == NC_OK &&
        cursor < doc->line_count) {
        const char *label = nc_vocab_label_for_word(doc->lines[cursor].text, &word);
        snapshot->selected_word_start = word.start;
        snapshot->selected_word_end = word.end;
        strncpy(snapshot->selected_label, label, sizeof(snapshot->selected_label) - 1);
    }
}
