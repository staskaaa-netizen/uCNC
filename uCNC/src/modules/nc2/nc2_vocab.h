#ifndef NC2_VOCAB_H
#define NC2_VOCAB_H

#include "nc2.h"

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
const char *nc2_vocab_label(const char *line, const nc2_field_t *field);

#endif
