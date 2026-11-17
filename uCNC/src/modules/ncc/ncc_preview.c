#include "ncc_preview.h"

#include "ncc_draw.h"
#include "../nc_sender/nc_sender.h"
#include "ncc_files.h"
#include "ncc_tools.h"

#include "../g7x/g7x_contour.h"
#include "../../cnc.h"
#include "../lvds_renderer/lvds_hstx.h"
#if __has_include("../lvds_renderer/lvds_psram.h")
#include "../lvds_renderer/lvds_psram.h"
#define NCC_PREVIEW_HAVE_PSRAM 1
#else
#define NCC_PREVIEW_HAVE_PSRAM 0
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The pane's furniture: a band at the top for the ruler labels, a margin under
   the drawing, and the chuck at the left of the stock. The numbers are nc's
   (`nc_layout.h`), so the drawing sits where the old one sat. */
/* The band the rulers and the callouts live in. nc's was 82 and the bench kept
   asking to take the empty part of it away - *"i still 25-30 empty px on top of
   our image of preview"* - so the drawing starts 28 pixels higher and the band
   is what the dimension layer actually reaches (the tallest callout sits 34
   pixels above the stock). */
#define NCC_PREVIEW_TOP_BAND 54
#define NCC_CHUCK_C 15.0f

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
} ncc_preview_info_t;

/* The frames the drawing meets, and the one place they are converted:

   - the **program's**: what the document writes. X is a diameter in G7 (the
     lathe's default) and a radius in G8; Z is a length.
   - the **drawing's**: X is always a diameter, so `stock_x`, the tool's `R` and
     every row of the pane have one meaning whatever mode the program is in.
   - the **machine's**: the axis position the runtime reports, in axis
     millimetres - X is a radius - with the work offset still in it.
   - the **mask's**: the stock's own pixels in PSRAM, which is the drawing's
     frame without the stock's origin.

   A document line updates `diameter`; a machine point goes through the work
   offset once. Nothing else in this file halves or doubles X. */
typedef struct {
    bool diameter;              /* G7 (a diameter) or G8 (a radius) */
    float work_x;               /* the active work offset, axis mm (a radius) */
    float work_z;
} ncc_frame_t;

static void ncc_frame_document(ncc_frame_t *f)
{
    if (!f) {
        return;
    }
    f->diameter = true;
    f->work_x = 0.0f;
    f->work_z = 0.0f;
}

/* The machine's frame, read from the core's own modal state: the coordinate
   system in use (`G54`...) plus `G92`. It is the same offset `parser_machine_to_work()`
   subtracts for the DRO, so a part drawn from the machine's position lands on
   the rows the program wrote. */
static void ncc_frame_machine(ncc_frame_t *f)
{
    float coord[AXIS_COUNT] = { 0 };
    float g92[AXIS_COUNT] = { 0 };
    uint8_t system;

    if (!f) {
        return;
    }
    ncc_frame_document(f);
    system = parser_get_coord_system();
    if (system >= 54u) {
        parser_get_coordsys((uint8_t)(system - 54u), coord);
    }
    parser_get_coordsys(92u, g92);
    f->work_x = coord[AXIS_X] + g92[AXIS_X];
    f->work_z = coord[AXIS_Z] + g92[AXIS_Z];
}

/* A `G7`/`G8` anywhere on the line changes the mode for that line and the ones
   after it - the modal group the machine itself keeps. */
static void ncc_frame_line(ncc_frame_t *f, const char *line)
{
    ncc_field_t fields[NCC_MAX_FIELDS];
    int count;
    int i;

    if (!f || !line) {
        return;
    }
    count = ncc_fields(line, fields, NCC_MAX_FIELDS);
    for (i = 0; i < count; i++) {
        double g;

        if (fields[i].letter != 'G' || fields[i].value == fields[i].end) {
            continue;
        }
        g = strtod(line + fields[i].value, 0);
        if (g == 7.0) {
            f->diameter = true;
        } else if (g == 8.0) {
            f->diameter = false;
        }
    }
}

/* The program's X, as the drawing reads it: a diameter in both modes. */
static float ncc_frame_program_x(const ncc_frame_t *f, float x)
{
    return (f && !f->diameter) ? x * 2.0f : x;
}

/* The machine's axis point, as the drawing reads it. The axis is a radius in
   both modes, so this is the diameter the rows are fitted to. */
static float ncc_frame_machine_x(const ncc_frame_t *f, float axis_x)
{
    return (axis_x - (f ? f->work_x : 0.0f)) * 2.0f;
}

static float ncc_frame_machine_z(const ncc_frame_t *f, float axis_z)
{
    return axis_z - (f ? f->work_z : 0.0f);
}

/* The stock's own pixels: row 0 at its top (X0) and column 0 at its left end.
   The pane is these plus the stock's origin and the mask is these exactly -
   `ncc_pane_x()`/`ncc_pane_y()` are the only way back - so the cut and the
   drawing use one conversion. */
static int ncc_stock_row(const ncc_preview_info_t *p, int stock_h,
                         float x_diam)
{
    if (!p || p->stock_x <= 0.0001f || stock_h <= 0) {
        return 0;
    }
    return (int)lroundf((x_diam / p->stock_x) * (float)stock_h);
}

static int ncc_stock_col(const ncc_preview_info_t *p, int stock_w, float z)
{
    if (!p || p->visible_z <= 0.0001f || stock_w <= 0) {
        return 0;
    }
    return (int)lroundf(((p->stock_z + z) / p->visible_z) * (float)stock_w);
}

static int ncc_pane_x(int stock_left, int mask_x)
{
    return stock_left + mask_x;
}

static int ncc_pane_y(int stock_top, int mask_y)
{
    return stock_top + mask_y;
}

static int ncc_mask_y(int stock_top, int pane_y)
{
    return pane_y - stock_top;
}

/* A point in the drawing's frame to the pane's pixels. */
static int ncc_map_x(const ncc_preview_info_t *p, int stock_top, int stock_h,
                     float x_diam)
{
    return ncc_pane_y(stock_top, ncc_stock_row(p, stock_h, x_diam));
}

static int ncc_map_z(const ncc_preview_info_t *p, int stock_left, int stock_w,
                     float z)
{
    return ncc_pane_x(stock_left, ncc_stock_col(p, stock_w, z));
}

typedef enum {
    NCC_SEG_FEED = 0,
    NCC_SEG_ROUGH,
    NCC_SEG_FINISH
} ncc_preview_segment_t;

/* The value of a letter on a line, read the way the editor reads it: the fields
   of the line, one letter each. */
static bool ncc_line_value(const char *line, char want, float *out)
{
    ncc_field_t fields[NCC_MAX_FIELDS];
    int count = ncc_fields(line, fields, NCC_MAX_FIELDS);
    int i;

    for (i = 0; i < count; i++) {
        if (fields[i].letter != want || fields[i].value == fields[i].end) {
            continue;
        }
        if (out) {
            char *end;
            double value = strtod(line + fields[i].value, &end);

            if (end == line + fields[i].value || !isfinite(value) ||
                fabs(value) > 1000000.0) {
                return false;
            }
            *out = (float)value;
        }
        return true;
    }
    return false;
}

static bool ncc_line_is_setup(const char *line, const char *command)
{
    return g7x_command_is(g7x_skip_line_number(line), command);
}

/* What the drawing needs, from the document: the setup rows (`G970`..`G973`),
   which are the pane's own numbers, and the points the contour reaches, which
   the drawing is fitted to. A point is read with the sender's rule, so an
   increment counts from the row above it. */
static void ncc_preview_collect(const ncc_document_t *doc, ncc_preview_info_t *p)
{
    ncc_frame_t frame;
    float last_x = 0.0f;
    float last_z = 0.0f;
    size_t i;

    memset(p, 0, sizeof(*p));
    p->stock_x = 50.0f;
    p->stock_z = 75.0f;
    p->stock_i = 0.0f;
    p->stock_e = 3.0f;
    p->chuck_c = NCC_CHUCK_C;
    p->min_x = 1000000.0f;
    p->min_z = 1000000.0f;
    if (!doc) {
        p->visible_z = p->stock_z + p->stock_e;
        return;
    }
    ncc_frame_document(&frame);
    last_x = p->stock_x;
    for (i = 0u; i < doc->line_count; i++) {
        const char *line = doc->lines[i];
        float px = last_x;
        float pz = last_z;
        float x_diam;
        g7x_contour_cmd_t cmd;

        ncc_frame_line(&frame, line);
        if (ncc_line_is_setup(line, "G971")) {
            (void)ncc_line_value(line, 'X', &p->stock_x);
            (void)ncc_line_value(line, 'Z', &p->stock_z);
            (void)ncc_line_value(line, 'I', &p->stock_i);
            (void)ncc_line_value(line, 'E', &p->stock_e);
        }
        if (ncc_line_is_setup(line, "G972")) {
            (void)ncc_line_value(line, 'C', &p->chuck_c);
        }

        /* The point follows every line the controller is given, so a rapid
           before a cycle is what that cycle's first row measures from. */
        if (nc_sender_emit_line_is_direct(line) &&
            nc_sender_emit_line_point(line, &px, &pz, 0, 0u, 0)) {
            last_x = px;
            last_z = pz;
        }

        cmd = g7x_contour_cmd_from_line(g7x_skip_line_number(line));
        if (cmd == G7X_CONTOUR_NONE || cmd == G7X_CONTOUR_END) {
            continue;
        }
        x_diam = ncc_frame_program_x(&frame, px);
        if (x_diam < p->min_x) p->min_x = x_diam;
        if (x_diam > p->max_x) p->max_x = x_diam;
        if (pz < p->min_z) p->min_z = pz;
        if (pz > p->max_z) p->max_z = pz;
    }
    if (p->stock_x <= 0.0f) p->stock_x = 50.0f;
    if (p->stock_z <= 0.0f) p->stock_z = 75.0f;
    if (p->stock_i < 0.0f) p->stock_i = 0.0f;
    if (p->stock_i >= p->stock_x) p->stock_i = 0.0f;
    if (p->stock_e < 0.0f) p->stock_e = 0.0f;
    if (p->chuck_c <= 0.0f) p->chuck_c = NCC_CHUCK_C;
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

float ncc_preview_stock_x(const ncc_document_t *doc)
{
    ncc_preview_info_t info;

    if (!doc) {
        return 0.0f;
    }
    memset(&info, 0, sizeof(info));
    ncc_preview_collect(doc, &info);
    return info.stock_x;
}

static void ncc_dashdot(int x0, int y0, int x1, int y1, lvds_color_t color)
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
static void ncc_origin_marker(int x, int y)
{
    lvds_draw_ellipse(x, y, 11, 11, ncc_col_text());
    lvds_draw_ellipse(x, y, 6, 6, ncc_col_text());
    lvds_draw_line(x - 15, y, x - 8, y, ncc_col_text());
    lvds_draw_line(x + 8, y, x + 15, y, ncc_col_text());
    lvds_draw_line(x, y - 15, x, y - 8, ncc_col_text());
    lvds_draw_line(x, y + 8, x, y + 15, ncc_col_text());
}

static void ncc_arrowhead(int x, int y, int dir_x, int dir_y, lvds_color_t color)
{
    int px = -dir_y;
    int py = dir_x;

    lvds_draw_line(x, y, x - dir_x * 7 + px * 3, y - dir_y * 7 + py * 3, color);
    lvds_draw_line(x, y, x - dir_x * 7 - px * 3, y - dir_y * 7 - py * 3, color);
}

/* The chuck: the jaws the work is held in, as nc drew them (`nc_preview_chuck()`
   and its relief, `nc_preview.c`) - a plain dark-grey bar along the bottom of
   the pane that runs from the pane's left edge to a little past the stock's
   left end, its length and its thickness both read from `G972 C`, with the
   jaw's pivot circle at the stock's foot.

   The port had replaced that with a vertical block *inside* the stock, and the
   stock was then filled over it, so the chuck was never on the glass and `G972`
   had nothing to change (bench: "chuck g972 stopped working"). Drawing it after
   the stock is what lets it be seen at all - and the bench's own reading of the
   bar's length is the clamp length. */
static void ncc_chuck(const ncc_preview_info_t *p, int pane_x, int pane_w,
                      int pane_bottom, int stock_left, int stock_top,
                      int stock_w, int stock_h)
{
    float c = p->chuck_c > 0.0f ? p->chuck_c : NCC_CHUCK_C;
    float visible_z = p->visible_z > 0.0f ? p->visible_z : 1.0f;
    float stock_x = p->stock_x > 0.0f ? p->stock_x : 1.0f;
    int c_w = (int)((c / visible_z) * (float)stock_w + 0.5f);
    int c_h = (int)((c / stock_x) * (float)stock_h + 0.5f);
    int block_w;
    int block_y;

    if (c_w < 8) {
        c_w = 8;
    }
    if (c_h < 8) {
        c_h = 8;
    }
    block_w = stock_left + c_w - pane_x;
    if (block_w > pane_w) {
        block_w = pane_w;
    }
    /* Half of it under the stock, exactly as nc drew it: the bar straddles the
       stock's bottom edge, so the half below it is what the operator sees and
       the half over the work is covered by the stock fill drawn after it
       (bench: "in nc it was as needed - half of it is under stock"). */
    block_y = stock_top + stock_h - c_h / 2;
    if (block_y + c_h > pane_bottom) {
        c_h = pane_bottom - block_y;
    }
    if (block_w <= 0 || c_h <= 0) {
        return;
    }
    ncc_fill(pane_x, block_y, block_w, c_h, ncc_col_prev_chuck());
    ncc_frame(pane_x, block_y, block_w, c_h, ncc_col_prev_chuck_ink());
}

/* The jaw's pivot: the small circle centred on the stock's bottom corner, nc's
   own relief mark - what says the bar and the work are one held thing. Its
   upper half is under the stock, like the bar's, and the stock fill drawn after
   it leaves the lower half on the glass. */
static void ncc_chuck_relief(const ncc_preview_info_t *p, int stock_left,
                             int stock_top, int stock_h)
{
    float c = p->chuck_c > 0.0f ? p->chuck_c : NCC_CHUCK_C;
    float stock_x = p->stock_x > 0.0f ? p->stock_x : 1.0f;
    int r = (int)((c / stock_x) * (float)stock_h + 0.5f) / 8;

    if (r < 2) {
        r = 2;
    }
    lvds_draw_fill_ellipse(stock_left, stock_top + stock_h, r, r,
                           ncc_col_prev_bg());
    lvds_draw_ellipse(stock_left, stock_top + stock_h, r, r,
                      ncc_col_prev_chuck_ink());
}

static void ncc_diameter_dimension(int x, int y0, int y1, const char *label)
{
    lvds_draw_line(x, y0, x, y1, ncc_col_dim());
    ncc_arrowhead(x, y1, 0, 1, ncc_col_dim());
    if (label && label[0]) {
        ncc_text(x + 6, y1 - 8, label, ncc_col_dim(), ncc_col_prev_bg(),
                 LVDS_FONT_NORMAL);
    }
}

static void ncc_z_point_dimension(int start_x, int point_x, int zero_x,
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
    lvds_draw_line(point_x, ext_top, point_x, ext_bottom, ncc_col_dim());
    lvds_draw_line(start_x, dim_y, point_x, dim_y, ncc_col_dim());
    if (start_x == zero_x) {
        ncc_fill(start_x - 1, dim_y - 1, 3, 3, ncc_col_dim());
    } else {
        ncc_arrowhead(point_x, dim_y, -1, 0, ncc_col_dim());
    }
    if (!label || !label[0]) {
        return;
    }
    text_x = point_x - ncc_text_width(label, LVDS_FONT_SMALL) / 2;
    ncc_text(text_x, center_y - 26, label, ncc_col_dim(), ncc_col_prev_bg(),
             LVDS_FONT_SMALL);
}

static void ncc_x_point_dimension(int dim_x, int start_y, int point_y,
                                  int zero_y, int point_x, const char *label)
{
    lvds_draw_line(dim_x, start_y, dim_x, point_y, ncc_col_dim());
    lvds_draw_line(point_x + 3, point_y, dim_x, point_y, ncc_col_dim());
    if (start_y == zero_y) {
        ncc_fill(dim_x - 1, start_y - 1, 3, 3, ncc_col_dim());
    }
    ncc_arrowhead(dim_x, point_y, 0, 1, ncc_col_dim());
    if (!label || !label[0]) {
        return;
    }
    ncc_text(dim_x + 4, point_y - 6, label, ncc_col_dim(), ncc_col_prev_bg(),
             LVDS_FONT_SMALL);
}

/* The stock's outer diameter is the drawing's reference in both `#` states, so
   it has one owner here and is not repeated by contour-point dimensions at the
   same X. The DIN layer adds the rulers and the finished/bore diameter. */
static void ncc_stock_diameter_dimension(const ncc_preview_info_t *p,
                                         int stock_left, int stock_top,
                                         int stock_w, int stock_h)
{
    char label[16];
    int stock_right = stock_left + stock_w;
    int dim_x = stock_right + 14;

    if (dim_x > LVDS_VIEW_WIDTH - 24) {
        dim_x = stock_right - 18;
    }
    snprintf(label, sizeof(label), "%.0f", (double)p->stock_x);
    ncc_diameter_dimension(dim_x, stock_top, stock_top + stock_h, label);
}

/* The rulers and the finished diameter, plus the centrelines the dimensions are
   taken from. The outer stock diameter is already on the glass in both modes. */
static void ncc_din_layer(const ncc_preview_info_t *p, int stock_left,
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
    ncc_dashdot(stock_left - 34, stock_top, stock_right + 18, stock_top,
                ncc_col_dim());
    ncc_dashdot(z0_x, stock_top - 26, z0_x, stock_bottom + 20, ncc_col_dim());
    ncc_origin_marker(z0_x, stock_top);

    if (p->stock_i > 0.0f && p->stock_i < p->stock_x) {
        id_y = ncc_map_x(p, stock_top, stock_h, p->stock_i);
        ncc_dashdot(stock_left - 18, id_y, stock_right + 8, id_y, ncc_col_dim());
        snprintf(label, sizeof(label), "%.0f", (double)p->stock_i);
        ncc_diameter_dimension(dim_x - 18, stock_top, id_y, label);
    }
}

/* The point markers and the dimension callouts for the contour's own rows: the
   numbers the operator reads off the drawing. The point follows every line the
   controller is given, so an increment counts from where the row above left the
   tool. */
/* The point on the contour a row reaches, marked: the operator's *own line* is
   filled and every other is a ring, so the row the editor's cursor is on is the
   one the drawing points at (nc's own marker, `nc_preview_contour_point_marker()`
   - the port had dropped it, and the bench missed it: 2026-09-28, *"we have
   missied in some point marking active select line in edit to line on preview
   screen"*). */
static void ncc_contour_point_marker(int x, int y, bool selected)
{
    if (selected) {
        lvds_draw_fill_ellipse(x, y, 3, 3, ncc_col_dim());
    } else {
        lvds_draw_ellipse(x, y, 5, 5, ncc_col_dim());
    }
}

static void ncc_contour_points(const ncc_document_t *doc,
                               const ncc_preview_info_t *p, int z0_x,
                               int stock_left, int stock_right, int stock_w,
                               int stock_top, int stock_h)
{
    ncc_frame_t frame;
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
    ncc_frame_document(&frame);
    for (i = 0u; i < doc->line_count; i++) {
        const char *line = doc->lines[i];
        g7x_contour_cmd_t cmd;
        float px = x;
        float pz = z;
        bool moved;

        if (!(doc->path[0] && ncc_path_is_program(doc->path))) {
            continue;                   /* text is not a drawing */
        }
        ncc_frame_line(&frame, line);
        moved = nc_sender_emit_line_is_direct(line) &&
                nc_sender_emit_line_point(line, &px, &pz, 0, 0u, 0);
        if (moved) {
            x = px;
            z = pz;
        }
        cmd = g7x_contour_cmd_from_line(g7x_skip_line_number(line));
        if (cmd == G7X_CONTOUR_NONE || cmd == G7X_CONTOUR_RAPID) {
            continue;
        }
        if (moved) {
            int mx = ncc_map_z(p, stock_left, stock_w, z);
            int my = ncc_map_x(p, stock_top, stock_h,
                               ncc_frame_program_x(&frame, x));
            float feature = 0.0f;

            ncc_contour_point_marker(mx, my, i == doc->cursor);
            /* The stock's outer diameter already has its own dimension. A
               contour point at that X must not print the same number again. */
            if (fabsf(x - p->stock_x) > 0.0001f) {
                snprintf(label, sizeof(label), "%.0f", (double)x);
                ncc_x_point_dimension(x_dim, prev_x_py, my, stock_top, mx, label);
            }
            prev_x_py = my;
            snprintf(label, sizeof(label), "%.0f", (double)z);
            ncc_z_point_dimension(prev_z_px, mx, z0_x, stock_top, my, label);
            prev_z_px = mx;
            if (ncc_line_value(line, 'R', &feature) && feature > 0.0001f) {
                snprintf(label, sizeof(label), "R%.0f", (double)feature);
                ncc_text(mx + 4, my - 16, label, ncc_col_dim(),
                         ncc_col_prev_bg(), LVDS_FONT_SMALL);
            } else if (ncc_line_value(line, 'C', &feature) && feature > 0.0001f) {
                snprintf(label, sizeof(label), "C%.0f", (double)feature);
                ncc_text(mx + 4, my - 16, label, ncc_col_dim(),
                         ncc_col_prev_bg(), LVDS_FONT_SMALL);
            }
        }
    }
    snprintf(label, sizeof(label), "%.0f", (double)-p->visible_z);
    ncc_z_point_dimension(prev_z_px, stock_left, z0_x, stock_top, stock_top,
                          label);
}

/* One emitted line, drawn: rapids dashed and thin, cuts solid, the roughing
   passes pale and the finish cut in the stock's own colour so the three read
   apart. */
/* One arc of an emitted path, drawn as the arc it is: every point of it is
   walked and mapped like any other point of the drawing, so an `R` corner is
   round instead of the chord between its ends (bench, 2026-09-28: *"gren line of
   ofssete path for rnd shows chamfer path"*).

   The arc's centre comes the way the line writes it: `I`/`K` off the start
   point (what the cycle emits for an expanded corner), or from `R` - the two
   points and the radius give two centres, and the commanded turn picks the one
   whose arc is the short way, which is what a positive `R` means. False when the
   line says neither; the caller then draws the chord it would have drawn.

   **`I` and `R` are radius values, and the drawing's `X` is a diameter** - the
   lathe's own convention (`lathe_comp`, and the cycle's emitted `I5.000` for a
   five-millimetre corner) - so the arc is worked out in the *radius* plane and
   doubled back when each point is mapped. `x0`/`x1` are already in the
   drawing's frame, so a `G8` program is doubled once, by its frame, before it
   gets here. Working in the diameter plane instead is what drew a corner as a
   wide loop over the profile. */
static bool ncc_emitted_arc(const ncc_preview_info_t *p, int stock_left,
                            int stock_w,
                            int stock_top, int stock_h, const char *line,
                            float x0, float z0, float x1, float z1,
                            bool cw, int width, lvds_color_t color)
{
    float i;
    float k;
    float cx;
    float cz;
    float r;
    float rx0 = x0 * 0.5f;              /* the drawing's X is a diameter */
    float rx1 = x1 * 0.5f;
    float a0;
    float a1;
    float span;
    int steps;
    int step;
    float px = rx0;
    float pz = z0;

    if (ncc_line_value(line, 'I', &i)) {
        if (!ncc_line_value(line, 'K', &k)) {
            k = 0.0f;
        }
        cx = rx0 + i;
        cz = z0 + k;
        r = sqrtf((rx0 - cx) * (rx0 - cx) + (z0 - cz) * (z0 - cz));
        if (r < 0.0001f) {
            return false;
        }
    } else {
        float rad;
        float dx;
        float dz;
        float d;
        float h2;
        float h;
        float mx;
        float mz;
        float nx;
        float nz;
        float a;
        float b;
        float sweep;

        if (!ncc_line_value(line, 'R', &rad) || fabsf(rad) < 0.0001f) {
            return false;
        }
        dx = rx1 - rx0;
        dz = z1 - z0;
        d = sqrtf(dx * dx + dz * dz);
        if (d < 0.0001f) {
            return false;
        }
        h2 = rad * rad - d * d * 0.25f;
        h = h2 > 0.0f ? sqrtf(h2) : 0.0f;
        mx = (rx0 + rx1) * 0.5f;
        mz = (z0 + z1) * 0.5f;
        nx = -dz / d;                   /* a unit normal to the chord */
        nz = dx / d;
        cx = mx + h * nx;
        cz = mz + h * nz;
        a = atan2f(rx0 - cz, z0 - cx);
        b = atan2f(rx1 - cz, z1 - cx);
        sweep = b - a;
        while (sweep <= 0.0f) {
            sweep += 6.28318531f;
        }
        while (sweep > 6.28318531f) {
            sweep -= 6.28318531f;
        }
        if (cw) {
            sweep = 6.28318531f - sweep; /* the commanded way round */
        }
        if (sweep > 3.14159265f) {
            cx = mx - h * nx;           /* the other centre: the short arc */
            cz = mz - h * nz;
        }
        r = fabsf(rad);
    }
    a0 = atan2f(rx0 - cx, z0 - cz);
    a1 = atan2f(rx1 - cx, z1 - cz);
    span = a1 - a0;
    /* The half a commanded turn takes: the angle in this frame grows the way a
       `G3` turns and shrinks the way a `G2` does, and neither is forced short -
       a corner round is short of its own accord, and a major arc stays major.
       (The other way round is what drew the corner as a loop: bench, 2026-09-28,
       *"seem still straigh line in panel"* was this, one turn early.) */
    if (cw) {
        while (span >= 0.0f) {
            span -= 6.28318531f;
        }
        while (span < -6.28318531f) {
            span += 6.28318531f;
        }
    } else {
        while (span <= 0.0f) {
            span += 6.28318531f;
        }
        while (span > 6.28318531f) {
            span -= 6.28318531f;
        }
    }
    /* One segment per couple of pixels of the arc, within reason: enough that a
       radius reads as a curve and not as a chamfer, few enough to stay cheap in
       a frame that is drawn while the machine moves. */
    {
        int arc_px = (int)(fabsf(span) * r *
                           (float)(2 * stock_h) / (p->stock_x > 0.001f
                                                       ? p->stock_x
                                                       : 50.0f));

        steps = ncc_clampi(arc_px / 2, 4, 32);
    }
    for (step = 1; step <= steps; step++) {
        float t = (float)step / (float)steps;
        float a = a0 + span * t;
        float sx = cx + r * cosf(a);    /* a radius */
        float sz = cz + r * sinf(a);
        int mx0 = ncc_map_z(p, stock_left, stock_w, pz);
        int my0 = ncc_map_x(p, stock_top, stock_h, px * 2.0f);
        int mx1 = ncc_map_z(p, stock_left, stock_w, sz);
        int my1 = ncc_map_x(p, stock_top, stock_h, sx * 2.0f);

        lvds_draw_line_w(mx0, my0, mx1, my1, color, width);
        px = sx;
        pz = sz;
    }
    return true;
}

static void ncc_emitted_line(const ncc_preview_info_t *p, int stock_left,
                             int stock_w, int stock_top, int stock_h,
                             const ncc_frame_t *frame, const char *line,
                             ncc_preview_segment_t segment, float *last_x,
                             float *last_z, bool *have_last, bool debug,
                             bool selected)
{
    g7x_contour_cmd_t cmd;
    float x;
    float z;
    float x0d;
    float x1d;
    int x0;
    int y0;
    int x1;
    int y1;
    lvds_color_t color = ncc_col_prev_profile();

    if (!p || !line || !last_x || !last_z || !have_last) {
        return;
    }
    cmd = g7x_contour_cmd_from_line(line);
    if (cmd == G7X_CONTOUR_NONE || cmd == G7X_CONTOUR_END) {
        return;
    }
    x = *last_x;
    z = *last_z;
    if (!nc_sender_emit_line_point(line, &x, &z, 0, 0u, 0)) {
        return;
    }
    if (!*have_last) {
        *last_x = x;
        *last_z = z;
        *have_last = true;
        return;
    }
    x0d = ncc_frame_program_x(frame, *last_x);
    x1d = ncc_frame_program_x(frame, x);
    x0 = ncc_map_z(p, stock_left, stock_w, *last_z);
    y0 = ncc_map_x(p, stock_top, stock_h, x0d);
    x1 = ncc_map_z(p, stock_left, stock_w, z);
    y1 = ncc_map_x(p, stock_top, stock_h, x1d);
    if (segment == NCC_SEG_ROUGH) {
        color = ncc_col_prev_cut();
    }
    if (cmd == G7X_CONTOUR_ARC_CW || cmd == G7X_CONTOUR_ARC_CCW) {
        lvds_color_t arc_color = debug ? ncc_col_run() : color;
        bool cw = cmd == G7X_CONTOUR_ARC_CW;
        int width = selected ? 3 : 1;

        if (!ncc_emitted_arc(p, stock_left, stock_w, stock_top, stock_h, line,
                             x0d, *last_z, x1d, z, cw, width, arc_color)) {
            lvds_draw_line_w(x0, y0, x1, y1, arc_color, width);
        }
    } else if (cmd == G7X_CONTOUR_RAPID) {
        ncc_dashdot(x0, y0, x1, y1, debug ? ncc_col_run() : ncc_col_dim());
    } else if (debug || selected) {
        /* The debug line: the path the tool's centre follows, in the panel's
           own green, so the offset can be seen against the profile - and the
           row the editor's cursor is on drawn wide, so the line the operator is
           editing is the line the drawing points at (bench: *"marking active
           select line in edit"*). */
        lvds_draw_line_w(x0, y0, x1, y1, debug ? ncc_col_run() : color,
                         selected ? 3 : 1);
    } else {
        lvds_draw_line(x0, y0, x1, y1, color);
    }
    *last_x = x;
    *last_z = z;
}

/* Move the frame to the line the sender is about to draw: the `G7`/`G8` rows
   between the last one and this one are modal, and the row itself is included
   because a mode written on the same line applies to it. */
static void ncc_frame_advance(ncc_frame_t *f, const ncc_document_t *doc,
                              size_t *at, size_t line)
{
    if (!f || !doc || !at || line >= doc->line_count) {
        return;
    }
    while (*at < doc->line_count && *at <= line) {
        ncc_frame_line(f, doc->lines[*at]);
        (*at)++;
    }
}

static bool ncc_preview_tool_radius(void *user, int tool, float *radius)
{
    ncc_tool_t row;

    (void)user;
    if (!radius || tool <= 0 || !ncc_tools_number(tool, &row) ||
        row.r <= 0.0f) {
        return false;
    }
    *radius = row.r;
    return true;
}

/* The part as the cycles cut it: walk the sender and draw every move it emits,
   so what is on the glass is what the machine is handed - roughing passes and
   all. */
static void ncc_emitted_preview(const ncc_document_t *doc,
                                const ncc_preview_info_t *p, int stock_left,
                                int stock_w, int stock_top, int stock_h)
{
    nc_sender_emit_stream_t stream;
    g7x_doc_t source;
    ncc_frame_t frame;
    float last_x = 0.0f;
    float last_z = 0.0f;
    bool have_last = false;
    bool first_point = true;
    size_t emitted_line = 0u;
    size_t frame_at = 0u;
    size_t guard = 0u;
    ncc_preview_segment_t segment = NCC_SEG_FEED;

    if (!doc || !p) {
        return;
    }
    ncc_frame_document(&frame);
    source = ncc_document_g7x(doc);
    nc_sender_emit_stream_begin(&stream, &source, 0u,
                                ncc_preview_tool_radius, 0);
    nc_sender_emit_stream_set_log(&stream, false);
    while (stream.active && guard++ < 4096u) {
        char line[NCC_MAX_LINE_LEN];
        nc_sender_emit_result_t result = nc_sender_emit_stream_next(&stream, line,
                                                        sizeof(line),
                                                        &emitted_line);

        if (result == NC_SENDER_EMIT_ERROR) {
            break;
        }
        if (result != NC_SENDER_EMIT_LINE) {
            continue;
        }
        if (line[0] == '(') {
            if (strstr(line, "rough")) {
                segment = NCC_SEG_ROUGH;
            } else if (strstr(line, "finish")) {
                segment = NCC_SEG_FINISH;
            }
            continue;
        }
        ncc_frame_advance(&frame, doc, &frame_at, emitted_line);
        if (first_point) {
            /* The first move only says where the tool is. */
            if (nc_sender_emit_line_point(line, &last_x, &last_z, 0, 0u, 0)) {
                first_point = false;
                have_last = true;
            }
            continue;
        }
        ncc_emitted_line(p, stock_left, stock_w, stock_top, stock_h, &frame,
                         line, segment, &last_x, &last_z, &have_last, false,
                         emitted_line == doc->cursor);
    }
}

/* One shift, walked: the sender is asked for the finish path with the
   compensation the caller names - `radius` 0 for the path as programmed - and
   every move it hands over is drawn in the panel's own green over the profile. */
static void ncc_offset_walk(const ncc_document_t *doc,
                            const ncc_preview_info_t *p, int stock_left,
                            int stock_w, int stock_top, int stock_h,
                            g7x_comp_side_t side, float radius)
{
    nc_sender_emit_stream_t stream;
    g7x_doc_t source;
    ncc_frame_t frame;
    float last_x = 0.0f;
    float last_z = 0.0f;
    bool have_last = false;
    bool first_point = true;
    bool in_finish = true;      /* no cycle comment yet: the program *is* the
                                   profile, so it is all finish path */
    size_t emitted_line = 0u;
    size_t frame_at = 0u;
    size_t guard = 0u;
    ncc_preview_segment_t segment = NCC_SEG_FINISH;

    if (!doc || !p) {
        return;
    }
    ncc_frame_document(&frame);
    source = ncc_document_g7x(doc);
    nc_sender_emit_stream_begin(&stream, &source, 0u,
                                ncc_preview_tool_radius, 0);
    nc_sender_emit_stream_set_log(&stream, false);
    stream.comp_side = side;
    stream.comp_radius = radius;
    while (stream.active && guard++ < 4096u) {
        char line[NCC_MAX_LINE_LEN];
        nc_sender_emit_result_t result = nc_sender_emit_stream_next(&stream, line,
                                                        sizeof(line),
                                                        &emitted_line);

        if (result == NC_SENDER_EMIT_ERROR) {
            break;
        }
        if (result != NC_SENDER_EMIT_LINE) {
            continue;
        }
        if (line[0] == '(') {
            /* Only the *finish* pass is the offset tool path: the roughing
               passes are the cycle's own staircase, and drawing them green
               buried the profile under them (bench, 2026-09-28: "all lines are
               green. now only finish should be taken into account?"). */
            if (strstr(line, "rough")) {
                in_finish = false;
            } else if (strstr(line, "finish")) {
                in_finish = true;
            }
            continue;
        }
        if (!in_finish) {
            continue;
        }
        ncc_frame_advance(&frame, doc, &frame_at, emitted_line);
        if (first_point) {
            if (nc_sender_emit_line_point(line, &last_x, &last_z, 0, 0u, 0)) {
                first_point = false;
                have_last = true;
            }
            continue;
        }
        ncc_emitted_line(p, stock_left, stock_w, stock_top, stock_h, &frame,
                         line, segment, &last_x, &last_z, &have_last, true,
                         emitted_line == doc->cursor);
    }
}

/* The debug line in EDIT: **all three at once** - `G40` (the path as programmed,
   no shift at all), `G41` (the tool on the left) and `G42` (on the right) - each
   walked the way the cycle would cut it, all in the panel's green, so the two
   shifts and the path they came from are read against each other (bench,
   2026-09-28: *"now on offset for debug - drawm all three variants g40 g41 and
   42. all in green is ok"*).

   A program that already carries `G41`/`G42` of its own overrides the direction
   as the walk meets it; the three lines are the *candidates* a debug line is for.
   No radius in the table - no tool named - draws nothing: there is no shift to
   show and the profile is already on the glass. */
static void ncc_offset_preview(const ncc_document_t *doc,
                               const ncc_preview_info_t *p, int stock_left,
                               int stock_w, int stock_top, int stock_h)
{
    ncc_tool_t tool;

    if (!doc || !p || doc->line_count == 0u) {
        return;
    }
    if (!ncc_tools_active(doc, doc->line_count - 1u, &tool) ||
        tool.r <= 0.0001f) {
        return;                 /* nothing to offset by */
    }
    ncc_offset_walk(doc, p, stock_left, stock_w, stock_top, stock_h,
                    G7X_COMP_LEFT,
                    0.0f);                              /* G40 */
    ncc_offset_walk(doc, p, stock_left, stock_w, stock_top, stock_h,
                    G7X_COMP_LEFT,
                    tool.r);                            /* G41 */
    ncc_offset_walk(doc, p, stock_left, stock_w, stock_top, stock_h,
                    G7X_COMP_RIGHT,
                    tool.r);                            /* G42 */
}

/* A file that is not a program, shown as its own text: the entries are edited
   here, and reading one as G-code would draw nonsense. */
static void ncc_preview_text_file(const ncc_document_t *doc, int x, int y, int w,
                                  int h)
{
    int row_h = 14;
    int rows = (h - 24) / row_h;
    int cols = (w - 16) / ncc_col_width(LVDS_FONT_SMALL);
    int row;

    if (!doc || rows < 1 || cols < 4) {
        return;
    }
    for (row = 0; row < rows && (size_t)row < doc->line_count; row++) {
        ncc_text_clip(x + 8, y + 20 + row * row_h, doc->lines[row], cols,
                      ncc_col_text(), ncc_col_prev_bg(), LVDS_FONT_SMALL);
    }
}

/* --- the live stock -------------------------------------------------------

   The mask of what is still there while a run is cutting, so the operator sees
   the material the program has taken off: nc's own layer, kept whole (the stock's
   own colour for what is left, the pane's ground where the tool has been). The
   mask is one byte per pixel and lives in PSRAM on the machine; the desktop has
   no PSRAM, so the station's own backend hands out a scratch region of the same
   size (`lvds_host.c`) and the two draw the same picture. */
#define NCC_LIVE_STOCK_MAX_W 680
#define NCC_LIVE_STOCK_MAX_H 380
#define NCC_LIVE_STOCK_PSRAM_OFFSET (512u * 1024u)

static uint8_t *g_ncc_live_mask;
static bool g_ncc_live_ready;
static bool g_ncc_live_was_cutting;
static bool g_ncc_live_has_last;
static int g_ncc_live_w;
static int g_ncc_live_h;
static float g_ncc_live_setup_x;
static float g_ncc_live_setup_z;
static float g_ncc_live_setup_i;
static float g_ncc_live_work_x;
static float g_ncc_live_work_z;
static float g_ncc_live_last_x;         /* diameters, as the program reads them */
static float g_ncc_live_last_z;

/* Per-column material geometry and depth, refreshed only for columns a cut has
   touched. This is not a second mask: it is four small facts per Z column so a
   frame never scans the complete mask again. */
typedef struct {
    uint16_t inner;
    uint16_t outer;
    uint8_t base;
    uint8_t frac;
} ncc_live_column_t;

static ncc_live_column_t g_ncc_live_columns[NCC_LIVE_STOCK_MAX_W];
static uint8_t g_ncc_live_col_dirty[NCC_LIVE_STOCK_MAX_W];

/* Rows overpainted by the tool glyph, outside a mask change, plus whether the
   pane was painted whole. */
static int g_ncc_live_dirty_y0;
static int g_ncc_live_dirty_y1;

/* The nose circle as the last painted frame left it: the next frame takes it
   back with the same small box and lets the mask repaint the stock under it. */
static int g_ncc_tip_drawn;
static int g_ncc_tip_x;
static int g_ncc_tip_y;
static int g_ncc_tip_r;
static bool g_ncc_live_draw_all;

/* The parts of a frame, for the screen's meter. */
static uint32_t g_ncc_preview_stock_us;
static uint32_t g_ncc_preview_geom_us;

/* The `#` selection: the DIN layer (the rulers and the dimension callouts). It
   is on by default, which is nc's full picture, and the editor's `#` turns it
   into the stock and the path alone. RUN does not ask - see the draw. */
static bool g_ncc_preview_din = true;

bool ncc_preview_din(void)
{
    return g_ncc_preview_din;
}

bool ncc_preview_toggle_din(void)
{
    g_ncc_preview_din = !g_ncc_preview_din;
    return g_ncc_preview_din;
}

static bool ncc_live_alloc(void)
{
    if (g_ncc_live_mask) {
        return true;
    }
#if NCC_PREVIEW_HAVE_PSRAM
    if (!lvds_psram_available()) {
        (void)lvds_psram_init();
    }
    if (lvds_psram_available()) {
        g_ncc_live_mask = (uint8_t *)lvds_psram_ptr(NCC_LIVE_STOCK_PSRAM_OFFSET);
    }
#endif
    return g_ncc_live_mask != 0;
}

/* The stock the mask was made for: a different stock, a different box or a
   different setup and the mask means nothing - it is made again. */
static bool ncc_live_context_changed(const ncc_preview_info_t *p, int stock_w,
                                     int stock_h, const ncc_frame_t *frame)
{
    return !g_ncc_live_ready || g_ncc_live_w != stock_w ||
           g_ncc_live_h != stock_h || g_ncc_live_setup_x != p->stock_x ||
           g_ncc_live_setup_z != p->visible_z ||
           g_ncc_live_setup_i != p->stock_i ||
           g_ncc_live_work_x != frame->work_x ||
           g_ncc_live_work_z != frame->work_z;
}

static void ncc_live_mark_columns(int x0, int x1)
{
    x0 = ncc_clampi(x0, 0, g_ncc_live_w - 1);
    x1 = ncc_clampi(x1, 0, g_ncc_live_w - 1);
    if (x1 < x0) {
        int t = x0;

        x0 = x1;
        x1 = t;
    }
    memset(g_ncc_live_col_dirty + x0, 1, (size_t)(x1 - x0 + 1));
}

static void ncc_live_refresh_column(int col)
{
    ncc_live_column_t *column = &g_ncc_live_columns[col];
    int inner = -1;
    int outer = -1;
    int row;
    int span;
    int scaled;

    for (row = 0; row < g_ncc_live_h; row++) {
        if (g_ncc_live_mask[(size_t)row * NCC_LIVE_STOCK_MAX_W + col]) {
            if (inner < 0) {
                inner = row;
            }
            outer = row;
        }
    }
    if (outer < 0) {
        column->inner = 0xffffu;
        column->outer = 0xffffu;
        column->base = 0u;
        column->frac = 0u;
        return;
    }
    column->inner = (uint16_t)inner;
    column->outer = (uint16_t)outer;
    span = g_ncc_live_h > 1 ? g_ncc_live_h - 1 : 1;
    scaled = (g_ncc_live_h - 1 - outer) * 48 / span;
    column->base = (uint8_t)ncc_clampi(scaled / 16, 0, 3);
    column->frac = (uint8_t)(scaled & 15);
}

static void ncc_live_reset(const ncc_preview_info_t *p, int stock_w, int stock_h,
                           const ncc_frame_t *frame)
{
    int material_top = 0;
    int y;

    g_ncc_live_ready = false;
    g_ncc_live_has_last = false;
    if (!p || !ncc_live_alloc()) {
        return;                     /* no room: the drawing stays the static one */
    }
    g_ncc_live_w = ncc_clampi(stock_w, 1, NCC_LIVE_STOCK_MAX_W);
    g_ncc_live_h = ncc_clampi(stock_h, 1, NCC_LIVE_STOCK_MAX_H);
    memset(g_ncc_live_mask, 0,
           (size_t)NCC_LIVE_STOCK_MAX_W * NCC_LIVE_STOCK_MAX_H);
    /* The bore is not material: the mask starts below it, as the stock's own
       inner diameter does. */
    if (p->stock_i > 0.0f && p->stock_x > 0.0f) {
        material_top = ncc_clampi(
            (int)((p->stock_i / p->stock_x) * (float)g_ncc_live_h), 0,
            g_ncc_live_h - 1);
    }
    for (y = material_top; y < g_ncc_live_h; y++) {
        memset(g_ncc_live_mask + (size_t)y * NCC_LIVE_STOCK_MAX_W, 1,
               (size_t)g_ncc_live_w);
    }
    g_ncc_live_setup_x = p->stock_x;
    g_ncc_live_setup_z = p->visible_z;
    g_ncc_live_setup_i = p->stock_i;
    g_ncc_live_work_x = frame->work_x;
    g_ncc_live_work_z = frame->work_z;
    memset(g_ncc_live_col_dirty, 1, (size_t)g_ncc_live_w);
    g_ncc_live_dirty_y0 = g_ncc_live_h;
    g_ncc_live_dirty_y1 = -1;
    g_ncc_live_draw_all = true;
    g_ncc_live_ready = true;
}

/* Hand the mask's rows [y0, y1] to the drawing: something painted over them
   (the tool's own box, taken back) and the material has to come back. */
static void ncc_live_mark_rows(int y0, int y1)
{
    if (!g_ncc_live_ready) {
        return;
    }
    y0 = ncc_clampi(y0, 0, g_ncc_live_h - 1);
    y1 = ncc_clampi(y1, 0, g_ncc_live_h - 1);
    if (y0 < g_ncc_live_dirty_y0) {
        g_ncc_live_dirty_y0 = y0;
    }
    if (y1 > g_ncc_live_dirty_y1) {
        g_ncc_live_dirty_y1 = y1;
    }
}

/* Take a rectangle of material away. */
static void ncc_live_remove(int x0, int y0, int x1, int y1)
{
    int y;

    if (!g_ncc_live_mask || !g_ncc_live_ready) {
        return;
    }
    x0 = ncc_clampi(x0, 0, g_ncc_live_w - 1);
    x1 = ncc_clampi(x1, 0, g_ncc_live_w - 1);
    y0 = ncc_clampi(y0, 0, g_ncc_live_h - 1);
    y1 = ncc_clampi(y1, 0, g_ncc_live_h - 1);
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
    ncc_live_mark_columns(x0, x1);
    for (y = y0; y <= y1; y++) {
        uint8_t *slice = g_ncc_live_mask + (size_t)y * NCC_LIVE_STOCK_MAX_W + x0;
        int span = x1 - x0 + 1;

        /* The complete changed columns are handed to the drawing. A column's
           shade belongs to its new cut surface, so redrawing only the removed
           cells would leave the material above it in the previous depth
           colour. */
        memset(slice, 0, (size_t)span);
    }
}

/* The material the nose clears at one point of the path: **the circle itself**,
   of the row's own radius, with its lowest point on the point the program
   addresses - so what the brush leaves behind is round, and what it clears is
   exactly the disc it passed over (bench, 2026-09-28: *"the shape used in
   framebuffer to draw remaining of stock by traveling along path of machine by
   drawing bg color other[ than] stock color"*).

   The material the tool's own body has displaced goes too - a plunged turning
   tool takes the surface down to wherever its nose has reached, and a pass is
   that band walked along the path - but the *bound* of it is the nose's own
   **inner** arc: the circle's upper half on the glass, whose deepest point is
   the tip and which rises on either side. Clearing from the circle's *outer*
   half instead (the first attempt) left the cut flaring deeper at its ends and
   read as a square block, not a circle (bench, 2026-09-28: *"just brush is still
   square. not round"*). */
/* One rule in both modes: the disc is the nose's own circle, and the material
   is everything below its **upper** arc on the glass. With `G41`/`G42` the
   machine's point is the tool's centre, so that arc is one radius above it and
   the swept surface is the tangent the nose cuts. Without a shift the machine's
   point is the tool corner, and the same arc is still one radius above the
   corner - the surface the nose leaves. The glass is the measurement: with
   `T1 R3`, a `G40` row at X20 leaves its material at X14 (one nose diameter
   less), while the compensated `G41` run, whose machine path is X26, leaves it
   on X20. Moving the `G40` bound down by a radius put the clearing one radius
   too deep on the bench (2026-10-17). */
static void ncc_live_clear(int sx, int sy, int r)
{
    int dx;

    /* The brush is the nose's own **round disc**, of the row's radius, and the
       material it takes off is bounded by the disc's upper arc: no shift, the
       point is the surface and the arc is through it; with one, the point is the
       centre and the arc is one radius inside (bench, 2026-10-17: "replace this
       yellow 2px dot with tool radius round dot"). */
    for (dx = -r; dx <= r; dx++) {
        int span = r * r - dx * dx;
        int h = span > 0 ? (int)sqrtf((float)span) : 0;
        int top = sy - h;

        ncc_live_remove(sx + dx, top, sx + dx, g_ncc_live_h - 1);
    }
}

/* The glyph is the nose's own circle - the same circle the brush clears - and
   nothing else. The flanks, polygon and fill were removed; the take-back is
   just the circle's box, so the mask repaints a small band. */
static void ncc_live_tip_draw(int tip_x, int tip_y, int r, int px, int py,
                              int pw, int ph)
{
    int dy;

    for (dy = -r; dy <= r; dy++) {
        int span = r * r - dy * dy;
        int w = span > 0 ? (int)sqrtf((float)span) : 0;
        int y = tip_y + dy;
        int x0 = tip_x - w;
        int x1 = tip_x + w;

        if (y < py || y >= py + ph) {
            continue;
        }
        if (x0 < px) {
            x0 = px;
        }
        if (x1 > px + pw - 1) {
            x1 = px + pw - 1;
        }
        if (x1 >= x0) {
            ncc_fill(x0, y, x1 - x0 + 1, 1, ncc_col_tool());
        }
    }
    g_ncc_tip_x = tip_x;
    g_ncc_tip_y = tip_y;
    g_ncc_tip_r = r + 1;
}

/* Take the circle back with its own box and hand the rows it covered to the
   mask's band, which puts the stock back under it. */
static void ncc_live_tip_erase(int stock_top, int px, int py, int pw, int ph)
{
    int x0;
    int x1;
    int y0;
    int y1;

    if (!g_ncc_tip_drawn) {
        return;
    }
    x0 = g_ncc_tip_x - g_ncc_tip_r;
    x1 = g_ncc_tip_x + g_ncc_tip_r;
    y0 = g_ncc_tip_y - g_ncc_tip_r;
    y1 = g_ncc_tip_y + g_ncc_tip_r;
    if (x0 < px) x0 = px;
    if (x1 > px + pw - 1) x1 = px + pw - 1;
    if (y0 < py) y0 = py;
    if (y1 > py + ph - 1) y1 = py + ph - 1;
    if (x1 >= x0 && y1 >= y0) {
        ncc_fill(x0, y0, x1 - x0 + 1, y1 - y0 + 1, ncc_col_prev_bg());
    }
    ncc_live_mark_rows(ncc_mask_y(stock_top, y0),
                       ncc_mask_y(stock_top, y1));
}


/* The cut between two points: a turning tool takes a band off, so the removal is
   walked along the move at the drawing's own resolution - the samples are what
   keeps a fast move from leaving gaps - and each sample is the nose's own
   circle (`ncc_live_clear()`). */
static void ncc_live_sweep(const ncc_preview_info_t *p, int stock_w, int stock_h,
                           int r, float x0, float z0, float x1, float z1)
{
    int sx0;
    int sx1;
    int sy0;
    int sy1;
    int samples;
    int i;
    if (!p || !g_ncc_live_ready) {
        return;
    }
    /* The brush is the nose's circle at the machine's point, through the
       stock's own conversion. */
    sx0 = ncc_stock_col(p, stock_w, z0);
    sx1 = ncc_stock_col(p, stock_w, z1);
    sy0 = ncc_stock_row(p, stock_h, x0);
    sy1 = ncc_stock_row(p, stock_h, x1);
    samples = ncc_clampi((sx1 - sx0) < 0 ? sx0 - sx1 : sx1 - sx0, 1, 80);
    if ((sy1 - sy0) < 0 ? (sy0 - sy1) > samples : (sy1 - sy0) > samples) {
        samples = ncc_clampi((sy1 - sy0) < 0 ? sy0 - sy1 : sy1 - sy0, 1, 80);
    }
    for (i = 0; i <= samples; i++) {
        float t = (float)i / (float)samples;
        int sx = sx0 + (int)((float)(sx1 - sx0) * t);
        int sy = sy0 + (int)((float)(sy1 - sy0) * t);

        ncc_live_clear(sx, sy, r);
    }
}

static void ncc_live_update(const ncc_preview_info_t *p,
                            const ncc_preview_run_t *run,
                            const ncc_frame_t *frame, int stock_w, int stock_h)
{
    /* The machine's point through the one conversion: the work offset and the
       axis radius-to-diameter rule, in the frame's own place. */
    float diam_x = ncc_frame_machine_x(frame, run->x);
    float z = ncc_frame_machine_z(frame, run->z);
    /* The brush's radius, at the drawing's own scale: the row's `R` in the
       stock's pixels, the same measure `ncc_stock_row()` fits X to. */
    int r = 1;

    if (run->tool && run->tool->valid && p->stock_x > 0.001f) {
        r = (int)lroundf(run->tool->r * (float)(2 * stock_h) / p->stock_x);
    }
    if (r < 6) {
        r = 6;              /* a square tool still has to be visible: R0 -> 6px */
    }
    if (stock_h > 0 && r > stock_h / 2) {
        r = stock_h / 2;                /* a cut, not the whole part */
    }

    if (g_ncc_live_has_last) {
        ncc_live_sweep(p, stock_w, stock_h, r, g_ncc_live_last_x,
                       g_ncc_live_last_z, diam_x, z);
    } else {
        ncc_live_sweep(p, stock_w, stock_h, r, diam_x, z, diam_x, z);
    }
    g_ncc_live_last_x = diam_x;
    g_ncc_live_last_z = z;
    g_ncc_live_has_last = true;
}

static void ncc_live_draw_column(int stock_left, int stock_top, int col,
                                 int y0, int y1)
{
    const ncc_live_column_t *column = &g_ncc_live_columns[col];
    int top;
    int bottom;

    if (column->outer == 0xffffu) {
        return;
    }
    top = column->inner > y0 ? column->inner : y0;
    bottom = column->outer < y1 ? column->outer : y1;
    if (bottom < top) {
        return;
    }
    ncc_stock_depth_fill_level(stock_left + col, stock_top + top, 1,
                               bottom - top + 1, column->base, column->frac);
}

/* What is left of the stock, drawn from the column cache. Untouched stock is
   the darkest amber. A cut refreshes only the columns it changed; the glyph's
   overpaint refreshes only its row band. Dither is emitted as same-colour runs,
   so no frame scans the whole mask or writes the stock one pixel at a time. */
static void ncc_live_draw(int stock_left, int stock_top, bool full)
{
    bool paint_all = full || g_ncc_live_draw_all;
    int col;
    int y0;
    int y1;

    if (!g_ncc_live_mask || !g_ncc_live_ready) {
        return;
    }
    for (col = 0; col < g_ncc_live_w; col++) {
        if (g_ncc_live_col_dirty[col]) {
            ncc_live_refresh_column(col);
        }
    }
    y0 = g_ncc_live_dirty_y0;
    y1 = g_ncc_live_dirty_y1;
    if (!paint_all && y0 > y1) {
        bool dirty_column = false;

        for (col = 0; col < g_ncc_live_w; col++) {
            if (g_ncc_live_col_dirty[col]) {
                dirty_column = true;
                break;
            }
        }
        if (!dirty_column) {
            return;
        }
    }

    if (paint_all) {
        ncc_fill(stock_left, stock_top, g_ncc_live_w, g_ncc_live_h,
                 ncc_col_prev_bg());
        for (col = 0; col < g_ncc_live_w; col++) {
            ncc_live_draw_column(stock_left, stock_top, col, 0,
                                 g_ncc_live_h - 1);
        }
        memset(g_ncc_live_col_dirty, 0, (size_t)g_ncc_live_w);
    } else {
        if (y0 <= y1) {
            y0 = ncc_clampi(y0, 0, g_ncc_live_h - 1);
            y1 = ncc_clampi(y1, y0, g_ncc_live_h - 1);
            ncc_fill(stock_left, stock_top + y0, g_ncc_live_w, y1 - y0 + 1,
                     ncc_col_prev_bg());
            for (col = 0; col < g_ncc_live_w; col++) {
                ncc_live_draw_column(stock_left, stock_top, col, y0, y1);
            }
        }
        for (col = 0; col < g_ncc_live_w; col++) {
            if (!g_ncc_live_col_dirty[col]) {
                continue;
            }
            ncc_fill(stock_left + col, stock_top, 1, g_ncc_live_h,
                     ncc_col_prev_bg());
            ncc_live_draw_column(stock_left, stock_top, col, 0,
                                 g_ncc_live_h - 1);
            g_ncc_live_col_dirty[col] = 0;
        }
    }
    g_ncc_live_draw_all = false;
    g_ncc_live_dirty_y0 = g_ncc_live_h;
    g_ncc_live_dirty_y1 = -1;
}

void ncc_preview_draw(const ncc_document_t *doc, const ncc_preview_run_t *run,
                      int x, int y, int w, int h)
{
    ncc_preview_info_t preview;
    ncc_frame_t live_frame;
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
    bool paint = !run || run->paint;

    /* The pane is cleared when it is painted whole - and only then: on a frame
       that repaints what moved (the live stock's band) the rest of the pane is
       already right, and clearing it here would take the part off the glass. */
    if (paint && (!run || run->full)) {
        ncc_fill(x, y, w, h, ncc_col_prev_bg());
    }
    if (doc && doc->path[0] && !ncc_path_is_program(doc->path)) {
        if (paint) {
            ncc_preview_text_file(doc, x, y, w, h);
        }
        return;
    }
    ncc_preview_collect(doc, &preview);
    ncc_frame_machine(&live_frame);

    usable_w = (float)(w - 72);
    usable_h = (float)(h - NCC_PREVIEW_TOP_BAND - 28);
    if (usable_w < 40.0f) usable_w = 40.0f;
    if (usable_h < 40.0f) usable_h = 40.0f;
    z_scale = usable_w / preview.visible_z;
    x_scale = usable_h / (preview.stock_x * 0.5f);
    scale = z_scale < x_scale ? z_scale : x_scale;
    if (scale <= 0.0f) scale = 1.0f;
    stock_w = (int)(preview.visible_z * scale + 0.5f);
    stock_h = (int)((preview.stock_x * 0.5f) * scale + 0.5f);
    stock_w = ncc_clampi(stock_w, 24, (int)usable_w);
    stock_h = ncc_clampi(stock_h, 24, (int)usable_h);
    stock_left = x + 20;
    stock_top = y + NCC_PREVIEW_TOP_BAND;
    z0_x = stock_left + (int)(preview.stock_z * scale + 0.5f);
    z0_x = ncc_clampi(z0_x, stock_left, stock_left + stock_w);
    g_ncc_preview_stock_us = 0u;
    g_ncc_preview_geom_us = 0u;

    /* The stock: the plain block, or - while the machine is cutting, and on the
       run screen afterwards - what is left of it, from the live mask. */
    {
        uint32_t t0 = mcu_micros();
        bool live = run && run->busy;
        /* What the pane is wearing this frame: after a frame that painted the
           pane whole (a mode change, the first frame) the stock is painted from
           nothing; otherwise only what the cut changed, because the rest of the
           pixels are already right (the chuck is part of the still picture, and
           is under the stock in any case - nc drew it that way). */
        bool paint_all = run ? run->full : true;
        bool context_changed = ncc_live_context_changed(&preview, stock_w,
                                                       stock_h, &live_frame);
        bool keep = run && run->screen_run && !run->busy && g_ncc_live_ready &&
                    !context_changed;

        /* Last frame's nose circle, taken back before anything this frame
           draws; the rows it covered go to the mask's band. */
        if (paint && !paint_all) {
            ncc_live_tip_erase(stock_top, x, y, w, h);
        }
        g_ncc_tip_drawn = 0;

        /* A cut starts a part, and nothing else does: the mask is made again
           when the machine starts cutting, and a run that has parked keeps what
           it made (`nc`'s own rule - the finished part stays on the glass until
           the drawing is asked for something else). */
        if (live && (!g_ncc_live_was_cutting || context_changed)) {
            ncc_live_reset(&preview, stock_w, stock_h, &live_frame);
            paint_all = true;
        }
        g_ncc_live_was_cutting = live || keep;
        if ((live || keep) && g_ncc_live_ready) {
            if (live) {
                ncc_live_update(&preview, run, &live_frame, stock_w, stock_h);
            }
            /* nc's order: the jaws first, so the material the mask paints is
               what covers their upper half. */
            if (paint) {
                if (paint_all) {
                    ncc_chuck(&preview, x, w, y + h, stock_left, stock_top,
                              stock_w, stock_h);
                    ncc_chuck_relief(&preview, stock_left, stock_top, stock_h);
                }
                ncc_live_draw(stock_left, stock_top, paint_all);
            }
        } else {
            if (paint && paint_all) {
                ncc_chuck(&preview, x, w, y + h, stock_left, stock_top,
                          stock_w, stock_h);
                ncc_chuck_relief(&preview, stock_left, stock_top, stock_h);
                ncc_stock_fill(stock_left, stock_top, stock_w, stock_h);
                if (preview.stock_i > 0.0f) {
                    int id_h = ncc_map_x(&preview, stock_top, stock_h,
                                         preview.stock_i) -
                               stock_top;

                    if (id_h > 0 && id_h < stock_h) {
                        ncc_fill(stock_left, stock_top, stock_w, id_h,
                                 ncc_col_prev_bg());
                    }
                }
            }
            if (paint && live) {
                /* The mask is what shows the cut; with nowhere to put it, say so
                   rather than drawing a stock that never changes. */
                ncc_text_clip(stock_left + 4, stock_top + 16,
                              "Live stock needs PSRAM", 28, ncc_col_error(),
                              ncc_col_prev_bg(), LVDS_FONT_NORMAL);
            }
        }
        g_ncc_preview_stock_us = mcu_micros() - t0;
    }
    {
        uint32_t t0 = mcu_micros();
        /* The run's own rule first, then the editor's: while the machine is
           cutting the drawing is the stock (the cut is what the operator is
           watching), and a parked run shows the path - wearing the DIN layer
           the editor's `#` left selected.

           The DIN layer itself - the rulers, the centrelines and the stock's
           own dimension callouts - is drawn on the run screen too, cutting or
           parked, whenever `#` left it on (bench, 2026-09-28: *"enable din in
           run preview i think i see the problem"*): the cut is only readable
           against those numbers, and a frame the operator cannot measure is a
           frame they have to take on trust. */
        bool running = run && run->screen_run && run->busy;
        bool dims = ncc_preview_din();

    if (!paint) {
        /* Nothing of the pane is painted this frame (the TOOLS screen's rows
           own it): the mask has moved, and the band it marked dirty is left for
           the frame that really paints - clearing it here would lose it. */
        g_ncc_preview_geom_us = 0u;
        return;
    }
    ncc_stock_diameter_dimension(&preview, stock_left, stock_top,
                                 stock_w, stock_h);
    if (dims) {
        ncc_din_layer(&preview, stock_left, stock_top, stock_w, stock_h, z0_x);
    }
    /* The rows' own marks and their dimensions - the point of each row, the
       callouts that measure it, the `C`/`R` of the row - are drawn on the run
       screen too, cutting as well as parked: they are what the cut is read
       against, and the DIN layer alone only says where the *stock* is (bench,
       2026-09-28: *"and dimensions too ?"*). They are redrawn every frame, so a
       live frame's band repaint cannot leave a piece of a callout behind. */
    ncc_contour_points(doc, &preview, z0_x, stock_left, stock_left + stock_w,
                       stock_w, stock_top, stock_h);
    if (!running) {
        ncc_emitted_preview(doc, &preview, stock_left, stock_w, stock_top,
                            stock_h);
        /* The debug line: where the tool's centre goes with the nose radius
           compensated, drawn over the programmed profile (EDIT only - a run
           has the real path on the glass already). */
        ncc_offset_preview(doc, &preview, stock_left, stock_w, stock_top,
                           stock_h);
    }
    /* The nose circle on the point the machine is at. It is the same circle
       the brush clears with; there is no insert body. */
    if (run && run->busy && run->tool && run->tool->valid &&
        preview.stock_x > 0.001f) {
        int r = (int)lroundf(run->tool->r * (float)(2 * stock_h) /
                             preview.stock_x);
        int tx = ncc_map_z(&preview, stock_left, stock_w,
                           ncc_frame_machine_z(&live_frame, run->z));
        int ty = ncc_map_x(&preview, stock_top, stock_h,
                           ncc_frame_machine_x(&live_frame, run->x));

        if (r < 2) {
            r = 2;
        }
        ncc_live_tip_draw(tx, ty, r, x, y, w, h);
        g_ncc_tip_drawn = 1;
    }
        g_ncc_preview_geom_us = mcu_micros() - t0;
    }
}
