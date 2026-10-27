"""The cost of a text's layout through ysp.gfx, on the simulated display.

    python text_cost.py [--reps 2000] [--csv out.csv]

For a word, a sentence, a paragraph (wrapped at 600 px) and an Arabic
sentence, one Text is set alternately to two strings of the same class
(their glyphs built once at setup, as a trial loop would have them), reps
times each: the wall time of the Python call (text.text = s: the layout,
the conversion into runs, the upload of no new glyph) and the layout's own
time (Text.layout_time: Skribidi and the conversion). Then the first layout
of a paragraph in a new font (every glyph built and uploaded). Prints
medians, p90 and max in microseconds; the simulated display draws nothing,
so this is CPU time.
"""
import argparse
import os
import statistics
import time

import ysp.gfx as ygfx
import ysp.screen as yscr

FONTS = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")
LOREM = ("Reading research shows text in short lines with clear spacing. The eyes jump from word to word, and "
         "each fixation takes in a few letters to the left and more to the right. Long words are fixated more "
         "often, and frequent words are skipped. ")
CASES = [
    ("word", dict(size=64), "GREEN", "BLUE"),
    ("sentence", dict(size=32), "The quick brown fox jumps over the lazy dog.", "A lazy dog sleeps under the old brown tree."),
    ("paragraph", dict(size=20, width=600, align="start"), (LOREM * 3)[:520], (LOREM * 3)[40:560]),
    ("arabic", dict(size=32), "مرحبا بالعالم وأهلا بكم في القراءة", "القراءة السريعة للكلمات في الجملة"),
]

ap = argparse.ArgumentParser()
ap.add_argument("--reps", type=int, default=2000)
ap.add_argument("--rounds", type=int, default=3)
ap.add_argument("--csv", default="")
a = ap.parse_args()


def stats(v):
    v = sorted(v)
    return statistics.median(v), v[int(0.9 * (len(v) - 1))], v[-1]


rows = []
with yscr.Screen(backend="sim") as s:
    g = ygfx.Gfx(s)
    f = g.font(os.path.join(FONTS, "segoeui.ttf"))
    texts = []
    for name, kw, s1, s2 in CASES:
        t = g.text(s1, font=f, **kw)
        t.text = s2                                     # both strings' glyphs built
        texts.append(t)
    # rounds interleaved over the cases, so a change of clock or core (the
    # i7-1360P has P and E cores) lands on every case alike
    calls = [[] for _ in CASES]
    lays = [[] for _ in CASES]
    for r in range(a.rounds):
        for c, (name, kw, s1, s2) in enumerate(CASES):
            t = texts[c]
            for i in range(a.reps // a.rounds):
                x = s1 if i % 2 == 0 else s2
                t0 = time.perf_counter()
                t.text = x
                calls[c].append(time.perf_counter() - t0)
                lays[c].append(t.layout_time)
    for c, (name, kw, s1, s2) in enumerate(CASES):
        rows.append((name, len(s1), texts[c].n_lines, stats(calls[c]), stats(lays[c])))
    # the first layout of a paragraph in a font with no glyph built yet
    f2 = g.font(os.path.join(FONTS, "arial.ttf"))
    t0 = time.perf_counter()
    p = g.text(CASES[2][2], font=f2, **CASES[2][1])
    first = time.perf_counter() - t0
    g.close()

print("%-10s %6s %5s  %-28s %-28s" % ("text", "chars", "lines", "call us p50/p90/max", "layout_time us p50/p90/max"))
for name, n, nl, c, l in rows:
    print("%-10s %6d %5d  %8.1f %8.1f %9.1f  %8.1f %8.1f %9.1f" % (name, n, nl, *(1e6 * x for x in c), *(1e6 * x for x in l)))
print("first paragraph in a new font (Arial): %.2f ms, of it layout %.3f ms, glyph builds %.3f ms, upload %.3f ms, %d glyphs"
      % (1e3 * first, 1e3 * p.layout_time, 1e3 * p.build_time, 1e3 * p.upload_time, p.new_glyphs))
if a.csv:
    with open(a.csv, "w", newline="\n") as out:
        out.write("host,text,chars,lines,call_p50_us,call_p90_us,call_max_us,layout_p50_us,layout_p90_us,layout_max_us\n")
        for name, n, nl, c, l in rows:
            out.write("python,%s,%d,%d,%s\n" % (name, n, nl, ",".join("%.2f" % (1e6 * x) for x in c + l)))
