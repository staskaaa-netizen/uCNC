#ifndef NCC_H
#define NCC_H

#include <stdbool.h>
#include <stddef.h>

#include "../g7x/g7x_blocks.h"
#include "../nc_sender/nc_sender.h"

/* ncc: the program, and the value editor that walks it.

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
   nc and ncc unchanged (`nc.h`'s numbers). */
#ifndef NCC_MAX_LINES
#define NCC_MAX_LINES 256
#endif
#ifndef NCC_MAX_LINE_LEN
#define NCC_MAX_LINE_LEN 96
#endif
#define NCC_WRAP_LINE_LEN 46
#define NCC_MAX_VISIBLE_LINES 17
#define NCC_PATH_MAX 96

/* The fields one line may be cut into, and the longest value typed at one. */
#define NCC_MAX_FIELDS NC_SENDER_MAX_FIELDS
#define NCC_VALUE_MAX 16

/* A field: the letter that starts it, where the letter is, and where its value
   is. A letter with no number is a field with an empty value - which is what a
   file writes when it wants the operator to type the number (`T`, ` Q`). */
typedef nc_sender_field_t ncc_field_t;

typedef struct {
    char lines[NCC_MAX_LINES][NCC_MAX_LINE_LEN];
    size_t line_count;
    size_t cursor;              /* the line being edited */
    int field;                  /* the picked field of it, or -1 */
    char draft[NCC_VALUE_MAX];  /* what has been typed at the picked field */
    bool drafting;              /* the draft has replaced the value */
    char path[NCC_PATH_MAX];
    bool dirty;
    /* The pad: the line its name stands on (until the first entry lands there),
       where the cursor was when it opened, whether it is up, and the line the
       *next* entry goes to - the cursor sits on the first row written (that is
       the one to edit), while the next press belongs under the last row. */
    size_t pad_label;
    size_t pad_origin;
    size_t pad_next;
    bool pad_open;
} ncc_document_t;

/* The keys the editor acts on. The machine's `B`/`C` are one key with two
   meanings - step through the fields, or sign and point while a value is
   picked - which is how a keypad with no `-` and no `.` still types a negative
   decimal number. */
typedef enum {
    NCC_KEY_NONE = 0,
    NCC_KEY_UP,                 /* B: the line above, or the sign */
    NCC_KEY_DOWN,               /* C: the line below, or the point */
    NCC_KEY_NEXT,               /* D: the next field, or accept the last one */
    NCC_KEY_ACCEPT,             /* `#` */
    NCC_KEY_DELETE,             /* `*` */
    NCC_KEY_DIGIT               /* `0`-`9`, the character in `ch` */
} ncc_key_t;

void ncc_document_init(ncc_document_t *doc);

/* The fields of a line, in order, up to `max`: false when `out` is full is not
   an error, the count returned is simply smaller than the line. Text inside
   `(...)` is a comment and is not a field. */
int ncc_fields(const char *line, ncc_field_t *out, int max);

/* The fields of the line the cursor is on. */
int ncc_document_fields(const ncc_document_t *doc, ncc_field_t *out, int max);

bool ncc_insert_line(ncc_document_t *doc, size_t at, const char *text);
bool ncc_set_line(ncc_document_t *doc, size_t at, const char *text);
bool ncc_delete_line(ncc_document_t *doc, size_t at);
void ncc_cursor_move(ncc_document_t *doc, int delta);

/* The picked field, and what the keys do with it. `ncc_key` answers false when
   the key is not the editor's, so the screen can offer it to the pad, the file
   list or the modes. While a field is picked the keys are the value's - `B`/`C`
   are its sign and its point - which is why a row that carries nothing to type
   (`G41`) must not leave one picked (`ncc_pick_landed()` in `ncc.c`). */
bool ncc_pick_field(ncc_document_t *doc, int index);
void ncc_unpick(ncc_document_t *doc);
bool ncc_key(ncc_document_t *doc, ncc_key_t key, char ch);

/* The word of the cursor's row one of the TOOLS pad's keys stands for: the key's
   number is the word's number (`2` the row's `R`, `3` its `O`), and the last key
   walks any words the row carries past the ninth. -1 when the key names
   nothing. The pad's labels and its press both ask this. */
int ncc_pad_word_index(const ncc_document_t *doc, char key);

/* A line that is nothing but a `G` word the vocabulary knows, now that the
   operator has finished typing it, becomes that code's own line - the words it
   takes, each at zero - and its first value is left picked, so the operator
   walks into the cycle instead of being left with a bare `G71`. nc did this when
   its G field was accepted; nc2 dropped the table, and the word stayed bare.

   "Finished typing" is the whole of the trigger: the accept key, or `D` stepping
   off a word something was typed into. Travelling over a line must not rewrite
   or pick it.

   Answers the code it named (whether or not there was a line to write) and 0
   when there is nothing to say: a line with anything else on it, a code the
   table does not hold, or a field still being typed. */
int ncc_gcode_resolve(ncc_document_t *doc);

/* The document as the cycle scan sees it: g7x owns the block rules and takes
   the lines, not the document, so this is the one place the two meet. */
g7x_doc_t ncc_document_g7x(const ncc_document_t *doc);

/* The pad: its name as a line under the cursor while the operator is choosing,
   and the entry's rows where that line stands. **A press that writes ends the
   pad** - nc's own rule, its menu cancelled itself before every entry - so the
   keys go back to the root and the row that landed keeps its value picked; the
   walk that stays up for every point is the path builder's own (`ncc_contour_*`).
   `ncc_pad_close` is how the operator ends it, and it takes the name line back
   only when nothing was written.

   A row that starts with a space joins the line the press began on instead of
   standing on one of its own: at the end of it, or at its front for `N`. That is
   how `F`, `C` and `R` finish a line the operator is already on, and how `N`
   numbers one. */
bool ncc_pad_open(ncc_document_t *doc, const char *name);
bool ncc_pad_write(ncc_document_t *doc, const char *rows);
void ncc_pad_close(ncc_document_t *doc);
bool ncc_pad_active(const ncc_document_t *doc);

/* The path builder - G7X's `7`, the address `47`.

   The pad becomes the nine directions and **stays** until `5`, and each press
   writes one `G1` row below the cursor: the axes that move at the step, the
   others carried over from the point the row above reaches. It is the "same
   result as before" nc's contour pad gave - a profile one press per point, a
   chamfer one press, and a contour outside a cycle just as easy - and it is a
   key that *does* something rather than one that writes a row of its own, which
   is why it is not an entry the card can hold (nc's own reason, `nc_menu.c`).

   The row the press writes is an ordinary program row and keeps its value
   picked, so the digits type the real number over the step's starting point;
   `D` takes a corner's second word and closes the point, `*` takes the point
   back, `#` steps the distance, and `5` ends it. */
bool ncc_contour_active(void);
bool ncc_contour_begin(ncc_document_t *doc);
void ncc_contour_leave(void);
/* One press while the builder is up. False when the key is not the builder's -
   a digit with the point's value picked belongs to the editor's field flow. */
bool ncc_contour_key(ncc_document_t *doc, char key);
/* The cell's word on the builder's pad, in key order 1..9. */
const char *ncc_contour_label(char key);
/* The distance one press moves, as the pad's band shows it. */
float ncc_contour_step(void);

#endif
