#ifndef NC_DRAW_H
#define NC_DRAW_H

/* The panel's drawing vocabulary, moved out of nc_visual.c when that
   file passed the size trigger: the screens and the preview both draw
   with these and none of them owns state - data in, pixels out. The
   numbers they draw with (nc_layout.h) and the footer items they label
   (nc_menu.h) come with them. Sources inside the NC module, never a
   module of its own.

   The 3x3 grid drawn by nc_visual_draw_modal_items() is deliberately
   here: MANUAL's jog pad, EDIT's floating helper and the planned path
   builder are three users of one visual, not three drawings. */

#include <stdbool.h>
#include <stdint.h>

#include "nc_layout.h"
#include "nc_menu.h"
#include "nc_palette.h"
#include "nc_preview.h"
#include "nc_tools.h"

typedef enum {
    NC_PREVIEW_SEG_FEED = 0,
    NC_PREVIEW_SEG_ROUGH,
    NC_PREVIEW_SEG_FINISH
} nc_preview_segment_t;
typedef struct {
    float x;
    float z;
} nc_preview_v2_t;

bool nc_visual_draw_center_arc(const nc_preview_info_t *preview, int z0_x, int stock_w, int stock_top, int stock_h, float start_x, float start_z, float end_x, float end_z, float i_off, float k_off, bool cw, lvds_color_t color, int width);
bool nc_visual_draw_emitted_motion_line(const nc_preview_info_t *preview, int z0_x, int stock_w, int stock_top, int stock_h, const char *line, nc_preview_segment_t segment, bool selected, float *last_x, float *last_z, bool *have_last);
bool nc_visual_draw_explicit_arc(const nc_preview_info_t *preview, int z0_x, int stock_w, int stock_top, int stock_h, float start_x, float start_z, float end_x, float end_z, float r, bool cw, lvds_color_t color, int width);
bool nc_visual_r_arc_center(float start_z, float start_x, float end_z, float end_x, float r, bool cw, nc_preview_v2_t *center);
bool nc_visual_tool_keypad_point(int digit, int ox, int oy, int step, int *x, int *y);
float nc_visual_absf(float v);
float nc_visual_directed_arc_sweep(float a0, float a1, bool cw);
int nc_visual_clampi(int v, int lo, int hi);
int nc_visual_preview_x(const nc_preview_info_t *p, int stock_top, int stock_h, float x);
int nc_visual_preview_z(const nc_preview_info_t *p, int z0_x, int stock_w, float z);
int nc_visual_tool_orient_digits(int orient, int *digits, int max_digits);
int nc_visual_tool_polygon_points(int tip_x, int tip_y, int orient, int size, int *px, int *py);
int nc_visual_tool_tip_digit(int orient);
int nc_visual_wrap_lines(const char *text, int cols, char lines[NC_FOOTER_LINES][24]);
void nc_visual_draw_arrowhead(int x, int y, int dir_x, int dir_y, lvds_color_t color);
void nc_visual_draw_centerline(int x0, int y0, int x1, int y1);
void nc_visual_draw_chuck(const nc_preview_info_t *preview, int stock_left, int stock_top, int stock_w, int stock_h);
void nc_visual_draw_chuck_hatching(int x, int y, int w, int h, lvds_color_t color);
void nc_visual_draw_chuck_relief(const nc_preview_info_t *preview, int stock_left, int stock_top, int stock_h);
void nc_visual_draw_contour_point_marker(int x, int y, bool filled);
void nc_visual_draw_dashdot_line(int x0, int y0, int x1, int y1, lvds_color_t color);
void nc_visual_draw_dashed_segment(const nc_preview_info_t *preview, int z0_x, int stock_w, int stock_top, int stock_h, float z0, float x0, float z1, float x1, lvds_color_t color);
void nc_visual_draw_diameter_dimension(int x, int y0, int y1, const char *label);
void nc_visual_draw_footer_status(const char *message, const char *footer_text);
void nc_visual_draw_modal_items(int x, int y, const nc_footer_item_t *items, size_t count, uint16_t active_mask);
void nc_visual_draw_origin_marker(int x, int y);
void nc_visual_draw_preview_tool_panel(int x, int y, int w, int h, const nc_tool_t *tool);
void nc_visual_draw_text_clip(int x, int y, const char *text, int cols, lvds_color_t fg, lvds_color_t bg, int font);
void nc_visual_draw_tool_cell(const char *line, char letter, int x, int y, int cols, lvds_color_t fg, lvds_color_t bg, bool active);
void nc_visual_draw_tool_glyph(int tip_x, int tip_y, int size, const nc_tool_t *tool, lvds_color_t bg, bool selected);
void nc_visual_draw_tool_glyph_centered(int x, int y, int box_size, int marker_size, const nc_tool_t *tool, lvds_color_t bg, bool selected);
void nc_visual_draw_tool_param(const char *line, char letter, const char *label, int x, int y, int value_cols, bool active);
void nc_visual_draw_tool_polygon(int tip_x, int tip_y, int orient, int size, int thick, lvds_color_t fill, lvds_color_t edge, lvds_color_t mount);
void nc_visual_draw_x_point_dimension(int dim_x, int start_y, int point_y, int zero_y, int point_x, const char *label);
void nc_visual_draw_z_point_dimension(int start_x, int point_x, int zero_x, int center_y, int point_y, const char *label);
void nc_visual_fill_triangle(int x1, int y1, int x2, int y2, int x3, int y3, lvds_color_t color);
void nc_visual_tool_edges(int orient, bool *left, bool *top, bool *right, bool *bottom);
void nc_visual_tool_marker_line(int x1, int y1, int x2, int y2, lvds_color_t color, int thick);

#endif
