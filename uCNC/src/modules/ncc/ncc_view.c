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

/* The view reads the controller through `ncc_visual_internal.h`; this local
   alias keeps the drawing code expressed in terms of the document it paints. */
#define ncc_view_doc (*ncc_visual_document())

/* The code pane's first visible line. */
static size_t g_pane_first;

/* How many rows of the file stay in view below the cursor's. In G-code the next
   block is what the operator is reading for, so the pane never lets the cursor
   reach its last row: the *text* moves instead (bench: "cursor should never
   reach last line. in g code it is always necessary to see next line. so as
   before leave - 6 lines and move text, but not the cursor to the end."). */
#define NCC_LOOKAHEAD_ROWS 6

/* The frame meter - back by the bench's own request ("give me back fps meter it
   need to be tested"). One frame is one `ncc_visual_draw()`, and the reading is
   the last whole second's frames; it is what says whether a change cost the
   panel its frame rate, and the live stock's mask redraw is the one to watch
   (`ncc/TESTING.md`). It is a debug reading: dim, small, and in the header's
   far corner, out of everything the operator reads. */
#define NCC_FPS_COLS 8
static uint16_t g_ncc_fps;
static uint16_t g_ncc_fps_frames;
static uint32_t g_ncc_fps_start_ms;
static uint16_t g_ncc_analog_mv[4];

/* Batch 1 bench scale: 361 counts at 5.0 V and 250 at 3.3 V. */
#define NCC_ADC_MV_PER_COUNT 13.6f

static void ncc_fps_tick(void)
{
    uint32_t now = mcu_millis();
    uint32_t elapsed;

    if (!g_ncc_fps_start_ms) {
        g_ncc_fps_start_ms = now;
    }
    g_ncc_fps_frames++;
    elapsed = now - g_ncc_fps_start_ms;
    if (elapsed >= 1000u) {
        g_ncc_fps = (uint16_t)(((uint32_t)g_ncc_fps_frames * 1000u) / elapsed);
        g_ncc_fps_frames = 0u;
        g_ncc_fps_start_ms = now;
    }
}

static void ncc_draw_diagnostics(void)
{
    char buf[96];

    /* The bottom, in the notes' own left corner: the header's right corner is
       where `uCNC` and the machine's message live, and the meter is a
       diagnostic, not something read at a glance (bench, 2026-09-28: "move fps
       to bottom"). */
    snprintf(buf, sizeof(buf), "%u FPS", (unsigned)g_ncc_fps);
    ncc_text_clip(NCC_TEXT_X, NCC_BODY_BOTTOM - 11, buf, NCC_FPS_COLS,
                  ncc_col_dim(), ncc_col_bg(), LVDS_FONT_SMALL);
    snprintf(buf, sizeof(buf),
             "E0:%ld E1:%ld E2:%ld "
             "AI0:%u.%02uV AI1:%u.%02uV AI2:%u.%02uV AI3:%u.%02uV",
             (long)ncc_visual_encoder_position(0),
             (long)ncc_visual_encoder_position(1),
             (long)ncc_visual_encoder_position(2),
             (unsigned)(g_ncc_analog_mv[0] / 1000u),
             (unsigned)((g_ncc_analog_mv[0] % 1000u) / 10u),
             (unsigned)(g_ncc_analog_mv[1] / 1000u),
             (unsigned)((g_ncc_analog_mv[1] % 1000u) / 10u),
             (unsigned)(g_ncc_analog_mv[2] / 1000u),
             (unsigned)((g_ncc_analog_mv[2] % 1000u) / 10u),
             (unsigned)(g_ncc_analog_mv[3] / 1000u),
             (unsigned)((g_ncc_analog_mv[3] % 1000u) / 10u));
    ncc_text_clip(NCC_TEXT_X + 68, NCC_BODY_BOTTOM - 11, buf, 76,
                  ncc_col_text(), ncc_col_bg(), LVDS_FONT_SMALL);
}

uint16_t ncc_visual_fps(void)
{
    return g_ncc_fps;
}

/* --- drawing -------------------------------------------------------------- */

/* The header: the four screens with the one in play marked, then the file, and
   the body's messages at the right end.

   This is the panel's only band. There is no footer: what a footer says - which
   screen you are on and what just happened - is one line of text, and one line
   belongs where the operator is already looking. The machine's keys are the pad
   in the drawing's corner, so nothing is left along the bottom. */
static void ncc_draw_header(void)
{
    static const ncc_mode_t order[4] = {
        NCC_MODE_MANUAL, NCC_MODE_PROGRAM, NCC_MODE_TOOLS, NCC_MODE_RUN
    };
    static const char *const names[4] = { "MANUAL", "EDIT", "TOOLS", "RUN" };
    int y = 6;
    int x = 8;
    int i;

    ncc_fill(0, 0, LVDS_VIEW_WIDTH, NCC_HEADER_H, ncc_col_header());
    for (i = 0; i < 4; i++) {
        bool active = order[i] == ncc_visual_mode();
        int w;

        if (i) {
            x += 10;
        }
        w = ncc_text_width(names[i], LVDS_FONT_NORMAL);
        /* The screen in play wears the block, the way nc's tabs did: its name on
           the panel's own selection colour, so "which screen am I on" is a shape
           and not a word to read (bench: "marking manual tools run with line but
           not with full yellow background as it was before"). */
        if (active) {
            ncc_fill(x - 6, 1, w + 12, NCC_HEADER_H - 2, ncc_col_select());
        }
        ncc_text(x, y, names[i],
                 active ? ncc_col_pad_text() : ncc_col_dim(),
                 active ? ncc_col_select() : ncc_col_header(),
                 LVDS_FONT_NORMAL);
        x += w;
    }

    /* The message, between the screens and the frame meter, and only the room
       that is left, so a long one can never push the meter off the glass.

       The file the operator is editing is *not* here: it is the pane's own top
       row (`ncc_draw_file_row()`), where the cursor can reach it with `B` and
       open its folder with `D`. nc2 put the path in the header because it had
       taken that row out of the pane; with the row back, the header naming it
       again is the same path twice, one above the other (bench: "file name is
       still header next to run"). */
    {
        int small_w = ncc_col_width(LVDS_FONT_SMALL);
        int msg_cols = ncc_visual_status()[0]
                           ? (ncc_text_width(ncc_visual_status(), LVDS_FONT_SMALL) +
                              small_w - 1) / small_w
                           : 0;
        int msg_x;

        /* `uCNC` has the far corner now that the frame meter is at the bottom;
           the message stops short of it. */
        int fps_w = (int)strlen("uCNC") * small_w + small_w * 2;

        /* The file the operator is editing is in the pane below, so the header's
           message gets everything between the screens and the frame meter - a
           sentence is said once, in full, where the bench asked for it. */
        {
            int room = (LVDS_VIEW_WIDTH - 8 - fps_w - (x + 12)) / small_w;

            if (msg_cols > room) {
                msg_cols = room;
            }
        }
        msg_x = LVDS_VIEW_WIDTH - 8 - fps_w - msg_cols * small_w;
        if (msg_cols > 0) {
            ncc_text_clip(msg_x, y + 1, ncc_visual_status(), msg_cols, ncc_col_text(),
                          ncc_col_header(), LVDS_FONT_SMALL);
        }
    }
    /* `uCNC` itself, in the header's own right corner: the strip gave the word
       up when the state moved to its left end, and the frame meter left this
       corner for the bottom, so the machine's name has it (bench, 2026-09-28). */
    ncc_text(LVDS_VIEW_WIDTH - 8 - ncc_text_width("uCNC", LVDS_FONT_SMALL),
             y + 1, "uCNC", ncc_col_dim(), ncc_col_header(), LVDS_FONT_SMALL);
    ncc_draw_diagnostics();
}

/* One row of the program: its number, then the text. The row the cursor (or the
   run) is on wears the selection colour, the rows of the block it heads wear
   the pale one, and the picked field is drawn as the box being typed into -
   three pieces, so everything the operator did not touch keeps its colour. */
static void ncc_draw_row(size_t index, int x, int y, int w, bool selected,
                         bool path)
{
    const char *text = ncc_view_doc.lines[index];
    bool cursor = selected;
    lvds_color_t bg = cursor ? ncc_col_select()
                             : (path ? ncc_col_block() : ncc_col_bg());
    int col_w = ncc_col_width(LVDS_FONT_NORMAL);
    char number[8];
    int text_x = x + NCC_LINE_NO_PAD;
    int cols;
    ncc_field_t fields[NCC_MAX_FIELDS];
    int count;
    int picked = -1;

    /* The row's mark is the *band the text sits on*, not the whole row: nc drew
       it with each text run's own background, which leaves the picked word's
       box standing proud of it above and below (bench: "on nc we have its black
       bigger in height compared to yellow strip. do same here"). */
    snprintf(number, sizeof(number), "%3u", (unsigned)(index + 1u));
    ncc_text(text_x, y, number, ncc_col_dim(), bg, LVDS_FONT_NORMAL);
    text_x += 4 * col_w;
    cols = (w - (text_x - x) - 4) / col_w;
    if (cols <= 0) {
        return;
    }
    count = ncc_fields(text, fields, NCC_MAX_FIELDS);
    /* The box belongs to the **document**, not to one screen: TOOLS edits the
       same rows through the preview pane, and the picked field is the one piece
       of state the operator cannot see there (bench, 2026-09-28: *"tool edit is
       broken it does not mark current editable item value"*, and now again
       2026-10-17: "select item is not shown as selected, on legend is shown" -
       the pad's label said which word was picked while the row showed nothing). */
    if (cursor && (ncc_visual_mode() == NCC_MODE_PROGRAM || ncc_visual_mode() == NCC_MODE_TOOLS) &&
        ncc_view_doc.field >= 0 && ncc_view_doc.field < count) {
        picked = ncc_view_doc.field;
    }
    if (picked < 0) {
        ncc_text_clip(text_x, y, text, cols, ncc_col_text(), bg,
                      LVDS_FONT_NORMAL);
        return;
    }
    {
        const ncc_field_t *f = &fields[picked];
        int head = (int)f->start;
        int field = (int)(f->end - f->start);
        char piece[NCC_MAX_LINE_LEN];

        if (head > cols) {
            head = cols;
        }
        if (field > cols - head) {
            field = cols - head;
        }
        /* The box the value goes into: nc's own, a row taller than the band the
           line is drawn on (`nc_text_draw_line_with_word()`), drawn before the
           text so the value sits inside it. */
        if (field > 0) {
            ncc_fill(text_x + head * col_w, y - 2, field * col_w,
                     NCC_ROW_H - 4, ncc_col_field_bg());
        }
        /* Each piece is copied out of the line and handed over as a string of
           exactly its own length. The renderer's clip helper ends a string
           *longer* than its column count with "...", and a pointer into the
           line is longer than the word it names - which is how the word being
           typed came out as "G..." instead of "G970" (bench: "g970 one edit
           shows ... still if i select 3 digit number"). nc copied the piece
           before drawing it for the same reason. */
        memcpy(piece, text, (size_t)head);
        piece[head] = '\0';
        ncc_text_clip(text_x, y, piece, head, ncc_col_text(), bg,
                      LVDS_FONT_NORMAL);
        memcpy(piece, text + f->start, (size_t)field);
        piece[field] = '\0';
        ncc_text_clip(text_x + head * col_w, y, piece, field,
                      ncc_col_field_fg(), ncc_col_field_bg(), LVDS_FONT_NORMAL);
        ncc_text_clip(text_x + (head + field) * col_w, y, text + f->end,
                      cols - head - field, ncc_col_text(), bg, LVDS_FONT_NORMAL);
    }
}

/* The file the program in the pane belongs to, on the pane's own top row: the
   path and the dirty mark, or "(no file)" on a program that has never been
   saved. It is nc's row, back, and it does three jobs: it names the file, it is
   the row the legend uses when the cursor is on line 1 (so no program line is
   ever covered), and the cursor can sit on it - `B` from line 1 - where it
   reads as a field and `D` opens the folder the file lives in with the file
   itself selected. */
static void ncc_draw_file_row(int x, int y, int w)
{
    char line[NCC_PATH_MAX + 4];
    lvds_color_t fg = ncc_visual_name_selected() ? ncc_col_field_fg() : ncc_col_dim();
    lvds_color_t text_bg = ncc_visual_name_selected() ? ncc_col_field_bg() : ncc_col_bg();
    int cols = (w - NCC_LINE_TEXT_PAD - 4) / ncc_col_width(LVDS_FONT_NORMAL);

    snprintf(line, sizeof(line), "%s%s",
             ncc_view_doc.path[0] ? ncc_view_doc.path : "(no file)",
             ncc_view_doc.dirty ? " *" : "");
    if (ncc_visual_name_selected()) {
        ncc_fill(x, y - 2, w - 2, NCC_ROW_H - 2, ncc_col_select());
    }
    ncc_text_clip(x + NCC_LINE_TEXT_PAD, y, line, cols, fg, text_bg,
                  LVDS_FONT_NORMAL);
}

/* The rows of the file the screen has open, in the rectangle it is handed: the
   file's own row, then the line numbers, the text, the mark on the line in play
   (or the cursor's) and the pale block around it - and the word under the cursor
   named on the row above. The window moves only when the cursor would leave it:
   six rows of the file after the cursor's stay in view, because in G-code the
   next block is what the operator is reading for. */
static void ncc_draw_rows(int x, int y, int w, int h)
{
    int rows = h / NCC_ROW_H;
    size_t mark = (ncc_visual_mode() == NCC_MODE_RUN) ? ncc_visual_run_line()
                                           : ncc_view_doc.cursor;
    size_t first;
    size_t path_first = 0u;
    size_t path_last = 0u;
    bool have_path = false;
    bool legend;
    int top;
    int shown;
    int i;

    /* The pane's first row is the file the program belongs to, so the program
       itself gets one row fewer than the band has. */
    shown = rows - 1;
    if (shown < 1) {
        return;
    }
    /* The line in play heads a path: the cycle's rows are the pale mark beside
       it, on the run screen and in the editor alike - the editor is where the
       program is read, so the block the cursor sits in is the first thing its
       own screen should say. */
    {
        g7x_doc_t view = ncc_document_g7x(&ncc_view_doc);

        have_path = g7x_doc_line_path(&view, mark, &path_first, &path_last);
    }
    /* The window: full at the end of the file, and never letting the cursor
       come closer than the look-ahead to the pane's last row. */
    if (ncc_view_doc.line_count > (size_t)shown &&
        g_pane_first + (size_t)shown > ncc_view_doc.line_count) {
        g_pane_first = ncc_view_doc.line_count - (size_t)shown;
    }
    if (mark < g_pane_first) {
        g_pane_first = mark;
    }
    {
        int look = shown - 1 - NCC_LOOKAHEAD_ROWS;

        if (look < 1) {
            look = 1;               /* a pane too short to keep them all */
        }
        if (mark > g_pane_first + (size_t)look) {
            g_pane_first = mark - (size_t)look;
        }
    }
    first = g_pane_first;
    if (first > mark) {
        first = mark;
    }
    /* The file the program belongs to, on the pane's own top row - nc's row,
       back: it is where the operator reads which file they are editing, it is
       the row the legend uses when the cursor is on line 1 (so no program line
       is ever covered), and `D` on it opens the folder the file lives in with
       the file itself selected. */
    ncc_draw_file_row(x, y + 4, w);
    top = y + 4 + NCC_ROW_H;
    legend = ncc_visual_mode() != NCC_MODE_RUN && ncc_view_doc.field >= 0;

    for (i = 0; i < shown && first + (size_t)i < ncc_view_doc.line_count; i++) {
        size_t index = first + (size_t)i;
        bool selected = index == mark;
        bool path = have_path && index >= path_first && index <= path_last &&
                    !selected;

        ncc_draw_row(index, x, top + i * NCC_ROW_H, w, selected, path);
    }
    /* The legend of the word being read, on the row above the cursor's - the
       row the operator's eye is already on, so the meaning of the word they are
       typing is where they are looking (bench: "on edit - no legend in top
       row"). On line 1 that row is the file's, which is why the file row is
       there - the marked line keeps its own row. A run draws none:
       nothing is being typed there. */
    if (legend) {
        ncc_field_t fields[NCC_MAX_FIELDS];
        int count = ncc_document_fields(&ncc_view_doc, fields, NCC_MAX_FIELDS);

        if (ncc_view_doc.field < count) {
            int at = (int)(mark - first);
            int hint_y = top + (at - 1) * NCC_ROW_H;
            char text[48];
            int cols = (w - NCC_LINE_TEXT_PAD - 4) /
                       ncc_col_width(LVDS_FONT_NORMAL);

            snprintf(text, sizeof(text), ">  %s",
                     ncc_vocab_label(ncc_view_doc.lines[ncc_view_doc.cursor],
                                     &fields[ncc_view_doc.field]));
            /* The legend is the word's own band: black on the pale yellow the
               panel marks a cycle's rows with, so the meaning of the word being
               typed wears the same mark wherever the row lands (bench: "should
               be same black with yellow background as before. better - pale
               yellow as in g7x group mark"). */
            ncc_fill(x, hint_y - 2, w - 2, NCC_ROW_H - 2, ncc_col_block());
            ncc_text_clip(x + NCC_LINE_TEXT_PAD, hint_y, text, cols,
                          ncc_col_pad_text(), ncc_col_block(), LVDS_FONT_NORMAL);
        }
    }
}

/* The card: what is in the folder the operator is looking at, the one picked lit,
   and the name being typed for a new file. It is the whole bottom band while it
   is up - a picker, with no pad: the digits are the new file's name there. */
static void ncc_draw_list(void)
{
    const int list_x = NCC_TEXT_X;
    const int list_y = NCC_TEXT_Y;
    const int list_w = NCC_PAD_X + NCC_PAD_W - NCC_TEXT_X;
    const int list_h = NCC_TEXT_H;
    int rows = (list_h - 40) / NCC_ROW_H;
    int i;

    ncc_text_clip(list_x + 4, list_y + 6, ncc_file_dir(),
                  (list_w - 12) / ncc_col_width(LVDS_FONT_NORMAL),
                  ncc_col_text(), ncc_col_bg(), LVDS_FONT_NORMAL);
    for (i = 0; i < ncc_file_count() && i < rows; i++) {
        const ncc_file_entry_t *entry = ncc_file_entry(i);
        int y = list_y + 30 + i * NCC_ROW_H;
        char line[NCC_NAME_MAX + 4];

        if (i == ncc_file_selected()) {
            ncc_fill(list_x + 2, y - 2, list_w - 6, NCC_ROW_H - 2,
                     ncc_col_select());
        }
        snprintf(line, sizeof(line), "%s%s", entry->name,
                 entry->is_dir ? "/" : "");
        ncc_text_clip(list_x + 8, y, line, (list_w - 16) / 8, ncc_col_text(),
                      i == ncc_file_selected() ? ncc_col_select() : ncc_col_bg(),
                      LVDS_FONT_NORMAL);
    }
    if (ncc_file_new_active()) {
        char line[NCC_NAME_MAX + 8];

        snprintf(line, sizeof(line), "NEW  %s.nc",
                 ncc_file_new_name()[0] ? ncc_file_new_name() : "_");
        ncc_text_clip(list_x + 8, list_y + list_h - 24, line, 40,
                      ncc_col_field_fg(), ncc_col_field_bg(), LVDS_FONT_NORMAL);
    }
}

/* The pad's nine labels on the run screen: the run's own keys. The rest of the
   pad is empty there - the card's entries are the editor's. */
static const char *const g_nc_sender_run_labels[9] = {
    "SINGLE", "FROM", "FULL", "HOLD", "STOP", "", "", "", ""
};

/* The label of one pad key on the active screen, or "" when it has none. */
const char *ncc_pad_label(char key)
{
    if (key < '1' || key > '9') {
        return "";
    }
    /* The builder's own pad: nine directions, in key order. */
    if (ncc_contour_active()) {
        return ncc_contour_label(key);
    }
    if (ncc_visual_mode() == NCC_MODE_RUN) {
        return g_nc_sender_run_labels[key - '1'];
    }
    if (ncc_visual_mode() == NCC_MODE_MANUAL) {
        return ncc_manual_pad_label(key);
    }
    if (ncc_visual_mode() == NCC_MODE_TOOLS && key >= '2' && key <= '9') {
        /* The row editor's keys are the row's own words: the label is the
           letter of the word the key stands for, so the pad reads the same
           whether a row carries `R O F Q D E S X Z` or its own order. */
        static char letter[2];
        int index = ncc_pad_word_index(&ncc_view_doc, key);

        if (index < 0) {
            return "";
        }
        {
            ncc_field_t fields[NCC_MAX_FIELDS];

            if (ncc_document_fields(&ncc_view_doc, fields, NCC_MAX_FIELDS) <= index) {
                return "";
            }
            letter[0] = fields[index].letter;
            letter[1] = '\0';
        }
        return letter;
    }
    /* Under G7X, `7` is the path builder - the one cell the card cannot fill. */
    if (ncc_visual_address()[0] == '4' && ncc_visual_address()[1] == '\0' && key == '7') {
        return "PATH";
    }
    /* The tool table's group is the TOOLS screen's own pad root, so the root of
       the other screens does not offer the same thing a second time (bench:
       "push 7 table to tools? because it seems to be the same"). */
    if (ncc_visual_mode() != NCC_MODE_TOOLS && ncc_visual_address()[0] == '\0' && key == '7') {
        return "";
    }
    return ncc_visual_label(key);
}

/* The key the pad draws as the one in play: the run holds `4`, and MANUAL
   flashes the key the machine just acted on. */
static char ncc_pad_hot(void)
{
    char key;

    if (ncc_visual_mode() == NCC_MODE_RUN) {
        return nc_sender_run_hold() ? '4' : 0;
    }
    if (ncc_visual_mode() != NCC_MODE_MANUAL) {
        return 0;
    }
    for (key = '1'; key <= '9'; key++) {
        if (ncc_manual_flash(key)) {
            return key;
        }
    }
    return 0;
}

/* The pad's own band: where the operator is, then the nine slots. */
static void ncc_draw_pad_band(void)
{
    const char *labels[9];
    char name[NCC_PRESET_ROW_MAX];
    char line[64];
    char key;
    int i;

    if (ncc_contour_active()) {
        snprintf(line, sizeof(line), "PATH  step %.1f mm",
                 (double)ncc_contour_step());
    } else if (ncc_visual_mode() == NCC_MODE_RUN) {
        snprintf(line, sizeof(line), "RUN  line %lu",
                 (unsigned long)(ncc_visual_run_line() + 1u));
    } else if (ncc_visual_mode() == NCC_MODE_MANUAL) {
        snprintf(line, sizeof(line), "MANUAL  %c %s",
                 ncc_manual_axis() ? 'Z' : 'X',
                 ncc_manual_continuous()
                     ? "feed" : "step");
    } else {
        /* The keys carry their own outlines (they are buttons); the band under
           them is only the address line, with no box drawn around the lot. */
        ncc_pad_name(name, sizeof(name));
        if (ncc_visual_address()[0]) {
            snprintf(line, sizeof(line), "%s  %s", ncc_visual_address(), name);
        } else {
            snprintf(line, sizeof(line), "%s", name);
        }
    }
    ncc_text_clip(NCC_PAD_X + 2, NCC_PAD_Y - 22, line,
                  (NCC_PAD_W - 4) / ncc_col_width(LVDS_FONT_SMALL),
                  ncc_col_text(), ncc_col_bg(), LVDS_FONT_SMALL);
    for (key = '1'; key <= '9'; key++) {
        i = key - '1';
        labels[i] = ncc_pad_label(key);
    }
    ncc_draw_pad(NCC_PAD_X, NCC_PAD_Y, NCC_PAD_W, NCC_PAD_H, labels,
                 ncc_pad_hot());
}

/* The word the DRO's corner carries: what the machine is doing, or refusing to
   do, right now. It is the controller's *own* answer (`cnc_get_status()`, the
   same word its status line prints) rather than bits the panel picks out of the
   state for itself - `EXEC_RUN` is not the flag the controller reads, and it
   stayed set after the last block, so the strip went on saying RUN with the
   machine reporting `<Idle>` (bench, 2026-09-28: "status is run ... if i get
   ucnc status - it shows idle correctly"). A run the panel is still feeding
   counts too - it says RUN from the moment the stream starts. */
static const char *ncc_state_label(const ncc_runtime_state_t *rt)
{
    (void)rt;
    if (nc_sender_run_error()) return "ERROR";
    if (nc_sender_run_hold()) return "HOLD";
    if (nc_sender_run_streaming()) return "RUN";
    switch (cnc_get_status()) {
    case EXEC_STATUS_ALARM: return "ALARM";
    case EXEC_STATUS_LOCKED: return "LOCKED";
    case EXEC_STATUS_CHECK: return "CHECK";
    case EXEC_STATUS_PROBING: return "PROBING";
    case EXEC_STATUS_DWELL: return "DWELL";
    case EXEC_STATUS_RUNNING: return "RUN";
    case EXEC_STATUS_JOGGING: return "JOG";
    case EXEC_STATUS_HOMING: return "HOMING";
    case EXEC_STATUS_HOLD:
    case EXEC_STATUS_HOLD_PENDING:
    case EXEC_STATUS_HOLD_RESUMING:
    case EXEC_STATUS_DOOR_CLOSED:
    case EXEC_STATUS_DOOR_OPENED:
    case EXEC_STATUS_DOOR_OPENED_PAUSING:
    case EXEC_STATUS_DOOR_CLOSED_RESUMING:
        return "HOLD";
    default:
        break;
    }
    return "IDLE";
}

/* True when that state is a fault: it wears the same red as a message the panel
   would show, so a machine that needs attention says so on the glass. */
static bool ncc_state_is_fault(const ncc_runtime_state_t *rt)
{
    uint16_t state = rt ? rt->exec_state : 0u;

    return cnc_has_alarm() ||
           (state & (EXEC_KILL | EXEC_LIMITS | EXEC_POSITION_MAYBE_LOST)) ||
           nc_sender_run_error();
}

/* Break a sentence into lines of at most `cols` characters, at spaces, and
   answer how many it took. One long word is cut rather than left to run off the
   panel. */
static int ncc_wrap(const char *text, int cols, char lines[][48], int max)
{
    const char *rest = text;
    int count = 0;

    while (rest && *rest && count < max && cols > 0) {
        size_t len = strlen(rest);

        if (len > (size_t)cols) {
            size_t cut = (size_t)cols;

            while (cut > 0u && rest[cut] != ' ') {
                cut--;
            }
            if (cut == 0u) {
                cut = (size_t)cols;     /* one very long word: cut it */
            }
            len = cut;
        }
        memcpy(lines[count], rest, len);
        lines[count][len] = '\0';
        count++;
        rest += len;
        while (*rest == ' ') {
            rest++;
        }
    }
    return count;
}

/* The notes in the space above the 3x3 (the bench: *"use space above 3x3 to fit
   labels like errors or big message or helpers"*).

   An error goes first, and it goes **nc's way**: a block of white letters on the
   fault's own red, as many lines as the sentence needs (`nc_visual_draw_fault()`
   - bench: *"error style - red background with white letters. same as nc"*). It
   is drawn in the panel's own font, not the labels' small one, because the one
   thing an operator must not miss is worth the room. What the screen's keys do
   follows, from the screen's own answer (`ncc_visual_usage()`, the same lines
   the station's side strip shows), so the machine carries its own help instead
   of the PC being the only place it is written. */
static void ncc_draw_notes(void)
{
    const char *const *lines = 0;
    size_t count = ncc_visual_usage(&lines);
    ncc_runtime_state_t rt;
    bool fault;
    int y = NCC_NOTES_Y + 2;
    int cols_small = (NCC_NOTES_W - 8) / ncc_col_width(LVDS_FONT_SMALL);
    int cols_band = (NCC_NOTES_W - 8) / ncc_col_width(LVDS_FONT_NORMAL);
    size_t i;

    ncc_state_runtime(&rt);
    fault = ncc_state_is_fault(&rt);
    if (ncc_visual_status()[0] || fault) {
        char text[64];
        char wrapped[NCC_FAULT_LINES][48];
        int wrapped_count;
        int line;

        if (ncc_visual_status()[0]) {
            snprintf(text, sizeof(text), "%s", ncc_visual_status());
        } else {
            snprintf(text, sizeof(text), "uCNC %s", ncc_state_label(&rt));
        }
        if (fault) {
            /* The fault block: white on red, and as tall as it needs to be.
               Everything the panel can ever have to say fits: the status line
               is `NCC_STATUS_MAX` characters, and the block's own font is
               measured against it - the panel's own if the sentence fits its
               columns, the labels' smaller one if it does not. An error that
               cannot be read in full is worse than no error at all (bench:
               "there is nothing more annoying than an error you cannot read. So
               it should fit all possible messages"). */
            int cols = cols_band;
            int row_h = NCC_FAULT_ROW_H;
            int font = LVDS_FONT_NORMAL;
            int band_h;

            if ((int)(strlen(text) + strlen(ncc_visual_refused())) >
                cols_band * NCC_FAULT_LINES) {
                cols = cols_small;
                row_h = 11;
                font = LVDS_FONT_SMALL;
            }
            wrapped_count = ncc_wrap(text, cols, wrapped, NCC_FAULT_LINES);
            /* The block that was refused rides under the sentence, in the same
               red: the program's line is where to look, and this is what the
               machine was given from it. */
            if (ncc_visual_refused()[0] && wrapped_count < NCC_FAULT_LINES) {
                wrapped_count += ncc_wrap(ncc_visual_refused(), cols,
                                          wrapped + wrapped_count,
                                          NCC_FAULT_LINES - wrapped_count);
            }
            band_h = wrapped_count * row_h + 4;
            ncc_fill(NCC_NOTES_X, y - 2, NCC_NOTES_W - 4, band_h,
                     ncc_col_error());
            for (line = 0; line < wrapped_count; line++) {
                ncc_text_clip(NCC_NOTES_X + 4, y + line * row_h,
                              wrapped[line], cols, ncc_col_field_fg(),
                              ncc_col_error(), font);
            }
            y += band_h + 4;
        } else {
            /* A plain message is a **title** over the notes: the panel's own
               font, one step up from the labels under it (bench, 2026-10-04:
               *"make it one font bigger in size too. it is a title sort of"*),
               and the whole column's width so it is not cut at the panel's edge
               (bench: "error ms is being clipped. no - use full label space to
               show it"). */
            wrapped_count = ncc_wrap(text, cols_band, wrapped, 2);
            for (line = 0; line < wrapped_count; line++) {
                ncc_text_clip(NCC_NOTES_X + 4, y, wrapped[line], cols_band,
                              ncc_col_text(), ncc_col_bg(),
                              LVDS_FONT_NORMAL);
                y += NCC_FAULT_ROW_H;
            }
        }
    }
    for (i = 0u; i < count && y + 8 <= NCC_NOTES_BOTTOM; i++) {
        ncc_text_clip(NCC_NOTES_X + 4, y, lines[i], cols_small, ncc_col_dim(),
                      ncc_col_bg(), LVDS_FONT_SMALL);
        y += 11;
    }
}

/* The tool the cursor is on, drawn in the pane the tools screen leaves for it:
   its own fields. The insert picture and the `O` code are gone: the operator
   checks the tool itself, and the radius is the one number the brush and the
   cycle compensation use. The names are the table's own letters - the same
   file the rows above edit, read the other way. */
static void ncc_draw_tool_view(int x, int y, int w, int h)
{
    static const struct {
        char letter;
        const char *name;
    } params[] = {
        { 'T', "Tool" }, { 'R', "Radius" }, { 'F', "Feed" },
        { 'Q', "F.feed" }, { 'D', "DOC" }, { 'E', "FDOC" },
        { 'S', "RPM" }, { 'X', "X offset" }, { 'Z', "Z offset" }
    };
    const char *row = (ncc_view_doc.line_count && ncc_view_doc.cursor < ncc_view_doc.line_count)
                          ? ncc_view_doc.lines[ncc_view_doc.cursor]
                          : "";
    ncc_tool_t tool;
    int line_y = y + 26;
    char buf[48];
    size_t i;

    if (ncc_tool_from_line(row, &tool)) {
        snprintf(buf, sizeof(buf), "TOOL T%d", tool.t);
    } else {
        memset(&tool, 0, sizeof(tool));
        snprintf(buf, sizeof(buf), "TOOL  -");
    }
    ncc_text_clip(x + 4, y + 2, buf, (w - 8) / ncc_col_width(LVDS_FONT_NORMAL),
                  ncc_col_text(), ncc_col_bg(), LVDS_FONT_NORMAL);

    for (i = 0u; i < sizeof(params) / sizeof(params[0]); i++) {
        char value[24];
        int cols = (w - 96) / ncc_col_width(LVDS_FONT_NORMAL);

        if (!ncc_tool_word_text(row, params[i].letter, value, sizeof(value))) {
            snprintf(value, sizeof(value), "-");
        }
        ncc_text_clip(x + 4, line_y, params[i].name, 10, ncc_col_dim(),
                      ncc_col_bg(), LVDS_FONT_NORMAL);
        ncc_text_clip(x + 92, line_y, value, cols, ncc_col_text(), ncc_col_bg(),
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
static void ncc_draw_dro(void)
{
    ncc_runtime_state_t rt;
    float work[AXIS_COUNT] = { 0 };
    lvds_color_t bg;
    const char *state;
    bool fault;
    char buf[40];
    int x = NCC_DRO_X + 8;
    int y = NCC_DRO_Y + (NCC_DRO_H - 14) / 2;

    ncc_state_runtime(&rt);
    work[AXIS_X] = rt.x;
    work[AXIS_Z] = rt.z;
    parser_machine_to_work(work);
    state = ncc_state_label(&rt);
    fault = ncc_state_is_fault(&rt);
    bg = fault ? ncc_col_error()
               : (nc_sender_run_active() || nc_sender_run_streaming()
                      ? ncc_col_run()
                      : ncc_col_header());

    ncc_fill(NCC_DRO_X, NCC_DRO_Y, NCC_DRO_W, NCC_DRO_H, bg);
    /* The strip reads left to right: what the machine is doing, the two axes a
       lathe hand reads - X a diameter, Z the length - then the feed and the
       spindle's speed, and at the right edge the *setup* words: the tool in the
       spindle, the compensation it is cutting with (G40/G41/G42) and the
       coordinate system the work is in (G54...). `uCNC` itself left the strip
       for the header when the state word moved to the left, where an eye lands
       first (bench, 2026-09-28). */
    ncc_text(x, y, state, ncc_col_dim(), bg, LVDS_FONT_NORMAL);
    x += 8 * (int)strlen(state) + 16;
    ncc_text(x, y, "X", ncc_col_dim(), bg, LVDS_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "%9.3f", (double)work[AXIS_X]);
    ncc_text_clip(x + 16, y, buf, 9, ncc_col_text(), bg, LVDS_FONT_NORMAL);
    x += 100;
    ncc_text(x, y, "Z", ncc_col_dim(), bg, LVDS_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "%9.3f", (double)work[AXIS_Z]);
    ncc_text_clip(x + 16, y, buf, 9, ncc_col_text(), bg, LVDS_FONT_NORMAL);
    x += 100;
    ncc_text(x, y, "F", ncc_col_dim(), bg, LVDS_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "%7.1f", (double)rt.feed);
    ncc_text_clip(x + 16, y, buf, 7, ncc_col_text(), bg, LVDS_FONT_NORMAL);
    x += 84;
    ncc_text(x, y, "S", ncc_col_dim(), bg, LVDS_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "%7u", rt.spindle);
    ncc_text_clip(x + 16, y, buf, 7, ncc_col_text(), bg, LVDS_FONT_NORMAL);

    /* The setup words, right-aligned: the tool the run has in the spindle, the
       compensation in play, and the coordinate system the figures above are
       read in. The tool is the machine's own answer (`nc_sender_run_tool()`), which is
       -1 until a `T` has gone out. */
    {
        int tool = nc_sender_run_tool();
        g7x_comp_side_t comp = nc_sender_run_comp_mode();

        snprintf(buf, sizeof(buf), "T%d %s G%u", tool,
                 comp == G7X_COMP_LEFT ? "G41" :
                 (comp == G7X_COMP_RIGHT ? "G42" : "G40"),
                 (unsigned)parser_get_coord_system());
    }
    ncc_text_clip(NCC_DRO_X + NCC_DRO_W - 8 - ncc_text_width(buf, LVDS_FONT_NORMAL),
                  y, buf, 22, ncc_col_text(), bg, LVDS_FONT_NORMAL);
}

/* The drawing, every screen and whatever the screen has open: the machine is
   making a part, and the part belongs at the top where the operator looks. What
   the machine is doing is read once, and the tool the program has chosen is what
   the glyph rides the cut with. */
static void ncc_draw_preview(bool full)
{
    ncc_runtime_state_t rt;
    ncc_preview_run_t run;
    ncc_tool_t tool;

    /* Every screen draws it, the TOOLS screen included - its rows paint over
       the pane in the same frame, so what that screen shows is unchanged, but
       the live stock has to keep up wherever the operator is standing. Skipping
       the drawing there froze the mask while the machine cut, and the pane on
       the RUN screen then caught up with one straight sweep across the material
       the tool had really walked. */
    ncc_state_runtime(&rt);
    run.busy = ncc_state_busy() || nc_sender_run_active() || nc_sender_run_hold();
    run.screen_run = ncc_visual_mode() == NCC_MODE_RUN;
    /* The TOOLS screen's rows own the pane: the live stock is kept up to date
       there and not painted, so the table is what that screen shows. */
    run.paint = ncc_visual_mode() != NCC_MODE_TOOLS;
    run.full = full;
    /* The machine's own axis position: the drawing's frame owns the work offset
       and the radius/diameter conversion (`ncc_preview.c`, "the frames"),
       so a touch-off (`G10 L20`) and `G54` move the cut with the part in one
       place, and the screen does not make a second copy of that rule. */
    run.x = rt.x;
    run.z = rt.z;
    /* And the **tool the machine is wearing**, by its number, from the table -
       never from whichever document this screen happens to hold: the TOOLS
       screen's buffer *is* the table (its first row is `T1`), and a run started
       after its `T` row never re-speaks the number. `nc_sender_run_tool()` is that
       number, kept across runs, and with no run at all the editor's own row
       answers (bench, 2026-10-04: *"now glyph shows t1 but in code i see only
       t2 and it was just run with proper t2 ?"*). */
    run.tool = 0;
    if (run.busy) {
        int t = nc_sender_run_tool();

        if (t > 0 ? ncc_tools_number(t, &tool)
                  : ncc_tools_active(&ncc_view_doc, ncc_visual_run_line(), &tool)) {
            run.tool = &tool;
        }
    }
    ncc_preview_draw(&ncc_view_doc, &run, NCC_PREVIEW_X, NCC_PREVIEW_Y, NCC_PREVIEW_W,
                     NCC_PREVIEW_PANE_H);
}

/* What the last frame showed that does not move on its own, in two bands: the
   rows of the file the pane draws, and what the screen wears around them (the
   header, the notes, the pad and its caption). While neither changes, only the
   drawing and the machine's strip are painted again; when only the *mark* moves
   - which is what a run does - the rows are painted and the chrome is left
   alone. The bench: "fps is dead slow - again full screen is refreshed not only
   preview area?" */
typedef struct {
    ncc_mode_t mode;
    bool list;
    size_t cursor;
    size_t mark;                /* the line the rows draw their mark on */
    int field;
    size_t lines;
    char address[NCC_ADDR_MAX + 1];
} ncc_rows_key_t;

typedef struct {
    ncc_mode_t mode;
    bool list;
    bool fault;
    char pad_hot;               /* MANUAL's key flash, and the run's hold */
    char status[64];
    char address[NCC_ADDR_MAX + 1];
    uint16_t fps;
    int32_t encoder_positions[3];
    uint16_t analog_mv[4];
} ncc_chrome_key_t;

static ncc_rows_key_t g_rows_key;
static ncc_chrome_key_t g_chrome_key;
static bool g_frame_key_valid;

/* The line the panel is on, said out loud. `nc` did this (`nc_editor.c`'s
   `[MSG:NC SELECT RUN 7: G71 U3 ...]`) and the bench walks a program off the
   serial as much as off the glass: the generated cycle is bracketed by the
   G7x messages, and the line each of them came from is this one. */
static void ncc_serial_selected_line(const ncc_rows_key_t *key)
{
    char text[97];

    text[0] = '\0';
    if (ncc_view_doc.line_count > 0u && key->mark < ncc_view_doc.line_count) {
        snprintf(text, sizeof(text), "%.96s", ncc_view_doc.lines[key->mark]);
    }
    grbl_stream_printf("[MSG:NCC SELECT %s %lu: %s]\r\n",
                       ncc_visual_screen_name(),
                       (unsigned long)(key->mark + 1u), text);
}

static void ncc_frame_keys_of(ncc_rows_key_t *rows, ncc_chrome_key_t *chrome)
{
    ncc_runtime_state_t rt;

    memset(rows, 0, sizeof(*rows));
    rows->mode = ncc_visual_mode();
    rows->list = ncc_visual_list();
    rows->cursor = ncc_view_doc.cursor;
    rows->mark = (ncc_visual_mode() == NCC_MODE_RUN) ? ncc_visual_run_line()
                                          : ncc_view_doc.cursor;
    rows->field = ncc_view_doc.field;
    rows->lines = ncc_view_doc.line_count;
    snprintf(rows->address, sizeof(rows->address), "%s", ncc_visual_address());

    memset(chrome, 0, sizeof(*chrome));
    chrome->mode = ncc_visual_mode();
    chrome->list = ncc_visual_list();
    ncc_state_runtime(&rt);
    chrome->fault = ncc_state_is_fault(&rt);
    chrome->pad_hot = ncc_pad_hot();
    snprintf(chrome->status, sizeof(chrome->status), "%s", ncc_visual_status());
    snprintf(chrome->address, sizeof(chrome->address), "%s", ncc_visual_address());
    chrome->fps = g_ncc_fps;
    for (uint8_t i = 0; i < 3u; i++) {
        chrome->encoder_positions[i] = ncc_visual_encoder_position(i);
    }
    for (uint8_t i = 0; i < 4u; i++) {
        int16_t raw = io_get_pinvalue((uint8_t)(ANALOG0 + i));
        float millivolts;

        if (raw < 0) {
            raw = 0;
        }
        millivolts = (float)raw * NCC_ADC_MV_PER_COUNT;
        chrome->analog_mv[i] = (uint16_t)(millivolts + 0.5f);
        g_ncc_analog_mv[i] = chrome->analog_mv[i];
    }
}

void ncc_visual_draw(void)
{
    ncc_rows_key_t rows_key;
    ncc_chrome_key_t chrome_key;
    bool full;
    bool rows;
    bool chrome;

    if (ncc_boot_active()) {
        /* The splash is a still picture: it is drawn once (the frame that comes
           up already asks for one) and the flag is cleared here, because this
           path does not reach the frame machinery that would. */
        ncc_boot_draw();
        /* The renderer draws into the PSRAM backbuffer and the scanout reads
           SRAM: without this the panel keeps showing whatever it was showing -
           black on a cold boot, or the last work screen on a restart. It is a
           no-op on the host backend, which is why every host frame looked right
           while the glass stayed dark. */
        lvds_hstx_present();
        ncc_visual_clear_dirty();
        return;
    }
    ncc_frame_keys_of(&rows_key, &chrome_key);
    /* A mode or a screen change paints the lot: the bands are in different
       places then. Anything else is per band. */
    full = ncc_visual_dirty() || !g_frame_key_valid ||
           rows_key.mode != g_rows_key.mode || rows_key.list != g_rows_key.list;
    /* The line the panel is on, to the serial: a run walking a cycle, or an
       operator stepping the cursor. A *change* of the line and nothing else - a
       new screen or a file list shows the same line and is not a select, and the
       station draws this serial in its strip, where a line nothing asked for
       would make a still screen look busy (`--painttest` holds that). */
    if (g_frame_key_valid && rows_key.mark != g_rows_key.mark) {
        ncc_serial_selected_line(&rows_key);
    }
    rows = full || memcmp(&rows_key, &g_rows_key, sizeof(rows_key)) != 0;
    chrome = full ||
             memcmp(&chrome_key, &g_chrome_key, sizeof(chrome_key)) != 0;
    if (full) {
        ncc_fill(0, 0, LVDS_VIEW_WIDTH, LVDS_VIEW_HEIGHT, ncc_col_bg());
    }
    if (full || chrome) {
        ncc_draw_header();
    }
    /* The two bands that move on their own: the drawing (the machine's own
       position, and the tool with it) and the strip's figures. Everything else
       is drawn when something it shows has changed. */
    ncc_draw_preview(full);
    ncc_draw_dro();
    if (ncc_visual_list()) {
        /* The list is one column, not the editor's pane: it takes the whole
           bottom band while it is up, the way a picker does. */
        if (rows) {
            ncc_fill(NCC_TEXT_X, NCC_TEXT_Y,
                     NCC_PAD_X + NCC_PAD_W - NCC_TEXT_X, NCC_TEXT_H,
                     ncc_col_bg());
            ncc_draw_list();
        }
    } else if (ncc_visual_mode() == NCC_MODE_MANUAL) {
        /* MANUAL is a machine panel: the pane carries the stops and the value
           the keys change and the pad its jog keys. */
        if (rows) {
            ncc_fill(NCC_TEXT_X, NCC_TEXT_Y, NCC_TEXT_W, NCC_TEXT_H,
                     ncc_col_bg());
            ncc_manual_draw_pane(NCC_TEXT_X, NCC_TEXT_Y, NCC_TEXT_W,
                                 NCC_TEXT_H);
        }
        if (full || chrome) {
            ncc_draw_notes();
            ncc_draw_pad_band();
        }
    } else if (ncc_visual_mode() == NCC_MODE_TOOLS) {
        /* The tools screen: the table's own rows in the pane under the header -
           they are the text the operator edits, and the top half is where the
           eye reads it - and the tool the cursor is on drawn in the pane below
           (bench: "on tools - bring it back just fit into current screen so top
           one is text lines, bottom one is tool view"). */
        if (full) {
            ncc_fill(NCC_PREVIEW_X, NCC_PREVIEW_Y, NCC_PREVIEW_W,
                     NCC_PREVIEW_PANE_H, ncc_col_bg());
        }
        if (rows) {
            ncc_fill(NCC_PREVIEW_X, NCC_PREVIEW_Y, NCC_PREVIEW_W,
                     NCC_PREVIEW_PANE_H, ncc_col_bg());
            ncc_draw_rows(NCC_PREVIEW_X, NCC_PREVIEW_Y, NCC_PREVIEW_W,
                          NCC_PREVIEW_PANE_H);
            ncc_fill(NCC_TEXT_X, NCC_TEXT_Y, NCC_TEXT_W, NCC_TEXT_H,
                     ncc_col_bg());
            ncc_draw_tool_view(NCC_TEXT_X, NCC_TEXT_Y, NCC_TEXT_W, NCC_TEXT_H);
        }
        if (full || chrome) {
            ncc_draw_notes();
            ncc_draw_pad_band();
        }
    } else {
        if (rows) {
            ncc_fill(NCC_TEXT_X, NCC_TEXT_Y, NCC_TEXT_W, NCC_TEXT_H,
                     ncc_col_bg());
            ncc_draw_rows(NCC_TEXT_X, NCC_TEXT_Y, NCC_TEXT_W, NCC_TEXT_H);
        }
        if (full || chrome) {
            ncc_draw_notes();
            ncc_draw_pad_band();
        }
    }
    g_rows_key = rows_key;
    g_chrome_key = chrome_key;
    g_frame_key_valid = true;
    ncc_visual_clear_dirty();
    lvds_hstx_present();
    ncc_fps_tick();
}

/* --- the shell's own questions -------------------------------------------- */



bool ncc_visual_periodic_needed(void)
{
    /* What changes on its own: the machine doing something (the DRO's figures,
       a held feed), MANUAL's key flash. An *unsaved* program is not one of
       them - that flag is about writing the file back, and it used to keep a
       still screen repainting every 20 ms. */
    return ncc_state_busy() || nc_sender_run_active() ||
           nc_sender_run_hold() || nc_sender_run_error() || ncc_visual_mode() == NCC_MODE_MANUAL;
}

size_t ncc_visual_usage(const char *const **lines)
{
    static const char *const program[] = {
        "The program, one line at a time.",
        "D walks a line's fields.",
        "A digit types the value, B is the",
        "sign and C the point.",
        "B/C move by line; B on line 1 is",
        "the file's row, and D opens it.",
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
    if (ncc_visual_list()) {
        table = files;
        count = sizeof(files) / sizeof(files[0]);
    } else if (ncc_visual_mode() == NCC_MODE_MANUAL) {
        table = manual;
        count = sizeof(manual) / sizeof(manual[0]);
    } else if (ncc_visual_mode() == NCC_MODE_TOOLS) {
        table = tools;
        count = sizeof(tools) / sizeof(tools[0]);
    } else if (ncc_visual_mode() == NCC_MODE_RUN) {
        table = run;
        count = sizeof(run) / sizeof(run[0]);
    } else {
        table = program;
        count = sizeof(program) / sizeof(program[0]);
    }
    *lines = table;
    return count;
}

bool ncc_visual_key_meaning(char key, ncc_visual_key_meaning_t *meaning)
{
    if (!meaning) {
        return false;
    }
    meaning->label = 0;
    meaning->on_menu = false;
    meaning->step = false;
    if (ncc_visual_list()) {
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
    if (ncc_visual_mode() == NCC_MODE_MANUAL) {
        if (key >= '1' && key <= '9') {
            meaning->label = ncc_manual_pad_label(key);
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
    if (ncc_visual_mode() == NCC_MODE_RUN) {
        if (key >= '1' && key <= '5') {
            meaning->label = ncc_pad_label(key);
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
        meaning->label = ncc_pad_label(key);
        meaning->on_menu = true;
        return meaning->label[0] != '\0';
    }
    switch (key) {
    case 'A': meaning->label = "level"; break;
    case '0': meaning->label = "files"; break;
    case 'B': meaning->label = "line -"; meaning->step = true; break;
    case 'C': meaning->label = "line +"; meaning->step = true; break;
    case 'D': meaning->label = "field"; break;
    case '#': meaning->label = "preview"; break;
    case '*': meaning->label = "delete"; break;
    default: return false;
    }
    return true;
}
