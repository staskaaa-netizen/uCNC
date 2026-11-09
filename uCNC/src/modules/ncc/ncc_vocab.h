#ifndef NCC_VOCAB_H
#define NCC_VOCAB_H

#include "ncc.h"

/* What a word means, for the legend the editor shows above the line the cursor
   is on.

   The editor cuts a line into fields at its letters and nothing else (that is
   the whole of the rule), so a word's *meaning* is not something the editor can
   know - and the operator typing `U3` into a `G71` header has one meaning in
   mind while the same `U` on a plain `G1` row has another. The meanings are
   data here, the same table nc kept (`nc_vocab.c`), so a word the panel's own
   entries write is named for what it is instead of "NC word". */

/* The label of one field of a line: the G code the line carries gives the
   context, and a word outside every known code falls back to the axis letter's
   own name. */
const char *ncc_vocab_label(const char *line, const ncc_field_t *field);

/* The G codes the panel knows - what the code is called, the words it takes, and
   the line a key writes for it. nc kept this table (`nc_vocab.c`) and nc2
   dropped it, which is why a typed `G71` stayed a bare `G71` with nothing to
   walk into; `ncc_gcode_resolve()` is what uses it now. The line is the words in
   the order they are written, each at zero: the operator types the code and the
   fields are there to fill in. NULL for a code the table does not hold, and NULL
   from the template lookup for a code that takes no words (a `G21` is a whole
   line by itself). */
const char *ncc_vocab_gcode_name(int gcode);
const char *ncc_vocab_gcode_parameters(int gcode);
const char *ncc_vocab_gcode_template(int gcode);

#endif
