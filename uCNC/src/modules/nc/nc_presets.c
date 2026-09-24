#include "nc_presets.h"
#include "nc_vocab.h"

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

/* The compiled entries, one row each: an id (the key path, see
   `nc_presets.h`), the name the key reads as, and the rows it writes - `\n`
   separated, because a section's rows are lines of a program. Everything the
   card can change about an entry is here and nowhere else in the code.

   Two of them are worth the note: `11` writes *nothing* (a blank line is a
   section like any other, and a separator comment or a command the operator
   keeps needing is their edit), and `32`/`33` start with a space so they add
   their word to the line the cursor is on rather than starting one. */
typedef struct {
    int id;
    const char *name;
    const char *rows;
} nc_preset_builtin_t;

static const nc_preset_builtin_t g_nc_preset_builtins[] = {
    { NC_PRESET_ID_INS, "INS", "" },
    { NC_PRESET_ID_END, "G80", "G80" },
    { NC_PRESET_ID_SETUP, "SETUP",
      "G970 X0 U0 Z0 W0\nG971 X0 Z0 I0 E0\nG972 C0\nG973 P0" },
    { NC_PRESET_ID_M6, "M6", "M6" },
    { NC_PRESET_ID_M3, "M3", "M3 S1000" },
    { NC_PRESET_ID_STOP, "STOP", "M5" },
    { NC_PRESET_ID_M4, "M4", "M4 S1000" },
    { NC_PRESET_ID_CHMF, "CHMF", " C0" },
    { NC_PRESET_ID_RND, "RND", " R0" },
    { NC_PRESET_ID_OD, "OD ROUGH", 0 },
    { NC_PRESET_ID_BORE, "ID BORE", 0 },
    { NC_PRESET_ID_FACE, "FACE", 0 },
    { NC_PRESET_ID_FINISH, "FINISH", "G70 P0 Q0" },
    { NC_PRESET_ID_THREAD_OD, "THREAD OD", "G76 X0 Z0 P0 Q0 F0 I0 L0 R0" },
    { NC_PRESET_ID_THREAD_ID, "THREAD ID", "G76 X0 Z0 P0 Q0 F0 I-0.2 L0 R0" },
    { NC_PRESET_ID_TAP, "TAP", "G33 X0 Z0 K0 F0" },
    { NC_PRESET_ID_DRILL, "DRILL", "G1 X0 Z0 F0" },
    { NC_PRESET_ID_PECK, "PECK", "G1 X0 Z0 F0" },
    { NC_PRESET_ID_DWELL, "DWELL", "G4 P0" }
};

static void nc_preset_add_builtin(int id,
                                  const char *name,
                                  const char *rows)
{
    nc_preset_rec_t *rec;
    const char *row;
    int i;

    if (g_nc_preset_count >= NC_PRESET_MAX || !rows) {
        return;
    }
    rec = &g_nc_presets[g_nc_preset_count++];
    memset(rec, 0, sizeof(*rec));
    rec->id = id;
    strncpy(rec->name, name, sizeof(rec->name) - 1);
    row = rows;
    for (i = 0; i < NC_PRESET_MAX_LINES; i++) {
        const char *end = strchr(row, '\n');
        size_t len = end ? (size_t)(end - row) : strlen(row);

        if (len >= sizeof(rec->lines[0])) {
            len = sizeof(rec->lines[0]) - 1u;
        }
        memcpy(rec->lines[rec->count], row, len);
        rec->lines[rec->count][len] = '\0';
        rec->count++;
        if (!end) {
            break;              /* the last row */
        }
        row = end + 1;
    }
}

static void nc_presets_load_builtin(void)
{
    size_t i;

    g_nc_preset_count = 0;
    for (i = 0u; i < sizeof(g_nc_preset_builtins) / sizeof(g_nc_preset_builtins[0]); i++) {
        const char *rows = g_nc_preset_builtins[i].rows;
        if (g_nc_preset_builtins[i].id == NC_PRESET_ID_OD) {
            rows = nc_vocab_gcode_template(71);
        } else if (g_nc_preset_builtins[i].id == NC_PRESET_ID_BORE ||
                   g_nc_preset_builtins[i].id == NC_PRESET_ID_FACE) {
            rows = nc_vocab_gcode_template(72);
        }
        nc_preset_add_builtin(g_nc_preset_builtins[i].id,
                              g_nc_preset_builtins[i].name,
                              rows);
    }
}

/* Ids the menus used to hold, mapped to the entry they always named: `10` was a
   one-level path on an older footer, `44`/`80` were two levels of the layout
   before the G7X and OPS menus were arranged as they are now. A card's section
   keeps meaning what it meant - the operator's edited text is not thrown away
   for a renumbering - and the file is not rewritten behind their back; the name
   is theirs to fix when they next open the file.

   `80`'s entry is the end mark, which the OPS menu no longer offers (the G7X
   menu's `6 G80` is the one key for it), so its id is that path: `46`. */
static int nc_preset_id_current(int id)
{
    switch (id) {
    case 10: return 16;              /* setup:  OPS `1` then `6` */
    case 44: return 48;              /* finish: G7X `4` then `8` */
    case 80: return 46;              /* end:    G7X `4` then `6` */
    default: return id;
    }
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

/* One section of the file, applied over the compiled set. The file defines the
   entries it names - an edited `[41]` is the operator's OD preset and stays so -
   while an id the file does not mention keeps its compiled entry, so a card
   written before an entry existed still offers it (that is how `44 FINISH`
   survives a `presets.txt` from an older build). A section without a name or
   without a line is dropped, which leaves the compiled entry it would have
   replaced in place. */
static void nc_preset_apply_section(const nc_preset_rec_t *section)
{
    nc_preset_rec_t *dst;

    if (!section || section->id <= 0 || section->count == 0 ||
        !nc_preset_valid_name(section->name)) {
        return;
    }
    dst = nc_preset_find_id(section->id);
    if (dst) {
        *dst = *section;
        return;
    }
    if (g_nc_preset_count < NC_PRESET_MAX) {
        g_nc_presets[g_nc_preset_count] = *section;
        g_nc_preset_count++;
    }
}

static bool nc_presets_read_file(void)
{
    fs_file_t *fp;
    char line[NC_MAX_LINE_LEN + 8];
    size_t used = 0;
    int read_count = 0;
    nc_preset_rec_t section;
    bool have_section = false;

    fp = fs_open(NC_PRESET_FILE_PATH, "r");
    if (!fp) {
        return false;
    }
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
                    if (have_section) {
                        nc_preset_apply_section(&section);
                    }
                    memset(&section, 0, sizeof(section));
                    /* An id the menus used to hold is read as the entry it
                       always named, so the rest of the file's rules - "the
                       section replaces the compiled entry with that id", "the
                       last section to name an entry wins" - work on one id per
                       entry, whatever the card was written with. */
                    section.id = nc_preset_id_current(id);
                    have_section = true;
                }
            } else if (have_section && strncmp(p, "name=", 5u) == 0) {
                strncpy(section.name, p + 5, sizeof(section.name) - 1);
            } else if (have_section && strncmp(p, "line=", 5u) == 0) {
                if (section.count < NC_PRESET_MAX_LINES) {
                    strncpy(section.lines[section.count],
                            p + 5,
                            sizeof(section.lines[0]) - 1);
                    section.count++;
                }
            }
        } else if (c != '\r') {
            line[used++] = c;
        }
    }
    if (have_section) {
        nc_preset_apply_section(&section);
    }
    fs_close(fp);
    /* Anything the file defines is in place now; the compiled entries it does
       not define are untouched and still available. */
    return have_section;
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
    size_t last;
    bool inserted = false;
    bool appended = false;
    int i;

    if (!doc || !rec || rec->count == 0) {
        return NC_ERR_BAD_ARG;
    }
    at = doc->cursor_line + 1u;
    if (doc->line_count == 0) {
        at = 0;
    }
    /* The line an appending row continues: the one above the insert point, or
       the last row this entry wrote. */
    last = at > 0u ? at - 1u : (size_t)-1;
    for (i = 0; i < rec->count; i++) {
        const char *text = rec->lines[i];
        nc_result_t r;

        /* A row that starts with a space **continues the line above** instead of
           starting one: that is how a value that belongs on the line already
           written - a `Q` on a cycle header, a `C`/`R` on a contour row - gets
           into the program without the controller ever seeing a line break
           (bench: "on N/Q or other things to be added inline - just use trick by
           not have a new line before values. so controller will know it all"). */
        if (text[0] == ' ' && last != (size_t)-1) {
            char joined[NC_MAX_LINE_LEN];
             nc_word_t words[24];
             int count;
             int n = snprintf(joined, sizeof(joined), "%s%s",
                              doc->lines[last].text, text);

            if (n <= 0 || n >= (int)sizeof(joined)) {
                return NC_ERR_BAD_ARG;
            }
            r = nc_set_line(doc, last, joined);
            if (r != NC_OK) {
                return r;
            }
            /* A value written to be typed is left picked, and the line under the
               cursor stays the cursor's: this row is part of the line the
               operator is already on. */
            count = nc_parse_words(joined, words, 24);
            doc->selected_word = count > 0 ? count - 1 : -1;
            appended = true;
            continue;
        }
        r = nc_insert_line(doc, at + (size_t)i, text);
        if (r != NC_OK) {
            return r;
        }
        last = at + (size_t)i;
        inserted = true;
    }
    if (inserted) {
        doc->cursor_line = at;
        if (!appended) {
            doc->selected_word = -1;
        }
    }
    return NC_OK;
}

bool nc_insert_preset_id(nc_document_t *doc, int id)
{
    nc_preset_rec_t *rec;

    (void)nc_presets_sync();
    rec = nc_preset_find_id(id);
    return rec && nc_preset_insert_lines(doc, rec) == NC_OK;
}

bool nc_preset_name_for_id(int id, char *out, size_t out_sz)
{
    nc_preset_rec_t *rec;

    if (!out || out_sz == 0u) {
        return false;
    }
    out[0] = '\0';
    (void)nc_presets_sync();
    rec = nc_preset_find_id(id);
    if (!rec || !nc_preset_valid_name(rec->name)) {
        return false;
    }
    strncpy(out, rec->name, out_sz - 1u);
    out[out_sz - 1u] = '\0';
    return true;
}
