#ifndef NC_SEND_JOB_H
#define NC_SEND_JOB_H

/* Glue between the NC sender and the Grbl protocol: pull one expanded line at a
   time and hand it over when the controller reports ready. */

#include "nc_sender.h"
#include "grbl_stream.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    nc_sender_t sender;
    grbl_stream_t grbl;
    unsigned lines_sent;
    unsigned lines_skipped;
    unsigned started_ms;
    bool waiting_hello;
    bool finished;
    bool failed;
    bool aborted;
    char message[96];
} nc_send_job_t;

bool nc_send_job_start(nc_send_job_t *job,
                       const nc_document_t *doc,
                       const nc_sender_target_t *target,
                       const grbl_port_t *port,
                       void *port_ctx,
                       const char *device,
                       unsigned baud);
/* Send the next line when possible and read whatever the controller sent. */
void nc_send_job_pump(nc_send_job_t *job);
bool nc_send_job_active(const nc_send_job_t *job);
void nc_send_job_abort(nc_send_job_t *job);
void nc_send_job_close(nc_send_job_t *job);

const char *nc_send_job_status(const nc_send_job_t *job);

#ifdef __cplusplus
}
#endif

#endif
