#include "nc2_visual.h"

#include "nc2.h"
#include "nc2_boot.h"
#include "nc2_draw.h"
#include "nc2_files.h"
#include "nc2_layout.h"
#include "nc2_manual.h"
#include "nc2_preview.h"
#include "nc2_presets.h"
#include "nc2_run.h"
#include "nc2_state.h"

#include "../../cnc.h"
#include "../../core/interpolator.h"
#include "../../core/parser.h"
#include "../lvds_renderer/lvds_hstx.h"

#include <stdio.h>
#include <string.h>

static nc2_document_t g_doc;
static char g_address[NC2_ADDR_MAX + 1];
static char g_status[64];
static char g_labels[9][NC2_PRESET_ROW_MAX];
static bool g_dirty;
static bool g_list;                 /* the file list is the screen */
static nc2_mode_t g_mode = NC2_MODE_PROGRAM;
static uint32_t g_last_key_ms;      /* when the screen was last touched */

/* The panel writes the program back itself once the operator has left it alone
   for a moment, the way nc does: the file on the card is what runs, so an edit
   that is never flushed is an edit that is not there. */
#define NC2_IDLE_SAVE_MS 1500u

static void nc2_visual_load_tools(void);

static void nc2_statusf(const char *text)
{
    snprintf(g_status, sizeof(g_status), "%s", text ? text : "");
}

void nc2_visual_status_set(const char *text)
{
    nc2_statusf(text);
    g_dirty = true;
}

void nc2_visual_mark_dirty(void)
{
    g_dirty = true;
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
    nc2_run_init();
    nc2_state_init();
    switch (nc2_state_mode()) {
    case NC2_MODE_MANUAL:
    case NC2_MODE_RUN:
    case NC2_MODE_TOOLS:
        g_mode = nc2_state_mode();
        break;
    default:
        g_mode = NC2_MODE_PROGRAM;
        break;
    }
    /* The one write the panel makes by itself, and only onto a card that has no
       entry of its own: after that every entry is a file. */
    (void)nc2_boot_seed();
    /* Onto the file the panel had open, and where it was in it. A card that
       remembers nothing opens on an empty program, which is a program. */
    if (g_mode == NC2_MODE_TOOLS) {
        nc2_visual_load_tools();
    } else if (!nc2_state_load_document(g_mode, &g_doc)) {
        nc2_document_init(&g_doc);
        (void)nc2_insert_line(&g_doc, 0u, "");
    }
    nc2_labels_at(g_address);
}

/* The tool table nc2 ships when the card has none: the same row nc's default
   writes, because TOOLS edits one table whichever module is driving. */
#define NC2_TOOL_DEFAULT "T1 R0.8 O3 F120 Q60 D2.0 E0.5 S800 X0 Z0"

/* TOOLS is the tool table, and the table is a file like any other: the screen is
   the editor on `/D/nc/files/tool.t`, with the table's own first row when the
   card has none. */
static void nc2_visual_load_tools(void)
{
    const char *path = nc2_state_path(NC2_MODE_TOOLS);

    if (!path[0]) {
        path = NC2_TOOL_PATH;
    }
    if (!nc2_file_load(&g_doc, path)) {
        nc2_document_init(&g_doc);
        (void)nc2_insert_line(&g_doc, 0u, NC2_TOOL_DEFAULT);
        snprintf(g_doc.path, sizeof(g_doc.path), "%s", path);
        (void)nc2_file_save(&g_doc);
        nc2_statusf("New tool table");
    }
    nc2_state_remember_path(NC2_MODE_TOOLS, g_doc.path);
}

/* Jump to a screen: the program (and the run, which shows the same file), the
   tool table, and MANUAL. Each keeps what it had open, so a look at the tools
   does not lose the program's place. */
void nc2_visual_select_mode(nc2_mode_t mode)
{
    if (mode != NC2_MODE_PROGRAM && mode != NC2_MODE_RUN &&
        mode != NC2_MODE_MANUAL && mode != NC2_MODE_TOOLS) {
        return;
    }
    if (mode == g_mode) {
        return;
    }
    if (g_mode == NC2_MODE_MANUAL) {
        nc2_manual_feed_cancel();      /* a jog must not run behind the next screen */
    }
    if (g_mode == NC2_MODE_PROGRAM || g_mode == NC2_MODE_TOOLS) {
        (void)nc2_visual_save();       /* whatever this screen had open */
    }
    g_list = false;
    g_mode = mode;
    nc2_state_set_mode(mode);
    nc2_address_reset(g_address);
    nc2_labels_at(g_address);
    if (mode == NC2_MODE_TOOLS) {
        nc2_visual_load_tools();
    } else if (mode == NC2_MODE_PROGRAM) {
        if (!nc2_state_load_document(NC2_MODE_PROGRAM, &g_doc)) {
            nc2_document_init(&g_doc);
            (void)nc2_insert_line(&g_doc, 0u, "");
        }
    } else if (mode == NC2_MODE_RUN && !nc2_run_active()) {
        nc2_run_set_line(&g_doc, g_doc.cursor);
    }
    nc2_statusf("");
    g_dirty = true;
}

nc2_mode_t nc2_visual_mode(void)
{
    return g_mode;
}

/* The mode key: the screens built so far, in the machine's own order. */
static void nc2_visual_next_mode(void)
{
    switch (g_mode) {
    case NC2_MODE_MANUAL:
        nc2_visual_select_mode(NC2_MODE_PROGRAM);
        break;
    case NC2_MODE_PROGRAM:
        nc2_visual_select_mode(NC2_MODE_TOOLS);
        break;
    case NC2_MODE_TOOLS:
        nc2_visual_select_mode(NC2_MODE_RUN);
        break;
    case NC2_MODE_RUN:
    default:
        nc2_visual_select_mode(NC2_MODE_MANUAL);
        break;
    }
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
    nc2_state_remember_path(g_mode, g_doc.path);
    nc2_state_flush();
    g_dirty = true;
    return true;
}

const char *nc2_visual_path(void)
{
    return g_doc.path;
}

size_t nc2_visual_cursor(void)
{
    return g_doc.cursor;
}

const char *nc2_visual_screen_name(void)
{
    if (g_list) {
        return "FILES";
    }
    if (g_mode == NC2_MODE_RUN) {
        return "RUN";
    }
    if (g_mode == NC2_MODE_MANUAL) {
        return "MANUAL";
    }
    return g_mode == NC2_MODE_TOOLS ? "TOOLS" : "EDIT";
}

bool nc2_visual_save(void)
{
    if (!nc2_file_save(&g_doc)) {
        nc2_statusf("Save failed");
        g_dirty = true;
        return false;
    }
    g_doc.dirty = false;
    nc2_state_remember_path(g_mode == NC2_MODE_TOOLS ? NC2_MODE_TOOLS
                                                      : NC2_MODE_PROGRAM,
                            g_doc.path);
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

/* The line the RUN screen is on: what the pane marks and what `1 SINGLE` acts
   on. It is the line in play, not the sender's position, and it is clamped into
   the program because a finished run stands just past its last line. */
static size_t nc2_visual_run_line(void)
{
    size_t line = nc2_run_display_line();

    if (g_doc.line_count && line >= g_doc.line_count) {
        line = g_doc.line_count - 1u;
    }
    return line;
}

/* The run's own keys: `1 SINGLE 2 FROM 3 FULL 4 HOLD 5 STOP`, `#` reloads, and
   `B`/`C` walk the mark while nothing is running. False when the key is not the
   run's, so the screen can offer it to the modes. */
static bool nc2_visual_run_key(char key)
{
    size_t line = nc2_visual_run_line();
    char text[48];

    switch (key) {
    case '1':
        nc2_statusf(nc2_run_send_unit(&g_doc, line) ? "SINGLE sent" : "Nothing to send");
        break;
    case '2':
        if (nc2_run_start(&g_doc, line)) {
            snprintf(text, sizeof(text), "Run from line %lu", (unsigned long)(line + 1u));
            nc2_statusf(text);
        } else {
            nc2_statusf("Nothing to run");
        }
        break;
    case '3':
        nc2_statusf(nc2_run_start(&g_doc, 0u) ? "Full run" : "Nothing to run");
        break;
    case '4':
        if (nc2_run_toggle_hold()) {
            nc2_statusf(nc2_run_hold() ? "Hold" : "Resume");
        } else {
            nc2_statusf("No active run");
        }
        break;
    case '5':
        nc2_run_stop();
        nc2_statusf("Stopped");
        break;
    case '#':
        /* RELOAD: the fault and the run go, and the file is read back off the
           card, so an edit made since it was opened is what runs next. */
        nc2_run_reset();
        (void)nc2_state_load_document(NC2_MODE_RUN, &g_doc);
        nc2_statusf("Reloaded");
        break;
    case 'B':
    case 'C':
        if (nc2_run_active()) {
            return true;            /* RUN owns the cursor while it runs */
        }
        if (key == 'B') {
            line = line > 0u ? line - 1u : 0u;
        } else if (line + 1u < g_doc.line_count) {
            line++;
        }
        nc2_run_set_line(&g_doc, line);
        g_doc.cursor = line;
        break;
    default:
        return false;
    }
    g_dirty = true;
    return true;
}

void nc2_visual_key(char key)
{
    nc2_key_t editor_key = NC2_KEY_NONE;

    g_last_key_ms = mcu_millis();
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
    /* MANUAL is a machine panel, not the editor: the pad is the jog keys, `A`
       leaves (when no field is open), and the digits never touch the program -
       so MANUAL works with no program at all. */
    if (g_mode == NC2_MODE_MANUAL) {
        if (key == 'A' && !nc2_manual_field_active()) {
            nc2_manual_feed_cancel();
            nc2_visual_next_mode();
            return;
        }
        (void)nc2_manual_key(key);
        g_dirty = true;
        return;
    }
    if (g_doc.line_count == 0u) {
        return;                     /* no program: the keys have nothing to act on */
    }
    /* The run screen: `0` still opens the card and `A` still leaves, and the
       rest of the pad is the run's. */
    if (g_mode == NC2_MODE_RUN) {
        if (key == '0') {
            if (nc2_file_scan("/D")) {
                g_list = true;
                nc2_statusf("");
            } else {
                nc2_statusf("The card is not answering");
            }
            g_dirty = true;
            return;
        }
        if (key == 'A') {
            nc2_visual_next_mode();
            return;
        }
        (void)nc2_visual_run_key(key);
        return;
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
        /* Up a level, and at the root the mode key: the program and the run
           are the two screens built so far. */
        if (g_address[0]) {
            nc2_address_pop(g_address);
            nc2_pad_close(&g_doc);
            nc2_labels_at(g_address);
            nc2_statusf("");
            g_dirty = true;
        } else {
            nc2_visual_next_mode();
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

void nc2_visual_hold_key(char key)
{
    if (g_mode == NC2_MODE_MANUAL) {
        nc2_manual_hold(key);
    }
}

void nc2_visual_tick(unsigned ms)
{
    if (nc2_boot_active()) {
        nc2_boot_tick(ms);
        if (nc2_boot_active()) {
            /* The logo is a still picture: nothing to do while it stands. */
            nc2_visual_clear_dirty();
        } else {
            /* It has just left, and the work screen has to take its place - or
               the panel keeps the logo on the glass while the screen believes
               it is gone. */
            nc2_visual_mark_dirty();
        }
    }
}

/* How often a screen that has something of its own to show redraws: the frame
   the DRO's figures and a held feed need, and no more. */
#define NC2_PUMP_PERIOD_MS 20u

bool nc2_visual_pump(uint32_t now_ms)
{
    static uint32_t last_pump_ms;
    static uint32_t last_draw_ms;
    unsigned elapsed = (unsigned)(now_ms - last_pump_ms);

    last_pump_ms = now_ms;
    /* The logo's clock first: it is the screen's own, and it is what makes the
       first start leave on its own - the module that draws has no clock of its
       own to run it with. */
    nc2_visual_tick(elapsed);
    nc2_visual_idle_tasks();
    if (nc2_visual_dirty()) {
        last_draw_ms = now_ms;
        return true;
    }
    if (nc2_visual_periodic_needed() &&
        (uint32_t)(now_ms - last_draw_ms) >= NC2_PUMP_PERIOD_MS) {
        last_draw_ms = now_ms;
        return true;
    }
    return false;
}

void nc2_visual_idle_tasks(void)
{
    if (g_list || g_mode == NC2_MODE_MANUAL || !g_doc.dirty || !g_doc.path[0]) {
        return;
    }
    if ((uint32_t)(mcu_millis() - g_last_key_ms) >= NC2_IDLE_SAVE_MS) {
        (void)nc2_visual_save();
    }
}

/* --- drawing -------------------------------------------------------------- */

/* The header: the four screens with the one in play marked, then the file, and
   the body's messages at the right end.

   This is the panel's only band. There is no footer: what a footer says - which
   screen you are on and what just happened - is one line of text, and one line
   belongs where the operator is already looking. The machine's keys are the pad
   in the drawing's corner, so nothing is left along the bottom. */
static void nc2_draw_header(void)
{
    static const nc2_mode_t order[4] = {
        NC2_MODE_MANUAL, NC2_MODE_PROGRAM, NC2_MODE_TOOLS, NC2_MODE_RUN
    };
    static const char *const names[4] = { "MANUAL", "EDIT", "TOOLS", "RUN" };
    /* MANUAL is a machine panel: it has no file, so it shows none. The list
       shows the folder it is walking. */
    const char *path = g_list ? nc2_file_dir()
                              : (g_mode == NC2_MODE_MANUAL
                                     ? ""
                                     : (g_doc.path[0] ? g_doc.path : ""));
    int y = 6;
    int x = 8;
    int i;

    nc2_fill(0, 0, LVDS_HSTX_WIDTH, NC2_HEADER_H, nc2_col_header());
    for (i = 0; i < 4; i++) {
        bool active = order[i] == g_mode;
        int w;

        if (i) {
            x += 10;
        }
        w = nc2_text_width(names[i], LVDS_FONT_NORMAL);
        /* The active screen is the bright one, and is underlined: at a glance,
           on a panel that may be at an angle, "which screen am I on" is one
           word and one line. */
        nc2_text(x, y, names[i], active ? nc2_col_text() : nc2_col_dim(),
                 nc2_col_header(), LVDS_FONT_NORMAL);
        if (active) {
            nc2_hline(x, y + 16, w, nc2_col_accent());
        }
        x += w;
    }

    /* The file, between the screens and the messages - and only the room that is
       left, so a long path can never push a message off the glass. */
    {
        int small_w = nc2_col_width(LVDS_FONT_SMALL);
        int msg_cols = g_status[0]
                           ? (nc2_text_width(g_status, LVDS_FONT_SMALL) +
                              small_w - 1) / small_w
                           : 0;
        int msg_x;
        int path_cols;

        if (msg_cols > LVDS_HSTX_WIDTH / (2 * small_w)) {
            msg_cols = LVDS_HSTX_WIDTH / (2 * small_w);
        }
        msg_x = LVDS_HSTX_WIDTH - 8 - msg_cols * small_w;
        path_cols = (msg_x - 12 - x) / small_w;
        if (path_cols > 0 && path[0]) {
            char line[NC2_PATH_MAX + 4];

            snprintf(line, sizeof(line), "%s%s", path, g_doc.dirty ? " *" : "");
            nc2_text_clip(x + 12, y + 1, line, path_cols, nc2_col_dim(),
                          nc2_col_header(), LVDS_FONT_SMALL);
        }
        if (msg_cols > 0) {
            nc2_text_clip(msg_x, y + 1, g_status, msg_cols, nc2_col_text(),
                          nc2_col_header(), LVDS_FONT_SMALL);
        }
    }
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

/* One row of the program: its number, then the text. The row the cursor (or the
   run) is on wears the selection colour, the rows of the block it heads wear
   the pale one, and the picked field is drawn as the box being typed into -
   three pieces, so everything the operator did not touch keeps its colour. */
static void nc2_draw_row(size_t index, int y, bool selected, bool path)
{
    const char *text = g_doc.lines[index];
    bool cursor = selected;
    lvds_color_t bg = cursor ? nc2_col_select()
                             : (path ? nc2_col_block() : nc2_col_bg());
    int col_w = nc2_col_width(LVDS_FONT_NORMAL);
    char number[8];
    int x = NC2_LEFT_PANE_X + NC2_LINE_NO_PAD;
    int cols;
    nc2_field_t fields[NC2_MAX_FIELDS];
    int count;
    int picked = -1;

    if (cursor || path) {
        nc2_fill(NC2_LEFT_PANE_X, y - 2, NC2_LEFT_PANE_W - 2, NC2_ROW_H - 2,
                 cursor ? nc2_col_select() : nc2_col_block());
    }
    snprintf(number, sizeof(number), "%3u", (unsigned)(index + 1u));
    nc2_text(x, y, number, nc2_col_dim(), bg, LVDS_FONT_NORMAL);
    x += 4 * col_w;
    cols = (NC2_LEFT_PANE_W - (x - NC2_LEFT_PANE_X) - 4) / col_w;
    if (cols <= 0) {
        return;
    }
    count = nc2_fields(text, fields, NC2_MAX_FIELDS);
    if (cursor && g_mode == NC2_MODE_PROGRAM && g_doc.field >= 0 &&
        g_doc.field < count) {
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
    size_t mark = g_doc.cursor;
    size_t first = 0u;
    size_t path_first = 0u;
    size_t path_last = 0u;
    bool have_path = false;
    size_t i;

    if (g_mode == NC2_MODE_RUN) {
        mark = nc2_visual_run_line();
    }
    /* The line in play heads a path: the cycle's rows are the pale mark beside
       it, on the run screen and in the editor alike - the editor is where the
       program is read, so the block the cursor sits in is the first thing its
       own screen should say. */
    {
        g7x_doc_t view = nc2_document_g7x(&g_doc);

        have_path = g7x_doc_line_path(&view, mark, &path_first, &path_last);
    }
    if (mark >= NC2_CODE_ROWS) {
        first = mark - NC2_CODE_ROWS + 1u;
    }
    for (i = first; i < g_doc.line_count && i - first < NC2_CODE_ROWS; i++) {
        bool selected = i == mark;
        bool path = have_path && i >= path_first && i <= path_last && !selected;

        nc2_draw_row(i, NC2_PANE_Y + 4 + (int)(i - first) * NC2_ROW_H, selected, path);
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

/* The pad's nine labels on the run screen: the run's own keys. The rest of the
   pad is empty there - the card's entries are the editor's. */
static const char *const g_nc2_run_labels[9] = {
    "SINGLE", "FROM", "FULL", "HOLD", "STOP", "", "", "", ""
};

/* The label of one pad key on the active screen, or "" when it has none. */
static const char *nc2_pad_label(char key)
{
    if (key < '1' || key > '9') {
        return "";
    }
    if (g_mode == NC2_MODE_RUN) {
        return g_nc2_run_labels[key - '1'];
    }
    if (g_mode == NC2_MODE_MANUAL) {
        return nc2_manual_pad_label(key);
    }
    return g_labels[key - '1'];
}

/* The key the pad draws as the one in play: the run holds `4`, and MANUAL
   flashes the key the machine just acted on. */
static char nc2_pad_hot(void)
{
    char key;

    if (g_mode == NC2_MODE_RUN) {
        return nc2_run_hold() ? '4' : 0;
    }
    if (g_mode != NC2_MODE_MANUAL) {
        return 0;
    }
    for (key = '1'; key <= '9'; key++) {
        if (nc2_manual_flash(key)) {
            return key;
        }
    }
    return 0;
}

/* The pad's own band: where the operator is, then the nine slots. */
static void nc2_draw_pad_band(void)
{
    const char *labels[9];
    char name[NC2_PRESET_ROW_MAX];
    char line[64];
    char key;
    int i;

    if (g_mode == NC2_MODE_RUN) {
        snprintf(line, sizeof(line), "RUN  line %lu",
                 (unsigned long)(nc2_visual_run_line() + 1u));
    } else if (g_mode == NC2_MODE_MANUAL) {
        snprintf(line, sizeof(line), "MANUAL  %c %s",
                 nc2_manual_axis() ? 'Z' : 'X',
                 nc2_manual_continuous()
                     ? "feed" : "step");
    } else {
        /* The keys carry their own outlines (they are buttons); the band under
           them is only the address line, with no box drawn around the lot. */
        nc2_pad_name(name, sizeof(name));
        if (g_address[0]) {
            snprintf(line, sizeof(line), "%s  %s", g_address, name);
        } else {
            snprintf(line, sizeof(line), "%s", name);
        }
    }
    nc2_text_clip(NC2_PAD_X + 2, NC2_PAD_Y - 22, line,
                  (NC2_PAD_W - 4) / nc2_col_width(LVDS_FONT_SMALL),
                  nc2_col_text(), nc2_col_bg(), LVDS_FONT_SMALL);
    for (key = '1'; key <= '9'; key++) {
        i = key - '1';
        labels[i] = nc2_pad_label(key);
    }
    nc2_draw_pad(NC2_PAD_X, NC2_PAD_Y, NC2_PAD_W, NC2_PAD_H, labels,
                 nc2_pad_hot());
}

/* The word the DRO's corner carries: what the machine is doing, or refusing to
   do, right now. */
static const char *nc2_state_label(const nc2_runtime_state_t *rt)
{
    uint16_t state = rt ? rt->exec_state : 0u;

    if (cnc_has_alarm()) return "ALARM";
    if (state & EXEC_KILL) return "KILLED";
    if (state & EXEC_LIMITS) return "LIMITS";
    if (state & EXEC_POSITION_MAYBE_LOST) return "POS LOST";
    if (state & EXEC_DOOR) return "DOOR";
    if (nc2_run_hold() || (state & EXEC_HOLD)) return "HOLD";
    if (nc2_run_active() || (state & (EXEC_RUN | EXEC_JOG))) return "RUN";
    if (state & EXEC_HOMING) return "HOMING";
    if (nc2_run_error()) return "ERROR";
    return "IDLE";
}

/* True when that state is a fault: it wears the same red as a message the panel
   would show, so a machine that needs attention says so on the glass. */
static bool nc2_state_is_fault(const nc2_runtime_state_t *rt)
{
    uint16_t state = rt ? rt->exec_state : 0u;

    return cnc_has_alarm() ||
           (state & (EXEC_KILL | EXEC_LIMITS | EXEC_POSITION_MAYBE_LOST)) ||
           nc2_run_error();
}

/* The floating DRO: the work position, the feed and the spindle, and the
   machine's own state word. It is drawn only while the machine has something to
   say - a run, a jog, a hold, a fault - so a screen that is not running is all
   drawing. */
static void nc2_draw_dro(void)
{
    nc2_runtime_state_t rt;
    float work[AXIS_COUNT] = { 0 };
    lvds_color_t bg;
    const char *state;
    bool fault;
    char buf[40];
    int col_w = nc2_col_width(LVDS_FONT_NORMAL);

    nc2_state_runtime(&rt);
    work[AXIS_X] = rt.x;
    work[AXIS_Z] = rt.z;
    parser_machine_to_work(work);
    state = nc2_state_label(&rt);
    fault = nc2_state_is_fault(&rt);
    bg = fault ? nc2_col_error()
               : (nc2_run_active() || (rt.exec_state & EXEC_RUN)
                      ? nc2_col_run()
                      : nc2_col_header());

    nc2_fill(NC2_DRO_X, NC2_DRO_Y, NC2_DRO_W, NC2_DRO_H, bg);
    nc2_text(NC2_DRO_X + 6, NC2_DRO_Y + 4, "X", nc2_col_dim(), bg, LVDS_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "%9.3f", (double)work[AXIS_X]);
    nc2_text_clip(NC2_DRO_X + 24, NC2_DRO_Y + 4, buf, 9, nc2_col_text(), bg,
                  LVDS_FONT_NORMAL);
    nc2_text(NC2_DRO_X + 6, NC2_DRO_Y + 22, "Z", nc2_col_dim(), bg, LVDS_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "%9.3f", (double)work[AXIS_Z]);
    nc2_text_clip(NC2_DRO_X + 24, NC2_DRO_Y + 22, buf, 9, nc2_col_text(), bg,
                  LVDS_FONT_NORMAL);

    nc2_text(NC2_DRO_X + 120, NC2_DRO_Y + 4, "F", nc2_col_dim(), bg, LVDS_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "%7.1f", (double)rt.feed);
    nc2_text_clip(NC2_DRO_X + 138, NC2_DRO_Y + 4, buf, 7, nc2_col_text(), bg,
                  LVDS_FONT_NORMAL);
    nc2_text(NC2_DRO_X + 120, NC2_DRO_Y + 22, "S", nc2_col_dim(), bg, LVDS_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "%7u", rt.spindle);
    nc2_text_clip(NC2_DRO_X + 138, NC2_DRO_Y + 22, buf, 7, nc2_col_text(), bg,
                  LVDS_FONT_NORMAL);

    snprintf(buf, sizeof(buf), "uCNC %s", state);
    nc2_text_clip(NC2_DRO_X + 6, NC2_DRO_Y + 48, buf,
                  (NC2_DRO_W - 12) / col_w, nc2_col_dim(), bg, LVDS_FONT_NORMAL);
}

void nc2_visual_draw(void)
{
    if (nc2_boot_active()) {
        nc2_boot_draw();
        /* The renderer draws into the PSRAM backbuffer and the scanout reads
           SRAM: without this the panel keeps showing whatever it was showing -
           black on a cold boot. It is a no-op on the host backend, which is
           why every host frame looked right while the glass stayed dark. */
        lvds_hstx_present();
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
    } else if (g_mode == NC2_MODE_MANUAL) {
        /* MANUAL is a machine panel: the pane carries the stops and the value
           the keys change, the pad its jog keys, and there is no drawing - the
           machine's own figures are the DRO's. */
        nc2_draw_panes();
        nc2_manual_draw_pane(NC2_LEFT_PANE_X, NC2_PANE_Y, NC2_LEFT_PANE_W,
                             NC2_PANE_H);
        if (nc2_state_busy() || nc2_run_active() || nc2_run_hold() ||
            nc2_run_error()) {
            nc2_draw_dro();
        }
        nc2_draw_pad_band();
    } else {
        nc2_draw_panes();
        nc2_draw_program();
        /* The drawing goes on after the panes and before the pad: the pad is the
           machine's keys and sits in the drawing's corner, so it is drawn last
           of the three. */
        nc2_preview_draw(&g_doc, NC2_RIGHT_PANE_X, NC2_PANE_Y, NC2_RIGHT_PANE_W,
                         NC2_PANE_H);
        /* The DRO floats over the drawing and only while the machine has
           something to say; the pad is the machine's keys and is drawn last. */
        if (nc2_state_busy() || nc2_run_active() || nc2_run_hold() ||
            nc2_run_error()) {
            nc2_draw_dro();
        }
        nc2_draw_pad_band();
    }
    nc2_visual_clear_dirty();
    lvds_hstx_present();
}

/* --- the shell's own questions -------------------------------------------- */

bool nc2_visual_periodic_needed(void)
{
    /* What changes on its own: the first start's logo, the machine doing
       something (the DRO's figures, a held feed), MANUAL's key flash. An
       *unsaved* program is not one of them - that flag is about writing the
       file back, and it used to keep a still screen repainting every 20 ms. */
    return nc2_boot_active() || nc2_state_busy() || nc2_run_active() ||
           nc2_run_hold() || nc2_run_error() || g_mode == NC2_MODE_MANUAL;
}

size_t nc2_visual_usage(const char *const **lines)
{
    static const char *const program[] = {
        "The program, one line at a time.",
        "D walks a line's fields.",
        "A digit types the value, B is the",
        "sign and C the point.",
        "B/C move by line with nothing picked.",
        "1-9 press the pad's entries.",
        "A up a level, 0 the card, A the run."
    };
    static const char *const run[] = {
        "Send the program to the machine.",
        "1 SINGLE  2 FROM  3 FULL.",
        "4 HOLD (again resumes)  5 STOP.",
        "B/C pick the line, # reload, 0 files."
    };
    static const char *const manual[] = {
        "Jog the machine by hand.",
        "Digits jog: 2/8 X, 4/6 Z.",
        "7/9 spindle CCW/CW, 5 stop.",
        "1/3 change the step or the feed.",
        "# swaps step for feeding.",
        "* types the stops, D touches off,",
        "0 zeroes the axis, B/C pick it."
    };
    static const char *const tools[] = {
        "The tool table, one tool a line.",
        "D walks a line's fields.",
        "A digit types the value, B is the",
        "sign and C the point.",
        "1-9 press the pad's entries.",
        "A leaves, 0 opens the card."
    };
    static const char *const files[] = {
        "Pick a program from the card.",
        "B/C step the list, D opens.",
        "5 new  6 delete  8 refresh.",
        "0 back to the program."
    };
    const char *const *table;
    size_t count;

    if (!lines) {
        return 0u;
    }
    if (g_list) {
        table = files;
        count = sizeof(files) / sizeof(files[0]);
    } else if (g_mode == NC2_MODE_MANUAL) {
        table = manual;
        count = sizeof(manual) / sizeof(manual[0]);
    } else if (g_mode == NC2_MODE_TOOLS) {
        table = tools;
        count = sizeof(tools) / sizeof(tools[0]);
    } else if (g_mode == NC2_MODE_RUN) {
        table = run;
        count = sizeof(run) / sizeof(run[0]);
    } else {
        table = program;
        count = sizeof(program) / sizeof(program[0]);
    }
    *lines = table;
    return count;
}

bool nc2_visual_key_meaning(char key, nc2_visual_key_meaning_t *meaning)
{
    if (!meaning) {
        return false;
    }
    meaning->label = 0;
    meaning->on_menu = false;
    meaning->step = false;
    if (g_list) {
        switch (key) {
        case 'B': meaning->label = "list -"; meaning->step = true; break;
        case 'C': meaning->label = "list +"; meaning->step = true; break;
        case 'D': meaning->label = "open"; break;
        case '#': meaning->label = "open"; break;
        case '5': meaning->label = "new"; break;
        case '6': meaning->label = "delete"; break;
        case '8': meaning->label = "refresh"; break;
        case '0': meaning->label = "back"; break;
        default: return false;
        }
        return true;
    }
    if (g_mode == NC2_MODE_MANUAL) {
        if (key >= '1' && key <= '9') {
            meaning->label = nc2_manual_pad_label(key);
            meaning->on_menu = true;
            return true;
        }
        switch (key) {
        case '#': meaning->label = "step/feed"; break;
        case '*': meaning->label = "stops"; break;
        case 'D': meaning->label = "touch"; break;
        case '0': meaning->label = "zero"; break;
        case 'A': meaning->label = "edit"; break;
        case 'B': meaning->label = "axis X"; meaning->step = true; break;
        case 'C': meaning->label = "axis Z"; meaning->step = true; break;
        default: return false;
        }
        return true;
    }
    if (g_mode == NC2_MODE_RUN) {
        if (key >= '1' && key <= '5') {
            meaning->label = nc2_pad_label(key);
            meaning->on_menu = true;
            return true;
        }
        switch (key) {
        case '#': meaning->label = "RELOAD"; break;
        case '0': meaning->label = "files"; break;
        case 'A': meaning->label = "edit"; break;
        case 'B': meaning->label = "line -"; meaning->step = true; break;
        case 'C': meaning->label = "line +"; meaning->step = true; break;
        default: return false;
        }
        return true;
    }
    if (key >= '1' && key <= '9') {
        meaning->label = nc2_pad_label(key);
        meaning->on_menu = true;
        return meaning->label[0] != '\0';
    }
    switch (key) {
    case 'A': meaning->label = "level"; break;
    case '0': meaning->label = "files"; break;
    case 'B': meaning->label = "line -"; meaning->step = true; break;
    case 'C': meaning->label = "line +"; meaning->step = true; break;
    case 'D': meaning->label = "field"; break;
    case '#': meaning->label = "type"; break;
    case '*': meaning->label = "delete"; break;
    default: return false;
    }
    return true;
}
