# CAM keyboard notes

The live keyboard path uses uCNC `SOFTI2C()` only. The TCA8418 keypad is read on demand by `cam_keyboard_update()`, with the RP2350 LVDS target currently mapping:

- SDA: `CAM_KB_I2C_SDA DOUT16` on GPIO2
- SCL: `CAM_KB_I2C_SCL DOUT17` on GPIO3
- Address: `0x34`

Earlier RP2350-specific keyboard paths were removed from release code:

- The local RP2350 bit-bang path worked, but it duplicated `softi2c`.
- The RP2350 hardware I2C test was not boot-reliable in this target. Hardware I2C1 wants SDA on GPIO2 and SCL on GPIO3, but the SDK blocking read/write path can stall the machine when the bus is miswired or the device is absent. Bring it back only as a nonblocking state-machine driver.
