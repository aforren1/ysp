"""pulse_stats.py - onset latency, width error, missed pulses and call cost
from the CSV files of pulse_bench.c and pulse_bench.py.

    python pulse_stats.py out_dir [out_dir ...]

Writes summary.txt in the first out_dir and prints it.

The clock fit is serial_bench's (sb_fit.m, raw_win32.c): of the "e" probes,
the one with the shortest path (round trip minus the board's own time) in
each 200 ms; the board midpoint (rx + tx) / 2 maps to the host midpoint
(t0 + t2) / 2, a least-squares line through those points. One point lies
anywhere inside its half path, so the bound of a mapped time is the largest
half path of the points used ("fit +-"). Only the onset latency depends on
the fit; a width is the difference of two board stamps.

Statistics: the median; pN is the value at rank ceil(N/100 n) of the sorted
values, as sb_stats.m.

Public domain / MIT-0, as the rest of ysp.
"""
import csv
import math
import os
import sys
from collections import defaultdict

WRAP = 1 << 32


def q(v, p):
    if not v:
        return float("nan")
    s = sorted(v)
    return s[max(1, math.ceil(p / 100 * len(s))) - 1]


def med(v):
    if not v:
        return float("nan")
    s = sorted(v)
    n = len(s)
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def unwrap(v, ref):
    return ref + (v - ref) % WRAP


def fit(probes):
    """probes: list of (t0_ns, t2_ns, rx_us, tx_us), rx/tx unwrapped."""
    probes = sorted(probes)
    pts = []
    i = 0
    t00 = probes[0][0]
    while i < len(probes):
        b = (probes[i][0] - t00) // 200_000_000
        best = None
        while i < len(probes) and (probes[i][0] - t00) // 200_000_000 == b:
            t0, t2, rx, tx = probes[i]
            path = (t2 - t0) / 1000 - (tx - rx)  # us
            if best is None or path < best[0]:
                best = (path, (rx + tx) / 2, (t0 + t2) / 2)
            i += 1
        pts.append(best)
    x0 = pts[-1][1]
    m = len(pts)
    if m >= 3:
        sx = sum(p[1] - x0 for p in pts)
        sy = sum(p[2] for p in pts)
        sxx = sum((p[1] - x0) ** 2 for p in pts)
        sxy = sum((p[1] - x0) * p[2] for p in pts)
        slope = (m * sxy - sx * sy) / (m * sxx - sx * sx)
        y0 = (sy - slope * sx) / m
    else:
        slope = 1000.0
        y0 = med([p[2] - 1000.0 * (p[1] - x0) for p in pts])
    resid = [abs(p[2] - (y0 + slope * (p[1] - x0))) / 1000 for p in pts]
    return {
        "map": lambda x: y0 + slope * (x - x0),
        "slope": slope,
        "unc": max(p[0] for p in pts) / 2,
        "unc_min": min(p[0] for p in pts) / 2,
        "resid": max(resid),
        "npts": m,
    }


def load(dirs):
    rows = {k: [] for k in ("probes", "pulses", "board", "cells")}
    for d in dirs:
        for k in rows:
            path = os.path.join(d, k + ".csv")
            if os.path.exists(path):
                with open(path, newline="") as f:
                    rows[k] += list(csv.DictReader(f))
    return rows


def analyse(rows):
    probes = defaultdict(list)
    pulses = defaultdict(list)
    board = defaultdict(list)
    cells = {}
    for r in rows["cells"]:
        cells[int(r["cell"])] = r
    for r in rows["probes"]:
        probes[int(r["cell"])].append((int(r["t0_ns"]), int(r["t2_ns"]), int(r["rx_us"]), int(r["tx_us"])))
    for r in rows["pulses"]:
        pulses[int(r["cell"])].append(r)
    for r in rows["board"]:
        board[int(r["cell"])].append(r)
    out = []
    for cell in sorted(cells):
        c = cells[cell]
        mode = c["mode"]
        wus = int(c["width_us"])
        pr = probes[cell]
        if len(pr) < 2:
            continue
        ref = pr[0][2]
        pr = [(a, b, unwrap(rx, ref), unwrap(tx, ref)) for a, b, rx, tx in pr]
        F = fit(pr)
        line = mode.endswith("_dtr") or mode.endswith("_rts")
        bit = 2 if mode.endswith("_rts") else 1
        # board edges in order: (t_us, kind) with kind 1 on, 0 off
        edges = []
        gap = 0
        state = 0
        for r in board[cell]:
            k = r["kind"]
            t = unwrap(int(r["t_us"]), ref)
            v = int(r["v"])
            if k == "G":
                if int(r["block"]) >= 0:
                    gap = max(gap, v)
            elif k == "B":
                edges.append((t, 1 if v == 0x55 else 0))
            elif k == "L":
                s = 1 if v & bit else 0
                if s != state:
                    edges.append((t, s))
                state = s
        edges.sort()
        ps = sorted(pulses[cell], key=lambda r: int(r["i"]))
        onset, werr, call, missed, off_lat, host_werr = [], [], [], 0, [], []
        j = 0
        for k, r in enumerate(ps):
            ts = int(r["t_start_ns"])
            tn = int(ps[k + 1]["t_start_ns"]) if k + 1 < len(ps) else ts + 10**12
            call.append((int(r["t_ret_ns"]) - ts) / 1000)
            if int(r["t_off_ns"]) >= 0:
                host_werr.append((int(r["t_off_ns"]) - int(r["t_ret_ns"])) / 1000 - wus)
            on = off = None
            while j < len(edges) and F["map"](edges[j][0]) < ts - 1_000_000:
                j += 1
            while j < len(edges) and F["map"](edges[j][0]) < tn - 1_000_000:
                t, s = edges[j]
                j += 1
                if s == 1 and on is None:
                    on = t
                elif s == 0 and on is not None:
                    off = t
                    break
            if on is None or off is None:
                missed += 1
                continue
            onset.append((F["map"](on) - ts) / 1000)
            werr.append((off - on) * F["slope"] / 1000 - wus)
            if int(r["t_off_ns"]) >= 0:
                off_lat.append((F["map"](off) - int(r["t_off_ns"])) / 1000)
        out.append({
            "cell": cell, "run": c["run"], "mode": mode, "width_ms": wus / 1000,
            "n": len(ps), "missed": missed, "policy": c["async_policy"],
            "onset": onset, "werr": werr, "call": call, "off_lat": off_lat,
            "host_werr": host_werr, "fit": F, "gap": gap,
            # a blocking pulse holds the caller for the width; the rest is its cost
            "call_net": [x - wus for x in call] if mode in ("ysp_pulse", "ysp_py_pulse") else call,
        })
    return out


def fmt(v, digits=1):
    return "nan" if v != v else f"{v:.{digits}f}"


def five(v):
    return " ".join(fmt(x) for x in (med(v), q(v, 5), q(v, 95), q(v, 99), max(v) if v else float("nan")))


def report(res):
    lines = []
    lines.append("Times in microseconds. Columns: median p5 p95 p99 max; werr also min.")
    lines.append("onset = board's first edge (mapped) - call start; werr = board width - requested;")
    lines.append("call = call return - call start; off_lat = board's trailing edge (mapped) - trailing call start.")
    lines.append("")
    hdr = f"{'cell':>5} {'mode':<19} {'w ms':>4} {'n':>4} {'miss':>4} {'fit+-':>6} {'gap':>4} | onset | werr | call | off_lat | host_werr"
    lines.append(hdr)
    for r in sorted(res, key=lambda r: (r["mode"], r["width_ms"], r["cell"])):
        F = r["fit"]
        lines.append(
            f"{r['cell']:>5} {r['mode']:<19} {r['width_ms']:>4g} {r['n']:>4} {r['missed']:>4} "
            f"{fmt(F['unc']):>6} {r['gap']:>4} | {five(r['onset'])} | {five(r['werr'])} "
            f"min {fmt(min(r['werr']) if r['werr'] else float('nan'))} | "
            f"{five(r['call'])} | {five(r['off_lat'])} | {five(r['host_werr'])}")
    lines.append("")
    lines.append("Pooled over the cells given (runs), per mode and width: n, missed | werr median p5 p95 p99 max min")
    groups = defaultdict(list)
    for r in res:
        groups[(r["mode"], r["width_ms"])].append(r)
    for (mode, w), rs in sorted(groups.items()):
        v = [x for r in rs for x in r["werr"]]
        lines.append(f"{mode:<19} {w:>4g} {sum(r['n'] for r in rs):>5} {sum(r['missed'] for r in rs):>4} | "
                     f"{five(v)} {fmt(min(v) if v else float('nan'))}")
    lines.append("")
    lines.append("Pooled over widths and runs, per mode: n | onset median p5 p95 p99 max | call median p99 max "
                 "(ysp_pulse, ysp_py_pulse: minus the width) | per-cell onset medians | fit +- range")
    groups = defaultdict(list)
    for r in res:
        groups[r["mode"]].append(r)
    for mode, rs in sorted(groups.items()):
        on = [x for r in rs for x in r["onset"]]
        ca = [x for r in rs for x in r["call_net"]]
        meds = [med(r["onset"]) for r in rs if r["onset"]]
        uncs = [r["fit"]["unc"] for r in rs]
        lines.append(f"{mode:<19} {len(on):>5} | {five(on)} | {fmt(med(ca))} {fmt(q(ca, 99))} "
                     f"{fmt(max(ca) if ca else float('nan'))} | "
                     f"{fmt(min(meds) if meds else float('nan'))} to {fmt(max(meds) if meds else float('nan'))} | "
                     f"{fmt(min(uncs))} to {fmt(max(uncs))}")
    lines.append("")
    lines.append("Fits: cell, points, slope (ns per board us), bound min/max (us), largest residual (us), worker policy")
    for r in sorted(res, key=lambda r: r["cell"]):
        F = r["fit"]
        lines.append(f"{r['cell']:>5} {F['npts']:>3} {F['slope']:.6f} {fmt(F['unc_min'])}/{fmt(F['unc'])} "
                     f"{fmt(F['resid'])} {r['policy']}")
    return "\n".join(lines) + "\n"


def main(argv):
    if len(argv) < 2:
        sys.exit("usage: pulse_stats.py out_dir [out_dir ...]")
    res = analyse(load(argv[1:]))
    text = report(res)
    with open(os.path.join(argv[1], "summary.txt"), "w", newline="\n") as f:
        f.write(text)
    sys.stdout.buffer.write(text.encode())  # LF on Windows too


if __name__ == "__main__":
    main(sys.argv)
