#include "lvds_palette.h"

#include <stddef.h>

typedef struct {
    const char *name;
    const char *hex;
} lvds_palette_color_def_t;

typedef struct {
    const char *name;
    lvds_palette_color_id_t color;
} lvds_palette_element_def_t;

#define LVDS_RENDERER_MAX_PALETTE_COLORS 64

#if LC_COLOR_COUNT > LVDS_RENDERER_MAX_PALETTE_COLORS
#error "LeanCam LVDS color table exceeds the 64-color HSTX paletted output limit"
#endif

/* Available colors. Edit RGB values here. */
static const lvds_palette_color_def_t g_color_def[LC_COLOR_COUNT] = {
    [gray_192] = {"gray_192", "#C0C0C0"},
    [gray_128] = {"gray_128", "#808080"},
    [gray_160] = {"gray_160", "#A0A0A0"},
    [gray_96] = {"gray_96", "#606060"},
    [black] = {"black", "#242506"},
    [white_warm] = {"white_warm", "#ffffff"},
    [yellow] = {"yellow", "#fff701"},
    [yellow_light] = {"yellow_light", "#FFF582"},
    [red] = {"red", "#E60000"},
    [green] = {"green", "#24c863"},
    [white] = {"white", "#DDDDDD"},
    [yellow_pale] = {"yellow_pale", "#fff701"},
    [brown] = {"brown", "#3A3216"},
    [red_bright] = {"red_bright", "#ff0000"},
    [green_bright] = {"green_bright", "#3eff24"},
    
};

/* Element-to-color map. Reassign UI objects to available colors here. */
static const lvds_palette_element_def_t g_element_def[LC_ELEM_COUNT] = {
    [LC_ELEM_BACKGROUND] = {"main.background", gray_192},
    [LC_ELEM_HEADER] = {"main.header", gray_128},
    [LC_ELEM_PANEL] = {"main.panel", green},
    [LC_ELEM_BORDER] = {"main.border", gray_160},
    [LC_ELEM_TEXT] = {"main.text", black},
    [LC_ELEM_SECONDARY_TEXT] = {"main.secondary_text", black},
    [LC_ELEM_VALUE_TEXT] = {"main.value_text", yellow},
    [LC_ELEM_SELECTED_TEXT] = {"main.selected_text", white_warm},
    [LC_ELEM_ERROR] = {"main.error", red},
    [LC_ELEM_OK] = {"main.ok", green},
    [LC_ELEM_STOCK] = {"main.stock", white},
    [LC_ELEM_SELECTED_ROW] = {"main.selected_row", gray_160},
    [LC_ELEM_FOOTER_BG] = {"footer.background", gray_128},
    [LC_ELEM_FOOTER_TEXT] = {"footer.text", black},
    [LC_ELEM_FOOTER_VALUE] = {"footer.value", yellow},
    [LC_ELEM_CUT] = {"main.cut", yellow_pale},
    [LC_ELEM_HATCH] = {"main.hatch", brown},
    [LC_ELEM_TOOL] = {"main.tool", red_bright},

    [LC_ELEM_PREVIEW_BG] = {"preview.background", gray_192},
    [LC_ELEM_PREVIEW_FRAME] = {"preview.frame", black},
    [LC_ELEM_PREVIEW_TEXT] = {"preview.text", black},
    [LC_ELEM_PREVIEW_DIM_TEXT] = {"preview.dim_text", black},
    [LC_ELEM_PREVIEW_VALUE_TEXT] = {"preview.value_text", yellow},
    [LC_ELEM_PREVIEW_ACTIVE_TEXT] = {"preview.active_text", gray_128},
    [LC_ELEM_PREVIEW_ACTIVE_BG] = {"preview.active_bg", white_warm},
    [LC_ELEM_PREVIEW_STOCK] = {"preview.stock", white},
    [LC_ELEM_PREVIEW_CHUCK] = {"preview.chuck", gray_96},
    [LC_ELEM_PREVIEW_CHUCK_TEXT] = {"preview.chuck_text", white_warm},
    [LC_ELEM_PREVIEW_CUT] = {"preview.cut", yellow_pale},
    [LC_ELEM_PREVIEW_HATCH] = {"preview.hatch", brown},
    [LC_ELEM_PREVIEW_PROFILE] = {"preview.profile", red_bright},
    [LC_ELEM_PREVIEW_TOOL] = {"preview.tool", green_bright}, //tool lines
    [LC_ELEM_PREVIEW_TOOL_MARK] = {"preview.tool_mark", red},
    [LC_ELEM_PREVIEW_TOOL_OUTLINE] = {"preview.tool_outline", yellow_light},

    [LC_ELEM_LIVE_BG] = {"live.background", gray_192},
    [LC_ELEM_LIVE_FRAME] = {"live.frame", black},
    [LC_ELEM_LIVE_TEXT] = {"live.text", black},
    [LC_ELEM_LIVE_VALUE_TEXT] = {"live.value_text", yellow},
    [LC_ELEM_LIVE_DEBUG_TEXT] = {"live.debug_text", black},
    [LC_ELEM_LIVE_STOCK] = {"live.stock", white},
    [LC_ELEM_LIVE_AXIS] = {"live.axis", white_warm},
    [LC_ELEM_LIVE_TOOL] = {"live.tool", green_bright}, //tool lines
    [LC_ELEM_LIVE_TOOL_MARK] = {"live.tool_mark", red},
    [LC_ELEM_LIVE_TOOL_OUTLINE] = {"live.tool_outline", yellow_light},
    [LC_ELEM_LIVE_CHUCK] = {"live.chuck", gray_96},
    [LC_ELEM_LIVE_CHUCK_OUTLINE] = {"live.chuck_outline", yellow_light},
    [LC_ELEM_LIVE_COLLISION] = {"live.collision", red},
};

static lvds_color_t g_color[LC_COLOR_COUNT];
static bool g_palette_ready;

static int lvds_palette_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint8_t lvds_palette_hex_byte(const char *s, uint8_t fallback)
{
    int hi;
    int lo;

    if (!s) {
        return fallback;
    }
    hi = lvds_palette_hex_nibble(s[0]);
    lo = lvds_palette_hex_nibble(s[1]);
    if (hi < 0 || lo < 0) {
        return fallback;
    }
    return (uint8_t)((hi << 4) | lo);
}

static lvds_color_t lvds_palette_parse_hex(const char *hex)
{
    uint8_t r;
    uint8_t g;
    uint8_t b;

    if (!hex) {
        return lvds_hstx_rgb(255, 255, 255);
    }
    if (hex[0] == '#') {
        hex++;
    }
    r = lvds_palette_hex_byte(hex + 0, 255);
    g = lvds_palette_hex_byte(hex + 2, 255);
    b = lvds_palette_hex_byte(hex + 4, 255);
    return lvds_hstx_rgb(r, g, b);
}

void lvds_palette_reset(void)
{
    g_palette_ready = false;
}

void lvds_palette_init(void)
{
    if (g_palette_ready) {
        return;
    }

    for (int i = 0; i < LC_COLOR_COUNT; i++) {
        g_color[i] = lvds_palette_parse_hex(g_color_def[i].hex);
    }
    g_palette_ready = true;
}

lvds_color_t lvds_palette_color(lvds_palette_color_id_t id)
{
    if (!g_palette_ready) {
        lvds_palette_init();
    }
    if (id < 0 || id >= LC_COLOR_COUNT) {
        return g_color[white_warm];
    }
    return g_color[id];
}

lvds_color_t lvds_palette_element(lvds_palette_element_id_t id)
{
    if (id < 0 || id >= LC_ELEM_COUNT) {
        return lvds_palette_color(white_warm);
    }
    return lvds_palette_color(g_element_def[id].color);
}

const char *lvds_palette_color_name(lvds_palette_color_id_t id)
{
    if (id < 0 || id >= LC_COLOR_COUNT) {
        return "";
    }
    return g_color_def[id].name;
}

const char *lvds_palette_element_name(lvds_palette_element_id_t id)
{
    if (id < 0 || id >= LC_ELEM_COUNT) {
        return "";
    }
    return g_element_def[id].name;
}
