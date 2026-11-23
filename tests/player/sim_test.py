#!/usr/bin/env python3
"""Build check_test.py's example packs and run test_player_sim on them.

Run with one interpreter:  python3 tests/player/sim_test.py --bin DIR [--out DIR] [--keep]

The packs are test_player_check's: rdk, afc2, oddball, visuomotor,
bimanual, stroop when a font is found (--font PATH), and "sound"; with
stroop, align_packs.py's stroop_start and stroop_old.
--keep: the data sets stay under OUT/sim (test_player_sim --keep).
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_test  # noqa: E402  (its build_packs())
import compare  # noqa: E402  (its tool())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="the build folder (test_player_sim, ypak)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--font", default=None)
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    a.bin = os.path.abspath(a.bin)
    a.out = os.path.abspath(a.out or os.path.join(a.bin, "player_test_sim"))
    os.makedirs(a.out, exist_ok=True)
    if not a.font:
        for f in ("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                  "C:/Windows/Fonts/arial.ttf"):
            if os.path.exists(f):
                a.font = f
                break
    try:
        args = check_test.build_packs(a)
        exe = compare.tool(a.bin, "test_player_sim")
        stroop = [x.split("=", 1)[1] for x in args if x.startswith("stroop=")]
        if stroop:
            # the align case's packs (align_packs.py): explicit start, and
            # the pack an older ypak built from stroop
            r = subprocess.run([sys.executable, os.path.join(HERE, "align_packs.py"), compare.tool(a.bin, "ypak"),
                                os.path.join(a.out, "stroop_src"), stroop[0], a.out], capture_output=True, text=True)
            if r.returncode != 0:
                raise compare.Fail("align_packs.py: %s" % r.stderr)
            args += r.stdout.split()
    except compare.Fail as e:
        print("FAILED: %s" % e, file=sys.stderr)
        return 1
    if a.keep:
        keep = os.path.join(a.out, "sim")
        shutil.rmtree(keep, ignore_errors=True)
        args += ["--keep", keep]
    return subprocess.run([exe] + args).returncode


if __name__ == "__main__":
    sys.exit(main())
