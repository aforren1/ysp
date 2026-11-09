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
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


class Fail(Exception):
    pass


def tool(bin_dir, name):
    for d in (bin_dir, os.path.join(bin_dir, "pack"), os.path.join(bin_dir, "player")):
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    raise Fail("no program %s under %s" % (name, bin_dir))


def run(args, cwd=None):
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
    return run([os.path.join(bin_dir, "player", "ysp_player")] + args)


def build_pack(bin_dir, src, dest):
    run([os.path.join(bin_dir, "pack", "ypak"), "build", os.path.join(src, "pack.json"), "-o", dest], cwd=src)


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
         "--trial-seeds", seeds])
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
         "--rig-dir", rig, "--cal", os.path.join(rig, "display.yspcal")])
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
    report = []
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
    frame_grid = all(abs(s * 60 - round(s * 60)) < 1e-6 for s in soa)
    report.append(("soa_s on the display's frames (rate=display)", "%d values, 0.8 to 1.2 s" % len(soa), "exact" if frame_grid and min(soa) >= 0.8 and max(soa) <= 1.2 else "FAIL", "jitter snapped to 60 Hz"))
    print_report("Oddball EEG (trigger_flip.c: not comparable column for column)", report)
    return bad


EXAMPLES = {"rdk": do_rdk, "stroop": do_stroop, "afc2": do_afc2, "oddball": do_oddball}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="the build folder (ysp_player, ypak, the C examples)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--font", default=None)
    ap.add_argument("--only", default=None)
    a = ap.parse_args()
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
