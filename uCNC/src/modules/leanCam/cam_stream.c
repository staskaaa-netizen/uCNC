#ifdef ARDUINO_ARCH_ESP32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#endif

#include "../../cnc.h"
#include "cam_stream.h"

#include <string.h>
#include <stdio.h>

#define CAM_STREAM_QUEUE_LEN   512

/*
 * This already worked in your tree.
 * Keep it exactly like this.
 */
#define cam_stream_readonly serial_stream_readonly

#ifdef ARDUINO_ARCH_ESP32
static QueueHandle_t g_cam_q = NULL;
static TaskHandle_t  g_cam_task = NULL;
#endif

static bool g_cam_started = false;
static bool g_cam_done = true;

/* preview window shown by RA */
static cam_preview_snapshot_t g_cam_preview = {0};

/* ------------------------------------------------------------------------- */
/* stream callbacks                                                          */
/* ------------------------------------------------------------------------- */

static uint8_t cam_stream_available(void)
{
#ifdef ARDUINO_ARCH_ESP32
    if (!g_cam_q)
        return 0;

    return (uxQueueMessagesWaiting(g_cam_q) > 0) ? 1 : 0;
#else
    return 0;
#endif
}

static uint8_t cam_stream_getc(void)
{
#ifdef ARDUINO_ARCH_ESP32
    uint8_t c = 0;

    if (!g_cam_q)
        return 0;

    if (xQueueReceive(g_cam_q, &c, 0) == pdTRUE)
        return c;

    return 0;
#else
    return 0;
#endif
}

static void cam_stream_clear(void)
{
#ifdef ARDUINO_ARCH_ESP32
    if (!g_cam_q)
        return;

    xQueueReset(g_cam_q);
#endif
}

/* ------------------------------------------------------------------------- */
/* preview helpers                                                           */
/* ------------------------------------------------------------------------- */

static void cam_preview_clear(void)
{
    memset(&g_cam_preview, 0, sizeof(g_cam_preview));
}

static void cam_preview_shift_left_once(void)
{
    uint8_t i;

    if (g_cam_preview.count == 0)
        return;

    for (i = 1; i < g_cam_preview.count; i++)
    {
        g_cam_preview.line_no[i - 1] = g_cam_preview.line_no[i];
        strcpy(g_cam_preview.text[i - 1], g_cam_preview.text[i]);
    }

    g_cam_preview.line_no[g_cam_preview.count - 1] = 0;
    g_cam_preview.text[g_cam_preview.count - 1][0] = '\0';
    g_cam_preview.count--;
}

static void cam_preview_push(uint32_t line_no, const char *s)
{
    uint8_t idx;

    if (!s)
        s = "";

    if (g_cam_preview.count >= CAM_PREVIEW_LINES)
        cam_preview_shift_left_once();

    idx = g_cam_preview.count;
    if (g_cam_preview.count < CAM_PREVIEW_LINES)
        g_cam_preview.count++;

    g_cam_preview.line_no[idx] = line_no;
    strncpy(g_cam_preview.text[idx], s, CAM_PREVIEW_TEXT_LEN - 1);
    g_cam_preview.text[idx][CAM_PREVIEW_TEXT_LEN - 1] = '\0';
}

bool cam_stream_get_preview(cam_preview_snapshot_t *out)
{
    if (!out)
        return false;

    memcpy(out, &g_cam_preview, sizeof(*out));
    return true;
}

/* ------------------------------------------------------------------------- */
/* public producer                                                           */
/* ------------------------------------------------------------------------- */

bool cam_stream_send_line(const char *s, uint32_t line_no)
{
#ifdef ARDUINO_ARCH_ESP32
    const char *orig = s;

    if (!s)
        return false;

    if (!cam_stream_begin())
        return false;

    while (*s)
    {
        uint8_t c = (uint8_t)(*s++);
        if (xQueueSend(g_cam_q, &c, pdMS_TO_TICKS(100)) != pdTRUE)
            return false;
    }

    {
        uint8_t nl = '\n';
        if (xQueueSend(g_cam_q, &nl, pdMS_TO_TICKS(100)) != pdTRUE)
            return false;
    }

    cam_preview_push(line_no, orig);
    return true;
#else
    (void)s;
    (void)line_no;
    return false;
#endif
}

void cam_stream_finish(void)
{
    g_cam_done = true;
}

void cam_stream_abort(void)
{
#ifdef ARDUINO_ARCH_ESP32
    cam_stream_clear();
#endif
    cam_preview_clear();
    g_cam_done = true;
}

/* ------------------------------------------------------------------------- */
/* task                                                                      */
/* ------------------------------------------------------------------------- */

#ifdef ARDUINO_ARCH_ESP32
static void cam_stream_task(void *arg)
{
    (void)arg;

    /* switch parser input to our stream */
    cam_stream_readonly(cam_stream_getc, cam_stream_available, cam_stream_clear);

    while (1)
    {
        /*
         * Restore normal input stream after producer has finished,
         * queue is empty, and the machine is no longer executing.
         */
        if (g_cam_done &&
            !cam_stream_available() &&
            !cnc_get_exec_state(EXEC_RUN) &&
            !cnc_get_exec_state(EXEC_HOLD) &&
            !cnc_get_exec_state(EXEC_JOG))
        {
            grbl_stream_change(NULL);

            g_cam_started = false;
            g_cam_task = NULL;
            vTaskDelete(NULL);
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}
#endif

bool cam_stream_begin(void)
{
#ifdef ARDUINO_ARCH_ESP32
    if (!g_cam_q)
        g_cam_q = xQueueCreate(CAM_STREAM_QUEUE_LEN, sizeof(uint8_t));

    if (!g_cam_q)
        return false;

    if (!g_cam_started)
    {
        xQueueReset(g_cam_q);
        cam_preview_clear();
        g_cam_done = false;

        if (xTaskCreatePinnedToCore(
                cam_stream_task,
                "cam_stream",
                4096,
                NULL,
                1,
                &g_cam_task,
                0   /* same core as RA */) != pdPASS)
        {
            g_cam_task = NULL;
            g_cam_started = false;
            g_cam_done = true;
            return false;
        }

        g_cam_started = true;
    }
    else
    {
        g_cam_done = false;
    }

    return true;
#else
    return false;
#endif
}

DECL_MODULE(cam_stream)
{
#ifdef ARDUINO_ARCH_ESP32
    if (!g_cam_q)
        g_cam_q = xQueueCreate(CAM_STREAM_QUEUE_LEN, sizeof(uint8_t));
#endif
}
