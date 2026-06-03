/* G71/G72 generator support.
 * This module intentionally does not call the live RS274 parser. LeanCam needs
 * offline preview/preflight/run generation, so G7x keeps a tiny source-row
 * reader and only reuses uCNC's public modal constants.
 */
#include "g71_g72.h"

#include <stdlib.h>
#include <string.h>

void g7x_modal_default(g7x_modal_t *modal)
{
    if (!modal)
        return;

    modal->units = G7X_UNITS_MM;
    modal->distance = G7X_DISTANCE_ABSOLUTE;
}

void g7x_modal_from_ucnc_modes(g7x_modal_t *modal, const uint8_t *modalgroups)
{
    if (!modal)
        return;

    g7x_modal_default(modal);
    if (!modalgroups)
        return;

    if (modalgroups[2] == 90u || modalgroups[2] == 91u)
        modal->distance = modalgroups[2] == 91u ? G7X_DISTANCE_INCREMENTAL : G7X_DISTANCE_ABSOLUTE;
    if (modalgroups[4] == 20u || modalgroups[4] == 21u)
        modal->units = modalgroups[4] == 20u ? G7X_UNITS_INCH : G7X_UNITS_MM;
}

bool g7x_command_is(const char *line, const char *cmd)
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

bool g7x_get_field_text(const char *line, const char *key, char *out, size_t out_sz)
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

bool g7x_get_field_float(const char *line, const char *key, float *out)
{
    char text[32];

    if (!out || !g7x_get_field_text(line, key, text, sizeof(text)) || !text[0] || text[0] == '(')
        return false;

    *out = strtof(text, NULL);
    return true;
}

bool g7x_modal_apply_line(g7x_modal_t *modal, const char *line)
{
    bool changed = false;

    if (!modal || !line)
        return false;

    if (g7x_command_is(line, "G20"))
    {
        modal->units = G7X_UNITS_INCH;
        changed = true;
    }
    else if (g7x_command_is(line, "G21"))
    {
        modal->units = G7X_UNITS_MM;
        changed = true;
    }

    if (g7x_command_is(line, "G90"))
    {
        modal->distance = G7X_DISTANCE_ABSOLUTE;
        changed = true;
    }
    else if (g7x_command_is(line, "G91"))
    {
        modal->distance = G7X_DISTANCE_INCREMENTAL;
        changed = true;
    }

    return changed;
}

g7x_cycle_t g7x_cycle_from_line(const char *line)
{
    if (g7x_command_is(line, "G71"))
        return G7X_CYCLE_G71;
    if (g7x_command_is(line, "G72"))
        return G7X_CYCLE_G72;
    return G7X_CYCLE_NONE;
}

g7x_contour_cmd_t g7x_contour_cmd_from_line(const char *line)
{
    if (g7x_command_is(line, "G80"))
        return G7X_CONTOUR_END;
    if (g7x_command_is(line, "G0"))
        return G7X_CONTOUR_RAPID;
    if (g7x_command_is(line, "G1"))
        return G7X_CONTOUR_LINE;
    if (g7x_command_is(line, "G2"))
        return G7X_CONTOUR_ARC_CW;
    if (g7x_command_is(line, "G3"))
        return G7X_CONTOUR_ARC_CCW;
    return G7X_CONTOUR_NONE;
}

const char *g7x_result_text(g7x_result_t result)
{
    switch (result)
    {
        case G7X_OK: return "ok";
        case G7X_BAD_FIELD: return "bad field";
        case G7X_UNSUPPORTED: return "unsupported";
        case G7X_WRITE_FAILED: return "write failed";
        default: return "unknown";
    }
}
