#!/usr/bin/env python3
"""Fetch and verify Twemoji Mozilla, ysp's default emoji font, for a pack.

    uv run --no-project python tools/fetch_twemoji.py              # into examples/pack/emoji/fonts/
    uv run --no-project python tools/fetch_twemoji.py --out DIR    # into DIR
    uv run --no-project python tools/fetch_twemoji.py --check      # verify what is there; fetch nothing

Twemoji Mozilla v0.7.0 is a COLR version 0 font (glyf outlines, 3,689 color
glyphs, no gradients), the default emoji font of the layout library and the
pack tool (docs/layout.md, Color glyphs). Its art is CC BY 4.0 (Twitter,
Inc and other contributors), its build code Apache-2.0 (Mozilla): a pack
that embeds it must carry the attribution (examples/pack/emoji/
ATTRIBUTION.txt is one) and the license file this script fetches beside
the font. The repository does not hold the font: CC BY 4.0 allows it, but
the repository keeps third-party binaries out (third_party/README.md).

Each file is compared byte for byte with the SHA-256 value here; a file
that differs is not written. Standard library only.
"""
import argparse
import hashlib
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TAG = "v0.7.0"   # mozilla/twemoji-colr, commit c0460fd
FILES = {
    "Twemoji.Mozilla.ttf": ("https://github.com/mozilla/twemoji-colr/releases/download/%s/Twemoji.Mozilla.ttf" % TAG,
                            "6d90152ee0d29e82fe2a87793af5aa4b7ad13e6538360889e141e81ed299ee8e"),
    "LICENSE.md": ("https://raw.githubusercontent.com/mozilla/twemoji-colr/%s/LICENSE.md" % TAG,
                   "64419edc28e9163204c3be73f835a8dfc34cd6c9b8f7d067bb685f119f839a99"),
}


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(ROOT, "examples", "pack", "emoji", "fonts"))
    ap.add_argument("--check", action="store_true", help="verify the files in --out; fetch nothing")
    a = ap.parse_args()
    bad = 0
    os.makedirs(a.out, exist_ok=True)
    for name, (url, want) in FILES.items():
        path = os.path.join(a.out, name)
        if os.path.exists(path):
            with open(path, "rb") as f:
                got = sha256(f.read())
            if got == want:
                print("ok: %s" % path)
                continue
            if a.check:
                print("differs: %s (sha256 %s, want %s)" % (path, got, want), file=sys.stderr)
                bad += 1
                continue
        elif a.check:
            print("missing: %s" % path, file=sys.stderr)
            bad += 1
            continue
        req = urllib.request.Request(url, headers={"User-Agent": "ysp-fetch"})
        with urllib.request.urlopen(req, timeout=120) as r:
            data = r.read()
        got = sha256(data)
        if got != want:
            print("refused: %s gave sha256 %s, want %s; nothing written" % (url, got, want), file=sys.stderr)
            bad += 1
            continue
        tmp = path + ".part"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
        print("fetched: %s (%d bytes)" % (path, len(data)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
