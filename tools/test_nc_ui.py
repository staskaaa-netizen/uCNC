"""Build the Win32 NC panel shell and render one frame headlessly.

The shell runs the real NC screen code (modules/nc/nc_visual.c) against the host
LVDS backend, so the dumped BMP is the firmware layout.
"""
from pathlib import Path
import os
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
         '-DBOARDMAP="src/modules/g7x/tests/virtual_board.h"',
         f"-I{ROOT / 'uCNC'}", f"-I{SRC / 'modules' / 'nc'}",
         f"-I{SRC}",
         f"-I{SRC / 'modules' / 'g7x'}", f"-I{SRC / 'modules' / 'lvds_renderer'}",
         f"-I{TOOL}"]

LDFLAGS = ["-mwindows", "-lgdi32", "-luser32", "-lcomdlg32"]


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
              SRC / "modules" / "g7_g8" / "parser_g7_g8.c"]
    files += [lvds / "lvds_draw_api.c", lvds / "lvds_palette.c",
              lvds / "lvds_font_cond_6x8.c", lvds / "lvds_font_ibm_8x14.c"]
    return files


if __name__ == "__main__":
    exe = OUT / "nc_ui.exe"
    sources = [TOOL / "lvds_host.c", TOOL / "host_shim.c", TOOL / "main.c",
               *core_sources(),
               *module_sources()]
    cmd = [os.environ.get("CC", "gcc"), *FLAGS, *[str(p) for p in sources],
           "-o", str(exe), *LDFLAGS]
    result = subprocess.run(cmd, capture_output=True, text=True)
    (OUT / "build.log").write_text(result.stdout + result.stderr)
    if result.returncode:
        print(result.stderr[-8000:])
        sys.exit(1)

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
