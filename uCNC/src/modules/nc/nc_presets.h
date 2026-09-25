#ifndef NC_PRESETS_H
#define NC_PRESETS_H

#include "nc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The addresses the compiled entries live at: the key path that inserts the
   entry, one digit per level, and the name of the file the operator edits in
   `/D/presets` (`41.txt` is G7X `4` then `2`). They are written down once, here,
   because a renumbering has to reach every caller - the path builder inserting
   an end mark, the editor's menus, and the compiled table - and a literal that
   missed the note is a feature that quietly stops working (which is how the
   setup block went unreachable in the first place).

   An address is not a section and nothing more than a place: at each one there
   is a name that may be empty - the first row of the file - and the rows the key
   writes, which may not. `docs/nc-preset-file.md` is the contract. */
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
/* Retried from the NC input path until the folder is settled. */
bool nc_presets_sync(void);
bool nc_insert_preset_id(nc_document_t *doc, int id);
/* The name the card gives the entry at `id` (`name=`), which is what a key is
   labelled with. False when no entry has that id. */
bool nc_preset_name_for_id(int id, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif
