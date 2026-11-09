#include "lathe_comp.h"

#include <math.h>
#include <string.h>

/* One support: the element's own geometry after it has been moved. A line is
   two points; an arc is a centre and a radius, and the two ends are on it. */
typedef struct {
    float x;
    float z;
} v2_t;

typedef struct {
    bool arc;
    bool cw;
    v2_t a;                     /* where it starts, on the offset geometry */
    v2_t b;                     /* where it ends */
    v2_t c;                     /* the arc's centre (an arc only) */
    float r;                    /* the arc's radius (an arc only) */
} support_t;

static v2_t v2(float x, float z)
{
    v2_t p;

    p.x = x;
    p.z = z;
    return p;
}

static v2_t v2_add(v2_t a, v2_t b) { return v2(a.x + b.x, a.z + b.z); }
static v2_t v2_sub(v2_t a, v2_t b) { return v2(a.x - b.x, a.z - b.z); }
static v2_t v2_mul(v2_t a, float s) { return v2(a.x * s, a.z * s); }
static float v2_len(v2_t a) { return sqrtf((a.x * a.x) + (a.z * a.z)); }
static float v2_cross(v2_t a, v2_t b) { return (a.x * b.z) - (a.z * b.x); }

static v2_t v2_unit(v2_t a)
{
    float l = v2_len(a);

    return l > 1e-6f ? v2_mul(a, 1.0f / l) : v2(0.0f, 0.0f);
}

static float v2_dist(v2_t a, v2_t b) { return v2_len(v2_sub(a, b)); }

/* The normal to the left of a direction, in G7x's plane and sense (`g7x_v2_*`,
   and its note that G18's Z/X orientation reverses CW against an X/Z drawing),
   so an element this module hands back is read the way G7x reads the contour it
   was given. */
static v2_t left_normal(v2_t a, v2_t b)
{
    v2_t d = v2_unit(v2_sub(b, a));

    return v2(-d.z, d.x);
}

/* The centre of an arc from `a` to `b`. G7x's own convention is followed
   exactly: the centre of a `cw` arc lies to the *left* of the chord, and a
   negative radius is the other way. */
static bool arc_centre(const lathe_comp_element_t *el,
                       v2_t a,
                       v2_t b,
                       v2_t *centre,
                       float *radius)
{
    if (el->has_centre) {
        float r;

        *centre = v2(a.x + el->i, a.z + el->k);
        r = v2_dist(*centre, a);
        if (r <= 1e-4f || fabsf(v2_dist(*centre, b) - r) > 2e-3f) {
            return false;
        }
        *radius = r;
        return true;
    }
    {
        float r = fabsf(el->r);
        v2_t mid = v2_mul(v2_add(a, b), 0.5f);
        v2_t d = v2_sub(b, a);
        float chord = v2_len(d);
        float h;
        float sign;

        if (r <= 1e-4f || chord <= 1e-4f || r < (chord * 0.5f)) {
            return false;
        }
        h = sqrtf((r * r) - ((chord * 0.5f) * (chord * 0.5f)));
        sign = el->cw ? 1.0f : -1.0f;
        if (el->r < 0.0f) {
            sign = -sign;
        }
        *centre = v2(mid.x + (sign * (-d.z / chord) * h),
                     mid.z + (sign * (d.x / chord) * h));
        *radius = r;
        return true;
    }
}

/* Where the offset geometry of one element begins and ends, before any joining.
   A line moves along its normal; an arc keeps its centre and its radius grows or
   shrinks by the tool's, depending on which side of it the offset lands. */
static lathe_comp_result_t offset_support(const lathe_comp_element_t *el,
                                          v2_t a,
                                          v2_t b,
                                          float radius,
                                          lathe_comp_side_t side,
                                          support_t *out)
{
    float sign = (side == LATHE_COMP_LEFT) ? 1.0f : -1.0f;

    memset(out, 0, sizeof(*out));
    out->arc = el->arc;
    out->cw = el->cw;
    if (!el->arc) {
        v2_t n = v2_mul(left_normal(a, b), sign * radius);

        out->a = v2_add(a, n);
        out->b = v2_add(b, n);
        return LATHE_COMP_OK;
    }
    {
        v2_t c;
        float r;
        float r2;
        bool inside;

        if (!arc_centre(el, a, b, &c, &r)) {
            return LATHE_COMP_NO_TURN;
        }
        /* The tool is on the centre's side of the arc when the offset and the
           turn agree: that arc loses the radius, which is where `R > r` comes
           from (a shoulder fillet is the case). The other way it gains it. */
        inside = ((side == LATHE_COMP_LEFT) == el->cw);
        r2 = inside ? (r - radius) : (r + radius);
        if (r2 <= 1e-4f) {
            return LATHE_COMP_ARC_TOO_SMALL;
        }
        out->c = c;
        out->r = r2;
        out->a = v2_add(c, v2_mul(v2_unit(v2_sub(a, c)), r2));
        out->b = v2_add(c, v2_mul(v2_unit(v2_sub(b, c)), r2));
        return LATHE_COMP_OK;
    }
}

/* The point two supports meet on. Two lines meet on their intersection, and a
   tangent join - an arc against the line or arc it runs into - meets on the same
   point from both sides, which is what a fillet or a round in the profile is.
   Anything else is a corner the offset cannot reach: refuse it rather than cut
   something the operator did not draw. */
static bool join_point(const support_t *first, const support_t *second, v2_t *out)
{
    if (!first->arc && !second->arc) {
        v2_t d1 = v2_sub(first->b, first->a);
        v2_t d2 = v2_sub(second->b, second->a);
        float den = v2_cross(d1, d2);

        if (fabsf(den) < 1e-6f) {
            *out = second->a;       /* the same direction: the shared point */
            return true;
        }
        {
            float t = v2_cross(v2_sub(second->a, first->a), d2) / den;

            *out = v2_add(first->a, v2_mul(d1, t));
            return true;
        }
    }
    if (v2_dist(first->b, second->a) <= 2e-3f) {
        *out = v2_mul(v2_add(first->b, second->a), 0.5f);
        return true;
    }
    return false;
}

lathe_comp_result_t lathe_comp_offset(const lathe_comp_contour_t *in,
                                      float radius,
                                      lathe_comp_side_t side,
                                      lathe_comp_contour_t *out)
{
    support_t support[LATHE_COMP_MAX_ELEMENTS];
    lathe_comp_contour_t shifted;
    v2_t point;
    lathe_comp_result_t result;
    int i;

    if (!in || !out || in->count < 0 || radius < 0.0f) {
        return LATHE_COMP_NO_TURN;
    }
    if (in->count > LATHE_COMP_MAX_ELEMENTS) {
        return LATHE_COMP_TOO_MANY;
    }
    memset(&shifted, 0, sizeof(shifted));
    shifted.count = in->count;
    if (in->count <= 0) {
        *out = shifted;
        return LATHE_COMP_OK;
    }
    point = v2(in->x, in->z);
    for (i = 0; i < in->count; i++) {
        const lathe_comp_element_t *el = &in->elements[i];

        result = offset_support(el, point, v2(el->x, el->z), radius, side,
                                &support[i]);
        if (result != LATHE_COMP_OK) {
            return result;
        }
        point = v2(el->x, el->z);
    }
    /* Join each neighbour pair, and let the joins trim or extend the two
       elements they belong to. */
    for (i = 0; i + 1 < in->count; i++) {
        v2_t join;

        if (!join_point(&support[i], &support[i + 1], &join)) {
            return LATHE_COMP_NO_TURN;
        }
        support[i].b = join;
        support[i + 1].a = join;
    }
    shifted.x = support[0].a.x;
    shifted.z = support[0].a.z;
    for (i = 0; i < in->count; i++) {
        lathe_comp_element_t *el = &shifted.elements[i];

        memset(el, 0, sizeof(*el));
        el->x = support[i].b.x;
        el->z = support[i].b.z;
        el->arc = support[i].arc;
        el->cw = support[i].cw;
        if (support[i].arc) {
            el->r = support[i].r;
            el->has_centre = true;
            el->i = support[i].c.x - support[i].a.x;
            el->k = support[i].c.z - support[i].a.z;
        }
    }
    *out = shifted;
    return LATHE_COMP_OK;
}

const char *lathe_comp_result_text(lathe_comp_result_t result)
{
    switch (result) {
    case LATHE_COMP_OK: return "ok";
    case LATHE_COMP_TOO_MANY: return "too many contour elements";
    case LATHE_COMP_ARC_TOO_SMALL: return "arc too small for the tool";
    case LATHE_COMP_NO_TURN: return "corner the offset cannot reach";
    default: return "unknown";
    }
}
