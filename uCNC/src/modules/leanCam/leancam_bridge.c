#include "leancam_bridge.h"
#include "leancam_app.h"
#include "leancam_autosave.h"
#include "leancam_ui.h"
#include "leancam_templates.h"
#include "leancam_files.h"
#include "leancam_expr.h"
#include "leancam_editor.h"
#include "leancam_file_browser.h"
#include "leancam_file_prompt.h"
#include "leancam_gcode.h"
#include "leancam_menu.h"
#include "leancam_nc_viewer.h"
#include "leancam_paths.h"
#include "leancam_presets.h"
#include "leancam_resource.h"
#include "leancam_regions.h"
#include "leancam_run.h"
#include "leancam_snapshot.h"
#include "leancam_text.h"
#include "leancam_tool_catalog.h"
#include "leancam_validate.h"
#if LC_SELECTED_RUN_USE_STREAM
#include "cam_stream.h"
#endif
#include "../file_system.h"
#include "../../cnc.h"
#include "../../interface/grbl_stream.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>

#define LC_FILES_DIR LC_DEFAULT_DIR
#define LC_TOOLS_FILE LC_FILES_DIR "/tools.lct"

#ifndef LC_VISIBLE_PROGRAM_LINES
#define LC_VISIBLE_PROGRAM_LINES 18
#endif

#ifndef LC_VISIBLE_TOOL_LINES
#define LC_VISIBLE_TOOL_LINES 12
#endif

#ifndef LC_FILE_REFRESH_RETRY_MS
#define LC_FILE_REFRESH_RETRY_MS 1500u
#endif

#ifndef LC_AUTOSAVE_RETRY_MS
#define LC_AUTOSAVE_RETRY_MS 500u
#endif

#ifndef LC_AUTOSAVE_BUSY_MAX_TRIES
#define LC_AUTOSAVE_BUSY_MAX_TRIES 3u
#endif

#ifndef LC_SELECTED_RUN_USE_STREAM
#define LC_SELECTED_RUN_USE_STREAM 0
#endif

#ifndef LEANCAM_DIRECT_STREAM_BUFFER
#define LEANCAM_DIRECT_STREAM_BUFFER 8192u
#endif

#ifndef LC_BRIDGE_SERIAL_DEBUG
#define LC_BRIDGE_SERIAL_DEBUG 1
#endif

#ifndef LC_BRIDGE_R_CORNER_DEBUG
#define LC_BRIDGE_R_CORNER_DEBUG 0
#endif

#if LC_BRIDGE_SERIAL_DEBUG
#define LC_BRIDGE_DBG(fmt, ...) grbl_stream_printf(__romstr__("[MSG:LC " fmt "]\r\n"), ##__VA_ARGS__)
#else
#define LC_BRIDGE_DBG(fmt, ...)
#endif

#ifndef LC_DRAFT_EDIT_SERIAL_DEBUG
#define LC_DRAFT_EDIT_SERIAL_DEBUG 0
#endif

#if LC_DRAFT_EDIT_SERIAL_DEBUG
#define LC_DRAFT_DBG(fmt, ...) LC_BRIDGE_DBG("draft " fmt, ##__VA_ARGS__)
#else
#define LC_DRAFT_DBG(fmt, ...)
#endif

static bool lc_debug_snapshot_should_print(const char *line);
static void lc_debug_print_r_corner_geometry(const ui_snapshot_frame_t *f);

#if LC_BRIDGE_R_CORNER_DEBUG
typedef struct {
    float x;
    float z;
} lc_debug_v2_t;

static lc_debug_v2_t lc_debug_v2_add(lc_debug_v2_t a, lc_debug_v2_t b)
{
    lc_debug_v2_t r = { a.x + b.x, a.z + b.z };
    return r;
}

static lc_debug_v2_t lc_debug_v2_sub(lc_debug_v2_t a, lc_debug_v2_t b)
{
    lc_debug_v2_t r = { a.x - b.x, a.z - b.z };
    return r;
}

static lc_debug_v2_t lc_debug_v2_mul(lc_debug_v2_t a, float s)
{
    lc_debug_v2_t r = { a.x * s, a.z * s };
    return r;
}

static float lc_debug_v2_dot(lc_debug_v2_t a, lc_debug_v2_t b)
{
    return (a.x * b.x) + (a.z * b.z);
}

static float lc_debug_v2_cross(lc_debug_v2_t a, lc_debug_v2_t b)
{
    return (a.x * b.z) - (a.z * b.x);
}

static float lc_debug_v2_len(lc_debug_v2_t a)
{
    return sqrtf(lc_debug_v2_dot(a, a));
}

static bool lc_debug_v2_norm(lc_debug_v2_t a, lc_debug_v2_t *out)
{
    float l = lc_debug_v2_len(a);

    if (!out || l < 0.0001f)
        return false;
    out->x = a.x / l;
    out->z = a.z / l;
    return true;
}

static float lc_debug_clampf(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static bool lc_debug_build_r_corner(lc_debug_v2_t p0,
                                    lc_debug_v2_t p1,
                                    lc_debug_v2_t p2,
                                    float r,
                                    lc_debug_v2_t *t1,
                                    lc_debug_v2_t *t2,
                                    lc_debug_v2_t *c,
                                    bool *cw)
{
    lc_debug_v2_t p0r = { p0.x * 0.5f, p0.z };
    lc_debug_v2_t p1r = { p1.x * 0.5f, p1.z };
    lc_debug_v2_t p2r = { p2.x * 0.5f, p2.z };
    lc_debug_v2_t a;
    lc_debug_v2_t b;
    lc_debug_v2_t bis;
    float len_a = lc_debug_v2_len(lc_debug_v2_sub(p0r, p1r));
    float len_b = lc_debug_v2_len(lc_debug_v2_sub(p2r, p1r));
    float dot;
    float half;
    float tan_half;
    float sin_half;
    float tangent;

    if (!t1 || !t2 || !c || !cw || r <= 0.0f || len_a < 0.0001f || len_b < 0.0001f)
        return false;
    if (!lc_debug_v2_norm(lc_debug_v2_sub(p0r, p1r), &a) ||
        !lc_debug_v2_norm(lc_debug_v2_sub(p2r, p1r), &b))
        return false;
    dot = lc_debug_clampf(lc_debug_v2_dot(a, b), -1.0f, 1.0f);
    if (fabsf(dot) > 0.999f)
        return false;
    half = acosf(dot) * 0.5f;
    tan_half = tanf(half);
    sin_half = sinf(half);
    if (fabsf(tan_half) < 0.0001f || fabsf(sin_half) < 0.0001f)
        return false;
    tangent = r / tan_half;
    if (tangent > len_a + 0.0001f || tangent > len_b + 0.0001f)
        return false;
    if (!lc_debug_v2_norm(lc_debug_v2_add(a, b), &bis))
        return false;

    *t1 = lc_debug_v2_add(p1r, lc_debug_v2_mul(a, tangent));
    *t2 = lc_debug_v2_add(p1r, lc_debug_v2_mul(b, tangent));
    *c = lc_debug_v2_add(p1r, lc_debug_v2_mul(bis, r / sin_half));
    if (fabsf(lc_debug_v2_len(lc_debug_v2_sub(*t1, *c)) - r) > 0.01f ||
        fabsf(lc_debug_v2_len(lc_debug_v2_sub(*t2, *c)) - r) > 0.01f)
        return false;
    *cw = lc_debug_v2_cross(lc_debug_v2_sub(*t1, *c), lc_debug_v2_sub(*t2, *c)) < 0.0f;
    t1->x *= 2.0f;
    t2->x *= 2.0f;
    c->x *= 2.0f;
    return true;
}
#endif

static void lc_debug_print_r_corner_geometry(const ui_snapshot_frame_t *f)
{
#if LC_BRIDGE_R_CORNER_DEBUG
    uint8_t i;

    if (!f || f->leancam_preview_region_count < 3)
        return;

    for (i = 1; i + 1u < f->leancam_preview_region_count && i < UI_LC_PREVIEW_REGION_MAX; ++i) {
        const char *prev = f->leancam_preview_region[i - 1u];
        const char *line = f->leancam_preview_region[i];
        const char *next = f->leancam_preview_region[i + 1u];
        float x0, z0, x1, z1, x2, z2, r;
        lc_debug_v2_t t1;
        lc_debug_v2_t t2;
        lc_debug_v2_t c;
        bool cw;

        if (!lc_text_command_is(prev, "G1") ||
            !lc_text_command_is(line, "G1") ||
            !lc_text_command_is(next, "G1"))
            continue;
        if (!lc_text_get_field_float(line, "R", &r) || r <= 0.0f)
            continue;
        if (!lc_text_get_field_float(prev, "X", &x0) ||
            !lc_text_get_field_float(prev, "Z", &z0) ||
            !lc_text_get_field_float(line, "X", &x1) ||
            !lc_text_get_field_float(line, "Z", &z1) ||
            !lc_text_get_field_float(next, "X", &x2) ||
            !lc_text_get_field_float(next, "Z", &z2))
            continue;
        if (lc_debug_build_r_corner((lc_debug_v2_t){x0, z0},
                                    (lc_debug_v2_t){x1, z1},
                                    (lc_debug_v2_t){x2, z2},
                                    r,
                                    &t1,
                                    &t2,
                                    &c,
                                    &cw)) {
            LC_BRIDGE_DBG("R corner L%u T1 X%.3f Z%.3f T2 X%.3f Z%.3f C X%.3f Z%.3f R%.3f %s",
                          (unsigned)i,
                          (double)t1.x,
                          (double)t1.z,
                          (double)t2.x,
                          (double)t2.z,
                          (double)c.x,
                          (double)c.z,
                          (double)r,
                          cw ? "CW" : "CCW");
        } else {
            LC_BRIDGE_DBG("R corner L%u invalid R%.3f",
                          (unsigned)i,
                          (double)r);
        }
    }
#else
    (void)f;
#endif
}

#ifndef G33_ELS_LOCK_REV_MIN
#define G33_ELS_LOCK_REV_MIN 2
#endif

static leancam_ui_t g_leancam_ui;
#define g_lc_mode (*lc_app_mode_ptr())
#define g_catalog_kind (*lc_app_catalog_ptr())
static bool g_debug_snapshot_armed = false;
#define g_line_sim_armed (lc_run_sim_armed())
static int  g_prog_scroll = 0;
static uint32_t g_sim_arm_block_until_ms = 0;
static char g_last_msg[UI_SNAPSHOT_POPUP_LEN];
#define g_draft_field_index (lc_editor_field_index())

static int lc_setup_line(const char **line_out);
#define lc_count_brace_fields(line) lc_editor_field_count(line)
static void lc_begin_setup_if_missing_then(const char *tmpl);
static int lc_has_setup(void);
static void lc_autosave(void);
static void lc_schedule_autosave(void);
static void lc_ensure_program_visible(void);
static bool lc_find_raw_region(const program_t *prog, int index, int *start_out, int *end_out);
static void lc_debug_dump_nc_range(const program_t *prog, int start, int end, const char *why);
static bool lc_save_tool_catalog(void);
void __attribute__((weak)) leancam_bridge_after_autosave(void);

static bool lc_line_command_is(const char *line, const char *cmd)
{
    return lc_text_command_is(line, cmd);
}

typedef struct
{
    fs_file_t *fp;
} lc_gcode_file_emit_t;

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

static bool lc_save_tool_catalog(void)
{
    static program_t tool_prog;
    int selected_t = -1;
    int i;

    if (leancam_files_busy() || !lc_resource_can_autosave())
    {
        lc_set_msg("LC: file IO busy");
        return false;
    }

    if (g_leancam_ui.cur_line >= 0 && g_leancam_ui.cur_line < g_leancam_ui.prog.count)
        selected_t = lc_tool_line_t_value(g_leancam_ui.prog.lines[g_leancam_ui.cur_line]);

    lc_tool_catalog_copy_from_program(&g_leancam_ui.prog);
    lc_tool_catalog_copy_to_program(&tool_prog);
    if (!leancam_files_save_plain(LC_TOOLS_FILE, &tool_prog))
    {
        lc_set_msg("LC: tools save failed");
        return false;
    }

    lc_tool_catalog_copy_to_program(&g_leancam_ui.prog);
    if (selected_t >= 0)
    {
        for (i = 0; i < g_leancam_ui.prog.count; ++i)
        {
            if (lc_tool_line_t_value(g_leancam_ui.prog.lines[i]) == selected_t)
            {
                g_leancam_ui.cur_line = i;
                break;
            }
        }
    }
    lc_ensure_program_visible();
    lc_set_msg("LC: tools saved");
    return true;
}

static bool lc_debug_snapshot_should_print(const char *line)
{
    static char last_line[UI_LC_LINE_LEN];

    if (!line) {
        line = "";
    }
    if (g_debug_snapshot_armed || strcmp(line, last_line) != 0) {
        ui_snapshot_strcpy(last_line, line, sizeof(last_line));
        return true;
    }
    return false;
}

static void lc_refresh_files(void)
{
    if (!lc_file_browser_refresh(LC_FILES_DIR, LC_FILE_REFRESH_RETRY_MS))
    {
        lc_set_msg("LC: refresh failed");
        return;
    }
    lc_tool_catalog_load_from_storage(LC_TOOLS_FILE);
    lc_set_msg("LC: files refreshed");
}

static void lc_delete_selected_file(void)
{
    char path[LC_FILE_PATH_MAX];

    if (leancam_files_busy())
    {
        lc_set_msg("LC: file IO busy");
        return;
    }

    if (!lc_file_browser_selected_valid())
    {
        lc_set_msg("LC: no file selected");
        return;
    }

    if (!lc_file_browser_selected_path(LC_FILES_DIR, path, sizeof(path)))
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
    {
        leancam_ui_new(&g_leancam_ui);
        lc_presets_clear_meta();
    }

    lc_refresh_files();
    lc_file_browser_clamp_selected();
    lc_set_msg("LC: file deleted");
}

static void lc_duplicate_selected_file_begin(void)
{
    char path[LC_FILE_PATH_MAX];
    const char *name;

    if (leancam_files_busy())
    {
        lc_set_msg("LC: file IO busy");
        return;
    }

    if (!lc_file_browser_selected_valid())
    {
        lc_set_msg("LC: no file selected");
        return;
    }

    name = lc_file_browser_selected_name();
    if (!lc_path_has_suffix_ci(name, ".nc"))
    {
        lc_set_msg("LC: copy nc only");
        return;
    }

    if (!lc_file_browser_selected_path(LC_FILES_DIR, path, sizeof(path)))
    {
        lc_set_msg("LC: build path failed");
        return;
    }

    lc_file_prompt_begin_duplicate(path);
    g_lc_mode = LC_MODE_FILE_NAME;
    lc_set_msg("LC: copy name");
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

static int lc_gcode_discard_line(const char *line, void *user)
{
    (void)line;
    (void)user;
    return 1;
}

#ifndef LC_SERIAL_GCODE_MAX_LINES
#define LC_SERIAL_GCODE_MAX_LINES 160u
#endif

#ifndef LC_SERIAL_GCODE_LINE_LEN
#define LC_SERIAL_GCODE_LINE_LEN 96u
#endif

#ifndef LC_SERIAL_GCODE_PACE_MS
#define LC_SERIAL_GCODE_PACE_MS 20u
#endif

typedef struct {
    uint16_t count;
    uint16_t index;
    uint32_t next_ms;
    uint32_t start_ms;
    int start;
    int end;
    lc_gcode_result_t result;
    int err_line;
    char err[64];
    char lines[LC_SERIAL_GCODE_MAX_LINES][LC_SERIAL_GCODE_LINE_LEN];
    char current_line[LC_SERIAL_GCODE_LINE_LEN];
    uint16_t draw_seq;
    uint16_t current_index;
    bool active;
    bool end_printed;
} lc_serial_gcode_dump_t;

static lc_serial_gcode_dump_t g_serial_gcode_dump;

static int lc_serial_capture_gcode_line(const char *line, void *user)
{
    lc_serial_gcode_dump_t *dump = (lc_serial_gcode_dump_t *)user;

    if (!dump || dump->count >= LC_SERIAL_GCODE_MAX_LINES)
        return 0;
    snprintf(dump->lines[dump->count],
             sizeof(dump->lines[dump->count]),
             "%.90s",
             line ? line : "");
    dump->count++;
    return 1;
}

static void lc_serial_debug_begin_selected_gcode(int start, int end)
{
    lc_gcode_result_t r;
    int err_line = start + 1;
    int made = 0;
    lc_serial_gcode_dump_t *dump = &g_serial_gcode_dump;

    memset(dump, 0, sizeof(*dump));
    dump->start = start;
    dump->end = end;
    dump->err_line = err_line;
    dump->start_ms = mcu_millis();
    dump->next_ms = dump->start_ms + LC_SERIAL_GCODE_PACE_MS;
    r = leancam_gcode_emit_program_header(lc_serial_capture_gcode_line, dump) ?
        LC_GCODE_OK : LC_GCODE_STREAM_REJECT;
    if (r == LC_GCODE_OK)
        r = lc_run_emit_selected_range(&g_leancam_ui.prog,
                                       start,
                                       end,
                                       lc_serial_capture_gcode_line,
                                       dump,
                                       dump->err,
                                       sizeof(dump->err),
                                       &err_line,
                                       &made);
    if (r == LC_GCODE_OK &&
        !leancam_gcode_emit_program_footer_ex(lc_serial_capture_gcode_line, dump, dump->err, sizeof(dump->err)))
        r = LC_GCODE_STREAM_REJECT;
    dump->result = r;
    dump->err_line = err_line;
    dump->active = true;
    grbl_stream_printf(__romstr__("[MSG:LC draw begin L%d-L%d count=%u pace=%u]\r\n"),
                       start + 1,
                       end + 1,
                       (unsigned)dump->count,
                       (unsigned)LC_SERIAL_GCODE_PACE_MS);
}

static void lc_serial_debug_gcode_tick(uint32_t now)
{
    lc_serial_gcode_dump_t *dump = &g_serial_gcode_dump;

    if (!dump->active || (int32_t)(now - dump->next_ms) < 0)
        return;
    dump->next_ms = now + LC_SERIAL_GCODE_PACE_MS;
    if (dump->index < dump->count) {
        snprintf(dump->current_line,
                 sizeof(dump->current_line),
                 "%s",
                 dump->lines[dump->index]);
        dump->current_index = dump->index;
        dump->draw_seq++;
        dump->index++;
        leancam_bridge_request_render();
        return;
    }
    if (!dump->end_printed) {
        uint32_t dt = now - dump->start_ms;
        grbl_stream_printf(__romstr__("[MSG:LC draw end r=%d count=%u drawn=%u dt=%u err_line=%d err=%s]\r\n"),
                           (int)dump->result,
                           (unsigned)dump->count,
                           (unsigned)dump->index,
                           (unsigned)dt,
                           dump->err_line,
                           dump->err);
        if (dump->result != LC_GCODE_OK) {
            lc_set_msgf("LC: L%d %s",
                        dump->err_line,
                        dump->err[0] ? dump->err : lc_run_gcode_result_name(dump->result));
        }
        dump->end_printed = true;
        dump->draw_seq++;
        leancam_bridge_request_render();
        return;
    }
    dump->active = false;
}

static const char *lc_find_setup_in_program(const program_t *p, int before_or_at)
{
    int i;

    if (!p)
        return NULL;

    if (before_or_at >= p->count)
        before_or_at = p->count - 1;

    for (i = before_or_at; i >= 0; --i)
        if (lc_line_command_is(p->lines[i], "SETUP"))
            return p->lines[i];

    return NULL;
}

static bool lc_line_get_float3_key(const char *line, const char *a, const char *b, const char *c, float *out)
{
    return lc_text_get_field_float(line, a, out) ||
           lc_text_get_field_float(line, b, out) ||
           lc_text_get_field_float(line, c, out);
}

static const char *lc_effective_tool_for_cycle(const program_t *prog, int before_or_at, const char *cycle)
{
    return lc_validate_effective_tool_for_cycle(prog, before_or_at, cycle);
}

static const char *lc_effective_tool_for_draft(const program_t *prog,
                                               int before_or_at,
                                               const char *draft,
                                               const char *preview)
{
    const char *tool = lc_effective_tool_for_cycle(prog, before_or_at, preview);

    if (!tool && lc_line_command_is(draft, "TOOLCALL"))
    {
        char raw[32];
        char resolved[32];
        char *endp;
        long t;

        if (lc_editor_line_get_field_text(draft, "T", raw, sizeof(raw)) &&
            leancam_expr_resolve_field_value(raw, NULL, NULL, draft, resolved, sizeof(resolved)))
        {
            t = strtol(resolved, &endp, 10);
            if (endp && *endp == 0 && t > 0)
                tool = lc_tool_catalog_find_in_program_or_catalog(prog, before_or_at, (int)t);
        }
    }

    return tool;
}

static bool lc_line_is_thread(const char *line)
{
    return line &&
           ((strncmp(line, "G33", 3) == 0 && (line[3] == 0 || line[3] == ' ')) ||
            (strncmp(line, "G76", 3) == 0 && (line[3] == 0 || line[3] == ' ')));
}

static int lc_line_display_indent(const program_t *prog, int index, const char *line)
{
    return lc_region_display_indent(prog, index, line);
}

static bool lc_expand_committed_preset(int row)
{
    const char *setup = NULL;
    int target_row = row;
    char err[64];

    (void)lc_setup_line(&setup);
    if (!lc_presets_expand_committed(&g_leancam_ui.prog,
                                     row,
                                     setup,
                                     &target_row,
                                     err,
                                     sizeof(err)))
    {
        if (err[0])
            lc_set_msg(err);
        return false;
    }

    g_leancam_ui.cur_line = target_row;
    return true;
}

static bool lc_resolve_template_for_insert(const char *tmpl, int insert_index, char *out, uint32_t out_len)
{
    static const char *default_tool =
        "TOOL T1 R0.8 ORIENT3 R_FEED120 FIN_FEED60 DOC2.0 FIN_DOC0.5 RPM800 XOFF0 ZOFF0";
    const char *p;
    const char *setup = NULL;
    const char *tool = NULL;
    char *w;

    if (!tmpl || !out || out_len == 0)
        return false;

    out[0] = 0;
    (void)lc_setup_line(&setup);
    tool = lc_effective_tool_for_cycle(&g_leancam_ui.prog, insert_index, tmpl);
    if (!tool && lc_line_command_is(tmpl, "TOOLCALL"))
        tool = lc_tool_catalog_find_in_program_or_catalog(&g_leancam_ui.prog, insert_index, 1);
    if (!tool)
        tool = default_tool;

    p = tmpl;
    w = out;
    while (*p && (uint32_t)(w - out) < out_len - 1u)
    {
        if (*p == '{')
        {
            const char *close = strchr(p + 1, '}');
            char raw[UI_LC_LINE_LEN];
            char resolved[UI_LC_LINE_LEN];
            uint32_t n;
            int written;

            if (!close)
                return false;

            n = (uint32_t)(close - p - 1);
            if (n >= sizeof(raw))
                n = sizeof(raw) - 1u;
            if (n)
                memcpy(raw, p + 1, n);
            raw[n] = 0;

            if (raw[0] &&
                !leancam_expr_resolve_field_value(raw,
                                                  setup,
                                                  tool,
                                                  tmpl,
                                                  resolved,
                                                  sizeof(resolved)))
                return false;

            written = snprintf(w,
                               out_len - (uint32_t)(w - out),
                               "%s",
                               raw[0] ? resolved : "");
            if (written < 0 || (uint32_t)written >= out_len - (uint32_t)(w - out))
                return false;
            w += written;
            p = close + 1;
            continue;
        }
        *w++ = *p++;
    }

    *w = 0;
    return true;
}

static void lc_insert_process_preset_template(const char *tmpl)
{
    char preset[MAX_LEN];
    int insert_after;
    int insert_index;

    if (leancam_files_busy())
    {
        lc_set_msg("LC: file IO busy");
        return;
    }

    if (!lc_has_setup())
    {
        if (leancam_ui_begin_template(&g_leancam_ui, lc_template_setup()))
        {
            lc_editor_reset();
            g_lc_mode = LC_MODE_DRAFT;
            lc_set_msg("LC: setup first");
        }
        return;
    }

    insert_after = g_leancam_ui.cur_line;
    insert_index = insert_after + 1;
    if (!lc_resolve_template_for_insert(tmpl, insert_index, preset, sizeof(preset)))
    {
        lc_set_msg("LC: preset unresolved");
        return;
    }

    if (!prog_insert_after(&g_leancam_ui.prog, insert_after, preset))
    {
        lc_set_msg("LC: no room for preset");
        return;
    }

    g_leancam_ui.cur_line = insert_index;
    if (!lc_expand_committed_preset(g_leancam_ui.cur_line))
    {
        (void)prog_delete(&g_leancam_ui.prog, insert_index);
        if (g_leancam_ui.cur_line >= g_leancam_ui.prog.count)
            g_leancam_ui.cur_line = g_leancam_ui.prog.count - 1;
        lc_set_msg("LC: preset expand failed");
        return;
    }

    g_lc_mode = LC_MODE_PROGRAM;
    lc_schedule_autosave();
    lc_set_msg("LC: preset expanded");
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
    (void)tool_line;

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

static void lc_prepare_selected_file_for_run(void)
{
    char in_path[LC_FILE_PATH_MAX];
    program_t prog;
    int made = 0;
    int fail_line = 0;
    lc_gcode_result_t r = LC_GCODE_OK;
    char err[64];

    if (leancam_files_busy())
    {
        lc_set_msg("LC: file IO busy");
        return;
    }

    if (!lc_file_browser_selected_valid())
    {
        lc_set_msg("LC: no file selected");
        return;
    }

    if (!lc_path_has_suffix_ci(lc_file_browser_selected_name(), ".nc"))
    {
        lc_set_msg("LC: select .nc");
        return;
    }
    lc_publish_msg("LC: preparing %s", lc_file_browser_selected_name());

    if (!lc_file_browser_selected_path(LC_FILES_DIR, in_path, sizeof(in_path)))
    {
        lc_set_msg("LC: path failed");
        return;
    }

    lc_publish_msg("LC: loading %s", lc_path_basename(in_path));

    if (!leancam_files_load(in_path, &prog))
    {
        lc_set_msg("LC: load failed");
        leancam_bridge_request_render();
        return;
    }
    lc_publish_msg("LC: checking cycles");
    r = lc_run_preflight_program(&prog, err, sizeof(err), &fail_line, &made);
    if (r != LC_GCODE_OK)
    {
        lc_set_msgf("LC: L%d %.36s", fail_line, err[0] ? err : lc_run_gcode_result_name(r));
        leancam_bridge_request_render();
        return;
    }

    if (made <= 0)
    {
        lc_set_msg("LC: nothing to run");
        leancam_bridge_request_render();
        return;
    }

    lc_set_msgf("LC: ready %d lines", made);
    leancam_bridge_request_render();
}

void __attribute__((weak)) leancam_bridge_request_render(void)
{
}

void __attribute__((weak)) leancam_bridge_after_autosave(void)
{
}

static void lc_autosave(void)
{
    if (leancam_files_busy())
    {
        if (!lc_autosave_defer_busy(mcu_millis(),
                                    LC_AUTOSAVE_RETRY_MS,
                                    LC_AUTOSAVE_BUSY_MAX_TRIES))
        {
            lc_set_msg("LC: save busy, stopped");
            LC_BRIDGE_DBG("autosave stopped busy tries=%u", (unsigned)LC_AUTOSAVE_BUSY_MAX_TRIES);
            return;
        }

        lc_set_msg("LC: saving");
        LC_BRIDGE_DBG("autosave deferred try=%u",
                      (unsigned)lc_autosave_busy_tries());
        return;
    }

    lc_autosave_clear();

    if (g_catalog_kind == LC_CATALOG_TOOLS)
    {
        LC_BRIDGE_DBG("tools save begin count=%d cur=%d",
                      g_leancam_ui.prog.count,
                      g_leancam_ui.cur_line);
        if (lc_save_tool_catalog())
        {
            LC_BRIDGE_DBG("tools save ok cur=%d", g_leancam_ui.cur_line);
            leancam_bridge_after_autosave();
        }
        else
        {
            LC_BRIDGE_DBG("tools save failed");
        }
        return;
    }

    if (!g_leancam_ui.current_path[0])
        return;

    LC_BRIDGE_DBG("autosave begin path=%.48s count=%d cur=%d",
                  g_leancam_ui.current_path,
                  g_leancam_ui.prog.count,
                  g_leancam_ui.cur_line);
    if (leancam_ui_save(&g_leancam_ui, g_leancam_ui.current_path))
    {
        leancam_files_debug_probe("bridge-save-ok");
        leancam_files_debug_probe("bridge-program-save-ok");
        leancam_files_debug_probe("bridge-msg-begin");
        lc_set_msg("LC: saved");
        leancam_files_debug_probe("bridge-msg-ok");
        LC_BRIDGE_DBG("autosave ok cur=%d", g_leancam_ui.cur_line);
        leancam_files_debug_probe("bridge-log-ok");
        leancam_bridge_after_autosave();
    }
    else
    {
        lc_set_msg("LC: save failed");
        LC_BRIDGE_DBG("autosave failed");
    }
}

static void lc_schedule_autosave(void)
{
    lc_autosave_schedule(mcu_millis(), 250u);
    g_debug_snapshot_armed = true;
    lc_set_msg("LC: saving");
    LC_BRIDGE_DBG("autosave scheduled");
}

static int lc_has_setup(void)
{
    int i;

    for (i = 0; i < g_leancam_ui.prog.count; ++i)
    {
        if (lc_line_command_is(g_leancam_ui.prog.lines[i], "SETUP"))
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
        if (leancam_ui_begin_template(&g_leancam_ui, lc_template_setup()))
        {
            lc_editor_reset();
            g_lc_mode = LC_MODE_DRAFT;
            lc_set_msg("LC: setup first");
        }
        return;
    }

    if (leancam_ui_begin_template(&g_leancam_ui, tmpl))
    {
        lc_editor_reset();
        g_lc_mode = LC_MODE_DRAFT;
        lc_set_msg("LC: edit draft");
    }
}

static void lc_catalog_new_entry(void)
{
    const char *tmpl = lc_template_catalog((lc_menu_catalog_kind_t)g_catalog_kind);

    if (!tmpl)
        return;
    if (leancam_ui_begin_template(&g_leancam_ui, tmpl))
    {
        lc_editor_reset();
        g_lc_mode = LC_MODE_DRAFT;
        lc_set_msg("LC: catalog draft");
    }
}

static void lc_catalog_duplicate_entry(void)
{
    if (g_catalog_kind == LC_CATALOG_NONE)
        return;
    if (g_leancam_ui.cur_line < 0 || g_leancam_ui.cur_line >= g_leancam_ui.prog.count)
    {
        lc_set_msg("LC: no entry");
        return;
    }
    if (prog_insert_after(&g_leancam_ui.prog, g_leancam_ui.cur_line, g_leancam_ui.prog.lines[g_leancam_ui.cur_line]))
    {
        g_leancam_ui.cur_line++;
        lc_schedule_autosave();
        lc_set_msg("LC: entry copied");
    }
}

static void lc_open_catalog(lc_catalog_kind_t kind)
{
    if (leancam_files_busy())
    {
        lc_set_msg("LC: file IO busy");
        return;
    }

    switch (kind)
    {
        case LC_CATALOG_TOOLS: break;
        default: return;
    }

    leancam_ui_new(&g_leancam_ui);
    lc_presets_clear_meta();

    if (kind == LC_CATALOG_TOOLS)
    {
        lc_tool_catalog_load_from_storage(LC_TOOLS_FILE);
        lc_tool_catalog_copy_to_program(&g_leancam_ui.prog);
    }

    if (g_leancam_ui.prog.count > 0 && g_leancam_ui.cur_line < 0)
        g_leancam_ui.cur_line = 0;

    g_catalog_kind = kind;
    g_prog_scroll = 0;
    g_lc_mode = LC_MODE_PROGRAM;
    lc_set_msg("LC: tools");
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
        lc_line_command_is(g_leancam_ui.prog.lines[0], "SETUP"))
    {
        lc_set_msg("LC: setup locked");
        return;
    }

    leancam_ui_delete_line(&g_leancam_ui);
    lc_schedule_autosave();
}

static bool lc_close_active_region_with_g80(void)
{
    int start;
    int end;

    if (leancam_files_busy())
    {
        lc_set_msg("LC: file IO busy");
        return true;
    }

    if (g_leancam_ui.draft_active ||
        g_catalog_kind != LC_CATALOG_NONE ||
        g_leancam_ui.cur_line < 0 ||
        g_leancam_ui.cur_line >= g_leancam_ui.prog.count)
        return false;

    if (!lc_find_raw_region(&g_leancam_ui.prog, g_leancam_ui.cur_line, &start, &end))
        return false;
    if (end >= start && end < g_leancam_ui.prog.count &&
        lc_region_is_end(g_leancam_ui.prog.lines[end]))
        return false;

    if (!prog_insert_after(&g_leancam_ui.prog, end, "G80"))
    {
        lc_set_msg("LC: no room for G80");
        return true;
    }

    g_leancam_ui.cur_line = end + 1;
    lc_ensure_program_visible();
    lc_set_msg("LC: G80 inserted");
    lc_schedule_autosave();
    leancam_bridge_request_render();
    return true;
}

static void lc_new_file_finish(void)
{
    char path[LC_FILE_PATH_MAX];

    if (leancam_files_busy())
    {
        lc_set_msg("LC: file IO busy");
        return;
    }

    if (lc_file_prompt_name_empty())
    {
        lc_set_msg("LC: empty name");
        return;
    }

    if (!leancam_files_make_new_path(LC_FILES_DIR, lc_file_prompt_name(), path, sizeof(path)))
    {
        lc_set_msg("LC: path failed");
        return;
    }

    leancam_ui_new(&g_leancam_ui);
    lc_presets_clear_meta();
    strncpy(g_leancam_ui.current_path, path, sizeof(g_leancam_ui.current_path) - 1);
    g_leancam_ui.current_path[sizeof(g_leancam_ui.current_path) - 1] = 0;

    g_prog_scroll = 0;

    if (leancam_ui_begin_template(&g_leancam_ui, lc_template_setup()))
    {
        lc_editor_reset();
        g_lc_mode = LC_MODE_DRAFT;
        lc_set_msg("LC: new file, setup first");
    }
    else
    {
        g_lc_mode = LC_MODE_PROGRAM;
        lc_set_msg("LC: new file");
    }
}

static void lc_duplicate_file_finish(void)
{
    static program_t copy_prog;
    char path[LC_FILE_PATH_MAX];
    const char *source_path;

    if (leancam_files_busy())
    {
        lc_set_msg("LC: file IO busy");
        return;
    }

    source_path = lc_file_prompt_duplicate_source();
    if (!source_path[0])
    {
        lc_file_prompt_finish_duplicate();
        lc_set_msg("LC: copy source lost");
        return;
    }

    if (lc_file_prompt_name_empty())
    {
        lc_set_msg("LC: empty name");
        return;
    }

    if (!leancam_files_make_new_path(LC_FILES_DIR, lc_file_prompt_name(), path, sizeof(path)))
    {
        lc_set_msg("LC: path failed");
        return;
    }

    if (strcmp(path, source_path) == 0)
    {
        lc_set_msg("LC: same name");
        return;
    }

    if (!leancam_files_load(source_path, &copy_prog))
    {
        lc_set_msg("LC: copy load failed");
        return;
    }

    if (!leancam_files_save(path, &copy_prog))
    {
        lc_set_msg("LC: copy save failed");
        return;
    }

    lc_file_prompt_finish_duplicate();
    lc_refresh_files();
    g_lc_mode = LC_MODE_FILES;
    lc_set_msg("LC: file copied");
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

    if (!lc_file_browser_selected_path(LC_FILES_DIR, path, sizeof(path)))
    {
        lc_set_msg("LC: build path failed");
        return;
    }

    name = lc_file_browser_selected_name();
    if (!lc_path_has_suffix_ci(name, ".nc"))
    {
        lc_set_msg("LC: unsupported file");
        return;
    }

    if (!leancam_ui_load(&g_leancam_ui, path))
    {
        lc_set_msg("LC: load failed");
        return;
    }
    lc_presets_clear_meta();

    g_prog_scroll = 0;
    g_lc_mode = LC_MODE_PROGRAM;
    lc_set_msg("");
}

static void lc_ensure_program_visible(void)
{
    int visible_lines = (g_catalog_kind == LC_CATALOG_TOOLS) ? LC_VISIBLE_TOOL_LINES : LC_VISIBLE_PROGRAM_LINES;

    if (g_leancam_ui.cur_line < 0)
    {
        g_prog_scroll = 0;
        return;
    }

    if (g_prog_scroll < 0)
        g_prog_scroll = 0;

    if (g_leancam_ui.cur_line < g_prog_scroll)
        g_prog_scroll = g_leancam_ui.cur_line;

    if (g_leancam_ui.cur_line >= g_prog_scroll + visible_lines)
        g_prog_scroll = g_leancam_ui.cur_line - visible_lines + 1;

    if (g_prog_scroll < 0)
        g_prog_scroll = 0;
}

void leancam_bridge_init(void)
{
    lc_app_init();
    lc_autosave_init();
    lc_run_init();
    leancam_menu_set_program_other_templates(false);
    leancam_files_init();
    leancam_ui_init(&g_leancam_ui);

    lc_file_prompt_clear();
    g_prog_scroll = 0;
    lc_nc_viewer_init();
    lc_file_browser_init();
    g_catalog_kind = LC_CATALOG_NONE;
    lc_tool_catalog_init();
    lc_set_msg("LC: init");

#ifdef LEANCAM_RP2350_STANDALONE_DEMO
#ifdef ENABLE_SD_CARD_V2
    g_lc_mode = LC_MODE_FILES;
    lc_file_browser_mark_waiting(mcu_millis() + 250u);
    lc_set_msg("LC: waiting for SD");
#else
    prog_add(&g_leancam_ui.prog,
             "SETUP L{75} OD{50} ID{0} CLAMP{8} EXTRA{3} CLR{1}");
    prog_add(&g_leancam_ui.prog,
             "TOOL T1 R0.8 ORIENT3 R_FEED120 FIN_FEED60 DOC2.0 FIN_DOC0.5 RPM800 XOFF0 ZOFF0");
    prog_add(&g_leancam_ui.prog,
             "G71 U2 R1 X0.5 Z0.5 F120");
    prog_add(&g_leancam_ui.prog,
             "G1 X50 Z0");
    prog_add(&g_leancam_ui.prog,
             "G1 X32 Z-35 C1 R0");
    prog_add(&g_leancam_ui.prog,
             "G1 X50 Z-35");
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

    now = mcu_millis();
    if (lc_autosave_due(now) && !g_leancam_ui.draft_active)
    {
        lc_autosave();
    }

    lc_serial_debug_gcode_tick(now);

    if (g_lc_mode != LC_MODE_FILES || lc_file_browser_ready() || leancam_files_busy())
        return;

    if (!lc_file_browser_should_retry(now))
        return;

    lc_refresh_files();
}


int leancam_bridge_wants_key_menu(void)
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

static void lc_run_selected_line_stream(const program_t *prog, int start, int end)
{
    uint32_t line_no = 10u;
    lc_gcode_result_t r = LC_GCODE_OK;
    char err[64];
    int err_line = start + 1;
    int made = 0;

    err[0] = 0;

    if (!cam_stream_begin())
    {
        lc_set_msg("LC: stream begin fail");
        return;
    }

    if (!leancam_gcode_emit_program_header(lc_cam_emit_gcode_line, &line_no))
        r = LC_GCODE_STREAM_REJECT;

    if (r == LC_GCODE_OK)
        r = lc_run_emit_selected_range(prog, start, end, lc_cam_emit_gcode_line, &line_no,
                                       err, sizeof(err), &err_line, &made);

    if (r == LC_GCODE_OK && !leancam_gcode_emit_program_footer_ex(lc_cam_emit_gcode_line, &line_no, err, sizeof(err)))
        r = LC_GCODE_STREAM_REJECT;

    cam_stream_finish();

    if (r == LC_GCODE_OK)
        lc_set_msgf("LC: stream L%d-L%d sent", start + 1, end + 1);
    else
        lc_set_msgf("LC: L%d %.32s", err_line, err[0] ? err : lc_run_gcode_result_name(r));
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

static void lc_run_selected_line_direct_stream(const program_t *prog, int start, int end)
{
    lc_direct_stream_emit_t emit_ctx;
    lc_gcode_result_t r = LC_GCODE_OK;
    char err[64];
    int err_line = start + 1;
    int made = 0;

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

    if (r == LC_GCODE_OK)
        r = lc_run_emit_selected_range(prog, start, end, lc_direct_stream_append, &emit_ctx,
                                       err, sizeof(err), &err_line, &made);

    if (r == LC_GCODE_OK && !leancam_gcode_emit_program_footer_ex(lc_direct_stream_append, &emit_ctx, err, sizeof(err)))
        r = LC_GCODE_STREAM_REJECT;

    if (r != LC_GCODE_OK)
    {
        lc_direct_stream_reset();
        lc_set_msgf("LC: L%d %.32s", err_line,
                    emit_ctx.overflow ? "stream overflow" : (err[0] ? err : lc_run_gcode_result_name(r)));
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
    lc_set_msgf("LC: queued L%d-L%d %lu lines", start + 1, end + 1, (unsigned long)emit_ctx.lines);
}
#endif

static void lc_run_selected_line(void)
{
    char out_path[LC_FILE_PATH_MAX];
    const char *line;
    fs_file_t *fp;
    lc_gcode_file_emit_t emit_ctx;
    int run_line_no;
    int run_start;
    int run_end;
    int err_line;
    int made;
    lc_gcode_result_t r;
    char err[64];

    if (g_leancam_ui.cur_line < 0 || g_leancam_ui.cur_line >= g_leancam_ui.prog.count)
    {
        lc_set_msg("LC: no line");
        return;
    }

    line = g_leancam_ui.prog.lines[g_leancam_ui.cur_line];
    run_line_no = g_leancam_ui.cur_line + 1;
    run_start = g_leancam_ui.cur_line;
    run_end = g_leancam_ui.cur_line;

    if (lc_find_raw_region(&g_leancam_ui.prog, g_leancam_ui.cur_line, &run_start, &run_end))
        lc_debug_dump_nc_range(&g_leancam_ui.prog, run_start, run_end, "selected-run");

    if (!lc_find_setup_in_program(&g_leancam_ui.prog, run_start))
    {
        lc_set_msg("LC: no setup");
        return;
    }

    err[0] = 0;
    err_line = run_start + 1;
    made = 0;
    r = leancam_gcode_emit_program_header(lc_gcode_discard_line, NULL) ? LC_GCODE_OK : LC_GCODE_STREAM_REJECT;
    if (r == LC_GCODE_OK)
        r = lc_run_emit_selected_range(&g_leancam_ui.prog, run_start, run_end, lc_gcode_discard_line, NULL,
                                       err, sizeof(err), &err_line, &made);
    if (r == LC_GCODE_OK && !leancam_gcode_emit_program_footer_ex(lc_gcode_discard_line, NULL, err, sizeof(err)))
        r = LC_GCODE_STREAM_REJECT;
    if (r != LC_GCODE_OK)
    {
        lc_set_msgf("LC: L%d %.32s", err_line, err[0] ? err : lc_run_gcode_result_name(r));
        return;
    }

#if LC_SELECTED_RUN_USE_STREAM
    lc_run_selected_line_stream(&g_leancam_ui.prog, run_start, run_end);
    return;
#endif

#ifdef LEANCAM_RP2350_DIRECT_STREAM
    lc_run_selected_line_direct_stream(&g_leancam_ui.prog, run_start, run_end);
    return;
#endif

    if (!lc_path_make_lrun(LC_FILES_DIR, g_leancam_ui.current_path, line, run_line_no, out_path, sizeof(out_path)))
    {
        lc_set_msg("LC: lrun path failed");
        return;
    }

    lc_publish_msg("LC: writing %s", lc_path_basename(out_path));

    if (!lc_resource_file_begin("selected-run"))
    {
        lc_set_msg("LC: file IO busy");
        leancam_bridge_request_render();
        return;
    }
    fp = fs_open(out_path, "w");
    if (!fp)
    {
        lc_resource_file_end();
        lc_set_msg("LC: lrun open failed");
        leancam_bridge_request_render();
        return;
    }

    emit_ctx.fp = fp;
    r = LC_GCODE_OK;

    if (!leancam_gcode_emit_program_header(lc_write_line_to_file, &emit_ctx))
        r = LC_GCODE_STREAM_REJECT;

    if (r == LC_GCODE_OK)
        r = lc_run_emit_selected_range(&g_leancam_ui.prog, run_start, run_end, lc_write_line_to_file, &emit_ctx,
                                       err, sizeof(err), &err_line, &made);

    if (r == LC_GCODE_OK && !leancam_gcode_emit_program_footer_ex(lc_write_line_to_file, &emit_ctx, err, sizeof(err)))
        r = LC_GCODE_STREAM_REJECT;

    fs_close(fp);
    lc_resource_file_end();

    if (r == LC_GCODE_OK)
    {
        lc_publish_msg("LC: running %s", lc_path_basename(out_path));
        fs_file_run(out_path);
    }
    else
    {
        lc_set_msgf("LC: L%d %.32s", err_line, err[0] ? err : lc_run_gcode_result_name(r));
    }
}


static void lc_draft_log_state(const char *event)
{
#if LC_DRAFT_EDIT_SERIAL_DEBUG
    char field[24];
    uint8_t count = lc_count_brace_fields(g_leancam_ui.draft_line);

    lc_editor_field_name(&g_leancam_ui, g_draft_field_index, field, sizeof(field));
    LC_DRAFT_DBG("%s idx=%u/%u field=%s input=%.24s line=%.72s",
                 event ? event : "state",
                 (unsigned)g_draft_field_index,
                 (unsigned)count,
                 field,
                 g_leancam_ui.input_buf,
                 g_leancam_ui.draft_line);
#else
    (void)event;
#endif
}


static void lc_build_draft_preview_line(char *out, uint32_t out_len)
{
    lc_editor_build_preview_line(&g_leancam_ui, out, out_len);
}

static int lc_accept_active_field_bridge_owned(void)
{
    int draft_index;
    const char *setup = NULL;
    const char *tool = NULL;
    char preview_line[MAX_LEN];
    char err[40];

    if (!g_leancam_ui.draft_active) return 0;

    lc_draft_log_state("accept begin");

    if (g_leancam_ui.draft_replace_index >= 0)
        draft_index = g_leancam_ui.draft_replace_index;
    else
        draft_index = g_leancam_ui.draft_insert_after + 1;
    (void)lc_setup_line(&setup);
    lc_build_draft_preview_line(preview_line, sizeof(preview_line));
    tool = lc_effective_tool_for_draft(&g_leancam_ui.prog, draft_index, g_leancam_ui.draft_line, preview_line);

    if (!lc_editor_accept_active_field(&g_leancam_ui, setup, tool, err, sizeof(err)))
    {
        if (err[0])
            lc_set_msgf("LC: %.32s", err);
        return 0;
    }

    lc_draft_log_state("accept next");
    return 1;
}

static bool lc_find_raw_region(const program_t *prog, int index, int *start_out, int *end_out)
{
    return lc_region_find(prog, index, start_out, end_out);
}

static void lc_debug_dump_nc_range(const program_t *prog, int start, int end, const char *why)
{
    int i;

    if (!prog || start < 0 || end < start || end >= prog->count)
        return;

    LC_BRIDGE_DBG("nc range %s L%d..L%d",
                  why ? why : "run",
                  start + 1,
                  end + 1);
    for (i = start; i <= end; ++i)
        LC_BRIDGE_DBG("nc L%d %.96s", i + 1, prog->lines[i]);
}

static int lc_commit_draft_bridge_owned(void)
{
    bool inserting_new_line;
    LC_BRIDGE_DBG("commit begin draft=%u field=%u line=%.64s",
                  g_leancam_ui.draft_active ? 1u : 0u,
                  (unsigned)g_draft_field_index,
                  g_leancam_ui.draft_line);
    inserting_new_line = g_leancam_ui.draft_active && g_leancam_ui.draft_replace_index < 0;

    if (g_leancam_ui.draft_active)
    {
        const char *setup = NULL;
        const char *tool = NULL;
        int draft_line_index = g_leancam_ui.draft_replace_index >= 0 ?
                               g_leancam_ui.draft_replace_index :
                               g_leancam_ui.draft_insert_after + 1;
        char preview_line[MAX_LEN];

        lc_build_draft_preview_line(preview_line, sizeof(preview_line));
        (void)lc_setup_line(&setup);
        tool = lc_effective_tool_for_draft(&g_leancam_ui.prog, draft_line_index, g_leancam_ui.draft_line, preview_line);

        if (!lc_editor_prepare_draft_for_commit(&g_leancam_ui, setup, tool))
        {
            lc_set_msg("LC: unresolved field");
            LC_BRIDGE_DBG("commit resolve fail");
            return 0;
        }
    }

    if (leancam_ui_commit_draft(&g_leancam_ui))
    {
        if (inserting_new_line &&
            g_leancam_ui.cur_line >= 0 &&
            g_leancam_ui.cur_line < g_leancam_ui.prog.count)
        {
            (void)lc_expand_committed_preset(g_leancam_ui.cur_line);
        }

        LC_BRIDGE_DBG("commit ui ok cur=%d count=%d line=%.64s",
                      g_leancam_ui.cur_line,
                      g_leancam_ui.prog.count,
                      (g_leancam_ui.cur_line >= 0 && g_leancam_ui.cur_line < g_leancam_ui.prog.count) ?
                      g_leancam_ui.prog.lines[g_leancam_ui.cur_line] : "");
        lc_editor_reset();
        g_lc_mode = LC_MODE_PROGRAM;
        lc_run_sim_set_armed(false);
        g_sim_arm_block_until_ms = mcu_millis() + 250u;
        lc_ensure_program_visible();
        lc_set_msg("LC: committed");
        lc_schedule_autosave();
        leancam_bridge_request_render();
        return 1;
    }

    lc_set_msg("LC: unresolved field");
    LC_BRIDGE_DBG("commit ui failed");
    return 0;
}

static lc_menu_mode_t lc_menu_cb_get_mode(void *user)
{
    (void)user;
    return (lc_menu_mode_t)g_lc_mode;
}

static lc_menu_catalog_kind_t lc_menu_cb_get_catalog(void *user)
{
    (void)user;
    return (lc_menu_catalog_kind_t)g_catalog_kind;
}

static void lc_menu_cb_set_mode(void *user, lc_menu_mode_t mode)
{
    (void)user;
    if (mode != LC_MENU_MODE_FILE_NAME)
    {
        lc_file_prompt_clear();
    }
    g_lc_mode = (lc_mode_t)mode;
}

static void lc_menu_cb_set_catalog(void *user, lc_menu_catalog_kind_t catalog)
{
    (void)user;
    g_catalog_kind = (lc_catalog_kind_t)catalog;
}

static void lc_menu_cb_set_message(void *user, const char *message)
{
    (void)user;
    lc_set_msg(message);
}

static int lc_menu_cb_file_count(void *user)
{
    (void)user;
    return leancam_files_count();
}

static int lc_menu_cb_file_selected(void *user)
{
    (void)user;
    return lc_file_browser_selected();
}

static void lc_menu_cb_file_set_selected(void *user, int selected)
{
    (void)user;
    lc_file_browser_set_selected(selected);
}

static void lc_menu_cb_files_refresh(void *user)
{
    (void)user;
    lc_refresh_files();
}

static void lc_menu_cb_files_delete_selected(void *user)
{
    (void)user;
    lc_delete_selected_file();
}

static void lc_menu_cb_files_duplicate_selected(void *user)
{
    (void)user;
    lc_duplicate_selected_file_begin();
}

static void lc_menu_cb_files_prepare_run(void *user)
{
    (void)user;
    lc_prepare_selected_file_for_run();
}

static void lc_menu_cb_files_open_selected(void *user)
{
    (void)user;
    lc_open_selected_file();
}

static void lc_menu_cb_filename_clear(void *user)
{
    (void)user;
    lc_file_prompt_clear();
}

static size_t lc_menu_cb_filename_len(void *user)
{
    (void)user;
    return lc_file_prompt_name_len();
}

static void lc_menu_cb_filename_backspace(void *user)
{
    (void)user;
    lc_file_prompt_backspace();
}

static void lc_menu_cb_filename_append_digit(void *user, char digit)
{
    (void)user;
    lc_file_prompt_append_digit(digit);
}

static void lc_menu_cb_catalog_open(void *user, lc_menu_catalog_kind_t catalog)
{
    (void)user;
    lc_open_catalog((lc_catalog_kind_t)catalog);
}

static void lc_menu_cb_filename_finish(void *user)
{
    (void)user;
    if (lc_file_prompt_duplicate_pending())
        lc_duplicate_file_finish();
    else
        lc_new_file_finish();
}

static void lc_menu_cb_catalog_new_entry(void *user)
{
    (void)user;
    lc_catalog_new_entry();
}

static void lc_menu_cb_catalog_duplicate_entry(void *user)
{
    (void)user;
    lc_catalog_duplicate_entry();
}

static void lc_menu_cb_program_move_prev(void *user)
{
    (void)user;
    lc_run_sim_set_armed(false);
    leancam_ui_move_up(&g_leancam_ui);
    lc_ensure_program_visible();
}

static void lc_menu_cb_program_move_next(void *user)
{
    (void)user;
    lc_run_sim_set_armed(false);
    leancam_ui_move_down(&g_leancam_ui);
    lc_ensure_program_visible();
}

static bool lc_menu_cb_program_begin_edit(void *user, bool asset)
{
    (void)user;
    lc_run_sim_set_armed(false);
    if (!leancam_ui_begin_edit_current(&g_leancam_ui))
        return false;

    lc_editor_reset();
    g_lc_mode = LC_MODE_DRAFT;
    lc_set_msg(asset ? "LC: edit asset" : "LC: edit line");
    return true;
}

static void lc_menu_cb_program_run_selected(void *user)
{
    uint32_t now;

    (void)user;
    now = mcu_millis();
    if (!lc_run_sim_armed() &&
        g_sim_arm_block_until_ms != 0 &&
        (int32_t)(now - g_sim_arm_block_until_ms) < 0)
    {
        lc_set_msg("LC: preview settling");
        leancam_bridge_request_render();
        return;
    }
    g_sim_arm_block_until_ms = 0;

    if (!lc_run_sim_armed() && lc_close_active_region_with_g80())
        return;

    if (!lc_run_sim_armed())
    {
        int run_start = g_leancam_ui.cur_line;
        int run_end = g_leancam_ui.cur_line;

        if (g_leancam_ui.cur_line >= 0 &&
            g_leancam_ui.cur_line < g_leancam_ui.prog.count &&
            lc_find_raw_region(&g_leancam_ui.prog, g_leancam_ui.cur_line, &run_start, &run_end)) {
            lc_serial_debug_begin_selected_gcode(run_start, run_end);
        }
        lc_run_sim_set_armed(true);
        lc_set_msg("LC: sim preview");
        leancam_bridge_request_render();
        return;
    }
    lc_run_sim_set_armed(false);
    lc_run_selected_line();
}

static void lc_menu_cb_program_delete_current(void *user)
{
    (void)user;
    lc_run_sim_set_armed(false);
    lc_delete_current_line();
    lc_ensure_program_visible();
}

static void lc_menu_cb_program_back_to_files(void *user)
{
    (void)user;
    if (lc_run_sim_armed())
    {
        lc_run_sim_set_armed(false);
        lc_set_msg("LC: sim closed");
        leancam_bridge_request_render();
        return;
    }
    lc_refresh_files();
    g_lc_mode = LC_MODE_FILES;
    g_catalog_kind = LC_CATALOG_NONE;
    leancam_menu_set_program_other_templates(false);
    lc_set_msg("LC: files");
}

static void lc_menu_cb_program_begin_template(void *user, lc_menu_template_t tmpl)
{
    lc_template_selection_t selection;

    (void)user;
    lc_run_sim_set_armed(false);
    selection = lc_template_select(tmpl);
    if (!selection.text)
        return;

    if (selection.action == LC_TEMPLATE_ACTION_PRESET)
        lc_insert_process_preset_template(selection.text);
    else if (selection.action == LC_TEMPLATE_ACTION_DRAFT)
        lc_begin_setup_if_missing_then(selection.text);
}

static unsigned lc_menu_cb_draft_field_count(void *user)
{
    (void)user;
    return (unsigned)lc_count_brace_fields(g_leancam_ui.draft_line);
}

static unsigned lc_menu_cb_draft_field_index(void *user)
{
    (void)user;
    return (unsigned)g_draft_field_index;
}

static bool lc_menu_cb_draft_has_input(void *user)
{
    (void)user;
    return g_leancam_ui.input_buf[0] != 0;
}

static void lc_menu_cb_draft_cancel(void *user)
{
    (void)user;
    leancam_ui_cancel_draft(&g_leancam_ui);
    lc_editor_reset();
    g_lc_mode = LC_MODE_PROGRAM;
    lc_set_msg("LC: draft cancel");
}

static bool lc_menu_cb_draft_accept_field(void *user)
{
    bool accepted;

    (void)user;
    lc_draft_log_state("key D next");
    accepted = lc_accept_active_field_bridge_owned() != 0;
    if (accepted) {
        g_leancam_ui.dirty = true;
        leancam_bridge_request_render();
    } else {
        LC_DRAFT_DBG("key D next rejected");
    }
    return accepted;
}

static bool lc_menu_cb_draft_commit(void *user)
{
    (void)user;
    return lc_commit_draft_bridge_owned() != 0;
}

static void lc_menu_cb_draft_backspace(void *user)
{
    (void)user;
    lc_draft_log_state("key * backspace before");
    leancam_ui_backspace(&g_leancam_ui);
    lc_draft_log_state("key * backspace after");
    g_leancam_ui.dirty = true;
    leancam_bridge_request_render();
}

static void lc_menu_cb_draft_toggle_sign(void *user)
{
    (void)user;
    lc_draft_log_state("key B sign before");
    lc_editor_toggle_sign(&g_leancam_ui);
    lc_draft_log_state("key B sign after");
    g_leancam_ui.dirty = true;
    leancam_bridge_request_render();
}

static void lc_menu_cb_draft_add_dot(void *user)
{
    (void)user;
    lc_draft_log_state("key C dot before");
    lc_editor_add_dot(&g_leancam_ui);
    lc_draft_log_state("key C dot after");
    g_leancam_ui.dirty = true;
    leancam_bridge_request_render();
}

static void lc_menu_cb_draft_input_digit(void *user, char digit)
{
    (void)user;
    LC_DRAFT_DBG("key %c digit before idx=%u input=%.24s",
                 digit,
                 (unsigned)g_draft_field_index,
                 g_leancam_ui.input_buf);
    lc_editor_input_digit(&g_leancam_ui, digit);
    lc_draft_log_state("digit after");
    g_leancam_ui.dirty = true;
    leancam_bridge_request_render();
}

static void lc_menu_cb_nc_scroll_prev(void *user)
{
    (void)user;
    lc_nc_viewer_scroll_prev();
}

static void lc_menu_cb_nc_scroll_next(void *user)
{
    (void)user;
    lc_nc_viewer_scroll_next();
}

static const lc_menu_actions_t g_lc_menu_actions = {
    .get_mode = lc_menu_cb_get_mode,
    .get_catalog = lc_menu_cb_get_catalog,
    .set_mode = lc_menu_cb_set_mode,
    .set_catalog = lc_menu_cb_set_catalog,
    .set_message = lc_menu_cb_set_message,

    .file_count = lc_menu_cb_file_count,
    .file_selected = lc_menu_cb_file_selected,
    .file_set_selected = lc_menu_cb_file_set_selected,
    .files_refresh = lc_menu_cb_files_refresh,
    .files_delete_selected = lc_menu_cb_files_delete_selected,
    .files_duplicate_selected = lc_menu_cb_files_duplicate_selected,
    .files_prepare_run = lc_menu_cb_files_prepare_run,
    .files_open_selected = lc_menu_cb_files_open_selected,
    .filename_clear = lc_menu_cb_filename_clear,
    .filename_len = lc_menu_cb_filename_len,
    .filename_backspace = lc_menu_cb_filename_backspace,
    .filename_append_digit = lc_menu_cb_filename_append_digit,
    .filename_finish = lc_menu_cb_filename_finish,

    .catalog_open = lc_menu_cb_catalog_open,
    .catalog_new_entry = lc_menu_cb_catalog_new_entry,
    .catalog_duplicate_entry = lc_menu_cb_catalog_duplicate_entry,

    .program_move_prev = lc_menu_cb_program_move_prev,
    .program_move_next = lc_menu_cb_program_move_next,
    .program_begin_edit = lc_menu_cb_program_begin_edit,
    .program_delete_current = lc_menu_cb_program_delete_current,
    .program_run_selected = lc_menu_cb_program_run_selected,
    .program_begin_template = lc_menu_cb_program_begin_template,
    .program_back_to_files = lc_menu_cb_program_back_to_files,

    .draft_field_count = lc_menu_cb_draft_field_count,
    .draft_field_index = lc_menu_cb_draft_field_index,
    .draft_has_input = lc_menu_cb_draft_has_input,
    .draft_cancel = lc_menu_cb_draft_cancel,
    .draft_accept_field = lc_menu_cb_draft_accept_field,
    .draft_commit = lc_menu_cb_draft_commit,
    .draft_backspace = lc_menu_cb_draft_backspace,
    .draft_toggle_sign = lc_menu_cb_draft_toggle_sign,
    .draft_add_dot = lc_menu_cb_draft_add_dot,
    .draft_input_digit = lc_menu_cb_draft_input_digit,

    .nc_back_to_files = lc_menu_cb_program_back_to_files,
    .nc_scroll_prev = lc_menu_cb_nc_scroll_prev,
    .nc_scroll_next = lc_menu_cb_nc_scroll_next,
};

void leancam_bridge_handle_key(ui_key_t key)
{
    (void)leancam_menu_handle_key(NULL, &g_lc_menu_actions, key);
}


static int lc_setup_line(const char **line_out)
{
    int i;

    if (line_out) *line_out = NULL;

    for (i = 0; i < g_leancam_ui.prog.count; ++i)
    {
        if (lc_line_command_is(g_leancam_ui.prog.lines[i], "SETUP"))
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
            char preview_line[MAX_LEN];
            char validate_line[MAX_LEN];
            uint8_t hi_start = 0;
            uint8_t hi_end = 0;

            (void)lc_setup_line(&setup);
            lc_build_draft_preview_line(preview_line, sizeof(preview_line));
            tool = lc_effective_tool_for_draft(&g_leancam_ui.prog, draft_vidx, g_leancam_ui.draft_line, preview_line);
            leancam_expr_build_draft_display(buf,
                                             sizeof(buf),
                                             g_leancam_ui.draft_line,
                                             g_leancam_ui.input_buf,
                                             g_draft_field_index,
                                             setup,
                                             tool,
                                             g_leancam_ui.draft_line,
                                             &hi_start,
                                             &hi_end);
            if (!lc_editor_resolve_draft_line_for_commit(&g_leancam_ui, setup, tool, validate_line, sizeof(validate_line)))
                strncpy(validate_line, preview_line, sizeof(validate_line) - 1u);
            validate_line[sizeof(validate_line) - 1u] = 0;
            if (lc_line_is_thread(validate_line))
            {
                float start_lane = 0.0f;
                float stop_lane = 0.0f;

                if (lc_calc_thread_lanes(validate_line, tool,
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

            lc_snapshot_put_line_ex(f,
                                    row++,
                                    buf,
                                    g_draft_field_index < lc_count_brace_fields(g_leancam_ui.draft_line),
                                    hi_start,
                                    hi_end);
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
                char display_line[UI_LC_LINE_LEN];

                ui_snapshot_strcpy(display_line, g_leancam_ui.prog.lines[pi], sizeof(display_line));

                if (lc_line_is_thread(g_leancam_ui.prog.lines[pi]))
                {
                    const char *tool = lc_effective_tool_for_cycle(&g_leancam_ui.prog, pi, g_leancam_ui.prog.lines[pi]);
                    float start_lane = 0.0f;
                    float stop_lane = 0.0f;

                    if (lc_calc_thread_lanes(display_line, tool,
                                             &start_lane, &stop_lane, NULL, NULL, NULL))
                    {
                        char line_buf[UI_LC_LINE_LEN];
                        snprintf(line_buf, sizeof(line_buf), "%.56s | ELS %.2f/%.2f",
                                 display_line, start_lane, stop_lane);
                        row = lc_snapshot_put_program_command(f,
                                                              row,
                                                              pi + 1,
                                                              lc_line_display_indent(&g_leancam_ui.prog, pi, g_leancam_ui.prog.lines[pi]),
                                                              line_buf,
                                                              (pi == g_leancam_ui.cur_line) && (!g_leancam_ui.draft_active));
                        continue;
                    }
                    else
                    {
                        row = lc_snapshot_put_program_command(f,
                                                              row,
                                                              pi + 1,
                                                              lc_line_display_indent(&g_leancam_ui.prog, pi, g_leancam_ui.prog.lines[pi]),
                                                              display_line,
                                                              (pi == g_leancam_ui.cur_line) && (!g_leancam_ui.draft_active));
                        continue;
                    }
                }
                else
                {
                    row = lc_snapshot_put_program_command(f,
                                                          row,
                                                          pi + 1,
                                                          lc_line_display_indent(&g_leancam_ui.prog, pi, g_leancam_ui.prog.lines[pi]),
                                                          display_line,
                                                          (pi == g_leancam_ui.cur_line) && (!g_leancam_ui.draft_active));
                    continue;
                }
            }
        }
    }

    leancam_menu_copy_footer(f->leancam_helper,
                             sizeof(f->leancam_helper),
                             (lc_menu_mode_t)g_lc_mode,
                             (lc_menu_catalog_kind_t)g_catalog_kind,
                             g_leancam_ui.draft_active,
                             (unsigned)g_draft_field_index,
                             (unsigned)lc_count_brace_fields(g_leancam_ui.draft_line),
                             g_leancam_ui.input_buf);
}

static void lc_snapshot_nc_view(ui_snapshot_frame_t *f)
{
    int row = 0;
    int i;
    char buf[UI_LC_LINE_LEN];
    int top_line = lc_nc_viewer_top_line();
    uint8_t line_count = lc_nc_viewer_line_count();
    uint8_t selected_row = lc_nc_viewer_selected_row();

    snprintf(buf, sizeof(buf), "FILE: %s  top:%d", lc_path_basename(lc_nc_viewer_path()), top_line + 1);
    lc_snapshot_put_line(f, row++, buf, false);

    for (i = 0; i < line_count && row < UI_LC_MAX_LINES; ++i)
    {
        snprintf(buf, sizeof(buf), "%04d %.80s", top_line + i + 1, lc_nc_viewer_line((uint8_t)i));
        lc_snapshot_put_line(f, row++, buf, (i == selected_row));
    }

    if (line_count == 0 && row < UI_LC_MAX_LINES)
        lc_snapshot_put_line(f, row++, "<empty nc file>", false);

    leancam_menu_copy_footer(f->leancam_helper,
                             sizeof(f->leancam_helper),
                             (lc_menu_mode_t)g_lc_mode,
                             (lc_menu_catalog_kind_t)g_catalog_kind,
                             false,
                             0,
                             0,
                             NULL);
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
        setup = NULL;
        line = NULL;
        tool = NULL;
    }
    else if (g_lc_mode == LC_MODE_NC_VIEW)
    {
        setup = lc_nc_viewer_setup_line()[0] ? lc_nc_viewer_setup_line() : NULL;
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
        if (lc_line_command_is(g_leancam_ui.draft_line, "TOOL"))
            tool = preview_line;
        lc_editor_field_name_from_line(g_leancam_ui.draft_line,
                                       g_draft_field_index,
                                       f->leancam_active_field,
                                       sizeof(f->leancam_active_field));
        ui_snapshot_strcpy(f->leancam_preview_line, line, sizeof(f->leancam_preview_line));
    }
    else if (g_leancam_ui.cur_line >= 0 && g_leancam_ui.cur_line < g_leancam_ui.prog.count)
    {
        line = g_leancam_ui.prog.lines[g_leancam_ui.cur_line];
    }

    if (!tool && g_lc_mode != LC_MODE_FILES && g_lc_mode != LC_MODE_NC_VIEW)
        tool = lc_effective_tool_for_cycle(&g_leancam_ui.prog, before_or_at, line);

    ui_snapshot_strcpy(f->leancam_setup_line, setup ? setup : "", sizeof(f->leancam_setup_line));
    if (!g_leancam_ui.draft_active)
        ui_snapshot_strcpy(f->leancam_preview_line, line ? line : "", sizeof(f->leancam_preview_line));
    ui_snapshot_strcpy(f->leancam_tool_line, tool ? tool : "", sizeof(f->leancam_tool_line));
}

static void lc_snapshot_raw_preview_region(ui_snapshot_frame_t *f)
{
    int row;
    int start;
    int end;
    int i;
    int out = 0;

    if (!f || g_lc_mode == LC_MODE_FILES || g_lc_mode == LC_MODE_NC_VIEW)
        return;

    row = g_leancam_ui.cur_line;
    if (g_leancam_ui.draft_active)
    {
        if (g_leancam_ui.draft_replace_index >= 0)
            row = g_leancam_ui.draft_replace_index;
        else
            row = g_leancam_ui.draft_insert_after + 1;
    }

    if (row < 0 || row >= g_leancam_ui.prog.count)
        return;

    if (!lc_find_raw_region(&g_leancam_ui.prog, row, &start, &end) || end < start)
        return;

    for (i = start; i <= end && out < UI_LC_PREVIEW_REGION_MAX; ++i)
    {
        const char *line = g_leancam_ui.prog.lines[i];

        if (!lc_region_is_header(line) &&
            !lc_region_is_contour(line) &&
            !lc_region_is_end(line))
            continue;

        ui_snapshot_strcpy(f->leancam_preview_region[out],
                           line,
                           sizeof(f->leancam_preview_region[out]));
        f->leancam_preview_region_selected[out] = (uint8_t)(i == row ? 1 : 0);
        out++;
    }

    f->leancam_preview_region_count = (uint8_t)out;
}

void leancam_bridge_fill_snapshot(ui_snapshot_frame_t *f)
{
    int i;
    int row = 0;
    char buf[UI_LC_LINE_LEN];
    char status[UI_LC_LINE_LEN];

    if (!f)
        return;

    lc_snapshot_reset_frame(f);
    f->leancam_mode = (uint8_t)g_lc_mode;
    f->leancam_show_menu = (bool)leancam_bridge_wants_key_menu();

    ui_snapshot_strcpy(f->leancam_message, g_last_msg, sizeof(f->leancam_message));
    leancam_menu_copy_title(f->leancam_title,
                            sizeof(f->leancam_title),
                            (lc_menu_mode_t)g_lc_mode,
                            (lc_menu_catalog_kind_t)g_catalog_kind);
    if ((g_lc_mode == LC_MODE_PROGRAM || g_lc_mode == LC_MODE_DRAFT) &&
        g_catalog_kind == LC_CATALOG_NONE &&
        g_leancam_ui.current_path[0])
    {
        snprintf(f->leancam_title,
                 sizeof(f->leancam_title),
                 "%s",
                 lc_path_basename(g_leancam_ui.current_path));
    }

    switch (g_lc_mode)
    {
        case LC_MODE_FILES:
        {
            int cnt = leancam_files_count();

            lc_snapshot_put_line(f, row++, "Storage: " LC_FILES_DIR, false);
            if (!lc_file_browser_ready() && row < UI_LC_MAX_LINES)
                lc_snapshot_put_line(f, row++, "Waiting for SD card...", false);

            for (i = 0; i < cnt && row < UI_LC_MAX_LINES; ++i)
            {
                snprintf(buf, sizeof(buf), "%s", leancam_files_name(i));
                lc_snapshot_put_line(f, row++, buf, (i == lc_file_browser_selected()));
            }

            if (row < UI_LC_MAX_LINES)
                lc_snapshot_put_line(f, row++, "NEW...", (lc_file_browser_selected() == cnt));

            leancam_menu_copy_footer(f->leancam_helper,
                                     sizeof(f->leancam_helper),
                                     (lc_menu_mode_t)g_lc_mode,
                                     (lc_menu_catalog_kind_t)g_catalog_kind,
                                     false,
                                     0,
                                     0,
                                     NULL);
            break;
        }

        case LC_MODE_FILE_NAME:
            snprintf(buf, sizeof(buf), "NAME{%s}", lc_file_prompt_name());
            lc_snapshot_put_line(f, row++, buf, true);
            leancam_menu_copy_footer(f->leancam_helper,
                                     sizeof(f->leancam_helper),
                                     (lc_menu_mode_t)g_lc_mode,
                                     (lc_menu_catalog_kind_t)g_catalog_kind,
                                     false,
                                     0,
                                     0,
                                     NULL);
            break;

        case LC_MODE_PROGRAM:
            lc_snapshot_program(f);
            break;

        case LC_MODE_DRAFT:
            lc_snapshot_program(f);
            break;

        case LC_MODE_NC_VIEW:
            lc_snapshot_nc_view(f);
            break;

        default:
            break;
    }

    lc_snapshot_preview_sources(f);
    lc_snapshot_raw_preview_region(f);
    f->leancam_fullscreen_sim = g_line_sim_armed &&
                                !g_leancam_ui.draft_active &&
                                g_lc_mode == LC_MODE_PROGRAM;
    if (f->leancam_fullscreen_sim) {
        f->leancam_sim_preview_active = g_serial_gcode_dump.active;
        f->leancam_sim_preview_seq = g_serial_gcode_dump.draw_seq;
        f->leancam_sim_preview_index = g_serial_gcode_dump.current_index;
        f->leancam_sim_preview_count = g_serial_gcode_dump.count;
        ui_snapshot_strcpy(f->leancam_sim_preview_line,
                           g_serial_gcode_dump.current_line,
                           sizeof(f->leancam_sim_preview_line));
    }
    if (lc_debug_snapshot_should_print(f->leancam_preview_line)) {
        uint8_t dbg_i;
        LC_BRIDGE_DBG("snapshot mode=%u draft=%u cur=%d count=%d lines=%u preview=%.64s tool=%.48s",
                      (unsigned)g_lc_mode,
                      g_leancam_ui.draft_active ? 1u : 0u,
                      g_leancam_ui.cur_line,
                      g_leancam_ui.prog.count,
                      (unsigned)f->leancam_line_count,
                      f->leancam_preview_line,
                      f->leancam_tool_line);
        LC_BRIDGE_DBG("preview region count=%u",
                      (unsigned)f->leancam_preview_region_count);
        for (dbg_i = 0; dbg_i < f->leancam_preview_region_count && dbg_i < UI_LC_PREVIEW_REGION_MAX; ++dbg_i) {
            LC_BRIDGE_DBG("preview region %u sel=%u %.96s",
                          (unsigned)dbg_i,
                          (unsigned)f->leancam_preview_region_selected[dbg_i],
                          f->leancam_preview_region[dbg_i]);
        }
        lc_debug_print_r_corner_geometry(f);
        for (dbg_i = 0; dbg_i < f->leancam_line_count && dbg_i < UI_LC_MAX_LINES; ++dbg_i) {
            LC_BRIDGE_DBG("display row %u sel=%u hi=%u..%u %.96s",
                          (unsigned)dbg_i,
                          (unsigned)f->leancam_line_selected[dbg_i],
                          (unsigned)f->leancam_field_hi_start[dbg_i],
                          (unsigned)f->leancam_field_hi_end[dbg_i],
                          f->leancam_lines[dbg_i]);
        }
        g_debug_snapshot_armed = false;
    }

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
