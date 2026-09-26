"""Build the Win32 NC panel shell, render one frame and run its checks.

The shell runs the real NC screen code (modules/nc/nc_visual.c) against the host
LVDS backend, so the dumped BMP is the firmware layout. It also dumps the
floating 3x3 helper (EDIT, then footer slot 4) by replaying the machine's own
key path. The same binary then runs every headless check through the firmware
fs_* API in a scratch root, so the repository tree stays clean.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "uCNC" / "src"
TOOL = ROOT / "tools" / "nc_ui_win"
OUT = ROOT / "tmp" / "nc-ui-tests"
OUT.mkdir(parents=True, exist_ok=True)

FLAGS = ["-std=gnu11", "-O1", "-w",
         "-DPIO_UNIT_TESTING", "-DUCNC_IGNORE_BOARDMAP_OVERRIDES",
         "-DUCNC_IGNORE_HAL_OVERRIDES", "-DENABLE_PARSER_MODULES",
         "-DENABLE_MAIN_LOOP_MODULES", "-DENABLE_O_CODES", "-DENABLE_G7X_MODULE",
         "-DG33_ENCODER=0", "-DDISABLE_SAFE_SETTINGS", "-DDISABLE_ENDPROGRAM_LOCK",
         "-DEMULATE_GRBL_STARTUP=3", "-DLVDS_WIDTH=800", "-DLVDS_HEIGHT=600",
         "-include", "host_boardmap.h",
         f"-I{ROOT / 'uCNC'}", f"-I{SRC / 'modules' / 'nc'}",
         f"-I{SRC / 'modules' / 'nc2'}",
         f"-I{SRC}", f"-I{SRC / 'modules'}",
         f"-I{SRC / 'modules' / 'g7x'}", f"-I{SRC / 'modules' / 'lvds_renderer'}",
         f"-I{TOOL}"]

# -static avoids a runtime dependency on libwinpthread-1.dll from the toolchain.
LDFLAGS = ["-mwindows", "-static", "-lgdi32", "-luser32", "-lcomdlg32"]


def fail(message, detail=""):
    """Report a failure the way CI can see it.

    A plain line for whoever reads the log, and - when this runs in GitHub
    Actions - an `::error::` annotation carrying the first useful line of the
    detail. The annotation is what shows on the run and in the API, so a failure
    can be diagnosed without the log (which needs rights this repository's
    readers may not have)."""
    print(message)
    if os.environ.get("GITHUB_ACTIONS"):
        text = " / ".join((detail or message).split())
        print(f"::error title=nc_ui::{text[:1500]}")
    sys.exit(1)


def check(run, wanted, message):
    """Run the station once and insist on the line a passing check prints."""
    print(run.stdout.strip())
    if run.returncode or wanted not in run.stdout:
        fail(f"FAIL {message}",
             run.stdout[-1500:] or run.stderr[-1500:] or f"exit {run.returncode}")


def bmp_region(path, x0, y0, x1, y1):
    """One region of a 32bpp top-down .bmp, as raw bytes (the dumps write that
    shape). Used where two frames are the same picture except for the text: a
    program written in increments *is* different text, but the part it draws is
    the same part."""
    data = path.read_bytes()
    offset = int.from_bytes(data[10:14], "little")
    width = int.from_bytes(data[18:22], "little")
    height = int.from_bytes(data[22:26], "little")
    if height < 0:
        fail(f"FAIL {path} is not a top-down dump")
    out = bytearray()
    for y in range(y0, y1):
        row = offset + (y * width + x0) * 4
        out += data[row:row + (x1 - x0) * 4]
    return bytes(out)


def core_sources():
    files = []
    for directory in ("", "core", "interface", "hal/kinematics", "hal/tools",
                      "hal/tools/tools", "modules"):
        files += sorted((SRC / directory).glob("*.c"))
    files += [SRC / "hal" / "mcus" / "mcu.c",
              SRC / "hal" / "mcus" / "virtual" / "mcu_virtual.c",
              SRC / "hal" / "mcus" / "virtual" / "virtual_windows.c"]
    return files


def module_sources():
    nc = SRC / "modules" / "nc"
    g7x = SRC / "modules" / "g7x"
    lvds = SRC / "modules" / "lvds_renderer"
    names = ["nc.c", "nc_emit.c", "nc_g7x.c", "nc_files.c", "nc_feedback.c",
             "nc_draw.c", "nc_editor.c", "nc_manual.c", "nc_menu.c", "nc_palette.c",
             "nc_presets.c", "nc_run.c", "nc_preview.c",
             "nc_state.c", "nc_text.c", "nc_tools.c", "nc_vocab.c", "nc_visual.c"]
    files = [nc / name for name in names]
    # nc2 is the module that replaces nc; until the panel is switched over it is
    # built here so its own checks run with the same machine and card.
    nc2 = SRC / "modules" / "nc2"
    files += sorted(nc2.glob("*.c"))
    files += [g7x / "g7x.c", g7x / "g7x_blocks.c", g7x / "g7x_contour.c", g7x / "g7x_source.c",
              SRC / "modules" / "cam_keyboard" / "cam_keyboard.c",
              SRC / "modules" / "g7_g8" / "parser_g7_g8.c"]
    files += [lvds / "lvds_draw_api.c", lvds / "lvds_palette.c",
              lvds / "lvds_font_cond_6x8.c", lvds / "lvds_font_ibm_8x14.c"]
    return files


EXE = OUT / "nc_ui.exe"


def build():
    """Build the shell. Returns the exe path, exits the process on failure."""
    sources = [TOOL / "lvds_host.c", TOOL / "host_fs.c", TOOL / "host_shim.c",
               TOOL / "host_spindle.c", TOOL / "host_tests.c", TOOL / "main.c",
               *core_sources(),
               *module_sources()]
    cmd = [os.environ.get("CC", "gcc"), *FLAGS, *[str(p) for p in sources],
           "-o", str(EXE), *LDFLAGS]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
    except FileNotFoundError as missing:
        fail(f"FAIL no C compiler: {cmd[0]} ({missing})",
             "put gcc on PATH, or set CC to the compiler's path")
    (OUT / "build.log").write_text(result.stdout + result.stderr)
    if result.returncode:
        fail("FAIL the station did not build", result.stderr[-1500:])
    return EXE


if __name__ == "__main__":
    exe = build()

    frame = OUT / "frame.bmp"
    run = subprocess.run([str(exe), "--dump", str(frame)], capture_output=True,
                         text=True)
    if run.returncode:
        fail("FAIL the station did not render a frame",
             run.stdout[-1500:] or run.stderr[-1500:])
    if not frame.exists() or frame.stat().st_size < 54:
        fail("FAIL no frame written")
    print(f"nc_ui: built and rendered {frame}")

    modal = OUT / "modal.bmp"
    run = subprocess.run([str(exe), "--files", str(OUT / "modal-root"),
                          "--keys", "F2,4", "--dump", str(modal)],
                         capture_output=True, text=True)
    if run.returncode or not modal.exists() or modal.stat().st_size < 54:
        print(run.stdout[-2000:])
        fail("FAIL the 3x3 helper did not render",
             run.stdout[-1500:] or run.stderr[-1500:])
    print(f"nc_ui: 3x3 helper rendered {modal}")

    root = OUT / "presets-root"
    shutil.rmtree(root, ignore_errors=True)
    run = subprocess.run([str(exe), "--files", str(root), "--presettest"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "presettest: PASS" not in run.stdout:
        fail("FAIL preset file contract",
             run.stdout[-1500:] or run.stderr[-1500:])
    if not (root / "presets").is_dir():
        fail(f"FAIL {root / 'presets'} was not created")

    run = subprocess.run([str(exe), "--files", str(OUT / "stream-root"),
                          "--streamtest"], capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "streamtest: PASS" not in run.stdout:
        fail("FAIL the panel's one-shot blocks did not reach the reader",
             run.stdout[-1500:] or run.stderr[-1500:])

    run = subprocess.run([str(exe), "--files", str(OUT / "pad-root"),
                          "--padtest"], capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "padtest: PASS" not in run.stdout:
        fail("FAIL the keypad does not match the machine's key row",
             run.stdout[-1500:] or run.stderr[-1500:])

    # The legend the editor shows for the word under the cursor: every word the
    # panel writes into a line must say what it is, not "NC word".
    run = subprocess.run([str(exe), "--vocabtest"], capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "vocabtest: PASS" not in run.stdout:
        fail("FAIL a word the panel writes has no legend",
             run.stdout[-1500:] or run.stderr[-1500:])

    # The labels: a fault in the message area is white on red, and the
    # controller's state sits in the DRO's bottom-right corner.
    root = OUT / "label-root"
    shutil.rmtree(root, ignore_errors=True)
    (root / "nc" / "files").mkdir(parents=True, exist_ok=True)
    run = subprocess.run([str(exe), "--files", str(root), "--labeltest"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "labeltest: PASS" not in run.stdout:
        fail("FAIL the error label or the DRO state is not drawn",
             run.stdout[-1500:] or run.stderr[-1500:])

    run = subprocess.run([str(exe), "--files", str(OUT / "feed-root"),
                          "--feedtest"], capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "feedtest: PASS" not in run.stdout:
        fail("FAIL a held key does not feed to the stop",
             run.stdout[-1500:] or run.stderr[-1500:])

    # The station's spindle, off the machine's own signals (PWM0/DOUT0) rather
    # than an encoder the desktop does not have.
    run = subprocess.run([str(exe), "--files", str(OUT / "spindle-root"),
                          "--spindletest"], capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "spindletest: PASS" not in run.stdout:
        fail("FAIL the spindle does not run from the machine's own signals",
             run.stdout[-1500:] or run.stderr[-1500:])

    # The demo the station ships with (tools/nc_ui_win/examples): a fresh card
    # is seeded from it once, and the sample program itself has to load, scan
    # and expand the way the panel does it.
    root = OUT / "demo-root"
    shutil.rmtree(root, ignore_errors=True)
    run = subprocess.run([str(exe), "--files", str(root), "--demotest"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "demotest: PASS" not in run.stdout:
        fail("FAIL the demo does not seed the card or expand as a program",
             run.stdout[-1500:] or run.stderr[-1500:])
    for name in ("lathe-demo.nc", "tool.t"):
        if not (root / "nc" / "files" / name).exists():
            fail(f"FAIL the demo card has no {name}")

    # The entries ship as files too, one per address, and the card is seeded
    # with them. They are generated from the compiled table by
    # `--dump-presets`, so the shipped folder and the fallback the panel uses
    # when a card has no file cannot drift: this regenerates the folder and
    # compares it, byte for byte.
    shipped = TOOL / "examples" / "presets"
    regenerated = OUT / "presets-dump"
    shutil.rmtree(regenerated, ignore_errors=True)
    run = subprocess.run([str(exe), "--files", str(OUT / "presets-dump-root"),
                          "--dump-presets", str(regenerated)],
                         capture_output=True, text=True)
    if run.returncode:
        fail("FAIL the preset entries do not write out as files",
             run.stdout[-800:] or run.stderr[-800:])
    wanted = {p.name for p in shipped.glob("*.txt")} if shipped.is_dir() else set()
    have = {p.name for p in regenerated.glob("*.txt")}
    if not wanted or wanted != have:
        fail(f"FAIL the shipped preset files are {sorted(wanted)} and the "
             f"panel writes {sorted(have)}: rebuild them with "
             f"`nc_ui.exe --dump-presets tools/nc_ui_win/examples/presets`")
    for name in sorted(wanted):
        # Line endings are the checkout's business - the reader skips `\r` on
        # either - so the rows are what is compared.
        got = (shipped / name).read_bytes().replace(b"\r\n", b"\n")
        want = (regenerated / name).read_bytes().replace(b"\r\n", b"\n")
        if got != want:
            fail(f"FAIL the shipped {name} is not what the panel writes: "
                 f"regenerate tools/nc_ui_win/examples/presets")
    print(f"nc_ui: {len(wanted)} preset files are the entries the panel writes")

    # Fanuc's increments, `U` and `W`. `--uwtest` proves the collapse in the
    # sender: the same profile written absolutely and written with increments
    # leaves the stream as the same lines, inside a cycle as well as outside it.
    run = subprocess.run([str(exe), "--files", str(OUT / "uw-root"),
                          "--uwtest"], capture_output=True, text=True)
    check(run, "uwtest: PASS", "the increments do not collapse to the lines they mean")

    # And the preview draws them: the same profile written both ways has to be
    # the same picture, because the drawing reads the increments through the one
    # rule the sender uses (nc_emit_line_point()).
    uw_root = OUT / "uw-root"
    (uw_root / "nc" / "files").mkdir(parents=True, exist_ok=True)
    (uw_root / "nc" / "files" / "absolute.nc").write_text(
        "G970 X-5 U60 Z-60 W5\nG971 X50 Z50 I0 E0\nG0 X52 Z2\n"
        "G71 U1 R0.5 X0.5 Z0.5 F450 P10 Q20\nN10 G0 X30 Z0\nG1 Z-15\n"
        "N20 G1 X50 Z-15\nG80\n", encoding="utf-8")
    (uw_root / "nc" / "files" / "increments.nc").write_text(
        "G970 X-5 U60 Z-60 W5\nG971 X50 Z50 I0 E0\nG0 X52 Z2\n"
        "G71 U1 R0.5 X0.5 Z0.5 F450 P10 Q20\nN10 G0 U-22 W-2\nG1 W-15\n"
        "N20 G1 U20\nG80\n", encoding="utf-8")
    dumps = {}
    for name in ("absolute", "increments"):
        (uw_root / "nc_state.txt").write_text(
            f"MODE=EDIT\nEDIT=/D/nc/files/{name}.nc\n", encoding="utf-8")
        dump = OUT / f"uw-{name}.bmp"
        run = subprocess.run([str(exe), "--files", str(uw_root),
                              "--dump-bench", str(dump)],
                             capture_output=True, text=True)
        if run.returncode or not dump.exists() or dump.stat().st_size < 54:
            fail(f"FAIL the {name} spelling did not render",
                 run.stdout[-800:] or run.stderr[-800:])
        # The drawing only: the code beside it is the operator's text, and the
        # two spellings are different text by design.
        dumps[name] = bmp_region(dump, 0, 80, 400, 542)
    if dumps["absolute"] != dumps["increments"]:
        fail("FAIL the increments and the absolutes draw different parts")
    print("nc_ui: the increments and the absolutes draw the same frame")

    # The window composes the bench in one bitmap and blits it whole, and keeps
    # the strip in it until the strip changes: that is what stopped it blinking
    # while a feed or a run repainted. Both halves of the cache are checked.
    run = subprocess.run([str(exe), "--files", str(OUT / "paint-root"),
                          "--painttest"], capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "painttest: PASS" not in run.stdout:
        fail("FAIL the bench is not composed once and blitted whole",
             run.stdout[-1500:] or run.stderr[-1500:])

    run = subprocess.run([str(exe), "--keytest"], capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "keytest: PASS" not in run.stdout:
        fail("FAIL a keypad key does not decode on both edges",
             run.stdout[-1500:] or run.stderr[-1500:])

    # The contour pad - `4 G7X`, then `7`: every press writes one row of the
    # profile and the pad stays up until `5`. The check reads the program back
    # off the card after each press, and its own fixture is written by the test.
    root = OUT / "contour-root"
    run = subprocess.run([str(exe), "--files", str(root), "--contourtest"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "contourtest: PASS" not in run.stdout:
        fail("FAIL the contour pad does not write the profile it walks",
             run.stdout[-1500:] or run.stderr[-1500:])

    # nc2's first start: the module that replaces nc writes the entries it ships
    # onto a card that has none, once, with the logo up - and then they are files
    # like every other, so deleting one is how an address stops being an entry.
    root = OUT / "seed-root"
    shutil.rmtree(root, ignore_errors=True)
    run = subprocess.run([str(exe), "--files", str(root), "--seedtest"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "seedtest: PASS" not in run.stdout:
        fail("FAIL nc2's first start does not seed the card",
             run.stdout[-1500:] or run.stderr[-1500:])

    # nc2's value editor: the fields a line cuts into, what a keystroke does to
    # the picked one, and the line the pad's name stands on.
    run = subprocess.run([str(exe), "--edit2test"], capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "edit2test: PASS" not in run.stdout:
        fail("FAIL nc2's value editor does not walk the fields",
             run.stdout[-1500:] or run.stderr[-1500:])

    # nc2's pad: the digits walked are the address, the file with that name is
    # the slot, and pressing one writes where the pad's name stood.
    root = OUT / "pad2-root"
    shutil.rmtree(root, ignore_errors=True)
    run = subprocess.run([str(exe), "--files", str(root), "--pad2test"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "pad2test: PASS" not in run.stdout:
        fail("FAIL nc2's pad does not answer with the file tree",
             run.stdout[-1500:] or run.stderr[-1500:])

    # nc2's screen: the program down the left and the pad's corner on the right,
    # with the pad walking the files and the program keeping what it wrote.
    root = OUT / "screen2-root"
    shutil.rmtree(root, ignore_errors=True)
    run = subprocess.run([str(exe), "--files", str(root), "--screen2test"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "screen2test: PASS" not in run.stdout:
        fail("FAIL nc2's screen does not draw or write the program",
             run.stdout[-1500:] or run.stderr[-1500:])

    # nc2's file list: `0` opens the card, and the picker walks, opens, makes and
    # deletes - with the folders in it and no pad in the way.
    root = OUT / "file2-root"
    shutil.rmtree(root, ignore_errors=True)
    run = subprocess.run([str(exe), "--files", str(root), "--file2test"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "file2test: PASS" not in run.stdout:
        fail("FAIL nc2's file list does not walk, open, make or delete",
             run.stdout[-1500:] or run.stderr[-1500:])

    # nc2's sender against nc's: the same program has to leave as the same lines,
    # from the top and from a run that starts in the middle, with the increments
    # written out as the absolutes they mean.
    root = OUT / "emit2-root"
    shutil.rmtree(root, ignore_errors=True)
    run = subprocess.run([str(exe), "--files", str(root), "--emit2test"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "emit2test: PASS" not in run.stdout:
        fail("FAIL nc2's sender does not agree with nc's",
             run.stdout[-1500:] or run.stderr[-1500:])

    # nc2's run: the panel hands the machine what the program means one unit at
    # a time, the machine arrives where the program says, and the floating DRO is
    # up only while it is busy.
    root = OUT / "run2-root"
    shutil.rmtree(root, ignore_errors=True)
    run = subprocess.run([str(exe), "--files", str(root), "--run2test"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "run2test: PASS" not in run.stdout:
        fail("FAIL nc2's run does not hand over what the program means",
             run.stdout[-1500:] or run.stderr[-1500:])

    # nc2's MANUAL: the machine panel. The digits jog, the stops are typed, the
    # spindle runs from the keys, and a jog is always the G91 pair with the G90
    # that puts the machine back.
    root = OUT / "manual2-root"
    shutil.rmtree(root, ignore_errors=True)
    run = subprocess.run([str(exe), "--files", str(root), "--manual2test"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "manual2test: PASS" not in run.stdout:
        fail("FAIL nc2's MANUAL does not jog, stop or run the spindle",
             run.stdout[-1500:] or run.stderr[-1500:])

    # nc2's TOOLS: the tool table is a file the editor writes, with the shipped
    # row when the card has none, and a look at it does not lose the program.
    root = OUT / "tools2-root"
    shutil.rmtree(root, ignore_errors=True)
    run = subprocess.run([str(exe), "--files", str(root), "--tools2test"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "tools2test: PASS" not in run.stdout:
        fail("FAIL nc2's TOOLS does not edit the tool table as a file",
             run.stdout[-1500:] or run.stderr[-1500:])

    # nc2's marks, read off the glass: the bright line and the pale block around
    # it, on both code screens.
    root = OUT / "block2-root"
    shutil.rmtree(root, ignore_errors=True)
    run = subprocess.run([str(exe), "--files", str(root), "--block2test"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "block2test: PASS" not in run.stdout:
        fail("FAIL nc2 does not mark the line and its block on the glass",
             run.stdout[-1500:] or run.stderr[-1500:])

    # nc2's DRO: up only while the machine is busy, green while it runs, red for
    # a fault, with the state said once.
    root = OUT / "label2-root"
    shutil.rmtree(root, ignore_errors=True)
    run = subprocess.run([str(exe), "--files", str(root), "--label2test"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "label2test: PASS" not in run.stdout:
        fail("FAIL nc2's DRO is not the one place the state is said",
             run.stdout[-1500:] or run.stderr[-1500:])

    # nc2's pacer: one unit at a time, and the mark never ahead of the sender.
    root = OUT / "pace2-root"
    shutil.rmtree(root, ignore_errors=True)
    run = subprocess.run([str(exe), "--files", str(root), "--pace2test"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "pace2test: PASS" not in run.stdout:
        fail("FAIL nc2's sender does not wait for the machine",
             run.stdout[-1500:] or run.stderr[-1500:])

    # the demo the release carries, read the way nc2 reads it.
    root = OUT / "demo2-root"
    shutil.rmtree(root, ignore_errors=True)
    run = subprocess.run([str(exe), "--files", str(root), "--demo2test"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "demo2test: PASS" not in run.stdout:
        fail("FAIL nc2 does not read the demo it ships",
             run.stdout[-1500:] or run.stderr[-1500:])

    # The operator's own program and tool table, kept with the NC module
    # (uCNC/src/modules/nc/tests/fixtures). Copied into a scratch root because
    # the firmware writes its state next to them, then checked through the
    # module's own code: the file, the tool table, the G71 block scan, the
    # expansion RUN and the preview share, and the T word that links them.
    fixtures = SRC / "modules" / "nc" / "tests" / "fixtures"
    root = OUT / "file-root"
    shutil.rmtree(root, ignore_errors=True)
    (root / "nc" / "files").mkdir(parents=True, exist_ok=True)
    for name in ("facing.nc", "tool.t"):
        shutil.copy(fixtures / name, root / "nc" / "files" / name)

    run = subprocess.run([str(exe), "--files", str(root), "--filetest"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "filetest: PASS" not in run.stdout:
        fail("FAIL the NC program and its tool table do not check out",
             run.stdout[-1500:] or run.stderr[-1500:])

    # The editor's new-file field: a path the frame dumps cannot see, because
    # the field is only drawn while the file list is up.
    run = subprocess.run([str(exe), "--files", str(root), "--newfiletest"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "newfiletest: PASS" not in run.stdout:
        fail("FAIL the new-file field does not take the typed name",
             run.stdout[-1500:] or run.stderr[-1500:])

    # The editor's typed-key paths - also invisible to a frame dump, and where
    # an extraction can hand a handler the key instead of its character.
    # It types into the program and saves it, and the new-file check above left
    # the state pointing at the file it created, so both go back first.
    (root / "nc" / "files" / "facing.nc").write_bytes(
        (fixtures / "facing.nc").read_bytes())
    (root / "nc" / "files" / "12.nc").unlink(missing_ok=True)
    (root / "nc_state.txt").write_text(
        "MODE=EDIT\nEDIT=/D/nc/files/facing.nc\n", encoding="utf-8")
    run = subprocess.run([str(exe), "--files", str(root), "--editortest"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "editortest: PASS" not in run.stdout:
        fail("FAIL typed keys do not reach the word, the helper and the field",
             run.stdout[-1500:] or run.stderr[-1500:])
    (root / "nc" / "files" / "facing.nc").write_bytes(
        (fixtures / "facing.nc").read_bytes())

    # A key that changes the screen has to ask for a repaint (RUN's line keys
    # did not, so the highlight only moved on the next footer key).
    root = OUT / "file-root"
    run = subprocess.run([str(exe), "--files", str(root), "--dirtytest"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "dirtytest: PASS" not in run.stdout:
        fail("FAIL a key that moves the RUN cursor does not ask for a repaint",
             run.stdout[-1500:] or run.stderr[-1500:])

    # RUN's FROM and FULL have to send the program: they only armed the run and
    # the machine never received a character.
    run = subprocess.run([str(exe), "--files", str(root), "--runtest"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "runtest: PASS" not in run.stdout:
        fail("FAIL RUN's FROM/FULL do not send the program",
             run.stdout[-1500:] or run.stderr[-1500:])

    # What RUN marks on the pane: the line the sender is on is bright and the
    # cycle block it belongs to is pale, and both go away when the mark leaves
    # the block. Read off the drawn frame - the snapshot can carry the mark
    # while the pane paints every row flat.
    run = subprocess.run([str(exe), "--files", str(root), "--blocktest"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "blocktest: PASS" not in run.stdout:
        fail("FAIL RUN does not mark the line and the block it belongs to",
             run.stdout[-1500:] or run.stderr[-1500:])

    # The sender is paced: one unit at a time, waiting for the machine. That is
    # what keeps the mark on the code that is cutting instead of on the line the
    # reader has already swallowed.
    run = subprocess.run([str(exe), "--files", str(root), "--pacetest"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "pacetest: PASS" not in run.stdout:
        fail("FAIL RUN does not pace the program to the machine",
             run.stdout[-1500:] or run.stderr[-1500:])

    # The MANUAL stops: typed on the pad, taken from the setup with `D`, and
    # respected by a step and a feed on both sides.
    root = OUT / "file-root"
    run = subprocess.run([str(exe), "--files", str(root), "--stoptest"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "stoptest: PASS" not in run.stdout:
        fail("FAIL the MANUAL stops are not typed, taken and respected",
             run.stdout[-1500:] or run.stderr[-1500:])

    # And the same file on screen. The remembered state points EDIT at it, which
    # is also how the machine reopens the last program after a reboot.
    (root / "nc_state.txt").write_text(
        "MODE=EDIT\nEDIT=/D/nc/files/facing.nc\n", encoding="utf-8")

    edit = OUT / "facing-edit.bmp"
    run = subprocess.run([str(exe), "--files", str(root),
                          "--dump-bench", str(edit)],
                         capture_output=True, text=True)
    if run.returncode or not edit.exists() or edit.stat().st_size < 54:
        print(run.stdout[-2000:])
        fail("FAIL the program did not render in EDIT",
             run.stdout[-1500:] or run.stderr[-1500:])

    view = OUT / "facing-view.bmp"
    run = subprocess.run([str(exe), "--files", str(root), "--keys", "#",
                          "--dump-bench", str(view)],
                         capture_output=True, text=True)
    if run.returncode or not view.exists() or view.stat().st_size < 54:
        print(run.stdout[-2000:])
        fail("FAIL the program did not render on the panel",
             run.stdout[-1500:] or run.stderr[-1500:])
    print(f"nc_ui: {fixtures / 'facing.nc'} rendered in EDIT {edit} "
          f"and in the full-screen view {view}")

    # `W` is the pad's `#` on a PC keyboard, and on EDIT that key is VIEW: the
    # two frames have to be the same picture, down to the pixel (the panel and
    # the strip both), or the key the operator presses is not the key the
    # machine gets.
    view_w = OUT / "facing-view-w.bmp"
    run = subprocess.run([str(exe), "--files", str(root), "--keys", "W",
                          "--dump-bench", str(view_w)],
                         capture_output=True, text=True)
    if run.returncode or not view_w.exists() or view_w.stat().st_size < 54:
        print(run.stdout[-2000:])
        fail("FAIL the PC key W did not render the view",
             run.stdout[-1500:] or run.stderr[-1500:])
    if view.read_bytes() != view_w.read_bytes():
        fail("FAIL 'W' and '#' draw different frames on EDIT (VIEW)")
    print(f"nc_ui: 'W' and '#' are the same key on EDIT: {view_w} is identical")

    # The card root as the list shows it: the text files sit beside the programs
    # (the preset file included), which is what `0` opens on the machine.
    listing = OUT / "root-list.bmp"
    run = subprocess.run([str(exe), "--files", str(root),
                          "--keys", "F4,0,C,4,C,4",
                          "--dump-bench", str(listing)],
                         capture_output=True, text=True)
    if run.returncode or not listing.exists() or listing.stat().st_size < 54:
        print(run.stdout[-2000:])
        fail("FAIL the file list did not render",
             run.stdout[-1500:] or run.stderr[-1500:])
    print(f"nc_ui: /D listing rendered {listing}")
