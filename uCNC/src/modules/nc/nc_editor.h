#ifndef NC_EDITOR_H
#define NC_EDITOR_H

/* The editor: the draft a value is typed into, the floating helper that picks
   what to type (the cycle, tool and G-code menus), and the edit of the word the
   cursor has selected.

   The document is not the editor's: it is the screen's buffer - RUN sends it,
   TOOLS holds the tool table in it, the preview reads it - so every call that
   needs it is handed it. What the editor owns is what only it uses, and what it
   may touch outside itself is handed in too: the screen's status line and
   repaint flag, and whether this screen may change the code at all.

   The key map stays with the screen: the order in which a key is offered to the
   footer, the modes, MANUAL and the editor is the screen's business, so the
   screen offers the helper its key first (it owns every key while it is up) and
   the selected word its key where its own sequence wants it.

   Sources of the NC module, never a module of its own - like nc_draw.c,
   nc_preview.c and nc_manual.c. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nc.h"
#include "nc_menu.h"
#include "nc_presets.h"
#include "nc_state.h"
#include "nc_visual.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    nc_document_t *doc;              /* the screen's buffer */
    const nc_snapshot_t *snapshot;   /* the frame being drawn, or NULL */
    nc_mode_t mode;
    bool editable;                   /* this screen may change the code */
    bool code_view;                  /* EDIT/RUN: the pane shows code */
    char *status;                    /* the screen's status line */
    size_t status_size;
    bool *dirty;
    /* A footer action the helper's menu picked. The screen runs it - the menu
       is the screen's, and so is what its entries do - and clears this to
       NC_FOOTER_ACTION_NONE before the next key. */
    uint8_t follow;
} nc_editor_ctx_t;

/* The floating helper owns every key while it is up. */
bool nc_editor_modal_key(nc_editor_ctx_t *ctx, nc_visual_key_t key, char ch);

/* The file-name row above line 1 is the cursor. */
bool nc_editor_key_name(nc_editor_ctx_t *ctx, nc_visual_key_t key);

/* The new-file field, the word/field keys and the selected value. */
bool nc_editor_key_edit(nc_editor_ctx_t *ctx, nc_visual_key_t key, char ch);

/* The plain cursor keys, once the screen has decided this view may be edited. */
void nc_editor_key_cursor(nc_editor_ctx_t *ctx, nc_visual_key_t key);

/* A word is selected and the keys type into it. */
bool nc_editor_selected_word_key(nc_editor_ctx_t *ctx, nc_visual_key_t key, char ch);

/* The footer's menu entries (cycles, tools, G-code, ops) open the helper. */
void nc_editor_open_modal(nc_editor_ctx_t *ctx, uint8_t action);

/* A footer action the editor owns: the helper's menus, the one-line inserts,
   the file list with open/new/delete/refresh, saving, and the cursor steps.
   False when the action belongs to another owner. */
bool nc_editor_action(nc_editor_ctx_t *ctx, uint8_t action);

/* The G/T field: the line itself takes the digits, so there is no panel. */
void nc_editor_open_field(nc_editor_ctx_t *ctx, char prefix);

/* Movement keys drop a half-typed value instead of applying it. */
void nc_editor_clear_draft(void);

/* The draft line and the helper panel. */
void nc_editor_draw_aids(nc_editor_ctx_t *ctx);

/* The file list the editor opens into, and the code pane with its name row. */
void nc_editor_draw_files(nc_editor_ctx_t *ctx);
void nc_editor_draw_pane(nc_editor_ctx_t *ctx);

/* What the footer actions ask for. */
void nc_editor_move_line(nc_editor_ctx_t *ctx, int delta);
void nc_editor_move_tool_line(nc_editor_ctx_t *ctx, int delta);
void nc_editor_open_files(nc_editor_ctx_t *ctx, const char *root, bool seed_samples);
void nc_editor_open_current_folder(nc_editor_ctx_t *ctx);
void nc_editor_new_file_begin(nc_editor_ctx_t *ctx);
void nc_editor_new_file_end(nc_editor_ctx_t *ctx);
void nc_editor_seed_demo(nc_editor_ctx_t *ctx);
nc_result_t nc_editor_insert_tool_ref(nc_editor_ctx_t *ctx);
/* The tool rows of the TOOLS screen: the document's, read the same way the
   code cursor reads the program. */
int nc_editor_selected_tool_index(nc_editor_ctx_t *ctx);
int nc_editor_find_tool_line(nc_editor_ctx_t *ctx, int selected_tool,
                             int *selected_line);
bool nc_editor_selected_tool_word(nc_editor_ctx_t *ctx, char *letter,
                                  int *line_index);

/* A failed autosave must not wall the machine off: the first press reports, the
   second insists, which is what `kind` remembers. */
enum {
    NC_EDITOR_UNSAVED_NONE = 0,
    NC_EDITOR_UNSAVED_MODE,
    NC_EDITOR_UNSAVED_OPEN,
    NC_EDITOR_UNSAVED_NEW
};
bool nc_editor_save_current(nc_editor_ctx_t *ctx);
bool nc_editor_proceed_without_saving(nc_editor_ctx_t *ctx, uint8_t kind);

/* The selection goes to the serial log, the way the bench reads it. */
void nc_editor_serial_selected_line(nc_editor_ctx_t *ctx);
void nc_editor_serial_selected_file(void);

#ifdef __cplusplus
}
#endif

#endif
