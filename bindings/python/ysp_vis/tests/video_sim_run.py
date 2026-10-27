"""One movie run on the simulated display and ysp/rt.h's virtual clock, as
examples/video/play.c --sim runs it: the same loop, the same desc, so the
two give the same decisions frame by frame (test_video_sim.py compares).

    python video_sim_run.py CLIP OUT.csv [--index]

OUT.csv has video_play's first seven columns: display, vblank, onset_ns,
decision, why, frame, movie_t_ns (one line per display frame whose update
made a record). It runs in its own process: the virtual clock is the
module's, and it must start before anything reads the clock.
"""
import sys

from ysp import _vis

_vis._virtual_clock(True)   # before the screen opens, as play.c's yrt_virtual_begin(0)

import ysp.gfx as ygfx  # noqa: E402
import ysp.screen as yscr  # noqa: E402
import ysp.video as yvid  # noqa: E402

clip, out = sys.argv[1], sys.argv[2]
if "--index" in sys.argv:
    yvid.make_index(clip)
rows = []
with yscr.Screen(backend="sim", size=(640, 360)) as s, ygfx.Gfx(s, background=0.45) as g, yvid.Movie(g, clip) as m:
    m.play()
    while True:
        f = s.begin()
        playing = m.update()
        r = m.last()
        if r is not None and r.display == f.index:
            rows.append((f.index, f.vblank, f.onset_ns, r.decision, r.why, r.frame, r.movie_t_ns))
        g.begin()
        m.draw()
        g.end()
        s.flip()
        if not playing:
            break
with open(out, "w", newline="\n") as fh:
    fh.write("display,vblank,onset_ns,decision,why,frame,movie_t_ns\n")
    for row in rows:
        fh.write(",".join(str(x) for x in row) + "\n")
_vis._virtual_clock(False)
