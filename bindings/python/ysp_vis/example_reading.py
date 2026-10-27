"""Reading by rapid serial visual presentation (RSVP) with ysp-vis: a sentence, one word every 0.25 s.

    python example_reading.py          # a 1280 x 720 window
    python example_reading.py --full   # the full screen
    python example_reading.py --sim    # the simulated display: no window

Each word is a Text laid out at setup, centered on fixation. A timeline
channel per word, bound to the word's visible, turns it on for its 0.25 s,
so the stream runs without a Python call per word or per frame: the loop
is tl.evaluate(), g.frame(), s.flip(). After the stream the whole sentence
is shown on one line, with each word's box in screen pixels (words()), as an
eye tracker's interest areas would take them. The flip records give each
word's measured onset.
"""
import gc
import sys

import ysp.gfx as ygfx
import ysp.screen as yscr
import ysp.timeline as ytl

SIM = "--sim" in sys.argv
opts = {"backend": "sim"} if SIM else {} if "--full" in sys.argv else {"windowed": True, "size": (1280, 720)}
SENTENCE = "The quick brown fox jumps over the lazy dog near the riverbank"
SOA, SIZE = 0.25, 48

words = SENTENCE.split()
with yscr.Screen(gfx={"background": 0.5}, **opts) as s:
    g = ygfx.Gfx(s)
    font = g.font(r"C:\Windows\Fonts\segoeui.ttf")
    shown = g.list([g.text(w, font=font, size=SIZE, color=0.0, visible=0) for w in words])
    assert all(t.missing == 0 for t in shown)
    tl = ytl.Timeline(len(words))
    for k in range(len(words)):
        tl.set_keys(k, 1, [(k * SOA, 1.0, "step"), ((k + 1) * SOA, 0.0, "step")])
    g.bind(shown, "visible", tl, list(range(len(words))))     # one channel per word
    sentence = g.text(SENTENCE, font=font, size=32, color=0.0, width=0.9 * g.size[0])
    g.prime()
    gc.freeze()
    records = []
    try:
        f = s.begin()
        start = f.onset + 0.5
        tl.anchor(1, start)
        while f.onset < start + len(words) * SOA + 0.25:
            tl.evaluate(f.onset, f.period)
            g.frame(shown)
            s.flip()
            f = s.begin()
            records.extend(f.done)
        g.frame(sentence)
        s.flip()
        s.wait_flip()
        records.extend(s.records())
    except yscr.Abort:
        print("aborted")
        sys.exit(1)
    boxes = sentence.words()
    g.unbind()
    g.close()

first = {}
for r in records:
    k = int((r.target - start + 0.002) // SOA)
    if 0 <= k < len(words) and r.target >= start - 0.002:
        first.setdefault(k, r)
on = [first[k].onset for k in sorted(first)]
soas = [b - a for a, b in zip(on, on[1:])]
print("%d words; measured SOA %.4f to %.4f s; %d missed frames" % (len(first), min(soas), max(soas),
                                                                   sum(r.missed for r in records)))
print("the sentence's words, screen px (x0, y0, x1, y1):")
for w, a, b, line, x0, y0, x1, y1 in boxes:
    print("  %-10s chars %2d-%2d line %d  (%6.1f, %6.1f, %6.1f, %6.1f)" % (w, a, b, line, x0, y0, x1, y1))
