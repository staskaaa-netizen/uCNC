"""Build nc2 and run its own checks, on their own.

`nc2` is the module that replaces `nc`; this is its standalone target (AGENTS.md
8), so the module can be built and checked without the station's window, without
`nc`'s screens and without the release's packing. The build itself is the
station's - one list of sources, one owner - and the checks are the module's own
flags, each of which sets up the card it needs:

    --seedtest     a card with no entries gets the shipped ones, once, with the logo
    --edit2test    the fields a line cuts into, and what a keystroke does to one
    --pad2test     the pad is the file tree: address, slot, and the press that writes
    --screen2test  the program pane, the pad's corner and the program it writes
    --file2test    `0` opens the card, and the picker walks, opens, makes, deletes
    --emit2test    the sender against nc's, line for line, from the top and mid-file
    --run2test     the run hands over what the program means, the machine arrives,
                   and the DRO floats only while it is busy
    --manual2test  the jog keys move the machine, the stops are typed, the spindle
                   runs from the keys, and the axis zeroes and touches off
    --present2test every screen hands its frame to the panel (the copy the
                   machine needs and the host cannot see in the pixels)

The station's own suite (`tools/test_nc_ui.py`) runs these with everything else;
this target is for working on the module alone.
"""
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_nc_ui as station  # noqa: E402  (the build is the station's)

CHECKS = ("--seedtest", "--edit2test", "--pad2test", "--screen2test",
          "--file2test", "--emit2test", "--run2test", "--manual2test",
          "--tools2test", "--block2test", "--label2test", "--pace2test",
          "--demo2test", "--present2test", "--pump2test", "--contour2test",
          "--case2test")


def main():
    exe = station.build()
    failures = 0
    for flag in CHECKS:
        root = station.OUT / ("nc2" + flag.lstrip("-") + "-root")
        shutil.rmtree(root, ignore_errors=True)
        run = subprocess.run([str(exe), "--files", str(root), flag],
                             capture_output=True, text=True)
        print(run.stdout.strip())
        if run.returncode or ": PASS" not in run.stdout:
            print(f"nc2: FAIL {flag}")
            failures += 1
    if failures:
        print(f"nc2: {failures} check(s) failed")
        return 1
    print("nc2: all checks pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
