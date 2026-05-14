Import("env")

from pathlib import Path

disphstx_root = Path("C:/acc/rp2350/DispHSTX")
sd_root = Path("C:/acc/RP2350-PiZero/C/03-MicroSD/src")

if not disphstx_root.exists():
    raise RuntimeError("DispHSTX root not found: %s" % disphstx_root)
if not sd_root.exists():
    raise RuntimeError("RP2350 MicroSD source root not found: %s" % sd_root)

env.Prepend(
    CPPPATH=[
        str(sd_root / "ff15" / "source"),
        str(sd_root / "include"),
        str(sd_root / "sd_driver"),
        "uCNC/src/modules/leancam_rp2350_hdmi",
        str(disphstx_root / "_display" / "disphstx"),
        str(disphstx_root / "_lib" / "inc"),
    ],
)

env.Append(
    CPPDEFINES=[
        ("PICO_MAX_SHARED_IRQ_HANDLERS", "8u"),
    ],
    ASFLAGS=[
        "-IuCNC/src/modules/leancam_rp2350_hdmi",
        "-I%s" % (disphstx_root / "_display" / "disphstx"),
        "-I%s" % (disphstx_root / "_lib" / "inc"),
    ],
)

env.BuildSources(
    "$BUILD_DIR/DispHSTX",
    str(disphstx_root),
    src_filter=[
        "+<_display/disphstx/disphstx_dvi_m33.S>",
        "+<_display/disphstx/disphstx_vga_m33.S>",
        "+<_display/disphstx/disphstx_dvi.c>",
        "+<_display/disphstx/disphstx_dvi_render.c>",
        "+<_display/disphstx/disphstx_vga.c>",
        "+<_display/disphstx/disphstx_vga_render.c>",
        "+<_display/disphstx/disphstx_vmode.c>",
        "+<_display/disphstx/disphstx_vmode_simple.c>",
        "+<_display/disphstx/disphstx_vmode_format.c>",
        "+<_display/disphstx/disphstx_vmode_time.c>",
        "+<_display/disphstx/disphstx_picolibsk.c>",
        "+<_font/font_boldB_8x14.c>",
        "+<_font/font_boldB_8x16.c>",
        "+<_font/font_bold_8x14.c>",
        "+<_font/font_bold_8x16.c>",
        "+<_font/font_bold_8x8.c>",
        "+<_font/font_cond_6x8.c>",
        "+<_font/font_game_8x8.c>",
        "+<_font/font_ibmtiny_8x8.c>",
        "+<_font/font_ibm_8x14.c>",
        "+<_font/font_ibm_8x16.c>",
        "+<_font/font_ibm_8x8.c>",
        "+<_font/font_italic_8x8.c>",
        "+<_font/font_thin_8x8.c>",
        "+<_font/font_tiny_5x8.c>",
        "+<_lib/src/lib_drawcan.c>",
        "+<_lib/src/lib_drawcan1.c>",
        "+<_lib/src/lib_drawcan2.c>",
        "+<_lib/src/lib_drawcan3.c>",
        "+<_lib/src/lib_drawcan4.c>",
        "+<_lib/src/lib_drawcan6.c>",
        "+<_lib/src/lib_drawcan8.c>",
        "+<_lib/src/lib_drawcan12.c>",
        "+<_lib/src/lib_drawcan16.c>",
        "+<_lib/src/lib_rand.c>",
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
