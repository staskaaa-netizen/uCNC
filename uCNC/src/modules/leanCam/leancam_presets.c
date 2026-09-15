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

bool lc_presets_insert_region(program_t *prog,
                              int insert_after,
                              lc_preset_meta_kind_t kind,
                              const char *preset,
                              const char *setup_line,
                              int *target_row,
                              char *err,
                              size_t err_sz)
{
    char raw[MAX_LEN];
    char start[MAX_LEN];
    char target[MAX_LEN];
    char close[MAX_LEN];
    char end[MAX_LEN] = "G80";
    char u[24] = "0";
    char w[24] = "0";
    char r[24] = "0";
    char x_allow[24] = "0";
    char z_allow[24] = "0";
    char f_r[24] = "0";
    char stock_x[24] = "0";
    int insert_count;
    int cur_line;

    if (target_row)
        *target_row = insert_after;
    if (err && err_sz)
        err[0] = 0;
    if (!prog || !preset || kind == LC_PRESET_META_NONE)
        return false;

    (void)lc_code_get_field_text(preset, "U", u, sizeof(u));
    (void)lc_code_get_field_text(preset, "W", w, sizeof(w));
    (void)lc_code_get_field_text(preset, "R", r, sizeof(r));
    (void)lc_code_get_field_text(preset, "X", x_allow, sizeof(x_allow));
    (void)lc_code_get_field_text(preset, "Z", z_allow, sizeof(z_allow));
    (void)lc_code_get_field_text(preset, "F_R", f_r, sizeof(f_r));
    if (kind == LC_PRESET_META_ID)
    {
        if (!lc_preset_setup_positive(setup_line, "I", stock_x, sizeof(stock_x)))
        {
            lc_preset_set_err(err, err_sz, "LC: ID needs G971 I > 0");
            return false;
        }
    }
    else
    {
        if (!lc_preset_setup_positive(setup_line, "X", stock_x, sizeof(stock_x)))
        {
            lc_preset_set_err(err, err_sz, "LC: G971 X needed");
            return false;
        }
    }

    insert_count = (kind == LC_PRESET_META_RECESS) ? 3 : 5;
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

    cur_line = insert_after;
    if (!prog_insert_after(prog, cur_line, raw))
        return false;
    cur_line++;

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



