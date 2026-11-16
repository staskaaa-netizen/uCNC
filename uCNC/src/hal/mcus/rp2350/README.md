_µCNC for RP2040 can be built this way_

## Method one - PlatformIO (preferred)

1. Get [Visual Studio Code](https://code.visualstudio.com/download) and install it.
2. Install the PlatformIO extension.
3. Open uCNC folder in VSCode.
4. Edit ```cnc_config.h file``` and ```cnc_hal_config.h file``` to fit your needs and board.
5. If needed edit the platformio.ini file environment for your board. Compile the sketch and upload it to your board.

## Method two - Arduino IDE (easiest)

1. Get [Arduino IDE](https://www.arduino.cc/en/software) and install it.
2. If you don't have install ESP8266 for Arduino with Arduino board manager has explained [here](https://github.com/earlephilhower/arduino-pico#installing-via-arduino-boards-manager)
3. Go to uCNC folder and open uCNC.ino sketch.
4. Edit ```cnc_config.h file``` and ```cnc_hal_config.h file``` to fit your needs and board.
5. Compile the sketch and upload it to your board.

## Full-duplex 74HC595/74HC165 PIO

RP2350 can move a four-register 595 output chain and a four-register 165 input
chain in one PIO transaction. The DATA/CLK/LATCH pins must be consecutive; the
165 serial return may be on another GPIO.

```c
#define IC74HC595_CUSTOM_SHIFT_IO
#define IC74HC595_PIO_DATA 20
#define IC74HC595_PIO_CLK 21
#define IC74HC595_PIO_LATCH 22
#define IC74HC595_PIO_DATA_IN 23
```

The PIO uses a two-pulse 595 latch at the frame boundaries, so the outputs stay
stable while the shared clock shifts both chips. Carriers with a separate 165
parallel-load pin pulse that pin in `shift_register_io_pins()` immediately
before the PIO transfer.

Keep separate PIO instruction memories between owners. The NCC machine build
uses:

| PIO owner | Block | State machine | Instruction space |
| --- | ---: | ---: | --- |
| Spindle encoder | PIO0 | SM0 | fixed offset 0, 24 instructions |
| 595/165 extender | PIO1 | SM0 | dynamically allocated program |
| Unused | PIO2 | - | reserved |

The extender must define `IC74HC595_PIO_INDEX 1` on this build. Moving it back
to PIO0 overwrites or reuses the encoder program's instruction region.
