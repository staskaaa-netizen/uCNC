#ifndef NC2_H
#define NC2_H

#include <stdbool.h>
#include <stddef.h>

#include "../g7x/g7x_blocks.h"

/* nc2: the program, and the value editor that walks it.

   A document is lines of program and a cursor. The editor does one thing with
   them: **a line is cut into fields at its letters** - `X45.2` is the field `X`
   with the value `45.2`, `T` on its own is the field `T` waiting for a number -
   and the keys walk those fields and type into the one that is picked. Nothing
   is checked while editing: what a line *means* is the loader's, G7x's and the
   sender's business, and a program half-typed is not an error, it is half-typed.

   The pad's helper lives here too, because it is the document it touches: opening
   a pad writes its name as a line under the cursor - the title where the operator
   is looking, and the place the entry will land - and picking a slot writes the
   entry's rows there. */

/* The same limits the card's files are written under, so a program moves between
   nc and nc2 unchanged (`nc.h`'s numbers). */
#ifndef NC2_MAX_LINES
#define NC2_MAX_LINES 256
#endif
#ifndef NC2_MAX_LINE_LEN
#define NC2_MAX_LINE_LEN 96
#endif
#define NC2_WRAP_LINE_LEN 46
#define NC2_MAX_VISIBLE_LINES 17
#define NC2_PATH_MAX 96

/* The fields one line may be cut into, and the longest value typed at one. */
#define NC2_MAX_FIELDS 24
#define NC2_VALUE_MAX 16

/* A field: the letter that starts it, where the letter is, and where its value
   is. A letter with no number is a field with an empty value - which is what a
   file writes when it wants the operator to type the number (`T`, ` Q`). */
typedef struct {
    char letter;
    size_t start;               /* the letter itself */
    size_t value;               /* first character of the value, == end if none */
    size_t end;                 /* one past the value */
} nc2_field_t;

typedef struct {
    char lines[NC2_MAX_LINES][NC2_MAX_LINE_LEN];
    size_t line_count;
    size_t cursor;              /* the line being edited */
    int field;                  /* the picked field of it, or -1 */
    char draft[NC2_VALUE_MAX];  /* what has been typed at the picked field */
    bool drafting;              /* the draft has replaced the value */
    char path[NC2_PATH_MAX];
    bool dirty;
    /* The pad: the line its name stands on (until the first entry lands there),
       where the cursor was when it opened, whether it is up, and the line the
       *next* entry goes to - the cursor sits on the first row written (that is
       the one to edit), while the next press belongs under the last row. */
    size_t pad_label;
    size_t pad_origin;
    size_t pad_next;
    bool pad_open;
} nc2_document_t;

/* The keys the editor acts on. The machine's `B`/`C` are one key with two
   meanings - step through the fields, or sign and point while a value is
   picked - which is how a keypad with no `-` and no `.` still types a negative
   decimal number. */
typedef enum {
    NC2_KEY_NONE = 0,
    NC2_KEY_UP,                 /* B: the line above, or the sign */
    NC2_KEY_DOWN,               /* C: the line below, or the point */
    NC2_KEY_NEXT,               /* D: the next field, or accept the last one */
    NC2_KEY_ACCEPT,             /* `#` */
    NC2_KEY_DELETE,             /* `*` */
    NC2_KEY_DIGIT               /* `0`-`9`, the character in `ch` */
} nc2_key_t;

void nc2_document_init(nc2_document_t *doc);

/* The fields of a line, in order, up to `max`: false when `out` is full is not
   an error, the count returned is simply smaller than the line. Text inside
   `(...)` is a comment and is not a field. */
int nc2_fields(const char *line, nc2_field_t *out, int max);

/* The fields of the line the cursor is on. */
int nc2_document_fields(const nc2_document_t *doc, nc2_field_t *out, int max);

bool nc2_insert_line(nc2_document_t *doc, size_t at, const char *text);
bool nc2_set_line(nc2_document_t *doc, size_t at, const char *text);
bool nc2_delete_line(nc2_document_t *doc, size_t at);
void nc2_cursor_move(nc2_document_t *doc, int delta);

/* The picked field, and what the keys do with it. `nc2_key` answers false when
   the key is not the editor's, so the screen can offer it to the pad, the file
   list or the modes. */
bool nc2_pick_field(nc2_document_t *doc, int index);
void nc2_unpick(nc2_document_t *doc);
bool nc2_key(nc2_document_t *doc, nc2_key_t key, char ch);

/* The document as the cycle scan sees it: g7x owns the block rules and takes
   the lines, not the document, so this is the one place the two meet. */
g7x_doc_t nc2_document_g7x(const nc2_document_t *doc);

/* The pad: its name as a line under the cursor while the operator is choosing,
   and the entry's rows where that line stands. The pad *stays* - pressing a
   second slot writes under the first - which is what makes a profile walk one
   press per point; `nc2_pad_close` is how it ends, and it takes the name line
   back only when nothing was written. */
bool nc2_pad_open(nc2_document_t *doc, const char *name);
bool nc2_pad_write(nc2_document_t *doc, const char *rows);
void nc2_pad_close(nc2_document_t *doc);
bool nc2_pad_active(const nc2_document_t *doc);

#endif
