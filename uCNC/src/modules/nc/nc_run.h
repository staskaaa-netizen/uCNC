#ifndef NC_RUN_H
#define NC_RUN_H

#include "nc.h"
#include "nc_emit.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NC_RUN_STEP_NEEDS_PROGRAM = 0,
    NC_RUN_STEP_HOLD,
    NC_RUN_STEP_SKIPPED,
    NC_RUN_STEP_EMITTED,
    NC_RUN_STEP_COMPLETE
} nc_run_step_result_t;

void nc_run_init(void);
bool nc_run_arm(const nc_document_t *doc, size_t line);
void nc_run_reset(void);
void nc_run_stop(void);
bool nc_run_toggle_hold(void);
bool nc_run_active(void);
bool nc_run_hold(void);
bool nc_run_done(void);
size_t nc_run_line(void);
void nc_run_set_line(const nc_document_t *doc, size_t line);
void nc_run_send_line(const char *line);
bool nc_run_start_stream(const nc_document_t *doc, size_t line);
nc_run_step_result_t nc_run_step(const nc_document_t *doc,
                                 char *out,
                                 size_t out_sz,
                                 size_t *emitted_line);
nc_run_step_result_t nc_run_step_sendable(const nc_document_t *doc,
                                          char *out,
                                          size_t out_sz,
                                          size_t *emitted_line);

#ifdef __cplusplus
}
#endif

#endif
