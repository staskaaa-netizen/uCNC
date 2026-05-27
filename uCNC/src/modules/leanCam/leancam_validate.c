#include "leancam_validate.h"
#include "leancam_tool_catalog.h"
#include "leancam_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool lc_validate_line_get_float(const char *line, const char *key, float *out)
{
    return lc_text_get_field_float(line, key, out);
}

static bool lc_validate_float_is_int(float v)
{
    int iv = (int)v;
    float d = v - (float)iv;
    if (d < 0.0f)
        d = -d;
    return d < 0.001f;
}

const char *lc_validate_effective_tool_for_cycle(const program_t *prog, int before_or_at, const char *cycle)
{
    int i;
    float tv = 0.0f;
    const char *tool;

    if (cycle && lc_validate_line_get_float(cycle, "T", &tv) && tv > 0.0f && lc_validate_float_is_int(tv))
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

            if (lc_text_command_is(line, "TOOLCALL") &&
                lc_validate_line_get_float(line, "T", &header_t) &&
                header_t > 0.0f &&
                lc_validate_float_is_int(header_t))
            {
                tool = lc_tool_catalog_find_in_program_or_catalog(prog, i, (int)header_t);
                if (tool)
                    return tool;
            }

            if (lc_text_command_is(line, "TOOL"))
                return line;
        }
    }

    return NULL;
}

bool lc_validate_tool_call(const program_t *prog,
                           int before_or_at,
                           const char *line,
                           char *err,
                           size_t err_sz)
{
    float tv = 0.0f;

    if (err && err_sz)
        err[0] = 0;
    if (!lc_text_command_is(line, "TOOLCALL"))
        return true;
    if (!lc_validate_line_get_float(line, "T", &tv) || tv <= 0.0f || !lc_validate_float_is_int(tv))
    {
        if (err && err_sz) snprintf(err, err_sz, "TOOLCALL T INVALID");
        return false;
    }
    (void)prog;
    (void)before_or_at;
    return true;
}
