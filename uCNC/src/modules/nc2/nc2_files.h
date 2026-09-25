#ifndef NC2_FILES_H
#define NC2_FILES_H

#include "nc2.h"
#include "nc2_layout.h"

#include <stdbool.h>

/* The card: a program read in and written back. The file *list* is next; this is
   the half the screen needs to open the file it was left on and to put the
   operator's edits back. */

/* A path the panel reads as a program (`.nc`). Everything else on the card is
   text - a preset entry, a tool table, notes - and is not run. */
bool nc2_path_is_program(const char *path);
/* A path the editor may open at all: a program or a text file. */
bool nc2_path_is_text(const char *path);

/* Read `path` into `doc`. False when the file is not there or is empty; the
   document is then left as it was. A line longer than the panel's own is cut
   where it does not fit, because the panel could not show or edit the rest. */
bool nc2_file_load(nc2_document_t *doc, const char *path);
/* Write the document back to its own path. False when there is no path. */
bool nc2_file_save(const nc2_document_t *doc);

/* The list: the folder the operator is looking at, and what is in it. The
   entries are read once per scan (a folder, not a directory service), sorted with
   the folders first so the screen cannot shuffle under the operator's finger, and
   `..` is the way back up. */
typedef struct {
    char name[NC2_NAME_MAX];
    bool is_dir;
} nc2_file_entry_t;

/* Read `dir` into the list and put the selection on the first entry. False when
   the folder cannot be read. */
bool nc2_file_scan(const char *dir);
const char *nc2_file_dir(void);
int nc2_file_count(void);
const nc2_file_entry_t *nc2_file_entry(int index);
int nc2_file_selected(void);
void nc2_file_step(int delta);
/* The path of what is selected, or false when nothing is. */
bool nc2_file_selected_path(char *out, size_t out_sz);
/* Delete the selected file. False for a folder, or when the card refuses. */
bool nc2_file_delete_selected(void);
/* Create `name.ext` in the folder being listed, and answer its path. */
bool nc2_file_create(const char *name, const char *ext, char *out, size_t out_sz);

/* The name being typed for a new file, which the digits of the pad build - the
   machine has no letter keys, so a new program is named with a number. */
void nc2_file_new_begin(void);
void nc2_file_new_end(void);
bool nc2_file_new_active(void);
void nc2_file_new_digit(char digit);
void nc2_file_new_backspace(void);
const char *nc2_file_new_name(void);

#endif
