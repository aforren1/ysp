#!/usr/bin/env python3
"""An executable reference model of format version 2 trial timing.

Source of truth: docs/experiment_format.md (part 2: 2.5 steps, 2.6
responses; part 3; decisions D16 to D41 with their dated notes) and the
readings F1 to F38 of docs/experiment.md. The player's source was not read.
Each rule cites its section. Where the text does not decide, a `GAP:` comment
names the reading taken; `Run.gaps` collects the GAPs a case reached, so a
difference from the player there is a question, not a player bug.

Scope: one leaf (`reps N`), one template, no variables. A template has
`states` (with shared `exits`, `flags`, `max_visits`/`over`, `until`) or
`events`/`after`. Steps: on, off, wait_s, at_s, mark, flag, set (a constant),
until, if on a flag. Exits: after_s, on a region, on response, instant with
`if`. Regions: one 2D absolute role (`hand`), a circle, `stay_s` literal,
`rate` sample. A key response (2.6) with a scripted participant.

All times are int64 ns on the simulated clock (2.5: durations in seconds in
the definition; the C APIs take ns). Standard library only.

Usage as a program:
  python v2model.py DEF.json [--participant script.csv] [--samples s.csv]
prints the expected rows as JSON.
"""
import json
import math
import sys

NS = 1_000_000_000

# The simulated display (player.md "--sim"; ysp/screen.h BACKEND_SIM).
# GAP: player.md's allowed sections give "60 Hz" only. Measured on --sim
# (probe 1, player 0.6.0 at 9cb7e2e): frame k's onset is 1000 s + (k + 7) x
# 16,666,667 ns, frame 0 the first trial's first frame.
PERIOD = 16_666_667
FRAME0 = 1000 * NS + 7 * PERIOD
# timeline.h QUANTIZATION: an event lands on the first frame with
# onset + lead x period >= t, lead x period truncated to whole ns; the
# default lead is 0.5 (docs/timeline.md "The default lead is half a frame").
LEAD = PERIOD // 2
# GAP: when a frame's flip record reaches the player. player.md
# (--test-record-delay N "hands each record to the player N frames later")
# implies a default; measured (probe 3): frame L's record is in at frame
# L + 1, before that frame's evaluate.
RECORD_DELAY = 1



def onset(k):
    return FRAME0 + k * PERIOD


def lead_frame(t):
    """First frame whose window reaches t (timeline.h; D20 amendment)."""
    return max(0, -((FRAME0 + LEAD - t) // PERIOD))


def hit_frame(t):
    """First frame whose onset is at or after t (D16; 3.11 rule 2)."""
    return max(0, -((FRAME0 - t) // PERIOD))


def ns_of(x):
    """Seconds in a definition to ns (D4). Measured to agree with the player
    for values with up to 9 decimals."""
    return int(round(float(x) * NS))


def fmt(ns):
    """The data set's time text: seconds with 9 decimals (2.10)."""
    if ns is None:
        return ""
    s = "-" if ns < 0 else ""
    ns = abs(ns)
    return "%s%d.%09d" % (s, ns // NS, ns % NS)


class Refused(Exception):
    pass


class Loop(Exception):
    """State changes at one time without end (see `zero-time-cycle`)."""


# ---------------------------------------------------------------- definition

class Exit:
    __slots__ = ("kind", "on", "after", "cond", "to", "outcome", "code", "src")

    def __init__(self, kind, to, code, src, on=None, after=None, cond=None, outcome=None):
        self.kind = kind          # 'on', 'after', 'instant'
        self.on = on              # a region's name or 'response'
        self.after = after        # ns
        self.cond = cond          # (flag, negate) or None
        self.to = to              # a state's name or 'end'
        self.outcome = outcome    # text or None
        self.code = code          # the state row's code (3.15)
        self.src = src            # 'own', 'shared', 'added', 'until_on', 'until_timeout'


class State:
    def __init__(self, name, index):
        self.name = name
        self.index = index
        self.parts = [[]]         # steps between untils (3.11 rule 9)
        self.untils = []          # (control, timeout ns or None) after part i
        self.exits = []           # own, then shared, then the added response exit
        self.authored = False     # own or shared exits (F34 rule 1)
        self.max_visits = None
        self.over = None          # (to, outcome)


class Region:
    def __init__(self, name, d):
        self.name = name
        c = d.get("center", [0, 0])
        self.cx, self.cy = float(c[0]), float(c[1])
        self.radius = float(d["radius"])
        self.outside = bool(d.get("outside", False))
        self.stay = ns_of(d.get("stay_s", 0))

    def test(self, xy):
        # 3.7 region: `le` the radius on the norm, `ge` with outside; a value
        # equal to the level passes both (D17 note: `ge` takes equality).
        if xy is None:
            return False      # sig.md: before its first sample a source has no value
        dist = math.hypot(xy[0] - self.cx, xy[1] - self.cy)
        return dist >= self.radius if self.outside else dist <= self.radius


class Response:
    def __init__(self, d):
        self.frm = d["from"]
        self.choices = []
        for c in d.get("choices", []):
            if isinstance(c, str):
                self.choices.append((c, c))
            else:
                self.choices.append((c.get("name", c["key"]), c["key"]))
        self.key_answer = d.get("key_answer")
        self.duration = ns_of(d.get("duration_s", 0))
        self.min_rt = ns_of(d.get("minimum_valid_rt_s", 0))
        self.ends_trial = d.get("response_ends_trial", True)   # 2.6 default
        self.max_responses = int(d.get("max_responses", 0))
        # 2.6: the player sets yrsp_desc.persist from response_ends_trial.
        self.persist = not self.ends_trial
        self.wait_release = bool(d.get("wait_for_key_release", False))


def _steps(lst):
    """Normalizes steps; returns the list and the untils in it (top level)."""
    out = []
    for s in lst:
        if "on" in s or "off" in s:
            k = "on" if "on" in s else "off"
            names = s[k] if isinstance(s[k], list) else [s[k]]
            out.append((k, names))
        elif "wait_s" in s:
            out.append(("wait", ns_of(s["wait_s"])))
        elif "at_s" in s:
            out.append(("at", ns_of(s["at_s"])))
        elif "mark" in s:
            out.append(("mark", s["mark"]))
        elif "flag" in s:
            out.append(("flag", s["flag"], bool(s["value"])))
        elif "set" in s:
            out.append(("set", s["set"]))
        elif "until" in s:
            to = s.get("timeout_s")
            out.append(("until", s["until"], ns_of(to) if to is not None else None))
        elif "if" in s or "unless" in s:
            neg = "unless" in s
            v = s["unless" if neg else "if"]
            out.append(("if", v[1:], neg, _steps(s["steps"])))
        else:
            raise ValueError("model: unsupported step %r" % (s,))
    return out


def _split(steps):
    parts, untils = [[]], []
    for s in steps:
        if s[0] == "until":
            untils.append((s[1], s[2]))
            parts.append([])
        else:
            parts[-1].append(s)
    return parts, untils


class Template:
    def __init__(self, name, d):
        self.name = name
        self.flags = list(d.get("flags", []))
        self.regions = {}
        for cn, cd in d.get("controls", {}).items():
            if cd.get("control") != "region":
                raise ValueError("model: only region controls")
            self.regions[cn] = Region(cn, cd)
        self.response = Response(d["response"]) if "response" in d else None
        if "states" in d:
            self.kind = "states"
            self._states(d)
        else:
            self.kind = "events"
            self.parts, self.untils = _split(_steps(d.get("events", [])))
            self.after = _steps(d.get("after", []))

    def _states(self, d):
        self.states = []
        self.by_name = {}
        for i, sd in enumerate(d["states"]):
            st = State(sd["name"], i)
            st.parts, st.untils = _split(_steps(sd.get("steps", [])))
            for j, e in enumerate(sd.get("exits", [])):
                st.exits.append(self._exit(e, j, "own"))
            if "max_visits" in sd:
                st.max_visits = int(sd["max_visits"])
                ov = sd["over"]
                st.over = (ov["to"], ov.get("outcome"))
            self.states.append(st)
            self.by_name[st.name] = st
        # D20 rule 11: a shared exit is one of each listed state's exits and
        # comes after the state's own exits in list order.
        for e in d.get("exits", []):
            for sn in e["in"]:
                st = self.by_name[sn]
                st.exits.append(self._exit(e, len(st.exits), "shared"))
        for st in self.states:
            st.authored = len(st.exits) > 0
        # 3.13: response_ends_trial true is a shared exit {on: response, to:
        # end} in every state, after their own exits. GAP (F28 wording "own
        # and shared exits (and the response's)"): after the authored shared
        # exits too, so its code is the count of own and shared exits.
        if self.response is not None and self.response.ends_trial:
            for st in self.states:
                st.exits.append(Exit("on", "end", len(st.exits), "added", on="response"))

    @staticmethod
    def _exit(e, code, src):
        cond = None
        if "if" in e:
            cond = (e["if"][1:], False)
        kind = "on" if "on" in e else ("after" if "after_s" in e else "instant")
        return Exit(kind, e["to"], code, src, on=e.get("on"),
                    after=ns_of(e["after_s"]) if "after_s" in e else None,
                    cond=cond, outcome=e.get("outcome"))


def leaf_of(defn):
    """The one leaf: (template name, trial count)."""
    tl = defn["timeline"]
    if len(tl) != 1 or "trial" not in tl[0]:
        raise ValueError("model: one leaf only")
    n = 1
    for r in tl[0].get("rules", []):
        w = r.split()
        if w[0] == "reps":
            n = int(w[1])
        else:
            raise ValueError("model: rules other than reps")
    return tl[0]["trial"], n


# ---------------------------------------------------------------- refusals

def _waiting_region(tpl, name):
    # F26 (D20 note, 2026-11-12): an `on` or `until` on a region whose stay_s
    # is 0 is met at its arm while the test holds, so it does not wait.
    r = tpl.regions.get(name)
    return r is not None and r.stay > 0


def predict_refusal(defn):
    """The loader's refusals a generated definition can hit, or None.

    D20 rule 6 with F5 and F26 (a cycle of edges that do not wait), the
    instant-exit rule (3.11 rule 10; 3.16), F37 (2.6), and 3.11 rule 6
    (`at_s` after an `until` in `events`)."""
    tname, _ = leaf_of(defn)
    td = defn["trials"][tname]
    tpl = Template(tname, td)
    r = tpl.response
    # F37 (2.6, DECIDED 2026-11-12): a key response that nothing ends.
    if r is not None and not r.ends_trial and r.duration == 0 and r.max_responses == 0:
        return "F37"
    # experiment.h 0.5.1 (the coordinator, 2026-11-12; was the GAP
    # min-rt-over-duration): a response whose minimum_valid_rt_s is at or
    # above its duration_s, in ns, is refused at load (response.h refuses
    # that desc). Literals only here: the generator gives no variables.
    if r is not None and r.duration > 0 and r.min_rt >= r.duration:
        return "min-rt-over-duration"
    # experiment.md, "A response's from": a step must show it (in `events`
    # for an events template; any state's steps with states).
    if r is not None:
        def shows(steps):
            for s in steps:
                if s[0] == "on" and r.frm in s[1]:
                    return True
                if s[0] == "if" and shows(s[3]):
                    return True
            return False
        if tpl.kind == "events":
            ok = any(shows(p) for p in tpl.parts)
        else:
            ok = any(shows(p) for st in tpl.states for p in st.parts)
        if not ok:
            return "from-not-shown"
    if tpl.kind == "events":
        seen_until = False
        for s in _steps(td.get("events", [])):
            if s[0] == "until":
                seen_until = True
            elif s[0] == "at" and seen_until:
                return "at_s after until"
        return None
    # 3.11 rule 10: a state whose exits are all instant must have a last
    # exit with no `if`. Shared exits count among its exits
    # (tests/experiment/good/v2_instant_shared.json's note).
    for st in tpl.states:
        own = [e for e in st.exits if e.src in ("own", "shared")]
        if own and all(e.kind == "instant" for e in own) and own[-1].cond is not None:
            return "instant-if-last"
    # D20 rule 6, F5, F26: every edge that is not an `on` or `after_s` exit
    # that waits does not wait: instant exits, `over`, and the steps' end of
    # a state with no authored exits unless its steps hold an `until` that
    # waits. The added response exit is no author's exit (F34 rule 1).
    edges = {st.name: [] for st in tpl.states}
    for i, st in enumerate(tpl.states):
        for e in st.exits:
            if e.src == "added" or e.to == "end":
                continue
            if e.kind == "instant":
                edges[st.name].append(e.to)
            elif e.kind == "on" and e.on != "response" and not _waiting_region(tpl, e.on):
                edges[st.name].append(e.to)
        if st.over is not None and st.over[0] != "end":
            edges[st.name].append(st.over[0])
        if not st.authored:
            waits = any(_waiting_region(tpl, c) for c, _ in st.untils)
            if not waits and i + 1 < len(tpl.states):
                edges[st.name].append(tpl.states[i + 1].name)
    color = {}

    def dfs(u):
        color[u] = 1
        for v in edges[u]:
            if color.get(v) == 1:
                return True
            if v not in color and dfs(v):
                return True
        color[u] = 2
        return False

    for st in tpl.states:
        if st.name not in color and dfs(st.name):
            return "cycle"
    return None


# ---------------------------------------------------------------- simulation

class Ev:
    """A timeline event of a step (2.5): it lands on the frame its time gives
    by the lead rule, not before the frame that built its table."""
    __slots__ = ("t", "kind", "name", "val", "built", "visit", "seq", "landed", "removed", "frame")

    def __init__(self, t, kind, name, val, built, visit, seq):
        self.t, self.kind, self.name, self.val = t, kind, name, val
        self.built, self.visit, self.seq = built, visit, seq
        self.landed = self.removed = False
        self.frame = None


class Det:
    """A region's hit detect (3.7 region; sig.md "End a step on a hold")."""

    def __init__(self, region, arm_t, held):
        self.r = region
        self.arm = arm_t
        self.made = None          # the frame of the arm
        self.hold = None          # the hold's start
        self.hit = None
        self.prev = False
        inside = region.test(held)
        if region.stay == 0:
            # D35 (f), G16: entry `now`: a test true at the arm is a hit at
            # the arm's time, with the input's held value.
            if inside:
                self.hit = arm_t
            self.prev = inside
        elif inside:
            # entry `arm`: a hold true at the arm counts from the arm.
            self.hold = arm_t

    def sample(self, t, xy):
        """Returns a hit time or None. The next sample finds a completed
        hold before it is tested (sig.md)."""
        if self.hit is not None:
            return None
        r = self.r
        if r.stay > 0 and self.hold is not None and self.hold + r.stay <= t:
            self.hit = self.hold + r.stay
            return self.hit
        inside = r.test(xy)
        if r.stay == 0:
            if inside and not self.prev:
                self.hit = t
            self.prev = inside
            return self.hit
        if inside:
            if self.hold is None:
                self.hold = max(t, self.arm)
                if r.stay == 0:
                    self.hit = self.hold
                    return self.hit
        else:
            self.hold = None
        return None

    def frame(self, o):
        """A still input completes a hold at a frame whose onset is at or
        after its end (sig.md)."""
        if self.hit is None and self.r.stay > 0 and self.hold is not None and self.hold + self.r.stay <= o:
            self.hit = self.hold + self.r.stay
            return self.hit
        return None


class Visit:
    def __init__(self, state, entry_t, entry_f, vid):
        self.state = state
        self.entry_t = entry_t
        self.entry_f = entry_f
        self.vid = vid
        self.part = 0
        self.part_t = entry_t
        self.part_f = entry_f
        self.waiting = False
        self.wait_t = None
        self.wait_f = None
        self.part_end = None      # the cursor's end of the running part
        self.dets = {}            # control name -> Det, for own and shared exits
        self.until_det = None
        self.events = []


class Trial:
    def __init__(self, index, k0):
        self.index = index
        self.k0 = k0
        self.gaps = set()         # the GAPs this trial reached
        self.rows = []            # state rows
        self.lands = []           # (kind, name, frame)
        self.outcome = None       # D36: the latest set
        self.flag_events = []     # (t, seq, name, value) that happened
        self.visits = {}
        self.done = None          # (time, frame) the states or events ended
        self.end_frame = None
        # response
        self.L = None             # the `from` landing frame
        self.press = None         # (t, name, rt) scheduled
        self.pi = 0               # the next scripted press
        self.n_valid = 0          # responses so far
        self.release = None
        self.resp_valid = None    # (name, rt)
        self.resp_end = None      # (t, kind) kind 'press' or 'deadline'
        self.resp_pending = None  # a press that waits for the record
        self.resp_seen = None     # frame the end was taken
        self.resp_used = False
        # events templates
        self.ev_done = None


class Run:
    """Runs a session of one leaf on the simulated display."""

    def __init__(self, defn, script=None, samples=None, max_trial_s=120):
        self.defn = defn
        tname, self.n = leaf_of(defn)
        self.tpl = Template(tname, defn["trials"][tname])
        self.script = script or {}
        self.samples = sorted(samples or [], key=lambda s: s[0])
        self.si = 0               # next sample
        self.hand = None
        self.fed_until = None
        self.gaps = set()
        self.amb = set()          # sample times in an arm-order window
        self.max_trial_ns = max_trial_s * NS
        self.seq = 0
        self.vid = 0

    # ------------------------------------------------------------ helpers
    def gap(self, name):
        self.gaps.add(name)
        tr = getattr(self, "tr", None)
        if tr is not None:
            tr.gaps.add(name)

    def flag_at(self, tr, name):
        best = None
        for (t, seq, n, v) in tr.flag_events:
            if n == name and (best is None or (t, seq) > best[0]):
                best = ((t, seq), v)
        return bool(best and best[1])

    def build(self, tr, steps, t0, frame, visit):
        """An op table from t0 (ytl_at): returns its events and the cursor's
        end (3.11 rule 7: wait_s, then and at_s move it; at_s counts from
        the state's entry time, 3.11 rule 6)."""
        cur = t0
        evs = []

        def walk(lst):
            nonlocal cur
            for s in lst:
                k = s[0]
                if k in ("on", "off"):
                    for nm in s[1]:
                        evs.append(self._ev(cur, k, nm, None, frame, visit))
                elif k == "wait":
                    cur += s[1]
                elif k == "at":
                    cur = t0 + s[1]
                elif k == "mark":
                    evs.append(self._ev(cur, "mark", s[1], None, frame, visit))
                elif k == "flag":
                    evs.append(self._ev(cur, "flag", s[1], s[2], frame, visit))
                elif k == "set":
                    evs.append(self._ev(cur, "set", s[1], None, frame, visit))
                elif k == "if":
                    # 2.5: the steps go in the table when the variable is
                    # true as the table is built; F14: a flag reads there.
                    v = self.flag_at(tr, s[1])
                    if v != s[2]:
                        walk(s[3])
        walk(steps)
        return evs, cur

    def _ev(self, t, kind, name, val, frame, visit):
        self.seq += 1
        return Ev(t, kind, name, val, frame, visit, self.seq)

    def flag_window(self, x):
        """GAP (arm order): an arm made at a frame (a timed exit, a response
        that became final, a hold that a later sample found) has a time x
        that is not the input's position: samples between x and the last fed
        input are fed before an arm at a past time, or after an arm at a
        future time. 3.11 rules 2 to 4 do not say which the arm sees; sig.md
        says an arm tests "the input's held value". The generator keeps
        samples out of these windows."""
        upto = self.fed_until
        if self.hand is None:
            return      # no held value yet: both orders give the same test
        k0 = self.tr.k0
        start = x == onset(k0) and k0 > 0
        for (t, _, _) in self.samples:
            if start and onset(k0 - 1) < t <= x:
                # An arm at the trial's first onset: the trial's start and
                # the steps that end at once may come before or after the
                # first frame's poll.
                hit = True
            elif x <= upto:
                hit = x <= t <= upto and not (t == upto == x and self.trigger == t)
            else:
                hit = upto < t <= x
            if hit:
                self.gap("arm-order")
                self.amb.add(t)

    # ------------------------------------------------------------ session
    def run(self):
        results = []
        k = 0
        for i in range(self.n):
            tr = Trial(i, k)
            self.trial(tr)
            results.append(tr)
            if tr.end_frame is None:
                break
            k = tr.end_frame + 1   # 3.11 rule 10: the next trial starts on the next frame
        return results

    def trial(self, tr):
        tpl = self.tpl
        k = tr.k0
        self.cur = None
        self.tr = tr
        self.pend = []            # events not landed
        e0 = onset(k)
        self.fed_until = onset(k - 1) if k > 0 else e0 - 1
        self.trigger = None
        self.carry = []
        self.late = []
        if tpl.kind == "states":
            # 3.11 rule 1: the first state's entry time is the first frame's
            # onset. GAP (arm order): whether the trial starts before or
            # after its first frame's poll; samples in that poll are flagged.
            self.enter(tpl.states[0], e0, k, self.carry, "S")
        else:
            self.enter_events(e0, k)
        try:
            while tr.end_frame is None:
                if onset(k) - onset(tr.k0) > self.max_trial_ns:
                    return
                self.entries = 0
                self.frame(k)
                k += 1
        except Loop:
            tr.loop = True

    # ------------------------------------------------------------ frames
    def frame(self, k):
        tr = self.tr
        o = onset(k)
        # Phase P: the poll's inputs in time order (3.11 rule 4).
        while True:
            nxt = self.next_input(o)
            if nxt is None:
                break
            kind, t, payload = nxt
            self.fed_until = t
            self.trigger = t
            if kind == "sample":
                self.hand = payload
                cands = self.drain_sample(t)
            elif kind == "press":
                cands = self.feed_press(t, k, payload)
            else:
                cands = self.feed_release(t, k)
            if self.carry:
                cands = self.carry + cands
                self.carry = []
            if cands:
                self.resolve(cands, k, "P", t)
        self.fed_until = o
        self.trigger = None
        # Phase R: the landing frame's flip record and the deadline
        # (yrsp_update_frame(), before ytl_evaluate(); response.h FRAME LOOP).
        cands = self.response_frame(k) + self.carry
        self.carry = []
        # Phase T: timed exits whose window this frame reaches (3.11 rule 2).
        self.resolve(cands, k, "T", o)
        self.arm_holds(k)
        # Phase E: the evaluate.
        self.evaluate(k)
        # Phase F: hits that ysig_frame() finds (3.11 rule 5), then a second
        # evaluate for the tables they add.
        rounds = 0
        while True:
            cands = self.late
            self.late = []
            if self.cur is not None:
                for name, d in self.cur.dets.items():
                    h = d.frame(o)
                    if h is not None:
                        cands.append(("hit", h, name, self.cur.vid))
                ud = self.cur.until_det
                if ud is not None:
                    shared = [n for n, d in self.cur.dets.items() if d is ud]
                    h = ud.hit if shared and ud.hit is not None and any(c[2] == shared[0] for c in cands) else ud.frame(o)
                    if h is not None:
                        cands.append(("until", h, None, self.cur.vid))
            if not cands:
                break
            rounds += 1
            if rounds > 1:
                # GAP (3.11 rule 5): a hold armed by a state that a frame's
                # hit started, and that ends before the same frame's onset.
                # Read: rule 2, its next state starts on the first frame at
                # or after the hit, this frame; the frame tests it again.
                self.gap("refound-in-frame")
            self.resolve(cands, k, "F", o)
            self.arm_holds(k)
            self.evaluate(k)
        self.end_check(k)

    def arm_holds(self, k):
        """A hold that an arm of this frame starts (its test true at the
        arm) and that ends at or before this frame's onset is an arm-made
        hit: it is resolved before the frame's evaluate (the coordinator's
        item 2, 2026-11-12; F38: the exit wins at its time, so the state's
        steps at or after it never land)."""
        o = onset(k)
        while self.cur is not None:
            v = self.cur
            cands = []
            for name, d in v.dets.items():
                if d.made == k:
                    h = d.frame(o)
                    if h is not None:
                        cands.append(("hit", h, name, v.vid))
            ud = v.until_det
            if ud is not None and ud.made == k:
                h = ud.frame(o)
                if h is not None:
                    cands.append(("until", h, None, v.vid))
            if not cands:
                return
            self.resolve(cands, k, "F", o)

    def next_input(self, o):
        tr = self.tr
        ts = self.samples[self.si][0] if self.si < len(self.samples) else None
        tp = None
        if tr.press is not None and tr.pi < len(tr.press):
            # The participant presses rt after the flip (player.md); the
            # presses exist from the landing frame's evaluate on, so a press
            # at the landing frame's onset is fed in the next frame's poll.
            tp = tr.press[tr.pi][0]
        tr_ = tr.release
        if tr_ is not None and (tp is None or tr_ <= tp):
            tp = None       # a key's release comes before the next press
        if ts is not None and ts <= o and (tp is None or ts <= tp) and (tr_ is None or ts <= tr_):
            # GAP: a sample and a press at one time; the sample first.
            self.si += 1
            s = self.samples[self.si - 1]
            return ("sample", s[0], (s[1], s[2]))
        if tp is not None and tp <= o:
            tr.pi += 1
            return ("press", tp, tr.pi - 1)
        if tr.release is not None and tr.release <= o:
            t = tr.release
            tr.release = None
            return ("release", t, None)
        return None

    def drain_sample(self, t):
        cands = []
        v = self.cur
        if v is None:
            return cands
        for name, d in v.dets.items():
            h = d.sample(t, self.hand)
            if h is not None:
                cands.append(("hit", h, name, v.vid))
        ud = v.until_det
        if ud is not None:
            shared = [n for n, d in v.dets.items() if d is ud]
            if shared:
                h = next((c[1] for c in cands if c[2] == shared[0]), None)
            else:
                h = ud.sample(t, self.hand)
            if h is not None:
                cands.append(("until", h, None, v.vid))
        return cands

    # ------------------------------------------------------------ response
    def feed_press(self, t, k, i):
        """2.6, response.h THE WINDOW AND RT: each press is EARLY, an
        anticipation (rt < min_rt), on time, or LATE (rt >= duration); an
        on-time press of a choice is a response. The window ends at the
        first response, or with persist after max_responses responses (0:
        never), or at that response's release with wait_for_key_release.
        The result's response is the earliest response."""
        tr, r = self.tr, self.tpl.response
        if tr.resp_end is not None or tr.resp_pending is not None or getattr(tr, "await_release", False):
            return []           # AFTER_END: logged, never a response
        _, name, rt, hold = tr.press[i]
        if rt < r.min_rt or (r.duration > 0 and rt >= r.duration):
            return []           # an anticipation or a late press
        if name not in [c[0] for c in r.choices]:
            return []
        if tr.resp_valid is None:
            tr.resp_valid = (name, rt)
        tr.n_valid += 1
        ends = (not r.persist) or (r.max_responses > 0 and tr.n_valid >= r.max_responses)
        if not ends:
            return []
        if r.wait_release:
            # response.h: with wait_for_key_release the window ends at that
            # response's release, or max_hold_ns (2 s) after the press.
            tr.release = t + min(hold, 2 * NS)
            tr.await_release = True
            return []
        return self.end_by(t, k)

    def feed_release(self, t, k):
        tr = self.tr
        if not getattr(tr, "await_release", False) or tr.resp_end is not None:
            return []
        tr.await_release = False
        # GAP (2.6, F34 rule 2, 3.11 rule 11): the time of a response that
        # ends at its release. Read: the window's end (the release), as a
        # persistent window's exit takes its deadline's time.
        self.gap("release-time")
        return self.end_by(t, k)

    def end_by(self, t, k):
        tr = self.tr
        # 3.11 rule 11: the player waits for the landing frame's flip record
        # before a response counts (yrsp_set_wait_final()).
        if k >= tr.L + RECORD_DELAY:
            tr.resp_end = (t, "press")
            tr.resp_seen = k
            return [("resp", t, None, None)]
        tr.resp_pending = t
        return []

    def response_frame(self, k):
        tr, r = self.tr, self.tpl.response
        if r is None or tr.L is None or tr.resp_end is not None:
            return []
        if k < tr.L + RECORD_DELAY:
            return []
        if tr.resp_pending is not None:
            t = tr.resp_pending
            tr.resp_pending = None
            tr.resp_end = (t, "press")
            tr.resp_seen = k
            if hit_frame(t) < k:
                self.gap("response-final-late")
            return [("resp", t, None, None)]
        if r.duration > 0:
            d = onset(tr.L) + r.duration
            if getattr(tr, "await_release", False):
                # GAP: a response that waits for its release when the
                # deadline comes. Read: the response is in; the release ends
                # the window (jsPsych).
                if onset(k) >= d:
                    self.gap("release-after-deadline")
                return []
            if onset(k) >= d:
                tr.resp_end = (d, "deadline")
                tr.resp_seen = k
                return [("resp", d, None, None)]
        return []

    def window_open(self):
        """F34 rule 3 and F35: the window is open when its `from` was shown,
        or a table that the trial ran shows it and nothing removed that
        step, and the response has not ended."""
        tr, r = self.tr, self.tpl.response
        if r is None or tr.resp_end is not None:
            return False
        if tr.L is not None:
            return True
        return any(ev.kind == "on" and ev.name == r.frm and not ev.removed and not ev.landed
                   for ev in self.pend)

    # ------------------------------------------------------------ evaluate
    def evaluate(self, k):
        tr = self.tr
        landed = []
        for ev in self.pend:
            if ev.removed or ev.landed:
                continue
            if max(lead_frame(ev.t), ev.built) <= k:
                landed.append(ev)
        landed.sort(key=lambda e: (e.t, e.seq))
        for ev in landed:
            ev.landed = True
            ev.frame = k
            if ev.kind == "flag":
                tr.flag_events.append((ev.t, ev.seq, ev.name, ev.val))
            else:
                kind = {"on": "onset", "off": "offset"}.get(ev.kind, ev.kind)
                tr.lands.append((kind, ev.name, k))
            r = self.tpl.response
            if ev.kind == "on" and r is not None and ev.name == r.frm and tr.L is None:
                tr.L = k
                rows = self.script.get(tr.index)
                if rows:
                    # player.md: several rows of one trial are several
                    # presses in time order, each rt from the same onset.
                    tr.press = [(onset(k) + ns_of(rt_s), name, ns_of(rt_s), ns_of(hold_s))
                                for (name, rt_s, hold_s) in rows]
        self.pend = [e for e in self.pend if not (e.landed or e.removed)]

    # ------------------------------------------------------------ states
    def visit_timed(self, v):
        """The visit's timed candidates: after_s from the entry frame's onset
        (D20 rule 2), an until's timeout from the wait's entry frame, the
        steps' end of a part (3.11 rule 7)."""
        # F40 (the user's decision, 2026-11-12; closes the GAP
        # zero-time-cycle): a timed exit (after_s, and an until's timeout_s)
        # is never taken on the state's entry frame: it lands on the later
        # of its lead-rule frame and the frame after the entry frame. Only
        # instant exits leave a state within its entry frame. The fourth
        # field is the first frame that may take it. GAP (F40, "the state's
        # entry frame" for an until's timeout): the wait's entry frame, as
        # the wait is a state of its own (3.11 rule 9).
        out = []
        st = v.state
        if st is not None:
            for e in st.exits:
                if e.kind == "after":
                    out.append(("exit", onset(v.entry_f) + e.after, e, v.vid, v.entry_f + 1))
        if v.waiting:
            to = self.parts_untils(v)[1][v.part][1]
            if to is not None:
                out.append(("timeout", onset(v.wait_f) + to, None, v.vid, v.wait_f + 1))
        elif v.part_end is not None:
            out.append(("partend", v.part_end, None, v.vid, 0))
        return out

    def parts_untils(self, v):
        if v.state is None:
            return self.tpl.parts, self.tpl.untils
        return v.state.parts, v.state.untils

    def order_of(self, c, v):
        """Rule 4's list order. Returns (order, exit) or None if the
        candidate does not apply to the visit."""
        kind, t, x, vid = c
        st = v.state
        n_list = len(st.exits) if st is not None else 0
        if kind == "resp":
            if st is None:
                return None
            best = None
            for e in st.exits:
                if e.kind == "on" and e.on == "response":
                    best = (e.code, e)
                    break
            return best
        if vid != v.vid:
            return None
        if kind == "hit":
            for e in st.exits:
                if e.kind == "on" and e.on == x:
                    return (e.code, e)
            return None
        if kind == "exit":
            return (x.code, x)
        if kind == "until":
            # GAP (F28): an until's `on` follows the state's exits in rule
            # 4's order, then its timeout.
            return (n_list + 0.25, None)
        if kind == "timeout":
            return (n_list + 0.5, None)
        if kind == "partend":
            # GAP (tie-steps-end): a part's end at the time of an exit: the
            # exit first, so it removes the part's events at that time
            # (F38); but the last part's end of a state with no authored
            # exits comes before the added response exit.
            parts, _ = self.parts_untils(v)
            if st is not None and not st.authored and v.part == len(parts) - 1:
                return (-1, None)
            return (n_list + 0.75, None)
        return None

    def resolve(self, cands, k, phase, now):
        """D20 rule 4 with 3.11 rule 4: the earliest pending time wins, then
        the list order. In the poll (phase P) a timed candidate is pending
        once a drained hit is at or after its time; at the frame (phase T
        and F) every timed candidate this frame's window reaches is."""
        tr = self.tr
        cands = list(cands)
        while self.cur is not None:
            v = self.cur
            pool = []
            for c in cands:
                oe = self.order_of(c, v)
                if oe is not None:
                    pool.append((c[1], oe[0], c, oe[1]))
            if phase == "P":
                if not cands:
                    return
                # A drained hit or response that this state does not take
                # still makes the timed candidates at or before it pending:
                # a steps' end at its time may lead to a state that takes it.
                limit = max(c[1] for c in cands)
                # A response's deadline is a time known in advance: like an
                # after_s time it is pending once a drained hit is at or
                # after it (3.11 rule 4: the earliest pending time wins).
                tr, r = self.tr, self.tpl.response
                if (r is not None and r.duration > 0 and tr.L is not None and tr.resp_end is None
                        and k >= tr.L + RECORD_DELAY and not getattr(tr, "await_release", False)
                        and onset(tr.L) + r.duration <= limit):
                    d = onset(tr.L) + r.duration
                    tr.resp_end = (d, "deadline")
                    tr.resp_seen = k
                    c = ("resp", d, None, None)
                    cands.append(c)
                    oe = self.order_of(c, v)
                    if oe is not None:
                        pool.append((d, oe[0], c, oe[1]))
            else:
                limit = onset(k) + LEAD
            for c5 in self.visit_timed(v):
                c = c5[:4]
                if c[1] <= limit and k >= c5[4]:
                    oe = self.order_of(c, v)
                    if oe is not None:
                        pool.append((c[1], oe[0], c, oe[1]))
            if not pool:
                return
            pool.sort(key=lambda p: (p[0], p[1]))
            t, _, c, ex = pool[0]
            if len(pool) > 1 and pool[1][0] == t and "partend" in (c[0], pool[1][2][0]):
                # GAP (D20 rule 4): a steps' end and an exit at one time; the
                # steps' end is in no list. Read: a state with no authored
                # exits goes on when its steps end (F34 rule 1) before the
                # added response exit; an authored exit comes before a part's
                # end (order_of).
                self.gap("tie-steps-end")
            # A candidate that this state does not take and that is earlier
            # than the one taken happened in this state: it is gone (D20
            # rule 3: a hit from before the next state's entry does not
            # count).
            keep = []
            for x in cands:
                if x is c:
                    continue
                if self.order_of(x, v) is None and x[1] < t:
                    continue
                keep.append(x)
            cands = keep
            self.take(c, ex, t, k, phase, cands)
        # The states have ended: a response that ends now ends a waiting trial.

    def take(self, c, ex, t, k, phase, cands):
        tr = self.tr
        v = self.cur
        kind = c[0]
        if kind == "resp":
            tr.resp_used = True
            if t < v.entry_t:
                # GAP (3.11 rule 11 against D20 rule 3): a response that
                # becomes final in a state entered after the response's time
                # (a record that came late). Reading (the coordinator,
                # 2026-11-12; the player's): the exit is taken at the later
                # of the response's time and the state's entry. A reading,
                # not a decision, so the mark stays.
                self.gap("response-before-entry")
                t = v.entry_t
        if kind in ("partend", "until"):
            self.part_step(v, kind, t, k, phase, cands)
            return
        if kind == "timeout":
            st = v.state
            tr.outcome = "timeout"
            if st is None:
                # F35: the events end there with outcome timeout; a part's
                # change is no exit, so earlier parts' events go on.
                self.events_end(t, k)
                return
            # 3.11 rule 9: the timeout is an exit to end with outcome
            # "timeout"; F28: "its timeout has the code after the state's
            # own and shared exits (and the response's)": the count of those
            # exits, the next index; the until's `on` has no code, since it
            # ends no visit. Was a GAP; decided for this reading (the
            # coordinator, 2026-11-12: the player changes).
            if v.wait_f != v.entry_f and lead_frame(t) <= v.wait_f:
                self.gap("F40-until-entry")   # F40 moved it (visit_timed)
            self.leave(v, t, len(st.exits), "exit", k)
            self.finish_states(t, k)
            return
        if kind in ("hit", "resp", "exit"):
            self.leave(v, t, ex.code, "exit", k)
            if ex.outcome is not None:
                tr.outcome = ex.outcome
            elif kind == "resp" and tr.resp_end[1] == "deadline" and tr.resp_valid is None:
                # F34 rule 2: on the deadline the exit's outcome is "timeout"
                # unless the exit gives one. GAP: "the deadline" read as a
                # deadline with no response; a persistent window that ends at
                # its deadline after a press sets none.
                tr.outcome = "timeout"
            self.go(ex.to, t, k, phase, cands)

    def part_step(self, v, kind, t, k, phase, cands):
        parts, untils = self.parts_untils(v)
        if kind == "partend":
            if v.part < len(untils):
                # 3.11 rule 9: the wait arms its control when the steps
                # before it end (D35 (h)); its timeout counts from the wait's
                # entry frame.
                self.remove_after(v, t, strict=True)
                v.part_end = None
                v.waiting = True
                v.wait_t = t
                v.wait_f = k
                ctl = untils[v.part][0]
                # An until on a control that the state's exits also name: the
                # exits stay armed (F3), so the until gets a detect of its
                # own and the exits' hold goes on. Was a GAP; decided for
                # this reading (the coordinator, 2026-11-12: the player
                # changes).
                self.flag_window(t)
                v.until_det = Det(self.tpl.regions[ctl], t, self.hand)
                v.until_det.made = k
                if v.until_det.hold is not None and t > onset(k):
                    self.gap("future-arm-hold")
                if v.until_det.hit is not None:
                    if t > onset(k):
                        self.gap("future-arm-hit")
                    # A hit that an arm makes is taken before the frame's
                    # evaluate (F38: the exit wins at its time). Was the GAP
                    # arm-hit-drain; decided (the coordinator, 2026-11-12).
                    cands.append(("until", t, None, v.vid))
                return
            # The last part's end.
            if v.state is None:
                self.events_end(t, k)
                return
            if v.state.authored:
                v.part_end = None      # 3.11 rule 10: leaves only by an exit
                return
            # F34 rule 1: a state with no authored exits goes on when its
            # steps end; code -1 (3.15).
            self.leave(v, t, -1, "steps", k)
            st = v.state
            nxt = self.tpl.states[st.index + 1] if st.index + 1 < len(self.tpl.states) else None
            self.go(nxt.name if nxt else "end", t, k, phase, cands)
            return
        # The until's hit: the next part starts at the hit's time on the
        # first frame at or after it (D16; 3.11 rule 2).
        v.waiting = False
        v.until_det = None
        v.part += 1
        f = max(k, hit_frame(t)) if phase == "P" else k
        v.part_t, v.part_f = t, f
        if v.state is not None and any(st[0] == "at" for st in parts[v.part]):
            # GAP (3.11 rules 6 and 9): `at_s` after an `until` in a state.
            # Rule 6 counts from the state's entry; rule 9 makes the steps
            # after the until "the next state". Read: rule 9, from the
            # until's hit.
            self.gap("at-after-until")
        evs, end = self.build(self.tr, parts[v.part], t, f, v.vid)
        v.events.extend(evs)
        self.pend.extend(evs)
        v.part_end = end

    def remove_after(self, v, t, strict):
        """3.11 rule 8 and F38: taking an exit removes the visit's events at
        or after the exit's time; events before it happen (a flag among them
        holds at once). GAP: at a steps' end (code -1) or a part's end an
        event at the end's time stays (measured: probe 1's mark at the
        steps' end lands), so only later ones go."""
        tr = self.tr
        for ev in v.events:
            if ev.landed or ev.removed:
                continue
            if ev.t > t or (not strict and ev.t >= t):
                ev.removed = True
            elif ev.kind == "flag":
                tr.flag_events.append((ev.t, ev.seq, ev.name, ev.val))
                ev.landed = True   # happened; it lands in this frame's evaluate
                ev.frame = None

    def leave(self, v, t, code, how, k):
        self.remove_after(v, t, strict=(how == "steps"))
        if v.state is not None:
            self.tr.rows.append((v.state.name, v.entry_f, t, code))

    def go(self, to, t, k, phase, cands):
        if to == "end":
            self.finish_states(t, k)
            return
        self.enter(self.tpl.by_name[to], t, k, cands, phase)

    def finish_states(self, t, k):
        self.cur = None
        if self.tr.done is None:
            self.tr.done = (t, k)

    def enter(self, st, t, k, cands, phase="T"):
        tr = self.tr
        # 3.11 rule 2: a hit's next state starts on the first frame at or
        # after the hit's time; a timed exit's on the frame its window
        # reaches; every exit taken in a frame is in that frame here.
        f = k
        self.entries = getattr(self, "entries", 0) + 1
        if self.entries > 4096:
            # A guard, not a rule: F40 (below, visit_timed) makes every cycle
            # that the loader accepts advance. Was the GAP zero-time-cycle.
            self.gap("model-loop")
            raise Loop()
        n = tr.visits.get(st.name, 0) + 1
        tr.visits[st.name] = n
        if st.max_visits is not None and n > st.max_visits:
            # D20 rule 12, F20: a visit of no time, code -2, then over's to
            # and outcome on the same frame.
            tr.rows.append((st.name, f, t, -2))
            if st.over[1] is not None:
                tr.outcome = st.over[1]
            self.go(st.over[0], t, k, phase, cands)
            return
        # D20 rule 4, F21: instant exits are tested when the state starts,
        # in list order, before its steps; the first true is taken.
        for e in st.exits:
            if e.kind != "instant":
                continue
            ok = True
            if e.cond is not None:
                ok = self.flag_at(tr, e.cond[0]) != e.cond[1]
            if ok:
                tr.rows.append((st.name, f, t, e.code))
                if e.outcome is not None:
                    tr.outcome = e.outcome
                self.go(e.to, t, k, phase, cands)
                return
        self.vid += 1
        v = Visit(st, t, f, self.vid)
        self.cur = v
        # 3.11 rule 3: the state arms the controls its exits name at its
        # entry time.
        names = [e.on for e in st.exits if e.kind == "on" and e.on != "response"]
        if names:
            self.flag_window(t)
        for nm in names:
            if nm in v.dets:
                continue
            d = Det(self.tpl.regions[nm], t, self.hand)
            d.made = k
            if d.hold is not None and t > onset(k):
                # GAP (future-arm-hold): a hold whose test is true at an arm
                # after the frame's onset (a timed exit that the lead rule
                # takes early). Read: sig.md, the hold counts from the arm.
                self.gap("future-arm-hold")
            v.dets[nm] = d
            if d.hit is not None:
                if t > onset(k):
                    self.gap("future-arm-hit")
                # A hit that the arm itself makes is taken before the frame's
                # evaluate, so F38 removes the state's steps at its time. Was
                # the GAP arm-hit-drain; decided (the coordinator,
                # 2026-11-12: the player resolves it before the evaluate).
                cands.append(("hit", d.hit, nm, v.vid))
        evs, end = self.build(tr, st.parts[0], t, f, v.vid)
        v.events.extend(evs)
        self.pend.extend(evs)
        v.part_end = end

    def enter_events(self, t, k):
        """An events template (2.5; 3.11 rule 9 for until; F27: no state
        rows; F35)."""
        self.vid += 1
        v = Visit(None, t, k, self.vid)
        self.cur = v
        evs, end = self.build(self.tr, self.tpl.parts[0], t, k, v.vid)
        v.events.extend(evs)
        self.pend.extend(evs)
        v.part_end = end

    def events_end(self, t, k):
        # F35: a part's change is no exit; the events' end.
        self.cur = None
        self.tr.ev_done = (t, k)
        self.tr.done = (t, k)

    # ------------------------------------------------------------ the end
    def end_check(self, k):
        tr = self.tr
        if tr.done is None or tr.end_frame is not None:
            return
        tpl = self.tpl
        r = tpl.response
        if tpl.kind == "states":
            # 3.11 rule 10 with F34 rule 3: a trial whose states reach end
            # while the response's window is open waits for its end, then
            # ends. The end lands on the frame where both are over.
            if self.window_open():
                return
            tr.end_frame = k
            return
        # Events template (2.5): `after` starts at max(end, events end): end
        # is the response's end (the base time of the frame that took it,
        # ytl_base_time() at its onset), or 0 with no response.
        if self.window_open():
            return
        if getattr(tr, "after_built", None) is None:
            start = tr.done[0]
            if r is not None and tr.resp_end is not None and tr.resp_seen is not None:
                start = max(start, onset(tr.resp_seen))
            evs, end = self.build(tr, tpl.after, start, k, -1)
            self.pend.extend(evs)
            tr.after_built = (start, end, k)
            # The end MARK at the after's end.
            tr.after_end = end
            self.evaluate(k)
        start, end, kb = tr.after_built
        if max(lead_frame(end), kb) <= k:
            tr.end_frame = k

    # ------------------------------------------------------------ result
    def outcome_of(self, tr):
        """D36 (d) as amended; 3.11 "Outcome": the latest set, else version
        1's rule from the response, else "done"."""
        if tr.outcome is not None:
            return tr.outcome
        r = self.tpl.response
        if r is None:
            # 3.11 "Outcome": else "done". GAP: an events template with no
            # response and no until timeout is no D36 template (F16), and
            # 2.5's `outcome` default (empty) is ytr_update()'s, not the
            # column's; the column takes "done" there too.
            return "done"
        if tr.resp_valid is None:
            return "timeout"
        if r.key_answer is not None:
            return "correct" if tr.resp_valid[0] == r.key_answer else "wrong"
        return "responded"


def k_of_onset(o):
    return (o - FRAME0) // PERIOD


def simulate(defn, script=None, samples=None):
    """Returns (trials, gaps). trials: list of dicts with the expected rows."""
    run = Run(defn, script, samples)
    trials = run.run()
    out = []
    for tr in trials:
        d = {
            "trial_index": tr.index,
            "first_frame": tr.k0,
            "hang": tr.end_frame is None,
            "loop": getattr(tr, "loop", False),
            "gaps": sorted(tr.gaps),
            "end_frame": tr.end_frame,
            "time_elapsed_s": fmt(onset(tr.end_frame) - onset(0)) if tr.end_frame is not None else None,
            "states": [{"text": n, "frame": f, "t_s": fmt(onset(f)), "end_s": fmt(e), "code": c}
                       for (n, f, e, c) in tr.rows],
            "lands": [{"kind": kd, "name": nm, "frame": f, "t_s": fmt(onset(f))} for (kd, nm, f) in tr.lands],
            "outcome": run.outcome_of(tr),
            "response": tr.resp_valid[0] if tr.resp_valid else "",
            "rt_s": fmt(tr.resp_valid[1]) if tr.resp_valid else "",
            "flags": {f: (1 if run.flag_at(tr, f) else 0) for f in run.tpl.flags},
        }
        out.append(d)
    return out, sorted(run.gaps)


def read_script(path):
    """player.md: script:FILE, `trial_index,response,rt_s,hold_s`."""
    out = {}
    with open(path, newline="") as f:
        lines = f.read().splitlines()
    for ln in lines[1:]:
        if not ln.strip():
            continue
        w = ln.split(",")
        if w[1] == "":
            continue
        out.setdefault(int(w[0]), []).append((w[1], float(w[2]), float(w[3]) if len(w) > 3 and w[3] else 0.0))
    return out


def read_samples(path):
    """player.md: `t_s,role,x,y`, t_s from the first frame's onset."""
    out = []
    with open(path, newline="") as f:
        lines = f.read().splitlines()
    for ln in lines[1:]:
        if not ln.strip():
            continue
        w = ln.split(",")
        out.append((FRAME0 + ns_of(w[0]), float(w[2]), float(w[3])))
    return out


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("definition")
    ap.add_argument("--participant")
    ap.add_argument("--samples")
    a = ap.parse_args(argv)
    with open(a.definition) as f:
        defn = json.load(f)
    ref = predict_refusal(defn)
    if ref:
        print(json.dumps({"refused": ref}))
        return 0
    script = read_script(a.participant) if a.participant else None
    samples = read_samples(a.samples) if a.samples else None
    trials, gaps = simulate(defn, script, samples)
    print(json.dumps({"trials": trials, "gaps": gaps}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
