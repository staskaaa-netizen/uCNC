#ifndef CAM_STREAM_H
#define CAM_STREAM_H

#include <stdint.h>
#include <stdbool.h>

#ifndef CAM_PREVIEW_LINES
#define CAM_PREVIEW_LINES 16
#endif

#ifndef CAM_PREVIEW_TEXT_LEN
#define CAM_PREVIEW_TEXT_LEN 96
#endif

typedef struct
{
    uint32_t line_no[CAM_PREVIEW_LINES];
    char     text[CAM_PREVIEW_LINES][CAM_PREVIEW_TEXT_LEN];
    uint8_t  count;
} cam_preview_snapshot_t;

/* Start using the CAM queue as parser input. Safe to call repeatedly. */
bool cam_stream_begin(void);

/* Push one complete line. Newline is appended automatically. */
bool cam_stream_send_line(const char *s, uint32_t line_no);

/* Mark current CAM program complete. Stream restores when queue/runtime drain. */
void cam_stream_finish(void);

/* Clear queued CAM data and preview. */
void cam_stream_abort(void);

bool cam_stream_get_preview(cam_preview_snapshot_t *out);

#endif
