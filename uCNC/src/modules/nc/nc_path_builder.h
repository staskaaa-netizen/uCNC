#ifndef NC_PATH_BUILDER_H
#define NC_PATH_BUILDER_H

/* The 3x3 path builder: the pad walks the contour. The interaction model is
   written down in docs/nc-path-builder.md; this header is its boundary.

   The builder owns one thing: where the next point is and which word is waiting
   for a value. The pad's drawing (nc_draw_modal_items), the marking of a word
   and the typing into it are the editor's and are reused as they are. Cycle
   templates are inserted through NC's vocabulary before PATH opens; this code
   only appends contour rows to a closed block found by nc_g7x.c. The stock
   corner comes from the preview (nc_preview_collect()).

   Sources of the NC module, never a module of its own - like nc_editor.c,
   nc_manual.c and nc_preview.c. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nc_editor.h"
#include "nc_menu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The footer action the builder owns: the G7X submenu's PATH entry. False when
   the action belongs to another owner. */
bool nc_path_builder_action(nc_editor_ctx_t *ctx, uint8_t action);

bool nc_path_builder_active(void);

/* A key while the builder is up. It owns the digits, the step, undo and cancel;
   the keys that type into a marked word are handed back to the editor, so the
   field flow is one implementation, not two. */
bool nc_path_builder_key(nc_editor_ctx_t *ctx, nc_visual_key_t key, char ch);

/* The screen is leaving: completed rows stay in the program. `0` removes rows
   inserted in the active PATH session before it exits. */
void nc_path_builder_leave(void);

/* The pad, drawn with the editor's own grid and sat where its helper sits. */
void nc_path_builder_draw(const nc_editor_ctx_t *ctx);

/* What a pad key means here, for a shell that labels its own keypad from the
   same table the pad is drawn with. NULL when the key means nothing. */
const char *nc_path_builder_key_hint(char key);

/* The strip while the builder is up: the keys the pad does not carry. */
const nc_footer_item_t *nc_path_builder_footer(size_t *count);

#ifdef __cplusplus
}
#endif

#endif
