"""Build and run the standalone `nc_sender` host test.

The test links the sender, G7x and lathe compensation without any NC panel,
document model or framebuffer. It therefore proves the module's public boundary:
generic program lines in, controller blocks and source-line metadata out.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UCNC = ROOT / "uCNC"
SRC = UCNC / "src"
SENDER = SRC / "modules" / "nc_sender"
G7X = SRC / "modules" / "g7x"
LATHE_COMP = SRC / "modules" / "lathe_comp"
OUT = ROOT / "tmp" / "nc-sender-module-tests"


def compiler() -> str:
    found = os.environ.get("CC")
    if found and Path(found).exists():
        path = Path(found)
    else:
        on_path = shutil.which("gcc")
        path = Path(on_path) if on_path else None
        if path is None:
            for guess in (
                Path(r"C:\acc\x86_64-11.2.0-release-posix-seh-rt_v9-rev3")
                / "mingw64" / "bin" / "gcc.exe",
                Path(r"C:\acc\openconnect\msys64\mingw64\bin") / "gcc.exe",
            ):
                if guess.exists():
                    path = guess
                    break
    if path is None:
        sys.exit("nc_sender: no host C compiler: put gcc on PATH or set CC")
    os.environ["PATH"] = f"{path.parent}{os.pathsep}{os.environ['PATH']}"
    return str(path)


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / "nc_sender_host_test.exe"
    sources = [
        SENDER / "tests" / "nc_sender_host_test.c",
        SENDER / "nc_sender.c",
        G7X / "g7x.c",
        G7X / "g7x_blocks.c",
        G7X / "g7x_contour.c",
        G7X / "g7x_source.c",
        LATHE_COMP / "lathe_comp.c",
    ]
    cmd = [
        compiler(),
        "-std=gnu11",
        "-O1",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-DG7X_HOST_TEST",
        f"-I{SENDER}",
        f"-I{G7X}",
        f"-I{LATHE_COMP}",
        f"-I{SRC}",
        *[str(path) for path in sources],
        "-o",
        str(exe),
    ]
    build = subprocess.run(cmd, capture_output=True, text=True)
    (OUT / "build.log").write_text(build.stdout + build.stderr)
    if build.returncode:
        print(build.stderr[-6000:])
        return 1
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode:
        print(run.stderr[-3000:])
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
