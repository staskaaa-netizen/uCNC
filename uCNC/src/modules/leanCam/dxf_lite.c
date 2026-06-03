#include "dxf_lite.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    double x;
    double y;
    double bulge;
    int have_x;
    int have_y;
} dxf_lite_vertex_t;

typedef enum {
    DXF_ENTITY_NONE = 0,
    DXF_ENTITY_LINE,
    DXF_ENTITY_ARC,
    DXF_ENTITY_CIRCLE,
    DXF_ENTITY_LWPOLYLINE,
    DXF_ENTITY_POLYLINE,
    DXF_ENTITY_VERTEX,
    DXF_ENTITY_IGNORE
} dxf_lite_entity_t;

typedef struct {
    dxf_lite_entity_t entity;
    int in_entities;
    int saw_entities;
    int old_poly_active;
    int closed;
    double x1;
    double y1;
    double x2;
    double y2;
    double cx;
    double cy;
    double r;
    double a1;
    double a2;
    int have_x1;
    int have_y1;
    int have_x2;
    int have_y2;
    int have_cx;
    int have_cy;
    int have_r;
    int have_a1;
    int have_a2;
    dxf_lite_vertex_t verts[DXF_LITE_MAX_POLY_VERTS];
    unsigned vert_count;
    dxf_lite_vertex_t vertex;
} dxf_lite_state_t;

static char *dxf_trim(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s))
        s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

static int dxf_read_pair(dxf_lite_reader_t *reader, int *code, char *value, size_t value_len)
{
    char code_line[64];
    char *endp;
    int r;

    r = reader->read_line(reader->user, code_line, sizeof(code_line));
    if (r <= 0)
        return r;
    r = reader->read_line(reader->user, value, value_len);
    if (r <= 0)
        return DXF_LITE_ERR_MALFORMED;

    *code = (int)strtol(dxf_trim(code_line), &endp, 10);
    (void)endp;
    memmove(value, dxf_trim(value), strlen(dxf_trim(value)) + 1u);
    return 1;
}

static void dxf_warn(const dxf_lite_callbacks_t *cb, void *user, const char *msg)
{
    if (cb && cb->warning)
        cb->warning(user, msg);
}

static int dxf_add_vertex(dxf_lite_state_t *st, const dxf_lite_vertex_t *v)
{
    if (!v->have_x || !v->have_y)
        return DXF_LITE_ERR_MALFORMED;
    if (st->vert_count >= DXF_LITE_MAX_POLY_VERTS)
        return DXF_LITE_ERR_LIMIT;
    st->verts[st->vert_count++] = *v;
    return DXF_LITE_OK;
}

static void dxf_emit_bulge(const dxf_lite_callbacks_t *cb, void *user,
                           const dxf_lite_vertex_t *a,
                           const dxf_lite_vertex_t *b)
{
    double dx = b->x - a->x;
    double dy = b->y - a->y;
    double chord = sqrt((dx * dx) + (dy * dy));
    double theta = 4.0 * atan(a->bulge);
    double r;
    double h;
    double mx;
    double my;
    double nx;
    double ny;
    double cx;
    double cy;

    if (chord <= 0.0)
        return;
    if (fabs(a->bulge) < 1e-12) {
        if (cb && cb->line)
            cb->line(user, a->x, a->y, b->x, b->y);
        return;
    }

    r = chord / (2.0 * sin(fabs(theta) * 0.5));
    h = r * cos(fabs(theta) * 0.5);
    mx = (a->x + b->x) * 0.5;
    my = (a->y + b->y) * 0.5;
    nx = -dy / chord;
    ny = dx / chord;
    if (a->bulge < 0.0) {
        nx = -nx;
        ny = -ny;
    }
    cx = mx + (nx * h);
    cy = my + (ny * h);
    if (cb && cb->arc)
        cb->arc(user, a->x, a->y, b->x, b->y, cx, cy, a->bulge > 0.0);
}

static int dxf_emit_polyline(dxf_lite_state_t *st, const dxf_lite_callbacks_t *cb, void *user)
{
    unsigned i;

    if (st->vert_count < 2)
        return st->vert_count == 0 ? DXF_LITE_OK : DXF_LITE_ERR_MALFORMED;

    for (i = 0; i + 1u < st->vert_count; ++i)
        dxf_emit_bulge(cb, user, &st->verts[i], &st->verts[i + 1u]);
    if (st->closed)
        dxf_emit_bulge(cb, user, &st->verts[st->vert_count - 1u], &st->verts[0]);
    return DXF_LITE_OK;
}

static int dxf_finish_entity(dxf_lite_state_t *st, const dxf_lite_callbacks_t *cb, void *user)
{
    const double pi = 3.14159265358979323846;
    double a1;
    double a2;
    int r = DXF_LITE_OK;

    switch (st->entity) {
        case DXF_ENTITY_LINE:
            if (!st->have_x1 || !st->have_y1 || !st->have_x2 || !st->have_y2)
                return DXF_LITE_ERR_MALFORMED;
            if (cb && cb->line)
                cb->line(user, st->x1, st->y1, st->x2, st->y2);
            break;
        case DXF_ENTITY_ARC:
            if (!st->have_cx || !st->have_cy || !st->have_r || !st->have_a1 || !st->have_a2)
                return DXF_LITE_ERR_MALFORMED;
            a1 = st->a1 * pi / 180.0;
            a2 = st->a2 * pi / 180.0;
            if (cb && cb->arc)
                cb->arc(user,
                        st->cx + cos(a1) * st->r,
                        st->cy + sin(a1) * st->r,
                        st->cx + cos(a2) * st->r,
                        st->cy + sin(a2) * st->r,
                        st->cx,
                        st->cy,
                        1);
            break;
        case DXF_ENTITY_CIRCLE:
            if (!st->have_cx || !st->have_cy || !st->have_r)
                return DXF_LITE_ERR_MALFORMED;
            if (cb && cb->circle)
                cb->circle(user, st->cx, st->cy, st->r);
            break;
        case DXF_ENTITY_LWPOLYLINE:
            r = dxf_emit_polyline(st, cb, user);
            break;
        case DXF_ENTITY_VERTEX:
            if (st->old_poly_active)
                r = dxf_add_vertex(st, &st->vertex);
            break;
        default:
            break;
    }

    st->entity = st->old_poly_active ? DXF_ENTITY_POLYLINE : DXF_ENTITY_NONE;
    st->have_x1 = st->have_y1 = st->have_x2 = st->have_y2 = 0;
    st->have_cx = st->have_cy = st->have_r = st->have_a1 = st->have_a2 = 0;
    memset(&st->vertex, 0, sizeof(st->vertex));
    return r;
}

static void dxf_reset_poly(dxf_lite_state_t *st)
{
    st->vert_count = 0;
    st->closed = 0;
    memset(st->verts, 0, sizeof(st->verts));
    memset(&st->vertex, 0, sizeof(st->vertex));
}

static int dxf_start_entity(dxf_lite_state_t *st, const char *name, const dxf_lite_callbacks_t *cb, void *user)
{
    int r;

    if (st->entity == DXF_ENTITY_VERTEX) {
        r = dxf_finish_entity(st, cb, user);
        if (r != DXF_LITE_OK)
            return r;
    } else if (st->entity != DXF_ENTITY_POLYLINE) {
        r = dxf_finish_entity(st, cb, user);
        if (r != DXF_LITE_OK)
            return r;
    }

    if (strcmp(name, "LINE") == 0) {
        st->entity = DXF_ENTITY_LINE;
    } else if (strcmp(name, "ARC") == 0) {
        st->entity = DXF_ENTITY_ARC;
    } else if (strcmp(name, "CIRCLE") == 0) {
        st->entity = DXF_ENTITY_CIRCLE;
    } else if (strcmp(name, "LWPOLYLINE") == 0) {
        dxf_reset_poly(st);
        st->old_poly_active = 0;
        st->entity = DXF_ENTITY_LWPOLYLINE;
    } else if (strcmp(name, "POLYLINE") == 0) {
        dxf_reset_poly(st);
        st->old_poly_active = 1;
        st->entity = DXF_ENTITY_POLYLINE;
    } else if (strcmp(name, "VERTEX") == 0 && st->old_poly_active) {
        memset(&st->vertex, 0, sizeof(st->vertex));
        st->entity = DXF_ENTITY_VERTEX;
    } else if (strcmp(name, "SEQEND") == 0 && st->old_poly_active) {
        r = dxf_emit_polyline(st, cb, user);
        st->old_poly_active = 0;
        st->entity = DXF_ENTITY_NONE;
        if (r != DXF_LITE_OK)
            return r;
    } else {
        if (strcmp(name, "ENDSEC") != 0 && strcmp(name, "EOF") != 0)
            dxf_warn(cb, user, "unsupported entity ignored");
        st->entity = st->old_poly_active ? DXF_ENTITY_POLYLINE : DXF_ENTITY_IGNORE;
    }

    return DXF_LITE_OK;
}

static int dxf_handle_group(dxf_lite_state_t *st, int code, const char *value)
{
    double v = strtod(value, NULL);

    if (st->entity == DXF_ENTITY_LINE) {
        if (code == 10) { st->x1 = v; st->have_x1 = 1; }
        else if (code == 20) { st->y1 = v; st->have_y1 = 1; }
        else if (code == 11) { st->x2 = v; st->have_x2 = 1; }
        else if (code == 21) { st->y2 = v; st->have_y2 = 1; }
    } else if (st->entity == DXF_ENTITY_ARC || st->entity == DXF_ENTITY_CIRCLE) {
        if (code == 10) { st->cx = v; st->have_cx = 1; }
        else if (code == 20) { st->cy = v; st->have_cy = 1; }
        else if (code == 40) { st->r = v; st->have_r = 1; }
        else if (code == 50) { st->a1 = v; st->have_a1 = 1; }
        else if (code == 51) { st->a2 = v; st->have_a2 = 1; }
    } else if (st->entity == DXF_ENTITY_LWPOLYLINE) {
        if (code == 70) {
            st->closed = ((int)v & 1) != 0;
        } else if (code == 10) {
            dxf_lite_vertex_t nv;
            memset(&nv, 0, sizeof(nv));
            if (st->vert_count >= DXF_LITE_MAX_POLY_VERTS)
                return DXF_LITE_ERR_LIMIT;
            nv.x = v;
            nv.have_x = 1;
            st->verts[st->vert_count++] = nv;
        } else if (code == 20 && st->vert_count > 0) {
            st->verts[st->vert_count - 1u].y = v;
            st->verts[st->vert_count - 1u].have_y = 1;
        } else if (code == 42 && st->vert_count > 0) {
            st->verts[st->vert_count - 1u].bulge = v;
        }
    } else if (st->entity == DXF_ENTITY_POLYLINE) {
        if (code == 70)
            st->closed = ((int)v & 1) != 0;
    } else if (st->entity == DXF_ENTITY_VERTEX) {
        if (code == 10) { st->vertex.x = v; st->vertex.have_x = 1; }
        else if (code == 20) { st->vertex.y = v; st->vertex.have_y = 1; }
        else if (code == 42) { st->vertex.bulge = v; }
    }

    return DXF_LITE_OK;
}

int dxf_lite_parse(dxf_lite_reader_t *reader,
                   const dxf_lite_callbacks_t *cb,
                   void *user)
{
    dxf_lite_state_t st;
    char value[128];
    int code;
    int r;
    int last_code = -1;
    char last_value[128] = {0};

    if (!reader || !reader->read_line)
        return DXF_LITE_ERR_ARG;

    memset(&st, 0, sizeof(st));
    while ((r = dxf_read_pair(reader, &code, value, sizeof(value))) > 0) {
        if (last_code == 0 && strcmp(last_value, "SECTION") == 0 && code == 2 && strcmp(value, "ENTITIES") == 0) {
            st.in_entities = 1;
            st.saw_entities = 1;
        }

        if (code == 0) {
            if (strcmp(value, "ENDSEC") == 0) {
                r = dxf_finish_entity(&st, cb, user);
                if (r != DXF_LITE_OK)
                    return r;
                st.in_entities = 0;
                st.entity = DXF_ENTITY_NONE;
            } else if (strcmp(value, "EOF") == 0) {
                r = dxf_finish_entity(&st, cb, user);
                if (r != DXF_LITE_OK)
                    return r;
                return st.saw_entities ? DXF_LITE_OK : DXF_LITE_ERR_MALFORMED;
            } else if (st.in_entities) {
                r = dxf_start_entity(&st, value, cb, user);
                if (r != DXF_LITE_OK)
                    return r;
            }
        } else if (st.in_entities) {
            r = dxf_handle_group(&st, code, value);
            if (r != DXF_LITE_OK)
                return r;
        }

        last_code = code;
        strncpy(last_value, value, sizeof(last_value) - 1u);
        last_value[sizeof(last_value) - 1u] = 0;
    }

    if (r < 0)
        return r;
    return DXF_LITE_ERR_MALFORMED;
}
