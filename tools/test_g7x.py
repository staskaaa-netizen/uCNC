"""Build/run G7x tests on Windows with a MinGW GCC available on PATH.

Uses the actual uCNC virtual MCU, parser and planner. No external test framework
or hardware is required. Outputs stay under the ignored tmp/ directory.
"""
from pathlib import Path
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tmp" / "g7x-tests"
OUT.mkdir(parents=True, exist_ok=True)
os.chdir(ROOT)
gcc = os.environ.get("CC", "gcc")
module = "uCNC/src/modules/g7x"
common = [f"{module}/g7x.c", f"{module}/g7x_contour.c"]

def run(name, sources, flags):
    exe = OUT / (name + ".exe")
    cmd = [gcc, "-std=gnu11", "-O1", "-ffunction-sections", "-fdata-sections",
           "-Wl,--gc-sections", "-IuCNC", *flags, *sources, "-lm", "-o", str(exe)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    (OUT / (name + ".build.log")).write_text(result.stdout + result.stderr)
    if result.returncode:
        print(result.stderr[-10000:])
        return result.returncode
    return subprocess.run([str(exe)], timeout=120, cwd=OUT).returncode

if __name__ == "__main__":
    suite = sys.argv[1] if len(sys.argv) > 1 else "all"
    failed = 0
    if suite in ("all", "generator"):
        failed |= run("generator", common + [f"{module}/tests/g7x_host_test.c"],
                      ["-DG7X_HOST_TEST", "-Wall", "-Wextra", "-Werror"])
    if suite in ("all", "nc"):
        failed |= run("nc", common + ["uCNC/src/modules/nc/nc.c",
                      "uCNC/src/modules/nc/nc_emit.c",
                      "uCNC/src/modules/nc/tests/nc_emit_host_test.c"],
                      ["-DG7X_HOST_TEST", "-DNC_HOST_TEST"])
    if suite in ("all", "parser"):
        sources = []
        for directory in ("", "core", "interface", "hal/kinematics", "hal/tools", "hal/tools/tools", "modules"):
            sources += [str(p) for p in (Path("uCNC/src") / directory).glob("*.c")]
        sources += ["uCNC/src/hal/mcus/mcu.c",
                    "uCNC/src/hal/mcus/virtual/mcu_virtual.c",
                    "uCNC/src/hal/mcus/virtual/virtual_windows.c",
                    "uCNC/src/modules/g7_g8/parser_g7_g8.c", *common,
                    "uCNC/src/modules/nc/nc.c", "uCNC/src/modules/nc/nc_run.c",
                    f"{module}/tests/g7x_parser_test.c"]
        flags = ["-DPIO_UNIT_TESTING", "-DUCNC_IGNORE_BOARDMAP_OVERRIDES",
                 "-DUCNC_IGNORE_HAL_OVERRIDES", "-DENABLE_PARSER_MODULES",
                 "-DENABLE_MAIN_LOOP_MODULES", "-DENABLE_O_CODES", "-DENABLE_G7X_MODULE", "-DG33_ENCODER=0",
                 '-DBOARDMAP="src/modules/g7x/tests/virtual_board.h"',
                 "-DDISABLE_SAFE_SETTINGS", "-DDISABLE_ENDPROGRAM_LOCK",
                 "-DEMULATE_GRBL_STARTUP=3", "-pthread"]
        failed |= run("parser", sources, flags)
    sys.exit(bool(failed))
