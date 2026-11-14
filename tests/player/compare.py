#!/usr/bin/env python3
"""Compare the player's data with the C programs' on the virtual clock.

Run with one interpreter:  python3 tests/player/compare.py --bin DIR [--out DIR]

Each worked example of docs/experiment_format.md 2.14 is built as a pack and
run by ysp_player. Then its C program runs on one realized design: the seed
of the player's leaf (D8) goes to the program's --seed, so both draw one
order; the program's responses go back to the player as a script, so both
hear one participant. Every column is reported as:

  exact   equal on every trial
  format  different by a decision of the format (named)
  n/a     not comparable (with what was checked instead)

Exit code 1 when a column marked exact differs.
"""
import argparse
import csv
import json
import math
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


class Fail(Exception):
    pass


def tool(bin_dir, name):
    # a multi-config build (MSVC) puts programs in Release/, with .exe
    for d in (bin_dir, os.path.join(bin_dir, "pack"), os.path.join(bin_dir, "player")):
        for sub in ("", "Release"):
            for ext in ("", ".exe"):
                p = os.path.join(d, sub, name + ext)
                if os.path.isfile(p):
                    return p
    raise Fail("no program %s under %s" % (name, bin_dir))


# --hz: the simulated display's rate for the player and every C program that
# runs on --sim, so both sides see one frame grid. None keeps their default
# (60 Hz) and passes no option, so a 60 Hz run's commands stay as they were.
HZ = None


def run(args, cwd=None):
    if HZ is not None and "--sim" in args:
        args = args + ["--sim-hz", HZ]
    r = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        raise Fail("%s: exit %d\n%s\n%s" % (" ".join(args), r.returncode, r.stdout, r.stderr))
    return r.stdout


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def norm(a):
    """Numbers compare by value (0.5 and 0.50); text compares as text."""
    try:
        return float(a)
    except (TypeError, ValueError):
        return a


def compare(rows_p, rows_c, pairs):
    """pairs: (player col, C col, class, why). Returns report rows and the
    number of exact columns that differ."""
    report = []
    bad = 0
    if len(rows_p) != len(rows_c):
        report.append(("<row count>", "%d vs %d" % (len(rows_p), len(rows_c)), "FAIL", ""))
        return report, 1
    for pc, cc, klass, why in pairs:
        # a column missing on a side is a failure, even missing on both: two
        # empty columns would compare equal and show nothing
        missing = [n for n, row in ((pc, rows_p[0] if rows_p else {}), (cc, rows_c[0] if rows_c else {})) if n not in row]
        if missing:
            report.append(("%s <- %s" % (pc, cc), "column missing: %s" % ", ".join(missing), "FAIL", why))
            bad += 1
            continue
        diff = 0
        first = None
        for i, (a, b) in enumerate(zip(rows_p, rows_c)):
            va = a.get(pc, "")
            vb = b.get(cc, "")
            if norm(va) != norm(vb):
                diff += 1
                if first is None:
                    first = (i, va, vb)
        status = "exact" if klass == "float32" else klass
        note = why
        if klass == "float32":
            import struct
            for j, (a2, b2) in enumerate(zip(rows_p, rows_c)):
                pass
            diff = sum(1 for a2, b2 in zip(rows_p, rows_c)
                       if struct.pack("<f", float(a2.get(pc) or 0)) != struct.pack("<f", float(b2.get(cc) or 0)))
            first = None
            if diff:
                status = "FAIL"
                bad += 1
                note = "differs as float32 on %d of %d" % (diff, len(rows_p))
            else:
                note = "equal as float32 (the C file has 9 digits, the player the double of the same float)"
        elif klass == "exact":
            if diff:
                status = "FAIL"
                bad += 1
                note = "differs on %d of %d trials (first: trial %d, player %r, C %r)" % (diff, len(rows_p), first[0], first[1], first[2])
            else:
                note = "equal on %d trials" % len(rows_p)
        report.append(("%s <- %s" % (pc, cc), "%d/%d differ" % (diff, len(rows_p)), status, note))
    return report, bad


def print_report(name, report):
    print("\n## %s\n" % name)
    print("| Player column <- C column | Result | Class | Note |")
    print("|---|---|---|---|")
    for row in report:
        print("| %s | %s | %s | %s |" % row)


def session_seed(out):
    with open(os.path.join(out, "session.json")) as f:
        return json.load(f)["node_seeds"][0]["seed"]


def player(bin_dir, args):
    # a data set never overwrites: the folder is new each run
    out = args[args.index("--out") + 1]
    shutil.rmtree(out, ignore_errors=True)
    return run([tool(bin_dir, "ysp_player")] + args)


def build_pack(bin_dir, src, dest):
    run([tool(bin_dir, "ypak"), "build", os.path.join(src, "pack.json"), "-o", dest], cwd=src)


def write_script(rows, path, resp="response", rt="rt_s", hold=0.08):
    with open(path, "w", newline="\n") as f:
        f.write("trial_index,response,rt_s,hold_s\n")
        for r in rows:
            f.write("%s,%s,%s,%s\n" % (r["trial_index"], r[resp], r[rt] or "0", hold))


def do_rdk(a, ex):
    src = os.path.join(HERE, "rdk")
    pack = os.path.join(a.out, "rdk.ysppak")
    build_pack(a.bin, src, pack)
    p0 = os.path.join(a.out, "rdk_p0")
    player(a.bin, [pack, "--out", p0, "--sim", "--seed", "7", "--field", "participant=1"])
    seed = session_seed(p0)
    p0_rows = read_csv(os.path.join(p0, "trials.csv"))
    seeds = os.path.join(a.out, "rdk_seeds.txt")
    with open(seeds, "w") as f:
        for r in p0_rows:
            f.write("%s\n" % r["dots_seed"])
    c_out, c_steps = os.path.join(a.out, "rdk_c.csv"), os.path.join(a.out, "rdk_c_steps.csv")
    run([tool(a.bin, "trial_rdk"), "--sim", "--reps", "5", "--seed", seed, "--out", c_out, "--steps", c_steps,
         "--trial-seeds", seeds, "--silent-trials", "3,17"])
    c_rows = read_csv(c_out)
    script = os.path.join(a.out, "rdk_script.csv")
    write_script(c_rows, script, hold=0.06)
    p1 = os.path.join(a.out, "rdk_p1")
    player(a.bin, [pack, "--out", p1, "--sim", "--seed", "7", "--field", "participant=1", "--participant", "script:" + script])
    rows = read_csv(os.path.join(p1, "trials.csv"))
    pairs = [(c, c, "exact", "") for c in ["coherence", "coherent_direction", "key_answer", "index", "block", "rep", "condition", "track",
                                            "practice", "warmup", "requeued", "after_break", "trial_index", "correct",
                                            "response", "rt_s", "rt_key_duration_s", "onset_src", "onset_tier", "onset_residual_s",
                                            "landing_residual_s", "onset_frame", "rsp_tier", "rsp_flags", "n_anticipations", "n_early"]]
    pairs += [("dots_seed", "rdk_seed", "exact", "D8 seed; given to the program by --trial-seeds"),
              ("dots_updates", "rdk_updates", "exact", ""),
              ("dots_digest", "rdk_digest", "exact", "yrdk_digest over one field: the same dots"),
              ("frames_dropped", "frames_dropped", "exact", "")]
    report, bad = compare(rows, c_rows, pairs)
    # the steps file (C) against streams/dots.csv
    s_p = read_csv(os.path.join(p1, "streams", "dots.csv"))
    s_c = read_csv(c_steps)
    r2, bad2 = compare(s_p, s_c, [("trial_index", "trial_index", "exact", ""), ("update", "update", "exact", ""), ("t_s", "t_s", "exact", "stream column t_s against the steps file's t_s"),
                                  ("coherence", "coherence", "float32", ""), ("direction", "direction", "float32", ""), ("speed", "speed", "float32", "")])
    report += [("stream: " + r[0], r[1], r[2], r[3]) for r in r2]
    print_report("RDK (trial_rdk.c; order and dots from one realized design)", report)
    return bad + bad2


def do_stroop(a, ex):
    src = os.path.join(HERE, "stroop")
    work = os.path.join(a.out, "stroop_src")
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(src, work)
    fonts = os.path.join(work, "fonts")
    os.makedirs(fonts, exist_ok=True)
    shutil.copyfile(a.font, os.path.join(fonts, "NotoSans-Regular.ttf"))
    pack = os.path.join(a.out, "stroop.ysppak")
    build_pack(a.bin, work, pack)
    p0 = os.path.join(a.out, "stroop_p0")
    player(a.bin, [pack, "--out", p0, "--sim", "--seed", "11", "--field", "participant=1"])
    seed = session_seed(p0)
    c_out = os.path.join(a.out, "stroop_c.csv")
    run([tool(a.bin, "trial_stroop"), "--sim", "--reps", "3", "--seed", seed, "--out", c_out, "--font", a.font])
    # the C file starts with comment lines
    lines = [ln for ln in open(c_out, newline="") if not ln.startswith("#")]
    c_rows = list(csv.DictReader(lines))
    script = os.path.join(a.out, "stroop_script.csv")
    with open(script, "w", newline="\n") as f:
        f.write("trial_index,response,rt_s,hold_s\n")
        for r in c_rows:
            f.write("%s,%s,%s,0.08\n" % (r["trial_index"], r["response"], r["rt_s"] or "0"))
    p1 = os.path.join(a.out, "stroop_p1")
    player(a.bin, [pack, "--out", p1, "--sim", "--seed", "11", "--field", "participant=1", "--participant", "script:" + script])
    rows = read_csv(os.path.join(p1, "trials.csv"))
    pairs = [("trial_index", "trial_index", "exact", ""), ("word", "stimulus", "exact", "C's stimulus column is the table's word"),
             ("ink", "ink", "exact", ""), ("congruent", "congruent", "exact", ""), ("answer", "answer", "exact", ""),
             ("response", "response", "exact", ""), ("rt_s", "rt_s", "exact", ""), ("correct", "correct", "exact", ""),
             ("onset_tier", "onset_tier", "exact", ""), ("onset_src", "onset_src", "exact", ""),
             ("rsp_flags", "rsp_flags", "exact", ""), ("n_anticipations", "n_anticipations", "exact", "")]
    report, bad = compare(rows, c_rows, pairs)
    d = sum(1 for r, c in zip(rows, c_rows) if c["response"] and r["rsp_tier"] != c["rsp_tier"])
    report.append(("rsp_tier <- rsp_tier", "%d/%d differ where a response exists" % (d, len(rows)), "format",
                   "YRSP_DATA_COLS writes it empty with no response (2.10 item 8); the program's hand-written CSV writes 0"))
    if d:
        bad += 1
    # columns the C program has that the format derives another way
    for pc, cc, why in [("practice", "block", "format 2.10: practice is the trial info's flag (0/1); C's block is the text practice or test"),
                        ("feedback", "feedback", "format 2.3: $outcome is correct/wrong/timeout; C writes too_slow (2.14's table says 'same': a finding)")]:
        d = 0
        for r, c in zip(rows, c_rows):
            va, vb = r.get(pc, ""), c.get(cc, "")
            if cc == "block":
                vb = "1" if vb == "practice" else "0"
            if cc == "feedback":
                va = "too_slow" if va == "timeout" else va
            if norm(va) != norm(vb):
                d += 1
        report.append(("%s <- %s" % (pc, cc), "%d/%d differ after the mapping" % (d, len(rows)), "format", why))
        if d:
            bad += 1
    print_report("Stroop (trial_stroop.c; order from one realized design)", report)
    return bad


def do_afc2(a, ex):
    src = os.path.join(HERE, "afc2")
    pack = os.path.join(a.out, "afc2.ysppak")
    build_pack(a.bin, src, pack)
    rig = os.path.join(a.out, "afc2_rig")
    shutil.rmtree(rig, ignore_errors=True)
    os.makedirs(rig)
    run([tool(a.bin, "player_make_rig"), rig])
    profile = os.path.join(rig, "profile.json")
    p0 = os.path.join(a.out, "afc2_p0")
    player(a.bin, [pack, "--out", p0, "--sim", "--seed", "5", "--field", "participant=1", "--rig", profile])
    seed = session_seed(p0)
    c_out = os.path.join(a.out, "afc2_c.csv")
    run([tool(a.bin, "trial_2afc_adaptive"), "--sim", "--design-timing", "--trials", "60", "--seed", seed, "--out", c_out,
         "--rig-dir", rig, "--cal", os.path.join(rig, "display.yspcal"), "--silent-trials", "5,40"])
    c_rows = read_csv(c_out)
    script = os.path.join(a.out, "afc2_script.csv")
    write_script(c_rows, script, hold=0.08)
    p1 = os.path.join(a.out, "afc2_p1")
    player(a.bin, [pack, "--out", p1, "--sim", "--seed", "5", "--field", "participant=1", "--rig", profile, "--participant", "script:" + script])
    rows = read_csv(os.path.join(p1, "trials.csv"))
    # the program's rows carry numbers at 6 decimals where the player keeps the double
    for r in rows:
        for k in ("level", "contrast_shown"):
            if r.get(k):
                r[k] = "%.6f" % float(r[k])
    pairs = [(c, c, "exact", "") for c in ["index", "block", "rep", "condition", "track", "practice", "warmup", "requeued", "after_break",
                                            "side", "trial_seed", "trial_index", "correct", "response", "rt_s", "rt_key_duration_s", "onset_src",
                                            "onset_tier", "onset_residual_s", "landing_residual_s", "onset_frame", "rsp_tier", "rsp_flags",
                                            "n_anticipations", "n_early", "method", "stimulus"]]
    pairs += [("level", "intensity", "exact", "the track's proposal, at the 6 decimals of the C column"),
              ("contrast_shown", "contrast_shown", "exact", "ycol_michelson's shown, 6 decimals")]
    report, bad = compare(rows, c_rows, pairs)
    # estimate: the C program writes it at 9 decimals
    est_diff = 0
    for r, c in zip(rows, c_rows):
        va, vb = r.get("estimate", ""), c.get("estimate", "")
        if (va == "") != (vb == "") or (va and abs(float(va) - float(vb)) > 5e-9):
            est_diff += 1
    report.append(("estimate <- estimate", "%d/%d differ past 5e-9" % (est_diff, len(rows)), "FAIL" if est_diff else "exact", "9 decimals in the C column"))
    bad += 1 if est_diff else 0
    for col, why in [("scene_contrast", "ycol_michelson's scene: the definition names no variable for it"),
                     ("code_peak", "2.14 names this: no variable of the definition holds the codes"),
                     ("code_trough", "2.14 names this: no variable of the definition holds the codes"),
                     ("key_answer", "the template's `columns` do not ask for it; `correct` covers it")]:
        report.append((col, "-", "n/a", why))
    print_report("2AFC staircase (trial_2afc_adaptive.c; one realized design, the program's timings set to the definition's)", report)
    return bad


def do_oddball(a, ex):
    src = os.path.join(HERE, "oddball")
    pack = os.path.join(a.out, "oddball.ysppak")
    build_pack(a.bin, src, pack)
    rig = os.path.join(a.out, "afc2_rig")
    profile = os.path.join(rig, "profile.json")
    p = os.path.join(a.out, "oddball_p")
    player(a.bin, [pack, "--out", p, "--sim", "--seed", "3", "--field", "participant=1", "--rig", profile])
    rows = read_csv(os.path.join(p, "trials.csv"))
    events = read_csv(os.path.join(p, "events.csv"))
    frames = read_csv(os.path.join(p, "frames.csv"))
    planned = {r["frame"]: r["planned_s"] for r in frames}
    outs = [e for e in events if e["kind"] == "out" and e["code"] in ("1", "2")]
    resp_outs = [e for e in events if e["kind"] == "out" and e["code"] not in ("1", "2")]
    want = {"left": "101", "right": "102"}
    resp_ok = len(resp_outs) == len(rows) and all(e["code"] == want[r["response"]] for e, r in zip(resp_outs, rows))
    report_resp = ("response.trigger: code by the response (101, 102)", "%d/%d" % (len(resp_outs), len(rows)), "exact" if resp_ok else "FAIL", "out rows with the choice's code; --sim sends from the frame, not a device rule")
    bad = 0
    report = [report_resp]
    if not resp_ok:
        bad += 1
    # one code from the table at the picture's flip, one for the response
    n_trig_ok = sum(1 for r in rows if r["trig_n"] == "2" and r["trig_failed"] == "0")
    report.append(("trig_n, trig_failed per trial", "%d/%d" % (n_trig_ok, len(rows)), "exact" if n_trig_ok == len(rows) else "FAIL", "two codes per trial (stimulus and response), none refused by the channel"))
    if n_trig_ok != len(rows):
        bad += 1
    # each `out` row: the code from the row's table, at its flip's planned vblank
    n_code_ok = 0
    for e, r in zip(outs, rows):
        if e["code"] == r["code"] and e["deadline_s"] == planned.get(e["frame"], "?"):
            n_code_ok += 1
    report.append(("events.csv out: code and deadline_s = flip planned vblank", "%d/%d" % (n_code_ok, len(outs)), "exact" if n_code_ok == len(outs) and len(outs) == len(rows) else "FAIL",
                   "the trigger fires with the picture's frame (ysp/timeline.h G1, yscr_trigger)"))
    if not (n_code_ok == len(outs) and len(outs) == len(rows)):
        bad += 1
    report.append(("pic_*, resp columns vs trigger_flip", "-", "n/a",
                   "trigger_flip.c has no trials, images or responses: it pulses codes at flips; the deadline-at-planned-vblank path is what the two share"))
    c = run([tool(a.bin, "device_trigger_flip"), "--frames", "120", "--every", "10"])
    summary = [ln for ln in c.splitlines() if "triggers" in ln]
    report.append(("device_trigger_flip", summary[-1] if summary else "(no summary)", "n/a", "the C program's own count of writes at flips, for the same mechanism"))
    soa = [float(r["soa_s"]) for r in rows]
    hz = float(HZ) if HZ is not None else 60.0
    frame_grid = all(abs(s * hz - round(s * hz)) < 1e-6 for s in soa)
    soa_ok = frame_grid and min(soa) >= 0.8 and max(soa) <= 1.2
    report.append(("soa_s on the display's frames (rate=display)", "%d values, 0.8 to 1.2 s" % len(soa), "exact" if soa_ok else "FAIL", "jitter snapped to %g Hz" % hz))
    if not soa_ok:
        bad += 1
    print_report("Oddball EEG (trigger_flip.c: not comparable column for column)", report)
    return bad


# --- format version 2 (3.18): the player draws first, the C program runs on
# the player's order and draws and writes its scripted hands' samples, the
# player runs on those samples (3.19 items 1 and 2) ----------------------

def read_c(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(l for l in f if not l.startswith("#")))


def dec_of(text):
    return len(text.split(".")[1]) if "." in text else 0


def same_at(va, vb, scale=1.0, shift=0.0):
    """The player's value against the C file's text, at the C file's
    decimals (C rounds on write); empty against empty."""
    if va in ("", None) or vb in ("", None):
        return (va or "") == (vb or "")
    try:
        x = float(va) * scale + shift
        float(vb)
    except ValueError:
        return str(va) == str(vb)   # text compares as text
    return ("%%.%df" % dec_of(vb)) % x == ("%%.%df" % dec_of(vb)) % float(vb) or \
        (abs(x) < 0.5 * 10 ** -dec_of(vb) and abs(float(vb)) < 0.5 * 10 ** -dec_of(vb))


def state_invariant(events_path):
    """The state rows that no reading of the format allows, or None (the
    reference model's INVARIANT, tests/model/diff.py). A row's entry time
    is the trial's first onset for its first row and the end of the row
    before for the others (3.11 rule 1): exit times never go back, the
    first row ends at or after its planned onset, and entry frames never go back
    (rule 2). A row may end before its own t_s: a state entered between
    frames has an entry before its frame's onset."""
    def ns(x):
        a, b = x.split(".")
        return int(a) * 1000000000 + int(b.ljust(9, "0")[:9])
    if not os.path.exists(events_path):
        return None
    rows = {}
    with open(events_path, newline="") as f:
        for e in csv.DictReader(f):
            if e["kind"] == "state":
                rows.setdefault(e["trial_index"], []).append(e)
    for ti, rr in rows.items():
        for j, r in enumerate(rr):
            if j == 0:
                # the first onset as planned: a late flip record's t_s is later
                if ns(r["end_s"]) < ns(r["planned_s"] or r["t_s"]):
                    return "trial %s state row 0 ends before the trial's first onset: %s" % (ti, r["end_s"])
                continue
            p = rr[j - 1]
            if ns(r["end_s"]) < ns(p["end_s"]):
                return "trial %s state row %d (%s) ends at %s, before its entry (row %d's end %s)" % (ti, j, r["text"], r["end_s"], j - 1, p["end_s"])
            if int(r["frame"]) < int(p["frame"]):
                return "trial %s state row %d is on frame %s, before row %d's %s" % (ti, j, r["frame"], j - 1, p["frame"])
    return None

def v2_rig(a, name):
    rig = os.path.join(a.out, name + "_rig")
    shutil.rmtree(rig, ignore_errors=True)
    os.makedirs(rig)
    run([tool(a.bin, "player_make_rig"), rig, "--view", "1024", "320"])
    return os.path.join(rig, "profile.json")


# The most rows a "format" class may differ in: the counts the format's
# findings give (docs/experiment.md, F25 and F33). More is a failure.
FORMAT_BOUND = {"move_tier": 1, "cursor_end_deg": 1, "--traj cursor_x": 765, "--traj cursor_y": 726}


def v2_columns(rows_p, rows_c, pairs):
    """pairs: (player col, C col, class, why, scale, shift). A column
    missing on a side fails, as compare() does; a "format" class fails past
    its FORMAT_BOUND."""
    report, bad = [], 0
    if len(rows_p) != len(rows_c):
        return [("<row count>", "%d vs %d" % (len(rows_p), len(rows_c)), "FAIL", "")], 1
    for pc, cc, klass, why, scale, shift in pairs:
        missing = [n for n, row in ((pc, rows_p[0] if rows_p else {}), (cc, rows_c[0] if rows_c else {})) if n not in row]
        if missing:
            report.append(("%s <- %s" % (pc, cc), "column missing: %s" % ", ".join(missing), "FAIL", why))
            bad += 1
            continue
        if klass == "map":
            m = scale if isinstance(scale, dict) else None
            diff = sum(1 for a2, b2 in zip(rows_p, rows_c) if (m.get(a2[pc], a2[pc]) if m else scale(a2[pc])) != b2[cc])
            report.append(("%s <- %s" % (pc, cc), "%d/%d differ after the mapping" % (diff, len(rows_p)), "format" if not diff else "FAIL", why))
            bad += 1 if diff else 0
            continue
        diff, first = 0, None
        for i, (a2, b2) in enumerate(zip(rows_p, rows_c)):
            if not same_at(a2.get(pc, ""), b2.get(cc, ""), scale, shift):
                diff += 1
                first = first or (i, a2.get(pc, ""), b2.get(cc, ""))
        over = klass == "format" and diff > FORMAT_BOUND.get(cc, 0)
        status = klass if not diff else ("FAIL" if klass == "exact" or over else klass)
        if (klass == "exact" and diff) or over:
            bad += 1
        note = why or ("equal on %d rows" % len(rows_p) if not diff else "first: row %d, player %r, C %r" % first)
        report.append(("%s <- %s" % (pc, cc), "%d/%d differ" % (diff, len(rows_p)), status, note))
    return report, bad


def do_visuomotor(a, ex):
    src = os.path.join(HERE, "visuomotor")
    pack = os.path.join(a.out, "visuomotor.ysppak")
    build_pack(a.bin, src, pack)
    profile = v2_rig(a, "visuomotor")
    common = ["--sim", "--seed", "3", "--field", "participant=1", "--rig", profile, "--sim-gain", "hand=0.25"]
    draws = os.path.join(a.out, "visuomotor_draws.csv")
    player(a.bin, [pack, "--out", os.path.join(a.out, "visuomotor_draws"), "--test-draws", draws] + common)
    c_out, c_traj, smp = (os.path.join(a.out, "visuomotor_c" + x) for x in (".csv", "_traj.csv", "_samples.csv"))
    run([tool(a.bin, "trial_visuomotor"), "--sim", "--test-order", draws, "--test-samples-out", smp, "--out", c_out, "--traj", c_traj])
    p1 = os.path.join(a.out, "visuomotor_p")
    player(a.bin, [pack, "--out", p1, "--samples", smp] + common)
    inv_bad = state_invariant(os.path.join(p1, "events.csv"))
    rows, c_rows = read_csv(os.path.join(p1, "trials.csv")), read_c(c_out)
    one = (1.0, 0.0)
    pairs = [(c, c, "exact", "", 1.0, 0.0) for c in ("trial_index", "target_deg", "rotation_deg", "rt_s", "mt_s", "cross_s", "onset_tier",
                                                     "onset_src", "rsp_flags")]
    pairs += [("node", "block", "exact", "the node's name", 1.0, 0.0),
              ("feedback", "feedback", "exact", "", 1.0, 0.0),
              ("peak_speed", "peak_speed_px_s", "exact", "px per s; the C file has 1 decimal", 1.0, 0.0),
              ("rsp_tier", "move_tier", "format", "the C file writes the result's stamp tier also with no response (trial 2, early): 0; the player leaves it empty", 1.0, 0.0)]
    report, bad = v2_columns(rows, c_rows, pairs)
    report.append(("state rows: entry times and frames never go back", inv_bad or "hold", "FAIL" if inv_bad else "exact", "3.11 rules 1 and 2 (tests/model's INVARIANT)"))
    bad += 1 if inv_bad else 0
    # the angles: atan2(-y, x) minus target_deg, at the C file's 4 decimals
    for xy, cc in ((("hand_end_x", "hand_end_y"), "hand_end_deg"), (("cursor_end_x", "cursor_end_y"), "cursor_end_deg")):
        d = 0
        for r, c in zip(rows, c_rows):
            ang = math.degrees(math.atan2(-float(r[xy[1]]), float(r[xy[0]]))) - float(r["target_deg"])
            while ang > 180:
                ang -= 360
            while ang <= -180:
                ang += 360
            if not same_at(ang, c[cc]):
                d += 1
        klass = "exact" if cc == "hand_end_deg" else "format"
        why = "analysis: atan2(-y, x) minus target_deg (-0.0000 and 0.0000 are one)"
        if klass == "format":
            why += "; the program's cursor is a float in window px, the player's a double (F25): a trial can round the other way at 4 decimals"
        over = klass == "format" and d > FORMAT_BOUND.get(cc, 0)
        report.append(("%s, %s <- %s" % (xy[0], xy[1], cc), "%d/%d differ" % (d, len(rows)), "FAIL" if (d and (klass == "exact" or over)) else klass, why))
        bad += 1 if (d and (klass == "exact" or over)) else 0
    # the trajectory (--traj) against streams/path.csv: t_s from target_onset_s, + the window's center
    path, traj = read_csv(os.path.join(p1, "streams", "path.csv")), read_c(c_traj)
    onset = {r["trial_index"]: float(r["target_onset_s"]) for r in rows}
    for pc, cc, shift, klass, why in (("trial_index", "trial_index", 0, "exact", ""), ("t_s", "t_s", None, "exact", "minus the trial's target_onset_s"),
                                      ("hand_x", "hand_x", 512, "exact", "plus 512"), ("hand_y", "hand_y", 384, "exact", "plus 384"),
                                      ("cursor_x", "cursor_x", 512, "format", "the program turns about (512, 384) in window px, in float; the player about (0, 0), in double: the last bits differ, and at the C file's 3 decimals some rows round the other way (F25)"),
                                      ("cursor_y", "cursor_y", 384, "format", "as cursor_x (F25)")):
        if len(path) != len(traj):
            report.append(("path.csv rows", "%d vs %d" % (len(path), len(traj)), "FAIL", ""))
            bad += 1
            break
        d = 0
        for r, c in zip(path, traj):
            v = float(r[pc]) - onset[r["trial_index"]] if shift is None else (float(r[pc]) + shift)
            if not same_at(v, c[cc]):
                d += 1
        over = klass == "format" and d > FORMAT_BOUND.get("--traj " + cc, 0)
        status = "exact" if not d else ("FAIL" if klass == "exact" or over else klass)
        bad += 1 if (d and (klass == "exact" or over)) else 0
        report.append(("path.csv %s <- --traj %s" % (pc, cc), "%d/%d differ" % (d, len(path)), status, why or "equal"))
    # every other column of the program's files, and why it is not compared
    for cc, why in (("cycle", "analysis: rep plus 0, 5 or 15 by node (3.18)"), ("hand_third_deg", "analysis on streams/path.csv"),
                    ("hand_pv_deg", "analysis on streams/path.csv"), ("n_samples", "analysis: the rows of streams/path.csv a trial"),
                    ("input", "session.json's signal_roles (D40)"), ("early_move", "analysis: n_early above 0")):
        report.append(("(none) <- %s" % cc, "not compared", "n/a", why))
    print_report("Visuomotor rotation (trial_visuomotor.c; the player's order, the program's hand)", report)
    return bad


def block_of(node):
    """blocks/b3_reach -> "3": the program's block number."""
    m = re.match(r"blocks/b(\d+)_", node)
    return m.group(1) if m else node


def do_bimanual(a, ex):
    src = os.path.join(HERE, "bimanual")
    pack = os.path.join(a.out, "bimanual.ysppak")
    build_pack(a.bin, src, pack)
    profile = v2_rig(a, "bimanual")
    common = ["--sim", "--seed", "3", "--field", "participant=1", "--rig", profile, "--sim-gain", "left=0.0078125", "--sim-gain", "right=0.0078125"]
    draws = os.path.join(a.out, "bimanual_draws.csv")
    player(a.bin, [pack, "--out", os.path.join(a.out, "bimanual_draws"), "--test-draws", draws] + common)
    names = {x: os.path.join(a.out, "bimanual_c" + x) for x in (".csv", "_reaches.csv", "_track.csv", "_freq.csv", "_samples.csv")}
    run([tool(a.bin, "trial_bimanual"), "--sim", "--mapping", "split", "--perturb", "rotation", "--design-timing", "--test-order", draws,
         "--test-samples-out", names["_samples.csv"], "--out", names[".csv"], "--reaches-out", names["_reaches.csv"],
         "--track", names["_track.csv"], "--freq", names["_freq.csv"]])
    p1 = os.path.join(a.out, "bimanual_p")
    player(a.bin, [pack, "--out", p1, "--samples", names["_samples.csv"]] + common)
    rows = [r for r in read_csv(os.path.join(p1, "trials.csv")) if r["trial_type"] == "reach"]
    c_rows = read_c(names["_reaches.csv"])
    pairs = [("aim_from_x", "start_x_cm", "exact", "", 1.0, 0.0), ("aim_from_y", "start_y_cm", "exact", "y with a sign change", -1.0, 0.0),
             ("aim_x", "target_x_cm", "exact", "", 1.0, 0.0), ("aim_y", "target_y_cm", "exact", "y with a sign change", -1.0, 0.0),
             ("aim_deg", "target_deg", "exact", "", 1.0, 0.0), ("index", "block_reach", "exact", "", 1.0, 0.0),
             ("perturb", "perturbation", "exact", "", 1.0, 0.0)]
    pairs += [(c, c, "exact", "", 1.0, 0.0) for c in ("rt_s", "onset_tier", "onset_src", "rsp_flags")]
    pairs += [("rsp_tier", "move_tier", "exact", "", 1.0, 0.0), ("peak_speed", "peak_speed_cm_s", "exact", "", 1.0, 0.0),
              ("fast", "fast", "exact", "", 1.0, 0.0), ("arrived_s", "mt_s", "exact", "arrived_s minus 1 (stay_s)", 1.0, -1.0),
              ("rsp_role", "move_hand", "map", "left and right for L and R", {"left": "L", "right": "R"}, 0.0),
              ("node", "block", "map", "the block's number in the node's name (blocks/b1_reach)", block_of, 0.0)]
    report, bad = v2_columns(rows, c_rows, pairs)
    # the tracking trials' rows (the program's --out) by the columns 3.18 calls Same
    trows = [r for r in read_csv(os.path.join(p1, "trials.csv")) if r["trial_type"] == "track"]
    rep2, bad2 = v2_columns(trows, read_c(names[".csv"]), [
        ("node", "block", "map", "tracking: the block's number in the node's name", block_of, 0.0),
        ("index", "block_trial", "exact", "tracking", 1.0, 0.0), ("perturb", "perturbation", "exact", "tracking", 1.0, 0.0),
        ("mapping", "mapping", "exact", "tracking (data)", 1.0, 0.0)])
    report += rep2
    bad += bad2
    inv_bad = state_invariant(os.path.join(p1, "events.csv"))
    report.append(("state rows: entry times and frames never go back", inv_bad or "hold", "FAIL" if inv_bad else "exact", "3.11 rules 1 and 2 (tests/model's INVARIANT)"))
    bad += 1 if inv_bad else 0
    for cc, why in (("reach_index", "analysis: the row's rank among trial_type reach"),
                    ("init_err_deg", "analysis on streams/path.csv"), ("--out trial_index", "analysis: the rank among trial_type track"),
                    ("--out mse_cm2 .. r_theta_deg", "analysis on streams/frames.csv"), ("--out n_frames, n_missed", "analysis on streams/frames.csv and frames.csv"),
                    ("--out n_norecord", "cannot be written: the player waits for every flip record"),
                    ("--out n_other", "cannot be written: an adapter takes only its own device"),
                    ("--out onset_tier, onset_src", "analysis: the move state row's frame joined to frames.csv"),
                    ("--freq (every column)", "analysis on streams/frames.csv")):
        report.append(("(none) <- %s" % cc, "not compared", "n/a", why))
    # --track against streams/frames.csv, per tracking trial
    fr, tr = read_csv(os.path.join(p1, "streams", "frames.csv")), read_c(names["_track.csv"])
    if len(fr) != len(tr):
        report.append(("frames.csv rows", "%d vs %d" % (len(fr), len(tr)), "FAIL", ""))
        bad += 1
    else:
        t0 = {}
        for r in fr:
            t0.setdefault(r["trial_index"], float(r["t_s"]))
        for pc, cc, sc in (("t_s", "t_s", None), ("goal_x", "target_x", 1.0), ("goal_y", "target_y", -1.0), ("cursor_x", "cursor_x", 1.0),
                           ("cursor_y", "cursor_y", -1.0), ("left_x", "l_x", 1.0), ("left_y", "l_y", -1.0), ("right_x", "r_x", 1.0),
                           ("right_y", "r_y", -1.0), ("inside", "inside", 1.0)):
            d = half = 0
            for r, c in zip(fr, tr):
                v = float(r[pc]) - t0[r["trial_index"]] if sc is None else float(r[pc]) * sc
                if sc is None:
                    ok = abs(v - float(c[cc])) <= 0.5e-6 + 1e-9   # the C file's 6 decimals, rounded either way at a half
                else:
                    ok = same_at(v, c[cc])
                    # the program's float against the player's double, a
                    # value at a half of the C file's last decimal: it
                    # rounds the other way (F25)
                    if not ok and abs(v - float(c[cc])) <= 0.5 * 10 ** -dec_of(c[cc]) + 1e-6 and \
                            abs(abs(v * 10 ** dec_of(c[cc])) % 1 - 0.5) * 10 ** -dec_of(c[cc]) < 2e-6:   # a float's step near 10
                        half += 1
                d += 0 if ok else 1
            why = {"t_s": "minus the trial's first row; the C file's 6 decimals", "goal_x": "the program's target at the planned onset, the player's at the record's: one time on the simulated display (3.18 lists them as different on a real one)"}.get(pc, "y with a sign change" if sc == -1.0 else "")
            if d and half == d:
                # 0 rows at 60 Hz (33,120 rows); a faster display has more
                # rows, so more values land at a half
                report.append(("frames.csv %s <- --track %s" % (pc, cc), "%d/%d differ" % (d, len(fr)), "format",
                               "%s; every one a value at a half of the C file's last decimal that rounds the other way (the program's float, the player's double, F25)" % (why or "equal")))
                continue
            report.append(("frames.csv %s <- --track %s" % (pc, cc), "%d/%d differ" % (d, len(fr)), "exact" if not d else "FAIL", why or "equal"))
            bad += 1 if d else 0
    print_report("Two-mouse tracking and reaching (trial_bimanual.c --design-timing; the player's draws, the program's hands)", report)
    return bad


EXAMPLES = {"rdk": do_rdk, "stroop": do_stroop, "afc2": do_afc2, "oddball": do_oddball, "visuomotor": do_visuomotor,
            "bimanual": do_bimanual}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="the build folder (ysp_player, ypak, the C examples)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--font", default=None)
    ap.add_argument("--only", default=None)
    ap.add_argument("--hz", default=None, help="the simulated display's rate (default: the programs' 60 Hz)")
    a = ap.parse_args()
    global HZ
    HZ = a.hz
    a.bin = os.path.abspath(a.bin)
    a.out = os.path.abspath(a.out or os.path.join(a.bin, "player_compare"))
    if not os.path.isdir(a.out):
        os.makedirs(a.out)
    if not a.font:
        for f in ("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
            if os.path.exists(f):
                a.font = f
                break
    bad = 0
    try:
        for name, fn in EXAMPLES.items():
            if a.only and name != a.only:
                continue
            if name == "stroop" and not a.font:
                print("\n## Stroop\n\nskipped: no font (--font PATH)\n")
                continue
            bad += fn(a, name)
    except Fail as e:
        print("FAILED: %s" % e, file=sys.stderr)
        return 1
    print("\nclasses: exact equal on one realized design; format: different by the format's decision; n/a: not comparable")
    if bad:
        print("%d exact columns differ" % bad, file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
