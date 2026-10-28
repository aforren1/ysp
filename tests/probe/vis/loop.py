"""The minimal ysp-vis frame in Python, for the 10-minute loop and the idle gap.

    python loop.py [--windowed] [--seconds 600] [--idle 30] [--csv out.csv]

Each frame: begin(), Gfx.begin(), one rect(), Gfx.end(), flip(); the
frame's completed records kept in a list for the whole run, as a script
keeps its data. With --idle S the loop stops calling the module for S
seconds after frame 120 (time.sleep), then goes on. Writes one CSV row per
flip record (table.py reads them). gc.freeze() after setup. Shift+Esc ends
a run.
"""
import argparse
import gc
import time

import ysp.gfx as ygfx
import ysp.screen as yscr

TITLE = "ysp-vis probe"

ap = argparse.ArgumentParser()
ap.add_argument("--windowed", action="store_true")
ap.add_argument("--seconds", type=float, default=600.0)
ap.add_argument("--idle", type=float, default=0.0)
ap.add_argument("--csv", default="py.csv")
a = ap.parse_args()

opts = {"windowed": True, "size": (1280, 720)} if a.windowed else {}
rows = []
with yscr.Screen(**opts) as s, ygfx.Gfx(s, background=0.5) as g:
    s.title(TITLE)
    line = s.describe()
    w, h = s.size
    gc.freeze()
    t_end = None
    try:
        while True:
            f = s.begin()
            if t_end is None:
                t_end = f.onset + a.seconds + a.idle
            rows.extend(f.done)
            if f.onset >= t_end:
                s.flip()
                break
            x = (f.index % 120) / 120.0 * (w - 100) - (w - 100) / 2.0
            g.begin()
            g.rect(x, 0, 100, 100, 1.0)
            g.end()
            s.flip()
            if a.idle > 0 and f.index == 119:
                time.sleep(a.idle)      # no call into the module: the window thread keeps the window alive
    except yscr.Abort as e:
        print(f"abort 0x{e.reasons:x}")
    s.wait_flip()
    last = rows[-1].index if rows else -1
    rows.extend(r for r in s.records() if r.index > last)

with open(a.csv, "w", newline="\n") as out:
    out.write(f"# python {line}\n")
    out.write("index,target_ns,planned_ns,onset_ns,residual_ns,dropped,path,tier,flags,draw_ns,swap_ns\n")
    for r in rows:
        ph = r.phases
        draw = -1 if ph[yscr.PHASE_DRAW] is None else round(ph[yscr.PHASE_DRAW] * 1e9)
        swap = -1 if ph[yscr.PHASE_SWAP] is None else round(ph[yscr.PHASE_SWAP] * 1e9)
        out.write(f"{r.index:d},{r.target_ns:d},{r.planned_ns:d},{r.onset_ns:d},{r.residual_ns:d},{r.dropped:d},"
                  f"{r.path:d},{r.tier:d},{r.flags:d},{draw:d},{swap:d}\n")
print(f"{len(rows):d} records -> {a.csv}")
