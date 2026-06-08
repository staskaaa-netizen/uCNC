/* G71/G72 generator support.
 * The stream API remains the SIM/preview bridge. Parser integration below is
 * currently a safe shell: it recognizes G71/G72 regions and suppresses contour
 * source rows so they do not execute as normal motion. It does not yet feed
 * generated rough/finish moves back through the live parser.
 */
#include "g71_g72.h"

#include "../../module.h"
#ifdef ENABLE_PARSER_MODULES
#include "../../cnc.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ENABLE_PARSER_MODULES
#define G7X_EXTENDED_CODE EXTENDED_MCODE(710)

static bool g7x_parser_region_active;
static g7x_cycle_t g7x_parser_cycle;
static g7x_cycle_t g7x_parser_pending_cycle;

bool g7x_parse(void *args);
bool g7x_exec_modifier(void *args);
bool g7x_exec(void *args);
bool g7x_reset(void *args);

CREATE_EVENT_LISTENER(gcode_parse, g7x_parse);
CREATE_EVENT_LISTENER(gcode_exec_modifier, g7x_exec_modifier);
CREATE_EVENT_LISTENER(gcode_exec, g7x_exec);
CREATE_EVENT_LISTENER(parser_reset, g7x_reset);
#endif

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

bool g7x_command_is(const char *line, const char *cmd)
{
    size_t n;
    char next;

    if (!line || !cmd)
        return false;

    while (*line == ' ' || *line == '\t')
        line++;

    n = strlen(cmd);
    if (strncmp(line, cmd, n) != 0)
        return false;

    next = line[n];
    if (next == 0 || next == ' ' || next == '\t')
        return true;

    if ((cmd[0] == 'G' || cmd[0] == 'M') && next >= '0' && next <= '9')
        return false;

    return true;
}

bool g7x_get_field_text(const char *line, const char *key, char *out, size_t out_sz)
{
    const char *p;
    const char *b;
    const char *e;
    size_t key_len;
    size_t len;
    bool saw_token = false;

    if (!line || !key || !out || out_sz == 0)
        return false;

    out[0] = 0;
    key_len = strlen(key);
    p = line;

    while (*p)
    {
        bool is_command_token;

        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;

        is_command_token = !saw_token;
        b = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        e = p;
        saw_token = true;

        if (!is_command_token &&
            (size_t)(e - b) > key_len &&
            strncmp(b, key, key_len) == 0 &&
            b[key_len] != '_')
        {
            const char *v = b + key_len;
            const char *vend = e;

            if (*v == '=')
                v++;
            if (memchr(v, '{', (size_t)(vend - v)) ||
                memchr(v, '}', (size_t)(vend - v)))
                return false;

            len = (size_t)(vend - v);
            if (len >= out_sz)
                len = out_sz - 1;
            if (len)
                memcpy(out, v, len);
            out[len] = 0;
            return true;
        }
    }

    return false;
}

bool g7x_get_field_float(const char *line, const char *key, float *out)
{
    char text[32];

    if (!out || !g7x_get_field_text(line, key, text, sizeof(text)) || !text[0] || text[0] == '(')
        return false;

    *out = strtof(text, NULL);
    return true;
}

bool g7x_modal_apply_line(g7x_modal_t *modal, const char *line)
{
    bool changed = false;

    if (!modal || !line)
        return false;

    if (g7x_command_is(line, "G20"))
    {
        modal->units = G7X_UNITS_INCH;
        changed = true;
    }
    else if (g7x_command_is(line, "G21"))
    {
        modal->units = G7X_UNITS_MM;
        changed = true;
    }

    if (g7x_command_is(line, "G90"))
    {
        modal->distance = G7X_DISTANCE_ABSOLUTE;
        changed = true;
    }
    else if (g7x_command_is(line, "G91"))
    {
        modal->distance = G7X_DISTANCE_INCREMENTAL;
        changed = true;
    }

    return changed;
}

g7x_cycle_t g7x_cycle_from_line(const char *line)
{
    if (g7x_command_is(line, "G71"))
        return G7X_CYCLE_G71;
    if (g7x_command_is(line, "G72"))
        return G7X_CYCLE_G72;
    return G7X_CYCLE_NONE;
}

g7x_contour_cmd_t g7x_contour_cmd_from_line(const char *line)
{
    if (g7x_command_is(line, "G80"))
        return G7X_CONTOUR_END;
    if (g7x_command_is(line, "G0"))
        return G7X_CONTOUR_RAPID;
    if (g7x_command_is(line, "G1"))
        return G7X_CONTOUR_LINE;
    if (g7x_command_is(line, "G2"))
        return G7X_CONTOUR_ARC_CW;
    if (g7x_command_is(line, "G3"))
        return G7X_CONTOUR_ARC_CCW;
    return G7X_CONTOUR_NONE;
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
        default:
            return false;
    }

    if (profile)
        *profile = p;
    return true;
}

static bool g7x_field_float(const char *line, char key, float *out)
{
    char k[2];

    k[0] = key;
    k[1] = '\0';
    return g7x_get_field_float(line, k, out);
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
                                int *ccw)
{
    g7x_v2_t a;
    g7x_v2_t b;
    float dot;
    float angle;
    float trim;
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

    if (t1)
        *t1 = g7x_v2_add(p1, g7x_v2_mul(a, trim));
    if (t2)
        *t2 = g7x_v2_add(p1, g7x_v2_mul(b, trim));
    if (center) {
        g7x_v2_t bis;
        float bis_len;
        if (!g7x_v2_norm(g7x_v2_add(a, b), &bis))
            return false;
        bis_len = amount / sinf(angle * 0.5f);
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
                              g7x_contour_element_t *insert)
{
    g7x_v2_t p0;
    g7x_v2_t p1;
    g7x_v2_t p2;
    g7x_v2_t t1;
    g7x_v2_t t2;
    g7x_v2_t center;
    g7x_corner_kind_t kind;
    float amount;
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
    p0.x = prev->d * 0.5f;
    p0.z = prev->z;
    p1.x = corner->d * 0.5f;
    p1.z = corner->z;
    p2.x = next->d * 0.5f;
    p2.z = next->z;

    if (!g7x_corner_tangents(p0, p1, p2, amount, &t1, &t2, &center, &ccw))
        return false;

    memset(insert, 0, sizeof(*insert));
    corner->d = t1.x * 2.0f;
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
        insert->r = amount;
    } else {
        insert->kind = G7X_SEGMENT_LINE;
    }
    insert->d = t2.x * 2.0f;
    insert->z = t2.z;
    return true;
}

static void g7x_expand_corners(g7x_contour_region_t *region)
{
    unsigned i;

    if (!region || region->count < 3)
        return;

    for (i = 1; i + 1 < region->count && region->count < G7X_MAX_CONTOUR_ELEMENTS; i++) {
        g7x_contour_element_t insert;
        unsigned move;

        if (!g7x_expand_corner(&region->elements[i - 1], &region->elements[i], &region->elements[i + 1], &insert))
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

g7x_result_t g7x_stream_begin(g7x_stream_t *stream, const char *cycle_line)
{
    g7x_cycle_profile_t profile;

    if (!stream || !cycle_line)
        return G7X_BAD_FIELD;

    g7x_stream_reset(stream);
    stream->region.cycle = g7x_cycle_from_line(cycle_line);
    if (!g7x_cycle_profile(stream->region.cycle, &profile))
        return G7X_UNSUPPORTED;

    stream->region.active = 1;
    stream->feed = 120.0f;
    stream->region.retract = 1.0f;
    (void)g7x_field_float(cycle_line, 'R', &stream->region.retract);
    (void)g7x_field_float(cycle_line, 'X', &stream->region.x_allow);
    (void)g7x_field_float(cycle_line, 'Z', &stream->region.z_allow);
    (void)g7x_field_float(cycle_line, 'F', &stream->feed);
    if (!g7x_field_float(cycle_line, profile.rough_doc_word, &stream->doc))
        return G7X_BAD_FIELD;
    stream->doc = fabsf(stream->doc);
    if (stream->doc <= 0.0001f)
        return G7X_BAD_FIELD;

    stream->active = true;
    return G7X_OK;
}

static g7x_result_t g7x_stream_prepare(g7x_stream_t *stream)
{
    unsigned i;

    if (!stream || stream->region.count < 2)
        return G7X_BAD_FIELD;

    g7x_expand_corners(&stream->region);

    stream->start_x = stream->region.elements[0].d;
    stream->start_z = stream->region.elements[0].z;
    stream->min_x = stream->max_x = stream->start_x;
    stream->min_z = stream->max_z = stream->start_z;
    for (i = 1; i < stream->region.count; i++) {
        const g7x_contour_element_t *el = &stream->region.elements[i];
        if (el->d < stream->min_x) stream->min_x = el->d;
        if (el->d > stream->max_x) stream->max_x = el->d;
        if (el->z < stream->min_z) stream->min_z = el->z;
        if (el->z > stream->max_z) stream->max_z = el->z;
    }

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

g7x_result_t g7x_stream_add_line(g7x_stream_t *stream, const char *line, bool *done)
{
    g7x_contour_cmd_t cmd;
    g7x_contour_element_t *el;

    if (done)
        *done = false;
    if (!stream || !stream->active || !line)
        return G7X_BAD_FIELD;

    cmd = g7x_contour_cmd_from_line(line);
    if (cmd == G7X_CONTOUR_END) {
        if (done)
            *done = true;
        return g7x_stream_prepare(stream);
    }
    if (cmd != G7X_CONTOUR_RAPID &&
        cmd != G7X_CONTOUR_LINE &&
        cmd != G7X_CONTOUR_ARC_CW &&
        cmd != G7X_CONTOUR_ARC_CCW)
        return G7X_OK;
    if (stream->region.count >= G7X_MAX_CONTOUR_ELEMENTS)
        return G7X_BAD_FIELD;

    el = &stream->region.elements[stream->region.count++];
    memset(el, 0, sizeof(*el));
    el->kind = cmd == G7X_CONTOUR_ARC_CW || cmd == G7X_CONTOUR_ARC_CCW ? G7X_SEGMENT_ARC : G7X_SEGMENT_LINE;
    el->cw = cmd == G7X_CONTOUR_ARC_CW;
    el->gcode_cw = el->cw;
    if (!g7x_field_float(line, 'X', &el->d) ||
        !g7x_field_float(line, 'Z', &el->z))
        return G7X_BAD_FIELD;
    if (el->kind == G7X_SEGMENT_ARC && !g7x_field_float(line, 'R', &el->r))
        return G7X_BAD_FIELD;
    if (g7x_field_float(line, 'C', &el->outgoing_amount))
        el->outgoing_kind = G7X_CORNER_CHMF;
    if (g7x_field_float(line, 'R', &el->outgoing_amount) && el->kind == G7X_SEGMENT_LINE)
        el->outgoing_kind = G7X_CORNER_RND;

    return G7X_OK;
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
        if (x < min_x || x > max_x || fabsf(b->d - a->d) < 0.0001f)
            continue;
        hit = a->z + ((x - a->d) / (b->d - a->d)) * (b->z - a->z);
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
        if (z < min_z || z > max_z || fabsf(b->z - a->z) < 0.0001f)
            continue;
        hit = a->d + ((z - a->z) / (b->z - a->z)) * (b->d - a->d);
        if (!found || hit < best) {
            best = hit;
            found = true;
        }
    }
    if (found)
        *x = best;
    return found;
}

g7x_step_result_t g7x_stream_next(g7x_stream_t *stream, char *out, size_t out_sz)
{
    if (!stream || !stream->active || !out || out_sz == 0)
        return G7X_STEP_ERROR;

    if (!stream->started) {
        stream->started = true;
        snprintf(out, out_sz, "(NC %s generated)",
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
                    snprintf(out, out_sz, "(G72 rough Z%.3f)", stream->pass);
                    return G7X_STEP_LINE;
                case 1:
                    snprintf(out, out_sz, "G0 X%.3f Z%.3f", stream->max_x + stream->region.retract, stream->pass);
                    return G7X_STEP_LINE;
                case 2:
                    snprintf(out, out_sz, "G1 X%.3f F%.3f", x_hit + stream->region.x_allow, stream->feed);
                    return G7X_STEP_LINE;
                default:
                    stream->stage = 0;
                    stream->pass += (float)stream->dir * stream->doc;
                    snprintf(out, out_sz, "G0 X%.3f", stream->max_x + stream->region.retract);
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
                    snprintf(out, out_sz, "(G71 rough X%.3f)", stream->pass);
                    return G7X_STEP_LINE;
                case 1:
                    snprintf(out, out_sz, "G0 X%.3f Z%.3f", stream->pass + stream->region.retract, stream->start_z);
                    return G7X_STEP_LINE;
                case 2:
                    snprintf(out, out_sz, "G1 X%.3f F%.3f", stream->pass, stream->feed);
                    return G7X_STEP_LINE;
                case 3:
                    snprintf(out, out_sz, "G1 Z%.3f F%.3f", z_hit - ((float)stream->dir * stream->region.z_allow), stream->feed);
                    return G7X_STEP_LINE;
                case 4:
                    snprintf(out, out_sz, "G0 X%.3f", stream->pass + stream->region.retract);
                    return G7X_STEP_LINE;
                default:
                    stream->stage = 0;
                    stream->pass -= stream->doc;
                    snprintf(out, out_sz, "G0 Z%.3f", stream->start_z);
                    return G7X_STEP_LINE;
                }
            }
        }
    }

    if (stream->finish_i == 0) {
        stream->finish_i++;
        snprintf(out, out_sz, "(G7x finish contour)");
        return G7X_STEP_LINE;
    }
    if (stream->finish_i <= stream->region.count) {
        const g7x_contour_element_t *el = &stream->region.elements[stream->finish_i - 1u];
        stream->finish_i++;
        if (el->kind == G7X_SEGMENT_ARC && el->has_center)
            snprintf(out, out_sz, "%s X%.3f Z%.3f I%.3f K%.3f", el->gcode_cw ? "G2" : "G3", el->d, el->z, el->i, el->k);
        else if (el->kind == G7X_SEGMENT_ARC)
            snprintf(out, out_sz, "%s X%.3f Z%.3f R%.3f", el->gcode_cw ? "G2" : "G3", el->d, el->z, el->r);
        else
            snprintf(out, out_sz, "G1 X%.3f Z%.3f", el->d, el->z);
        return G7X_STEP_LINE;
    }

    g7x_stream_reset(stream);
    return G7X_STEP_DONE;
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

#ifdef ENABLE_PARSER_MODULES
bool g7x_parse(void *args)
{
    gcode_parse_args_t *ptr = (gcode_parse_args_t *)args;

    if (!ptr || !ptr->error || !ptr->cmd) {
        return EVENT_CONTINUE;
    }

    if (ptr->word == 'G' && (ptr->code == 71 || ptr->code == 72)) {
        if (ptr->cmd->group_extended != 0 || ptr->cmd->groups != 0) {
            *(ptr->error) = STATUS_GCODE_MODAL_GROUP_VIOLATION;
            return EVENT_HANDLED;
        }
        ptr->cmd->group_extended = G7X_EXTENDED_CODE;
        g7x_parser_pending_cycle = ptr->code == 72 ? G7X_CYCLE_G72 : G7X_CYCLE_G71;
        *(ptr->error) = STATUS_OK;
        return EVENT_HANDLED;
    }

    if (ptr->cmd->group_extended == G7X_EXTENDED_CODE &&
        (ptr->word == 'U' || ptr->word == 'W')) {
        *(ptr->error) = STATUS_OK;
        return EVENT_HANDLED;
    }

    return EVENT_CONTINUE;
}

bool g7x_exec_modifier(void *args)
{
    gcode_exec_args_t *ptr = (gcode_exec_args_t *)args;

    if (!ptr || !ptr->cmd || !ptr->new_state || !ptr->words) {
        return EVENT_CONTINUE;
    }

    if (g7x_parser_region_active &&
        CHECKFLAG(ptr->cmd->groups, GCODE_GROUP_MOTION)) {
        if (ptr->new_state->groups.motion == G80) {
            g7x_parser_region_active = false;
            g7x_parser_cycle = G7X_CYCLE_NONE;
        } else if (ptr->new_state->groups.motion == G0 ||
                   ptr->new_state->groups.motion == G1 ||
                   ptr->new_state->groups.motion == G2 ||
                   ptr->new_state->groups.motion == G3) {
            ptr->cmd->words = 0;
            memset(ptr->words, 0, sizeof(*ptr->words));
            ptr->new_state->groups.motion = G80;
            ptr->new_state->groups.motion_mantissa = 0;
        }
    }

    return EVENT_CONTINUE;
}

bool g7x_exec(void *args)
{
    gcode_exec_args_t *ptr = (gcode_exec_args_t *)args;

    if (!ptr || !ptr->cmd || !ptr->error ||
        ptr->cmd->group_extended != G7X_EXTENDED_CODE) {
        return EVENT_CONTINUE;
    }

    g7x_parser_region_active = true;
    g7x_parser_cycle = g7x_parser_pending_cycle == G7X_CYCLE_NONE ?
                       G7X_CYCLE_G71 :
                       g7x_parser_pending_cycle;
    g7x_parser_pending_cycle = G7X_CYCLE_NONE;
    proto_print("[MSG:G7X parser shell active]\r\n");
    *(ptr->error) = STATUS_OK;
    return EVENT_HANDLED;
}

bool g7x_reset(void *args)
{
    (void)args;
    g7x_parser_region_active = false;
    g7x_parser_cycle = G7X_CYCLE_NONE;
    g7x_parser_pending_cycle = G7X_CYCLE_NONE;
    return EVENT_CONTINUE;
}
#endif

DECL_MODULE(g71_g72)
{
#ifdef ENABLE_PARSER_MODULES
    ADD_EVENT_LISTENER(gcode_parse, g7x_parse);
    ADD_EVENT_LISTENER(gcode_exec_modifier, g7x_exec_modifier);
    ADD_EVENT_LISTENER(gcode_exec, g7x_exec);
    ADD_EVENT_LISTENER(parser_reset, g7x_reset);
#endif
}
