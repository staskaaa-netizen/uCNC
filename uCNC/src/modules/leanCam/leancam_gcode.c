#include "leancam_gcode.h"

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

typedef struct
{
    float d_start;
    float d_end;
    float z_start;
    float z_profile_end;
    float d_profile_end;
    float z_end;
    float d_corner_end;
    float z_corner_end;
    float arc_i;
    float arc_k;
    float amount;
    lc_corner_kind_t kind;
} lc_turn_profile_t;

static int lc_starts_with(const char *s, const char *prefix)
{
    return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
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

static float lc_profile_max_d(const lc_turn_profile_t *p)
{
    float m;

    if (!p)
        return 0.0f;

    m = lc_maxf(p->d_start, p->d_end);
    m = lc_maxf(m, p->d_profile_end);
    m = lc_maxf(m, p->d_corner_end);
    return m;
}

static float lc_profile_min_d(const lc_turn_profile_t *p)
{
    float m;

    if (!p)
        return 0.0f;

    m = lc_minf(p->d_start, p->d_end);
    m = lc_minf(m, p->d_profile_end);
    m = lc_minf(m, p->d_corner_end);
    return m;
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
    const char *p;
    const char *open;
    const char *close;
    unsigned name_len;
    unsigned n;

    if (!line || !name || !out || out_len == 0)
        return 0;

    out[0] = 0;
    name_len = (unsigned)strlen(name);
    p = line;

    while ((p = strstr(p, name)) != NULL)
    {
        if ((p == line || *(p - 1) == '|') && p[name_len] == '{')
        {
            open = p + name_len;
            close = strchr(open + 1, '}');
            if (!close)
                return 0;

            n = (unsigned)(close - open - 1);
            if (n >= out_len)
                n = out_len - 1;

            memcpy(out, open + 1, n);
            out[n] = 0;
            return 1;
        }
        p += name_len;
    }

    return 0;
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
    (void)lc_field_float(line, "FEED", &ctx->rough_feed);
    (void)lc_field_float2(line, "FIN_FEED", "FINISH_FEED", &ctx->finish_feed);
    (void)lc_field_float3(line, "DOC", "R_DOC", "ROUGH_DOC", &ctx->rough_doc);
    (void)lc_field_float(line, "ROUGH_DEPTH_OF_CUT", &ctx->rough_doc);
    (void)lc_field_float2(line, "FIN_DOC", "FINISH_DEPTH_OF_CUT", &ctx->finish_doc);
    if (lc_field_float3(line, "S", "RPM", "SPINDLE_RPM", &rpm) && rpm > 0.0f)
        ctx->spindle_rpm = (int)rpm;

    lc_sanitize_cut_ctx(ctx);
}

static void lc_setup_float2(const char *setup, const char *a, const char *b, float def, float *out)
{
    if (setup && lc_field_float2(setup, a, b, out))
        return;

    *out = def;
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

static lc_gcode_result_t lc_read_corner(const char *line,
                                        const char *cycle,
                                        lc_corner_kind_t *kind,
                                        float *amount,
                                        char *err,
                                        unsigned err_len)
{
    float rnd = 0.0f;
    float chmf = 0.0f;
    int has_rnd;
    int has_chmf;

    if (kind) *kind = LC_CORNER_NONE;
    if (amount) *amount = 0.0f;

    has_rnd = lc_field_float3(line, "RND", "ROUND", "RADIUS", &rnd);
    has_chmf = lc_field_float3(line, "CHMF", "CHAMFER", "C", &chmf);

    if (has_rnd && rnd < 0.0f)
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: RND must be >= 0", cycle);
    if (has_chmf && chmf < 0.0f)
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: CHMF must be >= 0", cycle);
    if (rnd > 0.0f && chmf > 0.0f)
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: RND and CHMF are exclusive", cycle);

    if (rnd > 0.0f)
    {
        if (kind) *kind = LC_CORNER_RND;
        if (amount) *amount = rnd;
    }
    else if (chmf > 0.0f)
    {
        if (kind) *kind = LC_CORNER_CHMF;
        if (amount) *amount = chmf;
    }

    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_build_turn_profile(const char *line,
                                               const char *cycle,
                                               int is_od,
                                               float d1,
                                               float z1,
                                               float d2,
                                               float z2,
                                               lc_turn_profile_t *profile,
                                               char *err,
                                               unsigned err_len)
{
    float dt = d2;
    lc_corner_kind_t corner = LC_CORNER_NONE;
    float corner_amount = 0.0f;
    lc_gcode_result_t r;

    if (!profile)
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: no profile", cycle);

    memset(profile, 0, sizeof(*profile));

    (void)lc_field_float3(line, "DT", "D_TAPER", "TAPER_DIAMETER", &dt);

    r = lc_read_corner(line, cycle, &corner, &corner_amount, err, err_len);
    if (r != LC_GCODE_OK)
        return r;

    if (dt <= 0.0f)
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: DT must be > 0", cycle);

    if (corner_amount > 0.0f && corner_amount >= lc_absf(z1 - z2))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: corner must fit Z span", cycle);

    profile->d_start = dt;
    profile->d_end = d2;
    profile->z_start = z1;
    profile->z_end = z2;
    profile->z_corner_end = z2;
    profile->amount = corner_amount;
    profile->kind = corner;

    if (corner_amount > 0.0f)
    {
        /*
         * Work in real lathe section geometry: Z plus radius, not diameter.
         * The saved/programmed X value remains diameter, but all direction,
         * trim and fillet math is done in radius-space so taper + corner is
         * geometrically meaningful.
         *
         * Vertex P is the theoretical sharp intersection at (Z2, D2/2).
         * Ray A goes backwards along the finished taper/profile.
         * Ray B goes along the shoulder face: outward for OD, inward for ID.
         *
         * CHMF: amount is distance from P along both rays. For a straight
         *       cylinder this gives the old Z+amount / X +/- 2*amount result.
         * RND:  amount is fillet radius. Tangency distance is
         *       R / tan(theta/2), then arc centre lies on the angle bisector.
         */
        const float eps = 0.000001f;
        float r_start = dt * 0.5f;
        float r_end = d2 * 0.5f;
        float dz = z2 - z1;
        float dr = r_end - r_start;
        float len = sqrtf((dz * dz) + (dr * dr));
        float ax, az;
        float bx, bz;
        float dot;
        float trim;

        if (len <= eps)
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: profile span too small", cycle);

        /* A = direction from vertex back along taper/profile. */
        az = -dz / len;
        ax = -dr / len;

        /* B = direction from vertex along the shoulder face. */
        bz = 0.0f;
        bx = is_od ? 1.0f : -1.0f;

        dot = (az * bz) + (ax * bx);
        if (dot >  0.999f) dot =  0.999f;
        if (dot < -0.999f) dot = -0.999f;

        if (corner == LC_CORNER_CHMF)
        {
            trim = corner_amount;
            profile->z_profile_end = z2 + (az * trim);
            profile->d_profile_end = 2.0f * (r_end + (ax * trim));
            profile->d_corner_end = 2.0f * (r_end + (bx * trim));
        }
        else
        {
            float cot_half;
            float inv_sin_half;
            float bis_z;
            float bis_x;
            float bis_len;
            float center_z;
            float center_r;

            cot_half = sqrtf((1.0f + dot) / (1.0f - dot));
            trim = corner_amount * cot_half;

            profile->z_profile_end = z2 + (az * trim);
            profile->d_profile_end = 2.0f * (r_end + (ax * trim));
            profile->d_corner_end = 2.0f * (r_end + (bx * trim));

            bis_z = az + bz;
            bis_x = ax + bx;
            bis_len = sqrtf((bis_z * bis_z) + (bis_x * bis_x));
            if (bis_len <= eps)
                return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: bad corner angle", cycle);

            inv_sin_half = sqrtf(2.0f / (1.0f - dot));
            center_z = z2 + (bis_z / bis_len) * corner_amount * inv_sin_half;
            center_r = r_end + (bis_x / bis_len) * corner_amount * inv_sin_half;

            profile->arc_k = center_z - profile->z_profile_end;
            profile->arc_i = center_r - (profile->d_profile_end * 0.5f);
        }
    }
    else
    {
        profile->z_profile_end = z2;
        profile->d_profile_end = d2;
        profile->d_corner_end = d2;
    }

    if (profile->z_profile_end < lc_minf(z1, z2) - 0.001f ||
        profile->z_profile_end > lc_maxf(z1, z2) + 0.001f)
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: corner too large for taper length", cycle);

    if (is_od)
    {
        if (dt > d1)
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: DT must be <= D1", cycle);
        /*
         * Do not reject when the corner/fillet endpoint is outside stock OD.
         * That is valid CNC air-cutting: the mathematical profile may extend
         * beyond the available material, while the real stock/simulation is
         * clipped by D1/setup OD. DOC planning is based on profile_min.
         */
    }
    else
    {
        if (dt < d1)
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: DT must be >= D1", cycle);
        /* Same rule for ID: profile may extend through air/empty bore. */
    }

    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_emit_od_corner(const lc_turn_profile_t *p,
                                           float d_offset,
                                           lc_gcode_send_fn send,
                                           void *user,
                                           char *err,
                                           unsigned err_len)
{
    if (!p || p->kind == LC_CORNER_NONE)
        return LC_GCODE_OK;

    if (p->kind == LC_CORNER_CHMF)
    {
        if (!lc_emit(send, user, "G1 X%.3f Z%.3f", p->d_corner_end + d_offset, p->z_corner_end))
            return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: chamfer write failed");
    }
    else
    {
        if (!lc_emit(send, user, "G2 X%.3f Z%.3f I%.3f K%.3f", p->d_corner_end + d_offset, p->z_corner_end, p->arc_i, p->arc_k))
            return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: radius write failed");
    }

    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_emit_id_corner(const lc_turn_profile_t *p,
                                           float d_offset,
                                           lc_gcode_send_fn send,
                                           void *user,
                                           char *err,
                                           unsigned err_len)
{
    if (!p || p->kind == LC_CORNER_NONE)
        return LC_GCODE_OK;

    if (p->kind == LC_CORNER_CHMF)
    {
        if (!lc_emit(send, user, "G1 X%.3f Z%.3f", p->d_corner_end + d_offset, p->z_corner_end))
            return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: chamfer write failed");
    }
    else
    {
        if (!lc_emit(send, user, "G3 X%.3f Z%.3f I%.3f K%.3f", p->d_corner_end + d_offset, p->z_corner_end, p->arc_i, p->arc_k))
            return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: radius write failed");
    }

    return LC_GCODE_OK;
}

static int lc_line_q_mode(const char *line, int fallback)
{
    float qf;

    if (lc_field_float(line, "Q", &qf))
        return (int)(qf + (qf >= 0.0f ? 0.5f : -0.5f));
    return fallback;
}

static lc_gcode_result_t lc_validate_q_mode(const char *cycle,
                                            int q,
                                            char *err,
                                            unsigned err_len)
{
    if (q < 0 || q > 2)
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: Q must be 0, 1, or 2", cycle);
    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_emit_q_retract(const char *cycle,
                                           float retract_x,
                                           float zsafe,
                                           int q,
                                           bool clamp_x_min_zero,
                                           lc_gcode_send_fn send,
                                           void *user,
                                           char *err,
                                           unsigned err_len)
{
    if (clamp_x_min_zero && retract_x < 0.0f)
        retract_x = 0.0f;

    if (q == 0)
    {
        if (!lc_emit(send, user, "G0 X%.3f Z%.3f", retract_x, zsafe))
            return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        return LC_GCODE_OK;
    }
    if (q == 1)
    {
        if (!lc_emit(send, user, "G0 X%.3f", retract_x))
            return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        if (!lc_emit(send, user, "G0 Z%.3f", zsafe))
            return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        return LC_GCODE_OK;
    }
    if (q == 2)
    {
        if (!lc_emit(send, user, "G0 Z%.3f", zsafe))
            return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        if (!lc_emit(send, user, "G0 X%.3f", retract_x))
            return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        return LC_GCODE_OK;
    }

    return lc_validate_q_mode(cycle, q, err, err_len);
}

static lc_gcode_result_t lc_run_od(const char *line,
                                   const char *setup,
                                   const char *tool,
                                   const lc_gcode_line_options_t *options,
                                   lc_gcode_send_fn send,
                                   void *user,
                                   char *err,
                                   unsigned err_len)
{
    float d1, z1, d2, z2, tc;
    float setup_od;
    float xsafe;
    float zsafe;
    float profile_max;
    float profile_min;
    float max_rough_offset;
    float stock;
    int passes = 0;
    int q;
    lc_cut_ctx_t ctx;
    lc_turn_profile_t profile;
    lc_gcode_result_t r;

    if (!setup)
        return lc_fail(LC_GCODE_NO_SETUP, err, err_len, "OD: no SETUP");

    if (!lc_field_float2(line, "D1", "DIAMETER_1", &d1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: missing/bad D1");
    if (!lc_field_float2(line, "Z1", "Z_1",        &z1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: missing/bad Z1");
    if (!lc_field_float2(line, "Z2", "Z_2",        &z2)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: missing/bad Z2");
    if (!lc_field_float2(line, "D2", "DIAMETER_2", &d2)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: missing/bad D2");

    r = lc_setup_clearance("ID", setup, &tc, err, err_len);
    if (r != LC_GCODE_OK)
        return r;

    lc_setup_float2(setup, "OD", "OUTER_DIAMETER", d1, &setup_od);

    xsafe = setup_od + tc;
    if (d1 + tc > xsafe) xsafe = d1 + tc;
    if (d2 + tc > xsafe) xsafe = d2 + tc;

    zsafe = z1 + tc;
    if (z2 + tc > zsafe) zsafe = z2 + tc;

    if (d1 <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: D1 must be > 0");
    if (d2 <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: D2 must be > 0");
    if (d2 > d1) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: D2 must be <= D1");
    if (z2 > z1) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: Z2 must be <= Z1");
    q = lc_line_q_mode(line, 0);
    r = lc_validate_q_mode("OD", q, err, err_len);
    if (r != LC_GCODE_OK)
        return r;

    r = lc_build_turn_profile(line, "OD", 1, d1, z1, d2, z2, &profile, err, err_len);
    if (r != LC_GCODE_OK)
        return r;

    profile_max = lc_profile_max_d(&profile);
    profile_min = lc_profile_min_d(&profile);

    lc_read_cut_ctx(tool, &ctx);
    lc_override_ctx_from_line(line, &ctx);
    max_rough_offset = d1 - profile_min;
    if (max_rough_offset < 0.0f)
        max_rough_offset = 0.0f;
    if (ctx.finish_doc >= max_rough_offset)
        ctx.finish_doc = 0.0f;
    if (lc_too_many_steps(max_rough_offset - ctx.finish_doc, ctx.rough_doc))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: too many rough passes");

    /*
     * Roughing follows the same final profile, offset outward in X by stock.
     * Use profile_min to schedule DOC, not profile_max.  Keep retract X at
     * the stock/profile envelope plus clearance; the roughing offset is a
     * diameter stock allowance, not an extra radius-mode safe-X distance.
     */
    xsafe = lc_maxf(xsafe, profile_max + tc);

    if (!lc_emit(send, user, "(LC OD DT %.3f D2 %.3f RND %.3f CHMF %.3f)",
                 profile.d_start,
                 profile.d_end,
                 profile.kind == LC_CORNER_RND ? profile.amount : 0.0f,
                 profile.kind == LC_CORNER_CHMF ? profile.amount : 0.0f))
        return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
    if (!lc_emit_cycle_preamble(send, user, &ctx, options)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: preamble write failed");
    if (!lc_emit(send, user, "G0 X%.3f Z%.3f", xsafe, zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");

    stock = max_rough_offset;
    while (stock - ctx.rough_doc > ctx.finish_doc)
    {
        float retract_x;
        if (++passes > LC_GCODE_MAX_PASSES)
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: too many rough passes");
        stock -= ctx.rough_doc;
        retract_x = lc_maxf(xsafe, profile_max + stock + tc);
        if (!lc_emit(send, user, "(OD rough stock %.3f)", stock)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
        if (!lc_emit(send, user, "G0 X%.3f Z%.3f", profile.d_start + stock + tc, z1 + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
        if (!lc_emit(send, user, "G1 X%.3f F%.3f", profile.d_start + stock, ctx.rough_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
        if (!lc_emit(send, user, "G1 X%.3f Z%.3f", profile.d_profile_end + stock, profile.z_profile_end)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
        r = lc_emit_od_corner(&profile, stock, send, user, err, err_len);
        if (r != LC_GCODE_OK)
            return r;
        r = lc_emit_q_retract("OD", retract_x, zsafe, q, false, send, user, err, err_len);
        if (r != LC_GCODE_OK)
            return r;
    }

    if (ctx.finish_doc > 0.0f && max_rough_offset > ctx.finish_doc)
    {
        stock = ctx.finish_doc;
        if (!lc_emit(send, user, "(OD rough finish-stock %.3f)", stock)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
        if (!lc_emit(send, user, "G0 X%.3f Z%.3f", profile.d_start + stock + tc, z1 + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
        if (!lc_emit(send, user, "G1 X%.3f F%.3f", profile.d_start + stock, ctx.rough_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
        if (!lc_emit(send, user, "G1 X%.3f Z%.3f", profile.d_profile_end + stock, profile.z_profile_end)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
        r = lc_emit_od_corner(&profile, stock, send, user, err, err_len);
        if (r != LC_GCODE_OK)
            return r;
        r = lc_emit_q_retract("OD", lc_maxf(xsafe, profile_max + stock + tc), zsafe, q, false, send, user, err, err_len);
        if (r != LC_GCODE_OK)
            return r;
    }

    if (!lc_emit(send, user, "(OD finish DT %.3f D2 %.3f)", profile.d_start, profile.d_end)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
    if (!lc_emit(send, user, "G0 X%.3f Z%.3f", profile.d_start + tc, z1 + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
    if (!lc_emit(send, user, "G1 X%.3f F%.3f", profile.d_start, ctx.finish_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
    if (!lc_emit(send, user, "G1 X%.3f Z%.3f", profile.d_profile_end, profile.z_profile_end)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
    r = lc_emit_od_corner(&profile, 0.0f, send, user, err, err_len);
    if (r != LC_GCODE_OK)
        return r;
    r = lc_emit_q_retract("OD", lc_maxf(xsafe, profile_max + tc), zsafe, q, false, send, user, err, err_len);
    if (r != LC_GCODE_OK)
        return r;
    if ((!options || options->emit_spindle_stop) && !lc_emit(send, user, "M5")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");

    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_run_id(const char *line,
                                   const char *setup,
                                   const char *tool,
                                   const lc_gcode_line_options_t *options,
                                   lc_gcode_send_fn send,
                                   void *user,
                                   char *err,
                                   unsigned err_len)
{
    float d1, z1, d2, z2, tc;
    float zsafe;
    float profile_max;
    float max_rough_offset;
    float stock;
    int passes = 0;
    int q;
    lc_cut_ctx_t ctx;
    lc_turn_profile_t profile;
    lc_gcode_result_t r;

    if (!setup)
        return lc_fail(LC_GCODE_NO_SETUP, err, err_len, "ID: no SETUP");

    if (!lc_field_float2(line, "D1", "DIAMETER_1", &d1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: missing/bad D1");
    if (!lc_field_float2(line, "Z1", "Z_1",        &z1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: missing/bad Z1");
    if (!lc_field_float2(line, "Z2", "Z_2",        &z2)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: missing/bad Z2");
    if (!lc_field_float2(line, "D2", "DIAMETER_2", &d2)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: missing/bad D2");

    r = lc_setup_clearance("CUT", setup, &tc, err, err_len);
    if (r != LC_GCODE_OK)
        return r;

    if (d1 <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: D1 must be > 0");
    if (d2 < d1) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: D2 must be >= D1");
    if (z2 > z1) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: Z2 must be <= Z1");
    q = lc_line_q_mode(line, 2);
    r = lc_validate_q_mode("ID", q, err, err_len);
    if (r != LC_GCODE_OK)
        return r;

    r = lc_build_turn_profile(line, "ID", 0, d1, z1, d2, z2, &profile, err, err_len);
    if (r != LC_GCODE_OK)
        return r;

    profile_max = lc_profile_max_d(&profile);

    lc_read_cut_ctx(tool, &ctx);
    lc_override_ctx_from_line(line, &ctx);
    max_rough_offset = profile_max - d1;
    if (max_rough_offset < 0.0f)
        max_rough_offset = 0.0f;
    if (ctx.finish_doc >= max_rough_offset)
        ctx.finish_doc = 0.0f;
    if (lc_too_many_steps(max_rough_offset - ctx.finish_doc, ctx.rough_doc))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: too many rough passes");

    zsafe = z1 + tc;
    if (!lc_emit(send, user, "(LC ID DT %.3f D2 %.3f RND %.3f CHMF %.3f)",
                 profile.d_start,
                 profile.d_end,
                 profile.kind == LC_CORNER_RND ? profile.amount : 0.0f,
                 profile.kind == LC_CORNER_CHMF ? profile.amount : 0.0f))
        return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
    if (!lc_emit_cycle_preamble(send, user, &ctx, options)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: preamble write failed");

    stock = max_rough_offset;
    while (stock - ctx.rough_doc > ctx.finish_doc)
    {
        float retract_x;
        if (++passes > LC_GCODE_MAX_PASSES)
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: too many rough passes");
        stock -= ctx.rough_doc;
        retract_x = profile.d_start - stock - tc;
        if (!lc_emit(send, user, "(ID rough stock %.3f)", stock)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
        if (!lc_emit(send, user, "G0 X%.3f Z%.3f", profile.d_start - stock, zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
        if (!lc_emit(send, user, "G1 X%.3f F%.3f", profile.d_start - stock, ctx.rough_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
        if (!lc_emit(send, user, "G1 X%.3f Z%.3f", profile.d_profile_end - stock, profile.z_profile_end)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
        r = lc_emit_id_corner(&profile, -stock, send, user, err, err_len);
        if (r != LC_GCODE_OK)
            return r;
        r = lc_emit_q_retract("ID", retract_x, zsafe, q, true, send, user, err, err_len);
        if (r != LC_GCODE_OK)
            return r;
    }

    if (ctx.finish_doc > 0.0f && max_rough_offset > ctx.finish_doc)
    {
        stock = ctx.finish_doc;
        if (!lc_emit(send, user, "(ID rough finish-stock %.3f)", stock)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
        if (!lc_emit(send, user, "G0 X%.3f Z%.3f", profile.d_start - stock, zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
        if (!lc_emit(send, user, "G1 X%.3f F%.3f", profile.d_start - stock, ctx.rough_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
        if (!lc_emit(send, user, "G1 X%.3f Z%.3f", profile.d_profile_end - stock, profile.z_profile_end)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
        r = lc_emit_id_corner(&profile, -stock, send, user, err, err_len);
        if (r != LC_GCODE_OK)
            return r;
        r = lc_emit_q_retract("ID", profile.d_start - stock - tc, zsafe, q, true, send, user, err, err_len);
        if (r != LC_GCODE_OK)
            return r;
    }

    if (!lc_emit(send, user, "(ID finish DT %.3f D2 %.3f)", profile.d_start, profile.d_end)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
    if (!lc_emit(send, user, "G0 X%.3f Z%.3f", profile.d_start, zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
    if (!lc_emit(send, user, "G1 X%.3f F%.3f", profile.d_start, ctx.finish_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
    if (!lc_emit(send, user, "G1 X%.3f Z%.3f", profile.d_profile_end, profile.z_profile_end)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
    r = lc_emit_id_corner(&profile, 0.0f, send, user, err, err_len);
    if (r != LC_GCODE_OK)
        return r;
    r = lc_emit_q_retract("ID", profile.d_start - tc, zsafe, q, true, send, user, err, err_len);
    if (r != LC_GCODE_OK)
        return r;
    if ((!options || options->emit_spindle_stop) && !lc_emit(send, user, "M5")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");

    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_run_face(const char *line,
                                     const char *setup,
                                     const char *tool,
                                     const lc_gcode_line_options_t *options,
                                     lc_gcode_send_fn send,
                                     void *user,
                                     char *err,
                                     unsigned err_len)
{
    float d, z1 = 0.0f, z, doc, tc;
    float inner = 0.0f;
    float pass_z;
    int passes = 0;
    lc_cut_ctx_t ctx;
    lc_gcode_result_t r;

    if (!setup)
        return lc_fail(LC_GCODE_NO_SETUP, err, err_len, "FACE: no SETUP");

    if (!lc_field_float3(line, "D", "OD", "OUTER_DIAMETER", &d))
        lc_setup_float2(setup, "OD", "OUTER_DIAMETER", 0.0f, &d);
    (void)lc_field_float2(line, "Z1", "Z_1", &z1);
    if (!lc_field_float2(line, "Z", "Z_2", &z)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: missing/bad Z");
    r = lc_setup_clearance("OD", setup, &tc, err, err_len);
    if (r != LC_GCODE_OK)
        return r;
    lc_setup_float2(setup, "ID", "INNER_DIAMETER", 0.0f, &inner);
    lc_read_cut_ctx(tool, &ctx);
    lc_override_ctx_from_line(line, &ctx);
    doc = ctx.rough_doc;
    (void)lc_field_float2(line, "DOC", "ROUGH_DOC", &doc);

    if (d <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: D/SETUP.OD must be > 0");
    if (inner < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: SETUP.ID must be >= 0");
    if (inner >= d) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: SETUP.ID must be < D");
    if (z > z1) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: Z must be <= Z1");
    if (doc <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: DOC must be > 0");
    if (lc_too_many_steps(z1 - z, doc))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: too many rough passes");

    if (!lc_emit(send, user, "(LC FACE)")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
    if (!lc_emit_cycle_preamble(send, user, &ctx, options)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: preamble write failed");

    pass_z = z1;
    while (pass_z - doc > z)
    {
        if (++passes > LC_GCODE_MAX_PASSES)
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: too many rough passes");
        pass_z -= doc;
        if (!lc_emit(send, user, "(FACE rough Z%.3f)", pass_z)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
        if (!lc_emit(send, user, "G0 X%.3f Z%.3f", d + tc, pass_z + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
        if (!lc_emit(send, user, "G1 Z%.3f F%.3f", pass_z, ctx.rough_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
        if (!lc_emit(send, user, "G1 X%.3f", inner)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
        if (!lc_emit(send, user, "G0 Z%.3f", pass_z + doc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
        if (!lc_emit(send, user, "G0 X%.3f", d + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
    }

    if (!lc_emit(send, user, "(FACE finish Z%.3f)", z)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
    if (!lc_emit(send, user, "G0 X%.3f Z%.3f", d + tc, z + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
    if (!lc_emit(send, user, "G1 Z%.3f F%.3f", z, ctx.finish_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
    if (!lc_emit(send, user, "G1 X%.3f", inner)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
    if (!lc_emit(send, user, "G0 Z%.3f", z + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
    if (!lc_emit(send, user, "G0 X%.3f", d + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
    if ((!options || options->emit_spindle_stop) && !lc_emit(send, user, "M5")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");

    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_run_drill(const char *line,
                                      const char *tool,
                                      const lc_gcode_line_options_t *options,
                                      lc_gcode_send_fn send,
                                      void *user,
                                      char *err,
                                      unsigned err_len)
{
    float z1, depth, peck = 0.0f;
    float feed;
    float target;
    int passes = 0;
    int tool_no = 0;
    int has_tool_no;
    float tool_d = 0.0f;
    int has_tool_d;
    lc_cut_ctx_t ctx;

    if (!lc_field_float2(line, "Z1", "Z_START", &z1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "DRILL: missing/bad Z1");
    if (!lc_field_float(line, "DEPTH", &depth)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "DRILL: missing/bad DEPTH");
    lc_read_cut_ctx(tool, &ctx);
    lc_override_ctx_from_line(line, &ctx);
    feed = ctx.rough_feed;
    (void)lc_field_float(line, "PECK", &peck);
    (void)lc_field_float(line, "FEED", &feed);

    target = (depth <= 0.0f) ? depth : (z1 - depth);

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

static lc_gcode_result_t lc_run_cut(const char *line,
                                    const char *setup,
                                    const char *tool,
                                    const lc_gcode_line_options_t *options,
                                    lc_gcode_send_fn send,
                                    void *user,
                                    char *err,
                                    unsigned err_len)
{
    float d, z, width, tc;
    float setup_od;
    int q;
    lc_gcode_result_t r;
    lc_cut_ctx_t ctx;

    if (!setup)
        return lc_fail(LC_GCODE_NO_SETUP, err, err_len, "CUT: no SETUP");

    if (!lc_field_float2(line, "D", "DIAMETER", &d)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "CUT: missing/bad D");
    if (!lc_field_float(line, "Z", &z)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "CUT: missing/bad Z");
    if (!lc_field_float(line, "WIDTH", &width)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "CUT: missing/bad WIDTH");
    r = lc_setup_clearance("FACE", setup, &tc, err, err_len);
    if (r != LC_GCODE_OK)
        return r;
    lc_setup_float2(setup, "OD", "OUTER_DIAMETER", d, &setup_od);

    if (d < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "CUT: D must be >= 0");
    if (width < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "CUT: WIDTH must be >= 0");
    if (setup_od <= d) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "CUT: D must be < SETUP.OD");
    q = lc_line_q_mode(line, 1);
    r = lc_validate_q_mode("CUT", q, err, err_len);
    if (r != LC_GCODE_OK)
        return r;

    lc_read_cut_ctx(tool, &ctx);
    lc_override_ctx_from_line(line, &ctx);
    if (!lc_emit(send, user, "(LC CUT)")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "CUT: write failed");
    if (!lc_emit_cycle_preamble(send, user, &ctx, options)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "CUT: preamble write failed");
    if (!lc_emit(send, user, "G0 X%.3f Z%.3f", setup_od + tc, z)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "CUT: write failed");
    if (!lc_emit(send, user, "G1 X%.3f F%.3f", d, ctx.rough_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "CUT: write failed");
    if (width > 0.0f)
    {
        if (!lc_emit(send, user, "G1 Z%.3f", z - width)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "CUT: write failed");
        if (!lc_emit(send, user, "G1 Z%.3f", z)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "CUT: write failed");
    }
    r = lc_emit_q_retract("CUT", setup_od + tc, z + tc, q, false, send, user, err, err_len);
    if (r != LC_GCODE_OK)
        return r;
    if ((!options || options->emit_spindle_stop) && !lc_emit(send, user, "M5")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "CUT: write failed");

    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_run_groove(const char *line,
                                       const char *setup,
                                       const char *tool,
                                       const lc_gcode_line_options_t *options,
                                       lc_gcode_send_fn send,
                                       void *user,
                                       char *err,
                                       unsigned err_len)
{
    float d1, d2, z1, z2, width, tc;
    int q;
    lc_gcode_result_t r;
    lc_cut_ctx_t ctx;

    if (!setup)
        return lc_fail(LC_GCODE_NO_SETUP, err, err_len, "GROOVE: no SETUP");

    if (!lc_field_float(line, "D1", &d1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: missing/bad D1");
    if (!lc_field_float(line, "D2", &d2)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: missing/bad D2");
    if (!lc_field_float(line, "Z1", &z1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: missing/bad Z1");
    if (!lc_field_float(line, "Z2", &z2)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: missing/bad Z2");
    if (!lc_field_float(line, "WIDTH", &width)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: missing/bad WIDTH");
    r = lc_setup_clearance("GROOVE", setup, &tc, err, err_len);
    if (r != LC_GCODE_OK)
        return r;

    if (d1 <= d2) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: D2 must be < D1");
    if (d2 < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: D2 must be >= 0");
    if (z2 > z1) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: Z2 must be <= Z1");
    if (width < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: WIDTH must be >= 0");
    q = lc_line_q_mode(line, 1);
    r = lc_validate_q_mode("GROOVE", q, err, err_len);
    if (r != LC_GCODE_OK)
        return r;

    lc_read_cut_ctx(tool, &ctx);
    lc_override_ctx_from_line(line, &ctx);
    if (!lc_emit(send, user, "(LC GROOVE)")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");
    if (!lc_emit_cycle_preamble(send, user, &ctx, options)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: preamble write failed");
    if (!lc_emit(send, user, "G0 X%.3f Z%.3f", d1 + tc, z1)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");
    if (!lc_emit(send, user, "G1 X%.3f F%.3f", d2, ctx.rough_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");
    if (!lc_emit(send, user, "G1 Z%.3f", z2)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");
    if (width > 0.0f && z2 == z1)
        if (!lc_emit(send, user, "G1 Z%.3f", z1 - width)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");
    r = lc_emit_q_retract("GROOVE", d1 + tc, z1 + tc, q, false, send, user, err, err_len);
    if (r != LC_GCODE_OK)
        return r;
    if ((!options || options->emit_spindle_stop) && !lc_emit(send, user, "M5")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");

    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_run_thread(const char *line,
                                       const char *setup,
                                       const char *tool,
                                       int is_od,
                                       const lc_gcode_line_options_t *options,
                                       lc_gcode_send_fn send,
                                       void *user,
                                       char *err,
                                       unsigned err_len)
{
    const char *cycle = is_od ? "THR_OD" : "THR_ID";
    float d_start = 0.0f;
    float d_end = 0.0f;
    float z1;
    float z2;
    float pitch;
    float nominal = 0.0f;
    float depth = 0.0f;
    float doc = 0.0f;
    float tc;
    float lead;
    float taper = 0.0f;
    float n_value = 0.0f;
    float strategy_value = 1.0f;
    char field_text[32];
    float pass_depth;
    float last_depth = -1.0f;
    float xsafe;
    float zsafe;
    float z_span;
    int pass = 0;
    int pass_count = 0;
    int strategy = 1;
    int has_nominal;
    lc_cut_ctx_t ctx;
    lc_gcode_result_t r;

    if (!setup)
        return lc_fail(LC_GCODE_NO_SETUP, err, err_len, "%s: no SETUP", cycle);

    if (!lc_field_float3(line, "P", "PITCH", "K", &pitch))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: missing/bad P", cycle);

    has_nominal = lc_field_float2(line, "M", "NOM", &nominal);

    if (is_od)
    {
        if (!lc_field_float3(line, "D", "MAJOR", "OD", &d_start) &&
            !lc_field_float2(line, "D1", "OUTSIDE_DIAMETER", &d_start) &&
            has_nominal)
            d_start = nominal;
        if (lc_field_text3(line, "D2", "MINOR", "ID", field_text, sizeof(field_text)) &&
            !lc_parse_float_text(field_text, &d_end))
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "THR_OD: bad D2/MINOR");
        if (d_end <= 0.0f && has_nominal)
            d_end = nominal - (1.22687f * pitch);
    }
    else
    {
        if (!lc_field_float3(line, "D", "MINOR", "ID", &d_start) &&
            !lc_field_float2(line, "D1", "INSIDE_DIAMETER", &d_start) &&
            has_nominal)
            d_start = nominal - (1.08253f * pitch);
        if (lc_field_text3(line, "D2", "MAJOR", "OD", field_text, sizeof(field_text)) &&
            !lc_parse_float_text(field_text, &d_end))
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "THR_ID: bad D2/MAJOR");
        if (d_end <= 0.0f && has_nominal)
            d_end = nominal;
    }

    if (!lc_field_float2(line, "Z1", "Z_START", &z1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: missing/bad Z1", cycle);
    if (!lc_field_float2(line, "Z2", "Z_END", &z2)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: missing/bad Z2", cycle);

    (void)lc_field_float3(line, "THR_DEPTH", "THREAD_DEPTH", "DEPTH", &depth);
    if (d_end <= 0.0f && depth > 0.0f)
        d_end = is_od ? (d_start - depth) : (d_start + depth);

    r = lc_setup_clearance(cycle, setup, &tc, err, err_len);
    if (r != LC_GCODE_OK)
        return r;
    if (lc_field_text3(line, "DOC", "J", "DEPTH_OF_CUT", field_text, sizeof(field_text)))
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
    (void)lc_field_float3(line, "TAPER", "D_TAPER", "TAPER_DIAMETER", &taper);
    if (lc_field_float3(line, "N", "PASS", "PASSES", &n_value) && n_value > 0.0f)
        pass_count = (int)(n_value + 0.5f);
    if (lc_field_float3(line, "ST", "STRAT", "STRATEGY", &strategy_value))
        strategy = strategy_value > 0.5f ? 1 : 0;

    if (d_start <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: M/D must be > 0", cycle);
    if (d_end <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: need M or D2/depth", cycle);
    if (is_od && d_end >= d_start) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "THR_OD: final diameter must be smaller than start");
    if (!is_od && d_end <= d_start) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "THR_ID: final diameter must be larger than start");
    if (z1 == z2) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: Z span must be nonzero", cycle);
    if (pitch <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: P must be > 0", cycle);
    if (doc <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: DOC must be > 0", cycle);
    if (lead < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: LEAD must be >= 0", cycle);
    if (pass_count < 0 || pass_count > LC_GCODE_MAX_PASSES) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: bad pass count", cycle);

    depth = lc_absf(d_start - d_end);
    if (pass_count == 0 && lc_too_many_steps(depth, doc))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: too many passes", cycle);

    lc_read_cut_ctx(tool, &ctx);
    lc_override_ctx_from_line(line, &ctx);

    z_span = z2 - z1;
    zsafe = z1 + ((z_span < 0.0f) ? lead : -lead);
    xsafe = is_od ? (lc_maxf(d_start, d_start + taper) + tc)
                  : (lc_minf(d_start, d_start + taper) - tc);
    if (xsafe < 0.0f)
        xsafe = 0.0f;

    if (!lc_emit(send, user, "(LC %s M %.3f D %.3f D2 %.3f P %.3f DOC %.3f N %d ST %d)",
                 cycle, nominal, d_start, d_end, pitch, doc, pass_count, strategy))
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

        if (++pass > LC_GCODE_MAX_PASSES)
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "%s: too many passes", cycle);

        if (pass_count > 0)
        {
            float t = (float)pass / (float)pass_count;
            pass_depth = depth * (strategy ? sqrtf(t) : t);
        }
        else
        {
            pass_depth = strategy ? (doc * sqrtf((float)pass)) : (doc * (float)pass);
        }
        if (pass_depth > depth)
            pass_depth = depth;
        if (pass_depth <= last_depth)
            pass_depth = depth;

        pass_x1 = is_od ? (d_start - pass_depth) : (d_start + pass_depth);
        pass_x2 = pass_x1 + taper;

        if (!lc_emit(send, user, "(THREAD pass %d X%.3f)", pass, pass_x1)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        if (!lc_emit(send, user, "G0 X%.3f Z%.3f", xsafe, zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        if (!lc_emit(send, user, "G0 X%.3f", pass_x1)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        if (!lc_emit(send, user, "G33 X%.3f Z%.3f K%.3f", pass_x2, z2, pitch)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
        if (!lc_emit(send, user, "G0 X%.3f", xsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);

        last_depth = pass_depth;
    }

    if (!lc_emit(send, user, "G0 Z%.3f", zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);
    if ((!options || options->emit_spindle_stop) && !lc_emit(send, user, "M5")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "%s: write failed", cycle);

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

    if (lc_starts_with(line, "OD|"))
        return lc_run_od(line, setup_line, tool_line, options, send, user, err, err_len);
    if (lc_starts_with(line, "ID|"))
        return lc_run_id(line, setup_line, tool_line, options, send, user, err, err_len);
    if (lc_starts_with(line, "FACE|"))
        return lc_run_face(line, setup_line, tool_line, options, send, user, err, err_len);
    if (lc_starts_with(line, "DRILL|"))
        return lc_run_drill(line, tool_line, options, send, user, err, err_len);
    if (lc_starts_with(line, "CUT|"))
        return lc_run_cut(line, setup_line, tool_line, options, send, user, err, err_len);
    if (lc_starts_with(line, "PART|"))
        return lc_run_cut(line, setup_line, tool_line, options, send, user, err, err_len);
    if (lc_starts_with(line, "GROOVE|"))
        return lc_run_groove(line, setup_line, tool_line, options, send, user, err, err_len);
    if (lc_starts_with(line, "THR_OD|"))
        return lc_run_thread(line, setup_line, tool_line, 1, options, send, user, err, err_len);
    if (lc_starts_with(line, "THR_ID|"))
        return lc_run_thread(line, setup_line, tool_line, 0, options, send, user, err, err_len);

    return lc_fail(LC_GCODE_UNSUPPORTED, err, err_len, "unsupported cycle");
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
    if (!lc_emit(send, user, "(LeanCam generated)")) return 0;
    if (!lc_emit(send, user, "(Check setup, tools, and first motion before running)")) return 0;
    return lc_emit_modal_header(send, user);
}

int leancam_gcode_emit_program_footer(lc_gcode_send_fn send, void *user)
{
    if (!lc_emit(send, user, "M5")) return 0;
    if (!lc_emit(send, user, "M30")) return 0;
    return 1;
}

lc_gcode_result_t leancam_gcode_run_line(const char *line,
                                         const char *setup_line,
                                         const char *tool_line,
                                         lc_gcode_send_fn send,
                                         void *user)
{
    return leancam_gcode_run_line_ex(line, setup_line, tool_line, send, user, NULL, 0);
}
