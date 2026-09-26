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
#include "nc2_tools.h"
#include "nc2_vocab.h"

#include "../../cnc.h"
#include "../../core/interpolator.h"
#include "../../core/parser.h"
#include "../../interface/grbl_stream.h"
#include "../lvds_renderer/lvds_hstx.h"
#include "../g7x/g7x.h"

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
static size_t g_pane_first;         /* the code pane's first visible line */

/* How many rows of the file stay in view below the cursor's. In G-code the next
   block is what the operator is reading for, so the pane never lets the cursor
   reach its last row: the *text* moves instead (bench: "cursor should never
   reach last line. in g code it is always necessary to see next line. so as
   before leave - 6 lines and move text, but not the cursor to the end."). */
#define NC2_LOOKAHEAD_ROWS 6

/* The frame meter - back by the bench's own request ("give me back fps meter it
   need to be tested"). One frame is one `nc2_visual_draw()`, and the reading is
   the last whole second's frames; it is what says whether a change cost the
   panel its frame rate, and the live stock's mask redraw is the one to watch
   (`nc2/TESTING.md`). It is a debug reading: dim, small, and in the header's
   far corner, out of everything the operator reads. */
#define NC2_FPS_COLS 8
static uint16_t g_nc2_fps;
static uint16_t g_nc2_fps_frames;
static uint32_t g_nc2_fps_start_ms;

/* Where a frame's time goes: the whole drawing, the bands that do not move on
   their own (the header, the rows, the notes, the pad), the machine's strip, and
   the hand-off to the panel. Once a second they are averaged and logged to the
   console - the glass has room for the frame count, and the breakdown is what
   answers "why" (bench: "12 fps only. why ..."). */
static uint32_t g_nc2_acc_draw_us;
static uint32_t g_nc2_acc_screen_us;
static uint32_t g_nc2_acc_stock_us;
static uint32_t g_nc2_acc_geom_us;
static uint32_t g_nc2_acc_strip_us;
static uint32_t g_nc2_acc_present_us;

static unsigned nc2_ms_of(uint32_t us, uint16_t frames)
{
    return frames ? ((us / frames) + 500u) / 1000u : 0u;
}

static void nc2_fps_tick(void)
{
    uint32_t now = mcu_millis();
    uint32_t elapsed;

    if (!g_nc2_fps_start_ms) {
        g_nc2_fps_start_ms = now;
    }
    g_nc2_fps_frames++;
    elapsed = now - g_nc2_fps_start_ms;
    if (elapsed >= 1000u) {
        g_nc2_fps = (uint16_t)(((uint32_t)g_nc2_fps_frames * 1000u) / elapsed);
        if (g_nc2_fps_frames) {
            grbl_stream_printf("[MSG:NC2 fps %u draw %u screen %u stock %u "
                               "geom %u strip %u present %u]\r\n",
                               (unsigned)g_nc2_fps,
                               nc2_ms_of(g_nc2_acc_draw_us, g_nc2_fps_frames),
                               nc2_ms_of(g_nc2_acc_screen_us, g_nc2_fps_frames),
                               nc2_ms_of(g_nc2_acc_stock_us, g_nc2_fps_frames),
                               nc2_ms_of(g_nc2_acc_geom_us, g_nc2_fps_frames),
                               nc2_ms_of(g_nc2_acc_strip_us, g_nc2_fps_frames),
                               nc2_ms_of(g_nc2_acc_present_us,
                                         g_nc2_fps_frames));
        }
        g_nc2_acc_draw_us = 0u;
        g_nc2_acc_screen_us = 0u;
        g_nc2_acc_stock_us = 0u;
        g_nc2_acc_geom_us = 0u;
        g_nc2_acc_strip_us = 0u;
        g_nc2_acc_present_us = 0u;
        g_nc2_fps_frames = 0u;
        g_nc2_fps_start_ms = now;
    }
}

static void nc2_draw_fps(void)
{
    char buf[16];

    snprintf(buf, sizeof(buf), "%u FPS", (unsigned)g_nc2_fps);
    nc2_text_clip(LVDS_VIEW_WIDTH - 8 -
                      NC2_FPS_COLS * nc2_col_width(LVDS_FONT_SMALL),
                  NC2_HEADER_H - 11, buf, NC2_FPS_COLS, nc2_col_dim(),
                  nc2_col_header(), LVDS_FONT_SMALL);
}

uint16_t nc2_visual_fps(void)
{
    return g_nc2_fps;
}

/* The panel writes the program back itself once the operator has left it alone
   for a moment, the way nc does: the file on the card is what runs, so an edit
   that is never flushed is an edit that is not there. */
#define NC2_IDLE_SAVE_MS 1500u

static void nc2_visual_load_tools(void);
static void nc2_visual_load_tool_table(void);
static const char *nc2_pad_label(char key);
static void nc2_statusf(const char *text);

/* The wording of a refused line: the module that refused it knows *why* and
   hands its reason over (`g7x_take_refusal_text()` - reading it takes it, so it
   can never explain a later line); otherwise the controller's own code name is
   all there is. `nc`'s `nc_feedback_error()`, kept where the panel's words
   live. */
static const char *nc2_error_text(uint8_t error)
{
    const char *why = g7x_take_refusal_text();

    if (why && why[0]) {
        return why;
    }
    switch (error) {
    case STATUS_BAD_NUMBER_FORMAT: return "Invalid number";
    case STATUS_INVALID_STATEMENT: return "Invalid parameters or cycle contour";
    case STATUS_NEGATIVE_VALUE: return "Negative value not allowed";
    case STATUS_SYSTEM_GC_LOCK: return "Controller locked or cycle canceled";
    case STATUS_SOFT_LIMIT_ERROR: return "Target exceeds travel limits";
    case STATUS_GCODE_UNSUPPORTED_COMMAND: return "Unsupported command";
    case STATUS_GCODE_MODAL_GROUP_VIOLATION: return "Conflicting modal commands";
    case STATUS_GCODE_UNDEFINED_FEED_RATE: return "Set a valid feed rate (F)";
    case STATUS_GCODE_COMMAND_VALUE_NOT_INTEGER: return "Integer value required";
    case STATUS_GCODE_VALUE_WORD_MISSING: return "Required parameter missing";
    case STATUS_GCODE_UNUSED_WORDS: return "Unexpected parameter";
    case STATUS_INVALID_PLANE_SELECTED: return "Wrong plane for this command";
    case STATUS_SPINDLE_RPM_ERROR:
        return "Spindle feedback/synchronization error";
    default: return "Command rejected; check parameters";
    }
}

/* A line the controller refused, said where the operator reads: which line of
   the program it was, the code, and what it means. The strip's own word
   (`uCNC ERROR`) is a glance; this is the sentence (bench: "it says ucnc erro in
   strip only but not message itself?"). */
static bool nc2_visual_parse_error(void *args)
{
    uint8_t error = *(uint8_t *)args;
    char text[96];

    if (nc2_run_error()) {
        snprintf(text, sizeof(text), "Line %lu error %u: %s",
                 (unsigned long)(nc2_run_error_line() + 1u), (unsigned)error,
                 nc2_error_text(error));
    } else {
        snprintf(text, sizeof(text), "Error %u: %s", (unsigned)error,
                 nc2_error_text(error));
    }
    nc2_statusf(text);
    g_dirty = true;
    return EVENT_CONTINUE;
}
CREATE_EVENT_LISTENER(cnc_parse_cmd_error, nc2_visual_parse_error);

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
    /* The refusal's own words: the screen is where an operator reads them, and
       the listener is registered *after* the run's, so the line the run stopped
       on is already known when the sentence is built. */
    {
        static bool error_listener;

        if (!error_listener) {
            ADD_EVENT_LISTENER(cnc_parse_cmd_error, nc2_visual_parse_error);
            error_listener = true;
        }
    }
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
        nc2_visual_load_tool_table();
    } else if (!nc2_state_load_document(
                   g_mode == NC2_MODE_RUN ? NC2_MODE_PROGRAM : g_mode, &g_doc)) {
        /* A card that remembers a file it cannot open says so, instead of
           showing an empty program the operator would take for their own: the
           remembered name may be one the card's short names cannot answer to
           (`FACING~1.NC` is what the machine reads for `facing.nc`). */
        if (nc2_state_path(g_mode == NC2_MODE_RUN ? NC2_MODE_PROGRAM : g_mode)[0]) {
            char text[80];

            snprintf(text, sizeof(text), "Cannot open %.48s",
                     nc2_state_path(g_mode == NC2_MODE_RUN ? NC2_MODE_PROGRAM
                                                           : g_mode));
            nc2_statusf(text);
        }
        nc2_document_init(&g_doc);
        (void)nc2_insert_line(&g_doc, 0u, "");
    }
    if (g_mode == NC2_MODE_RUN) {
        /* The run and the editor are the same file, so a boot into RUN opens the
           program - and the tool table the glyph reads is loaded with it. */
        nc2_visual_load_tool_table();
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
    size_t i;

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
    /* The pointer lands on the tool the machine is using: the last `T` the
       program has by the line the editor is on, looked up in this table - so
       opening TOOLS puts the operator on the tool the cut is with, and the
       machine never moves it after that (bench: "we push tool to state of
       mashine but not jump around"). A program that names no tool - or a table
       that does not hold it - leaves the cursor on the first tool row, which is
       where this screen is worth reading from. */
    {
        nc2_tool_t tool;
        int wanted = -1;
        const char *program = nc2_state_path(NC2_MODE_PROGRAM);

        if (program[0]) {
            (void)nc2_tools_program_tool(program,
                                         nc2_state_cursor(NC2_MODE_PROGRAM),
                                         &wanted);
        }
        if (wanted >= 0 && g_doc.line_count &&
            (!nc2_tool_from_line(g_doc.lines[g_doc.cursor], &tool) ||
             tool.t != wanted)) {
            /* The cursor is not on that tool's row already. */
            for (i = 0u; i < g_doc.line_count; i++) {
                if (nc2_tool_from_line(g_doc.lines[i], &tool) &&
                    tool.t == wanted) {
                    g_doc.cursor = i;
                    break;
                }
            }
        }
        if (g_doc.line_count &&
            !nc2_tool_from_line(g_doc.lines[g_doc.cursor], &tool)) {
            for (i = 0u; i < g_doc.line_count; i++) {
                if (nc2_tool_from_line(g_doc.lines[i], &tool)) {
                    g_doc.cursor = i;
                    break;
                }
            }
        }
    }
}

/* The tool table the drawing and the tool view read, loaded once: a FAT read in
   a frame loop is exactly what the frame meter would show. */
static void nc2_visual_load_tool_table(void)
{
    const char *path = nc2_state_path(NC2_MODE_TOOLS);

    if (!path[0]) {
        path = NC2_TOOL_PATH;
    }
    (void)nc2_tools_load(path);
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
    /* TOOLS loads the tool table *into the screen's document*, and a run is
       sending that same document: a look at the tools in the middle of a
       program would have the pacer cut the table. The screen is not allowed
       while the run is armed - the same rule a jog's block has - and the rest
       of the screens are safe (MANUAL has no file; EDIT and RUN are the file the
       run is already sending). */
    if (mode == NC2_MODE_TOOLS && nc2_run_streaming()) {
        nc2_statusf("Program running - TOOLS waits");
        g_dirty = true;
        return;
    }
    if (g_mode == NC2_MODE_MANUAL) {
        nc2_manual_feed_cancel();      /* a jog must not run behind the next screen */
    }
    /* A screen change closes the path builder the way a mode key did in nc: the
       rows it wrote are ordinary program rows and stay. */
    nc2_contour_leave();
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
        nc2_visual_load_tool_table();
    } else if (mode == NC2_MODE_PROGRAM || mode == NC2_MODE_RUN) {
        /* The run and the editor are the same file. The mode key walks EDIT,
           TOOLS, RUN, and TOOLS holds the tool table - so a run that kept
           whatever the screen before it had open would send the table to the
           machine. What was left unsaved has been written by the `save` above. */
        if (!nc2_state_load_document(NC2_MODE_PROGRAM, &g_doc)) {
            nc2_document_init(&g_doc);
            (void)nc2_insert_line(&g_doc, 0u, "");
        }
        if (mode == NC2_MODE_RUN && !nc2_run_active()) {
            nc2_run_set_line(&g_doc, g_doc.cursor);
        }
        if (mode == NC2_MODE_RUN) {
            nc2_visual_load_tool_table();   /* what the glyph rides the cut with */
        }
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
    if (g_mode == NC2_MODE_TOOLS) {
        nc2_visual_load_tool_table();   /* the table the run and the view read */
    }
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
    const char *label;

    if (key < '1' || key > '9') {
        return 0;
    }
    /* The same word the pad draws: the shell labels its own keypad from the
       screen's answer, never from a second copy of it. */
    label = nc2_pad_label(key);
    return (label && label[0]) ? label : 0;
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

    /* G7X's `7` is the path builder: a key that *does* something rather than one
       that writes a row of its own, which is why it is not an entry (`nc`'s own
       reason). It opens the nine directions and stays until `5`. */
    if (strcmp(g_address, "4") == 0 && key == '7') {
        /* The pad's own name line goes back first: the builder writes the
           program's rows below the point, and a title line for an entry that is
           not being inserted would be litter in the operator's contour. */
        nc2_pad_close(&g_doc);
        if (nc2_contour_begin(&g_doc)) {
            char text[48];

            snprintf(text, sizeof(text), "Path: 5 ends, # %.1f mm",
                     (double)nc2_contour_step());
            nc2_statusf(text);
            g_dirty = true;
        }
        return;
    }
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
        /* A value that is picked takes the key: `0` is a digit there, and the
           field flow is one way to type a number, not two. Everywhere else `0`
           is the exit - with a pad up it leaves the pad in one press, which `A`
           cannot do (that is one level), and with nothing to leave it opens the
           card. */
        if (g_doc.field >= 0) {
            editor_key = NC2_KEY_DIGIT;
            break;
        }
        if (g_address[0]) {
            nc2_contour_leave();
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
        nc2_contour_leave();
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
    /* The path builder's keys come first: while it is up, the pad's digits are
       the directions and the just-written row keeps its value picked, so the
       digits that are *not* the builder's reach the editor's field flow. */
    if (nc2_contour_active() && nc2_contour_key(&g_doc, key)) {
        return;
    }
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

    nc2_fill(0, 0, LVDS_VIEW_WIDTH, NC2_HEADER_H, nc2_col_header());
    for (i = 0; i < 4; i++) {
        bool active = order[i] == g_mode;
        int w;

        if (i) {
            x += 10;
        }
        w = nc2_text_width(names[i], LVDS_FONT_NORMAL);
        /* The screen in play wears the block, the way nc's tabs did: its name on
           the panel's own selection colour, so "which screen am I on" is a shape
           and not a word to read (bench: "marking manual tools run with line but
           not with full yellow background as it was before"). */
        if (active) {
            nc2_fill(x - 6, 1, w + 12, NC2_HEADER_H - 2, nc2_col_select());
        }
        nc2_text(x, y, names[i],
                 active ? nc2_col_field_fg() : nc2_col_dim(),
                 active ? nc2_col_select() : nc2_col_header(),
                 LVDS_FONT_NORMAL);
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

        /* The frame meter has the far corner to itself; the message and the
           file stop short of it. */
        int fps_w = (NC2_FPS_COLS + 1) * small_w;

        if (msg_cols > (LVDS_VIEW_WIDTH - fps_w) / (2 * small_w)) {
            msg_cols = (LVDS_VIEW_WIDTH - fps_w) / (2 * small_w);
        }
        msg_x = LVDS_VIEW_WIDTH - 8 - fps_w - msg_cols * small_w;
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
    nc2_draw_fps();
}

/* One row of the program: its number, then the text. The row the cursor (or the
   run) is on wears the selection colour, the rows of the block it heads wear
   the pale one, and the picked field is drawn as the box being typed into -
   three pieces, so everything the operator did not touch keeps its colour. */
static void nc2_draw_row(size_t index, int x, int y, int w, bool selected,
                         bool path)
{
    const char *text = g_doc.lines[index];
    bool cursor = selected;
    lvds_color_t bg = cursor ? nc2_col_select()
                             : (path ? nc2_col_block() : nc2_col_bg());
    int col_w = nc2_col_width(LVDS_FONT_NORMAL);
    char number[8];
    int text_x = x + NC2_LINE_NO_PAD;
    int cols;
    nc2_field_t fields[NC2_MAX_FIELDS];
    int count;
    int picked = -1;

    if (cursor || path) {
        nc2_fill(x, y - 2, w - 2, NC2_ROW_H - 2,
                 cursor ? nc2_col_select() : nc2_col_block());
    }
    snprintf(number, sizeof(number), "%3u", (unsigned)(index + 1u));
    nc2_text(text_x, y, number, nc2_col_dim(), bg, LVDS_FONT_NORMAL);
    text_x += 4 * col_w;
    cols = (w - (text_x - x) - 4) / col_w;
    if (cols <= 0) {
        return;
    }
    count = nc2_fields(text, fields, NC2_MAX_FIELDS);
    if (cursor && g_mode == NC2_MODE_PROGRAM && g_doc.field >= 0 &&
        g_doc.field < count) {
        picked = g_doc.field;
    }
    if (picked < 0) {
        nc2_text_clip(text_x, y, text, cols, nc2_col_text(), bg,
                      LVDS_FONT_NORMAL);
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
        nc2_text_clip(text_x, y, text, head, nc2_col_text(), bg,
                      LVDS_FONT_NORMAL);
        nc2_text_clip(text_x + head * col_w, y, text + f->start, field,
                      nc2_col_field_fg(), nc2_col_field_bg(), LVDS_FONT_NORMAL);
        nc2_text_clip(text_x + (head + field) * col_w, y, text + f->end,
                      cols - head - field, nc2_col_text(), bg, LVDS_FONT_NORMAL);
    }
}

/* The rows of the file the screen has open, in the rectangle it is handed: the
   line numbers, the text, the mark on the line in play (or the cursor's) and the
   pale block around it - and the word under the cursor named on the row above.
   The window moves only when the cursor would leave it: six rows of the file
   after the cursor's stay in view, because in G-code the next block is what the
   operator is reading for. */
static void nc2_draw_rows(int x, int y, int w, int h)
{
    int rows = h / NC2_ROW_H;
    size_t mark = (g_mode == NC2_MODE_RUN) ? nc2_visual_run_line()
                                           : g_doc.cursor;
    size_t first;
    size_t path_first = 0u;
    size_t path_last = 0u;
    bool have_path = false;
    int i;

    if (rows <= 0) {
        return;
    }
    /* The line in play heads a path: the cycle's rows are the pale mark beside
       it, on the run screen and in the editor alike - the editor is where the
       program is read, so the block the cursor sits in is the first thing its
       own screen should say. */
    {
        g7x_doc_t view = nc2_document_g7x(&g_doc);

        have_path = g7x_doc_line_path(&view, mark, &path_first, &path_last);
    }
    /* The window: full at the end of the file, and never letting the cursor
       come closer than the look-ahead to the pane's last row. */
    if (g_doc.line_count > (size_t)rows &&
        g_pane_first + (size_t)rows > g_doc.line_count) {
        g_pane_first = g_doc.line_count - (size_t)rows;
    }
    if (mark < g_pane_first) {
        g_pane_first = mark;
    }
    if (mark > g_pane_first + (size_t)(rows - 1 - NC2_LOOKAHEAD_ROWS)) {
        g_pane_first = mark - (size_t)(rows - 1 - NC2_LOOKAHEAD_ROWS);
    }
    first = g_pane_first;
    if (first > mark) {
        first = mark;
    }
    for (i = 0; i < rows && first + (size_t)i < g_doc.line_count; i++) {
        size_t index = first + (size_t)i;
        bool selected = index == mark;
        bool path = have_path && index >= path_first && index <= path_last &&
                    !selected;

        nc2_draw_row(index, x, y + 4 + i * NC2_ROW_H, w, selected, path);
    }
    /* The legend of the word being read, on the row above the cursor's - the
       row the operator's eye is already on, so the meaning of the word they are
       typing is where they are looking (bench: "on edit - no legend in top
       row"). On the first row of the pane there is no row above, so it takes the
       pane's own top line, as nc's did. A run draws none: nothing is being typed
       there. */
    if (g_mode != NC2_MODE_RUN && g_doc.field >= 0) {
        nc2_field_t fields[NC2_MAX_FIELDS];
        int count = nc2_document_fields(&g_doc, fields, NC2_MAX_FIELDS);

        if (g_doc.field < count) {
            int at = (int)(g_doc.cursor - first);
            int hint_y = at > 0 ? y + 4 + (at - 1) * NC2_ROW_H : y + 4;
            char text[48];
            int cols = (w - NC2_LINE_TEXT_PAD - 4) /
                       nc2_col_width(LVDS_FONT_NORMAL);

            snprintf(text, sizeof(text), ">  %s",
                     nc2_vocab_label(g_doc.lines[g_doc.cursor],
                                     &fields[g_doc.field]));
            nc2_fill(x, hint_y - 2, w - 2, NC2_ROW_H - 2, nc2_col_bg());
            nc2_text_clip(x + NC2_LINE_TEXT_PAD, hint_y, text, cols,
                          nc2_col_accent(), nc2_col_bg(), LVDS_FONT_NORMAL);
        }
    }
}

/* The card: what is in the folder the operator is looking at, the one picked lit,
   and the name being typed for a new file. It is the whole bottom band while it
   is up - a picker, with no pad: the digits are the new file's name there. */
static void nc2_draw_list(void)
{
    const int list_x = NC2_TEXT_X;
    const int list_y = NC2_TEXT_Y;
    const int list_w = NC2_PAD_X + NC2_PAD_W - NC2_TEXT_X;
    const int list_h = NC2_TEXT_H;
    int rows = (list_h - 40) / NC2_ROW_H;
    int i;

    nc2_text_clip(list_x + 4, list_y + 6, nc2_file_dir(),
                  (list_w - 12) / nc2_col_width(LVDS_FONT_NORMAL),
                  nc2_col_text(), nc2_col_bg(), LVDS_FONT_NORMAL);
    for (i = 0; i < nc2_file_count() && i < rows; i++) {
        const nc2_file_entry_t *entry = nc2_file_entry(i);
        int y = list_y + 30 + i * NC2_ROW_H;
        char line[NC2_NAME_MAX + 4];

        if (i == nc2_file_selected()) {
            nc2_fill(list_x + 2, y - 2, list_w - 6, NC2_ROW_H - 2,
                     nc2_col_select());
        }
        snprintf(line, sizeof(line), "%s%s", entry->name,
                 entry->is_dir ? "/" : "");
        nc2_text_clip(list_x + 8, y, line, (list_w - 16) / 8, nc2_col_text(),
                      i == nc2_file_selected() ? nc2_col_select() : nc2_col_bg(),
                      LVDS_FONT_NORMAL);
    }
    if (nc2_file_new_active()) {
        char line[NC2_NAME_MAX + 8];

        snprintf(line, sizeof(line), "NEW  %s.nc",
                 nc2_file_new_name()[0] ? nc2_file_new_name() : "_");
        nc2_text_clip(list_x + 8, list_y + list_h - 24, line, 40,
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
    /* The builder's own pad: nine directions, in key order. */
    if (nc2_contour_active()) {
        return nc2_contour_label(key);
    }
    if (g_mode == NC2_MODE_RUN) {
        return g_nc2_run_labels[key - '1'];
    }
    if (g_mode == NC2_MODE_MANUAL) {
        return nc2_manual_pad_label(key);
    }
    /* Under G7X, `7` is the path builder - the one cell the card cannot fill. */
    if (g_address[0] == '4' && g_address[1] == '\0' && key == '7') {
        return "PATH";
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

    if (nc2_contour_active()) {
        snprintf(line, sizeof(line), "PATH  step %.1f mm",
                 (double)nc2_contour_step());
    } else if (g_mode == NC2_MODE_RUN) {
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

/* The notes in the space above the 3x3 (the bench: *"use space above 3x3 to fit
   labels like errors or big message or helpers"*).

   An error goes first and in the fault's own red - it is the one thing an
   operator must not miss, and it is said here so it cannot be lost among the
   figures on the strip. What the screen's keys do follows, from the screen's own
   answer (`nc2_visual_usage()`, the same lines the station's side strip shows),
   so the machine carries its own help instead of the PC being the only place it
   is written. */
static void nc2_draw_notes(void)
{
    const char *const *lines = 0;
    size_t count = nc2_visual_usage(&lines);
    nc2_runtime_state_t rt;
    bool fault;
    int y = NC2_NOTES_Y + 2;
    int msg_cols = (NC2_NOTES_W - 8) / nc2_col_width(LVDS_FONT_NORMAL);
    int cols = (NC2_NOTES_W - 8) / nc2_col_width(LVDS_FONT_SMALL);
    size_t i;

    nc2_state_runtime(&rt);
    fault = nc2_state_is_fault(&rt);
    if (g_status[0] || fault) {
        char text[64];

        if (g_status[0]) {
            snprintf(text, sizeof(text), "%s", g_status);
        } else {
            snprintf(text, sizeof(text), "uCNC %s", nc2_state_label(&rt));
        }
        nc2_text_clip(NC2_NOTES_X + 4, y, text, msg_cols,
                      fault ? nc2_col_error() : nc2_col_accent(), nc2_col_bg(),
                      LVDS_FONT_NORMAL);
        y += 18;
    }
    for (i = 0u; i < count && y + 8 <= NC2_NOTES_BOTTOM; i++) {
        nc2_text_clip(NC2_NOTES_X + 4, y, lines[i], cols, nc2_col_dim(),
                      nc2_col_bg(), LVDS_FONT_SMALL);
        y += 11;
    }
}

/* The tool the cursor is on, drawn in the pane the tools screen leaves for it:
   its shape, tip on the crosshair of its own X0/Z0, and the numbers its row
   holds under it. The names are the table's own letters - the same file the
   rows above edit, read the other way (bench: *"on tools - bring it back just
   fit into current screen so top one is text lines, bottom one is tool
   view"*). */
static void nc2_draw_tool_view(int x, int y, int w, int h)
{
    static const struct {
        char letter;
        const char *name;
    } params[] = {
        { 'T', "Tool" }, { 'O', "Orient" }, { 'R', "Radius" }, { 'D', "DOC" },
        { 'E', "FDOC" }, { 'F', "Feed" }, { 'Q', "F.feed" }, { 'S', "RPM" },
        { 'X', "X offset" }, { 'Z', "Z offset" }
    };
    const char *row = (g_doc.line_count && g_doc.cursor < g_doc.line_count)
                          ? g_doc.lines[g_doc.cursor]
                          : "";
    nc2_tool_t tool;
    int box = nc2_clampi(w - 80, 80, 170);
    int gx = x + (w - box) / 2;
    int gy = y + 26;
    int cx = gx + box / 2;
    int cy = gy + box / 2;
    int line_y = gy + box + 16;
    char buf[48];
    size_t i;

    if (nc2_tool_from_line(row, &tool)) {
        snprintf(buf, sizeof(buf), "TOOL T%d", tool.t);
    } else {
        memset(&tool, 0, sizeof(tool));
        snprintf(buf, sizeof(buf), "TOOL  -");
    }
    nc2_text_clip(x + 4, y + 2, buf, (w - 8) / nc2_col_width(LVDS_FONT_NORMAL),
                  nc2_col_text(), nc2_col_bg(), LVDS_FONT_NORMAL);

    /* The crosshair is the tool's own X0/Z0: the picture is read against it, so
       it goes on the glass before the tool does. */
    lvds_draw_line(gx, cy, gx + box, cy, nc2_col_dim());
    lvds_draw_line(cx, gy, cx, gy + box, nc2_col_dim());
    nc2_text_clip(gx + 2, cy + 4, "X0", 2, nc2_col_dim(), nc2_col_bg(),
                  LVDS_FONT_SMALL);
    nc2_text_clip(cx + 4, gy + box - 10, "Z0", 2, nc2_col_dim(), nc2_col_bg(),
                  LVDS_FONT_SMALL);
    if (tool.valid) {
        nc2_draw_tool_glyph_centered(gx, gy, box, box / 2, &tool,
                                     nc2_col_bg());
    }

    for (i = 0u; i < sizeof(params) / sizeof(params[0]); i++) {
        char value[24];
        int cols = (w - 96) / nc2_col_width(LVDS_FONT_NORMAL);

        if (!nc2_tool_word_text(row, params[i].letter, value, sizeof(value))) {
            snprintf(value, sizeof(value), "-");
        }
        nc2_text_clip(x + 4, line_y, params[i].name, 10, nc2_col_dim(),
                      nc2_col_bg(), LVDS_FONT_NORMAL);
        nc2_text_clip(x + 92, line_y, value, cols, nc2_col_text(), nc2_col_bg(),
                      LVDS_FONT_NORMAL);
        line_y += 18;
        if (line_y + 18 > y + h) {
            break;
        }
    }
}

/* The machine's own strip, across the middle of the screen: the work position,
   the feed and the spindle, and the state word, on one line. It is the DRO and
   the line between the two halves at once - the top is what the machine is
   making, the bottom is what the operator types - so it is drawn on every
   screen and never moves. Its colour is the machine's: the panel's grey while
   nothing is happening, the run's green while it moves, the fault's red when
   something is wrong. */
static void nc2_draw_dro(void)
{
    nc2_runtime_state_t rt;
    float work[AXIS_COUNT] = { 0 };
    lvds_color_t bg;
    const char *state;
    bool fault;
    char buf[40];
    int x = NC2_DRO_X + 8;
    int y = NC2_DRO_Y + (NC2_DRO_H - 14) / 2;

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
    /* The work position first, in the two axes a lathe hand reads - X a
       diameter, Z the length - then the feed and the spindle's speed, and the
       state word at the far end where it is out of the figures' way. */
    nc2_text(x, y, "X", nc2_col_dim(), bg, LVDS_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "%9.3f", (double)work[AXIS_X]);
    nc2_text_clip(x + 16, y, buf, 9, nc2_col_text(), bg, LVDS_FONT_NORMAL);
    x += 100;
    nc2_text(x, y, "Z", nc2_col_dim(), bg, LVDS_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "%9.3f", (double)work[AXIS_Z]);
    nc2_text_clip(x + 16, y, buf, 9, nc2_col_text(), bg, LVDS_FONT_NORMAL);
    x += 100;
    nc2_text(x, y, "F", nc2_col_dim(), bg, LVDS_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "%7.1f", (double)rt.feed);
    nc2_text_clip(x + 16, y, buf, 7, nc2_col_text(), bg, LVDS_FONT_NORMAL);
    x += 84;
    nc2_text(x, y, "S", nc2_col_dim(), bg, LVDS_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "%7u", rt.spindle);
    nc2_text_clip(x + 16, y, buf, 7, nc2_col_text(), bg, LVDS_FONT_NORMAL);

    snprintf(buf, sizeof(buf), "uCNC %s", state);
    nc2_text_clip(NC2_DRO_X + NC2_DRO_W - 8 - nc2_text_width(buf, LVDS_FONT_NORMAL),
                  y, buf, 16, nc2_col_dim(), bg, LVDS_FONT_NORMAL);
}

/* The drawing, every screen and whatever the screen has open: the machine is
   making a part, and the part belongs at the top where the operator looks. What
   the machine is doing is read once, and the tool the program has chosen is what
   the glyph rides the cut with. */
static void nc2_draw_preview(bool full)
{
    nc2_runtime_state_t rt;
    nc2_preview_run_t run;
    nc2_tool_t tool;

    nc2_state_runtime(&rt);
    run.busy = nc2_state_busy() || nc2_run_active() || nc2_run_hold();
    run.screen_run = g_mode == NC2_MODE_RUN;
    run.full = full;
    run.x = rt.x;
    run.z = rt.z;
    run.tool = 0;
    if (run.busy && nc2_tools_active(&g_doc, nc2_visual_run_line(), &tool)) {
        run.tool = &tool;
    }
    nc2_preview_draw(&g_doc, &run, NC2_PREVIEW_X, NC2_PREVIEW_Y, NC2_PREVIEW_W,
                     NC2_PREVIEW_PANE_H);
}

/* What the last frame showed that does not move on its own, in two bands: the
   rows of the file the pane draws, and what the screen wears around them (the
   header, the notes, the pad and its caption). While neither changes, only the
   drawing and the machine's strip are painted again; when only the *mark* moves
   - which is what a run does - the rows are painted and the chrome is left
   alone. The bench: "fps is dead slow - again full screen is refreshed not only
   preview area?" */
typedef struct {
    nc2_mode_t mode;
    bool list;
    size_t cursor;
    size_t mark;                /* the line the rows draw their mark on */
    int field;
    size_t lines;
    char address[NC2_ADDR_MAX + 1];
} nc2_rows_key_t;

typedef struct {
    nc2_mode_t mode;
    bool list;
    bool fault;
    char pad_hot;               /* MANUAL's key flash, and the run's hold */
    char status[64];
    char address[NC2_ADDR_MAX + 1];
    uint16_t fps;
} nc2_chrome_key_t;

static nc2_rows_key_t g_rows_key;
static nc2_chrome_key_t g_chrome_key;
static bool g_frame_key_valid;

static void nc2_frame_keys_of(nc2_rows_key_t *rows, nc2_chrome_key_t *chrome)
{
    nc2_runtime_state_t rt;

    memset(rows, 0, sizeof(*rows));
    rows->mode = g_mode;
    rows->list = g_list;
    rows->cursor = g_doc.cursor;
    rows->mark = (g_mode == NC2_MODE_RUN) ? nc2_visual_run_line()
                                          : g_doc.cursor;
    rows->field = g_doc.field;
    rows->lines = g_doc.line_count;
    snprintf(rows->address, sizeof(rows->address), "%s", g_address);

    memset(chrome, 0, sizeof(*chrome));
    chrome->mode = g_mode;
    chrome->list = g_list;
    nc2_state_runtime(&rt);
    chrome->fault = nc2_state_is_fault(&rt);
    chrome->pad_hot = nc2_pad_hot();
    snprintf(chrome->status, sizeof(chrome->status), "%s", g_status);
    snprintf(chrome->address, sizeof(chrome->address), "%s", g_address);
    chrome->fps = g_nc2_fps;
}

void nc2_visual_draw(void)
{
    nc2_rows_key_t rows_key;
    nc2_chrome_key_t chrome_key;
    uint32_t t_start;
    uint32_t t_band;
    bool full;
    bool rows;
    bool chrome;

    if (nc2_boot_active()) {
        nc2_boot_draw();
        /* The renderer draws into the PSRAM backbuffer and the scanout reads
           SRAM: without this the panel keeps showing whatever it was showing -
           black on a cold boot. It is a no-op on the host backend, which is
           why every host frame looked right while the glass stayed dark. */
        lvds_hstx_present();
        return;
    }
    t_start = mcu_micros();
    t_band = t_start;
    nc2_frame_keys_of(&rows_key, &chrome_key);
    /* A mode or a screen change paints the lot: the bands are in different
       places then. Anything else is per band. */
    full = g_dirty || !g_frame_key_valid ||
           rows_key.mode != g_rows_key.mode || rows_key.list != g_rows_key.list;
    rows = full || memcmp(&rows_key, &g_rows_key, sizeof(rows_key)) != 0;
    chrome = full ||
             memcmp(&chrome_key, &g_chrome_key, sizeof(chrome_key)) != 0;
    if (full) {
        nc2_fill(0, 0, LVDS_VIEW_WIDTH, LVDS_VIEW_HEIGHT, nc2_col_bg());
    }
    t_band = mcu_micros();
    if (full || chrome) {
        nc2_draw_header();
    }
    g_nc2_acc_screen_us += mcu_micros() - t_band;   /* the header */
    /* The two bands that move on their own: the drawing (the machine's own
       position, and the tool with it) and the strip's figures. Everything else
       is drawn when something it shows has changed. */
    nc2_draw_preview(full);
    {
        uint32_t stock_us = 0u;
        uint32_t geom_us = 0u;

        /* What the drawing itself spent its time in (the preview knows). */
        nc2_preview_times(&stock_us, &geom_us);
        g_nc2_acc_stock_us += stock_us;
        g_nc2_acc_geom_us += geom_us;
    }
    t_band = mcu_micros();
    nc2_draw_dro();
    g_nc2_acc_strip_us += mcu_micros() - t_band;
    if (g_list) {
        /* The list is one column, not the editor's pane: it takes the whole
           bottom band while it is up, the way a picker does. */
        if (rows) {
            nc2_fill(NC2_TEXT_X, NC2_TEXT_Y,
                     NC2_PAD_X + NC2_PAD_W - NC2_TEXT_X, NC2_TEXT_H,
                     nc2_col_bg());
            nc2_draw_list();
        }
    } else if (g_mode == NC2_MODE_MANUAL) {
        /* MANUAL is a machine panel: the pane carries the stops and the value
           the keys change and the pad its jog keys. */
        if (rows) {
            nc2_fill(NC2_TEXT_X, NC2_TEXT_Y, NC2_TEXT_W, NC2_TEXT_H,
                     nc2_col_bg());
            nc2_manual_draw_pane(NC2_TEXT_X, NC2_TEXT_Y, NC2_TEXT_W,
                                 NC2_TEXT_H);
        }
        if (full || chrome) {
            nc2_draw_notes();
            nc2_draw_pad_band();
        }
    } else if (g_mode == NC2_MODE_TOOLS) {
        /* The tools screen: the table's own rows in the pane under the header -
           they are the text the operator edits, and the top half is where the
           eye reads it - and the tool the cursor is on drawn in the pane below
           (bench: "on tools - bring it back just fit into current screen so top
           one is text lines, bottom one is tool view"). */
        if (full) {
            nc2_fill(NC2_PREVIEW_X, NC2_PREVIEW_Y, NC2_PREVIEW_W,
                     NC2_PREVIEW_PANE_H, nc2_col_bg());
        }
        if (rows) {
            nc2_fill(NC2_PREVIEW_X, NC2_PREVIEW_Y, NC2_PREVIEW_W,
                     NC2_PREVIEW_PANE_H, nc2_col_bg());
            nc2_draw_rows(NC2_PREVIEW_X, NC2_PREVIEW_Y, NC2_PREVIEW_W,
                          NC2_PREVIEW_PANE_H);
            nc2_fill(NC2_TEXT_X, NC2_TEXT_Y, NC2_TEXT_W, NC2_TEXT_H,
                     nc2_col_bg());
            nc2_draw_tool_view(NC2_TEXT_X, NC2_TEXT_Y, NC2_TEXT_W, NC2_TEXT_H);
        }
        if (full || chrome) {
            nc2_draw_notes();
            nc2_draw_pad_band();
        }
    } else {
        if (rows) {
            nc2_fill(NC2_TEXT_X, NC2_TEXT_Y, NC2_TEXT_W, NC2_TEXT_H,
                     nc2_col_bg());
            nc2_draw_rows(NC2_TEXT_X, NC2_TEXT_Y, NC2_TEXT_W, NC2_TEXT_H);
        }
        if (full || chrome) {
            nc2_draw_notes();
            nc2_draw_pad_band();
        }
    }
    g_nc2_acc_screen_us += mcu_micros() - t_band;   /* the bands around it */
    g_rows_key = rows_key;
    g_chrome_key = chrome_key;
    g_frame_key_valid = true;
    nc2_visual_clear_dirty();
    t_band = mcu_micros();
    g_nc2_acc_draw_us += t_band - t_start;
    lvds_hstx_present();
    g_nc2_acc_present_us += mcu_micros() - t_band;
    nc2_fps_tick();
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
