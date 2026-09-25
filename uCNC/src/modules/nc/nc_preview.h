/* The preview: what there is to draw, and the drawing of it.

   `nc_sim.c` was the old name of the data half - it never simulated anything,
   it collected the stock, the contour extent and the cycle - and the renderer
   followed it out of the screen when `nc_visual.c` passed the size rule. The
   two are one unit: a preview with no screen to draw in is not a thing that
   can exist, so this is a source of the NC module, never a module of its own.

   The boundary: the preview takes the request below and owns nothing the
   screen owns - what it keeps is what it draws with (the layer switches, the
   live stock mask, one frame's cache, and the live band the tool marker is held
   inside). It never calls back into the screen, and the screen never draws the
   stock itself. */
#ifndef NC_PREVIEW_H
#define NC_PREVIEW_H

#include "nc.h"
#include "nc_menu.h"
#include "nc_state.h"
#include "../g7x/g7x_contour.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float stock_x;
    float stock_z;
    float stock_i;
    float stock_e;
    float chuck_c;
    float stock_visible_z;
    float min_x;
    float max_x;
    float min_z;
    float max_z;
    int path_segments;
    int rapid_segments;
    int arc_segments;
    int cycles;
    g7x_cycle_t last_cycle;
} nc_preview_info_t;

/* Everything the renderer needs to know about the frame it is drawing. The
   three run flags are separate on purpose: the code they feed asked three
   different questions before the split (`runtime_busy` is the parser's own
   state, `streaming` and `hold` are the sender's), and collapsing them here
   would be a behaviour change inside a move. */
typedef struct {
    const nc_document_t *doc;        /* the program to show, or NULL */
    const nc_document_t *screen_doc; /* the screen's own open document: the tool
                                        table in TOOLS, the contour in RUN */
    const nc_runtime_state_t *runtime;
    const char *tool_path;           /* tool offset file, or NULL */
    size_t run_line;                 /* the line RUN is on */
    nc_mode_t mode;
    char *status;                    /* the screen's status line: a preview
                                        error is written here, as it was */
    size_t status_size;
    bool full;                       /* whole body: no code pane, no panel */
    bool runtime_busy;               /* nc_runtime_state_t says RUN or HOLD */
    bool streaming;                  /* hold held through nc_run_active() */
    bool hold;
} nc_preview_ctx_t;

/* The layers the footer keys switch. The screen reads them to light its own
   key and toggles them on a press; nothing else may set them. */
typedef enum {
    NC_PREVIEW_LAYER_STOCK = 0,
    NC_PREVIEW_LAYER_PATH,
    NC_PREVIEW_LAYER_ROUGH,
    NC_PREVIEW_LAYER_DIMS,
    NC_PREVIEW_LAYER_COUNT
} nc_preview_layer_t;

/* What this frame's preview cost, by phase, in microseconds. The panel's frame
   counter adds them up; a caller that does not measure passes NULL. */
typedef struct {
    uint32_t collect;
    uint32_t clear;
    uint32_t stock;
    uint32_t geom;
    uint32_t tool;
} nc_preview_times_t;

void nc_preview_collect(const nc_document_t *doc, nc_preview_info_t *preview);

/* Coordinate mapping shared with MANUAL's jog direction. */
int nc_preview_map_x(const nc_preview_info_t *preview, int stock_top, int stock_h, float x);
int nc_preview_map_z(const nc_preview_info_t *preview, int z0_x, int stock_w, float z);

/* Draw one frame of the preview into (x, y, w, h). */
void nc_preview_draw(const nc_preview_ctx_t *ctx, int x, int y, int w, int h,
                     bool clear_bg, nc_preview_times_t *times);

bool nc_preview_layer(nc_preview_layer_t layer);
bool nc_preview_toggle_layer(nc_preview_layer_t layer);

/* A footer action the preview owns: the four layer switches. The message that
   goes with the new state comes back with it, and NULL means "not mine". */
const char *nc_preview_action(uint8_t action);

/* The drawing context changed (a full frame is about to be drawn): the frame
   cache no longer describes what is on the panel. */
void nc_preview_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif
