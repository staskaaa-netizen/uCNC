#include "leancam_sim_preview.h"
#include "ra8876_ll.h"
#include "../../cnc.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define SIM_AREA_X0          500
#define SIM_AREA_Y0          180
#define SIM_AREA_X1         1023
#define SIM_AREA_Y1          529
#define SIM_AREA_W          (SIM_AREA_X1 - SIM_AREA_X0 + 1)
#define SIM_AREA_H          (SIM_AREA_Y1 - SIM_AREA_Y0 + 1)

#define SIM_STOCK_TOP        200
#define SIM_RIGHT_MARGIN      70
#define SIM_BOTTOM_MARGIN     80
#define SIM_REDRAW_MS        250
#define SIM_AXIS_EXT          30

#define SIM_COL_BG       RA_BLACK
#define SIM_COL_STOCK    RA_GRAY
#define SIM_COL_STOCK_2  0x528A
#define SIM_COL_FRAME    RA_GRAY
#define SIM_COL_AXIS     RA_WHITE
#define SIM_COL_DIM      RA_CYAN
#define SIM_COL_HI       RA_YELLOW
#define SIM_COL_CHUCK    RA_DGRAY
#define SIM_COL_CHUCK_FILL 0xC618

#define SIM_LC_MODE_PROGRAM  2u
#define SIM_LC_MODE_DRAFT    3u

#define SIM_TILE_H           12
#define SIM_CHUCK_H          30

#define SIM_LABEL_Z1_DX     -48
#define SIM_LABEL_Z1_DY     -20
#define SIM_LABEL_D1_DX     -48
#define SIM_LABEL_D1_DY      -4
#define SIM_LABEL_Z2_DX     -48
#define SIM_LABEL_Z2_DY     -20
#define SIM_LABEL_D2_DX     -48
#define SIM_LABEL_D2_DY      -4

typedef struct
{
    float length;
    float od;
    float id;
    float clamp;
    float extra;
} sim_setup_t;

static int sim_clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float sim_absf(float v)
{
    return (v < 0.0f) ? -v : v;
}

static bool sim_get_field_text(const char *line, const char *name, char *out, unsigned out_len)
{
    const char *p;
    unsigned name_len;

    if (!line || !name || !out || out_len == 0)
        return false;

    out[0] = 0;
    name_len = (unsigned)strlen(name);
    p = line;
    if (p[0] == '>' && p[1] == ' ')
        p += 2;

    while ((p = strstr(p, name)) != NULL)
    {
        const char *open;
        const char *close;
        unsigned n;

        if ((p == line || p[-1] == '|' || (p >= line + 2 && p[-1] == ' ' && p[-2] == '>')) &&
            p[name_len] == '{')
        {
            open = p + name_len;
            close = strchr(open + 1, '}');
            if (!close)
                return false;

            n = (unsigned)(close - open - 1);
            if (n >= out_len)
                n = out_len - 1;

            memcpy(out, open + 1, n);
            out[n] = 0;
            return true;
        }

        p += name_len;
    }

    return false;
}

static bool sim_field_float(const char *line, const char *name, float *out)
{
    char buf[32];
    char *s;
    char *endp;

    if (!sim_get_field_text(line, name, buf, sizeof(buf)))
        return false;

    s = buf;
    while (*s == ' ') s++;
    if (*s == '(') s++;
    if (*s == '*' || *s == 0)
        return false;

    *out = (float)strtod(s, &endp);
    return endp != s;
}

static bool sim_field_float2(const char *line, const char *a, const char *b, float *out)
{
    return sim_field_float(line, a, out) || sim_field_float(line, b, out);
}

static bool sim_field_float3(const char *line, const char *a, const char *b, const char *c, float *out)
{
    return sim_field_float(line, a, out) || sim_field_float(line, b, out) || sim_field_float(line, c, out);
}

static void sim_read_setup(const ui_snapshot_frame_t *frame, sim_setup_t *setup)
{
    const char *line = frame ? frame->leancam_setup_line : NULL;

    setup->length = 100.0f;
    setup->od = 50.0f;
    setup->id = 0.0f;
    setup->clamp = 0.0f;
    setup->extra = 0.0f;

    if (!line || !line[0])
        return;

    (void)sim_field_float2(line, "L", "LENGTH", &setup->length);
    (void)sim_field_float2(line, "OD", "OUTER_DIAMETER", &setup->od);
    (void)sim_field_float2(line, "ID", "INNER_DIAMETER", &setup->id);
    (void)sim_field_float2(line, "CLAMP", "CLAMP_LENGTH", &setup->clamp);
    (void)sim_field_float2(line, "EXTRA", "EXTRA_LENGTH", &setup->extra);

    if (setup->length <= 0.0f) setup->length = 100.0f;
    if (setup->od <= 0.0f) setup->od = 50.0f;
    if (setup->id < 0.0f) setup->id = 0.0f;
    if (setup->clamp < 0.0f) setup->clamp = 0.0f;
    if (setup->extra < 0.0f) setup->extra = 0.0f;
}

static void sim_build_view(const sim_setup_t *setup, lcam_sim_view_t *view)
{
    float visible_len;
    float z_scale;
    float d_scale;
    int stock_h;
    int stock_w;
    int usable_w;
    int usable_h;

    visible_len = setup->length + setup->extra;
    if (visible_len <= 0.0f)
        visible_len = 100.0f;

    usable_w = (SIM_AREA_X1 - SIM_RIGHT_MARGIN) - SIM_AREA_X0;
    usable_h = (SIM_AREA_Y1 - SIM_BOTTOM_MARGIN) - SIM_STOCK_TOP;
    if (usable_w < 1) usable_w = 1;
    if (usable_h < 1) usable_h = 1;

    z_scale = (float)usable_w / visible_len;
    d_scale = (float)usable_h / setup->od;
    if (z_scale <= 0.0f) z_scale = 1.0f;
    if (d_scale <= 0.0f) d_scale = 1.0f;

    stock_w = (int)(visible_len * z_scale + 0.5f);
    stock_h = (int)(setup->od * d_scale + 0.5f);
    if (stock_w < 1) stock_w = 1;
    if (stock_h < 1) stock_h = 1;
    if (stock_w > usable_w) stock_w = usable_w;
    if (stock_h > usable_h) stock_h = usable_h;

    view->x0 = SIM_AREA_X0;
    view->y0 = SIM_AREA_Y0;
    view->x1 = SIM_AREA_X1;
    view->y1 = SIM_AREA_Y1;
    view->stock_right = SIM_AREA_X1 - SIM_RIGHT_MARGIN;
    view->stock_left = view->stock_right - stock_w;
    if (view->stock_left < SIM_AREA_X0)
        view->stock_left = SIM_AREA_X0;
    view->stock_top = SIM_STOCK_TOP;
    view->stock_bottom = SIM_STOCK_TOP + stock_h;
    if (view->stock_bottom > SIM_AREA_Y1 - SIM_BOTTOM_MARGIN)
        view->stock_bottom = SIM_AREA_Y1 - SIM_BOTTOM_MARGIN;
    view->z_zero_x = sim_clampi(view->stock_left + (int)(setup->length * z_scale + 0.5f),
                                view->stock_left,
                                view->stock_right);

    view->z_scale = z_scale;
    view->d_scale = d_scale;
    view->stock_len = visible_len;
    view->stock_od = setup->od;
}

int sim_z_to_px(const lcam_sim_view_t *view, float z)
{
    int x;

    if (!view)
        return 0;

    x = view->z_zero_x + (int)(z * view->z_scale + ((z >= 0.0f) ? 0.5f : -0.5f));
    return sim_clampi(x, view->stock_left, view->stock_right);
}

int sim_d_to_py(const lcam_sim_view_t *view, float d)
{
    int y;

    if (!view)
        return 0;

    y = view->stock_top + (int)(d * view->d_scale + 0.5f);
    return sim_clampi(y, view->stock_top, view->stock_bottom);
}

static void sim_textf(int x, int y, uint16_t fg, uint16_t bg, const char *fmt, ...)
{
    char buf[64];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ra_text((uint16_t)x, (uint16_t)y, fg, bg, buf);
}

static void sim_draw_stock(const lcam_sim_view_t *view, const sim_setup_t *setup)
{
    int z0_x;
    int y;

    ra_fill_rect((uint16_t)view->x0, (uint16_t)view->y0, (uint16_t)view->x1, (uint16_t)view->y1, SIM_COL_BG);

    for (y = view->stock_top; y <= view->stock_bottom; y += SIM_TILE_H)
    {
        int y2 = y + SIM_TILE_H - 1;
        if (y2 > view->stock_bottom)
            y2 = view->stock_bottom;

        ra_fill_rect((uint16_t)view->stock_left,
                     (uint16_t)y,
                     (uint16_t)view->stock_right,
                     (uint16_t)y2,
                     SIM_COL_STOCK);
    }

    z0_x = view->z_zero_x;

    ra_draw_line(view->stock_left - SIM_AXIS_EXT,
                 view->stock_top,
                 view->stock_right + SIM_AXIS_EXT,
                 view->stock_top,
                 SIM_COL_AXIS);
    ra_draw_line(z0_x,
                 view->stock_top - SIM_AXIS_EXT,
                 z0_x,
                 view->stock_bottom + SIM_AXIS_EXT,
                 SIM_COL_AXIS);

    sim_textf(z0_x - 18, view->stock_top - 18, SIM_COL_DIM, SIM_COL_BG, "Z0");
    sim_textf(view->stock_left + 6, view->stock_top - 18, SIM_COL_DIM, SIM_COL_BG, "Z-%.0f", (double)view->stock_len);
    sim_textf(view->stock_right + 6, view->stock_top - 6, SIM_COL_DIM, SIM_COL_BG, "X0");
    if (setup->extra > 0.0f)
        sim_textf(z0_x + 5, view->stock_top - 22, SIM_COL_DIM, SIM_COL_BG, "EX %.1f", (double)setup->extra);

    sim_textf(view->stock_left + 6, view->stock_bottom + SIM_CHUCK_H + 10, SIM_COL_DIM, SIM_COL_BG, "L %.1f", (double)setup->length);
    sim_textf(view->stock_left + 86, view->stock_bottom + SIM_CHUCK_H + 10, SIM_COL_DIM, SIM_COL_BG, "OD %.1f", (double)setup->od);
}

static void sim_draw_chuck(const lcam_sim_view_t *view, const sim_setup_t *setup)
{
    int chuck_w;
    int jaw_h;
    int x1;
    int y1;
    int y2;

    if (setup->clamp <= 0.0f)
        return;

    chuck_w = (int)(setup->clamp * view->z_scale + 0.5f);
    chuck_w = sim_clampi(chuck_w, 10, 70);
    jaw_h = SIM_CHUCK_H;
    x1 = view->stock_left;

    y1 = view->stock_bottom + 1;
    y2 = view->stock_bottom + jaw_h;

    ra_fill_rect((uint16_t)x1, (uint16_t)y1, (uint16_t)(x1 + chuck_w), (uint16_t)y2, SIM_COL_CHUCK_FILL);
    sim_textf(x1 + 3, y1 + 9, RA_BLACK, SIM_COL_CHUCK_FILL, "CL %.1f", (double)setup->clamp);
}

static void sim_draw_hatch_rect(int x1, int y1, int x2, int y2, int spacing, bool vertical)
{
    int p;

    if (spacing < 2) spacing = 2;
    if (spacing > 12) spacing = 12;

    ra_fill_rect((uint16_t)x1, (uint16_t)y1, (uint16_t)x2, (uint16_t)y2, SIM_COL_HI);
    if (vertical)
    {
        for (p = x1 + spacing; p < x2; p += spacing)
            ra_draw_line(p, y1, p, y2, RA_BLACK);
    }
    else
    {
        for (p = y1 + spacing; p < y2; p += spacing)
            ra_draw_line(x1, p, x2, p, RA_BLACK);
    }
}

static bool sim_is_cycle(const char *line, const char *name)
{
    size_t n;

    if (!line || !name)
        return false;
    if (line[0] == '>' && line[1] == ' ')
        line += 2;

    n = strlen(name);
    return strncmp(line, name, n) == 0 && line[n] == '|';
}

static bool sim_should_draw_for_mode(uint8_t mode)
{
    return mode == SIM_LC_MODE_PROGRAM || mode == SIM_LC_MODE_DRAFT;
}

static void sim_begin_offscreen(uint32_t *old_draw_base)
{
    if (old_draw_base)
        *old_draw_base = ra_get_draw_base();

    ra_set_draw_base(RA8876_BACKBUF_ADDR);
}

static void sim_present_offscreen(uint32_t old_draw_base)
{
    ra_set_draw_base(old_draw_base);
    ra_blit(RA8876_BACKBUF_ADDR,
            SIM_AREA_X0,
            SIM_AREA_Y0,
            RA8876_VISIBLE_ADDR,
            SIM_AREA_X0,
            SIM_AREA_Y0,
            SIM_AREA_W,
            SIM_AREA_H);
}

static void sim_draw_status_message(const ui_snapshot_frame_t *frame)
{
    const char *msg = (frame && frame->leancam_message[0]) ? frame->leancam_message : "LC: ready";

    ra_fill_rect(SIM_AREA_X0, SIM_AREA_Y0, SIM_AREA_X1, SIM_AREA_Y1, SIM_COL_BG);
    sim_textf(SIM_AREA_X0 + 20, SIM_AREA_Y0 + 34, SIM_COL_DIM, SIM_COL_BG, "G-code generator");
    sim_textf(SIM_AREA_X0 + 20, SIM_AREA_Y0 + 58, SIM_COL_AXIS, SIM_COL_BG, "%-48.48s", msg);
}

static bool sim_preview_sources_changed(const ui_snapshot_frame_t *frame,
                                        char *last_setup,
                                        char *last_line,
                                        char *last_tool)
{
    if (!frame || !last_setup || !last_line || !last_tool)
        return false;

    if (strcmp(last_setup, frame->leancam_setup_line) == 0 &&
        strcmp(last_line, frame->leancam_preview_line) == 0 &&
        strcmp(last_tool, frame->leancam_tool_line) == 0)
        return false;

    return true;
}

static void sim_store_preview_sources(const ui_snapshot_frame_t *frame,
                                      char *last_setup,
                                      char *last_line,
                                      char *last_tool)
{
    if (!frame || !last_setup || !last_line || !last_tool)
        return;

    ui_snapshot_strcpy(last_setup, frame->leancam_setup_line, UI_LC_LINE_LEN);
    ui_snapshot_strcpy(last_line, frame->leancam_preview_line, UI_LC_LINE_LEN);
    ui_snapshot_strcpy(last_tool, frame->leancam_tool_line, UI_LC_LINE_LEN);
}

static void sim_draw_corner_label(const lcam_sim_view_t *view, int x, int y, const char *name, float v, int dx, int dy)
{
    int tx = sim_clampi(x + dx, view->x0 + 2, view->x1 - 70);
    int ty = sim_clampi(y + dy, view->y0 + 2, view->y1 - 18);

    sim_textf(tx, ty, SIM_COL_DIM, SIM_COL_BG, "%s %.2f", name, (double)v);
}

static void sim_draw_dz_corner_labels(const lcam_sim_view_t *view,
                                      int start_x,
                                      int start_d_y,
                                      float d1,
                                      float z1,
                                      int end_x,
                                      int end_d_y,
                                      float d2,
                                      float z2)
{
    int start_z_y = start_d_y;
    int end_z_y = end_d_y;

    if (end_d_y < start_d_y)
    {
        start_z_y = end_d_y;
        end_z_y = start_d_y;
    }

    sim_draw_corner_label(view, start_x, start_z_y, "Z1", z1, SIM_LABEL_Z1_DX, SIM_LABEL_Z1_DY);
    sim_draw_corner_label(view, start_x, start_d_y, "D1", d1, SIM_LABEL_D1_DX, SIM_LABEL_D1_DY);
    sim_draw_corner_label(view, end_x, end_z_y, "Z2", z2, SIM_LABEL_Z2_DX, SIM_LABEL_Z2_DY);
    sim_draw_corner_label(view, end_x, end_d_y, "D2", d2, SIM_LABEL_D2_DX, SIM_LABEL_D2_DY);
}

static void sim_draw_od_preview(const lcam_sim_view_t *view, const char *line, const ui_snapshot_frame_t *frame)
{
    float d1;
    float d2;
    float z1;
    float z2;
    float doc;
    int x1;
    int x2;
    int y1;
    int y2;
    int label_x1;
    int label_x2;
    int label_y1;
    int label_y2;
    int tmp;
    int spacing = 0;

    if (!sim_field_float2(line, "D1", "DIAMETER_1", &d1)) return;
    if (!sim_field_float2(line, "D2", "DIAMETER_2", &d2)) return;
    if (!sim_field_float2(line, "Z1", "Z_1", &z1)) return;
    if (!sim_field_float2(line, "Z2", "Z_2", &z2)) return;

    label_x1 = sim_z_to_px(view, z1);
    label_x2 = sim_z_to_px(view, z2);
    label_y1 = sim_d_to_py(view, d1);
    label_y2 = sim_d_to_py(view, d2);
    x1 = label_x1;
    x2 = label_x2;
    y1 = label_y1;
    y2 = label_y2;
    if (x2 < x1) { tmp = x1; x1 = x2; x2 = tmp; }
    if (y2 < y1) { tmp = y1; y1 = y2; y2 = tmp; }
    if (x2 <= x1) x2 = x1 + 1;
    if (y2 <= y1) y2 = y1 + 1;

    if (sim_field_float3(line, "DOC", "R_DOC", "ROUGH_DOC", &doc) ||
        (frame && sim_field_float3(frame->leancam_tool_line, "R_DOC", "ROUGH_DOC", "ROUGH_DEPTH_OF_CUT", &doc)))
        spacing = (int)(sim_absf(doc) * view->d_scale + 0.5f);
    if (spacing <= 0) spacing = 4;

    sim_draw_hatch_rect(x1, y1, x2, y2, spacing, false);
    sim_draw_dz_corner_labels(view, label_x1, label_y1, d1, z1, label_x2, label_y2, d2, z2);
}

static void sim_draw_id_preview(const lcam_sim_view_t *view, const char *line, const ui_snapshot_frame_t *frame)
{
    float d1;
    float d2;
    float z1;
    float z2;
    float doc;
    int x1;
    int x2;
    int y1;
    int y2;
    int label_x1;
    int label_x2;
    int label_y1;
    int label_y2;
    int tmp;
    int spacing = 0;

    if (!sim_field_float2(line, "D1", "DIAMETER_1", &d1)) return;
    if (!sim_field_float2(line, "D2", "DIAMETER_2", &d2)) return;
    if (!sim_field_float2(line, "Z1", "Z_1", &z1)) return;
    if (!sim_field_float2(line, "Z2", "Z_2", &z2)) return;

    label_x1 = sim_z_to_px(view, z1);
    label_x2 = sim_z_to_px(view, z2);
    label_y1 = sim_d_to_py(view, d1);
    label_y2 = sim_d_to_py(view, d2);
    x1 = label_x1;
    x2 = label_x2;
    y1 = label_y1;
    y2 = label_y2;
    if (x2 < x1) { tmp = x1; x1 = x2; x2 = tmp; }
    if (y2 < y1) { tmp = y1; y1 = y2; y2 = tmp; }
    if (x2 <= x1) x2 = x1 + 1;
    if (y2 <= y1) y2 = y1 + 1;

    if (sim_field_float3(line, "DOC", "R_DOC", "ROUGH_DOC", &doc) ||
        (frame && sim_field_float3(frame->leancam_tool_line, "R_DOC", "ROUGH_DOC", "ROUGH_DEPTH_OF_CUT", &doc)))
        spacing = (int)(sim_absf(doc) * view->d_scale + 0.5f);
    if (spacing <= 0) spacing = 4;

    sim_draw_hatch_rect(x1, y1, x2, y2, spacing, false);
    sim_draw_dz_corner_labels(view, label_x1, label_y1, d1, z1, label_x2, label_y2, d2, z2);
}

static void sim_draw_face_preview(const lcam_sim_view_t *view, const char *line, const ui_snapshot_frame_t *frame)
{
    float d1 = 0.0f;
    float d;
    float z1 = 0.0f;
    float z;
    int x1;
    int x2;
    int label_x1;
    int label_x2;
    int label_y1;
    int label_y2;
    int y2;
    int tmp;
    float doc;
    int spacing = 0;

    if (!sim_field_float3(line, "D", "OD", "OUTER_DIAMETER", &d)) d = view->stock_od;
    (void)sim_field_float2(line, "Z1", "Z_1", &z1);
    if (!sim_field_float2(line, "Z", "Z_2", &z)) return;

    label_x1 = sim_z_to_px(view, z1);
    label_x2 = sim_z_to_px(view, z);
    label_y1 = sim_d_to_py(view, d1);
    label_y2 = sim_d_to_py(view, d);
    x1 = label_x1;
    x2 = label_x2;
    if (x2 < x1) { tmp = x1; x1 = x2; x2 = tmp; }
    y2 = label_y2;
    if (x2 <= x1) x2 = x1 + 1;

    if (sim_field_float3(line, "DOC", "R_DOC", "ROUGH_DOC", &doc) ||
        (frame && sim_field_float3(frame->leancam_tool_line, "R_DOC", "ROUGH_DOC", "ROUGH_DEPTH_OF_CUT", &doc)))
        spacing = (int)(sim_absf(doc) * view->d_scale + 0.5f);
    if (spacing <= 0) spacing = 4;

    sim_draw_hatch_rect(x1, view->stock_top, x2, y2, spacing, true);
    sim_draw_dz_corner_labels(view, label_x1, label_y1, d1, z1, label_x2, label_y2, d, z);
}

static void sim_draw_groove_preview(const lcam_sim_view_t *view, const char *line)
{
    float d1;
    float d2;
    float z1;
    float z2;
    int x1;
    int x2;
    int y1;
    int y2;
    int tmp;

    if (!sim_field_float(line, "D1", &d1)) return;
    if (!sim_field_float(line, "D2", &d2)) return;
    if (!sim_field_float(line, "Z1", &z1)) return;
    if (!sim_field_float(line, "Z2", &z2)) return;

    x1 = sim_z_to_px(view, z1);
    x2 = sim_z_to_px(view, z2);
    y1 = sim_d_to_py(view, d1);
    y2 = sim_d_to_py(view, d2);
    if (x2 < x1) { tmp = x1; x1 = x2; x2 = tmp; }
    if (y2 < y1) { tmp = y1; y1 = y2; y2 = tmp; }
    if (x2 <= x1) x2 = x1 + 1;
    if (y2 <= y1) y2 = y1 + 1;

    sim_draw_hatch_rect(x1, y1, x2, y2, 4, true);
    sim_draw_corner_label(view, x1, y1, "D1", d1, -54, -18);
    sim_draw_corner_label(view, x2, y2, "D2", d2, 6, 4);
}

void leancam_sim_preview_draw(const ui_snapshot_frame_t *frame)
{
    static uint32_t last_draw_ms = 0;
    static uint8_t last_mode = 0xff;
    static bool was_visible = false;
    static bool was_msg_visible = false;
    static char last_setup[UI_LC_LINE_LEN];
    static char last_line[UI_LC_LINE_LEN];
    static char last_tool[UI_LC_LINE_LEN];
    static char last_msg[UI_SNAPSHOT_POPUP_LEN];
    sim_setup_t setup;
    lcam_sim_view_t view;
    const char *line;
    uint32_t now;
    uint32_t old_draw_base = RA8876_VISIBLE_ADDR;
    bool mode_changed;
    bool source_changed;

    if (!frame || !frame->leancam_active)
        return;

    mode_changed = (last_mode != frame->leancam_mode);

    if (!sim_should_draw_for_mode(frame->leancam_mode))
    {
        if (frame->leancam_message[0])
        {
            if (was_visible || !was_msg_visible || mode_changed ||
                strcmp(last_msg, frame->leancam_message) != 0)
            {
                sim_begin_offscreen(&old_draw_base);
                sim_draw_status_message(frame);
                sim_present_offscreen(old_draw_base);
                ui_snapshot_strcpy(last_msg, frame->leancam_message, sizeof(last_msg));
            }

            was_msg_visible = true;
        }
        else
        {
            if (was_visible || was_msg_visible || mode_changed)
            {
                sim_begin_offscreen(&old_draw_base);
                ra_fill_rect(SIM_AREA_X0, SIM_AREA_Y0, SIM_AREA_X1, SIM_AREA_Y1, SIM_COL_BG);
                sim_present_offscreen(old_draw_base);
            }

            was_msg_visible = false;
            last_msg[0] = 0;
        }

        was_visible = false;
        last_mode = frame->leancam_mode;
        last_setup[0] = 0;
        last_line[0] = 0;
        last_tool[0] = 0;
        return;
    }

    source_changed = sim_preview_sources_changed(frame, last_setup, last_line, last_tool);
    if (!mode_changed && !source_changed)
        return;

    now = mcu_millis();
    if (!mode_changed &&
        last_draw_ms != 0 &&
        (uint32_t)(now - last_draw_ms) < SIM_REDRAW_MS)
        return;

    last_draw_ms = now;
    last_mode = frame->leancam_mode;
    was_visible = true;
    was_msg_visible = false;
    last_msg[0] = 0;
    sim_store_preview_sources(frame, last_setup, last_line, last_tool);

    sim_begin_offscreen(&old_draw_base);
    sim_read_setup(frame, &setup);
    sim_build_view(&setup, &view);
    sim_draw_stock(&view, &setup);
    sim_draw_chuck(&view, &setup);

    line = frame->leancam_preview_line;
    if (!line || !line[0] || sim_is_cycle(line, "SETUP"))
    {
        sim_present_offscreen(old_draw_base);
        return;
    }

    if (sim_is_cycle(line, "OD"))
        sim_draw_od_preview(&view, line, frame);
    else if (sim_is_cycle(line, "ID"))
        sim_draw_id_preview(&view, line, frame);
    else if (sim_is_cycle(line, "FACE"))
        sim_draw_face_preview(&view, line, frame);
    else if (sim_is_cycle(line, "GROOVE"))
        sim_draw_groove_preview(&view, line);

    sim_present_offscreen(old_draw_base);
}
