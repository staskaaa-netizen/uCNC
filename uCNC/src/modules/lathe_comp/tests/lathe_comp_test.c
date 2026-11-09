/* The offset geometry, on contours a lathe profile is made of: straight moves
   with a sharp corner, a fillet arc, and the arc the tool cannot fit in. X is in
   radius space, the same space G7x's region is built in. */
#include "../lathe_comp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;

static void near(float got, float want, const char *what)
{
    if (fabsf(got - want) > 0.002f) {
        printf("FAIL %s: got %.3f, wanted %.3f\n", what, (double)got,
               (double)want);
        fails++;
    }
}

static void element(lathe_comp_element_t *el, float x, float z)
{
    memset(el, 0, sizeof(*el));
    el->x = x;
    el->z = z;
}

/* A profile turned along Z and then faced out along X: the tool on the left of
   the path (G41) moves both lines out by its radius, and the two meet on the
   corner's own intersection. */
static void test_lines_and_corner(void)
{
    lathe_comp_contour_t in;
    lathe_comp_contour_t out;

    memset(&in, 0, sizeof(in));
    in.x = 10.0f;
    in.z = 0.0f;
    in.count = 2;
    element(&in.elements[0], 10.0f, -20.0f);
    element(&in.elements[1], 20.0f, -20.0f);
    if (lathe_comp_offset(&in, 1.0f, LATHE_COMP_LEFT, &out) != LATHE_COMP_OK) {
        puts("FAIL the straight contour was refused");
        fails++;
        return;
    }
    near(out.x, 11.0f, "the start is one radius to the left");
    near(out.z, 0.0f, "the start keeps its Z");
    near(out.elements[0].x, 11.0f, "the first line's end");
    near(out.elements[0].z, -19.0f, "the corner is on the intersection");
    near(out.elements[1].x, 20.0f, "the face keeps its X");
    near(out.elements[1].z, -19.0f, "the face moved by the radius");
    if (out.elements[0].arc || out.elements[1].arc) {
        puts("FAIL a straight contour came back with an arc");
        fails++;
    }
}

/* A fillet the tool runs inside of: the arc keeps its centre and *loses* the
   radius, which is the case the bench's `R > r` check is about. */
static void test_fillet_loses_the_radius(void)
{
    lathe_comp_contour_t in;
    lathe_comp_contour_t out;

    memset(&in, 0, sizeof(in));
    in.x = 10.0f;
    in.z = -10.0f;
    in.count = 1;
    element(&in.elements[0], 12.0f, -12.0f);
    in.elements[0].arc = true;
    in.elements[0].cw = true;
    in.elements[0].has_centre = true;
    in.elements[0].i = 2.0f;            /* centre at (12, -10) */
    in.elements[0].k = 0.0f;
    if (lathe_comp_offset(&in, 0.5f, LATHE_COMP_LEFT, &out) != LATHE_COMP_OK) {
        puts("FAIL the fillet was refused");
        fails++;
        return;
    }
    if (!out.elements[0].arc) {
        puts("FAIL the fillet came back as a line");
        fails++;
        return;
    }
    near(out.elements[0].r, 1.5f, "the arc lost the tool's radius");
    near(out.x, 10.5f, "the arc starts on its own radius");
    near(out.z, -10.0f, "the arc starts on its own radius");
    near(out.elements[0].x, 12.0f, "the arc ends on its own radius");
    near(out.elements[0].z, -11.5f, "the arc ends on its own radius");
    /* The other side of the arc gains the radius instead. */
    if (lathe_comp_offset(&in, 0.5f, LATHE_COMP_RIGHT, &out) != LATHE_COMP_OK) {
        puts("FAIL the fillet was refused from the other side");
        fails++;
        return;
    }
    near(out.elements[0].r, 2.5f, "the arc gained the tool's radius outward");
}

/* The tool cannot pass through an arc it is as big as: `R > r` is the check. */
static void test_arc_too_small(void)
{
    lathe_comp_contour_t in;
    lathe_comp_contour_t out;

    memset(&in, 0, sizeof(in));
    in.x = 10.0f;
    in.z = -10.0f;
    in.count = 1;
    element(&in.elements[0], 12.0f, -12.0f);
    in.elements[0].arc = true;
    in.elements[0].cw = true;
    in.elements[0].has_centre = true;
    in.elements[0].i = 2.0f;
    in.elements[0].k = 0.0f;
    if (lathe_comp_offset(&in, 2.0f, LATHE_COMP_LEFT, &out) !=
        LATHE_COMP_ARC_TOO_SMALL) {
        puts("FAIL an arc as big as the tool was accepted");
        fails++;
    }
    if (lathe_comp_offset(&in, 2.5f, LATHE_COMP_LEFT, &out) !=
        LATHE_COMP_ARC_TOO_SMALL) {
        puts("FAIL an arc smaller than the tool was accepted");
        fails++;
    }
}

/* A corner the offset cannot reach is refused, not guessed: an arc that runs
   into a line at an angle leaves a gap the simple join has no answer for. */
static void test_sharp_arc_join_refused(void)
{
    lathe_comp_contour_t in;
    lathe_comp_contour_t out;

    memset(&in, 0, sizeof(in));
    in.x = 10.0f;
    in.z = -10.0f;
    in.count = 2;
    element(&in.elements[0], 12.0f, -12.0f);
    in.elements[0].arc = true;
    in.elements[0].cw = true;
    in.elements[0].has_centre = true;
    in.elements[0].i = 2.0f;
    in.elements[0].k = 0.0f;
    element(&in.elements[1], 12.0f, -20.0f);    /* square into the arc's end */
    if (lathe_comp_offset(&in, 0.5f, LATHE_COMP_LEFT, &out) !=
        LATHE_COMP_NO_TURN) {
        puts("FAIL a corner the offset cannot reach was accepted");
        fails++;
    }
}

int main(void)
{
    test_lines_and_corner();
    test_fillet_loses_the_radius();
    test_arc_too_small();
    test_sharp_arc_join_refused();
    if (fails) {
        printf("lathe_comp: FAILURES %d\n", fails);
        return 1;
    }
    puts("lathe_comp: ALL PASS");
    return 0;
}
