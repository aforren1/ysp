"""Throughput of `ypak build` per resource kind (docs/pack.md 8.2).

    uv run --no-project python tests/pack/throughput.py YPAK CORPUS [--rounds 5] [--json OUT]

CORPUS is a folder of source files. Each kind is built as one pack of every
file of that kind in CORPUS, ROUNDS times; the median wall time is reported
with the source bytes, the files and the pack's size. The time includes the
process start and the pack's write (one `ypak build` per round), so it is
what a person running the tool waits for. Kinds and their files:

    table (xlsx)     *.xlsx
    table (csv)      *.csv
    texture (jpeg)   *.jpg   (refused files are left out first)
    texture (png)    *.png
    video (index)    *.mp4 and *.mpg made canonical (copied, then indexed)
    frameseq         *.yspvseq
    audio            *.wav   (at the description's project rate)

Run it under the measurement guard (CLAUDE.md): one measurement at a time.
Standard library only.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time

KINDS = [
    ("table (xlsx)", "table", (".xlsx",), {}),
    ("table (csv)", "table", (".csv",), {}),
    ("texture (jpeg)", "texture", (".jpg", ".jpeg"), {"primaries": "device"}),
    ("texture (png)", "texture", (".png",), {"primaries": "device"}),
    ("video (index)", "video", (".mp4", ".mpg"), {}),
    ("frameseq", "frameseq", (".yspvseq",), {}),
    ("audio", "audio", (".wav",), {}),
]


def run(ypak, args):
    t0 = time.perf_counter()
    p = subprocess.run([ypak] + args, capture_output=True, text=True)
    return time.perf_counter() - t0, p


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("ypak")
    ap.add_argument("corpus")
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--json")
    a = ap.parse_args()
    work = tempfile.mkdtemp(prefix="ypak-throughput-")
    rows = []
    for label, kind, exts, extra in KINDS:
        files = sorted(f for f in os.listdir(a.corpus) if f.lower().endswith(exts))
        ok = []
        for f in files:   # keep the files the tool takes; the refusals are the tests' business
            r = {"kind": kind, "source": f}
            r.update(extra)
            if kind == "table":
                r["name"] = f + ".pstb"
            d = {"format": "ysp-pack-source", "version": 1, "audio": {"rate": 48000, "channels": 2}, "resources": [r]}
            dp = os.path.join(a.corpus, "_one.json")
            with open(dp, "w", newline="\n") as fh:
                json.dump(d, fh)
            _, p = run(a.ypak, ["build", dp, "-o", os.path.join(work, "one.ysppak")])
            if p.returncode == 0:
                ok.append(r)
            else:
                print("  left out %s: %s" % (f, p.stderr.strip()[:160]))
        if not ok:
            continue
        d = {"format": "ysp-pack-source", "version": 1, "audio": {"rate": 48000, "channels": 2}, "resources": ok}
        dp = os.path.join(a.corpus, "_kind.json")
        with open(dp, "w", newline="\n") as fh:
            json.dump(d, fh)
        src = sum(os.path.getsize(os.path.join(a.corpus, r["source"])) for r in ok)
        times = []
        out = os.path.join(work, "kind.ysppak")
        for _ in range(a.rounds):
            t, p = run(a.ypak, ["build", dp, "-o", out])
            if p.returncode:
                raise SystemExit("%s: %s" % (label, p.stderr))
            times.append(t)
        med = statistics.median(times)
        row = {"kind": label, "files": len(ok), "source_bytes": src, "pack_bytes": os.path.getsize(out),
               "median_s": med, "min_s": min(times), "max_s": max(times), "mb_per_s": src / med / 1e6,
               "ms_per_file": 1000 * med / len(ok)}
        rows.append(row)
        print("%-16s %3d files %10.1f MB -> %10.1f MB  %8.3f s (%.3f to %.3f)  %7.1f MB/s  %8.1f ms a file" %
              (label, len(ok), src / 1e6, row["pack_bytes"] / 1e6, med, min(times), max(times), row["mb_per_s"], row["ms_per_file"]))
        os.remove(dp)
    one = os.path.join(a.corpus, "_one.json")
    if os.path.exists(one):
        os.remove(one)
    if a.json:
        with open(a.json, "w", newline="\n") as fh:
            json.dump(rows, fh, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
