"""The time from Screen() to a Gfx that can draw, with and without the open
started while the screen settles (Screen(gfx=...)), cold and warm program
cache. ysp/gfx.h measured the same in C (OPEN START); this is the binding.

    python gfx_open.py [--rounds 3] [--full]

Prints one line per open: the mode, the cache, seconds from Screen() to the
end of Gfx() (prime included), and the describe line's open_start.
"""
import argparse
import os
import shutil
import tempfile
import time

import ysp.gfx as ygfx
import ysp.screen as yscr

ap = argparse.ArgumentParser()
ap.add_argument("--rounds", type=int, default=3)
ap.add_argument("--full", action="store_true")
a = ap.parse_args()
win = {} if a.full else {"windowed": True, "size": (1280, 720)}


def one(started, cache):
    t0 = time.perf_counter()
    if started:
        s = yscr.Screen(gfx={"background": 0.5, "cache": cache}, **win)
        g = ygfx.Gfx(s)
    else:
        s = yscr.Screen(**win)
        g = ygfx.Gfx(s, background=0.5, cache=cache)
    t = time.perf_counter() - t0
    line = g.describe()
    mode = line[line.find("open_start="):].split(",")[0].split(" ")[0] if "open_start=" in line else "?"
    p = g.programs()
    g.close()
    s.close()
    return t, mode, p["loaded"], p["compiled"]


for r in range(a.rounds):
    d = tempfile.mkdtemp(prefix="ysp_gfx_open_")
    try:
        for started in (False, True):
            shutil.rmtree(d, ignore_errors=True)
            os.makedirs(d)
            t, mode, ld, cp = one(started, d)            # a cold cache: an empty folder
            print("round %d %-8s cold: %.3f s (%s; %d loaded, %d compiled)" % (r, "started" if started else "after", t, mode, ld, cp))
            t, mode, ld, cp = one(started, d)            # the same folder again: warm
            print("round %d %-8s warm: %.3f s (%s; %d loaded, %d compiled)" % (r, "started" if started else "after", t, mode, ld, cp))
    finally:
        shutil.rmtree(d, ignore_errors=True)
