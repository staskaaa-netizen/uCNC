#include "nc_send_job.h"

#include <stdio.h>
#include <string.h>

/* Controllers that never greet still accept lines after this grace period. */
#define NC_SEND_JOB_HELLO_MS 1500u

bool nc_send_job_start(nc_send_job_t *job,
                       const nc_document_t *doc,
                       const nc_sender_target_t *target,
                       const grbl_port_t *port,
                       void *port_ctx,
                       const char *device,
                       unsigned baud)
{
    if (!job || !port)
        return false;
    memset(job, 0, sizeof(*job));
    nc_sender_begin(&job->sender, doc, target);
    grbl_stream_init(&job->grbl, port, port_ctx);
    if (!grbl_stream_open(&job->grbl, device, baud)) {
        job->failed = true;
        snprintf(job->message, sizeof(job->message), "%s",
                 job->grbl.last_error[0] ? job->grbl.last_error : "open failed");
        return false;
    }
    job->waiting_hello = true;
    snprintf(job->message, sizeof(job->message), "connecting");
    return true;
}

void nc_send_job_pump(nc_send_job_t *job)
{
    char line[NC_MAX_LINE_LEN];

    if (!job || job->finished || job->failed || job->aborted)
        return;

    grbl_stream_poll(&job->grbl);

    if (job->waiting_hello) {
        unsigned elapsed = job->grbl.port->elapsed_ms ?
                           job->grbl.port->elapsed_ms(job->grbl.ctx) : 0u;
        if (!job->grbl.connected && elapsed < NC_SEND_JOB_HELLO_MS) {
            snprintf(job->message, sizeof(job->message), "connecting");
            return;
        }
        job->waiting_hello = false;
        job->started_ms = elapsed;
    }

    if (grbl_stream_failed(&job->grbl)) {
        job->failed = true;
        snprintf(job->message, sizeof(job->message), "controller %s: %s",
                 grbl_stream_state_text(&job->grbl),
                 job->grbl.last_error[0] ? job->grbl.last_error : "fault");
        return;
    }
    if (!grbl_stream_ready(&job->grbl))
        return;

    for (;;) {
        nc_sender_result_t result;
        size_t source_line = 0u;

        result = nc_sender_next(&job->sender, line, sizeof(line), &source_line);
        if (result == NC_SENDER_DONE) {
            job->finished = true;
            snprintf(job->message, sizeof(job->message), "sent %u lines",
                     job->lines_sent);
            return;
        }
        if (result == NC_SENDER_ERROR) {
            job->failed = true;
            snprintf(job->message, sizeof(job->message), "line %lu: %s",
                     (unsigned long)(source_line + 1u),
                     g7x_result_text(nc_sender_last_error(&job->sender)));
            return;
        }
        if (!nc_sender_line_sendable(line)) {
            job->lines_skipped++;
            continue;
        }
        if (!grbl_stream_send(&job->grbl, line)) {
            if (!job->grbl.last_error[0])
                snprintf(job->message, sizeof(job->message), "send failed");
            else
                snprintf(job->message, sizeof(job->message), "%s",
                         job->grbl.last_error);
            job->failed = grbl_stream_failed(&job->grbl);
            return;
        }
        job->lines_sent++;
        snprintf(job->message, sizeof(job->message), "line %lu sent",
                 (unsigned long)(source_line + 1u));
        return;
    }
}

bool nc_send_job_active(const nc_send_job_t *job)
{
    if (!job)
        return false;
    return !job->finished && !job->failed && !job->aborted;
}

void nc_send_job_abort(nc_send_job_t *job)
{
    if (!job || job->finished || job->failed)
        return;
    grbl_stream_feed_hold(&job->grbl);
    grbl_stream_soft_reset(&job->grbl);
    job->aborted = true;
    snprintf(job->message, sizeof(job->message), "aborted");
}

void nc_send_job_close(nc_send_job_t *job)
{
    if (!job)
        return;
    grbl_stream_close(&job->grbl);
}

const char *nc_send_job_status(const nc_send_job_t *job)
{
    return job ? job->message : "";
}
