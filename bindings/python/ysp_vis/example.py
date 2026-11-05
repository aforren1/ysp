"""A frame loop with ysp-vis: a square that steps across the screen for 3 s.

    python example.py              # a 1280 x 720 window
    python example.py --full       # the full screen
    python example.py --sim        # the simulated display: no window

Shift+Esc ends it early. Prints the describe line and a summary of the flip
records: misses, late targets, the residuals and the paths.
"""
import gc
import sys

import ysp.gfx as ygfx
import ysp.screen as yscr

opts = {"windowed": True, "size": (1280, 720)}
if "--full" in sys.argv:
    opts = {}
if "--sim" in sys.argv:
    opts = {"backend": "sim"}

with yscr.Screen(**opts) as s, ygfx.Gfx(s, background=0.5) as g:
    print(s.describe())
    w, h = s.size
    if "--sim" not in sys.argv:
        s.title("ysp-vis example")
    gc.freeze()                       # after setup: no full collection of the setup's heap in a frame
    try:
        t_end = None
        while True:
            f = s.begin()             # the predicted onset of this frame's flip
            if t_end is None:
                t_end = f.onset + 3.0
            if f.onset >= t_end:
                s.flip()
                break
            x = ((f.onset - (t_end - 3.0)) / 3.0 - 0.5) * (w - 100)
            g.begin()
            g.rect(x, 0, 100, 100, (1.0, 1.0, 1.0))
            g.end()
            s.flip()                  # presents at f.onset
    except yscr.Abort as a:
        print(f"aborted: reasons 0x{a.reasons:x}, {a.presses:d} press(es)")
    s.wait_flip()
    records = s.records()             # every flip since open, oldest first
    missed = sum(r.missed for r in records)
    late = sum(r.late for r in records)
    res = sorted(abs(r.residual) for r in records)
    paths = {}
    for r in records:
        paths[r.path] = paths.get(r.path, 0) + 1
    p50 = 1e6 * res[len(res) // 2] if res else 0
    worst = 1e6 * res[-1] if res else 0
    print(f"{len(records):d} records, {missed:d} missed, {late:d} late targets, residual p50 {p50:.1f} us, "
          f"max {worst:.1f} us, paths {paths}")
