#!/usr/bin/env python3
"""Recreate or verify third_party/lodepng/: the pack tool's PNG decoder.

    uv run --no-project python tools/vendor_pack.py            # verify (default)
    uv run --no-project python tools/vendor_pack.py --write    # fetch into third_party/lodepng/
    uv run --no-project python tools/vendor_pack.py --write --cache DIR   # keep the files between runs (CI)

The files are downloaded from raw.githubusercontent.com at the full commit
SHA below and compared byte for byte with the SHA-256 values here; nothing
else is accepted. The pack tool (CMake option YSP_BUILD_PACK) needs them; the
layout library (YSP_BUILD_LAYOUT) does not. This is the only fetch the core
pack tool needs (tables, WAV, textures, verify, list); tools/vendor_layout.py
is for glyph runs only (docs/pack.md 5.1). Standard library only. The
sibling tools/vendor_layout.py fetches the layout stack the same way.
"""
import argparse
import hashlib
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "third_party", "lodepng")
REPO = "lvandeve/lodepng"
COMMIT = "e584a8490db1b76b81bf298c11c09d3b55d9c270"   # 2026-10-06
FILES = {
    "lodepng.cpp": "6aec876971ec860527859458d3095220fc5ee1258ec37eb59e1e7dd9cf136a81",
    "lodepng.h": "9a1fca0238c38333bdc8545336961b73fd0b76957bbacf033186991be9d7cf85",
    "LICENSE": "bdafa318b8e637d600ca1b60545e894881977a16a16b74809c95e805701621e3",
}


def fetch(name, cache):
    """The file's bytes: from the cache when its hash is right, else downloaded."""
    if cache:
        cp = os.path.join(cache, COMMIT, name)
        if os.path.exists(cp):
            with open(cp, "rb") as f:
                data = f.read()
            if hashlib.sha256(data).hexdigest() == FILES[name]:
                return data
    url = "https://raw.githubusercontent.com/%s/%s/%s" % (REPO, COMMIT, name)
    with urllib.request.urlopen(url, timeout=60) as r:
        data = r.read()
    if cache and hashlib.sha256(data).hexdigest() == FILES[name]:
        os.makedirs(os.path.join(cache, COMMIT), exist_ok=True)
        with open(os.path.join(cache, COMMIT, name), "wb") as f:
            f.write(data)
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--write", action="store_true", help="download and write third_party/lodepng/")
    ap.add_argument("--cache", help="a folder that keeps the downloaded files between runs")
    a = ap.parse_args()
    bad = 0
    if a.write:
        os.makedirs(DEST, exist_ok=True)
    for name, want in sorted(FILES.items()):
        path = os.path.join(DEST, name)
        if a.write:
            data = fetch(name, a.cache)
            got = hashlib.sha256(data).hexdigest()
            if got != want:
                print("error: %s from %s@%s has SHA-256 %s, expected %s" % (name, REPO, COMMIT[:12], got, want))
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
    print("ok: lodepng %s, %d files" % (COMMIT[:12], len(FILES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
