"""Tail of the DTR/RTS/byte call cost per session: the share of calls over
1 ms and 2 ms, p50/p99/max. Both the onset call (t_ret - t_start) and the
trailing call (t_off_ret - t_off) count. Reads the pulses.csv of
pulse_bench.c, pulse_bench.py and device_pulse.c.

    python call_tail.py dir [dir ...]

Public domain / MIT-0, as the rest of ysp.
"""
import csv
import math
import os
import sys
from collections import defaultdict


def q(v, p):
    s = sorted(v)
    return s[max(1, math.ceil(p / 100 * len(s))) - 1]


for d in sys.argv[1:]:
    path = os.path.join(d, "pulses.csv")
    if not os.path.exists(path):
        continue
    by = defaultdict(list)
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            m = r["mode"]
            if m in ("ysp_pulse", "ysp_py_pulse"):  # their call holds the width
                continue
            ts, tr = int(r["t_start_ns"]), int(r["t_ret_ns"])
            by[m].append((tr - ts) / 1e3)
            to, tor = int(r["t_off_ns"]), int(r["t_off_ret_ns"])
            if to >= 0 and tor >= 0:
                by[m].append((tor - to) / 1e3)
    for m, v in sorted(by.items()):
        n = len(v)
        o1 = sum(x > 1000 for x in v)
        o2 = sum(x > 2000 for x in v)
        print(f"{d:<45} {m:<16} n {n:5d} p50 {q(v,50):7.1f} p99 {q(v,99):7.1f} max {max(v):8.1f} "
              f">1ms {100*o1/n:5.2f}% >2ms {100*o2/n:5.2f}%")
