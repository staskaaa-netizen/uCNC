"""Build and run the nc_send host tests and CLI smoke checks.

Uses MinGW GCC from PATH (or CC). No serial port and no hardware are needed:
the tests drive a scripted fake controller.
"""
from pathlib import Path
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "nc_sender"
OUT = ROOT / "tmp" / "nc-sender-tests"
OUT.mkdir(parents=True, exist_ok=True)

NC = ROOT / "uCNC" / "src" / "modules" / "nc"
G7X = ROOT / "uCNC" / "src" / "modules" / "g7x"
UCNC = ROOT / "uCNC"

MODULES = [NC / "nc.c", NC / "nc_emit.c", NC / "nc_g7x.c",
           G7X / "g7x.c", G7X / "g7x_contour.c", G7X / "g7x_source.c"]
SENDER = [TOOL / "nc_sender.c", TOOL / "grbl_stream.c",
          TOOL / "grbl_port_win32.c", TOOL / "nc_send_job.c"]
FLAGS = ["-std=gnu11", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter",
         "-DG7X_HOST_TEST", "-DNC_HOST_TEST",
         "-DNC_MAX_LINES=4096", "-DNC_MAX_LINE_LEN=128",
         f"-I{TOOL}", f"-I{NC}", f"-I{G7X}", f"-I{UCNC}"]


def build(name, main_sources):
    exe = OUT / (name + ".exe")
    cmd = [os.environ.get("CC", "gcc"), *FLAGS, *[str(p) for p in main_sources],
           "-o", str(exe)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    (OUT / (name + ".build.log")).write_text(result.stdout + result.stderr)
    if result.returncode:
        print(result.stderr[-6000:])
        sys.exit(1)
    return exe


def run(exe, args, expect_ok=True):
    result = subprocess.run([str(exe), *args], capture_output=True, text=True,
                            cwd=TOOL)
    if expect_ok and result.returncode:
        print(f"FAIL {exe.name} {' '.join(args)}: rc={result.returncode}")
        print(result.stdout[-2000:])
        print(result.stderr[-2000:])
        sys.exit(1)
    return result


def check(condition, message):
    if not condition:
        print("FAIL " + message)
        sys.exit(1)


if __name__ == "__main__":
    test_exe = build("nc_sender_test",
                     [TOOL / "tests" / "nc_sender_host_test.c", *SENDER, *MODULES])
    result = run(test_exe, [])
    check("passed" in result.stdout, "host tests did not report success")

    cli = build("nc_send", [TOOL / "main.c", *SENDER, *MODULES])
    sample = "tests/two_line_cycle.nc"

    result = run(cli, ["--check", "--quiet", sample])
    check("lines ready" in result.stdout, "check run produced no line count")

    grbl_out = OUT / "expanded_grbl.nc"
    run(cli, ["--quiet", "--expand", str(grbl_out), sample])
    grbl_text = grbl_out.read_text()
    check("G0 X26.25" in grbl_text, "grbl target did not convert X to radius")
    check("G71" not in grbl_text and "G80" not in grbl_text,
          "grbl target left a cycle command in the output")

    ucnc_out = OUT / "expanded_ucnc.nc"
    run(cli, ["--target", "ucnc", "--quiet", "--expand", str(ucnc_out), sample])
    ucnc_text = ucnc_out.read_text()
    check("G0 X54 Z2" in ucnc_text, "ucnc target changed the program X values")

    bad = OUT / "bad_thread.nc"
    bad.write_text("G0 X40 Z2\nG33 X38 Z-20 K1.5\n")
    result = run(cli, ["--check", "--quiet", str(bad)], expect_ok=False)
    check(result.returncode == 2, "threading program was not rejected")
    check("unsupported" in result.stderr, "threading rejection has no explanation")

    print("nc_send host tests and CLI checks passed")
