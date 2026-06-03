/* LeanCam module contract:
 * Purpose: convert LeanCam internal UI state into renderer-neutral snapshot structures.
 * Called by: visual state builder after bridge has updated application state.
 * Calls into: text/program formatting helpers only.
 * Owns: no long-lived application state; it prepares display data for consumers.
 */
#include "leancam_snapshot.h"

#include <stdio.h>
#include <string.h>

void lc_snapshot_reset_frame(ui_snapshot_frame_t *f)
{
    int i;

    if (!f)
        return;

    f->leancam_active = true;
    f->leancam_line_count = 0;
    f->leancam_setup_line[0] = 0;
    f->leancam_preview_line[0] = 0;
    f->leancam_preview_region_count = 0;
    f->leancam_fullscreen_sim = false;
    f->leancam_sim_preview_active = false;
    f->leancam_sim_preview_seq = 0;
    f->leancam_sim_preview_index = 0;
    f->leancam_sim_preview_count = 0;
    f->leancam_sim_preview_line[0] = 0;
    f->leancam_tool_line[0] = 0;
    f->leancam_active_field[0] = 0;
    f->leancam_thread_lane_valid = false;
    f->leancam_thread_start_lane = 0.0f;
    f->leancam_thread_stop_lane = 0.0f;
    f->leancam_thread_ramp_lane = 0.0f;
    f->leancam_thread_lock_lane = 0.0f;
    f->leancam_thread_z_speed = 0.0f;

    for (i = 0; i < UI_LC_MAX_LINES; ++i)
    {
        f->leancam_lines[i][0] = 0;
        f->leancam_line_selected[i] = 0;
        f->leancam_field_hi_start[i] = 0;
        f->leancam_field_hi_end[i] = 0;
    }

    for (i = 0; i < UI_LC_PREVIEW_REGION_MAX; ++i)
    {
        f->leancam_preview_region[i][0] = 0;
        f->leancam_preview_region_selected[i] = 0;
    }
}

void lc_snapshot_put_line_ex(ui_snapshot_frame_t *f,
                             int row,
                             const char *s,
                             bool selected,
                             uint8_t hi_start,
                             uint8_t hi_end)
{
    uint32_t len;

    if (!f || row < 0 || row >= UI_LC_MAX_LINES)
        return;

    ui_snapshot_strcpy(f->leancam_lines[row], s ? s : "", UI_LC_LINE_LEN);
    f->leancam_line_selected[row] = selected ? 1u : 0u;

    len = (uint32_t)strlen(f->leancam_lines[row]);
    if (hi_start > len) hi_start = (uint8_t)len;
    if (hi_end > len) hi_end = (uint8_t)len;
    if (hi_end < hi_start) hi_end = hi_start;

    f->leancam_field_hi_start[row] = hi_start;
    f->leancam_field_hi_end[row] = hi_end;

    if (row + 1 > f->leancam_line_count)
        f->leancam_line_count = (uint8_t)(row + 1);
}

void lc_snapshot_put_line(ui_snapshot_frame_t *f, int row, const char *s, bool selected)
{
    lc_snapshot_put_line_ex(f, row, s, selected, 0, 0);
}

void lc_snapshot_format_program_line(int line_no,
                                     int indent,
                                     const char *display_line,
                                     char *out,
                                     size_t out_sz)
{
    const char *pad;

    if (!out || out_sz == 0)
        return;
    out[0] = 0;

    pad = indent ? "    " : "";
    snprintf(out, out_sz, "%02d %s%.76s", line_no, pad, display_line ? display_line : "");
}

static size_t lc_snapshot_wrap_span(const char *s, size_t max_len)
{
    size_t i;
    size_t last_space = 0;

    if (!s)
        return 0;
    for (i = 0; s[i] && i < max_len; ++i)
    {
        if (s[i] == ' ')
            last_space = i;
    }
    if (!s[i])
        return i;
    return last_space > 0 ? last_space : max_len;
}

int lc_snapshot_put_program_command(ui_snapshot_frame_t *f,
                                    int row,
                                    int line_no,
                                    int indent,
                                    const char *display_line,
                                    bool selected)
{
    const char *p = display_line ? display_line : "";
    const char *pad = indent ? "    " : "";
    char out[UI_LC_LINE_LEN];
    bool first = true;
    size_t prefix_len;
    size_t max_text;

    while (row < UI_LC_MAX_LINES)
    {
        size_t span;

        prefix_len = first ? (3u + strlen(pad)) : 7u;
        max_text = UI_LC_LINE_LEN > prefix_len + 1u ? UI_LC_LINE_LEN - prefix_len - 1u : 0u;
        if (max_text == 0u)
            break;

        while (*p == ' ')
            ++p;
        span = lc_snapshot_wrap_span(p, max_text);

        if (first)
            snprintf(out, sizeof(out), "%02d %s%.*s", line_no, pad, (int)span, p);
        else
            snprintf(out, sizeof(out), "   ... %.*s", (int)span, p);

        lc_snapshot_put_line(f, row++, out, selected);
        if (!p[span])
            break;

        p += span;
        first = false;
    }

    return row;
}


