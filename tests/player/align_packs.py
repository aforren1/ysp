#!/usr/bin/env python3
"""The packs of test_player_sim's align case (ysp/experiment.h 0.7.0: a
text with no `align` is centered; format 2.11).

Run with one interpreter:  python3 tests/player/align_packs.py YPAK STROOP_SRC STROOP_PACK OUT_DIR

STROOP_SRC is a copy of tests/player/stroop with its font, STROOP_PACK the
pack ypak built from it. Writes into OUT_DIR:

- stroop_start.ysppak: the example with "align": "start" on every text,
  built by this ypak. Its glyph runs are the ones ypak 0.12.0 laid out for
  the example with no `align` (start was the default then).
- stroop_old.ysppak: the pack ypak 0.12.0 built from the example: the
  entries of stroop_start, but the definition as STROOP_PACK stores it (no
  `align`; ysp/experiment.h 0.6.0 left its default "start" out, 0.7.0 its
  default "center", so both store these bytes), and the experiment entry's
  derivation naming ysp/experiment.h 0.6.0. The player must find the
  "start" blocks for it. Written by bindings/python/ysp_pack/tests/
  packwriter.py, independent of the tool.

Prints NAME=PATH for each, one per line.
"""
import io
import json
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "bindings", "python", "ysp_pack", "tests"))
import packwriter  # noqa: E402

MANIFEST = "ysp/manifest.json"


def entries(path):
    """(name, kind, data) of every entry but the manifest and ysp/chunks, and the manifest."""
    with zipfile.ZipFile(path) as z:
        man = json.loads(z.read(MANIFEST))
        kinds = {e["name"]: e["kind"] for e in man["entries"]}
        out = [(n, kinds[n], z.read(n)) for n in z.namelist() if n not in (MANIFEST, "ysp/chunks")]
    return out, man


def main():
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    ypak, src, stroop_pack, out = sys.argv[1:]
    os.makedirs(out, exist_ok=True)
    work = os.path.join(out, "stroop_start_src")
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(src, work)
    defn = os.path.join(work, "stroop.json")
    with open(defn, encoding="utf-8") as f:
        d = json.load(f)
    for s in d["stimuli"].values():
        if s["kind"] == "text":
            s["align"] = "start"
    with open(defn, "w", encoding="utf-8", newline="\n") as f:
        json.dump(d, f)
    start = os.path.join(out, "stroop_start.ysppak")
    subprocess.run([ypak, "build", "pack.json", "-o", start], cwd=work, check=True, stdout=subprocess.DEVNULL)
    shutil.rmtree(work, ignore_errors=True)

    ents, man = entries(start)
    plain, _ = entries(stroop_pack)
    exp_name = man["experiment"]["entry"]
    exp_bytes = [e[2] for e in plain if e[0] == exp_name][0]
    ents = [(n, k, exp_bytes if n == exp_name else b) for n, k, b in ents]
    # the manifest's entries with their derivations: packwriter writes the
    # hashes, then the same list again with the records the player reads
    first = packwriter.build(ents)
    with zipfile.ZipFile(io.BytesIO(first)) as z:
        rec = json.loads(z.read(MANIFEST))["entries"]
    derivs = {e["name"]: e.get("derivation") for e in man["entries"]}
    for e in rec:
        if derivs.get(e["name"]):
            e["derivation"] = derivs[e["name"]]
        if e["name"] == exp_name:
            e["derivation"] = {"by": "ysp/experiment.h 0.6.0", "op": "canonical"}
    tool = man["tool"]
    tool["version"] = "0.12.0"
    tool["libraries"]["ysp/experiment.h"] = "0.6.0"
    old = os.path.join(out, "stroop_old.ysppak")
    with open(old, "wb") as f:
        f.write(packwriter.build(ents, extra_manifest={"entries": rec, "tool": tool}))
    print("stroop_start=%s" % start)
    print("stroop_old=%s" % old)
    return 0


if __name__ == "__main__":
    sys.exit(main())
