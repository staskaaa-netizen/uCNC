#include "nc2_boot.h"

#include "nc2_presets.h"

#include "../file_system.h"
#include "../lvds_renderer/lvds_draw_api.h"
#include "../lvds_renderer/lvds_hstx.h"
#include "../lvds_renderer/lvds_palette.h"

#include <stdio.h>
#include <string.h>

/* The logo stands for a moment even when the write is instant - on the machine
   it is twenty small files, so a screen that came and went in one frame would be
   a flicker, not an answer to "what just happened?". Only the first start pays
   it: a card that already has entries never sees this screen. */
#define NC2_BOOT_MIN_MS 1200u

/* The entries the panel ships: the groups the pad opens with, and the words and
   templates under them. An address is one to three digits and the file's name;
   `rows == NULL` is a slot that holds only the things under it, which is what a
   group is. This table is the whole of the fallback - there is no second copy of
   these anywhere, and no compiled entry the panel falls back on at run time. */
typedef struct {
    const char *address;
    const char *name;
    const char *rows;
} nc2_default_t;

static const nc2_default_t g_nc2_defaults[] = {
    { "1", "OPS", 0 },
    { "2", "TOOL", 0 },
    { "3", "WORD", 0 },
    { "4", "G7X", 0 },
    { "5", "THREAD", 0 },
    { "6", "PECK", 0 },
    { "7", "TABLE", 0 },

    /* The table's own group, which is what the TOOLS screen's pad offers: a
       tool's *row* for the table (the `2 TOOL` group is the program's tool
       change and spindle - `M6`, `M3`, `M4`, `M5` - and pressing those into a
       tool table is what the bench called wrong: "on ttols - 3x3 is wrong
       here"). A press adds the shipped default row and the digits type the
       number over its `1`, because the row that lands keeps its first field
       picked. */
    { "71", "ADD T1", "T1 R0.8 O3 F120 Q60 D2.0 E0.5 S800 X0 Z0" },
    { "72", "ADD T2", "T2 R0.4 O3 F100 Q50 D0.5 E0.2 S1200 X0 Z0" },
    { "73", "ADD T3", "T3 R0.2 O3 F80 Q40 D0.3 E0.1 S1500 X0 Z0" },

    { "11", "INS", "" },                    /* one blank line */
    { "16", "SETUP", "G970 X0 U0 Z0 W0\nG971 X0 Z0 I0 E0\nG972 C0\nG973 P0" },
    { "23", "M6", "M6" },
    { "24", "M3", "M3 S1000" },
    { "25", "STOP", "M5" },
    { "26", "M4", "M4 S1000" },
    { "32", "CHMF", " C0" },                /* continues the row above */
    { "33", "RND", " R0" },
    { "34", "U INC", " U" },
    { "35", "W INC", " W" },
    /* The words the pad had before the port put its entries in files and only
       these four were carried across (`5fc2225e` dropped the G, and the insert
       table that named the rest went with `2c53109d`): the line number, the
       code, and the P/Q the cycles name their profile rows with. */
    { "36", "LINE NO", " N" },
    { "37", "G CODE", " G" },
    { "38", "PROFILE START", " P" },
    { "39", "PROFILE END", " Q" },
    { "41", "OD ROUGH", "G71 U0 R0 X0 Z0 F0 P0 Q0" },
    { "42", "ID BORE", "G72 W0 R0 X0 Z0 F0 P0 Q0" },
    { "43", "FACE", "G72 W0 R0 X0 Z0 F0 P0 Q0" },
    { "46", "G80", "G80" },
    { "48", "FINISH", "G70 P0 Q0" },
    { "51", "THREAD OD", "G76 X0 Z0 P0 Q0 F0 I0 L0 R0" },
    { "52", "THREAD ID", "G76 X0 Z0 P0 Q0 F0 I-0.2 L0 R0" },
    { "53", "TAP", "G33 X0 Z0 K0 F0" },
    { "61", "DRILL", "G1 X0 Z0 F0" },
    { "62", "PECK", "G1 X0 Z0 F0" },
    { "63", "DWELL", "G4 P0" }
};

static bool g_nc2_boot_active;
static unsigned g_nc2_boot_left_ms;
static int g_nc2_boot_written;
static int g_nc2_boot_total;

size_t nc2_boot_count(void)
{
    return sizeof(g_nc2_defaults) / sizeof(g_nc2_defaults[0]);
}

bool nc2_boot_entry(size_t index, const char **address, const char **name,
                    const char **rows)
{
    if (index >= nc2_boot_count()) {
        return false;
    }
    if (address) {
        *address = g_nc2_defaults[index].address;
    }
    if (name) {
        *name = g_nc2_defaults[index].name;
    }
    if (rows) {
        *rows = g_nc2_defaults[index].rows;
    }
    return true;
}

bool nc2_boot_seed(void)
{
    size_t i;

    g_nc2_boot_active = false;
    g_nc2_boot_written = 0;
    g_nc2_boot_total = 0;
    (void)fs_mkdir(NC2_PRESET_DIR);
    g_nc2_boot_total = (int)(sizeof(g_nc2_defaults) /
                             sizeof(g_nc2_defaults[0]));
    for (i = 0u; i < sizeof(g_nc2_defaults) / sizeof(g_nc2_defaults[0]); i++) {
        const nc2_default_t *d = &g_nc2_defaults[i];

        /* Only what the card does not have: an address the operator has - his
           own words among them - is left exactly as it is. A card that already
           carries the shipped set is not written at all, which is what makes
           this the one start that has to do it (bench: a file the release added
           never reached a card that already had a folder). */
        if (nc2_preset_exists(d->address)) {
            continue;
        }
        if (nc2_preset_write(d->address, d->name, d->rows) == 1) {
            g_nc2_boot_written++;
        }
    }
    if (g_nc2_boot_written == 0) {
        return false;               /* nothing could be written: say nothing */
    }
    g_nc2_boot_left_ms = NC2_BOOT_MIN_MS;
    g_nc2_boot_active = true;
    return true;
}

bool nc2_boot_active(void)
{
    return g_nc2_boot_active;
}

void nc2_boot_tick(unsigned ms)
{
    if (!g_nc2_boot_active) {
        return;
    }
    if (ms >= g_nc2_boot_left_ms) {
        g_nc2_boot_left_ms = 0u;
        g_nc2_boot_active = false;
        return;
    }
    g_nc2_boot_left_ms -= ms;
}

void nc2_boot_draw(void)
{
    lvds_color_t bg = lvds_palette_color(black);
    lvds_color_t ink = lvds_palette_color(white_warm);
    lvds_color_t mark = lvds_palette_color(green_bright);
    char line[48];
    int cx = LVDS_VIEW_WIDTH / 2;
    const int y0 = LVDS_VIEW_HEIGHT / 2 - 60;

    lvds_draw_fill_rect(0, 0, LVDS_VIEW_WIDTH, LVDS_VIEW_HEIGHT, bg);

    /* The mark, then what happened. The name is drawn twice, a size apart, so
       the screen reads as a logo and not as a message box. */
    lvds_draw_fill_rect(cx - 60, y0, 120, 3, mark);
    snprintf(line, sizeof(line), "uCNC");
    lvds_draw_text(cx - lvds_draw_text_width(line, LVDS_FONT_NORMAL) / 2, y0 + 24,
                   line, ink, bg, LVDS_FONT_NORMAL);
    snprintf(line, sizeof(line), "programming station");
    lvds_draw_text(cx - lvds_draw_text_width(line, LVDS_FONT_SMALL) / 2, y0 + 48,
                   line, ink, bg, LVDS_FONT_SMALL);

    if (g_nc2_boot_written > 0) {
        snprintf(line, sizeof(line), "entries written: %d",
                 g_nc2_boot_written);
        lvds_draw_text(cx - lvds_draw_text_width(line, LVDS_FONT_SMALL) / 2,
                       y0 + 84, line, mark, bg, LVDS_FONT_SMALL);
        snprintf(line, sizeof(line), "they are files in /D/presets");
        lvds_draw_text(cx - lvds_draw_text_width(line, LVDS_FONT_SMALL) / 2,
                       y0 + 100, line, ink, bg, LVDS_FONT_SMALL);
        snprintf(line, sizeof(line), "edit them, or delete one");
        lvds_draw_text(cx - lvds_draw_text_width(line, LVDS_FONT_SMALL) / 2,
                       y0 + 116, line, ink, bg, LVDS_FONT_SMALL);
    }
}
