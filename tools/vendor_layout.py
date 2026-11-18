#!/usr/bin/env python3
"""Recreate or verify third_party/: Skribidi and its layout dependencies.

    uv run --no-project python tools/vendor_layout.py              # verify (default)
    uv run --no-project python tools/vendor_layout.py --write      # rewrite third_party/
    uv run --no-project python tools/vendor_layout.py --write --out DIR --no-patch
    uv run --no-project python tools/vendor_layout.py --extract DIR   # whole upstream trees

Each component is downloaded as a GitHub tarball by full commit SHA (a git
checkout of HarfBuzz fails on Windows: some test file names are not legal
there). Only the files listed in third_party/MANIFEST.sha256 are taken; the
Skribidi patches (PATCHES, in order) are applied with `git apply`; then every
file is compared byte for byte with third_party/ and with its SHA-256 in the
manifest.

--extract writes every pin's whole tree (Skribidi patched), to work out the
file list of a new pin; --list FILE then takes the paths from FILE (one per
line) instead of the manifest and writes a new manifest. third_party/README.md
gives the procedures (update a pin, add a patch). Standard library only.
"""
import argparse
import hashlib
import http.client
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TP = os.path.join(ROOT, "third_party")
MANIFEST = os.path.join(TP, "MANIFEST.sha256")
PATCHES = ["skribidi-0001-bidi-run-merge.patch", "skribidi-0002-l1-trailing-whitespace.patch",
           "skribidi-0003-font-fallback.patch", "skribidi-0004-emoji-family-fallback.patch",
           "skribidi-0005-text-grapheme-breaks.patch", "skribidi-0006-caret-split-grapheme.patch"]   # in order

# component directory -> (GitHub repository, full commit SHA, label)
PINS = {
    "skribidi":    ("memononen/skribidi",   "dee63d6ba76aeddd49dea6d1b2508cf9aa391f46", "dee63d6"),
    "harfbuzz":    ("harfbuzz/harfbuzz",    "c7a7457b7385f33178e8cf87615ca077a810bbe7", "14.6.0"),
    "sheenbidi":   ("Tehreer/SheenBidi",    "83f77108a2873600283f6da4b326a2dca7a3a7a6", "83f7710"),
    "libunibreak": ("adah1972/libunibreak", "304585d8e2d63187507368d612c3d5fff1486368", "libunibreak_6_1"),
    "budouxc":     ("memononen/budouxc",    "a044d49afc654117fac7623fff15bec15943270c", "a044d49"),
}
# Files that come from another repository than their directory's pin. budouxc
# ships BudouX's models without their license; models/ja.json there equals
# google/budoux at this commit byte for byte.
RENAMES = {
    "budouxc/LICENSE-BudouX": ("google/budoux", "898b6a11bccc66117f7de8b3e5db65e9cfdfb8ed", "LICENSE"),
}
# What third_party/ holds besides the vendored files.
OURS = {"README.md", "CMakeLists.txt", "MANIFEST.sha256", ".gitattributes"}

# The vendored files are the license files below plus the include closure of
# the sources third_party/CMakeLists.txt compiles (--closure computes it).
LICENSES = ["skribidi/LICENSE", "harfbuzz/COPYING", "harfbuzz/AUTHORS", "harfbuzz/src/ms-use/COPYING",
            "sheenbidi/LICENSE", "libunibreak/LICENCE", "libunibreak/AUTHORS", "budouxc/LICENSE",
            "budouxc/LICENSE-BudouX"]
UNIBREAK = ["linebreak", "linebreakdata", "linebreakdef", "wordbreak", "graphemebreak", "eastasianwidthdef",
            "emojidef", "unibreakbase", "unibreakdef"]
SKRIBIDI = ["attribute_collection", "attributes", "common", "editor", "editor_rules", "font_collection",
            "icon_collection", "layout", "rich_layout", "rich_text", "text"]
SKB_INC = ["-Iskribidi/include", "-Iskribidi/src", "-Iharfbuzz/src", "-Isheenbidi/Headers", "-Ilibunibreak/src",
           "-Ibudouxc/include"]
# (compiler, flags, source) relative to an --extract tree
UNITS = ([("c++", ["-std=c++17"], "harfbuzz/src/harfbuzz.cc"),
          ("cc", ["-DSB_CONFIG_UNITY", "-Isheenbidi/Headers"], "sheenbidi/Source/SheenBidi.c"),
          ("cc", ["-Ibudouxc/include"], "budouxc/src/budoux.c")]
         + [("cc", [], "libunibreak/src/%s.c" % f) for f in UNIBREAK]
         + [("cc", ["-std=gnu17"] + SKB_INC, "skribidi/src/skb_%s.c" % f) for f in SKRIBIDI]
         + [("cc", ["-std=gnu17"] + SKB_INC, "skribidi/include/skribidi/skb_editor.h")])


def closure(tree, cc, cxx):
    """The files the units include, by the compiler's own dependency output."""
    files = set(LICENSES)
    root = os.path.abspath(tree)
    for kind, flags, src in UNITS:
        out = subprocess.run([cxx if kind == "c++" else cc, "-MM"] + flags + (["-x", "c"] if src.endswith(".h") else [])
                             + [src], cwd=root, check=True, capture_output=True, text=True).stdout
        out = out.replace("\\\n", " ").split(":", 1)[1]
        for tok in out.split():
            p = os.path.relpath(os.path.normpath(os.path.join(root, tok)), root).replace(os.sep, "/")
            files.add(p)
    return sorted(files)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fetch(repo, sha, cache):
    name = repo.replace("/", "_") + "-" + sha + ".tar.gz"
    if cache:
        p = os.path.join(cache, name)
        if os.path.exists(p):
            with open(p, "rb") as f:
                return f.read()
    url = "https://codeload.github.com/%s/tar.gz/%s" % (repo, sha)
    print("fetch", url, file=sys.stderr)
    # The HarfBuzz tarball is large and codeload drops slow transfers.
    for attempt in range(4):
        try:
            with urllib.request.urlopen(url, timeout=120) as r:
                data = r.read()
            break
        except (OSError, http.client.HTTPException) as e:
            if attempt == 3:
                raise
            print("retry after:", e, file=sys.stderr)
    if cache:
        os.makedirs(cache, exist_ok=True)
        with open(os.path.join(cache, name + ".part"), "wb") as f:
            f.write(data)
        os.replace(os.path.join(cache, name + ".part"), os.path.join(cache, name))
    return data


def extract(data, wanted):
    """wanted: paths inside the repository, or None for all. Returns path -> bytes."""
    out = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as t:
        for m in t:
            if not m.isfile():
                continue
            rel = m.name.split("/", 1)[1] if "/" in m.name else ""
            if wanted is None or rel in wanted:
                out[rel] = t.extractfile(m).read()
    if wanted is not None:
        missing = sorted(set(wanted) - set(out))
        if missing:
            sys.exit("not in the tarball: " + ", ".join(missing))
    return out


def read_manifest():
    entries = {}
    with open(MANIFEST, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                h, p = line.split(None, 1)
                entries[p] = h
    return entries


def apply_patches(work, patch):
    for name in PATCHES:
        pfile = os.path.join(TP, "patches", name)
        print("patch %s sha256:%s" % (name, sha256(open(pfile, "rb").read())), file=sys.stderr)
        if patch:
            comp = name.split("-", 1)[0]
            # core.autocrlf off for this call: Windows runners set it true
            # globally, and git apply then writes the patched files with CRLF,
            # which the manifest's hashes refuse.
            subprocess.run(["git", "-c", "core.autocrlf=false", "-c", "core.eol=lf", "apply",
                            "--whitespace=nowarn", pfile], cwd=os.path.join(work, comp), check=True)


def write_files(base, files):
    for p, data in files.items():
        full = os.path.join(base, p)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(data)


def build_tree(paths, cache, patch, work):
    """Downloads and writes paths (component/relative) under work; patches."""
    groups = {}
    for p in paths:
        if p in RENAMES:
            repo, sha, src = RENAMES[p]
        else:
            comp, rel = p.split("/", 1)
            if comp not in PINS:
                sys.exit("no pin for " + p)
            repo, sha, src = PINS[comp][0], PINS[comp][1], rel
        groups.setdefault((repo, sha), {})[src] = p
    for (repo, sha), m in sorted(groups.items()):
        files = extract(fetch(repo, sha, cache), m)
        write_files(work, {dst: files[src] for src, dst in m.items()})
    apply_patches(work, patch)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--write", action="store_true", help="write the tree instead of verifying it")
    ap.add_argument("--out", default=TP, help="where --write writes (default third_party/)")
    ap.add_argument("--no-patch", action="store_true", help="leave Skribidi unpatched (with --write --out only)")
    ap.add_argument("--list", help="take the paths from this file and write a new manifest")
    ap.add_argument("--extract", help="write every pin's whole tree here, Skribidi patched")
    ap.add_argument("--cache", help="keep downloaded tarballs here")
    ap.add_argument("--closure", help="print the file list of an --extract tree (needs gcc or clang)")
    ap.add_argument("--cc", default="gcc")
    ap.add_argument("--cxx", default="g++")
    a = ap.parse_args()
    if a.closure:
        print("\n".join(closure(a.closure, a.cc, a.cxx)))
        return 0
    if a.extract:
        for comp, (repo, sha, _) in sorted(PINS.items()):
            write_files(os.path.join(a.extract, comp), extract(fetch(repo, sha, a.cache), None))
        apply_patches(a.extract, True)
        print("extracted to", a.extract)
        return 0
    if a.no_patch and (not a.write or os.path.abspath(a.out) == TP):
        sys.exit("--no-patch needs --write and --out elsewhere")
    if a.list:
        with open(a.list, encoding="utf-8") as f:
            want = {l.strip(): None for l in f if l.strip() and not l.startswith("#")}
    else:
        want = read_manifest()
    work = tempfile.mkdtemp(prefix="ysp_vendor_")
    try:
        build_tree(sorted(want), a.cache, not a.no_patch, work)
        got = {p: open(os.path.join(work, p), "rb").read() for p in want}
        bad = 0
        if not a.list and not a.no_patch:
            for p, h in want.items():
                if sha256(got[p]) != h:
                    print("manifest mismatch:", p); bad += 1
        if a.write:
            out = os.path.abspath(a.out)
            for comp in sorted({p.split("/", 1)[0] for p in want}):
                shutil.rmtree(os.path.join(out, comp), ignore_errors=True)
            write_files(out, got)
            if a.list:
                with open(os.path.join(out, "MANIFEST.sha256"), "w", encoding="utf-8", newline="\n") as f:
                    f.write("# SHA-256 of every vendored file, patches applied (tools/vendor_layout.py)\n")
                    for p in sorted(got):
                        f.write("%s  %s\n" % (sha256(got[p]), p))
            print("wrote %d files, %d bytes, to %s" % (len(got), sum(len(d) for d in got.values()), out))
        else:
            for p, data in got.items():
                full = os.path.join(TP, p)
                if not os.path.exists(full):
                    print("missing:", p); bad += 1
                elif open(full, "rb").read() != data:
                    print("differs:", p); bad += 1
            comps = {p.split("/", 1)[0] for p in want}
            for dirpath, _, names in os.walk(TP):
                for n in names:
                    rel = os.path.relpath(os.path.join(dirpath, n), TP).replace(os.sep, "/")
                    top = rel.split("/", 1)[0]
                    if rel in OURS or top == "patches":
                        continue
                    if top in comps and rel not in want:
                        print("extra:", rel); bad += 1
            print("%s: %d files, %d bytes" % ("FAIL" if bad else "ok", len(got), sum(len(d) for d in got.values())))
        return 1 if bad else 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
