#ifndef NCC_FILES_H
#define NCC_FILES_H

#include "ncc.h"
#include "ncc_layout.h"

#include <stdbool.h>

/* The card: a program read in and written back. The file *list* is next; this is
   the half the screen needs to open the file it was left on and to put the
   operator's edits back. */

/* A path the panel reads as a program (`.nc`). Everything else on the card is
   text - a preset entry, a tool table, notes - and is not run. */
bool ncc_path_is_program(const char *path);
/* A path the editor may open at all: a program or a text file. */
bool ncc_path_is_text(const char *path);
/* Does the path end with this suffix, either case? The card is FAT with short
   names only, so what the machine reads back is `LATHE-~1.NC` for a file written
   on a PC as `lathe-demo.nc`: the extension is capitals, and asking for it in
   lower case is how a whole card of programs went missing. */
bool ncc_path_has_suffix(const char *path, const char *suffix);

/* Read `path` into `doc`. False when the file is not there or is empty; the
   document is then left as it was. A line longer than the panel's own is cut
   where it does not fit, because the panel could not show or edit the rest. */
bool ncc_file_load(ncc_document_t *doc, const char *path);
/* Write the document back to its own path. False when there is no path. */
bool ncc_file_save(const ncc_document_t *doc);

/* The list: the folder the operator is looking at, and what is in it. The
   entries are read once per scan (a folder, not a directory service), sorted with
   the folders first so the screen cannot shuffle under the operator's finger, and
   `..` is the way back up. */
typedef struct {
    char name[NCC_NAME_MAX];
    bool is_dir;
} ncc_file_entry_t;

/* Read `dir` into the list and put the selection on the first entry. False when
   the folder cannot be read. */
bool ncc_file_scan(const char *dir);
const char *ncc_file_dir(void);
int ncc_file_count(void);
const ncc_file_entry_t *ncc_file_entry(int index);
int ncc_file_selected(void);
void ncc_file_step(int delta);
/* The path of what is selected, or false when nothing is. */
bool ncc_file_selected_path(char *out, size_t out_sz);
/* Land the selection on `path` when it is in the folder being listed - the way
   the list opens on the file the editor has - and leave it where it was when
   the path is not there. */
bool ncc_file_select_path(const char *path);
/* Delete the selected file. False for a folder, or when the card refuses. */
bool ncc_file_delete_selected(void);
/* Create `name.ext` in the folder being listed, and answer its path. */
bool ncc_file_create(const char *name, const char *ext, char *out, size_t out_sz);

/* The name being typed for a new file, which the digits of the pad build - the
   machine has no letter keys, so a new program is named with a number. */
void ncc_file_new_begin(void);
void ncc_file_new_end(void);
bool ncc_file_new_active(void);
void ncc_file_new_digit(char digit);
void ncc_file_new_backspace(void);
const char *ncc_file_new_name(void);

#endif
