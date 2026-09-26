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
    g7x = SRC / "modules" / "g7x"
    lvds = SRC / "modules" / "lvds_renderer"
    # nc2 is the panel module now: nc is retired, and the station compiles the
    # same screen the machine runs. `nc2_module.c` is the firmware's own module
    # entry (the keypad and the LVDS boot), so it is not part of the tool - the
    # station drives the screen itself.
    nc2 = SRC / "modules" / "nc2"
    files = [p for p in sorted(nc2.glob("*.c")) if p.name != "nc2_module.c"]
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

    # The card, through the firmware's own fs_* API: it lists, and the root has
    # the folders and files it should.
    run = subprocess.run([str(exe), "--files", str(OUT / "fs-root"),
                          "--fstest"], capture_output=True, text=True)
    check(run, "nc_ui: fs root", "the card does not list through fs_*")

    # The panel's one-shot blocks on the reader the run also uses.
    run = subprocess.run([str(exe), "--files", str(OUT / "stream-root"),
                          "--streamtest"], capture_output=True, text=True)
    check(run, "streamtest: PASS",
          "the panel's one-shot blocks do not reach the reader")

    # Every keypad key decodes on both edges.
    run = subprocess.run([str(exe), "--keytest"], capture_output=True, text=True)
    check(run, "keytest: PASS", "a keypad key does not decode on both edges")

    # The window composes the bench in one bitmap and blits it whole, and keeps
    # the strip in it until the strip changes: that is what stopped it blinking
    # while a feed or a run repainted.
    run = subprocess.run([str(exe), "--files", str(OUT / "paint-root"),
                          "--painttest"], capture_output=True, text=True)
    check(run, "painttest: PASS",
          "the bench is not composed once and blitted whole")

    run = subprocess.run([str(exe), "--version"], capture_output=True, text=True)
    check(run, "nc_ui: uCNC programming station (PC), built",
          "the station does not say which build it is")

    # The entries ship as files, one per address, and a fresh card is seeded with
    # them. They are generated from the module's own table by `--dump-presets`,
    # so the shipped folder and the first start cannot drift: this regenerates
    # the folder and compares it, byte for byte.
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

    # nc2's own checks, each with the card it needs. They are the module's
    # (`tools/test_nc2.py` runs the same list on its own), and the station runs
    # them with the window's own build so a release is never a build that has not
    # run its tests.
    for flag, marker, message in (
        ("--seedtest", "seedtest: PASS",
         "nc2's first start does not seed the card"),
        ("--edit2test", "edit2test: PASS",
         "nc2's value editor does not walk the fields"),
        ("--pad2test", "pad2test: PASS",
         "nc2's pad does not answer with the file tree"),
        ("--screen2test", "screen2test: PASS",
         "nc2's screen does not draw or write the program"),
        ("--file2test", "file2test: PASS",
         "nc2's file list does not walk, open, make or delete"),
        ("--emit2test", "emit2test: PASS",
         "nc2's sender does not make what nc made"),
        ("--run2test", "run2test: PASS",
         "nc2's run does not hand over what the program means"),
        ("--manual2test", "manual2test: PASS",
         "nc2's MANUAL does not jog, stop or run the spindle"),
        ("--tools2test", "tools2test: PASS",
         "nc2's TOOLS does not edit the tool table as a file"),
        ("--block2test", "block2test: PASS",
         "nc2 does not mark the line and its block on the glass"),
        ("--label2test", "label2test: PASS",
         "nc2's DRO is not the one place the state is said"),
        ("--pace2test", "pace2test: PASS",
         "nc2's sender does not wait for the machine"),
        ("--demo2test", "demo2test: PASS",
         "nc2 does not read the demo it ships"),
    ):
        root = OUT / (flag.lstrip("-") + "-root")
        shutil.rmtree(root, ignore_errors=True)
        run = subprocess.run([str(exe), "--files", str(root), flag],
                             capture_output=True, text=True)
        check(run, marker, message)

    # The demo card the release ships, and the seed it comes from: a fresh card
    # gets the program and the table beside it exactly once.
    root = OUT / "demo2-root"
    for name in ("lathe-demo.nc", "tool.t"):
        if not (root / "nc" / "files" / name).exists():
            fail(f"FAIL the demo card has no {name}")
    print("nc_ui: the demo card is seeded as the release ships it")

    # Fanuc's increments, `U` and `W`: the same profile written absolutely and
    # written with increments has to be the same picture, because the drawing
    # reads the increments through the one rule the sender uses.
    uw_root = OUT / "uw-root"
    shutil.rmtree(uw_root, ignore_errors=True)
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
        # two spellings are different text by design. nc2's preview is the right
        # pane - nc's own split, in `nc2_layout.h`.
        dumps[name] = bmp_region(dump, 374, 26, 790, 596)
    if dumps["absolute"] != dumps["increments"]:
        fail("FAIL the increments and the absolutes draw different parts")
    print("nc_ui: the increments and the absolutes draw the same frame")

    # The operator's own program, on the glass: the real job the module's
    # fixtures hold, rendered in EDIT and in the run.
    fixtures = SRC / "modules" / "nc" / "tests" / "fixtures"
    root = OUT / "file-root"
    shutil.rmtree(root, ignore_errors=True)
    (root / "nc" / "files").mkdir(parents=True, exist_ok=True)
    for name in ("facing.nc", "tool.t"):
        shutil.copy(fixtures / name, root / "nc" / "files" / name)
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

    run_view = OUT / "facing-run.bmp"
    run = subprocess.run([str(exe), "--files", str(root), "--keys", "F4",
                          "--dump-bench", str(run_view)],
                         capture_output=True, text=True)
    if run.returncode or not run_view.exists() or run_view.stat().st_size < 54:
        print(run.stdout[-2000:])
        fail("FAIL the program did not render on the run screen",
             run.stdout[-1500:] or run.stderr[-1500:])
    print(f"nc_ui: {fixtures / 'facing.nc'} rendered in EDIT {edit} "
          f"and in RUN {run_view}")

    # `W` is the pad's `#` on a PC keyboard: opening the card's list and pressing
    # the finish key has to open the file under the selection, and the picture
    # `W` draws has to be the one `#` draws, down to the pixel (the panel and the
    # strip both). A key the shell dropped would leave the list up instead.
    frames = {}
    for key, name in (("", "list"), ("#", "hash"), ("W", "w")):
        keys = "0" + ("," + key if key else "")
        out = OUT / f"pc-key-{name}.bmp"
        run = subprocess.run([str(exe), "--files", str(root), "--keys", keys,
                              "--dump-bench", str(out)],
                             capture_output=True, text=True)
        if run.returncode or not out.exists() or out.stat().st_size < 54:
            print(run.stdout[-2000:])
            fail(f"FAIL the '{keys}' frame did not render",
                 run.stderr[-1500:])
        frames[name] = out.read_bytes()
    if frames["hash"] == frames["list"]:
        fail("FAIL `#` did not open the file under the selection")
    if frames["hash"] != frames["w"]:
        fail("FAIL 'W' and '#' draw different frames")
    print("nc_ui: 'W' is the pad's '#' - the same key, the same frame")

    # The card root as the list shows it: the folders and the text files sit
    # beside the programs, which is what `0` opens on the machine.
    listing = OUT / "root-list.bmp"
    run = subprocess.run([str(exe), "--files", str(root),
                          "--keys", "0", "--dump-bench", str(listing)],
                         capture_output=True, text=True)
    if run.returncode or not listing.exists() or listing.stat().st_size < 54:
        print(run.stdout[-2000:])
        fail("FAIL the file list did not render",
             run.stdout[-1500:] or run.stderr[-1500:])
    print(f"nc_ui: /D listing rendered {listing}")
