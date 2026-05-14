#include <Arduino.h>
#include <SPI.h>
#include <SDFS.h>

#ifndef LEANCAM_SD_PROBE_DELAY_MS
#define LEANCAM_SD_PROBE_DELAY_MS 8000UL
#endif

#define LC_SD_SCK 30
#define LC_SD_MOSI 31
#define LC_SD_MISO 40
#define LC_SD_CS 43

static bool g_sd_probe_started;
static bool g_sd_probe_done;
static uint32_t g_sd_probe_start_ms;

extern "C" void leancam_sd_arduino_probe_task(uint32_t now_ms)
{
    if (g_sd_probe_done) {
        return;
    }

    if (!g_sd_probe_started) {
        g_sd_probe_started = true;
        g_sd_probe_start_ms = now_ms;
        Serial.printf("[LC_SD:SDFS probe armed delay=%lu]\r\n", (uint32_t)LEANCAM_SD_PROBE_DELAY_MS);
        return;
    }

    if ((uint32_t)(now_ms - g_sd_probe_start_ms) < LEANCAM_SD_PROBE_DELAY_MS) {
        return;
    }

    Serial.printf("[LC_SD:SDFS begin on SPI1 sck=%d mosi=%d miso=%d cs=%d]\r\n",
                  LC_SD_SCK, LC_SD_MOSI, LC_SD_MISO, LC_SD_CS);

    SPI1.end();
    SPI1.setSCK(LC_SD_SCK);
    SPI1.setTX(LC_SD_MOSI);
    SPI1.setRX(LC_SD_MISO);
    SPI1.setCS(LC_SD_CS);
    SPI1.begin();

    SDFS.setConfig(SDFSConfig(LC_SD_CS, SD_SCK_MHZ(1), SPI1));
    bool ok = SDFS.begin();
    Serial.printf("[LC_SD:SDFS begin result=%d]\r\n", ok ? 1 : 0);

    if (ok) {
        File root = SDFS.open("/", "r");
        if (root) {
            Serial.printf("[LC_SD:root opened]\r\n");
            File entry = root.openNextFile();
            if (entry) {
                Serial.printf("[LC_SD:first=%s size=%lu]\r\n", entry.name(), (uint32_t)entry.size());
                entry.close();
            } else {
                Serial.printf("[LC_SD:root empty]\r\n");
            }
            root.close();
        } else {
            Serial.printf("[LC_SD:root open failed]\r\n");
        }
    }

    g_sd_probe_done = true;
}
