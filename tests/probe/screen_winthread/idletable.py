"""The idle-gap table of Part 1 (short/idle_*): hung and ghost times from the
watcher, and the frames after the gap (docs/winthread_probe.md's definition)."""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze as A

D = sys.argv[1]


def watch(txt):
    part = txt.split("idle ")[1].split("after idle")[0] if "idle " in txt else ""
    hung = ghost = None
    for line in part.splitlines():
        m = re.match(r"([0-9.]+) hung=(\d) fg_ours=(\d) fg_ghost=(\d)", line)
        if not m:
            continue
        t, h, _, g = float(m.group(1)), m.group(2), m.group(3), m.group(4)
        if h == "1" and hung is None:
            hung = t
        if g == "1" and ghost is None:
            ghost = t
    return ("%.2f s" % hung if hung is not None else "never") + ", " + ("%.2f s" % ghost if ghost is not None else "never")


print("| Gap | Window | Host | OFF: hung, ghost | OFF: frame 120; bad frames | ON: hung, ghost | ON: frame 120; bad frames |")
print("|---|---|---|---|---|---|---|")
for gap in (5, 10, 30):
    for mode in ("fs", "win"):
        for host in ("c", "py"):
            cells = []
            for wt in (0, 1):
                base = os.path.join(D, "idle_%s_%d_%s_wt%d" % (host, gap, mode, wt))
                try:
                    txt = open(base + ".log", encoding="utf-8", errors="replace").read()
                    g = A.gap(base + ".csv", 120)
                    g = re.sub(r"^(frame \d+: )wait [0-9.]+ ms, ", r"\1", g)
                    cells += [watch(txt), g]
                except (OSError, IndexError, ValueError) as e:
                    cells += ["?", "? %s" % e]
            print("| %d s | %s | %s | %s | %s | %s | %s |" % (gap, "fullscreen" if mode == "fs" else "1280 x 720",
                                                         "C" if host == "c" else "Python", *cells))
