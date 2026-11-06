"""Psychtoolbox's and ysp/video.h's movie playback, scored the same way.

    uv run --no-project python analyze.py OUT_DIR [OUT_DIR ...]

For each cell of run.sh (TAG.csv beside TAG_vbl.csv) it takes the flips
that showed a new movie frame, maps each to the kernel's vblank by its
reported onset, and counts from the kernel's vblank numbers:
  shown      new frames flipped
  unshown    frames between the first and last shown that never were
  repeats    vblanks on which the previous frame stayed (a gap > 1)
  slips      changes of (vblank - frame): each repeat or unshown frame
             moves it; 0 means every frame stayed one refresh
  stamp      reported onset minus the kernel's vblank time, us
  flagged    the tool's own count: PTB, Flip's missed > 0 and PlayMovie's
             dropped frames; ysp, late flips and the repeated and dropped
             counts in its log
CPU and peak RSS are the whole process (startup included) from rusage.py.
PTB's times are GetSecs (CLOCK_REALTIME on Linux); vbl_log's third column
maps them to CLOCK_MONOTONIC. ysp's are CLOCK_MONOTONIC already.
"""
import bisect
import csv
import glob
import os
import re
import statistics
import sys


def read_vbl(path):
    seq, t, off = [], [], []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            s, ns = int(r["seq"]), int(r["mono_ns"])
            # vbl_log can miss an event (twice in 36 cells here); a flip on
            # that vblank would score as off any vblank. The counter still
            # numbers it, so put it back between its neighbors.
            if seq and 1 < s - seq[-1] <= 4:
                step = (ns - t[-1]) / (s - seq[-1])
                for k in range(1, s - seq[-1]):
                    seq.append(seq[-1] + 1)
                    t.append(round(t[-1] + step))
            seq.append(s)
            t.append(ns)
            off.append(int(r["real_minus_mono_ns"]))
    return seq, t, statistics.median(off)


def nearest(t_list, x):
    i = bisect.bisect_left(t_list, x)
    best = None
    for j in (i - 1, i):
        if 0 <= j < len(t_list) and (best is None or abs(t_list[j] - x) < abs(t_list[best] - x)):
            best = j
    return best


def pct(v, p):
    if not v:
        return float("nan")
    s = sorted(v)
    return s[min(len(s) - 1, max(0, int(round(p / 100 * (len(s) - 1)))))]


def ptb_rows(path, off):
    with open(path) as f:
        head = f.readline()
        lines = [ln for ln in f if not ln.startswith("#")]
    meta = dict(zip(head.split()[1::2], head.split()[2::2]))
    fps = float(meta["fps"])
    rows = []
    for r in csv.DictReader(lines):
        rows.append({
            "frame": round(float(r["pts"]) * fps),
            "onset": round(float(r["vbl"]) * 1e9) - off,
            "late": float(r["missed"]) > 0,
            "slack_ms": (float(r["vbl"]) - float(r["t_got"])) * 1e3,
        })
    extra = {"dropped": int(meta["dropped"]), "open_s": float(meta["open_s"]), "play_cpu_s": float(meta["cpu_s"])}
    return rows, extra


def ysp_rows(path):
    rows, ahead = [], []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if r["decision"] != "1":       # repeats are scored from the vblank gaps
                continue
            rows.append({"frame": int(r["frame"]), "onset": int(r["onset_ns"]),
                         "late": int(r["flags"]) & 0x2 != 0})
            if "ahead" in r:
                ahead.append(int(r["ahead"]))
    # The queue fills at the start and empties at the end by design.
    body = ahead[10:-8]
    return rows, {"ahead_min": min(body) if body else None}


def rusage(path):
    if not os.path.exists(path):
        return None
    m = re.search(r"wall ([\d.]+) s; user ([\d.]+) s; sys ([\d.]+) s; .*peak RSS ([\d.]+) MB", open(path).read())
    return {"wall": float(m[1]), "cpu": float(m[2]) + float(m[3]), "rss": float(m[4])} if m else None


def what_ran(out, tag, ptb):
    if ptb:
        p = os.path.join(out, tag + "_gst.txt")
        made = re.findall(r'creating element "([\w-]+)"', open(p, errors="replace").read()) if os.path.exists(p) else []
        dec = sorted({e for e in made if "dec" in e and "h264" in e})
        sink = sorted({e for e in made if e.endswith("sink") and "audio" in e or e in ("pulsesink", "alsasink")})
        return "+".join(dec) or "?", "+".join(sink) or "?"
    log = open(os.path.join(out, tag + ".log"), errors="replace").read()
    d = re.search(r"ysp_video [\d.]+: FFmpeg [^(]*\([^;]*; H\.264, ([^)]*)\)", log)
    a = re.search(r"ysp_audio: backend=([\w,-]+)", log)
    return (d[1] if d else "?"), (a[1] if a else "?")


def ysp_log_counts(out, tag):
    log = open(os.path.join(out, tag + ".log"), errors="replace").read()
    m = re.search(r"shown (\d+), repeated (\d+), dropped (\d+)", log)
    return (int(m[2]), int(m[3])) if m else (None, None)


def cell(out, tag):
    seq, t, off = read_vbl(os.path.join(out, tag + "_vbl.csv"))
    ptb = tag.startswith("ptb")
    rows, extra = ptb_rows(os.path.join(out, tag + ".csv"), off) if ptb else ysp_rows(os.path.join(out, tag + ".csv"))
    err, kept, lost = [], [], 0
    for r in rows:
        j = nearest(t, r["onset"])
        if j is None or abs(t[j] - r["onset"]) > 2_000_000:
            lost += 1
            continue
        err.append((r["onset"] - t[j]) / 1000)
        kept.append((seq[j], r["frame"], r["late"]))
    repeats = unshown = backward = slips = 0
    lag = [s - f for s, f, _ in kept]
    for (s0, f0, _), (s1, f1, _) in zip(kept, kept[1:]):
        if s1 - s0 > 1:
            repeats += s1 - s0 - 1
        if f1 - f0 > 1:
            unshown += f1 - f0 - 1
        if f1 <= f0:
            backward += 1
    slips = sum(1 for a, b in zip(lag, lag[1:]) if a != b)
    flagged_late = sum(1 for _, _, late in kept if late)
    dec, snd = what_ran(out, tag, ptb)
    ru = rusage(os.path.join(out, tag + ".time"))
    res = {
        "tag": tag, "decoder": dec, "sound": snd, "shown": len(kept), "lost": lost,
        "first": kept[0][1] if kept else None, "last": kept[-1][1] if kept else None,
        "unshown": unshown, "repeats": repeats, "backward": backward, "slips": slips,
        "lag_span": (max(lag) - min(lag)) if lag else None,
        "err_med": statistics.median(err) if err else float("nan"),
        "err_max": max(abs(e) for e in err) if err else float("nan"),
        "flag_late": flagged_late, "ru": ru,
    }
    if ptb:
        res["tool"] = "missed %d, dropped %d" % (flagged_late, extra["dropped"])
        res["note"] = "open %.2f s; frame release to flip p1 %.2f, p50 %.2f ms; playback CPU %.1f s" % (
            extra["open_s"], pct([r["slack_ms"] for r in rows], 1), pct([r["slack_ms"] for r in rows], 50), extra["play_cpu_s"])
    else:
        rep, drop = ysp_log_counts(out, tag)
        res["tool"] = "late %d, repeated %s, dropped %s" % (flagged_late, rep, drop)
        res["note"] = "queue min %s of 7" % extra["ahead_min"]
    return res


def main():
    print("| Run | Cell | Decoder | Sound | Shown (frames) | Unshown | Repeats | Slips | Stamp med / max abs, us | Tool's own count | CPU per frame, ms | Peak RSS, MB | Notes |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for out in sys.argv[1:]:
        tags = sorted(os.path.basename(p)[:-8] for p in glob.glob(os.path.join(out, "*_vbl.csv")))
        for tag in tags:
            if not os.path.exists(os.path.join(out, tag + ".csv")):
                print("| %s | %s | no CSV (see %s.log) |" % (os.path.basename(out.rstrip("/")), tag, tag))
                continue
            c = cell(out, tag)
            ru = c["ru"]
            cpu = "%.2f" % (1e3 * ru["cpu"] / c["shown"]) if ru and c["shown"] else "?"
            rss = "%.0f" % ru["rss"] if ru else "?"
            shown = "%d (%s to %s)" % (c["shown"], c["first"], c["last"])
            if c["lost"]:
                shown += ", %d off any vblank" % c["lost"]
            if c["backward"]:
                shown += ", %d backward" % c["backward"]
            print("| %s | %s | %s | %s | %s | %d | %d | %d | %+.2f / %.2f | %s | %s | %s | %s |" % (
                os.path.basename(out.rstrip("/")), c["tag"], c["decoder"], c["sound"], shown, c["unshown"],
                c["repeats"], c["slips"], c["err_med"], c["err_max"], c["tool"], cpu, rss, c["note"]))


if __name__ == "__main__":
    main()
