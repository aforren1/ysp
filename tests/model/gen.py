#!/usr/bin/env python3
"""A random generator of version 2 definitions and scripted participants.

Each case is small (2 to 6 states, 0 to 3 exits each) and puts times on and
off frame boundaries on purpose: equal after_s, steps that end at a
response's deadline, presses and samples on frame onsets and a few ms before
the model's hits. A case is a folder with exp.json, pack.json, part.csv,
samples.csv (when it has a region) and expect.json (v2model's output).

  uv run --no-project python tests/model/gen.py SEED [--out DIR]
"""
import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import v2model as M  # noqa: E402

P = M.PERIOD
# GEN_AVOID: comma-separated features to leave out, so a known difference
# does not hide others: state_at (at_s in a state's steps), until_timeout.
AVOID = set(filter(None, os.environ.get("GEN_AVOID", "").split(",")))
# GEN_MULTI=1: responses that take several presses (max_responses 2 and 3,
# presses before and after the window, wait_for_key_release) and scripts
# with up to 4 presses a trial (player.md, script:FILE).
MULTI_ENV = os.environ.get("GEN_MULTI", "") not in ("", "0")
MULTI = MULTI_ENV
STIMS = ["st0", "st1", "st2", "st3"]
FLAGS = ["f0", "f1"]
MARKS = ["m0", "m1", "m2"]


def sec(ns):
    return float("%d.%09d" % (ns // M.NS, ns % M.NS))


def dur(rng, lo_frames=1, hi_frames=24):
    """A duration on, near or off a frame boundary, or at the lead rule's
    edge (timeline.h QUANTIZATION)."""
    n = rng.randint(lo_frames, hi_frames)
    c = rng.random()
    if c < 0.06:
        # Within half a frame: F40 (a timed exit is not taken on its
        # state's entry frame) and steps that end on the frame they start.
        return sec(rng.choice([1_000_000, 3_000_000, 5_000_000, M.LEAD - 1, M.LEAD, M.LEAD + 1, P - 1]))
    if c < 0.25:
        ns = n * P                       # on a boundary
    elif c < 0.35:
        ns = n * P + rng.choice([-1, 1])
    elif c < 0.45:
        ns = n * P - M.LEAD + rng.choice([0, 1])   # the half-frame edge
    elif c < 0.6:
        ns = rng.randint(1, 60) * 10_000_000       # 10 ms steps: shared ties
    else:
        ns = rng.randint(n * P - P // 2, n * P + P // 2) // 1_000_000 * 1_000_000
    return sec(max(ns, 1_000_000))


class Gen:
    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.seed = seed

    def steps(self, regions, depth=0, allow_until=True, allow_flag=True, allow_at=True, state=True):
        rng = self.rng
        if state and "state_at" in AVOID:
            allow_at = False
        out = []
        n = rng.choice([0, 1, 1, 2, 2, 3, 4])
        for _ in range(n):
            c = rng.random()
            if c < 0.28:
                out.append({"on": rng.choice(STIMS)})
            elif c < 0.38:
                out.append({"off": rng.choice(STIMS)})
            elif c < 0.58:
                out.append({"wait_s": dur(rng, 1, 12)})
            elif c < 0.63 and allow_at:
                out.append({"at_s": dur(rng, 0, 12)})
            elif c < 0.72:
                out.append({"mark": rng.choice(MARKS)})
            elif c < 0.80 and allow_flag:
                out.append({"flag": rng.choice(FLAGS), "value": rng.random() < 0.7})
            elif c < 0.86:
                out.append({"set": rng.choice(STIMS), "props": {"x": rng.choice([-20, 0, 20])}})
            elif c < 0.94 and allow_until and regions and depth == 0:
                u = {"until": rng.choice(list(regions))}
                if rng.random() < 0.7 and "until_timeout" not in AVOID:
                    u["timeout_s"] = dur(rng, 3, 30)
                out.append(u)
            elif allow_flag and depth == 0:
                k = "if" if rng.random() < 0.7 else "unless"
                body = self.steps(regions, depth + 1, False, False, False)
                if body:
                    out.append({k: "$" + rng.choice(FLAGS), "steps": body})
        return out

    def regions(self):
        rng = self.rng
        regs = {}
        for i in range(rng.choice([0, 0, 1, 1, 2])):
            d = {"control": "region", "of": "hand", "radius": rng.choice([10, 20])}
            if rng.random() < 0.3:
                d["center"] = [50, 0]
            if rng.random() < 0.2:
                d["outside"] = True
            st = rng.random()
            if st < 0.45:
                pass                                  # stay_s 0: entry now
            elif st < 0.7:
                d["stay_s"] = dur(rng, 1, 8)
            else:
                d["stay_s"] = sec(rng.randint(1, 6) * P)
            regs["r%d" % i] = d
        return regs

    def response(self):
        rng = self.rng
        if rng.random() < (0.1 if MULTI else 0.4):
            return None
        r = {"role": "keys", "choices": ["f", "j"], "from": rng.choice(STIMS)}
        # Most windows have a deadline, so that most trials end (a window
        # with no deadline that no press ends waits for ever, F34 rule 3).
        if rng.random() < 0.03:
            pass                                      # no deadline
        else:
            r["duration_s"] = dur(rng, 6, 60)
        if rng.random() < 0.3:
            r["minimum_valid_rt_s"] = dur(rng, 1, 6)
            if "duration_s" in r and r["minimum_valid_rt_s"] >= r["duration_s"]:
                del r["minimum_valid_rt_s"]
        if rng.random() < 0.4:
            r["response_ends_trial"] = False
            if rng.random() < 0.5:
                r["max_responses"] = rng.choice([1, 2])
        if rng.random() < 0.3:
            r["key_answer"] = "f"
        if rng.random() < 0.2 and "release" not in AVOID:
            r["wait_for_key_release"] = True
        if MULTI:
            if "duration_s" not in r:
                r["duration_s"] = dur(rng, 12, 60)
                if r.get("minimum_valid_rt_s", 0) >= r["duration_s"]:
                    r.pop("minimum_valid_rt_s", None)
            if rng.random() < 0.7:
                r["response_ends_trial"] = False
                r["max_responses"] = rng.choice([0, 2, 2, 3, 3])
                if r["max_responses"] == 0:
                    del r["max_responses"]
            if rng.random() < 0.5 and "minimum_valid_rt_s" not in r:
                r["minimum_valid_rt_s"] = dur(rng, 1, 6)
                if r["minimum_valid_rt_s"] >= r["duration_s"]:
                    del r["minimum_valid_rt_s"]
            r["wait_for_key_release"] = rng.random() < 0.4
            if not r["wait_for_key_release"]:
                del r["wait_for_key_release"]
        return r

    def exit(self, names, regions, has_resp, instant_ok=True):
        rng = self.rng
        e = {}
        c = rng.random()
        if c < 0.35:
            e["after_s"] = dur(rng, 1, 30)
        elif c < 0.6 and regions:
            e["on"] = rng.choice(list(regions))
        elif c < 0.75 and has_resp:
            e["on"] = "response"
        elif instant_ok:
            if rng.random() < 0.6:
                e["if"] = "$" + rng.choice(FLAGS)
        else:
            e["after_s"] = dur(rng, 1, 30)
        e["to"] = rng.choice(names + ["end", "end"])
        if rng.random() < 0.3:
            e["outcome"] = rng.choice(["o1", "o2"])
        return e

    def definition(self):
        rng = self.rng
        regions = self.regions()
        resp = self.response()
        events_tpl = rng.random() < 0.15
        t = {}
        if regions:
            t["controls"] = regions
        if resp:
            t["response"] = resp
        if events_tpl:
            t["events"] = self.steps(regions, allow_flag=False, state=False)
            if rng.random() < 0.5:
                t["after"] = self.steps({}, allow_until=False, allow_flag=False, allow_at=False)
        else:
            n = rng.randint(2, 6)
            names = ["s%d" % i for i in range(n)]
            states = []
            for i in range(n):
                st = {"name": names[i]}
                s = self.steps(regions)
                if s:
                    st["steps"] = s
                ex = [self.exit(names, regions, resp is not None) for _ in range(rng.choice([0, 0, 1, 1, 2, 3]))]
                # 3.11 rule 10: a last instant exit has no `if` (mostly).
                if ex and all(("on" not in e and "after_s" not in e) for e in ex) and rng.random() < 0.9:
                    ex[-1].pop("if", None)
                if ex:
                    st["exits"] = ex
                if rng.random() < 0.12:
                    st["max_visits"] = rng.choice([1, 2])
                    ov = {"to": rng.choice(names + ["end"])}
                    if rng.random() < 0.5:
                        ov["outcome"] = "over"
                    st["over"] = ov
                states.append(st)
            t["states"] = states
            t["flags"] = list(FLAGS)
            shared = []
            if rng.random() < 0.25:
                for _ in range(rng.randint(1, 2)):
                    e = self.exit(names, regions, resp is not None, instant_ok=False)
                    e["in"] = sorted(rng.sample(names, rng.randint(1, n)), key=names.index)
                    shared.append(e)
            # Most templates end: a shared exit to end after 2 to 4 s in the
            # states that already have exits (so no state loses its steps'
            # end), and a waiting until with a timeout.
            # Cycles end too: a visit limit with `over` to end (D20 rule 12)
            # on most states that have none.
            for st in states:
                if "max_visits" not in st and rng.random() < 0.7:
                    st["max_visits"] = rng.randint(3, 6)
                    st["over"] = {"to": "end"}
            with_exits = [st["name"] for st in states if st.get("exits")]
            if with_exits and rng.random() < 0.9:
                shared.append({"after_s": rng.choice([2.5, 3, 3.5, 4]), "to": "end", "in": with_exits})
            if shared:
                t["exits"] = shared
        # The response's `from` must be shown by a step (experiment.md, "A
        # response's from"): add one in the first state or the events.
        if resp:
            on = {"on": resp["from"]}
            if events_tpl:
                t["events"].insert(rng.randint(0, len(t["events"])) if not any("until" in s for s in t["events"]) else 0, on)
            else:
                st = t["states"][rng.randrange(len(t["states"]))]
                st.setdefault("steps", []).insert(0, on)
        d = {
            "format": "ysp-experiment", "version": 2, "name": "gen%d" % self.seed,
            "stimuli": {s: {"kind": "shape", "shape": "rect", "w": 10, "h": 10} for s in STIMS},
            "trials": {"t": t},
            "timeline": [{"name": "m", "trial": "t", "rules": ["reps %d" % rng.randint(2, 4)]}],
        }
        if resp:
            d["devices"] = {"keys": {"direction": "in", "family": ["keyboard"]}}
        if regions:
            d["roles"] = {"hand": {"type": "position2d", "units": "display_px"}}
        return d

    def samples(self, regions, span_ns):
        """A hand path: positions inside, on the edge of and outside the
        regions, at times on and near frame onsets."""
        if not regions:
            return []
        rng = self.rng
        pts = [(0, 0), (5, 0), (10, 0), (20, 0), (50, 0), (60, 0), (100, 0), (0, 100)]
        out = [(M.FRAME0, *rng.choice(pts))]
        t = M.FRAME0
        while t < M.FRAME0 + span_ns:
            c = rng.random()
            if c < 0.4:
                t = M.onset(M.hit_frame(t + rng.randint(1, 20) * P))
            elif c < 0.6:
                t = M.onset(M.hit_frame(t + rng.randint(1, 20) * P)) + rng.choice([-1, 1, -3_000_000, 3_000_000])
            else:
                t += rng.randint(1, 400) * 1_000_000
            out.append((t, *rng.choice(pts)))
        return out

    def presses(self, r, dur_ns, mrt, rt0, hold0):
        """1 to 4 presses in time order, each after the last release
        (player.md): before the minimum RT, in the window, at and after
        its deadline."""
        rng = self.rng
        out = []
        c = rng.random()
        rt = mrt - rng.choice([1, 1_000_000, 20_000_000]) if (mrt and c < 0.3) else rt0
        rt = max(rt, 0)
        for _ in range(rng.randint(1, 4)):
            hold = rng.choice([20_000_000, rng.randint(1, 6) * P + rng.choice([0, -1, 1]),
                               rng.randint(5, 200) * 1_000_000])
            out.append((rng.choice(["f", "j"]), sec(rt), sec(hold)))
            # Not 0: the player 0.8.0 checks rt_s + hold_s against the next
            # rt_s in double seconds, so an exact tie can read as an
            # overlap and the script is refused (a finding, not a rule).
            gap = rng.choice([1, 1, P, rng.randint(1, 300) * 1_000_000])
            nxt = rt + hold + gap
            if dur_ns and rng.random() < 0.15:
                nxt = max(nxt, dur_ns + rng.choice([-1, 0, 1]))
            rt = nxt
        return out

    def script(self, defn, trials_expect, n):
        rng = self.rng
        r = defn["trials"]["t"].get("response")
        rows = {}
        if r is None:
            return rows
        dur_ns = M.ns_of(r.get("duration_s", 0))
        mrt = M.ns_of(r.get("minimum_valid_rt_s", 0))
        for i in range(n):
            if rng.random() < 0.2:
                continue
            c = rng.random()
            if c < 0.15:
                rt = 0
            elif c < 0.3:
                rt = rng.randint(1, 30) * P + rng.choice([0, 0, -1, 1])
            elif c < 0.45 and dur_ns:
                rt = dur_ns + rng.choice([-3_000_000, -1, 0])
            elif c < 0.55 and mrt:
                rt = mrt + rng.choice([-1, 0, 1])
            elif c < 0.7 and trials_expect and i < len(trials_expect):
                # A few ms before or at one of the model's state changes.
                tr = trials_expect[i]
                ends = [M.ns_of(s["end_s"]) for s in tr["states"]]
                L = [x for x in tr["lands"] if x["kind"] == "onset" and x["name"] == r["from"]]
                if ends and L:
                    o = M.onset(L[0]["frame"])
                    cand = [e - o for e in ends if e - o > 0]
                    if cand:
                        rt = rng.choice(cand) + rng.choice([0, -1, -2_000_000, -5_000_000])
                    else:
                        rt = rng.randint(50, 900) * 1_000_000
                else:
                    rt = rng.randint(50, 900) * 1_000_000
            else:
                rt = rng.randint(50, 900) * 1_000_000
            rt = max(rt, 0)
            hold = 0.02
            if r.get("wait_for_key_release"):
                hold = sec(rng.choice([rng.randint(1, 12) * P + rng.choice([0, 0, -1, 1]),
                                       rng.randint(5, 300) * 1_000_000]))
            rows[i] = [(rng.choice(["f", "j"]), sec(rt), hold)]
            if MULTI:
                rows[i] = self.presses(r, dur_ns, mrt, rt, hold)
        return rows


def make_case(seed):
    """Returns (defn, script rows, samples, expect)."""
    g = Gen(seed)
    # Refused definitions are a quarter of the cases at most: draw again.
    for _ in range(6):
        defn = g.definition()
        if M.predict_refusal(defn) is None or g.rng.random() < 0.25:
            break
    n = M.leaf_of(defn)[1]
    regions = defn["trials"]["t"].get("controls", {})
    samples = g.samples(regions, 6 * M.NS * n)
    expect = {"refused": M.predict_refusal(defn)}
    script = {}
    if expect["refused"] is None:
        trials, _ = M.simulate(defn, None, samples)
        script = g.script(defn, trials, n)
        # Keep samples out of the windows whose order 3.11 does not decide
        # (Run.flag_window); a few rounds, then keep the GAP.
        for _ in range(12):
            run = M.Run(defn, script, samples)
            run.run()
            if "arm-order" not in run.gaps or not run.amb:
                break
            samples = [s for s in samples if s[0] not in run.amb or s[0] == M.FRAME0]
        trials, gaps = M.simulate(defn, script, samples)
        expect = {"refused": None, "trials": trials, "gaps": gaps}
    return defn, script, samples, expect


def write_case(seed, d):
    defn, script, samples, expect = make_case(seed)
    os.makedirs(d, exist_ok=True)

    def w(name, text):
        with open(os.path.join(d, name), "w", newline="\n") as f:
            f.write(text)
    w("exp.json", json.dumps(defn, indent=1) + "\n")
    w("pack.json", json.dumps({
        "format": "ysp-pack-source", "version": 1,
        "experiment": {"entry": "experiment/main.json", "source": "exp.json",
                       "media_type": "application/vnd.ysp.experiment"},
        "resources": []}, indent=1) + "\n")
    w("part.csv", "trial_index,response,rt_s,hold_s\n" + "".join(
        "%d,%s,%.9f,%.9f\n" % (i, r[0], r[1], r[2]) for i, rows in sorted(script.items()) for r in rows))
    if samples:
        w("samples.csv", "t_s,role,x,y\n" + "".join(
            "%s,hand,%g,%g\n" % (M.fmt(t - M.FRAME0), x, y) for (t, x, y) in samples))
    w("expect.json", json.dumps(expect, indent=1) + "\n")
    return expect


def main(argv):
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("seed", type=int)
    ap.add_argument("--out", default=None)
    a = ap.parse_args(argv)
    if a.out:
        write_case(a.seed, a.out)
    else:
        defn, script, samples, expect = make_case(a.seed)
        print(json.dumps({"definition": defn, "script": script, "samples": len(samples), "expect": expect}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
