Import("env")

from pathlib import Path

sd_root = Path("C:/acc/RP2350-PiZero/C/03-MicroSD/src")

if not sd_root.exists():
    raise RuntimeError("RP2350 MicroSD source root not found: %s" % sd_root)

env.Prepend(
    CPPPATH=[
        str(sd_root / "ff15" / "source"),
        str(sd_root / "include"),
        str(sd_root / "sd_driver"),
        "uCNC/src/modules/leancam_rp2350_sd",
    ],
)

env.BuildSources(
    "$BUILD_DIR/no-OS-FatFS-SD",
    str(sd_root),
    src_filter=[
        "+<ff15/source/ff.c>",
        "+<ff15/source/ffsystem.c>",
        "+<ff15/source/ffunicode.c>",
        "+<sd_driver/dma_interrupts.c>",
        "+<sd_driver/sd_card.c>",
        "+<sd_driver/SDIO/rp2040_sdio.c>",
        "+<sd_driver/SDIO/sd_card_sdio.c>",
        "+<sd_driver/SPI/my_spi.c>",
        "+<sd_driver/SPI/sd_card_spi.c>",
        "+<sd_driver/SPI/sd_spi.c>",
        "+<src/crash.c>",
        "+<src/crc.c>",
        "+<src/f_util.c>",
        "+<src/ff_stdio.c>",
        "+<src/file_stream.c>",
        "+<src/glue.c>",
        "+<src/my_debug.c>",
        "+<src/my_rtc.c>",
        "+<src/sd_timeouts.c>",
        "+<src/util.c>",
    ],
)
