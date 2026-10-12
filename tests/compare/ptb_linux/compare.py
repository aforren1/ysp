"""Psychtoolbox's Flip and ysp/screen.h's flips against the kernel's vblanks.

    uv run --no-project python compare.py OUT_DIR [OUT_DIR ...]

For each cell of run.sh (NAME.csv beside NAME_vbl.csv) it maps every
reported onset to the nearest kernel vblank and prints:
  stamp error   reported onset minus that vblank's kernel time, us
  on target     the flip landed on the vblank it asked for
  late, early   it landed after or before that vblank
  flagged       late flips the tool itself reported (PTB: missed > 0,
                ysp: dropped > 0), and on-time flips it reported as late
Psychtoolbox's times are GetSecs (CLOCK_REALTIME on Linux); vbl_log's third
column maps them to CLOCK_MONOTONIC. ysp's are CLOCK_MONOTONIC already.
"""
import bisect
import csv
import os
import statistics
import sys


def read_vbl(path):
    seq, t, off = [], [], []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            seq.append(int(r["seq"]))
            t.append(int(r["mono_ns"]))
            off.append(int(r["real_minus_mono_ns"]))
    return seq, t, statistics.median(off), max(off) - min(off)


def nearest(t_list, x):
    i = bisect.bisect_left(t_list, x)
    best = None
    for j in (i - 1, i):
        if 0 <= j < len(t_list) and (best is None or abs(t_list[j] - x) < abs(t_list[best] - x)):
            best = j
    return best


def pct(v, p):
    if not v:
        return float("nan")
    s = sorted(v)
    k = max(1, -(-len(s) * p // 100))
    return s[int(k) - 1]


def ptb_rows(path, off):
    rows = []
    with open(path) as f:
        lines = [ln for ln in f if not ln.startswith("#")]
    for r in csv.DictReader(lines):
        rows.append({
            "k": int(r["k"]),
            "onset": round(float(r["vbl"]) * 1e9) - off,
            "late_flag": float(r["missed"]) > 0,
        })
    return rows


def ysp_rows(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if int(r["index"]) < 0:
                continue
            rows.append({
                "target": int(r["target_ns"]),
                "onset": int(r["onset_ns"]),
                "late_flag": int(r["dropped"]) > 0,
                "flags": int(r["flags"]),
            })
    return rows


def cell(out, name):
    seq, t, off, off_span = read_vbl(os.path.join(out, name + "_vbl.csv"))
    path = os.path.join(out, name + ".csv")
    ptb = name.startswith("ptb")
    rows = ptb_rows(path, off) if ptb else ysp_rows(path)
    err, on, late, early, flag_late, flag_false, lost = [], 0, 0, 0, 0, 0, 0
    prev = None
    for r in rows:
        j = nearest(t, r["onset"])
        if j is None or abs(t[j] - r["onset"]) > 2_000_000:
            lost += 1
            prev = None
            continue
        err.append((r["onset"] - t[j]) / 1000)
        if ptb:
            want = None if prev is None else seq[prev] + r["k"]
        else:
            w = nearest(t, r["target"])
            want = seq[w] if w is not None and abs(t[w] - r["target"]) < 8_000_000 else None
        prev = j
        if want is None:
            continue
        if seq[j] == want:
            on += 1
            if r["late_flag"]:
                flag_false += 1
        elif seq[j] > want:
            late += 1
            if r["late_flag"]:
                flag_late += 1
        else:
            early += 1
    abs_err = [abs(e) for e in err]
    return {
        "name": name, "n": len(rows), "lost": lost,
        "err_med": statistics.median(err) if err else float("nan"),
        "err_p99": pct(abs_err, 99), "err_max": max(abs_err) if abs_err else float("nan"),
        "on": on, "late": late, "early": early, "flag_late": flag_late, "flag_false": flag_false,
        "off_span_us": off_span / 1000,
    }


def main():
    names = ["ptb_next", "ysp_next", "ptb_hold", "ysp_hold",
             "ptb_next_load", "ysp_next_load", "ptb_hold_load", "ysp_hold_load"]
    print("%-14s %-15s %5s %4s | %9s %8s %8s | %5s %4s %5s | %10s %10s | %s" % (
        "run", "cell", "n", "lost", "err med", "|err|p99", "max", "on", "late", "early",
        "late flagd", "false flag", "real-mono span us"))
    for out in sys.argv[1:]:
        for nm in names:
            if not os.path.exists(os.path.join(out, nm + ".csv")):
                continue
            c = cell(out, nm)
            print("%-14s %-15s %5d %4d | %9.3f %8.3f %8.3f | %5d %4d %5d | %4d of %-3d %10d | %.1f" % (
                os.path.basename(out.rstrip("/")), nm, c["n"], c["lost"], c["err_med"], c["err_p99"],
                c["err_max"], c["on"], c["late"], c["early"], c["flag_late"], c["late"], c["flag_false"],
                c["off_span_us"]))


if __name__ == "__main__":
    main()
