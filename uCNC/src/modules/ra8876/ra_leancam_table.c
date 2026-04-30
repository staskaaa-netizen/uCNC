#include "ra_leancam_table.h"

#include <string.h>
#include <stdio.h>

#ifndef UI_TEXT_CHAR_W
#define UI_TEXT_CHAR_W 8
#endif

static void lc_append_padded(char *dst, size_t dst_sz, int *pos, const char *txt, int width)
{
    int n;

    if (!dst || !pos || *pos < 0 || (size_t)*pos >= dst_sz) return;
    if (!txt) txt = "";
    if (width < 1) width = 1;

    n = snprintf(dst + *pos, dst_sz - (size_t)*pos, "%-*s", width, txt);
    if (n < 0) return;

    *pos += n;
    if ((size_t)*pos >= dst_sz) {
        dst[dst_sz - 1] = '\0';
        *pos = (int)dst_sz - 1;
    }
}

static void lc_append_sep(char *dst, size_t dst_sz, int *pos)
{
    int n;

    if (!dst || !pos || *pos < 0 || (size_t)*pos >= dst_sz) return;

    n = snprintf(dst + *pos, dst_sz - (size_t)*pos, " ");
    if (n < 0) return;

    *pos += n;
    if ((size_t)*pos >= dst_sz) {
        dst[dst_sz - 1] = '\0';
        *pos = (int)dst_sz - 1;
    }
}

static int lc_trimmed_len(const char *s)
{
    int len;

    if (!s) return 0;
    len = (int)strlen(s);
    while (len > 0 && s[len - 1] == ' ') len--;
    return len;
}

static bool lc_raw_hi_hits_token(uint8_t hi_s, uint8_t hi_e, int tok_s, int tok_e, int val_s, int val_e)
{
    int hs = (int)hi_s;
    int he = (int)hi_e;

    if (he <= hs) return false;

    if (val_e > val_s && hs < val_e && he > val_s) return true;
    if (hs < tok_e && he > tok_s) return true;
    return false;
}

void ra_lc_build_table_line(const ui_snapshot_frame_t *frame, int row, ra_lc_table_line_t *out)
{
    const char *line;
    const char *p;
    const char *bar;
    char module[24];
    int mlen;
    int hpos = 0;
    int vpos = 0;
    uint8_t raw_hi_s = 0;
    uint8_t raw_hi_e = 0;

    if (!out) return;
    memset(out, 0, sizeof(*out));

    if (!frame || row < 0 || row >= UI_LC_MAX_LINES) return;

    line = frame->leancam_lines[row];
    raw_hi_s = frame->leancam_field_hi_start[row];
    raw_hi_e = frame->leancam_field_hi_end[row];

    bar = strchr(line, '|');
    if (!bar) {
        ui_snapshot_strcpy(out->header, line, sizeof(out->header));
        out->table_like = false;
        return;
    }

    mlen = (int)(bar - line);
    if (mlen >= (int)sizeof(module)) mlen = (int)sizeof(module) - 1;
    if (mlen < 0) mlen = 0;
    memcpy(module, line, (size_t)mlen);
    module[mlen] = '\0';

    lc_append_padded(out->header, sizeof(out->header), &hpos, module, 10);
    lc_append_padded(out->value,  sizeof(out->value),  &vpos, "",     10);
    lc_append_sep(out->header, sizeof(out->header), &hpos);
    lc_append_sep(out->value,  sizeof(out->value),  &vpos);

    p = bar + 1;
    while (*p && hpos < (UI_LC_LINE_LEN - 2)) {
        const char *next_bar = strchr(p, '|');
        const char *open;
        const char *close;
        const char *name_end;
        char name[32];
        char value[32];
        int name_len;
        int value_len;
        int width;
        int col_start;
        int tok_s;
        int tok_e;
        int val_s = -1;
        int val_e = -1;

        if (!next_bar) next_bar = p + strlen(p);
        if (next_bar <= p) break;

        open = memchr(p, '{', (size_t)(next_bar - p));
        close = open ? memchr(open, '}', (size_t)(next_bar - open)) : NULL;
        name_end = open ? open : next_bar;

        name_len = (int)(name_end - p);
        while (name_len > 0 && p[name_len - 1] == ' ') name_len--;
        if (name_len >= (int)sizeof(name)) name_len = (int)sizeof(name) - 1;
        if (name_len < 0) name_len = 0;
        memcpy(name, p, (size_t)name_len);
        name[name_len] = '\0';

        value[0] = '\0';
        if (open && close && close > open) {
            value_len = (int)(close - open - 1);
            if (value_len >= (int)sizeof(value)) value_len = (int)sizeof(value) - 1;
            if (value_len < 0) value_len = 0;
            memcpy(value, open + 1, (size_t)value_len);
            value[value_len] = '\0';
            val_s = (int)((open + 1) - line);
            val_e = (int)(close - line);
        }

        width = lc_trimmed_len(name);
        if (lc_trimmed_len(value) > width) width = lc_trimmed_len(value);
        if (width < 6) width = 6;
        width += 2;

        if (hpos + width + 3 >= UI_LC_LINE_LEN - 1)
            width = (UI_LC_LINE_LEN - 4) - hpos;
        if (width <= 0) break;

        col_start = hpos;
        tok_s = (int)(p - line);
        tok_e = (int)(next_bar - line);

        lc_append_padded(out->header, sizeof(out->header), &hpos, name, width);
        lc_append_padded(out->value,  sizeof(out->value),  &vpos, value, width);

        if (lc_raw_hi_hits_token(raw_hi_s, raw_hi_e, tok_s, tok_e, val_s, val_e)) {
            out->hi_start = (uint8_t)col_start;
            out->hi_end = (uint8_t)hpos;
            out->has_hi = true;
        }

        lc_append_sep(out->header, sizeof(out->header), &hpos);
        lc_append_sep(out->value,  sizeof(out->value),  &vpos);

        if (*next_bar == '|') p = next_bar + 1;
        else break;
    }

    out->table_like = true;
}
