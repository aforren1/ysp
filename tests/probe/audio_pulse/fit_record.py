# fit_record.py - fits a straight line to each capture position candidate
# in pulse_record_probe's CSV and prints the residuals.
#
#   uv run --no-project python tests/probe/audio_pulse/fit_record.py out.csv [skip_s]
#
# Candidates, each a (frame, time) pair per new reply (4 bytes a frame):
#   write_index                         at the reply's timestamp
#   write_index + source_usec           the frame at the source
#   write_index + source + transport
#   read_index + source + transport + (write - read): the same as above
#   read + n                            at the callback (callback times)
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
    return k, [t - (mt + (x - mx) * k) for x, t in zip(xs, ts)]


def show(name, xs, ts, rate):
    k, res = fit(xs, ts)
    a = sorted(abs(r) for r in res)
    p99 = a[int(0.99 * (len(a) - 1))]
    ppm = (1e9 / rate / k - 1) * 1e6
    print(f"{name:30s} n={len(xs):6d} sd={statistics.pstdev(res)/1e3:8.2f} us"
          f" p99={p99/1e3:8.2f} max={a[-1]/1e3:8.2f} us  device {ppm:+7.2f} ppm")


def main():
    rows = list(csv.DictReader(open(sys.argv[1], newline="")))
    skip = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
    t0 = int(rows[0]["cb_ns"])
    rows = [r for r in rows if int(r["cb_ns"]) - t0 >= skip * 1e9]
    rate = int(rows[0]["rate"])
    bpf = 4
    ts = [int(r["ts_ns"]) for r in rows]
    wi = [int(r["write_index"]) / bpf for r in rows]
    ri = [int(r["read_index"]) / bpf for r in rows]
    src = [int(r["source_usec"]) * rate / 1e6 for r in rows]
    tr = [int(r["transport_usec"]) * rate / 1e6 for r in rows]
    print(f"{sys.argv[1]}: {len(rows)} replies at {rate} Hz")
    show("write_index", wi, ts, rate)
    show("write_index + source", [a + b for a, b in zip(wi, src)], ts, rate)
    show("write_index + source + transport", [a + b + c for a, b, c in zip(wi, src, tr)], ts, rate)
    show("read + n @ callback", [int(r["read"]) + int(r["n"]) for r in rows], [int(r["cb_ns"]) for r in rows], rate)
    lag = [int(r["read"]) + int(r["n"]) - a for r, a in zip(rows, ri)]
    print(f"source_usec p50 {statistics.median([int(r['source_usec']) for r in rows])} us,"
          f" transport p50 {statistics.median([int(r['transport_usec']) for r in rows])} us;"
          f" frames delivered less the reply's read_index: min {min(lag):.0f} max {max(lag):.0f};"
          f" write - read index: p50 {statistics.median([a - b for a, b in zip(wi, ri)]):.0f} frames;"
          f" stamp - callback p50 {statistics.median([a - int(r['cb_ns']) for r, a in zip(rows, ts)])/1e3:.1f} us")


main()
