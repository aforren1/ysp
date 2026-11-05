"""A two-alternative forced-choice grating trial with ysp-vis and ysp.timeline.

    python example_2afc.py          # a 1280 x 720 window; answer with the Left and Right arrows
    python example_2afc.py --full   # the full screen
    python example_2afc.py --sim    # the simulated display: no window, random answers

Each trial: a fixation dot for 0.5 s, then two gratings, left and right,
for 0.2 s with a 50 ms contrast ramp on and off; one of them has the higher
contrast. Answer which one. The fixation dot turns green or red. The
timeline builds the trial (events and tweens on base 1); the frame loop is
three calls: tl.evaluate(), g.frame(scene), s.flip(). The gratings' gate and
contrasts are bound to the timeline's channels, so no Python call sets a
stimulus in a frame. Each trial prints the response, the reaction time from
the stimulus onset that the flip record measured, and the onset's residual.
"""
import gc
import random
import sys

import ysp.gfx as ygfx
import ysp.screen as yscr
import ysp.timeline as ytl

SIM = "--sim" in sys.argv
opts = {"backend": "sim"} if SIM else {} if "--full" in sys.argv else {"windowed": True, "size": (1280, 720)}
LEFT, RIGHT = 0x40000050, 0x4000004F           # SDL keycodes of the arrow keys
TRIAL = 1                                       # the timeline's base: trial time
BASE = 0.2                                      # the lower contrast; the other is BASE + DELTA

with yscr.Screen(gfx={"background": 0.5}, **opts) as s:
    g = ygfx.Gfx(s)
    pair = g.group()                            # the two gratings: one gate for both
    left = g.grating(x=-200, w=200, sf=1 / 20, aperture="circle", edge="cosine", edge_width=12, group=pair)
    right = left.copy()
    right.x = 200
    fix = g.shape("circle", w=10, color=0.0)
    scene = g.list([left, right, fix])

    tl = ytl.Timeline(["gate", "c_left", "c_right"])
    g.bind(pair, "opacity", tl, "gate")         # a group's opacity multiplies its members' gates
    g.bind(left, "contrast", tl, "c_left")
    g.bind(right, "contrast", tl, "c_right")
    g.prime()
    gc.freeze()

    try:
        for trial, delta in enumerate([0.15, 0.1, 0.05, 0.1, 0.15]):
            side = random.choice((0, 1))
            tl.clear(TRIAL)
            q = tl.seq(TRIAL)
            q.set("c_left", BASE + (delta if side == 0 else 0)).set("c_right", BASE + (delta if side == 1 else 0))
            q.wait(0.5).mark(1)                 # mark 1: the stimulus onset
            q.then("gate", 1.0, duration=0.05, ease="cosine").wait(0.1).then("gate", 0.0, duration=0.05, ease="cosine")
            fix.color = 0.0
            f = s.begin()
            tl.anchor(TRIAL, f.onset)
            onset_frame, onset, answer = None, None, None
            while answer is None:
                for e in tl.evaluate(f.onset, f.period):
                    if e.kind == ytl.MARK and e.code == 1:
                        onset_frame = f.index      # the flip record of this frame gives the onset
                g.frame(scene)
                s.flip()
                f = s.begin()
                for r in f.done:
                    if r.index == onset_frame:
                        onset = r                   # the measured onset of the stimulus frame
                for e in s.poll():
                    if e.kind == yscr.KIND_KEYBOARD and e.type == yscr.PRESS and e.code in (LEFT, RIGHT) and onset:
                        answer = (0 if e.code == LEFT else 1, e.t)
                if SIM and onset and f.onset - onset.onset > 0.4:
                    answer = (random.choice((0, 1)), f.onset)
            correct = answer[0] == side
            fix.color = (0.0, 0.8, 0.0) if correct else (0.8, 0.0, 0.0)
            g.frame(scene)
            s.flip()
            where = "left" if side == 0 else "right"
            verdict = "correct" if correct else "wrong"
            print(f"trial {trial:d}: delta {delta:.2f}, {where}, {verdict}, RT {answer[1] - onset.onset:.3f} s, "
                  f"onset residual {onset.residual * 1e6:+.1f} us")
            for _ in range(30):                 # 0.5 s of feedback
                s.begin()
                g.frame(scene)
                s.flip()
    except yscr.Abort:
        print("aborted")
    g.unbind()
    g.close()
