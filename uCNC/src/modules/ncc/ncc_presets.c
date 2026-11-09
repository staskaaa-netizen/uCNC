/* The entries, held in RAM.

   One big table in flash is the whole of what the panel ships, and the pad
   reads a *copy* of it: `ncc_presets_init()` lays the shipped entries into a
   RAM table at boot, so no key press ever touches the card. A card may carry
   one file, `/D/ncc_presets.txt`, and it is read once at boot too - each
   `[address] name` block replaces that address, and everything the file does
   not name keeps the shipped entry.

   This replaces the folder of files nc2 seeded onto the card. The card is not
   written here at all, and no address is ever looked up on the drive while the
   operator is working. `ncc/README.md` has the format and the reasoning. */
#include "ncc_presets.h"

#include "../file_system.h"

#include <stdio.h>
#include <string.h>

/* The shipped entries: the whole of the fallback, in one file, and the only
   place these addresses and rows exist. `rows == NULL` is a slot that holds
   only the things under it (a group), and `rows == ""` is an entry that writes
   one empty line - "a new line" is an entry like any other. */
typedef struct {
    const char *address;
    const char *name;
    const char *rows;
} ncc_builtin_t;

static const ncc_builtin_t g_ncc_builtins[] = {
    { "1", "OPS", 0 },
    { "2", "TOOL", 0 },
    { "3", "WORD", 0 },
    { "4", "G7X", 0 },
    { "5", "THREAD", 0 },
    { "6", "PECK", 0 },
    { "7", "TABLE", 0 },

    /* The table's own group, which is what the TOOLS screen's pad offers: `1`
       writes a new row of the table, and the digits type the tool's number over
       its `T`. The keys after it are **not** entries: they pick a word of the
       row the cursor is on, in the row's own order, so `2` picks the `R`. The
       row is `T` and `R` only; nine fixed "add T<n>" rows were a number in a
       template and nothing else. */
    { "71", "NEW", "T1 R0.8 F120 Q60 D2.0 E0.5 S800 X0 Z0" },
    /* The keys after `1` name the word they pick, in the row's own order. They
       write nothing - they are the row editor's field keys - so their entries
       are labels and nothing else. The orientation word is gone; the rest fill
       keys 2..9. */
    { "72", "R", 0 },
    { "73", "F", 0 },
    { "74", "Q", 0 },
    { "75", "D", 0 },
    { "76", "E", 0 },
    { "77", "S", 0 },
    { "78", "X", 0 },
    { "79", "Z", 0 },

    /* The setup rows, one key each: the whole block, and the three the operator
       reaches for on their own. */
    { "11", "SETUP", "G970 X0 U0 Z0 W0\nG971 X0 Z0 I0 E0\nG972 C0\nG973 P0" },
    { "14", "VIEW", "G970 X0 U0 Z0 W0" },
    { "15", "STOCK", "G971 X0 Z0 I0 E0" },
    { "16", "CLAMP", "G972 C0" },
    /* The tool and the machine commands. `M2` ends the program - the bench
       found `M5` in that slot wrote a spindle stop where a program end was
       meant (2026-09-28) - and the three compensation words are the panel's
       own: it applies them, the controller only parses the group. */
    /* A lone letter is finished by the editor's "a letter with no number is a
       field waiting for a number" rule: the operator types the tool's number
       over the empty value. The line is its own - a tool call is a line, and the
       bench said so about `Tn` (2026-09-28: "Tn insertes in same line - but it
       sould not"). */
    { "21", "Tn", "T" },
    { "22", "T0", "T0" },
    { "23", "M6", "M6" },
    { "24", "M3", "M3 S1000" },
    { "25", "M2", "M2" },
    { "26", "M4", "M4 S1000" },
    { "27", "G40", "G40" },
    { "28", "G41", "G41" },
    { "29", "G42", "G42" },
    /* The two words that *finish* a line the operator is on rather than start
       one: a leading space is the pad's own join - at the end of that line for
       `F`, at its front for `N`, where a line number belongs (bench, 2026-09-28:
       "appends are append for all plus prepend for n"). `G` and `M` write lines
       of their own, like `T`: a bare word with its value picked. */
    { "31", "Gn", "G" },
    { "36", "Nn", " N" },
    { "37", "Fn", " F" },
    { "34", "Mn", "M" },
    /* G7X: the cycles, and the two corner words a *profile* carries - `C` and
       `R` mean nothing outside one, which is where the bench's `G1 X50 Z5 C0
       R0` came from. */
    { "41", "G71", "G71 U0 R0 X0 Z0 F0 P0 Q0" },
    { "42", "G72", "G72 W0 R0 X0 Z0 F0 P0 Q0" },
    { "43", "FACE", "G72 W0 R0 X0 Z0 F0 P0 Q0" },
    { "44", "CHMF", " C0" },
    /* The row with nothing on it: a line of its own for the operator to type
       into. nc shipped this ability at `[11]`; with the stock block there, this
       is the key that keeps it. The path builder itself is `47` - a key that
       *does* something rather than one that writes a row, so it is not here. */
    { "45", "PATH", "" },
    { "46", "RND", " R0" },
    { "48", "G70", "G70 P0 Q0" },
    { "49", "G80", "G80" },
    { "51", "THREAD OD", "G76 X0 Z0 P0 Q0 F0 I0 L0 R0" },
    { "52", "THREAD ID", "G76 X0 Z0 P0 Q0 F0 I-0.2 L0 R0" },
    { "53", "TAP", "G33 X0 Z0 K0 F0" },
    { "61", "DRILL", "G1 X0 Z0 F0" },
    { "62", "PECK", "G1 X0 Z0 F0" },
    { "63", "DWELL", "G4 P0" }
};

/* How many RAM slots the pad can hold. The shipped table is 37 and the pad is
   three levels deep, so this is the ceiling a card file can grow into before an
   address is refused rather than silently dropped. */
#define NCC_PRESET_MAX 64

typedef struct {
    char address[NCC_PRESET_ADDR_MAX + 1];
    char name[NCC_PRESET_NAME_MAX];
    char rows[NCC_PRESET_ROWS_MAX];
    bool entry;                 /* it writes rows of its own */
} ncc_entry_t;

static ncc_entry_t g_ncc_entry[NCC_PRESET_MAX];
static int g_ncc_count;
static bool g_ncc_settled;      /* the drive has answered at least once */

/* --- the RAM table ------------------------------------------------------- */

static bool ncc_address_ok(const char *address)
{
    size_t i;
    size_t len;

    if (!address || !address[0]) {
        return false;
    }
    len = strlen(address);
    if (len > NCC_PRESET_ADDR_MAX) {
        return false;
    }
    for (i = 0u; i < len; i++) {
        if (address[i] < '1' || address[i] > '9') {
            return false;
        }
    }
    return true;
}

static ncc_entry_t *ncc_find(const char *address)
{
    int i;

    for (i = 0; i < g_ncc_count; i++) {
        if (strcmp(g_ncc_entry[i].address, address) == 0) {
            return &g_ncc_entry[i];
        }
    }
    return 0;
}

/* The entry at `address`, added when it is not there yet. NULL when the table is
   full: an address that cannot be held is refused, never quietly forgotten. */
static ncc_entry_t *ncc_touch(const char *address)
{
    ncc_entry_t *e;

    if (!ncc_address_ok(address)) {
        return 0;
    }
    e = ncc_find(address);
    if (e) {
        return e;
    }
    if (g_ncc_count >= NCC_PRESET_MAX) {
        return 0;
    }
    e = &g_ncc_entry[g_ncc_count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->address, sizeof(e->address), "%s", address);
    return e;
}

static void ncc_put(ncc_entry_t *e, const char *name, const char *rows)
{
    if (name && name[0]) {
        snprintf(e->name, sizeof(e->name), "%s", name);
    }
    if (rows) {
        snprintf(e->rows, sizeof(e->rows), "%s", rows);
        e->entry = true;
    }
}

static void ncc_load_builtins(void)
{
    size_t i;

    g_ncc_count = 0;
    for (i = 0u; i < sizeof(g_ncc_builtins) / sizeof(g_ncc_builtins[0]); i++) {
        const ncc_builtin_t *b = &g_ncc_builtins[i];
        ncc_entry_t *e = ncc_touch(b->address);

        if (!e) {
            break;
        }
        ncc_put(e, b->name, b->rows);
    }
}

/* --- the one card file --------------------------------------------------- */

/* One line of the card file, `\r` dropped and the buffer cut at its size the way
   the reader the panel uses everywhere does. `line` is always terminated. */
static bool ncc_read_line(fs_file_t *fp, char *line, size_t line_sz)
{
    size_t used = 0u;

    while (fs_available(fp)) {
        char c;

        if (fs_read(fp, (uint8_t *)&c, 1u) != 1u) {
            break;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            break;
        }
        if (used + 1u < line_sz) {
            line[used++] = c;
        }
    }
    line[used] = '\0';
    return used > 0u || fs_available(fp) > 0;
}

/* One `[address] name` block: the rows under it are what the key writes, and a
   row that starts with a space continues the row above. A block with no rows
   keeps the shipped rows for that address, so naming an address only to change
   its name cannot wipe the entry. */
typedef struct {
    bool open;
    char address[NCC_PRESET_ADDR_MAX + 1];
    char name[NCC_PRESET_NAME_MAX];
    char rows[NCC_PRESET_ROWS_MAX];
    size_t used;
} ncc_block_t;

static void ncc_block_flush(ncc_block_t *b)
{
    ncc_entry_t *e;

    if (!b->open) {
        return;
    }
    e = ncc_touch(b->address);
    if (e) {
        b->rows[b->used] = '\0';
        ncc_put(e, b->name, b->used > 0u ? b->rows : 0);
    }
    b->open = false;
}

static void ncc_block_row(ncc_block_t *b, const char *row)
{
    size_t len = strlen(row);
    bool continues = row[0] == ' ' && b->used > 0u;

    if (!b->open) {
        return;
    }
    if (continues) {
        /* Joined straight onto the row above: a value that belongs on a line
           already written must not put a break inside a block the controller
           reads as one. */
        if (b->used + len >= sizeof(b->rows)) {
            return;
        }
        memcpy(b->rows + b->used, row, len);
        b->used += len;
        return;
    }
    if (b->used + len + 2u >= sizeof(b->rows)) {
        return;
    }
    if (b->used > 0u) {
        b->rows[b->used++] = '\n';
    }
    memcpy(b->rows + b->used, row, len);
    b->used += len;
}

static void ncc_block_header(ncc_block_t *b, const char *line)
{
    const char *close = strchr(line, ']');
    const char *name;
    size_t len;

    if (!close) {
        return;
    }
    ncc_block_flush(b);
    len = (size_t)(close - (line + 1));
    if (len == 0u || len > NCC_PRESET_ADDR_MAX) {
        return;
    }
    memcpy(b->address, line + 1, len);
    b->address[len] = '\0';
    if (!ncc_address_ok(b->address)) {
        b->address[0] = '\0';
        return;
    }
    b->name[0] = '\0';
    b->used = 0u;
    b->open = true;
    name = close + 1;
    while (*name == ' ' || *name == '\t') {
        name++;
    }
    if (*name) {
        snprintf(b->name, sizeof(b->name), "%s", name);
    }
}

/* Read the card's one file over the shipped table. False only when the file is
   there and could not be read - a card with no file is not an error, it is a
   card with the shipped entries. */
static bool ncc_load_card(void)
{
    char line[NCC_PRESET_ROW_MAX + 8];
    ncc_block_t block;
    fs_file_t *fp = fs_open(NCC_PRESET_FILE, "r");

    if (!fp) {
        return true;
    }
    memset(&block, 0, sizeof(block));
    while (ncc_read_line(fp, line, sizeof(line))) {
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line[0] == '[') {
            ncc_block_header(&block, line);
            continue;
        }
        if (block.open) {
            ncc_block_row(&block, line);
        }
    }
    ncc_block_flush(&block);
    fs_close(fp);
    return true;
}

/* --- the panel's side ---------------------------------------------------- */

void ncc_presets_init(void)
{
    g_ncc_settled = false;
    ncc_load_builtins();
}

bool ncc_presets_sync(void)
{
    fs_file_t *root;

    if (g_ncc_settled) {
        return true;
    }
    /* The card is mounted long after module init: until the drive answers, the
       shipped table is what the pad reads, and the file is tried again on the
       next call. The probe is the card's own root, which is what the file list
       opens (`ncc_files.c`), so a drive that answers here is one that can hand
       the file over. */
    root = fs_opendir("/D");
    if (!root) {
        return false;
    }
    fs_close(root);
    (void)ncc_load_card();
    g_ncc_settled = true;
    return true;
}

int ncc_preset_read(const char *address, char *name, size_t name_sz,
                    char *rows, size_t rows_sz)
{
    ncc_entry_t *e;
    const char *row;
    int count = 1;

    if (name && name_sz > 0u) {
        name[0] = '\0';
    }
    if (rows && rows_sz > 0u) {
        rows[0] = '\0';
    }
    if (!ncc_address_ok(address)) {
        return -1;
    }
    e = ncc_find(address);
    if (!e) {
        return -1;
    }
    if (name && name_sz > 0u) {
        snprintf(name, name_sz, "%s", e->name);
    }
    if (!e->entry) {
        return 0;
    }
    if (rows && rows_sz > 0u) {
        snprintf(rows, rows_sz, "%s", e->rows);
    }
    for (row = e->rows; *row; row++) {
        if (*row == '\n') {
            count++;
        }
    }
    return count;
}

void ncc_address_reset(char *address)
{
    if (address) {
        address[0] = '\0';
    }
}

bool ncc_address_push(char *address, char digit)
{
    size_t len;

    if (!address || digit < '1' || digit > '9') {
        return false;
    }
    len = strlen(address);
    if (len + 1u > NCC_ADDR_MAX) {
        return false;               /* the pad is three levels deep */
    }
    address[len] = digit;
    address[len + 1u] = '\0';
    return true;
}

void ncc_address_pop(char *address)
{
    size_t len;

    if (!address) {
        return;
    }
    len = strlen(address);
    if (len > 0u) {
        address[len - 1u] = '\0';
    }
}

/* True when anything lives under `address` - which is what makes it a pad
   rather than a row of program text. Asked of the RAM table, so it is a walk
   and never a card access. */
static bool ncc_has_children(const char *address)
{
    size_t len = strlen(address);
    int i;

    if (len >= NCC_ADDR_MAX) {
        return false;
    }
    for (i = 0; i < g_ncc_count; i++) {
        const char *child = g_ncc_entry[i].address;

        if (strlen(child) > len && strncmp(child, address, len) == 0) {
            return true;
        }
    }
    return false;
}

int ncc_slot(const char *address, char digit, char *label, size_t label_sz)
{
    char child[NCC_ADDR_MAX + 2];
    ncc_entry_t *e;
    int kind = NCC_SLOT_EMPTY;

    if (label && label_sz > 0u) {
        label[0] = '\0';
    }
    if (!address || digit < '1' || digit > '9') {
        return NCC_SLOT_EMPTY;
    }
    snprintf(child, sizeof(child), "%s%c", address, digit);
    e = ncc_find(child);
    if (e && e->entry) {
        kind |= NCC_SLOT_ENTRY;
    }
    if (ncc_has_children(child)) {
        kind |= NCC_SLOT_PAD;
    }
    if (label && label_sz > 0u) {
        if (e && e->name[0]) {
            snprintf(label, label_sz, "%s", e->name);
        } else {
            snprintf(label, label_sz, "%s", child);
        }
    }
    return kind;
}

size_t ncc_preset_builtin_count(void)
{
    return sizeof(g_ncc_builtins) / sizeof(g_ncc_builtins[0]);
}

bool ncc_preset_builtin(size_t index, const char **address, const char **name,
                        const char **rows)
{
    if (index >= ncc_preset_builtin_count()) {
        return false;
    }
    if (address) {
        *address = g_ncc_builtins[index].address;
    }
    if (name) {
        *name = g_ncc_builtins[index].name;
    }
    if (rows) {
        *rows = g_ncc_builtins[index].rows;
    }
    return true;
}
