#include "leancam_text.h"

#include <stdlib.h>
#include <string.h>

bool lc_text_command_is(const char *line, const char *cmd)
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

bool lc_text_get_field_text(const char *line, const char *key, char *out, size_t out_sz)
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

bool lc_text_get_field_float(const char *line, const char *key, float *out)
{
    char text[32];

    if (!out || !lc_text_get_field_text(line, key, text, sizeof(text)) || !text[0] || text[0] == '(')
        return false;

    *out = strtof(text, NULL);
    return true;
}
