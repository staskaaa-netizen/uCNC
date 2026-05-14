#include "../../cnc.h"

#ifndef LEANCAM_SD_INIT_DELAY_MS
#define LEANCAM_SD_INIT_DELAY_MS 5000UL
#endif

static bool g_sd_delay_started;
static bool g_sd_delay_done;
static uint32_t g_sd_delay_start_ms;

static bool leancam_rp2350_sd_delay_update(void *args)
{
    (void)args;

    if (g_sd_delay_done) {
        return EVENT_CONTINUE;
    }

    if (!g_sd_delay_started) {
        g_sd_delay_started = true;
        g_sd_delay_start_ms = mcu_millis();
        proto_info("LC_SD:delayed init armed delay=%lu", (uint32_t)LEANCAM_SD_INIT_DELAY_MS);
        return EVENT_CONTINUE;
    }

    if ((uint32_t)(mcu_millis() - g_sd_delay_start_ms) < LEANCAM_SD_INIT_DELAY_MS) {
        return EVENT_CONTINUE;
    }

    proto_info("LC_SD:loading sd_card_v2");
    LOAD_MODULE(sd_card_v2);
    proto_info("LC_SD:sd_card_v2 loaded, use $sdmnt to mount");
    g_sd_delay_done = true;
    return EVENT_CONTINUE;
}

CREATE_EVENT_LISTENER(cnc_dotasks, leancam_rp2350_sd_delay_update);

DECL_MODULE(leancam_rp2350_sd_delay)
{
    ADD_EVENT_LISTENER(cnc_dotasks, leancam_rp2350_sd_delay_update);
}
