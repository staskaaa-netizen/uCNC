#ifndef NC_PRESETS_H
#define NC_PRESETS_H

#include "nc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The section ids: the key path that inserts the entry, one digit per level, in
   the file the operator reads and edits (`docs/nc-preset-file.md`). They are
   written down once, here, because a renumbering has to reach every caller - the
   path builder inserting an end mark, the editor's menus, and the compiled
   table - and a literal that missed the note is a feature that quietly stops
   working (which is how the setup block went unreachable in the first place). */
/* An id is an address and nothing more: at each one there is a name that may be
   empty and the rows the key writes, which may not. The file format, the alias
   table below the ids and the fixed-size record table are how that map is
   *spelled* today; `docs/nc-preset-file.md` ("The shape this is, and the shape it
   could collapse to") keeps the design honest about which parts are load-bearing
   and which are scaffolding around a one-to-one map. */
#define NC_PRESET_ID_SETUP     16
#define NC_PRESET_ID_END       46
#define NC_PRESET_ID_INS       11
#define NC_PRESET_ID_M6        23
#define NC_PRESET_ID_M3        24
#define NC_PRESET_ID_STOP      25
#define NC_PRESET_ID_M4        26
#define NC_PRESET_ID_CHMF      32
#define NC_PRESET_ID_RND       33
#define NC_PRESET_ID_OD        41
#define NC_PRESET_ID_BORE      42
#define NC_PRESET_ID_FACE      43
#define NC_PRESET_ID_FINISH    48
#define NC_PRESET_ID_THREAD_OD 51
#define NC_PRESET_ID_THREAD_ID 52
#define NC_PRESET_ID_TAP       53
#define NC_PRESET_ID_DRILL     61
#define NC_PRESET_ID_PECK      62
#define NC_PRESET_ID_DWELL     63

/* Boot call: compiled presets, then whatever the SD card can answer with. */
bool nc_presets_init(void);
/* Retried from the NC input path until the preset file is settled. */
bool nc_presets_sync(void);
bool nc_insert_preset_id(nc_document_t *doc, int id);
/* The name the card gives the entry at `id` (`name=`), which is what a key is
   labelled with. False when no entry has that id. */
bool nc_preset_name_for_id(int id, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif
