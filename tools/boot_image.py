"""Turn a picture into the panel's boot image.

The panel draws with **sixteen colours** (`lvds_palette.c`; the HSTX output is
paletted and the table is full), so a boot picture is not a picture file at
runtime: it is that picture reduced to sixteen colours, stored as runs in flash,
and painted once at start-up. This tool does the reducing and writes the
module's asset - the way `lvds_font_ibm_8x14.c` is a generated file beside the
font it came from.

    python tools\\boot_image.py <picture> [--preview out.bmp] [--dry-run]

**The picture brings its own sixteen colours by default.** The panel's own table
has no blue in it and its greens are bright, so a picture quantised onto the
panel's colours comes out grey (the first attempt at the lathe picture did: the
overalls and the machine both lost their colour). The splash therefore takes the
palette for itself, paints, and **gives it back** before the panel draws a
pixel (`ncc_boot_release()` -> `lvds_hstx_palette_reset()`), which is what keeps
the picture's colour without changing anything about the panel's own.

`--panel-palette` keeps the old behaviour - no palette of its own, quantised onto
the panel's sixteen - for anyone who wants a splash that cannot disturb them.

`--preview` writes what the panel would show (the picture fitted to the glass
and centred on the panel's black) so the result can be looked at before it is
flashed. Without `--dry-run` the asset is written to
`uCNC/src/modules/ncc/ncc_boot_image.{c,h}`.

The picture is fitted to the panel's *view*: 600 wide by 800 tall (the glass is
mounted turned and the renderer owns the turn), so a wide picture lands as a band
across the middle.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PALETTE_C = ROOT / "uCNC" / "src" / "modules" / "lvds_renderer" / "lvds_palette.c"
ASSET_C = ROOT / "uCNC" / "src" / "modules" / "ncc" / "ncc_boot_image.c"
ASSET_H = ROOT / "uCNC" / "src" / "modules" / "ncc" / "ncc_boot_image.h"

# The picture's size, from the renderer: `LVDS_VIEW_WIDTH x LVDS_VIEW_HEIGHT`
# with the panel mounted turned (`lvds_hstx.h`).
VIEW_W = 600
VIEW_H = 800
# The palette has sixteen slots and the first is the backend's own black
# (`palette_reset()` in lvds_hstx.c), so a picture that wants every colour it
# asks for kept exactly - and not the nearest of whatever got there first - uses
# fifteen.
COLOURS = 15


def panel_palette():
    """The panel's colours, in id order, read from the file that defines them.

    Reading them here rather than writing them down again is what keeps the tool
    and the palette from drifting: a colour that moves in `lvds_palette.c` moves
    in the next boot image too."""
    text = PALETTE_C.read_text()
    entries = re.findall(r"\[(\w+)\]\s*=\s*\{\"\w+\",\s*\"#([0-9a-fA-F]{6})\"\}",
                         text)
    if not entries:
        sys.exit(f"boot_image: no colours found in {PALETTE_C}")
    return [(name, tuple(int(hexv[i:i + 2], 16) for i in (0, 2, 4)))
            for name, hexv in entries]


def fit(image, width, height):
    """The picture at the view's width (or its height, whichever binds), scaled
    with no smoothing so pixel art stays pixel art."""
    from PIL import Image

    scale = width / image.width
    if int(image.height * scale + 0.5) > height:
        scale = height / image.height
    w = max(1, int(image.width * scale + 0.5))
    h = max(1, int(image.height * scale + 0.5))
    return image.resize((w, h), Image.NEAREST)


def own_palette(image):
    """The picture's own sixteen colours, and the picture as indices into them.

    The sixteen are what the renderer will take for the splash, in this order;
    the panel's palette is untouched by any of it until the splash lets go."""
    reduced = image.convert("RGB").convert(
        "P", palette=__import__("PIL.Image", fromlist=["Image"]).Palette.ADAPTIVE,
        colors=COLOURS)
    raw = reduced.getpalette()
    used = reduced.getcolors(1 << 24) or []
    used.sort(reverse=True)
    index_of = [entry[1] for entry in used]
    colours = []
    for old in index_of:
        colours.append(tuple(raw[old * 3:old * 3 + 3]))
    remap = {old: new for new, old in enumerate(index_of)}
    pixels = [remap[value] for value in reduced.get_flattened_data()]
    return colours, pixels


def panel_quantised(image, palette_names):
    """The picture mapped onto the panel's own sixteen, the way the glass would
    have mapped every colour it was given (nearest in 5-6-5)."""
    five = [((r >> 3), (g >> 2), (b >> 3)) for _n, (r, g, b) in palette_names]
    cache = {}
    pixels = []
    for r, g, b in image.convert("RGB").get_flattened_data():
        key = (r >> 3, g >> 2, b >> 3)
        best = cache.get(key)
        if best is None:
            best = min(range(len(five)),
                       key=lambda i: (key[0] - five[i][0]) ** 2 +
                       (key[1] - five[i][1]) ** 2 +
                       (key[2] - five[i][2]) ** 2)
            cache[key] = best
        pixels.append(best)
    return [c for _n, c in palette_names], pixels


def row_runs(pixels, width, height):
    """The pixels as (index, count) runs, one row at a time, so painting them is
    a fill per run and no run crosses a row."""
    out = []
    for y in range(height):
        row = pixels[y * width:(y + 1) * width]
        last = None
        count = 0
        for value in row:
            if value == last and count < 0xFFFF:
                count += 1
                continue
            if last is not None:
                out.append((last, count))
            last = value
            count = 1
        if last is not None:
            out.append((last, count))
    return out


def clean(pixels, width, height):
    """A 3x3 median over the colour *indices*: the source picture has been
    resampled (it holds twenty-eight thousand colours), so a straight reduction
    leaves single-pixel speckle - noise on the glass, and a run of its own in
    the asset for every speck. The median takes the speckle out and the runs
    with it, which is a sixteenth of the flash. The picture's own edges survive:
    three pixels of one colour in a line are already a majority."""
    out = list(pixels)
    for y in range(height):
        for x in range(width):
            window = []
            for dy in (-1, 0, 1):
                yy = min(max(y + dy, 0), height - 1)
                for dx in (-1, 0, 1):
                    xx = min(max(x + dx, 0), width - 1)
                    window.append(pixels[yy * width + xx])
            window.sort()
            out[y * width + x] = window[4]
    return out


def write_asset(name, colours, pixels, width, height):
    stream = row_runs(pixels, width, height)
    colour_rows = []
    for i in range(0, len(colours), 4):
        chunk = ", ".join(f"{{0x{r:02X}, 0x{g:02X}, 0x{b:02X}}}" for r, g, b
                          in colours[i:i + 4])
        colour_rows.append("    " + chunk + ",")
    run_rows = []
    for i in range(0, len(stream), 16):
        chunk = ", ".join(f"{index}, {count}u"
                          for index, count in stream[i:i + 16])
        run_rows.append("    " + chunk + ",")
    ASSET_H.write_text(
        "#ifndef NCC_BOOT_IMAGE_H\n"
        "#define NCC_BOOT_IMAGE_H\n"
        "\n"
        "#include <stdint.h>\n"
        "\n"
        f"/* The boot picture ({name}): its own sixteen colours and the rows that\n"
        "   use them. Generated by `tools/boot_image.py` from the picture beside\n"
        "   it - change the picture and run the tool, never this. The colours are\n"
        "   taken from the renderer's palette when the splash starts and given\n"
        "   back when it ends (`ncc_boot_release()`), so the panel's own colours\n"
        "   are the panel's own again by the time it draws. */\n"
        f"#define NCC_BOOT_IMAGE_W {width}\n"
        f"#define NCC_BOOT_IMAGE_H {height}\n"
        f"#define NCC_BOOT_IMAGE_X {(VIEW_W - width) // 2}\n"
        f"#define NCC_BOOT_IMAGE_Y {(VIEW_H - height) // 2}\n"
        f"#define NCC_BOOT_IMAGE_COLOURS {len(colours)}\n"
        "/* The picture is centred on the panel's black; this is the colour the\n"
        "   letterbox is painted in. */\n"
        "#define NCC_BOOT_IMAGE_BG 0\n"
        "\n"
        "typedef struct {\n"
        "    uint8_t r;\n"
        "    uint8_t g;\n"
        "    uint8_t b;\n"
        "} ncc_boot_colour_t;\n"
        "\n"
        "extern const ncc_boot_colour_t ncc_boot_image_colours[NCC_BOOT_IMAGE_COLOURS];\n"
        "/* index, count, ... */\n"
        "extern const uint16_t ncc_boot_image_runs[];\n"
        "extern const uint32_t ncc_boot_image_run_count;\n"
        "\n"
        "#endif\n")
    ASSET_C.write_text(
        "/* Generated by `tools/boot_image.py` - see ncc_boot_image.h. */\n"
        '#include "ncc_boot_image.h"\n'
        "\n"
        "const ncc_boot_colour_t ncc_boot_image_colours[NCC_BOOT_IMAGE_COLOURS] = {\n"
        + "\n".join(colour_rows) + "\n"
        "};\n"
        "\n"
        "const uint16_t ncc_boot_image_runs[] = {\n"
        + "\n".join(run_rows) + "\n"
        "};\n"
        "\n"
        f"const uint32_t ncc_boot_image_run_count = {len(stream)}u;\n")
    return len(stream)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("picture")
    parser.add_argument("--preview")
    parser.add_argument("--panel-palette", action="store_true",
                        help="quantise onto the panel's own sixteen colours "
                             "instead of taking a palette for the picture")
    parser.add_argument("--no-clean", action="store_true",
                        help="keep the speckle the resampled source brings")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    from PIL import Image

    picture = fit(Image.open(args.picture), VIEW_W, VIEW_H)
    if args.panel_palette:
        colours, pixels = panel_quantised(picture, panel_palette())
        note = "the panel's own colours"
    else:
        colours, pixels = own_palette(picture)
        note = "the picture's own colours"
    print(f"boot_image: {args.picture} -> {picture.width}x{picture.height}, "
          f"{len(colours)} colours ({note})")
    if not args.no_clean:
        pixels = clean(pixels, picture.width, picture.height)

    if args.preview:
        background = (0, 0, 0)
        out = Image.new("RGB", (VIEW_W, VIEW_H), background)
        band = Image.new("RGB", (picture.width, picture.height))
        band.putdata([colours[i] for i in pixels])
        out.paste(band, ((VIEW_W - picture.width) // 2,
                         (VIEW_H - picture.height) // 2))
        out.save(args.preview)
        print(f"boot_image: preview written to {args.preview}")

    if args.dry_run:
        return
    count = write_asset(Path(args.picture).name, colours, pixels,
                        picture.width, picture.height)
    print(f"boot_image: {count} runs written to {ASSET_C.name}")


if __name__ == "__main__":
    main()
