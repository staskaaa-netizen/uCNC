#include "leancam_gcode.h"

#include <stdarg.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if (ctx->rough_doc <= 0.0f) ctx->rough_doc = 2.0f;
    if (ctx->finish_doc < 0.0f) ctx->finish_doc = 0.0f;
    if (ctx->spindle_rpm <= 0) ctx->spindle_rpm = LC_GCODE_SPINDLE_RPM;
}

static void lc_override_ctx_from_line(const char *line, lc_cut_ctx_t *ctx)
{
    float rpm;

    if (!line || !ctx)
        return;

    (void)lc_field_float3(line, "R_FEED", "ROUGH_FEED", "FEED", &ctx->rough_feed);
    (void)lc_field_float2(line, "FIN_FEED", "FINISH_FEED", &ctx->finish_feed);
    (void)lc_field_float3(line, "R_DOC", "ROUGH_DOC", "ROUGH_DEPTH_OF_CUT", &ctx->rough_doc);
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

static void lc_setup_float3(const char *setup, const char *a, const char *b, const char *c, float def, float *out)
{
    if (setup && lc_field_float3(setup, a, b, c, out))
        return;

    *out = def;
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

    if (!tool)
        return;

    (void)lc_field_float3(tool, "R_FEED", "ROUGH_FEED", "FEED", &ctx->rough_feed);
    (void)lc_field_float2(tool, "FIN_FEED", "FINISH_FEED", &ctx->finish_feed);
    (void)lc_field_float3(tool, "R_DOC", "ROUGH_DOC", "ROUGH_DEPTH_OF_CUT", &ctx->rough_doc);
    (void)lc_field_float2(tool, "FIN_DOC", "FINISH_DEPTH_OF_CUT", &ctx->finish_doc);
    {
        float rpm;
        if (lc_field_float3(tool, "S", "RPM", "SPINDLE_RPM", &rpm) && rpm > 0.0f)
            ctx->spindle_rpm = (int)rpm;
    }

    lc_sanitize_cut_ctx(ctx);
}

static int lc_emit_modal_header(lc_gcode_send_fn send, void *user)
{
    if (!lc_emit(send, user, "G21")) return 0;
    if (!lc_emit(send, user, "G90")) return 0;
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
    float rough_target;
    float pass_d;
    int passes = 0;
    lc_cut_ctx_t ctx;

    if (!setup)
        return lc_fail(LC_GCODE_NO_SETUP, err, err_len, "OD: no SETUP");

    if (!lc_field_float2(line, "D1", "DIAMETER_1", &d1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: missing/bad D1");
    if (!lc_field_float2(line, "Z1", "Z_1",        &z1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: missing/bad Z1");
    if (!lc_field_float2(line, "Z2", "Z_2",        &z2)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: missing/bad Z2");
    if (!lc_field_float2(line, "D2", "DIAMETER_2", &d2)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: missing/bad D2");

    if (!lc_field_float3(line, "CLR", "CLEAR", "TOOL_CLEARANCE", &tc))
        lc_setup_float3(setup, "CLR", "CLEAR", "TOOL_CLEARANCE", 1.0f, &tc);

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
    if (tc < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: CLEAR must be >= 0");

    lc_read_cut_ctx(tool, &ctx);
    lc_override_ctx_from_line(line, &ctx);
    if (ctx.finish_doc >= (d1 - d2))
        ctx.finish_doc = 0.0f;
    rough_target = d2 + ctx.finish_doc;
    if (lc_too_many_steps(d1 - rough_target, ctx.rough_doc))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: too many rough passes");

    if (!lc_emit(send, user, "(LC OD)")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
    if (!lc_emit_cycle_preamble(send, user, &ctx, options)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: preamble write failed");
    if (!lc_emit(send, user, "G0 X%.3f Z%.3f", xsafe, zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");

    pass_d = d1;
    while (pass_d - ctx.rough_doc > rough_target)
    {
        if (++passes > LC_GCODE_MAX_PASSES)
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: too many rough passes");
        pass_d -= ctx.rough_doc;
        if (!lc_emit(send, user, "(OD rough D%.3f)", pass_d)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
        if (!lc_emit(send, user, "G0 X%.3f Z%.3f", pass_d + tc, z1 + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
        if (!lc_emit(send, user, "G1 X%.3f F%.3f", pass_d, ctx.rough_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
        if (!lc_emit(send, user, "G1 Z%.3f", z2)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
        if (!lc_emit(send, user, "G0 X%.3f", xsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
        if (!lc_emit(send, user, "G0 Z%.3f", zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
    }

    if (rough_target > d2)
    {
        if (!lc_emit(send, user, "(OD rough finish-stock D%.3f)", rough_target)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
        if (!lc_emit(send, user, "G0 X%.3f Z%.3f", rough_target + tc, z1 + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
        if (!lc_emit(send, user, "G1 X%.3f F%.3f", rough_target, ctx.rough_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
        if (!lc_emit(send, user, "G1 Z%.3f", z2)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
        if (!lc_emit(send, user, "G0 X%.3f", xsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
        if (!lc_emit(send, user, "G0 Z%.3f", zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
    }

    if (!lc_emit(send, user, "(OD finish D%.3f)", d2)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
    if (!lc_emit(send, user, "G0 X%.3f Z%.3f", d2 + tc, z1 + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
    if (!lc_emit(send, user, "G1 X%.3f F%.3f", d2, ctx.finish_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
    if (!lc_emit(send, user, "G1 Z%.3f", z2)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
    if (!lc_emit(send, user, "G0 X%.3f", xsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
    if (!lc_emit(send, user, "G0 Z%.3f", zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
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
    float rough_target;
    float pass_d;
    int passes = 0;
    lc_cut_ctx_t ctx;

    if (!setup)
        return lc_fail(LC_GCODE_NO_SETUP, err, err_len, "ID: no SETUP");

    if (!lc_field_float2(line, "D1", "DIAMETER_1", &d1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: missing/bad D1");
    if (!lc_field_float2(line, "Z1", "Z_1",        &z1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: missing/bad Z1");
    if (!lc_field_float2(line, "Z2", "Z_2",        &z2)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: missing/bad Z2");
    if (!lc_field_float2(line, "D2", "DIAMETER_2", &d2)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: missing/bad D2");

    if (!lc_field_float3(line, "CLR", "CLEAR", "TOOL_CLEARANCE", &tc))
        lc_setup_float3(setup, "CLR", "CLEAR", "TOOL_CLEARANCE", 1.0f, &tc);

    if (d1 <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: D1 must be > 0");
    if (d2 < d1) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: D2 must be >= D1");
    if (z2 > z1) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: Z2 must be <= Z1");
    if (tc < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: CLEAR must be >= 0");

    lc_read_cut_ctx(tool, &ctx);
    lc_override_ctx_from_line(line, &ctx);
    if (ctx.finish_doc >= (d2 - d1))
        ctx.finish_doc = 0.0f;
    rough_target = d2 - ctx.finish_doc;
    if (lc_too_many_steps(rough_target - d1, ctx.rough_doc))
        return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: too many rough passes");

    zsafe = z1 + tc;
    if (!lc_emit(send, user, "(LC ID)")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
    if (!lc_emit_cycle_preamble(send, user, &ctx, options)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: preamble write failed");

    pass_d = d1;
    while (pass_d + ctx.rough_doc < rough_target)
    {
        if (++passes > LC_GCODE_MAX_PASSES)
            return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: too many rough passes");
        pass_d += ctx.rough_doc;
        if (!lc_emit(send, user, "(ID rough D%.3f)", pass_d)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
        if (!lc_emit(send, user, "G0 X%.3f Z%.3f", pass_d, zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
        if (!lc_emit(send, user, "G1 Z%.3f F%.3f", z2, ctx.rough_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
        if (!lc_emit(send, user, "G0 Z%.3f", zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
    }

    if (rough_target > d1)
    {
        if (!lc_emit(send, user, "(ID rough finish-stock D%.3f)", rough_target)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
        if (!lc_emit(send, user, "G0 X%.3f Z%.3f", rough_target, zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
        if (!lc_emit(send, user, "G1 Z%.3f F%.3f", z2, ctx.rough_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
        if (!lc_emit(send, user, "G0 Z%.3f", zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
    }

    if (!lc_emit(send, user, "(ID finish D%.3f)", d2)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
    if (!lc_emit(send, user, "G0 X%.3f Z%.3f", d2, zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
    if (!lc_emit(send, user, "G1 Z%.3f F%.3f", z2, ctx.finish_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
    if (!lc_emit(send, user, "G0 Z%.3f", zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
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

    if (!setup)
        return lc_fail(LC_GCODE_NO_SETUP, err, err_len, "FACE: no SETUP");

    if (!lc_field_float3(line, "D", "OD", "OUTER_DIAMETER", &d))
        lc_setup_float2(setup, "OD", "OUTER_DIAMETER", 0.0f, &d);
    (void)lc_field_float2(line, "Z1", "Z_1", &z1);
    if (!lc_field_float2(line, "Z", "Z_2", &z)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: missing/bad Z");
    if (!lc_field_float2(line, "DOC", "ROUGH_DOC", &doc)) doc = 1.0f;
    if (!lc_field_float3(line, "CLR", "CLEAR", "TOOL_CLEARANCE", &tc))
        lc_setup_float3(setup, "CLR", "CLEAR", "TOOL_CLEARANCE", 1.0f, &tc);
    lc_setup_float2(setup, "ID", "INNER_DIAMETER", 0.0f, &inner);

    if (d <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: D/SETUP.OD must be > 0");
    if (inner < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: SETUP.ID must be >= 0");
    if (inner >= d) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: SETUP.ID must be < D");
    if (z > z1) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: Z must be <= Z1");
    if (doc <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: DOC must be > 0");
    if (tc < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: CLEAR must be >= 0");

    lc_read_cut_ctx(tool, &ctx);
    lc_override_ctx_from_line(line, &ctx);
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
        if (!lc_emit(send, user, "G0 X%.3f", d + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
    }

    if (!lc_emit(send, user, "(FACE finish Z%.3f)", z)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
    if (!lc_emit(send, user, "G0 X%.3f Z%.3f", d + tc, z + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
    if (!lc_emit(send, user, "G1 Z%.3f F%.3f", z, ctx.finish_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
    if (!lc_emit(send, user, "G1 X%.3f", inner)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
    if (!lc_emit(send, user, "G0 X%.3f", d + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
    if (!lc_emit(send, user, "G0 Z%.3f", z + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
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
    lc_cut_ctx_t ctx;

    if (!setup)
        return lc_fail(LC_GCODE_NO_SETUP, err, err_len, "CUT: no SETUP");

    if (!lc_field_float2(line, "D", "DIAMETER", &d)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "CUT: missing/bad D");
    if (!lc_field_float(line, "Z", &z)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "CUT: missing/bad Z");
    if (!lc_field_float(line, "WIDTH", &width)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "CUT: missing/bad WIDTH");
    if (!lc_field_float3(line, "CLR", "CLEAR", "TOOL_CLEARANCE", &tc))
        lc_setup_float3(setup, "CLR", "CLEAR", "TOOL_CLEARANCE", 1.0f, &tc);
    lc_setup_float2(setup, "OD", "OUTER_DIAMETER", d, &setup_od);

    if (d < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "CUT: D must be >= 0");
    if (width < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "CUT: WIDTH must be >= 0");
    if (setup_od <= d) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "CUT: D must be < SETUP.OD");
    if (tc < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "CUT: CLEAR must be >= 0");

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
    if (!lc_emit(send, user, "G0 X%.3f", setup_od + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "CUT: write failed");
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
    lc_cut_ctx_t ctx;

    if (!setup)
        return lc_fail(LC_GCODE_NO_SETUP, err, err_len, "GROOVE: no SETUP");

    if (!lc_field_float(line, "D1", &d1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: missing/bad D1");
    if (!lc_field_float(line, "D2", &d2)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: missing/bad D2");
    if (!lc_field_float(line, "Z1", &z1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: missing/bad Z1");
    if (!lc_field_float(line, "Z2", &z2)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: missing/bad Z2");
    if (!lc_field_float(line, "WIDTH", &width)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: missing/bad WIDTH");
    if (!lc_field_float3(line, "CLR", "CLEAR", "TOOL_CLEARANCE", &tc))
        lc_setup_float3(setup, "CLR", "CLEAR", "TOOL_CLEARANCE", 1.0f, &tc);

    if (d1 <= d2) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: D2 must be < D1");
    if (d2 < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: D2 must be >= 0");
    if (z2 > z1) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: Z2 must be <= Z1");
    if (width < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: WIDTH must be >= 0");
    if (tc < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: CLEAR must be >= 0");

    lc_read_cut_ctx(tool, &ctx);
    lc_override_ctx_from_line(line, &ctx);
    if (!lc_emit(send, user, "(LC GROOVE)")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");
    if (!lc_emit_cycle_preamble(send, user, &ctx, options)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: preamble write failed");
    if (!lc_emit(send, user, "G0 X%.3f Z%.3f", d1 + tc, z1)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");
    if (!lc_emit(send, user, "G1 X%.3f F%.3f", d2, ctx.rough_feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");
    if (!lc_emit(send, user, "G1 Z%.3f", z2)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");
    if (width > 0.0f && z2 == z1)
        if (!lc_emit(send, user, "G1 Z%.3f", z1 - width)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");
    if (!lc_emit(send, user, "G0 X%.3f", d1 + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");
    if ((!options || options->emit_spindle_stop) && !lc_emit(send, user, "M5")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");

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
