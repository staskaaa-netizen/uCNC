Import("env")

from pathlib import Path

disphstx_root = Path("C:/acc/rp2350/DispHSTX")

if not disphstx_root.exists():
    raise RuntimeError("DispHSTX root not found: %s" % disphstx_root)

env.Prepend(
    CPPPATH=[
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
