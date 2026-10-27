"""The doc's tables: one row per CSV, labeled from the file name."""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze as A

def wtcpu(csv):
    log = csv[:-4] + ".log"
    try:
        m = re.search(r"wt_cpu_ms=([-0-9.]+) over_s=([0-9.]+)", open(log, encoding="utf-8", errors="replace").read())
        if m and not m.group(1).startswith("-"):
            return "%.0f ms in %.0f s" % (float(m.group(1)), float(m.group(2)))
    except OSError:
        pass
    return "-"

print("| Host | N | Window | Design | Frames | Missed (vblanks) | Late target | Occluded | Residual us p50 / p99 | Work ms p50 / p99 / max | Path, tier | Window thread CPU |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|")
for p in sys.argv[1:]:
    b = os.path.basename(p)[:-4]
    host, mode, n, wt = re.match(r"(c|py|m|o)_(fs|win)_(\d+)_wt(\d)", b).groups()
    s = A.summarize(p)
    paths = ", ".join("%s %d" % kv for kv in sorted(s["paths"].items()))
    tiers = ", ".join("t%s %d" % kv for kv in sorted(s["tiers"].items()))
    print("| %s | %s | %s | %s | %d | %d (%d) | %d | %d | %.1f / %.1f | %.2f / %.2f / %.1f | %s; %s | %s |" % (
        {"c": "C", "py": "Python 3.12", "m": "MATLAB R2023a", "o": "Octave 10.1"}[host], n, "fullscreen" if mode == "fs" else "1280 x 720",
        "ON" if wt == "1" else "OFF", s["frames"], s["missed"], s["dropped"], s["late"], s["occ"],
        s["res"][0], s["res"][1], s["work"][0], s["work"][1], s["work"][2], paths, tiers, wtcpu(p)))
