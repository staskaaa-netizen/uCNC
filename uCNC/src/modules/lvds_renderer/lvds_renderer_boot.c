#include "../../cnc.h"
#include "lvds_hstx.h"
#include "lvds_renderer.h"
#include "lvds_renderer_state.h"

#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#ifndef LEANCAM_USE_PSRAM_LIVE_SIM
#define LEANCAM_USE_PSRAM_LIVE_SIM 0
#endif

#ifndef LVDS_RENDERER_BOOT_DELAY_MS
#define LVDS_RENDERER_BOOT_DELAY_MS 0
#endif

#ifndef LVDS_RENDERER_WATCHDOG_MS
#define LVDS_RENDERER_WATCHDOG_MS 500
#endif

#if LEANCAM_USE_PSRAM_FB || LEANCAM_USE_PSRAM_BACKBUFFER || LEANCAM_USE_PSRAM_LIVE_SIM
#include "lvds_psram.h"
#endif

static void lvds_debug_pause(const char *msg)
{
    proto_info("LVDS:%s t=%lu sys=%lu", msg, mcu_millis(),
               (uint32_t)(clock_get_hz(clk_sys) / 1000U));
}

static bool lvds_renderer_update(void *args)
{
    (void)args;
#if LVDS_RENDERER_WATCHDOG_MS > 0
    watchdog_update();
#endif
    lvds_renderer_state_poll();
    lvds_renderer_draw_poll();
    return EVENT_CONTINUE;
}

CREATE_EVENT_LISTENER(cnc_io_dotasks, lvds_renderer_update);

DECL_MODULE(lvds_renderer)
{

    bool display_ok;
    lvds_debug_pause("module entered");

#if LVDS_RENDERER_WATCHDOG_MS > 0
    if (watchdog_caused_reboot()) {
        proto_info("LVDS:watchdog reboot");
    }
#endif

#if LVDS_RENDERER_BOOT_DELAY_MS > 0
    proto_info("LVDS:boot delay %lu ms", (uint32_t)LVDS_RENDERER_BOOT_DELAY_MS);
#endif

    lvds_debug_pause("before psram");

#if LVDS_RENDERER_WATCHDOG_MS > 0
    proto_info("LVDS:watchdog armed %lu ms", (uint32_t)LVDS_RENDERER_WATCHDOG_MS);
    watchdog_enable((uint32_t)LVDS_RENDERER_WATCHDOG_MS, true);
    watchdog_update();
#endif

#if LEANCAM_USE_PSRAM_FB || LEANCAM_USE_PSRAM_BACKBUFFER || LEANCAM_USE_PSRAM_LIVE_SIM
    bool psram_ok = lvds_psram_init();
    watchdog_update();
    proto_info("LVDS:psram init=%d available=%d", psram_ok ? 1 : 0,
               lvds_psram_available() ? 1 : 0);
#else
    proto_info("LVDS:psram disabled");
#endif

    lvds_debug_pause("before display init");
    display_ok = lvds_hstx_init();
#if LVDS_RENDERER_WATCHDOG_MS > 0
    watchdog_update();
#endif
    proto_info("LVDS:display init returned ok=%d err=%d backbuf=%d",
               display_ok ? 1 : 0,
               lvds_hstx_last_error(),
               lvds_hstx_backbuffer_active() ? 1 : 0);

    if (display_ok) {
        lvds_renderer_state_init();
        lvds_renderer_draw_init();
        ADD_EVENT_LISTENER(cnc_io_dotasks, lvds_renderer_update);
    }
    lvds_debug_pause(display_ok ? "renderer armed" : "display unavailable");
}
