#!/usr/bin/env python3
"""Recreate or verify the pack tool's third-party files in third_party/.

    uv run --no-project python tools/vendor_pack.py            # verify (default)
    uv run --no-project python tools/vendor_pack.py --write    # fetch into third_party/
    uv run --no-project python tools/vendor_pack.py --write --cache DIR   # keep the files between runs (CI)

Three sets, each at a full commit SHA, each file compared byte for byte with
the SHA-256 value here; nothing else is accepted:

    third_party/lodepng/   PNG decoder and inflate (zlib license): textures,
                           and the deflated parts of .xlsx files
    third_party/stb/       stb_image.h (public domain or MIT): JPEG textures
    third_party/pl_mpeg/   pl_mpeg.h (MIT): MPEG-1 video indexes, the same
                           pin as the top-level YSP_FETCH_PL_MPEG
    third_party/libsamplerate/  libsamplerate 0.2.2 (BSD-2-Clause): the
                           opt-in resampling of audio to the project rate

The pack tool (CMake option YSP_BUILD_PACK) needs them; the layout library
(YSP_BUILD_LAYOUT) does not. This is the only fetch the core pack tool needs
(tables, WAV, textures, video, verify, list); tools/vendor_layout.py is for
glyph runs only (docs/pack.md 5.1). Standard library only.
"""
import argparse
import hashlib
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SETS = [
    {
        "dest": "lodepng",
        "repo": "lvandeve/lodepng",
        "commit": "e584a8490db1b76b81bf298c11c09d3b55d9c270",   # 2026-10-06
        "files": {
            "lodepng.cpp": "6aec876971ec860527859458d3095220fc5ee1258ec37eb59e1e7dd9cf136a81",
            "lodepng.h": "9a1fca0238c38333bdc8545336961b73fd0b76957bbacf033186991be9d7cf85",
            "LICENSE": "bdafa318b8e637d600ca1b60545e894881977a16a16b74809c95e805701621e3",
        },
    },
    {
        "dest": "stb",
        "repo": "nothings/stb",
        "commit": "2c980bb59875b0d32144a71867fbdebb2f77cd20",   # 2026-08-02; stb_image.h 2.30
        "files": {
            "stb_image.h": "594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3",
            "LICENSE": "bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63",
        },
    },
    {
        "dest": "pl_mpeg",
        "repo": "phoboslab/pl_mpeg",
        "commit": "c871f2be022ece7ef4f64230b4fb8e1fb9eb6023",   # CMakeLists.txt's YSP_FETCH_PL_MPEG pin
        "files": {
            "pl_mpeg.h": "3a8cb30c83c2a1147719c30fe0c8b93da2987aa43140077c575b39aaa75fc2c9",
        },
    },
    {
        "dest": "libsamplerate",
        "repo": "libsndfile/libsamplerate",
        "commit": "c96f5e3de9c4488f4e6c97f59f5245f22fda22f7",   # tag 0.2.2
        "files": {
            "src/samplerate.c": "ecc11b6bec5bd8131a3dd510a7017a5b7b503573d9b49b7e02ee5c3c86568717",
            "src/src_sinc.c": "8e59137d758dae5027410af7f81010e7a9c33892070091866510f0f8b638237e",
            "src/src_linear.c": "f04a3e79b7150f2f6e6ad2b05979d874e03bf567a682bbb6507c797d4ea8a972",
            "src/src_zoh.c": "c089678c9e6463896507a8d476fc4a33641c46fbe94aa8243acf725d4c203adb",
            "src/common.h": "0826d52807152f5577ecb519c3df46f4453a9b7e55ad9b2880ff82a95977c3ca",
            "src/fastest_coeffs.h": "60b342165e7be7c58efd9002163c07b65ece52994ee839dbe06908991ef1fb1b",
            "src/mid_qual_coeffs.h": "22b2b75ef0344f60787f19c3bfd594d47303c0fafe7e13d0e7517612b8963d7d",
            "src/high_qual_coeffs.h": "35d4bc6ed93cbd5a89510d7d7efda00116b5c28a25369c07288bc7d25a937df6",
            "include/samplerate.h": "dfe5f4107782bcf6e9b8421a9e7c4772e19446a4c18727c73b22286d3fbb6626",
            "COPYING": "2c1f76ce2effdddb425018405d5690c0b1ab4e6976e35296b0a6db65c5e1a55d",
        },
    },
]


def fetch(s, name, cache):
    """The file's bytes: from the cache when its hash is right, else downloaded."""
    want = s["files"][name]
    if cache:
        cp = os.path.join(cache, s["commit"], os.path.basename(name))
        if os.path.exists(cp):
            with open(cp, "rb") as f:
                data = f.read()
            if hashlib.sha256(data).hexdigest() == want:
                return data
    url = "https://raw.githubusercontent.com/%s/%s/%s" % (s["repo"], s["commit"], name)
    with urllib.request.urlopen(url, timeout=60) as r:
        data = r.read()
    if cache and hashlib.sha256(data).hexdigest() == want:
        os.makedirs(os.path.join(cache, s["commit"]), exist_ok=True)
        with open(os.path.join(cache, s["commit"], os.path.basename(name)), "wb") as f:
            f.write(data)
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--write", action="store_true", help="download and write the files into third_party/")
    ap.add_argument("--cache", help="a folder that keeps the downloaded files between runs")
    a = ap.parse_args()
    bad = 0
    for s in SETS:
        dest = os.path.join(ROOT, "third_party", s["dest"])
        if a.write:
            os.makedirs(dest, exist_ok=True)
        for name, want in sorted(s["files"].items()):
            path = os.path.join(dest, os.path.basename(name))   # flat: the repository path is only the URL
            if a.write:
                data = fetch(s, name, a.cache)
                got = hashlib.sha256(data).hexdigest()
                if got != want:
                    print("error: %s from %s@%s has SHA-256 %s, expected %s" % (name, s["repo"], s["commit"][:12], got, want))
                    bad += 1
                    continue
                with open(path, "wb") as f:
                    f.write(data)
            if not os.path.exists(path):
                print("missing: %s (run with --write)" % os.path.relpath(path, ROOT))
                bad += 1
                continue
            with open(path, "rb") as f:
                got = hashlib.sha256(f.read()).hexdigest()
            if got != want:
                print("differs: %s has SHA-256 %s, expected %s" % (os.path.relpath(path, ROOT), got, want))
                bad += 1
    if bad:
        return 1
    print("ok: " + ", ".join("%s %s (%d files)" % (s["dest"], s["commit"][:12], len(s["files"])) for s in SETS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
