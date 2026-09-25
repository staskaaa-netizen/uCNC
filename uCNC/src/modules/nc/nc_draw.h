#ifndef NC_DRAW_H
#define NC_DRAW_H

/* Shared panel drawing primitives: text, tool glyphs, the footer and modal
   grid. Preview stock/path geometry belongs to nc_preview.c. The numbers they
   draw with (nc_layout.h) and footer items (nc_menu.h) remain shared here.
   These are source files inside NC, not separate firmware modules.

   The 3x3 grid drawn by nc_draw_modal_items() is deliberately
   here: MANUAL's jog pad and EDIT's floating helper are two users of one
   visual, not two drawings. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nc_layout.h"
#include "nc_menu.h"
#include "nc_palette.h"
#include "nc_tools.h"

int nc_draw_clampi(int v, int lo, int hi);
int nc_draw_wrap_lines(const char *text, int cols, char lines[NC_FOOTER_LINES][24]);
void nc_draw_footer_status(const char *message, const char *footer_text);
void nc_draw_modal_items(int x, int y, const nc_footer_item_t *items, size_t count, uint16_t active_mask);
void nc_draw_text_clip(int x, int y, const char *text, int cols, lvds_color_t fg, lvds_color_t bg, int font);
void nc_draw_tool_cell(const char *line, char letter, int x, int y, int cols, lvds_color_t fg, lvds_color_t bg, bool active);
void nc_draw_tool_glyph(int tip_x, int tip_y, int size, const nc_tool_t *tool, lvds_color_t bg, bool selected);
void nc_draw_tool_glyph_centered(int x, int y, int box_size, int marker_size, const nc_tool_t *tool, lvds_color_t bg, bool selected);
void nc_draw_tool_param(const char *line, char letter, const char *label, int x, int y, int value_cols, bool active);
void nc_draw_fill_triangle(int x1, int y1, int x2, int y2, int x3, int y3, lvds_color_t color);

#endif
