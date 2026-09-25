#ifndef NC2_PRESETS_H
#define NC2_PRESETS_H

#include <stdbool.h>
#include <stddef.h>

/* The entries.

   An address is the digits walked on the pad, and the file whose name is that
   address is the slot: `4` then `7` is `47`, and one level deeper is one more
   digit (`471`). There is no format to parse and nothing to hold in RAM: the
   first row of the file is the name, every row after it is what the key writes,
   and a row that starts with a space continues the row above it. The file list
   and the editor are then the entry editor - this file only finds the file and
   spells it. `nc2`'s README is the contract. */

#define NC2_PRESET_DIR "/D/presets/"
#define NC2_PRESET_SUFFIX ".txt"

/* The same folder, for the calls that take a *directory* rather than a file:
   `fs_opendir()` writes into the string it is given to drop a trailing `/`, so a
   path that ends in one has to be a caller's own buffer - a literal would be
   written to read-only memory (see `fs_opendir()`, `file_system.c`). */
#define NC2_PRESET_ROOT "/D/presets"

/* How many digits an address may have: the pad is three levels deep. */
#define NC2_PRESET_ADDR_MAX 3

/* The longest row an entry may carry. A row longer than the program's own line
   width could not be typed into the program anyway; the reader cuts one that
   does not fit rather than losing the file. */
#define NC2_PRESET_ROW_MAX 96

/* The file one address lives in. False when the address is not one to three
   digits, or when it does not fit. */
bool nc2_preset_path(const char *address, char *out, size_t out_sz);

/* True when there is a file at that address. */
bool nc2_preset_exists(const char *address);

/* The entry at `address`: the name (the first row) and the rows after it, joined
   with `\n` - a row that continues the one above keeps its leading space, so what
   is read is what was written. `rows` may be NULL when only the name is wanted.
   False when there is no file: an address with no file is not an entry, and
   there is no table behind it to fall back on. */
bool nc2_preset_read(const char *address, char *name, size_t name_sz,
                     char *rows, size_t rows_sz);

/* Write one entry, and never over one: 1 when it was written, 0 when the file is
   already there, -1 on an I/O error. `rows == NULL` writes the name alone, which
   is what a slot that only holds children is. */
int nc2_preset_write(const char *address, const char *name, const char *rows);

/* True when the folder holds at least one entry file: the difference between a
   card the operator has used and one that has never seen an entry. */
bool nc2_presets_any(void);

#endif
