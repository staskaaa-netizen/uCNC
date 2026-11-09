#include "ncc_visual.h"
#include "ncc_visual_internal.h"

#include "ncc.h"
#include "ncc_boot.h"
#include "ncc_draw.h"
#include "ncc_files.h"
#include "ncc_layout.h"
#include "ncc_manual.h"
#include "ncc_preview.h"
#include "ncc_presets.h"
#include "../nc_sender/nc_sender.h"
#include "ncc_state.h"
#include "ncc_tools.h"
#include "ncc_vocab.h"

#include "../../cnc.h"
#include "../../core/interpolator.h"
#include "../../core/parser.h"
#include "../../interface/grbl_stream.h"
#include "../lvds_renderer/lvds_hstx.h"
#include "../g7x/g7x.h"

#include <stdio.h>
#include <string.h>

static ncc_document_t g_doc;
static char g_address[NCC_ADDR_MAX + 1];
static char g_status[NCC_STATUS_MAX];
/* What the panel handed over for the line the controller refused: the block,
   where the sentence says only which line of the program it belongs to. */
static char g_refused[NCC_STATUS_MAX];
static char g_labels[9][NCC_PRESET_ROW_MAX];
static bool g_dirty;
static bool g_list;                 /* the file list is the screen */
static bool g_name_selected;        /* the pane's file-name row holds the cursor */
static ncc_mode_t g_mode = NCC_MODE_PROGRAM;
static uint32_t g_last_key_ms;      /* when the screen was last touched */
static g7x_doc_t g_run_source;

static bool ncc_visual_tool_radius(void *user, int tool, float *radius)
{
    ncc_tool_t row;

    (void)user;
    if (!radius || tool <= 0 || !ncc_tools_number(tool, &row) ||
        row.r <= 0.0f) {
        return false;
    }
    *radius = row.r;
    return true;
}

static void ncc_visual_run_status(void *user, const char *text)
{
    (void)user;
    ncc_visual_status_set(text);
}

static g7x_doc_t *ncc_visual_run_source(void)
{
    g_run_source = ncc_document_g7x(&g_doc);
    return &g_run_source;
}

/* The panel writes the program back itself once the operator has left it alone
   for a moment, the way nc does: the file on the card is what runs, so an edit
   that is never flushed is an edit that is not there. */
#define NCC_IDLE_SAVE_MS 1500u

static void ncc_visual_load_tools(void);
static void ncc_visual_load_tool_table(void);
static void ncc_statusf(const char *text);

/* The wording of a refused line: the module that refused it knows *why* and
   hands its reason over (`g7x_take_refusal_text()` - reading it takes it, so it
   can never explain a later line); otherwise the controller's own code name is
   all there is. `nc`'s `nc_feedback_error()`, kept where the panel's words
   live. */
static const char *ncc_error_text(uint8_t error)
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
static bool ncc_visual_parse_error(void *args)
{
    uint8_t error = *(uint8_t *)args;
    char text[96];

    if (nc_sender_run_error()) {
        snprintf(text, sizeof(text), "Line %lu error %u: %s",
                 (unsigned long)(nc_sender_run_error_line() + 1u), (unsigned)error,
                 ncc_error_text(error));
        /* And *what* was refused: the fault names the line of the program it
           belongs to, and that line is a cycle as often as not - the whole of
           the block is what went out, and which block it was is the one thing
           the program's own line cannot say. It is read with the sentence, in
           the block, and again on the console for a log to pick up. */
        snprintf(g_refused, sizeof(g_refused), "%s", nc_sender_run_refused_text());
        if (g_refused[0]) {
            grbl_stream_printf("[MSG:NCC REFUSED %.96s]\r\n", g_refused);
        }
    } else {
        snprintf(text, sizeof(text), "Error %u: %s", (unsigned)error,
                 ncc_error_text(error));
        g_refused[0] = '\0';
    }
    ncc_statusf(text);
    g_dirty = true;
    return EVENT_CONTINUE;
}
CREATE_EVENT_LISTENER(cnc_parse_cmd_error, ncc_visual_parse_error);

static void ncc_statusf(const char *text)
{
    snprintf(g_status, sizeof(g_status), "%s", text ? text : "");
}

void ncc_visual_status_set(const char *text)
{
    ncc_statusf(text);
    g_dirty = true;
}

void ncc_visual_mark_dirty(void)
{
    g_dirty = true;
}

/* The nine labels of the address the operator is standing at. Read every frame
   the pad is drawn: nine small files on a card that answers in microseconds, and
   a cache would be a second copy of what the card already says. */
/* The address the pad *reads* from. The screens share one pad, and its entries
   are files - but a screen can have a subject of its own: the TOOLS screen has
   the tool table open, so its pad is the tool group, because pressing a
   program's cycle entry into a tool table is what the bench called wrong
   ("on ttols - 3x3 is wrong here"). What the operator walks with `A` and leaves
   with `0` is still the screen's own address: on TOOLS the root *is* the tool
   group's level, so `A` leaves the pad the way it does everywhere. */
static const char *ncc_pad_root(void)
{
    return g_mode == NCC_MODE_TOOLS ? "7" : "";
}

static void ncc_labels_at(const char *address)
{
    char full[NCC_ADDR_MAX + 3];
    char key;

    snprintf(full, sizeof(full), "%s%s", ncc_pad_root(), address ? address : "");
    for (key = '1'; key <= '9'; key++) {
        int kind = ncc_slot(full, key, g_labels[key - '1'],
                            sizeof(g_labels[0]));

        if (kind == NCC_SLOT_EMPTY) {
            g_labels[key - '1'][0] = '\0';
        }
    }
}

void ncc_visual_init(void)
{
    /* The picture first, before anything asks for a panel colour: the palette
       holds sixteen and the panel's own colours are taken as they are used, so
       a splash raised afterwards would find the slots full and be painted in
       the panel's greys - which is exactly what the glass did while the host,
       which has no palette, showed the picture in full colour (bench, and the
       reason the host backend now keeps a real palette too). */
    ncc_boot_start();
    ncc_draw_init();
    ncc_document_init(&g_doc);
    ncc_address_reset(g_address);
    g_name_selected = false;
    ncc_statusf("");
    g_dirty = true;
    nc_sender_run_init(ncc_visual_run_status, 0, ncc_visual_tool_radius, 0);
    /* The refusal's own words: the screen is where an operator reads them, and
       the listener is registered *after* the run's, so the line the run stopped
       on is already known when the sentence is built. */
    {
        static bool error_listener;

        if (!error_listener) {
            ADD_EVENT_LISTENER(cnc_parse_cmd_error, ncc_visual_parse_error);
            error_listener = true;
        }
    }
    ncc_state_init();
    switch (ncc_state_mode()) {
    case NCC_MODE_MANUAL:
    case NCC_MODE_RUN:
    case NCC_MODE_TOOLS:
        g_mode = ncc_state_mode();
        break;
    default:
        g_mode = NCC_MODE_PROGRAM;
        break;
    }
    /* The entries live in RAM: the shipped table is laid down now, and the one
       card file - if the card carries one - is read by the first sync the drive
       answers. Nothing is written to the card. */
    ncc_presets_init();
    /* Onto the file the panel had open, and where it was in it. A card that
       remembers nothing opens on an empty program, which is a program. */
    if (g_mode == NCC_MODE_TOOLS) {
        ncc_visual_load_tools();
        ncc_visual_load_tool_table();
    } else if (!ncc_state_load_document(
                   g_mode == NCC_MODE_RUN ? NCC_MODE_PROGRAM : g_mode, &g_doc)) {
        /* A card that remembers a file it cannot open says so, instead of
           showing an empty program the operator would take for their own: the
           remembered name may be one the card's short names cannot answer to
           (`FACING~1.NC` is what the machine reads for `facing.nc`). */
        if (ncc_state_path(g_mode == NCC_MODE_RUN ? NCC_MODE_PROGRAM : g_mode)[0]) {
            char text[80];

            snprintf(text, sizeof(text), "Cannot open %.48s",
                     ncc_state_path(g_mode == NCC_MODE_RUN ? NCC_MODE_PROGRAM
                                                           : g_mode));
            ncc_statusf(text);
        }
        ncc_document_init(&g_doc);
        (void)ncc_insert_line(&g_doc, 0u, "");
    }
    if (g_mode == NCC_MODE_RUN || g_mode == NCC_MODE_PROGRAM) {
        /* The run and the editor are the same file. The tool table is read here
           for **both**: the RUN screen's tip rides the cut with it, and the
           editor's offset line needs the tool's own nose radius - without this
           the drawing shifted by the built-in row's `R0.8` whatever the card's
           table said, and three shifts of the wrong radius collapse onto the
           programmed path (bench, 2026-09-28: *"only line is seen here? or its
           takes no tool without running?"*). */
        ncc_visual_load_tool_table();
    }
    ncc_labels_at(g_address);
}

/* The tool table ncc ships when the card has none: the same row nc's default
   writes, because TOOLS edits one table whichever module is driving. */
#define NCC_TOOL_DEFAULT "T1 R0.8 F120 Q60 D2.0 E0.5 S800 X0 Z0"

/* TOOLS is the tool table, and the table is a file like any other: the screen is
   the editor on `/D/nc/files/tool.t`, with the table's own first row when the
   card has none. */
static void ncc_visual_load_tools(void)
{
    const char *path = ncc_state_path(NCC_MODE_TOOLS);
    size_t i;

    if (!path[0]) {
        path = NCC_TOOL_PATH;
    }
    if (!ncc_file_load(&g_doc, path)) {
        ncc_document_init(&g_doc);
        (void)ncc_insert_line(&g_doc, 0u, NCC_TOOL_DEFAULT);
        snprintf(g_doc.path, sizeof(g_doc.path), "%s", path);
        (void)ncc_file_save(&g_doc);
        ncc_statusf("New tool table");
    }
    ncc_state_remember_path(NCC_MODE_TOOLS, g_doc.path);
    /* The pointer lands on the tool the machine is using, and it takes the
       *machine's* answer first: the last `T` the run handed over, which is the
       tool in the spindle. Only a machine that has not run anything falls back
       to reading the program (its last `T`, not the one at the editor's line -
       the editor's cursor sits at the top of the file, and reading from there
       finds nothing and left the pointer jumping to the first row: bench, "it
       still wants to jump to 1 line ... forget to take tool latest position").
       Nothing moves it after that but the operator's own keys. */
    {
        ncc_tool_t tool;
        int wanted = nc_sender_run_tool();
        const char *program = ncc_state_path(NCC_MODE_PROGRAM);

        if (wanted < 0 && program[0]) {
            (void)ncc_tools_program_tool(program, (size_t)-1, &wanted);
        }
        if (wanted >= 0 && g_doc.line_count &&
            (!ncc_tool_from_line(g_doc.lines[g_doc.cursor], &tool) ||
             tool.t != wanted)) {
            /* The cursor is not on that tool's row already. */
            for (i = 0u; i < g_doc.line_count; i++) {
                if (ncc_tool_from_line(g_doc.lines[i], &tool) &&
                    tool.t == wanted) {
                    g_doc.cursor = i;
                    break;
                }
            }
        }
        if (g_doc.line_count &&
            !ncc_tool_from_line(g_doc.lines[g_doc.cursor], &tool)) {
            for (i = 0u; i < g_doc.line_count; i++) {
                if (ncc_tool_from_line(g_doc.lines[i], &tool)) {
                    g_doc.cursor = i;
                    break;
                }
            }
        }
    }
}

/* The tool table the drawing and the tool view read, loaded once: a FAT read in
   a frame loop is exactly what the frame meter would show. */
static void ncc_visual_load_tool_table(void)
{
    const char *path = ncc_state_path(NCC_MODE_TOOLS);

    if (!path[0]) {
        path = NCC_TOOL_PATH;
    }
    (void)ncc_tools_load(path);
}

/* Jump to a screen: the program (and the run, which shows the same file), the
   tool table, and MANUAL. Each keeps what it had open, so a look at the tools
   does not lose the program's place. */
void ncc_visual_select_mode(ncc_mode_t mode)
{
    ncc_mode_t from = g_mode;

    if (mode != NCC_MODE_PROGRAM && mode != NCC_MODE_RUN &&
        mode != NCC_MODE_MANUAL && mode != NCC_MODE_TOOLS) {
        return;
    }
    if (mode == g_mode) {
        return;
    }
    /* The file-name row belongs to the screen it was reached on. */
    g_name_selected = false;
    /* TOOLS loads the tool table *into the screen's document*, and a run is
       sending that same document: a look at the tools in the middle of a
       program would have the pacer cut the table. The screen is not allowed
       while the run is armed - the same rule a jog's block has - and the rest
       of the screens are safe (MANUAL has no file; EDIT and RUN are the file the
       run is already sending). */
    if (mode == NCC_MODE_TOOLS && nc_sender_run_streaming()) {
        ncc_statusf("Program running - TOOLS waits");
        g_dirty = true;
        return;
    }
    if (from == NCC_MODE_MANUAL) {
        ncc_manual_feed_cancel();      /* a jog must not run behind the next screen */
    }
    /* A screen change closes the path builder the way a mode key did in nc: the
       rows it wrote are ordinary program rows and stay. */
    ncc_contour_leave();
    if (from == NCC_MODE_PROGRAM || from == NCC_MODE_TOOLS) {
        (void)ncc_visual_save();       /* whatever this screen had open */
    }
    g_list = false;
    g_mode = mode;
    ncc_state_set_mode(mode);
    ncc_state_flush();             /* the screen is what the card remembers */
    ncc_address_reset(g_address);
    g_name_selected = false;
    ncc_labels_at(g_address);
    if (mode == NCC_MODE_TOOLS) {
        ncc_visual_load_tools();
        ncc_visual_load_tool_table();
    } else if (mode == NCC_MODE_PROGRAM || mode == NCC_MODE_RUN) {
        /* The run and the editor are the same file. The mode key walks EDIT,
           TOOLS, RUN, and TOOLS holds the tool table - so a run that kept
           whatever the screen before it had open would send the table to the
           machine. What was left unsaved has been written by the `save` above.

           Only TOOLS puts something *else* in the buffer, so only TOOLS has to
           give the program back. Coming from RUN the document already **is**
           the program - and while a run is in flight that buffer is the one the
           pacer is cutting, so re-reading the card under it swapped the program
           the machine was half way through (and a card read inside the frame
           loop is the delay the tool table is loaded once to avoid; bench,
           2026-09-28: *"if i go from run while it is runnig to edit i am stuck
           here"*). Coming from MANUAL there is nothing to give back either, and
           an empty buffer - a screen that never had a program - still reads the
           card. */
        if (from == NCC_MODE_TOOLS || g_doc.line_count == 0u) {
            if (!ncc_state_load_document(NCC_MODE_PROGRAM, &g_doc)) {
                ncc_document_init(&g_doc);
                (void)ncc_insert_line(&g_doc, 0u, "");
            }
        }
        if (mode == NCC_MODE_RUN && !nc_sender_run_active()) {
            nc_sender_run_set_line(ncc_visual_run_source(), g_doc.cursor);
        }
        if (mode == NCC_MODE_RUN || mode == NCC_MODE_PROGRAM) {
            /* What the glyph rides the cut with (RUN) and what the editor's own
               offset line is drawn at (EDIT): one table, read on the screen that
               shows either. The table lives on TOOLS - walking TOOLS and back
               must bring the edit the operator has just made with it. */
            ncc_visual_load_tool_table();
        }
    }
    ncc_statusf("");
    g_dirty = true;
}

ncc_mode_t ncc_visual_mode(void)
{
    return g_mode;
}

/* The mode key: the screens built so far, in the machine's own order. */
static void ncc_visual_next_mode(void)
{
    switch (g_mode) {
    case NCC_MODE_MANUAL:
        ncc_visual_select_mode(NCC_MODE_PROGRAM);
        break;
    case NCC_MODE_PROGRAM:
        ncc_visual_select_mode(NCC_MODE_TOOLS);
        break;
    case NCC_MODE_TOOLS:
        ncc_visual_select_mode(NCC_MODE_RUN);
        break;
    case NCC_MODE_RUN:
    default:
        ncc_visual_select_mode(NCC_MODE_MANUAL);
        break;
    }
}

bool ncc_visual_open(const char *path)
{
    /* The document is the run's while the pacer is feeding it: opening another
       file would swap the program out from under a machine that is half way
       through it. The card can still be walked - the list reads - but nothing
       is opened onto the run (bench, 2026-09-28: *"its need a safety not to
       edit [a] running file"*). */
    if (nc_sender_run_streaming()) {
        ncc_statusf("Program running - the file is the run's");
        g_dirty = true;
        return false;
    }
    if (!ncc_file_load(&g_doc, path)) {
        ncc_statusf("Cannot open that file");
        g_dirty = true;
        return false;
    }
    ncc_address_reset(g_address);
    ncc_labels_at(g_address);
    ncc_state_remember_path(g_mode, g_doc.path);
    ncc_state_flush();
    g_dirty = true;
    return true;
}

const char *ncc_visual_path(void)
{
    return g_doc.path;
}

/* Into the card, on the file that is open: `0` and `D` on the file-name row are
   the same move - the folder this file lives in, with the file itself selected,
   so the list opens *on* the program being worked on instead of on whatever
   sorts first. `0` used to open the card's *root*, which meant walking down to
   the folder again every time (bench: "it was better approach in nc - same
   folder if 0 pressed not root"). Nothing open - a program that was never saved
   - falls back to the card's root. */
static void ncc_visual_open_current_folder(void)
{
    char dir[NCC_PATH_MAX];
    const char *path = g_doc.path;
    const char *slash = 0;
    size_t i;
    size_t len = strlen(path);

    for (i = 0u; i < len; i++) {
        if (path[i] == '/' || path[i] == '\\') {
            slash = path + i;
        }
    }
    if (!slash || slash == path) {
        snprintf(dir, sizeof(dir), "%s", "/D");
    } else {
        snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
    }
    if (!ncc_file_scan(dir)) {
        ncc_statusf("The card is not answering");
        return;
    }
    (void)ncc_file_select_path(path);
    g_list = true;
    ncc_statusf("");
}

size_t ncc_visual_cursor(void)
{
    return g_doc.cursor;
}

const char *ncc_visual_screen_name(void)
{
    if (g_list) {
        return "FILES";
    }
    if (g_mode == NCC_MODE_RUN) {
        return "RUN";
    }
    if (g_mode == NCC_MODE_MANUAL) {
        return "MANUAL";
    }
    return g_mode == NCC_MODE_TOOLS ? "TOOLS" : "EDIT";
}

bool ncc_visual_save(void)
{
    if (!ncc_file_save(&g_doc)) {
        ncc_statusf("Save failed");
        g_dirty = true;
        return false;
    }
    g_doc.dirty = false;
    ncc_state_remember_path(g_mode == NCC_MODE_TOOLS ? NCC_MODE_TOOLS
                                                      : NCC_MODE_PROGRAM,
                            g_doc.path);
    if (g_mode == NCC_MODE_TOOLS) {
        ncc_visual_load_tool_table();   /* the table the run and the view read */
    }
    ncc_state_remember_cursor(&g_doc);
    ncc_state_flush();
    g_dirty = true;
    return true;
}

const char *ncc_visual_address(void)
{
    return g_address;
}

const char *ncc_visual_slot_label(char key)
{
    const char *label;

    if (key < '1' || key > '9') {
        return 0;
    }
    /* The same word the pad draws: the shell labels its own keypad from the
       screen's answer, never from a second copy of it. */
    label = ncc_pad_label(key);
    return (label && label[0]) ? label : 0;
}

bool ncc_visual_dirty(void)
{
    return g_dirty;
}

void ncc_visual_clear_dirty(void)
{
    g_dirty = false;
}

const char *ncc_visual_status(void)
{
    return g_status;
}

const ncc_document_t *ncc_visual_document(void)
{
    return &g_doc;
}

bool ncc_visual_list(void)
{
    return g_list;
}

bool ncc_visual_name_selected(void)
{
    return g_name_selected;
}

const char *ncc_visual_refused(void)
{
    return g_refused;
}

const char *ncc_visual_label(char key)
{
    if (key < '1' || key > '9') {
        return "";
    }
    return g_labels[key - '1'];
}

/* The name of the pad the operator is standing in: the file at the address, or
   the digits themselves when nobody wrote one. */
void ncc_pad_name(char *out, size_t out_sz)
{
    char full[NCC_ADDR_MAX + 3];
    char name[NCC_PRESET_ROW_MAX];

    snprintf(full, sizeof(full), "%s%s", ncc_pad_root(), g_address);
    if (ncc_preset_read(full, name, sizeof(name), 0, 0u) >= 0 && name[0]) {
        snprintf(out, out_sz, "%s", name);
        return;
    }
    snprintf(out, out_sz, "%s", g_address[0] ? g_address : "MAIN");
}

/* One press of a slot: rows are written where the pad's name stands, children
   are opened, and a slot that is both does both. The pad stays where it is, which
   is what makes a profile one press per point. */
static void ncc_pad_press(char key)
{
    char full[NCC_ADDR_MAX + 3];
    char label[NCC_PRESET_ROW_MAX];
    char rows[NCC_PRESET_ROW_MAX * 4];
    int kind;

    snprintf(full, sizeof(full), "%s%s", ncc_pad_root(), g_address);

    /* The TOOLS screen's keys after `1` are its row editor, not entries: each
       one picks a word of the row under the cursor - the key's number is the
       word's number - and the digits then type that field. `1` itself is the
       table's own entry, a new row; `2` picks its radius. The row has no more
       words, so later keys name nothing. */
    if (g_mode == NCC_MODE_TOOLS && key >= '2' && key <= '9') {
        int index = ncc_pad_word_index(&g_doc, key);

        if (index >= 0 && ncc_pick_field(&g_doc, index)) {
            ncc_statusf(ncc_pad_label(key));
            g_dirty = true;
        }
        return;
    }
    /* The root of the other screens has no tool-table group: it is the TOOLS
       screen's own pad (see `ncc_pad_label()`). */
    if (g_mode != NCC_MODE_TOOLS && g_address[0] == '\0' && key == '7') {
        return;
    }
    /* G7X's `7` is the path builder: a key that *does* something rather than one
       that writes a row of its own, which is why it is not an entry (`nc`'s own
       reason). It opens the nine directions and stays until `5`. */
    if (strcmp(g_address, "4") == 0 && key == '7') {
        /* The pad's own name line goes back first: the builder writes the
           program's rows below the point, and a title line for an entry that is
           not being inserted would be litter in the operator's contour. */
        ncc_pad_close(&g_doc);
        if (ncc_contour_begin(&g_doc)) {
            char text[48];

            snprintf(text, sizeof(text), "Path: 5 ends, # %.1f mm",
                     (double)ncc_contour_step());
            ncc_statusf(text);
            g_dirty = true;
        }
        return;
    }
    kind = ncc_slot(full, key, label, sizeof(label));
    if (kind == NCC_SLOT_EMPTY) {
        return;                     /* nothing there: nothing happens */
    }
    if (kind & NCC_SLOT_ENTRY) {
        char child[NCC_ADDR_MAX + 2];

        snprintf(child, sizeof(child), "%s%c", full, key);
        if (ncc_preset_read(child, 0, 0u, rows, sizeof(rows)) > 0) {
            if (!ncc_pad_active(&g_doc) && !ncc_pad_open(&g_doc, label)) {
                ncc_statusf("No room in the program");
                g_dirty = true;
                return;
            }
            if (ncc_pad_write(&g_doc, rows)) {
                /* The entry landed, so the press is over: the keys go back to
                   the root and the next digit opens a group rather than the
                   next slot of the one that just wrote. The pad used to stay
                   where it was, and `A` was the only way out (bench,
                   2026-09-28: *"g4x does not exit after insert normally only
                   via A"*); nc's own menu closed on every entry
                   (`nc_editor_modal_cancel()`), and only the path builder's
                   walk stays up for the next point. What the entry left picked
                   stays picked - the digits are its value's from here. */
                ncc_address_reset(g_address);
                ncc_labels_at(g_address);
            }
            g_doc.dirty = true;
            ncc_statusf(label);
        }
    }
    if (kind & NCC_SLOT_PAD) {
        if (!ncc_address_push(g_address, key)) {
            return;
        }
        ncc_labels_at(g_address);
        (void)ncc_pad_open(&g_doc, label);
        ncc_statusf("");
        g_dirty = true;
    }
}

/* The line the RUN screen is on: what the pane marks and what `1 SINGLE` acts
   on. It is the line in play, not the sender's position, and it is clamped into
   the program because a finished run stands just past its last line. */
size_t ncc_visual_run_line(void)
{
    size_t line = nc_sender_run_display_line();

    if (g_doc.line_count && line >= g_doc.line_count) {
        line = g_doc.line_count - 1u;
    }
    return line;
}

/* The run's own keys: `1 SINGLE 2 FROM 3 FULL 4 HOLD 5 STOP`, `#` reloads, and
   `B`/`C` walk the mark while nothing is running. False when the key is not the
   run's, so the screen can offer it to the modes. */
static bool ncc_visual_run_key(char key)
{
    size_t line = ncc_visual_run_line();
    char text[48];

    switch (key) {
    case '1':
        ncc_statusf(nc_sender_run_send_unit(ncc_visual_run_source(), line)
                        ? "SINGLE sent" : "Nothing to send");
        break;
    case '2':
        if (nc_sender_run_start(ncc_visual_run_source(), line)) {
            snprintf(text, sizeof(text), "Run from line %lu", (unsigned long)(line + 1u));
            ncc_statusf(text);
        } else {
            ncc_statusf("Nothing to run");
        }
        break;
    case '3':
        ncc_statusf(nc_sender_run_start(ncc_visual_run_source(), 0u)
                        ? "Full run" : "Nothing to run");
        break;
    case '4':
        if (nc_sender_run_toggle_hold()) {
            ncc_statusf(nc_sender_run_hold() ? "Hold" : "Resume");
        } else {
            ncc_statusf("No active run");
        }
        break;
    case '5':
        nc_sender_run_stop();
        ncc_statusf("Stopped");
        break;
    case '#':
    {
        /* RELOAD: the fault and the run go, and the file is read back off the
           card, so an edit made since it was opened is what runs next. The mark
           stays where the fault stopped it - the operator is standing there to
           fix that line (bench: "on error - it still jumps to first line. no i
           should stay here"). */
        size_t keep = ncc_visual_run_line();

        nc_sender_run_reset();
        (void)ncc_state_load_document(NCC_MODE_RUN, &g_doc);
        nc_sender_run_set_line(ncc_visual_run_source(), keep);
        ncc_statusf("Reloaded");
        break;
    }
    case 'B':
    case 'C':
        if (nc_sender_run_active()) {
            return true;            /* RUN owns the cursor while it runs */
        }
        if (key == 'B') {
            line = line > 0u ? line - 1u : 0u;
        } else if (line + 1u < g_doc.line_count) {
            line++;
        }
        nc_sender_run_set_line(ncc_visual_run_source(), line);
        g_doc.cursor = line;
        break;
    default:
        return false;
    }
    g_dirty = true;
    return true;
}

/* The G code a line was just finished as, if any: a lone `G` word the
   vocabulary knows becomes its own line with its first value picked
   (`ncc_gcode_resolve()`), and the operator is told what the code is and which
   words it takes - the sentence nc's G field put in its status line. */
static void ncc_visual_gcode_note(void)
{
    int code = ncc_gcode_resolve(&g_doc);

    if (code > 0) {
        const char *name = ncc_vocab_gcode_name(code);
        const char *params = ncc_vocab_gcode_parameters(code);
        char text[64];

        snprintf(text, sizeof(text), "G%d %s: %s", code, name ? name : "",
                 params ? params : "");
        ncc_statusf(text);
    }
}

/* While a run is in flight the program belongs to the run: the pacer reads the
   lines as it goes, so a row written under it is not the program the operator
   started - the machine would cut something they never wrote, and the file the
   run came from would be written back mid-cut. The keys that only *look* still
   work - `B`/`C` walk the cursor, `#` switches the drawing (the preview's own
   state, not the file) and `A`/`0` leave - and every key that would write the
   text answers with the reason (bench, 2026-09-28: *"its need a safety not to
   edit [a] running file"*). A picked value or the path builder makes `#` and
   `0` writers too, so those two are the look-keys only when neither is up. */
static bool ncc_visual_locked_key_ok(char key, bool name_row)
{
    if (key == 'B' || key == 'C' || key == 'A') {
        return true;
    }
    if (name_row) {
        return key == 'D';          /* the folder the file lives in: a read */
    }
    if (g_doc.field >= 0 || ncc_contour_active()) {
        return false;
    }
    return key == '0' || key == '#';
}

void ncc_visual_key(char key)
{
    ncc_key_t editor_key = NCC_KEY_NONE;

    g_last_key_ms = mcu_millis();
    /* The operator has seen the picture: the first key ends it, and the work
       screen is drawn in its place (the next pump sees the screen is dirty). */
    if (ncc_boot_active()) {
        ncc_boot_release();
        ncc_visual_mark_dirty();
        return;
    }
    /* The file list is its own screen: a picker, with the keys a picker needs and
       no pad (the pad's digits name a new file while one is being made). */
    if (g_list) {
        char path[NCC_PATH_MAX];
        const ncc_file_entry_t *entry;

        /* The card is read-only while the run is in flight: `5` and `6` write
           (a new file, a delete) and the run's own file is one of the rows an
           operator could pick. Opening is refused by `ncc_visual_open()`
           whatever the way in, so only the two that touch the card are
           answered here. */
        if (nc_sender_run_streaming() && (key == '5' || key == '6')) {
            ncc_statusf("Program running - the file is the run's");
            g_dirty = true;
            return;
        }

        if (ncc_file_new_active()) {
            if (key >= '0' && key <= '9') {
                ncc_file_new_digit(key);
            } else if (key == '*') {
                ncc_file_new_end();
                ncc_statusf("New file cancelled");
            } else if (key == '#' || key == 'D') {
                if (ncc_file_new_name()[0] &&
                    ncc_file_create(ncc_file_new_name(), ".nc", path,
                                    sizeof(path))) {
                    ncc_file_new_end();
                    if (ncc_visual_open(path)) {
                        g_list = false;
                        ncc_statusf("Created and opened");
                    }
                } else {
                    ncc_statusf("Give the new file a number");
                }
            }
            g_dirty = true;
            return;
        }
        switch (key) {
        case 'B':
            ncc_file_step(-1);
            break;
        case 'C':
            ncc_file_step(1);
            break;
        case 'D':
        case '#':
            if (!ncc_file_selected_path(path, sizeof(path))) {
                ncc_statusf("Nothing to open");
                break;
            }
            entry = ncc_file_entry(ncc_file_selected());
            if (entry && entry->is_dir) {
                if (!ncc_file_scan(path)) {
                    ncc_statusf("Cannot read that folder");
                }
                break;
            }
            if (ncc_visual_open(path)) {
                g_list = false;
            }
            break;
        case '5':
            ncc_file_new_begin();
            ncc_statusf("New file: a number, # for OK");
            break;
        case '6':
            if (ncc_file_delete_selected()) {
                ncc_statusf("Deleted");
            } else {
                ncc_statusf("Cannot delete that");
            }
            break;
        case '8':
            if (!ncc_file_scan(ncc_file_dir())) {
                ncc_statusf("Refresh failed");
            }
            break;
        case '*':
        case '0':
            /* `0` is the way out of whatever is up, and the list is one of those
               things: out of the list is the program. */
            g_list = false;
            ncc_statusf("");
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
    if (g_mode == NCC_MODE_MANUAL) {
        if (key == 'A' && !ncc_manual_field_active()) {
            ncc_manual_feed_cancel();
            ncc_visual_next_mode();
            return;
        }
        (void)ncc_manual_key(key);
        g_dirty = true;
        return;
    }
    if (g_doc.line_count == 0u) {
        return;                     /* no program: the keys have nothing to act on */
    }
    /* The run screen: `0` still opens the card and `A` still leaves, and the
       rest of the pad is the run's. */
    if (g_mode == NCC_MODE_RUN) {
        if (key == '0') {
        ncc_visual_open_current_folder();
            g_dirty = true;
            return;
        }
        if (key == 'A') {
            ncc_visual_next_mode();
            return;
        }
        (void)ncc_visual_run_key(key);
        return;
    }
    /* EDIT, with the run in flight: the keys that look still work, the keys
       that would write answer with the reason (see
       `ncc_visual_locked_key_ok()`). */
    if (nc_sender_run_streaming() && !ncc_visual_locked_key_ok(key, g_name_selected)) {
        ncc_statusf("Program running - the file is the run's");
        g_dirty = true;
        return;
    }
    /* The file-name row above line 1 holds the cursor when `B` steps up onto
       it: `D` opens the folder the file lives in with the file itself selected,
       `C` steps back to the program, and any other key hands the cursor back to
       the code and then does its own job (nc's own row and its own keys). */
    if (g_name_selected) {
        if (key == 'B') {
            g_dirty = true;             /* the row's own key: it stays */
            return;
        }
        if (key == 'C') {
            g_name_selected = false;
            g_doc.cursor = 0u;
            g_dirty = true;
            return;
        }
        if (key == 'D') {
            ncc_visual_open_current_folder();
            g_dirty = true;
            return;
        }
        g_name_selected = false;
    } else if (key == 'B' && g_doc.field < 0 && g_doc.cursor == 0u) {
        /* Up from line 1 is the file's row, not the line above it (there is
           none) and not the mode. */
        g_name_selected = true;
        g_dirty = true;
        return;
    }
    /* `#` is the preview's key, and it is the editor's too - which of the two
       it is is the state the operator is in, and the order is fixed:

         1. the path builder is modal and `#` is its step key, so it goes first;
         2. a value that is picked takes `#` as its accept, exactly as nc2 had
            it (typing a number is the one time the key is not the drawing's);
         3. everywhere else `#` switches the preview between nc's full picture -
            the stock, the path and the DIN layer - and the stock and the path
            alone. */
    if (key == '#') {
        if (ncc_contour_active() && ncc_contour_key(&g_doc, key)) {
            /* The pad's step key took it. */
        } else if (g_doc.field >= 0) {
            (void)ncc_key(&g_doc, NCC_KEY_ACCEPT, key);
            ncc_visual_gcode_note();
        } else {
            ncc_preview_toggle_din();
            ncc_statusf(ncc_preview_din() ? "Preview: DIN"
                                          : "Preview: stock + path");
        }
        g_dirty = true;
        return;
    }
    switch (key) {
    case 'B': editor_key = NCC_KEY_UP; break;
    case 'C': editor_key = NCC_KEY_DOWN; break;
    case 'D': editor_key = NCC_KEY_NEXT; break;
    case '*': editor_key = NCC_KEY_DELETE; break;
    case '0':
        /* A value that is picked takes the key: `0` is a digit there, and the
           field flow is one way to type a number, not two. Everywhere else `0`
           is the exit - with a pad up it leaves the pad in one press, which `A`
           cannot do (that is one level), and with nothing to leave it opens the
           card. */
        if (g_doc.field >= 0) {
            editor_key = NCC_KEY_DIGIT;
            break;
        }
        if (g_address[0]) {
            ncc_contour_leave();
            ncc_address_reset(g_address);
            ncc_pad_close(&g_doc);
            ncc_labels_at(g_address);
            ncc_statusf("");
            g_dirty = true;
            return;
        }
        ncc_visual_open_current_folder();
        g_dirty = true;
        return;
    case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        editor_key = NCC_KEY_DIGIT;
        break;
    case 'A':
        /* Up a level, and at the root the mode key: the program and the run
           are the two screens built so far. */
        ncc_contour_leave();
        if (g_address[0]) {
            ncc_address_pop(g_address);
            ncc_pad_close(&g_doc);
            ncc_labels_at(g_address);
            ncc_statusf("");
            g_dirty = true;
        } else {
            ncc_visual_next_mode();
        }
        return;
    default:
        return;
    }
    g_dirty = true;
    /* The path builder's keys come first: while it is up, the pad's digits are
       the directions and the just-written row keeps its value picked, so the
       digits that are *not* the builder's reach the editor's field flow. */
    if (ncc_contour_active() && ncc_contour_key(&g_doc, key)) {
        return;
    }
    /* The editor first: with a value picked the digits are the value's, and with
       nothing picked they are the pad's. The G-code sentence and the resolve
       belong to a code the operator has *just finished typing* - `D` steps off
       the word that was typed, `#` accepts it (that path is above) - and to
       nothing else: with the sentence on every key the editor took, the cursor
       *travelling* over a line that happened to be a lone `G` word rewrote and
       picked it, while a lone `M` word (which the vocabulary does not hold) was
       left alone - which is exactly the difference the bench saw (2026-09-28:
       *"it still wants to select this g41 or g42 while i travel along. and not m
       commands"*). */
    {
        bool typed = g_doc.field >= 0 && g_doc.drafting;

        if (ncc_key(&g_doc, editor_key, key)) {
            /* `D` steps off a word the operator typed: that is the code they
               have just finished, so it resolves here. `#` is above. Nothing
               else - travelling over a line must not touch it. */
            if (editor_key == NCC_KEY_NEXT && typed) {
                ncc_visual_gcode_note();
            }
            return;
        }
    }
    if (editor_key == NCC_KEY_DIGIT) {
        ncc_pad_press(key);
    }
}

void ncc_visual_hold_key(char key)
{
    if (g_mode == NCC_MODE_MANUAL) {
        ncc_manual_hold(key);
    }
}

void ncc_visual_tick(unsigned ms)
{
    if (ncc_boot_active()) {
        ncc_boot_tick(ms);
        if (!ncc_boot_active()) {
            /* It has just let go, and the work screen has to take its place -
               or the panel keeps the splash on the glass while the screen
               believes it is gone. */
            ncc_visual_mark_dirty();
        }
        return;
    }
    /* The card is mounted long after module init, so the one preset file is
       read here, on the first loop the drive answers on. Until then the shipped
       table is what the pad reads; after, the file has replaced what it names.
       The sync is the rest of the tick. */
    (void)ncc_presets_sync();
}

/* How often a screen that has something of its own to show redraws: the frame
   the DRO's figures and a held feed need, and no more. */
#define NCC_PUMP_PERIOD_MS 20u

bool ncc_visual_pump(uint32_t now_ms)
{
    static uint32_t last_pump_ms;
    static uint32_t last_draw_ms;
    /* The first pass only starts the clock: `now_ms` is the machine's own
       milliseconds since boot, which is a whole start-up's worth by the time the
       panel first looks, and counting that as elapsed would end the splash
       before it was ever drawn. */
    unsigned elapsed = last_pump_ms ? (unsigned)(now_ms - last_pump_ms) : 0u;

    last_pump_ms = now_ms;
    /* The screen's own clock first: it is where the card's one preset file is
       picked up, and the module that draws has no clock of its own. */
    ncc_visual_tick(elapsed);
    ncc_visual_idle_tasks();
    if (ncc_visual_dirty()) {
        last_draw_ms = now_ms;
        return true;
    }
    if (ncc_visual_periodic_needed() &&
        (uint32_t)(now_ms - last_draw_ms) >= NCC_PUMP_PERIOD_MS) {
        last_draw_ms = now_ms;
        return true;
    }
    return false;
}

void ncc_visual_idle_tasks(void)
{
    if (g_list || g_mode == NCC_MODE_MANUAL || !g_doc.dirty || !g_doc.path[0]) {
        return;
    }
    if ((uint32_t)(mcu_millis() - g_last_key_ms) >= NCC_IDLE_SAVE_MS) {
        (void)ncc_visual_save();
    }
}

