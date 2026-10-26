"""Summarizes the probe's per-frame CSVs (docs/av_probe.md).

    uv run --no-project python analyze.py CSV...

One row per file: frames; missed (records with dropped > 0, and the
vblanks dropped); late targets (the caller's lateness); residual p50 /
p99 / max in us; paths and tiers; the work between begin() and flip_at()
p50 / p99 / max in ms and the frames over 16.7 ms; the time outside the
two calls (the host's loop overhead) p99 and max in ms.
"""
import sys

PATHS = {0: "unknown", 1: "composed", 2: "overlay", 3: "independent", 4: "sim", 5: "browser"}
LATE, EST, NEVER = 0x004, 0x002, 0x040 | 0x080


def pct(v, p):
    if not v:
        return float("nan")
    v = sorted(v)
    k = min(len(v) - 1, max(0, int(round(p / 100.0 * (len(v) - 1)))))
    return v[k]


def summarize(path):
    tag, rows = "", []
    with open(path, newline="") as f:
        for line in f:
            if line.startswith("#"):
                tag = line[1:].strip()
                continue
            if line.startswith("index"):
                continue
            rows.append([int(x) for x in line.split(",")])
    rec = [r for r in rows if r[8] != 0 or r[13] & NEVER]
    missed = [r for r in rec if r[10] > 0]
    late = [r for r in rec if r[13] & LATE]
    est = [r for r in rec if r[13] & EST]
    resid = [abs(r[9]) / 1e3 for r in rec if r[8] != 0]
    work = [r[2] / 1e6 for r in rows if r[2] > 0]
    outside = [r[4] / 1e6 for r in rows if r[4] > 0]
    paths, tiers = {}, {}
    for r in rec:
        paths[PATHS.get(r[11], r[11])] = paths.get(PATHS.get(r[11], r[11]), 0) + 1
        tiers[r[12]] = tiers.get(r[12], 0) + 1
    over = sum(1 for w in work if w > 16.7)
    return {
        "tag": tag, "frames": len(rows), "records": len(rec),
        "missed": len(missed), "dropped": sum(r[10] for r in missed), "late": len(late), "est": len(est),
        "res": (pct(resid, 50), pct(resid, 99), max(resid) if resid else float("nan")),
        "work": (pct(work, 50), pct(work, 99), max(work) if work else float("nan")), "over": over,
        "out": (pct(outside, 99), max(outside) if outside else float("nan")),
        "paths": paths, "tiers": tiers,
        "late_idx": [r[0] for r in late][:10], "miss_idx": [r[0] for r in missed][:10],
    }


def main():
    print("| run | frames | records | missed (vblanks) | late target | estimated | residual us p50/p99/max "
          "| work ms p50/p99/max | > 16.7 ms | outside ms p99/max | path | tier |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for p in sys.argv[1:]:
        s = summarize(p)
        print("| %s | %d | %d | %d (%d) | %d | %d | %.1f / %.1f / %.1f | %.3f / %.3f / %.3f | %d | %.3f / %.3f | %s | %s |" % (
            s["tag"], s["frames"], s["records"], s["missed"], s["dropped"], s["late"], s["est"],
            *s["res"], *s["work"], s["over"], *s["out"],
            " ".join("%s %d" % kv for kv in sorted(s["paths"].items())),
            " ".join("%s:%d" % kv for kv in sorted(s["tiers"].items()))))
        if s["late_idx"] or s["miss_idx"]:
            print("<!-- %s late %s missed %s -->" % (p, s["late_idx"], s["miss_idx"]))


if __name__ == "__main__":
    main()
