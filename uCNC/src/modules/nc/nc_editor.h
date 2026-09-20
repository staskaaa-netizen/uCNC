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
#include "nc_state.h"
#include "nc_visual.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    nc_document_t *doc;              /* the screen's buffer */
    const nc_snapshot_t *snapshot;   /* the frame being drawn, or NULL */
    bool editable;                   /* this screen may change the code */
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

/* A word is selected and the keys type into it. */
bool nc_editor_selected_word_key(nc_editor_ctx_t *ctx, nc_visual_key_t key, char ch);

/* The footer's menu entries (cycles, tools, G-code, ops) open the helper. */
void nc_editor_open_modal(nc_editor_ctx_t *ctx, uint8_t action);

/* The G/T field: the line itself takes the digits, so there is no panel. */
void nc_editor_open_field(nc_editor_ctx_t *ctx, char prefix);

/* Movement keys drop a half-typed value instead of applying it. */
void nc_editor_clear_draft(void);

/* The draft line and the helper panel. */
void nc_editor_draw_aids(nc_editor_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif
