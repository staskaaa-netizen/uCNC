"""Build ncc - both shapes - and run its own station and host checks.

`ncc` is the third NC module: nc2's screens, with the entries held in RAM, the
preview switched by `#`, and one layout file for both orientations. This script
is its whole software check:

  * the firmware the machine would run, built twice - portrait (the panel
    mounted turned, what the board runs) and landscape (`-DLVDS_PANEL_TURN=0`,
    the only place the second branch of `ncc_layout.h` is exercised);
  * `tools/ncc_ui_win/` - the station, built against `modules/ncc/` and
    `modules/nc_sender/` - and the module's own checks, each with the card it
    needs;
  * `uCNC/src/modules/ncc/tests/ncc_host_test.c`, which pins what the station
    cannot see from a frame: the card file's rules and the layout's arithmetic.

The station is nc2's, forked the way `tools/nc_ui_win` was for nc2; the checks
that were wired to nc2's folder-and-seed entries are rewritten here for ncc's
one-table-in-RAM model, and the rest are the same checks.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_nc_ui as station  # noqa: E402  (the host core, one owner)

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "uCNC" / "src"
MODULE = SRC / "modules" / "ncc"
NC_SENDER = SRC / "modules" / "nc_sender"
NCC_STATION = ROOT / "tools" / "ncc_ui_win"
OUT = ROOT / "tmp" / "ncc-tests"
ENV = "RP2350-LEANCAM-LVDS-NCC"
LANDSCAPE = "-DLVDS_PANEL_TURN=0"
SOURCE_LINE_LIMIT = 2000
MODULE_LINE_LIMIT = 10000

# The module's own checks, the way nc2's suite names them.
CHECKS = ("--seedtest", "--preview2test", "--gcode2test", "--chamfer2test",
          "--refuse2test",
          "--blockline2test",
          "--chuck2test", "--colour2test", "--firstline2test", "--edit2test",
          "--pad2test", "--screen2test", "--boot2test",
          "--file2test", "--emit2test", "--run2test", "--runlock2test",
          "--manual2test",
          "--live2test", "--brush2test", "--frame2test", "--glyph2test",
          "--fps2test", "--scroll2test",
          "--fault2test",
          "--tools2test", "--offset2test", "--comp2test", "--state2test",
          "--block2test", "--label2test", "--pace2test",
          "--demo2test", "--present2test", "--pump2test", "--contour2test",
          "--case2test", "--walk2test", "--vocab2test")


def compiler() -> str:
    """A host C compiler: `CC`, then `gcc` on PATH, then the MinGW trees this
    machine keeps outside the repository. The compiler's own `bin` goes on PATH,
    because a MinGW `gcc` finds `cc1`'s DLLs there and nowhere else."""
    path = None
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
    if path is None:
        sys.exit("ncc: no host C compiler: put gcc on PATH or set CC")
    os.environ["PATH"] = f"{path.parent}{os.pathsep}{os.environ['PATH']}"
    return str(path)


def pio() -> str:
    found = shutil.which("pio")
    if found:
        return found
    win = Path(os.environ.get("USERPROFILE", "")) / ".platformio" / "penv" / \
        "Scripts" / "pio.exe"
    if win.exists():
        return str(win)
    sys.exit("ncc: no PlatformIO CLI on PATH and no virtualenv to fall back on")


# --- the firmware the board runs -------------------------------------------

def build_firmware(name: str, build_dir: Path, extra: str = "") -> bool:
    env = dict(os.environ)
    env.setdefault("PLATFORMIO_CORE_DIR", r"C:\pio")
    env["PLATFORMIO_BUILD_DIR"] = str(build_dir)
    if extra:
        env["PLATFORMIO_BUILD_FLAGS"] = extra
    elif "PLATFORMIO_BUILD_FLAGS" in env:
        del env["PLATFORMIO_BUILD_FLAGS"]
    OUT.mkdir(parents=True, exist_ok=True)
    print(f"ncc: {name} firmware")
    result = subprocess.run([pio(), "run", "-e", ENV], cwd=ROOT, env=env,
                            capture_output=True, text=True)
    log = result.stdout + result.stderr
    (OUT / f"firmware-{name}.log").write_text(log)
    if result.returncode or "[SUCCESS]" not in log:
        print("\n".join(log.strip().splitlines()[-25:]))
        return False
    return True


# --- the station ------------------------------------------------------------

def check_float_printf(build_dir: Path) -> bool:
    """The machine's `snprintf` has to be able to print a float.

    Every screen says its numbers with `%.3f` - the DRO, the preview's
    dimensions, and the blocks a cycle expands to. The RP2350 core links
    newlib-nano, whose `snprintf` **drops float conversions** unless the float
    formatter is pulled in, and a dropped one is an empty string: the machine was
    handed `G0 X` where the panel meant `G0 X55.000`, and refused it with
    `error 2` (bench, 2026-09-27 - three bench rounds went into that sentence).
    The host is no guide at all: it links glibc, which formats floats whether or
    not anyone asks, so this reads the ELF the machine build itself made
    (`rp2350.ini`'s `-Wl,-u,_printf_float`)."""
    elf = build_dir / ENV / "firmware.elf"
    core = Path(os.environ.get("PLATFORMIO_CORE_DIR", r"C:\pio"))
    nm = next((p for p in sorted((core / "packages").glob(
        "*/bin/arm-none-eabi-nm.exe"))), None)
    if not elf.exists() or nm is None:
        print(f"ncc: no {nm or 'nm'} to read {elf}: the float printf was not "
              "checked")
        return True
    result = subprocess.run([str(nm), str(elf)], capture_output=True, text=True)
    if "_printf_float" not in result.stdout:
        print("ncc: FAIL the firmware has no float printf - every number the "
              "panel prints would be an empty string on the machine")
        return False
    print("ncc: the firmware links the float formatter")
    return True


def station_flags() -> list:
    """The station's flags with the panel swapped: its own `-I` for nc2's
    directory comes out, the forked station's and ncc's go in."""
    out = []
    for flag in station.FLAGS:
        probe = flag.replace("\\", "/")
        if probe.endswith("modules/nc2") or probe.endswith("nc_ui_win"):
            continue
        out.append(flag)
    out += [f"-I{NCC_STATION}", f"-I{MODULE}", f"-I{NC_SENDER}"]
    return out


def build_station() -> Path:
    exe = OUT / "ncc_ui.exe"
    OUT.mkdir(parents=True, exist_ok=True)
    app = [NCC_STATION / name for name in
           ("lvds_host.c", "host_fs.c", "host_shim.c", "host_spindle.c",
            "host_tests.c", "main.c")]
    # What the station compiles beside the panel, with nc2's sources swapped
    # for ncc's: the host owns one copy of this list (test_nc_ui.module_sources).
    mods = [p for p in station.module_sources()
            if "modules/nc2" not in str(p).replace("\\", "/")]
    mods += [p for p in sorted(MODULE.glob("*.c"))
             if p.name != "ncc_module.c"]
    mods += sorted(NC_SENDER.glob("*.c"))
    cmd = [compiler(), *station_flags(), *[str(p) for p in app],
           *[str(p) for p in station.core_sources()], *[str(p) for p in mods],
           "-o", str(exe), *station.LDFLAGS]
    result = subprocess.run(cmd, capture_output=True, text=True)
    log = result.stdout + result.stderr
    (OUT / "station-build.log").write_text(log)
    if result.returncode:
        print("\n".join(log.strip().splitlines()[-40:]))
        raise SystemExit("ncc: the station did not build")
    return exe


def run_checks(exe: Path, flags=CHECKS) -> int:
    failures = 0
    for flag in flags:
        root = OUT / ("ncc" + flag.lstrip("-") + "-root")
        shutil.rmtree(root, ignore_errors=True)
        result = subprocess.run([str(exe), "--files", str(root), flag],
                                capture_output=True, text=True)
        print(result.stdout.strip())
        if result.returncode or ": PASS" not in result.stdout:
            print(f"ncc: FAIL {flag}")
            failures += 1
    return failures


# --- what a frame cannot show ----------------------------------------------

def build_host_test(name: str, extra: str = "") -> Path:
    exe = OUT / f"ncc_host_{name}.exe"
    flags = [f for f in station.FLAGS
             if not f.replace("\\", "/").endswith("modules/nc2")]
    others = [p for p in station.module_sources()
              if not any(part in str(p).replace("\\", "/")
                         for part in ("modules/nc2", "modules/lvds_renderer"))]
    cmd = [compiler(), *flags, f"-I{MODULE}", *([extra] if extra else []),
           str(MODULE / "tests" / "ncc_host_test.c"),
           str(MODULE / "ncc_presets.c"),
           str(NC_SENDER / "nc_sender.c"),
           str(NCC_STATION / "host_fs.c"),
           str(NCC_STATION / "host_shim.c"),
           *[str(p) for p in station.core_sources()],
           *[str(p) for p in others],
           "-o", str(exe)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    log = result.stdout + result.stderr
    (OUT / f"host-{name}-build.log").write_text(log)
    if result.returncode:
        print("\n".join(log.strip().splitlines()[-30:]))
        raise SystemExit(f"ncc: the {name} host test did not build")
    return exe


def run_host_test(name: str, exe: Path) -> bool:
    root = OUT / f"card-{name}"
    shutil.rmtree(root, ignore_errors=True)
    shipped = NCC_STATION / "examples" / "ncc_presets.txt"
    result = subprocess.run([str(exe), str(root), str(shipped)],
                            capture_output=True, text=True)
    print(result.stdout.strip())
    if result.returncode or "ncc_host: PASS" not in result.stdout:
        print(f"ncc: FAIL {name} host checks")
        return False
    return True


def check_layout_branch() -> bool:
    """What shape `ncc_layout.h` picks in the module's own include order.

    The machine and the host get the panel's size from different places - the
    boardmap for the machine, the command line for the station - so a layout that
    asks the *view* its shape can answer differently in the two while every host
    frame looks right. That is exactly what kept the 3x3 keys at 64 px on the
    glass while the station drew 86. This asks the preprocessor, in the order the
    module's files really include it."""
    probe = MODULE / "tests" / "ncc_layout_probe.c"
    checked = 0
    for name, flags, pad_key in (("portrait", [], 70),
                                 ("landscape", ["-DLVDS_PANEL_TURN=0"], 64)):
        cmd = [compiler(), "-E", "-dM", *flags, f"-I{MODULE}",
               f"-I{SRC / 'modules' / 'lvds_renderer'}", str(probe)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        found = [line for line in result.stdout.splitlines()
                 if line.startswith("#define NCC_PAD_KEY")]
        if result.returncode or not found or found[0].split()[-1] != str(pad_key):
            print(f"ncc: FAIL the {name} layout is not what the include order "
                  f"gives ({found or result.stderr.strip()[:200]})")
            return False
        checked += 1
    print(f"ncc: the layout takes the right shape in both ({checked} shapes)")
    return True


def check_source_budget() -> bool:
    """Keep the panel below the repository's size trigger.

    The 2026-11 split moved framebuffer layout out of the controller. This
    check keeps that boundary from silently dissolving back into one large
    `ncc_visual.c`; generated image data is not handwritten source.
    """
    generated = {"ncc_boot_image.c"}
    checked = 0
    total = 0
    for path in sorted(MODULE.glob("*.[ch]")):
        if path.name in generated:
            continue
        lines = sum(1 for _ in path.open(encoding="utf-8"))
        total += lines
        if lines > SOURCE_LINE_LIMIT:
            print(f"ncc: FAIL {path.name} has {lines} lines "
                  f"(limit {SOURCE_LINE_LIMIT})")
            return False
        checked += 1
    if total > MODULE_LINE_LIMIT:
        print(f"ncc: FAIL the module has {total} handwritten lines "
              f"(limit {MODULE_LINE_LIMIT})")
        return False
    print(f"ncc: {checked} handwritten source files stay under "
          f"{SOURCE_LINE_LIMIT} lines; module total {total}/{MODULE_LINE_LIMIT}")
    return True


def check_shipped_entries(exe: Path) -> bool:
    """The release's readable copy of the entries is what the panel writes: the
    station dumps the table and it must be byte for byte the file in the tree."""
    shipped = NCC_STATION / "examples" / "ncc_presets.txt"
    out = OUT / "ncc_presets.txt"
    result = subprocess.run([str(exe), "--dump-presets", str(out)],
                            capture_output=True, text=True)
    print(result.stdout.strip())
    if result.returncode or not out.exists():
        print("ncc: FAIL --dump-presets did not write the entries")
        return False
    if out.read_bytes() != shipped.read_bytes():
        print(f"ncc: FAIL {shipped} is not what the panel writes - regenerate it")
        return False
    return True


def main() -> int:
    failed = 0
    selected = [arg for arg in sys.argv[1:] if arg.startswith("--")]

    # One check on its own, for the bench's own loop:
    #   python tools\test_ncc.py --brush2test
    # builds the station (the same sources the full run compiles) and runs only
    # the flags named. The firmware and the host tests are not touched, because
    # the check being iterated is the station's.
    if selected:
        unknown = [flag for flag in selected if flag not in CHECKS]
        if unknown:
            sys.exit(f"ncc: no such station check: {', '.join(unknown)}")
        if run_checks(build_station(), selected):
            return 1
        print(f"ncc: {len(selected)} station check(s) passed")
        return 0

    if not build_firmware("portrait", OUT / "build-portrait"):
        print("ncc: FAIL portrait firmware")
        failed = 1
    elif not check_float_printf(OUT / "build-portrait"):
        failed = 1
    if not build_firmware("landscape", OUT / "build-landscape", LANDSCAPE):
        print("ncc: FAIL landscape firmware")
        failed = 1

    for name, extra in (("portrait", ""), ("landscape", LANDSCAPE)):
        if not run_host_test(name, build_host_test(name, extra)):
            failed = 1

    station_exe = build_station()
    if not check_layout_branch():
        failed = 1
    if not check_source_budget():
        failed = 1
    if run_checks(station_exe):
        print("ncc: FAIL the station's checks")
        failed = 1
    if not check_shipped_entries(station_exe):
        failed = 1

    if failed:
        print("ncc: FAILED")
        return 1
    print("ncc: both shapes build, the entries and bands check out, and every panel check passes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
