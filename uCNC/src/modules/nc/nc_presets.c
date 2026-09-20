#include "nc_presets.h"

#include "../file_system.h"

#include <stdio.h>
#include <string.h>

#define NC_PRESET_FILE_PATH "/D/presets.txt"
#define NC_PRESET_MAX 24
#define NC_PRESET_MAX_LINES 8

typedef struct {
    int id;
    char name[16];
    uint8_t count;
    char lines[NC_PRESET_MAX_LINES][NC_MAX_LINE_LEN];
} nc_preset_rec_t;

static nc_preset_rec_t g_nc_presets[NC_PRESET_MAX];
static int g_nc_preset_count;
/* The preset file lives on the SD card, and the card is mounted from the main
   loop - long after module init. So the file is resolved lazily: every call
   until it is settled keeps the compiled presets and tries again. */
static bool g_nc_presets_file_settled;
static bool g_nc_presets_from_file;

static const char *const g_nc_preset_od[] = {
    "G71 U0 R0 X0 Z0 F0 P0 Q0 N0"
};
static const char *const g_nc_preset_id[] = {
    "G72 W0 R0 X0 Z0 F0 P0 Q0 N0"
};
static const char *const g_nc_preset_face[] = {
    "G72 W0 R0 X0 Z0 F0 P0 Q0 N0"
};
static const char *const g_nc_preset_thread_od[] = {
    "G76 X0 Z0 P0 Q0 F0 I0 L0 R0"
};
static const char *const g_nc_preset_thread_id[] = {
    "G76 X0 Z0 P0 Q0 F0 I-0.2 L0 R0"
};
static const char *const g_nc_preset_tap[] = {
    "G33 X0 Z0 K0 F0"
};
static const char *const g_nc_preset_drill[] = {
    "G1 X0 Z0 F0"
};
static const char *const g_nc_preset_peck[] = {
    "G1 X0 Z0 F0"
};
static const char *const g_nc_preset_dwell[] = {
    "G4 P0"
};
static const char *const g_nc_preset_end[] = {
    "G80"
};
static const char *const g_nc_preset_setup[] = {
    "G970 X0 U0 Z0 W0",
    "G971 X0 Z0 I0 E0",
    "G972 C0",
    "G973 P0"
};
static const char *const g_nc_preset_line[] = {
    "G1 X0 Z0 C0 R0"
};
static const char *const g_nc_preset_arc[] = {
    "G2 X0 Z0 R0 I0 K0 F0"
};

static void nc_preset_add_builtin(int id,
                                  const char *name,
                                  const char *const *lines,
                                  int count)
{
    nc_preset_rec_t *rec;
    int i;

    if (g_nc_preset_count >= NC_PRESET_MAX || count <= 0) {
        return;
    }
    rec = &g_nc_presets[g_nc_preset_count++];
    memset(rec, 0, sizeof(*rec));
    rec->id = id;
    strncpy(rec->name, name, sizeof(rec->name) - 1);
    if (count > NC_PRESET_MAX_LINES) {
        count = NC_PRESET_MAX_LINES;
    }
    rec->count = (uint8_t)count;
    for (i = 0; i < count; i++) {
        strncpy(rec->lines[i], lines[i], sizeof(rec->lines[i]) - 1);
    }
}

static void nc_presets_load_builtin(void)
{
    g_nc_preset_count = 0;
    nc_preset_add_builtin(41, "OD ROUGH", g_nc_preset_od, 1);
    nc_preset_add_builtin(42, "ID BORE", g_nc_preset_id, 1);
    nc_preset_add_builtin(43, "FACE", g_nc_preset_face, 1);
    nc_preset_add_builtin(51, "THREAD OD", g_nc_preset_thread_od, 1);
    nc_preset_add_builtin(52, "THREAD ID", g_nc_preset_thread_id, 1);
    nc_preset_add_builtin(53, "TAP", g_nc_preset_tap, 1);
    nc_preset_add_builtin(61, "DRILL", g_nc_preset_drill, 1);
    nc_preset_add_builtin(62, "PECK", g_nc_preset_peck, 1);
    nc_preset_add_builtin(63, "DWELL", g_nc_preset_dwell, 1);
    nc_preset_add_builtin(80, "END", g_nc_preset_end, 1);
    nc_preset_add_builtin(10, "SETUP", g_nc_preset_setup, 4);
    nc_preset_add_builtin(21, "LINE", g_nc_preset_line, 1);
    nc_preset_add_builtin(22, "ARC", g_nc_preset_arc, 1);
}

static bool nc_presets_write_builtin_file(void)
{
    fs_file_t *fp = fs_open(NC_PRESET_FILE_PATH, "w");
    bool ok = true;
    int i;
    int j;

    if (!fp) {
        return false;
    }
    for (i = 0; i < g_nc_preset_count; i++) {
        char line[NC_MAX_LINE_LEN + 16];
        int n;

        n = snprintf(line, sizeof(line), "[%d]\nname=%s\n",
                     g_nc_presets[i].id,
                     g_nc_presets[i].name);
        if (n > 0) {
            ok = fs_write(fp, (const uint8_t *)line, (size_t)n) == (size_t)n;
        }
        for (j = 0; ok && j < g_nc_presets[i].count; j++) {
            n = snprintf(line, sizeof(line), "line=%s\n",
                         g_nc_presets[i].lines[j]);
            if (n > 0) {
                ok = fs_write(fp, (const uint8_t *)line, (size_t)n) == (size_t)n;
            }
        }
        if (ok) {
            ok = fs_write(fp, (const uint8_t *)"\n", 1u) == 1u;
        }
        if (!ok) {
            break;
        }
    }
    fs_close(fp);
    if (!ok) {
        /* A half-written default would look like the operator's own file on
           the next boot, so drop it and let a later call try again. */
        (void)fs_remove(NC_PRESET_FILE_PATH);
    }
    return ok;
}

static nc_preset_rec_t *nc_preset_find_id(int id)
{
    int i;
    for (i = 0; i < g_nc_preset_count; i++) {
        if (g_nc_presets[i].id == id) {
            return &g_nc_presets[i];
        }
    }
    return 0;
}

static bool nc_preset_valid_name(const char *name)
{
    return name && name[0] && strlen(name) < 16;
}

static nc_preset_rec_t *nc_preset_begin_section(int id)
{
    nc_preset_rec_t *rec;

    if (id <= 0 || g_nc_preset_count >= NC_PRESET_MAX) {
        return 0;
    }
    rec = &g_nc_presets[g_nc_preset_count];
    memset(rec, 0, sizeof(*rec));
    rec->id = id;
    return rec;
}

static void nc_preset_commit_section(nc_preset_rec_t *rec)
{
    if (rec && rec->count > 0 && nc_preset_valid_name(rec->name)) {
        g_nc_preset_count++;
    }
}

static bool nc_presets_read_file(void)
{
    fs_file_t *fp;
    char line[NC_MAX_LINE_LEN + 8];
    size_t used = 0;
    int read_count = 0;
    nc_preset_rec_t *current = 0;

    fp = fs_open(NC_PRESET_FILE_PATH, "r");
    if (!fp) {
        return false;
    }
    g_nc_preset_count = 0;
    while (fs_available(fp) && read_count < 4096) {
        char c;
        if (fs_read(fp, (uint8_t *)&c, 1u) != 1u) {
            break;
        }
        read_count++;
        if (c == '\n' || used + 1u >= sizeof(line)) {
            char *p = line;
            line[used] = '\0';
            used = 0;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == '[') {
                int id = 0;
                if (sscanf(p, "[%d]", &id) == 1) {
                    nc_preset_commit_section(current);
                    current = nc_preset_begin_section(id);
                }
            } else if (current && strncmp(p, "name=", 5u) == 0) {
                strncpy(current->name, p + 5, sizeof(current->name) - 1);
            } else if (current && strncmp(p, "line=", 5u) == 0) {
                if (current->count < NC_PRESET_MAX_LINES) {
                    strncpy(current->lines[current->count],
                            p + 5,
                            sizeof(current->lines[0]) - 1);
                    current->count++;
                }
            }
        } else if (c != '\r') {
            line[used++] = c;
        }
    }
    nc_preset_commit_section(current);
    fs_close(fp);
    if (g_nc_preset_count == 0) {
        nc_presets_load_builtin();
        return false;
    }
    return true;
}

/* Settles the preset file once the drive can answer: loads it when it is
   there, writes the compiled default when it is not. Until then the compiled
   presets stay in use, so a machine that boots without a card, or with one
   inserted later, converges on the first call that finds the drive. */
bool nc_presets_sync(void)
{
    fs_file_info_t info;

    if (g_nc_presets_file_settled) {
        return g_nc_presets_from_file;
    }
    if (fs_finfo(NC_PRESET_FILE_PATH, &info)) {
        g_nc_presets_file_settled = true;
        g_nc_presets_from_file = nc_presets_read_file();
        return g_nc_presets_from_file;
    }
    /* No file yet: leave the compiled default on the card so it can be
       edited. A failed write means the drive is not mounted yet - keep the
       compiled presets and retry on the next call. */
    if (nc_presets_write_builtin_file()) {
        g_nc_presets_file_settled = true;
    }
    return false;
}

bool nc_presets_init(void)
{
    g_nc_presets_file_settled = false;
    g_nc_presets_from_file = false;
    nc_presets_load_builtin();
    return nc_presets_sync();
}

static nc_result_t nc_preset_insert_lines(nc_document_t *doc,
                                          const nc_preset_rec_t *rec)
{
    size_t at;
    int i;

    if (!doc || !rec || rec->count == 0) {
        return NC_ERR_BAD_ARG;
    }
    at = doc->cursor_line + 1u;
    if (doc->line_count == 0) {
        at = 0;
    }
    for (i = 0; i < rec->count; i++) {
        nc_result_t r = nc_insert_line(doc, at + (size_t)i, rec->lines[i]);
        if (r != NC_OK) {
            return r;
        }
    }
    doc->cursor_line = at;
    doc->selected_word = -1;
    return NC_OK;
}

bool nc_insert_preset_id(nc_document_t *doc, int id)
{
    nc_preset_rec_t *rec;

    (void)nc_presets_sync();
    rec = nc_preset_find_id(id);
    return rec && nc_preset_insert_lines(doc, rec) == NC_OK;
}

nc_result_t nc_insert_preset(nc_document_t *doc, nc_preset_t preset)
{
    switch (preset) {
    case NC_PRESET_OD:
        return nc_insert_preset_id(doc, 41) ? NC_OK : NC_ERR_BAD_ARG;
    case NC_PRESET_ID:
        return nc_insert_preset_id(doc, 42) ? NC_OK : NC_ERR_BAD_ARG;
    case NC_PRESET_FACE:
        return nc_insert_preset_id(doc, 43) ? NC_OK : NC_ERR_BAD_ARG;
    case NC_PRESET_LINE:
        return nc_insert_preset_id(doc, 21) ? NC_OK : NC_ERR_BAD_ARG;
    case NC_PRESET_ARC:
        return nc_insert_preset_id(doc, 22) ? NC_OK : NC_ERR_BAD_ARG;
    case NC_PRESET_SETUP:
        return nc_insert_preset_id(doc, 10) ? NC_OK : NC_ERR_BAD_ARG;
    case NC_PRESET_END:
        return nc_insert_preset_id(doc, 80) ? NC_OK : NC_ERR_BAD_ARG;
    default:
        return NC_ERR_BAD_ARG;
    }
}
