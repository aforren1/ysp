# fit_capture.py - fits a straight line to each capture position candidate
# in alsa_capture_probe's CSV and prints the residuals.
#
#   uv run --no-project python tests/probe/audio_alsa/fit_capture.py out.csv [skip_s]
#
# Candidates, each a (frame, time) pair per callback:
#   read+n+delay @ status   the frame at the converter at the status stamp
#   read+n+avail @ status   the same with avail in place of delay
#   read+n       @ callback the frame after the block at the callback's entry
#   audio stamp  @ status   (mode "link") the audio timestamp as frames
# skip_s drops the first seconds (default 0.5). Rows after a trigger change
# (a restart, a move of more than 1 ms) end the fit there; the restarts are counted.
import csv
import statistics
import sys


def fit(xs, ts):
    n = len(xs)
    mx = sum(xs) / n
    mt = sum(ts) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxt = sum((x - mx) * (t - mt) for x, t in zip(xs, ts))
    k = sxt / sxx
    res = [t - (mt + (x - mx) * k) for x, t in zip(xs, ts)]
    return k, res


def show(name, xs, ts, rate=48000):
    k, res = fit(xs, ts)
    a = sorted(abs(r) for r in res)
    p99 = a[int(0.99 * (len(a) - 1))]
    ppm = (1e9 / rate / k - 1) * 1e6
    print(f"{name:26s} n={len(xs):6d} sd={statistics.pstdev(res)/1e3:8.2f} us"
          f" p99={p99/1e3:8.2f} max={a[-1]/1e3:8.2f} us  device {ppm:+7.2f} ppm")


def main():
    rows = list(csv.DictReader(open(sys.argv[1], newline="")))
    skip = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5
    rows = [r for r in rows if r["state"] == "3"]   # RUNNING
    t0 = int(rows[0]["cb_ns"])
    trig0 = rows[0]["trig_ns"]
    # "link" maps its stamps from CLOCK_MONOTONIC_RAW row by row, so the
    # trigger stamp wobbles by a few us there; a restart moves it by ms
    moved = lambda a, b: abs(int(a) - int(b)) > 1000000
    restarts = sum(1 for a, b in zip(rows, rows[1:]) if moved(a["trig_ns"], b["trig_ns"]))
    keep = []
    for r in rows:
        if moved(r["trig_ns"], trig0):
            break
        if int(r["cb_ns"]) - t0 >= skip * 1e9:
            keep.append(r)
    print(f"{sys.argv[1]}: {len(rows)} running rows, {restarts} restarts, {len(keep)} fitted")
    f = [int(r["read"]) + int(r["n"]) for r in keep]
    ts = [int(r["ts_ns"]) for r in keep]
    cb = [int(r["cb_ns"]) for r in keep]
    d = [int(r["delay"]) for r in keep]
    av = [int(r["avail"]) for r in keep]
    show("read+n+delay @ status", [a + b for a, b in zip(f, d)], ts)
    show("read+n+avail @ status", [a + b for a, b in zip(f, av)], ts)
    show("read+n+delay @ callback", [a + b for a, b in zip(f, d)], cb)
    show("read+n @ callback", f, cb)
    dd = [a - b for a, b in zip(d, av)]
    print(f"delay-avail: min {min(dd)} max {max(dd)} median {statistics.median(dd)} frames;"
          f" delay min {min(d)} max {max(d)}; status stamp - callback p50"
          f" {statistics.median([a - b for a, b in zip(ts, cb)])/1e3:.1f} us")
    blk = sorted(int(r["n"]) for r in keep)
    print(f"blocks: min {blk[0]} median {blk[len(blk)//2]} max {blk[-1]} frames")
    if keep and keep[0]["audio_type"] != "0":
        types = sorted(set(r["audio_type"] for r in keep))
        au = [int(r["audio_ns"]) * 48000 / 1e9 for r in keep]
        print(f"audio stamp types reported: {types}")
        show("audio stamp @ status", au, ts)


main()
