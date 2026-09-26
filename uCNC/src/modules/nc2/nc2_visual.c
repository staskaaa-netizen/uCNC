#include "nc2_visual.h"

#include "nc2.h"
#include "nc2_boot.h"
#include "nc2_draw.h"
#include "nc2_files.h"
#include "nc2_layout.h"
#include "nc2_preview.h"
#include "nc2_presets.h"
#include "nc2_state.h"

#include "../../cnc.h"
#include "../lvds_renderer/lvds_hstx.h"

#include <stdio.h>
#include <string.h>

static nc2_document_t g_doc;
static char g_address[NC2_ADDR_MAX + 1];
static char g_status[64];
static char g_labels[9][NC2_PRESET_ROW_MAX];
static bool g_dirty;
static bool g_list;                 /* the file list is the screen */

static void nc2_statusf(const char *text)
{
    snprintf(g_status, sizeof(g_status), "%s", text ? text : "");
}

/* The nine labels of the address the operator is standing at. Read every frame
   the pad is drawn: nine small files on a card that answers in microseconds, and
   a cache would be a second copy of what the card already says. */
static void nc2_labels_at(const char *address)
{
    char key;

    for (key = '1'; key <= '9'; key++) {
        int kind = nc2_slot(address, key, g_labels[key - '1'],
                            sizeof(g_labels[0]));

        if (kind == NC2_SLOT_EMPTY) {
            g_labels[key - '1'][0] = '\0';
        }
    }
}

void nc2_visual_init(void)
{
    nc2_draw_init();
    nc2_document_init(&g_doc);
    nc2_address_reset(g_address);
    nc2_statusf("");
    g_dirty = true;
    nc2_state_init();
    /* The one write the panel makes by itself, and only onto a card that has no
       entry of its own: after that every entry is a file. */
    (void)nc2_boot_seed();
    /* Onto the file the panel had open, and where it was in it. A card that
       remembers nothing opens on an empty program, which is a program. */
    if (!nc2_state_load_document(NC2_MODE_PROGRAM, &g_doc)) {
        nc2_document_init(&g_doc);
        (void)nc2_insert_line(&g_doc, 0u, "");
    }
    nc2_labels_at(g_address);
}

bool nc2_visual_open(const char *path)
{
    if (!nc2_file_load(&g_doc, path)) {
        nc2_statusf("Cannot open that file");
        g_dirty = true;
        return false;
    }
    nc2_address_reset(g_address);
    nc2_labels_at(g_address);
    nc2_state_remember_path(NC2_MODE_PROGRAM, g_doc.path);
    nc2_state_flush();
    g_dirty = true;
    return true;
}

const char *nc2_visual_path(void)
{
    return g_doc.path;
}

const char *nc2_visual_screen_name(void)
{
    return g_list ? "FILES" : "EDIT";
}

bool nc2_visual_save(void)
{
    if (!nc2_file_save(&g_doc)) {
        nc2_statusf("Save failed");
        g_dirty = true;
        return false;
    }
    g_doc.dirty = false;
    nc2_state_remember_cursor(&g_doc);
    nc2_state_flush();
    g_dirty = true;
    return true;
}

const char *nc2_visual_address(void)
{
    return g_address;
}

const char *nc2_visual_slot_label(char key)
{
    if (key < '1' || key > '9') {
        return 0;
    }
    return g_labels[key - '1'][0] ? g_labels[key - '1'] : 0;
}

bool nc2_visual_dirty(void)
{
    return g_dirty;
}

void nc2_visual_clear_dirty(void)
{
    g_dirty = false;
}

const char *nc2_visual_status(void)
{
    return g_status;
}

/* The name of the pad the operator is standing in: the file at the address, or
   the digits themselves when nobody wrote one. */
static void nc2_pad_name(char *out, size_t out_sz)
{
    char name[NC2_PRESET_ROW_MAX];

    if (!g_address[0]) {
        snprintf(out, out_sz, "MAIN");
        return;
    }
    if (nc2_preset_read(g_address, name, sizeof(name), 0, 0u) >= 0 && name[0]) {
        snprintf(out, out_sz, "%s", name);
        return;
    }
    snprintf(out, out_sz, "%s", g_address);
}

/* One press of a slot: rows are written where the pad's name stands, children
   are opened, and a slot that is both does both. The pad stays where it is, which
   is what makes a profile one press per point. */
static void nc2_pad_press(char key)
{
    char label[NC2_PRESET_ROW_MAX];
    char rows[NC2_PRESET_ROW_MAX * 4];
    int kind;

    kind = nc2_slot(g_address, key, label, sizeof(label));
    if (kind == NC2_SLOT_EMPTY) {
        return;                     /* nothing there: nothing happens */
    }
    if (kind & NC2_SLOT_ENTRY) {
        char child[NC2_ADDR_MAX + 2];

        snprintf(child, sizeof(child), "%s%c", g_address, key);
        if (nc2_preset_read(child, 0, 0u, rows, sizeof(rows)) > 0) {
            if (!nc2_pad_active(&g_doc) && !nc2_pad_open(&g_doc, label)) {
                nc2_statusf("No room in the program");
                g_dirty = true;
                return;
            }
            (void)nc2_pad_write(&g_doc, rows);
            g_doc.dirty = true;
            nc2_statusf(label);
        }
    }
    if (kind & NC2_SLOT_PAD) {
        if (!nc2_address_push(g_address, key)) {
            return;
        }
        nc2_labels_at(g_address);
        (void)nc2_pad_open(&g_doc, label);
        nc2_statusf("");
        g_dirty = true;
    }
}

void nc2_visual_key(char key)
{
    nc2_key_t editor_key = NC2_KEY_NONE;

    /* The file list is its own screen: a picker, with the keys a picker needs and
       no pad (the pad's digits name a new file while one is being made). */
    if (g_list) {
        char path[NC2_PATH_MAX];
        const nc2_file_entry_t *entry;

        if (nc2_file_new_active()) {
            if (key >= '0' && key <= '9') {
                nc2_file_new_digit(key);
            } else if (key == '*') {
                nc2_file_new_end();
                nc2_statusf("New file cancelled");
            } else if (key == '#' || key == 'D') {
                if (nc2_file_new_name()[0] &&
                    nc2_file_create(nc2_file_new_name(), ".nc", path,
                                    sizeof(path))) {
                    nc2_file_new_end();
                    if (nc2_visual_open(path)) {
                        g_list = false;
                        nc2_statusf("Created and opened");
                    }
                } else {
                    nc2_statusf("Give the new file a number");
                }
            }
            g_dirty = true;
            return;
        }
        switch (key) {
        case 'B':
            nc2_file_step(-1);
            break;
        case 'C':
            nc2_file_step(1);
            break;
        case 'D':
        case '#':
            if (!nc2_file_selected_path(path, sizeof(path))) {
                nc2_statusf("Nothing to open");
                break;
            }
            entry = nc2_file_entry(nc2_file_selected());
            if (entry && entry->is_dir) {
                if (!nc2_file_scan(path)) {
                    nc2_statusf("Cannot read that folder");
                }
                break;
            }
            if (nc2_visual_open(path)) {
                g_list = false;
            }
            break;
        case '5':
            nc2_file_new_begin();
            nc2_statusf("New file: a number, # for OK");
            break;
        case '6':
            if (nc2_file_delete_selected()) {
                nc2_statusf("Deleted");
            } else {
                nc2_statusf("Cannot delete that");
            }
            break;
        case '8':
            if (!nc2_file_scan(nc2_file_dir())) {
                nc2_statusf("Refresh failed");
            }
            break;
        case '*':
        case '0':
            /* `0` is the way out of whatever is up, and the list is one of those
               things: out of the list is the program. */
            g_list = false;
            nc2_statusf("");
            break;
        default:
            break;
        }
        g_dirty = true;
        return;
    }
    if (g_doc.line_count == 0u) {
        return;                     /* no program: the keys have nothing to act on */
    }
    switch (key) {
    case 'B': editor_key = NC2_KEY_UP; break;
    case 'C': editor_key = NC2_KEY_DOWN; break;
    case 'D': editor_key = NC2_KEY_NEXT; break;
    case '#': editor_key = NC2_KEY_ACCEPT; break;
    case '*': editor_key = NC2_KEY_DELETE; break;
    case '0':
        /* `0` is the exit everywhere, so with a pad up it leaves the pad - one
           press out of however deep, which `A` cannot do (that is one level). The
           program itself has nothing to leave, so there it opens the card. */
        if (g_address[0]) {
            nc2_address_reset(g_address);
            nc2_pad_close(&g_doc);
            nc2_labels_at(g_address);
            nc2_statusf("");
            g_dirty = true;
            return;
        }
        if (nc2_file_scan("/D")) {
            g_list = true;
            nc2_statusf("");
        } else {
            nc2_statusf("The card is not answering");
        }
        g_dirty = true;
        return;
    case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        editor_key = NC2_KEY_DIGIT;
        break;
    case 'A':
        /* Up a level. At the root it is the mode key, which is the screens'
           business - there is one screen so far. */
        if (g_address[0]) {
            nc2_address_pop(g_address);
            nc2_pad_close(&g_doc);
            nc2_labels_at(g_address);
            nc2_statusf("");
            g_dirty = true;
        }
        return;
    default:
        return;
    }
    g_dirty = true;
    /* The editor first: with a value picked the digits are the value's, and with
       nothing picked they are the pad's. */
    if (nc2_key(&g_doc, editor_key, key)) {
        return;
    }
    if (editor_key == NC2_KEY_DIGIT) {
        nc2_pad_press(key);
    }
}

void nc2_visual_tick(unsigned ms)
{
    if (nc2_boot_active()) {
        nc2_boot_tick(ms);
        nc2_visual_clear_dirty();
    }
}

/* --- drawing -------------------------------------------------------------- */

static void nc2_draw_header(void)
{
    char line[80];
    const char *path = g_doc.path[0] ? g_doc.path : "(no program)";

    nc2_fill(0, 0, LVDS_HSTX_WIDTH, NC2_HEADER_H, nc2_col_header());
    snprintf(line, sizeof(line), "%s  %s%s", g_list ? "FILES" : "EDIT", path,
             g_doc.dirty ? " *" : "");
    nc2_text_clip(8, 6, line, (LVDS_HSTX_WIDTH - 16) / nc2_col_width(LVDS_FONT_NORMAL),
                  nc2_col_text(), nc2_col_header(), LVDS_FONT_NORMAL);
}

/* The two panes: the program on the left, the drawing on the right, and the line
   between them - the *only* line, because a border on all four sides of each pane
   is four lines saying what one already says. The preview's own drawing is the
   next piece of the module, so its pane is here and the pad sits in its corner
   meanwhile. */
static void nc2_draw_panes(void)
{
    nc2_fill(NC2_LEFT_PANE_X, NC2_PANE_Y, NC2_LEFT_PANE_W, NC2_PANE_H,
             nc2_col_bg());
    nc2_fill(NC2_RIGHT_PANE_X, NC2_PANE_Y, NC2_RIGHT_PANE_W, NC2_PANE_H,
             nc2_col_bg());
    lvds_draw_line(NC2_SPLIT_X, NC2_PANE_Y, NC2_SPLIT_X, NC2_PANE_BOTTOM,
                   nc2_col_dim());
}

/* One row of the program: its number, then the text - and, on the row the cursor
   is on, the picked field drawn as the box being typed into, three pieces so
   everything the operator did not touch keeps its own colour. */
static void nc2_draw_row(size_t index, int y)
{
    const char *text = g_doc.lines[index];
    bool cursor = index == g_doc.cursor;
    lvds_color_t bg = cursor ? nc2_col_select() : nc2_col_bg();
    int col_w = nc2_col_width(LVDS_FONT_NORMAL);
    char number[8];
    int x = NC2_LEFT_PANE_X + NC2_LINE_NO_PAD;
    int cols;
    nc2_field_t fields[NC2_MAX_FIELDS];
    int count;
    int picked = -1;

    if (cursor) {
        nc2_fill(NC2_LEFT_PANE_X, y - 2, NC2_LEFT_PANE_W - 2, NC2_ROW_H - 2,
                 nc2_col_select());
    }
    snprintf(number, sizeof(number), "%3u", (unsigned)(index + 1u));
    nc2_text(x, y, number, nc2_col_dim(), bg, LVDS_FONT_NORMAL);
    x += 4 * col_w;
    cols = (NC2_LEFT_PANE_W - (x - NC2_LEFT_PANE_X) - 4) / col_w;
    if (cols <= 0) {
        return;
    }
    count = nc2_fields(text, fields, NC2_MAX_FIELDS);
    if (cursor && g_doc.field >= 0 && g_doc.field < count) {
        picked = g_doc.field;
    }
    if (picked < 0) {
        nc2_text_clip(x, y, text, cols, nc2_col_text(), bg, LVDS_FONT_NORMAL);
        return;
    }
    {
        const nc2_field_t *f = &fields[picked];
        int head = (int)f->start;
        int field = (int)(f->end - f->start);

        if (head > cols) {
            head = cols;
        }
        if (field > cols - head) {
            field = cols - head;
        }
        nc2_text_clip(x, y, text, head, nc2_col_text(), bg, LVDS_FONT_NORMAL);
        nc2_text_clip(x + head * col_w, y, text + f->start, field,
                      nc2_col_field_fg(), nc2_col_field_bg(), LVDS_FONT_NORMAL);
        nc2_text_clip(x + (head + field) * col_w, y, text + f->end,
                      cols - head - field, nc2_col_text(), bg, LVDS_FONT_NORMAL);
    }
}

static void nc2_draw_program(void)
{
    size_t first = 0u;
    size_t i;

    if (g_doc.cursor >= NC2_CODE_ROWS) {
        first = g_doc.cursor - NC2_CODE_ROWS + 1u;
    }
    for (i = first; i < g_doc.line_count && i - first < NC2_CODE_ROWS; i++) {
        nc2_draw_row(i, NC2_PANE_Y + 4 + (int)(i - first) * NC2_ROW_H);
    }
}

/* The card: what is in the folder the operator is looking at, the one picked lit,
   and the name being typed for a new file. It is the whole screen while it is up -
   a picker, with no pad: the digits are the new file's name there. */
static void nc2_draw_list(void)
{
    int rows = (NC2_PANE_H - 40) / NC2_ROW_H;
    int i;

    nc2_text_clip(NC2_LEFT_PANE_X + 4, NC2_PANE_Y + 6, nc2_file_dir(),
                  (NC2_LEFT_PANE_W + NC2_RIGHT_PANE_W - 20) /
                  nc2_col_width(LVDS_FONT_NORMAL),
                  nc2_col_text(), nc2_col_bg(), LVDS_FONT_NORMAL);
    for (i = 0; i < nc2_file_count() && i < rows; i++) {
        const nc2_file_entry_t *entry = nc2_file_entry(i);
        int y = NC2_PANE_Y + 30 + i * NC2_ROW_H;
        char line[NC2_NAME_MAX + 4];

        if (i == nc2_file_selected()) {
            nc2_fill(NC2_LEFT_PANE_X + 2, y - 2,
                     NC2_LEFT_PANE_W + NC2_RIGHT_PANE_W - 24, NC2_ROW_H - 2,
                     nc2_col_select());
        }
        snprintf(line, sizeof(line), "%s%s", entry->name,
                 entry->is_dir ? "/" : "");
        nc2_text_clip(NC2_LEFT_PANE_X + 8, y, line, 48, nc2_col_text(),
                      i == nc2_file_selected() ? nc2_col_select() : nc2_col_bg(),
                      LVDS_FONT_NORMAL);
    }
    if (nc2_file_new_active()) {
        char line[NC2_NAME_MAX + 8];

        snprintf(line, sizeof(line), "NEW  %s.nc",
                 nc2_file_new_name()[0] ? nc2_file_new_name() : "_");
        nc2_text_clip(NC2_LEFT_PANE_X + 8, NC2_PANE_BOTTOM - 24, line, 40,
                      nc2_col_field_fg(), nc2_col_field_bg(), LVDS_FONT_NORMAL);
    }
}

/* The pad's own band: where the operator is, then the nine slots. */
static void nc2_draw_pad_band(void)
{
    const char *labels[9];
    char name[NC2_PRESET_ROW_MAX];
    char line[64];
    char key;
    int i;

    /* The keys carry their own outlines (they are buttons); the band under them
       is only the address line, with no box drawn around the lot. */
    nc2_pad_name(name, sizeof(name));
    if (g_address[0]) {
        snprintf(line, sizeof(line), "%s  %s", g_address, name);
    } else {
        snprintf(line, sizeof(line), "%s", name);
    }
    nc2_text_clip(NC2_PAD_X + 2, NC2_PAD_Y - 22, line,
                  (NC2_PAD_W - 4) / nc2_col_width(LVDS_FONT_SMALL),
                  nc2_col_text(), nc2_col_bg(), LVDS_FONT_SMALL);
    for (key = '1'; key <= '9'; key++) {
        i = key - '1';
        labels[i] = g_labels[i];
    }
    nc2_draw_pad(NC2_PAD_X, NC2_PAD_Y, NC2_PAD_W, NC2_PAD_H, labels, 0);
}

void nc2_visual_draw(void)
{
    if (nc2_boot_active()) {
        nc2_boot_draw();
        return;
    }
    nc2_fill(0, 0, LVDS_HSTX_WIDTH, LVDS_HSTX_HEIGHT, nc2_col_bg());
    nc2_draw_header();
    if (g_list) {
        /* The list is one column, not the editor's two panes: it takes the whole
           body while it is up, the way a picker does. */
        nc2_fill(NC2_LEFT_PANE_X, NC2_PANE_Y,
                 NC2_RIGHT_PANE_X + NC2_RIGHT_PANE_W - NC2_LEFT_PANE_X, NC2_PANE_H,
                 nc2_col_bg());
        nc2_draw_list();
    } else {
        nc2_draw_panes();
        nc2_draw_program();
        /* The drawing goes on after the panes and before the pad: the pad is the
           machine's keys and sits in the drawing's corner, so it is drawn last
           of the three. */
        nc2_preview_draw(&g_doc, NC2_RIGHT_PANE_X, NC2_PANE_Y, NC2_RIGHT_PANE_W,
                         NC2_PANE_H);
        nc2_draw_pad_band();
    }
    if (g_status[0]) {
        nc2_fill(0, LVDS_HSTX_HEIGHT - 22, LVDS_HSTX_WIDTH, 22, nc2_col_header());
        nc2_text_clip(8, LVDS_HSTX_HEIGHT - 18, g_status,
                      (LVDS_HSTX_WIDTH - 16) / nc2_col_width(LVDS_FONT_SMALL),
                      nc2_col_text(), nc2_col_header(), LVDS_FONT_SMALL);
    }
    nc2_visual_clear_dirty();
}
