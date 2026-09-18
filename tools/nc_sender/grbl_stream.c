#include "grbl_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void grbl_stream_init(grbl_stream_t *stream,
                      const grbl_port_t *port,
                      void *ctx)
{
    if (!stream)
        return;
    memset(stream, 0, sizeof(*stream));
    stream->port = port;
    stream->ctx = ctx;
    stream->state = GRBL_STATE_CLOSED;
}

bool grbl_stream_open(grbl_stream_t *stream,
                      const char *device,
                      unsigned baud)
{
    if (!stream || !stream->port || !stream->port->open || !device)
        return false;
    stream->rx_len = 0u;
    stream->soft_reset_seen = false;
    stream->connected = false;
    if (!stream->port->open(stream->ctx, device, baud)) {
        stream->state = GRBL_STATE_ERROR;
        snprintf(stream->last_error, sizeof(stream->last_error),
                 "cannot open %s", device);
        return false;
    }
    stream->state = GRBL_STATE_CONNECTING;
    stream->opened_ms = stream->port->elapsed_ms ?
                        stream->port->elapsed_ms(stream->ctx) : 0u;
    /* Grbl answers a bare newline with its greeting. */
    (void)stream->port->write(stream->ctx, "\r\n", 2u);
    return true;
}

void grbl_stream_close(grbl_stream_t *stream)
{
    if (!stream || !stream->port)
        return;
    if (stream->port->close)
        stream->port->close(stream->ctx);
    stream->state = GRBL_STATE_CLOSED;
    stream->connected = false;
}

static const char *grbl_stream_trim(char *text)
{
    char *end;

    while (*text == ' ' || *text == '\t')
        text++;
    end = text + strlen(text);
    while (end > text && (end[-1] == '\r' || end[-1] == '\n' ||
                          end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    return text;
}

/* "<Idle|MPos:1.000,2.000,3.000|FS:0,0>" and similar reports. */
static void grbl_stream_parse_status(grbl_stream_t *stream, const char *report)
{
    const char *p = report;
    const char *field;

    if (*p == '<')
        p++;
    field = p;
    while (*field && *field != '|' && *field != '>')
        field++;
    if (field != p) {
        size_t len = (size_t)(field - p);
        if (len >= sizeof(stream->status_state))
            len = sizeof(stream->status_state) - 1u;
        memcpy(stream->status_state, p, len);
        stream->status_state[len] = '\0';
        if (strncmp(stream->status_state, "Hold", 4u) == 0)
            stream->state = GRBL_STATE_HELD;
        else if (strncmp(stream->status_state, "Alarm", 5u) == 0)
            stream->state = GRBL_STATE_ALARM;
    }

    {
        const char *mpos = strstr(report, "MPos:");
        const char *wpos = strstr(report, "WPos:");
        const char *fs = strstr(report, "FS:");
        if (mpos) {
            if (sscanf(mpos + 5, "%f,%f,%f", &stream->mpos[0], &stream->mpos[1],
                       &stream->mpos[2]) == 3)
                stream->have_mpos = true;
        }
        if (wpos) {
            if (sscanf(wpos + 5, "%f,%f,%f", &stream->wpos[0], &stream->wpos[1],
                       &stream->wpos[2]) == 3)
                stream->have_wpos = true;
        }
        if (fs) {
            float feed = 0.0f;
            float spindle = 0.0f;
            if (sscanf(fs + 3, "%f,%f", &feed, &spindle) == 2) {
                stream->feed = feed;
                stream->spindle = spindle;
            }
        }
    }
    stream->have_status = true;
}

static void grbl_stream_handle_line(grbl_stream_t *stream, char *line)
{
    const char *text = grbl_stream_trim(line);

    if (!*text)
        return;
    snprintf(stream->last_response, sizeof(stream->last_response), "%.48s", text);

    if (strncmp(text, "ok", 2u) == 0) {
        stream->acknowledged++;
        if (stream->state == GRBL_STATE_BUSY)
            stream->state = GRBL_STATE_IDLE;
        return;
    }
    if (strncmp(text, "error:", 6u) == 0 || strcmp(text, "error") == 0) {
        stream->errors++;
        snprintf(stream->last_error, sizeof(stream->last_error), "%.48s", text);
        stream->state = GRBL_STATE_ERROR;
        return;
    }
    if (strncmp(text, "ALARM", 5u) == 0) {
        snprintf(stream->last_error, sizeof(stream->last_error), "%.48s", text);
        stream->state = GRBL_STATE_ALARM;
        return;
    }
    if (text[0] == '<') {
        grbl_stream_parse_status(stream, text);
        return;
    }
    /* Greeting banner, with or without the "[MSG:...]" form. */
    if (strncmp(text, "Grbl", 4u) == 0 || text[0] == '[' ||
        strstr(text, "['$' for help]")) {
        stream->connected = true;
        if (stream->state == GRBL_STATE_CONNECTING)
            stream->state = GRBL_STATE_IDLE;
        return;
    }
}

void grbl_stream_poll(grbl_stream_t *stream)
{
    char buf[64];

    if (!stream || !stream->port || !stream->port->read)
        return;
    for (;;) {
        int got = stream->port->read(stream->ctx, buf, sizeof(buf));
        int i;
        if (got <= 0)
            break;
        for (i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (stream->rx_len) {
                    stream->rx[stream->rx_len] = '\0';
                    grbl_stream_handle_line(stream, stream->rx);
                    stream->rx_len = 0u;
                }
                continue;
            }
            if (stream->rx_len + 1u < sizeof(stream->rx))
                stream->rx[stream->rx_len++] = c;
            else
                stream->rx_len = 0u; /* oversized noise: drop the line */
        }
    }
}

bool grbl_stream_ready(const grbl_stream_t *stream)
{
    if (!stream)
        return false;
    return stream->state == GRBL_STATE_IDLE ||
           stream->state == GRBL_STATE_CONNECTING;
}

bool grbl_stream_failed(const grbl_stream_t *stream)
{
    return !stream || stream->state == GRBL_STATE_ERROR ||
           stream->state == GRBL_STATE_ALARM;
}

bool grbl_stream_busy(const grbl_stream_t *stream)
{
    return stream && stream->state == GRBL_STATE_BUSY;
}

void grbl_stream_send_realtime(grbl_stream_t *stream, char code)
{
    if (!stream || !stream->port || !stream->port->write)
        return;
    if (stream->state == GRBL_STATE_CLOSED)
        return;
    (void)stream->port->write(stream->ctx, &code, 1u);
}

bool grbl_stream_send(grbl_stream_t *stream, const char *line)
{
    char out[GRBL_LINE_MAX + 4u];
    size_t len;

    if (!stream || !line || !stream->port || !stream->port->write)
        return false;
    if (!grbl_stream_ready(stream))
        return false;

    len = strlen(line);
    if (len >= sizeof(out) - 2u)
        return false;
    memcpy(out, line, len);
    out[len++] = '\n';
    if (stream->port->write(stream->ctx, out, len) != (int)len) {
        snprintf(stream->last_error, sizeof(stream->last_error), "write failed");
        stream->state = GRBL_STATE_ERROR;
        return false;
    }
    snprintf(stream->last_line, sizeof(stream->last_line), "%.*s",
             (int)sizeof(stream->last_line) - 1, line);
    stream->sent++;
    stream->state = GRBL_STATE_BUSY;
    return true;
}

void grbl_stream_query_status(grbl_stream_t *stream)
{
    grbl_stream_send_realtime(stream, '?');
}

void grbl_stream_feed_hold(grbl_stream_t *stream)
{
    grbl_stream_send_realtime(stream, '!');
    if (stream && stream->state == GRBL_STATE_BUSY)
        stream->state = GRBL_STATE_HELD;
}

void grbl_stream_resume(grbl_stream_t *stream)
{
    grbl_stream_send_realtime(stream, '~');
    if (stream && stream->state == GRBL_STATE_HELD)
        stream->state = GRBL_STATE_BUSY;
}

void grbl_stream_soft_reset(grbl_stream_t *stream)
{
    grbl_stream_send_realtime(stream, 0x18);
    if (stream) {
        stream->soft_reset_seen = true;
        stream->connected = false;
        stream->state = GRBL_STATE_CONNECTING;
        stream->rx_len = 0u;
    }
}

void grbl_stream_unlock(grbl_stream_t *stream)
{
    if (grbl_stream_ready(stream))
        (void)grbl_stream_send(stream, "$X");
}

bool grbl_stream_wait_idle(grbl_stream_t *stream, unsigned timeout_ms)
{
    unsigned start;

    if (!stream || !stream->port)
        return false;
    start = stream->port->elapsed_ms ? stream->port->elapsed_ms(stream->ctx) : 0u;
    for (;;) {
        grbl_stream_poll(stream);
        if (grbl_stream_ready(stream))
            return true;
        if (grbl_stream_failed(stream))
            return false;
        if (stream->port->elapsed_ms) {
            unsigned now = stream->port->elapsed_ms(stream->ctx);
            if (now - start >= timeout_ms)
                return false;
        }
        if (stream->port->sleep_ms)
            stream->port->sleep_ms(stream->ctx, 2u);
        else
            return false;
    }
}

const char *grbl_stream_state_text(const grbl_stream_t *stream)
{
    if (!stream)
        return "?";
    switch (stream->state) {
    case GRBL_STATE_CLOSED: return "closed";
    case GRBL_STATE_CONNECTING: return stream->connected ? "connected" : "connecting";
    case GRBL_STATE_IDLE: return "idle";
    case GRBL_STATE_BUSY: return "sending";
    case GRBL_STATE_HELD: return "hold";
    case GRBL_STATE_ALARM: return "alarm";
    case GRBL_STATE_ERROR: return "error";
    }
    return "?";
}
