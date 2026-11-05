"""A movie through the real binding (ysp.video), for the phase B measurements.

    python movie.py CLIP [--wav WAV] [--windowed] [--hw auto|off|dxva] [--out PREFIX]

Each display frame: begin(), Movie.update() (timed), Gfx.begin(),
Movie.draw() at 1:1 in the screen's center, Gfx.end(), flip(). With --wav
the soundtrack plays on the default output and the movie clock follows it
(Movie.soundtrack). The records are kept until the end, as a script keeps
its data; gc.freeze() after setup. Writes PREFIX.frames.csv (the movie's
FRAME records), PREFIX.flips.csv (the flip records, with the DRAW,
SWAP and UPLOAD phases), PREFIX.update.csv (the update()
times) and PREFIX.txt (the describe lines, the events, the sound's start).
Shift+Esc ends a run.
"""
import argparse
import gc
import time

import ysp.audio as yau
import ysp.gfx as ygfx
import ysp.screen as yscr
import ysp.video as yvid

ap = argparse.ArgumentParser()
ap.add_argument("clip")
ap.add_argument("--wav")
ap.add_argument("--windowed", action="store_true")
ap.add_argument("--hw", default="auto")
ap.add_argument("--out", default="movie_py")
a = ap.parse_args()

opts = {"windowed": True, "size": (1280, 720)} if a.windowed else {}
flips, upd = [], []
au = yau.Audio() if a.wav else None
lines = []
try:
    with yscr.Screen(**opts) as s, ygfx.Gfx(s, background=0.45) as g, yvid.Movie(g, a.clip, hw_decode=a.hw) as m:
        s.title("ysp-vis movie probe")
        lines += ["python " + s.describe(), m.describe()]
        if au:
            m.soundtrack(au, a.wav)
            lines.append(au.describe())
        gc.freeze()
        m.play()
        clock = time.perf_counter_ns
        try:
            while True:
                f = s.begin()
                flips.extend(f.done)
                t0 = clock()
                playing = m.update()
                upd.append((f.index, clock() - t0))
                g.begin()
                m.draw()
                g.end()
                s.flip()
                if not playing:
                    break
        except yscr.Abort as e:
            lines.append(f"abort 0x{e.reasons:x}")
        s.wait_flip()
        last = flips[-1].index if flips else -1
        flips.extend(r for r in s.records() if r.index > last)
        frames = m.records()
        events = m.events()
        lines.append(m.describe())
        lines.append(f"records_lost {m.records_lost:d} events_lost {m.events_lost:d}")
    for e in events:
        lines.append("event " + repr(e))
        if au and e["kind"] == "sound" and e["action"] == "start":
            o = au.wait(e["play_id"], 2.0)
            lines.append(f"sound onset: target {o.target_ns:d} ns, onset {o.onset_ns:d} ns, residual {o.residual_ns:d} ns, "
                         f"tier {o.tier:d}, flags 0x{o.flags:x}")
finally:
    if au:
        au.close()

with open(a.out + ".txt", "w", newline="\n") as out:
    out.write("\n".join(lines) + "\n")
with open(a.out + ".frames.csv", "w", newline="\n") as out:
    out.write("display,frame,decision,why,flags,onset_ns,due_ns,residual_ns,movie_t_ns,ahead,tier\n")
    for r in frames:
        out.write(f"{r.display:d},{r.frame:d},{r.decision:d},{r.why:d},{r.flags:d},{r.onset_ns:d},"
                  f"{r.due_ns:d},{r.residual_ns:d},{r.movie_t_ns:d},{r.ahead:d},{r.tier:d}\n")
with open(a.out + ".flips.csv", "w", newline="\n") as out:
    out.write(f"# {lines[0]}\n")
    out.write("index,target_ns,planned_ns,onset_ns,residual_ns,dropped,path,tier,flags,draw_ns,swap_ns,upload_ns\n")
    for r in flips:
        ph = [-1 if x is None else round(x * 1e9) for x in r.phases]
        draw, swap, upload = ph[yscr.PHASE_DRAW], ph[yscr.PHASE_SWAP], ph[yscr.PHASE_UPLOAD]
        out.write(f"{r.index:d},{r.target_ns:d},{r.planned_ns:d},{r.onset_ns:d},{r.residual_ns:d},{r.dropped:d},"
                  f"{r.path:d},{r.tier:d},{r.flags:d},{draw:d},{swap:d},{upload:d}\n")
with open(a.out + ".update.csv", "w", newline="\n") as out:
    out.write("index,update_ns\n")
    out.write("".join(f"{i:d},{ns:d}\n" for i, ns in upd))
print(f"{len(frames):d} frame records, {len(flips):d} flip records -> {a.out}.*")
