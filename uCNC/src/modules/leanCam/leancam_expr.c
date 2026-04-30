#include "leancam_expr.h"

#include <string.h>

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
        while (name_start > line && *(name_start - 1) != '|')
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
                                      const char *this_line,
                                      char *out,
                                      uint32_t out_len,
                                      uint8_t depth);

static int lc_resolve_raw_value(const char *raw,
                                const char *setup_line,
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
        if (strncmp(expr, "SETUP.", 6) == 0)
            return lc_resolve_value_from_line(setup_line, expr + 6, setup_line, this_line, out, out_len, (uint8_t)(depth + 1u));

        if (strncmp(expr, "THIS.", 5) == 0)
            return lc_resolve_value_from_line(this_line, expr + 5, setup_line, this_line, out, out_len, (uint8_t)(depth + 1u));

        ui_snapshot_strcpy(out, expr, out_len);
        return out[0] != 0;
    }

    ui_snapshot_strcpy(out, raw, out_len);
    return out[0] != 0;
}

static int lc_resolve_value_from_line(const char *line,
                                      const char *field,
                                      const char *setup_line,
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
        return 0;

    if (!lc_copy_span(raw, sizeof(raw), open + 1, close))
        return 0;

    return lc_resolve_raw_value(raw, setup_line, this_line, out, out_len, depth);
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

void leancam_expr_build_draft_display(char *dst,
                                      uint32_t dst_len,
                                      const char *draft,
                                      const char *input,
                                      uint8_t active_index,
                                      const char *setup_line,
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

    lc_append_cstr(dst, dst_len, &pos, "> ");

    scan = draft;
    copy_from = draft;

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
            (void)lc_resolve_raw_value(raw, setup_line, this_line, shown, sizeof(shown), 0);

        lc_append_cstr(dst, dst_len, &pos, shown);

        min_len = (uint32_t)strlen(shown);
        if (idx == active_index)
        {
            char old_shown[UI_LC_LINE_LEN];
            uint32_t old_len;

            old_shown[0] = 0;
            (void)lc_resolve_raw_value(raw, setup_line, this_line, old_shown, sizeof(old_shown), 0);
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
