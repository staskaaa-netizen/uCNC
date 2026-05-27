#include "leancam_run.h"
#include "leancam_text.h"
#include "leancam_validate.h"

#include <stdio.h>

static bool g_lc_run_sim_armed = false;

void lc_run_init(void)
{
    g_lc_run_sim_armed = false;
}

bool lc_run_sim_armed(void)
{
    return g_lc_run_sim_armed;
}

void lc_run_sim_set_armed(bool armed)
{
    g_lc_run_sim_armed = armed;
}

bool lc_run_sim_toggle_arm(void)
{
    g_lc_run_sim_armed = !g_lc_run_sim_armed;
    return g_lc_run_sim_armed;
}

bool lc_run_emit_banner(int run_line_no, const char *tool, lc_gcode_send_fn send, void *user)
{
    char buf[96];

    if (!send)
        return false;

    snprintf(buf, sizeof(buf), "(--- LeanCam L%d ---)", run_line_no);
    if (!send(buf, user))
        return false;

    if (tool && tool[0])
    {
        snprintf(buf, sizeof(buf), "(--- %.84s ---)", tool);
        if (!send(buf, user))
            return false;
    }

    return true;
}

const char *lc_run_gcode_result_name(lc_gcode_result_t r)
{
    switch (r)
    {
        case LC_GCODE_OK:            return "ok";
        case LC_GCODE_UNSUPPORTED:   return "unsupported";
        case LC_GCODE_NO_SETUP:      return "no setup";
        case LC_GCODE_BAD_FIELD:     return "bad field";
        case LC_GCODE_STREAM_REJECT: return "write failed";
        default:                     return "gcode failed";
    }
}

static int lc_run_discard_line(const char *line, void *user)
{
    (void)line;
    (void)user;
    return 1;
}

static bool lc_run_is_context_line(const char *line)
{
    return line &&
           (lc_text_command_is(line, "SETUP") ||
            lc_text_command_is(line, "TOOLCALL") ||
            lc_text_command_is(line, "TOOL") ||
            line[0] == '(');
}

static const char *lc_run_setup_for_line(const program_t *prog, int before_or_at)
{
    int i;

    if (!prog)
        return NULL;
    if (before_or_at >= prog->count)
        before_or_at = prog->count - 1;

    for (i = before_or_at; i >= 0; --i)
        if (lc_text_command_is(prog->lines[i], "SETUP"))
            return prog->lines[i];

    return NULL;
}

lc_gcode_result_t lc_run_preflight_program(const program_t *prog,
                                           char *err,
                                           unsigned err_len,
                                           int *err_line_out,
                                           int *made_out)
{
    return lc_run_emit_selected_range(prog,
                                      0,
                                      prog ? prog->count - 1 : -1,
                                      lc_run_discard_line,
                                      NULL,
                                      err,
                                      err_len,
                                      err_line_out,
                                      made_out);
}

static bool lc_run_emit_setup_comment(const program_t *prog,
                                      lc_gcode_send_fn send,
                                      void *send_user)
{
    const char *setup = NULL;
    char buf[MAX_LEN + 12];

    if (!send)
        return false;
    if (!prog)
        return true;

    setup = lc_run_setup_for_line(prog, prog->count - 1);
    if (!setup || !setup[0])
        return true;

    snprintf(buf, sizeof(buf), "(LC %s)", setup);
    return send(buf, send_user);
}

lc_gcode_result_t lc_run_emit_program(const program_t *prog,
                                      lc_gcode_send_fn send,
                                      void *send_user,
                                      char *err,
                                      unsigned err_len,
                                      int *err_line_out,
                                      int *made_out)
{
    lc_gcode_result_t r;

    if (err && err_len)
        err[0] = 0;
    if (err_line_out)
        *err_line_out = 0;
    if (made_out)
        *made_out = 0;
    if (!prog || prog->count <= 0 || !send)
        return LC_GCODE_BAD_FIELD;

    if (!leancam_gcode_emit_program_header(send, send_user))
        return LC_GCODE_STREAM_REJECT;
    if (!lc_run_emit_setup_comment(prog, send, send_user))
        return LC_GCODE_STREAM_REJECT;

    r = lc_run_emit_selected_range(prog,
                                   0,
                                   prog->count - 1,
                                   send,
                                   send_user,
                                   err,
                                   err_len,
                                   err_line_out,
                                   made_out);
    if (r != LC_GCODE_OK)
        return r;

    if (!leancam_gcode_emit_program_footer_ex(send, send_user, err, err_len))
        return LC_GCODE_STREAM_REJECT;

    return LC_GCODE_OK;
}

lc_gcode_result_t lc_run_emit_selected_range(const program_t *prog,
                                             int start,
                                             int end,
                                             lc_gcode_send_fn send,
                                             void *send_user,
                                             char *err,
                                             unsigned err_len,
                                             int *err_line_out,
                                             int *made_out)
{
    int i;
    int made = 0;
    lc_gcode_result_t r = LC_GCODE_OK;

    if (err && err_len)
        err[0] = 0;
    if (err_line_out)
        *err_line_out = start + 1;
    if (made_out)
        *made_out = 0;

    if (!prog || start < 0 || end < start || end >= prog->count || !send)
        return LC_GCODE_BAD_FIELD;

    for (i = start; i <= end; ++i)
    {
        const char *line = prog->lines[i];
        const char *setup = NULL;
        const char *tool = NULL;

        if (!line || !line[0])
            continue;
        if (lc_run_is_context_line(line))
            continue;

        setup = lc_run_setup_for_line(prog, i);
        tool = lc_validate_effective_tool_for_cycle(prog, i, line);

        if (!lc_run_emit_banner(i + 1, tool, send, send_user))
        {
            if (err_line_out)
                *err_line_out = i + 1;
            if (made_out)
                *made_out = made;
            return LC_GCODE_STREAM_REJECT;
        }

        r = leancam_gcode_run_program_line_ex(line, setup, tool, send, send_user, err, err_len);
        if (r != LC_GCODE_OK)
        {
            if (err_line_out)
                *err_line_out = i + 1;
            if (made_out)
                *made_out = made;
            return r;
        }

        made++;
    }

    if (made_out)
        *made_out = made;
    return LC_GCODE_OK;
}
