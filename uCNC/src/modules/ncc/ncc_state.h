#ifndef NCC_STATE_H
#define NCC_STATE_H

#include "ncc.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the panel remembers, and the machine's own numbers.

   The file is `/D/nc_state.txt` and its keys are nc's (`MODE=`, `SPINDLE=`, one
   key per mode holding the file that mode had open), so a card carries its state
   between the two modules - and a station's card keeps working when the panel is
   switched over. The cursor is remembered for the session only, the way nc does
   it. */

#define NCC_STATE_PATH "/D/nc_state.txt"

/* The tool table, which ncc's TOOLS screen edits like any other file: the same
   path nc uses, so the two modules read one table. */
#define NCC_TOOL_PATH "/D/nc/files/tool.t"

/* ncc's screens, and the keys nc's state file uses for them, so a card written
   by one module reads in the other. The card's list is a view, not a mode, and
   is not remembered. */
typedef enum {
    NCC_MODE_PROGRAM = 0,
    NCC_MODE_MANUAL,
    NCC_MODE_TOOLS,
    NCC_MODE_RUN,
    NCC_MODE_COUNT
} ncc_mode_t;

/* The machine as the DRO reads it: what it is doing, where it is, the feed and
   the spindle's speed. */
typedef struct {
    uint16_t exec_state;
    float x;
    float z;
    float feed;
    unsigned spindle;
} ncc_runtime_state_t;

void ncc_state_init(void);
void ncc_state_set_mode(ncc_mode_t mode);
ncc_mode_t ncc_state_mode(void);
void ncc_state_remember_path(ncc_mode_t mode, const char *path);
const char *ncc_state_path(ncc_mode_t mode);
/* Where the cursor was in the file that mode had open. Asked for by the tools
   screen, which reads the program's own answer without opening it. */
void ncc_state_remember_cursor(const ncc_document_t *doc);
/* Read the file the mode last had open into `doc`, and put its cursor back.
   False when nothing is remembered, or the card cannot answer. */
bool ncc_state_load_document(ncc_mode_t mode, ncc_document_t *doc);
void ncc_state_save(void);
/* Write the state file if anything has changed since the last flush. */
void ncc_state_flush(void);

/* The speed the MANUAL spindle keys start at: the last one the machine had, so
   starting the spindle keeps the operator's speed across a reboot. */
unsigned ncc_state_manual_spindle(void);
void ncc_state_remember_manual_spindle(unsigned rpm);

/* The machine's own numbers, read from the core - nothing here is remembered,
   it is asked for. */
void ncc_state_runtime(ncc_runtime_state_t *state);
/* True while the machine is doing something: the DRO appears when it is, and a
   fault must take it over as soon as it happens. */
bool ncc_state_busy(void);

#ifdef __cplusplus
}
#endif

#endif
