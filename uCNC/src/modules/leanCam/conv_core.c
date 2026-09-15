#include "conv_core.h"
#include <string.h>

void prog_init(program_t *p)
{
    p->count = 0;
}

bool prog_add(program_t *p, const char *line)
{
    if (p->count >= MAX_LINES)
        return false;

    strncpy(p->lines[p->count], line, MAX_LEN - 1);
    p->lines[p->count][MAX_LEN - 1] = 0;
    p->count++;
    return true;
}

bool prog_insert_after(program_t *p, int after_idx, const char *line)
{
    int ins;
    int i;

    if (p->count >= MAX_LINES)
        return false;

    if (after_idx < -1)
        after_idx = -1;
    if (after_idx >= p->count)
        after_idx = p->count - 1;

    ins = after_idx + 1;

    for (i = p->count; i > ins; --i)
        strcpy(p->lines[i], p->lines[i - 1]);

    strncpy(p->lines[ins], line, MAX_LEN - 1);
    p->lines[ins][MAX_LEN - 1] = 0;
    p->count++;
    return true;
}

bool prog_delete(program_t *p, int idx)
{
    int i;

    if (idx < 0 || idx >= p->count)
        return false;

    for (i = idx; i < p->count - 1; ++i)
        strcpy(p->lines[i], p->lines[i + 1]);

    p->count--;
    return true;
}

bool find_field(const char *line, int from, int *s, int *e)
{
    const char *a;
    const char *b;

    a = strchr(line + from, '{');
    if (!a)
        return false;

    b = strchr(a, '}');
    if (!b)
        return false;

    *s = (int)(a - line) + 1;
    *e = (int)(b - line);
    return true;
}

bool set_field(char *line, int s, int e, const char *val)
{
    char tmp[MAX_LEN];
    int pre = s;
    int suf = (int)strlen(line) - e;

    if (pre + (int)strlen(val) + suf >= MAX_LEN)
        return false;

    memcpy(tmp, line, pre);
    strcpy(tmp + pre, val);
    strcpy(tmp + pre + (int)strlen(val), line + e);
    strcpy(line, tmp);
    return true;
}

bool field_is_required_or_unresolved(const char *line, int s, int e)
{
    const char *p = line + s;
    int len = e - s;

    if (len == 1 && p[0] == '*')
        return true;

    if (len >= 6 && strncmp(p, "(auto)", 6) == 0)
        return true;

    if (len >= 7 && strncmp(p, "(SETUP.", 7) == 0)
        return true;

    if (len >= 5 && strncmp(p, "(CUT.", 5) == 0)
        return true;

    return false;
}

bool find_first_required_or_unresolved(const char *line, int *s, int *e)
{
    int cs, ce;
    int pos = 0;

    while (find_field(line, pos, &cs, &ce)) {
        if (field_is_required_or_unresolved(line, cs, ce)) {
            *s = cs;
            *e = ce;
            return true;
        }
        pos = ce;
    }

    return false;
}

bool find_next_required_or_unresolved(const char *line, int from, int *s, int *e)
{
    int cs, ce;
    int pos = from;

    while (find_field(line, pos, &cs, &ce)) {
        if (field_is_required_or_unresolved(line, cs, ce)) {
            *s = cs;
            *e = ce;
            return true;
        }
        pos = ce;
    }

    return false;
}

bool line_has_unresolved_required(const char *line)
{
    int s, e;
    return find_first_required_or_unresolved(line, &s, &e);
}