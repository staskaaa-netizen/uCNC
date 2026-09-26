"""Render a short screenshot show of the NC panel, from the real program.

One frame per action, in the order an operator meets them: jogging by hand,
opening a file, inserting a cycle with the 3x3 helper, the full-screen view, the
tool table and its own 3x3 submenu, and two steps of RUN. Every frame is the
panel alone - the 800x600 screen the machine draws, nothing of the bench around
it - written as both .bmp and .png, plus a contact sheet with all of them and
SHOW.md describing each step.

The program and tool table are the NC module's own fixtures
(uCNC/src/modules/nc/tests/fixtures), so the show is the same job the tests run
on. Run it after a change with:

    python tools\\nc_ui_show.py
"""
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib

import test_nc_ui as bench

ROOT = bench.ROOT
SRC = bench.SRC
FIXTURES = SRC / "modules" / "nc" / "tests" / "fixtures"
OUT = ROOT / "tmp" / "nc-ui-show"
ROOT_DIR = OUT / "root"

# The steps, in order. `keys` is the bench's own key script, `ticks` runs the
# machine on afterwards (nothing moves otherwise).
STEPS = [
    ("01-manual-stop", "F1,*", 2,
     "MANUAL: `*` opens the minus stop of the picked axis. Type it, `*` takes it "
     "and opens the plus one; `D` puts the axis limit the setup states in it."),
    ("02-manual-step", "F1,8", 2,
     "MANUAL: a step jog - `8` is X-. The step and the feed are the two values "
     "the pane shows; `1`/`3` change the one the mode is using."),
    ("03-manual-feed", "F1,#,HOLD8,WAIT6,RELEASE", 0,
     "MANUAL: `#` swaps the step for feeding, and holding a direction key feeds "
     "until it comes up - the DRO floats while it moves."),
    ("04-files-list", "F4,0", 2,
     "RUN: `0 FILE` opens the file list. Nothing is marked until `B`/`C` move "
     "the cursor, and the left pane previews what it points at."),
    ("05-file-open", "F4,0,4", 2,
     "RUN: `4 OPEN` loads the program - the same document the editor and the "
     "preview use, never a copy."),
    ("06-edit-3x3", "F2,C,4", 2,
     "EDIT: `4 G7X` opens the floating 3x3 helper on a labelled line of its "
     "own (OD / ID / FACE / Q / N / G80)."),
    ("07-edit-insert", "F2,C,4,1", 2,
     "EDIT: choosing OD replaces that line with the template, ready for the "
     "field-by-field entry."),
    ("08-edit-typed", "F2,C,C,D,5", 2,
     "EDIT: `D` picks the first field of the line the cursor is on, and the "
     "digits type over its value - the box around it is the field being typed."),
    ("09-tools-table", "F3", 2,
     "TOOLS: the global tool table from `tool.t`. The tip block shows the "
     "tool's line, its numbers and the orientation glyph."),
    ("10-tools-field", "F3,D", 2,
     "TOOLS: the same editor, field by field - `D` walks the row and the digits "
     "type the value the tool cuts with."),
    ("11-run-single", "F4,DOWN,DOWN,DOWN,DOWN,1", 30,
     "RUN: `1 SINGLE` runs the block the mark is on. The setup lines are the "
     "panel's, so they are not sent; reaching the G71 line sends the whole "
     "cycle, and the mark stays on the line the operator stepped from."),
    ("12-run-full", "F4,3", 20,
     "RUN: `3 FULL` arms the whole program. `4 HOLD` and `5 STOP` are beside "
     "it, and `# RELOAD` re-reads the file from the card."),
]


def write_png(path, width, height, pixels):
    """Minimal 32bpp BGRA buffer -> PNG (no dependencies)."""
    raw = bytearray()
    for y in range(height):
        raw.append(0)                       # filter: none
        row = y * width * 4
        for x in range(width):
            b, g, r = pixels[row + x * 4], pixels[row + x * 4 + 1], pixels[row + x * 4 + 2]
            raw += bytes((r, g, b))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)


def read_bmp(path):
    data = path.read_bytes()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bits = struct.unpack_from("<H", data, 28)[0]
    bottom_up = height > 0
    height = abs(height)
    body = data[offset:]
    if bits == 32:
        return width, height, body
    if bits != 24:
        raise SystemExit(f"show: {path} is {bits}bpp, cannot read it")
    # 24bpp: three bytes a pixel, rows padded to four. The panel frame is
    # written bottom-up; the bench frame is not, hence `bottom_up`.
    stride = (width * 3 + 3) & ~3
    pixels = bytearray(width * height * 4)
    for y in range(height):
        src = (height - 1 - y if bottom_up else y) * stride
        dst = y * width * 4
        for x in range(width):
            b, g, r = body[src + x * 3], body[src + x * 3 + 1], body[src + x * 3 + 2]
            pixels[dst + x * 4] = b
            pixels[dst + x * 4 + 1] = g
            pixels[dst + x * 4 + 2] = r
            pixels[dst + x * 4 + 3] = 255
    return width, height, pixels


def contact_sheet(frames, path, columns=3, gap=8):
    width, height, _ = frames[0]["size"], frames[0]["height"], None
    rows = (len(frames) + columns - 1) // columns
    sheet_w = columns * width + (columns + 1) * gap
    sheet_h = rows * height + (rows + 1) * gap
    sheet = bytearray(b"\x30\x30\x30\x00" * (sheet_w * sheet_h))

    for i, frame in enumerate(frames):
        col = i % columns
        row = i // columns
        x0 = gap + col * (width + gap)
        y0 = gap + row * (height + gap)
        for y in range(height):
            src = y * width * 4
            dst = ((y0 + y) * sheet_w + x0) * 4
            sheet[dst:dst + width * 4] = frame["pixels"][src:src + width * 4]
    write_png(path, sheet_w, sheet_h, sheet)
    return sheet_w, sheet_h


def main():
    exe = bench.build()

    shutil.rmtree(OUT, ignore_errors=True)
    (ROOT_DIR / "nc" / "files").mkdir(parents=True, exist_ok=True)
    for name in ("facing.nc", "tool.t"):
        shutil.copy(FIXTURES / name, ROOT_DIR / "nc" / "files" / name)
    # Boot EDIT on the program and keep the tool table beside it, which is also
    # how the machine reopens the last session.
    (ROOT_DIR / "nc_state.txt").write_text(
        "MODE=EDIT\n"
        "EDIT=/D/nc/files/facing.nc\n"
        "RUN=/D/nc/files/facing.nc\n"
        "TOOLS=/D/nc/files/tool.t\n",
        encoding="utf-8")

    frames = []
    for name, keys, ticks, caption in STEPS:
        bmp = OUT / f"{name}.bmp"
        cmd = [str(exe), "--files", str(ROOT_DIR), "--keys", keys,
               "--dump", str(bmp)]
        if ticks:
            cmd[cmd.index("--dump"):cmd.index("--dump")] = ["--ticks", str(ticks)]
        run = subprocess.run(cmd, capture_output=True, text=True)
        if run.returncode or not bmp.exists():
            print(run.stdout[-2000:])
            print(run.stderr[-4000:])
            sys.exit(f"FAIL could not render {name}")
        width, height, pixels = read_bmp(bmp)
        write_png(OUT / f"{name}.png", width, height, pixels)
        frames.append({"name": name, "size": width, "height": height,
                       "pixels": pixels, "caption": caption})
        print(f"nc_ui show: {name}.png")

    sheet_w, sheet_h = contact_sheet(frames, OUT / "contact-sheet.png")
    index = ["# NC panel show", "",
             "Every frame is the panel alone: the 800x600 screen the machine "
             "draws, exactly as it draws it. (The bench that produced them has a "
             "key row beside the panel; that is a PC aid for driving it and is "
             "not part of the machine, so it is not in these images.) The "
             "program and tool table are the NC module's fixtures "
             "(`uCNC/src/modules/nc/tests/fixtures`).", "",
             f"Contact sheet: `contact-sheet.png` ({sheet_w}x{sheet_h}).", ""]
    for frame in frames:
        index.append(f"- `{frame['name']}.png` - {frame['caption']}")
    (OUT / "SHOW.md").write_text("\n".join(index) + "\n", encoding="utf-8")
    print(f"nc_ui show: {len(frames)} frames, contact sheet {sheet_w}x{sheet_h}, "
          f"index {OUT / 'SHOW.md'}")


if __name__ == "__main__":
    main()
