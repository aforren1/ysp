#!/usr/bin/env python3
"""Compares ysp_player --sim with the reference model on generated cases.

For each seed: gen.py writes a case, `ypak build` packs it, `ysp_player
--sim` runs it, and the trials' state rows, landing rows (onset, offset,
mark, set), end times, outcomes, responses, RTs and flags are compared with
v2model's, exactly at the CSV's 9 decimals. A loader refusal is compared
with v2model.predict_refusal(). A failing case is shrunk (states, exits,
steps dropped while it still fails) and printed with its seed.

  uv run --no-project python tests/model/diff.py --bin BUILD --seeds 0:2000 \\
      --work SCRATCH [--jobs 8]

Programs run in folders made under --work (mkdtemp); nothing is deleted.
"""
import argparse
import ast
import re
import copy
import csv
import json
import multiprocessing
import os
import subprocess
import sys
import tempfile
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen  # noqa: E402
import v2model as M  # noqa: E402

LAND = ("onset", "offset", "mark", "set")
# Paths that override BIN/pack/ypak and BIN/player/ysp_player (player_test.py
# passes its own, a mutation test another player).
YPAK = None
PLAYER = None
# --hz: the player's --sim-hz and the model's grid; None is 60 Hz with no
# option, so a 60 Hz run's commands stay as they were. --late: the hand's
# samples reach the player this many seconds after their time
# (--test-sample-late); the model has no arrival time, so it predicts the
# rows of samples fed at their time.
HZ = None
LATE = None
EXTRA = []   # --extra: more player options (experiments: --test-record-delay N)


def setup(hz, late, extra=None):
    global HZ, LATE, EXTRA
    HZ, LATE = hz, late
    EXTRA = list(extra or [])
    gen.set_hz(float(hz) if hz is not None else 60.0)

# The fixed set that tests/player/player_test.py runs (t_v2_model): seeds
# whose cases passed on player 0.9.0 (v2-lower aa6df27) and reach no GAP, so
# none needs refound-in-frame or a future arm. SEEDS_MULTI run with
# GEN_MULTI (several presses a trial).
SEEDS = [27, 113, 690, 733, 1001, 1855, 2631, 3474, 3707, 4011, 4275, 4486, 4573, 4588, 5380, 7303,
         8425, 8822, 9175, 9583, 9609, 10572, 10691, 11790, 12002, 12109, 12293, 13099, 13106,
         13442, 13858, 14230, 14376, 14595, 14598, 14782, 15915, 16003, 16433, 16442,
         # sig.h's semantics of an arm in the past: a disarm of every control
         # of a state left ended the next state's arm (f9fad11)
         62160, 63378, 65482]
SEEDS_MULTI = [187, 504, 523, 814, 1242, 1445, 2737, 3665, 4018, 4762, 24301, 25201, 26962, 27583]


def run_player(bin_dir, d, defn, script, samples):
    """Writes the case into d, builds and runs it. Returns a dict."""
    with open(os.path.join(d, "exp.json"), "w", newline="\n") as f:
        json.dump(defn, f, indent=1)
    with open(os.path.join(d, "pack.json"), "w", newline="\n") as f:
        json.dump({"format": "ysp-pack-source", "version": 1,
                   "experiment": {"entry": "experiment/main.json", "source": "exp.json",
                                  "media_type": "application/vnd.ysp.experiment"},
                   "resources": []}, f)
    with open(os.path.join(d, "part.csv"), "w", newline="\n") as f:
        f.write("trial_index,response,rt_s,hold_s\n")
        for i, rows in sorted(script.items()):
            for r in rows:
                f.write("%d,%s,%.9f,%.9f\n" % (i, r[0], r[1], r[2]))
    if samples:
        with open(os.path.join(d, "samples.csv"), "w", newline="\n") as f:
            f.write("t_s,role,x,y\n")
            for (t, x, y) in samples:
                f.write("%s,hand,%g,%g\n" % (M.fmt(t - M.FRAME0), x, y))
    pk = os.path.join(d, "p.ysppak")
    b = subprocess.run([YPAK or os.path.join(bin_dir, "pack", "ypak"), "build", "pack.json", "-o", pk],
                       cwd=d, capture_output=True, text=True, timeout=60)
    if b.returncode != 0:
        return {"refused": (b.stdout + b.stderr).strip().splitlines()[-1:]}
    out = os.path.join(d, "out")
    n = 0
    while os.path.exists(out):
        n += 1
        out = os.path.join(d, "out%d" % n)
    cmd = [PLAYER or os.path.join(bin_dir, "player", "ysp_player"), pk, "--out", out, "--sim", "--seed", "1",
           "--field", "participant=1", "--max-seconds", "120",
           # A script with no rows makes the player 0.6.0 use `correct`
           # (player.md: a trial with no row is silent), so give `silent`.
           "--participant", "script:part.csv" if script else "silent"]
    if samples:
        cmd += ["--samples", "samples.csv"]
        if LATE is not None:
            cmd += ["--test-sample-late", "hand=%s" % LATE]
    if HZ is not None:
        cmd += ["--sim-hz", str(HZ)]
    cmd += EXTRA
    p = subprocess.run(cmd, cwd=d, capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        return {"refused": ["player exit %d: %s" % (p.returncode, (p.stdout + p.stderr).strip()[-300:])]}
    trials = {}
    with open(os.path.join(out, "trials.csv"), newline="") as f:
        for row in csv.DictReader(f):
            trials[int(row["trial_index"])] = {
                "time_elapsed_s": row["time_elapsed_s"], "outcome": row.get("outcome", ""),
                "response": row.get("response", ""), "rt_s": row.get("rt_s", ""),
                "flags": {k: int(row[k]) for k in gen.FLAGS if k in row and row[k] != ""},
                "states": [], "lands": []}
    with open(os.path.join(out, "events.csv"), newline="") as f:
        for row in csv.DictReader(f):
            ti = int(row["trial_index"])
            if ti not in trials:
                continue
            if row["kind"] == "state":
                trials[ti]["states"].append({"text": row["text"], "frame": int(row["frame"]),
                                             "t_s": row["t_s"], "end_s": row["end_s"],
                                             "code": int(row["code"])})
            elif row["kind"] in LAND:
                trials[ti]["lands"].append({"kind": row["kind"],
                                            "name": row["text"] if row["kind"] == "mark" else row["stimulus"],
                                            "frame": int(row["frame"]), "t_s": row["t_s"]})
    arms = None
    try:
        with open(os.path.join(out, "session.json")) as f:
            arms = json.load(f).get("sig_arms")
    except (OSError, ValueError):
        pass
    return {"refused": None, "trials": [trials[i] for i in sorted(trials)], "out": out, "sig_arms": arms}


def invariant(trials):
    """State rows that no reading of the format allows, or None. A row's
    entry time is the first frame's onset for a trial's first row and the
    end of the row before for the others (3.11 rule 1); so exit times never
    go back, the first row ends at or after its t_s, and entry frames never
    go back (3.11 rule 2). A row's end_s can be before its own t_s: a state
    entered by a hit, or by an F40 timed exit, between frames has an entry
    time before its entry frame's onset, and an exit at that time (an
    instant exit, a hit at the arm) ends it there."""
    def ns(x):
        a, b = x.split(".")
        return int(a) * M.NS + int(b.ljust(9, "0")[:9])
    for i, tr in enumerate(trials):
        rows = tr["states"]
        for j, r in enumerate(rows):
            if j == 0:
                if ns(r["end_s"]) < ns(r["t_s"]):
                    return "trial %d state row 0 ends before the trial's first onset: %s" % (i, r)
                continue
            p = rows[j - 1]
            if ns(r["end_s"]) < ns(p["end_s"]):
                return "trial %d state row %d ends before its entry (row %d's end %s): %s" % (i, j, j - 1, p["end_s"], r)
            if r["frame"] < p["frame"]:
                return "trial %d state row %d is on a frame before row %d's: %s" % (i, j, j - 1, r)
    return None


def trial_gaps(exp, diff):
    """The GAPs of the trial where the first difference is (the trials
    before it agree, so an earlier GAP did not change rows)."""
    if not diff or exp.get("refused") is not None or not diff.startswith("trial "):
        return exp.get("gaps", []) if diff and not diff.startswith("trial ") else []
    i = int(diff.split()[1])
    tr = exp["trials"]
    return tr[i]["gaps"] if i < len(tr) else []


def compare(exp, got):
    """Returns the first difference as text, or None."""
    loop = exp["refused"] is None and exp["trials"] and exp["trials"][-1].get("loop")
    guard = got["refused"] is not None and "at one time" in " ".join(got["refused"])
    if loop or guard:
        if loop and guard:
            return None
        return "loop: model %s, player %r" % (bool(loop), got["refused"])
    hang = exp["refused"] is None and exp["trials"] and exp["trials"][-1].get("hang")
    budget = got["refused"] is not None and "budget" in " ".join(got["refused"])
    if hang or budget:
        if hang and budget:
            return None
        return "hang: model %s, player %r" % (bool(hang), got["refused"])
    if (exp["refused"] is None) != (got["refused"] is None):
        return "refusal: model %r, player %r" % (exp["refused"], got["refused"])
    if exp["refused"] is not None:
        return None
    et, gt = exp["trials"], got["trials"]
    for i in range(max(len(et), len(gt))):
        if i >= len(et) or i >= len(gt):
            return "trial %d: model has %d trials, player %d" % (i, len(et), len(gt))
        e, g = et[i], gt[i]
        for j in range(max(len(e["states"]), len(g["states"]))):
            es = e["states"][j] if j < len(e["states"]) else None
            gs = g["states"][j] if j < len(g["states"]) else None
            if es != gs:
                return "trial %d state row %d: model %s, player %s" % (i, j, es, gs)
        el = Counter((x["kind"], x["name"], x["frame"]) for x in e["lands"])
        gl = Counter((x["kind"], x["name"], x["frame"]) for x in g["lands"])
        if el != gl:
            only_e = sorted((el - gl).elements())
            only_g = sorted((gl - el).elements())
            return "trial %d landings: model only %s, player only %s" % (i, only_e[:4], only_g[:4])
        for k in ("time_elapsed_s", "outcome", "response", "rt_s", "flags"):
            if e[k] != g[k]:
                return "trial %d %s: model %r, player %r" % (i, k, e[k], g[k])
    return None


def signature(diff):
    """The kind of a difference: what differs, not where."""
    m = re.match(r"trial \d+ (state row \d+|landings|\w+)", diff)
    if not m:
        return diff.split(":")[0]
    kind = re.sub(r"\d+", "N", m.group(1))
    if kind.startswith("state row"):
        mm = re.search(r"model (\{.*?\}|None), player (\{.*?\}|None)", diff)
        a, b = (ast.literal_eval(mm.group(1)), ast.literal_eval(mm.group(2))) if mm else (None, None)
        if a and b:
            kind += " " + ",".join(k for k in ("text", "frame", "end_s", "code") if a.get(k) != b.get(k))
        else:
            kind += " count"
    return kind


GUARD_RE = re.compile(r"sig\.h: (-?\d+) hits retracted, (-?\d+) row runs, (-?\d+) derived runs changed; (-?\d+) pinned, "
                      r"(-?\d+) before the window, (-?\d+) logs full, (-?\d+) no window; (-?\d+) unverified")
GUARD_KEYS = ("retracted", "rows_stale", "derived_stale", "pinned", "far", "full", "off", "unverified")
# player 0.12.0 (sig.h 0.7.0) adds the late calls' counts
GUARD_LATE_RE = re.compile(r"late: (-?\d+) before the window, (-?\d+) logs full, (-?\d+) no window, (-?\d+) unverified, "
                           r"(-?\d+) hits voided")
GUARD_LATE_KEYS = ("late_far", "late_full", "late_off", "late_unverified", "late_hits_voided")


def guard_counts(got):
    """The counts of the player's guard stop (v2.c v2_arm_check(), and the
    frame's arm_unverified check), or None when it did not stop."""
    txt = " ".join(got.get("refused") or [])
    # the message's tail is kept (300 characters): its counts come last
    if "internal guard" not in txt and not GUARD_RE.search(txt):
        return None
    m = GUARD_RE.search(txt)
    if m:
        d = dict(zip(GUARD_KEYS, (int(x) for x in m.groups())))
        m = GUARD_LATE_RE.search(txt)
        if m:
            d.update(zip(GUARD_LATE_KEYS, (int(x) for x in m.groups())))
        return d
    m = re.search(r"\((\d+) arm_unverified", txt)
    return {"unverified": int(m.group(1)) if m else -1}


def model_expect(defn, script, samples):
    ref = M.predict_refusal(defn)
    if ref:
        return {"refused": ref}
    trials, gaps = M.simulate(defn, script, samples)
    return {"refused": None, "trials": trials, "gaps": gaps}


# ---------------------------------------------------------------- shrinking

def _retarget(t, name):
    def fix(e):
        if e.get("to") == name:
            e["to"] = "end"
    for st in t.get("states", []):
        for e in st.get("exits", []):
            fix(e)
        if "over" in st:
            fix(st["over"])
    for e in t.get("exits", []):
        fix(e)
        if name in e.get("in", []):
            e["in"].remove(name)
    t["exits"] = [e for e in t.get("exits", []) if e.get("in")]
    if not t["exits"]:
        del t["exits"]


def shrinks(defn):
    """Smaller variants of a definition, the largest cuts first."""
    t = defn["trials"]["t"]
    rules = defn["timeline"][0]["rules"]
    n = int(rules[0].split()[1])
    if n > 1:
        d = copy.deepcopy(defn)
        d["timeline"][0]["rules"] = ["reps %d" % (n - 1)]
        yield d
    for i, st in enumerate(t.get("states", [])):
        if len(t["states"]) > 1:
            d = copy.deepcopy(defn)
            tt = d["trials"]["t"]
            name = tt["states"][i]["name"]
            del tt["states"][i]
            _retarget(tt, name)
            yield d
    for j in range(len(t.get("exits", []))):
        d = copy.deepcopy(defn)
        del d["trials"]["t"]["exits"][j]
        if not d["trials"]["t"]["exits"]:
            del d["trials"]["t"]["exits"]
        yield d
    for i, st in enumerate(t.get("states", [])):
        for j in range(len(st.get("exits", []))):
            d = copy.deepcopy(defn)
            s2 = d["trials"]["t"]["states"][i]
            del s2["exits"][j]
            if not s2["exits"]:
                del s2["exits"]
            yield d
        if "max_visits" in st:
            d = copy.deepcopy(defn)
            s2 = d["trials"]["t"]["states"][i]
            del s2["max_visits"], s2["over"]
            yield d
        for j in range(len(st.get("steps", []))):
            d = copy.deepcopy(defn)
            del d["trials"]["t"]["states"][i]["steps"][j]
            yield d
    for key in ("events", "after"):
        for j in range(len(t.get(key, []))):
            d = copy.deepcopy(defn)
            del d["trials"]["t"][key][j]
            yield d
    r = t.get("response")
    if r:
        for k in ("minimum_valid_rt_s", "key_answer", "max_responses"):
            if k in r:
                d = copy.deepcopy(defn)
                del d["trials"]["t"]["response"][k]
                yield d


def shrink(bin_dir, work, defn, script, samples, first_diff, gaps, budget=200):
    """Keeps a smaller definition while its first difference has the same
    kind and its trial reaches no GAP the original did not."""
    sig = signature(first_diff)
    kind = first_diff.split(":")[0]
    cur = defn
    tries = 0
    progress = True
    while progress and tries < budget:
        progress = False
        for cand in shrinks(cur):
            tries += 1
            if tries > budget:
                break
            try:
                exp = model_expect(cand, script, samples)
            except Exception:
                continue
            if exp["refused"] is not None and kind != "refusal":
                continue
            d = tempfile.mkdtemp(prefix="shr", dir=work)
            got = run_player(bin_dir, d, cand, script, samples)
            diff = compare(exp, got)
            if diff and signature(diff) == sig and set(trial_gaps(exp, diff)) <= set(gaps):
                cur = cand
                first_diff = diff
                progress = True
                break
    return cur, first_diff


# ---------------------------------------------------------------- driver

def check_set(work, seeds=None, seeds_multi=None):
    """Runs the fixed set in one process; returns a list of (seed, why) for
    each case that fails, or that reaches a GAP (the set must reach none)."""
    bad = []
    for multi, lst in ((False, SEEDS if seeds is None else seeds), (True, SEEDS_MULTI if seeds_multi is None else seeds_multi)):
        gen.MULTI = multi
        try:
            for seed in lst:
                defn, script, samples, exp = gen.make_case(seed)
                d = tempfile.mkdtemp(prefix="s%d_" % seed, dir=work)
                gaps = sorted({g for t in exp.get("trials", []) for g in t["gaps"]})
                if gaps:
                    bad.append((seed, "the case reaches GAPs %s" % gaps))
                    continue
                got = run_player(None, d, defn, script, samples)
                diff = None
                for side, tr in (("model", exp.get("trials")), ("player", got.get("trials"))):
                    if tr and invariant(tr):
                        diff = "invariant %s: %s" % (side, invariant(tr))
                diff = diff or compare(exp, got)
                if diff:
                    bad.append((seed, "%s (case %s)" % (diff, d)))
        finally:
            gen.MULTI = gen.MULTI_ENV
    return bad


def one(args):
    seed, bin_dir, work, do_shrink = args
    defn, script, samples, exp = gen.make_case(seed)
    d = tempfile.mkdtemp(prefix="s%d_" % seed, dir=work)
    with open(os.path.join(d, "expect.json"), "w", newline="\n") as f:
        json.dump(exp, f, indent=1)
    try:
        got = run_player(bin_dir, d, defn, script, samples)
    except subprocess.TimeoutExpired:
        got = {"refused": ["timeout"]}
    diff = compare(exp, got)
    inv_m = invariant(exp["trials"]) if exp.get("refused") is None and exp.get("trials") else None
    inv_p = invariant(got["trials"]) if got.get("refused") is None and got.get("trials") else None
    res = {"seed": seed, "dir": d, "inv_model": inv_m, "inv_player": inv_p, "refused": exp["refused"], "player_refused": got["refused"],
           "sig_arms": got.get("sig_arms"), "guard": guard_counts(got),
           "gaps": trial_gaps(exp, diff), "diff": diff,
           "hang": diff is None and exp["refused"] is None and bool(exp["trials"])
                   and bool(exp["trials"][-1].get("hang") or exp["trials"][-1].get("loop"))}
    if diff and do_shrink:
        small, sdiff = shrink(bin_dir, work, defn, script, samples, diff, res["gaps"])
        sd = os.path.join(d, "min")
        os.makedirs(sd, exist_ok=True)
        run_player(bin_dir, sd, small, script, samples)
        with open(os.path.join(sd, "expect.json"), "w", newline="\n") as f:
            json.dump(model_expect(small, script, samples), f, indent=1)
        res["min_diff"] = sdiff
        res["min_dir"] = sd
        res["min_gaps"] = trial_gaps(model_expect(small, script, samples), sdiff)
    return res


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="the build folder (pack/ypak, player/ysp_player)")
    ap.add_argument("--seeds", default="0:100", help="FROM:TO")
    ap.add_argument("--work", required=True)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--no-shrink", action="store_true")
    ap.add_argument("--log", default=None, help="a JSON lines file of every case")
    ap.add_argument("--fixed", action="store_true", help="run the fixed set (SEEDS, SEEDS_MULTI) only")
    ap.add_argument("--hz", default=None, help="the simulated display's rate (default 60 Hz, no option)")
    ap.add_argument("--late", default=None, help="the hand's samples reach the player S seconds late")
    ap.add_argument("--extra", default="", help="more player options, one string (the model does not see them)")
    a = ap.parse_args(argv)
    extra = a.extra.split()
    setup(a.hz, a.late, extra)
    lo, hi = (int(x) for x in a.seeds.split(":"))
    os.makedirs(a.work, exist_ok=True)
    if a.fixed:
        global YPAK, PLAYER
        YPAK = os.path.join(os.path.abspath(a.bin), "pack", "ypak")
        PLAYER = os.path.join(os.path.abspath(a.bin), "player", "ysp_player")
        bad = check_set(tempfile.mkdtemp(prefix="fixed", dir=a.work))
        for seed, why in bad:
            print("FAIL seed %d: %s" % (seed, why))
        print("fixed set: %d cases, %d failures" % (len(SEEDS) + len(SEEDS_MULTI), len(bad)))
        return 1 if bad else 0
    work = tempfile.mkdtemp(prefix="run", dir=a.work)
    jobs = [(s, os.path.abspath(a.bin), work, not a.no_shrink) for s in range(lo, hi)]
    n = refused = agreed_ref = bad = gap_bad = hangs = inv_m = inv_p = 0
    log = open(a.log, "w", newline="\n") if a.log else None
    guards = Counter()
    n_guard = 0
    arms = Counter()
    n_arms = 0
    with multiprocessing.Pool(min(a.jobs, 8), initializer=setup, initargs=(a.hz, a.late, extra)) as pool:
        for r in pool.imap_unordered(one, jobs):
            n += 1
            if log:
                log.write(json.dumps(r) + "\n")
                log.flush()
            if r.get("hang"):
                hangs += 1
            if r.get("guard"):
                n_guard += 1
                for k, v in r["guard"].items():
                    if v:
                        guards[k] += 1
            if r.get("sig_arms"):
                n_arms += 1
                for k, v in r["sig_arms"].items():
                    arms[k] += v
            for side, key in (("model", "inv_model"), ("player", "inv_player")):
                if r.get(key):
                    if side == "model":
                        inv_m += 1
                    else:
                        inv_p += 1
                    # A model row that breaks the invariant is a model bug,
                    # never a GAP; reported also where the case reaches one.
                    print("INVARIANT %s seed %d: %s" % (side, r["seed"], r[key]))
                    print("  case %s" % r["dir"])
            if r["refused"] is not None:
                refused += 1
                if r["diff"] is None:
                    agreed_ref += 1
            if r["diff"]:
                bad += 1
                g = r.get("min_gaps", r["gaps"])
                if g:
                    gap_bad += 1
                print("MISMATCH seed %d%s: %s" % (r["seed"], " (GAPs %s)" % ",".join(g) if g else "",
                                                  r.get("min_diff", r["diff"])))
                print("  case %s" % r.get("min_dir", r["dir"]))
                sys.stdout.flush()
    print("cases %d, refused by the model %d (agreed %d), hangs both agree on %d (rows not compared), "
          "mismatches %d (%d at a GAP); invariant failures: model %d, player %d" %
          (n, refused, agreed_ref, hangs, bad, gap_bad, inv_m, inv_p))
    print("guard stops %d (cases with a count above 0: %s); sessions with sig_arms %d, summed: %s" %
          (n_guard, dict(guards), n_arms, dict(arms)))
    return 1 if (bad or inv_m or inv_p) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
