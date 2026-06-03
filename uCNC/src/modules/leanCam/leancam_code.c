/* LeanCam module contract:
 * Purpose: one NC row/code module for command/field parsing, template expressions,
 *          G71/G72 region ownership, and cheap row validation.
 * Called by: bridge, editor, presets, run/preflight, tool catalog, and generator.
 * Calls into: tool catalog for validation lookup plus plain C string helpers.
 * Owns: no persistent state; keep it deterministic and side-effect free.
 */
#include "leancam_code.h"
#include "leancam_tool_catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool lc_code_command_is(const char *line, const char *cmd)
{
    size_t n;

    if (!line || !cmd)
        return false;
    while (*line == ' ' || *line == '\t')
        line++;
    n = strlen(cmd);
    return strncmp(line, cmd, n) == 0 &&
           (line[n] == 0 || line[n] == ' ' || line[n] == '\t');
}

bool lc_code_get_field_text(const char *line, const char *key, char *out, size_t out_sz)
{
    const char *p;
    const char *b;
    const char *e;
    size_t key_len;
    size_t len;
    bool saw_token = false;

    if (!line || !key || !out || out_sz == 0)
        return false;

    out[0] = 0;
    key_len = strlen(key);
    p = line;
    while (*p)
    {
        bool is_command_token;

        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;

        is_command_token = !saw_token;
        b = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        e = p;
        saw_token = true;

        if (!is_command_token &&
            (size_t)(e - b) > key_len &&
            strncmp(b, key, key_len) == 0 &&
            b[key_len] != '_')
        {
            const char *v = b + key_len;
            const char *vend = e;

            if (*v == '=')
                v++;
            if (memchr(v, '{', (size_t)(vend - v)) ||
                memchr(v, '}', (size_t)(vend - v)))
                return false;

            len = (size_t)(vend - v);
            if (len >= out_sz)
                len = out_sz - 1;
            if (len)
                memcpy(out, v, len);
            out[len] = 0;
            return true;
        }
    }

    return false;
}

bool lc_code_get_field_float(const char *line, const char *key, float *out)
{
    char text[32];

    if (!out || !lc_code_get_field_text(line, key, text, sizeof(text)) || !text[0] || text[0] == '(')
        return false;

    *out = strtof(text, NULL);
    return true;
}


static int lc_copy_span(char *dst, uint32_t dst_len, const char *a, const char *b)
{
    uint32_t n;

    if (!dst || dst_len == 0)
        return 0;

    dst[0] = 0;

    if (!a || !b || b < a)
        return 0;

    n = (uint32_t)(b - a);
    if (n >= dst_len)
        n = dst_len - 1u;

    if (n)
        memcpy(dst, a, n);
    dst[n] = 0;
    return 1;
}

static int lc_find_named_field(const char *line,
                               const char *field,
                               const char **open_out,
                               const char **close_out)
{
    const char *scan;
    uint32_t flen;

    if (open_out) *open_out = NULL;
    if (close_out) *close_out = NULL;

    if (!line || !field || !field[0])
        return 0;

    if (!strchr(line, '{'))
    {
        static char plain_value[UI_LC_LINE_LEN + 1u];

        plain_value[0] = '{';
        if (!lc_code_get_field_text(line, field, plain_value + 1, sizeof(plain_value) - 1u))
        {
            if (strcmp(field, "RPM") == 0 &&
                (lc_code_get_field_text(line, "S", plain_value + 1, sizeof(plain_value) - 1u) ||
                 lc_code_get_field_text(line, "SPINDLE_RPM", plain_value + 1, sizeof(plain_value) - 1u)))
            {
                /* handled below */
            }
            else
            {
                return 0;
            }
        }

        if (open_out) *open_out = plain_value;
        if (close_out) *close_out = plain_value + strlen(plain_value);
        return 1;
    }

    flen = (uint32_t)strlen(field);
    scan = line;

    while (*scan)
    {
        const char *o = strchr(scan, '{');
        const char *c = o ? strchr(o + 1, '}') : NULL;
        const char *name_start;
        uint32_t name_len;

        if (!o || !c)
            return 0;

        name_start = o;
        while (name_start > line &&
               *(name_start - 1) != ' ' &&
               *(name_start - 1) != '\t')
            name_start--;

        name_len = (uint32_t)(o - name_start);
        if (name_len == flen && strncmp(name_start, field, flen) == 0)
        {
            if (open_out) *open_out = o;
            if (close_out) *close_out = c;
            return 1;
        }

        scan = c + 1;
    }

    return 0;
}

static int lc_strip_default_expr(const char *raw,
                                 char *expr,
                                 uint32_t expr_len)
{
    uint32_t n;

    if (!raw || !expr || expr_len == 0)
        return 0;

    expr[0] = 0;
    n = (uint32_t)strlen(raw);

    if (n >= 2u && raw[0] == '(' && raw[n - 1u] == ')')
    {
        n -= 2u;
        if (n >= expr_len)
            n = expr_len - 1u;
        if (n)
            memcpy(expr, raw + 1, n);
        expr[n] = 0;
        return 1;
    }

    return 0;
}

static int lc_resolve_value_from_line(const char *line,
                                      const char *field,
                                      const char *setup_line,
                                      const char *tool_line,
                                      const char *this_line,
                                      char *out,
                                      uint32_t out_len,
                                      uint8_t depth);

static int lc_resolve_raw_value(const char *raw,
                                const char *setup_line,
                                const char *tool_line,
                                const char *this_line,
                                char *out,
                                uint32_t out_len,
                                uint8_t depth)
{
    char expr[UI_LC_LINE_LEN];

    if (!out || out_len == 0)
        return 0;

    out[0] = 0;

    if (!raw || !raw[0])
        return 0;

    if (depth > 6u)
        return 0;

    if (lc_strip_default_expr(raw, expr, sizeof(expr)))
    {
        raw = expr;
    }

    if (strncmp(raw, "SETUP.", 6) == 0)
        return lc_resolve_value_from_line(setup_line, raw + 6, setup_line, tool_line, this_line, out, out_len, (uint8_t)(depth + 1u));

    if (strncmp(raw, "TOOL.", 5) == 0)
        return lc_resolve_value_from_line(tool_line, raw + 5, setup_line, tool_line, this_line, out, out_len, (uint8_t)(depth + 1u));

    if (strncmp(raw, "TOOLCALL.", 9) == 0)
        return lc_resolve_value_from_line(tool_line, raw + 9, setup_line, tool_line, this_line, out, out_len, (uint8_t)(depth + 1u));

    if (strncmp(raw, "THIS.", 5) == 0)
        return lc_resolve_value_from_line(this_line, raw + 5, setup_line, tool_line, this_line, out, out_len, (uint8_t)(depth + 1u));

    if (raw == expr)
    {
        ui_snapshot_strcpy(out, raw, out_len);
        return out[0] != 0;
    }

    ui_snapshot_strcpy(out, raw, out_len);
    return out[0] != 0;
}

static int lc_resolve_value_from_line(const char *line,
                                      const char *field,
                                      const char *setup_line,
                                      const char *tool_line,
                                      const char *this_line,
                                      char *out,
                                      uint32_t out_len,
                                      uint8_t depth)
{
    const char *open;
    const char *close;
    char raw[UI_LC_LINE_LEN];

    if (!out || out_len == 0)
        return 0;

    out[0] = 0;

    if (!lc_find_named_field(line, field, &open, &close))
    {
        if (field && strcmp(field, "RPM") == 0)
        {
            if (lc_find_named_field(line, "S", &open, &close) ||
                lc_find_named_field(line, "SPINDLE_RPM", &open, &close))
            {
                if (!lc_copy_span(raw, sizeof(raw), open + 1, close))
                    return 0;
                return lc_resolve_raw_value(raw, setup_line, tool_line, this_line, out, out_len, depth);
            }
            ui_snapshot_strcpy(out, "800", out_len);
            return 1;
        }
        return 0;
    }

    if (!lc_copy_span(raw, sizeof(raw), open + 1, close))
        return 0;

    return lc_resolve_raw_value(raw, setup_line, tool_line, this_line, out, out_len, depth);
}

int lc_code_resolve_field_value(const char *raw,
                                     const char *setup_line,
                                     const char *tool_line,
                                     const char *this_line,
                                     char *out,
                                     uint32_t out_len)
{
    return lc_resolve_raw_value(raw, setup_line, tool_line, this_line, out, out_len, 0);
}

static void lc_append_span(char *dst, uint32_t dst_len, uint32_t *pos, const char *a, const char *b)
{
    uint32_t n;

    if (!dst || dst_len == 0 || !pos || *pos >= dst_len)
        return;

    if (!a || !b || b <= a)
        return;

    n = (uint32_t)(b - a);
    if (n > dst_len - *pos - 1u)
        n = dst_len - *pos - 1u;

    if (n)
    {
        memcpy(dst + *pos, a, n);
        *pos += n;
        dst[*pos] = 0;
    }
}

static void lc_append_cstr(char *dst, uint32_t dst_len, uint32_t *pos, const char *s)
{
    uint32_t n;

    if (!dst || dst_len == 0 || !pos || *pos >= dst_len)
        return;

    if (!s)
        s = "";

    n = (uint32_t)strlen(s);
    if (n > dst_len - *pos - 1u)
        n = dst_len - *pos - 1u;

    if (n)
    {
        memcpy(dst + *pos, s, n);
        *pos += n;
        dst[*pos] = 0;
    }
}

void lc_code_build_draft_display(char *dst,
                                      uint32_t dst_len,
                                      const char *draft,
                                      const char *input,
                                      uint8_t active_index,
                                      const char *setup_line,
                                      const char *tool_line,
                                      const char *this_line,
                                      uint8_t *hi_start,
                                      uint8_t *hi_end)
{
    const char *scan;
    const char *copy_from;
    uint32_t pos = 0;
    uint8_t idx = 0;

    if (hi_start) *hi_start = 0;
    if (hi_end)   *hi_end = 0;

    if (!dst || dst_len == 0)
        return;

    dst[0] = 0;
    if (!draft) draft = "";
    if (!input) input = "";
    if (!this_line) this_line = draft;

    scan = draft;
    copy_from = draft;

    if (!strchr(draft, '{'))
    {
        uint8_t plain_idx = 0;
        const char *p = draft;

        while (*p && pos + 1u < dst_len)
        {
            const char *tok;
            const char *end;
            const char *value;
            uint32_t field_start;
            uint32_t field_end;

            if (*p == ' ' || *p == '\t')
            {
                dst[pos++] = *p++;
                dst[pos] = 0;
                continue;
            }

            tok = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
            end = p;

            value = tok;
            while (value < end &&
                   ((*value >= 'A' && *value <= 'Z') ||
                    (*value >= 'a' && *value <= 'z') ||
                    *value == '_'))
                value++;
            if (value < end && *value == '=')
                value++;

            if (tok == draft || value >= end)
            {
                lc_append_span(dst, dst_len, &pos, tok, end);
                continue;
            }

            lc_append_span(dst, dst_len, &pos, tok, value);
            field_start = pos;
            if (plain_idx == active_index && input[0])
                lc_append_cstr(dst, dst_len, &pos, input);
            else
                lc_append_span(dst, dst_len, &pos, value, end);
            field_end = pos;

            if (plain_idx == active_index)
            {
                if (hi_start) *hi_start = (field_start > 255u) ? 255u : (uint8_t)field_start;
                if (hi_end)   *hi_end   = (field_end   > 255u) ? 255u : (uint8_t)field_end;
            }
            if (plain_idx < 250u)
                plain_idx++;
        }
        return;
    }

    while (scan && *scan && pos + 1u < dst_len)
    {
        const char *open = strchr(scan, '{');
        const char *close = open ? strchr(open + 1, '}') : NULL;
        char raw[UI_LC_LINE_LEN];
        char shown[UI_LC_LINE_LEN];
        uint32_t field_start;
        uint32_t field_end;
        uint32_t min_len;

        if (!open || !close)
            break;

        lc_append_span(dst, dst_len, &pos, copy_from, open);

        field_start = pos;
        lc_append_cstr(dst, dst_len, &pos, "{");

        lc_copy_span(raw, sizeof(raw), open + 1, close);
        shown[0] = 0;

        if (idx == active_index && input[0])
            ui_snapshot_strcpy(shown, input, sizeof(shown));
        else
            (void)lc_resolve_raw_value(raw, setup_line, tool_line, this_line, shown, sizeof(shown), 0);

        lc_append_cstr(dst, dst_len, &pos, shown);

        min_len = (uint32_t)strlen(shown);
        if (idx == active_index)
        {
            char old_shown[UI_LC_LINE_LEN];
            uint32_t old_len;

            old_shown[0] = 0;
            (void)lc_resolve_raw_value(raw, setup_line, tool_line, this_line, old_shown, sizeof(old_shown), 0);
            old_len = (uint32_t)strlen(old_shown);

            while (min_len < old_len && pos + 1u < dst_len)
            {
                dst[pos++] = ' ';
                dst[pos] = 0;
                min_len++;
            }
        }

        lc_append_cstr(dst, dst_len, &pos, "}");
        field_end = pos;

        if (idx == active_index)
        {
            if (hi_start) *hi_start = (field_start > 255u) ? 255u : (uint8_t)field_start;
            if (hi_end)   *hi_end   = (field_end   > 255u) ? 255u : (uint8_t)field_end;
        }

        idx++;
        scan = close + 1;
        copy_from = close + 1;
    }

    lc_append_cstr(dst, dst_len, &pos, copy_from);
}


bool lc_code_region_is_header(const char *line)
{
    return lc_code_command_is(line, "G71") ||
           lc_code_command_is(line, "G72");
}

bool lc_code_region_is_contour(const char *line)
{
    return lc_code_command_is(line, "G1") ||
           lc_code_command_is(line, "G2") ||
           lc_code_command_is(line, "G3");
}

bool lc_code_region_is_end(const char *line)
{
    return lc_code_command_is(line, "G80");
}

int lc_code_region_display_indent(const program_t *prog, int index, const char *line)
{
    int i;

    if (!lc_code_region_is_contour(line))
        return 0;
    if (!prog || index <= 0)
        return 0;

    for (i = index - 1; i >= 0; --i)
    {
        const char *prev = prog->lines[i];
        if (lc_code_region_is_header(prev))
            return 1;
        if (lc_code_region_is_end(prev))
            break;
        if (lc_code_command_is(prev, "SETUP") ||
            lc_code_command_is(prev, "TOOL") ||
            lc_code_command_is(prev, "TOOLCALL"))
            continue;
        if (lc_code_region_is_contour(prev))
            continue;
        break;
    }

    return 0;
}

bool lc_code_region_find(const program_t *prog, int index, int *start_out, int *end_out)
{
    int start = -1;
    int end = -1;
    int i;

    if (start_out) *start_out = -1;
    if (end_out) *end_out = -1;
    if (!prog || index < 0 || index >= prog->count)
        return false;

    if (lc_code_region_is_header(prog->lines[index]))
    {
        start = index;
    }
    else if (lc_code_region_is_contour(prog->lines[index]) || lc_code_region_is_end(prog->lines[index]))
    {
        for (i = index; i >= 0; --i)
        {
            if (lc_code_region_is_header(prog->lines[i]))
            {
                start = i;
                break;
            }
            if (i != index && !lc_code_region_is_contour(prog->lines[i]) && !lc_code_region_is_end(prog->lines[i]))
                break;
        }
    }

    if (start < 0)
        return false;

    end = start;
    for (i = start + 1; i < prog->count; ++i)
    {
        if (lc_code_region_is_header(prog->lines[i]))
            break;
        if (lc_code_region_is_end(prog->lines[i]))
        {
            end = i;
            break;
        }
        if (!lc_code_region_is_contour(prog->lines[i]))
            break;
        end = i;
    }

    if (start_out) *start_out = start;
    if (end_out) *end_out = end;
    return true;
}


static bool lc_code_validate_line_get_float(const char *line, const char *key, float *out)
{
    return lc_code_get_field_float(line, key, out);
}

static bool lc_code_validate_float_is_int(float v)
{
    int iv = (int)v;
    float d = v - (float)iv;
    if (d < 0.0f)
        d = -d;
    return d < 0.001f;
}

const char *lc_code_effective_tool_for_cycle(const program_t *prog, int before_or_at, const char *cycle)
{
    int i;
    float tv = 0.0f;
    const char *tool;

    if (cycle && lc_code_validate_line_get_float(cycle, "T", &tv) && tv > 0.0f && lc_code_validate_float_is_int(tv))
    {
        tool = lc_tool_catalog_find_in_program_or_catalog(prog, before_or_at, (int)tv);
        if (tool)
            return tool;
    }

    if (prog)
    {
        if (before_or_at >= prog->count)
            before_or_at = prog->count - 1;

        for (i = before_or_at; i >= 0; --i)
        {
            float header_t = 0.0f;
            const char *line = prog->lines[i];

            if (!line)
                continue;

            if (lc_code_command_is(line, "TOOLCALL") &&
                lc_code_validate_line_get_float(line, "T", &header_t) &&
                header_t > 0.0f &&
                lc_code_validate_float_is_int(header_t))
            {
                tool = lc_tool_catalog_find_in_program_or_catalog(prog, i, (int)header_t);
                if (tool)
                    return tool;
            }

            if (lc_code_command_is(line, "TOOL"))
                return line;
        }
    }

    return NULL;
}

bool lc_code_validate_tool_call(const program_t *prog,
                           int before_or_at,
                           const char *line,
                           char *err,
                           size_t err_sz)
{
    float tv = 0.0f;

    if (err && err_sz)
        err[0] = 0;
    if (!lc_code_command_is(line, "TOOLCALL"))
        return true;
    if (!lc_code_validate_line_get_float(line, "T", &tv) || tv <= 0.0f || !lc_code_validate_float_is_int(tv))
    {
        if (err && err_sz) snprintf(err, err_sz, "TOOLCALL T INVALID");
        return false;
    }
    (void)prog;
    (void)before_or_at;
    return true;
}



