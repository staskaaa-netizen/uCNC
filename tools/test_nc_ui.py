"""Build the Win32 NC panel shell, render one frame and check the preset file.

The shell runs the real NC screen code (modules/nc/nc_visual.c) against the host
LVDS backend, so the dumped BMP is the firmware layout. It also dumps the
floating 3x3 helper (EDIT, then footer slot 4) by replaying the machine's own
key path. The same binary then checks the /D/presets.txt contract (write the
compiled default when missing, use an edited file, fall back on an unparsable
one) through the firmware fs_* API, in a scratch root so the repository tree
stays clean.
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
         f"-I{SRC}", f"-I{SRC / 'modules'}",
         f"-I{SRC / 'modules' / 'g7x'}", f"-I{SRC / 'modules' / 'lvds_renderer'}",
         f"-I{TOOL}"]

# -static avoids a runtime dependency on libwinpthread-1.dll from the toolchain.
LDFLAGS = ["-mwindows", "-static", "-lgdi32", "-luser32", "-lcomdlg32"]


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
             "nc_menu.c", "nc_palette.c", "nc_presets.c", "nc_run.c", "nc_sim.c",
             "nc_state.c", "nc_text.c", "nc_tools.c", "nc_vocab.c", "nc_visual.c"]
    files = [nc / name for name in names]
    files += [g7x / "g7x.c", g7x / "g7x_contour.c", g7x / "g7x_source.c",
              SRC / "modules" / "cam_keyboard" / "cam_keyboard.c",
              SRC / "modules" / "g7_g8" / "parser_g7_g8.c"]
    files += [lvds / "lvds_draw_api.c", lvds / "lvds_palette.c",
              lvds / "lvds_font_cond_6x8.c", lvds / "lvds_font_ibm_8x14.c"]
    return files


EXE = OUT / "nc_ui.exe"


def build():
    """Build the shell. Returns the exe path, exits the process on failure."""
    sources = [TOOL / "lvds_host.c", TOOL / "host_fs.c", TOOL / "host_shim.c",
               TOOL / "main.c",
               *core_sources(),
               *module_sources()]
    cmd = [os.environ.get("CC", "gcc"), *FLAGS, *[str(p) for p in sources],
           "-o", str(EXE), *LDFLAGS]
    result = subprocess.run(cmd, capture_output=True, text=True)
    (OUT / "build.log").write_text(result.stdout + result.stderr)
    if result.returncode:
        print(result.stderr[-8000:])
        sys.exit(1)
    return EXE


if __name__ == "__main__":
    exe = build()

    frame = OUT / "frame.bmp"
    run = subprocess.run([str(exe), "--dump", str(frame)], capture_output=True,
                         text=True)
    if run.returncode:
        print(run.stdout[-4000:])
        print(run.stderr[-4000:])
        sys.exit(1)
    if not frame.exists() or frame.stat().st_size < 54:
        print("FAIL no frame written")
        sys.exit(1)
    print(f"nc_ui: built and rendered {frame}")

    modal = OUT / "modal.bmp"
    run = subprocess.run([str(exe), "--files", str(OUT / "modal-root"),
                          "--keys", "F2,4", "--dump", str(modal)],
                         capture_output=True, text=True)
    if run.returncode or not modal.exists() or modal.stat().st_size < 54:
        print(run.stdout[-2000:])
        print(run.stderr[-4000:])
        print("FAIL the 3x3 helper did not render")
        sys.exit(1)
    print(f"nc_ui: 3x3 helper rendered {modal}")

    root = OUT / "presets-root"
    shutil.rmtree(root, ignore_errors=True)
    run = subprocess.run([str(exe), "--files", str(root), "--presettest"],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "presettest: OK" not in run.stdout:
        print(run.stderr[-4000:])
        print("FAIL preset file contract")
        sys.exit(1)
    if not (root / "presets.txt").exists():
        print(f"FAIL {root / 'presets.txt'} was not created")
        sys.exit(1)

    run = subprocess.run([str(exe), "--files", str(OUT / "stream-root"),
                          "--streamtest"], capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "streamtest: PASS" not in run.stdout:
        print(run.stderr[-4000:])
        print("FAIL the panel's one-shot blocks did not reach the reader")
        sys.exit(1)

    run = subprocess.run([str(exe), "--files", str(OUT / "pad-root"),
                          "--padtest"], capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "padtest: PASS" not in run.stdout:
        print(run.stderr[-4000:])
        print("FAIL the keypad does not match the machine's key row")
        sys.exit(1)

    run = subprocess.run([str(exe), "--files", str(OUT / "feed-root"),
                          "--feedtest"], capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "feedtest: PASS" not in run.stdout:
        print(run.stderr[-4000:])
        print("FAIL a held key does not feed to the stop")
        sys.exit(1)

    run = subprocess.run([str(exe), "--keytest"], capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "keytest: PASS" not in run.stdout:
        print(run.stderr[-4000:])
        print("FAIL a keypad key does not decode on both edges")
        sys.exit(1)

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
        print(run.stderr[-4000:])
        print("FAIL the NC program and its tool table do not check out")
        sys.exit(1)

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
        print(run.stderr[-4000:])
        print("FAIL the program did not render in EDIT")
        sys.exit(1)

    view = OUT / "facing-view.bmp"
    run = subprocess.run([str(exe), "--files", str(root), "--keys", "#",
                          "--dump-bench", str(view)],
                         capture_output=True, text=True)
    if run.returncode or not view.exists() or view.stat().st_size < 54:
        print(run.stdout[-2000:])
        print(run.stderr[-4000:])
        print("FAIL the program did not render on the panel")
        sys.exit(1)
    print(f"nc_ui: {fixtures / 'facing.nc'} rendered in EDIT {edit} "
          f"and in the full-screen view {view}")

    # The card root as the list shows it: the text files sit beside the programs
    # (the preset file included), which is what `0` opens on the machine.
    listing = OUT / "root-list.bmp"
    run = subprocess.run([str(exe), "--files", str(root),
                          "--keys", "F4,0,C,4,C,4",
                          "--dump-bench", str(listing)],
                         capture_output=True, text=True)
    if run.returncode or not listing.exists() or listing.stat().st_size < 54:
        print(run.stdout[-2000:])
        print(run.stderr[-4000:])
        print("FAIL the file list did not render")
        sys.exit(1)
    print(f"nc_ui: /D listing rendered {listing}")
