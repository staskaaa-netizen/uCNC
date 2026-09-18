#include "src/cnc.h"

/* Boot indicator on a stepper driver output (the only LEDs on this machine).

   Pattern:
     - LED on while ucnc_init() runs, so a boot hang is visible as a steady
       light (modules load inside ucnc_init: SD mount and the NC/LVDS bring-up);
     - three quick blinks and then off when boot completed;
     - nothing at all means setup() was never reached (power, flash image or
       an upload that did not take).

   The default pin is the stepper enable line, whose driver LED is the least
   intrusive to toggle while the machine is still locked. Point BOOT_LED_PIN at
   STEP0_BIT or DIR0_BIT if you prefer one of those LEDs, or -1 to disable. */
#ifndef BOOT_LED_PIN
#define BOOT_LED_PIN STEP0_EN_BIT
#endif

#if BOOT_LED_PIN >= 0
#include <pico/stdlib.h>

static void boot_led_init(void)
{
	gpio_init(BOOT_LED_PIN);
	gpio_set_dir(BOOT_LED_PIN, GPIO_OUT);
	gpio_put(BOOT_LED_PIN, 0);
}

static void boot_led_level(int on)
{
	gpio_put(BOOT_LED_PIN, on ? 1 : 0);
}

static void boot_led_done(void)
{
	unsigned i;

	for (i = 0; i < 3u; i++) {
		boot_led_level(1);
		sleep_ms(80);
		boot_led_level(0);
		sleep_ms(120);
	}
}
#else
#define boot_led_init() do { } while (0)
#define boot_led_level(on) do { (void)(on); } while (0)
#define boot_led_done() do { } while (0)
#endif

void setup()
{
	// put your setup code here, to run once:
	boot_led_init();
	boot_led_level(1);	/* booting: steady light if it hangs here */
	ucnc_init();
	boot_led_done();	/* three blinks then off: boot completed */
}

void loop()
{
	// put your main code here, to run repeatedly:
	ucnc_run();
}
