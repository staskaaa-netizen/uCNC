/* The preview: the stock, the contour, the dimension layer and the live tool
   marker. It draws what the screen hands it and owns only what it draws with -
   see nc_preview.h for the request it is given and the switches it keeps. */
#include "nc_preview.h"

#include <stdio.h>
#include <string.h>

#include "../../cnc.h"
#include "nc_draw.h"
#include "nc_emit.h"
#include "nc_g7x.h"
#include "nc_layout.h"
#include "nc_menu.h"
#include "nc_tools.h"
#include "../lvds_renderer/lvds_draw_api.h"
#include "../lvds_renderer/lvds_hstx.h"
#if __has_include("../lvds_renderer/lvds_psram.h")
#include "../lvds_renderer/lvds_psram.h"
#define NC_PREVIEW_HAVE_PSRAM 1
#else
#define NC_PREVIEW_HAVE_PSRAM 0
#endif

/* The switches the footer keys toggle. They are the preview's own state: the
   screen reports the key it read and reads back whether the layer is on. */
static bool g_nc_preview_layer[NC_PREVIEW_LAYER_COUNT] = { true, true, true, true };

/* The live stock: the mask of what is still there, drawn on the machine when
   RUN is cutting, so the operator sees the material the program has taken off.
   The tool marker's rectangle is remembered so it can be erased next frame
   without redrawing the pane. */
static uint8_t *g_nc_live_stock_mask;
static bool g_nc_live_stock_ready;
static bool g_nc_live_stock_was_active;
static bool g_nc_live_stock_has_last;
static int g_nc_live_stock_w;
static int g_nc_live_stock_h;
static float g_nc_live_stock_setup_x;
static float g_nc_live_stock_setup_z;
static float g_nc_live_stock_setup_i;
static float g_nc_live_stock_last_x;
static float g_nc_live_stock_last_z;
static bool g_nc_live_tool_rect_valid;
static int g_nc_live_tool_rect_x;
static int g_nc_live_tool_rect_y;
static int g_nc_live_tool_rect_w;
static int g_nc_live_tool_rect_h;
/* While RUN is streaming and nothing about the drawing changed, the collected
   preview and the box it was fitted into are reused instead of recomputed. */
static bool g_nc_live_preview_cache_valid;
static nc_preview_info_t g_nc_live_preview_cache;
static nc_tool_t g_nc_live_tool_cache;
static bool g_nc_live_have_tool_cache;
static int g_nc_live_stock_w_cache;
static int g_nc_live_stock_h_cache;
static int g_nc_live_stock_left_cache;
static int g_nc_live_stock_top_cache;
static int g_nc_live_z0_x_cache;

bool nc_preview_layer(nc_preview_layer_t layer)
{
    if (layer < 0 || layer >= NC_PREVIEW_LAYER_COUNT) {
        return false;
    }
    return g_nc_preview_layer[layer];
}

bool nc_preview_toggle_layer(nc_preview_layer_t layer)
{
    if (layer < 0 || layer >= NC_PREVIEW_LAYER_COUNT) {
        return false;
    }
    g_nc_preview_layer[layer] = !g_nc_preview_layer[layer];
    return g_nc_preview_layer[layer];
}

void nc_preview_invalidate(void)
{
    g_nc_live_tool_rect_valid = false;
    g_nc_live_preview_cache_valid = false;
}

/* What there is to draw: the stock, the contour extent and the cycle,
   collected from the document before anything is painted. */
bool nc_preview_line_word_float(const char *line, char letter, float *out)
{
    nc_word_t words[16];
    int count;
    int i;

    count = nc_parse_words(line, words, 16);
    for (i = 0; i < count; i++) {
        if (words[i].letter == letter) {
            return nc_word_value(line, &words[i], out);
        }
    }

    return false;
}

void nc_preview_collect(const nc_document_t *doc, nc_preview_info_t *p)
{
    float last_x = 0.0f;
    float last_z = 0.0f;
    bool have_last = false;
    size_t i;

    if (!p) {
        return;
    }

    memset(p, 0, sizeof(*p));
    p->stock_x = 50.0f;
    p->stock_z = 75.0f;
    p->stock_i = 0.0f;
    p->stock_e = 3.0f;
    p->chuck_c = 15.0f;
    p->min_x = 1000000.0f;
    p->min_z = 1000000.0f;

    if (!doc) {
        return;
    }

    for (i = 0; i < doc->line_count; i++) {
        const char *line = doc->lines[i].text;
        g7x_cycle_t cycle;
        g7x_contour_cmd_t cmd;
        float px = last_x;
        float pz = last_z;
        bool has_x;
        bool has_z;

        if (!line) {
            continue;
        }

        if (g7x_command_is(line, "G971")) {
            (void)nc_preview_line_word_float(line, 'X', &p->stock_x);
            (void)nc_preview_line_word_float(line, 'Z', &p->stock_z);
            (void)nc_preview_line_word_float(line, 'I', &p->stock_i);
            (void)nc_preview_line_word_float(line, 'E', &p->stock_e);
        }
        if (g7x_command_is(line, "G972")) {
            (void)nc_preview_line_word_float(line, 'C', &p->chuck_c);
        }

        cycle = g7x_cycle_from_line(line);
        if (cycle != G7X_CYCLE_NONE) {
            p->cycles++;
            p->last_cycle = cycle;
        }

        cmd = g7x_contour_cmd_from_line(line);
        if (cmd == G7X_CONTOUR_NONE || cmd == G7X_CONTOUR_END) {
            continue;
        }

        has_x = nc_preview_line_word_float(line, 'X', &px);
        has_z = nc_preview_line_word_float(line, 'Z', &pz);
        if (!have_last) {
            last_x = has_x ? px : p->stock_x;
            last_z = has_z ? pz : 0.0f;
            have_last = true;
        }
        if (!has_x && !has_z) {
            continue;
        }

        if (px < p->min_x) p->min_x = px;
        if (px > p->max_x) p->max_x = px;
        if (pz < p->min_z) p->min_z = pz;
        if (pz > p->max_z) p->max_z = pz;
        if (cmd == G7X_CONTOUR_RAPID) {
            p->rapid_segments++;
        } else if (cmd == G7X_CONTOUR_ARC_CW || cmd == G7X_CONTOUR_ARC_CCW) {
            p->arc_segments++;
            p->path_segments++;
        } else {
            p->path_segments++;
        }
        last_x = px;
        last_z = pz;
    }

    if (p->stock_x <= 0.0f) p->stock_x = 50.0f;
    if (p->stock_z <= 0.0f) p->stock_z = 75.0f;
    if (p->stock_i < 0.0f) p->stock_i = 0.0f;
    if (p->stock_i >= p->stock_x) p->stock_i = 0.0f;
    if (p->stock_e < 0.0f) p->stock_e = 0.0f;
    if (p->chuck_c <= 0.0f) p->chuck_c = 15.0f;
    p->stock_visible_z = p->stock_z + p->stock_e;
    if (p->stock_visible_z <= 0.0f) p->stock_visible_z = p->stock_z;
    if (p->min_x > p->max_x) {
        p->min_x = 0.0f;
        p->max_x = p->stock_x;
    }
    if (p->min_z > p->max_z) {
        p->min_z = 0.0f;
        p->max_z = p->stock_z;
    }
}

#if NC_PREVIEW_DIN_STYLE
static void nc_visual_draw_din_layer(const nc_preview_ctx_t *ctx,
                                     const nc_preview_info_t *preview,
                                     int stock_left,
                                     int stock_top,
                                     int stock_w,
                                     int stock_h,
                                     int z0_x)
{
    char label[16];
    int stock_right;
    int stock_bottom;
    int dim_x;
    int id_y;

    if (!preview) {
        return;
    }

    stock_right = stock_left + stock_w;
    stock_bottom = stock_top + stock_h;
    dim_x = stock_right + 14;
    if (dim_x > LVDS_HSTX_WIDTH - 24) {
        dim_x = stock_right - 18;
    }

    nc_visual_draw_centerline(stock_left - 34,
                              stock_top,
                              stock_right + 18,
                              stock_top);
    nc_visual_draw_centerline(z0_x,
                              stock_top - 26,
                              z0_x,
                              stock_bottom + 20);
    nc_visual_draw_origin_marker(z0_x, stock_top);

    /*lvds_draw_line(z0_x, stock_top, z0_x + 24, stock_top - 22, NC_VISUAL_DIM);
    lvds_draw_text(z0_x + 27,
                   stock_top - 29,
                   "+Z",
                   NC_VISUAL_DIM,
                   NC_VISUAL_PREVIEW_BG,
                   LVDS_FONT_NORMAL);
    lvds_draw_line(z0_x, stock_top, z0_x - 18, stock_top - 24, NC_VISUAL_DIM);
    lvds_draw_text(z0_x - 35,
                   stock_top - 39,
                   "+X",
                   NC_VISUAL_DIM,
                   NC_VISUAL_PREVIEW_BG,
                   LVDS_FONT_NORMAL);*/

    if (ctx->mode != NC_MODE_RUN) {
        snprintf(label, sizeof(label), "%.0f", preview->stock_x);
        nc_visual_draw_diameter_dimension(dim_x,
                                          stock_top,
                                          stock_bottom,
                                          label);
    }
    if (ctx->mode != NC_MODE_RUN &&
        preview->stock_i > 0.0f && preview->stock_i < preview->stock_x) {
        id_y = nc_visual_preview_x(preview, stock_top, stock_h, preview->stock_i);
        nc_visual_draw_centerline(stock_left - 18, id_y, stock_right + 8, id_y);
        snprintf(label, sizeof(label), "%.0f", preview->stock_i);
        nc_visual_draw_diameter_dimension(dim_x - 18,
                                          stock_top,
                                          id_y,
                                          label);
    }
}
#endif

static void nc_visual_draw_contour_points(const nc_preview_ctx_t *ctx,
                                          const nc_document_t *doc,
                                          const nc_preview_info_t *preview,
                                          int z0_x,
                                          int stock_left,
                                          int stock_right,
                                          int stock_w,
                                          int stock_top,
                                          int stock_h)
{
#if NC_PREVIEW_DIN_POINT_MARKERS
    size_t i;
    float x = preview ? preview->stock_x : 0.0f;
    float z = 0.0f;
    int prev_z_px = z0_x;
    int prev_x_py = stock_top;
    int x_dim = stock_right + 15;
    char label[16];

    /* The dimension callouts follow the same switch as the rest of the layer:
       EDIT shows them in the pane and on the whole body alike, so turning `DIM`
       off on the full body is what hides them in the split view too. */
    if (!doc || !preview || ctx->mode == NC_MODE_MANUAL ||
        ctx->mode == NC_MODE_TOOLS) {
        return;
    }

    for (i = 0; i < doc->line_count; i++) {
        const char *line = doc->lines[i].text;
        g7x_contour_cmd_t cmd;
        bool has_x;
        bool has_z;

        if (!nc_g7x_line_is_any_contour(doc, i)) {
            continue;
        }
        cmd = g7x_contour_cmd_from_line(line);
        if (cmd == G7X_CONTOUR_NONE || cmd == G7X_CONTOUR_RAPID) {
            continue;
        }

        has_x = nc_preview_line_word_float(line, 'X', &x);
        has_z = nc_preview_line_word_float(line, 'Z', &z);
        if (has_x || has_z) {
            int px = nc_visual_preview_z(preview, z0_x, stock_w, z);
            int py = nc_visual_preview_x(preview, stock_top, stock_h, x);
            float feature = 0.0f;
            if (ctx->full ||
                (ctx->mode == NC_MODE_RUN &&
                 !ctx->streaming &&
                 !ctx->hold &&
                 !cnc_get_exec_state(EXEC_RUN | EXEC_HOLD))) {
                nc_visual_draw_contour_point_marker(px, py, i == doc->cursor_line);
            }
            snprintf(label, sizeof(label), "%.0f", x);
            nc_visual_draw_x_point_dimension(x_dim, prev_x_py, py, stock_top, px, label);
            prev_x_py = py;
            snprintf(label, sizeof(label), "%.0f", z);
            nc_visual_draw_z_point_dimension(prev_z_px, px, z0_x, stock_top, py, label);
            prev_z_px = px;
            if (nc_preview_line_word_float(line, 'R', &feature) && feature > 0.0001f) {
                snprintf(label, sizeof(label), "R%.0f", feature);
                lvds_draw_text(px + 4, py - 16, label, NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_SMALL);
            } else if (nc_preview_line_word_float(line, 'C', &feature) && feature > 0.0001f) {
                snprintf(label, sizeof(label), "C%.0f", feature);
                lvds_draw_text(px + 4, py - 16, label, NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_SMALL);
            }
        }
    }
    snprintf(label, sizeof(label), "%.0f", -preview->stock_visible_z);
    nc_visual_draw_z_point_dimension(prev_z_px,
                                     stock_left,
                                     z0_x,
                                     stock_top,
                                     stock_top,
                                     label);
#else
    (void)doc;
    (void)preview;
    (void)z0_x;
    (void)stock_left;
    (void)stock_right;
    (void)stock_w;
    (void)stock_top;
    (void)stock_h;
#endif
}

static void nc_visual_draw_emitted_preview(const nc_preview_ctx_t *ctx,
                                           const nc_document_t *doc,
                                           const nc_preview_info_t *preview,
                                           int z0_x,
                                           int stock_w,
                                           int stock_top,
                                           int stock_h,
                                           size_t max_lines)
{
    nc_emit_stream_t stream;
    float last_x = 0.0f;
    float last_z = 0.0f;
    bool have_last = false;
    size_t emitted_line = 0;
    size_t drawn_lines = 0;
    size_t guard = 0;
    nc_preview_segment_t segment = NC_PREVIEW_SEG_FEED;

    if (!doc || !preview) {
        return;
    }

    nc_emit_stream_begin(&stream, doc, 0);
    nc_emit_stream_set_log(&stream, false);
    while (stream.active && guard++ < max_lines * 8u + 64u) {
        char line[NC_MAX_LINE_LEN];
        nc_emit_result_t result = nc_emit_stream_next(&stream,
                                                      line,
                                                      sizeof(line),
                                                      &emitted_line);
        if (result == NC_EMIT_ERROR) {
            snprintf(ctx->status, ctx->status_size,
                     "Preview: %s at line %lu", g7x_result_text(stream.error),
                     (unsigned long)(emitted_line + 1u));
            break;
        }
        if (result == NC_EMIT_LINE) {
            if (line[0] == '(') {
                if (strstr(line, "rough")) {
                    segment = NC_PREVIEW_SEG_ROUGH;
                } else if (strstr(line, "finish")) {
                    segment = NC_PREVIEW_SEG_FINISH;
                }
                continue;
            }
            if (drawn_lines >= max_lines) {
                break;
            }
            if (ctx->mode == NC_MODE_PROGRAM &&
                ((segment == NC_PREVIEW_SEG_ROUGH && !nc_preview_layer(NC_PREVIEW_LAYER_ROUGH)) ||
                 (segment != NC_PREVIEW_SEG_ROUGH && !nc_preview_layer(NC_PREVIEW_LAYER_PATH)))) {
                drawn_lines++;
                continue;
            }
            (void)nc_visual_draw_emitted_motion_line(preview,
                                                     z0_x,
                                                     stock_w,
                                                     stock_top,
                                                     stock_h,
                                                     line,
                                                     segment,
                                                     emitted_line == doc->cursor_line,
                                                     &last_x,
                                                     &last_z,
                                                     &have_last);
            drawn_lines++;
        } else if (emitted_line >= doc->line_count &&
                   nc_emit_stream_line(&stream) >= doc->line_count) {
            break;
        }
    }
}

static float nc_live_runtime_x_to_diam(float runtime_x)
{
    return nc_visual_absf(runtime_x) * 2.0f;
}

static bool nc_visual_live_tool_rect(const nc_preview_info_t *preview,
                                     const nc_runtime_state_t *runtime,
                                     int z0_x,
                                     int stock_w,
                                     int stock_top,
                                     int stock_h,
                                     int *rx,
                                     int *ry,
                                     int *rw,
                                     int *rh)
{
    int sx;
    int sy;
    int pad = 16;
    int min_x;
    int max_x;
    int min_y;
    int max_y;
    int x0;
    int y0;
    int x1;
    int y1;

    if (!preview || !runtime) {
        return false;
    }

    sx = nc_visual_preview_z(preview, z0_x, stock_w, runtime->z);
    sy = nc_visual_preview_x(preview, stock_top, stock_h, nc_live_runtime_x_to_diam(runtime->x));
    sx = nc_visual_clampi(sx, 0, LVDS_HSTX_WIDTH - 1);
    sy = nc_visual_clampi(sy, 44, LVDS_HSTX_HEIGHT - 1);

    min_x = 0;
    max_x = nc_visual_clampi(z0_x + stock_w + pad, 0, LVDS_HSTX_WIDTH - 1);
    min_y = nc_visual_clampi(stock_top - pad, 44, LVDS_HSTX_HEIGHT - 1);
    max_y = nc_visual_clampi(stock_top + stock_h + pad, 44, LVDS_HSTX_HEIGHT - 1);
    x0 = nc_visual_clampi(sx - pad, min_x, max_x);
    y0 = nc_visual_clampi(sy - pad, min_y, max_y);
    x1 = nc_visual_clampi(sx + pad, min_x, max_x);
    y1 = nc_visual_clampi(sy + pad, min_y, max_y);

    if (rx) *rx = x0;
    if (ry) *ry = y0;
    if (rw) *rw = x1 - x0 + 1;
    if (rh) *rh = y1 - y0 + 1;
    return true;
}

static void nc_visual_draw_live_tool(const nc_preview_info_t *preview,
                                     const nc_runtime_state_t *runtime,
                                     int z0_x,
                                     int stock_w,
                                     int stock_top,
                                     int stock_h,
                                     int pane_x,
                                     int pane_y,
                                     int pane_w,
                                     int pane_h,
                                     const nc_tool_t *tool)
{
    int sx;
    int sy;
    int rx;
    int ry;
    int rw;
    int rh;

    if (!preview || !runtime || !tool || !tool->valid) {
        return;
    }

    sx = nc_visual_preview_z(preview, z0_x, stock_w, runtime->z);
    sy = nc_visual_preview_x(preview, stock_top, stock_h, nc_live_runtime_x_to_diam(runtime->x));
    /* Inside the pane or not at all. A marker clamped to the whole screen is
       drawn over the header, the code pane or the footer when the axis is out
       of view - and because only the pane is redrawn every frame, that marker
       stayed there: the "cursor drawn but not cleared". Out of view, the DRO
       says where the axis is; the preview does not guess. */
    if (sx < pane_x || sx + NC_LIVE_TOOL_GLYPH > pane_x + pane_w ||
        sy < pane_y || sy + NC_LIVE_TOOL_GLYPH > pane_y + pane_h) {
        return;
    }

    if (nc_visual_live_tool_rect(preview, runtime, z0_x, stock_w, stock_top, stock_h, &rx, &ry, &rw, &rh)) {
        g_nc_live_tool_rect_x = rx;
        g_nc_live_tool_rect_y = ry;
        g_nc_live_tool_rect_w = rw;
        g_nc_live_tool_rect_h = rh;
        g_nc_live_tool_rect_valid = true;
    }
    nc_visual_draw_tool_glyph(sx, sy, NC_LIVE_TOOL_GLYPH, tool, NC_VISUAL_PREVIEW_BG, false);
}

static bool nc_live_stock_alloc(void)
{
    if (g_nc_live_stock_mask) {
        return true;
    }
#if NC_PREVIEW_HAVE_PSRAM
    if (!lvds_psram_available()) {
        (void)lvds_psram_init();
    }
    if (lvds_psram_available()) {
        g_nc_live_stock_mask = (uint8_t *)lvds_psram_ptr(NC_LIVE_STOCK_PSRAM_OFFSET);
    }
#endif
    return g_nc_live_stock_mask != NULL;
}

static bool nc_live_stock_context_changed(const nc_preview_info_t *preview,
                                          int stock_w,
                                          int stock_h)
{
    if (!preview) {
        return true;
    }
    return !g_nc_live_stock_ready ||
           g_nc_live_stock_w != stock_w ||
           g_nc_live_stock_h != stock_h ||
           g_nc_live_stock_setup_x != preview->stock_x ||
           g_nc_live_stock_setup_z != preview->stock_visible_z ||
           g_nc_live_stock_setup_i != preview->stock_i;
}

static void nc_live_stock_reset(const nc_preview_info_t *preview,
                                int stock_w,
                                int stock_h)
{
    int material_top = 0;
    int y;

    g_nc_live_stock_ready = false;
    g_nc_live_stock_has_last = false;
    if (!preview || !nc_live_stock_alloc()) {
        return;
    }

    stock_w = nc_visual_clampi(stock_w, 1, NC_LIVE_STOCK_MAX_W);
    stock_h = nc_visual_clampi(stock_h, 1, NC_LIVE_STOCK_MAX_H);
    memset(g_nc_live_stock_mask, 0, (size_t)NC_LIVE_STOCK_MAX_W * NC_LIVE_STOCK_MAX_H);
    if (preview->stock_i > 0.0f && preview->stock_x > 0.0f) {
        material_top = nc_visual_clampi((int)((preview->stock_i / preview->stock_x) * (float)stock_h),
                                        0,
                                        stock_h - 1);
    }
    for (y = material_top; y < stock_h; y++) {
        memset(g_nc_live_stock_mask + ((size_t)y * NC_LIVE_STOCK_MAX_W), 1, (size_t)stock_w);
    }

    g_nc_live_stock_w = stock_w;
    g_nc_live_stock_h = stock_h;
    g_nc_live_stock_setup_x = preview->stock_x;
    g_nc_live_stock_setup_z = preview->stock_visible_z;
    g_nc_live_stock_setup_i = preview->stock_i;
    g_nc_live_stock_ready = true;
}

static void nc_live_stock_remove_rect(int x0, int y0, int x1, int y1)
{
    int y;

    if (!g_nc_live_stock_mask || !g_nc_live_stock_ready) {
        return;
    }
    x0 = nc_visual_clampi(x0, 0, g_nc_live_stock_w - 1);
    x1 = nc_visual_clampi(x1, 0, g_nc_live_stock_w - 1);
    y0 = nc_visual_clampi(y0, 0, g_nc_live_stock_h - 1);
    y1 = nc_visual_clampi(y1, 0, g_nc_live_stock_h - 1);
    if (x1 < x0) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y1 < y0) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    for (y = y0; y <= y1; y++) {
        memset(g_nc_live_stock_mask + ((size_t)y * NC_LIVE_STOCK_MAX_W) + x0,
               0,
               (size_t)(x1 - x0 + 1));
    }
}

static void nc_live_stock_cut_sweep(const nc_preview_info_t *preview,
                                    int z0_x,
                                    int stock_left,
                                    int stock_w,
                                    int stock_top,
                                    int stock_h,
                                    float x0,
                                    float z0,
                                    float x1,
                                    float z1)
{
    int sx0;
    int sx1;
    int sy0;
    int sy1;
    int samples;
    int i;

    if (!preview || !g_nc_live_stock_ready) {
        return;
    }
    sx0 = nc_visual_preview_z(preview, z0_x, stock_w, z0) - stock_left;
    sx1 = nc_visual_preview_z(preview, z0_x, stock_w, z1) - stock_left;
    sy0 = nc_visual_preview_x(preview, stock_top, stock_h, x0) - stock_top;
    sy1 = nc_visual_preview_x(preview, stock_top, stock_h, x1) - stock_top;
    samples = nc_visual_absf((float)(sx1 - sx0)) > nc_visual_absf((float)(sy1 - sy0)) ?
              nc_visual_absf((float)(sx1 - sx0)) :
              nc_visual_absf((float)(sy1 - sy0));
    samples = nc_visual_clampi(samples, 1, 80);
    for (i = 0; i <= samples; i++) {
        float t = (float)i / (float)samples;
        int sx = sx0 + (int)((float)(sx1 - sx0) * t);
        int sy = sy0 + (int)((float)(sy1 - sy0) * t);
        nc_live_stock_remove_rect(sx - 1, sy, sx + 1, g_nc_live_stock_h - 1);
    }
}

static void nc_live_stock_update(const nc_preview_info_t *preview,
                                 const nc_runtime_state_t *runtime,
                                 int z0_x,
                                 int stock_left,
                                 int stock_w,
                                 int stock_top,
                                 int stock_h)
{
    float diam_x;

    if (!preview || !runtime || !g_nc_live_stock_ready) {
        return;
    }
    diam_x = nc_live_runtime_x_to_diam(runtime->x);
    if (g_nc_live_stock_has_last) {
        nc_live_stock_cut_sweep(preview,
                                z0_x,
                                stock_left,
                                stock_w,
                                stock_top,
                                stock_h,
                                g_nc_live_stock_last_x,
                                g_nc_live_stock_last_z,
                                diam_x,
                                runtime->z);
    } else {
        nc_live_stock_cut_sweep(preview,
                                z0_x,
                                stock_left,
                                stock_w,
                                stock_top,
                                stock_h,
                                diam_x,
                                runtime->z,
                                diam_x,
                                runtime->z);
    }
    g_nc_live_stock_last_x = diam_x;
    g_nc_live_stock_last_z = runtime->z;
    g_nc_live_stock_has_last = true;
}

static void nc_live_stock_draw(int stock_left,
                               int stock_top,
                               int clip_x,
                               int clip_y,
                               int clip_w,
                               int clip_h)
{
    int y0;
    int y1;
    int y;
    int clip_left = clip_x - stock_left;
    int clip_right = clip_left + clip_w - 1;

    if (!g_nc_live_stock_mask || !g_nc_live_stock_ready) {
        return;
    }
    clip_left = nc_visual_clampi(clip_left, 0, g_nc_live_stock_w - 1);
    clip_right = nc_visual_clampi(clip_right, 0, g_nc_live_stock_w - 1);
    y0 = nc_visual_clampi(clip_y - stock_top, 0, g_nc_live_stock_h - 1);
    y1 = nc_visual_clampi(clip_y + clip_h - stock_top - 1, 0, g_nc_live_stock_h - 1);
    if (clip_right < clip_left || y1 < y0) {
        return;
    }
    lvds_draw_fill_rect(stock_left + clip_left,
                        stock_top + y0,
                        clip_right - clip_left + 1,
                        y1 - y0 + 1,
                        NC_VISUAL_PREVIEW_BG);
    for (y = y0; y <= y1; y++) {
        const uint8_t *row = g_nc_live_stock_mask + ((size_t)y * NC_LIVE_STOCK_MAX_W);
        int x = clip_left;
        while (x <= clip_right) {
            int start;
            while (x <= clip_right && !row[x]) {
                x++;
            }
            start = x;
            while (x <= clip_right && row[x]) {
                x++;
            }
            if (x > start) {
                lvds_draw_fill_rect(stock_left + start,
                                    stock_top + y,
                                    x - start,
                                    1,
                                    NC_VISUAL_PREVIEW_STOCK);
            }
        }
    }
}

static bool nc_visual_draw_live_stock(const nc_preview_ctx_t *ctx,
                                      nc_preview_times_t *times,
                                      const nc_preview_info_t *preview,
                                      const nc_runtime_state_t *runtime,
                                      const nc_tool_t *tool,
                                      int stock_left,
                                      int stock_top,
                                      int stock_w,
                                      int stock_h,
                                      int z0_x,
                                      int tool_panel_x,
                                      int tool_panel_y,
                                      int tool_panel_w,
                                      int pane_h,
                                      bool draw_static_panel)
{
    bool live_run;
    bool context_changed;
    bool retain_stock;
    int clear_y;
    int clear_h;
    uint32_t t0;
    uint32_t t1;
    uint32_t t2;
    uint32_t t3;
    uint32_t t4;

    live_run = ctx->runtime_busy || ctx->streaming;

    if (!preview || !runtime) {
        g_nc_live_stock_was_active = false;
        g_nc_live_tool_rect_valid = false;
        return false;
    }
    context_changed = nc_live_stock_context_changed(preview, stock_w, stock_h);
    retain_stock = ctx->mode == NC_MODE_RUN &&
                   g_nc_live_stock_ready &&
                   !context_changed;
    if (!live_run && !retain_stock) {
        g_nc_live_stock_was_active = false;
        g_nc_live_tool_rect_valid = false;
        return false;
    }
    if (live_run && (!g_nc_live_stock_was_active || context_changed)) {
        nc_live_stock_reset(preview, stock_w, stock_h);
        g_nc_live_tool_rect_valid = false;
    }
    g_nc_live_stock_was_active = live_run || retain_stock;
    if (!g_nc_live_stock_ready) {
        nc_visual_draw_text_clip(stock_left,
                                 stock_top + 16,
                                 "Live stock needs PSRAM",
                                 28,
                                 NC_VISUAL_ERROR,
                                 NC_VISUAL_PREVIEW_BG,
                                 LVDS_FONT_NORMAL);
        return true;
    }
    t0 = mcu_micros();
    clear_y = nc_visual_clampi(stock_top - 24, 44, NC_FOOTER_Y - 1);
    clear_h = nc_visual_clampi(stock_top + stock_h + 74 - clear_y, 1, NC_FOOTER_Y - clear_y);
    lvds_draw_fill_rect(tool_panel_x, clear_y, tool_panel_w, clear_h, NC_VISUAL_PREVIEW_BG);
    t1 = mcu_micros();
    if (live_run) {
        nc_live_stock_update(preview, runtime, z0_x, stock_left, stock_w, stock_top, stock_h);
    }
    nc_visual_draw_chuck(preview, stock_left, stock_top, stock_w, stock_h);
    nc_visual_draw_chuck_relief(preview, stock_left, stock_top, stock_h);
    nc_live_stock_draw(stock_left, stock_top, stock_left, stock_top, stock_w, stock_h);
    t2 = mcu_micros();
#if NC_PREVIEW_DIN_STYLE
    if (nc_preview_layer(NC_PREVIEW_LAYER_DIMS)) {
        nc_visual_draw_din_layer(ctx, preview, stock_left, stock_top, stock_w, stock_h, z0_x);
    }
#endif
    if (nc_preview_layer(NC_PREVIEW_LAYER_DIMS)) {
        nc_visual_draw_contour_points(ctx, ctx->screen_doc,
                                          preview,
                                          z0_x,
                                          stock_left,
                                          stock_left + stock_w,
                                          stock_w,
                                          stock_top,
                                          stock_h);
    }
#if !NC_PREVIEW_DIN_STYLE
    lvds_draw_line(z0_x, stock_top - 18, z0_x, stock_top + stock_h + 18, NC_VISUAL_DIM);
    lvds_draw_text(z0_x - 12, stock_top - 34, "Z0", NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_NORMAL);
#endif
    t3 = mcu_micros();
    nc_visual_draw_live_tool(preview, runtime, z0_x, stock_w, stock_top, stock_h,
                             tool_panel_x, tool_panel_y, tool_panel_w, pane_h, tool);
    if (draw_static_panel) {
        nc_visual_draw_preview_tool_panel(tool_panel_x,
                                          tool_panel_y,
                                          tool_panel_w,
                                          58,
                                          tool);
    }
    t4 = mcu_micros();
    times->clear += t1 - t0;
    times->stock += t2 - t1;
    times->geom += t3 - t2;
    times->tool += t4 - t3;
    return true;
}

void nc_preview_draw(const nc_preview_ctx_t *ctx,
                     int x,
                     int y,
                     int w,
                     int h,
                     bool clear_bg,
                     nc_preview_times_t *times)
{
    const nc_document_t *doc = ctx->doc;
    const nc_runtime_state_t *runtime = ctx->runtime;
    nc_preview_times_t discard = { 0 };
    nc_preview_info_t preview;
    int stock_w;
    int stock_h;
    int stock_left;
    int stock_top;
    int z0_x;
    float usable_w;
    float usable_h;
    float z_scale;
    float x_scale;
    float scale;
    nc_tool_t tool;
    bool have_tool = false;
    size_t tool_line = doc && doc->line_count ? doc->cursor_line : 0;
    uint32_t t0 = mcu_micros();
    uint32_t t1;
    uint32_t t2;
    uint32_t t3;
    uint32_t t5;
    bool use_live_cache = !clear_bg &&
                          ctx->mode == NC_MODE_RUN &&
                          g_nc_live_preview_cache_valid;

    if (!times) {
        times = &discard;
    }

    if (doc && doc->path[0] && !nc_path_supported(doc->path)) {
        /* Text is not a program. The editor shows it; the preview must not read
           it as G-code - the preset file is the file this is for. */
        if (clear_bg) {
            lvds_draw_fill_rect(x, y, w, h, NC_VISUAL_PREVIEW_BG);
        }
        nc_visual_draw_text_clip(x + 12, y + 12, "Text file - no preview", 24,
                                 NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG,
                                 LVDS_FONT_NORMAL);
        return;
    }

    if (doc && ctx->mode == NC_MODE_RUN && ctx->run_line < doc->line_count) {
        tool_line = ctx->run_line;
    }

    if (use_live_cache) {
        preview = g_nc_live_preview_cache;
        tool = g_nc_live_tool_cache;
        have_tool = g_nc_live_have_tool_cache;
        stock_w = g_nc_live_stock_w_cache;
        stock_h = g_nc_live_stock_h_cache;
        stock_left = g_nc_live_stock_left_cache;
        stock_top = g_nc_live_stock_top_cache;
        z0_x = g_nc_live_z0_x_cache;
        t1 = t0;
        t2 = t0;
        t3 = t0;
    } else {
        nc_preview_collect(doc, &preview);
        t1 = mcu_micros();
        if (clear_bg) {
            lvds_draw_fill_rect(x, y, w, h, NC_VISUAL_PREVIEW_BG);
            /* No caption: the tab strip carries the screen name and the header
               carries the file or the run state. */
        }
        t2 = mcu_micros();

        /* Content box inside the pane: the stock starts below a short band at
           the top (the caption that used to live there is gone - the tab strip
           says the screen and the header says the file) and the drawing stops
           short of the pane's own bottom edge, so no part of the preview -
           stock, chuck or a dimension label - can be drawn over the footer
           strip below it. SIM keeps a little more room under the stock for its
           labels. */
        usable_w = (float)(w - 72);
        usable_h = (float)(h - NC_PREVIEW_TOP_BAND -
                           (ctx->full ? 28 : 8));
        if (usable_w < 40.0f) usable_w = 40.0f;
        if (usable_h < 40.0f) usable_h = 40.0f;
        z_scale = usable_w / preview.stock_visible_z;
        x_scale = usable_h / (preview.stock_x * 0.5f);
        scale = z_scale < x_scale ? z_scale : x_scale;
        if (scale <= 0.0f) scale = 1.0f;
        stock_w = (int)(preview.stock_visible_z * scale + 0.5f);
        stock_h = (int)((preview.stock_x * 0.5f) * scale + 0.5f);
        if (stock_w < 24) stock_w = 24;
        if (stock_h < 24) stock_h = 24;
        if (stock_w > (int)usable_w) stock_w = (int)usable_w;
        if (stock_h > (int)usable_h) stock_h = (int)usable_h;

        stock_left = x + 20;
        stock_top = y + NC_PREVIEW_TOP_BAND;
        z0_x = stock_left + (int)(preview.stock_z * scale + 0.5f);
        if (z0_x < stock_left) z0_x = stock_left;
        if (z0_x > stock_left + stock_w) z0_x = stock_left + stock_w;
        memset(&tool, 0, sizeof(tool));
        if (doc && ctx->mode == NC_MODE_TOOLS &&
            nc_tool_active_from_table(doc, tool_line, ctx->screen_doc, &tool)) {
            have_tool = true;
        } else if (doc && nc_tool_active_from_file(doc,
                                                   tool_line,
                                                   ctx->tool_path,
                                                   &tool)) {
            have_tool = true;
        }
        t3 = mcu_micros();
        if (ctx->mode == NC_MODE_RUN) {
            g_nc_live_preview_cache = preview;
            g_nc_live_tool_cache = tool;
            g_nc_live_have_tool_cache = have_tool;
            g_nc_live_stock_w_cache = stock_w;
            g_nc_live_stock_h_cache = stock_h;
            g_nc_live_stock_left_cache = stock_left;
            g_nc_live_stock_top_cache = stock_top;
            g_nc_live_z0_x_cache = z0_x;
            g_nc_live_preview_cache_valid = true;
        }
    }
    times->collect += (t1 - t0) + (t3 - t2);
    times->clear += t2 - t1;
    if (ctx->mode == NC_MODE_RUN &&
        nc_visual_draw_live_stock(ctx,
                                  times,
                                  &preview,
                                  runtime,
                                  have_tool ? &tool : NULL,
                                  stock_left,
                                  stock_top,
                                  stock_w,
                                  stock_h,
                                  z0_x,
                                  x,
                                  y,
                                  w,
                                  h,
                                  clear_bg)) {
        return;
    }

    t0 = mcu_micros();
    nc_visual_draw_chuck(&preview, stock_left, stock_top, stock_w, stock_h);
    nc_visual_draw_chuck_relief(&preview, stock_left, stock_top, stock_h);
    if (nc_preview_layer(NC_PREVIEW_LAYER_STOCK) || !ctx->full) {
        lvds_draw_fill_rect(stock_left, stock_top, stock_w, stock_h, NC_VISUAL_PREVIEW_STOCK);
        if (preview.stock_i > 0.0f) {
            int id_h = nc_visual_preview_x(&preview, stock_top, stock_h, preview.stock_i) - stock_top;
            if (id_h > 0 && id_h < stock_h) {
                lvds_draw_fill_rect(stock_left, stock_top, stock_w, id_h, NC_VISUAL_PREVIEW_BG);
            }
        }
    }
    t1 = mcu_micros();
#if NC_PREVIEW_DIN_STYLE
    if (nc_preview_layer(NC_PREVIEW_LAYER_DIMS)) {
        nc_visual_draw_din_layer(ctx, &preview, stock_left, stock_top, stock_w, stock_h, z0_x);
    }
#endif
    if (nc_preview_layer(NC_PREVIEW_LAYER_DIMS)) {
        nc_visual_draw_contour_points(ctx,
                                      doc,
                                      &preview,
                                          z0_x,
                                          stock_left,
                                          stock_left + stock_w,
                                          stock_w,
                                          stock_top,
                                          stock_h);
    }
#if !NC_PREVIEW_DIN_STYLE
    lvds_draw_line(z0_x, stock_top - 18, z0_x, stock_top + stock_h + 18, NC_VISUAL_DIM);
    lvds_draw_text(z0_x - 12, stock_top - 34, "Z0", NC_VISUAL_DIM, NC_VISUAL_PREVIEW_BG, LVDS_FONT_NORMAL);
#endif
    t2 = mcu_micros();
    if (!ctx->full) {
        nc_visual_draw_preview_tool_panel(x, y, w, h, have_tool ? &tool : NULL);
    }
    t3 = mcu_micros();
    nc_visual_draw_emitted_preview(ctx, doc,
                                   &preview,
                                   z0_x,
                                   stock_w,
                                   stock_top,
                                   stock_h,
                                   96u);
    t5 = mcu_micros();
    if (have_tool && !ctx->full) {
        nc_visual_draw_live_tool(&preview, runtime, z0_x, stock_w, stock_top,
                                 stock_h, x, y, w, h, &tool);
        nc_visual_draw_preview_tool_panel(x, y, w, h, &tool);
    }
    times->stock += t1 - t0;
    times->geom += (t2 - t1) + (t5 - t3);
    times->tool += (t3 - t2) + (mcu_micros() - t5);
}
