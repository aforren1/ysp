"""The table of the loop CSVs (loop.py, loop_mex.m), with docs/av_probe.md's columns.

    python table.py CSV...                 one row per file
    python table.py --after FIRST CSV      the records around frame FIRST (after an idle gap)

Missed: records with dropped > 0 (the vblanks in brackets). Late target:
FLIP_LATE_TARGET. Occluded: FLIP_OCCLUDED. Residual: |onset - target| of
the flips shown. Work: the record's DRAW phase, the host's time from
begin()'s return to the flip call (nothing is marked, so all of it goes to
DRAW).
"""
import sys

PATHS = {0: "unknown", 1: "composed", 2: "overlay", 3: "independent", 4: "sim"}
LATE, OCC, NEVER = 0x004, 0x008, 0x040 | 0x080


def pct(v, p):
    if not v:
        return float("nan")
    v = sorted(v)
    return v[min(len(v) - 1, max(0, int(round(p / 100.0 * (len(v) - 1)))))]


def load(path):
    tag, rows = "", []
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                tag = line[1:].strip()
            elif not line.startswith("index") and line.strip():
                rows.append([int(float(x)) for x in line.split(",")])
    return tag, rows


def summarize(path):
    tag, rows = load(path)
    shown = [r for r in rows if not r[8] & NEVER]
    missed = [r for r in rows if r[5] > 0]
    late = sum(1 for r in rows if r[8] & LATE)
    occ = sum(1 for r in rows if r[8] & OCC)
    res = [abs(r[4]) / 1e3 for r in shown]
    work = [r[9] / 1e6 for r in rows if r[9] >= 0]
    paths, tiers = {}, {}
    for r in rows:
        paths[PATHS.get(r[6], r[6])] = paths.get(PATHS.get(r[6], r[6]), 0) + 1
        tiers[r[7]] = tiers.get(r[7], 0) + 1
    return "| %s | %d | %d (%d) | %d | %d | %.1f / %.1f | %.2f / %.2f / %.2f | %d | %s; %s |" % (
        tag.split(" ")[0], len(rows), len(missed), sum(r[5] for r in missed), late, occ, pct(res, 50), pct(res, 99),
        pct(work, 50), pct(work, 99), max(work) if work else float("nan"), sum(1 for w in work if w > 8.0),
        ", ".join("%s %d" % kv for kv in sorted(paths.items())),
        ", ".join("t%s %d" % kv for kv in sorted(tiers.items())))


def main():
    if sys.argv[1] == "--after":
        first = int(sys.argv[2])
        for r in load(sys.argv[3])[1]:
            if first - 2 <= r[0] < first + 20:
                print("%d: res %.3f ms, dropped %d, %s, tier %d, flags 0x%x, work %.2f ms" % (
                    r[0], r[4] / 1e6, r[5], PATHS.get(r[6], r[6]), r[7], r[8], r[9] / 1e6))
        return
    print("| Host | Records | Missed (vblanks) | Late target | Occluded | Residual us p50 / p99 | "
          "Work ms p50 / p99 / max | Work > 8 ms | Path; tier |")
    print("|---|---|---|---|---|---|---|---|---|")
    for p in sys.argv[1:]:
        print(summarize(p))


if __name__ == "__main__":
    main()
