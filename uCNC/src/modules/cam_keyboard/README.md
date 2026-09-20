# CAM keyboard notes

The live keyboard path uses uCNC `SOFTI2C()` only. The TCA8418 keypad is read on demand by `cam_keyboard_update()`, with the RP2350 LVDS target currently mapping:

- SDA: `CAM_KB_I2C_SDA DOUT16` on GPIO2
- SCL: `CAM_KB_I2C_SCL DOUT17` on GPIO3
- Address: `0x34`

Earlier RP2350-specific keyboard paths were removed from release code:

- The local RP2350 bit-bang path worked, but it duplicated `softi2c`.
- The RP2350 hardware I2C test was not boot-reliable in this target. Hardware I2C1 wants SDA on GPIO2 and SCL on GPIO3, but the SDK blocking read/write path can stall the machine when the bus is miswired or the device is absent. Bring it back only as a nonblocking state-machine driver.

## Keypad characters

The keypad reports its keys as characters: digits `0`-`9`, `*`, `#`, and
`A`-`D`, decoded from the TCA8418 matrix in `cam_keyboard_decode_key()`
(`* 0 # D` / `1 2 3 C` / `4 5 6 B` / `7 8 9 A`, row 1 first) -
`ui_input_keypad.c` is the char-to-code table and `ui_key_char()` the inverse.
Everything above the driver reacts to the character, not to a hardware code:
the NC module maps it with `nc_visual_key_for_char()`, and the desktop panel
shell (`tools/nc_ui_win`) presses the same characters, so the bench and the
machine cannot drift apart. A key that is added here needs a meaning there.

Press and release are both tracked, and the event byte carries both facts:
**bit 7 is the release flag, the low seven bits are the key**. Decoding a
release as "no key" (which this module did) is a nasty little fault: the release
never cleared the key, so the driver went on reporting it as down, and
everything above took the next press of that key for a repeat and threw it
away. Every key then worked once and only once, until a different key was
pressed. `cam_keyboard_decode_key()` therefore decodes the key either way and
the press/release edge comes from bit 7 alone.

`cam_keyboard_key()` reports the key that is **down right now**, cleared by its
release; `cam_keyboard_pressed_char()` is the key that went down in this read.
They are two facts and not one: a poll delayed by a screen change (the NC module
reads the card and redraws while switching) can see a whole tap - press *and*
release - in one read, and with only "the key that is down" to look at, that tap
would be lost. `ui_input_keypad_held_key()` is the held character, which is what
MANUAL's feed needs: holding a direction key feeds, and the release has to be
visible for the axis to stop where it is. A read failure clears the key as well,
so a driver that cannot read the keypad never looks like a key that is held.
