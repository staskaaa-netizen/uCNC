/* LeanCam module contract:
 * Purpose: expand operator preset buttons into visible G-code-ish program rows.
 * Called by: leancam_bridge/template menu actions.
 * Calls into: template/schema/expression helpers and program insertion helpers.
 * Owns: no persistent state; presets are edit-time conveniences, not stored private cycle commands.
 */
#include "leancam_presets.h"
#include "leancam_code.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static lc_preset_meta_t g_preset_meta[MAX_LINES];

static bool lc_preset_setup_positive(const char *setup_line,
                                     const char *key,
                                     char *out,
                                     size_t out_sz)
{
    float v;

    if (!lc_code_get_field_text(setup_line, key, out, out_sz))
        return false;
    v = strtof(out, NULL);
    return v > 0.0f;
}

static void lc_preset_set_err(char *err, size_t err_sz, const char *msg)
{
    if (!err || err_sz == 0)
        return;
    snprintf(err, err_sz, "%s", msg ? msg : "");
}

void lc_presets_clear_meta(void)
{
    memset(g_preset_meta, 0, sizeof(g_preset_meta));
}

lc_preset_meta_kind_t lc_presets_kind_from_line(const char *line)
{
    if (lc_code_command_is(line, "OD")) return LC_PRESET_META_OD;
    if (lc_code_command_is(line, "ID")) return LC_PRESET_META_ID;
    if (lc_code_command_is(line, "FACE")) return LC_PRESET_META_FACE;
    if (lc_code_command_is(line, "RECESS")) return LC_PRESET_META_RECESS;
    return LC_PRESET_META_NONE;
}

lc_preset_meta_kind_t lc_presets_region_kind(const program_t *prog, int header_idx)
{
    lc_preset_meta_kind_t kind;

    if (!prog || header_idx < 0 || header_idx >= prog->count)
        return LC_PRESET_META_NONE;
    if (!lc_code_region_is_header(prog->lines[header_idx]))
        return LC_PRESET_META_NONE;

    if (header_idx < MAX_LINES)
    {
        kind = g_preset_meta[header_idx].kind;
        if (kind != LC_PRESET_META_NONE)
            return kind;
    }

    if (lc_code_command_is(prog->lines[header_idx], "G72"))
        return LC_PRESET_META_FACE;

    return LC_PRESET_META_OD;
}

bool lc_presets_expand_committed(program_t *prog,
                                 int row,
                                 const char *setup_line,
                                 int *target_row,
                                 char *err,
                                 size_t err_sz)
{
    lc_preset_meta_kind_t kind;
    char preset[MAX_LEN];
    char raw[MAX_LEN];
    char start[MAX_LEN];
    char target[MAX_LEN];
    char close[MAX_LEN];
    char end[MAX_LEN] = "G80";
    char t[24] = "0";
    char o[24] = "0";
    char u[24] = "0";
    char w[24] = "0";
    char r[24] = "0";
    char x_allow[24] = "0";
    char z_allow[24] = "0";
    char f_r[24] = "0";
    char f_f[24] = "0";
    char rpm[24] = "0";
    char stock_x[24] = "0";
    int insert_count;
    int cur_line;
    float vf;

    if (target_row)
        *target_row = row;
    if (err && err_sz)
        err[0] = 0;
    if (!prog || row < 0 || row >= prog->count)
        return false;

    strncpy(preset, prog->lines[row], sizeof(preset) - 1u);
    preset[sizeof(preset) - 1u] = 0;

    kind = lc_presets_kind_from_line(preset);
    if (kind == LC_PRESET_META_NONE)
        return false;

    (void)lc_code_get_field_text(preset, "T", t, sizeof(t));
    (void)lc_code_get_field_text(preset, "O", o, sizeof(o));
    (void)lc_code_get_field_text(preset, "U", u, sizeof(u));
    (void)lc_code_get_field_text(preset, "W", w, sizeof(w));
    (void)lc_code_get_field_text(preset, "R", r, sizeof(r));
    (void)lc_code_get_field_text(preset, "X", x_allow, sizeof(x_allow));
    (void)lc_code_get_field_text(preset, "Z", z_allow, sizeof(z_allow));
    (void)lc_code_get_field_text(preset, "F_R", f_r, sizeof(f_r));
    (void)lc_code_get_field_text(preset, "F_F", f_f, sizeof(f_f));
    (void)lc_code_get_field_text(preset, "RPM", rpm, sizeof(rpm));

    if (kind == LC_PRESET_META_ID)
    {
        if (!lc_preset_setup_positive(setup_line, "ID", stock_x, sizeof(stock_x)))
        {
            lc_preset_set_err(err, err_sz, "LC: ID needs SETUP.ID > 0");
            return false;
        }
    }
    else
    {
        if (!lc_preset_setup_positive(setup_line, "OD", stock_x, sizeof(stock_x)))
        {
            lc_preset_set_err(err, err_sz, "LC: SETUP.OD needed");
            return false;
        }
    }

    insert_count = (kind == LC_PRESET_META_RECESS) ? 2 : 4;
    if (prog->count + insert_count > MAX_LINES)
    {
        lc_preset_set_err(err, err_sz, "LC: no room for preset");
        return false;
    }

    if (kind == LC_PRESET_META_FACE)
        snprintf(raw, sizeof(raw), "G72 W%.10s R%.10s X%.10s Z%.10s F%.10s",
                 w, r, x_allow, z_allow, f_r);
    else
        snprintf(raw, sizeof(raw), "G71 U%.10s R%.10s X%.10s Z%.10s F%.10s",
                 u, r, x_allow, z_allow, f_r);

    strncpy(prog->lines[row], raw, MAX_LEN - 1);
    prog->lines[row][MAX_LEN - 1] = 0;

    if (row < MAX_LINES)
    {
        lc_preset_meta_t *meta = &g_preset_meta[row];
        memset(meta, 0, sizeof(*meta));
        meta->kind = kind;
        if (lc_code_get_field_float(preset, "T", &vf)) meta->t = (int)vf;
        if (lc_code_get_field_float(preset, "O", &vf)) meta->o = (int)vf;
        if (lc_code_get_field_float(preset, "F_F", &vf)) meta->finish_feed = vf;
        if (lc_code_get_field_float(preset, "RPM", &vf)) meta->rpm = (int)vf;
    }

    cur_line = row;
    if (kind == LC_PRESET_META_RECESS)
    {
        snprintf(target, sizeof(target), "G1 X{} Z{} C{0} R{0}");
        if (!prog_insert_after(prog, cur_line, target))
            return false;
        cur_line++;
        if (!prog_insert_after(prog, cur_line, end))
            return false;
        cur_line++;
    }
    else
    {
        snprintf(start, sizeof(start), "G1 X%s Z0", stock_x);
        snprintf(target, sizeof(target), "G1 X{} Z{} C{0} R{0}");
        snprintf(close, sizeof(close), "G1 X%s Z{}", stock_x);
        if (!prog_insert_after(prog, cur_line, start))
            return false;
        cur_line++;
        if (!prog_insert_after(prog, cur_line, target))
            return false;
        cur_line++;
        if (target_row)
            *target_row = cur_line;
        if (!prog_insert_after(prog, cur_line, close))
            return false;
        cur_line++;
        if (!prog_insert_after(prog, cur_line, end))
            return false;
        cur_line++;
        return true;
    }

    if (target_row)
        *target_row = cur_line;
    return true;
}



