"""The before/after table of the grid after a gap: run_gaps.sh's logs and CSVs in DIR,
one row per run; the header column is the run's TAG.
    uv run --no-project python d3table.py DIR"""
import csv, glob, os, re, sys

NOM = 256880 / 15413000 * 1e9
D = sys.argv[1]
rows_out = []
for log in sorted(glob.glob(os.path.join(D, "*.log"))):
    name = os.path.basename(log)[:-4]
    m = re.match(r"(comp|dxgi)_(fs|win)_(\d+)_(on|off)_v(\w+?)(_rep)?$", name)
    if not m:
        continue
    be, mode, gap, wt, v, rep = m.groups()
    txt = open(log, encoding="utf-8", errors="replace").read()
    un = re.search(r"GRID_UNSTABLE records (\d+)", txt)
    per = re.search(r"period [0-9.]+ ns \(([-+0-9.]+) ppm", txt)
    ga = re.search(r"gap_anchors=(\d+)\(max (\d+)us, refused (\d+)\)", txt)
    path = log[:-4] + ".csv"
    first = maxerr = float("nan")
    if os.path.exists(path):
        r = [x for x in csv.DictReader(open(path)) if x["src"] == "0" and float(x["t"]) > 0]
        r.sort(key=lambda x: float(x["t"]))
        ts = [float(x["t"]) for x in r]
        gi = max(range(1, len(ts)), key=lambda i: ts[i] - ts[i - 1])
        post = r[gi:]
        first = float(post[0]["off_ns"]) / 1e3
        offs = [abs(float(x["off_ns"])) for x in post]
        maxerr = max([o for o in offs if o > NOM / 100] or [0]) / 1e3
    rows_out.append((be, mode, int(gap), wt, v, rep or "", un.group(1) if un else "?", per.group(1) if per else "?",
                     "%.0f" % first, "%.0f" % maxerr, ("%s (%s us)" % (ga.group(1), ga.group(2))) if ga else "0"))
print("| Backend | Window | Gap s | Window thread | Header | Period before the gap, ppm | First OS time after the gap, us off the grid | GRID_UNSTABLE records after | Largest onset error, us | Gap anchors (move) |")
print("|---|---|---|---|---|---|---|---|---|---|")
for r in sorted(rows_out, key=lambda r: (r[0], r[1], r[2], r[3], r[5], r[4])):
    be, mode, gap, wt, v, rep, un, per, first, maxerr, ga = r
    print("| %s | %s | %d%s | %s | %s | %s | %s | %s | %s | %s |" % (
        "COMPOSITION" if be == "comp" else "DXGI_FLIP", "fullscreen" if mode == "fs" else "1280 x 720", gap,
        " (again)" if rep else "", wt.upper(), v, per, first, un, maxerr, ga))
