/* G71/G72 generator and parser module.
 * This module is intentionally self-contained: NC/LeanCam screens may use the
 * stream API for preview, but G71/G72 execution must work without any UI module
 * loaded. Parser integration owns the modal region, suppresses source contour
 * rows, and feeds generated rough/finish motion back through the live parser.
 */
#include "g7x.h"

#ifndef G7X_HOST_TEST
#include "../../cnc.h"
#include "../../module.h"
#include "../g7_g8/parser_g7_g8.h"
#endif

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ENABLE_PARSER_MODULES) && !defined(G7X_HOST_TEST)
#define G7X_EXTENDED_CODE EXTENDED_MCODE(710)
#define G7X_PARSER_BURST_BLOCKS 1u

static bool g7x_parser_region_active;
static bool g7x_parser_runner_active;
static g7x_cycle_t g7x_parser_pending_cycle;
static g7x_stream_t g7x_parser_stream;
static parser_state_t g7x_parser_runner_state;
static float g7x_parser_pending_doc;
static bool g7x_parser_pending_doc_set;
static g7x_corner_kind_t g7x_parser_pending_corner_kind;
static float g7x_parser_pending_corner_amount;

bool g7x_parse(void *args);
bool g7x_exec_modifier(void *args);
bool g7x_reset(void *args);
bool g7x_dotasks(void *args);

CREATE_EVENT_LISTENER(gcode_parse, g7x_parse);
CREATE_EVENT_LISTENER(gcode_exec_modifier, g7x_exec_modifier);
CREATE_EVENT_LISTENER(parser_reset, g7x_reset);
CREATE_EVENT_LISTENER(cnc_dotasks, g7x_dotasks);
#endif

#if defined(ENABLE_PARSER_MODULES) && !defined(G7X_HOST_TEST)
static void g7x_parser_clear_state(void)
{
    g7x_parser_region_active = false;
    g7x_parser_runner_active = false;
    g7x_parser_pending_cycle = G7X_CYCLE_NONE;
    g7x_parser_pending_doc = 0.0f;
    g7x_parser_pending_doc_set = false;
    g7x_parser_pending_corner_kind = G7X_CORNER_NONE;
    g7x_parser_pending_corner_amount = 0.0f;
    g7x_stream_reset(&g7x_parser_stream);
    memset(&g7x_parser_runner_state, 0, sizeof(g7x_parser_runner_state));
}
#endif

bool g7x_parser_busy(void)
{
#if defined(ENABLE_PARSER_MODULES) && !defined(G7X_HOST_TEST)
    return g7x_parser_region_active || g7x_parser_runner_active;
#else
    return false;
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
        case G7X_CYCLE_G76:
            p.pass_axis = G7X_AXIS_X;
            p.cut_axis = G7X_AXIS_Z;
            p.contour_monotonic_axis = G7X_AXIS_Z;
            p.rough_doc_word = 'J';
            p.name = "G76";
            break;
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
    max_a = g7x_v2_len(g7x_v2_sub(p0, p1)) * 0.45f;
    max_b = g7x_v2_len(g7x_v2_sub(p2, p1)) * 0.45f;
    if (trim > max_a)
        trim = max_a;
    if (trim > max_b)
        trim = max_b;
    if (trim <= 0.0001f)
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

#if defined(ENABLE_PARSER_MODULES) && !defined(G7X_HOST_TEST)
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

static void g7x_expand_corners(g7x_contour_region_t *region, bool x_is_radius)
{
    unsigned i;

    if (!region || region->count < 3)
        return;

    for (i = 1; i + 1 < region->count && region->count < G7X_MAX_CONTOUR_ELEMENTS; i++) {
        g7x_contour_element_t insert;
        unsigned move;

        if (!g7x_expand_corner(&region->elements[i - 1], &region->elements[i], &region->elements[i + 1], &insert, x_is_radius))
            continue;

        for (move = region->count; move > i + 1; move--)
            region->elements[move] = region->elements[move - 1];
        region->elements[i + 1] = insert;
        region->count++;
        i++;
    }
}

void g7x_stream_reset(g7x_stream_t *stream)
{
    if (stream)
        memset(stream, 0, sizeof(*stream));
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

static g7x_result_t g7x_stream_prepare(g7x_stream_t *stream, bool x_is_radius)
{
    unsigned i;
    int z_dir = 0;
    int x_dir = 0;

    if (!stream || stream->region.count < 2)
        return G7X_BAD_FIELD;

    g7x_expand_corners(&stream->region, x_is_radius);

    stream->start_x = stream->region.elements[0].d;
    stream->start_z = stream->region.elements[0].z;
    stream->min_x = stream->max_x = stream->start_x;
    stream->min_z = stream->max_z = stream->start_z;
    for (i = 1; i < stream->region.count; i++) {
        const g7x_contour_element_t *prev = &stream->region.elements[i - 1];
        const g7x_contour_element_t *el = &stream->region.elements[i];
        float dz = el->z - prev->z;
        float dx = el->d - prev->d;
        if (el->d < stream->min_x) stream->min_x = el->d;
        if (el->d > stream->max_x) stream->max_x = el->d;
        if (el->z < stream->min_z) stream->min_z = el->z;
        if (el->z > stream->max_z) stream->max_z = el->z;
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

    if ((stream->region.cycle == G7X_CYCLE_G71 && !z_dir) ||
        (stream->region.cycle == G7X_CYCLE_G72 && !x_dir))
        return G7X_UNSUPPORTED;

    stream->dir = stream->start_z > ((stream->min_z + stream->max_z) * 0.5f) ? -1 : 1;
    if (stream->region.cycle == G7X_CYCLE_G72) {
        stream->pass = stream->start_z + ((float)stream->dir * stream->doc);
        stream->final_pass = stream->dir < 0 ?
                             stream->min_z + stream->region.z_allow :
                             stream->max_z - stream->region.z_allow;
    } else {
        stream->pass = stream->max_x - stream->doc;
        stream->final_pass = stream->min_x + stream->region.x_allow;
        stream->dir = stream->start_z > stream->region.elements[stream->region.count - 1].z ? -1 : 1;
    }
    stream->started = false;
    stream->finish = false;
    stream->stage = 0;
    stream->finish_i = 0;
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
    if (!has_x || !has_z || stream->region.count >= G7X_MAX_CONTOUR_ELEMENTS)
        return G7X_BAD_FIELD;

    el = &stream->region.elements[stream->region.count++];
    memset(el, 0, sizeof(*el));
    el->kind = cmd == G7X_CONTOUR_ARC_CW || cmd == G7X_CONTOUR_ARC_CCW ? G7X_SEGMENT_ARC : G7X_SEGMENT_LINE;
    el->cw = cmd == G7X_CONTOUR_ARC_CW;
    el->gcode_cw = el->cw;
    el->d = x;
    el->z = z;
    el->source_line = stream->pending_source_line;
    if (el->kind == G7X_SEGMENT_ARC) {
        if (has_i && has_k) {
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

g7x_step_result_t g7x_stream_next_event(g7x_stream_t *stream, g7x_event_t *event)
{
    if (!stream || !stream->active || !event)
        return G7X_STEP_ERROR;

    if (!stream->started) {
        stream->started = true;
        g7x_event_comment(event,
                          "NC %s generated",
                          stream->region.cycle == G7X_CYCLE_G72 ? "G72" : "G71");
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
                    g7x_event_motion(event, 0, true, stream->max_x + stream->region.retract, true, stream->pass, false, 0.0f);
                    return G7X_STEP_LINE;
                case 2:
                    g7x_event_motion(event, 1, true, x_hit + stream->region.x_allow, false, 0.0f, true, stream->feed);
                    return G7X_STEP_LINE;
                default:
                    stream->stage = 0;
                    stream->pass += (float)stream->dir * stream->doc;
                    g7x_event_motion(event, 0, true, stream->max_x + stream->region.retract, false, 0.0f, false, 0.0f);
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
                    g7x_event_motion(event, 0, true, stream->pass + stream->region.retract, true, stream->start_z, false, 0.0f);
                    return G7X_STEP_LINE;
                case 2:
                    g7x_event_motion(event, 1, true, stream->pass, false, 0.0f, true, stream->feed);
                    return G7X_STEP_LINE;
                case 3:
                    g7x_event_motion(event, 1, false, 0.0f, true, z_hit - ((float)stream->dir * stream->region.z_allow), true, stream->feed);
                    return G7X_STEP_LINE;
                case 4:
                    g7x_event_motion(event, 0, true, stream->pass + stream->region.retract, false, 0.0f, false, 0.0f);
                    return G7X_STEP_LINE;
                default:
                    stream->stage = 0;
                    stream->pass -= stream->doc;
                    g7x_event_motion(event, 0, false, 0.0f, true, stream->start_z, false, 0.0f);
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
    if (stream->finish_i <= stream->region.count) {
        const g7x_contour_element_t *el = &stream->region.elements[stream->finish_i - 1u];
        stream->finish_i++;
        memset(event, 0, sizeof(*event));
        event->type = G7X_EVENT_MOTION;
        event->motion.motion = el->kind == G7X_SEGMENT_ARC ? (el->gcode_cw ? 2u : 3u) : 1u;
        event->motion.source_line = el->source_line;
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
    if (stream->finish_i == stream->region.count + 1u) {
        float x;
        float z;
        stream->finish_i++;
        g7x_return_clearance_point(stream, &x, &z);
        g7x_event_motion(event, 0, true, x, false, 0.0f, false, 0.0f);
        return G7X_STEP_LINE;
    }
    if (stream->finish_i == stream->region.count + 2u) {
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
    *x = stream->max_x + stream->region.retract;
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
        default: return "unknown";
    }
}

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

void g7x_thread_reset(g7x_thread_stream_t *stream)
{
    if (stream)
        memset(stream, 0, sizeof(*stream));
}

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
    float depth;

    if (!stream)
        return G7X_BAD_FIELD;

    g7x_thread_reset(stream);

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
    if (pass_count == 0 && g7x_too_many_steps(depth, doc))
        return G7X_BAD_FIELD;
    if (pass_count == 0)
        pass_count = (int)ceilf(depth / doc);
    if (pass_count <= 0)
        pass_count = 1;

    stream->d_start = d_start;
    stream->d_end = d_end;
    stream->depth = depth;
    stream->doc = doc;
    stream->pitch = pitch;
    stream->z1 = z1;
    stream->z2 = z2;
    stream->taper = taper;
    stream->z_span = z2 - z1;
    stream->degression = degression;
    stream->pass_count = pass_count;
    stream->spring_left = spring_passes;
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
}

static bool g7x_thread_prepare_next_pass(g7x_thread_stream_t *stream)
{
    float pass_depth;
    float z_shift = 0.0f;

    if (!stream)
        return false;

    if (stream->pass < stream->pass_count) {
        float t;
        stream->pass++;
        t = (float)stream->pass / (float)stream->pass_count;
        pass_depth = stream->depth *
                     (stream->strategy ? powf(t, 1.0f / stream->degression) : t);
        if (pass_depth > stream->depth)
            pass_depth = stream->depth;
        if (pass_depth <= stream->last_depth)
            pass_depth = stream->depth;
        stream->last_depth = pass_depth;
        stream->pass_x1 = stream->d_start + ((stream->d_end < stream->d_start) ? -pass_depth : pass_depth);
    } else if (stream->spring_left > 0) {
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

g7x_step_result_t g7x_thread_next(g7x_thread_stream_t *stream, char *out, size_t out_sz)
{
    if (!stream || !stream->active || !out || out_sz == 0)
        return G7X_STEP_ERROR;

    for (;;) {
        switch (stream->stage++) {
        case 0:
            (void)snprintf(out, out_sz,
                           "(G76 D %.3f X %.3f P %.3f DOC %.3f R %.3f N %d)",
                           stream->d_start,
                           stream->d_end,
                           stream->pitch,
                           stream->doc,
                           stream->degression,
                           stream->pass_count);
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
            if (stream->pass <= stream->pass_count) {
                (void)snprintf(out, out_sz, "(THREAD pass %d X%.3f Z%.3f)",
                               stream->pass, stream->pass_x1, stream->pass_z1);
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
            proto_info("G7X EXEC ERROR %u", (unsigned)error);
            return error;
        }

        memcpy(base_state, &state, sizeof(*base_state));
        emitted++;
    }

    return STATUS_OK;
}

bool g7x_parse(void *args)
{
    gcode_parse_args_t *ptr = (gcode_parse_args_t *)args;

    if (!ptr || !ptr->error || !ptr->cmd) {
        return EVENT_CONTINUE;
    }

    if (ptr->word == 'G' && (ptr->code == 71 || ptr->code == 72)) {
        proto_info("G7X parse G%u", (unsigned)ptr->code);
        if (g7x_parser_region_active ||
            ptr->cmd->group_extended != 0) {
            *(ptr->error) = STATUS_GCODE_MODAL_GROUP_VIOLATION;
            return EVENT_HANDLED;
        }
        ptr->cmd->groups = 0;
        ptr->cmd->words = 0;
        ptr->cmd->group_extended = G7X_EXTENDED_CODE;
        g7x_parser_pending_cycle = ptr->code == 72 ? G7X_CYCLE_G72 : G7X_CYCLE_G71;
        g7x_parser_pending_doc = 0.0f;
        g7x_parser_pending_doc_set = false;
        *(ptr->error) = STATUS_OK;
        return EVENT_HANDLED;
    }

    if (ptr->cmd->group_extended == G7X_EXTENDED_CODE &&
        (ptr->word == 'U' || ptr->word == 'W')) {
        g7x_parser_pending_doc = ptr->value;
        g7x_parser_pending_doc_set = true;
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

    if (!ptr || !ptr->cmd || !ptr->new_state || !ptr->words || !ptr->error) {
        return EVENT_CONTINUE;
    }

    if (ptr->cmd->group_extended == G7X_EXTENDED_CODE) {
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
            *(ptr->error) = STATUS_INVALID_STATEMENT;
            return EVENT_HANDLED;
        }

        g7x_parser_region_active = true;
        ptr->new_state->feedrate = feed;
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

    if (g7x_parser_region_active &&
        CHECKFLAG(ptr->cmd->groups, GCODE_GROUP_MOTION)) {
        if (ptr->new_state->groups.motion == G80) {
            bool done = false;
            g7x_result_t result = g7x_stream_add_parsed(&g7x_parser_stream,
                                                        G7X_CONTOUR_END,
                                                        0.0f, false,
                                                        0.0f, false,
                                                        0.0f, false,
                                                        0.0f, false,
                                                        0.0f, false,
                                                        G7X_CORNER_NONE,
                                                        0.0f,
                                                        &done);
            g7x_parser_region_active = false;
            if (result != G7X_OK) {
                proto_info("G7X parser collect failed: %s", g7x_result_text(result));
                g7x_parser_clear_state();
                *(ptr->error) = STATUS_INVALID_STATEMENT;
            } else {
                proto_info("G7X region ready count=%u", (unsigned)g7x_parser_stream.region.count);
                memcpy(&g7x_parser_runner_state, ptr->new_state, sizeof(g7x_parser_runner_state));
                g7x_parser_runner_active = true;
                proto_print("[MSG:G7X generated blocks queued]\r\n");
            }
            ptr->cmd->groups = 0;
            ptr->cmd->words = 0;
            memset(ptr->words, 0, sizeof(*ptr->words));
            *(ptr->error) = STATUS_OK;
        } else if (ptr->new_state->groups.motion == G0 ||
                   ptr->new_state->groups.motion == G1 ||
                   ptr->new_state->groups.motion == G2 ||
                   ptr->new_state->groups.motion == G3) {
            g7x_result_t result;
            bool has_r = CHECKFLAG(ptr->cmd->words, GCODE_WORD_R);
            bool has_i = CHECKFLAG(ptr->cmd->words, GCODE_WORD_I);
            bool has_k = CHECKFLAG(ptr->cmd->words, GCODE_WORD_K);

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
                                           NULL);
            g7x_parser_pending_corner_kind = G7X_CORNER_NONE;
            g7x_parser_pending_corner_amount = 0.0f;
            if (result != G7X_OK) {
                proto_info("G7X parser collect failed: %s", g7x_result_text(result));
                g7x_parser_clear_state();
                *(ptr->error) = STATUS_INVALID_STATEMENT;
            }
            ptr->cmd->groups = 0;
            ptr->cmd->words = 0;
            memset(ptr->words, 0, sizeof(*ptr->words));
            ptr->new_state->groups.motion = G80;
            ptr->new_state->groups.motion_mantissa = 0;
        }
    }

    return EVENT_CONTINUE;
}

bool g7x_dotasks(void *args)
{
    bool done = false;
    uint8_t error;

    (void)args;
    if (!g7x_parser_runner_active) {
        return EVENT_CONTINUE;
    }
    if (planner_get_buffer_freeblocks() < 2u) {
        return EVENT_CONTINUE;
    }

    error = g7x_parser_exec_stream(&g7x_parser_runner_state, G7X_PARSER_BURST_BLOCKS, &done);
    if (error != STATUS_OK) {
        proto_info("G7X generated blocks failed: %u", (unsigned)error);
        g7x_parser_clear_state();
        return EVENT_CONTINUE;
    }

    parser_set_state_from_module(&g7x_parser_runner_state);
    if (done) {
        g7x_parser_runner_active = false;
        proto_print("[MSG:G7X generated blocks done]\r\n");
    }
    return EVENT_CONTINUE;
}

bool g7x_reset(void *args)
{
    (void)args;
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
    ADD_EVENT_LISTENER(cnc_dotasks, g7x_dotasks);
#endif
}
#endif
