/* The preset entries: an address, and at that address two things.

   An **address** is the key path that inserts the entry - `42` is G7X `4` then
   `2` - because that is the one thing the pads need to find it. At each address
   there is a **name** that may be empty and the **rows** the key writes, which
   may not. That is the whole model; everything else here is only how the card
   spells it.

   The card spells it as files: one per address, in /D/presets, named after the
   address (`41.txt`), first row the name, the rest the rows, and a row that
   starts with a space continues the row before it. So there is no format for
   the panel to parse and no second editor to write: the file list and the
   editor that already exist *are* the preset editor, and the entries are the
   operator's own text files. A card with no folder, or no file for an address,
   answers with the compiled table below - that is what keeps a machine without
   a card working, and it is the only part of this that lives in flash.

   What is in RAM is one name per address the pads can reach and a flag saying
   whether the folder has that entry: about a kilobyte, read once when the drive
   settles. The rows are not kept - they are read from the file when a key
   writes them, so the panel's memory cannot be grown by anything on the card.

   `docs/nc-preset-file.md` is the contract, including what the file format this
   replaced used to be and why it is gone. */
#include "nc_presets.h"
#include "nc_vocab.h"

#include "../file_system.h"

#include <stdio.h>
#include <string.h>

#define NC_PRESET_DIR "/D/presets/"
#define NC_PRESET_SUFFIX ".txt"
/* The addresses the pads can reach: one digit for the pad, one for the slot. */
#define NC_PRESET_ADDR_FIRST 10
#define NC_PRESET_ADDR_LAST 69
#define NC_PRESET_ADDR_COUNT (NC_PRESET_ADDR_LAST - NC_PRESET_ADDR_FIRST + 1)
#define NC_PRESET_NAME_LEN 16

/* The compiled entries, one row each: the address, the name a key reads as
   (which the card may empty or replace) and the rows it writes, `\n` separated
   because a section's rows are lines of a program. Everything a card can say
   about an entry is here and nowhere else in the code. */
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
    /* Fanuc's increments, appended to the line under the cursor: the row starts
       with a space, so the word is added to that line and left picked - the
       distance is typed straight in. On a move `U` and `W` have always meant the
       distance from where the tool is, so a profile can be a list of points with
       the steps between them written as increments - and there is no contour for
       the panel to walk out direction by direction. */
    { NC_PRESET_ID_U_INC, "U INC", " U" },
    { NC_PRESET_ID_W_INC, "W INC", " W" },
    { NC_PRESET_ID_DRILL, "DRILL", "G1 X0 Z0 F0" },
    { NC_PRESET_ID_PECK, "PECK", "G1 X0 Z0 F0" },
    { NC_PRESET_ID_DWELL, "DWELL", "G4 P0" }
};

/* What the folder said, read once: which addresses have an entry file, and the
   name each of them carries (the file's first row). */
static bool g_nc_preset_there[NC_PRESET_ADDR_COUNT];
static char g_nc_preset_names[NC_PRESET_ADDR_COUNT][NC_PRESET_NAME_LEN];
/* The folder lives on the card, and the card is mounted from the main loop -
   long after module init. Until the drive answers, the compiled entries are what
   the panel uses. */
static bool g_nc_presets_settled;

static int nc_preset_slot(int id)
{
    if (id < NC_PRESET_ADDR_FIRST || id > NC_PRESET_ADDR_LAST) {
        return -1;
    }
    return id - NC_PRESET_ADDR_FIRST;
}

static const nc_preset_builtin_t *nc_preset_builtin(int id)
{
    size_t i;

    for (i = 0u; i < sizeof(g_nc_preset_builtins) / sizeof(g_nc_preset_builtins[0]);
         i++) {
        if (g_nc_preset_builtins[i].id == id) {
            return &g_nc_preset_builtins[i];
        }
    }
    return 0;
}

/* The rows of a compiled entry. Two of them are the vocabulary's templates
   rather than a fixed line, so a cycle header follows the dialect the panel
   knows instead of drifting away from it. */
static const char *nc_preset_builtin_rows(int id)
{
    const nc_preset_builtin_t *builtin = nc_preset_builtin(id);

    if (!builtin) {
        return 0;
    }
    if (id == NC_PRESET_ID_OD) {
        return nc_vocab_gcode_template(71);
    }
    if (id == NC_PRESET_ID_BORE || id == NC_PRESET_ID_FACE) {
        return nc_vocab_gcode_template(72);
    }
    return builtin->rows;
}

static bool nc_preset_path(int id, char *out, size_t out_sz)
{
    int n;

    if (!out || out_sz == 0u || nc_preset_slot(id) < 0) {
        return false;
    }
    n = snprintf(out, out_sz, "%s%d%s", NC_PRESET_DIR, id, NC_PRESET_SUFFIX);
    return n > 0 && (size_t)n < out_sz;
}

/* --- one entry file, read once -------------------------------------------- */

/* Where a row goes when the entry is written: the document, the line the next
   continuing row joins, and whether anything has been written yet. */
typedef struct {
    nc_document_t *doc;
    size_t at;          /* where the next new row goes */
    size_t last;        /* the line a continuing row joins, or (size_t)-1 */
    size_t first;       /* the first line this entry wrote, or (size_t)-1 */
    bool wrote;
    bool appended;      /* a row was joined onto a line already there */
} nc_preset_out_t;

/* Put one row of an entry into the program.

   A row that starts with a space **continues the line above** instead of
   starting one: that is how a value that belongs on the line already written - a
   `Q` on a cycle header, a `C`/`R` on a contour row - gets into the program
   without the controller ever seeing a line break inside a block it has to read
   as one (bench: "on N/Q or other things to be added inline - just use trick by
   not have a new line before values. so controller will know it all"). It is the
   same rule the editor's own wrapping uses. */
static nc_result_t nc_preset_put_row(nc_preset_out_t *out, const char *row)
{
    nc_result_t r;

    if (!out || !out->doc || !row) {
        return NC_ERR_BAD_ARG;
    }
    if (row[0] == ' ' && out->last != (size_t)-1) {
        char joined[NC_MAX_LINE_LEN];
        nc_word_t words[24];
        int count;
        int n = snprintf(joined, sizeof(joined), "%s%s",
                         out->doc->lines[out->last].text, row);

        if (n <= 0 || n >= (int)sizeof(joined)) {
            return NC_ERR_LINE_TOO_LONG;
        }
        r = nc_set_line(out->doc, out->last, joined);
        if (r == NC_OK) {
            /* The word just written is left picked, so the number is typed
               straight into it - which is the whole reason a row can start with
               a space. The cursor stays on the line the operator was on. */
            count = nc_parse_words(joined, words, 24);
            out->doc->selected_word = count > 0 ? count - 1 : -1;
            out->appended = true;
        }
    } else {
        r = nc_insert_line(out->doc, out->at, row);
        if (r == NC_OK) {
            out->last = out->at;
            if (out->first == (size_t)-1) {
                out->first = out->at;
            }
            out->at++;
        }
    }
    if (r == NC_OK) {
        out->wrote = true;
    }
    return r;
}

/* The rows a compiled entry carries, which are `\n` separated in the table. An
   entry with empty rows writes one empty line - "a new line" is an entry like
   any other. */
static nc_result_t nc_preset_put_text(nc_preset_out_t *out, const char *rows)
{
    const char *row = rows;

    for (;;) {
        const char *end = strchr(row, '\n');
        size_t len = end ? (size_t)(end - row) : strlen(row);
        char line[NC_MAX_LINE_LEN];
        nc_result_t r;

        if (len >= sizeof(line)) {
            len = sizeof(line) - 1u;
        }
        memcpy(line, row, len);
        line[len] = '\0';
        r = nc_preset_put_row(out, line);
        if (r != NC_OK || !end) {
            return r;
        }
        row = end + 1;
    }
}

/* One entry file: the first row is the name, every row after it is written, and
   a file with no row after the name is not an entry (the rows are the mandatory
   half of the model). `out` may be NULL, which is how the settle reads a name
   without writing anything. False when the file cannot be read. */
static bool nc_preset_read_file(int id, char *name, size_t name_sz,
                                nc_preset_out_t *out, bool *has_rows)
{
    char path[NC_MAX_LINE_LEN];
    char row[NC_MAX_LINE_LEN];
    fs_file_t *fp;
    size_t used = 0u;
    bool first = true;
    bool ok = true;

    if (name && name_sz > 0u) {
        name[0] = '\0';
    }
    if (has_rows) {
        *has_rows = false;
    }
    if (!nc_preset_path(id, path, sizeof(path))) {
        return false;
    }
    fp = fs_open(path, "r");
    if (!fp) {
        return false;
    }
    while (fs_available(fp)) {
        char c;

        if (fs_read(fp, (uint8_t *)&c, 1u) != 1u) {
            ok = false;
            break;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n' || used + 1u >= sizeof(row)) {
            row[used] = '\0';
            if (first) {
                first = false;
                if (name && name_sz > 0u) {
                    strncpy(name, row, name_sz - 1u);
                    name[name_sz - 1u] = '\0';
                }
            } else {
                if (has_rows) {
                    *has_rows = true;
                }
                if (out && nc_preset_put_row(out, row) != NC_OK) {
                    ok = false;
                    break;
                }
            }
            used = 0u;
            /* A row longer than the buffer is cut here: the panel's own line is
               shorter than this buffer, so what is dropped could not be typed
               into the program anyway. */
            continue;
        }
        row[used++] = c;
    }
    if (ok && used > 0u) {
        row[used] = '\0';
        if (first) {
            if (name && name_sz > 0u) {
                strncpy(name, row, name_sz - 1u);
                name[name_sz - 1u] = '\0';
            }
        } else {
            if (has_rows) {
                *has_rows = true;
            }
            if (out && nc_preset_put_row(out, row) != NC_OK) {
                ok = false;
            }
        }
    }
    fs_close(fp);
    return ok;
}

/* --- the folder, read once ------------------------------------------------ */

/* Read what the folder says about every address. Creating the folder is the one
   write the panel makes by itself, and only once: it is where the operator's
   entries will go, and a card with no folder answers with the compiled table. */
static bool nc_presets_scan(void)
{
    fs_file_info_t info;
    char name[NC_PRESET_NAME_LEN];
    int id;

    if (!fs_finfo(NC_PRESET_DIR, &info)) {
        return fs_mkdir(NC_PRESET_DIR);
    }
    memset(g_nc_preset_there, 0, sizeof(g_nc_preset_there));
    memset(g_nc_preset_names, 0, sizeof(g_nc_preset_names));
    for (id = NC_PRESET_ADDR_FIRST; id <= NC_PRESET_ADDR_LAST; id++) {
        int slot = nc_preset_slot(id);
        bool rows = false;

        if (!nc_preset_read_file(id, name, sizeof(name), 0, &rows) || !rows) {
            continue;               /* no file, or no rows after the name */
        }
        g_nc_preset_there[slot] = true;
        strncpy(g_nc_preset_names[slot], name, sizeof(g_nc_preset_names[0]) - 1u);
    }
    return true;
}

bool nc_presets_init(void)
{
    g_nc_presets_settled = false;
    return nc_presets_sync();
}

bool nc_presets_sync(void)
{
    if (g_nc_presets_settled) {
        return true;
    }
    /* A drive that cannot answer yet (no card, or one inserted later) is tried
       again on the next call; the compiled entries are in use until then. */
    if (!nc_presets_scan()) {
        return false;
    }
    g_nc_presets_settled = true;
    return true;
}

bool nc_insert_preset_id(nc_document_t *doc, int id)
{
    nc_preset_out_t out;
    int slot;

    if (!doc) {
        return false;
    }
    (void)nc_presets_sync();
    out.doc = doc;
    out.at = doc->cursor_line + 1u;
    if (doc->line_count == 0u) {
        out.at = 0u;
    }
    out.last = out.at > 0u ? out.at - 1u : (size_t)-1;
    out.first = (size_t)-1;
    out.wrote = false;
    out.appended = false;
    slot = nc_preset_slot(id);
    if (slot >= 0 && g_nc_preset_there[slot]) {
        /* The operator's own file for this address: read it and write it. */
        if (!nc_preset_read_file(id, 0, 0u, &out, 0)) {
            return false;
        }
    } else {
        const char *rows = nc_preset_builtin_rows(id);

        if (!rows) {
            return false;
        }
        if (nc_preset_put_text(&out, rows) != NC_OK) {
            return false;
        }
    }
    /* The cursor lands on the first line the entry wrote, and a word left
       picked by a continuing row stays picked (`--presettest` checks both). */
    if (out.first != (size_t)-1) {
        doc->cursor_line = out.first;
        if (!out.appended) {
            doc->selected_word = -1;
        }
    }
    return out.wrote;
}

bool nc_preset_name_for_id(int id, char *out, size_t out_sz)
{
    const nc_preset_builtin_t *builtin;
    int slot = nc_preset_slot(id);

    if (!out || out_sz == 0u) {
        return false;
    }
    out[0] = '\0';
    (void)nc_presets_sync();
    /* The file's first row names the entry; an entry file with an empty first
       row keeps the compiled name, and an address with no file at all answers
       with the compiled entry. */
    if (slot >= 0 && g_nc_preset_there[slot] && g_nc_preset_names[slot][0]) {
        strncpy(out, g_nc_preset_names[slot], out_sz - 1u);
        out[out_sz - 1u] = '\0';
        return true;
    }
    builtin = nc_preset_builtin(id);
    if (!builtin || !builtin->name[0]) {
        return false;
    }
    strncpy(out, builtin->name, out_sz - 1u);
    out[out_sz - 1u] = '\0';
    return true;
}
