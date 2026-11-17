#include "ncc_files.h"

#include "../file_system.h"
#include "../g7x/g7x.h"
#include "../../interface/grbl_stream.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How many entries one scan reports to the console. The list a card hands back
   is the one thing a panel cannot show the operator, so the scan says what it
   saw: every name, and whether it became an entry (`+`) or was left out (`-`).
   Bounded, because a card is not a log file - the first twenty say what shape
   the names are in, and the summary line says how many there were. */
#define NCC_LIST_LOG_MAX 20u

/* The end of the path, if it is this suffix - **either case**.

   The card is FAT and the machine's FatFs is built without long filenames
   (`FF_USE_LFN 0`), so what a directory hands back is an 8.3 name: a file
   written on a PC as `lathe-demo.nc` is stored and reported as something like
   `LATHE-~1.NC`, extension in capitals. nc asked the same question the same way
   (`nc_has_suffix_ci()`); ncc compared with strcmp and so quietly dropped every
   program the operator had - the list showed no `.nc` at all, and RUN would not
   open one (bench: "it does not show their names properly nor does it seem to
   open them"). */
bool ncc_path_has_suffix(const char *path, const char *suffix)
{
    size_t path_len;
    size_t suffix_len;
    size_t i;

    if (!path || !suffix) {
        return false;
    }
    path_len = strlen(path);
    suffix_len = strlen(suffix);
    if (suffix_len == 0u || path_len < suffix_len) {
        return false;
    }
    path += path_len - suffix_len;
    for (i = 0u; i < suffix_len; i++) {
        if (toupper((unsigned char)path[i]) !=
            toupper((unsigned char)suffix[i])) {
            return false;
        }
    }
    return true;
}

bool ncc_path_is_program(const char *path)
{
    return ncc_path_has_suffix(path, ".nc");
}

bool ncc_path_is_text(const char *path)
{
    return ncc_path_is_program(path) || ncc_path_has_suffix(path, ".t") ||
           ncc_path_has_suffix(path, ".txt");
}

bool ncc_file_document_is_preview_safe(const ncc_document_t *doc)
{
    bool saw_command = false;
    size_t i;

    if (!doc) {
        return false;
    }
    for (i = 0u; i < doc->line_count; i++) {
        const char *line = doc->lines[i];
        const char *p = line;
        ncc_field_t fields[NCC_MAX_FIELDS];
        int count;
        int first = -1;
        int j;
        char *end;
        double value;

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p || *p == '(' || *p == ';' || *p == '%' || *p == '$') {
            continue;
        }
        count = ncc_fields(line, fields, NCC_MAX_FIELDS);
        for (j = 0; j < count; j++) {
            if (toupper((unsigned char)fields[j].letter) != 'N') {
                first = j;
                break;
            }
        }
        /* Macro-like text such as SETUP or TOOLCALL has no numeric value after
           its first command letter. It is not a program the preview may parse. */
        if (first < 0 || fields[first].value == fields[first].end) {
            return false;
        }
        for (j = 0; j < count; j++) {
            if (fields[j].value == fields[j].end) {
                continue;
            }
            value = strtod(line + fields[j].value, &end);
            if (end == line + fields[j].value || !isfinite(value) ||
                fabs(value) > 1000000.0) {
                return false;
            }
        }
        if (toupper((unsigned char)fields[first].letter) == 'G' ||
            nc_sender_emit_line_is_direct(g7x_skip_line_number(p))) {
            saw_command = true;
            continue;
        }
        {
            g7x_contour_cmd_t cmd =
                g7x_contour_cmd_from_line(g7x_skip_line_number(p));

            if (cmd == G7X_CONTOUR_NONE || cmd == G7X_CONTOUR_END) {
                return false;
            }
        }
        saw_command = true;
    }
    return saw_command;
}

/* One line at a time, cut at the panel's own width: a longer line could not be
   shown or typed into, so what the panel cannot work with is not loaded as if it
   could. A carriage return is dropped - the card may have been written on a PC. */
bool ncc_file_load(ncc_document_t *doc, const char *path)
{
    ncc_document_t loaded;
    fs_file_t *fp;
    char line[NCC_MAX_LINE_LEN];
    size_t used = 0u;

    if (!doc || !path || !path[0]) {
        return false;
    }
    fp = fs_open(path, "r");
    if (!fp) {
        return false;
    }
    ncc_document_init(&loaded);
    snprintf(loaded.path, sizeof(loaded.path), "%s", path);
    while (fs_available(fp) > 0) {
        char c;

        if (fs_read(fp, (uint8_t *)&c, 1u) != 1u) {
            fs_close(fp);
            return false;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            line[used] = '\0';
            if (!ncc_insert_line(&loaded, loaded.line_count, line)) {
                fs_close(fp);
                return false;
            }
            used = 0u;
            continue;
        }
        if (used + 1u < sizeof(line)) {
            line[used++] = c;
        }
    }
    fs_close(fp);
    if (used > 0u) {
        line[used] = '\0';
        if (!ncc_insert_line(&loaded, loaded.line_count, line)) {
            return false;
        }
    }
    if (loaded.line_count == 0u) {
        /* A file with nothing in it is a program with nothing in it: the editor
           needs one line to put the cursor on, which is what a new file is. */
        if (!ncc_insert_line(&loaded, 0u, "")) {
            return false;
        }
    }
    loaded.cursor = 0u;
    loaded.dirty = false;
    *doc = loaded;
    return true;
}

bool ncc_file_save(const ncc_document_t *doc)
{
    fs_file_t *fp;
    size_t i;

    if (!doc || !doc->path[0]) {
        return false;
    }
    fp = fs_open(doc->path, "w");
    if (!fp) {
        return false;
    }
    for (i = 0u; i < doc->line_count; i++) {
        size_t len = strlen(doc->lines[i]);

        if ((len && fs_write(fp, (const uint8_t *)doc->lines[i], len) != len) ||
            fs_write(fp, (const uint8_t *)"\n", 1u) != 1u) {
            fs_close(fp);
            return false;
        }
    }
    fs_close(fp);
    return true;
}

/* --- the list ------------------------------------------------------------- */

static ncc_file_entry_t g_ncc_entries[NCC_FILES_MAX];
static int g_ncc_entry_count;
static int g_ncc_selected;
static char g_ncc_dir[NCC_PATH_MAX];
static char g_ncc_new_name[NCC_NAME_MAX];
static bool g_ncc_new_active;

static bool ncc_entry_less(const ncc_file_entry_t *a, const ncc_file_entry_t *b)
{
    if (a->is_dir != b->is_dir) {
        return a->is_dir;           /* folders first */
    }
    if (strcmp(a->name, "..") == 0) {
        return true;                /* and up is always at the top */
    }
    return strcmp(a->name, b->name) < 0;
}

/* Can the panel show and open this name at all?

   nc asked this of every entry it listed (`nc_files_valid_entry_name()`), and
   ncc did not - which is how a card whose driver hands back an entry the panel
   cannot use put that entry on the glass. Printable ASCII only, and not the
   two names a directory walk uses for itself: a byte the panel would draw as a
   box, or a name a path cannot carry, is not a file the operator can open. */
static bool ncc_name_is_usable(const char *name)
{
    const char *p;
    bool has_text = false;

    if (!name || !name[0] || strcmp(name, ".") == 0 || strcmp(name, "/") == 0) {
        return false;
    }
    for (p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;

        if (c < 33u || c > 126u || c == '/' || c == '\\') {
            return false;
        }
        has_text = true;
    }
    return has_text;
}

bool ncc_file_scan(const char *dir)
{
    fs_file_t *dp;
    unsigned shown = 0u;
    unsigned skipped = 0u;
    char keep[NCC_NAME_MAX];
    bool keep_selected;

    if (!dir || !dir[0]) {
        return false;
    }
    /* A refresh is not a move: the folder is the same one, so the entry the
       operator was standing on stays picked. It used to fall back to the first
       entry of the new list - which is `..` in any folder under the root - so
       the mark jumped off the file they were about to open (bench: "on refresh
       button pressed - it select next file"). A *change* of folder is a
       different thing and starts with nothing picked but the list itself. */
    keep_selected = strcmp(dir, g_ncc_dir) == 0 && g_ncc_selected >= 0 &&
                    g_ncc_selected < g_ncc_entry_count;
    keep[0] = '\0';
    if (keep_selected) {
        snprintf(keep, sizeof(keep), "%s", g_ncc_entries[g_ncc_selected].name);
    }
    g_ncc_entry_count = 0;
    g_ncc_selected = 0;
    snprintf(g_ncc_dir, sizeof(g_ncc_dir), "%s", dir);
    /* Up one level, unless this is the card's own root. */
    if (strcmp(dir, "/D") != 0) {
        snprintf(g_ncc_entries[g_ncc_entry_count].name, NCC_NAME_MAX, "%s", "..");
        g_ncc_entries[g_ncc_entry_count].is_dir = true;
        g_ncc_entry_count++;
    }
    dp = fs_opendir(g_ncc_dir);
    if (!dp) {
        grbl_stream_printf("[MSG:NCC list %.32s: cannot read it]\r\n",
                           g_ncc_dir);
        return false;
    }
    while (g_ncc_entry_count < NCC_FILES_MAX) {
        fs_file_info_t info;
        const char *slash;
        const char *name;

        /* Every field of the info is the driver's to write, but the name goes
           in first and a driver that only *appended* to it would grow a path
           out of the last entry's. nc cleared it before every call
           (`memset(&info, 0, ...)`); ncc did not, and clears it here. */
        memset(&info, 0, sizeof(info));
        if (!fs_next_file(dp, &info)) {
            break;
        }
        slash = strrchr(info.full_name, '/');
        name = slash ? slash + 1 : info.full_name;
        if (!ncc_name_is_usable(name) ||
            (!info.is_dir && !ncc_path_is_text(name))) {
            skipped++;
            if (shown < NCC_LIST_LOG_MAX) {
                grbl_stream_printf("[MSG:NCC list - %.48s]\r\n", name);
                shown++;
            }
            continue;               /* folders, and what the editor can open */
        }
        if (shown < NCC_LIST_LOG_MAX) {
            grbl_stream_printf("[MSG:NCC list + %.48s]\r\n", name);
            shown++;
        }
        snprintf(g_ncc_entries[g_ncc_entry_count].name, NCC_NAME_MAX, "%s", name);
        g_ncc_entries[g_ncc_entry_count].is_dir = info.is_dir;
        g_ncc_entry_count++;
    }
    fs_close(dp);
    grbl_stream_printf("[MSG:NCC list %.32s: %d entries, %u left out]\r\n",
                       g_ncc_dir, g_ncc_entry_count, skipped);
    /* Sorted, because the card's order is whatever the drive feels like and a
       list that reshuffles under a finger is a list that gets things deleted. */
    {
        int i;
        int j;

        for (i = 1; i < g_ncc_entry_count; i++) {
            ncc_file_entry_t hold = g_ncc_entries[i];

            for (j = i; j > 0 && ncc_entry_less(&hold, &g_ncc_entries[j - 1]);
                 j--) {
                g_ncc_entries[j] = g_ncc_entries[j - 1];
            }
            g_ncc_entries[j] = hold;
        }
    }
    if (keep_selected && keep[0]) {
        int i;

        for (i = 0; i < g_ncc_entry_count; i++) {
            if (strcmp(g_ncc_entries[i].name, keep) == 0) {
                g_ncc_selected = i;
                break;
            }
        }
    }
    return true;
}

const char *ncc_file_dir(void)
{
    return g_ncc_dir;
}

int ncc_file_count(void)
{
    return g_ncc_entry_count;
}

const ncc_file_entry_t *ncc_file_entry(int index)
{
    if (index < 0 || index >= g_ncc_entry_count) {
        return 0;
    }
    return &g_ncc_entries[index];
}

int ncc_file_selected(void)
{
    return g_ncc_selected;
}

void ncc_file_step(int delta)
{
    int at = g_ncc_selected + delta;

    if (g_ncc_entry_count == 0) {
        return;
    }
    if (at < 0) {
        at = 0;
    }
    if (at >= g_ncc_entry_count) {
        at = g_ncc_entry_count - 1;
    }
    g_ncc_selected = at;
}

/* The path of the entry at `index` - `..` is a *path* like any other, the
   folder above this one, and walking there is the caller's scan. It used to
   scan here as well and return without writing `out`, so the caller scanned
   whatever its uninitialised buffer held: a card that answered that with a
   directory got a list nobody asked for, and one that did not got a garbage
   path into the driver (bench: "it is stuck"). */
static bool ncc_file_path_at(int index, char *out, size_t out_sz)
{
    const ncc_file_entry_t *entry = ncc_file_entry(index);
    int n;

    if (!entry || !out || out_sz == 0u) {
        return false;
    }
    if (strcmp(entry->name, "..") == 0) {
        const char *slash = strrchr(g_ncc_dir, '/');

        if (!slash || slash == g_ncc_dir) {
            return false;
        }
        n = snprintf(out, out_sz, "%.*s", (int)(slash - g_ncc_dir), g_ncc_dir);
        return n > 0 && (size_t)n < out_sz;
    }
    n = snprintf(out, out_sz, "%s/%s", g_ncc_dir, entry->name);
    return n > 0 && (size_t)n < out_sz;
}

bool ncc_file_selected_path(char *out, size_t out_sz)
{
    return ncc_file_path_at(g_ncc_selected, out, out_sz);
}

/* Do two paths name the same entry? The card is FAT with short names only, so
   the name the panel remembers and the name the driver hands back can differ in
   case (`lathe-demo.nc` is read as `LATHE-~1.NC`), and the extension question
   is answered the same way everywhere else. */
static bool ncc_path_same(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Land the selection on `path`, when it is in the folder being listed: this is
   what makes the file list open *on* the file the editor has, instead of on
   whatever happens to sort first. False when the path is not there, and the
   selection is then left where the scan put it. */
bool ncc_file_select_path(const char *path)
{
    int i;

    if (!path || !path[0]) {
        return false;
    }
    for (i = 0; i < g_ncc_entry_count; i++) {
        char candidate[NCC_PATH_MAX];

        if (ncc_file_path_at(i, candidate, sizeof(candidate)) &&
            ncc_path_same(candidate, path)) {
            g_ncc_selected = i;
            return true;
        }
    }
    return false;
}

bool ncc_file_delete_selected(void)
{
    const ncc_file_entry_t *entry = ncc_file_entry(g_ncc_selected);
    char path[NCC_PATH_MAX];

    if (!entry || entry->is_dir || strcmp(entry->name, "..") == 0) {
        return false;
    }
    snprintf(path, sizeof(path), "%s/%s", g_ncc_dir, entry->name);
    if (!fs_remove(path)) {
        return false;
    }
    return ncc_file_scan(g_ncc_dir);
}

bool ncc_file_create(const char *name, const char *ext, char *out, size_t out_sz)
{
    char path[NCC_PATH_MAX];
    fs_file_t *fp;
    int n;

    if (!name || !name[0] || !ext) {
        return false;
    }
    n = snprintf(path, sizeof(path), "%s/%s%s", g_ncc_dir, name, ext);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        return false;
    }
    fp = fs_open(path, "w");
    if (!fp) {
        return false;
    }
    fs_close(fp);
    if (out && out_sz > 0u) {
        snprintf(out, out_sz, "%s", path);
    }
    return true;
}

void ncc_file_new_begin(void)
{
    g_ncc_new_name[0] = '\0';
    g_ncc_new_active = true;
}

void ncc_file_new_end(void)
{
    g_ncc_new_active = false;
    g_ncc_new_name[0] = '\0';
}

bool ncc_file_new_active(void)
{
    return g_ncc_new_active;
}

void ncc_file_new_digit(char digit)
{
    size_t len = strlen(g_ncc_new_name);

    if (len + 1u >= sizeof(g_ncc_new_name) || digit < '0' || digit > '9') {
        return;
    }
    g_ncc_new_name[len] = digit;
    g_ncc_new_name[len + 1u] = '\0';
}

void ncc_file_new_backspace(void)
{
    size_t len = strlen(g_ncc_new_name);

    if (len > 0u) {
        g_ncc_new_name[len - 1u] = '\0';
    }
}

const char *ncc_file_new_name(void)
{
    return g_ncc_new_name;
}
