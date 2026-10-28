"""N texts per frame in Python, for the ysp-vis text loop.

    python text_loop.py --mode words|page|label [--n 100] [--seconds 60] [--windowed] [--csv out.csv]

Each frame: Screen.begin(), Gfx.frame(scene), Screen.flip(). The scene:
  words  N one-word texts (5 to 9 letters, 28 px Segoe UI) on a grid, in one
         DrawList;
  page   one paragraph of 520 characters, 20 px, wrapped at 600 px;
  label  N one-word texts and a label whose text changes every frame
         ("frame 123": a layout and no new glyph in the frame loop, the
         label of a feedback display or a counter).
The frame's completed records are kept for the run; one CSV row per flip
record (table.py reads them). gc.freeze() after setup. Shift+Esc ends a run.
"""
import argparse
import gc
import os

import ysp.gfx as ygfx
import ysp.screen as yscr

WORDS = ["reading", "fixation", "saccade", "parafovea", "lexical", "syntax", "meaning", "context", "stimulus"]
LOREM = ("Reading research shows text in short lines with clear spacing. The eyes jump from word to word, and "
         "each fixation takes in a few letters to the left and more to the right. Long words are fixated more "
         "often, and frequent words are skipped. ") * 3

ap = argparse.ArgumentParser()
ap.add_argument("--mode", choices=("words", "page", "label"), default="words")
ap.add_argument("--n", type=int, default=100)
ap.add_argument("--seconds", type=float, default=60.0)
ap.add_argument("--windowed", action="store_true")
ap.add_argument("--csv", default="text.csv")
a = ap.parse_args()

opts = {"windowed": True, "size": (1280, 720)} if a.windowed else {}
rows = []
with yscr.Screen(gfx={"background": 0.5, "max_draws": 4096}, **opts) as s, ygfx.Gfx(s) as g:   # 4096: a text run took 3 blocks before ysp/gfx.h 0.17.2 (1 now); kept for comparison
    s.title("ysp-vis text probe")
    font = g.font(os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", "segoeui.ttf"))
    w, h = g.size
    items = []
    if a.mode in ("words", "label"):
        cols = 10 if a.n <= 100 else 20     # 500 words: 25 rows of 20, on the screen
        for i in range(a.n):
            items.append(g.text(WORDS[i % len(WORDS)], font=font, size=28, color=0.1,
                                x=(i % cols) * (w / cols) - w / 2 + w / cols / 2, y=(i // cols) * 40 - h / 2 + 40))
    else:
        items.append(g.text(LOREM[:520], font=font, size=20, width=600, align="start", color=0.1))
    label = None
    if a.mode == "label":
        label = g.text("frame 0123456789", font=font, size=40, color=0.9, y=h / 2 - 60)
        items.append(label)
    scene = g.list(items)
    g.prime()
    line = f"{s.describe()}; gfx {ygfx.__version__}; {a.mode} n {a.n:d}"
    gc.freeze()
    t_end = None
    try:
        while True:
            f = s.begin()
            if t_end is None:
                t_end = f.onset + a.seconds
            rows.extend(f.done)
            if f.onset >= t_end:
                s.flip()
                break
            if label is not None:
                label.text = f"frame {f.index:d}"              # a layout in the frame's work
            g.frame(scene)
            s.flip()
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
