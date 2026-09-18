#ifndef GRBL_STREAM_H
#define GRBL_STREAM_H

/* Grbl 1.1 line protocol client: one line at a time, wait for "ok", react to
   "error:", alarms and status reports. The caller pumps it; nothing here
   blocks or sleeps on its own. */

#include "grbl_port.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRBL_RX_MAX 256
#define GRBL_LINE_MAX 160

typedef enum {
    GRBL_STATE_CLOSED = 0,
    GRBL_STATE_CONNECTING,
    GRBL_STATE_IDLE,     /* ready for the next line */
    GRBL_STATE_BUSY,     /* line written, waiting for "ok" */
    GRBL_STATE_HELD,     /* feed hold acknowledged by the controller */
    GRBL_STATE_ALARM,
    GRBL_STATE_ERROR
} grbl_state_t;

typedef struct {
    const grbl_port_t *port;
    void *ctx;
    grbl_state_t state;
    char rx[GRBL_RX_MAX];
    size_t rx_len;
    char last_line[GRBL_LINE_MAX];
    char last_error[64];
    char last_response[64];
    unsigned sent;
    unsigned acknowledged;
    unsigned errors;
    unsigned opened_ms;
    bool connected;
    bool soft_reset_seen;
    /* Live machine state from the last status report. */
    bool have_status;
    bool have_mpos;
    bool have_wpos;
    float mpos[3];
    float wpos[3];
    float feed;
    float spindle;
    char status_state[16];
} grbl_stream_t;

void grbl_stream_init(grbl_stream_t *stream,
                      const grbl_port_t *port,
                      void *ctx);
bool grbl_stream_open(grbl_stream_t *stream,
                      const char *device,
                      unsigned baud);
void grbl_stream_close(grbl_stream_t *stream);

/* Consume every byte the controller has sent and update the state. */
void grbl_stream_poll(grbl_stream_t *stream);

/* Ready to accept the next program line. */
bool grbl_stream_ready(const grbl_stream_t *stream);
bool grbl_stream_failed(const grbl_stream_t *stream);
bool grbl_stream_busy(const grbl_stream_t *stream);

/* Send one line (terminator added) when the stream is idle. */
bool grbl_stream_send(grbl_stream_t *stream, const char *line);

/* Control commands. */
void grbl_stream_query_status(grbl_stream_t *stream);
void grbl_stream_feed_hold(grbl_stream_t *stream);
void grbl_stream_resume(grbl_stream_t *stream);
void grbl_stream_soft_reset(grbl_stream_t *stream);
void grbl_stream_unlock(grbl_stream_t *stream);
void grbl_stream_send_realtime(grbl_stream_t *stream, char code);

/* Wait up to `timeout_ms` for the pending line to be acknowledged. */
bool grbl_stream_wait_idle(grbl_stream_t *stream, unsigned timeout_ms);

const char *grbl_stream_state_text(const grbl_stream_t *stream);

#ifdef __cplusplus
}
#endif

#endif
