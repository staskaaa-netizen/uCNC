#include "leancam_regions.h"
#include "leancam_text.h"

bool lc_region_is_header(const char *line)
{
    return lc_text_command_is(line, "G71") ||
           lc_text_command_is(line, "G72");
}

bool lc_region_is_contour(const char *line)
{
    return lc_text_command_is(line, "G1") ||
           lc_text_command_is(line, "G2") ||
           lc_text_command_is(line, "G3");
}

bool lc_region_is_end(const char *line)
{
    return lc_text_command_is(line, "G80");
}

int lc_region_display_indent(const program_t *prog, int index, const char *line)
{
    int i;

    if (!lc_region_is_contour(line))
        return 0;
    if (!prog || index <= 0)
        return 0;

    for (i = index - 1; i >= 0; --i)
    {
        const char *prev = prog->lines[i];
        if (lc_region_is_header(prev))
            return 1;
        if (lc_region_is_end(prev))
            break;
        if (lc_text_command_is(prev, "SETUP") ||
            lc_text_command_is(prev, "TOOL") ||
            lc_text_command_is(prev, "TOOLCALL"))
            continue;
        if (lc_region_is_contour(prev))
            continue;
        break;
    }

    return 0;
}

bool lc_region_find(const program_t *prog, int index, int *start_out, int *end_out)
{
    int start = -1;
    int end = -1;
    int i;

    if (start_out) *start_out = -1;
    if (end_out) *end_out = -1;
    if (!prog || index < 0 || index >= prog->count)
        return false;

    if (lc_region_is_header(prog->lines[index]))
    {
        start = index;
    }
    else if (lc_region_is_contour(prog->lines[index]) || lc_region_is_end(prog->lines[index]))
    {
        for (i = index; i >= 0; --i)
        {
            if (lc_region_is_header(prog->lines[i]))
            {
                start = i;
                break;
            }
            if (i != index && !lc_region_is_contour(prog->lines[i]) && !lc_region_is_end(prog->lines[i]))
                break;
        }
    }

    if (start < 0)
        return false;

    end = start;
    for (i = start + 1; i < prog->count; ++i)
    {
        if (lc_region_is_header(prog->lines[i]))
            break;
        if (lc_region_is_end(prog->lines[i]))
        {
            end = i;
            break;
        }
        if (!lc_region_is_contour(prog->lines[i]))
            break;
        end = i;
    }

    if (start_out) *start_out = start;
    if (end_out) *end_out = end;
    return true;
}
