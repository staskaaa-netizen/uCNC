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

The NCC machine uses separate PIO instruction memories:

| Owner | PIO | State machines | GPIO base |
| --- | ---: | --- | ---: |
| 595/165 extender | PIO0 | SM0 | 0 |
| ENC0 | PIO1 | SM0 | 16 |
| ENC1, ENC2 | PIO2 | SM0, SM1 | 16 |

RP2350 PIO addresses either GPIO0-31 or GPIO16-47. Encoder blocks therefore
select base 16 before applying their quadrature program. Without that step,
GPIO32/33 alias onto GPIO0/1 and corrupt both the PSRAM-backed renderer and the
encoder count.

HSTX scanout is native and does not own a PIO block. Its GPIO12-19 pins and DMA
channels 10 and 11 are independent of the three PIO owners above.
