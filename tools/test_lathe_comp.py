"""Build and run lathe_comp's own host checks.

`lathe_comp` is the geometry of cutter radius compensation
(`docs/lathe-cutter-comp.md`); nothing in the tree calls it yet, so there is no
station or firmware check for it - this is the module's whole check, and it is
the one place its numbers are pinned.
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "uCNC" / "src" / "modules" / "lathe_comp"
OUT = ROOT / "tmp" / "lathe-comp-tests"


def compiler() -> str:
    """A host C compiler: `CC`, then `gcc` on PATH, then the MinGW trees this
    machine keeps outside the repository (the same rule the ncc station uses)."""
    import shutil

    found = os.environ.get("CC")
    if found and Path(found).exists():
        path = Path(found)
    else:
        on_path = shutil.which("gcc")
        if on_path:
            path = Path(on_path)
        else:
            for guess in (Path(r"C:\acc\x86_64-11.2.0-release-posix-seh-rt_v9-rev3")
                          / "mingw64" / "bin" / "gcc.exe",
                          Path(r"C:\acc\openconnect\msys64\mingw64\bin")
                          / "gcc.exe"):
                if guess.exists():
                    path = guess
                    break
            else:
                sys.exit("lathe_comp: no host C compiler: put gcc on PATH or set CC")
    os.environ["PATH"] = f"{path.parent}{os.pathsep}{os.environ['PATH']}"
    return str(path)


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / "lathe_comp_test.exe"
    cmd = [compiler(), "-std=gnu11", "-Wall", "-Wextra", "-O1", "-w",
           f"-I{MODULE}", str(MODULE / "lathe_comp.c"),
           str(MODULE / "tests" / "lathe_comp_test.c"), "-o", str(exe), "-lm"]
    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.returncode:
        print(build.stdout[-2000:])
        print(build.stderr[-2000:])
        print("lathe_comp: the checks did not build")
        return 1
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode or "ALL PASS" not in run.stdout:
        print("lathe_comp: FAILED")
        return 1
    print("lathe_comp: the offset geometry holds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
