#ifndef NC2_TOOLS_H
#define NC2_TOOLS_H

#include "nc2.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The tool table, read the way the tool screen draws it.

   The row is the machine's own (`T1 R0.8 O3 F120 Q60 D2.0 E0.5 S800 X0 Z0`),
   the same file `nc` wrote and the TOOLS screen edits, so the reading here is
   what turns a row into the numbers a tool drawing needs. It is `nc2`'s now:
   the panel owns the table, and `nc_tools.c` stays in the tree as the record of
   the module it replaced. */

#define NC2_TOOL_MAX 8

typedef struct {
    int t;                  /* the tool's number */
    float r;                /* nose radius */
    int orient;             /* the orientation code, as written */
    float feed;
    float finish_feed;      /* Q, the feed a finish cut uses */
    float doc;
    float finish_doc;       /* E */
    float rpm;              /* S */
    float xoff;
    float zoff;
    bool valid;
} nc2_tool_t;

/* One row of the table, or the shipped T<n> row when the text is not one. */
bool nc2_tool_from_line(const char *line, nc2_tool_t *tool);
/* Just the `T` number of a line, or false when it names none. The pacer asks
   this of every block it hands over, to remember the tool the machine has. */
bool nc2_tools_line_tool_number(const char *line, int *tool);
/* One field of a row as text, for the tool view's own lines. */
bool nc2_tool_word_text(const char *line, char letter, char *out, size_t out_sz);
/* The shipped row for a tool number, as text (static storage). */
const char *nc2_tool_default_line(int t);
/* Is the orientation code one the drawing can draw? */
bool nc2_tool_orient_valid(int orient);

/* The table the run and the tool view read: loaded once, not per frame (a FAT
   read in a frame loop is what the frame meter would show). */
void nc2_tools_clear(void);
bool nc2_tools_load(const char *path);
/* The tool a program is using by the time it reaches `upto_line`: the last `T`
   word at or above it, read from the card. False when the program names none -
   `tool` is then -1. */
bool nc2_tools_program_tool(const char *program_path, size_t upto_line,
                            int *tool);
/* The tool the program is using at `line`: the last `T` word at or above it,
   looked up in the loaded table (or the shipped row when the table has none). */
bool nc2_tools_active(const nc2_document_t *program, size_t line,
                      nc2_tool_t *tool);
/* The tool a table row describes, by its own row index (the tool view's
   subject): the row's `T` number, looked up, or the row's own numbers. */
bool nc2_tools_from_row(const char *row, nc2_tool_t *tool);

#ifdef __cplusplus
}
#endif

#endif
