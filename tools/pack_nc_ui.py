"""Build the Windows programming station and pack what a release ships.

The station is built and checked by `tools/test_nc_ui.py` (the same script the
CI runs), and what comes out is one zip:

    uCNC-programming-station.exe   the station, statically linked
    README.md                      the operator's usage
    desktop-sender.md              how the station fits the desktop tools
    examples/lathe-demo.nc         a program to open, preview and run
    examples/tool.t                the table that program calls T2 from

The `.exe` needs no DLL beside it (MinGW -static), and the `examples` folder is
read where the station runs from: a card with no program of its own is seeded
from it on the first start (`host_seed_card()`), so an unpacked zip opens with
something to look at.

    python tools/pack_nc_ui.py                # build, check, pack into dist/
    python tools/pack_nc_ui.py --skip-build   # pack what is already built

`--skip-build` refuses to pack an exe that is older than the sources it was
built from: the one thing this must never do is ship last night's station under
this morning's name. The exe's own timestamp and size are printed, and are what
`nc_ui.exe --version` and the window title report, so a station someone has can
be matched against the one in the zip.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile
import datetime

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "nc_ui_win"
EXE = ROOT / "tmp" / "nc-ui-tests" / "nc_ui.exe"
DIST = TOOL / "dist"
ZIP = DIST / "uCNC-programming-station-win64.zip"


def sources():
    """Every file the station is built from: the tool's own, and the firmware
    sources it compiles (the same set tools/test_nc_ui.py hands the compiler)."""
    files = [p for p in TOOL.iterdir() if p.suffix in (".c", ".h")]
    files.append(TOOL / "Makefile")
    for src in (ROOT / "uCNC" / "src").rglob("*"):
        if src.suffix in (".c", ".h"):
            files.append(src)
    return [p for p in files if p.exists()]


def build_time(path):
    return datetime.datetime.fromtimestamp(path.stat().st_mtime)


def main(argv):
    if "--skip-build" not in argv:
        build = subprocess.run([sys.executable, str(ROOT / "tools" / "test_nc_ui.py")])
        if build.returncode:
            print("pack: the station did not build and check out")
            return 1
    if not EXE.exists():
        print(f"pack: {EXE} is not there - build the station first")
        return 1
    newest = max(build_time(p) for p in sources())
    if build_time(EXE) < newest:
        print(f"pack: {EXE.name} is from {build_time(EXE):%Y-%m-%d %H:%M:%S}, "
              f"older than {newest:%Y-%m-%d %H:%M:%S} in the sources - build it "
              f"(drop --skip-build) instead of packing yesterday's station")
        return 1

    staging = DIST / "station"
    shutil.rmtree(staging, ignore_errors=True)
    (staging / "examples").mkdir(parents=True)
    shutil.copy2(EXE, staging / "uCNC-programming-station.exe")
    shutil.copy2(TOOL / "README.md", staging / "README.md")
    shutil.copy2(ROOT / "docs" / "desktop-sender.md", staging / "desktop-sender.md")
    examples = sorted((TOOL / "examples").iterdir())
    if not examples:
        print("pack: tools/nc_ui_win/examples is empty")
        return 1
    for example in examples:
        shutil.copy2(example, staging / "examples" / example.name)

    with zipfile.ZipFile(ZIP, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(staging.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(staging).as_posix())
    shutil.rmtree(staging, ignore_errors=True)

    with zipfile.ZipFile(ZIP) as archive:
        names = archive.namelist()
    print(f"pack: the station in it was built {build_time(EXE):%Y-%m-%d %H:%M:%S}"
          f" ({EXE.stat().st_size} bytes)")
    print(f"pack: {ZIP} ({ZIP.stat().st_size} bytes)")
    for name in names:
        print(f"pack:   {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
