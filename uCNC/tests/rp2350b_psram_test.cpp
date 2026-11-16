#include <Arduino.h>

#include <stddef.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "pico/sync.h"

extern "C" {
extern uint8_t __psram_start__[];
}

namespace {

constexpr size_t kExpectedPsramBytes = 8u * 1024u * 1024u;
constexpr uint32_t kAddressSeed = 0xA5C35A3Du;
constexpr uint32_t kAddressStride = 0x9E3779B1u;

volatile uint32_t *const kPsram =
    reinterpret_cast<volatile uint32_t *>(&__psram_start__);
bool g_finished;
bool g_passed;
size_t g_detected;
size_t g_first_error;

uint32_t address_pattern(size_t word_index, bool invert)
{
    uint32_t value = kAddressSeed ^ (static_cast<uint32_t>(word_index) * kAddressStride);
    return invert ? value ^ 0xFFFFFFFFu : value;
}

bool fill_and_verify_constant(size_t words, uint32_t value, size_t *first_error)
{
    for (size_t i = 0; i < words; ++i) {
        kPsram[i] = value;
    }
    __dmb();
    for (size_t i = 0; i < words; ++i) {
        if (kPsram[i] != value) {
            if (first_error) {
                *first_error = i;
            }
            return false;
        }
    }
    return true;
}

bool fill_and_verify_address(size_t words, bool invert, size_t *first_error)
{
    for (size_t i = 0; i < words; ++i) {
        kPsram[i] = address_pattern(i, invert);
    }
    __dmb();
    for (size_t i = 0; i < words; ++i) {
        if (kPsram[i] != address_pattern(i, invert)) {
            if (first_error) {
                *first_error = i;
            }
            return false;
        }
    }
    return true;
}

void print_result(const char *name, bool ok, size_t first_error)
{
    Serial.print("PSRAM pattern ");
    Serial.print(name);
    Serial.print(": ");
    if (ok) {
        Serial.println("PASS");
    } else {
        Serial.print("FAIL word=");
        Serial.print(static_cast<unsigned long>(first_error));
        Serial.print(" byte=");
        Serial.println(static_cast<unsigned long>(first_error * sizeof(uint32_t)));
    }
}

void print_failure(const char *reason)
{
    Serial.print("PSRAM TEST: FAIL ");
    Serial.println(reason);
    g_finished = true;
    g_passed = false;
}

} // namespace

void setup()
{
    Serial.begin(115200);
    const uint32_t start = millis();
    while (!Serial && (millis() - start) < 3000u) {
        delay(10);
    }
    delay(250);

    const size_t detected = rp2040.getPSRAMSize();
    g_detected = detected;
    Serial.println();
    Serial.println("WEACT RP2350B PSRAM bench test");
    Serial.print("PSRAM CS=GPIO");
    Serial.println(RP2350_PSRAM_CS);
    Serial.print("detected bytes=");
    Serial.println(static_cast<unsigned long>(detected));

    if (detected == 0u) {
        print_failure("chip not detected (check solder, orientation, GPIO0)");
        return;
    }
    if (detected != kExpectedPsramBytes) {
        print_failure("detected size is not the expected 8 MiB");
        return;
    }

    const size_t words = detected / sizeof(uint32_t);
    bool ok = fill_and_verify_constant(words, 0x00000000u, &g_first_error);
    print_result("00", ok, g_first_error);
    if (ok) {
        ok = fill_and_verify_constant(words, 0xFFFFFFFFu, &g_first_error);
        print_result("FF", ok, g_first_error);
    }
    if (ok) {
        ok = fill_and_verify_constant(words, 0x55555555u, &g_first_error);
        print_result("55", ok, g_first_error);
    }
    if (ok) {
        ok = fill_and_verify_constant(words, 0xAAAAAAAAu, &g_first_error);
        print_result("AA", ok, g_first_error);
    }
    if (ok) {
        ok = fill_and_verify_address(words, false, &g_first_error);
        print_result("address", ok, g_first_error);
    }
    if (ok) {
        ok = fill_and_verify_address(words, true, &g_first_error);
        print_result("address-inverse", ok, g_first_error);
    }

    g_finished = true;
    g_passed = ok;
    if (ok) {
        Serial.println("PSRAM TEST: PASS 8 MiB");
    } else {
        Serial.println("PSRAM TEST: FAIL data mismatch");
    }
}

void loop()
{
    delay(5000);
    Serial.print("PSRAM TEST: ");
    if (!g_finished) {
        Serial.println("IN PROGRESS");
        return;
    }
    Serial.print(g_passed ? "PASS" : "FAIL");
    Serial.print(" detected=");
    Serial.print(static_cast<unsigned long>(g_detected));
    Serial.print(" expected=");
    Serial.print(static_cast<unsigned long>(kExpectedPsramBytes));
    if (!g_passed) {
        Serial.print(" first_error_byte=");
        Serial.print(static_cast<unsigned long>(g_first_error * sizeof(uint32_t)));
    }
    Serial.println();
}
