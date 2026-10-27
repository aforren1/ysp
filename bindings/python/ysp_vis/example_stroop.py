"""The Stroop task with ysp-vis: name the ink color of a color word.

    python example_stroop.py          # a 1280 x 720 window
    python example_stroop.py --full   # the full screen
    python example_stroop.py --sim    # the simulated display: no window, no keys

12 trials. Each one: a fixation cross for 0.5 s, then RED, GREEN or BLUE in
red, green or blue ink until a key or 2 s. Press R for red ink, G for green,
B for blue. Half of the trials are congruent (the word names its ink).

One Text shows every word: between trials its text and color change, which
lays it out again (Skribidi and HarfBuzz; the glyphs of all three words
are built at setup, so a change builds none). The script prints what each
change cost. The word's onset is the flip record's (measured), and a
reaction time is the key's time minus that onset. Colors are linear device
RGB, not calibrated.
"""
import gc
import random
import sys
import time

import ysp.gfx as ygfx
import ysp.screen as yscr

SIM = "--sim" in sys.argv
opts = {"backend": "sim"} if SIM else {} if "--full" in sys.argv else {"windowed": True, "size": (1280, 720)}
INK = {"red": (0.6, 0.05, 0.05), "green": (0.05, 0.45, 0.05), "blue": (0.08, 0.1, 0.7)}
KEYS = {ord("r"): "red", ord("g"): "green", ord("b"): "blue"}   # SDL keycodes
FIX, LIMIT = 0.5, 2.0

rng = random.Random(7)
trials = [(w, w) for w in INK] * 2 + [(w, i) for w in INK for i in INK if i != w]   # 6 congruent, 6 not
rng.shuffle(trials)

with yscr.Screen(gfx={"background": 0.5}, **opts) as s:
    g = ygfx.Gfx(s)
    fix = g.shape("cross", w=24, h=24, width=3, color=0.0)
    word = g.text("RED GREEN BLUE", size=64)            # builds every glyph the trials use
    g.prime()
    gc.freeze()
    rows = []
    try:
        for n, (w, ink) in enumerate(trials):
            t0 = time.perf_counter()
            word.set(text=w.upper(), color=INK[ink])     # a layout, between frames
            change_s = time.perf_counter() - t0
            # fixation
            f = s.begin()
            end_fix = f.onset + FIX
            while f.onset < end_fix - f.period / 2:
                g.frame(fix)
                s.flip()
                f = s.begin()
            # the word: its first frame's measured onset, then keys until a response or the limit
            s.poll()                                     # drop the keys of the fixation
            g.frame(word)
            s.flip()
            onset = s.wait_flip().onset
            response, rt = None, None
            while True:
                f = s.begin()
                for e in s.poll():
                    if response is None and e.kind == yscr.KIND_KEYBOARD and e.type == yscr.PRESS and e.code in KEYS:
                        response, rt = KEYS[e.code], e.t - onset
                if response is not None or f.onset > onset + LIMIT:
                    break
                g.frame(word)
                s.flip()
            g.frame()                                    # a blank frame ends the trial
            s.flip()
            rows.append((n, w, ink, w == ink, response, rt, response == ink, change_s, word.layout_time))
    except yscr.Abort:
        print("aborted")
    g.close()

print("trial word  ink    congruent response rt_s   correct change_ms layout_ms")
for n, w, ink, c, r, rt, ok, ch, lay in rows:
    print("%5d %-5s %-6s %-9s %-8s %-6s %-7s %9.3f %9.3f" % (n, w, ink, c, r, "%.3f" % rt if rt else "-", ok,
                                                            1e3 * ch, 1e3 * lay))
done = [r for r in rows if r[5] is not None and r[6]]
for label, cong in (("congruent", True), ("incongruent", False)):
    rts = sorted(r[5] for r in done if r[3] == cong)
    if rts:
        print("%s: median RT %.3f s over %d correct trials" % (label, rts[len(rts) // 2], len(rts)))
