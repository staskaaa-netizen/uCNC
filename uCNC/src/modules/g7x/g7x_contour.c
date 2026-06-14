#include "g7x_contour.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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

    while (*p) {
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
            b[key_len] != '_' &&
            !(b[key_len] >= 'A' && b[key_len] <= 'Z') &&
            !(b[key_len] >= 'a' && b[key_len] <= 'z')) {
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

    if (g7x_command_is(line, "G20")) {
        modal->units = G7X_UNITS_INCH;
        changed = true;
    } else if (g7x_command_is(line, "G21")) {
        modal->units = G7X_UNITS_MM;
        changed = true;
    }

    if (g7x_command_is(line, "G90")) {
        modal->distance = G7X_DISTANCE_ABSOLUTE;
        changed = true;
    } else if (g7x_command_is(line, "G91")) {
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
    if (g7x_command_is(line, "G76"))
        return G7X_CYCLE_G76;
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

static bool g7x_contour_field_float(const char *line, char key, float *out)
{
    char k[2];

    k[0] = key;
    k[1] = '\0';
    return g7x_get_field_float(line, k, out);
}

g7x_result_t g7x_stream_begin(g7x_stream_t *stream, const char *cycle_line)
{
    g7x_cycle_profile_t profile;
    g7x_cycle_t cycle;
    float retract = 1.0f;
    float x_allow = 0.0f;
    float z_allow = 0.0f;
    float feed = 120.0f;
    float doc = 0.0f;

    if (!stream || !cycle_line)
        return G7X_BAD_FIELD;

    cycle = g7x_cycle_from_line(cycle_line);
    if (!g7x_cycle_profile(cycle, &profile))
        return G7X_UNSUPPORTED;

    (void)g7x_contour_field_float(cycle_line, 'R', &retract);
    if (g7x_contour_field_float(cycle_line, 'X', &x_allow))
        x_allow *= 0.5f;
    (void)g7x_contour_field_float(cycle_line, 'Z', &z_allow);
    (void)g7x_contour_field_float(cycle_line, 'F', &feed);
    if (!g7x_contour_field_float(cycle_line, profile.rough_doc_word, &doc))
        return G7X_BAD_FIELD;

    return g7x_stream_begin_parsed(stream, cycle, retract, x_allow, z_allow, feed, doc);
}

g7x_result_t g7x_stream_add_line(g7x_stream_t *stream, const char *line, bool *done)
{
    g7x_contour_cmd_t cmd;
    g7x_corner_kind_t corner_kind = G7X_CORNER_NONE;
    float x = 0.0f;
    float z = 0.0f;
    float r = 0.0f;
    float i = 0.0f;
    float k = 0.0f;
    float corner_amount = 0.0f;
    bool has_x;
    bool has_z;
    bool has_r;
    bool has_i;
    bool has_k;

    if (done)
        *done = false;
    if (!stream || !stream->active || !line)
        return G7X_BAD_FIELD;

    cmd = g7x_contour_cmd_from_line(line);
    if (cmd == G7X_CONTOUR_END)
        return g7x_stream_add_parsed(stream, cmd, 0.0f, false, 0.0f, false,
                                     0.0f, false, 0.0f, false, 0.0f, false,
                                     G7X_CORNER_NONE, 0.0f, done);
    if (cmd != G7X_CONTOUR_RAPID &&
        cmd != G7X_CONTOUR_LINE &&
        cmd != G7X_CONTOUR_ARC_CW &&
        cmd != G7X_CONTOUR_ARC_CCW)
        return G7X_OK;

    has_x = g7x_contour_field_float(line, 'X', &x);
    if (has_x)
        x *= 0.5f;
    has_z = g7x_contour_field_float(line, 'Z', &z);
    has_r = g7x_contour_field_float(line, 'R', &r);
    has_i = g7x_contour_field_float(line, 'I', &i);
    has_k = g7x_contour_field_float(line, 'K', &k);

    if (cmd == G7X_CONTOUR_LINE) {
        if (g7x_contour_field_float(line, 'C', &corner_amount)) {
            corner_kind = G7X_CORNER_CHMF;
        } else if (has_r && r > 0.0001f) {
            corner_kind = G7X_CORNER_RND;
            corner_amount = r;
        }
    }

    return g7x_stream_add_parsed(stream, cmd, x, has_x, z, has_z, r, has_r,
                                 i, has_i, k, has_k, corner_kind,
                                 corner_amount, done);
}

static bool g7x_field_float2(const char *line, const char *a, const char *b, float *out)
{
    return g7x_get_field_float(line, a, out) || g7x_get_field_float(line, b, out);
}

static bool g7x_field_float3(const char *line, const char *a, const char *b, const char *c, float *out)
{
    return g7x_get_field_float(line, a, out) ||
           g7x_get_field_float(line, b, out) ||
           g7x_get_field_float(line, c, out);
}

g7x_result_t g7x_thread_begin(g7x_thread_stream_t *stream,
                              const char *line,
                              float default_start_diameter,
                              float default_clearance)
{
    float d_start = default_start_diameter;
    float d_end = 0.0f;
    float z1 = 0.0f;
    float z2 = 0.0f;
    float pitch = 0.0f;
    float thread_height = 0.0f;
    float first_cut = 0.0f;
    float min_cut = 0.0f;
    float finish_allowance = 0.0f;
    float pq_scale = 1.0f;
    float taper = 0.0f;
    float spring_value = 0.0f;
    int spring_passes = 0;

    if (!stream || !line || !g7x_command_is(line, "G76"))
        return G7X_BAD_FIELD;

    if (!g7x_get_field_float(line, "F", &pitch) ||
        !g7x_get_field_float(line, "X", &d_end) ||
        !g7x_get_field_float(line, "Z", &z2) ||
        !g7x_get_field_float(line, "P", &thread_height) ||
        !g7x_get_field_float(line, "Q", &first_cut))
        return G7X_BAD_FIELD;

    (void)g7x_field_float2(line, "START_X", "X_START", &d_start);
    if (!g7x_field_float2(line, "Z1", "Z_START", &z1))
        z1 = 0.0f;
    (void)g7x_get_field_float(line, "SCALE", &pq_scale);
    (void)g7x_field_float2(line, "MIN_Q", "QMIN", &min_cut);
    (void)g7x_field_float2(line, "FINISH_R", "FINISH_ALLOW", &finish_allowance);
    (void)g7x_field_float3(line, "D", "TAPER", "D_TAPER", &taper);
    if (g7x_field_float2(line, "H", "SPRING", &spring_value) && spring_value > 0.0f)
        spring_passes = (int)(spring_value + 0.5f);
    if (pq_scale <= 0.0f)
        return G7X_BAD_FIELD;
    if (min_cut <= 0.0f)
        min_cut = first_cut;

    return g7x_thread_begin_semantic(stream,
                                     d_start,
                                     d_end,
                                     z1,
                                     z2,
                                     pitch,
                                     thread_height * pq_scale,
                                     first_cut * pq_scale,
                                     min_cut * pq_scale,
                                     finish_allowance,
                                     default_clearance,
                                     taper,
                                     spring_passes,
                                     0,
                                     0);
}
