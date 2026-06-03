/* LeanCam module contract:
 * Purpose: field-level text editing for one active program row.
 * Called by: leancam_bridge when a row is opened for editing or keypad text input arrives.
 * Calls into: text/schema helpers for field parsing; it should not perform file I/O, rendering, or execution.
 * Owns: active edit cursor/field state only.
 */
#include "leancam_editor.h"
#include "leancam_code.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_lc_editor_field_index = 0;

static bool lc_editor_token_value_span(const char *tok,
                                       const char *end,
                                       const char **value_start,
                                       const char **value_end)
{
    const char *p = tok;

    if (value_start) *value_start = NULL;
    if (value_end) *value_end = NULL;
    if (!tok || !end || tok >= end)
        return false;

    while (p < end &&
           ((*p >= 'A' && *p <= 'Z') ||
            (*p >= 'a' && *p <= 'z') ||
            *p == '_'))
        p++;
    if (p == tok)
        return false;
    if (p < end && *p == '=')
        p++;
    if (p >= end)
        return false;

    if (value_start) *value_start = p;
    if (value_end) *value_end = end;
    return true;
}

static bool lc_editor_token_named_value_span(const char *tok,
                                             const char *end,
                                             const char *key,
                                             const char **value_start,
                                             const char **value_end)
{
    const char *p;
    size_t key_len;

    if (value_start) *value_start = NULL;
    if (value_end) *value_end = NULL;
    if (!tok || !end || !key || tok >= end)
        return false;

    key_len = strlen(key);
    if (key_len == 0u || (size_t)(end - tok) <= key_len ||
        strncmp(tok, key, key_len) != 0)
        return false;

    p = tok + key_len;
    if (p < end && *p == '=')
        p++;
    else if ((*p >= 'A' && *p <= 'Z') ||
             (*p >= 'a' && *p <= 'z') ||
             *p == '_')
        return false;
    if (p >= end)
        return false;

    if (value_start) *value_start = p;
    if (value_end) *value_end = end;
    return true;
}

static bool lc_editor_plain_field_span(const char *line,
                                       uint8_t field_index,
                                       const char **value_start,
                                       const char **value_end)
{
    const char *p = line;
    uint8_t idx = 0;

    if (value_start) *value_start = NULL;
    if (value_end) *value_end = NULL;
    if (!line)
        return false;

    while (*p == ' ' || *p == '\t')
        p++;
    while (*p && *p != ' ' && *p != '\t')
        p++;

    while (*p)
    {
        const char *tok;
        const char *end;
        const char *vs;
        const char *ve;

        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        tok = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        end = p;

        if (!lc_editor_token_value_span(tok, end, &vs, &ve))
            continue;
        if (idx == field_index)
        {
            if (value_start) *value_start = vs;
            if (value_end) *value_end = ve;
            return true;
        }
        if (idx < 250u)
            idx++;
    }

    return false;
}

static bool lc_editor_active_field_is_negative(const leancam_ui_t *ui)
{
    const char *open;
    const char *close;
    const char *p;

    if (!ui || !ui->draft_active)
        return false;

    if (!lc_editor_find_field(ui->draft_line, g_lc_editor_field_index, &open, &close))
        return false;

    p = (*open == '{') ? open + 1 : open;
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

void lc_editor_reset(void)
{
    g_lc_editor_field_index = 0;
}

uint8_t lc_editor_field_index(void)
{
    return g_lc_editor_field_index;
}

void lc_editor_set_field_index(uint8_t index)
{
    g_lc_editor_field_index = index;
}

uint8_t lc_editor_field_count(const char *line)
{
    uint8_t n = 0;
    const char *p = line;
    const char *q;

    if (line && !strchr(line, '{'))
    {
        const char *s;
        const char *e;

        while (n < 250u && lc_editor_plain_field_span(line, n, &s, &e))
            n++;
        return n;
    }

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

bool lc_editor_find_field(const char *line, uint8_t field_index, const char **open_out, const char **close_out)
{
    const char *scan;
    uint8_t idx = 0;

    if (open_out) *open_out = NULL;
    if (close_out) *close_out = NULL;
    if (!line) return false;

    if (!strchr(line, '{'))
        return lc_editor_plain_field_span(line, field_index, open_out, close_out);

    scan = line;
    while (*scan)
    {
        const char *o = strchr(scan, '{');
        const char *c = o ? strchr(o + 1, '}') : NULL;
        if (!o || !c) return false;
        if (idx == field_index)
        {
            if (open_out) *open_out = o;
            if (close_out) *close_out = c;
            return true;
        }
        idx++;
        scan = c + 1;
    }
    return false;
}

bool lc_editor_replace_span(char *line, uint32_t line_len, const char *span_start, const char *span_end_exclusive, const char *replacement)
{
    char tmp[MAX_LEN];
    uint32_t prefix_len;

    if (!line || line_len == 0 || !span_start || !span_end_exclusive || span_end_exclusive < span_start)
        return false;
    if (!replacement)
        replacement = "";

    prefix_len = (uint32_t)(span_start - line);
    if (prefix_len >= sizeof(tmp))
        prefix_len = sizeof(tmp) - 1u;
    if (prefix_len)
        memcpy(tmp, line, prefix_len);
    tmp[prefix_len] = 0;
    strncat(tmp, replacement, sizeof(tmp) - strlen(tmp) - 1u);
    strncat(tmp, span_end_exclusive, sizeof(tmp) - strlen(tmp) - 1u);
    strncpy(line, tmp, line_len - 1u);
    line[line_len - 1u] = 0;
    return true;
}

static bool lc_editor_field_value_span(const char *line,
                                       const char *key,
                                       const char **open_out,
                                       const char **close_out)
{
    char pat[24];
    const char *p;
    const char *b;
    const char *e;

    if (open_out) *open_out = NULL;
    if (close_out) *close_out = NULL;
    if (!line || !key)
        return false;

    if (!strchr(line, '{'))
    {
        const char *scan = line;

        while (*scan == ' ' || *scan == '\t')
            scan++;
        while (*scan && *scan != ' ' && *scan != '\t')
            scan++;
        while (*scan)
        {
            const char *tok;
            const char *end;
            const char *value;

            while (*scan == ' ' || *scan == '\t')
                scan++;
            if (!*scan)
                break;
            tok = scan;
            while (*scan && *scan != ' ' && *scan != '\t')
                scan++;
            end = scan;

            if (!lc_editor_token_named_value_span(tok, end, key, &value, NULL))
                continue;
            if (open_out) *open_out = value - 1;
            if (close_out) *close_out = end;
            return true;
        }
        return false;
    }

    snprintf(pat, sizeof(pat), " %s{", key);
    p = strstr(line, pat);
    if (!p && strncmp(line, key, strlen(key)) == 0 && line[strlen(key)] == '{')
        p = line - 1;
    if (!p)
    {
        size_t key_len = strlen(key);

        p = line;
        while (*p)
        {
            const char *tok;
            const char *end;

            while (*p == ' ' || *p == '\t')
                p++;
            tok = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
            end = p;

            if ((size_t)(end - tok) > key_len &&
                strncmp(tok, key, key_len) == 0 &&
                tok[key_len] == '{')
            {
                b = tok + key_len;
                e = memchr(b + 1, '}', (size_t)(end - b - 1));
                if (!e || e < b + 1)
                    return false;
                if (open_out) *open_out = b;
                if (close_out) *close_out = e;
                return true;
            }
        }
        return false;
    }

    b = strchr(p + 1, '{');
    e = b ? strchr(b + 1, '}') : NULL;
    if (!b || !e || e < b + 1)
        return false;

    if (open_out) *open_out = b;
    if (close_out) *close_out = e;
    return true;
}

bool lc_editor_line_get_field_text(const char *line, const char *key, char *out, size_t out_sz)
{
    const char *open;
    const char *close;
    size_t n;

    if (!out || out_sz == 0)
        return false;
    out[0] = 0;
    if (!line || !key)
        return false;
    if (!lc_editor_field_value_span(line, key, &open, &close))
        return false;

    n = (size_t)(close - open - 1);
    if (n >= out_sz)
        n = out_sz - 1;
    memcpy(out, open + 1, n);
    out[n] = 0;
    return true;
}

bool lc_editor_line_set_field_text(char *line, uint32_t line_len, const char *key, const char *value)
{
    const char *open;
    const char *close;

    if (!line || !key || !value)
        return false;
    if (!lc_editor_field_value_span(line, key, &open, &close))
        return false;

    return lc_editor_replace_span(line, line_len, open + 1, close, value);
}

void lc_editor_normalize_source_edit(char *line, uint32_t line_len, const char *field_name)
{
    char value[24];
    float v;

    if (!line || !field_name || !lc_code_command_is(line, "G1"))
        return;

    if (strcmp(field_name, "C") == 0 &&
        lc_editor_line_get_field_text(line, "C", value, sizeof(value)))
    {
        v = strtof(value, NULL);
        if (v > 0.0f)
            (void)lc_editor_line_set_field_text(line, line_len, "R", "0");
    }
    else if (strcmp(field_name, "R") == 0 &&
             lc_editor_line_get_field_text(line, "R", value, sizeof(value)))
    {
        v = strtof(value, NULL);
        if (v > 0.0f)
            (void)lc_editor_line_set_field_text(line, line_len, "C", "0");
    }
}

void lc_editor_apply_accepted_field(leancam_ui_t *ui, const char *field_name, const char *accepted)
{
    float v = 0.0f;

    if (!ui || !field_name || !accepted || !ui->draft_active)
        return;

    lc_editor_normalize_source_edit(ui->draft_line, sizeof(ui->draft_line), field_name);

    if (accepted[0] == 0)
        return;

    v = strtof(accepted, NULL);
    if (v <= 0.0f)
        return;

    if (strcmp(field_name, "C") == 0)
        (void)lc_editor_line_set_field_text(ui->draft_line, sizeof(ui->draft_line), "R", "0");
    else if (strcmp(field_name, "R") == 0 && lc_code_command_is(ui->draft_line, "G1"))
        (void)lc_editor_line_set_field_text(ui->draft_line, sizeof(ui->draft_line), "C", "0");
}

void lc_editor_clear_input(leancam_ui_t *ui)
{
    if (ui)
        ui->input_buf[0] = 0;
}

void lc_editor_toggle_sign(leancam_ui_t *ui)
{
    char *buf;
    size_t len;

    if (!ui)
        return;

    buf = ui->input_buf;
    if (buf[0] == '-')
    {
        memmove(buf, buf + 1, strlen(buf));
        return;
    }

    len = strlen(buf);
    if (len + 1u < sizeof(ui->input_buf))
    {
        memmove(buf + 1, buf, len + 1u);
        buf[0] = '-';
    }
}

void lc_editor_add_dot(leancam_ui_t *ui)
{
    char *buf;
    size_t len;

    if (!ui)
        return;

    buf = ui->input_buf;
    if (strchr(buf, '.'))
        return;

    len = strlen(buf);
    if (len + 1u >= sizeof(ui->input_buf))
        return;

    if (len == 0u)
    {
        if (sizeof(ui->input_buf) > 2u)
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

void lc_editor_input_digit(leancam_ui_t *ui, char digit)
{
    if (!ui)
        return;

    if (ui->input_buf[0] == 0 && lc_editor_active_field_is_negative(ui))
        lc_editor_toggle_sign(ui);

    leancam_ui_input_char(ui, digit);
}

void lc_editor_field_name(const leancam_ui_t *ui, uint8_t field_index, char *out, size_t out_sz)
{
    if (!out || out_sz == 0)
        return;

    out[0] = 0;
    if (ui)
        lc_editor_field_name_from_line(ui->draft_line, field_index, out, out_sz);
    if (!out[0])
        snprintf(out, out_sz, "<none>");
}

void lc_editor_field_name_from_line(const char *line, uint8_t field_index, char *out, size_t out_sz)
{
    const char *p;
    uint8_t idx = 0;

    if (!out || out_sz == 0)
        return;
    out[0] = '\0';
    if (!line)
        return;

    if (!strchr(line, '{'))
    {
        const char *vs;
        const char *ve;
        const char *name_start;
        size_t len;

        if (!lc_editor_plain_field_span(line, field_index, &vs, &ve))
            return;
        (void)ve;
        name_start = vs;
        if (name_start > line && name_start[-1] == '=')
            name_start--;
        while (name_start > line &&
               *(name_start - 1) != ' ' &&
               *(name_start - 1) != '\t')
            name_start--;
        len = (size_t)(vs - name_start);
        if (len > 0 && name_start[len - 1u] == '=')
            len--;
        if (len >= out_sz)
            len = out_sz - 1;
        memcpy(out, name_start, len);
        out[len] = 0;
        return;
    }

    p = line;
    while (*p)
    {
        const char *open = strchr(p, '{');
        const char *name_start;
        size_t len;

        if (!open)
            return;

        if (idx == field_index)
        {
            name_start = open;
            while (name_start > line &&
                   *(name_start - 1) != ' ' &&
                   *(name_start - 1) != '\t')
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

void lc_editor_build_preview_line(const leancam_ui_t *ui, char *out, uint32_t out_len)
{
    const char *open;
    const char *close;

    if (!out || out_len == 0)
        return;

    out[0] = 0;
    if (!ui || !ui->draft_active)
        return;

    strncpy(out, ui->draft_line, out_len - 1u);
    out[out_len - 1u] = 0;

    if (!ui->input_buf[0])
        return;
    if (!lc_editor_find_field(out, g_lc_editor_field_index, &open, &close))
        return;

    (void)lc_editor_replace_span(out,
                                 out_len,
                                 (*open == '{') ? open + 1 : open,
                                 close,
                                 ui->input_buf);
}

void lc_editor_advance_field(const char *line)
{
    uint8_t field_count = lc_editor_field_count(line);

    if (g_lc_editor_field_index + 1u < field_count)
        g_lc_editor_field_index++;
    else
        g_lc_editor_field_index = field_count;
}

bool lc_editor_resolve_draft_line_for_commit(leancam_ui_t *ui,
                                             const char *setup_line,
                                             const char *tool_line,
                                             char *out,
                                             uint32_t out_len)
{
    const char *p;
    char *w;
    char preview_line[MAX_LEN];

    if (!out || out_len == 0 || !ui || !ui->draft_active)
        return false;

    lc_editor_build_preview_line(ui, preview_line, sizeof(preview_line));

    p = preview_line;
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
                !lc_code_resolve_field_value(raw,
                                                  setup_line,
                                                  tool_line,
                                                  preview_line,
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

bool lc_editor_prepare_draft_for_commit(leancam_ui_t *ui,
                                        const char *setup_line,
                                        const char *tool_line)
{
    char resolved_line[MAX_LEN];
    char field_name[24];

    if (!ui || !ui->draft_active)
        return false;

    if (!lc_editor_resolve_draft_line_for_commit(ui,
                                                 setup_line,
                                                 tool_line,
                                                 resolved_line,
                                                 sizeof(resolved_line)))
        return false;

    strncpy(ui->draft_line, resolved_line, sizeof(ui->draft_line) - 1u);
    ui->draft_line[sizeof(ui->draft_line) - 1u] = 0;

    lc_editor_field_name(ui, g_lc_editor_field_index, field_name, sizeof(field_name));
    lc_editor_normalize_source_edit(ui->draft_line,
                                    (uint32_t)sizeof(ui->draft_line),
                                    field_name);
    return true;
}

bool lc_editor_accept_active_field(leancam_ui_t *ui,
                                   const char *setup_line,
                                   const char *tool_line,
                                   char *err,
                                   size_t err_sz)
{
    const char *open;
    const char *close;
    char accepted[LEANCAM_INPUT_MAX];
    char raw[LEANCAM_INPUT_MAX];
    char field_name[24];
    uint32_t n;
    uint8_t field_count;

    if (err && err_sz)
        err[0] = 0;
    if (!ui || !ui->draft_active)
        return false;

    field_count = lc_editor_field_count(ui->draft_line);
    if (field_count == 0 || g_lc_editor_field_index >= field_count)
        return false;

    if (!lc_editor_find_field(ui->draft_line, g_lc_editor_field_index, &open, &close))
        return false;
    lc_editor_field_name(ui, g_lc_editor_field_index, field_name, sizeof(field_name));

    if (ui->input_buf[0])
    {
        strncpy(accepted, ui->input_buf, sizeof(accepted) - 1u);
        accepted[sizeof(accepted) - 1u] = 0;
    }
    else
    {
        n = (uint32_t)(close - open - ((*open == '{') ? 1 : 0));
        if (n >= sizeof(raw)) n = sizeof(raw) - 1u;
        memcpy(raw, open + ((*open == '{') ? 1 : 0), n);
        raw[n] = 0;

        if (!lc_code_resolve_field_value(raw, setup_line, tool_line, ui->draft_line, accepted, sizeof(accepted)) ||
            !accepted[0])
        {
            size_t raw_len = strlen(raw);
            if (raw_len >= 2u && raw[0] == '(' && raw[raw_len - 1u] == ')')
            {
                raw_len -= 2u;
                if (raw_len >= sizeof(accepted)) raw_len = sizeof(accepted) - 1u;
                memcpy(accepted, raw + 1, raw_len);
                accepted[raw_len] = 0;
            }
            else
            {
                if (err && err_sz)
                    snprintf(err, err_sz, "unresolved default");
                return false;
            }
        }
    }

    if (!lc_editor_replace_span(ui->draft_line,
                                (uint32_t)sizeof(ui->draft_line),
                                (*open == '{') ? open + 1 : open,
                                close,
                                accepted))
        return false;

    lc_editor_apply_accepted_field(ui, field_name, accepted);
    lc_editor_clear_input(ui);
    lc_editor_advance_field(ui->draft_line);
    return true;
}



