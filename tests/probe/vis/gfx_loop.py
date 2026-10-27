"""N gabors with a bound tween in Python, for the 10-minute ysp-vis gfx loop.

    python gfx_loop.py --n 1000 --mode list|instances [--seconds 600] [--windowed] [--csv out.csv]

Each frame: Screen.begin(), Gfx.frame(scene), Screen.flip(): three calls,
whatever N. The scene is N gabors (sigma 6 px, 48 px across) on a grid over
the screen, as one DrawList of N stimuli (--mode list) or one Instances
stimulus of N elements (--mode instances). A ysp.timeline Timeline drives
the contrast (a yoyo tween, 0.1 to 0.5 in 1 s) and the phase (1 cycle/s)
of every gabor through Gfx.bind(), and the Gfx evaluates it at each
frame's predicted onset (Gfx.timeline()): no Python call per stimulus.
The frame's completed records are kept for the run, as a script keeps its
data; one CSV row per flip record (table.py reads them). gc.freeze() after
setup. Shift+Esc ends a run.
"""
import argparse
import gc

import ysp.gfx as ygfx
import ysp.screen as yscr
import ysp.timeline as ytl

TITLE = "ysp-vis gfx probe"
SPACING = 48

ap = argparse.ArgumentParser()
ap.add_argument("--n", type=int, default=100)
ap.add_argument("--mode", choices=("list", "instances"), default="list")
ap.add_argument("--seconds", type=float, default=600.0)
ap.add_argument("--windowed", action="store_true")
ap.add_argument("--csv", default="gfx.csv")
a = ap.parse_args()

opts = {"windowed": True, "size": (1280, 720)} if a.windowed else {}
rows = []
with yscr.Screen(gfx={"background": 0.5}, **opts) as s, ygfx.Gfx(s) as g:
    s.title(TITLE)
    w, h = g.size
    cols = int(w // SPACING)
    xy = [((i % cols) * SPACING - w / 2 + SPACING / 2, (i // cols) * SPACING - h / 2 + SPACING / 2) for i in range(a.n)]
    tl = ytl.Timeline(["contrast", "phase"])
    tl.tween("contrast", 0.5, base=1, start=0.0, duration=1.0, from_=0.1, cycles="forever", yoyo=True, ease="sine.inOut")
    tl.tween("phase", a.seconds + 60.0, base=1, start=0.0, duration=a.seconds + 60.0, from_=0.0, ease="linear")
    if a.mode == "list":
        scene = g.list([g.gabor(sf=1 / 12, sigma=6, x=x, y=y) for x, y in xy])
        g.bind(scene, "contrast", tl, "contrast")          # one channel for every item
        g.bind(scene, "phase", tl, "phase")
    else:
        tmpl = g.gabor(sf=1 / 12, sigma=6)
        scene = g.instances(tmpl, a.n, fields=("xy",))
        scene.set_elements("xy", xy)
        g.bind(scene, "contrast", tl, "contrast")          # the template's: every element
        g.bind(scene, "phase", tl, "phase")
    g.timeline(tl)
    g.prime()
    line = "%s; gfx %s; n %d %s" % (s.describe(), ygfx.__version__, a.n, a.mode)
    gc.freeze()
    t_end = None
    try:
        while True:
            f = s.begin()
            if t_end is None:
                tl.anchor(1, f.onset)
                t_end = f.onset + a.seconds
            rows.extend(f.done)
            if f.onset >= t_end:
                s.flip()
                break
            g.frame(scene)
            s.flip()
    except yscr.Abort as e:
        print("abort 0x%x" % e.reasons)
    s.wait_flip()
    last = rows[-1].index if rows else -1
    rows.extend(r for r in s.records() if r.index > last)
    g.unbind()
    g.timeline(tl, evaluate=False)

with open(a.csv, "w", newline="\n") as out:
    out.write("# python %s\n" % line)
    out.write("index,target_ns,planned_ns,onset_ns,residual_ns,dropped,path,tier,flags,draw_ns,swap_ns\n")
    for r in rows:
        ph = r.phases
        out.write("%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n" % (
            r.index, r.target_ns, r.planned_ns, r.onset_ns, r.residual_ns, r.dropped, r.path, r.tier, r.flags,
            -1 if ph[yscr.PHASE_DRAW] is None else round(ph[yscr.PHASE_DRAW] * 1e9),
            -1 if ph[yscr.PHASE_SWAP] is None else round(ph[yscr.PHASE_SWAP] * 1e9)))
print("%d records -> %s" % (len(rows), a.csv))
