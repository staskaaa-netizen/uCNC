#ifndef NC2_FILES_H
#define NC2_FILES_H

#include "nc2.h"

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

#endif
