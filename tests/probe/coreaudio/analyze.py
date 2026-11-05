"""One table row from audio_clockstats --csv PREFIX on CoreAudio.

    uv run --no-project python tests/probe/coreaudio/analyze.py PREFIX_ticks.csv [--rate 48000] [--label TEXT]

Reads the per-callback rows (t_entry_ns, pos, pos_t_ns, w, getpos_ns,
render_ns) and prints one Markdown row: callbacks, the callback interval,
the HAL reports' residual from one least-squares line over the run and from
lines over 10 s pieces, the device rate against the ysp_rt clock, how old a
report is at the call that gives it, and the time line's cost per callback
(the column audio_clockstats calls GetPosition). The first 0.5 s (the
warm-up) is left out. Standard library only.
"""
import argparse
import csv
import math


def pct(xs, q):
    if not xs:
        return float("nan")
    s = sorted(xs)
    return s[min(len(s) - 1, max(0, int(math.floor(q * (len(s) - 1) + 0.5))))]


def line(px, py):
    n = len(px)
    mx = sum(px) / n
    my = sum(py) / n
    sxx = sum((x - mx) ** 2 for x in px)
    sxy = sum((x - mx) * (y - my) for x, y in zip(px, py))
    b = sxy / sxx if sxx else 0.0
    return my - b * mx, b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ticks")
    ap.add_argument("--rate", type=float, default=48000.0)
    ap.add_argument("--label", default="")
    a = ap.parse_args()
    rows = []
    with open(a.ticks, newline="") as f:
        for r in csv.DictReader(f):
            rows.append({k: int(v) for k, v in r.items()})
    if len(rows) < 10:
        raise SystemExit("too few rows")
    t0 = rows[0]["t_entry_ns"]
    rows = [r for r in rows if r["t_entry_ns"] - t0 >= 500_000_000]
    iv = [(b["t_entry_ns"] - a_["t_entry_ns"]) / 1e6 for a_, b in zip(rows, rows[1:])]
    rep = [r for r in rows if r["pos"] >= 0 and r["pos_t_ns"] > 0]
    # a report given twice adds nothing
    seen, pts = set(), []
    for r in rep:
        if r["pos"] not in seen:
            seen.add(r["pos"])
            pts.append((r["pos"], r["pos_t_ns"]))
    px = [p for p, _ in pts]
    py = [t for _, t in pts]
    c, k = line(px, py)
    res = [(t - (c + k * p)) / 1e3 for p, t in pts]
    drift = (1e9 / a.rate / k - 1.0) * 1e6 if k else float("nan")
    # 10 s pieces: the residual after the slow wander
    loc = []
    span = 10e9
    i = 0
    while i < len(pts):
        j = i
        while j < len(pts) and pts[j][1] - pts[i][1] < span:
            j += 1
        if j - i >= 20:
            c2, k2 = line(px[i:j], py[i:j])
            loc += [(py[m] - (c2 + k2 * px[m])) / 1e3 for m in range(i, j)]
        i = j
    age = [(r["t_entry_ns"] - r["pos_t_ns"]) / 1e6 for r in rep]
    cost = [r["getpos_ns"] / 1e3 for r in rows]
    sd = math.sqrt(sum(x * x for x in res) / len(res)) if res else float("nan")
    print("| Run | Callbacks | Interval p1 / p50 / p99 / max ms | Reports | Residual, one line: sd / p99 abs / max abs us"
          " | Residual, 10 s lines: p99 abs / max abs us | Drift ppm | Report age p50 / max ms | Time line cost p50 / p99 us |")
    print("|---|---|---|---|---|---|---|---|---|")
    ab = [abs(x) for x in res]
    lb = [abs(x) for x in loc]
    print(f"| {a.label} | {len(rows)} | {pct(iv, 0.01):.2f} / {pct(iv, 0.5):.2f} / {pct(iv, 0.99):.2f} / {max(iv):.2f}"
          f" | {len(pts)} | {sd:.2f} / {pct(ab, 0.99):.2f} / {max(ab):.2f}"
          f" | {pct(lb, 0.99):.2f} / {max(lb) if lb else float('nan'):.2f} | {drift:+.2f}"
          f" | {pct(age, 0.5):.2f} / {max(age):.2f} | {pct(cost, 0.5):.2f} / {pct(cost, 0.99):.2f} |")


if __name__ == "__main__":
    main()
