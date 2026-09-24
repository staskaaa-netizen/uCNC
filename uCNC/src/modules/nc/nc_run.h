#ifndef NC_RUN_H
#define NC_RUN_H

#include "nc.h"

#include <stdbool.h>
#include <stdint.h>
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
/* The pacer: hands the machine one block and waits until it has run before
   handing over the next. Called from the main loop, so a program runs while the
   panel is on any screen. */
void nc_run_pace(void);
bool nc_run_arm(const nc_document_t *doc, size_t line);
void nc_run_reset(void);
void nc_run_stop(void);
bool nc_run_toggle_hold(void);
bool nc_run_active(void);
bool nc_run_hold(void);
bool nc_run_done(void);
size_t nc_run_line(void);
/* The line the pane marks. *Not* the sender's position (`nc_run_line()`): the
   mark is the line in play - the line the operator stepped from, or the unit a
   run is walking through - and a unit that has finished keeps it until the
   operator takes the cursor with a line key, arms a new step or reloads the
   program. The sender walks on without it, which is how the pane once came to
   mark a cycle header that had never run. */
size_t nc_run_display_line(void);
/* True while the machine is still running the unit the pacer handed over. */
bool nc_run_running(size_t *line);
uint8_t nc_run_error(void);
size_t nc_run_error_line(void);
void nc_run_set_line(const nc_document_t *doc, size_t line);
bool nc_run_streaming(void);
bool nc_run_send_line(const char *line);
bool nc_run_send_document_line(const nc_document_t *doc, size_t line);
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
