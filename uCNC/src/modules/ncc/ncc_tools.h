#ifndef NCC_TOOLS_H
#define NCC_TOOLS_H

#include "ncc.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The tool table, read the way the tool screen draws it.

   The row is the machine's own (`T1 R0.8 F120 Q60 D2 E0.5 S800 X0 Z0`), the
   same file the TOOLS screen edits. The orientation word is gone; the rest are
   the table's own fields. The radius is what the live brush and the cycle
   compensation use. It is `ncc`'s now:
   the panel owns the table, and `nc_tools.c` stays in the tree as the record of
   the module it replaced. */

#define NCC_TOOL_MAX 8

typedef struct {
    int t;                  /* the tool's number */
    float r;                /* nose radius */
    float feed;
    float finish_feed;      /* Q, the feed a finish cut uses */
    float doc;
    float finish_doc;       /* E */
    float rpm;              /* S */
    float xoff;
    float zoff;
    bool valid;
} ncc_tool_t;

/* One row of the table, or the shipped T<n> row when the text is not one. */
bool ncc_tool_from_line(const char *line, ncc_tool_t *tool);
/* One field of a row as text, for the tool view's own lines. */
bool ncc_tool_word_text(const char *line, char letter, char *out, size_t out_sz);
/* The shipped row for a tool number, as text (static storage). */
const char *ncc_tool_default_line(int t);
/* The table the run and the tool view read: loaded once, not per frame (a FAT
   read in a frame loop is what the frame meter would show). */
void ncc_tools_clear(void);
bool ncc_tools_load(const char *path);
/* The tool a program is using by the time it reaches `upto_line`: the last `Tn`
   at or above it (G7x's own word, `g7x_line_tool()`), read from the card.
   False when the program names none - `tool` is then -1. */
bool ncc_tools_program_tool(const char *program_path, size_t upto_line,
                            int *tool);
/* The tool the program is using at `line` - G7x's state machine
   (`g7x_state_at()`) says which - looked up in the loaded table (or the shipped
   row when the table has none). False when the program names none. */
bool ncc_tools_active(const ncc_document_t *program, size_t line,
                      ncc_tool_t *tool);
/* The row the *table* holds for tool `t` - the loaded one, or the shipped row
   for that number - with no *document* asked. The run knows which tool its
   emitted lines carry (it read it when it handed them over), so a screen reads
   that tool through this instead of asking whichever document it happens to be
   showing: the table and the program are different owners, a run started after
   its `T` row still wears that tool, and the TOOLS screen's buffer is nobody's
   program (bench, 2026-10-04: *"glyph shows t1 but in code i see only t2 and it
   was just run with proper t2"*). */
bool ncc_tools_number(int t, ncc_tool_t *tool);
/* The tool a table row describes, by its own row index (the tool view's
   subject): the row's `T` number, looked up, or the row's own numbers. */

#ifdef __cplusplus
}
#endif

#endif
