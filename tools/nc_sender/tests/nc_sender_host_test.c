/* Host tests for the NC sender core and the Grbl protocol client.

   The fake controller is a scripted Grbl: it answers every received line with
   "ok" (or a scripted "error:"), remembers what it received, and can push
   status reports and a greeting. No serial port is involved. */

#include "nc_sender.h"
#include "nc_send_job.h"
#include "grbl_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_LINE_MAX 128
#define FAKE_LINES_MAX 256
#define FAKE_TX_MAX 1024

typedef struct {
    char lines[FAKE_LINES_MAX][FAKE_LINE_MAX];
    unsigned line_count;
    char pending[FAKE_LINE_MAX];
    size_t pending_len;
    char tx[FAKE_TX_MAX];
    size_t tx_len;
    bool auto_ok;
    bool fail_next;
    bool opened;
    bool closed;
    unsigned now_ms;
    char realtime[16];
    unsigned realtime_len;
    unsigned status_queries;
} fake_controller_t;

static void fake_push(fake_controller_t *fake, const char *text)
{
    size_t len = strlen(text);

    if (fake->tx_len + len >= sizeof(fake->tx))
        return;
    memcpy(fake->tx + fake->tx_len, text, len);
    fake->tx_len += len;
}

static bool fake_open(void *ctx, const char *device, unsigned baud)
{
    fake_controller_t *fake = ctx;

    (void)device;
    (void)baud;
    fake->opened = true;
    return true;
}

static void fake_close(void *ctx)
{
    ((fake_controller_t *)ctx)->closed = true;
}

static void fake_complete_line(fake_controller_t *fake, const char *line)
{
    if (fake->line_count < FAKE_LINES_MAX)
        snprintf(fake->lines[fake->line_count++], FAKE_LINE_MAX, "%s", line);

    if (line[0] == '?' || line[0] == '!' || line[0] == '~') {
        if (line[0] == '?')
            fake_push(fake, "<Idle|WPos:1.500,2.500,0.000|FS:100,500>\r\n");
        return;
    }
    if (fake->fail_next) {
        fake->fail_next = false;
        fake_push(fake, "error:20\r\n");
        return;
    }
    if (fake->auto_ok)
        fake_push(fake, "ok\r\n");
}

static int fake_write(void *ctx, const char *data, size_t len)
{
    fake_controller_t *fake = ctx;
    size_t i;

    for (i = 0; i < len; i++) {
        char c = data[i];
        if (c == '?') {
            /* Grbl real-time status request: no line terminator. */
            fake->status_queries++;
            fake_push(fake, "<Idle|WPos:1.500,2.500,0.000|FS:100,500>\r\n");
            continue;
        }
        if (c == 0x18 || c == '!' || c == '~') {
            if (fake->realtime_len < sizeof(fake->realtime))
                fake->realtime[fake->realtime_len++] = c;
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (fake->pending_len) {
                fake->pending[fake->pending_len] = '\0';
                fake_complete_line(fake, fake->pending);
                fake->pending_len = 0u;
            }
            continue;
        }
        if (fake->pending_len + 1u < sizeof(fake->pending))
            fake->pending[fake->pending_len++] = c;
    }
    return (int)len;
}

static int fake_read(void *ctx, char *buf, size_t len)
{
    fake_controller_t *fake = ctx;
    size_t take = fake->tx_len < len ? fake->tx_len : len;

    if (take == 0u)
        return 0;
    memcpy(buf, fake->tx, take);
    memmove(fake->tx, fake->tx + take, fake->tx_len - take);
    fake->tx_len -= take;
    return (int)take;
}

static unsigned fake_elapsed_ms(void *ctx)
{
    return ((fake_controller_t *)ctx)->now_ms;
}

static void fake_sleep_ms(void *ctx, unsigned ms)
{
    ((fake_controller_t *)ctx)->now_ms += ms;
}

static const grbl_port_t g_fake_port = {
    fake_open, fake_close, fake_write, fake_read, fake_elapsed_ms, fake_sleep_ms
};

/* ------------------------------------------------------------------ */

static bool contains(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

static void build_cycle_program(nc_document_t *doc)
{
    nc_document_init(doc);
    (void)nc_insert_line(doc, 0, "(two-line Fanuc roughing)");
    (void)nc_insert_line(doc, 1, "G21 G18 G90");
    (void)nc_insert_line(doc, 2, "G0 X54 Z2");
    (void)nc_insert_line(doc, 3, "G71 U1 R1");
    (void)nc_insert_line(doc, 4, "G71 P100 Q200 U0.5 W0.25 F300");
    (void)nc_insert_line(doc, 5, "N100 G1 X50 Z0");
    (void)nc_insert_line(doc, 6, "G1 X50 Z-10");
    (void)nc_insert_line(doc, 7, "N200 G1 X40 Z-10");
    (void)nc_insert_line(doc, 8, "G0 X80 Z0");
}

/* Grbl target: cycles expanded, G7/G8 gone, X in radius. */
static int test_expansion_grbl(void)
{
    nc_document_t doc;
    nc_sender_target_t target = nc_sender_grbl_target();
    nc_sender_t sender;
    char line[NC_MAX_LINE_LEN];
    char all[4096];
    unsigned count = 0;

    build_cycle_program(&doc);
    all[0] = '\0';
    nc_sender_begin(&sender, &doc, &target);
    for (;;) {
        size_t source_line = 0u;
        nc_sender_result_t result = nc_sender_next(&sender, line, sizeof(line),
                                                   &source_line);
        if (result == NC_SENDER_DONE)
            break;
        if (result == NC_SENDER_ERROR) {
            printf("FAIL grbl expansion error %s\n",
                   g7x_result_text(nc_sender_last_error(&sender)));
            return 1;
        }
        if (!nc_sender_line_sendable(line))
            continue;
        if (strlen(all) + strlen(line) + 2u >= sizeof(all))
            break;
        strcat(all, line);
        strcat(all, "\n");
        count++;
    }
    if (count == 0u || !contains(all, "G0 X27 Z2")) {
        printf("FAIL grbl expansion X conversion\n%s\n", all);
        return 1;
    }
    if (!contains(all, "G0 X40 Z0") || !contains(all, "G0 X26.25") ||
        !contains(all, "G0 Z1.250") || !contains(all, "G1 X25 Z0.000") ||
        !contains(all, "G1 X20 Z-10.000")) {
        printf("FAIL grbl expansion cycle lines\n%s\n", all);
        return 1;
    }
    if (contains(all, "G71") || contains(all, "G80") || contains(all, "(two-line"))
        return 1;
    return 0;
}

/* uCNC target: the program is passed through as written. */
static int test_expansion_ucnc(void)
{
    nc_document_t doc;
    nc_sender_target_t target = nc_sender_ucnc_target();
    nc_sender_t sender;
    char line[NC_MAX_LINE_LEN];
    char all[4096];
    bool saw_pass_through = false;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G7");
    (void)nc_insert_line(&doc, 1, "G0 X54 Z2");
    (void)nc_insert_line(&doc, 2, "G1 X50 Z-20");
    all[0] = '\0';
    nc_sender_begin(&sender, &doc, &target);
    for (;;) {
        nc_sender_result_t result = nc_sender_next(&sender, line, sizeof(line), NULL);
        if (result != NC_SENDER_OK)
            break;
        if (strcmp(line, "G7") == 0)
            saw_pass_through = true;
        if (strlen(all) + strlen(line) + 2u < sizeof(all)) {
            strcat(all, line);
            strcat(all, "\n");
        }
    }
    if (!saw_pass_through || !contains(all, "G0 X54 Z2") ||
        !contains(all, "G1 X50 Z-20"))
        return 1;
    return 0;
}

/* Radius mode (G8) must survive a Grbl conversion without halving X again. */
static int test_expansion_radius_mode(void)
{
    nc_document_t doc;
    nc_sender_target_t target = nc_sender_grbl_target();
    nc_sender_t sender;
    char line[NC_MAX_LINE_LEN];
    char all[512];

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G8");
    (void)nc_insert_line(&doc, 1, "G0 X10 Z0");
    all[0] = '\0';
    nc_sender_begin(&sender, &doc, &target);
    for (;;) {
        nc_sender_result_t result = nc_sender_next(&sender, line, sizeof(line), NULL);
        if (result != NC_SENDER_OK)
            break;
        if (!nc_sender_line_sendable(line))
            continue;
        strcat(all, line);
        strcat(all, "\n");
    }
    if (!contains(all, "G0 X10 Z0") || contains(all, "G8"))
        return 1;
    return 0;
}

static int test_threading_rejected(void)
{
    nc_document_t doc;
    nc_sender_target_t target = nc_sender_grbl_target();
    nc_sender_t sender;
    char line[NC_MAX_LINE_LEN];
    nc_sender_result_t result = NC_SENDER_DONE;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G0 X40 Z2");
    (void)nc_insert_line(&doc, 1, "G33 X38 Z-20 K1.5");
    nc_sender_begin(&sender, &doc, &target);
    while (result == NC_SENDER_DONE || result == NC_SENDER_OK)
        result = nc_sender_next(&sender, line, sizeof(line), NULL);
    if (result != NC_SENDER_ERROR ||
        nc_sender_last_error(&sender) != G7X_UNSUPPORTED)
        return 1;
    return 0;
}

static int test_grbl_protocol(void)
{
    fake_controller_t fake;
    grbl_stream_t stream;

    memset(&fake, 0, sizeof(fake));
    fake.auto_ok = true;
    fake_push(&fake, "Grbl 1.1f ['$' for help]\r\n");
    grbl_stream_init(&stream, &g_fake_port, &fake);
    if (!grbl_stream_open(&stream, "fake", 115200u))
        return 1;
    grbl_stream_poll(&stream);
    if (!stream.connected)
        return 1;

    if (!grbl_stream_send(&stream, "G0 X0 Z0") || !grbl_stream_busy(&stream))
        return 1;
    grbl_stream_poll(&stream);
    if (!grbl_stream_ready(&stream) || stream.acknowledged != 1u)
        return 1;

    fake.fail_next = true;
    if (!grbl_stream_send(&stream, "G1 X1"))
        return 1;
    grbl_stream_poll(&stream);
    if (stream.state != GRBL_STATE_ERROR || stream.errors != 1u ||
        !contains(stream.last_error, "error:20"))
        return 1;

    /* A status query returns the live position and rates. */
    stream.state = GRBL_STATE_IDLE;
    grbl_stream_query_status(&stream);
    grbl_stream_poll(&stream);
    if (!stream.have_wpos || stream.wpos[0] < 1.4f || stream.wpos[0] > 1.6f ||
        stream.feed < 99.0f || stream.spindle < 499.0f)
        return 1;

    grbl_stream_feed_hold(&stream);
    grbl_stream_resume(&stream);
    grbl_stream_soft_reset(&stream);
    if (fake.realtime_len != 3u || fake.realtime[0] != '!' ||
        fake.realtime[1] != '~' || fake.realtime[2] != (char)0x18)
        return 1;
    grbl_stream_close(&stream);
    if (!fake.closed)
        return 1;
    return 0;
}

static int test_send_job(void)
{
    fake_controller_t fake;
    nc_document_t doc;
    nc_sender_target_t target = nc_sender_grbl_target();
    nc_send_job_t job;
    unsigned guard = 0u;
    unsigned i;

    memset(&fake, 0, sizeof(fake));
    fake.auto_ok = true;
    fake_push(&fake, "Grbl 1.1f ['$' for help]\r\n");
    build_cycle_program(&doc);

    if (!nc_send_job_start(&job, &doc, &target, &g_fake_port, &fake,
                           "fake", 115200u))
        return 1;
    while (nc_send_job_active(&job) && guard++ < 400u)
        nc_send_job_pump(&job);
    if (!job.finished) {
        printf("FAIL job did not finish: %s\n", nc_send_job_status(&job));
        return 1;
    }
    if (job.lines_sent < 8u)
        return 1;
    for (i = 0; i < fake.line_count; i++) {
        if (!strcmp(fake.lines[i], "G71 U1 R1") ||
            !strcmp(fake.lines[i], "G71 P100 Q200 U0.5 W0.25 F300") ||
            contains(fake.lines[i], "(two-line") ||
            contains(fake.lines[i], "G80"))
            return 1;
        if (strstr(fake.lines[i], "X54"))
            return 1;
    }
    if (strcmp(fake.lines[0], "G21 G18 G90") != 0)
        return 1;
    nc_send_job_close(&job);
    return 0;
}

int main(void)
{
    int fails = 0;

    fails += test_expansion_grbl();
    fails += test_expansion_ucnc();
    fails += test_expansion_radius_mode();
    fails += test_threading_rejected();
    fails += test_grbl_protocol();
    fails += test_send_job();
    if (fails) {
        printf("NC sender host tests failed: %d\n", fails);
        return 1;
    }
    printf("NC sender host tests passed\n");
    return 0;
}
