#!/usr/bin/env python3
"""Build compare.py's example packs and run test_player_check on them.

Run with one interpreter:  python3 tests/player/check_test.py --bin DIR [--out DIR] [--time N]

The packs are the ones tests/player/compare.py builds: rdk, afc2, oddball,
visuomotor and bimanual, and stroop when a font is found (--font PATH); and
"sound", the rdk example with a sound stimulus (an audio entry).
--time N passes on: N calls of pl_check() per pack, median and max.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import compare  # noqa: E402  (its tool() and build_pack())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="the build folder (test_player_check, ypak)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--font", default=None)
    ap.add_argument("--time", default=None)
    a = ap.parse_args()
    a.bin = os.path.abspath(a.bin)
    a.out = os.path.abspath(a.out or os.path.join(a.bin, "player_test_check"))
    os.makedirs(a.out, exist_ok=True)
    if not a.font:
        for f in ("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                  "C:/Windows/Fonts/arial.ttf"):
            if os.path.exists(f):
                a.font = f
                break
    args = []
    try:
        for name in ("rdk", "afc2", "oddball", "visuomotor", "bimanual"):
            pack = os.path.join(a.out, name + ".ysppak")
            compare.build_pack(a.bin, os.path.join(HERE, name), pack)
            args.append("%s=%s" % (name, pack))
        if a.font:
            work = os.path.join(a.out, "stroop_src")
            shutil.rmtree(work, ignore_errors=True)
            shutil.copytree(os.path.join(HERE, "stroop"), work)
            os.makedirs(os.path.join(work, "fonts"), exist_ok=True)
            shutil.copyfile(a.font, os.path.join(work, "fonts", "NotoSans-Regular.ttf"))
            pack = os.path.join(a.out, "stroop.ysppak")
            compare.build_pack(a.bin, work, pack)
            args.append("stroop=%s" % pack)
        else:
            print("stroop: skipped, no font (--font PATH)")
        # the rdk example with a sound: pl_check()'s audio device
        work = os.path.join(a.out, "sound_src")
        shutil.rmtree(work, ignore_errors=True)
        shutil.copytree(os.path.join(HERE, "rdk"), work)
        samples = b"".join(struct.pack("<hh", 0, 0) for _ in range(4800))
        with open(os.path.join(work, "tone.wav"), "wb") as f:
            f.write(b"RIFF" + struct.pack("<I", 36 + len(samples)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, 48000, 192000, 4, 16) +
                    b"data" + struct.pack("<I", len(samples)) + samples)
        with open(os.path.join(work, "pack.json")) as f:
            src = json.load(f)
        src["audio"] = {"rate": 48000, "channels": 2}
        src["resources"].append({"name": "tone.wav", "kind": "audio", "source": "tone.wav"})
        with open(os.path.join(work, "pack.json"), "w", newline="\n") as f:
            json.dump(src, f)
        with open(os.path.join(work, "experiment.json")) as f:
            d = json.load(f)
        d["stimuli"]["tone"] = {"kind": "sound", "audio": "tone.wav"}
        d["trials"]["rdk"]["events"].insert(0, {"on": "tone"})
        with open(os.path.join(work, "experiment.json"), "w", newline="\n") as f:
            json.dump(d, f)
        pack = os.path.join(a.out, "sound.ysppak")
        compare.build_pack(a.bin, work, pack)
        args.append("sound=%s" % pack)
        exe = compare.tool(a.bin, "test_player_check")
    except compare.Fail as e:
        print("FAILED: %s" % e, file=sys.stderr)
        return 1
    if a.time:
        args += ["--time", a.time]
    return subprocess.run([exe] + args).returncode


if __name__ == "__main__":
    sys.exit(main())
