#include "leancam_bridge.h"
#include "leancam_ui.h"
#include "leancam_templates.h"
#include "leancam_files.h"
#include "leancam_expr.h"
#include "leancam_gcode.h"
#if LC_SELECTED_RUN_USE_STREAM
#include "cam_stream.h"
#endif
#include "../file_system.h"
#include "../../cnc.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>

#define LC_FILES_DIR LC_DEFAULT_DIR

#ifndef LC_VISIBLE_PROGRAM_LINES
#define LC_VISIBLE_PROGRAM_LINES 6
#endif

#ifndef LC_FILE_REFRESH_RETRY_MS
#define LC_FILE_REFRESH_RETRY_MS 1500u
#endif

#ifndef LC_SELECTED_RUN_USE_STREAM
#define LC_SELECTED_RUN_USE_STREAM 0
#endif

#ifndef LEANCAM_DIRECT_STREAM_BUFFER
#define LEANCAM_DIRECT_STREAM_BUFFER 8192u
#endif

#ifndef G33_ELS_LOCK_REV_MIN
#define G33_ELS_LOCK_REV_MIN 2
#endif

typedef enum
{
    LC_MODE_FILES = 0,
    LC_MODE_FILE_NAME,
    LC_MODE_PROGRAM,
    LC_MODE_DRAFT,
    LC_MODE_NC_VIEW
} lc_mode_t;

static leancam_ui_t g_leancam_ui;
static lc_mode_t g_lc_mode = LC_MODE_FILES;
static int  g_file_sel = 0;
static int  g_prog_scroll = 0;
static int  g_nc_top_line = 0;
static int  g_nc_selected_row = 0;
static char g_new_file_name[32];
static char g_last_msg[UI_SNAPSHOT_POPUP_LEN];
static char g_nc_path[LC_FILE_PATH_MAX];
static char g_nc_setup_line[UI_LC_LINE_LEN];
static char g_nc_lines[UI_LC_MAX_LINES][UI_LC_LINE_LEN];
static uint8_t g_nc_line_count = 0;
static bool g_nc_eof = false;
static uint8_t g_draft_field_index = 0;
static bool g_files_ready = false;
static uint32_t g_next_file_refresh_ms = 0;

static void lc_open_nc_viewer(const char *path);

typedef struct
{
    fs_file_t *fp;
} lc_gcode_file_emit_t;

static const char *lc_basename(const char *path)
{
    const char *p1;
    const char *p2;
    const char *p;

    if (!path || !path[0])
        return "<no file>";

    p1 = strrchr(path, '/');
    p2 = strrchr(path, '\\');
    p = p1;

    if (p2 && (!p || p2 > p))
        p = p2;

    return p ? (p + 1) : path;
}

static void lc_set_msg(const char *s)
{
    if (!s) s = "";
    strncpy(g_last_msg, s, sizeof(g_last_msg) - 1);
    g_last_msg[sizeof(g_last_msg) - 1] = 0;
}

static void lc_set_msgf(const char *fmt, ...)
{
    va_list ap;

    if (!fmt)
    {
        lc_set_msg("");
        return;
    }

    va_start(ap, fmt);
    vsnprintf(g_last_msg, sizeof(g_last_msg), fmt, ap);
    va_end(ap);
}

static void lc_publish_msg(const char *fmt, ...)
{
    va_list ap;

    if (fmt)
    {
        va_start(ap, fmt);
        vsnprintf(g_last_msg, sizeof(g_last_msg), fmt, ap);
        va_end(ap);
    }

    leancam_bridge_request_render();
}

static int lc_stricmp_local(const char *a, const char *b)
{
    while (*a && *b)
    {
        int ca = (*a >= 'A' && *a <= 'Z') ? (*a + ('a' - 'A')) : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? (*b + ('a' - 'A')) : *b;
        if (ca != cb) return ca - cb;
        ++a;
        ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static bool lc_has_suffix_ci_local(const char *name, const char *suffix)
{
    size_t ln;
    size_t ls;

    if (!name || !suffix)
        return false;

    ln = strlen(name);
    ls = strlen(suffix);
    if (ln < ls)
        return false;

    return lc_stricmp_local(name + ln - ls, suffix) == 0;
}

static void lc_refresh_files(void)
{
    int cnt;

    if (!leancam_files_refresh(LC_FILES_DIR))
    {
        g_files_ready = false;
        g_next_file_refresh_ms = mcu_millis() + LC_FILE_REFRESH_RETRY_MS;
        g_file_sel = 0;
        lc_set_msg("LC: refresh failed");
        return;
    }
   // cnc_clear_exec_state(0x80);

    g_files_ready = true;
    g_next_file_refresh_ms = 0;
    cnt = leancam_files_count();

    if (g_file_sel < 0)
        g_file_sel = 0;

    if (g_file_sel > cnt)
        g_file_sel = cnt;

    lc_set_msg("LC: files refreshed");
}

static void lc_delete_selected_file(void)
{
    char path[LC_FILE_PATH_MAX];
    int cnt;

    if (leancam_files_busy())
    {
        lc_set_msg("LC: file IO busy");
        return;
    }

    cnt = leancam_files_count();
    if (g_file_sel < 0 || g_file_sel >= cnt)
    {
        lc_set_msg("LC: no file selected");
        return;
    }

    if (!leancam_files_build_path(LC_FILES_DIR, g_file_sel, path, sizeof(path)))
    {
        lc_set_msg("LC: build path failed");
        return;
    }

    if (!leancam_files_delete_path(path))
    {
        lc_set_msg("LC: delete failed");
        return;
    }

    if (g_leancam_ui.current_path[0] && strcmp(g_leancam_ui.current_path, path) == 0)
        leancam_ui_new(&g_leancam_ui);

    lc_refresh_files();
    cnt = leancam_files_count();
    if (g_file_sel >= cnt)
        g_file_sel = cnt;
    lc_set_msg("LC: file deleted");
}

static const char *lc_gcode_result_name(lc_gcode_result_t r)
{
    switch (r)
    {
        case LC_GCODE_OK:            return "ok";
        case LC_GCODE_UNSUPPORTED:   return "unsupported";
        case LC_GCODE_NO_SETUP:      return "no setup";
        case LC_GCODE_BAD_FIELD:     return "bad field";
        case LC_GCODE_STREAM_REJECT: return "write failed";
        default:                     return "gcode failed";
    }
}

static int lc_write_line_to_file(const char *line, void *user)
{
    lc_gcode_file_emit_t *ctx = (lc_gcode_file_emit_t *)user;
    size_t len;
    uint8_t nl = '\n';

    if (!ctx || !ctx->fp || !line)
        return 0;

    len = strlen(line);
    if (len > 0 && fs_write(ctx->fp, (const uint8_t *)line, len) != len)
        return 0;

    return fs_write(ctx->fp, &nl, 1) == 1;
}

static bool lc_make_gcode_path(const char *lcam_path, char *out, int out_sz)
{
    const char *dot;
    size_t n;

    if (!lcam_path || !out || out_sz <= 0)
        return false;

    dot = strrchr(lcam_path, '.');
    if (!dot)
        dot = lcam_path + strlen(lcam_path);

    n = (size_t)(dot - lcam_path);
    if (n + 3 >= (size_t)out_sz)
        return false;

    memcpy(out, lcam_path, n);
    memcpy(out + n, ".nc", 4);
    return true;
}

static bool lc_is_name_char(char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9');
}

static void lc_sanitize_name_part(const char *in, char *out, size_t out_sz)
{
    size_t oi = 0;
    bool last_us = false;

    if (!out || out_sz == 0)
        return;

    if (!in)
        in = "";

    while (*in && oi + 1 < out_sz)
    {
        char c = *in++;

        if (lc_is_name_char(c) || c == '-')
        {
            out[oi++] = c;
            last_us = false;
        }
        else if (!last_us && oi > 0)
        {
            out[oi++] = '_';
            last_us = true;
        }
    }

    while (oi > 0 && out[oi - 1] == '_')
        --oi;

    out[oi] = 0;
}

static void lc_get_program_stem(char *out, size_t out_sz)
{
    const char *base;
    const char *dot;
    char tmp[48];
    size_t n;

    if (!out || out_sz == 0)
        return;

    base = lc_basename(g_leancam_ui.current_path);
    if (!base || !base[0] || base[0] == '<')
        base = "program";

    dot = strrchr(base, '.');
    n = dot ? (size_t)(dot - base) : strlen(base);
    if (n >= sizeof(tmp))
        n = sizeof(tmp) - 1;

    memcpy(tmp, base, n);
    tmp[n] = 0;

    lc_sanitize_name_part(tmp, out, out_sz);
    if (!out[0])
        strncpy(out, "program", out_sz - 1);
    out[out_sz - 1] = 0;
}

static void lc_get_operation_name(const char *line, char *out, size_t out_sz)
{
    char tmp[20];
    size_t n = 0;

    if (!out || out_sz == 0)
        return;

    if (!line)
        line = "";

    while (line[n] && line[n] != '|' && line[n] != '{' && line[n] != ' ' && n + 1 < sizeof(tmp))
        ++n;

    memcpy(tmp, line, n);
    tmp[n] = 0;

    lc_sanitize_name_part(tmp, out, out_sz);
    if (!out[0])
        strncpy(out, "OP", out_sz - 1);
    out[out_sz - 1] = 0;
}

static bool lc_make_lrun_path(const char *line, int line_no, char *out, int out_sz)
{
    char op[16];
    char stem[32];
    int n;

    if (!out || out_sz <= 0)
        return false;

    lc_get_operation_name(line, op, sizeof(op));
    lc_get_program_stem(stem, sizeof(stem));

    n = snprintf(out, (size_t)out_sz, "%s/%s_L%d_%s.lrun", LC_FILES_DIR, op, line_no, stem);
    return n > 0 && n < out_sz;
}

static const char *lc_find_setup_in_program(const program_t *p, int before_or_at)
{
    int i;

    if (!p)
        return NULL;

    if (before_or_at >= p->count)
        before_or_at = p->count - 1;

    for (i = before_or_at; i >= 0; --i)
        if (strncmp(p->lines[i], "SETUP|", 6) == 0)
            return p->lines[i];

    return NULL;
}

static const char *lc_find_prefix_in_program(const program_t *p, int before_or_at, const char *prefix)
{
    int i;
    size_t n;

    if (!p || !prefix)
        return NULL;

    n = strlen(prefix);
    if (before_or_at >= p->count)
        before_or_at = p->count - 1;

    for (i = before_or_at; i >= 0; --i)
        if (strncmp(p->lines[i], prefix, n) == 0)
            return p->lines[i];

    return NULL;
}

static bool lc_line_get_float_key(const char *line, const char *key, float *out)
{
    char pat[24];
    const char *p;
    const char *b;
    const char *e;
    char val[32];
    size_t len;

    if (!line || !key || !out)
        return false;

    snprintf(pat, sizeof(pat), "|%s{", key);
    p = strstr(line, pat);
    if (!p && strncmp(line, key, strlen(key)) == 0 && line[strlen(key)] == '{')
        p = line - 1;
    if (!p)
        return false;

    b = strchr(p + 1, '{');
    e = b ? strchr(b + 1, '}') : NULL;
    if (!b || !e || e <= b + 1 || b[1] == '(')
        return false;

    len = (size_t)(e - b - 1);
    if (len >= sizeof(val))
        len = sizeof(val) - 1;
    memcpy(val, b + 1, len);
    val[len] = '\0';

    *out = strtof(val, NULL);
    return true;
}

static bool lc_line_get_float3_key(const char *line, const char *a, const char *b, const char *c, float *out)
{
    return lc_line_get_float_key(line, a, out) || lc_line_get_float_key(line, b, out) || lc_line_get_float_key(line, c, out);
}

static bool lc_line_is_thread(const char *line)
{
    return line && (strncmp(line, "THR_OD|", 7) == 0 || strncmp(line, "THR_ID|", 7) == 0);
}

static void lc_get_field_name_by_index(const char *line, uint8_t wanted, char *out, size_t out_sz)
{
    const char *p;
    uint8_t idx = 0;

    if (!out || out_sz == 0)
        return;
    out[0] = '\0';
    if (!line)
        return;

    p = line;
    while (*p)
    {
        const char *open = strchr(p, '{');
        const char *name_start;
        size_t len;

        if (!open)
            return;

        if (idx == wanted)
        {
            name_start = open;
            while (name_start > line && *(name_start - 1) != '|')
                name_start--;
            len = (size_t)(open - name_start);
            if (len >= out_sz)
                len = out_sz - 1;
            memcpy(out, name_start, len);
            out[len] = '\0';
            return;
        }

        idx++;
        p = open + 1;
    }
}

static bool lc_calc_thread_lanes(const char *thread_line,
                                 const char *tool_line,
                                 float *start_lane,
                                 float *stop_lane,
                                 float *ramp_lane,
                                 float *lock_lane,
                                 float *z_speed)
{
    float pitch = 0.0f;
    float rpm = 0.0f;
    float accel = 0.0f;
    float max_z_speed = 0.0f;
    float speed;
    float ramp;
    float lock;

    if (!lc_line_is_thread(thread_line))
        return false;

    if (!lc_line_get_float3_key(thread_line, "P", "PITCH", "K", &pitch) || pitch <= 0.0f)
        return false;

    (void)lc_line_get_float3_key(thread_line, "S", "RPM", "SPINDLE", &rpm);
    if (rpm <= 0.0f && tool_line)
        (void)lc_line_get_float3_key(tool_line, "S", "RPM", "SPINDLE", &rpm);

    accel = g_settings.acceleration[AXIS_Z];
    max_z_speed = g_settings.max_feed_rate[AXIS_Z] * MIN_SEC_MULT;
    if (rpm > 0.0f)
    {
        speed = pitch * rpm * MIN_SEC_MULT;
        if (max_z_speed > 0.0f)
            speed = MIN(speed, max_z_speed);
    }
    else
    {
        speed = max_z_speed;
    }

    if (accel <= 0.0f || speed <= 0.0f)
        return false;

    ramp = (speed * speed) / (2.0f * accel);
    lock = pitch * (float)G33_ELS_LOCK_REV_MIN;

    if (ramp_lane) *ramp_lane = ramp;
    if (lock_lane) *lock_lane = lock;
    if (start_lane) *start_lane = MAX(ramp, lock);
    if (stop_lane) *stop_lane = ramp;
    if (z_speed) *z_speed = speed;
    return true;
}

static int lc_gcode_discard_line(const char *line, void *user)
{
    (void)line;
    (void)user;
    return 1;
}

static bool lc_is_context_line(const char *line)
{
    return line &&
           (strncmp(line, "SETUP|", 6) == 0 ||
            strncmp(line, "TOOL|", 5) == 0);
}

static lc_gcode_result_t lc_preflight_program_gcode(const program_t *prog,
                                                    int *made_out,
                                                    int *line_out,
                                                    char *err,
                                                    unsigned err_len)
{
    int i;
    int made = 0;
    lc_gcode_result_t r = LC_GCODE_OK;

    if (made_out) *made_out = 0;
    if (line_out) *line_out = 0;
    if (err && err_len > 0) err[0] = 0;

    if (!prog)
        return LC_GCODE_BAD_FIELD;

    for (i = 0; i < prog->count; ++i)
    {
        const char *line = prog->lines[i];
        const char *setup;
        const char *tool;

        if (!line || !line[0] || lc_is_context_line(line))
            continue;

        setup = lc_find_setup_in_program(prog, i);
        tool = lc_find_prefix_in_program(prog, i, "TOOL|");
        r = leancam_gcode_run_program_line_ex(line, setup, tool, lc_gcode_discard_line, NULL, err, err_len);
        if (r != LC_GCODE_OK)
        {
            if (line_out) *line_out = i + 1;
            if (made_out) *made_out = made;
            return r;
        }

        made++;
    }

    if (made_out) *made_out = made;
    return LC_GCODE_OK;
}

static int lc_emit_program_cycle_banner(lc_gcode_file_emit_t *emit_ctx, int line_no, const char *tool)
{
    char buf[96];

    snprintf(buf, sizeof(buf), "(--- LeanCam L%d ---)", line_no);
    if (!lc_write_line_to_file(buf, emit_ctx))
        return 0;

    if (tool && tool[0])
    {
        snprintf(buf, sizeof(buf), "(--- %.84s ---)", tool);
        if (!lc_write_line_to_file(buf, emit_ctx))
            return 0;
    }

    return 1;
}

static int lc_emit_setup_comment(lc_gcode_file_emit_t *emit_ctx, const program_t *prog)
{
    const char *setup;
    char buf[UI_LC_LINE_LEN + 12];

    setup = lc_find_setup_in_program(prog, prog ? prog->count - 1 : 0);
    if (!setup || !setup[0])
        return 1;

    snprintf(buf, sizeof(buf), "(LC %s)", setup);
    return lc_write_line_to_file(buf, emit_ctx);
}

static void lc_generate_selected_file_gcode(void)
{
    char in_path[LC_FILE_PATH_MAX];
    char out_path[LC_FILE_PATH_MAX];
    program_t prog;
    fs_file_t *fp;
    lc_gcode_file_emit_t emit_ctx;
    int cnt;
    int i;
    int made = 0;
    int fail_line = 0;
    lc_gcode_result_t r = LC_GCODE_OK;
    char err[64];

    if (leancam_files_busy())
    {
        lc_set_msg("LC: file IO busy");
        return;
    }

    cnt = leancam_files_count();
    if (g_file_sel < 0 || g_file_sel >= cnt)
    {
        lc_set_msg("LC: no file selected");
        return;
    }

    if (!lc_has_suffix_ci_local(leancam_files_name(g_file_sel), ".lcam"))
    {
        lc_set_msg("LC: select .lcam");
        return;
    }

    lc_publish_msg("LC: preparing %s", leancam_files_name(g_file_sel));

    if (!leancam_files_build_path(LC_FILES_DIR, g_file_sel, in_path, sizeof(in_path)) ||
        !lc_make_gcode_path(in_path, out_path, sizeof(out_path)))
    {
        lc_set_msg("LC: path failed");
        return;
    }

    lc_publish_msg("LC: loading %s", lc_basename(in_path));

    if (!leancam_files_load(in_path, &prog))
    {
        lc_set_msg("LC: load failed");
        leancam_bridge_request_render();
        return;
    }

    lc_publish_msg("LC: checking cycles");
    r = lc_preflight_program_gcode(&prog, &made, &fail_line, err, sizeof(err));
    if (r != LC_GCODE_OK)
    {
        lc_set_msgf("LC: L%d %.36s", fail_line, err[0] ? err : lc_gcode_result_name(r));
        leancam_bridge_request_render();
        return;
    }

    if (made <= 0)
    {
        lc_set_msg("LC: no cycles");
        leancam_bridge_request_render();
        return;
    }

    lc_publish_msg("LC: opening %s", lc_basename(out_path));

    cnc_set_file_io_critical(true);
    fp = fs_open(out_path, "w");
    if (!fp)
    {
        cnc_set_file_io_critical(false);
        lc_set_msg("LC: gcode open failed");
        leancam_bridge_request_render();
        return;
    }

    emit_ctx.fp = fp;
    made = 0;
    lc_publish_msg("LC: writing header");
    if (!leancam_gcode_emit_program_header(lc_write_line_to_file, &emit_ctx))
        r = LC_GCODE_STREAM_REJECT;
    if (r == LC_GCODE_OK && !lc_emit_setup_comment(&emit_ctx, &prog))
        r = LC_GCODE_STREAM_REJECT;

    for (i = 0; r == LC_GCODE_OK && i < prog.count; ++i)
    {
        const char *line = prog.lines[i];
        const char *setup;
        const char *tool;

        if (!line || !line[0] || lc_is_context_line(line))
            continue;

        lc_publish_msg("LC: gen L%d %.32s", i + 1, line);

        setup = lc_find_setup_in_program(&prog, i);
        tool = lc_find_prefix_in_program(&prog, i, "TOOL|");

        if (!lc_emit_program_cycle_banner(&emit_ctx, i + 1, tool))
        {
            r = LC_GCODE_STREAM_REJECT;
            break;
        }

        r = leancam_gcode_run_program_line_ex(line, setup, tool, lc_write_line_to_file, &emit_ctx, err, sizeof(err));
        if (r != LC_GCODE_OK)
            break;

        made++;
    }

    if (r == LC_GCODE_OK && !leancam_gcode_emit_program_footer(lc_write_line_to_file, &emit_ctx))
        r = LC_GCODE_STREAM_REJECT;

    lc_publish_msg("LC: closing %s", lc_basename(out_path));
    fs_close(fp);
    cnc_set_file_io_critical(false);

    if (r != LC_GCODE_OK)
    {
        (void)fs_remove(out_path);
        lc_set_msgf("LC: L%d %.36s", i + 1, err[0] ? err : lc_gcode_result_name(r));
        leancam_bridge_request_render();
        return;
    }

    lc_publish_msg("LC: refreshing files");
    lc_refresh_files();
    lc_set_msgf("LC: saved %s", lc_basename(out_path));
    lc_open_nc_viewer(out_path);
    leancam_bridge_request_render();
}

void __attribute__((weak)) leancam_bridge_request_render(void)
{
}

static void lc_autosave(void)
{
    if (!g_leancam_ui.current_path[0])
        return;

    if (leancam_ui_save(&g_leancam_ui, g_leancam_ui.current_path))
    {
        //cnc_clear_exec_state(0x80);
        lc_set_msg("LC: saved");
    }
    else
        lc_set_msg("LC: save failed");
}

static int lc_has_setup(void)
{
    int i;

    for (i = 0; i < g_leancam_ui.prog.count; ++i)
    {
        if (strncmp(g_leancam_ui.prog.lines[i], "SETUP|", 6) == 0)
            return 1;
    }

    return 0;
}

static int lc_has_tool(void)
{
    int i;

    for (i = 0; i < g_leancam_ui.prog.count; ++i)
    {
        if (strncmp(g_leancam_ui.prog.lines[i], "TOOL|", 5) == 0)
            return 1;
    }

    return 0;
}

static void lc_begin_setup_if_missing_then(const char *tmpl)
{
    if (leancam_files_busy())
    {
        lc_set_msg("LC: file IO busy");
        return;
    }

    if (!lc_has_setup())
    {
        if (leancam_ui_begin_template(&g_leancam_ui, g_leancam_setup_template))
        {
            g_draft_field_index = 0;
            g_lc_mode = LC_MODE_DRAFT;
            lc_set_msg("LC: setup first");
        }
        return;
    }

    if (leancam_ui_begin_template(&g_leancam_ui, tmpl))
    {
        g_draft_field_index = 0;
        g_lc_mode = LC_MODE_DRAFT;
        lc_set_msg("LC: edit draft");
    }
}

static void lc_begin_tool_template(void)
{
    if (leancam_files_busy())
    {
        lc_set_msg("LC: file IO busy");
        return;
    }

    if (!lc_has_setup())
    {
        if (leancam_ui_begin_template(&g_leancam_ui, g_leancam_setup_template))
        {
            g_draft_field_index = 0;
            g_lc_mode = LC_MODE_DRAFT;
            lc_set_msg("LC: setup first");
        }
        return;
    }

    if (leancam_ui_begin_template(&g_leancam_ui, g_leancam_tool_template))
    {
        g_draft_field_index = 0;
        g_lc_mode = LC_MODE_DRAFT;
        lc_set_msg("LC: tool draft");
    }
    else
        lc_set_msg("LC: tool draft failed");
}

static void lc_delete_current_line(void)
{
    if (leancam_files_busy())
    {
        lc_set_msg("LC: file IO busy");
        return;
    }

    if (g_leancam_ui.cur_line == 0 &&
        g_leancam_ui.prog.count > 0 &&
        strncmp(g_leancam_ui.prog.lines[0], "SETUP|", 6) == 0)
    {
        lc_set_msg("LC: setup locked");
        return;
    }

    leancam_ui_delete_line(&g_leancam_ui);
    lc_autosave();
}

static void lc_new_file_finish(void)
{
    char path[LC_FILE_PATH_MAX];

    if (leancam_files_busy())
    {
        lc_set_msg("LC: file IO busy");
        return;
    }

    if (!g_new_file_name[0])
    {
        lc_set_msg("LC: empty name");
        return;
    }

    if (!leancam_files_make_new_path(LC_FILES_DIR, g_new_file_name, path, sizeof(path)))
    {
        lc_set_msg("LC: path failed");
        return;
    }

    leancam_ui_new(&g_leancam_ui);
    strncpy(g_leancam_ui.current_path, path, sizeof(g_leancam_ui.current_path) - 1);
    g_leancam_ui.current_path[sizeof(g_leancam_ui.current_path) - 1] = 0;

    g_prog_scroll = 0;

    if (leancam_ui_begin_template(&g_leancam_ui, g_leancam_setup_template))
    {
        g_draft_field_index = 0;
        g_lc_mode = LC_MODE_DRAFT;
        lc_set_msg("LC: new file, setup first");
    }
    else
    {
        g_lc_mode = LC_MODE_PROGRAM;
        lc_set_msg("LC: new file");
    }
}

static void lc_nc_clear_window(void)
{
    int i;

    for (i = 0; i < UI_LC_MAX_LINES; ++i)
        g_nc_lines[i][0] = 0;

    g_nc_line_count = 0;
    g_nc_eof = false;
}

static bool lc_nc_read_line(fs_file_t *fp, char *out, int out_sz)
{
    int pos = 0;
    bool got = false;

    if (!fp || !out || out_sz <= 0)
        return false;

    while (fs_available(fp))
    {
        char c = 0;

        if (!fs_read(fp, (uint8_t *)&c, 1))
            break;

        got = true;
        if (c == '\r')
            continue;

        if (c == '\n')
            break;

        if (pos < out_sz - 1)
            out[pos++] = c;
    }

    out[pos] = 0;
    return got;
}

static void lc_nc_capture_setup_comment(const char *line)
{
    const char *p;
    const char *end;
    size_t n;

    if (!line || g_nc_setup_line[0])
        return;

    p = strstr(line, "SETUP|");
    if (!p)
        return;

    end = strchr(p, ')');
    if (!end)
        end = p + strlen(p);

    n = (size_t)(end - p);
    if (n >= sizeof(g_nc_setup_line))
        n = sizeof(g_nc_setup_line) - 1u;
    memcpy(g_nc_setup_line, p, n);
    g_nc_setup_line[n] = 0;
}

static bool lc_nc_load_window(void)
{
    fs_file_t *fp;
    char throwaway[UI_LC_LINE_LEN];
    int skipped = 0;
    int row = 0;

    lc_nc_clear_window();
    g_nc_setup_line[0] = 0;

    if (!g_nc_path[0])
        return false;

    cnc_set_file_io_critical(true);
    fp = fs_open(g_nc_path, "r");
    if (!fp)
    {
        cnc_set_file_io_critical(false);
        return false;
    }

    while (skipped < g_nc_top_line && lc_nc_read_line(fp, throwaway, sizeof(throwaway)))
    {
        lc_nc_capture_setup_comment(throwaway);
        skipped++;
    }

    while (row < UI_LC_MAX_LINES && lc_nc_read_line(fp, g_nc_lines[row], UI_LC_LINE_LEN))
    {
        lc_nc_capture_setup_comment(g_nc_lines[row]);
        row++;
    }

    g_nc_line_count = (uint8_t)row;
    g_nc_eof = !fs_available(fp);
    if (g_nc_line_count == 0)
        g_nc_selected_row = 0;
    else if (g_nc_selected_row >= g_nc_line_count)
        g_nc_selected_row = g_nc_line_count - 1;

    fs_close(fp);
    cnc_set_file_io_critical(false);

    return true;
}

static void lc_open_nc_viewer(const char *path)
{
    if (!path || !path[0])
    {
        lc_set_msg("LC: no nc path");
        return;
    }

    strncpy(g_nc_path, path, sizeof(g_nc_path) - 1);
    g_nc_path[sizeof(g_nc_path) - 1] = 0;
    g_nc_top_line = 0;
    g_nc_selected_row = 0;

    if (!lc_nc_load_window())
    {
        g_nc_path[0] = 0;
        lc_set_msg("LC: nc open failed");
        return;
    }

    g_lc_mode = LC_MODE_NC_VIEW;
    lc_set_msg("LC: nc viewer");
}

static void lc_open_selected_file(void)
{
    char path[LC_FILE_PATH_MAX];
    const char *name;

    if (leancam_files_busy())
    {
        lc_set_msg("LC: file IO busy");
        return;
    }

    if (!leancam_files_build_path(LC_FILES_DIR, g_file_sel, path, sizeof(path)))
    {
        lc_set_msg("LC: build path failed");
        return;
    }

    name = leancam_files_name(g_file_sel);
    if (lc_has_suffix_ci_local(name, ".nc"))
    {
        lc_open_nc_viewer(path);
        return;
    }

    if (!lc_has_suffix_ci_local(name, ".lcam"))
    {
        lc_set_msg("LC: unsupported file");
        return;
    }

    if (!leancam_ui_load(&g_leancam_ui, path))
    {
        lc_set_msg("LC: load failed");
        return;
    }

    g_prog_scroll = 0;
    g_lc_mode = LC_MODE_PROGRAM;
    lc_set_msg("");
}

static void lc_ensure_program_visible(void)
{
    if (g_leancam_ui.cur_line < 0)
    {
        g_prog_scroll = 0;
        return;
    }

    if (g_prog_scroll < 0)
        g_prog_scroll = 0;

    if (g_leancam_ui.cur_line < g_prog_scroll)
        g_prog_scroll = g_leancam_ui.cur_line;

    if (g_leancam_ui.cur_line >= g_prog_scroll + LC_VISIBLE_PROGRAM_LINES)
        g_prog_scroll = g_leancam_ui.cur_line - LC_VISIBLE_PROGRAM_LINES + 1;

    if (g_prog_scroll < 0)
        g_prog_scroll = 0;
}

void leancam_bridge_init(void)
{
    leancam_files_init();
    leancam_ui_init(&g_leancam_ui);

    g_new_file_name[0] = 0;
    g_nc_path[0] = 0;
    g_prog_scroll = 0;
    g_nc_top_line = 0;
    g_nc_selected_row = 0;
    g_file_sel = 0;
    g_files_ready = false;
    g_next_file_refresh_ms = 0;
    lc_set_msg("LC: init");

#ifdef LEANCAM_RP2350_STANDALONE_DEMO
#ifdef ENABLE_SD_CARD_V2
    g_lc_mode = LC_MODE_FILES;
    g_next_file_refresh_ms = mcu_millis() + 250u;
    lc_set_msg("LC: waiting for SD");
#else
    prog_add(&g_leancam_ui.prog,
             "SETUP|L{75}|OD{50}|ID{0}|CLAMP{8}|EXTRA{3}|CLR{1}|MAT{ST45}|WOFF{G54}");
    prog_add(&g_leancam_ui.prog,
             "TOOL|T{1}|D{6}|S{800}|R_FEED{120}|FIN_FEED{60}|R_DOC{2.0}|FIN_DOC{0.5}");
    prog_add(&g_leancam_ui.prog,
             "OD|D1{50}|Z1{0}|Z2{-35}|D2{32}|RND{0}|CHMF{1}|DT{32}|CLR{1}");
    g_leancam_ui.cur_line = 0;
    g_lc_mode = LC_MODE_PROGRAM;
    lc_set_msg("LC: RP2350 demo, no SD");
#endif
#else
    g_lc_mode = LC_MODE_FILES;
    lc_refresh_files();
#endif
}


void leancam_bridge_tick(void)
{
    uint32_t now;

    if (g_lc_mode != LC_MODE_FILES || g_files_ready || leancam_files_busy())
        return;

    now = mcu_millis();
    if (g_next_file_refresh_ms != 0 &&
        (int32_t)(now - g_next_file_refresh_ms) < 0)
    {
        return;
    }

    lc_refresh_files();
}


int leancam_bridge_wants_process_menu(void)
{
    return (g_lc_mode == LC_MODE_PROGRAM || g_lc_mode == LC_MODE_DRAFT || g_lc_mode == LC_MODE_NC_VIEW);
}

#if LC_SELECTED_RUN_USE_STREAM
static bool lc_cam_emit_line(const char *line, void *user)
{
    uint32_t *line_no = (uint32_t *)user;
    uint32_t n;

    if (!line_no)
        return false;

    n = *line_no;
    *line_no += 10u;
    return cam_stream_send_line(line, n);
}

static int lc_cam_emit_gcode_line(const char *line, void *user)
{
    return lc_cam_emit_line(line, user) ? 1 : 0;
}

static void lc_run_selected_line_stream(const char *line, const char *setup, const char *tool, int run_line_no)
{
    uint32_t line_no = 10u;
    lc_gcode_result_t r = LC_GCODE_OK;
    char err[64];

    err[0] = 0;

    if (!cam_stream_begin())
    {
        lc_set_msg("LC: stream begin fail");
        return;
    }

    if (!leancam_gcode_emit_program_header(lc_cam_emit_gcode_line, &line_no))
        r = LC_GCODE_STREAM_REJECT;

    if (r == LC_GCODE_OK)
    {
        char buf[96];

        snprintf(buf, sizeof(buf), "(--- LeanCam L%d ---)", run_line_no);
        if (!lc_cam_emit_line(buf, &line_no))
            r = LC_GCODE_STREAM_REJECT;

        if (r == LC_GCODE_OK && tool && tool[0])
        {
            snprintf(buf, sizeof(buf), "(--- %.84s ---)", tool);
            if (!lc_cam_emit_line(buf, &line_no))
                r = LC_GCODE_STREAM_REJECT;
        }
    }

    if (r == LC_GCODE_OK)
        r = leancam_gcode_run_program_line_ex(line, setup, tool, lc_cam_emit_gcode_line, &line_no, err, sizeof(err));

    if (r == LC_GCODE_OK && !leancam_gcode_emit_program_footer(lc_cam_emit_gcode_line, &line_no))
        r = LC_GCODE_STREAM_REJECT;

    cam_stream_finish();

    if (r == LC_GCODE_OK)
        lc_set_msg("LC: stream run sent");
    else
        lc_set_msgf("LC: L%d %.32s", run_line_no, err[0] ? err : lc_gcode_result_name(r));
}
#endif

#ifdef LEANCAM_RP2350_DIRECT_STREAM
static char g_lc_direct_stream_buf[LEANCAM_DIRECT_STREAM_BUFFER];
static uint32_t g_lc_direct_stream_len = 0;
static uint32_t g_lc_direct_stream_pos = 0;
static bool g_lc_direct_stream_active = false;

typedef struct
{
    uint32_t lines;
    bool overflow;
} lc_direct_stream_emit_t;

static void lc_direct_stream_reset(void)
{
    g_lc_direct_stream_len = 0;
    g_lc_direct_stream_pos = 0;
    g_lc_direct_stream_active = false;
}

static uint8_t lc_direct_stream_available(void)
{
    if (g_lc_direct_stream_pos < g_lc_direct_stream_len)
        return 1;

    if (g_lc_direct_stream_active)
    {
        lc_direct_stream_reset();
        grbl_stream_change(NULL);
    }

    return 0;
}

static uint8_t lc_direct_stream_getc(void)
{
    if (g_lc_direct_stream_pos < g_lc_direct_stream_len)
        return (uint8_t)g_lc_direct_stream_buf[g_lc_direct_stream_pos++];

    return 0;
}

static void lc_direct_stream_clear(void)
{
    lc_direct_stream_reset();
    grbl_stream_change(NULL);
}

static int lc_direct_stream_append(const char *line, void *user)
{
    lc_direct_stream_emit_t *ctx = (lc_direct_stream_emit_t *)user;
    size_t len;

    if (!line)
        return 0;

    len = strlen(line);
    if (g_lc_direct_stream_len + len + 1u >= (uint32_t)sizeof(g_lc_direct_stream_buf))
    {
        if (ctx)
            ctx->overflow = true;
        return 0;
    }

    if (len > 0)
    {
        memcpy(&g_lc_direct_stream_buf[g_lc_direct_stream_len], line, len);
        g_lc_direct_stream_len += (uint32_t)len;
    }
    g_lc_direct_stream_buf[g_lc_direct_stream_len++] = '\n';

    if (ctx)
        ctx->lines++;

    return 1;
}

static bool lc_direct_stream_banner(int run_line_no, const char *tool, lc_direct_stream_emit_t *ctx)
{
    char buf[96];

    snprintf(buf, sizeof(buf), "(--- LeanCam L%d ---)", run_line_no);
    if (!lc_direct_stream_append(buf, ctx))
        return false;

    if (tool && tool[0])
    {
        snprintf(buf, sizeof(buf), "(--- %.84s ---)", tool);
        if (!lc_direct_stream_append(buf, ctx))
            return false;
    }

    return true;
}

static void lc_run_selected_line_direct_stream(const char *line, const char *setup, const char *tool, int run_line_no)
{
    lc_direct_stream_emit_t emit_ctx;
    lc_gcode_result_t r = LC_GCODE_OK;
    char err[64];

    err[0] = 0;
    emit_ctx.lines = 0;
    emit_ctx.overflow = false;

    if (g_lc_direct_stream_active)
    {
        lc_set_msg("LC: stream busy");
        return;
    }

    if (cnc_has_alarm() || cnc_get_exec_state(EXEC_GCODE_LOCKED))
    {
        lc_set_msg("LC: machine locked");
        return;
    }

    lc_direct_stream_reset();

    if (!leancam_gcode_emit_program_header(lc_direct_stream_append, &emit_ctx))
        r = LC_GCODE_STREAM_REJECT;

    if (r == LC_GCODE_OK && !lc_direct_stream_banner(run_line_no, tool, &emit_ctx))
        r = LC_GCODE_STREAM_REJECT;

    if (r == LC_GCODE_OK)
        r = leancam_gcode_run_program_line_ex(line, setup, tool, lc_direct_stream_append, &emit_ctx, err, sizeof(err));

    if (r == LC_GCODE_OK && !leancam_gcode_emit_program_footer(lc_direct_stream_append, &emit_ctx))
        r = LC_GCODE_STREAM_REJECT;

    if (r != LC_GCODE_OK)
    {
        lc_direct_stream_reset();
        lc_set_msgf("LC: L%d %.32s", run_line_no,
                    emit_ctx.overflow ? "stream overflow" : (err[0] ? err : lc_gcode_result_name(r)));
        return;
    }

    if (g_lc_direct_stream_len == 0)
    {
        lc_set_msg("LC: no gcode");
        return;
    }

    g_lc_direct_stream_pos = 0;
    g_lc_direct_stream_active = true;
    grbl_stream_readonly(lc_direct_stream_getc, lc_direct_stream_available, lc_direct_stream_clear);
    lc_set_msgf("LC: queued L%d %lu lines", run_line_no, (unsigned long)emit_ctx.lines);
}
#endif

static void lc_run_selected_line(void)
{
    char out_path[LC_FILE_PATH_MAX];
    const char *line;
    const char *setup;
    const char *tool;
    fs_file_t *fp;
    lc_gcode_file_emit_t emit_ctx;
    int run_line_no;
    lc_gcode_result_t r;
    char err[64];

    if (g_leancam_ui.cur_line < 0 || g_leancam_ui.cur_line >= g_leancam_ui.prog.count)
    {
        lc_set_msg("LC: no line");
        return;
    }

    line = g_leancam_ui.prog.lines[g_leancam_ui.cur_line];
    run_line_no = g_leancam_ui.cur_line + 1;
    setup = lc_find_setup_in_program(&g_leancam_ui.prog, g_leancam_ui.cur_line);
    tool = lc_find_prefix_in_program(&g_leancam_ui.prog, g_leancam_ui.cur_line, "TOOL|");

    if (!setup)
    {
        lc_set_msg("LC: no setup");
        return;
    }

#if LC_SELECTED_RUN_USE_STREAM
    lc_run_selected_line_stream(line, setup, tool, run_line_no);
    return;
#endif

#ifdef LEANCAM_RP2350_DIRECT_STREAM
    lc_run_selected_line_direct_stream(line, setup, tool, run_line_no);
    return;
#endif

    if (!lc_make_lrun_path(line, run_line_no, out_path, sizeof(out_path)))
    {
        lc_set_msg("LC: lrun path failed");
        return;
    }

    lc_publish_msg("LC: writing %s", lc_basename(out_path));

    cnc_set_file_io_critical(true);
    fp = fs_open(out_path, "w");
    if (!fp)
    {
        cnc_set_file_io_critical(false);
        lc_set_msg("LC: lrun open failed");
        leancam_bridge_request_render();
        return;
    }

    emit_ctx.fp = fp;
    err[0] = 0;
    r = LC_GCODE_OK;

    if (!leancam_gcode_emit_program_header(lc_write_line_to_file, &emit_ctx))
        r = LC_GCODE_STREAM_REJECT;

    if (r == LC_GCODE_OK && !lc_emit_program_cycle_banner(&emit_ctx, run_line_no, tool))
        r = LC_GCODE_STREAM_REJECT;

    if (r == LC_GCODE_OK)
        r = leancam_gcode_run_program_line_ex(line, setup, tool, lc_write_line_to_file, &emit_ctx, err, sizeof(err));

    if (r == LC_GCODE_OK && !leancam_gcode_emit_program_footer(lc_write_line_to_file, &emit_ctx))
        r = LC_GCODE_STREAM_REJECT;

    fs_close(fp);
    cnc_set_file_io_critical(false);

    if (r == LC_GCODE_OK)
    {
        lc_publish_msg("LC: running %s", lc_basename(out_path));
        fs_file_run(out_path);
    }
    else
    {
        lc_set_msgf("LC: L%d %.32s", run_line_no, err[0] ? err : lc_gcode_result_name(r));
    }
}


static uint8_t lc_count_brace_fields(const char *s)
{
    uint8_t n = 0;
    const char *p = s;
    const char *q;

    while (p && *p)
    {
        p = strchr(p, '{');
        if (!p)
            break;

        q = strchr(p + 1, '}');
        if (!q)
            break;

        if (n < 250u)
            n++;

        p = q + 1;
    }

    return n;
}

static void lc_clear_input_buf(void)
{
    g_leancam_ui.input_buf[0] = 0;
}

static void lc_input_toggle_sign(void)
{
    char *buf = g_leancam_ui.input_buf;
    size_t len;

    if (buf[0] == '-')
    {
        memmove(buf, buf + 1, strlen(buf));
        return;
    }

    len = strlen(buf);
    if (len + 1u < sizeof(g_leancam_ui.input_buf))
    {
        memmove(buf + 1, buf, len + 1u);
        buf[0] = '-';
    }
}

static void lc_input_add_dot(void)
{
    char *buf = g_leancam_ui.input_buf;
    size_t len;

    if (strchr(buf, '.'))
        return;

    len = strlen(buf);
    if (len + 1u >= sizeof(g_leancam_ui.input_buf))
        return;

    if (len == 0u)
    {
        if (sizeof(g_leancam_ui.input_buf) > 2u)
        {
            buf[0] = '0';
            buf[1] = '.';
            buf[2] = 0;
        }
        return;
    }

    buf[len] = '.';
    buf[len + 1u] = 0;
}

static int lc_find_brace_field(const char *s, uint8_t field_index, const char **open_out, const char **close_out);

static bool lc_active_field_is_negative(void)
{
    const char *open;
    const char *close;
    const char *p;

    if (!g_leancam_ui.draft_active)
        return false;

    if (!lc_find_brace_field(g_leancam_ui.draft_line, g_draft_field_index, &open, &close))
        return false;

    p = open + 1;
    while (p < close && *p == ' ')
        p++;
    if (p < close && *p == '(')
    {
        p++;
        while (p < close && *p == ' ')
            p++;
    }

    return p < close && *p == '-';
}

static void lc_input_digit(char digit)
{
    if (g_leancam_ui.input_buf[0] == 0 && lc_active_field_is_negative())
        lc_input_toggle_sign();

    leancam_ui_input_char(&g_leancam_ui, digit);
}


static int lc_find_brace_field(const char *s, uint8_t field_index, const char **open_out, const char **close_out)
{
    const char *scan;
    uint8_t idx = 0;
    if (open_out) *open_out = NULL;
    if (close_out) *close_out = NULL;
    if (!s) return 0;
    scan = s;
    while (*scan)
    {
        const char *o = strchr(scan, '{');
        const char *c = o ? strchr(o + 1, '}') : NULL;
        if (!o || !c) return 0;
        if (idx == field_index)
        {
            if (open_out) *open_out = o;
            if (close_out) *close_out = c;
            return 1;
        }
        idx++;
        scan = c + 1;
    }
    return 0;
}

static int lc_replace_span(char *line, uint32_t line_len, const char *span_start, const char *span_end_exclusive, const char *replacement)
{
    /* This edits g_leancam_ui.draft_line, not the visible screen row.
     * Use MAX_LEN here; UI_LC_LINE_LEN may be only a display/snapshot limit.
     */
    char tmp[MAX_LEN];
    uint32_t prefix_len;
    if (!line || line_len == 0 || !span_start || !span_end_exclusive || span_end_exclusive < span_start) return 0;
    if (!replacement) replacement = "";
    prefix_len = (uint32_t)(span_start - line);
    if (prefix_len >= sizeof(tmp)) prefix_len = sizeof(tmp) - 1u;
    if (prefix_len) memcpy(tmp, line, prefix_len);
    tmp[prefix_len] = 0;
    strncat(tmp, replacement, sizeof(tmp) - strlen(tmp) - 1u);
    strncat(tmp, span_end_exclusive, sizeof(tmp) - strlen(tmp) - 1u);
    strncpy(line, tmp, line_len - 1u);
    line[line_len - 1u] = 0;
    return 1;
}

static void lc_build_draft_preview_line(char *out, uint32_t out_len)
{
    const char *open;
    const char *close;

    if (!out || out_len == 0)
        return;

    out[0] = 0;
    if (!g_leancam_ui.draft_active)
        return;

    strncpy(out, g_leancam_ui.draft_line, out_len - 1u);
    out[out_len - 1u] = 0;

    if (!g_leancam_ui.input_buf[0])
        return;
    if (!lc_find_brace_field(out, g_draft_field_index, &open, &close))
        return;

    (void)lc_replace_span(out, out_len, open + 1, close, g_leancam_ui.input_buf);
}

static int lc_accept_active_field_bridge_owned(void)
{
    const char *open;
    const char *close;
    char accepted[LEANCAM_INPUT_MAX];
    uint32_t n;
    uint8_t field_count;
    bool editing_existing;
    int replace_index;

    if (!g_leancam_ui.draft_active) return 0;

    field_count = lc_count_brace_fields(g_leancam_ui.draft_line);
    if (field_count == 0 || g_draft_field_index >= field_count) return 0;

    if (!lc_find_brace_field(g_leancam_ui.draft_line, g_draft_field_index, &open, &close)) return 0;

    if (g_leancam_ui.input_buf[0])
    {
        strncpy(accepted, g_leancam_ui.input_buf, sizeof(accepted) - 1u);
        accepted[sizeof(accepted) - 1u] = 0;
    }
    else
    {
        /* Empty input means keep existing/default text inside braces. */
        n = (uint32_t)(close - open - 1);
        if (n >= sizeof(accepted)) n = sizeof(accepted) - 1u;
        memcpy(accepted, open + 1, n);
        accepted[n] = 0;
    }

    /* IMPORTANT:
     * LeanCam file syntax is NAME{value}.  Do not replace the whole
     * {value} span, otherwise NAME{25} becomes NAME25 and the next '|'
     * separator is visually/logically lost.
     * Replace only the content between braces.
     */
    if (!lc_replace_span(g_leancam_ui.draft_line,
                         (uint32_t)sizeof(g_leancam_ui.draft_line),
                         open + 1, close, accepted))
        return 0;

    lc_clear_input_buf();

    editing_existing = (g_leancam_ui.draft_replace_index >= 0);
    replace_index = g_leancam_ui.draft_replace_index;

    /* Braces intentionally remain in the saved .lcam line, so field_count
     * does not decrease. Advance the single bridge-owned active field index.
     * When it reaches field_count, no field is highlighted; # commits.
     */
    if (g_draft_field_index + 1u < field_count)
        g_draft_field_index++;
    else
        g_draft_field_index = field_count;

    if (editing_existing &&
        replace_index >= 0 &&
        replace_index < g_leancam_ui.prog.count)
    {
        strncpy(g_leancam_ui.prog.lines[replace_index], g_leancam_ui.draft_line, MAX_LEN - 1);
        g_leancam_ui.prog.lines[replace_index][MAX_LEN - 1] = 0;
        g_leancam_ui.cur_line = replace_index;
        lc_autosave();
    }

    return 1;
}

static int lc_commit_draft_bridge_owned(void)
{
    if (leancam_ui_commit_draft(&g_leancam_ui))
    {
        if (g_leancam_ui.cur_line >= 0 &&
            g_leancam_ui.cur_line < g_leancam_ui.prog.count &&
            strncmp(g_leancam_ui.prog.lines[g_leancam_ui.cur_line], "SETUP|", 6) == 0 &&
            !lc_has_tool())
        {
            if (prog_insert_after(&g_leancam_ui.prog,
                                  g_leancam_ui.cur_line,
                                  "TOOL|T{1}|D{6}|S{800}|R_FEED{120}|FIN_FEED{60}|R_DOC{2.0}|FIN_DOC{0.5}"))
            {
                g_leancam_ui.cur_line++;
            }
        }
        g_draft_field_index = 0;
        lc_autosave();
        g_lc_mode = LC_MODE_PROGRAM;
        lc_ensure_program_visible();
        return 1;
    }

    lc_set_msg("LC: unresolved field");
    return 0;
}

void leancam_bridge_handle_key(ui_key_t key)
{
    size_t len;

    if (key == UI_KEY_NONE)
        return;

    switch (g_lc_mode)
    {
        case LC_MODE_FILES:
        {
            int cnt = leancam_files_count();

            switch (key)
            {
                case UI_KEY_CANCEL:
                    lc_refresh_files();
                    return;

                case UI_KEY_PREV:
                    if (g_file_sel > 0)
                        g_file_sel--;
                    return;

                case UI_KEY_NEXT:
                    if (g_file_sel < cnt)
                        g_file_sel++;
                    return;

                case UI_KEY_ACCEPT:
                    if (g_file_sel == cnt)
                    {
                        g_new_file_name[0] = 0;
                        g_lc_mode = LC_MODE_FILE_NAME;
                        lc_set_msg("LC: new name");
                    }
                    else
                    {
                        lc_open_selected_file();
                    }
                    return;

                case UI_KEY_FINISH:
                    lc_generate_selected_file_gcode();
                    return;

                case UI_KEY_BACKSPACE:
                    lc_delete_selected_file();
                    return;

                default:
                    return;
            }
        }

        case LC_MODE_FILE_NAME:
            switch (key)
            {
                case UI_KEY_CANCEL:
                    g_lc_mode = LC_MODE_FILES;
                    lc_set_msg("LC: cancel new");
                    return;

                case UI_KEY_BACKSPACE:
                    len = strlen(g_new_file_name);
                    if (len > 0)
                        g_new_file_name[len - 1] = 0;
                    return;

                case UI_KEY_ACCEPT:
                case UI_KEY_FINISH:
                    lc_new_file_finish();
                    return;

                case UI_KEY_DIGIT_0: case UI_KEY_DIGIT_1: case UI_KEY_DIGIT_2:
                case UI_KEY_DIGIT_3: case UI_KEY_DIGIT_4: case UI_KEY_DIGIT_5:
                case UI_KEY_DIGIT_6: case UI_KEY_DIGIT_7: case UI_KEY_DIGIT_8:
                case UI_KEY_DIGIT_9:
                    len = strlen(g_new_file_name);
                    if (len + 1 < sizeof(g_new_file_name))
                    {
                        g_new_file_name[len] = (char)('0' + (key - UI_KEY_DIGIT_0));
                        g_new_file_name[len + 1] = 0;
                    }
                    return;

                default:
                    return;
            }

        case LC_MODE_PROGRAM:
            switch (key)
            {
                case UI_KEY_CANCEL:
                    lc_refresh_files();
                    g_lc_mode = LC_MODE_FILES;
                    lc_set_msg("LC: files");
                    return;

                case UI_KEY_PREV:
                    leancam_ui_move_up(&g_leancam_ui);
                    lc_ensure_program_visible();
                    return;

                case UI_KEY_NEXT:
                    leancam_ui_move_down(&g_leancam_ui);
                    lc_ensure_program_visible();
                    return;

                case UI_KEY_ACCEPT:
                    if (leancam_ui_begin_edit_current(&g_leancam_ui))
                    {
                        g_draft_field_index = 0;
                        g_lc_mode = LC_MODE_DRAFT;
                        lc_set_msg("LC: edit line");
                    }
                    else
                        lc_set_msg("LC: nothing to edit");
                    return;

                case UI_KEY_FINISH:
                    lc_run_selected_line();
                    return;

                case UI_KEY_BACKSPACE:
                    lc_delete_current_line();
                    lc_ensure_program_visible();
                    return;

                case UI_KEY_DIGIT_0: lc_begin_tool_template();                                      return;
                case UI_KEY_DIGIT_1: lc_begin_setup_if_missing_then(g_leancam_templates[LC_TMPL_OD]);        return;
                case UI_KEY_DIGIT_2: lc_begin_setup_if_missing_then(g_leancam_templates[LC_TMPL_ID]);        return;
                case UI_KEY_DIGIT_3: lc_begin_setup_if_missing_then(g_leancam_templates[LC_TMPL_FACE]);      return;
                case UI_KEY_DIGIT_4: lc_begin_setup_if_missing_then(g_leancam_templates[LC_TMPL_DRILL]);     return;
                case UI_KEY_DIGIT_5: lc_begin_setup_if_missing_then(g_leancam_templates[LC_TMPL_TAP]);       return;
                case UI_KEY_DIGIT_6: lc_begin_setup_if_missing_then(g_leancam_templates[LC_TMPL_CUT]);       return;
                case UI_KEY_DIGIT_7: lc_begin_setup_if_missing_then(g_leancam_templates[LC_TMPL_CHAMFER]);   return;
                case UI_KEY_DIGIT_8: lc_begin_setup_if_missing_then(g_leancam_templates[LC_TMPL_THR_OD]); return;
                case UI_KEY_DIGIT_9: lc_begin_setup_if_missing_then(g_leancam_templates[LC_TMPL_THR_ID]); return;

                default:
                    return;
            }

        case LC_MODE_DRAFT:
            switch (key)
            {
                case UI_KEY_CANCEL:
                    leancam_ui_cancel_draft(&g_leancam_ui);
                    g_draft_field_index = 0;
                    g_lc_mode = LC_MODE_PROGRAM;
                    lc_set_msg("LC: draft cancel");
                    return;

                case UI_KEY_ACCEPT:
                    /* Single owner rule: bridge owns visible field and accepted target. */
                    if (!lc_accept_active_field_bridge_owned())
                    {
                        lc_set_msg("LC: no active field");
                        return;
                    }

                    if (g_draft_field_index >= lc_count_brace_fields(g_leancam_ui.draft_line))
                    {
                        (void)lc_commit_draft_bridge_owned();
                    }
                    else
                    {
                        lc_set_msg("LC: field accepted");
                    }
                    return;

                case UI_KEY_BACKSPACE:
                    if (g_leancam_ui.input_buf[0])
                    {
                        leancam_ui_backspace(&g_leancam_ui);
                    }
                    else
                    {
                        leancam_ui_cancel_draft(&g_leancam_ui);
                        g_draft_field_index = 0;
                        g_lc_mode = LC_MODE_PROGRAM;
                        lc_set_msg("LC: draft cancel");
                    }
                    return;

                case UI_KEY_FINISH:
                    (void)lc_commit_draft_bridge_owned();
                    return;

                case UI_KEY_PREV:     lc_input_toggle_sign(); return; /* B = +/- in draft */
                case UI_KEY_NEXT:     lc_input_add_dot();     return; /* C = decimal point in draft */

                case UI_KEY_DIGIT_0: lc_input_digit('0'); return;
                case UI_KEY_DIGIT_1: lc_input_digit('1'); return;
                case UI_KEY_DIGIT_2: lc_input_digit('2'); return;
                case UI_KEY_DIGIT_3: lc_input_digit('3'); return;
                case UI_KEY_DIGIT_4: lc_input_digit('4'); return;
                case UI_KEY_DIGIT_5: lc_input_digit('5'); return;
                case UI_KEY_DIGIT_6: lc_input_digit('6'); return;
                case UI_KEY_DIGIT_7: lc_input_digit('7'); return;
                case UI_KEY_DIGIT_8: lc_input_digit('8'); return;
                case UI_KEY_DIGIT_9: lc_input_digit('9'); return;

                default:
                    return;
            }

        case LC_MODE_NC_VIEW:
            switch (key)
            {
                case UI_KEY_CANCEL:
                    lc_refresh_files();
                    g_lc_mode = LC_MODE_FILES;
                    lc_set_msg("LC: files");
                    return;

                case UI_KEY_PREV:
                    if (g_nc_selected_row > 0)
                    {
                        g_nc_selected_row--;
                    }
                    else if (g_nc_top_line > 0)
                    {
                        g_nc_top_line--;
                        (void)lc_nc_load_window();
                    }
                    return;

                case UI_KEY_NEXT:
                    if (g_nc_selected_row + 1 < g_nc_line_count)
                    {
                        g_nc_selected_row++;
                    }
                    else if (!g_nc_eof)
                    {
                        g_nc_top_line++;
                        (void)lc_nc_load_window();
                    }
                    return;

                case UI_KEY_FINISH:
                    lc_set_msg("LC: nc view only");
                    return;

                default:
                    return;
            }

        default:
            return;
    }
}

static void lc_snapshot_line_ex(ui_snapshot_frame_t *f,
                                int row,
                                const char *s,
                                bool selected,
                                uint8_t hi_start,
                                uint8_t hi_end)
{
    uint32_t len;

    if (!f || row < 0 || row >= UI_LC_MAX_LINES)
        return;

    ui_snapshot_strcpy(f->leancam_lines[row], s ? s : "", UI_LC_LINE_LEN);
    f->leancam_line_selected[row] = selected ? 1u : 0u;

    len = (uint32_t)strlen(f->leancam_lines[row]);
    if (hi_start > len) hi_start = (uint8_t)len;
    if (hi_end   > len) hi_end   = (uint8_t)len;
    if (hi_end < hi_start) hi_end = hi_start;

    f->leancam_field_hi_start[row] = hi_start;
    f->leancam_field_hi_end[row]   = hi_end;

    if (row + 1 > f->leancam_line_count)
        f->leancam_line_count = (uint8_t)(row + 1);
}

static void lc_snapshot_line(ui_snapshot_frame_t *f, int row, const char *s, bool selected)
{
    lc_snapshot_line_ex(f, row, s, selected, 0, 0);
}

static int lc_setup_line(const char **line_out)
{
    int i;

    if (line_out) *line_out = NULL;

    for (i = 0; i < g_leancam_ui.prog.count; ++i)
    {
        if (strncmp(g_leancam_ui.prog.lines[i], "SETUP|", 6) == 0)
        {
            if (line_out) *line_out = g_leancam_ui.prog.lines[i];
            return 1;
        }
    }

    return 0;
}

static void lc_snapshot_program(ui_snapshot_frame_t *f)
{
    int row = 0;
    int v;
    int virtual_count;
    int draft_vidx = -1;
    char buf[UI_LC_LINE_LEN];

    lc_ensure_program_visible();

    snprintf(buf, sizeof(buf), "FILE: %s", lc_basename(g_leancam_ui.current_path));
    lc_snapshot_line(f, row++, buf, false);

    if (g_leancam_ui.draft_active)
    {
        if (g_leancam_ui.draft_replace_index >= 0)
            draft_vidx = g_leancam_ui.draft_replace_index;
        else
            draft_vidx = g_leancam_ui.draft_insert_after + 1;
    }

    virtual_count = g_leancam_ui.prog.count;
    if (g_leancam_ui.draft_active && g_leancam_ui.draft_replace_index < 0)
        virtual_count++;

    for (v = g_prog_scroll; v < virtual_count && row < UI_LC_MAX_LINES; ++v)
    {
        if (g_leancam_ui.draft_active && v == draft_vidx)
        {
            const char *setup = NULL;
            const char *tool = NULL;
            uint8_t hi_start = 0;
            uint8_t hi_end = 0;

            (void)lc_setup_line(&setup);
            tool = lc_find_prefix_in_program(&g_leancam_ui.prog, draft_vidx, "TOOL|");
            leancam_expr_build_draft_display(buf,
                                             sizeof(buf),
                                             g_leancam_ui.draft_line,
                                             g_leancam_ui.input_buf,
                                             g_draft_field_index,
                                             setup,
                                             g_leancam_ui.draft_line,
                                             &hi_start,
                                             &hi_end);

            if (lc_line_is_thread(g_leancam_ui.draft_line))
            {
                float start_lane = 0.0f;
                float stop_lane = 0.0f;

                if (lc_calc_thread_lanes(g_leancam_ui.draft_line, tool,
                                         &start_lane, &stop_lane,
                                         &f->leancam_thread_ramp_lane,
                                         &f->leancam_thread_lock_lane,
                                         &f->leancam_thread_z_speed))
                {
                    size_t used = strlen(buf);
                    snprintf(buf + used, sizeof(buf) - used, " | ELS %.2f/%.2f",
                             (double)start_lane, (double)stop_lane);
                }
            }

            lc_snapshot_line_ex(f, row++, buf, false, hi_start, hi_end);
        }
        else
        {
            int pi = v;

            if (g_leancam_ui.draft_active &&
                g_leancam_ui.draft_replace_index < 0 &&
                draft_vidx >= 0 &&
                v > draft_vidx)
            {
                pi = v - 1;
            }

            if (pi >= 0 && pi < g_leancam_ui.prog.count)
            {
                if (lc_line_is_thread(g_leancam_ui.prog.lines[pi]))
                {
                    const char *tool = lc_find_prefix_in_program(&g_leancam_ui.prog, pi, "TOOL|");
                    float start_lane = 0.0f;
                    float stop_lane = 0.0f;

                    if (lc_calc_thread_lanes(g_leancam_ui.prog.lines[pi], tool,
                                             &start_lane, &stop_lane, NULL, NULL, NULL))
                    {
                        snprintf(buf, sizeof(buf), "%02d %s | ELS %.2f/%.2f",
                                 pi + 1, g_leancam_ui.prog.lines[pi], start_lane, stop_lane);
                    }
                    else
                    {
                        snprintf(buf, sizeof(buf), "%02d %s", pi + 1, g_leancam_ui.prog.lines[pi]);
                    }
                }
                else
                {
                    snprintf(buf, sizeof(buf), "%02d %s", pi + 1, g_leancam_ui.prog.lines[pi]);
                }
                lc_snapshot_line(f,
                                 row++,
                                 buf,
                                 (pi == g_leancam_ui.cur_line) && (!g_leancam_ui.draft_active));
            }
        }
    }

    if (g_leancam_ui.draft_active)
    {
        uint8_t field_count = lc_count_brace_fields(g_leancam_ui.draft_line);
        if (g_draft_field_index < field_count)
        {
            snprintf(buf, sizeof(buf), "F%u/%u input='%.24s' | B=+/- | C=. | D accept | # finish",
                     (unsigned)(g_draft_field_index + 1u),
                     (unsigned)field_count,
                     g_leancam_ui.input_buf);
        }
        else
        {
            snprintf(buf, sizeof(buf), "FIELDS DONE | # finish | * back | A cancel");
        }
        ui_snapshot_strcpy(f->leancam_helper, buf, sizeof(f->leancam_helper));
    }
    else
    {
        ui_snapshot_strcpy(f->leancam_helper,
                           "0 tool | 1-9 new op | B/C move | D edit | A files | * delete | # run",
                           sizeof(f->leancam_helper));
    }
}

static void lc_snapshot_nc_view(ui_snapshot_frame_t *f)
{
    int row = 0;
    int i;
    char buf[UI_LC_LINE_LEN];

    snprintf(buf, sizeof(buf), "FILE: %s  top:%d", lc_basename(g_nc_path), g_nc_top_line + 1);
    lc_snapshot_line(f, row++, buf, false);

    for (i = 0; i < g_nc_line_count && row < UI_LC_MAX_LINES; ++i)
    {
        snprintf(buf, sizeof(buf), "%04d %s", g_nc_top_line + i + 1, g_nc_lines[i]);
        lc_snapshot_line(f, row++, buf, (i == g_nc_selected_row));
    }

    if (g_nc_line_count == 0 && row < UI_LC_MAX_LINES)
        lc_snapshot_line(f, row++, "<empty nc file>", false);

    ui_snapshot_strcpy(f->leancam_helper,
                       "B/C scroll | A files | # view only",
                       sizeof(f->leancam_helper));
}

static const char *lc_selected_file_setup_line(void)
{
    static program_t preview_prog;
    static char setup_copy[UI_LC_LINE_LEN];
    static int cached_sel = -2;
    static uint32_t cached_count = 0;
    char path[LC_FILE_PATH_MAX];
    int cnt = leancam_files_count();
    const char *setup;

    if (g_lc_mode != LC_MODE_FILES || !g_files_ready || g_file_sel < 0 || g_file_sel >= cnt)
        return NULL;

    if (!lc_has_suffix_ci_local(leancam_files_name(g_file_sel), ".lcam"))
        return NULL;

    if (cached_sel == g_file_sel && cached_count == (uint32_t)cnt)
        return setup_copy[0] ? setup_copy : NULL;

    cached_sel = g_file_sel;
    cached_count = (uint32_t)cnt;
    setup_copy[0] = 0;

    if (!leancam_files_build_path(LC_FILES_DIR, g_file_sel, path, sizeof(path)))
        return NULL;

    if (!leancam_files_load(path, &preview_prog))
        return NULL;

    setup = lc_find_setup_in_program(&preview_prog, preview_prog.count - 1);
    if (!setup || !setup[0])
        return NULL;

    ui_snapshot_strcpy(setup_copy, setup, sizeof(setup_copy));
    return setup_copy;
}

static void lc_snapshot_preview_sources(ui_snapshot_frame_t *f)
{
    const char *setup = NULL;
    const char *line = NULL;
    const char *tool = NULL;
    int before_or_at = g_leancam_ui.cur_line;

    if (!f)
        return;

    (void)lc_setup_line(&setup);

    if (g_lc_mode == LC_MODE_FILES)
    {
        setup = lc_selected_file_setup_line();
        line = NULL;
        tool = NULL;
    }
    else if (g_lc_mode == LC_MODE_NC_VIEW)
    {
        setup = g_nc_setup_line[0] ? g_nc_setup_line : NULL;
        line = NULL;
        tool = NULL;
    }
    else if (g_leancam_ui.draft_active)
    {
        char preview_line[MAX_LEN];

        if (g_leancam_ui.draft_replace_index >= 0)
            before_or_at = g_leancam_ui.draft_replace_index;
        else
            before_or_at = g_leancam_ui.draft_insert_after + 1;

        lc_build_draft_preview_line(preview_line, sizeof(preview_line));
        line = preview_line;
        lc_get_field_name_by_index(g_leancam_ui.draft_line,
                                   g_draft_field_index,
                                   f->leancam_active_field,
                                   sizeof(f->leancam_active_field));
        ui_snapshot_strcpy(f->leancam_preview_line, line, sizeof(f->leancam_preview_line));
    }
    else if (g_leancam_ui.cur_line >= 0 && g_leancam_ui.cur_line < g_leancam_ui.prog.count)
    {
        line = g_leancam_ui.prog.lines[g_leancam_ui.cur_line];
    }

    if (g_lc_mode != LC_MODE_FILES && g_lc_mode != LC_MODE_NC_VIEW)
        tool = lc_find_prefix_in_program(&g_leancam_ui.prog, before_or_at, "TOOL|");

    ui_snapshot_strcpy(f->leancam_setup_line, setup ? setup : "", sizeof(f->leancam_setup_line));
    if (!g_leancam_ui.draft_active)
        ui_snapshot_strcpy(f->leancam_preview_line, line ? line : "", sizeof(f->leancam_preview_line));
    ui_snapshot_strcpy(f->leancam_tool_line, tool ? tool : "", sizeof(f->leancam_tool_line));
}

void leancam_bridge_fill_snapshot(ui_snapshot_frame_t *f)
{
    int i;
    int row = 0;
    char buf[UI_LC_LINE_LEN];
    char status[UI_LC_LINE_LEN];

    if (!f)
        return;

    f->leancam_active = true;
    f->leancam_mode = (uint8_t)g_lc_mode;
    f->leancam_show_menu = (bool)leancam_bridge_wants_process_menu();
    f->leancam_line_count = 0;
    f->leancam_setup_line[0] = 0;
    f->leancam_preview_line[0] = 0;
    f->leancam_tool_line[0] = 0;
    f->leancam_active_field[0] = 0;
    f->leancam_thread_lane_valid = false;
    f->leancam_thread_start_lane = 0.0f;
    f->leancam_thread_stop_lane = 0.0f;
    f->leancam_thread_ramp_lane = 0.0f;
    f->leancam_thread_lock_lane = 0.0f;
    f->leancam_thread_z_speed = 0.0f;

    for (i = 0; i < UI_LC_MAX_LINES; ++i)
    {
        f->leancam_lines[i][0] = 0;
        f->leancam_line_selected[i] = 0;
        f->leancam_field_hi_start[i] = 0;
        f->leancam_field_hi_end[i] = 0;
    }

    ui_snapshot_strcpy(f->leancam_message, g_last_msg, sizeof(f->leancam_message));

    switch (g_lc_mode)
    {
        case LC_MODE_FILES:
        {
            int cnt = leancam_files_count();

            ui_snapshot_strcpy(f->leancam_title, "LeanCam Files", sizeof(f->leancam_title));
            lc_snapshot_line(f, row++, "Storage: " LC_FILES_DIR, false);
            if (!g_files_ready && row < UI_LC_MAX_LINES)
                lc_snapshot_line(f, row++, "Waiting for SD card...", false);

            for (i = 0; i < cnt && row < UI_LC_MAX_LINES; ++i)
            {
                snprintf(buf, sizeof(buf), "%s", leancam_files_name(i));
                lc_snapshot_line(f, row++, buf, (i == g_file_sel));
            }

            if (row < UI_LC_MAX_LINES)
                lc_snapshot_line(f, row++, "NEW...", (g_file_sel == cnt));

            ui_snapshot_strcpy(f->leancam_helper, "B/C move | D open/new | A refresh | * delete | # make .nc", sizeof(f->leancam_helper));
            break;
        }

        case LC_MODE_FILE_NAME:
            ui_snapshot_strcpy(f->leancam_title, "New LeanCam File", sizeof(f->leancam_title));
            snprintf(buf, sizeof(buf), "NAME{%s}", g_new_file_name);
            lc_snapshot_line(f, row++, buf, true);
            ui_snapshot_strcpy(f->leancam_helper, "digits=name | *=backspace | D/# create | A cancel", sizeof(f->leancam_helper));
            break;

        case LC_MODE_PROGRAM:
            ui_snapshot_strcpy(f->leancam_title, "LeanCam Program", sizeof(f->leancam_title));
            lc_snapshot_program(f);
            break;

        case LC_MODE_DRAFT:
            ui_snapshot_strcpy(f->leancam_title, "LeanCam Draft", sizeof(f->leancam_title));
            lc_snapshot_program(f);
            break;

        case LC_MODE_NC_VIEW:
            ui_snapshot_strcpy(f->leancam_title, "NC Viewer", sizeof(f->leancam_title));
            lc_snapshot_nc_view(f);
            break;

        default:
            ui_snapshot_strcpy(f->leancam_title, "LeanCam", sizeof(f->leancam_title));
            break;
    }

    lc_snapshot_preview_sources(f);

    if (lc_calc_thread_lanes(f->leancam_preview_line,
                             f->leancam_tool_line,
                             &f->leancam_thread_start_lane,
                             &f->leancam_thread_stop_lane,
                             &f->leancam_thread_ramp_lane,
                             &f->leancam_thread_lock_lane,
                             &f->leancam_thread_z_speed))
    {
        f->leancam_thread_lane_valid = true;
    }

    snprintf(status, sizeof(status), "%.44s  %.48s", f->leancam_title, g_last_msg);
    ui_snapshot_set_status(f, status);
}
