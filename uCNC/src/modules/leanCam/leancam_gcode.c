/* LeanCam module contract:
 * Purpose: expand stored G-code-ish LeanCam rows into executable NC/G-code lines.
 * Called by: run/preflight/sim preview code through explicit emit callbacks.
 * Calls into: schema/text/tool catalog helpers and caller-provided line emit callbacks.
 * Owns: generator-local parse/geometry state only; it must not render, poll keys, or perform file I/O directly.
 */
#include "leancam_gcode.h"
#include "leancam_code.h"

#include <stdarg.h>
#include <float.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef LC_GCODE_FEED_MM_MIN
#define LC_GCODE_FEED_MM_MIN 120.0f
#endif

#ifndef LC_GCODE_SPINDLE_RPM
#define LC_GCODE_SPINDLE_RPM 800
#endif



#ifndef LC_GCODE_MAX_PASSES
#define LC_GCODE_MAX_PASSES 500
#endif

#ifndef LC_GCODE_MAX_ABS_VALUE
#define LC_GCODE_MAX_ABS_VALUE 1000000.0f
#endif

#ifndef LC_GCODE_MAX_CONTOUR_ELEMENTS
#define LC_GCODE_MAX_CONTOUR_ELEMENTS 48
#endif

typedef struct
{
    float rough_feed;
    float finish_feed;
    float rough_doc;
    float finish_doc;
    int spindle_rpm;
} lc_cut_ctx_t;

static const lc_gcode_line_options_t lc_default_line_options = { 1, 1 };
static const lc_gcode_line_options_t lc_program_line_options = { 0, 0 };

typedef enum
{
    LC_CORNER_NONE = 0,
    LC_CORNER_RND,
    LC_CORNER_CHMF
} lc_corner_kind_t;

typedef enum
{
    LC_CONTOUR_LINE = 0,
    LC_CONTOUR_ARC
} lc_contour_kind_t;

typedef struct
{
    lc_contour_kind_t kind;
    float d;
    float z;
    float r;
    int cw;
    int gcode_cw;
    float i;
    float k;
    int has_center;
    lc_corner_kind_t outgoing_kind;
    float outgoing_amount;
} lc_contour_element_t;

typedef enum
{
    LC_RAW_NONE = 0,
    LC_RAW_G71,
    LC_RAW_G72
} lc_raw_cycle_t;

typedef struct
{
    int active;
    lc_raw_cycle_t cycle;
    lc_cut_ctx_t cut;
    float retract;
    float x_allow;
    float z_allow;
    lc_contour_element_t elements[LC_GCODE_MAX_CONTOUR_ELEMENTS];
    unsigned count;
} lc_raw_region_t;

static lc_raw_region_t g_raw_region;

typedef struct
{
    lc_raw_region_t raw_region;
} lc_gcode_internal_state_t;

typedef enum
{
    LC_STEP_PHASE_IDLE = 0,
    LC_STEP_PHASE_HEADER,
    LC_STEP_PHASE_BODY,
    LC_STEP_PHASE_DIRECT,
    LC_STEP_PHASE_G71,
    LC_STEP_PHASE_G76,
    LC_STEP_PHASE_FOOTER,
    LC_STEP_PHASE_DONE,
    LC_STEP_PHASE_ERROR
} lc_step_phase_t;

typedef struct
{
    program_t prog;
    int start;
    int end;
    int src;
    int err_line;
    lc_step_phase_t phase;
    unsigned header_idx;
    unsigned footer_idx;
    unsigned direct_idx;
    unsigned direct_count;
    char direct[8][96];
    lc_raw_region_t region;
    lc_raw_region_t effective;
    int g80_line;
    float clearance;
    float xsafe;
    float zsafe;
    float start_x;
    float start_z;
    float min_x;
    float max_x;
    float min_z;
    float max_z;
    float rough_start_x;
    float pass;
    float final_pass;
    int rough_dir;
    int pass_dir;
    int g71_stage;
    int g76_stage;
    int finish_stage;
    unsigned finish_i;
    int g76_pass;
    int g76_pass_count;
    int g76_spring_left;
    int g76_strategy;
    float g76_d_start;
    float g76_d_end;
    float g76_depth;
    float g76_doc;
    float g76_pitch;
    float g76_z1;
    float g76_z2;
    float g76_zsafe;
    float g76_xsafe;
    float g76_taper;
    float g76_z_span;
    float g76_angle_tan;
    float g76_final_z_shift;
    float g76_degression;
    float g76_last_depth;
    float g76_pass_x1;
    float g76_pass_x2;
    float g76_pass_z1;
    float g76_pass_zsafe;
    bool final_pending;
    bool rough_done;
} lc_gcode_stepper_state_t;

typedef char lc_gcode_stepper_size_check[
    sizeof(lc_gcode_stepper_state_t) <= sizeof(lc_gcode_stepper_t) ? 1 : -1
];

typedef char lc_gcode_state_snapshot_size_check[
    sizeof(lc_gcode_internal_state_t) <= sizeof(lc_gcode_state_snapshot_t) ? 1 : -1
];

int leancam_gcode_save_state(lc_gcode_state_snapshot_t *snapshot)
{
    lc_gcode_internal_state_t state;

    if (!snapshot)
        return 0;
    memset(&state, 0, sizeof(state));
    state.raw_region = g_raw_region;
    memset(snapshot, 0, sizeof(*snapshot));
    memcpy(snapshot->bytes, &state, sizeof(state));
    return 1;
}

int leancam_gcode_restore_state(const lc_gcode_state_snapshot_t *snapshot)
{
    lc_gcode_internal_state_t state;

    if (!snapshot)
        return 0;
    memcpy(&state, snapshot->bytes, sizeof(state));
    g_raw_region = state.raw_region;
    return 1;
}

static int lc_command_is(const char *line, const char *cmd)
{
    size_t n;

    if (!line || !cmd)
        return 0;
    while (*line == ' ' || *line == '\t')
        line++;
    n = strlen(cmd);
    return strncmp(line, cmd, n) == 0 &&
           (line[n] == 0 || line[n] == ' ' || line[n] == '\t');
}

static lc_gcode_result_t lc_fail(lc_gcode_result_t r, char *err, unsigned err_len, const char *fmt, ...)
{
    va_list ap;

    if (err && err_len > 0)
    {
        va_start(ap, fmt);
        vsnprintf(err, err_len, fmt ? fmt : "", ap);
        va_end(ap);
        err[err_len - 1] = 0;
    }

    return r;
}

static int lc_float_ok(float v)
{
    return v == v &&
           v <= FLT_MAX &&
           v >= -FLT_MAX &&
           v <= LC_GCODE_MAX_ABS_VALUE &&
           v >= -LC_GCODE_MAX_ABS_VALUE;
}

static int lc_parse_float_text(const char *s, float *out)
{
    char *endp;
    float v;

    if (!s || !out)
        return 0;

    while (*s == ' ')
        s++;
    if (*s == '(' || *s == '*' || *s == 0)
        return 0;

    v = (float)strtod(s, &endp);
    if (endp == s || !lc_float_ok(v))
        return 0;

    while (*endp == ' ')
        endp++;
    if (*endp != 0)
        return 0;

    *out = v;
    return 1;
}

static int lc_too_many_steps(float span, float step)
{
    if (span < 0.0f)
        span = -span;

    return step > 0.0f && (span / step) > (float)LC_GCODE_MAX_PASSES;
}

static float lc_absf(float v)
{
    return v < 0.0f ? -v : v;
}

static float lc_maxf(float a, float b)
{
    return a > b ? a : b;
}

static float lc_minf(float a, float b)
{
    return a < b ? a : b;
}

static float lc_clampf(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

typedef struct
{
    float x;
    float z;
} lc_v2_t;

typedef struct
{
    lc_v2_t t1;
    lc_v2_t t2;
    lc_v2_t c;
    float r;
    int cw;
} lc_corner_arc_t;

static lc_v2_t lc_v2_add(lc_v2_t a, lc_v2_t b)
{
    lc_v2_t r = { a.x + b.x, a.z + b.z };
    return r;
}

static lc_v2_t lc_v2_sub(lc_v2_t a, lc_v2_t b)
{
    lc_v2_t r = { a.x - b.x, a.z - b.z };
    return r;
}

static lc_v2_t lc_v2_mul(lc_v2_t a, float s)
{
    lc_v2_t r = { a.x * s, a.z * s };
    return r;
}

static float lc_v2_dot(lc_v2_t a, lc_v2_t b)
{
    return (a.x * b.x) + (a.z * b.z);
}

static float lc_v2_cross(lc_v2_t a, lc_v2_t b)
{
    return (a.x * b.z) - (a.z * b.x);
}

static float lc_v2_len(lc_v2_t a)
{
    return sqrtf(lc_v2_dot(a, a));
}

static int lc_v2_norm(lc_v2_t a, lc_v2_t *out)
{
    float l = lc_v2_len(a);

    if (!out || l < 0.0001f)
        return 0;

    out->x = a.x / l;
    out->z = a.z / l;
    return 1;
}

static int lc_build_r_corner(lc_v2_t p0,
                             lc_v2_t p1,
                             lc_v2_t p2,
                             float r,
                             lc_corner_arc_t *out)
{
    lc_v2_t p0r = { p0.x * 0.5f, p0.z };
    lc_v2_t p1r = { p1.x * 0.5f, p1.z };
    lc_v2_t p2r = { p2.x * 0.5f, p2.z };
    lc_v2_t a;
    lc_v2_t b;
    lc_v2_t bis;
    lc_v2_t t1;
    lc_v2_t t2;
    lc_v2_t c;
    float len_a = lc_v2_len(lc_v2_sub(p0r, p1r));
    float len_b = lc_v2_len(lc_v2_sub(p2r, p1r));
    float dot;
    float theta;
    float half;
    float tan_half;
    float sin_half;
    float tangent;
    float center_dist;
    float e1;
    float e2;
    float tangent_e1;
    float tangent_e2;

    if (!out || r <= 0.0f || len_a < 0.0001f || len_b < 0.0001f)
        return 0;
    if (!lc_v2_norm(lc_v2_sub(p0r, p1r), &a) ||
        !lc_v2_norm(lc_v2_sub(p2r, p1r), &b))
        return 0;

    dot = lc_clampf(lc_v2_dot(a, b), -1.0f, 1.0f);
    if (lc_absf(dot) > 0.999f)
        return 0;

    theta = acosf(dot);
    half = theta * 0.5f;
    tan_half = tanf(half);
    sin_half = sinf(half);
    if (lc_absf(tan_half) < 0.0001f || lc_absf(sin_half) < 0.0001f)
        return 0;

    tangent = r / tan_half;
    if (tangent > len_a + 0.0001f || tangent > len_b + 0.0001f)
        return 0;

    t1 = lc_v2_add(p1r, lc_v2_mul(a, tangent));
    t2 = lc_v2_add(p1r, lc_v2_mul(b, tangent));
    if (!lc_v2_norm(lc_v2_add(a, b), &bis))
        return 0;

    center_dist = r / sin_half;
    c = lc_v2_add(p1r, lc_v2_mul(bis, center_dist));

    e1 = lc_absf(lc_v2_len(lc_v2_sub(t1, c)) - r);
    e2 = lc_absf(lc_v2_len(lc_v2_sub(t2, c)) - r);
    tangent_e1 = lc_absf(lc_v2_dot(lc_v2_sub(t1, c), lc_v2_sub(p1r, p0r)));
    tangent_e2 = lc_absf(lc_v2_dot(lc_v2_sub(t2, c), lc_v2_sub(p2r, p1r)));
    if (e1 > 0.01f || e2 > 0.01f || tangent_e1 > 0.01f || tangent_e2 > 0.01f)
        return 0;

    out->t1 = (lc_v2_t){ t1.x * 2.0f, t1.z };
    out->t2 = (lc_v2_t){ t2.x * 2.0f, t2.z };
    out->c = (lc_v2_t){ c.x * 2.0f, c.z };
    out->r = r;
    out->cw = lc_v2_cross(lc_v2_sub(t1, c), lc_v2_sub(t2, c)) < 0.0f;
    return 1;
}

static int lc_emit(lc_gcode_send_fn send, void *user, const char *fmt, ...)
{
    char line[96];
    va_list ap;
    int n;

    if (!send)
        return 0;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (n < 0 || n >= (int)sizeof(line))
        return 0;

    return send(line, user) ? 1 : 0;
}

static int lc_get_field_text(const char *line, const char *name, char *out, unsigned out_len)
{
    if (!line || !name || !out || out_len == 0)
        return 0;
    out[0] = 0;
    return lc_code_get_field_text(line, name, out, (size_t)out_len) ? 1 : 0;
}

static int lc_field_float(const char *line, const char *name, float *out)
{
    char buf[32];
    if (!lc_get_field_text(line, name, buf, sizeof(buf)))
        return 0;

    return lc_parse_float_text(buf, out);
}

static int lc_field_float2(const char *line, const char *a, const char *b, float *out)
{
    if (lc_field_float(line, a, out))
        return 1;
    return lc_field_float(line, b, out);
}

static int lc_field_float3(const char *line, const char *a, const char *b, const char *c, float *out)
{
    if (lc_field_float(line, a, out))
        return 1;
    if (lc_field_float(line, b, out))
        return 1;
    return lc_field_float(line, c, out);
}

static int lc_field_text3(const char *line, const char *a, const char *b, const char *c, char *out, unsigned out_len)
{
    return lc_get_field_text(line, a, out, out_len) ||
           lc_get_field_text(line, b, out, out_len) ||
           lc_get_field_text(line, c, out, out_len);
}

static int lc_field_text2(const char *line, const char *a, const char *b, char *out, unsigned out_len)
{
    return lc_get_field_text(line, a, out, out_len) ||
           lc_get_field_text(line, b, out, out_len);
}

static int lc_field_tool_diameter(const char *line, float *out)
{
    return lc_field_float3(line, "TD", "TOOL_DIAMETER", "TOOL_DIA", out) ||
           lc_field_float3(line, "DIA", "DIAMETER", "D", out);
}

static int lc_get_tool_number(const char *line, const char *tool, int *out)
{
    float t;

    if (!out)
        return 0;

    if ((lc_field_float(line, "T", &t) || lc_field_float(tool, "T", &t)) && t > 0.0f)
    {
        *out = (int)t;
        return 1;
    }

    return 0;
}

static int lc_get_tool_diameter(const char *line, const char *tool, float *out)
{
    return (out &&
            (lc_field_tool_diameter(line, out) || lc_field_tool_diameter(tool, out)) &&
            *out > 0.0f);
}

static void lc_sanitize_cut_ctx(lc_cut_ctx_t *ctx)
{
    if (!ctx)
        return;

    if (ctx->rough_feed <= 0.0f) ctx->rough_feed = LC_GCODE_FEED_MM_MIN;
    if (ctx->finish_feed <= 0.0f) ctx->finish_feed = ctx->rough_feed;
    if (ctx->rough_doc < 0.0f) ctx->rough_doc = -ctx->rough_doc;
    if (ctx->finish_doc < 0.0f) ctx->finish_doc = -ctx->finish_doc;
    if (ctx->rough_doc <= 0.0f) ctx->rough_doc = 2.0f;
    if (ctx->spindle_rpm <= 0) ctx->spindle_rpm = LC_GCODE_SPINDLE_RPM;
}

static void lc_override_ctx_from_line(const char *line, lc_cut_ctx_t *ctx)
{
    float rpm;

    if (!line || !ctx)
        return;

    (void)lc_field_float3(line, "F", "R_FEED", "ROUGH_FEED", &ctx->rough_feed);
    (void)lc_field_float2(line, "F_R", "DOC_FEED_R", &ctx->rough_feed);
    (void)lc_field_float(line, "FEED", &ctx->rough_feed);
    (void)lc_field_float2(line, "FIN_FEED", "FINISH_FEED", &ctx->finish_feed);
    (void)lc_field_float2(line, "F_F", "FINISH_FEED_F", &ctx->finish_feed);
    (void)lc_field_float3(line, "DOC", "R_DOC", "ROUGH_DOC", &ctx->rough_doc);
    (void)lc_field_float2(line, "DOC_R", "ROUGH_DEPTH", &ctx->rough_doc);
    (void)lc_field_float(line, "ROUGH_DEPTH_OF_CUT", &ctx->rough_doc);
    (void)lc_field_float2(line, "FIN_DOC", "FINISH_DEPTH_OF_CUT", &ctx->finish_doc);
    (void)lc_field_float2(line, "DOC_F", "FINISH_DEPTH", &ctx->finish_doc);
    if (lc_field_float3(line, "S", "RPM", "SPINDLE_RPM", &rpm) && rpm > 0.0f)
        ctx->spindle_rpm = (int)rpm;

    lc_sanitize_cut_ctx(ctx);
}

static lc_gcode_result_t lc_setup_clearance(const char *cycle,
                                            const char *setup,
                                            float *out,
                                            char *err,
                                            unsigned err_len)
{
    if (!setup || !lc_field_float(setup, "CLR", out))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: missing/bad SETUP.CLR", cycle);
    if (*out < 0.0f)
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: SETUP.CLR must be >= 0", cycle);
    return LC_GCODE_OK;
}

static void lc_read_cut_ctx(const char *tool, lc_cut_ctx_t *ctx)
{
    if (!ctx)
        return;

    ctx->rough_feed = LC_GCODE_FEED_MM_MIN;
    ctx->finish_feed = LC_GCODE_FEED_MM_MIN * 0.5f;
    ctx->rough_doc = 2.0f;
    ctx->finish_doc = 0.5f;
    ctx->spindle_rpm = LC_GCODE_SPINDLE_RPM;

    lc_override_ctx_from_line(tool, ctx);
    lc_sanitize_cut_ctx(ctx);
}

static int lc_emit_modal_header(lc_gcode_send_fn send, void *user)
{
    if (!lc_emit(send, user, "G21")) return 0;
    if (!lc_emit(send, user, "G90")) return 0;
    if (!lc_emit(send, user, "G18"))  return 0;
    if (!lc_emit(send, user, "G7"))  return 0;
    return 1;
}

static int lc_emit_cycle_preamble(lc_gcode_send_fn send,
                                  void *user,
                                  const lc_cut_ctx_t *ctx,
                                  const lc_gcode_line_options_t *options)
{
    if (!options)
        options = &lc_default_line_options;

    if (options->emit_modal_header && !lc_emit_modal_header(send, user))
        return 0;

    if (!lc_emit(send, user, "S%d M3", ctx ? ctx->spindle_rpm : LC_GCODE_SPINDLE_RPM)) return 0;
    return 1;
}

static lc_gcode_result_t lc_run_drill(const char *line,
                                      const char *tool,
                                      const lc_gcode_line_options_t *options,
                                      lc_gcode_send_fn send,
                                      void *user,
                                      char *err,
                                      unsigned err_len)
{
    float z1 = 0.0f;
    float depth = 0.0f;
    float peck = 0.0f;
    float feed;
    float target = 0.0f;
    int has_target = 0;
    int passes = 0;
    int tool_no = 0;
    int has_tool_no;
    float tool_d = 0.0f;
    int has_tool_d;
    lc_cut_ctx_t ctx;

    (void)lc_field_float2(line, "Z1", "Z_START", &z1);
    if (lc_command_is(line, "G74"))
    {
        has_target = lc_field_float(line, "Z", &target);
        (void)lc_field_float2(line, "K", "PECK", &peck);
    }
    else
    {
        if (!lc_field_float3(line, "Z1", "Z_START", "Z", &z1))
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "DRILL: missing/bad Z");
        if (!lc_field_float(line, "DEPTH", &depth))
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "DRILL: missing/bad DEPTH");
        target = (depth <= 0.0f) ? depth : (z1 - depth);
        has_target = 1;
    }

    lc_read_cut_ctx(tool, &ctx);
    lc_override_ctx_from_line(line, &ctx);
    feed = ctx.rough_feed;
    (void)lc_field_float(line, "PECK", &peck);
    (void)lc_field_float(line, "FEED", &feed);
    (void)lc_field_float(line, "F", &feed);

    if (!has_target) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "DRILL: missing/bad Z");
    if (feed <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "DRILL: FEED must be > 0");
    if (peck < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "DRILL: PECK must be >= 0");
    if (target >= z1) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "DRILL: target must be below Z1");
    if (peck > 0.0f && lc_too_many_steps(z1 - target, peck))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "DRILL: too many pecks");

    has_tool_no = lc_get_tool_number(line, tool, &tool_no);
    has_tool_d = lc_get_tool_diameter(line, tool, &tool_d);
    if (has_tool_no && has_tool_d)
    {
        if (!lc_emit(send, user, "(LC DRILL T%d D %.3f Z1 %.3f Z %.3f PECK %.3f F %.3f)", tool_no, tool_d, z1, target, peck, feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: write failed");
    }
    else if (has_tool_no)
    {
        if (!lc_emit(send, user, "(LC DRILL T%d Z1 %.3f Z %.3f PECK %.3f F %.3f)", tool_no, z1, target, peck, feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: write failed");
    }
    else if (has_tool_d)
    {
        if (!lc_emit(send, user, "(LC DRILL D %.3f Z1 %.3f Z %.3f PECK %.3f F %.3f)", tool_d, z1, target, peck, feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: write failed");
    }
    else if (!lc_emit(send, user, "(LC DRILL Z1 %.3f Z %.3f PECK %.3f F %.3f)", z1, target, peck, feed))
    {
        return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: write failed");
    }
    if (!lc_emit_cycle_preamble(send, user, &ctx, options)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: preamble write failed");
    if (!lc_emit(send, user, "G0 X0 Z%.3f", z1)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: write failed");

    if (peck > 0.0f)
    {
        float z = z1 - peck;
        while (z > target)
        {
            if (++passes > LC_GCODE_MAX_PASSES)
                return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "DRILL: too many pecks");
            if (!lc_emit(send, user, "G1 Z%.3f F%.3f", z, feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: write failed");
            if (!lc_emit(send, user, "G0 Z%.3f", z1)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: write failed");
            z -= peck;
        }
    }

    if (!lc_emit(send, user, "G1 Z%.3f F%.3f", target, feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: write failed");
    if (!lc_emit(send, user, "G0 Z%.3f", z1)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: write failed");
    if ((!options || options->emit_spindle_stop) && !lc_emit(send, user, "M5")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: write failed");

    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_run_tap(const char *line,
                                    const char *tool,
                                    const lc_gcode_line_options_t *options,
                                    lc_gcode_send_fn send,
                                    void *user,
                                    char *err,
                                    unsigned err_len)
{
    float z1 = 0.0f;
    float depth = 0.0f;
    float pitch;
    float rpm_f;
    float target = 0.0f;
    int has_target = 0;
    int rpm;
    lc_cut_ctx_t ctx;

    (void)lc_field_float2(line, "Z1", "Z_START", &z1);
    if (lc_command_is(line, "G84"))
    {
        has_target = lc_field_float(line, "Z", &target);
    }
    else
    {
        if (!lc_field_float3(line, "Z1", "Z_START", "Z", &z1))
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "TAP: missing/bad Z");
        if (!lc_field_float(line, "DEPTH", &depth))
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "TAP: missing/bad DEPTH");
        target = (depth <= 0.0f) ? depth : (z1 - depth);
        has_target = 1;
    }
    if (!lc_field_float2(line, "PITCH", "P", &pitch)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "TAP: missing/bad PITCH");

    lc_read_cut_ctx(tool, &ctx);
    lc_override_ctx_from_line(line, &ctx);
    rpm = ctx.spindle_rpm;
    if (lc_field_float3(line, "RPM", "S", "SPINDLE_RPM", &rpm_f) && rpm_f > 0.0f)
        rpm = (int)rpm_f;

    pitch = lc_absf(pitch);

    if (!has_target) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "TAP: missing/bad Z");
    if (target >= z1) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "TAP: target must be below Z1");
    if (pitch <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "TAP: PITCH must be > 0");
    if (rpm <= 0) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "TAP: RPM must be > 0");

    ctx.spindle_rpm = rpm;
    if (!lc_emit(send, user, "(LC TAP Z1 %.3f Z %.3f P %.3f RPM %d)", z1, target, pitch, rpm)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "TAP: write failed");
    if (options && options->emit_modal_header && !lc_emit_modal_header(send, user)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "TAP: preamble write failed");
    if (!lc_emit(send, user, "S%d M3", rpm)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "TAP: write failed");
    if (!lc_emit(send, user, "G0 X0 Z%.3f", z1)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "TAP: write failed");
    if (!lc_emit(send, user, "G33 Z%.3f K%.3f", target, pitch)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "TAP: write failed");
    if (!lc_emit(send, user, "M4")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "TAP: write failed");
    if (!lc_emit(send, user, "G33 Z%.3f K%.3f", z1, pitch)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "TAP: write failed");
    if ((!options || options->emit_spindle_stop) && !lc_emit(send, user, "M5")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "TAP: write failed");

    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_run_thread(const char *line,
                                       const char *setup,
                                       const char *tool,
                                       const lc_gcode_line_options_t *options,
                                       lc_gcode_send_fn send,
                                       void *user,
                                       char *err,
                                       unsigned err_len)
{
    const char *cycle = "G76";
    float d_start = 0.0f;
    float d_end = 0.0f;
    float z1;
    float z2;
    float pitch;
    float depth = 0.0f;
    float doc = 0.0f;
    float tc;
    float lead;
    float taper = 0.0f;
    float compound_angle = 0.0f;
    float degression = 2.0f;
    float spring_value = 0.0f;
    float i_offset = 0.0f;
    float n_value = 0.0f;
    float strategy_value = 1.0f;
    char field_text[32];
    float pass_depth;
    float last_depth = -1.0f;
    float xsafe;
    float zsafe;
    float z_span;
    float angle_tan = 0.0f;
    float final_z_shift = 0.0f;
    int pass = 0;
    int pass_count = 0;
    int spring_passes = 0;
    int strategy = 1;
    lc_cut_ctx_t ctx;
    lc_gcode_result_t r;
    int has_i;
    int has_x_end;

    if (!setup)
        return lc_fail(LC_GCODE_NO_SETUP, err, err_len, "%s: no SETUP", cycle);

    /* LeanCam expands G76 into explicit G33 passes. LinuxCNC-style words are
     * accepted where they map cleanly: P=pitch, J=first cut/DOC, K=full thread
     * depth, R=degression, Q=compound angle, H=spring passes. Older LeanCam
     * aliases remain supported: X=final diameter, DEPTH=full depth, DOC=pass
     * depth, ANGLE=Q, SPRING=H.
     */
    if (!lc_field_float2(line, "P", "PITCH", &pitch) &&
        !lc_field_float2(line, "K_PITCH", "PITCH_K", &pitch))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: missing/bad P", cycle);

    if (!lc_field_float2(line, "START_X", "X_START", &d_start) &&
        !lc_field_float2(setup, "OD", "OUTER_DIAMETER", &d_start))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: need START_X or SETUP.OD", cycle);
    has_x_end = lc_field_text2(line, "X", "X_END", field_text, sizeof(field_text));
    if (has_x_end && !lc_parse_float_text(field_text, &d_end))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: bad X", cycle);

    if (!lc_field_float2(line, "Z1", "Z_START", &z1))
        z1 = 0.0f;
    if (!lc_field_float3(line, "Z2", "Z_END", "Z", &z2)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: missing/bad Z", cycle);

    has_i = lc_field_float2(line, "I", "PEAK_OFFSET", &i_offset);
    (void)lc_field_float3(line, "THR_DEPTH", "THREAD_DEPTH", "DEPTH", &depth);
    (void)lc_field_float2(line, "K", "FULL_DEPTH", &depth);
    depth = lc_absf(depth);
    if (!has_x_end && depth > 0.0f)
        d_end = d_start + ((has_i && i_offset > 0.0f) ? depth : -depth);

    r = lc_setup_clearance(cycle, setup, &tc, err, err_len);
    if (r != LC_GCODE_OK)
        return r;
    if (lc_field_text3(line, "J", "DOC", "DEPTH_OF_CUT", field_text, sizeof(field_text)))
    {
        if (!lc_parse_float_text(field_text, &doc))
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: bad DOC", cycle);
    }
    else
    {
        doc = 0.2f;
    }
    if (!lc_field_float2(line, "LEAD", "LEAD_IN", &lead))
        lead = pitch;
    (void)lc_field_float3(line, "D", "TAPER", "D_TAPER", &taper);
    (void)lc_field_float2(line, "Q", "ANGLE", &compound_angle);
    (void)lc_field_float2(line, "R", "DEGRESSION", &degression);
    if (lc_field_float2(line, "H", "SPRING", &spring_value) && spring_value > 0.0f)
        spring_passes = (int)(spring_value + 0.5f);
    if (lc_field_float3(line, "N", "PASS", "PASSES", &n_value) && n_value > 0.0f)
        pass_count = (int)(n_value + 0.5f);
    if (lc_field_float3(line, "ST", "STRAT", "STRATEGY", &strategy_value))
        strategy = strategy_value > 0.5f ? 1 : 0;

    if (d_start <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: M/D must be > 0", cycle);
    if (d_end <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: need M or D2/depth", cycle);
    if (d_end == d_start) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: final diameter must differ from start", cycle);
    if (z1 == z2) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: Z span must be nonzero", cycle);
    if (pitch <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: P must be > 0", cycle);
    if (doc <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: DOC must be > 0", cycle);
    if (lead < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: LEAD must be >= 0", cycle);
    if (degression < 1.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: R/degression must be >= 1", cycle);
    if (compound_angle < 0.0f || compound_angle >= 89.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: Q/ANGLE must be 0..89", cycle);
    if (spring_passes < 0 || spring_passes > LC_GCODE_MAX_PASSES) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: bad spring pass count", cycle);
    if (pass_count < 0 || pass_count > LC_GCODE_MAX_PASSES) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: bad pass count", cycle);

    depth = lc_absf(d_start - d_end);
    if (pass_count == 0 && lc_too_many_steps(depth, doc))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: too many passes", cycle);
    if (pass_count == 0)
        pass_count = (int)ceilf(depth / doc);
    if (pass_count <= 0)
        pass_count = 1;

    lc_read_cut_ctx(tool, &ctx);
    lc_override_ctx_from_line(line, &ctx);

    z_span = z2 - z1;
    if (compound_angle > 0.001f) {
        angle_tan = tanf(compound_angle * 0.01745329252f);
        if (!isfinite(angle_tan) || angle_tan <= 0.0001f)
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: bad Q/ANGLE", cycle);
        final_z_shift = (depth * 0.5f) * angle_tan;
        if (z_span < 0.0f)
            final_z_shift = -final_z_shift;
    }
    zsafe = z1 + ((z_span < 0.0f) ? lead : -lead);
    if (d_end < d_start)
        xsafe = lc_maxf(d_start, d_start + taper) + lc_absf(has_i ? i_offset : tc);
    else
        xsafe = lc_minf(d_start, d_start + taper) - lc_absf(has_i ? i_offset : tc);
    if (xsafe < 0.0f)
        xsafe = 0.0f;

    if (!lc_emit(send, user, "(LC %s D %.3f X %.3f P %.3f DOC %.3f R %.3f Q %.3f H %d N %d)",
                 cycle, d_start, d_end, pitch, doc, degression, compound_angle, spring_passes, pass_count))
        return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
    if (!lc_emit(send, user, "(ELS RAMP: lead-in and lead-out are reserve space, not finished thread)"))
        return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
    if (!lc_emit(send, user, "(ELS RAMP: add required start lane before Z1 and stop lane after Z2 per controller settings)"))
        return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
    if (!lc_emit_cycle_preamble(send, user, &ctx, options)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: preamble write failed", cycle);
    if (!lc_emit(send, user, "G0 X%.3f Z%.3f", xsafe, zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);

    while (last_depth < depth)
    {
        float pass_x1;
        float pass_x2;
        float pass_z1 = z1;
        float pass_zsafe = zsafe;
        float z_shift = 0.0f;

        if (++pass > LC_GCODE_MAX_PASSES)
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: too many passes", cycle);

        {
            float t = (float)pass / (float)pass_count;
            pass_depth = depth * (strategy ? powf(t, 1.0f / degression) : t);
        }
        if (pass_depth > depth)
            pass_depth = depth;
        if (pass_depth <= last_depth)
            pass_depth = depth;

        pass_x1 = d_start + ((d_end < d_start) ? -pass_depth : pass_depth);
        pass_x2 = pass_x1 + taper;
        if (angle_tan > 0.0f) {
            z_shift = (pass_depth * 0.5f) * angle_tan;
            if (z_span < 0.0f)
                z_shift = -z_shift;
            pass_z1 = z1 + z_shift - final_z_shift;
            pass_zsafe = pass_z1 + ((z_span < 0.0f) ? lead : -lead);
        }

        if (!lc_emit(send, user, "(THREAD pass %d X%.3f Z%.3f)", pass, pass_x1, pass_z1)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        if (!lc_emit(send, user, "G0 X%.3f Z%.3f", xsafe, pass_zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        if (!lc_emit(send, user, "G0 X%.3f", pass_x1)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        if (!lc_emit(send, user, "G0 Z%.3f", pass_z1)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        if (!lc_emit(send, user, "G33 X%.3f Z%.3f K%.3f", pass_x2, z2, pitch)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        if (!lc_emit(send, user, "G0 X%.3f", xsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);

        last_depth = pass_depth;
    }

    while (spring_passes-- > 0)
    {
        float pass_x1 = d_end;
        float pass_x2 = pass_x1 + taper;
        float pass_z1 = angle_tan > 0.0f ? z1 : z1;
        float pass_zsafe = angle_tan > 0.0f ? (pass_z1 + ((z_span < 0.0f) ? lead : -lead)) : zsafe;

        if (!lc_emit(send, user, "(THREAD spring X%.3f Z%.3f)", pass_x1, pass_z1)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        if (!lc_emit(send, user, "G0 X%.3f Z%.3f", xsafe, pass_zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        if (!lc_emit(send, user, "G0 X%.3f", pass_x1)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        if (!lc_emit(send, user, "G0 Z%.3f", pass_z1)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        if (!lc_emit(send, user, "G33 X%.3f Z%.3f K%.3f", pass_x2, z2, pitch)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        if (!lc_emit(send, user, "G0 X%.3f", xsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
    }

    if (!lc_emit(send, user, "G0 Z%.3f", zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
    if ((!options || options->emit_spindle_stop) && !lc_emit(send, user, "M5")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);

    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_emit_direct_gcode(const char *line,
                                              const lc_gcode_line_options_t *options,
                                              lc_gcode_send_fn send,
                                              void *user,
                                              char *err,
                                              unsigned err_len)
{
    if (options && options->emit_modal_header && !lc_emit_modal_header(send, user))
        return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G-code: preamble write failed");
    if (!lc_emit(send, user, "%s", line))
        return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G-code: write failed");
    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_emit_standalone_motion(const char *line,
                                                   const char *tool,
                                                   const lc_gcode_line_options_t *options,
                                                   lc_gcode_send_fn send,
                                                   void *user,
                                                   char *err,
                                                   unsigned err_len)
{
    lc_cut_ctx_t ctx;
    float x = 0.0f;
    float z = 0.0f;
    float r = 0.0f;
    int has_x;
    int has_z;
    int has_r;

    if (options && options->emit_modal_header && !lc_emit_modal_header(send, user))
        return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G-code: preamble write failed");

    lc_read_cut_ctx(tool, &ctx);
    lc_override_ctx_from_line(line, &ctx);

    has_x = lc_field_float(line, "X", &x);
    has_z = lc_field_float(line, "Z", &z);
    has_r = lc_field_float(line, "R", &r);

    if (!has_x && !has_z)
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G-code: missing X/Z");

    if (lc_command_is(line, "G1"))
    {
        if (has_x && has_z && !lc_emit(send, user, "G1 X%.3f Z%.3f F%.3f", x, z, ctx.rough_feed))
            return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G-code: write failed");
        if (has_x && !has_z && !lc_emit(send, user, "G1 X%.3f F%.3f", x, ctx.rough_feed))
            return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G-code: write failed");
        if (!has_x && has_z && !lc_emit(send, user, "G1 Z%.3f F%.3f", z, ctx.rough_feed))
            return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G-code: write failed");
        return LC_GCODE_OK;
    }

    if (!has_r || r <= 0.0f)
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G2/G3: missing/bad R");
    if (has_x && has_z && !lc_emit(send, user, "%s X%.3f Z%.3f R%.3f F%.3f", lc_command_is(line, "G2") ? "G2" : "G3", x, z, r, ctx.rough_feed))
        return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G-code: write failed");
    if (has_x && !has_z && !lc_emit(send, user, "%s X%.3f R%.3f F%.3f", lc_command_is(line, "G2") ? "G2" : "G3", x, r, ctx.rough_feed))
        return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G-code: write failed");
    if (!has_x && has_z && !lc_emit(send, user, "%s Z%.3f R%.3f F%.3f", lc_command_is(line, "G2") ? "G2" : "G3", z, r, ctx.rough_feed))
        return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G-code: write failed");
    return LC_GCODE_OK;
}

static float lc_raw_max_x(const lc_raw_region_t *region)
{
    float x = 0.0f;
    unsigned i;

    if (!region || region->count == 0)
        return 0.0f;
    x = region->elements[0].d;
    for (i = 1; i < region->count; ++i)
        x = lc_maxf(x, region->elements[i].d);
    return x;
}

static float lc_raw_min_x(const lc_raw_region_t *region)
{
    float x = 0.0f;
    unsigned i;

    if (!region || region->count == 0)
        return 0.0f;
    x = region->elements[0].d;
    for (i = 1; i < region->count; ++i)
        x = lc_minf(x, region->elements[i].d);
    return x;
}

static float lc_raw_max_z(const lc_raw_region_t *region)
{
    float z = 0.0f;
    unsigned i;

    if (!region || region->count == 0)
        return 0.0f;
    z = region->elements[0].z;
    for (i = 1; i < region->count; ++i)
        z = lc_maxf(z, region->elements[i].z);
    return z;
}

static float lc_raw_min_z(const lc_raw_region_t *region)
{
    float z = 0.0f;
    unsigned i;

    if (!region || region->count == 0)
        return 0.0f;
    z = region->elements[0].z;
    for (i = 1; i < region->count; ++i)
        z = lc_minf(z, region->elements[i].z);
    return z;
}

static lc_gcode_result_t lc_emit_raw_finish_chain(const lc_raw_region_t *region,
                                                  unsigned start_index,
                                                  lc_gcode_send_fn send,
                                                  void *user,
                                                  char *err,
                                                  unsigned err_len)
{
    unsigned i;

    if (!region)
        return LC_GCODE_OK;

    for (i = start_index; i < region->count; ++i)
    {
        const lc_contour_element_t *el = &region->elements[i];
        if (el->kind == LC_CONTOUR_ARC)
        {
            if (el->has_center)
            {
                if (!lc_emit(send, user, "%s X%.3f Z%.3f I%.3f K%.3f",
                             el->gcode_cw ? "G2" : "G3",
                             el->d,
                             el->z,
                             el->i,
                             el->k))
                    return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G7x: write failed");
            }
            else if (!lc_emit(send, user, "%s X%.3f Z%.3f R%.3f", el->gcode_cw ? "G2" : "G3", el->d, el->z, el->r))
                return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G7x: write failed");
        }
        else
        {
            if (!lc_emit(send, user, "G1 X%.3f Z%.3f", el->d, el->z))
                return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G7x: write failed");
        }
    }

    return LC_GCODE_OK;
}

static bool lc_raw_add_effective_element(lc_raw_region_t *out, const lc_contour_element_t *src)
{
    if (!out || !src || out->count >= LC_GCODE_MAX_CONTOUR_ELEMENTS)
        return false;
    out->elements[out->count++] = *src;
    return true;
}

static bool lc_raw_effective_forward_on_segment(const lc_raw_region_t *out,
                                                const lc_contour_element_t *seg0,
                                                const lc_contour_element_t *seg1,
                                                const lc_contour_element_t *next)
{
    const lc_contour_element_t *last;
    float sx;
    float sz;
    float dx;
    float dz;

    if (!out || out->count == 0 || !seg0 || !seg1 || !next)
        return false;

    last = &out->elements[out->count - 1u];
    sx = seg1->d - seg0->d;
    sz = seg1->z - seg0->z;
    dx = next->d - last->d;
    dz = next->z - last->z;

    if ((sx * sx) + (sz * sz) <= 0.000001f)
        return false;

    return ((dx * sx) + (dz * sz)) >= -0.0001f;
}

static bool lc_raw_expand_corner(const lc_contour_element_t *prev,
                                 const lc_contour_element_t *corner,
                                 const lc_contour_element_t *next,
                                 lc_contour_element_t *line_to_tangent,
                                 lc_contour_element_t *corner_move)
{
    const float eps = 0.0001f;
    float x1 = corner->d;
    float v1z = corner->z - prev->z;
    float v1x = x1 - prev->d;
    float v2z = next->z - corner->z;
    float v2x = next->d - x1;
    float l1 = sqrtf((v1z * v1z) + (v1x * v1x));
    float l2 = sqrtf((v2z * v2z) + (v2x * v2x));
    float a_z;
    float a_x;
    float b_z;
    float b_x;
    float dot;
    float angle;
    float trim1;
    float trim2;
    float t1z;
    float t1x;
    float t2z;
    float t2x;
    lc_corner_arc_t arc;

    if (!prev || !corner || !next || !line_to_tangent || !corner_move ||
        corner->outgoing_kind == LC_CORNER_NONE || corner->outgoing_amount <= 0.0f ||
        l1 <= eps || l2 <= eps)
        return false;
    if (prev->kind != LC_CONTOUR_LINE || corner->kind != LC_CONTOUR_LINE || next->kind != LC_CONTOUR_LINE)
        return false;

    a_z = v1z / l1;
    a_x = v1x / l1;
    b_z = v2z / l2;
    b_x = v2x / l2;
    dot = ((-a_z) * b_z) + ((-a_x) * b_x);
    dot = lc_maxf(-1.0f, lc_minf(1.0f, dot));
    angle = acosf(dot);
    if (angle <= 0.0001f || lc_absf(3.14159265f - angle) <= 0.0001f)
        return false;

    if (corner->outgoing_kind == LC_CORNER_RND)
    {
        trim1 = corner->outgoing_amount / tanf(angle * 0.5f);
        trim2 = trim1;
    }
    else
    {
        trim1 = (lc_absf(a_x) > eps && lc_absf(a_z) <= eps) ? corner->outgoing_amount * 2.0f : corner->outgoing_amount;
        trim2 = (lc_absf(b_x) > eps && lc_absf(b_z) <= eps) ? corner->outgoing_amount * 2.0f : corner->outgoing_amount;
    }
    if (trim1 <= eps || trim2 <= eps || trim1 >= l1 || trim2 >= l2)
        return false;

    t1z = corner->z - (a_z * trim1);
    t1x = x1 - (a_x * trim1);
    t2z = corner->z + (b_z * trim2);
    t2x = x1 + (b_x * trim2);

    *line_to_tangent = *corner;
    line_to_tangent->d = t1x;
    line_to_tangent->z = t1z;
    line_to_tangent->outgoing_kind = LC_CORNER_NONE;
    line_to_tangent->outgoing_amount = 0.0f;

    memset(corner_move, 0, sizeof(*corner_move));
    corner_move->d = t2x;
    corner_move->z = t2z;
    if (corner->outgoing_kind == LC_CORNER_RND)
    {
        lc_v2_t p0 = { prev->d, prev->z };
        lc_v2_t p1 = { corner->d, corner->z };
        lc_v2_t p2 = { next->d, next->z };
        if (!lc_build_r_corner(p0, p1, p2, corner->outgoing_amount, &arc))
            return false;

        line_to_tangent->d = arc.t1.x;
        line_to_tangent->z = arc.t1.z;
        corner_move->d = arc.t2.x;
        corner_move->z = arc.t2.z;
        corner_move->kind = LC_CONTOUR_ARC;
        corner_move->r = arc.r;
        corner_move->cw = arc.cw;
        corner_move->gcode_cw = !arc.cw;
        corner_move->i = (arc.c.x - arc.t1.x) * 0.5f;
        corner_move->k = arc.c.z - arc.t1.z;
        corner_move->has_center = 1;
    }
    else
    {
        corner_move->kind = LC_CONTOUR_LINE;
    }

    return true;
}

static bool lc_raw_build_effective_region(const lc_raw_region_t *src, lc_raw_region_t *out)
{
    unsigned i;

    if (!src || !out || src->count == 0)
        return false;

    memset(out, 0, sizeof(*out));
    out->active = src->active;
    out->cycle = src->cycle;
    out->cut = src->cut;
    out->retract = src->retract;
    out->x_allow = src->x_allow;
    out->z_allow = src->z_allow;

    if (!lc_raw_add_effective_element(out, &src->elements[0]))
        return false;

    for (i = 1; i < src->count; ++i)
    {
        const lc_contour_element_t *el = &src->elements[i];
        if (el->outgoing_kind != LC_CORNER_NONE && el->outgoing_amount > 0.0f && i + 1u < src->count)
        {
            lc_contour_element_t line_to_tangent;
            lc_contour_element_t corner_move;
            if (!lc_raw_expand_corner(&src->elements[i - 1u],
                                      el,
                                      &src->elements[i + 1u],
                                      &line_to_tangent,
                                      &corner_move))
                return false;
            if (!lc_raw_effective_forward_on_segment(out,
                                                     &src->elements[i - 1u],
                                                     el,
                                                     &line_to_tangent))
                return false;
            if (!lc_raw_add_effective_element(out, &line_to_tangent) ||
                !lc_raw_add_effective_element(out, &corner_move))
                return false;
        }
        else if (!lc_raw_add_effective_element(out, el))
        {
            return false;
        }
    }

    return true;
}

static bool lc_raw_rough_supported(const lc_raw_region_t *region)
{
    unsigned i;
    int allow_arcs;

    if (!region || region->count < 2)
        return false;

    allow_arcs = region->cycle == LC_RAW_G71;
    for (i = 0; i < region->count; ++i)
    {
        const lc_contour_element_t *el = &region->elements[i];
        if ((el->kind != LC_CONTOUR_LINE && (!allow_arcs || el->kind != LC_CONTOUR_ARC)) ||
            el->outgoing_kind != LC_CORNER_NONE ||
            el->outgoing_amount > 0.0f)
            return false;
    }

    return true;
}

static const char *lc_raw_rough_unsupported_reason(const lc_raw_region_t *region)
{
    unsigned i;

    if (!region || region->count < 2)
        return "G7x: empty contour";

    for (i = 0; i < region->count; ++i)
    {
        const lc_contour_element_t *el = &region->elements[i];
        if (el->kind != LC_CONTOUR_LINE)
            return region->cycle == LC_RAW_G71 ? "G71: unsupported arc roughing contour" : "G72: arc roughing unsupported";
        if (el->outgoing_kind != LC_CORNER_NONE || el->outgoing_amount > 0.0f)
            return "G7x: C/R roughing unsupported";
    }

    return NULL;
}

static bool lc_raw_axis_monotonic(const lc_raw_region_t *region, int use_z, int *dir_out)
{
    unsigned i;
    int dir = 0;

    if (dir_out)
        *dir_out = 0;
    if (!region || region->count < 2)
        return false;

    for (i = 1; i < region->count; ++i)
    {
        float a = use_z ? region->elements[i - 1].z : region->elements[i - 1].d;
        float b = use_z ? region->elements[i].z : region->elements[i].d;
        float delta = b - a;

        if (lc_absf(delta) <= 0.0001f)
            continue;

        if (dir == 0)
            dir = delta > 0.0f ? 1 : -1;
        else if ((dir > 0 && delta < -0.0001f) || (dir < 0 && delta > 0.0001f))
            return false;
    }

    if (dir == 0)
        return false;
    if (dir_out)
        *dir_out = dir;
    return true;
}

static bool lc_raw_x_boundary_at_z(const lc_raw_region_t *region, float z, int x_dir, float *x_out)
{
    unsigned i;
    bool found = false;
    float best = 0.0f;

    if (!region || !x_out || region->count < 2)
        return false;

    for (i = 1; i < region->count; ++i)
    {
        float x0 = region->elements[i - 1].d;
        float z0 = region->elements[i - 1].z;
        float x1 = region->elements[i].d;
        float z1 = region->elements[i].z;
        float dz = z1 - z0;

        if (lc_absf(dz) <= 0.0001f)
        {
            if (lc_absf(z - z0) <= 0.0001f)
            {
                float a = x_dir < 0 ? lc_minf(x0, x1) : lc_maxf(x0, x1);
                if (!found || (x_dir < 0 ? a < best : a > best))
                    best = a;
                found = true;
            }
            continue;
        }

        if ((z >= lc_minf(z0, z1) - 0.0001f) &&
            (z <= lc_maxf(z0, z1) + 0.0001f))
        {
            float t = (z - z0) / dz;
            if (t >= -0.0001f && t <= 1.0001f)
            {
                float x = x0 + ((x1 - x0) * t);
                if (!found || (x_dir < 0 ? x < best : x > best))
                    best = x;
                found = true;
            }
        }
    }

    if (!found)
        return false;
    *x_out = best;
    return true;
}

static bool lc_raw_pass_before_finish(float pass, float finish, int dir)
{
    return dir < 0 ? pass > finish + 0.0001f : pass < finish - 0.0001f;
}

static bool lc_raw_seg_crosses_x(float x0, float x1, float x)
{
    return x >= lc_minf(x0, x1) - 0.0001f &&
           x <= lc_maxf(x0, x1) + 0.0001f;
}

static bool lc_raw_seg_z_at_x(float x0, float z0, float x1, float z1, float x, int z_dir, float *z_out)
{
    float dx = x1 - x0;
    float t;

    if (!z_out || !lc_raw_seg_crosses_x(x0, x1, x))
        return false;

    if (lc_absf(dx) <= 0.0001f)
    {
        *z_out = z_dir < 0 ? lc_minf(z0, z1) : lc_maxf(z0, z1);
        return true;
    }

    t = (x - x0) / dx;
    if (t < -0.0001f || t > 1.0001f)
        return false;

    *z_out = z0 + (t * (z1 - z0));
    return true;
}

static float lc_raw_angle_norm(float a);
static float lc_raw_arc_sweep(float a0, float a1, int cw);

static bool lc_raw_arc_center(float x0,
                              float z0,
                              float x1,
                              float z1,
                              float r,
                              int cw,
                              float *cx_out,
                              float *cz_out)
{
    float dx = x1 - x0;
    float dz = z1 - z0;
    float chord = sqrtf((dx * dx) + (dz * dz));
    float half;
    float h2;
    float h;
    float mx;
    float mz;
    float nx;
    float nz;
    float cx[2];
    float cz[2];
    unsigned i;

    if (!cx_out || !cz_out || r <= 0.0f || chord <= 0.0001f || chord > (2.0f * r) + 0.0001f)
        return false;

    half = chord * 0.5f;
    h2 = (r * r) - (half * half);
    if (h2 < -0.0001f)
        return false;
    if (h2 < 0.0f)
        h2 = 0.0f;

    h = sqrtf(h2);
    mx = (x0 + x1) * 0.5f;
    mz = (z0 + z1) * 0.5f;
    nx = -dz / chord;
    nz = dx / chord;
    cx[0] = mx + (h * nx);
    cz[0] = mz + (h * nz);
    cx[1] = mx - (h * nx);
    cz[1] = mz - (h * nz);

    for (i = 0; i < 2; ++i)
    {
        float a0 = atan2f(z0 - cz[i], x0 - cx[i]);
        float a1 = atan2f(z1 - cz[i], x1 - cx[i]);
        float sweep = lc_raw_arc_sweep(a0, a1, cw);
        if (lc_absf(sweep) <= 3.14159265f + 0.0001f)
        {
            *cx_out = cx[i];
            *cz_out = cz[i];
            return true;
        }
    }

    *cx_out = cx[0];
    *cz_out = cz[0];
    return true;
}

static float lc_raw_angle_norm(float a)
{
    const float two_pi = 6.28318530718f;

    while (a < 0.0f)
        a += two_pi;
    while (a >= two_pi)
        a -= two_pi;
    return a;
}

static float lc_raw_arc_sweep(float a0, float a1, int cw)
{
    const float two_pi = 6.28318530718f;
    float sweep;

    a0 = lc_raw_angle_norm(a0);
    a1 = lc_raw_angle_norm(a1);
    sweep = a1 - a0;
    if (cw)
    {
        if (sweep >= 0.0f)
            sweep -= two_pi;
    }
    else if (sweep <= 0.0f)
    {
        sweep += two_pi;
    }
    return sweep;
}

static bool lc_raw_arc_point_on_sweep(float x0,
                                      float z0,
                                      float x1,
                                      float z1,
                                      float cx,
                                      float cz,
                                      int cw,
                                      float x,
                                      float z)
{
    float a0 = atan2f(z0 - cz, x0 - cx);
    float a1 = atan2f(z1 - cz, x1 - cx);
    float ap = atan2f(z - cz, x - cx);
    float full = lc_raw_arc_sweep(a0, a1, cw);
    float part = lc_raw_arc_sweep(a0, ap, cw);

    if (cw)
        return part <= 0.0001f && part >= full - 0.0001f;
    return part >= -0.0001f && part <= full + 0.0001f;
}

static bool lc_raw_arc_z_candidate(float x,
                                   float z,
                                   float x0,
                                   float z0,
                                   float x1,
                                   float z1,
                                   int cw,
                                   float cx,
                                   float cz,
                                   int z_dir,
                                   bool *found,
                                   float *best_z)
{
    if (!found || !best_z)
        return false;
    if (!lc_raw_arc_point_on_sweep(x0, z0, x1, z1, cx, cz, cw, x, z))
        return false;

    if (!*found || (z_dir < 0 ? z < *best_z : z > *best_z))
        *best_z = z;
    *found = true;
    return true;
}

static bool lc_raw_arc_z_at_x(float x0,
                              float z0,
                              const lc_contour_element_t *el,
                              float x,
                              int z_dir,
                              float *z_out)
{
    float cx;
    float cz;
    float x0r;
    float x1r;
    float xr;
    float rel_x;
    float dz2;
    float dz;
    bool found = false;
    float best_z = 0.0f;

    if (!el || !z_out || !lc_raw_seg_crosses_x(x0, el->d, x))
        return false;

    x0r = x0 * 0.5f;
    x1r = el->d * 0.5f;
    xr = x * 0.5f;
    if (!lc_raw_arc_center(x0r, z0, x1r, el->z, el->r, el->cw, &cx, &cz))
        return false;

    rel_x = xr - cx;
    dz2 = (el->r * el->r) - (rel_x * rel_x);
    if (dz2 < -0.0001f)
        return false;
    if (dz2 < 0.0f)
        dz2 = 0.0f;

    dz = sqrtf(dz2);
    (void)lc_raw_arc_z_candidate(xr, cz + dz, x0r, z0, x1r, el->z, el->cw, cx, cz, z_dir, &found, &best_z);
    (void)lc_raw_arc_z_candidate(xr, cz - dz, x0r, z0, x1r, el->z, el->cw, cx, cz, z_dir, &found, &best_z);
    if (!found)
        return false;

    *z_out = best_z;
    return true;
}

static bool lc_raw_z_limit_at_x(const lc_raw_region_t *region,
                                unsigned finish_count,
                                float x,
                                int z_dir,
                                float *z_limit)
{
    unsigned i;
    bool found = false;
    float best_z = 0.0f;

    if (!region || !z_limit || finish_count < 2)
        return false;

    for (i = 1; i < finish_count; ++i)
    {
        float z;

        const lc_contour_element_t *prev = &region->elements[i - 1];
        const lc_contour_element_t *el = &region->elements[i];

        if (el->kind == LC_CONTOUR_ARC)
        {
            if (!lc_raw_arc_z_at_x(prev->d, prev->z, el, x, z_dir, &z))
                continue;
        }
        else if (!lc_raw_seg_z_at_x(prev->d,
                                    prev->z,
                                    el->d,
                                    el->z,
                                    x,
                                    z_dir,
                                    &z))
            continue;

        if (!found || (z_dir < 0 ? z < best_z : z > best_z))
        {
            best_z = z;
            found = true;
        }
    }

    if (!found)
        return false;

    *z_limit = best_z;
    return true;
}

static bool lc_is_diagonal_g1(float x0, float z0, float x1, float z1)
{
    int x_changed = lc_absf(x1 - x0) > 0.001f;
    int z_changed = lc_absf(z1 - z0) > 0.001f;
    return x_changed && z_changed;
}

static lc_gcode_result_t lc_emit_raw_g71_axis_g1(float cur_x,
                                                 float cur_z,
                                                 float next_x,
                                                 float next_z,
                                                 float feed,
                                                 int emit_x,
                                                 int emit_z,
                                                 lc_gcode_send_fn send,
                                                 void *user,
                                                 char *err,
                                                 unsigned err_len)
{
    if (lc_is_diagonal_g1(cur_x, cur_z, next_x, next_z))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G71: diagonal rough move rejected");

    if (emit_x && emit_z)
    {
        if (!lc_emit(send, user, "G1 X%.3f Z%.3f F%.3f", next_x, next_z, feed))
            return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G71: write failed");
    }
    else if (emit_x)
    {
        if (!lc_emit(send, user, "G1 X%.3f F%.3f", next_x, feed))
            return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G71: write failed");
    }
    else
    {
        if (!lc_emit(send, user, "G1 Z%.3f F%.3f", next_z, feed))
            return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G71: write failed");
    }

    return LC_GCODE_OK;
}

static unsigned lc_raw_finish_count_without_close(const lc_raw_region_t *region)
{
    if (!region || region->count == 0)
        return 0;
    if (region->count >= 3)
    {
        const lc_contour_element_t *last = &region->elements[region->count - 1u];
        const lc_contour_element_t *prev = &region->elements[region->count - 2u];
        const lc_contour_element_t *first = &region->elements[0];

        if (last->kind == LC_CONTOUR_LINE &&
            lc_absf(last->z - prev->z) <= 0.0001f &&
            last->d >= first->d - 0.0001f)
            return region->count - 1u;
    }
    return region->count;
}

static lc_gcode_result_t lc_emit_raw_g71_breakpoint_rough(const lc_raw_region_t *region,
                                                          float clearance,
                                                          int z_dir,
                                                          lc_gcode_send_fn send,
                                                          void *user,
                                                          char *err,
                                                          unsigned err_len)
{
    unsigned finish_count;
    unsigned rough_count;
    float start_x;
    float rough_start_x;
    float start_z;
    float min_x;
    float pass;
    float retract;
    int passes = 0;

    if (!region || region->count < 2)
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G71: empty contour");

    finish_count = lc_raw_finish_count_without_close(region);
    if (finish_count < 2)
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G71: empty finish contour");
    rough_count = region->count;

    start_x = region->elements[0].d;
    start_z = region->elements[0].z;
    min_x = start_x;
    rough_start_x = start_x;
    for (unsigned i = 1; i < finish_count; ++i)
    {
        if (region->elements[i].d < min_x)
            min_x = region->elements[i].d;
        if (region->elements[i].d > rough_start_x)
            rough_start_x = region->elements[i].d;
    }
    for (unsigned i = finish_count; i < region->count; ++i)
        if (region->elements[i].d > rough_start_x)
            rough_start_x = region->elements[i].d;
    retract = lc_absf(region->retract);
    if (retract <= 0.0001f)
        retract = clearance;

    pass = rough_start_x - region->cut.rough_doc;
    while (pass > min_x + region->x_allow + 0.0001f)
    {
        float query_x = pass - region->x_allow;
        float z_limit;
        float cut_z_end;
        float safe_x = pass + retract;
        lc_gcode_result_t r;

        if (++passes > LC_GCODE_MAX_PASSES)
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G71: too many rough passes");
        if (!lc_raw_z_limit_at_x(region, rough_count, query_x, z_dir, &z_limit))
        {
            pass -= region->cut.rough_doc;
            continue;
        }
        cut_z_end = z_limit + ((float)z_dir * region->z_allow * -1.0f);

        if (!lc_emit(send, user, "(G71 rough X%.3f)", pass)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G71: write failed");
        if (!lc_emit(send, user, "G0 X%.3f Z%.3f", safe_x, start_z)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G71: write failed");
        r = lc_emit_raw_g71_axis_g1(safe_x, start_z, pass, start_z, region->cut.rough_feed, 1, 0, send, user, err, err_len);
        if (r != LC_GCODE_OK) return r;
        r = lc_emit_raw_g71_axis_g1(pass, start_z, pass, cut_z_end, region->cut.rough_feed, 0, 1, send, user, err, err_len);
        if (r != LC_GCODE_OK) return r;
        if (!lc_emit(send, user, "G0 X%.3f", safe_x)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G71: write failed");
        if (!lc_emit(send, user, "G0 Z%.3f", start_z + ((float)z_dir * clearance * -1.0f))) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G71: write failed");

        pass -= region->cut.rough_doc;
    }

    pass = min_x + region->x_allow;
    if (pass < rough_start_x - 0.0001f)
    {
        float query_x = pass - region->x_allow;
        float z_limit;
        float cut_z_end;
        float safe_x = pass + retract;
        lc_gcode_result_t r;

        if (++passes > LC_GCODE_MAX_PASSES)
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G71: too many rough passes");
        if (!lc_raw_z_limit_at_x(region, rough_count, query_x, z_dir, &z_limit))
            return LC_GCODE_OK;
        cut_z_end = z_limit + ((float)z_dir * region->z_allow * -1.0f);
        if (!lc_emit(send, user, "(G71 rough X%.3f)", pass)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G71: write failed");
        if (!lc_emit(send, user, "G0 X%.3f Z%.3f", safe_x, start_z)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G71: write failed");
        r = lc_emit_raw_g71_axis_g1(safe_x, start_z, pass, start_z, region->cut.rough_feed, 1, 0, send, user, err, err_len);
        if (r != LC_GCODE_OK) return r;
        r = lc_emit_raw_g71_axis_g1(pass, start_z, pass, cut_z_end, region->cut.rough_feed, 0, 1, send, user, err, err_len);
        if (r != LC_GCODE_OK) return r;
        if (!lc_emit(send, user, "G0 X%.3f", safe_x)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G71: write failed");
        if (!lc_emit(send, user, "G0 Z%.3f", start_z + ((float)z_dir * clearance * -1.0f))) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G71: write failed");
    }

    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_flush_raw_region(const char *setup,
                                             const lc_gcode_line_options_t *options,
                                             lc_gcode_send_fn send,
                                             void *user,
                                             char *err,
                                             unsigned err_len)
{
    lc_raw_region_t effective_region;
    const lc_raw_region_t *run_region;
    float clearance = 1.0f;
    float xsafe;
    float zsafe;
    float start_x;
    float start_z;
    float max_x;
    float min_z;
    float max_z;
    float pass;
    int passes = 0;
    int rough_dir = 0;
    lc_gcode_result_t r;

    if (!g_raw_region.active)
        return LC_GCODE_OK;

    if (g_raw_region.count < 1)
    {
        memset(&g_raw_region, 0, sizeof(g_raw_region));
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G7x: empty contour");
    }

    if (!lc_raw_build_effective_region(&g_raw_region, &effective_region))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G7x: bad corner geometry");
    run_region = &effective_region;

    (void)lc_setup_clearance("G7x", setup, &clearance, err, err_len);
    start_x = run_region->elements[0].d;
    start_z = run_region->elements[0].z;
    max_x = lc_raw_max_x(run_region);
    min_z = lc_raw_min_z(run_region);
    max_z = lc_raw_max_z(run_region);
    xsafe = max_x + clearance;
    zsafe = max_z + clearance;
    if (run_region->cycle == LC_RAW_G72 &&
        lc_raw_axis_monotonic(run_region, 0, &rough_dir) &&
        rough_dir > 0)
        xsafe = lc_raw_min_x(run_region) - clearance;

    if (!lc_emit(send, user, "(LC %s region elements %u)",
                 g_raw_region.cycle == LC_RAW_G72 ? "G72" : "G71",
                 run_region->count))
        return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G7x: write failed");
    if (!lc_emit_cycle_preamble(send, user, &run_region->cut, options))
        return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G7x: preamble write failed");
    if (!lc_emit(send, user, "G0 X%.3f Z%.3f", xsafe, zsafe))
        return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G7x: write failed");

    if (!lc_raw_rough_supported(run_region))
    {
        const char *reason = lc_raw_rough_unsupported_reason(run_region);
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s", reason ? reason : "G7x: unsupported roughing contour");
    }

    if (run_region->cycle == LC_RAW_G72)
    {
        int pass_dir = start_z > ((min_z + max_z) * 0.5f) ? -1 : 1;
        float finish_z = pass_dir < 0 ? min_z + run_region->z_allow : max_z - run_region->z_allow;
        if (!lc_raw_axis_monotonic(run_region, 0, &rough_dir))
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G72 contour is non-monotonic in X");
        if (rough_dir > 0)
            xsafe = lc_raw_min_x(run_region) - clearance;
        if (lc_too_many_steps(finish_z - start_z, run_region->cut.rough_doc))
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G72: too many rough passes");
        pass = start_z + ((float)pass_dir * run_region->cut.rough_doc);
        while (lc_raw_pass_before_finish(pass, finish_z, pass_dir))
        {
            float x_hit;
            float rough_x;

            if (++passes > LC_GCODE_MAX_PASSES)
                return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G72: too many rough passes");
            if (!lc_raw_x_boundary_at_z(run_region, pass, rough_dir, &x_hit))
                return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G72: unsupported roughing intersection");
            rough_x = x_hit - ((float)rough_dir * run_region->x_allow);
            if (!lc_emit(send, user, "(G72 rough Z%.3f)", pass)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G72: write failed");
            if (!lc_emit(send, user, "G0 Z%.3f", pass)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G72: write failed");
            if (!lc_emit(send, user, "G1 X%.3f F%.3f", rough_x, run_region->cut.rough_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G72: write failed");
            if (!lc_emit(send, user, "G0 X%.3f", xsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G72: write failed");
            pass += (float)pass_dir * run_region->cut.rough_doc;
        }
        if (lc_raw_pass_before_finish(start_z, finish_z, pass_dir))
        {
            float x_hit;
            float rough_x;
            if (++passes > LC_GCODE_MAX_PASSES)
                return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G72: too many rough passes");
            pass = finish_z;
            if (lc_raw_x_boundary_at_z(run_region, pass, rough_dir, &x_hit))
            {
                rough_x = x_hit - ((float)rough_dir * run_region->x_allow);
                if (!lc_emit(send, user, "(G72 rough Z%.3f)", pass)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G72: write failed");
                if (!lc_emit(send, user, "G0 Z%.3f", pass)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G72: write failed");
                if (!lc_emit(send, user, "G1 X%.3f F%.3f", rough_x, run_region->cut.rough_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G72: write failed");
                if (!lc_emit(send, user, "G0 X%.3f", xsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G72: write failed");
            }
        }
    }
    else
    {
        if (!lc_raw_axis_monotonic(run_region, 1, &rough_dir))
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G71 contour is non-monotonic in Z");
        r = lc_emit_raw_g71_breakpoint_rough(run_region, clearance, rough_dir, send, user, err, err_len);
        if (r != LC_GCODE_OK)
            return r;
    }

    if (!lc_emit(send, user, "(G7x finish continuous contour)"))
        return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G7x: write failed");
    if (!lc_emit(send, user, "G1 X%.3f Z%.3f F%.3f", start_x, start_z, run_region->cut.finish_feed))
        return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G7x: write failed");
    r = lc_emit_raw_finish_chain(run_region, 1u, send, user, err, err_len);
    if (r != LC_GCODE_OK)
        return r;
    if (!lc_emit(send, user, "G0 X%.3f Z%.3f", xsafe, zsafe))
        return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "G7x: write failed");

    memset(&g_raw_region, 0, sizeof(g_raw_region));
    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_start_raw_region(const char *line,
                                             const char *setup,
                                             const char *tool,
                                             const lc_gcode_line_options_t *options,
                                             lc_gcode_send_fn send,
                                             void *user,
                                             char *err,
                                             unsigned err_len)
{
    lc_gcode_result_t r;

    r = lc_flush_raw_region(setup, options, send, user, err, err_len);
    if (r != LC_GCODE_OK)
        return r;

    memset(&g_raw_region, 0, sizeof(g_raw_region));
    g_raw_region.active = 1;
    g_raw_region.cycle = lc_command_is(line, "G72") ? LC_RAW_G72 : LC_RAW_G71;
    lc_read_cut_ctx(tool, &g_raw_region.cut);
    if (g_raw_region.cycle == LC_RAW_G72)
    {
        if (!lc_field_float(line, "W", &g_raw_region.cut.rough_doc))
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G72: missing/bad W");
    }
    else if (!lc_field_float(line, "U", &g_raw_region.cut.rough_doc))
    {
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G71: missing/bad U");
    }
    if (!lc_field_float(line, "R", &g_raw_region.retract))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G7x: missing/bad R");
    (void)lc_field_float(line, "X", &g_raw_region.x_allow);
    (void)lc_field_float(line, "Z", &g_raw_region.z_allow);
    (void)lc_field_float(line, "F", &g_raw_region.cut.rough_feed);
    lc_override_ctx_from_line(line, &g_raw_region.cut);
    lc_sanitize_cut_ctx(&g_raw_region.cut);
    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_add_raw_contour_element(const char *line,
                                                    char *err,
                                                    unsigned err_len)
{
    lc_contour_element_t *el;
    float c = 0.0f;
    float rr = 0.0f;

    if (!g_raw_region.active)
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G1/G2/G3: no active G71/G72");
    if (g_raw_region.count >= LC_GCODE_MAX_CONTOUR_ELEMENTS)
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G7x: too many contour elements");

    el = &g_raw_region.elements[g_raw_region.count];
    memset(el, 0, sizeof(*el));
    el->kind = lc_command_is(line, "G1") ? LC_CONTOUR_LINE : LC_CONTOUR_ARC;
    if (!lc_field_float(line, "X", &el->d))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G7x contour: missing/bad X");
    if (!lc_field_float(line, "Z", &el->z))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G7x contour: missing/bad Z");
    if (el->kind == LC_CONTOUR_ARC)
    {
        if (!lc_field_float(line, "R", &el->r) || el->r <= 0.0f)
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G2/G3: missing/bad R");
        el->cw = lc_command_is(line, "G2");
        el->gcode_cw = el->cw;
    }
    else
    {
        (void)lc_field_float(line, "C", &c);
        (void)lc_field_float(line, "R", &rr);
        if (c > 0.0f && rr > 0.0f)
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G1: C and R exclusive");
        if (c > 0.0f)
        {
            el->outgoing_kind = LC_CORNER_CHMF;
            el->outgoing_amount = c;
        }
        else if (rr > 0.0f)
        {
            el->outgoing_kind = LC_CORNER_RND;
            el->outgoing_amount = rr;
        }
    }

    g_raw_region.count++;
    return LC_GCODE_OK;
}

lc_gcode_result_t leancam_gcode_run_line_with_options(const char *line,
                                                      const char *setup_line,
                                                      const char *tool_line,
                                                      const lc_gcode_line_options_t *options,
                                                      lc_gcode_send_fn send,
                                                      void *user,
                                                      char *err,
                                                      unsigned err_len)
{
    if (err && err_len > 0)
        err[0] = 0;

    if (!options)
        options = &lc_default_line_options;

    if (!line || !line[0])
        return lc_fail(LC_GCODE_UNSUPPORTED, err, err_len, "empty line");

    if (line[0] == '(')
    {
        return LC_GCODE_OK;
    }

    if (lc_command_is(line, "G71") || lc_command_is(line, "G72"))
        return lc_start_raw_region(line, setup_line, tool_line, options, send, user, err, err_len);
    if (lc_command_is(line, "G1") || lc_command_is(line, "G2") || lc_command_is(line, "G3"))
    {
        if (g_raw_region.active)
            return lc_add_raw_contour_element(line, err, err_len);
        return lc_emit_standalone_motion(line, tool_line, options, send, user, err, err_len);
    }
    if (lc_command_is(line, "G80"))
        return lc_flush_raw_region(setup_line, options, send, user, err, err_len);

    if (lc_command_is(line, "PROCESSCALL"))
        return lc_fail(LC_GCODE_UNSUPPORTED, err, err_len, "PROCESSCALL: preset store not available");
    if (lc_command_is(line, "G0") || lc_command_is(line, "G33"))
        return lc_emit_direct_gcode(line, options, send, user, err, err_len);
    if (lc_command_is(line, "G74"))
        return lc_run_drill(line, tool_line, options, send, user, err, err_len);
    if (lc_command_is(line, "G84"))
        return lc_run_tap(line, tool_line, options, send, user, err, err_len);
    if (lc_command_is(line, "G76"))
        return lc_run_thread(line, setup_line, tool_line, options, send, user, err, err_len);

    return lc_fail(LC_GCODE_UNSUPPORTED, err, err_len, "unsupported command");
}

lc_gcode_result_t leancam_gcode_run_line_ex(const char *line,
                                            const char *setup_line,
                                            const char *tool_line,
                                            lc_gcode_send_fn send,
                                            void *user,
                                            char *err,
                                            unsigned err_len)
{
    return leancam_gcode_run_line_with_options(line,
                                               setup_line,
                                               tool_line,
                                               &lc_default_line_options,
                                               send,
                                               user,
                                               err,
                                               err_len);
}

lc_gcode_result_t leancam_gcode_run_program_line_ex(const char *line,
                                                    const char *setup_line,
                                                    const char *tool_line,
                                                    lc_gcode_send_fn send,
                                                    void *user,
                                                    char *err,
                                                    unsigned err_len)
{
    return leancam_gcode_run_line_with_options(line,
                                               setup_line,
                                               tool_line,
                                               &lc_program_line_options,
                                               send,
                                               user,
                                               err,
                                               err_len);
}

int leancam_gcode_emit_program_header(lc_gcode_send_fn send, void *user)
{
    memset(&g_raw_region, 0, sizeof(g_raw_region));
    if (!lc_emit(send, user, "(LeanCam generated)")) return 0;
    if (!lc_emit(send, user, "(Check setup, tools, and first motion before running)")) return 0;
    return lc_emit_modal_header(send, user);
}

int leancam_gcode_emit_program_footer_ex(lc_gcode_send_fn send, void *user, char *err, unsigned err_len)
{
    if (g_raw_region.active)
    {
        (void)lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G71/G72: missing G80");
        memset(&g_raw_region, 0, sizeof(g_raw_region));
        return 0;
    }
    memset(&g_raw_region, 0, sizeof(g_raw_region));
    if (!lc_emit(send, user, "M5")) return 0;
    if (!lc_emit(send, user, "M30")) return 0;
    return 1;
}

int leancam_gcode_emit_program_footer(lc_gcode_send_fn send, void *user)
{
    char err[64];

    err[0] = 0;
    return leancam_gcode_emit_program_footer_ex(send, user, err, sizeof(err));
}

lc_gcode_result_t leancam_gcode_run_line(const char *line,
                                         const char *setup_line,
                                         const char *tool_line,
                                         lc_gcode_send_fn send,
                                         void *user)
{
    return leancam_gcode_run_line_ex(line, setup_line, tool_line, send, user, NULL, 0);
}

static lc_gcode_stepper_state_t *lc_step_state(lc_gcode_stepper_t *stepper)
{
    return stepper ? (lc_gcode_stepper_state_t *)stepper->bytes : NULL;
}

static const char *lc_step_setup_for_line(const program_t *prog, int before_or_at)
{
    for (int i = before_or_at; prog && i >= 0; --i)
        if (lc_command_is(prog->lines[i], "SETUP"))
            return prog->lines[i];
    return NULL;
}

static const char *lc_step_tool_for_line(const program_t *prog, int before_or_at)
{
    for (int i = before_or_at; prog && i >= 0; --i)
        if (lc_command_is(prog->lines[i], "TOOLCALL") || lc_command_is(prog->lines[i], "TOOL"))
            return prog->lines[i];
    return NULL;
}

static bool lc_step_is_context_line(const char *line)
{
    return line &&
           (lc_command_is(line, "SETUP") ||
            lc_command_is(line, "TOOLCALL") ||
            lc_command_is(line, "TOOL") ||
            line[0] == '(');
}

static bool lc_step_put(char *out, unsigned out_len, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (!out || out_len == 0)
        return false;
    va_start(ap, fmt);
    n = vsnprintf(out, out_len, fmt, ap);
    va_end(ap);
    return n >= 0 && n < (int)out_len;
}

typedef struct
{
    char (*lines)[96];
    unsigned count;
    unsigned max;
} lc_step_capture_t;

static int lc_step_capture_line(const char *line, void *user)
{
    lc_step_capture_t *ctx = (lc_step_capture_t *)user;
    if (!ctx || !line || ctx->count >= ctx->max)
        return 0;
    snprintf(ctx->lines[ctx->count++], 96, "%.95s", line);
    return 1;
}

static lc_gcode_result_t lc_step_prepare_direct(lc_gcode_stepper_state_t *s,
                                                const char *line,
                                                char *err,
                                                unsigned err_len)
{
    lc_step_capture_t cap;
    lc_gcode_state_snapshot_t snap;
    lc_gcode_result_t r;

    memset(&cap, 0, sizeof(cap));
    cap.lines = s->direct;
    cap.max = sizeof(s->direct) / sizeof(s->direct[0]);
    (void)leancam_gcode_save_state(&snap);
    memset(&g_raw_region, 0, sizeof(g_raw_region));
    r = leancam_gcode_run_program_line_ex(line,
                                          lc_step_setup_for_line(&s->prog, s->src),
                                          lc_step_tool_for_line(&s->prog, s->src),
                                          lc_step_capture_line,
                                          &cap,
                                          err,
                                          err_len);
    (void)leancam_gcode_restore_state(&snap);
    if (r != LC_GCODE_OK)
        return r;
    s->direct_count = cap.count;
    s->direct_idx = 0;
    s->phase = LC_STEP_PHASE_DIRECT;
    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_step_prepare_g71(lc_gcode_stepper_state_t *s,
                                             char *err,
                                             unsigned err_len)
{
    lc_gcode_state_snapshot_t snap;
    lc_gcode_result_t r;
    int i;
    unsigned finish_count;

    (void)leancam_gcode_save_state(&snap);
    memset(&g_raw_region, 0, sizeof(g_raw_region));
    r = lc_start_raw_region(s->prog.lines[s->src],
                            lc_step_setup_for_line(&s->prog, s->src),
                            lc_step_tool_for_line(&s->prog, s->src),
                            &lc_program_line_options,
                            lc_step_capture_line,
                            NULL,
                            err,
                            err_len);
    if (r != LC_GCODE_OK)
    {
        (void)leancam_gcode_restore_state(&snap);
        return r;
    }
    for (i = s->src + 1; i <= s->end; ++i)
    {
        const char *line = s->prog.lines[i];
        if (lc_command_is(line, "G80"))
            break;
        if (lc_command_is(line, "G1") || lc_command_is(line, "G2") || lc_command_is(line, "G3"))
        {
            r = lc_add_raw_contour_element(line, err, err_len);
            if (r != LC_GCODE_OK)
            {
                (void)leancam_gcode_restore_state(&snap);
                return r;
            }
        }
    }
    if (i > s->end || !lc_command_is(s->prog.lines[i], "G80"))
    {
        (void)leancam_gcode_restore_state(&snap);
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G71/G72: missing G80");
    }
    s->region = g_raw_region;
    s->g80_line = i;
    (void)leancam_gcode_restore_state(&snap);

    if (s->region.count < 1)
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G7x: empty contour");
    if (!lc_raw_build_effective_region(&s->region, &s->effective))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G7x: bad corner geometry");
    if (!lc_raw_rough_supported(&s->effective))
    {
        const char *reason = lc_raw_rough_unsupported_reason(&s->effective);
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s", reason ? reason : "G7x: unsupported roughing contour");
    }
    (void)lc_setup_clearance("G7x", lc_step_setup_for_line(&s->prog, s->src), &s->clearance, err, err_len);

    s->start_x = s->effective.elements[0].d;
    s->start_z = s->effective.elements[0].z;
    s->max_x = lc_raw_max_x(&s->effective);
    s->min_x = lc_raw_min_x(&s->effective);
    s->min_z = lc_raw_min_z(&s->effective);
    s->max_z = lc_raw_max_z(&s->effective);
    s->xsafe = s->max_x + s->clearance;
    s->zsafe = s->max_z + s->clearance;
    finish_count = lc_raw_finish_count_without_close(&s->effective);
    if (finish_count < 2)
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G7x: empty finish contour");

    if (s->effective.cycle == LC_RAW_G72)
    {
        if (!lc_raw_axis_monotonic(&s->effective, 0, &s->rough_dir))
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G72 contour is non-monotonic in X");
        if (s->rough_dir > 0)
            s->xsafe = s->min_x - s->clearance;
        s->pass_dir = s->start_z > ((s->min_z + s->max_z) * 0.5f) ? -1 : 1;
        s->final_pass = s->pass_dir < 0 ? s->min_z + s->effective.z_allow : s->max_z - s->effective.z_allow;
        if (lc_too_many_steps(s->final_pass - s->start_z, s->effective.cut.rough_doc))
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G72: too many rough passes");
        s->pass = s->start_z + ((float)s->pass_dir * s->effective.cut.rough_doc);
        s->final_pending = lc_raw_pass_before_finish(s->start_z, s->final_pass, s->pass_dir);
    }
    else
    {
        if (!lc_raw_axis_monotonic(&s->effective, 1, &s->rough_dir))
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G71 contour is non-monotonic in Z");
        s->rough_start_x = s->start_x;
        for (i = 1; i < (int)finish_count; ++i)
        {
            if (s->effective.elements[i].d < s->min_x)
                s->min_x = s->effective.elements[i].d;
            if (s->effective.elements[i].d > s->rough_start_x)
                s->rough_start_x = s->effective.elements[i].d;
        }
        for (i = (int)finish_count; i < (int)s->effective.count; ++i)
            if (s->effective.elements[i].d > s->rough_start_x)
                s->rough_start_x = s->effective.elements[i].d;

        if (lc_too_many_steps(s->rough_start_x - (s->min_x + s->effective.x_allow), s->effective.cut.rough_doc))
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G71: too many rough passes");
        s->pass = s->rough_start_x - s->effective.cut.rough_doc;
        s->final_pass = s->min_x + s->effective.x_allow;
        s->final_pending = s->final_pass < s->rough_start_x - 0.0001f;
    }
    s->rough_done = false;
    s->g71_stage = 0;
    s->finish_stage = 0;
    s->finish_i = 1;
    s->phase = LC_STEP_PHASE_G71;
    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_step_prepare_g76(lc_gcode_stepper_state_t *s,
                                             const char *line,
                                             char *err,
                                             unsigned err_len)
{
    const char *setup = lc_step_setup_for_line(&s->prog, s->src);
    const char *tool = lc_step_tool_for_line(&s->prog, s->src);
    lc_cut_ctx_t ctx;
    float tc = 0.0f;
    float z1 = 0.0f;
    float z2 = 0.0f;
    float pitch = 0.0f;
    float d_start = 0.0f;
    float d_end = 0.0f;
    float depth = 0.0f;
    float doc = 0.2f;
    float lead = 0.0f;
    float taper = 0.0f;
    float compound_angle = 0.0f;
    float degression = 2.0f;
    float spring_value = 0.0f;
    float pass_value = 0.0f;
    float strategy_value = 1.0f;
    float i_offset = 0.0f;
    bool has_i;
    int spring_passes = 0;
    int pass_count = 0;
    int strategy = 1;
    char field_text[32];
    lc_gcode_result_t r;

    if (!line)
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: missing line");
    if (!setup)
        return lc_fail(LC_GCODE_NO_SETUP, err, err_len, "G76: no SETUP");

    r = lc_setup_clearance("G76", setup, &tc, err, err_len);
    if (r != LC_GCODE_OK)
        return r;

    lc_read_cut_ctx(tool, &ctx);
    lc_override_ctx_from_line(line, &ctx);

    if (!lc_field_float2(line, "P", "PITCH", &pitch) &&
        !lc_field_float2(line, "K_PITCH", "PITCH_K", &pitch))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: missing/bad P");
    if (!lc_field_float2(line, "START_X", "X_START", &d_start) &&
        !lc_field_float2(setup, "OD", "OUTER_DIAMETER", &d_start))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: need START_X or SETUP.OD");
    if (!lc_field_float2(line, "Z1", "Z_START", &z1))
        z1 = 0.0f;
    if (!lc_field_float3(line, "Z2", "Z_END", "Z", &z2))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: missing/bad Z");

    has_i = lc_field_float2(line, "I", "PEAK_OFFSET", &i_offset);
    (void)lc_field_float3(line, "K", "THR_DEPTH", "DEPTH", &depth);
    (void)lc_field_float2(line, "K", "FULL_DEPTH", &depth);
    if (lc_field_text2(line, "X", "X_END", field_text, sizeof(field_text)) &&
        !lc_parse_float_text(field_text, &d_end))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: bad X");
    depth = lc_absf(depth);
    if (d_end == 0.0f && depth > 0.0f)
        d_end = d_start + ((has_i && i_offset > 0.0f) ? depth : -depth);

    if (lc_field_text3(line, "J", "DOC", "DEPTH_OF_CUT", field_text, sizeof(field_text)))
    {
        if (!lc_parse_float_text(field_text, &doc))
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: bad DOC");
    }
    if (!lc_field_float2(line, "LEAD", "LEAD_IN", &lead))
        lead = pitch;
    (void)lc_field_float3(line, "D", "TAPER", "D_TAPER", &taper);
    (void)lc_field_float2(line, "Q", "ANGLE", &compound_angle);
    (void)lc_field_float2(line, "R", "DEGRESSION", &degression);
    if (lc_field_float2(line, "H", "SPRING", &spring_value) && spring_value > 0.0f)
        spring_passes = (int)(spring_value + 0.5f);
    if (lc_field_float3(line, "N", "PASS", "PASSES", &pass_value) && pass_value > 0.0f)
        pass_count = (int)(pass_value + 0.5f);
    if (lc_field_float3(line, "ST", "STRAT", "STRATEGY", &strategy_value))
        strategy = strategy_value > 0.5f ? 1 : 0;

    if (d_start <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: M/D must be > 0");
    if (d_end <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: need X or depth");
    if (d_end == d_start) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: final diameter must differ from start");
    if (z1 == z2) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: Z span must be nonzero");
    if (pitch <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: P must be > 0");
    if (doc <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: DOC must be > 0");
    if (lead < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: LEAD must be >= 0");
    if (degression < 1.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: R/degression must be >= 1");
    if (compound_angle < 0.0f || compound_angle >= 89.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: Q/ANGLE must be 0..89");
    if (spring_passes < 0 || spring_passes > LC_GCODE_MAX_PASSES) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: bad spring pass count");
    if (pass_count < 0 || pass_count > LC_GCODE_MAX_PASSES) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: bad pass count");

    depth = lc_absf(d_start - d_end);
    if (pass_count == 0 && lc_too_many_steps(depth, doc))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: too many passes");
    if (pass_count == 0)
        pass_count = (int)ceilf(depth / doc);
    if (pass_count <= 0)
        pass_count = 1;

    s->g76_d_start = d_start;
    s->g76_d_end = d_end;
    s->g76_depth = depth;
    s->g76_doc = doc;
    s->g76_pitch = pitch;
    s->g76_z1 = z1;
    s->g76_z2 = z2;
    s->g76_taper = taper;
    s->g76_z_span = z2 - z1;
    s->g76_degression = degression;
    s->g76_pass_count = pass_count;
    s->g76_spring_left = spring_passes;
    s->g76_strategy = strategy;
    s->g76_pass = 0;
    s->g76_last_depth = 0.0f;
    s->g76_angle_tan = 0.0f;
    s->g76_final_z_shift = 0.0f;
    if (compound_angle > 0.001f)
    {
        s->g76_angle_tan = tanf(compound_angle * 0.01745329252f);
        if (!isfinite(s->g76_angle_tan) || s->g76_angle_tan <= 0.0001f)
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "G76: bad Q/ANGLE");
        s->g76_final_z_shift = (depth * 0.5f) * s->g76_angle_tan;
        if (s->g76_z_span < 0.0f)
            s->g76_final_z_shift = -s->g76_final_z_shift;
    }
    s->g76_zsafe = z1 + ((s->g76_z_span < 0.0f) ? lead : -lead);
    if (d_end < d_start)
        s->g76_xsafe = lc_maxf(d_start, d_start + taper) + lc_absf(has_i ? i_offset : tc);
    else
        s->g76_xsafe = lc_minf(d_start, d_start + taper) - lc_absf(has_i ? i_offset : tc);
    if (s->g76_xsafe < 0.0f)
        s->g76_xsafe = 0.0f;
    s->g76_stage = 0;
    s->phase = LC_STEP_PHASE_G76;
    return LC_GCODE_OK;
}

static bool lc_step_g76_prepare_next_pass(lc_gcode_stepper_state_t *s)
{
    float pass_depth;
    float z_shift = 0.0f;

    if (s->g76_pass < s->g76_pass_count)
    {
        s->g76_pass++;
        {
            float t = (float)s->g76_pass / (float)s->g76_pass_count;
            pass_depth = s->g76_depth * (s->g76_strategy ? powf(t, 1.0f / s->g76_degression) : t);
        }
        if (pass_depth > s->g76_depth)
            pass_depth = s->g76_depth;
        if (pass_depth <= s->g76_last_depth)
            pass_depth = s->g76_depth;
        s->g76_last_depth = pass_depth;
        s->g76_pass_x1 = s->g76_d_start + ((s->g76_d_end < s->g76_d_start) ? -pass_depth : pass_depth);
    }
    else if (s->g76_spring_left > 0)
    {
        s->g76_spring_left--;
        s->g76_pass_x1 = s->g76_d_end;
    }
    else
    {
        return false;
    }

    s->g76_pass_x2 = s->g76_pass_x1 + s->g76_taper;
    s->g76_pass_z1 = s->g76_z1;
    s->g76_pass_zsafe = s->g76_zsafe;
    if (s->g76_angle_tan > 0.0f)
    {
        float pass_depth_for_z = lc_absf(s->g76_d_start - s->g76_pass_x1);
        z_shift = (pass_depth_for_z * 0.5f) * s->g76_angle_tan;
        if (s->g76_z_span < 0.0f)
            z_shift = -z_shift;
        s->g76_pass_z1 = s->g76_z1 + z_shift - s->g76_final_z_shift;
        s->g76_pass_zsafe = s->g76_pass_z1 + ((s->g76_z_span < 0.0f) ? s->g76_pitch : -s->g76_pitch);
    }
    return true;
}

static bool lc_step_next_g76(lc_gcode_stepper_state_t *s, char *out, unsigned out_len)
{
    for (;;)
    {
        switch (s->g76_stage++)
        {
            case 0:
                return lc_step_put(out, out_len,
                                   "(LC G76 D %.3f X %.3f P %.3f DOC %.3f R %.3f N %d)",
                                   s->g76_d_start,
                                   s->g76_d_end,
                                   s->g76_pitch,
                                   s->g76_doc,
                                   s->g76_degression,
                                   s->g76_pass_count);
            case 1:
                return lc_step_put(out, out_len, "(ELS RAMP: lead-in and lead-out are reserve space, not finished thread)");
            case 2:
                return lc_step_put(out, out_len, "G0 X%.3f Z%.3f", s->g76_xsafe, s->g76_zsafe);
            case 3:
                if (!lc_step_g76_prepare_next_pass(s))
                {
                    s->g76_stage = 10;
                    continue;
                }
                if (s->g76_pass <= s->g76_pass_count)
                    return lc_step_put(out, out_len, "(THREAD pass %d X%.3f Z%.3f)", s->g76_pass, s->g76_pass_x1, s->g76_pass_z1);
                return lc_step_put(out, out_len, "(THREAD spring X%.3f Z%.3f)", s->g76_pass_x1, s->g76_pass_z1);
            case 4:
                return lc_step_put(out, out_len, "G0 X%.3f Z%.3f", s->g76_xsafe, s->g76_pass_zsafe);
            case 5:
                return lc_step_put(out, out_len, "G0 X%.3f", s->g76_pass_x1);
            case 6:
                return lc_step_put(out, out_len, "G0 Z%.3f", s->g76_pass_z1);
            case 7:
                return lc_step_put(out, out_len, "G33 X%.3f Z%.3f K%.3f", s->g76_pass_x2, s->g76_z2, s->g76_pitch);
            case 8:
                return lc_step_put(out, out_len, "G0 X%.3f", s->g76_xsafe);
            case 9:
                s->g76_stage = 3;
                continue;
            case 10:
                return lc_step_put(out, out_len, "G0 Z%.3f", s->g76_zsafe);
            case 11:
                s->src++;
                s->phase = LC_STEP_PHASE_BODY;
                s->g76_stage = 0;

                return lc_step_put(out, out_len, "M5");

            default:
                s->src++;
                s->phase = LC_STEP_PHASE_BODY;
                s->g76_stage = 0;
                return false;
        }
    }
}

static bool lc_step_next_g71_rough(lc_gcode_stepper_state_t *s, char *out, unsigned out_len)
{
    float query_x;
    float z_limit;
    float cut_z_end;
    float safe_x;
    float retract = lc_absf(s->effective.retract);

    if (retract <= 0.0001f)
        retract = s->clearance;
    while (s->pass > s->min_x + s->effective.x_allow + 0.0001f)
    {
        query_x = s->pass - s->effective.x_allow;
        if (lc_raw_z_limit_at_x(&s->effective, s->effective.count, query_x, s->rough_dir, &z_limit))
            break;
        s->pass -= s->effective.cut.rough_doc;
        s->g71_stage = 0;
    }
    if (s->pass <= s->min_x + s->effective.x_allow + 0.0001f)
    {
        if (!s->final_pending)
        {
            s->rough_done = true;
            s->g71_stage = 0;
            return false;
        }
        s->pass = s->final_pass;
        query_x = s->pass - s->effective.x_allow;
        if (!lc_raw_z_limit_at_x(&s->effective, s->effective.count, query_x, s->rough_dir, &z_limit))
        {
            s->final_pending = false;
            s->rough_done = true;
            s->g71_stage = 0;
            return false;
        }
    }

    query_x = s->pass - s->effective.x_allow;
    (void)lc_raw_z_limit_at_x(&s->effective, s->effective.count, query_x, s->rough_dir, &z_limit);
    cut_z_end = z_limit + ((float)s->rough_dir * s->effective.z_allow * -1.0f);
    safe_x = s->pass + retract;
    switch (s->g71_stage++)
    {
        case 0: return lc_step_put(out, out_len, "(G71 rough X%.3f)", s->pass);
        case 1: return lc_step_put(out, out_len, "G0 X%.3f Z%.3f", safe_x, s->start_z);
        case 2: return lc_step_put(out, out_len, "G1 X%.3f F%.3f", s->pass, s->effective.cut.rough_feed);
        case 3: return lc_step_put(out, out_len, "G1 Z%.3f F%.3f", cut_z_end, s->effective.cut.rough_feed);
        case 4: return lc_step_put(out, out_len, "G0 X%.3f", safe_x);
        default:
            s->g71_stage = 0;
            if (s->pass == s->final_pass)
                s->final_pending = false;
            else
                s->pass -= s->effective.cut.rough_doc;
            return lc_step_put(out, out_len, "G0 Z%.3f", s->start_z + ((float)s->rough_dir * s->clearance * -1.0f));
    }
}

static bool lc_step_next_g72_rough(lc_gcode_stepper_state_t *s, char *out, unsigned out_len)
{
    float x_hit;
    float rough_x;

    while (lc_raw_pass_before_finish(s->pass, s->final_pass, s->pass_dir))
    {
        if (lc_raw_x_boundary_at_z(&s->effective, s->pass, s->rough_dir, &x_hit))
            break;
        s->pass += (float)s->pass_dir * s->effective.cut.rough_doc;
        s->g71_stage = 0;
    }

    if (!lc_raw_pass_before_finish(s->pass, s->final_pass, s->pass_dir))
    {
        if (!s->final_pending)
        {
            s->rough_done = true;
            s->g71_stage = 0;
            return false;
        }
        s->pass = s->final_pass;
        if (!lc_raw_x_boundary_at_z(&s->effective, s->pass, s->rough_dir, &x_hit))
        {
            s->final_pending = false;
            s->rough_done = true;
            s->g71_stage = 0;
            return false;
        }
    }

    (void)lc_raw_x_boundary_at_z(&s->effective, s->pass, s->rough_dir, &x_hit);
    rough_x = x_hit - ((float)s->rough_dir * s->effective.x_allow);
    switch (s->g71_stage++)
    {
        case 0: return lc_step_put(out, out_len, "(G72 rough Z%.3f)", s->pass);
        case 1: return lc_step_put(out, out_len, "G0 Z%.3f", s->pass);
        case 2: return lc_step_put(out, out_len, "G1 X%.3f F%.3f", rough_x, s->effective.cut.rough_feed);
        default:
            s->g71_stage = 0;
            if (s->pass == s->final_pass)
                s->final_pending = false;
            else
                s->pass += (float)s->pass_dir * s->effective.cut.rough_doc;
            return lc_step_put(out, out_len, "G0 X%.3f", s->xsafe);
    }
}

void leancam_gcode_stepper_reset(lc_gcode_stepper_t *stepper)
{
    if (stepper)
        memset(stepper, 0, sizeof(*stepper));
}

lc_gcode_result_t leancam_gcode_stepper_begin(lc_gcode_stepper_t *stepper,
                                              const program_t *prog,
                                              int start,
                                              int end,
                                              char *err,
                                              unsigned err_len,
                                              int *err_line_out)
{
    lc_gcode_stepper_state_t *s = lc_step_state(stepper);

    if (err && err_len)
        err[0] = 0;
    if (err_line_out)
        *err_line_out = start + 1;
    if (!s || !prog || start < 0 || end < start || end >= prog->count)
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "bad step range");
    memset(s, 0, sizeof(*s));
    s->prog = *prog;
    s->start = start;
    s->end = end;
    s->src = start;
    s->err_line = start + 1;
    s->phase = LC_STEP_PHASE_HEADER;
    return LC_GCODE_OK;
}

lc_gcode_step_result_t leancam_gcode_stepper_next(lc_gcode_stepper_t *stepper,
                                                  char *out,
                                                  unsigned out_len,
                                                  char *err,
                                                  unsigned err_len,
                                                  int *err_line_out)
{
    static const char *headers[] = { "(LeanCam runtime stepped)", "G21", "G90", "G18", "G7" };
    static const char *footers[] = { "M5", "M30" };
    lc_gcode_stepper_state_t *s = lc_step_state(stepper);

    if (err && err_len)
        err[0] = 0;
    if (out && out_len)
        out[0] = 0;
    if (!s || !out || out_len == 0)
        return LC_GCODE_STEP_ERROR;

    for (;;)
    {
        if (err_line_out)
            *err_line_out = s->err_line;
        if (s->phase == LC_STEP_PHASE_HEADER)
        {
            if (s->header_idx < sizeof(headers) / sizeof(headers[0]))
            {
                lc_step_put(out, out_len, "%s", headers[s->header_idx++]);
                return LC_GCODE_STEP_LINE;
            }
            s->phase = LC_STEP_PHASE_BODY;
        }
        if (s->phase == LC_STEP_PHASE_DIRECT)
        {
            if (s->direct_idx < s->direct_count)
            {
                lc_step_put(out, out_len, "%s", s->direct[s->direct_idx++]);
                return LC_GCODE_STEP_LINE;
            }
            s->src++;
            s->phase = LC_STEP_PHASE_BODY;
        }
        if (s->phase == LC_STEP_PHASE_G71)
        {
            if (s->finish_stage == 0)
            {
                s->finish_stage = 1;
                lc_step_put(out, out_len, "(LC G71 region elements %u)", s->effective.count);
                return LC_GCODE_STEP_LINE;
            }
            if (s->finish_stage == 1)
            {
                s->finish_stage = 2;

                lc_step_put(out, out_len, "S%d M3", s->effective.cut.spindle_rpm);
                return LC_GCODE_STEP_LINE;
            }
            if (s->finish_stage == 2)
            {
                s->finish_stage = 3;
                lc_step_put(out, out_len, "G0 X%.3f Z%.3f", s->xsafe, s->zsafe);
                return LC_GCODE_STEP_LINE;
            }
            if (!s->rough_done)
            {
                bool emitted = s->effective.cycle == LC_RAW_G72 ?
                               lc_step_next_g72_rough(s, out, out_len) :
                               lc_step_next_g71_rough(s, out, out_len);
                if (emitted)
                    return LC_GCODE_STEP_LINE;
            }
            if (s->finish_stage == 3)
            {
                s->finish_stage = 4;
                lc_step_put(out, out_len, "(G7x finish continuous contour)");
                return LC_GCODE_STEP_LINE;
            }
            if (s->finish_stage == 4)
            {
                s->finish_stage = 5;
                lc_step_put(out, out_len, "G1 X%.3f Z%.3f F%.3f", s->start_x, s->start_z, s->effective.cut.finish_feed);
                return LC_GCODE_STEP_LINE;
            }
            if (s->finish_i < s->effective.count)
            {
                const lc_contour_element_t *el = &s->effective.elements[s->finish_i++];
                if (el->kind == LC_CONTOUR_ARC)
                {
                    if (el->has_center)
                        lc_step_put(out, out_len, "%s X%.3f Z%.3f I%.3f K%.3f", el->gcode_cw ? "G2" : "G3", el->d, el->z, el->i, el->k);
                    else
                        lc_step_put(out, out_len, "%s X%.3f Z%.3f R%.3f", el->gcode_cw ? "G2" : "G3", el->d, el->z, el->r);
                }
                else
                    lc_step_put(out, out_len, "G1 X%.3f Z%.3f", el->d, el->z);
                return LC_GCODE_STEP_LINE;
            }
            s->src = s->g80_line + 1;
            s->phase = LC_STEP_PHASE_BODY;
            lc_step_put(out, out_len, "G0 X%.3f Z%.3f", s->xsafe, s->zsafe);
            return LC_GCODE_STEP_LINE;
        }
        if (s->phase == LC_STEP_PHASE_G76)
        {
            if (lc_step_next_g76(s, out, out_len))
                return LC_GCODE_STEP_LINE;
            continue;
        }
        if (s->phase == LC_STEP_PHASE_BODY)
        {
            lc_gcode_result_t r;
            const char *line;

            while (s->src <= s->end && lc_step_is_context_line(s->prog.lines[s->src]))
                s->src++;
            if (s->src > s->end)
            {
                s->phase = LC_STEP_PHASE_FOOTER;
                continue;
            }
            s->err_line = s->src + 1;
            if (err_line_out)
                *err_line_out = s->err_line;
            line = s->prog.lines[s->src];
            if (lc_command_is(line, "G71") || lc_command_is(line, "G72"))
            {
                r = lc_step_prepare_g71(s, err, err_len);
                if (r != LC_GCODE_OK)
                    return LC_GCODE_STEP_ERROR;
                lc_step_put(out, out_len, "(--- LeanCam L%d ---)", s->src + 1);
                return LC_GCODE_STEP_LINE;
            }
            if (lc_command_is(line, "G76"))
            {
                r = lc_step_prepare_g76(s, line, err, err_len);
                if (r != LC_GCODE_OK)
                    return LC_GCODE_STEP_ERROR;
                lc_step_put(out, out_len, "(--- LeanCam L%d ---)", s->src + 1);
                return LC_GCODE_STEP_LINE;
            }
            r = lc_step_prepare_direct(s, line, err, err_len);
            if (r != LC_GCODE_OK)
                return LC_GCODE_STEP_ERROR;
            lc_step_put(out, out_len, "(--- LeanCam L%d ---)", s->src + 1);
            return LC_GCODE_STEP_LINE;
        }
        if (s->phase == LC_STEP_PHASE_FOOTER)
        {
            if (s->footer_idx < sizeof(footers) / sizeof(footers[0]))
            {
                lc_step_put(out, out_len, "%s", footers[s->footer_idx++]);
                return LC_GCODE_STEP_LINE;
            }
            s->phase = LC_STEP_PHASE_DONE;
        }
        if (s->phase == LC_STEP_PHASE_DONE)
            return LC_GCODE_STEP_DONE;
        return LC_GCODE_STEP_ERROR;
    }
}

