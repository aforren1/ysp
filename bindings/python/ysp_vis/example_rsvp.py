"""A rapid serial visual presentation (RSVP) stream with ysp-vis: 20 items at 10 per second.

    python example_rsvp.py          # a 1280 x 720 window; press Space at the vertical grating
    python example_rsvp.py --full   # the full screen
    python example_rsvp.py --sim    # the simulated display: no window

Needs NumPy (the items are made with it). The 20 items are 64 x 64 patches
side by side in one texture (an atlas); one image stimulus shows one patch
at a time through its source rectangle. A timeline track steps the
rectangle's x to the next patch every 0.1 s (STEP keys), bound to the
image's src_x, so the stream runs without a Python call per item or per
frame: the frame loop is tl.evaluate(), g.frame(), s.flip(). The flip
records then give the measured onset of each item.
"""
import gc
import sys

import numpy as np

import ysp.gfx as ygfx
import ysp.screen as yscr
import ysp.timeline as ytl

SIM = "--sim" in sys.argv
opts = {"backend": "sim"} if SIM else {} if "--full" in sys.argv else {"windowed": True, "size": (1280, 720)}
N, SIDE, SOA = 20, 64, 0.1
SPACE = 0x20                                   # SDL keycode

rng = np.random.default_rng(7)
target = int(rng.integers(8, 16))
yy, xx = np.mgrid[0:SIDE, 0:SIDE] - SIDE / 2 + 0.5
aperture = np.hypot(xx, yy) < SIDE / 2 - 1
patches = []
for i in range(N):
    ori = 0.0 if i == target else rng.uniform(20, 160)       # degrees from vertical
    t = np.deg2rad(ori)
    g = np.cos(2 * np.pi * (xx * np.cos(t) + yy * np.sin(t)) / 8.0)
    patches.append(np.where(aperture, 0.5 + 0.4 * g, 0.5))
atlas = np.repeat((np.hstack(patches) * 255 + 0.5).astype(np.uint8)[:, :, None], 3, axis=2)   # 64 x 1280 gray, linear

with yscr.Screen(gfx={"background": 0.5}, **opts) as s:
    gfx = ygfx.Gfx(s)
    tex = gfx.texture(atlas)
    item = gfx.image(tex, src=(0, 0, SIDE, SIDE), w=4 * SIDE, h=4 * SIDE)   # nearest: each texel 4 x 4 px
    fix = gfx.shape("circle", w=6, color=0.0)
    scene = gfx.list([item, fix])
    tl = ytl.Timeline(["src_x", "on"])
    tl.set_keys("src_x", 1, [(k * SOA, float(k * SIDE), "step") for k in range(N)])
    tl.set_keys("on", 1, [(0.0, 1.0, "step"), (N * SOA, 0.0, "step")])
    gfx.bind(item, "src_x", tl, "src_x")
    gfx.bind(item, "visible", tl, "on")
    gfx.prime()
    gc.freeze()
    try:
        f = s.begin()
        start = f.onset + 0.5
        tl.anchor(1, start)
        records, presses = [], []
        while f.onset < start + N * SOA + 0.5:
            tl.evaluate(f.onset, f.period)
            gfx.frame(scene)
            s.flip()
            f = s.begin()
            records.extend(f.done)
            presses += [e.t for e in s.poll() if e.kind == yscr.KIND_KEYBOARD and e.type == yscr.PRESS and e.code == SPACE]
        gfx.frame(scene)
        s.flip()
        s.wait_flip()
        records.extend(s.records())
    except yscr.Abort:
        print("aborted")
        sys.exit(1)
    # each item's first frame and its measured onset
    shown = [r for r in records if start - 0.002 <= r.target < start + N * SOA]
    firsts = {}
    for r in shown:
        k = int((r.target - start + 0.002) // SOA)
        firsts.setdefault(k, r)
    soas = [firsts[k + 1].onset - firsts[k].onset for k in range(N - 1) if k in firsts and k + 1 in firsts]
    print("%d items, target at %d; measured SOA %.4f to %.4f s; %d missed frames"
          % (len(firsts), target, min(soas), max(soas), sum(r.missed for r in shown)))
    if presses and target in firsts:
        print("Space %.3f s after the target's onset" % (presses[0] - firsts[target].onset))
    gfx.unbind()
    gfx.close()
