#ifndef NC_SENDER_H
#define NC_SENDER_H

/* Host-side NC program sender core.

   This reuses the module code that already runs on the machine: the NC
   document/editor, the NC emitter and the G7x generator. It turns a program
   into the line stream a controller can actually execute:

   - G71/G72 contours are expanded by the shared G7x stepper, so the controller
     does not need the cycle module or an interactive preview;
   - a target without lathe word support (stock Grbl) gets G7/G8 removed and X
     converted from diameter to radius;
   - a target without G33 support rejects threading instead of sending it.

   Nothing here talks to a machine. grbl_stream.h owns the wire protocol and
   nc_send_job.h glues the two together. */

#include "nc.h"
#include "nc_emit.h"
#include "g7x.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NC_SENDER_OK = 0,
    NC_SENDER_DONE,
    NC_SENDER_ERROR
} nc_sender_result_t;

typedef struct {
    /* Grbl has no G7/G8: drop them and convert X words to radius. uCNC keeps
       the lathe words and the program as written. */
    bool supports_lathe_words;
    /* G33/G76 need a controller that synchronises the spindle. Stock Grbl does
       not, so the sender reports the line instead of streaming it. */
    bool supports_threading;
    /* Copy comments into the output. Off for the wire, on for exported files. */
    bool keep_comments;
} nc_sender_target_t;

typedef struct {
    const nc_document_t *doc;
    nc_emit_stream_t emit;
    nc_sender_target_t target;
    bool diameter_mode;      /* G7 (diameter programming) is the NC default */
    bool started;
    unsigned expanded;       /* generated cycle lines seen */
    unsigned converted;      /* lines whose X was converted to radius */
    unsigned dropped_words;  /* G7/G8 words removed */
    g7x_result_t error;
} nc_sender_t;

/* Grbl-compatible defaults: no lathe words, no threading, no comments. */
nc_sender_target_t nc_sender_grbl_target(void);
/* uCNC defaults: the machine supports G7/G8 and G33/G76. */
nc_sender_target_t nc_sender_ucnc_target(void);

void nc_sender_begin(nc_sender_t *sender,
                     const nc_document_t *doc,
                     const nc_sender_target_t *target);

/* Host file load/save for the document. The machine module uses the uCNC
   filesystem driver; a desktop build has no SD card, so the sender carries a
   stdio equivalent with the same line-length rules. */
nc_result_t nc_sender_load_file(nc_document_t *doc, const char *path);
nc_result_t nc_sender_save_file(const nc_document_t *doc, const char *path);

/* Next line for the controller, already expanded and converted. Comments are
   still returned (or dropped) according to the target so a UI can show the
   same sequence; nc_sender_line_sendable() tells the transport what to send. */
nc_sender_result_t nc_sender_next(nc_sender_t *sender,
                                  char *out,
                                  size_t out_sz,
                                  size_t *source_line);

g7x_result_t nc_sender_last_error(const nc_sender_t *sender);

/* Empty, comment-only and uCNC-only setup lines are not sent to a controller. */
bool nc_sender_line_sendable(const char *line);

#ifdef __cplusplus
}
#endif

#endif
