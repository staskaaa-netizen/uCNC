#ifndef NCC_PRESETS_H
#define NCC_PRESETS_H

#include <stdbool.h>
#include <stddef.h>

/* The entries, held in RAM.

   An address is the digits walked on the pad, and the entry at that address is
   a name plus the rows a key writes (`4` then `7` is `47`; one level deeper is
   one more digit). There is no folder of files and no card read while the
   operator works: the shipped table below lives in flash and is copied into a
   RAM table once at boot, and the pad's labels and inserts read that table.

   A card may still carry one file, `/D/ncc_presets.txt`, and it is read once at
   boot too: each `[address] name` block follows the same rule the card always
   had - the lines under it are the rows the key writes, and a row that starts
   with a space continues the row above. A block replaces that address; the
   addresses it does not name keep the shipped entry, so a card can change one
   word and leave the rest alone. `ncc/README.md` is the contract. */

/* The longest row an entry may carry, the whole body an entry may carry, and
   the longest name a slot may read as. A row longer than the program's own line
   width could not be typed into the program anyway. */
#define NCC_PRESET_ROW_MAX 96
#define NCC_PRESET_ROWS_MAX (NCC_PRESET_ROW_MAX * 2)
#define NCC_PRESET_NAME_MAX 24

/* How many digits an address may have: the pad is three levels deep. */
#define NCC_PRESET_ADDR_MAX 3

/* The one card file the panel reads, at the card root so the file list and the
   editor can reach it. Nothing is written to it by the panel. */
#define NCC_PRESET_FILE "/D/ncc_presets.txt"

/* Copy the shipped table into RAM. Called once, at module init; the card file is
   read by the first `ncc_presets_sync()` that finds a mounted drive, because the
   card is mounted long after module init. */
void ncc_presets_init(void);
bool ncc_presets_sync(void);

/* The entry at `address`: the name and the rows after it joined with `\n` - a
   row that continues the one above keeps its leading space, so what is read is
   what was written. `rows` may be NULL when only the name is wanted. The answer
   is how many rows the entry writes, 0 when the address is a pad's name with no
   rows of its own, or -1 when nothing lives there. */
int ncc_preset_read(const char *address, char *name, size_t name_sz,
                    char *rows, size_t rows_sz);

/* The address the pad has walked to: the digits themselves, and "" at the root.
   Pushing is what pressing a slot that holds children does, popping is `A`. */
#define NCC_ADDR_MAX NCC_PRESET_ADDR_MAX
void ncc_address_reset(char *address);
bool ncc_address_push(char *address, char digit);
void ncc_address_pop(char *address);

/* What one slot of the pad holds: an entry (rows to write), a pad (things under
   it), both, or nothing. `label` gets the name the slot reads as - the entry's
   name, or the address itself when there is a pad there and no name to show.
   `label` may be NULL when only the kind is wanted. */
#define NCC_SLOT_EMPTY 0
#define NCC_SLOT_ENTRY 1
#define NCC_SLOT_PAD 2
int ncc_slot(const char *address, char digit, char *label, size_t label_sz);

/* The shipped entries themselves, for a tool that writes them out as the card's
   own file (`--dump-presets`): the release ships that file and it is generated
   from this table, so the two cannot drift. */
size_t ncc_preset_builtin_count(void);
/* The `index`th shipped entry: its address, its name and the rows under it
   (`rows` is NULL for a slot that only holds the things under it). False past
   the end. */
bool ncc_preset_builtin(size_t index, const char **address, const char **name,
                        const char **rows);

#endif
