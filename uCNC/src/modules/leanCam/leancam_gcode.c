#include "leancam_gcode.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LC_GCODE_FEED_MM_MIN
#define LC_GCODE_FEED_MM_MIN 120.0f
#endif

#ifndef LC_GCODE_SPINDLE_RPM
#define LC_GCODE_SPINDLE_RPM 800
#endif

typedef struct
{
    float rough_feed;
    float finish_feed;
    float rough_doc;
    float finish_doc;
    int spindle_rpm;
} lc_cut_ctx_t;

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
    char *endp;
    float v;

    if (!lc_get_field_text(line, name, buf, sizeof(buf)))
        return 0;

    if (buf[0] == '(' || buf[0] == '*' || buf[0] == 0)
        return 0;

    v = (float)strtod(buf, &endp);
    if (endp == buf)
        return 0;

    *out = v;
    return 1;
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

static void lc_setup_float2(const char *setup, const char *a, const char *b, float def, float *out)
{
    if (setup && lc_field_float2(setup, a, b, out))
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

    (void)lc_field_float2(tool, "ROUGH_FEED", "FEED", &ctx->rough_feed);
    (void)lc_field_float2(tool, "FIN_FEED", "FINISH_FEED", &ctx->finish_feed);
    (void)lc_field_float2(tool, "ROUGH_DOC", "ROUGH_DEPTH_OF_CUT", &ctx->rough_doc);
    (void)lc_field_float2(tool, "FIN_DOC", "FINISH_DEPTH_OF_CUT", &ctx->finish_doc);
    {
        float rpm;
        if (lc_field_float2(tool, "RPM", "SPINDLE_RPM", &rpm) && rpm > 0.0f)
            ctx->spindle_rpm = (int)rpm;
    }

    if (ctx->rough_feed <= 0.0f) ctx->rough_feed = LC_GCODE_FEED_MM_MIN;
    if (ctx->finish_feed <= 0.0f) ctx->finish_feed = ctx->rough_feed;
    if (ctx->rough_doc <= 0.0f) ctx->rough_doc = 2.0f;
    if (ctx->finish_doc < 0.0f) ctx->finish_doc = 0.0f;
}

static int lc_emit_preamble(lc_gcode_send_fn send, void *user, const lc_cut_ctx_t *ctx)
{
    if (!lc_emit(send, user, "G21")) return 0;
    if (!lc_emit(send, user, "G90")) return 0;
    if (!lc_emit(send, user, "G8"))  return 0;
    if (!lc_emit(send, user, "S%d M3", ctx ? ctx->spindle_rpm : LC_GCODE_SPINDLE_RPM)) return 0;
    return 1;
}

static lc_gcode_result_t lc_run_od(const char *line,
                                   const char *setup,
                                   const char *tool,
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
    lc_cut_ctx_t ctx;

    if (!setup)
        return lc_fail(LC_GCODE_NO_SETUP, err, err_len, "OD: no SETUP");

    if (!lc_field_float2(line, "D1", "DIAMETER_1", &d1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: missing/bad D1");
    if (!lc_field_float2(line, "Z1", "Z_1",        &z1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: missing/bad Z1");
    if (!lc_field_float2(line, "Z2", "Z_2",        &z2)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: missing/bad Z2");
    if (!lc_field_float2(line, "D2", "DIAMETER_2", &d2)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "OD: missing/bad D2");

    if (!lc_field_float2(line, "CLEAR", "TOOL_CLEARANCE", &tc))
        lc_setup_float2(setup, "CLEAR", "TOOL_CLEARANCE", 1.0f, &tc);

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

    lc_read_cut_ctx(tool, &ctx);
    if (ctx.finish_doc >= (d1 - d2))
        ctx.finish_doc = 0.0f;

    if (!lc_emit(send, user, "(LC OD)")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");
    if (!lc_emit_preamble(send, user, &ctx)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: preamble write failed");
    if (!lc_emit(send, user, "G0 X%.3f Z%.3f", xsafe, zsafe)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");

    rough_target = d2 + ctx.finish_doc;
    pass_d = d1;
    while (pass_d - ctx.rough_doc > rough_target)
    {
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
    if (!lc_emit(send, user, "M5")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "OD: write failed");

    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_run_id(const char *line,
                                   const char *setup,
                                   const char *tool,
                                   lc_gcode_send_fn send,
                                   void *user,
                                   char *err,
                                   unsigned err_len)
{
    float d1, z1, d2, z2, tc;
    float zsafe;
    float rough_target;
    float pass_d;
    lc_cut_ctx_t ctx;

    if (!setup)
        return lc_fail(LC_GCODE_NO_SETUP, err, err_len, "ID: no SETUP");

    if (!lc_field_float2(line, "D1", "DIAMETER_1", &d1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: missing/bad D1");
    if (!lc_field_float2(line, "Z1", "Z_1",        &z1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: missing/bad Z1");
    if (!lc_field_float2(line, "Z2", "Z_2",        &z2)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: missing/bad Z2");
    if (!lc_field_float2(line, "D2", "DIAMETER_2", &d2)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: missing/bad D2");

    if (!lc_field_float2(line, "CLEAR", "TOOL_CLEARANCE", &tc))
        lc_setup_float2(setup, "CLEAR", "TOOL_CLEARANCE", 1.0f, &tc);

    if (d1 <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: D1 must be > 0");
    if (d2 < d1) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: D2 must be >= D1");
    if (z2 > z1) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "ID: Z2 must be <= Z1");

    lc_read_cut_ctx(tool, &ctx);
    if (ctx.finish_doc >= (d2 - d1))
        ctx.finish_doc = 0.0f;

    zsafe = z1 + tc;
    if (!lc_emit(send, user, "(LC ID)")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");
    if (!lc_emit_preamble(send, user, &ctx)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: preamble write failed");

    rough_target = d2 - ctx.finish_doc;
    pass_d = d1;
    while (pass_d + ctx.rough_doc < rough_target)
    {
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
    if (!lc_emit(send, user, "M5")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "ID: write failed");

    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_run_face(const char *line,
                                     const char *setup,
                                     const char *tool,
                                     lc_gcode_send_fn send,
                                     void *user,
                                     char *err,
                                     unsigned err_len)
{
    float d, z1 = 0.0f, z, doc, tc;
    float inner = 0.0f;
    float pass_z;
    lc_cut_ctx_t ctx;

    if (!setup)
        return lc_fail(LC_GCODE_NO_SETUP, err, err_len, "FACE: no SETUP");

    if (!lc_field_float3(line, "D", "OD", "OUTER_DIAMETER", &d))
        lc_setup_float2(setup, "OD", "OUTER_DIAMETER", 0.0f, &d);
    (void)lc_field_float2(line, "Z1", "Z_1", &z1);
    if (!lc_field_float2(line, "Z", "Z_2", &z)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: missing/bad Z");
    if (!lc_field_float2(line, "DOC", "ROUGH_DOC", &doc)) doc = 1.0f;
    if (!lc_field_float2(line, "CLEAR", "TOOL_CLEARANCE", &tc))
        lc_setup_float2(setup, "CLEAR", "TOOL_CLEARANCE", 1.0f, &tc);
    lc_setup_float2(setup, "ID", "INNER_DIAMETER", 0.0f, &inner);

    if (d <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: D/SETUP.OD must be > 0");
    if (inner < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: SETUP.ID must be >= 0");
    if (inner >= d) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: SETUP.ID must be < D");
    if (z > z1) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: Z must be <= Z1");
    if (doc <= 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "FACE: DOC must be > 0");

    lc_read_cut_ctx(tool, &ctx);
    if (!lc_emit(send, user, "(LC FACE)")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");
    if (!lc_emit_preamble(send, user, &ctx)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: preamble write failed");

    pass_z = z1;
    while (pass_z - doc > z)
    {
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
    if (!lc_emit(send, user, "M5")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "FACE: write failed");

    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_run_drill(const char *line,
                                      lc_gcode_send_fn send,
                                      void *user,
                                      char *err,
                                      unsigned err_len)
{
    float z1, depth, peck = 0.0f;
    float feed = LC_GCODE_FEED_MM_MIN;
    float target;
    lc_cut_ctx_t ctx;

    if (!lc_field_float2(line, "Z1", "Z_START", &z1)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "DRILL: missing/bad Z1");
    if (!lc_field_float(line, "DEPTH", &depth)) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "DRILL: missing/bad DEPTH");
    (void)lc_field_float(line, "PECK", &peck);
    (void)lc_field_float(line, "FEED", &feed);
    lc_read_cut_ctx(NULL, &ctx);

    target = (depth <= 0.0f) ? depth : (z1 - depth);

    if (!lc_emit(send, user, "(LC DRILL)")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: write failed");
    if (!lc_emit_preamble(send, user, &ctx)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: preamble write failed");
    if (!lc_emit(send, user, "G0 X0 Z%.3f", z1)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: write failed");

    if (peck > 0.0f)
    {
        float z = z1 - peck;
        while (z > target)
        {
            if (!lc_emit(send, user, "G1 Z%.3f F%.3f", z, feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: write failed");
            if (!lc_emit(send, user, "G0 Z%.3f", z1)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: write failed");
            z -= peck;
        }
    }

    if (!lc_emit(send, user, "G1 Z%.3f F%.3f", target, feed)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: write failed");
    if (!lc_emit(send, user, "G0 Z%.3f", z1)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: write failed");
    if (!lc_emit(send, user, "M5")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "DRILL: write failed");

    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_run_cut(const char *line,
                                    const char *setup,
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
    if (!lc_field_float2(line, "CLEAR", "TOOL_CLEARANCE", &tc))
        lc_setup_float2(setup, "CLEAR", "TOOL_CLEARANCE", 1.0f, &tc);
    lc_setup_float2(setup, "OD", "OUTER_DIAMETER", d, &setup_od);

    if (d < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "CUT: D must be >= 0");
    if (width < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "CUT: WIDTH must be >= 0");
    if (setup_od <= d) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "CUT: D must be < SETUP.OD");

    lc_read_cut_ctx(NULL, &ctx);
    if (!lc_emit(send, user, "(LC CUT)")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "CUT: write failed");
    if (!lc_emit_preamble(send, user, &ctx)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "CUT: preamble write failed");
    if (!lc_emit(send, user, "G0 X%.3f Z%.3f", setup_od + tc, z)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "CUT: write failed");
    if (!lc_emit(send, user, "G1 X%.3f F%.3f", d, LC_GCODE_FEED_MM_MIN)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "CUT: write failed");
    if (width > 0.0f)
    {
        if (!lc_emit(send, user, "G1 Z%.3f", z - width)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "CUT: write failed");
        if (!lc_emit(send, user, "G1 Z%.3f", z)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "CUT: write failed");
    }
    if (!lc_emit(send, user, "G0 X%.3f", setup_od + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "CUT: write failed");
    if (!lc_emit(send, user, "M5")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "CUT: write failed");

    return LC_GCODE_OK;
}

static lc_gcode_result_t lc_run_groove(const char *line,
                                       const char *setup,
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
    if (!lc_field_float2(line, "CLEAR", "TOOL_CLEARANCE", &tc))
        lc_setup_float2(setup, "CLEAR", "TOOL_CLEARANCE", 1.0f, &tc);

    if (d1 <= d2) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: D2 must be < D1");
    if (d2 < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: D2 must be >= 0");
    if (z2 > z1) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: Z2 must be <= Z1");
    if (width < 0.0f) return lc_fail(LC_GCODE_BAD_FIELD, err, err_len, "GROOVE: WIDTH must be >= 0");

    lc_read_cut_ctx(NULL, &ctx);
    if (!lc_emit(send, user, "(LC GROOVE)")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");
    if (!lc_emit_preamble(send, user, &ctx)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: preamble write failed");
    if (!lc_emit(send, user, "G0 X%.3f Z%.3f", d1 + tc, z1)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");
    if (!lc_emit(send, user, "G1 X%.3f F%.3f", d2, LC_GCODE_FEED_MM_MIN)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");
    if (!lc_emit(send, user, "G1 Z%.3f", z2)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");
    if (width > 0.0f && z2 == z1)
        if (!lc_emit(send, user, "G1 Z%.3f", z1 - width)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");
    if (!lc_emit(send, user, "G0 X%.3f", d1 + tc)) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");
    if (!lc_emit(send, user, "M5")) return lc_fail(LC_GCODE_STREAM_REJECT, err, err_len, "GROOVE: write failed");

    return LC_GCODE_OK;
}

lc_gcode_result_t leancam_gcode_run_line_ex(const char *line,
                                            const char *setup_line,
                                            const char *tool_line,
                                            lc_gcode_send_fn send,
                                            void *user,
                                            char *err,
                                            unsigned err_len)
{
    if (err && err_len > 0)
        err[0] = 0;

    if (!line || !line[0])
        return lc_fail(LC_GCODE_UNSUPPORTED, err, err_len, "empty line");

    if (lc_starts_with(line, "OD|"))
        return lc_run_od(line, setup_line, tool_line, send, user, err, err_len);
    if (lc_starts_with(line, "ID|"))
        return lc_run_id(line, setup_line, tool_line, send, user, err, err_len);
    if (lc_starts_with(line, "FACE|"))
        return lc_run_face(line, setup_line, tool_line, send, user, err, err_len);
    if (lc_starts_with(line, "DRILL|"))
        return lc_run_drill(line, send, user, err, err_len);
    if (lc_starts_with(line, "CUT|"))
        return lc_run_cut(line, setup_line, send, user, err, err_len);
    if (lc_starts_with(line, "PART|"))
        return lc_run_cut(line, setup_line, send, user, err, err_len);
    if (lc_starts_with(line, "GROOVE|"))
        return lc_run_groove(line, setup_line, send, user, err, err_len);

    return lc_fail(LC_GCODE_UNSUPPORTED, err, err_len, "unsupported cycle");
}

lc_gcode_result_t leancam_gcode_run_line(const char *line,
                                         const char *setup_line,
                                         const char *tool_line,
                                         lc_gcode_send_fn send,
                                         void *user)
{
    return leancam_gcode_run_line_ex(line, setup_line, tool_line, send, user, NULL, 0);
}
