/* G71/G72 generator and parser module.
 * This module is intentionally self-contained: NC/LeanCam screens may use the
 * stream API for preview, but G71/G72 execution must work without any UI module
 * loaded. Parser integration owns the modal region, suppresses source contour
 * rows, and feeds generated rough/finish motion back through the live parser.
 */
#ifndef G7X_HOST_TEST
#include "../../cnc.h"
#include "../../module.h"
#include "../g7_g8/parser_g7_g8.h"
#endif
#include "g7x.h"
#include "g7x_contour.h"
#include "g7x_source.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ENABLE_PARSER_MODULES) && !defined(G7X_HOST_TEST)
#define G7X_G70_EXTENDED_CODE EXTENDED_MCODE(700)
#define G7X_EXTENDED_CODE EXTENDED_MCODE(710)
#define G7X_G76_EXTENDED_CODE EXTENDED_MCODE(760)
#define G7X_G33_MOTION_CODE 33

static bool g7x_parser_region_active;
static bool g7x_parser_runner_active;
#if G7X_ENABLE_G76 && defined(G33_ENCODER)
static bool g7x_parser_thread_runner_active;
#endif
static g7x_cycle_t g7x_parser_pending_cycle;
static g7x_stream_t g7x_parser_stream;
/* P/Q numbered range: armed waits for the N(P) block, collecting follows it
   until the N(Q) block closes the region. */
static bool g7x_parser_pq_armed;
static bool g7x_parser_pq_collecting;
static uint32_t g7x_parser_pq_p;
static uint32_t g7x_parser_pq_q;
static uint32_t g7x_parser_pq_last;
static g7x_history_t g7x_parser_history;
/* The last numbered range this run collected, kept as it was collected (before
   the corner expansion `prepare` does) so a later `G70 P Q` can re-run it as
   the finish cut. A run that starts at the G70 line never collected the range:
   the replay then refuses instead of guessing, and the DRO is what says so. */
static g7x_contour_region_t g7x_parser_kept_region;
static uint32_t g7x_parser_kept_p;
static uint32_t g7x_parser_kept_q;
static bool g7x_parser_kept_valid;
/* Why the line being parsed was refused, for the caller that shows errors on a
   screen rather than a terminal. Set on the way out of a refusal, cleared at
   the start of the next line's execution so it can never describe an older
   line. */
static char g7x_parser_refusal[48];
/* The copy handed to the caller that took it: it has to outlive the clear
   inside the take. */
static char g7x_parser_taken_refusal[48];
/* Fanuc two-line header: the second G71/G72 block completes the open header
   and its U/W words are finish allowances, not the depth of cut. */
static bool g7x_parser_continuation;
static float g7x_parser_pending_x_allow;
static bool g7x_parser_pending_x_allow_set;
static float g7x_parser_pending_z_allow;
static bool g7x_parser_pending_z_allow_set;
#if G7X_ENABLE_G76 && defined(G33_ENCODER)
static g7x_thread_stream_t g7x_parser_thread_stream;
#endif
static parser_state_t g7x_parser_runner_state;
static float g7x_parser_pending_doc;
static bool g7x_parser_pending_doc_set;
static g7x_corner_kind_t g7x_parser_pending_corner_kind;
static float g7x_parser_pending_corner_amount;

static void g7x_parser_set_refusal(const char *why)
{
    snprintf(g7x_parser_refusal, sizeof(g7x_parser_refusal), "%s",
             why ? why : "");
}

const char *g7x_take_refusal_text(void)
{
    static const char empty[] = "";

    if (!g7x_parser_refusal[0])
        return empty;
    /* Read once: the caller that shows it owns it now, so a later error cannot
       be explained with an older line's reason. */
    snprintf(g7x_parser_taken_refusal, sizeof(g7x_parser_taken_refusal), "%s",
             g7x_parser_refusal);
    g7x_parser_refusal[0] = '\0';
    return g7x_parser_taken_refusal;
}

bool g7x_parse(void *args);
bool g7x_exec_modifier(void *args);
bool g7x_reset(void *args);
static uint8_t g7x_parser_run_pending(parser_state_t *state);
bool g7x_parse_error(void *args);
bool g7x_execute_pending(void *args);

CREATE_EVENT_LISTENER(gcode_parse, g7x_parse);
CREATE_EVENT_LISTENER(gcode_exec_modifier, g7x_exec_modifier);
CREATE_EVENT_LISTENER(parser_reset, g7x_reset);
CREATE_EVENT_LISTENER(cnc_parse_cmd_error, g7x_parse_error);
CREATE_EVENT_LISTENER(gcode_execute_pending, g7x_execute_pending);
#else
const char *g7x_take_refusal_text(void)
{
    return "";
}
#endif

#if defined(ENABLE_PARSER_MODULES) && !defined(G7X_HOST_TEST)
static void g7x_parser_clear_state(void)
{
    g7x_parser_region_active = false;
    g7x_parser_runner_active = false;
    g7x_parser_pq_armed = false;
    g7x_parser_pq_collecting = false;
    g7x_parser_pq_p = 0u;
    g7x_parser_pq_q = 0u;
    g7x_parser_pq_last = 0u;
    g7x_parser_continuation = false;
    g7x_parser_pending_x_allow = 0.0f;
    g7x_parser_pending_x_allow_set = false;
    g7x_parser_pending_z_allow = 0.0f;
    g7x_parser_pending_z_allow_set = false;
#if G7X_ENABLE_G76 && defined(G33_ENCODER)
    g7x_parser_thread_runner_active = false;
#endif
    g7x_parser_pending_cycle = G7X_CYCLE_NONE;
    g7x_parser_pending_doc = 0.0f;
    g7x_parser_pending_doc_set = false;
    g7x_parser_pending_corner_kind = G7X_CORNER_NONE;
    g7x_parser_pending_corner_amount = 0.0f;
    g7x_stream_reset(&g7x_parser_stream);
#if G7X_ENABLE_G76 && defined(G33_ENCODER)
    g7x_thread_reset(&g7x_parser_thread_stream);
#endif
    memset(&g7x_parser_runner_state, 0, sizeof(g7x_parser_runner_state));
}
#endif

bool g7x_parser_busy(void)
{
#if defined(ENABLE_PARSER_MODULES) && !defined(G7X_HOST_TEST)
    return g7x_parser_region_active || g7x_parser_runner_active ||
           g7x_parser_pq_armed
#if G7X_ENABLE_G76 && defined(G33_ENCODER)
           || g7x_parser_thread_runner_active
#endif
           ;
#else
    return false;
#endif
}

bool g7x_parser_collecting(void)
{
#if defined(ENABLE_PARSER_MODULES) && !defined(G7X_HOST_TEST)
    return g7x_parser_region_active || g7x_parser_pq_armed;
#else
    return false;
#endif
}

const g7x_history_t *g7x_parser_numbered_history(void)
{
#if defined(ENABLE_PARSER_MODULES) && !defined(G7X_HOST_TEST)
    return &g7x_parser_history;
#else
    return NULL;
#endif
}

void g7x_parser_numbered_history_reset(void)
{
#if defined(ENABLE_PARSER_MODULES) && !defined(G7X_HOST_TEST)
    g7x_history_reset(&g7x_parser_history);
#endif
}

void g7x_parser_cancel(void)
{
#if defined(ENABLE_PARSER_MODULES) && !defined(G7X_HOST_TEST)
    g7x_parser_clear_state();
#endif
}

void g7x_modal_default(g7x_modal_t *modal)
{
    if (!modal)
        return;

    modal->units = G7X_UNITS_MM;
    modal->distance = G7X_DISTANCE_ABSOLUTE;
}

void g7x_modal_from_ucnc_modes(g7x_modal_t *modal, const uint8_t *modalgroups)
{
    if (!modal)
        return;

    g7x_modal_default(modal);
    if (!modalgroups)
        return;

    if (modalgroups[2] == 90u || modalgroups[2] == 91u)
        modal->distance = modalgroups[2] == 91u ? G7X_DISTANCE_INCREMENTAL : G7X_DISTANCE_ABSOLUTE;
    if (modalgroups[4] == 20u || modalgroups[4] == 21u)
        modal->units = modalgroups[4] == 20u ? G7X_UNITS_INCH : G7X_UNITS_MM;
}

bool g7x_cycle_profile(g7x_cycle_t cycle, g7x_cycle_profile_t *profile)
{
    g7x_cycle_profile_t p;

    memset(&p, 0, sizeof(p));
    p.cycle = cycle;

    switch (cycle)
    {
        case G7X_CYCLE_G70:
            /* A finish cut has no pass axis and no depth of cut: the profile is
               followed as programmed. The fields below are what the shared
               stream machinery asks for, not what G70 uses. */
            p.pass_axis = G7X_AXIS_X;
            p.cut_axis = G7X_AXIS_Z;
            p.contour_monotonic_axis = G7X_AXIS_Z;
            p.rough_doc_word = 'U';
            p.name = "G70";
            break;
        case G7X_CYCLE_G71:
            p.pass_axis = G7X_AXIS_X;
            p.cut_axis = G7X_AXIS_Z;
            p.contour_monotonic_axis = G7X_AXIS_Z;
            p.rough_doc_word = 'U';
            p.name = "G71";
            break;
        case G7X_CYCLE_G72:
            p.pass_axis = G7X_AXIS_Z;
            p.cut_axis = G7X_AXIS_X;
            p.contour_monotonic_axis = G7X_AXIS_X;
            p.rough_doc_word = 'W';
            p.name = "G72";
            break;
#if G7X_ENABLE_G76
        case G7X_CYCLE_G76:
            p.pass_axis = G7X_AXIS_X;
            p.cut_axis = G7X_AXIS_Z;
            p.contour_monotonic_axis = G7X_AXIS_Z;
            p.rough_doc_word = 'J';
            p.name = "G76";
            break;
#endif
        default:
            return false;
    }

    if (profile)
        *profile = p;
    return true;
}

typedef struct {
    float x;
    float z;
} g7x_v2_t;

static g7x_v2_t g7x_v2_sub(g7x_v2_t a, g7x_v2_t b)
{
    g7x_v2_t r = { a.x - b.x, a.z - b.z };
    return r;
}

static g7x_v2_t g7x_v2_add(g7x_v2_t a, g7x_v2_t b)
{
    g7x_v2_t r = { a.x + b.x, a.z + b.z };
    return r;
}

static g7x_v2_t g7x_v2_mul(g7x_v2_t a, float s)
{
    g7x_v2_t r = { a.x * s, a.z * s };
    return r;
}

static float g7x_v2_dot(g7x_v2_t a, g7x_v2_t b)
{
    return a.x * b.x + a.z * b.z;
}

static float g7x_v2_len(g7x_v2_t a)
{
    return sqrtf(g7x_v2_dot(a, a));
}

static bool g7x_v2_norm(g7x_v2_t a, g7x_v2_t *out)
{
    float len = g7x_v2_len(a);
    if (!out || len <= 0.0001f)
        return false;
    *out = g7x_v2_mul(a, 1.0f / len);
    return true;
}

static bool g7x_corner_tangents(g7x_v2_t p0,
                                g7x_v2_t p1,
                                g7x_v2_t p2,
                                float amount,
                                g7x_v2_t *t1,
                                g7x_v2_t *t2,
                                g7x_v2_t *center,
                                float *actual_amount,
                                int *ccw)
{
    g7x_v2_t a;
    g7x_v2_t b;
    float dot;
    float angle;
    float trim;
    float actual;
    float max_a;
    float max_b;

    if (amount <= 0.0001f ||
        !g7x_v2_norm(g7x_v2_sub(p0, p1), &a) ||
        !g7x_v2_norm(g7x_v2_sub(p2, p1), &b)) {
        return false;
    }

    dot = g7x_v2_dot(a, b);
    if (dot < -0.999f || dot > 0.999f)
        return false;
    angle = acosf(dot);
    trim = amount / tanf(angle * 0.5f);
    /* The room each tangent has: what is left of the incoming move and all of
       the outgoing one. A corner before this one has already taken its room out
       of `p0` - the walk is left to right and each corner *moves* the element to
       its tangent point - so two corners on one move can never overlap, and a
       corner is refused only when it truly cannot fit. Callers check the same
       limit first (`g7x_expand_corners()`), so nothing is cut down here. */
    max_a = g7x_v2_len(g7x_v2_sub(p0, p1));
    max_b = g7x_v2_len(g7x_v2_sub(p2, p1));
    if (trim > max_a || trim > max_b || trim <= 0.0001f)
        return false;

    actual = trim * tanf(angle * 0.5f);
    if (actual_amount)
        *actual_amount = actual;
    if (t1)
        *t1 = g7x_v2_add(p1, g7x_v2_mul(a, trim));
    if (t2)
        *t2 = g7x_v2_add(p1, g7x_v2_mul(b, trim));
    if (center) {
        g7x_v2_t bis;
        float bis_len;
        if (!g7x_v2_norm(g7x_v2_add(a, b), &bis))
            return false;
        bis_len = actual / sinf(angle * 0.5f);
        *center = g7x_v2_add(p1, g7x_v2_mul(bis, bis_len));
    }
    if (ccw) {
        float cross = a.x * b.z - a.z * b.x;
        *ccw = cross > 0.0f;
    }
    return true;
}

/* Editor-facing corner fit check. The generator clamps a corner amount to the
   same limit, so a UI can explain a programmed radius instead of silently
   showing a different one. */
bool g7x_corner_fit(float prev_x, float prev_z,
                    float corner_x, float corner_z,
                    float next_x, float next_z,
                    float requested,
                    float *max_amount,
                    float *limit_length)
{
    g7x_v2_t p0 = { prev_x, prev_z };
    g7x_v2_t p1 = { corner_x, corner_z };
    g7x_v2_t p2 = { next_x, next_z };
    g7x_v2_t a;
    g7x_v2_t b;
    float dot;
    float angle;
    float len_a;
    float len_b;
    float limit;
    float max_r;

    if (max_amount)
        *max_amount = 0.0f;
    if (limit_length)
        *limit_length = 0.0f;
    if (requested <= 0.0001f ||
        !g7x_v2_norm(g7x_v2_sub(p0, p1), &a) ||
        !g7x_v2_norm(g7x_v2_sub(p2, p1), &b))
        return false;
    dot = g7x_v2_dot(a, b);
    if (dot < -0.999f || dot > 0.999f)
        return false;
    angle = acosf(dot);
    len_a = g7x_v2_len(g7x_v2_sub(p0, p1));
    len_b = g7x_v2_len(g7x_v2_sub(p2, p1));
    limit = (len_a < len_b) ? len_a : len_b;
    /* The tangent fits the shorter move - no more than that, and no less: a
       round that reaches the far end of its move is a legitimate corner, and
       the incoming side is already short of whatever the corner before it
       took. */
    max_r = limit * tanf(angle * 0.5f);
    if (max_amount)
        *max_amount = max_r;
    if (limit_length)
        *limit_length = limit;
    return requested <= max_r + 0.0001f;
}

static bool g7x_expand_corner(g7x_contour_element_t *prev,
                              g7x_contour_element_t *corner,
                              g7x_contour_element_t *next,
                              g7x_contour_element_t *insert,
                              bool x_is_radius)
{
    g7x_v2_t p0;
    g7x_v2_t p1;
    g7x_v2_t p2;
    g7x_v2_t t1;
    g7x_v2_t t2;
    g7x_v2_t center;
    g7x_corner_kind_t kind;
    float amount;
    float actual_amount;
    int ccw = 0;

    if (!prev || !corner || !next || !insert ||
        corner->outgoing_kind == G7X_CORNER_NONE ||
        corner->outgoing_amount <= 0.0001f ||
        corner->kind != G7X_SEGMENT_LINE ||
        next->kind != G7X_SEGMENT_LINE) {
        return false;
    }

    kind = corner->outgoing_kind;
    amount = corner->outgoing_amount;
    p0.x = x_is_radius ? prev->d : prev->d * 0.5f;
    p0.z = prev->z;
    p1.x = x_is_radius ? corner->d : corner->d * 0.5f;
    p1.z = corner->z;
    p2.x = x_is_radius ? next->d : next->d * 0.5f;
    p2.z = next->z;

    if (!g7x_corner_tangents(p0, p1, p2, amount, &t1, &t2, &center, &actual_amount, &ccw))
        return false;

#if defined(ENABLE_PARSER_MODULES) && !defined(G7X_HOST_TEST) && defined(G7X_DEBUG_CORNERS)
    {
        float display_cx = x_is_radius ? center.x * 2.0f : center.x;
        float display_t1x = x_is_radius ? t1.x * 2.0f : t1.x;
        float display_t2x = x_is_radius ? t2.x * 2.0f : t2.x;
        if (corner->source_line == (size_t)-1) {
            proto_info("G7X corner line=? %c r%.3f t1 X%.3f Z%.3f t2 X%.3f Z%.3f center X%.3f Z%.3f",
                       kind == G7X_CORNER_RND ? 'R' : 'C',
                       actual_amount,
                       display_t1x,
                       t1.z,
                       display_t2x,
                       t2.z,
                       display_cx,
                       center.z);
        } else {
            proto_info("G7X corner line=%lu %c r%.3f t1 X%.3f Z%.3f t2 X%.3f Z%.3f center X%.3f Z%.3f",
                       (unsigned long)(corner->source_line + 1u),
                       kind == G7X_CORNER_RND ? 'R' : 'C',
                       actual_amount,
                       display_t1x,
                       t1.z,
                       display_t2x,
                       t2.z,
                       display_cx,
                       center.z);
        }
    }
#endif

    memset(insert, 0, sizeof(*insert));
    corner->d = x_is_radius ? t1.x : t1.x * 2.0f;
    corner->z = t1.z;
    corner->outgoing_kind = G7X_CORNER_NONE;
    corner->outgoing_amount = 0.0f;

    if (kind == G7X_CORNER_RND) {
        insert->kind = G7X_SEGMENT_ARC;
        insert->gcode_cw = !ccw;
        insert->cw = insert->gcode_cw;
        insert->has_center = 1;
        insert->i = center.x - t1.x;
        insert->k = center.z - t1.z;
        insert->r = actual_amount;
    } else {
        insert->kind = G7X_SEGMENT_LINE;
    }
    insert->source_line = corner->source_line;
    insert->d = x_is_radius ? t2.x : t2.x * 2.0f;
    insert->z = t2.z;
    return true;
}

/* Expand every R/C corner the contour asks for. A corner that does not fit the
   moves beside it is **refused**, not fitted: the operator wrote a number and
   the machine must cut that number or stop (bench: a programmed `R35` on a 20 mm
   move with a 15 mm step came out as `R3.375` and nothing said why - "it should
   reject it loudly"). The limit is the one `g7x_corner_fit()` reports: the
   tangent fits the shorter move, and the incoming move is measured *after* the
   corner before it - so two corners sharing a move cannot overlap. */
static g7x_result_t g7x_expand_corners(g7x_contour_region_t *region, bool x_is_radius)
{
    unsigned i;

    if (!region || region->count < 3)
        return G7X_OK;

    for (i = 1; i + 1 < region->count && region->count < G7X_MAX_CONTOUR_ELEMENTS; i++) {
        g7x_contour_element_t insert;
        g7x_contour_element_t *prev = &region->elements[i - 1];
        g7x_contour_element_t *corner = &region->elements[i];
        g7x_contour_element_t *next = &region->elements[i + 1];
        float max_amount = 0.0f;
        unsigned move;

        if (corner->outgoing_kind != G7X_CORNER_NONE &&
            corner->outgoing_amount > 0.0001f) {
            /* A straight corner leaves `max_amount` at zero - a round on a
               straight line has nothing to fit, so it is left out rather than
               refused. */
            (void)g7x_corner_fit(x_is_radius ? prev->d : prev->d * 0.5f, prev->z,
                                 x_is_radius ? corner->d : corner->d * 0.5f, corner->z,
                                 x_is_radius ? next->d : next->d * 0.5f, next->z,
                                 corner->outgoing_amount, &max_amount, 0);
            if (max_amount > 0.0001f &&
                corner->outgoing_amount > max_amount + 0.0005f) {
                return G7X_CORNER_TOO_LARGE;
            }
        }
        if (!g7x_expand_corner(prev, corner, next, &insert, x_is_radius))
            continue;

        for (move = region->count; move > i + 1; move--)
            region->elements[move] = region->elements[move - 1];
        region->elements[i + 1] = insert;
        region->count++;
        i++;
    }
    return G7X_OK;
}

void g7x_stream_reset(g7x_stream_t *stream)
{
    if (stream)
        memset(stream, 0, sizeof(*stream));
}

/* One step of the roughing pass level, stopped **on** the finish allowance.

   `step` is the direction the level moves in (negative for G71's X, the cut
   direction for G72's Z), `final_pass` the allowance boundary. A finishing
   allowance is only an allowance if the roughing leaves it: stepping past the
   boundary would hand that whole step to the automatic finish cut, which then
   takes many times what was programmed for it. The boundary is used once - the
   level has to be on the near side of it for the clamp to apply, so the pass
   that lands on it is not repeated. */
static float g7x_rough_step(float pass, float step, float final_pass)
{
    float next = pass + step;

    if (step < 0.0f) {
        if (pass > final_pass + 0.0005f && next < final_pass)
            next = final_pass;
    } else if (step > 0.0f) {
        if (pass < final_pass - 0.0005f && next > final_pass)
            next = final_pass;
    }
    return next;
}

g7x_result_t g7x_stream_begin_parsed(g7x_stream_t *stream,
                                     g7x_cycle_t cycle,
                                     float retract,
                                     float x_allow,
                                     float z_allow,
                                     float feed,
                                     float doc)
{
    g7x_cycle_profile_t profile;

    if (!stream || !g7x_cycle_profile(cycle, &profile))
        return G7X_UNSUPPORTED;

    g7x_stream_reset(stream);
    if (!isfinite(retract) || !isfinite(x_allow) || !isfinite(z_allow) ||
        !isfinite(feed) || !isfinite(doc) || retract <= 0.0f || feed <= 0.0f ||
        doc <= 0.0001f || x_allow < 0.0f || z_allow < 0.0f)
        return G7X_BAD_FIELD;
    stream->pending_source_line = (size_t)-1;
    stream->last_source_line = (size_t)-1;
    stream->region.cycle = cycle;
    stream->region.active = 1;
    stream->region.retract = retract;
    stream->region.x_allow = x_allow;
    stream->region.z_allow = z_allow;
    stream->feed = feed > 0.0001f ? feed : 120.0f;
    stream->doc = fabsf(doc);
    if (stream->doc <= 0.0001f)
        return G7X_BAD_FIELD;

    stream->active = true;
    return G7X_OK;
}

static bool g7x_arc_is_monotonic(const g7x_contour_element_t *start,
                                const g7x_contour_element_t *end);
static g7x_result_t g7x_stream_prepare(g7x_stream_t *stream, bool x_is_radius);

g7x_result_t g7x_stream_begin_finish(g7x_stream_t *stream,
                                     const g7x_contour_region_t *region,
                                     float retract,
                                     float feed)
{
    if (!stream || !region || region->count < 2u)
        return G7X_BAD_FIELD;
    if (!isfinite(retract) || retract <= 0.0f ||
        !isfinite(feed) || feed <= 0.0f)
        return G7X_BAD_FIELD;

    g7x_stream_reset(stream);
    stream->region = *region;
    stream->region.active = 1;
    stream->region.cycle = G7X_CYCLE_G70;
    /* Nothing is offset: the finish is the profile. What the roughing header's
       allowances asked for was already cut by the roughing cycle. */
    stream->region.x_allow = 0.0f;
    stream->region.z_allow = 0.0f;
    stream->region.retract = retract;
    stream->feed = feed;
    stream->doc = 1.0f;                 /* unused: G70 sets no pass level */
    stream->pending_source_line = (size_t)-1;
    stream->last_source_line = (size_t)-1;
    stream->active = true;
    return g7x_stream_prepare(stream, true);
}

static g7x_result_t g7x_stream_prepare(g7x_stream_t *stream, bool x_is_radius)
{
    unsigned i;
    int z_dir = 0;
    int x_dir = 0;
    /* A finish cut follows the profile as the program describes it: it takes no
       pass levels, so a contour that doubles back is fine - what the roughing
       has to refuse (parallel passes cannot clear a V) is not a question a
       finish pass asks. */
    bool finish_only;

    if (!stream || stream->region.count < 2)
        return G7X_BAD_FIELD;

    finish_only = stream->region.cycle == G7X_CYCLE_G70;
    {
        g7x_result_t corner = g7x_expand_corners(&stream->region, x_is_radius);

        if (corner != G7X_OK)
            return corner;
    }

    stream->start_x = stream->region.elements[0].d;
    stream->start_z = stream->region.elements[0].z;
    stream->min_x = stream->max_x = stream->start_x;
    stream->min_z = stream->max_z = stream->start_z;
    for (i = 1; i < stream->region.count; i++) {
        const g7x_contour_element_t *prev = &stream->region.elements[i - 1];
        const g7x_contour_element_t *el = &stream->region.elements[i];
        float dz = el->z - prev->z;
        float dx = el->d - prev->d;
        if (!finish_only && el->kind == G7X_SEGMENT_ARC &&
            !g7x_arc_is_monotonic(prev, el))
            return G7X_UNSUPPORTED;
        if (el->d < stream->min_x) stream->min_x = el->d;
        if (el->d > stream->max_x) stream->max_x = el->d;
        if (el->z < stream->min_z) stream->min_z = el->z;
        if (el->z > stream->max_z) stream->max_z = el->z;
        if (finish_only)
            continue;
        if (fabsf(dz) > 0.0001f) {
            int s = dz > 0.0f ? 1 : -1;
            if (z_dir && z_dir != s)
                return G7X_UNSUPPORTED;
            z_dir = s;
        }
        if (fabsf(dx) > 0.0001f) {
            int s = dx > 0.0f ? 1 : -1;
            if (x_dir && x_dir != s)
                return G7X_UNSUPPORTED;
            x_dir = s;
        }
    }

    if (finish_only) {
        /* No pass level to set and no roughing to run: the finish emission
           starts at the clearance point and follows the profile. */
        stream->dir = stream->start_z > ((stream->min_z + stream->max_z) * 0.5f)
                          ? -1 : 1;
        stream->started = false;
        stream->finish = true;
        stream->stage = 0;
        stream->finish_i = 0;
        stream->have_last = false;
        stream->pass_approach = true;
        return G7X_OK;
    }

    if ((stream->region.cycle == G7X_CYCLE_G71 && !z_dir) ||
        (stream->region.cycle == G7X_CYCLE_G72 && !x_dir))
        return G7X_UNSUPPORTED;

    stream->dir = stream->start_z > ((stream->min_z + stream->max_z) * 0.5f) ? -1 : 1;
    if (stream->region.cycle == G7X_CYCLE_G72) {
        stream->final_pass = stream->dir < 0 ?
                             stream->min_z + stream->region.z_allow :
                             stream->max_z - stream->region.z_allow;
        stream->pass = g7x_rough_step(stream->start_z,
                                      (float)stream->dir * stream->doc,
                                      stream->final_pass);
    } else {
        stream->final_pass = stream->min_x + stream->region.x_allow;
        stream->pass = g7x_rough_step(stream->max_x,
                                      -stream->doc,
                                      stream->final_pass);
        stream->dir = stream->start_z > stream->region.elements[stream->region.count - 1].z ? -1 : 1;
    }
    stream->started = false;
    stream->finish = false;
    stream->stage = 0;
    stream->finish_i = 0;
    /* The tool's position is not known until the cycle moves it, so the first
       rapid is always emitted; and the first pass is the one that needs the
       full approach - the later ones are already clear in X. */
    stream->have_last = false;
    stream->pass_approach = true;
    return G7X_OK;
}

g7x_result_t g7x_stream_add_parsed(g7x_stream_t *stream,
                                   g7x_contour_cmd_t cmd,
                                   float x,
                                   bool has_x,
                                   float z,
                                   bool has_z,
                                   float r,
                                   bool has_r,
                                   float i,
                                   bool has_i,
                                   float k,
                                   bool has_k,
                                   g7x_corner_kind_t corner_kind,
                                   float corner_amount,
                                   float feed,
                                   bool has_feed,
                                   bool *done)
{
    g7x_contour_element_t *el;

    if (done)
        *done = false;
    if (!stream || !stream->active)
        return G7X_BAD_FIELD;

    if (cmd == G7X_CONTOUR_END) {
        if (done)
            *done = true;
        return g7x_stream_prepare(stream, true);
    }

    if (cmd != G7X_CONTOUR_RAPID &&
        cmd != G7X_CONTOUR_LINE &&
        cmd != G7X_CONTOUR_ARC_CW &&
        cmd != G7X_CONTOUR_ARC_CCW)
        return G7X_OK;
    if ((!has_x && !has_z) || stream->region.count >= G7X_MAX_CONTOUR_ELEMENTS)
        return G7X_BAD_FIELD;
    if (!has_x || !has_z) {
        if (!stream->region.count)
            return G7X_BAD_FIELD;
        const g7x_contour_element_t *prev = &stream->region.elements[stream->region.count - 1u];
        if (!has_x) x = prev->d;
        if (!has_z) z = prev->z;
    }
    if (!isfinite(x) || !isfinite(z))
        return G7X_BAD_FIELD;

    el = &stream->region.elements[stream->region.count++];
    memset(el, 0, sizeof(*el));
    el->kind = cmd == G7X_CONTOUR_ARC_CW || cmd == G7X_CONTOUR_ARC_CCW ? G7X_SEGMENT_ARC : G7X_SEGMENT_LINE;
    el->cw = cmd == G7X_CONTOUR_ARC_CW;
    el->gcode_cw = el->cw;
    el->d = x;
    el->z = z;
    el->source_line = stream->pending_source_line;
    /* The first row is the profile's start point. When it was written as a
       rapid it is Fanuc's `P` block - a positioning move - so the cycle rapids
       to it and the cut starts with the row after it. A rapid *later* in the
       profile is followed as a cut instead: a cycle never puts a rapid through
       the material it is cutting. */
    if (stream->region.count == 1u && cmd == G7X_CONTOUR_RAPID)
        el->approach = true;
    if (has_feed && isfinite(feed) && feed > 0.0f) {
        el->feed = feed;
        el->has_feed = true;
    }
    if (el->kind == G7X_SEGMENT_ARC) {
        if (has_i || has_k) {
            el->has_center = 1;
            el->i = i;
            el->k = k;
        } else if (has_r) {
            el->r = r;
        } else {
            return G7X_BAD_FIELD;
        }
    } else {
        if (corner_kind != G7X_CORNER_NONE && corner_amount > 0.0001f) {
            el->outgoing_kind = corner_kind;
            el->outgoing_amount = corner_amount;
        } else if (has_r && r > 0.0001f) {
            el->outgoing_kind = G7X_CORNER_RND;
            el->outgoing_amount = r;
        }
    }

    return G7X_OK;
}

static bool g7x_arc_center(const g7x_contour_element_t *start,
                           const g7x_contour_element_t *end,
                           float *cx,
                           float *cz,
                           float *r)
{
    if (!start || !end || !cx || !cz || !r || end->kind != G7X_SEGMENT_ARC)
        return false;

    if (end->has_center) {
        *cx = start->d + end->i;
        *cz = start->z + end->k;
        *r = sqrtf(end->i * end->i + end->k * end->k);
        return *r > 0.0001f;
    }

    if (fabsf(end->r) > 0.0001f) {
        float sx = start->d;
        float sz = start->z;
        float ex = end->d;
        float ez = end->z;
        float mx = (sx + ex) * 0.5f;
        float mz = (sz + ez) * 0.5f;
        float vx = ex - sx;
        float vz = ez - sz;
        float chord = sqrtf(vx * vx + vz * vz);
        float h;
        float sign;

        if (chord <= 0.0001f || fabsf(end->r) < chord * 0.5f)
            return false;

        *r = fabsf(end->r);
        h = sqrtf((*r * *r) - ((chord * 0.5f) * (chord * 0.5f)));
        sign = end->gcode_cw ? 1.0f : -1.0f;
        if (end->r < 0.0f)
            sign = -sign;
        *cx = mx + sign * (-vz / chord) * h;
        *cz = mz + sign * (vx / chord) * h;
        return true;
    }

    return false;
}

static bool g7x_arc_is_monotonic(const g7x_contour_element_t *start,
                                const g7x_contour_element_t *end)
{
    float cx, cz, radius;
    const float tau = 6.28318530718f;
    if (!g7x_arc_center(start, end, &cx, &cz, &radius))
        return false;
    if (fabsf(hypotf(end->d - cx, end->z - cz) - radius) > 0.001f)
        return false;
    float a = atan2f(start->z - cz, start->d - cx);
    float b = atan2f(end->z - cz, end->d - cx);
    /* G18's Z/X orientation reverses CW relative to an X/Z drawing. */
    float direction = end->gcode_cw ? 1.0f : -1.0f;
    float sweep = fmodf(direction * (b - a) + tau, tau);
    if (sweep < 0.00001f)
        return false;
    for (unsigned i = 0; i < 4; i++) {
        float t = fmodf(direction * ((float)i * tau * 0.25f - a) + 2.0f * tau, tau);
        /* An interior extremum reverses one axis; the scanline generator
           supports only arcs monotonic in both axes. */
        if (t > 0.00001f && t < sweep - 0.00001f)
            return false;
    }
    return true;
}

static bool g7x_z_at_x(const g7x_contour_region_t *region, float x, int dir, float *z)
{
    unsigned i;
    bool found = false;
    float best = 0.0f;

    if (!region || !z)
        return false;
    for (i = 1; i < region->count; i++) {
        const g7x_contour_element_t *a = &region->elements[i - 1];
        const g7x_contour_element_t *b = &region->elements[i];
        float min_x = a->d < b->d ? a->d : b->d;
        float max_x = a->d > b->d ? a->d : b->d;
        float hit;
        if (x < min_x || x > max_x)
            continue;
        if (b->kind == G7X_SEGMENT_ARC) {
            float cx, cz, r, dx, dz;
            if (!g7x_arc_center(a, b, &cx, &cz, &r))
                continue;
            dx = x - cx;
            if (fabsf(dx) > r)
                continue;
            dz = sqrtf((r * r) - (dx * dx));
            {
                float z1 = cz + dz;
                float z2 = cz - dz;
                float min_z = a->z < b->z ? a->z : b->z;
                float max_z = a->z > b->z ? a->z : b->z;
                bool z1_ok = z1 >= min_z - 0.001f && z1 <= max_z + 0.001f;
                bool z2_ok = z2 >= min_z - 0.001f && z2 <= max_z + 0.001f;
                if (!z1_ok && !z2_ok)
                    continue;
                if (z1_ok && z2_ok)
                    hit = dir < 0 ? (z1 < z2 ? z1 : z2) : (z1 > z2 ? z1 : z2);
                else
                    hit = z1_ok ? z1 : z2;
            }
        } else {
            if (fabsf(b->d - a->d) < 0.0001f)
                continue;
            hit = a->z + ((x - a->d) / (b->d - a->d)) * (b->z - a->z);
        }
        if (!found || (dir < 0 ? hit < best : hit > best)) {
            best = hit;
            found = true;
        }
    }
    if (found)
        *z = best;
    return found;
}

static bool g7x_x_at_z(const g7x_contour_region_t *region, float z, float *x)
{
    unsigned i;
    bool found = false;
    float best = 0.0f;

    if (!region || !x)
        return false;
    for (i = 1; i < region->count; i++) {
        const g7x_contour_element_t *a = &region->elements[i - 1];
        const g7x_contour_element_t *b = &region->elements[i];
        float min_z = a->z < b->z ? a->z : b->z;
        float max_z = a->z > b->z ? a->z : b->z;
        float hit;
        if (z < min_z || z > max_z)
            continue;
        if (b->kind == G7X_SEGMENT_ARC) {
            float cx, cz, r, dz, dx;
            if (!g7x_arc_center(a, b, &cx, &cz, &r))
                continue;
            dz = z - cz;
            if (fabsf(dz) > r)
                continue;
            dx = sqrtf((r * r) - (dz * dz));
            {
                float x1 = cx + dx;
                float x2 = cx - dx;
                float min_x = a->d < b->d ? a->d : b->d;
                float max_x = a->d > b->d ? a->d : b->d;
                bool x1_ok = x1 >= min_x - 0.001f && x1 <= max_x + 0.001f;
                bool x2_ok = x2 >= min_x - 0.001f && x2 <= max_x + 0.001f;
                if (!x1_ok && !x2_ok)
                    continue;
                if (x1_ok && x2_ok)
                    hit = x1 < x2 ? x1 : x2;
                else
                    hit = x1_ok ? x1 : x2;
            }
        } else {
            if (fabsf(b->z - a->z) < 0.0001f)
                continue;
            hit = a->d + ((z - a->z) / (b->z - a->z)) * (b->d - a->d);
        }
        if (!found || hit < best) {
            best = hit;
            found = true;
        }
    }
    if (found)
        *x = best;
    return found;
}

static void g7x_return_clearance_point(const g7x_stream_t *stream, float *x, float *z);

static void g7x_block_set(g7x_motion_block_t *block,
                          uint8_t motion,
                          bool has_x,
                          float x,
                          bool has_z,
                          float z,
                          bool has_f,
                          float f)
{
    memset(block, 0, sizeof(*block));
    block->motion = motion;
    block->has_x = has_x;
    block->has_z = has_z;
    block->has_f = has_f;
    block->x = x;
    block->z = z;
    block->f = f;
}

static void g7x_event_comment(g7x_event_t *event, const char *fmt, ...)
{
    va_list args;

    memset(event, 0, sizeof(*event));
    event->type = G7X_EVENT_COMMENT;
    va_start(args, fmt);
    (void)vsnprintf(event->comment, sizeof(event->comment), fmt, args);
    va_end(args);
}

static void g7x_event_motion(g7x_event_t *event,
                             uint8_t motion,
                             bool has_x,
                             float x,
                             bool has_z,
                             float z,
                             bool has_f,
                             float f)
{
    memset(event, 0, sizeof(*event));
    event->type = G7X_EVENT_MOTION;
    event->motion.source_line = (size_t)-1;
    g7x_block_set(&event->motion, motion, has_x, x, has_z, z, has_f, f);
}

/* True when this motion would not move the tool: every axis it names is already
   where the generator last left it. Such a block is never emitted - each pass
   used to repeat the same X retract and the same Z return twice, and a
   zero-length feed costs a planner slot and a line of the log. */
static bool g7x_event_is_idle(const g7x_stream_t *stream, const g7x_event_t *event)
{
    const g7x_motion_block_t *m;
    bool moves = false;

    if (!stream->have_last || !event || event->type != G7X_EVENT_MOTION)
        return false;
    m = &event->motion;
    if (m->has_x && fabsf(m->x - stream->last_x) > 0.0005f)
        moves = true;
    if (m->has_z && fabsf(m->z - stream->last_z) > 0.0005f)
        moves = true;
    return !moves;
}

static void g7x_stream_note_motion(g7x_stream_t *stream, const g7x_event_t *event)
{
    const g7x_motion_block_t *m = &event->motion;

    if (m->has_x)
        stream->last_x = m->x;
    if (m->has_z)
        stream->last_z = m->z;
    stream->have_last = true;
}

static g7x_step_result_t g7x_stream_next_event_raw(g7x_stream_t *stream,
                                                   g7x_event_t *event);

/* One event per call, except a motion that would not move the tool: that one is
   dropped and the next event is fetched, so callers only ever see real moves. */
g7x_step_result_t g7x_stream_next_event(g7x_stream_t *stream, g7x_event_t *event)
{
    for (;;) {
        g7x_step_result_t result = g7x_stream_next_event_raw(stream, event);

        if (result != G7X_STEP_LINE || event->type != G7X_EVENT_MOTION)
            return result;
        if (g7x_event_is_idle(stream, event))
            continue;
        g7x_stream_note_motion(stream, event);
        return result;
    }
}

static g7x_step_result_t g7x_stream_next_event_raw(g7x_stream_t *stream, g7x_event_t *event)
{
    if (!stream || !stream->active || !event)
        return G7X_STEP_ERROR;

    /* Explicit OD clearance, in the stream's coordinate convention. No stock
       inference: the caller must provide a clear outward-X approach. */
    const float clear_x = stream->max_x + stream->region.x_allow + stream->region.retract;
    const float clear_z = stream->start_z - stream->dir *
                          (stream->region.z_allow + stream->region.retract);

    if (!stream->started) {
        stream->started = true;
        g7x_event_comment(event,
                          "NC %s generated",
                          stream->region.cycle == G7X_CYCLE_G72 ? "G72" :
                          (stream->region.cycle == G7X_CYCLE_G70 ? "G70"
                                                                 : "G71"));
        return G7X_STEP_LINE;
    }

    if (!stream->finish) {
        if (stream->region.cycle == G7X_CYCLE_G72) {
            float x_hit;
            if ((stream->dir < 0 && stream->pass < stream->final_pass) ||
                (stream->dir > 0 && stream->pass > stream->final_pass) ||
                !g7x_x_at_z(&stream->region, stream->pass, &x_hit)) {
                stream->finish = true;
                stream->stage = 0;
            } else {
                switch (stream->stage++) {
                case 0:
                    g7x_event_comment(event, "G72 rough Z%.3f", stream->pass);
                    return G7X_STEP_LINE;
                case 1:
                    /* The first pass is entered from wherever the tool stands:
                       out in X first, then to the face clearance. Every pass
                       after it is already clear in X (the pass ends with the X
                       retract), so the Z move goes straight to the depth - the
                       tool is outside the OD there, and a return to the start Z
                       would only make the rapid twice as long. */
                    g7x_event_motion(event, 0, true, clear_x, false, 0.0f, false, 0.0f);
                    return G7X_STEP_LINE;
                case 2:
                    g7x_event_motion(event, 0, false, 0.0f, true,
                                     stream->pass_approach ? clear_z : stream->pass,
                                     false, 0.0f);
                    return G7X_STEP_LINE;
                case 3:
                    g7x_event_motion(event, 1, false, 0.0f, true, stream->pass, true, stream->feed);
                    return G7X_STEP_LINE;
                case 4:
                    g7x_event_motion(event, 1, true, x_hit + stream->region.x_allow, false, 0.0f, true, stream->feed);
                    return G7X_STEP_LINE;
                default:
                    stream->stage = 0;
                    stream->pass_approach = false;
                    stream->pass = g7x_rough_step(stream->pass,
                                                  (float)stream->dir * stream->doc,
                                                  stream->final_pass);
                    g7x_event_motion(event, 0, true, clear_x, false, 0.0f, false, 0.0f);
                    return G7X_STEP_LINE;
                }
            }
        } else {
            float z_hit;
            if (stream->pass < stream->final_pass ||
                !g7x_z_at_x(&stream->region, stream->pass, stream->dir, &z_hit)) {
                stream->finish = true;
                stream->stage = 0;
            } else {
                switch (stream->stage++) {
                case 0:
                    g7x_event_comment(event, "G71 rough X%.3f", stream->pass);
                    return G7X_STEP_LINE;
                case 1:
                    g7x_event_motion(event, 0, true, clear_x, false, 0.0f, false, 0.0f);
                    return G7X_STEP_LINE;
                case 2:
                    g7x_event_motion(event, 0, false, 0.0f, true, clear_z, false, 0.0f);
                    return G7X_STEP_LINE;
                case 3:
                    g7x_event_motion(event, 1, true, stream->pass, false, 0.0f, true, stream->feed);
                    return G7X_STEP_LINE;
                case 4:
                    g7x_event_motion(event, 1, false, 0.0f, true, z_hit - ((float)stream->dir * stream->region.z_allow), true, stream->feed);
                    return G7X_STEP_LINE;
                case 5:
                    g7x_event_motion(event, 0, true, clear_x, false, 0.0f, false, 0.0f);
                    return G7X_STEP_LINE;
                default:
                    stream->stage = 0;
                    stream->pass_approach = false;
                    stream->pass = g7x_rough_step(stream->pass,
                                                  -stream->doc,
                                                  stream->final_pass);
                    /* Each OD pass cuts from the face end again, so the tool
                       goes back in Z for the next one - at the clearance X it is
                       already out at, so the return is a plain rapid in the air
                       (and the X is not repeated: it is still clear). */
                    g7x_event_motion(event, 0, false, 0.0f, true, clear_z, false, 0.0f);
                    return G7X_STEP_LINE;
                }
            }
        }
    }

    if (stream->finish_i == 0) {
        stream->finish_i++;
        g7x_event_comment(event, "G7x finish contour");
        return G7X_STEP_LINE;
    }
    if (stream->finish_i == 1 || stream->finish_i == 2) {
        bool move_x = stream->finish_i++ == 1;
        g7x_event_motion(event, 0, move_x, clear_x, !move_x, clear_z, false, 0.0f);
        return G7X_STEP_LINE;
    }
    if (stream->finish_i <= stream->region.count + 2u) {
        unsigned index = stream->finish_i - 3u;
        const g7x_contour_element_t *el = &stream->region.elements[index];
        /* The first row of the profile is the point the cut starts from. When
           the program wrote it as a `G0` - Fanuc's `P` block - the cycle rapids
           there (the row is a positioning move, not a cut); the cut itself is
           the row after it, and that is the one that carries the finish feed
           when the profile did not name one. */
        bool approach = el->approach;
        bool first_cut = (!approach && index == 0u) ||
                         (index == 1u && stream->region.elements[0].approach);

        stream->finish_i++;
        memset(event, 0, sizeof(*event));
        event->type = G7X_EVENT_MOTION;
        event->motion.motion = approach ? 0u :
                              (el->kind == G7X_SEGMENT_ARC ?
                                   (el->gcode_cw ? 2u : 3u) : 1u);
        event->motion.source_line = el->source_line;
        event->motion.has_f = el->has_feed || first_cut;
        event->motion.f = el->has_feed ? el->feed : stream->feed;
        event->motion.has_x = true;
        event->motion.has_z = true;
        event->motion.x = el->d;
        event->motion.z = el->z;
        if (el->kind == G7X_SEGMENT_ARC && el->has_center) {
            event->motion.has_i = true;
            event->motion.has_k = true;
            event->motion.i = el->i;
            event->motion.k = el->k;
        } else if (el->kind == G7X_SEGMENT_ARC) {
            event->motion.has_r = true;
            event->motion.r = el->r;
        }
        return G7X_STEP_LINE;
    }
    if (stream->finish_i == stream->region.count + 3u) {
        float x;
        float z;
        stream->finish_i++;
        g7x_return_clearance_point(stream, &x, &z);
        g7x_event_motion(event, 0, true, x, false, 0.0f, false, 0.0f);
        return G7X_STEP_LINE;
    }
    if (stream->finish_i == stream->region.count + 4u) {
        float x;
        float z;
        stream->finish_i++;
        g7x_return_clearance_point(stream, &x, &z);
        g7x_event_motion(event, 0, false, 0.0f, true, z, false, 0.0f);
        return G7X_STEP_LINE;
    }

    g7x_stream_reset(stream);
    return G7X_STEP_DONE;
}

/* The point the cycle retracts to and returns to: out of the material in X and
   back to the clear Z. Fanuc leaves the tool at the point the cycle started
   from after a stock-removal cycle, and a program written to that contract
   (or one that simply continues from its own `G0` before the block) expects
   the tool there - the cycle's own rapids are absolute, computed from the
   profile, so the clearance corner is the only start point it can express. */
static void g7x_return_clearance_point(const g7x_stream_t *stream, float *x, float *z)
{
    const g7x_contour_element_t *start;

    if (!stream || !x || !z) {
        return;
    }

    *x = stream->start_x;
    *z = stream->start_z;
    if (stream->region.count < 1 || stream->region.retract <= 0.0001f) {
        return;
    }

    start = &stream->region.elements[0];
    *x = start->d;
    *z = start->z;
    *x = stream->max_x + stream->region.x_allow + stream->region.retract;
    *z = stream->start_z - stream->dir * (stream->region.z_allow + stream->region.retract);
}

static void g7x_event_format_text(const g7x_event_t *event, char *out, size_t out_sz)
{
    const g7x_motion_block_t *block;
    int len;

    if (!event || !out || out_sz == 0) {
        return;
    }
    out[0] = '\0';
    if (event->type == G7X_EVENT_COMMENT) {
        (void)snprintf(out, out_sz, "(%s)", event->comment);
        return;
    }
    if (event->type != G7X_EVENT_MOTION) {
        return;
    }
    block = &event->motion;
    len = snprintf(out, out_sz, "G%u", (unsigned)block->motion);
    if (block->has_x && len > 0 && len < (int)out_sz)
        len += snprintf(out + len, out_sz - (size_t)len, " X%.3f", block->x * 2.0f);
    if (block->has_z && len > 0 && len < (int)out_sz)
        len += snprintf(out + len, out_sz - (size_t)len, " Z%.3f", block->z);
    if (block->has_r && len > 0 && len < (int)out_sz)
        len += snprintf(out + len, out_sz - (size_t)len, " R%.3f", block->r);
    if (block->has_i && len > 0 && len < (int)out_sz)
        len += snprintf(out + len, out_sz - (size_t)len, " I%.3f", block->i);
    if (block->has_k && len > 0 && len < (int)out_sz)
        len += snprintf(out + len, out_sz - (size_t)len, " K%.3f", block->k);
    if (block->has_f && len > 0 && len < (int)out_sz)
        (void)snprintf(out + len, out_sz - (size_t)len, " F%.3f", block->f);
}

g7x_step_result_t g7x_stream_next(g7x_stream_t *stream, char *out, size_t out_sz)
{
    g7x_event_t event;
    g7x_step_result_t step;

    if (!out || out_sz == 0) {
        return G7X_STEP_ERROR;
    }
    step = g7x_stream_next_event(stream, &event);
    if (step == G7X_STEP_LINE) {
        if (stream) {
            stream->last_source_line = event.type == G7X_EVENT_MOTION ?
                                       event.motion.source_line :
                                       (size_t)-1;
        }
        g7x_event_format_text(&event, out, out_sz);
    } else if (stream) {
        stream->last_source_line = (size_t)-1;
    }
    return step;
}

g7x_step_result_t g7x_stream_next_block(g7x_stream_t *stream, g7x_motion_block_t *block)
{
    g7x_event_t event;
    g7x_step_result_t step;

    if (!block) {
        return G7X_STEP_ERROR;
    }
    for (;;) {
        step = g7x_stream_next_event(stream, &event);
        if (step != G7X_STEP_LINE) {
            return step;
        }
        if (event.type == G7X_EVENT_MOTION) {
            *block = event.motion;
            stream->last_source_line = event.motion.source_line;
            return G7X_STEP_LINE;
        }
    }
}

const char *g7x_result_text(g7x_result_t result)
{
    switch (result)
    {
        case G7X_OK: return "ok";
        case G7X_BAD_FIELD: return "bad field";
        case G7X_UNSUPPORTED: return "unsupported";
        case G7X_WRITE_FAILED: return "write failed";
        case G7X_RANGE_MISSING: return "numbered range missing";
        case G7X_RANGE_AMBIGUOUS: return "numbered range ambiguous";
        case G7X_CORNER_TOO_LARGE: return "corner does not fit";
        default: return "unknown";
    }
}

void g7x_thread_reset(g7x_thread_stream_t *stream)
{
    if (stream)
        memset(stream, 0, sizeof(*stream));
}

#if G7X_ENABLE_G76
static float g7x_absf(float v)
{
    return v < 0.0f ? -v : v;
}

static float g7x_maxf(float a, float b)
{
    return a > b ? a : b;
}

static float g7x_minf(float a, float b)
{
    return a < b ? a : b;
}

static bool g7x_too_many_steps(float span, float step)
{
    span = g7x_absf(span);
    step = g7x_absf(step);
    return step > 0.0f && (span / step) > (float)G7X_MAX_THREAD_PASSES;
}
#endif

g7x_result_t g7x_thread_begin_parsed(g7x_thread_stream_t *stream,
                                     float d_start,
                                     float d_end,
                                     float z1,
                                     float z2,
                                     float pitch,
                                     float doc,
                                     float clearance,
                                     float lead,
                                     float taper,
                                     float compound_angle,
                                     float degression,
                                     int spring_passes,
                                     int pass_count,
                                     int strategy,
                                     float peak_offset)
{
#if G7X_ENABLE_G76
    float depth;
    float rough_depth;

    if (!stream)
        return G7X_BAD_FIELD;

    g7x_thread_reset(stream);

    if (!isfinite(d_start) || !isfinite(d_end) || !isfinite(z1) || !isfinite(z2) ||
        !isfinite(pitch) || !isfinite(doc) || !isfinite(clearance) ||
        !isfinite(lead) || !isfinite(taper) || !isfinite(compound_angle) ||
        !isfinite(degression) || !isfinite(peak_offset) || clearance <= 0.0f ||
        d_start + taper <= 0.0f || d_end + taper <= 0.0f)
        return G7X_BAD_FIELD;
    if (d_start <= 0.0f)
        return G7X_BAD_FIELD;
    if (d_end <= 0.0f)
        return G7X_BAD_FIELD;
    if (d_end == d_start)
        return G7X_BAD_FIELD;
    if (z1 == z2)
        return G7X_BAD_FIELD;
    if (pitch <= 0.0f)
        return G7X_BAD_FIELD;
    if (doc <= 0.0f)
        return G7X_BAD_FIELD;
    if (lead < 0.0f)
        return G7X_BAD_FIELD;
    if (degression < 1.0f)
        return G7X_BAD_FIELD;
    if (compound_angle < 0.0f || compound_angle >= 89.0f)
        return G7X_BAD_FIELD;
    if (spring_passes < 0 || spring_passes > G7X_MAX_THREAD_PASSES)
        return G7X_BAD_FIELD;
    if (pass_count < 0 || pass_count > G7X_MAX_THREAD_PASSES)
        return G7X_BAD_FIELD;

    depth = g7x_absf(d_start - d_end);
    rough_depth = depth;
    if (pass_count == 0 && g7x_too_many_steps(rough_depth, doc))
        return G7X_BAD_FIELD;
    if (pass_count == 0)
        pass_count = (int)ceilf(rough_depth / doc);
    if (pass_count <= 0)
        pass_count = 1;

    stream->d_start = d_start;
    stream->d_end = d_end;
    stream->depth = depth;
    stream->rough_depth = rough_depth;
    stream->doc = doc;
    stream->min_doc = 0.0f;
    stream->finish_allow = 0.0f;
    stream->pitch = pitch;
    stream->z1 = z1;
    stream->z2 = z2;
    stream->taper = taper;
    stream->z_span = z2 - z1;
    stream->degression = degression;
    stream->pass_count = pass_count;
    stream->spring_left = spring_passes;
    stream->finish_left = 0;
    stream->tool_angle = 0;
    stream->chamfer = 0;
    stream->strategy = strategy ? 1 : 0;
    stream->pass = 0;
    stream->last_depth = 0.0f;
    stream->angle_tan = 0.0f;
    stream->final_z_shift = 0.0f;

    if (compound_angle > 0.001f) {
        stream->angle_tan = tanf(compound_angle * 0.01745329252f);
        if (!isfinite(stream->angle_tan) || stream->angle_tan <= 0.0001f)
            return G7X_BAD_FIELD;
        stream->final_z_shift = (depth * 0.5f) * stream->angle_tan;
        if (stream->z_span < 0.0f)
            stream->final_z_shift = -stream->final_z_shift;
    }

    stream->zsafe = z1 + ((stream->z_span < 0.0f) ? lead : -lead);
    if (d_end < d_start)
        stream->xsafe = g7x_maxf(d_start, d_start + taper) + g7x_absf(peak_offset != 0.0f ? peak_offset : clearance);
    else
        stream->xsafe = g7x_minf(d_start, d_start + taper) - g7x_absf(peak_offset != 0.0f ? peak_offset : clearance);
    if (stream->xsafe < 0.0f)
        stream->xsafe = 0.0f;

    stream->stage = 0;
    stream->active = true;
    return G7X_OK;
#else
    (void)stream;
    (void)d_start;
    (void)d_end;
    (void)z1;
    (void)z2;
    (void)pitch;
    (void)doc;
    (void)clearance;
    (void)lead;
    (void)taper;
    (void)compound_angle;
    (void)degression;
    (void)spring_passes;
    (void)pass_count;
    (void)strategy;
    (void)peak_offset;
    return G7X_UNSUPPORTED;
#endif
}

g7x_result_t g7x_thread_begin_semantic(g7x_thread_stream_t *stream,
                                       float d_start,
                                       float d_end,
                                       float z1,
                                       float z2,
                                       float pitch,
                                       float thread_height,
                                       float first_cut,
                                       float min_cut,
                                       float finish_allowance,
                                       float clearance,
                                       float taper,
                                       int spring_passes,
                                       int chamfer,
                                       int tool_angle)
{
#if G7X_ENABLE_G76
    float diameter_depth;
    float rough_depth;
    float rough_end;
    int pass_count;
    g7x_result_t result;

    if (!stream)
        return G7X_BAD_FIELD;
    g7x_thread_reset(stream);
    if (!isfinite(d_start) || !isfinite(d_end) || !isfinite(z1) || !isfinite(z2) ||
        !isfinite(pitch) || !isfinite(thread_height) || !isfinite(first_cut) ||
        !isfinite(min_cut) || !isfinite(finish_allowance) ||
        !isfinite(clearance) || !isfinite(taper) ||
        thread_height <= 0.0f || first_cut <= 0.0f || min_cut <= 0.0f ||
        min_cut > first_cut || finish_allowance < 0.0f || clearance <= 0.0f)
        return G7X_BAD_FIELD;
    /* P/Q/minimum/finish are radial depths; X and taper are diameters.
       The start diameter is the thread crest, not the approach clearance. */
    diameter_depth = thread_height * 2.0f;
    if (fabsf(fabsf(d_start - d_end) - diameter_depth) > 0.001f)
        return G7X_BAD_FIELD;
    if (chamfer != 0 || tool_angle != 0)
        return G7X_UNSUPPORTED;
    rough_depth = diameter_depth - (finish_allowance * 2.0f);
    if (rough_depth <= 0.0f)
        return G7X_BAD_FIELD;

    pass_count = 0;
    {
        float cut = first_cut * 2.0f;
        float depth = 0.0f;
        while (depth < rough_depth && pass_count < G7X_MAX_THREAD_PASSES) {
            depth = fminf(depth + cut, rough_depth);
            cut = fmaxf(cut * 0.75f, min_cut * 2.0f);
            pass_count++;
        }
        if (depth < rough_depth || pass_count + spring_passes +
            (finish_allowance > 0.0f ? 1 : 0) > G7X_MAX_THREAD_PASSES)
            return G7X_BAD_FIELD;
    }

    rough_end = d_start + ((d_end < d_start) ? -rough_depth : rough_depth);
    result = g7x_thread_begin_parsed(stream,
                                     d_start,
                                     rough_end,
                                     z1,
                                     z2,
                                     pitch,
                                     first_cut,
                                     clearance,
                                     pitch,
                                     taper,
                                     0.0f,
                                     2.0f,
                                     spring_passes,
                                     pass_count,
                                     0,
                                     0.0f);
    if (result != G7X_OK)
        return result;

    stream->d_end = d_end;
    stream->depth = diameter_depth;
    stream->rough_depth = rough_depth;
    stream->min_doc = min_cut * 2.0f;
    stream->doc = first_cut * 2.0f;
    stream->semantic_schedule = true;
    stream->finish_allow = finish_allowance;
    stream->finish_left = finish_allowance > 0.0f ? 1 : 0;
    stream->tool_angle = tool_angle;
    stream->chamfer = chamfer;
    return G7X_OK;
#else
    (void)stream;
    (void)d_start;
    (void)d_end;
    (void)z1;
    (void)z2;
    (void)pitch;
    (void)thread_height;
    (void)first_cut;
    (void)min_cut;
    (void)finish_allowance;
    (void)clearance;
    (void)taper;
    (void)spring_passes;
    (void)chamfer;
    (void)tool_angle;
    return G7X_UNSUPPORTED;
#endif
}

#if G7X_ENABLE_G76
static bool g7x_thread_prepare_next_pass(g7x_thread_stream_t *stream)
{
    float pass_depth;
    float z_shift = 0.0f;

    if (!stream)
        return false;

    if (stream->pass < stream->pass_count) {
        float t;
        stream->pass++;
        stream->pass_kind = 0;
        t = (float)stream->pass / (float)stream->pass_count;
        if (stream->semantic_schedule) {
            pass_depth = stream->last_depth + stream->doc;
            stream->doc = fmaxf(stream->doc * 0.75f, stream->min_doc);
        } else {
            pass_depth = stream->rough_depth *
                         (stream->strategy ? powf(t, 1.0f / stream->degression) : t);
        }
        if (pass_depth > stream->rough_depth)
            pass_depth = stream->rough_depth;
        if (pass_depth <= stream->last_depth)
            pass_depth = stream->rough_depth;
        stream->last_depth = pass_depth;
        stream->pass_x1 = stream->d_start + ((stream->d_end < stream->d_start) ? -pass_depth : pass_depth);
    } else if (stream->finish_left > 0) {
        stream->pass_kind = 1;
        stream->finish_left--;
        stream->pass_x1 = stream->d_end;
    } else if (stream->spring_left > 0) {
        stream->pass_kind = 2;
        stream->spring_left--;
        stream->pass_x1 = stream->d_end;
    } else {
        return false;
    }

    stream->pass_x2 = stream->pass_x1 + stream->taper;
    stream->pass_z1 = stream->z1;
    stream->pass_zsafe = stream->zsafe;
    if (stream->angle_tan > 0.0f) {
        float pass_depth_for_z = g7x_absf(stream->d_start - stream->pass_x1);
        z_shift = (pass_depth_for_z * 0.5f) * stream->angle_tan;
        if (stream->z_span < 0.0f)
            z_shift = -z_shift;
        stream->pass_z1 = stream->z1 + z_shift - stream->final_z_shift;
        stream->pass_zsafe = stream->pass_z1 + ((stream->z_span < 0.0f) ? stream->pitch : -stream->pitch);
    }
    return true;
}
#endif

g7x_step_result_t g7x_thread_next(g7x_thread_stream_t *stream, char *out, size_t out_sz)
{
#if G7X_ENABLE_G76
    if (!stream || !stream->active || !out || out_sz == 0)
        return G7X_STEP_ERROR;

    for (;;) {
        switch (stream->stage++) {
        case 0:
            if (stream->min_doc > 0.0f || stream->finish_allow > 0.0f || stream->tool_angle) {
                (void)snprintf(out, out_sz,
                               "(G76 FANUC D %.3f X %.3f F %.3f P %.3f Q %.3f R %.3f A %d N %d)",
                               stream->d_start,
                               stream->d_end,
                               stream->pitch,
                               stream->depth * 0.5f,
                               stream->min_doc * 0.5f,
                               stream->finish_allow,
                               stream->tool_angle,
                               stream->pass_count);
            } else {
                (void)snprintf(out, out_sz,
                               "(G76 D %.3f X %.3f P %.3f DOC %.3f R %.3f N %d)",
                               stream->d_start,
                               stream->d_end,
                               stream->pitch,
                               stream->doc,
                               stream->degression,
                               stream->pass_count);
            }
            return G7X_STEP_LINE;
        case 1:
            (void)snprintf(out, out_sz, "(ELS RAMP: lead-in and lead-out are reserve space, not finished thread)");
            return G7X_STEP_LINE;
        case 2:
            (void)snprintf(out, out_sz, "G0 X%.3f Z%.3f", stream->xsafe, stream->zsafe);
            return G7X_STEP_LINE;
        case 3:
            if (!g7x_thread_prepare_next_pass(stream)) {
                stream->stage = 10;
                continue;
            }
            if (stream->pass_kind == 0) {
                (void)snprintf(out, out_sz, "(THREAD pass %d X%.3f Z%.3f)",
                               stream->pass, stream->pass_x1, stream->pass_z1);
            } else if (stream->pass_kind == 1) {
                stream->last_depth = stream->depth;
                (void)snprintf(out, out_sz, "(THREAD finish X%.3f Z%.3f)",
                               stream->pass_x1, stream->pass_z1);
            } else {
                (void)snprintf(out, out_sz, "(THREAD spring X%.3f Z%.3f)",
                               stream->pass_x1, stream->pass_z1);
            }
            return G7X_STEP_LINE;
        case 4:
            (void)snprintf(out, out_sz, "G0 X%.3f Z%.3f", stream->xsafe, stream->pass_zsafe);
            return G7X_STEP_LINE;
        case 5:
            (void)snprintf(out, out_sz, "G0 X%.3f", stream->pass_x1);
            return G7X_STEP_LINE;
        case 6:
            (void)snprintf(out, out_sz, "G0 Z%.3f", stream->pass_z1);
            return G7X_STEP_LINE;
        case 7:
            (void)snprintf(out, out_sz, "G33 X%.3f Z%.3f K%.3f",
                           stream->pass_x2, stream->z2, stream->pitch);
            return G7X_STEP_LINE;
        case 8:
            (void)snprintf(out, out_sz, "G0 X%.3f", stream->xsafe);
            return G7X_STEP_LINE;
        case 9:
            stream->stage = 3;
            continue;
        case 10:
            (void)snprintf(out, out_sz, "G0 Z%.3f", stream->zsafe);
            return G7X_STEP_LINE;
        case 11:
            stream->active = false;
            return G7X_STEP_DONE;
        default:
            stream->active = false;
            return G7X_STEP_DONE;
        }
    }
#else
    (void)stream;
    (void)out;
    (void)out_sz;
    return G7X_STEP_ERROR;
#endif
}

#if defined(ENABLE_PARSER_MODULES) && !defined(G7X_HOST_TEST)
static char g7x_parser_motion_letter(uint8_t motion)
{
    switch (motion) {
    case G0: return '0';
    case G1: return '1';
    case G2: return '2';
    case G3: return '3';
    default: return '?';
    }
}

static void g7x_parser_log_block(const g7x_motion_block_t *block)
{
    char line[96];
    int len;

    if (!block) {
        return;
    }
    len = snprintf(line, sizeof(line), "G7X EXEC G%c", g7x_parser_motion_letter(block->motion));
    if (block->has_x && len > 0 && len < (int)sizeof(line))
        len += snprintf(line + len, sizeof(line) - (size_t)len, " X%.3f", block->x);
    if (block->has_z && len > 0 && len < (int)sizeof(line))
        len += snprintf(line + len, sizeof(line) - (size_t)len, " Z%.3f", block->z);
    if (block->has_r && len > 0 && len < (int)sizeof(line))
        len += snprintf(line + len, sizeof(line) - (size_t)len, " R%.3f", block->r);
    if (block->has_i && len > 0 && len < (int)sizeof(line))
        len += snprintf(line + len, sizeof(line) - (size_t)len, " I%.3f", block->i);
    if (block->has_k && len > 0 && len < (int)sizeof(line))
        len += snprintf(line + len, sizeof(line) - (size_t)len, " K%.3f", block->k);
    if (block->has_f && len > 0 && len < (int)sizeof(line))
        (void)snprintf(line + len, sizeof(line) - (size_t)len, " F%.3f", block->f);
    proto_info("%s", line);
}

static uint32_t g7x_parser_block_number(const parser_words_t *words)
{
#ifdef GCODE_PROCESS_LINE_NUMBERS
    return words ? (uint32_t)words->n : 0u;
#else
    (void)words;
    return 0u;
#endif
}

/* P/Q block numbers are positive integers inside the parser's line range. */
static bool g7x_parser_pq_number(float value, uint32_t *out)
{
    if (!out || !isfinite(value) || value < 1.0f ||
        value > (float)MAX_LINE_NUMBER || floorf(value) != value)
        return false;
    *out = (uint32_t)value;
    return true;
}

/* Retain the profile block in the generator's internal coordinates so a later
   lookup can replay it without the original source text. */
static void g7x_parser_retain_contour(uint32_t number,
                                      uint8_t motion,
                                      const parser_words_t *words,
                                      const parser_cmd_explicit_t *cmd)
{
    char text[G7X_RETAINED_TEXT_LEN];
    int len;

    if (!number || !words || !cmd)
        return;
    len = snprintf(text, sizeof(text), "N%u G%c",
                   (unsigned)number, g7x_parser_motion_letter(motion));
    if (len > 0 && len < (int)sizeof(text) && CHECKFLAG(cmd->words, GCODE_WORD_X))
        len += snprintf(text + len, sizeof(text) - (size_t)len,
                        " X%.3f", words->xyzabc[AXIS_X]);
    if (len > 0 && len < (int)sizeof(text) && CHECKFLAG(cmd->words, GCODE_WORD_Z))
        len += snprintf(text + len, sizeof(text) - (size_t)len,
                        " Z%.3f", words->xyzabc[AXIS_Z]);
    if (len > 0 && len < (int)sizeof(text) && CHECKFLAG(cmd->words, GCODE_WORD_I))
        len += snprintf(text + len, sizeof(text) - (size_t)len,
                        " I%.3f", words->ijk[0]);
    if (len > 0 && len < (int)sizeof(text) && CHECKFLAG(cmd->words, GCODE_WORD_K))
        len += snprintf(text + len, sizeof(text) - (size_t)len,
                        " K%.3f", words->ijk[2]);
    if (len > 0 && len < (int)sizeof(text) && CHECKFLAG(cmd->words, GCODE_WORD_R))
        (void)snprintf(text + len, sizeof(text) - (size_t)len,
                       " R%.3f", words->r);
    (void)g7x_history_add(&g7x_parser_history, number, text);
}

/* A cycle header is still waiting for its first contour row. Only then may a
   second G71/G72 block complete it (Fanuc two-line form). */
static bool g7x_parser_header_waiting(void)
{
    return (g7x_parser_region_active || g7x_parser_pq_armed) &&
           g7x_parser_stream.region.count == 0u;
}

/* Close the active contour region and queue its generated blocks. Used by an
   explicit G80 and by the N(Q) block of a P/Q numbered range. */
static void g7x_parser_region_complete(gcode_exec_args_t *ptr)
{
    bool done = false;

    /* Keep the range as collected for a later `G70 P Q`. It is copied *before*
       the end mark runs `prepare`, which expands corners in place: the replay
       goes through the same prepare, so it must start from what the program
       wrote, not from an already expanded region. */
    if (g7x_parser_pq_collecting && g7x_parser_pq_p != 0u &&
        g7x_parser_stream.region.count >= 2u) {
        g7x_parser_kept_region = g7x_parser_stream.region;
        g7x_parser_kept_p = g7x_parser_pq_p;
        g7x_parser_kept_q = g7x_parser_pq_q;
        g7x_parser_kept_valid = true;
    }
    g7x_result_t result = g7x_stream_add_parsed(&g7x_parser_stream,
                                                G7X_CONTOUR_END,
                                                0.0f, false,
                                                0.0f, false,
                                                0.0f, false,
                                                0.0f, false,
                                                0.0f, false,
                                                G7X_CORNER_NONE,
                                                0.0f,
                                                0.0f, false,
                                                &done);

    g7x_parser_region_active = false;
    g7x_parser_pq_collecting = false;
    if (result != G7X_OK) {
        char why[48];

        /* The range never became a cycle, so there is nothing for a G70 to
           replay either: a finish cut of a contour the roughing refused is not
           a contour this run stands behind. */
        g7x_parser_kept_valid = false;
        proto_info("G7X parser collect failed: %s", g7x_result_text(result));
        g7x_parser_clear_state();
        snprintf(why, sizeof(why), "contour rejected: %s",
                 g7x_result_text(result));
        g7x_parser_set_refusal(why);
        *(ptr->error) = STATUS_INVALID_STATEMENT;
    } else {
        proto_info("G7X region ready count=%u", (unsigned)g7x_parser_stream.region.count);
        memcpy(&g7x_parser_runner_state, ptr->new_state, sizeof(g7x_parser_runner_state));
        g7x_parser_runner_active = true;
        /* The bench reads the generated cycle off the serial, so the run of
           generated blocks says where it starts and (`run_pending`) where it
           ends - the port had dropped both with the old runner. */
        proto_print("[MSG:G7X generated blocks queued]\r\n");
        *(ptr->error) = STATUS_OK;
    }
    ptr->cmd->groups = 0;
    ptr->cmd->words = 0;
    memset(ptr->words, 0, sizeof(*ptr->words));
}

static uint8_t g7x_parser_exec_stream(parser_state_t *base_state, unsigned max_blocks, bool *done)
{
    g7x_motion_block_t block;
    g7x_step_result_t step;
    unsigned emitted = 0;

    if (done)
        *done = false;
    if (!base_state)
        return STATUS_INVALID_STATEMENT;

    while (emitted < max_blocks) {
        parser_state_t state = {0};
        parser_words_t words = {0};
        parser_cmd_explicit_t cmd = {0};
        uint8_t error;

        step = g7x_stream_next_block(&g7x_parser_stream, &block);
        if (step == G7X_STEP_DONE) {
            if (done)
                *done = true;
            return STATUS_OK;
        }
        if (step == G7X_STEP_ERROR)
            return STATUS_INVALID_STATEMENT;

        g7x_parser_log_block(&block);
        memcpy(&state, base_state, sizeof(state));
        state.groups.nonmodal = 0;
        state.groups.motion = block.motion;
        state.groups.motion_mantissa = 0;
        SETFLAG(cmd.groups, GCODE_GROUP_MOTION);
        if (block.has_x) {
            SETFLAG(cmd.words, GCODE_WORD_X);
            words.xyzabc[AXIS_X] = block.x;
        }
        if (block.has_z) {
            SETFLAG(cmd.words, GCODE_WORD_Z);
            words.xyzabc[AXIS_Z] = block.z;
        }
        if (block.has_r) {
            SETFLAG(cmd.words, GCODE_WORD_R);
            words.r = block.r;
        }
        if (block.has_i) {
            SETFLAG(cmd.words, GCODE_WORD_I);
            words.ijk[0] = block.i;
        }
        if (block.has_k) {
            SETFLAG(cmd.words, GCODE_WORD_K);
            words.ijk[2] = block.k;
        }
        if (block.has_f) {
            SETFLAG(cmd.words, GCODE_WORD_F);
            words.f = block.f;
        }

        g7_g8_motion_words_to_program(&cmd, &words);
        error = parser_exec_generated_block(&state, &words, &cmd);
        if (error != STATUS_OK) {
            char why[48];

            proto_info("G7X EXEC ERROR %u", (unsigned)error);
            /* The status alone is what the panel can show when nothing else
               explains it, and "invalid parameters or cycle contour" on the
               last row of a good-looking block is not an answer: name the block
               the controller refused, so the glass carries the line the
               operator has to look at (bench, 2026-09-23). */
            snprintf(why, sizeof(why), "block refused: G%c",
                     g7x_parser_motion_letter(block.motion));
            {
                size_t used = strlen(why);

                if (block.has_x && used + 12u < sizeof(why))
                    used += (size_t)snprintf(why + used, sizeof(why) - used,
                                             " X%.3f", (double)block.x);
                if (block.has_z && used + 12u < sizeof(why))
                    used += (size_t)snprintf(why + used, sizeof(why) - used,
                                             " Z%.3f", (double)block.z);
            }
            g7x_parser_set_refusal(why);
            return error;
        }

        memcpy(base_state, &state, sizeof(*base_state));
        emitted++;
    }

    return STATUS_OK;
}

#if G7X_ENABLE_G76 && defined(G33_ENCODER)
static bool g7x_parser_parse_thread_line(const char *line, parser_state_t *state, parser_words_t *words, parser_cmd_explicit_t *cmd)
{
    float x;
    float z;
    float k;

    if (!line || !state || !words || !cmd)
        return false;

    if (strncmp(line, "G0", 2) == 0) {
        state->groups.motion = G0;
        state->groups.motion_mantissa = 0;
        SETFLAG(cmd->groups, GCODE_GROUP_MOTION);
        if (g7x_get_field_float(line, "X", &x)) {
            SETFLAG(cmd->words, GCODE_WORD_X);
            words->xyzabc[AXIS_X] = x;
        }
        if (g7x_get_field_float(line, "Z", &z)) {
            SETFLAG(cmd->words, GCODE_WORD_Z);
            words->xyzabc[AXIS_Z] = z;
        }
        return CHECKFLAG(cmd->words, GCODE_XYZ_AXIS);
    }

    if (strncmp(line, "G33", 3) == 0) {
        state->groups.motion = G7X_G33_MOTION_CODE;
        state->groups.motion_mantissa = 0;
        SETFLAG(cmd->groups, GCODE_GROUP_MOTION);
        cmd->group_extended = EXTENDED_MOTION_GCODE(G7X_G33_MOTION_CODE);
        if (g7x_get_field_float(line, "X", &x)) {
            SETFLAG(cmd->words, GCODE_WORD_X);
            words->xyzabc[AXIS_X] = x;
        }
        if (g7x_get_field_float(line, "Z", &z)) {
            SETFLAG(cmd->words, GCODE_WORD_Z);
            words->xyzabc[AXIS_Z] = z;
        }
        if (g7x_get_field_float(line, "K", &k)) {
            SETFLAG(cmd->words, GCODE_WORD_K);
            words->ijk[2] = k;
        }
        return CHECKFLAG(cmd->words, GCODE_XYZ_AXIS) && CHECKFLAG(cmd->words, GCODE_WORD_K);
    }

    return false;
}

static uint8_t g7x_parser_exec_thread(parser_state_t *base_state, unsigned max_blocks, bool *done)
{
    unsigned emitted = 0;

    if (done)
        *done = false;
    if (!base_state)
        return STATUS_INVALID_STATEMENT;

    while (emitted < max_blocks) {
        char line[96];
        g7x_step_result_t step = g7x_thread_next(&g7x_parser_thread_stream, line, sizeof(line));
        parser_state_t state = {0};
        parser_words_t words = {0};
        parser_cmd_explicit_t cmd = {0};
        uint8_t error;

        if (step == G7X_STEP_DONE) {
            if (done)
                *done = true;
            return STATUS_OK;
        }
        if (step == G7X_STEP_ERROR)
            return STATUS_INVALID_STATEMENT;

        if (line[0] == '(') {
            proto_info("G7X EXEC %s", line);
            continue;
        }

        proto_info("G7X EXEC %s", line);
        memcpy(&state, base_state, sizeof(state));
        state.groups.nonmodal = 0;
        if (!g7x_parser_parse_thread_line(line, &state, &words, &cmd))
            return STATUS_INVALID_STATEMENT;

        words.xyzabc[AXIS_X] *= 0.5f;
        g7_g8_motion_words_to_program(&cmd, &words);

        error = parser_exec_generated_block(&state, &words, &cmd);
        if (error != STATUS_OK) {
            proto_info("G7X EXEC ERROR %u", (unsigned)error);
            return error;
        }

        memcpy(base_state, &state, sizeof(*base_state));
        emitted++;
    }

    return STATUS_OK;
}


#endif

bool g7x_parse(void *args)
{
    gcode_parse_args_t *ptr = (gcode_parse_args_t *)args;

    if (!ptr || !ptr->error || !ptr->cmd) {
        return EVENT_CONTINUE;
    }

    /* A lathe cycle cuts a path this module computes from the profile, and the
       core parser takes G41/G42 for linear and arc motion but does nothing with
       them. Running a cycle with compensation active would cut the uncompensated
       path and call it a finish, so it is refused until a compensated contour is
       supported (stage 1 of `docs/lathe-cutter-comp.md`). */
    if (ptr->word == 'G' &&
        (ptr->code == 70 || ptr->code == 71 || ptr->code == 72
#if G7X_ENABLE_G76 && defined(G33_ENCODER)
         || ptr->code == 76
#endif
        ) &&
        ptr->new_state->groups.cutter_radius_compensation != G40) {
        proto_print("[MSG:G7X: G41/G42 not supported in a cycle]\r\n");
        ptr->new_state->groups.motion = G0;
        ptr->new_state->groups.motion_mantissa = 0;
        g7x_parser_set_refusal("no G41/G42 in a cycle");
        *(ptr->error) = STATUS_GCODE_UNSUPPORTED_COMMAND;
        return EVENT_HANDLED;
    }

    if (ptr->word == 'G' && ptr->code == 73) {
        /* G73 is pattern repeating roughing: it is a different roughing model
           from the scanline passes G71/G72 generate, not a variation of them,
           and this module does not implement it. Say so by name instead of
           leaving the line to the generic "unsupported command". */
        proto_print("[MSG:G7X: G73 pattern roughing is not implemented]\r\n");
        g7x_parser_set_refusal("G73 not implemented");
        *(ptr->error) = STATUS_GCODE_UNSUPPORTED_COMMAND;
        return EVENT_HANDLED;
    }

    if (ptr->word == 'G' && ptr->code == 70) {
        /* `G70 P Q`: the finish cut of the range this run collected. The words
           are read where every other cycle header reads them (the exec
           modifier), so here the line is only claimed. */
        proto_info("G7X parse G70");
        ptr->new_state->groups.motion = G0;
        ptr->new_state->groups.motion_mantissa = 0;
        if (g7x_parser_busy() || ptr->cmd->group_extended != 0) {
            *(ptr->error) = STATUS_GCODE_MODAL_GROUP_VIOLATION;
            return EVENT_HANDLED;
        }
        ptr->cmd->group_extended = G7X_G70_EXTENDED_CODE;
        *(ptr->error) = STATUS_OK;
        return EVENT_HANDLED;
    }

    if (ptr->word == 'G' && (ptr->code == 71 || ptr->code == 72
#if G7X_ENABLE_G76 && defined(G33_ENCODER)
                             || ptr->code == 76
#endif
                             )) {
        proto_info("G7X parse G%u", (unsigned)ptr->code);
        /* Header axes are parameters, not an invocation of prior modal G80. */
        ptr->new_state->groups.motion = G0;
        ptr->new_state->groups.motion_mantissa = 0;
        if (g7x_parser_busy() ||
            ptr->cmd->group_extended != 0) {
            /* Fanuc two-line header: a second G71/G72 block of the same cycle,
               seen before any contour row, completes the open header. */
            g7x_cycle_t want = ptr->code == 72 ? G7X_CYCLE_G72 : G7X_CYCLE_G71;
            if (ptr->code != 76 && !ptr->cmd->group_extended &&
                g7x_parser_header_waiting() &&
                g7x_parser_stream.region.cycle == want) {
                g7x_parser_continuation = true;
                g7x_parser_pending_x_allow = 0.0f;
                g7x_parser_pending_x_allow_set = false;
                g7x_parser_pending_z_allow = 0.0f;
                g7x_parser_pending_z_allow_set = false;
                ptr->cmd->group_extended = G7X_EXTENDED_CODE;
                *(ptr->error) = STATUS_OK;
                return EVENT_HANDLED;
            }
            *(ptr->error) = STATUS_GCODE_MODAL_GROUP_VIOLATION;
            return EVENT_HANDLED;
        }
#if G7X_ENABLE_G76 && defined(G33_ENCODER)
        if (ptr->code == 76) {
            ptr->cmd->group_extended = G7X_G76_EXTENDED_CODE;
            *(ptr->error) = STATUS_OK;
            return EVENT_HANDLED;
        }
#endif
        ptr->cmd->group_extended = G7X_EXTENDED_CODE;
        g7x_parser_pending_cycle = ptr->code == 72 ? G7X_CYCLE_G72 : G7X_CYCLE_G71;
        g7x_parser_pending_doc = 0.0f;
        g7x_parser_pending_doc_set = false;
        *(ptr->error) = STATUS_OK;
        return EVENT_HANDLED;
    }



    if (ptr->cmd->group_extended == G7X_EXTENDED_CODE &&
        (ptr->word == 'U' || ptr->word == 'W')) {
        if (g7x_parser_continuation) {
            /* Second Fanuc block: U/W are the X/Z finish allowances. X is a
               diameter word in G7 mode, so it needs the same halving the G7/G8
               modifier applies to X words. */
            float value = ptr->value;
            if (ptr->word == 'U') {
                if (g7_g8_is_diameter_mode())
                    value *= 0.5f;
                g7x_parser_pending_x_allow = value;
                g7x_parser_pending_x_allow_set = true;
            } else {
                g7x_parser_pending_z_allow = value;
                g7x_parser_pending_z_allow_set = true;
            }
        } else {
            g7x_parser_pending_doc = ptr->value;
            g7x_parser_pending_doc_set = true;
        }
        *(ptr->error) = STATUS_OK;
        return EVENT_HANDLED;
    }

    if (g7x_parser_region_active && ptr->word == 'C') {
        g7x_parser_pending_corner_kind = G7X_CORNER_CHMF;
        g7x_parser_pending_corner_amount = ptr->value;
        *(ptr->error) = STATUS_OK;
        return EVENT_HANDLED;
    }

    return EVENT_CONTINUE;
}

bool g7x_exec_modifier(void *args)
{
    gcode_exec_args_t *ptr = (gcode_exec_args_t *)args;
    g7x_contour_cmd_t cmd = G7X_CONTOUR_NONE;

    if (ptr && ptr->cmd && ptr->cmd->dry_run)
        return EVENT_CONTINUE;
    if (!ptr || !ptr->cmd || !ptr->new_state || !ptr->words || !ptr->error) {
        return EVENT_CONTINUE;
    }

    /* A new line is executing, so any reason the previous one was refused is
       history: whatever this line does sets its own, and a caller reading the
       text after an error always reads the reason for that error. */
    g7x_parser_set_refusal("");

#if G7X_ENABLE_G76 && defined(G33_ENCODER)
    if (ptr->cmd->group_extended == G7X_G76_EXTENDED_CODE) {
        float current[AXIS_COUNT];
        const uint32_t required = GCODE_WORD_X | GCODE_WORD_Z | GCODE_WORD_F |
                                  GCODE_WORD_P | GCODE_WORD_Q;
        const uint32_t allowed = required | GCODE_WORD_R | GCODE_WORD_I | GCODE_WORD_L
#ifdef GCODE_WORD_N
                                 | GCODE_WORD_N
#endif
                                 ;
        float units = ptr->new_state->groups.units == G20 ? INCH_MM_MULT : 1.0f;
        float finish = CHECKFLAG(ptr->cmd->words, GCODE_WORD_R) ? ptr->words->r : 0.0f;
        float taper = CHECKFLAG(ptr->cmd->words, GCODE_WORD_I) ? ptr->words->ijk[0] * 2.0f : 0.0f;
        float spring = CHECKFLAG(ptr->cmd->words, GCODE_WORD_L) ? ptr->words->l : 0.0f;
        g7x_result_t result;
        if (g7x_parser_busy() || (ptr->cmd->words & required) != required ||
            (ptr->cmd->words & ~allowed) || ptr->cmd->groups ||
            ptr->new_state->groups.distance_mode != G90 ||
            ptr->new_state->groups.plane != G18 ||
            ptr->new_state->groups.feedrate_mode != G94 ||
            spring < 0.0f || spring > G7X_MAX_THREAD_PASSES || floorf(spring) != spring) {
            if ((ptr->cmd->words & required) != required)
                g7x_parser_set_refusal("G76 needs X Z P Q F");
            else if (ptr->new_state->groups.distance_mode != G90 ||
                     ptr->new_state->groups.plane != G18 ||
                     ptr->new_state->groups.feedrate_mode != G94)
                g7x_parser_set_refusal("G76 needs G18 G90 G94");
            else if (g7x_parser_busy())
                g7x_parser_set_refusal("a cycle is already running");
            else
                g7x_parser_set_refusal("G76 values out of range");
            *(ptr->error) = STATUS_INVALID_STATEMENT;
            return EVENT_HANDLED;
        }
        /* The G7/G8 modifier has already normalized X to radius. The library
           uses diameters; all values here remain in the current program units. */
        mc_get_position(current);
        kinematics_apply_transform(current);
        parser_machine_to_work(current);
        result = g7x_thread_begin_semantic(&g7x_parser_thread_stream,
                    current[AXIS_X] * 2.0f / units,
                    ptr->words->xyzabc[AXIS_X] * 2.0f,
                    current[AXIS_Z] / units, ptr->words->xyzabc[AXIS_Z],
                    ptr->words->f, ptr->words->p, ptr->words->d,
                    ptr->words->d * 0.25f, finish, 1.0f / units, taper,
                    (int)spring, 0, 0);
        if (result != G7X_OK) {
            *(ptr->error) = STATUS_INVALID_STATEMENT;
            return EVENT_HANDLED;
        }
        memcpy(&g7x_parser_runner_state, ptr->new_state, sizeof(g7x_parser_runner_state));
        g7x_parser_thread_runner_active = true;
        ptr->cmd->group_extended = 0;
        ptr->cmd->groups = 0;
        ptr->cmd->words = 0;
        memset(ptr->words, 0, sizeof(*ptr->words));
        *(ptr->error) = STATUS_OK;
        return EVENT_HANDLED;
    }
#endif

    if (ptr->cmd->group_extended == G7X_G70_EXTENDED_CODE) {
        /* `G70 P Q`: re-run a range this run already collected, as the finish
           cut. The range must be the one that was collected - the module keeps
           exactly one, because that is all a program can ask for: the profile
           it just roughed. A range it never saw is refused, never guessed. */
        bool has_p = CHECKFLAG(ptr->cmd->words, GCODE_WORD_P);
        bool has_q = CHECKFLAG(ptr->cmd->words, GCODE_WORD_Q);
        uint32_t pq_p = 0u;
        uint32_t pq_q = 0u;
        float retract = CHECKFLAG(ptr->cmd->words, GCODE_WORD_R) ?
                        ptr->words->r : g7x_parser_kept_region.retract;
        /* No `F` on the `G70` means the feed in force: the `F500` on the `G71`
           header above it, or whatever the program last named. That is the
           parser's own modal feedrate, and it is the only copy of it -
           `kept_feed` was a second one, filled only by the `G80` path, so a
           range closed by its `N(Q)` row left it at zero and the finish ran at
           the 120 fallback (bench: "we have no f in our g70. so add it"). */
        float feed = CHECKFLAG(ptr->cmd->words, GCODE_WORD_F) ?
                     ptr->words->f :
                     ptr->new_state->feedrate /
                         (ptr->new_state->groups.units == G20 ? INCH_MM_MULT
                                                              : 1.0f);
        g7x_result_t result;

        if (!has_p || !has_q ||
            !g7x_parser_pq_number(ptr->words->p, &pq_p) ||
            !g7x_parser_pq_number(ptr->words->d, &pq_q) || pq_q < pq_p ||
            !g7x_parser_kept_valid ||
            g7x_parser_kept_p != pq_p || g7x_parser_kept_q != pq_q ||
            (ptr->cmd->words & ~(GCODE_WORD_P | GCODE_WORD_Q | GCODE_WORD_R |
                                 GCODE_WORD_F)) ||
            (ptr->cmd->groups & ~GCODE_GROUP_MOTION) ||
            ptr->new_state->groups.distance_mode != G90 ||
            ptr->new_state->groups.plane != G18 ||
            ptr->new_state->groups.feedrate_mode != G94) {
            proto_info("G7X G70 refused (range not collected)");
            if (!has_p || !has_q || pq_q < pq_p)
                g7x_parser_set_refusal("G70 needs P and Q");
            else if (ptr->new_state->groups.distance_mode != G90 ||
                     ptr->new_state->groups.plane != G18 ||
                     ptr->new_state->groups.feedrate_mode != G94)
                g7x_parser_set_refusal("G70 needs G18 G90 G94");
            else
                g7x_parser_set_refusal("G70 range not run yet");
            ptr->cmd->group_extended = 0;
            ptr->cmd->groups = 0;
            ptr->cmd->words = 0;
            memset(ptr->words, 0, sizeof(*ptr->words));
            *(ptr->error) = STATUS_INVALID_STATEMENT;
            return EVENT_HANDLED;
        }
        if (feed <= 0.0f)
            feed = 120.0f;
        if (!(retract > 0.0f))
            retract = 1.0f;
        result = g7x_stream_begin_finish(&g7x_parser_stream,
                                         &g7x_parser_kept_region,
                                         retract,
                                         feed);
        if (result != G7X_OK) {
            proto_info("G7X G70 replay failed: %s", g7x_result_text(result));
            g7x_parser_clear_state();
            *(ptr->error) = STATUS_INVALID_STATEMENT;
            return EVENT_HANDLED;
        }
        proto_info("G7X G70 finish P%u Q%u", (unsigned)pq_p, (unsigned)pq_q);
        memcpy(&g7x_parser_runner_state, ptr->new_state,
               sizeof(g7x_parser_runner_state));
        g7x_parser_runner_active = true;
        proto_print("[MSG:G7X generated blocks queued]\r\n");
        ptr->cmd->group_extended = 0;
        ptr->cmd->groups = 0;
        ptr->cmd->words = 0;
        memset(ptr->words, 0, sizeof(*ptr->words));
        *(ptr->error) = STATUS_OK;
        return EVENT_HANDLED;
    }

    if (ptr->cmd->group_extended == G7X_EXTENDED_CODE) {
        /* Second block of a Fanuc two-line header: merge it into the open
           cycle instead of starting a second one. */
        if (g7x_parser_continuation) {
            g7x_cycle_t cycle = g7x_parser_stream.region.cycle;
            bool has_p = CHECKFLAG(ptr->cmd->words, GCODE_WORD_P);
            bool has_q = CHECKFLAG(ptr->cmd->words, GCODE_WORD_Q);
            bool numbered = false;
            uint32_t pq_p = 0u;
            uint32_t pq_q = 0u;
            float retract = CHECKFLAG(ptr->cmd->words, GCODE_WORD_R) ?
                            ptr->words->r : g7x_parser_stream.region.retract;
            float x_allow = g7x_parser_stream.region.x_allow;
            float z_allow = g7x_parser_stream.region.z_allow;
            float feed = g7x_parser_stream.feed;
            float doc = g7x_parser_pending_doc_set ?
                        g7x_parser_pending_doc : g7x_parser_stream.doc;
            g7x_result_t result;

            g7x_parser_continuation = false;
            if (has_p || has_q) {
                if (!has_p || !has_q ||
                    !g7x_parser_pq_number(ptr->words->p, &pq_p) ||
                    !g7x_parser_pq_number(ptr->words->d, &pq_q) ||
                    pq_q < pq_p) {
                    g7x_parser_clear_state();
                    g7x_parser_set_refusal("needs P and Q numbers");
                    *(ptr->error) = STATUS_INVALID_STATEMENT;
                    return EVENT_HANDLED;
                }
                numbered = true;
            }
            if (CHECKFLAG(ptr->cmd->words, GCODE_WORD_X))
                x_allow = ptr->words->xyzabc[AXIS_X];
            else if (g7x_parser_pending_x_allow_set)
                x_allow = g7x_parser_pending_x_allow;
            if (CHECKFLAG(ptr->cmd->words, GCODE_WORD_Z))
                z_allow = ptr->words->xyzabc[AXIS_Z];
            else if (g7x_parser_pending_z_allow_set)
                z_allow = g7x_parser_pending_z_allow;
            if (CHECKFLAG(ptr->cmd->words, GCODE_WORD_F))
                feed = ptr->words->f;

            result = g7x_stream_begin_parsed(&g7x_parser_stream, cycle, retract,
                                             x_allow, z_allow, feed, doc);
            if (result != G7X_OK) {
                g7x_parser_clear_state();
                g7x_parser_set_refusal("cycle values out of range");
                *(ptr->error) = STATUS_INVALID_STATEMENT;
                return EVENT_HANDLED;
            }
            if (numbered) {
                /* The *second* line carries the range (the usual Fanuc form), so
                   it is this block that arms it. */
                g7x_parser_region_active = false;
                g7x_parser_pq_armed = true;
                g7x_parser_pq_collecting = false;
                g7x_parser_pq_p = pq_p;
                g7x_parser_pq_q = pq_q;
                g7x_parser_pq_last = 0u;
            }
            /* With no range on this line the *first* line already decided what
               the cycle is: it either armed a numbered range (waiting for its
               N(P) row) or opened a region. Overriding that here was the bug a
               Fanuc header pair with the range on the *first* line ran into:
               `region_active` and `pq_armed` were both left set, so the module
               ended the contour at the N(Q) row and then refused the G80 -
               "invalid parameters or cycle contour" on the last row of a
               perfectly good block (bench, 2026-09-23). */
            ptr->new_state->feedrate = feed *
                (ptr->new_state->groups.units == G20 ? INCH_MM_MULT : 1.0f);
            ptr->new_state->groups.motion = G1;
            g7x_parser_pending_doc = 0.0f;
            g7x_parser_pending_doc_set = false;
            g7x_parser_pending_x_allow = 0.0f;
            g7x_parser_pending_x_allow_set = false;
            g7x_parser_pending_z_allow = 0.0f;
            g7x_parser_pending_z_allow_set = false;
            g7x_parser_pending_corner_kind = G7X_CORNER_NONE;
            g7x_parser_pending_corner_amount = 0.0f;
            ptr->cmd->group_extended = 0;
            ptr->cmd->groups = 0;
            ptr->cmd->words = 0;
            memset(ptr->words, 0, sizeof(*ptr->words));
            proto_print("[MSG:G7X parser collect active]\r\n");
            *(ptr->error) = STATUS_OK;
            return EVENT_HANDLED;
        }
        /* Fanuc/Haas numbered range: P and Q select the profile blocks that
           follow this header instead of an explicit G80 terminator. */
        bool has_p = CHECKFLAG(ptr->cmd->words, GCODE_WORD_P);
        bool has_q = CHECKFLAG(ptr->cmd->words, GCODE_WORD_Q);
        bool numbered = false;
        uint32_t pq_p = 0u;
        uint32_t pq_q = 0u;

        if (has_p || has_q) {
            if (!has_p || !has_q ||
                !g7x_parser_pq_number(ptr->words->p, &pq_p) ||
                !g7x_parser_pq_number(ptr->words->d, &pq_q) ||
                pq_q < pq_p) {
                g7x_parser_set_refusal("needs P and Q numbers");
                *(ptr->error) = STATUS_INVALID_STATEMENT;
                return EVENT_HANDLED;
            }
            numbered = true;
        }
        if (g7x_parser_busy() || ptr->cmd->groups ||
            ptr->new_state->groups.distance_mode != G90 ||
            ptr->new_state->groups.plane != G18 ||
            ptr->new_state->groups.feedrate_mode != G94) {
            if (g7x_parser_busy())
                g7x_parser_set_refusal("a cycle is already running");
            else if (ptr->new_state->groups.distance_mode != G90)
                g7x_parser_set_refusal("cycle needs G90 absolute");
            else if (ptr->new_state->groups.plane != G18)
                g7x_parser_set_refusal("cycle needs G18 XZ plane");
            else if (ptr->new_state->groups.feedrate_mode != G94)
                g7x_parser_set_refusal("cycle needs G94 feed/min");
            else
                g7x_parser_set_refusal("header has other modal words");
            *(ptr->error) = STATUS_INVALID_STATEMENT;
            return EVENT_HANDLED;
        }
        g7x_cycle_t cycle = g7x_parser_pending_cycle == G7X_CYCLE_NONE ?
                            G7X_CYCLE_G71 :
                            g7x_parser_pending_cycle;
        float retract = CHECKFLAG(ptr->cmd->words, GCODE_WORD_R) ? ptr->words->r : 1.0f;
        float x_allow = CHECKFLAG(ptr->cmd->words, GCODE_WORD_X) ? ptr->words->xyzabc[AXIS_X] : 0.0f;
        float z_allow = CHECKFLAG(ptr->cmd->words, GCODE_WORD_Z) ? ptr->words->xyzabc[AXIS_Z] : 0.0f;
        float feed = CHECKFLAG(ptr->cmd->words, GCODE_WORD_F) ? ptr->words->f : 120.0f;
        g7x_result_t result = g7x_stream_begin_parsed(&g7x_parser_stream,
                                                       cycle,
                                                       retract,
                                                       x_allow,
                                                       z_allow,
                                                       feed,
                                                       g7x_parser_pending_doc);

        if (!g7x_parser_pending_doc_set || result != G7X_OK) {
            g7x_parser_set_refusal(!g7x_parser_pending_doc_set
                                       ? "header needs U or W depth"
                                       : "cycle values out of range");
            *(ptr->error) = STATUS_INVALID_STATEMENT;
            return EVENT_HANDLED;
        }

        if (numbered) {
            g7x_parser_region_active = false;
            g7x_parser_pq_armed = true;
            g7x_parser_pq_collecting = false;
            g7x_parser_pq_p = pq_p;
            g7x_parser_pq_q = pq_q;
            g7x_parser_pq_last = 0u;
        } else {
            g7x_parser_region_active = true;
        }
        ptr->new_state->feedrate = feed *
            (ptr->new_state->groups.units == G20 ? INCH_MM_MULT : 1.0f);
        ptr->new_state->groups.motion = G1;
        g7x_parser_pending_cycle = G7X_CYCLE_NONE;
        g7x_parser_pending_doc = 0.0f;
        g7x_parser_pending_doc_set = false;
        g7x_parser_pending_corner_kind = G7X_CORNER_NONE;
        g7x_parser_pending_corner_amount = 0.0f;
        ptr->cmd->group_extended = 0;
        ptr->cmd->groups = 0;
        ptr->cmd->words = 0;
        memset(ptr->words, 0, sizeof(*ptr->words));
        proto_print("[MSG:G7X parser collect active]\r\n");
        *(ptr->error) = STATUS_OK;
        return EVENT_HANDLED;
    }

    /* A numbered range stays armed until the N(P) block arrives. Blocks before
       it are ordinary program text and must not be swallowed by the collector. */
    if (g7x_parser_pq_armed && !g7x_parser_region_active) {
        bool motion = CHECKFLAG(ptr->cmd->groups, GCODE_GROUP_MOTION) ||
                      CHECKFLAG(ptr->cmd->words, GCODE_XZPLANE_AXIS);
        if (!motion || g7x_parser_block_number(ptr->words) != g7x_parser_pq_p)
            return EVENT_CONTINUE;
        if (ptr->new_state->groups.motion != G0 &&
            ptr->new_state->groups.motion != G1 &&
            ptr->new_state->groups.motion != G2 &&
            ptr->new_state->groups.motion != G3) {
            /* The N(P) block must be a contour move. */
            g7x_parser_clear_state();
            g7x_parser_set_refusal("N(P) must be a move");
            *(ptr->error) = STATUS_INVALID_STATEMENT;
            return EVENT_HANDLED;
        }
        g7x_parser_pq_armed = false;
        g7x_parser_pq_collecting = true;
        g7x_parser_region_active = true;
        g7x_parser_pq_last = g7x_parser_pq_p - 1u;
    }

    /* A profile row may carry the words the *finish* is to run with: Fanuc
       reads F/S/T from the profile, not from the roughing header. So the row
       takes them instead of refusing the line. F is kept on the element (the
       finish cut emits it where the program wrote it); S and T are read and
       not acted on here - the cycle runs at the speed and tool the program
       established before it, and a stock-removal cycle must not change either
       in the middle of a cut. Both are swallowed with the rest of the row, so
       nothing reaches the exec stage with them. */
    if (g7x_parser_region_active &&
        ((ptr->cmd->groups & ~GCODE_GROUP_MOTION) || ptr->cmd->group_extended ||
        (ptr->cmd->words & ~(GCODE_WORD_X | GCODE_WORD_Z | GCODE_WORD_F |
                             GCODE_WORD_R | GCODE_WORD_I | GCODE_WORD_K |
                             GCODE_WORD_S | GCODE_WORD_T)))) {
        g7x_parser_clear_state();
        g7x_parser_set_refusal("extra words in profile row");
        *(ptr->error) = STATUS_INVALID_STATEMENT;
        return EVENT_HANDLED;
    }
    if (g7x_parser_region_active &&
        (CHECKFLAG(ptr->cmd->groups, GCODE_GROUP_MOTION) ||
         CHECKFLAG(ptr->cmd->words, GCODE_XZPLANE_AXIS))) {
        if (ptr->new_state->groups.motion == G80) {
            if (g7x_parser_pq_collecting) {
                /* A numbered range ends at N(Q), never at G80. */
                g7x_parser_clear_state();
                g7x_parser_set_refusal("range ends at N(Q)");
                *(ptr->error) = STATUS_INVALID_STATEMENT;
                return EVENT_HANDLED;
            }
            g7x_parser_region_complete(ptr);
        } else if (ptr->new_state->groups.motion == G0 ||
                   ptr->new_state->groups.motion == G1 ||
                   ptr->new_state->groups.motion == G2 ||
                   ptr->new_state->groups.motion == G3) {
            g7x_result_t result;
            bool has_r = CHECKFLAG(ptr->cmd->words, GCODE_WORD_R);
            bool has_i = CHECKFLAG(ptr->cmd->words, GCODE_WORD_I);
            bool has_k = CHECKFLAG(ptr->cmd->words, GCODE_WORD_K);
            uint32_t number = g7x_parser_block_number(ptr->words);

            if (g7x_parser_pq_collecting) {
                /* Numbered profile blocks must increase and stay inside the
                   range; unnumbered rows are ordinary contour rows. */
                if (number != 0u &&
                    (number <= g7x_parser_pq_last || number > g7x_parser_pq_q)) {
                    g7x_parser_clear_state();
                    g7x_parser_set_refusal("P..Q numbers must rise");
                    *(ptr->error) = STATUS_INVALID_STATEMENT;
                    return EVENT_HANDLED;
                }
                if (number != 0u)
                    g7x_parser_pq_last = number;
            }

            if (ptr->new_state->groups.motion == G0)
                cmd = G7X_CONTOUR_RAPID;
            else if (ptr->new_state->groups.motion == G1)
                cmd = G7X_CONTOUR_LINE;
            else if (ptr->new_state->groups.motion == G2)
                cmd = G7X_CONTOUR_ARC_CW;
            else
                cmd = G7X_CONTOUR_ARC_CCW;

            result = g7x_stream_add_parsed(&g7x_parser_stream,
                                           cmd,
                                           ptr->words->xyzabc[AXIS_X],
                                           CHECKFLAG(ptr->cmd->words, GCODE_WORD_X),
                                           ptr->words->xyzabc[AXIS_Z],
                                           CHECKFLAG(ptr->cmd->words, GCODE_WORD_Z),
                                           ptr->words->r,
                                           has_r,
                                           ptr->words->ijk[0],
                                           has_i,
                                           ptr->words->ijk[2],
                                           has_k,
                                           g7x_parser_pending_corner_kind,
                                           g7x_parser_pending_corner_amount,
                                           ptr->words->f,
                                           CHECKFLAG(ptr->cmd->words, GCODE_WORD_F),
                                           NULL);
            g7x_parser_pending_corner_kind = G7X_CORNER_NONE;
            g7x_parser_pending_corner_amount = 0.0f;
            if (result != G7X_OK) {
                char why[48];

                proto_info("G7X parser collect failed: %s", g7x_result_text(result));
                g7x_parser_clear_state();
                snprintf(why, sizeof(why), "contour rejected: %s",
                         g7x_result_text(result));
                g7x_parser_set_refusal(why);
                *(ptr->error) = STATUS_INVALID_STATEMENT;
            } else if (g7x_parser_pq_collecting) {
                if (number != 0u) {
                    g7x_parser_retain_contour(number, ptr->new_state->groups.motion,
                                              ptr->words, ptr->cmd);
                    if (number == g7x_parser_pq_q) {
                        g7x_parser_region_complete(ptr);
                        ptr->new_state->groups.motion_mantissa = 0;
                        return EVENT_HANDLED;
                    }
                }
            }
            ptr->cmd->groups = 0;
            ptr->cmd->words = 0;
            memset(ptr->words, 0, sizeof(*ptr->words));
            ptr->new_state->groups.motion_mantissa = 0;
        }
    }

    return EVENT_CONTINUE;
}

static uint8_t g7x_parser_run_pending(parser_state_t *state)
{
    bool done = false;
    uint8_t error = STATUS_OK;
    /* Like core canned cycles, stay inside the source command until every
       generated block has been accepted. cnc_dotasks keeps realtime/UI alive;
       it does not read another source command. */
    while (!done) {
        if (!cnc_dotasks() || cnc_get_exec_state(EXEC_KILL | EXEC_CANCELING) ||
            !g7x_parser_busy()) {
            error = STATUS_SYSTEM_GC_LOCK;
            break;
        }
        if (cnc_get_exec_state(EXEC_HOLD | EXEC_DOOR))
            continue;
#if G7X_ENABLE_G76 && defined(G33_ENCODER)
        if (g7x_parser_thread_runner_active)
            error = g7x_parser_exec_thread(&g7x_parser_runner_state, 1u, &done);
        else
#endif
            error = g7x_parser_exec_stream(&g7x_parser_runner_state, 1u, &done);
        if (error != STATUS_OK)
            break;
    }
    if (error == STATUS_OK) {
        /* Cancel modal cycle motion; retain the caller's units/feed/tool. */
        state->groups.motion = G80;
        state->groups.motion_mantissa = 0;
        proto_print("[MSG:G7X generated blocks done]\r\n");
    }
    g7x_parser_clear_state();
    return error;
}

bool g7x_execute_pending(void *args)
{
    gcode_exec_args_t *ptr = (gcode_exec_args_t *)args;
    if (g7x_parser_runner_active
#if G7X_ENABLE_G76 && defined(G33_ENCODER)
        || g7x_parser_thread_runner_active
#endif
    ) {
        *(ptr->error) = g7x_parser_run_pending(ptr->new_state);
        return EVENT_HANDLED;
    }
    return EVENT_CONTINUE;
}

bool g7x_parse_error(void *args)
{
    (void)args;
    g7x_parser_clear_state();
    return EVENT_CONTINUE;
}

bool g7x_reset(void *args)
{
    (void)args;
    g7x_parser_numbered_history_reset();
    g7x_parser_kept_valid = false;
    g7x_parser_set_refusal("");
    g7x_parser_clear_state();
    return EVENT_CONTINUE;
}
#endif

#ifndef G7X_HOST_TEST
DECL_MODULE(g7x)
{
#ifdef ENABLE_PARSER_MODULES
    proto_print("[MSG:G7X module init]\r\n");
    ADD_EVENT_LISTENER(gcode_parse, g7x_parse);
    ADD_EVENT_LISTENER(gcode_exec_modifier, g7x_exec_modifier);
    ADD_EVENT_LISTENER(parser_reset, g7x_reset);
    ADD_EVENT_LISTENER(cnc_parse_cmd_error, g7x_parse_error);
    ADD_EVENT_LISTENER(gcode_execute_pending, g7x_execute_pending);
#endif
}
#endif
