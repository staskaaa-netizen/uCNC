#ifndef LATHE_COMP_H
#define LATHE_COMP_H

#include <stdbool.h>
#include <stddef.h>

/* Lathe cutter radius compensation: the geometry of a contour shifted by the
   tool's nose radius, and the checks that say when the tool cannot cut what the
   program asks for. It is a *path* transformation - by the time motion reaches
   the planner the compensated geometry is final - and it belongs here rather
   than in G7x, which consumes the answer: `docs/lathe-cutter-comp.md` says so,
   and G7x's own rule is that nothing in it implements compensation.

   The contour is in **radius space in X**, which is the space G7x's region is
   built in: a program's `X` is a diameter, and the conversion to a diameter is
   the sender's business when it writes the line out. A nose radius is a real
   length, so it is added here directly and never halved.

   What this module owns today is the geometry of one contour: the parallel
   offset of its lines and arcs, the corner intersections, and the check that an
   arc the offset turns inside of is still cuttable (`R > r`). Where the radius
   comes from (a tool table or a setting), the lead-in/lead-out, and the
   gouging checks are the caller's and the later stages' - see the doc. */

#ifndef LATHE_COMP_MAX_ELEMENTS
#define LATHE_COMP_MAX_ELEMENTS 64
#endif

typedef enum {
    LATHE_COMP_OK = 0,
    LATHE_COMP_TOO_MANY,        /* more elements than the output can hold */
    LATHE_COMP_ARC_TOO_SMALL,   /* an arc the offset turns inside of, R <= r */
    LATHE_COMP_NO_TURN          /* a corner the offset cannot reach */
} lathe_comp_result_t;

typedef enum {
    LATHE_COMP_LEFT = 0,        /* G41: the tool on the left of the path */
    LATHE_COMP_RIGHT            /* G42: the tool on the right */
} lathe_comp_side_t;

/* One element of the contour, as the caller has it: where it ends, and - for an
   arc - its radius and which way it turns (clockwise seen from the usual +Y, as
   G2/G3 are). `r == 0` is a straight move. */
typedef struct {
    float x;
    float z;
    float r;
    bool arc;
    bool cw;
    /* The arc's centre, as an offset from the element's own start (`I`/`K`), for
       a caller that has it. Without it an arc is placed from `r` and `cw`. */
    bool has_centre;
    float i;
    float k;
} lathe_comp_element_t;

typedef struct {
    float x;                    /* where the contour starts */
    float z;
    lathe_comp_element_t elements[LATHE_COMP_MAX_ELEMENTS];
    int count;
} lathe_comp_contour_t;

/* Shift the whole contour by `radius` to one side of the path it describes.

   A line moves along its own normal, and two adjacent lines meet on their
   intersection (an outside corner is extended, an inside one trimmed). An arc
   keeps its centre and the offset turns it inside or outside of it: an arc that
   is cut from the *inside* loses the radius (`R - r`, the case a shoulder fillet
   is) and one cut from the outside gains it. An arc the offset turns inside of
   that is no bigger than the tool (`R <= r`) is refused - the tool cannot pass
   through the corner it would leave.

   `out` may be the same contour as `in`. */
lathe_comp_result_t lathe_comp_offset(const lathe_comp_contour_t *in,
                                      float radius,
                                      lathe_comp_side_t side,
                                      lathe_comp_contour_t *out);

const char *lathe_comp_result_text(lathe_comp_result_t result);

#endif
