#include "leancam_paths.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

const char *lc_path_basename(const char *path)
{
    const char *p1;
    const char *p2;
    const char *p;

    if (!path || !path[0])
        return "<no file>";

    p1 = strrchr(path, '/');
    p2 = strrchr(path, '\\');
    p = p1 > p2 ? p1 : p2;
    return p ? p + 1 : path;
}

int lc_path_stricmp(const char *a, const char *b)
{
    unsigned char ca;
    unsigned char cb;

    if (!a)
        a = "";
    if (!b)
        b = "";

    while (*a || *b)
    {
        ca = (unsigned char)tolower((unsigned char)*a++);
        cb = (unsigned char)tolower((unsigned char)*b++);
        if (ca != cb)
            return (int)ca - (int)cb;
    }

    return 0;
}

bool lc_path_has_suffix_ci(const char *name, const char *suffix)
{
    size_t ln;
    size_t ls;

    if (!name || !suffix)
        return false;

    ln = strlen(name);
    ls = strlen(suffix);
    if (ln < ls)
        return false;

    return lc_path_stricmp(name + ln - ls, suffix) == 0;
}

static bool lc_path_is_name_char(char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

static void lc_path_sanitize_name_part(const char *in, char *out, size_t out_sz)
{
    size_t i = 0;
    char last = 0;

    if (!out || out_sz == 0)
        return;

    out[0] = 0;
    if (!in)
        return;

    while (*in && i + 1 < out_sz)
    {
        char c = *in++;
        if (!lc_path_is_name_char(c))
            c = '_';
        if (c == '_' && last == '_')
            continue;
        out[i++] = c;
        last = c;
    }

    while (i > 0 && out[i - 1] == '_')
        --i;
    out[i] = 0;
}

void lc_path_get_program_stem(const char *current_path, char *out, size_t out_sz)
{
    const char *base;
    const char *dot;
    size_t len;
    char tmp[40];

    if (!out || out_sz == 0)
        return;

    base = lc_path_basename(current_path);
    dot = strrchr(base, '.');
    len = dot && dot > base ? (size_t)(dot - base) : strlen(base);
    if (len >= sizeof(tmp))
        len = sizeof(tmp) - 1u;

    memcpy(tmp, base, len);
    tmp[len] = 0;

    lc_path_sanitize_name_part(tmp, out, out_sz);
    if (!out[0])
    {
        strncpy(out, "program", out_sz - 1u);
        out[out_sz - 1u] = 0;
    }
}

void lc_path_get_operation_name(const char *line, char *out, size_t out_sz)
{
    char tmp[20];
    size_t n = 0;

    if (!out || out_sz == 0)
        return;

    if (!line)
        line = "";

    while (line[n] && line[n] != '{' && line[n] != ' ' && line[n] != '\t' && n + 1 < sizeof(tmp))
        ++n;

    memcpy(tmp, line, n);
    tmp[n] = 0;

    lc_path_sanitize_name_part(tmp, out, out_sz);
    if (!out[0])
    {
        strncpy(out, "OP", out_sz - 1u);
        out[out_sz - 1u] = 0;
    }
}

bool lc_path_make_lrun(const char *dir,
                       const char *current_path,
                       const char *line,
                       int line_no,
                       char *out,
                       int out_sz)
{
    char op[16];
    char stem[32];
    int n;

    if (!dir || !out || out_sz <= 0)
        return false;

    lc_path_get_operation_name(line, op, sizeof(op));
    lc_path_get_program_stem(current_path, stem, sizeof(stem));

    n = snprintf(out, (size_t)out_sz, "%s/%s_L%d_%s.lrun", dir, op, line_no, stem);
    return n > 0 && n < out_sz;
}
