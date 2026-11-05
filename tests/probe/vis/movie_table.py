"""The table of movie runs (movie.py, movie_mex.m).

    python movie_table.py PREFIX...

One row per run. Display frames: the movie's FRAME records. Shown on time:
SHOWN for its due time (why DUE) without F_LATE. Repeated: by reason (DUE
is the cadence's own repeat, e.g. each frame twice at 30 fps on 60 Hz).
Dropped: video frames never shown (the frame count minus the frames shown).
Missed flips: flip records with dropped > 0 (their vblanks in brackets).
Work: the flip record's DRAW + UPLOAD phases, the host's time from
begin()'s return to the flip call (update() and the draw); update: the
call's own time. A/V: residual = flip onset minus the frame's due time on
the audio clock (records with F_AUDIO_CLOCK, SHOWN), as the records report
it: the display's and the sound's clocks, not light against sound.
"""
import collections
import sys

WHY = ("due", "cadence", "display_late", "decode_late", "drift", "seek", "loop", "manual")
F_LATE, F_AUDIO = 0x2, 0x10


def pct(v, p):
    if not v:
        return float("nan")
    v = sorted(v)
    return v[min(len(v) - 1, max(0, int(round(p / 100.0 * (len(v) - 1)))))]


def rows(path):
    out = []
    with open(path) as f:
        for line in f:
            if line[0].isdigit() or line[0] == "-":
                out.append([int(x) for x in line.split(",")])
    return out


def summarize(prefix):
    fr = rows(prefix + ".frames.csv")
    fl = rows(prefix + ".flips.csv")
    up = [u[1] / 1e6 for u in rows(prefix + ".update.csv")]
    with open(prefix + ".txt") as f:
        txt = f.read().splitlines()
    host = txt[0].split(" ")[0]
    n_frames = max(r[1] for r in fr) + 1 if fr else 0
    shown = [r for r in fr if r[2] == 1]
    on_time = sum(1 for r in shown if r[3] == 0 and not r[4] & F_LATE)
    rep = collections.Counter(WHY[r[3]] for r in fr if r[2] == 2)
    dropped = n_frames - len({r[1] for r in shown})
    missed = [r for r in fl if r[5] > 0]
    work = [(r[9] + max(r[11], 0)) / 1e6 for r in fl if r[9] >= 0]
    av = [r[7] / 1e6 for r in shown if r[4] & F_AUDIO]
    snd = [x for x in txt if x.startswith("sound onset")]
    return ("| %s | %d | %d | %d / %d | %s | %d | %d (%d) | %.2f / %.2f / %.2f | %.2f / %.2f / %.2f | %s | %s |" % (
        host, n_frames, len(fr), len(shown), on_time, ", ".join("%s %d" % kv for kv in sorted(rep.items())) or "0",
        dropped, len(missed), sum(r[5] for r in missed), pct(work, 50), pct(work, 99), max(work) if work else float("nan"),
        pct(up, 50), pct(up, 99), max(up) if up else float("nan"),
        ("%.3f / %.3f / %.3f (%d)" % (pct(av, 1), pct(av, 50), pct(av, 99), len(av))) if av else "-",
        snd[0].split("residual ")[1].split(",")[0] if snd else "-"))


def main():
    print("| Host | Video frames | Display frames | Shown / on time | Repeated (why) | Dropped | Missed flips (vblanks) "
          "| Work p50 / p99 / max, ms | update() p50 / p99 / max, ms | A/V p1 / p50 / p99, ms (n) | Sound start residual |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for p in sys.argv[1:]:
        print(summarize(p))


if __name__ == "__main__":
    main()
