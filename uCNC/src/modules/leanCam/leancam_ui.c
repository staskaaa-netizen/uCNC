#include "leancam_ui.h"
#include "leancam_files.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

static bool line_get_field_value(const char *line, const char *key, char *out, int out_sz)
{
    char pat[80];
    const char *p;
    const char *a;
    const char *b;
    int len;

    snprintf(pat, sizeof(pat), "%s{", key);
    p = strstr(line, pat);
    if (!p) return false;

    a = strchr(p, '{');
    b = strchr(p, '}');
    if (!a || !b || b <= a) return false;

    ++a;
    len = (int)(b - a);
    if (len >= out_sz) len = out_sz - 1;

    memcpy(out, a, len);
    out[len] = 0;
    return true;
}

static const char *find_last_setup_line_before(const program_t *p, int after_idx)
{
    int i;
    for (i = after_idx; i >= 0; --i) {
        if (strncmp(p->lines[i], "SETUP|", 6) == 0)
            return p->lines[i];
    }
    return NULL;
}

static void resolve_setup_refs_in_line(char *line, const program_t *p, int after_idx)
{
    const char *setup;
    int s, e;
    int pos = 0;
    char key[64];
    char value[64];

    setup = find_last_setup_line_before(p, after_idx);
    if (!setup) return;

    while (find_field(line, pos, &s, &e)) {
        int len = e - s;
        if (len > 8 && strncmp(line + s, "(SETUP.", 7) == 0) {
            int klen = len - 8;
            if (klen > 0 && klen < (int)sizeof(key)) {
                memcpy(key, line + s + 7, klen);
                key[klen] = 0;
                if (line_get_field_value(setup, key, value, sizeof(value))) {
                    set_field(line, s, e, value);
                    pos = 0;
                    continue;
                }
            }
        }
        pos = e;
    }
}

static bool find_next_any_field(const char *line, int from, int *s, int *e)
{
    return find_field(line, from, s, e);
}

static void draft_refresh_target(leancam_ui_t *ui)
{
    if (!ui->draft_active) {
        ui->draft_fs = ui->draft_fe = -1;
        return;
    }

    if (find_first_required_or_unresolved(ui->draft_line, &ui->draft_fs, &ui->draft_fe))
        return;

    if (find_field(ui->draft_line, 0, &ui->draft_fs, &ui->draft_fe))
        return;

    ui->draft_fs = ui->draft_fe = -1;
    ui->draft_replace_index = -1;
}

static void draft_move_next(leancam_ui_t *ui)
{
    int ns, ne;

    if (find_next_required_or_unresolved(ui->draft_line, ui->draft_fe, &ns, &ne)) {
        ui->draft_fs = ns;
        ui->draft_fe = ne;
        return;
    }

    if (find_next_any_field(ui->draft_line, ui->draft_fe, &ns, &ne)) {
        ui->draft_fs = ns;
        ui->draft_fe = ne;
        return;
    }

    leancam_ui_commit_draft(ui);
}

void leancam_ui_init(leancam_ui_t *ui)
{
    memset(ui, 0, sizeof(*ui));
    prog_init(&ui->prog);
    ui->cur_line = -1;
    ui->draft_fs = ui->draft_fe = -1;
}

void leancam_ui_new(leancam_ui_t *ui)
{
    leancam_ui_init(ui);
}

bool leancam_ui_begin_template(leancam_ui_t *ui, const char *tmpl)
{
    if (!tmpl) return false;

    ui->draft_active = true;
    strncpy(ui->draft_line, tmpl, MAX_LEN - 1);
    ui->draft_line[MAX_LEN - 1] = 0;
    ui->input_buf[0] = 0;
    ui->draft_insert_after = ui->cur_line;
    ui->draft_replace_index = -1;

    resolve_setup_refs_in_line(ui->draft_line, &ui->prog, ui->draft_insert_after);
    draft_refresh_target(ui);
    return true;
}

bool leancam_ui_begin_edit_current(leancam_ui_t *ui)
{
    if (!ui) return false;
    if (ui->draft_active) return false;
    if (ui->cur_line < 0 || ui->cur_line >= ui->prog.count) return false;

    ui->draft_active = true;
    strncpy(ui->draft_line, ui->prog.lines[ui->cur_line], MAX_LEN - 1);
    ui->draft_line[MAX_LEN - 1] = 0;
    ui->input_buf[0] = 0;

    /* Commit will replace this line in-place, not insert a new one. */
    ui->draft_replace_index = ui->cur_line;
    ui->draft_insert_after = ui->cur_line - 1;

    draft_refresh_target(ui);
    return true;
}

bool leancam_ui_accept_field(leancam_ui_t *ui)
{
    if (!ui->draft_active) return false;
    if (ui->draft_fs < 0 || ui->draft_fe <= ui->draft_fs) return false;

    if (ui->input_buf[0] != 0)
        set_field(ui->draft_line, ui->draft_fs, ui->draft_fe, ui->input_buf);

    ui->input_buf[0] = 0;
    draft_move_next(ui);
    ui->dirty = true;
    return true;
}

void leancam_ui_cancel_draft(leancam_ui_t *ui)
{
    ui->draft_active = false;
    ui->draft_line[0] = 0;
    ui->input_buf[0] = 0;
    ui->draft_fs = ui->draft_fe = -1;
    ui->draft_insert_after = -1;
    ui->draft_replace_index = -1;
}

/*bool leancam_ui_commit_draft(leancam_ui_t *ui)
{
    if (!ui->draft_active) return false;
    if (line_has_unresolved_required(ui->draft_line)) return false;

    if (!prog_insert_after(&ui->prog, ui->draft_insert_after, ui->draft_line))
        return false;

    ui->cur_line = ui->draft_insert_after + 1;
    leancam_ui_cancel_draft(ui);
    ui->dirty = true;
    return true;
}*/

static void lc_format_saved_float(char *dst, size_t dst_sz, float v)
{
    char *p;

    if (!dst || dst_sz == 0) return;

    snprintf(dst, dst_sz, "%.4f", v);

    p = dst + strlen(dst) - 1;
    while (p > dst && *p == '0') {
        *p-- = '\0';
    }
    if (p > dst && *p == '.') {
        *p = '\0';
    }
}

static bool lc_line_get_float_local(const char *line, const char *field, float *out)
{
    const char *p;
    const char *b;
    const char *e;
    char key[48];
    char val[48];
    size_t key_len;
    size_t val_len;

    if (!line || !field || !out) return false;

    p = line;
    while ((p = strchr(p, '|')) != NULL) {
        p++;

        b = strchr(p, '{');
        if (!b) continue;

        key_len = (size_t)(b - p);
        if (key_len >= sizeof(key)) key_len = sizeof(key) - 1;
        memcpy(key, p, key_len);
        key[key_len] = '\0';

        if (strcmp(key, field) != 0) continue;

        e = strchr(b, '}');
        if (!e) return false;

        val_len = (size_t)(e - b - 1);
        if (val_len >= sizeof(val)) val_len = sizeof(val) - 1;
        memcpy(val, b + 1, val_len);
        val[val_len] = '\0';

        if (val[0] == '(') return false;

        *out = strtof(val, NULL);
        return true;
    }

    return false;
}

static void lc_resolve_line_for_save(const char *in, char *out, size_t out_sz)
{
    const char *p;
    char *w;

    if (!in || !out || out_sz == 0) return;

    p = in;
    w = out;

    while (*p && (size_t)(w - out) < out_sz - 1) {
        if (p[0] == '{' && p[1] == '(') {
            const char *end = strstr(p, ")}");
            char expr[64];
            char num[32];
            size_t expr_len;
            float v = 0.0f;
            bool ok = false;

            if (!end) break;

            expr_len = (size_t)(end - (p + 2));
            if (expr_len >= sizeof(expr)) expr_len = sizeof(expr) - 1;
            memcpy(expr, p + 2, expr_len);
            expr[expr_len] = '\0';

            if (strncmp(expr, "THIS.", 5) == 0) {
                ok = lc_line_get_float_local(in, expr + 5, &v);
            } else if (strncmp(expr, "SETUP.", 6) == 0) {
                /*
                 * Minimal safe fallback for current setup-derived defaults.
                 * If you already have a real setup lookup helper, replace this
                 * block with that.
                 */
                ok = lc_line_get_float_local(in, expr + 6, &v);
            } else {
                v = strtof(expr, NULL);
                ok = true;
            }

            if (ok) {
                int n;

                lc_format_saved_float(num, sizeof(num), v);
                n = snprintf(w, out_sz - (size_t)(w - out), "{%s}", num);
                if (n < 0) break;
                w += n;
            } else {
                /* unresolved optional default: store empty rather than formula */
                if ((size_t)(w - out) + 2 >= out_sz) break;
                *w++ = '{';
                *w++ = '}';
            }

            p = end + 2;
            continue;
        }

        *w++ = *p++;
    }

    *w = '\0';
}

bool leancam_ui_commit_draft(leancam_ui_t *ui)
{
    char saved[MAX_LEN];

    if (!ui->draft_active) return false;
    if (line_has_unresolved_required(ui->draft_line)) return false;

    lc_resolve_line_for_save(ui->draft_line, saved, sizeof(saved));

    if (ui->draft_replace_index >= 0) {
        int idx = ui->draft_replace_index;
        if (idx < 0 || idx >= ui->prog.count) return false;
        strncpy(ui->prog.lines[idx], saved, MAX_LEN - 1);
        ui->prog.lines[idx][MAX_LEN - 1] = 0;
        ui->cur_line = idx;
    } else {
        if (!prog_insert_after(&ui->prog, ui->draft_insert_after, saved))
            return false;

        ui->cur_line = ui->draft_insert_after + 1;
    }
    leancam_ui_cancel_draft(ui);
    ui->dirty = true;
    return true;
}

void leancam_ui_input_char(leancam_ui_t *ui, int ch)
{
    size_t len;

    if (!ui->draft_active) return;
    if (!(isalnum(ch) || ch == '.' || ch == '-' || ch == '_' || ch == '+'))
        return;

    len = strlen(ui->input_buf);
    if (len + 1 < sizeof(ui->input_buf)) {
        ui->input_buf[len] = (char)ch;
        ui->input_buf[len + 1] = 0;
    }
}

void leancam_ui_backspace(leancam_ui_t *ui)
{
    size_t len;
    if (!ui->draft_active) return;

    len = strlen(ui->input_buf);
    if (len > 0)
        ui->input_buf[len - 1] = 0;
}


static bool lc_find_field_before(const char *line, int before, int *out_s, int *out_e)
{
    int pos = 0;
    int s = -1;
    int e = -1;
    int last_s = -1;
    int last_e = -1;

    if (!line || !out_s || !out_e) return false;
    if (before < 0) before = 0;

    while (find_field(line, pos, &s, &e)) {
        if (s >= before) break;
        last_s = s;
        last_e = e;
        pos = e;
    }

    if (last_s < 0 || last_e <= last_s) return false;

    *out_s = last_s;
    *out_e = last_e;
    return true;
}

void leancam_ui_move_field_prev(leancam_ui_t *ui)
{
    int s, e;

    if (!ui || !ui->draft_active) return;

    if (ui->draft_fs < 0 || ui->draft_fe <= ui->draft_fs) {
        draft_refresh_target(ui);
        return;
    }

    if (lc_find_field_before(ui->draft_line, ui->draft_fs, &s, &e)) {
        ui->draft_fs = s;
        ui->draft_fe = e;
        ui->input_buf[0] = 0;
    }
}

void leancam_ui_move_field_next(leancam_ui_t *ui)
{
    int s, e;

    if (!ui || !ui->draft_active) return;

    if (ui->draft_fs < 0 || ui->draft_fe <= ui->draft_fs) {
        draft_refresh_target(ui);
        return;
    }

    if (find_next_any_field(ui->draft_line, ui->draft_fe, &s, &e)) {
        ui->draft_fs = s;
        ui->draft_fe = e;
        ui->input_buf[0] = 0;
    }
}

void leancam_ui_move_up(leancam_ui_t *ui)
{
    if (ui->draft_active) return;
    if (ui->cur_line > 0) ui->cur_line--;
}

void leancam_ui_move_down(leancam_ui_t *ui)
{
    if (ui->draft_active) return;
    if (ui->cur_line < ui->prog.count - 1) ui->cur_line++;
}

void leancam_ui_delete_line(leancam_ui_t *ui)
{
    if (ui->draft_active) return;
    if (ui->cur_line < 0 || ui->cur_line >= ui->prog.count) return;

    if (ui->cur_line == 0 && strncmp(ui->prog.lines[0], "SETUP|", 6) == 0)
        return;

    prog_delete(&ui->prog, ui->cur_line);
    if (ui->prog.count == 0) ui->cur_line = -1;
    else if (ui->cur_line >= ui->prog.count) ui->cur_line = ui->prog.count - 1;
    ui->dirty = true;
}

bool leancam_ui_save(leancam_ui_t *ui, const char *path)
{
    if (!path || !path[0]) return false;
    //uint8_t old = cnc_enter_file_io_safe_phase();

    if (!leancam_files_save(path, &ui->prog)) return false;
   // cnc_leave_file_io_safe_phase(old);
    //cnc_clear_exec_state(0x80);

    strncpy(ui->current_path, path, sizeof(ui->current_path) - 1);
    ui->current_path[sizeof(ui->current_path) - 1] = 0;
    ui->dirty = false;
    return true;
}

bool leancam_ui_load(leancam_ui_t *ui, const char *path)
{
    if (!path || !path[0]) return false;
    //uint8_t old = cnc_enter_file_io_safe_phase();
  
    if (!leancam_files_load(path, &ui->prog)) return false;
    //cnc_leave_file_io_safe_phase(old);
    //cnc_clear_exec_state(0x80);

    strncpy(ui->current_path, path, sizeof(ui->current_path) - 1);
    ui->current_path[sizeof(ui->current_path) - 1] = 0;
    ui->cur_line = (ui->prog.count > 0) ? 0 : -1;
    leancam_ui_cancel_draft(ui);
    ui->dirty = false;
    return true;
}

void leancam_get_module_name(const char *line, char *out, int out_sz)
{
    int i = 0;
    if (!line || !out || out_sz <= 0) return;

    while (line[i] && line[i] != '|' && i < out_sz - 1) {
        out[i] = (char)tolower((unsigned char)line[i]);
        ++i;
    }
    out[i] = 0;
}