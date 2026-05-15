#include "../../cnc.h"
#include "leancam_display.h"
#include "leancam_hdmi_renderer.h"

#include "hardware/clocks.h"
#include "pico/stdlib.h"

#ifndef LEANCAM_USE_PSRAM_LIVE_SIM
#define LEANCAM_USE_PSRAM_LIVE_SIM 0
#endif

#if LEANCAM_USE_PSRAM_FB || LEANCAM_USE_PSRAM_BACKBUFFER || LEANCAM_USE_PSRAM_LIVE_SIM
#include "leancam_psram.h"
#endif

static void lc_hdmi_debug_pause(const char *msg)
{
    proto_info("LC_HDMI:%s t=%lu sys=%lu", msg, mcu_millis(),
               (uint32_t)(clock_get_hz(clk_sys) / 1000U));
    sleep_ms(25);
}

static bool leancam_rp2350_hdmi_update(void *args)
{
    (void)args;
    leancam_hdmi_renderer_poll();
    return EVENT_CONTINUE;
}

CREATE_EVENT_LISTENER(cnc_io_dotasks, leancam_rp2350_hdmi_update);

DECL_MODULE(leancam_rp2350_hdmi)
{

    bool display_ok;
    lc_hdmi_debug_pause("module entered");
    lc_hdmi_debug_pause("before psram");

#if LEANCAM_USE_PSRAM_FB || LEANCAM_USE_PSRAM_BACKBUFFER || LEANCAM_USE_PSRAM_LIVE_SIM
    bool psram_ok = lc_psram_init();
    proto_info("LC_HDMI:psram init=%d available=%d", psram_ok ? 1 : 0,
               lc_psram_available() ? 1 : 0);
    sleep_ms(25);
#else
    proto_info("LC_HDMI:psram disabled");
    sleep_ms(25);
#endif

    lc_hdmi_debug_pause("before display init");
    display_ok = lc_display_init();
    proto_info("LC_HDMI:display init returned ok=%d err=%d scanout=%p backbuf=%d",
               display_ok ? 1 : 0,
               lc_display_last_error(),
               lc_display_scanout_buffer(),
               lc_display_backbuffer_active() ? 1 : 0);
    sleep_ms(50);

    if (display_ok) {
        leancam_hdmi_renderer_init();
        ADD_EVENT_LISTENER(cnc_io_dotasks, leancam_rp2350_hdmi_update);
    }
    lc_hdmi_debug_pause(display_ok ? "renderer armed" : "display unavailable");
}
