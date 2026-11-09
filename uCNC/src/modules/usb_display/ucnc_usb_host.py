"""
Build the Pico SDK's TinyUSB for the uCNC USB-display targets.

The Arduino-Pico platform links its own TinyUSB *device* stack out of a
prebuilt archive, and a prebuilt device stack cannot be turned into a host one
at link time. The display needs the host, so these targets compile TinyUSB from
the sources the framework already ships, with the host enabled in
`tusb_config.h` (which is this project's own, named by CFG_TUSB_CONFIG_FILE).

Both halves are built: the core's USB objects still reference the device
symbols, so they have to resolve, while the firmware initialises the controller
as a host and never starts the device side.

Only the RP2040 family port is taken from `src/portable` - the rest of that
directory is other vendors' silicon and would not compile here.
"""

Import("env")  # noqa: F821 - provided by PlatformIO's script runner

import os

_fw = env.PioPlatform().get_package_dir("framework-arduinopico")  # noqa: F821
_tusb = os.path.join(_fw, "pico-sdk", "lib", "tinyusb")
_tusb_src = os.path.join(_tusb, "src")
_tusb_port = os.path.join(_tusb_src, "portable", "raspberrypi", "rp2040")

env.Append(CPPPATH=[_tusb_src, _tusb_port])  # noqa: F821

env.BuildSources(  # noqa: F821
    os.path.join("$BUILD_DIR", "tinyusb"),
    _tusb_src,
    src_filter=[
        "+<common/>",
        "+<host/>",
        "+<tusb.c>",
        "+<tusb_fifo.c>",
    ],
)

env.BuildSources(  # noqa: F821
    os.path.join("$BUILD_DIR", "tinyusb_rp2040"),
    _tusb_port,
    src_filter=["+<*>"],
)
