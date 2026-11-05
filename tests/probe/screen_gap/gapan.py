"""Summarize gapdiag CSVs (uv run --no-project python gapan.py CSV...): the grid's period before the gap against a
least-squares fit of the OS times, and where the OS times fall after it."""
import csv
import sys

NOM = 256880 / 15413000 * 1e9   # the mode's period, ns


def fit(pts):
    # t = a + b * k, with k the vblank number from the nominal period
    n = len(pts)
    mk = sum(k for k, _ in pts) / n
    mt = sum(t for _, t in pts) / n
    sxx = sum((k - mk) ** 2 for k, _ in pts)
    sxy = sum((k - mk) * (t - mt) for k, t in pts)
    b = sxy / sxx
    return mt - b * mk, b


def main(path):
    rows = [r for r in csv.DictReader(open(path))]
    for r in rows:
        for k in r:
            r[k] = float(r[k])
    comp = [r for r in rows if r["src"] == 0 and r["t"] > 0]
    wake = [r for r in rows if r["src"] == 2]
    # the gap: the largest step in t_call between completions
    comp.sort(key=lambda r: r["t"])
    gi = max(range(1, len(comp)), key=lambda i: comp[i]["t"] - comp[i - 1]["t"])
    pre, post = comp[:gi], comp[gi:]
    t0 = pre[0]["t"]
    pts = [(round((r["t"] - t0) / NOM), r["t"]) for r in pre]
    a, b = fit(pts)
    last = pre[-1]
    grid_p = last["period"]
    gap_s = (post[0]["t"] - last["t"]) / 1e9
    k1 = round((post[0]["t"] - t0) / NOM)
    pred_ls = a + b * k1
    print("%s: %d OS times before the gap over %.1f s, %d after; gap %.2f s; wakes logged %d" % (
        path.split("/")[-1], len(pre), (last["t"] - t0) / 1e9, len(post), gap_s, len(wake)))
    print("  period: grid %.4f ns (%+.1f ppm vs the mode), fitted over %d vblanks; least squares of the OS times %.4f ns (%+.1f ppm); "
          "grid minus LS %+.1f ppm" % (grid_p, (grid_p / NOM - 1) * 1e6, last["ref_span"], b, (b / NOM - 1) * 1e6,
                                         (grid_p / b - 1) * 1e6))
    print("  first OS time after the gap: %+.1f us off the grid; %+.1f us off the least-squares line" % (
        post[0]["off_ns"] / 1e3, (post[0]["t"] - pred_ls) / 1e3))
    offs = [r["off_ns"] / 1e3 for r in post[:24]]
    print("  offsets after the gap (us):", " ".join("%.0f" % o for o in offs))
    if len(post) > 30:
        pts2 = [(round((r["t"] - t0) / NOM), r["t"]) for r in post[20:]]
        a2, b2 = fit(pts2)
        print("  after the gap, least squares: period %.4f ns (%+.1f ppm); its line minus the pre-gap line at the first post time: %+.1f us" % (
            b2, (b2 / NOM - 1) * 1e6, ((a2 + b2 * k1) - pred_ls) / 1e3))
    if wake:
        # wakes during the gap against the pre-gap line
        gw = [w for w in wake if last["t"] < w["t"] < post[0]["t"]]
        if gw:
            d = [(w["t"] - (a + b * round((w["t"] - t0) / NOM))) / 1e3 for w in gw]
            n = len(d)
            ds = sorted(d)
            print("  waiter wakes in the gap: %d; wake minus the pre-gap line: first %.0f, median %.0f, p10 %.0f, p90 %.0f, last %.0f us" % (
                n, d[0], ds[n // 2], ds[n // 10], ds[(9 * n) // 10], d[-1]))
            # drift of the wakes over the gap: fit wake times
            pw = [(round((w["t"] - t0) / NOM), w["t"]) for w in gw]
            aw, bw = fit(pw) if len(pw) > 2 else (0, b)
            print("  waiter wakes, least squares period %.4f ns (%+.1f ppm vs the pre-gap fit)" % (bw, (bw / b - 1) * 1e6))
        pw = [w for w in wake if w["t"] < last["t"]]
        if pw:
            d = sorted((w["t"] - (a + b * round((w["t"] - t0) / NOM))) / 1e3 for w in pw)
            print("  waiter wakes before the gap: %d, wake minus the OS line: p10 %.0f, median %.0f, p90 %.0f us" % (
                len(d), d[len(d) // 10], d[len(d) // 2], d[(9 * len(d)) // 10]))




def dwm(path):
    """DWM's qpcVBlank (src 3) against the OS times and the grid."""
    rows = [r for r in csv.DictReader(open(path))]
    for r in rows:
        for k in r:
            r[k] = float(r[k])
    comp = sorted([r for r in rows if r["src"] == 0 and r["t"] > 0], key=lambda r: r["t"])
    dw = [r for r in rows if r["src"] == 3]
    if not dw:
        return
    gi = max(range(1, len(comp)), key=lambda i: comp[i]["t"] - comp[i - 1]["t"])
    pre, post = comp[:gi], comp[gi:]
    t0 = pre[0]["t"]
    a, b = fit([(round((r["t"] - t0) / NOM), r["t"]) for r in pre])
    a2, b2 = fit([(round((r["t"] - t0) / NOM), r["t"]) for r in post[20:]])
    def off(t, aa, bb):
        return (t - (aa + bb * round((t - aa) / bb))) / 1e3
    dpre = [d for d in dw if d["t"] < pre[-1]["t"]]
    dgap = [d for d in dw if pre[-1]["t"] < d["t"] < post[0]["t"]]
    dpost = [d for d in dw if d["t"] > post[20]["t"]]
    stale = sum(1 for d in dw if d["t_call"] - d["t"] > 2 * NOM)
    print("  DWM samples %d (stale, older than 2 periods when read: %d)" % (len(dw), stale))
    if dpre:
        o = sorted(off(d["t"], a, b) for d in dpre)
        print("  DWM vblank minus the pre-gap OS line: p10 %.1f median %.1f p90 %.1f us (%d)" % (o[len(o) // 10], o[len(o) // 2], o[(9 * len(o)) // 10], len(o)))
    if dgap:
        o = [off(d["t"], a, b) for d in dgap]
        o2 = [off(d["t"], a2, b2) for d in dgap]
        print("  DWM in the gap minus the pre line: first %.1f last %.1f; minus the post line: first %.1f last %.1f (%d)" % (o[0], o[-1], o2[0], o2[-1], len(o)))
    if dpost:
        o = sorted(off(d["t"], a2, b2) for d in dpost)
        print("  DWM after the gap minus the post OS line: p10 %.1f median %.1f p90 %.1f us" % (o[len(o) // 10], o[len(o) // 2], o[(9 * len(o)) // 10]))



if __name__ == "__main__":
    for p in sys.argv[1:]:
        main(p)
        dwm(p)
