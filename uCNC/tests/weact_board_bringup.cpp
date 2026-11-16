#include <Arduino.h>
#include <stdint.h>

#include "hardware/clocks.h"
#include "pico/stdlib.h"

namespace {

void report(const char *stage)
{
    Serial.print("BRINGUP ");
    Serial.print(stage);
    Serial.print(" sys_hz=");
    Serial.println(static_cast<unsigned long>(clock_get_hz(clk_sys)));
    Serial.flush();
}

} // namespace

void setup()
{
    Serial.begin(115200);
    const uint32_t start = millis();
    while (!Serial && (millis() - start) < 3000u) {
        delay(10);
    }

    report("start");

    const uint32_t psram_start = millis();
    const size_t detected = rp2040.getPSRAMSize();
    report("psram");
    Serial.print("detected=");
    Serial.print(static_cast<unsigned long>(detected));
    Serial.print(" probe_ms=");
    Serial.println(static_cast<unsigned long>(millis() - psram_start));

    report("ready");
}

void loop()
{
    delay(2000);
    report("alive");
}
