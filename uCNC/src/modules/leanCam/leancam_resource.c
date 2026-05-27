#include "leancam_resource.h"
#include "../../cnc.h"

#if __has_include("../lvds_renderer/lvds_psram.h")
#include "../lvds_renderer/lvds_psram.h"
#define LC_RESOURCE_HAVE_PSRAM 1
#else
#define LC_RESOURCE_HAVE_PSRAM 0
#endif

#define LC_RESOURCE_PSRAM_LIVE_SIM_OFFSET            (512u * 1024u)
#define LC_RESOURCE_PSRAM_TOOL_CATALOG_OFFSET        (768u * 1024u)
#define LC_RESOURCE_PSRAM_PREVIEW_GCODE_OFFSET       (896u * 1024u)
#define LC_RESOURCE_PSRAM_PREVIEW_GCODE_STATE_OFFSET (912u * 1024u)

static bool g_lc_resource_file_busy = false;
static bool g_lc_resource_frame_busy = false;

bool lc_resource_file_begin(const char *tag)
{
    (void)tag;

    if (g_lc_resource_file_busy)
        return false;

    g_lc_resource_file_busy = true;
    cnc_set_file_io_critical(true);
    return true;
}

void lc_resource_file_end(void)
{
    cnc_set_file_io_critical(false);
    g_lc_resource_file_busy = false;
}

bool lc_resource_file_busy(void)
{
    return g_lc_resource_file_busy;
}

bool lc_resource_can_autosave(void)
{
    return !g_lc_resource_file_busy;
}

void *lc_resource_psram_region(lc_psram_region_t id, size_t size)
{
#if LC_RESOURCE_HAVE_PSRAM
    size_t offset;

    if (!lvds_psram_available())
        return NULL;

    switch (id)
    {
        case LC_PSRAM_REGION_LIVE_SIM:
            offset = LC_RESOURCE_PSRAM_LIVE_SIM_OFFSET;
            break;
        case LC_PSRAM_REGION_TOOL_CATALOG:
            offset = LC_RESOURCE_PSRAM_TOOL_CATALOG_OFFSET;
            break;
        case LC_PSRAM_REGION_PREVIEW_GCODE:
            offset = LC_RESOURCE_PSRAM_PREVIEW_GCODE_OFFSET;
            break;
        case LC_PSRAM_REGION_PREVIEW_GCODE_STATE:
            offset = LC_RESOURCE_PSRAM_PREVIEW_GCODE_STATE_OFFSET;
            break;
        default:
            return NULL;
    }

    (void)size;
    return lvds_psram_ptr(offset);
#else
    (void)id;
    (void)size;
    return NULL;
#endif
}

bool lc_resource_frame_begin(void)
{
    if (g_lc_resource_frame_busy)
        return false;

    g_lc_resource_frame_busy = true;
    return true;
}

void lc_resource_frame_end(void)
{
    g_lc_resource_frame_busy = false;
}
