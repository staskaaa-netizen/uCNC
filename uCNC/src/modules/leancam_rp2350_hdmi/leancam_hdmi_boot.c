#include "../../cnc.h"
#include "leancam_display.h"
#include "leancam_hdmi_renderer.h"

#include "hardware/clocks.h"
#include "pico/stdlib.h"

#if LEANCAM_USE_PSRAM_FB || LEANCAM_USE_PSRAM_BACKBUFFER
#include "leancam_psram.h"
#endif

#ifdef LEANCAM_ENABLE_ARDUINO_SD_PROBE
void leancam_sd_arduino_probe_task(uint32_t now_ms);
#endif

static void lc_hdmi_debug_pause(const char *msg)
{
    proto_info("LC_HDMI:%s t=%lu sys=%lu", msg, mcu_millis(),
               (uint32_t)(clock_get_hz(clk_sys) / 1000U));
    sleep_ms(250);
}

static void lc_hdmi_boot_draw(bool display_ok)
{
    lc_color_t black = lc_display_rgb(0, 0, 0);
    lc_color_t panel = lc_display_rgb(20, 36, 42);
    lc_color_t line = lc_display_rgb(80, 140, 150);
    lc_color_t text = lc_display_rgb(230, 240, 230);
    lc_color_t accent = lc_display_rgb(245, 205, 80);
    lc_color_t ok = lc_display_rgb(90, 220, 120);
    lc_color_t bad = lc_display_rgb(230, 80, 70);

    lc_display_clear(black);
    lc_display_fill_rect(0, 0, LC_DISPLAY_WIDTH, 42, panel);
    lc_display_text(18, 10, "uCNC RP2350 LeanCam HDMI", text, panel, LC_FONT_NORMAL);
    lc_display_text(560, 10, "800x600 RGB222", accent, panel, LC_FONT_NORMAL);

    lc_display_rect(18, 62, 764, 196, line);
    lc_display_text(36, 82, "LeanCam display backend", accent, black, LC_FONT_LARGE);
    lc_display_text(36, 132, display_ok ? "HDMI: initialized" : "HDMI: init failed",
                    display_ok ? ok : bad, black, LC_FONT_NORMAL);
    lc_display_text(36, 162, "uCNC core: minimal RP2350 base", text, black, LC_FONT_NORMAL);
    lc_display_text(36, 192, "Machine backend: no motion test screen", text, black, LC_FONT_NORMAL);

    lc_display_rect(18, 286, 764, 216, line);
    lc_display_text(36, 306, "Next integration step", accent, black, LC_FONT_NORMAL);
    lc_display_text(36, 344, "Wire LeanCam snapshot/bridge drawing into these primitives.", text, black, LC_FONT_NORMAL);
    lc_display_text(36, 374, "Keep G33, G7/G8, encoders, I2C keyboard and shift registers out.", text, black, LC_FONT_NORMAL);

    lc_display_line(36, 456, 744, 456, line);
    lc_display_text(36, 476, "Fallback: build env RP2350-LEANCAM-MINIMAL", text, black, LC_FONT_NORMAL);
    lc_display_present();
}

static bool leancam_rp2350_hdmi_update(void *args)
{
    (void)args;
    leancam_hdmi_renderer_poll();
#ifdef LEANCAM_ENABLE_ARDUINO_SD_PROBE
    leancam_sd_arduino_probe_task(mcu_millis());
#endif
    return EVENT_CONTINUE;
}

CREATE_EVENT_LISTENER(cnc_dotasks, leancam_rp2350_hdmi_update);

DECL_MODULE(leancam_rp2350_hdmi)
{

    bool display_ok;
    lc_hdmi_debug_pause("module entered");
    lc_hdmi_debug_pause("before psram");

#if LEANCAM_USE_PSRAM_FB || LEANCAM_USE_PSRAM_BACKBUFFER
    bool psram_ok = lc_psram_init();
    proto_info("LC_HDMI:psram init=%d available=%d", psram_ok ? 1 : 0,
               lc_psram_available() ? 1 : 0);
    sleep_ms(250);
#else
    proto_info("LC_HDMI:psram disabled");
    sleep_ms(250);
#endif

    lc_hdmi_debug_pause("before display init");
    display_ok = lc_display_init();
    proto_info("LC_HDMI:display init returned ok=%d err=%d scanout=%p backbuf=%d",
               display_ok ? 1 : 0,
               lc_display_last_error(),
               lc_display_scanout_buffer(),
               lc_display_backbuffer_active() ? 1 : 0);
    sleep_ms(500);

    lc_hdmi_debug_pause("before draw");
    lc_hdmi_boot_draw(display_ok);
    if (display_ok) {
        leancam_hdmi_renderer_init();
        ADD_EVENT_LISTENER(cnc_dotasks, leancam_rp2350_hdmi_update);
    }
    lc_hdmi_debug_pause("after draw");
}
