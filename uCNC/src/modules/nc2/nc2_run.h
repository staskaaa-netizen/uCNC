#ifndef NC2_RUN_H
#define NC2_RUN_H

#include "nc2.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The run: the panel hands the machine what `nc2_emit` says the program does,
   one unit at a time, and the DRO reads the machine's own numbers back.

   `nc` sends the program as written and the machine's own G7x parser expands the
   cycles; `nc2` expands them in the panel, so what travels is plain motion. That
   is the sender `--emit2test` compares against `nc`'s expansion, and this file is
   the pacer over it: the same one-unit-at-a-time rule, so the mark is the line
   the machine is actually on. */

void nc2_run_init(void);
/* The pacer, from the main loop: hand over one line and wait, so a program runs
   while the panel is on any screen and the operator can still hold or stop it. */
void nc2_run_pace(void);

/* Run the program from `line` to its end. False when there is nothing to run. */
bool nc2_run_start(const nc2_document_t *doc, size_t line);
/* Hand the machine the one unit that owns `line` - a whole G7x block, or the
   single line. This is `1 SINGLE`, and it is not a program run. */
bool nc2_run_send_unit(const nc2_document_t *doc, size_t line);

void nc2_run_reset(void);
void nc2_run_stop(void);
bool nc2_run_toggle_hold(void);
void nc2_run_set_line(const nc2_document_t *doc, size_t line);

bool nc2_run_active(void);
bool nc2_run_hold(void);
bool nc2_run_done(void);
/* True while a program run is armed - a panel block must not cut into it. */
bool nc2_run_streaming(void);
/* True while the pacer is expanding a block: its lines go out back to back,
   because a contour is one cut, without a wait between rows. */
bool nc2_run_expanding(void);
/* The sender's position, and the line the pane marks (the unit in play, kept
   until the operator takes the cursor). */
size_t nc2_run_line(void);
size_t nc2_run_display_line(void);
bool nc2_run_running(size_t *line);

uint8_t nc2_run_error(void);
size_t nc2_run_error_line(void);

/* Hand one line to the machine on the panel's own reader - a jog, a zero, a
   spindle start - and refuse the lines this layer keeps to itself. */
bool nc2_run_send_line(const char *line);

#ifdef __cplusplus
}
#endif

#endif
