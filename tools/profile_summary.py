#!/usr/bin/env python3
"""Summarize a player profile (ysp_player --profile, a YSP_TRACE_RING build).

Usage: uv run --no-project python tools/profile_summary.py DIR/profile.csv

Prints, per zone: count, median, p99 and max in microseconds; then the
zones inside the worst frame (the longest pl.frame) and inside the longest
pl.trial_start, one line per zone name with its count, total and share. A
zone's time includes the zones nested in it. Times are the ysp_rt clock's, real ns also on --sim (the
virtual clock moves the frames, not the work).
"""
import csv
import sys


def pct(v, q):
    if not v:
        return float("nan")
    v = sorted(v)
    k = min(len(v) - 1, max(0, int(round(q * (len(v) - 1)))))
    return v[k]


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    rows = []
    loss = 0
    with open(sys.argv[1], newline="") as f:
        for r in csv.DictReader(f):
            if r["zone"] == "loss":
                loss += int(r["dur_ns"])
                continue
            rows.append((int(r["frame"]), r["zone"], int(r["t_ns"]), int(r["dur_ns"]), int(r["value"])))
    by = {}
    for fr, z, t, d, v in rows:
        by.setdefault(z, []).append(d)
    print("| Zone | Count | Median us | p99 us | Max us |")
    print("|---|---|---|---|---|")
    for z in sorted(by, key=lambda k: -max(by[k])):
        d = by[z]
        print("| %s | %d | %.2f | %.2f | %.2f |" % (z, len(d), pct(d, 0.5) / 1e3, pct(d, 0.99) / 1e3, max(d) / 1e3))
    if loss:
        print("\nrecords lost to a full ring: %d" % loss)

    def inside(name):
        cand = [r for r in rows if r[1] == name]
        if not cand:
            return
        top = max(cand, key=lambda r: r[3])
        t0, t1 = top[2], top[2] + top[3]
        print("\n%s %.2f us (frame %d):" % (name, top[3] / 1e3, top[0]))
        print("| Zone | Count | Total us | Share | Max us |")
        print("|---|---|---|---|---|")
        # one line per zone name, in the order of its first record: a
        # trial's end writes hundreds of rows, each its own zone
        agg = {}
        for fr, z, t, d, v in sorted((r for r in rows if r[2] >= t0 and r[2] + r[3] <= t1 and r is not top), key=lambda r: r[2]):
            a = agg.setdefault(z, [0, 0, 0])
            a[0] += 1
            a[1] += d
            a[2] = max(a[2], d)
        for z, (n, tot, mx) in agg.items():
            print("| %s | %d | %.2f | %.0f%% | %.2f |" % (z, n, tot / 1e3, 100.0 * tot / top[3] if top[3] else 0, mx / 1e3))

    inside("pl.frame")
    inside("pl.trial_start")
    return 0


if __name__ == "__main__":
    sys.exit(main())
