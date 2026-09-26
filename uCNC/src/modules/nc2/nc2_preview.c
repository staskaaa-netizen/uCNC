#include "nc2_preview.h"

#include "nc2_draw.h"
#include "nc2_emit.h"
#include "nc2_files.h"

#include "../g7x/g7x_contour.h"
#include "../../cnc.h"
#include "../lvds_renderer/lvds_hstx.h"
#if __has_include("../lvds_renderer/lvds_psram.h")
#include "../lvds_renderer/lvds_psram.h"
#define NC2_PREVIEW_HAVE_PSRAM 1
#else
#define NC2_PREVIEW_HAVE_PSRAM 0
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The pane's furniture: a band at the top for the ruler labels, a margin under
   the drawing, and the chuck at the left of the stock. The numbers are nc's
   (`nc_layout.h`), so the drawing sits where the old one sat. */
#define NC2_PREVIEW_TOP_BAND 82
#define NC2_CHUCK_C 15.0f

/* The live tool's own size: nc drew it at twenty pixels across, and the pane is
   read at the same distance, so it is the same here. */
#define NC2_LIVE_TOOL_GLYPH 20

/* What there is to draw: the stock, the contour's extent and the setup rows. */
typedef struct {
    float stock_x;
    float stock_z;
    float stock_i;
    float stock_e;
    float chuck_c;
    float visible_z;
    float min_x;
    float max_x;
    float min_z;
    float max_z;
} nc2_preview_info_t;

typedef enum {
    NC2_SEG_FEED = 0,
    NC2_SEG_ROUGH,
    NC2_SEG_FINISH
} nc2_preview_segment_t;

static int nc2_map_x(const nc2_preview_info_t *p, int stock_top, int stock_h,
                     float x)
{
    /* A program's X is a **diameter** (the lathe's own frame, Fanuc's G7) and
       the drawing is a radius: the stock's own X is a diameter too, and it fills
       the stock's half-height. nc's `nc_preview_map_x()` halved the value before
       fitting it, and the port dropped the halving - so the stock came out right
       and the profile came out **twice its size** (bench: "stock is drawn as it
       should, path is in full sizes"). */
    float f = (x * 0.5f) / (p->stock_x * 0.5f);

    return stock_top + (int)(f * (float)stock_h + 0.5f);
}

static int nc2_map_z(const nc2_preview_info_t *p, int z0_x, int stock_w, float z)
{
    float f = z / p->visible_z;

    return z0_x + (int)(f * (float)stock_w + 0.5f);
}

/* The value of a letter on a line, read the way the editor reads it: the fields
   of the line, one letter each. */
static bool nc2_line_value(const char *line, char want, float *out)
{
    nc2_field_t fields[NC2_MAX_FIELDS];
    int count = nc2_fields(line, fields, NC2_MAX_FIELDS);
    int i;

    for (i = 0; i < count; i++) {
        if (fields[i].letter != want || fields[i].value == fields[i].end) {
            continue;
        }
        if (out) {
            *out = (float)strtod(line + fields[i].value, 0);
        }
        return true;
    }
    return false;
}

static bool nc2_line_is_setup(const char *line, const char *command)
{
    return g7x_command_is(g7x_skip_line_number(line), command);
}

/* What the drawing needs, from the document: the setup rows (`G970`..`G973`),
   which are the pane's own numbers, and the points the contour reaches, which
   the drawing is fitted to. A point is read with the sender's rule, so an
   increment counts from the row above it. */
static void nc2_preview_collect(const nc2_document_t *doc, nc2_preview_info_t *p)
{
    float last_x = 0.0f;
    float last_z = 0.0f;
    size_t i;

    memset(p, 0, sizeof(*p));
    p->stock_x = 50.0f;
    p->stock_z = 75.0f;
    p->stock_i = 0.0f;
    p->stock_e = 3.0f;
    p->chuck_c = NC2_CHUCK_C;
    p->min_x = 1000000.0f;
    p->min_z = 1000000.0f;
    if (!doc) {
        p->visible_z = p->stock_z + p->stock_e;
        return;
    }
    last_x = p->stock_x;
    for (i = 0u; i < doc->line_count; i++) {
        const char *line = doc->lines[i];
        float px = last_x;
        float pz = last_z;
        g7x_contour_cmd_t cmd;

        if (nc2_line_is_setup(line, "G971")) {
            (void)nc2_line_value(line, 'X', &p->stock_x);
            (void)nc2_line_value(line, 'Z', &p->stock_z);
            (void)nc2_line_value(line, 'I', &p->stock_i);
            (void)nc2_line_value(line, 'E', &p->stock_e);
        }
        if (nc2_line_is_setup(line, "G972")) {
            (void)nc2_line_value(line, 'C', &p->chuck_c);
        }

        /* The point follows every line the controller is given, so a rapid
           before a cycle is what that cycle's first row measures from. */
        if (nc2_emit_line_is_direct(line) &&
            nc2_emit_line_point(line, &px, &pz, 0, 0u, 0)) {
            last_x = px;
            last_z = pz;
        }

        cmd = g7x_contour_cmd_from_line(g7x_skip_line_number(line));
        if (cmd == G7X_CONTOUR_NONE || cmd == G7X_CONTOUR_END) {
            continue;
        }
        if (px < p->min_x) p->min_x = px;
        if (px > p->max_x) p->max_x = px;
        if (pz < p->min_z) p->min_z = pz;
        if (pz > p->max_z) p->max_z = pz;
    }
    if (p->stock_x <= 0.0f) p->stock_x = 50.0f;
    if (p->stock_z <= 0.0f) p->stock_z = 75.0f;
    if (p->stock_i < 0.0f) p->stock_i = 0.0f;
    if (p->stock_i >= p->stock_x) p->stock_i = 0.0f;
    if (p->stock_e < 0.0f) p->stock_e = 0.0f;
    if (p->chuck_c <= 0.0f) p->chuck_c = NC2_CHUCK_C;
    p->visible_z = p->stock_z + p->stock_e;
    if (p->visible_z <= 0.0f) p->visible_z = p->stock_z;
    if (p->min_x > p->max_x) {
        p->min_x = 0.0f;
        p->max_x = p->stock_x;
    }
    if (p->min_z > p->max_z) {
        p->min_z = 0.0f;
        p->max_z = p->stock_z;
    }
}

/* --- the DIN layer, as nc draws it ---------------------------------------- */

float nc2_preview_stock_x(const nc2_document_t *doc)
{
    nc2_preview_info_t info;

    if (!doc) {
        return 0.0f;
    }
    memset(&info, 0, sizeof(info));
    nc2_preview_collect(doc, &info);
    return info.stock_x;
}

static void nc2_dashdot(int x0, int y0, int x1, int y1, lvds_color_t color)
{
    static const uint8_t pattern[] = { 18, 5, 3, 5 };
    int dx = x1 - x0;
    int dy = y1 - y0;
    int len2 = dx * dx + dy * dy;
    float len;
    int pos = 0;
    int pat = 0;

    if (len2 <= 0) {
        return;
    }
    len = sqrtf((float)len2);
    while (pos < (int)len) {
        int seg = pattern[pat & 3];
        int a = pos;
        int b = pos + seg;

        if (b > (int)len) {
            b = (int)len;
        }
        if ((pat & 1) == 0 && b > a) {
            int xa = x0 + (int)((float)dx * ((float)a / len));
            int ya = y0 + (int)((float)dy * ((float)a / len));
            int xb = x0 + (int)((float)dx * ((float)b / len));
            int yb = y0 + (int)((float)dy * ((float)b / len));

            lvds_draw_line(xa, ya, xb, yb, color);
        }
        pos += seg;
        pat++;
    }
}

/* The origin: a centreline mark where a pin stops - a centre mark on a lathe
   drawing - drawn as two circles and four ticks, the way nc draws it. */
static void nc2_origin_marker(int x, int y)
{
    lvds_draw_ellipse(x, y, 11, 11, nc2_col_text());
    lvds_draw_ellipse(x, y, 6, 6, nc2_col_text());
    lvds_draw_line(x - 15, y, x - 8, y, nc2_col_text());
    lvds_draw_line(x + 8, y, x + 15, y, nc2_col_text());
    lvds_draw_line(x, y - 15, x, y - 8, nc2_col_text());
    lvds_draw_line(x, y + 8, x, y + 15, nc2_col_text());
}

static void nc2_arrowhead(int x, int y, int dir_x, int dir_y, lvds_color_t color)
{
    int px = -dir_y;
    int py = dir_x;

    lvds_draw_line(x, y, x - dir_x * 7 + px * 3, y - dir_y * 7 + py * 3, color);
    lvds_draw_line(x, y, x - dir_x * 7 - px * 3, y - dir_y * 7 - py * 3, color);
}

static void nc2_chuck_hatching(int x, int y, int w, int h, lvds_color_t color)
{
    int s;

    if (w <= 0 || h <= 0) {
        return;
    }
    for (s = -h; s < w; s += 8) {
        int x0 = s > 0 ? s : 0;
        int y0 = s > 0 ? 0 : -s;
        int x1 = (s + h) < w ? s + h : w;
        int y1 = (s + h) < w ? h : w - s;

        y1 = nc2_clampi(y1, 0, h);
        lvds_draw_line(x + x0, y + y0, x + x1, y + y1, color);
    }
    for (s = 0; s < w + h; s += 8) {
        int x0 = s < w ? s : w;
        int y0 = s < w ? 0 : s - w;
        int x1 = s < h ? 0 : s - h;
        int y1 = s < h ? s : h;

        x1 = nc2_clampi(x1, 0, w);
        y0 = nc2_clampi(y0, 0, h);
        lvds_draw_line(x + x0, y + y0, x + x1, y + y1, color);
    }
}

/* The chuck: the jaws the work is held in, hatched the DIN way so the pane reads
   as a machine drawing. */
static void nc2_chuck(const nc2_preview_info_t *p, int stock_left, int stock_top,
                      int stock_w, int stock_h)
{
    int c_w = (int)(p->chuck_c * (float)stock_w / (p->visible_z > 0.0f
                                                   ? p->visible_z : 1.0f));

    if (c_w <= 0) {
        return;
    }
    if (c_w > stock_w / 3) {
        c_w = stock_w / 3;
    }
    nc2_fill(stock_left, stock_top, c_w, stock_h, nc2_col_prev_hatch());
    nc2_chuck_hatching(stock_left, stock_top, c_w, stock_h, nc2_col_prev_frame());
    nc2_frame(stock_left, stock_top, c_w, stock_h, nc2_col_prev_frame());
}

static void nc2_diameter_dimension(int x, int y0, int y1, const char *label)
{
    lvds_draw_line(x, y0, x, y1, nc2_col_dim());
    nc2_arrowhead(x, y1, 0, 1, nc2_col_dim());
    if (label && label[0]) {
        nc2_text(x + 6, y1 - 8, label, nc2_col_dim(), nc2_col_prev_bg(),
                 LVDS_FONT_NORMAL);
    }
}

static void nc2_z_point_dimension(int start_x, int point_x, int zero_x,
                                  int center_y, int point_y, const char *label)
{
    int dim_y = center_y - 15;
    int ext_top = center_y - 20;
    int ext_bottom = point_y + 3;
    int text_x;

    if (ext_bottom < ext_top) {
        int t = ext_bottom;

        ext_bottom = ext_top;
        ext_top = t;
    }
    lvds_draw_line(point_x, ext_top, point_x, ext_bottom, nc2_col_dim());
    lvds_draw_line(start_x, dim_y, point_x, dim_y, nc2_col_dim());
    if (start_x == zero_x) {
        nc2_fill(start_x - 1, dim_y - 1, 3, 3, nc2_col_dim());
    } else {
        nc2_arrowhead(point_x, dim_y, -1, 0, nc2_col_dim());
    }
    if (!label || !label[0]) {
        return;
    }
    text_x = point_x - nc2_text_width(label, LVDS_FONT_SMALL) / 2;
    nc2_text(text_x, center_y - 26, label, nc2_col_dim(), nc2_col_prev_bg(),
             LVDS_FONT_SMALL);
}

static void nc2_x_point_dimension(int dim_x, int start_y, int point_y,
                                  int zero_y, int point_x, const char *label)
{
    lvds_draw_line(dim_x, start_y, dim_x, point_y, nc2_col_dim());
    lvds_draw_line(point_x + 3, point_y, dim_x, point_y, nc2_col_dim());
    if (start_y == zero_y) {
        nc2_fill(dim_x - 1, start_y - 1, 3, 3, nc2_col_dim());
    }
    nc2_arrowhead(dim_x, point_y, 0, 1, nc2_col_dim());
    if (!label || !label[0]) {
        return;
    }
    nc2_text(dim_x + 4, point_y - 6, label, nc2_col_dim(), nc2_col_prev_bg(),
             LVDS_FONT_SMALL);
}

/* The rulers and the two overall dimensions: the diameter of the stock and the
   finished diameter, plus the centrelines the dimensions are taken from. */
static void nc2_din_layer(const nc2_preview_info_t *p, int stock_left,
                          int stock_top, int stock_w, int stock_h, int z0_x)
{
    char label[16];
    int stock_right = stock_left + stock_w;
    int stock_bottom = stock_top + stock_h;
    int dim_x = stock_right + 14;
    int id_y;

    if (dim_x > LVDS_VIEW_WIDTH - 24) {
        dim_x = stock_right - 18;
    }
    nc2_dashdot(stock_left - 34, stock_top, stock_right + 18, stock_top,
                nc2_col_dim());
    nc2_dashdot(z0_x, stock_top - 26, z0_x, stock_bottom + 20, nc2_col_dim());
    nc2_origin_marker(z0_x, stock_top);

    snprintf(label, sizeof(label), "%.0f", (double)p->stock_x);
    nc2_diameter_dimension(dim_x, stock_top, stock_bottom, label);
    if (p->stock_i > 0.0f && p->stock_i < p->stock_x) {
        id_y = nc2_map_x(p, stock_top, stock_h, p->stock_i);
        nc2_dashdot(stock_left - 18, id_y, stock_right + 8, id_y, nc2_col_dim());
        snprintf(label, sizeof(label), "%.0f", (double)p->stock_i);
        nc2_diameter_dimension(dim_x - 18, stock_top, id_y, label);
    }
}

/* The point markers and the dimension callouts for the contour's own rows: the
   numbers the operator reads off the drawing. The point follows every line the
   controller is given, so an increment counts from where the row above left the
   tool. */
static void nc2_contour_points(const nc2_document_t *doc,
                               const nc2_preview_info_t *p, int z0_x,
                               int stock_left, int stock_right, int stock_w,
                               int stock_top, int stock_h)
{
    float x = p ? p->stock_x : 0.0f;
    float z = 0.0f;
    int prev_z_px = z0_x;
    int prev_x_py = stock_top;
    int x_dim = stock_right + 15;
    char label[16];
    size_t i;

    if (!doc || !p) {
        return;
    }
    for (i = 0u; i < doc->line_count; i++) {
        const char *line = doc->lines[i];
        g7x_contour_cmd_t cmd;
        float px = x;
        float pz = z;
        bool moved;

        if (!(doc->path[0] && nc2_path_is_program(doc->path))) {
            continue;                   /* text is not a drawing */
        }
        moved = nc2_emit_line_is_direct(line) &&
                nc2_emit_line_point(line, &px, &pz, 0, 0u, 0);
        if (moved) {
            x = px;
            z = pz;
        }
        cmd = g7x_contour_cmd_from_line(g7x_skip_line_number(line));
        if (cmd == G7X_CONTOUR_NONE || cmd == G7X_CONTOUR_RAPID) {
            continue;
        }
        if (moved) {
            int mx = nc2_map_z(p, z0_x, stock_w, z);
            int my = nc2_map_x(p, stock_top, stock_h, x);
            float feature = 0.0f;

            snprintf(label, sizeof(label), "%.0f", (double)x);
            nc2_x_point_dimension(x_dim, prev_x_py, my, stock_top, mx, label);
            prev_x_py = my;
            snprintf(label, sizeof(label), "%.0f", (double)z);
            nc2_z_point_dimension(prev_z_px, mx, z0_x, stock_top, my, label);
            prev_z_px = mx;
            if (nc2_line_value(line, 'R', &feature) && feature > 0.0001f) {
                snprintf(label, sizeof(label), "R%.0f", (double)feature);
                nc2_text(mx + 4, my - 16, label, nc2_col_dim(),
                         nc2_col_prev_bg(), LVDS_FONT_SMALL);
            } else if (nc2_line_value(line, 'C', &feature) && feature > 0.0001f) {
                snprintf(label, sizeof(label), "C%.0f", (double)feature);
                nc2_text(mx + 4, my - 16, label, nc2_col_dim(),
                         nc2_col_prev_bg(), LVDS_FONT_SMALL);
            }
        }
    }
    snprintf(label, sizeof(label), "%.0f", (double)-p->visible_z);
    nc2_z_point_dimension(prev_z_px, stock_left, z0_x, stock_top, stock_top,
                          label);
}

/* One emitted line, drawn: rapids dashed and thin, cuts solid, the roughing
   passes pale and the finish cut in the stock's own colour so the three read
   apart. */
static void nc2_emitted_line(const nc2_preview_info_t *p, int z0_x, int stock_w,
                             int stock_top, int stock_h, const char *line,
                             nc2_preview_segment_t segment, float *last_x,
                             float *last_z, bool *have_last)
{
    g7x_contour_cmd_t cmd;
    float x;
    float z;
    int x0;
    int y0;
    int x1;
    int y1;
    lvds_color_t color = nc2_col_prev_profile();

    if (!p || !line || !last_x || !last_z || !have_last) {
        return;
    }
    cmd = g7x_contour_cmd_from_line(line);
    if (cmd == G7X_CONTOUR_NONE || cmd == G7X_CONTOUR_END) {
        return;
    }
    x = *last_x;
    z = *last_z;
    if (!nc2_emit_line_point(line, &x, &z, 0, 0u, 0)) {
        return;
    }
    if (!*have_last) {
        *last_x = x;
        *last_z = z;
        *have_last = true;
        return;
    }
    x0 = nc2_map_z(p, z0_x, stock_w, *last_z);
    y0 = nc2_map_x(p, stock_top, stock_h, *last_x);
    x1 = nc2_map_z(p, z0_x, stock_w, z);
    y1 = nc2_map_x(p, stock_top, stock_h, x);
    if (segment == NC2_SEG_ROUGH) {
        color = nc2_col_prev_cut();
    }
    if (cmd == G7X_CONTOUR_RAPID) {
        nc2_dashdot(x0, y0, x1, y1, nc2_col_dim());
    } else {
        lvds_draw_line(x0, y0, x1, y1, color);
    }
    *last_x = x;
    *last_z = z;
}

/* The part as the cycles cut it: walk the sender and draw every move it emits,
   so what is on the glass is what the machine is handed - roughing passes and
   all. */
static void nc2_emitted_preview(const nc2_document_t *doc,
                                const nc2_preview_info_t *p, int z0_x,
                                int stock_w, int stock_top, int stock_h)
{
    nc2_emit_stream_t stream;
    float last_x = 0.0f;
    float last_z = 0.0f;
    bool have_last = false;
    bool first_point = true;
    size_t emitted_line = 0u;
    size_t guard = 0u;
    nc2_preview_segment_t segment = NC2_SEG_FEED;

    if (!doc || !p) {
        return;
    }
    nc2_emit_stream_begin(&stream, doc, 0u);
    nc2_emit_stream_set_log(&stream, false);
    while (stream.active && guard++ < 4096u) {
        char line[NC2_MAX_LINE_LEN];
        nc2_emit_result_t result = nc2_emit_stream_next(&stream, line,
                                                        sizeof(line),
                                                        &emitted_line);

        if (result == NC2_EMIT_ERROR) {
            break;
        }
        if (result != NC2_EMIT_LINE) {
            continue;
        }
        if (line[0] == '(') {
            if (strstr(line, "rough")) {
                segment = NC2_SEG_ROUGH;
            } else if (strstr(line, "finish")) {
                segment = NC2_SEG_FINISH;
            }
            continue;
        }
        if (first_point) {
            /* The first move only says where the tool is. */
            if (nc2_emit_line_point(line, &last_x, &last_z, 0, 0u, 0)) {
                first_point = false;
                have_last = true;
            }
            continue;
        }
        nc2_emitted_line(p, z0_x, stock_w, stock_top, stock_h, line, segment,
                         &last_x, &last_z, &have_last);
    }
}

/* A file that is not a program, shown as its own text: the entries are edited
   here, and reading one as G-code would draw nonsense. */
static void nc2_preview_text_file(const nc2_document_t *doc, int x, int y, int w,
                                  int h)
{
    int row_h = 14;
    int rows = (h - 24) / row_h;
    int cols = (w - 16) / nc2_col_width(LVDS_FONT_SMALL);
    int row;

    if (!doc || rows < 1 || cols < 4) {
        return;
    }
    for (row = 0; row < rows && (size_t)row < doc->line_count; row++) {
        nc2_text_clip(x + 8, y + 20 + row * row_h, doc->lines[row], cols,
                      nc2_col_text(), nc2_col_prev_bg(), LVDS_FONT_SMALL);
    }
}

/* --- the live stock -------------------------------------------------------

   The mask of what is still there while a run is cutting, so the operator sees
   the material the program has taken off: nc's own layer, kept whole (the stock's
   own colour for what is left, the pane's ground where the tool has been). The
   mask is one byte per pixel and lives in PSRAM on the machine; the desktop has
   no PSRAM, so the station's own backend hands out a scratch region of the same
   size (`lvds_host.c`) and the two draw the same picture. */
#define NC2_LIVE_STOCK_MAX_W 680
#define NC2_LIVE_STOCK_MAX_H 380
#define NC2_LIVE_STOCK_PSRAM_OFFSET (512u * 1024u)

static uint8_t *g_nc2_live_mask;
static bool g_nc2_live_ready;
static bool g_nc2_live_was_cutting;
static bool g_nc2_live_has_last;
static int g_nc2_live_w;
static int g_nc2_live_h;
static float g_nc2_live_setup_x;
static float g_nc2_live_setup_z;
static float g_nc2_live_setup_i;
static float g_nc2_live_last_x;         /* diameters, as the program reads them */
static float g_nc2_live_last_z;

static bool nc2_live_alloc(void)
{
    if (g_nc2_live_mask) {
        return true;
    }
#if NC2_PREVIEW_HAVE_PSRAM
    if (!lvds_psram_available()) {
        (void)lvds_psram_init();
    }
    if (lvds_psram_available()) {
        g_nc2_live_mask = (uint8_t *)lvds_psram_ptr(NC2_LIVE_STOCK_PSRAM_OFFSET);
    }
#endif
    return g_nc2_live_mask != 0;
}

/* The stock the mask was made for: a different stock, a different box or a
   different setup and the mask means nothing - it is made again. */
static bool nc2_live_context_changed(const nc2_preview_info_t *p, int stock_w,
                                     int stock_h)
{
    return !g_nc2_live_ready || g_nc2_live_w != stock_w ||
           g_nc2_live_h != stock_h || g_nc2_live_setup_x != p->stock_x ||
           g_nc2_live_setup_z != p->visible_z ||
           g_nc2_live_setup_i != p->stock_i;
}

static void nc2_live_reset(const nc2_preview_info_t *p, int stock_w, int stock_h)
{
    int material_top = 0;
    int y;

    g_nc2_live_ready = false;
    g_nc2_live_has_last = false;
    if (!p || !nc2_live_alloc()) {
        return;                     /* no room: the drawing stays the static one */
    }
    g_nc2_live_w = nc2_clampi(stock_w, 1, NC2_LIVE_STOCK_MAX_W);
    g_nc2_live_h = nc2_clampi(stock_h, 1, NC2_LIVE_STOCK_MAX_H);
    memset(g_nc2_live_mask, 0,
           (size_t)NC2_LIVE_STOCK_MAX_W * NC2_LIVE_STOCK_MAX_H);
    /* The bore is not material: the mask starts below it, as the stock's own
       inner diameter does. */
    if (p->stock_i > 0.0f && p->stock_x > 0.0f) {
        material_top = nc2_clampi(
            (int)((p->stock_i / p->stock_x) * (float)g_nc2_live_h), 0,
            g_nc2_live_h - 1);
    }
    for (y = material_top; y < g_nc2_live_h; y++) {
        memset(g_nc2_live_mask + (size_t)y * NC2_LIVE_STOCK_MAX_W, 1,
               (size_t)g_nc2_live_w);
    }
    g_nc2_live_setup_x = p->stock_x;
    g_nc2_live_setup_z = p->visible_z;
    g_nc2_live_setup_i = p->stock_i;
    g_nc2_live_ready = true;
}

/* Take a rectangle of material away. */
static void nc2_live_remove(int x0, int y0, int x1, int y1)
{
    int y;

    if (!g_nc2_live_mask || !g_nc2_live_ready) {
        return;
    }
    x0 = nc2_clampi(x0, 0, g_nc2_live_w - 1);
    x1 = nc2_clampi(x1, 0, g_nc2_live_w - 1);
    y0 = nc2_clampi(y0, 0, g_nc2_live_h - 1);
    y1 = nc2_clampi(y1, 0, g_nc2_live_h - 1);
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
        memset(g_nc2_live_mask + (size_t)y * NC2_LIVE_STOCK_MAX_W + x0, 0,
               (size_t)(x1 - x0 + 1));
    }
}

/* The cut between two points: a turning tool takes a band off, so the removal is
   walked along the move at the drawing's own resolution - the samples are what
   keeps a fast move from leaving gaps. */
static void nc2_live_sweep(const nc2_preview_info_t *p, int z0_x, int stock_left,
                           int stock_w, int stock_top, int stock_h, float x0,
                           float z0, float x1, float z1)
{
    int sx0;
    int sx1;
    int sy0;
    int sy1;
    int samples;
    int i;

    if (!p || !g_nc2_live_ready) {
        return;
    }
    sx0 = nc2_map_z(p, z0_x, stock_w, z0) - stock_left;
    sx1 = nc2_map_z(p, z0_x, stock_w, z1) - stock_left;
    sy0 = nc2_map_x(p, stock_top, stock_h, x0) - stock_top;
    sy1 = nc2_map_x(p, stock_top, stock_h, x1) - stock_top;
    samples = nc2_clampi((sx1 - sx0) < 0 ? sx0 - sx1 : sx1 - sx0, 1, 80);
    if ((sy1 - sy0) < 0 ? (sy0 - sy1) > samples : (sy1 - sy0) > samples) {
        samples = nc2_clampi((sy1 - sy0) < 0 ? sy0 - sy1 : sy1 - sy0, 1, 80);
    }
    for (i = 0; i <= samples; i++) {
        float t = (float)i / (float)samples;
        int sx = sx0 + (int)((float)(sx1 - sx0) * t);
        int sy = sy0 + (int)((float)(sy1 - sy0) * t);

        nc2_live_remove(sx - 1, sy, sx + 1, g_nc2_live_h - 1);
    }
}

static void nc2_live_update(const nc2_preview_info_t *p,
                            const nc2_preview_run_t *run, int z0_x,
                            int stock_left, int stock_w, int stock_top,
                            int stock_h)
{
    /* The machine's X is a radius; the mask is in the program's own frame, so
       the walk is in diameters and the mapping halves it back. */
    float diam_x = (run->x < 0.0f ? -run->x : run->x) * 2.0f;

    if (g_nc2_live_has_last) {
        nc2_live_sweep(p, z0_x, stock_left, stock_w, stock_top, stock_h,
                       g_nc2_live_last_x, g_nc2_live_last_z, diam_x, run->z);
    } else {
        nc2_live_sweep(p, z0_x, stock_left, stock_w, stock_top, stock_h, diam_x,
                       run->z, diam_x, run->z);
    }
    g_nc2_live_last_x = diam_x;
    g_nc2_live_last_z = run->z;
    g_nc2_live_has_last = true;
}

/* What is left of the stock, drawn: the material in the stock's own colour and
   the cut away part in the pane's ground. */
static void nc2_live_draw(int stock_left, int stock_top)
{
    int y;

    if (!g_nc2_live_mask || !g_nc2_live_ready) {
        return;
    }
    nc2_fill(stock_left, stock_top, g_nc2_live_w, g_nc2_live_h,
             nc2_col_prev_bg());
    for (y = 0; y < g_nc2_live_h; y++) {
        const uint8_t *row = g_nc2_live_mask + (size_t)y * NC2_LIVE_STOCK_MAX_W;
        int x = 0;

        while (x < g_nc2_live_w) {
            int start;

            while (x < g_nc2_live_w && !row[x]) {
                x++;
            }
            start = x;
            while (x < g_nc2_live_w && row[x]) {
                x++;
            }
            if (x > start) {
                nc2_fill(stock_left + start, stock_top + y, x - start, 1,
                         nc2_col_prev_stock());
            }
        }
    }
}

void nc2_preview_draw(const nc2_document_t *doc, const nc2_preview_run_t *run,
                      int x, int y, int w, int h)
{
    nc2_preview_info_t preview;
    float usable_w;
    float usable_h;
    float z_scale;
    float x_scale;
    float scale;
    int stock_w;
    int stock_h;
    int stock_left;
    int stock_top;
    int z0_x;

    nc2_fill(x, y, w, h, nc2_col_prev_bg());
    if (doc && doc->path[0] && !nc2_path_is_program(doc->path)) {
        nc2_preview_text_file(doc, x, y, w, h);
        return;
    }
    nc2_preview_collect(doc, &preview);

    usable_w = (float)(w - 72);
    usable_h = (float)(h - NC2_PREVIEW_TOP_BAND - 28);
    if (usable_w < 40.0f) usable_w = 40.0f;
    if (usable_h < 40.0f) usable_h = 40.0f;
    z_scale = usable_w / preview.visible_z;
    x_scale = usable_h / (preview.stock_x * 0.5f);
    scale = z_scale < x_scale ? z_scale : x_scale;
    if (scale <= 0.0f) scale = 1.0f;
    stock_w = (int)(preview.visible_z * scale + 0.5f);
    stock_h = (int)((preview.stock_x * 0.5f) * scale + 0.5f);
    stock_w = nc2_clampi(stock_w, 24, (int)usable_w);
    stock_h = nc2_clampi(stock_h, 24, (int)usable_h);
    stock_left = x + 20;
    stock_top = y + NC2_PREVIEW_TOP_BAND;
    z0_x = stock_left + (int)(preview.stock_z * scale + 0.5f);
    z0_x = nc2_clampi(z0_x, stock_left, stock_left + stock_w);

    /* The stock: the plain block, or - while the machine is cutting, and on the
       run screen afterwards - what is left of it, from the live mask. */
    {
        bool live = run && run->busy;
        bool context_changed = nc2_live_context_changed(&preview, stock_w,
                                                       stock_h);
        bool keep = run && run->screen_run && !run->busy && g_nc2_live_ready &&
                    !context_changed;

        /* A cut starts a part, and nothing else does: the mask is made again
           when the machine starts cutting, and a run that has parked keeps what
           it made (`nc`'s own rule - the finished part stays on the glass until
           the drawing is asked for something else). */
        if (live && (!g_nc2_live_was_cutting || context_changed)) {
            nc2_live_reset(&preview, stock_w, stock_h);
        }
        g_nc2_live_was_cutting = live || keep;
        if ((live || keep) && g_nc2_live_ready) {
            if (live) {
                nc2_live_update(&preview, run, z0_x, stock_left, stock_w,
                                stock_top, stock_h);
            }
            nc2_chuck(&preview, stock_left, stock_top, stock_w, stock_h);
            nc2_live_draw(stock_left, stock_top);
        } else {
            nc2_chuck(&preview, stock_left, stock_top, stock_w, stock_h);
            nc2_fill(stock_left, stock_top, stock_w, stock_h,
                     nc2_col_prev_stock());
            if (preview.stock_i > 0.0f) {
                int id_h =
                    nc2_map_x(&preview, stock_top, stock_h, preview.stock_i) -
                    stock_top;

                if (id_h > 0 && id_h < stock_h) {
                    nc2_fill(stock_left, stock_top, stock_w, id_h,
                             nc2_col_prev_bg());
                }
            }
            if (live) {
                /* The mask is what shows the cut; with nowhere to put it, say so
                   rather than drawing a stock that never changes. */
                nc2_text_clip(stock_left + 4, stock_top + 16,
                              "Live stock needs PSRAM", 28, nc2_col_error(),
                              nc2_col_prev_bg(), LVDS_FONT_NORMAL);
            }
        }
    }
    nc2_din_layer(&preview, stock_left, stock_top, stock_w, stock_h, z0_x);
    nc2_contour_points(doc, &preview, z0_x, stock_left, stock_left + stock_w,
                       stock_w, stock_top, stock_h);
    nc2_emitted_preview(doc, &preview, z0_x, stock_w, stock_top, stock_h);
    /* The tool, riding the cut: the machine's own position, drawn as the shape
       its table row describes - nc's live tool, which the port had left out. It
       is only drawn while the machine is moving, and only when the whole glyph
       is inside the pane: a tool drawn outside the area that repaints would
       stay on the glass (`nc`'s own rule). */
    if (run && run->busy && run->tool && run->tool->valid) {
        int tx = nc2_map_z(&preview, z0_x, stock_w, run->z);
        int ty = nc2_map_x(&preview, stock_top, stock_h,
                           (run->x < 0.0f ? -run->x : run->x) * 2.0f);

        if (tx >= x && tx + NC2_LIVE_TOOL_GLYPH <= x + w &&
            ty >= y && ty + NC2_LIVE_TOOL_GLYPH <= y + h) {
            nc2_draw_tool_glyph(tx, ty, NC2_LIVE_TOOL_GLYPH, run->tool,
                                nc2_col_prev_bg());
        }
    }
}
