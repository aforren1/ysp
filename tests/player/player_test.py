#!/usr/bin/env python3
"""System tests of the player: definitions of valid but unusual shapes, run
through ysp_player --sim. Usage: player_test.py --bin BUILD_DIR [--out DIR]

Every case builds a pack with ypak and asserts on trials.csv and events.csv.
"""
import argparse
import csv
import itertools
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
fails = 0
cases = 0
# Bounds on what one case holds in memory: a run gone wrong (a mutant, a
# loop) can write gigabytes, so data files are streamed and output is cut.
MAX_ROWS = 20000
MAX_OUTPUT = 65536


def check(cond, msg):
    global fails
    if not cond:
        fails += 1
        print("  FAIL: %s" % msg)


def base(trials, timeline, stimuli=None, devices=None, methods=None, session=None, display=None):
    d = {
        "format": "ysp-experiment", "version": 1, "name": "t",
        "trials": trials, "timeline": timeline,
        "devices": devices if devices is not None else {"r": {"direction": "in", "family": ["keyboard"], "required": True}},
        "stimuli": stimuli if stimuli is not None else {"s": {"kind": "shape", "w": 10, "h": 10}, "t": {"kind": "shape", "w": 4, "h": 4, "layer": 1}},
    }
    if methods:
        d["methods"] = methods
    if session:
        d["session"] = session
    if display:
        d["display"] = display
    return d


def tmpl(events=None, after=None, key_answer=None, extra=None, response=True):
    t = {"events": events if events is not None else [{"on": "s"}]}
    if response:
        t["response"] = {"role": "r", "choices": ["left", "right"], "from": "s", "duration_s": 1}
        if key_answer:
            t["response"]["key_answer"] = key_answer
    if after is not None:
        t["after"] = after
    if extra:
        t.update(extra)
    return t


class Run:
    SOURCE_EXTRA = {}

    def __init__(self, ctx, name, definition, tables=None, args=None, pack_files=None, copy_in=None):
        self.ctx = ctx
        self.dir = os.path.join(ctx.out, name)
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(self.dir)
        for f in (copy_in or []):
            shutil.copy(f, self.dir)
        with open(os.path.join(self.dir, "experiment.json"), "w", newline="\n") as f:
            json.dump(definition, f)
        res = []
        for t in (tables or []):
            res.append({"name": t + ".pstb", "kind": "table", "source": t + ".csv"})
            with open(os.path.join(self.dir, t + ".csv"), "w", newline="\n") as f:
                f.write(tables[t])
        for r in (pack_files or []):
            res.append(r)
        with open(os.path.join(self.dir, "pack.json"), "w", newline="\n") as f:
            json.dump({"format": "ysp-pack-source", "version": 1,
                       "experiment": {"entry": "experiment/main.json", "source": "experiment.json", "media_type": "application/vnd.ysp.experiment"},
                       "resources": res, **Run.SOURCE_EXTRA}, f)
        self.pack = os.path.join(self.dir, "p.ysppak")
        self.data = os.path.join(self.dir, "data")
        self._rows = {}
        rc, out = self._run("build", [ctx.ypak, "build", "pack.json", "-o", self.pack])
        if rc != 0:
            self.rc = -1
            self.err = out
            return
        cmd = [ctx.player, self.pack, "--out", self.data, "--sim", "--seed", "99", "--field", "participant=3"] + (args or [])
        self.rc, self.err = self._run("player", cmd)

    def _run(self, tag, cmd):
        # output goes to a file and only its head is kept: a run that prints
        # without end must not fill this process's memory
        log = os.path.join(self.dir, tag + ".log")
        with open(log, "wb") as f:
            try:
                rc = subprocess.run(cmd, cwd=self.dir, stdout=f, stderr=subprocess.STDOUT, timeout=240).returncode
            except subprocess.TimeoutExpired:
                rc = -9   # a run that never ends fails its case, not the suite
        with open(log, "rb") as f:
            out = f.read(MAX_OUTPUT).decode("utf-8", "replace")
        if rc == -9:
            out += "\nthe run did not end in 240 s"
        return rc, out

    @property
    def trials(self):
        return self._cached("trials.csv")

    @property
    def events(self):
        return self._cached("events.csv")

    def _cached(self, name):
        if name not in self._rows:
            self._rows[name] = self.read(name)
        return self._rows[name]

    def rows(self, name):
        """The rows of a data file, streamed."""
        p = os.path.join(self.data, name)
        if not os.path.exists(p):
            return
        with open(p, newline="") as f:
            for row in csv.DictReader(f):
                yield row

    def count(self, name):
        return sum(1 for _ in self.rows(name))

    def read(self, name):
        """The rows of a small data file as a list; a file past MAX_ROWS fails
        the case (stream it with rows())."""
        out = list(itertools.islice(self.rows(name), MAX_ROWS + 1))
        if len(out) > MAX_ROWS:
            check(False, "%s/%s has more than %d rows: stream it" % (self.dir, name, MAX_ROWS))
            del out[MAX_ROWS:]
        return out


def same_file(a, b, chunk=1 << 20):
    with open(a, "rb") as fa, open(b, "rb") as fb:
        while True:
            x, y = fa.read(chunk), fb.read(chunk)
            if x != y:
                return False
            if not x:
                return True


def t_nested(ctx):
    d = base({"t": tmpl(key_answer="$target", extra={"columns": {"cond_copy": "$cond"}})},
             [{"name": "outer", "timeline_variables": "outer.pstb", "repetitions": 2, "data": {"block_name": "main"},
               "timeline": [{"name": "inner", "trial": "t", "timeline_variables": "inner.pstb", "rules": ["reps 1"]}]}])
    r = Run(ctx, "nested", d, tables={"outer": "cond,bias\nA,1\nB,2\n", "inner": "target,word\nleft,hi\nright,yo\n"}, args=["--rt", "0.2"])
    check(r.rc == 0, "nested: %s" % r.err)
    check(len(r.trials) == 2 * 2 * 2, "nested: 8 trials (2 reps x 2 rows x 2 inner), got %d" % len(r.trials))
    check({row["cond"] for row in r.trials} == {"A", "B"}, "nested: outer's table column inherited")
    check(all(row["block_name"] == "main" for row in r.trials), "nested: data inherited")
    check(all(row["cond_copy"] == row["cond"] for row in r.trials), "nested: a template column reads the outer row")
    check(all(row["node"] == "outer/inner" for row in r.trials), "nested: node path is outer/inner")
    conds = [row["cond"] for row in r.trials[:4]]
    check(conds == ["A", "A", "B", "B"], "nested: row A's children run before row B: %s" % conds)


def t_if_unless(ctx):
    d = base({"t": tmpl(events=[{"on": "s"}, {"if": "$first_in_block", "steps": [{"on": "t"}]},
                                {"unless": "$first_in_block", "steps": [{"off": "t"}]}], key_answer="$a",
                        after=[{"if": "$correct", "steps": [{"mark": "was_correct"}]}, {"unless": "$correct", "steps": [{"mark": "was_wrong"}]}])},
             [{"name": "kept", "if": "$do_it", "trial": "t", "timeline_variables": "x.pstb", "rules": ["reps 2"]},
              {"name": "skipped", "unless": "$do_it", "trial": "t", "timeline_variables": "x.pstb"}],
             session={"fields": {"participant": {"type": "integer", "required": True}, "do_it": {"type": "integer", "required": True}}})
    r = Run(ctx, "if_unless", d, tables={"x": "a\nleft\nright\n"}, args=["--field", "do_it=1", "--rt", "0.2"])
    check(r.rc == 0, "if: %s" % r.err)
    check(len(r.trials) == 4, "if: 4 kept trials, got %d" % len(r.trials))
    check(all(row["node"] == "kept" for row in r.trials), "if: node 'skipped' did not run")
    t_ons = [e for e in r.events if e["kind"] == "onset" and e["stimulus"] == "t"]
    check(len(t_ons) == 1, "if on a step: stimulus t shows only on the first trial (%d)" % len(t_ons))
    marks = [e["text"] for e in r.events if e["kind"] == "mark"]
    check(marks.count("was_correct") == 4, "if $correct after: 4 correct, marks %s" % marks)
    check(marks.count("was_wrong") == 0, "unless $correct after: none wrong")
    r2 = Run(ctx, "if_unless2", d, tables={"x": "a\nleft\nright\n"}, args=["--field", "do_it=0", "--rt", "0.2"])
    check(r2.rc == 0 and all(row["node"] == "skipped" for row in r2.trials) and len(r2.trials) == 2, "if on a node, the other way: %s %d" % (r2.err, len(r2.trials)))
    r3 = Run(ctx, "if_bad", d, tables={"x": "a\nleft\nright\n"}, args=["--field", "do_it=7"])
    check(r3.rc != 0 and "0 or 1" in r3.err, "an integer field not 0 or 1 in an if is refused at the session's start: %s" % r3.err)


def t_empty_and_zero(ctx):
    d = base({"t": tmpl()}, [{"name": "e", "trial": "t", "timeline_variables": "x.pstb"}, {"name": "f", "trial": "t", "timeline_variables": "y.pstb"}])
    r = Run(ctx, "empty", d, tables={"x": "a\n", "y": "a\nleft\n"}, args=["--rt", "0.2"])
    # The right outcome for a leaf on a table with a header and no rows: the
    # dry open refuses the definition at load, naming the node; a design with
    # no trials is a mistake to report, not a node to skip.
    check(r.rc != 0 and "node 'e'" in r.err and "no rows" in r.err, "an empty table on a leaf is refused at load: %s" % r.err)
    # A container over an empty table runs its children zero times.
    dc = base({"t": tmpl()}, [{"name": "e", "timeline_variables": "x.pstb", "timeline": [{"name": "c", "trial": "t", "rules": ["reps 2"]}]},
                              {"name": "f", "trial": "t", "timeline_variables": "y.pstb"}])
    rc = Run(ctx, "empty_container", dc, tables={"x": "a\n", "y": "a\nleft\n"}, args=["--rt", "0.2"])
    check(rc.rc == 0 and len(rc.trials) == 1 and rc.trials[0]["node"] == "f", "a container over an empty table runs nothing: %s" % rc.err)
    d2 = base({"t": tmpl()}, [{"name": "c", "timeline": []}, {"name": "z", "trial": "t", "timeline_variables": "y.pstb"}])
    r2 = Run(ctx, "zero_rep", d2, tables={"y": "a\nleft\n"}, args=["--rt", "0.2"])
    check(r2.rc == 0, "a container with no children (a zero-trial node): %s" % r2.err)
    if r2.rc == 0:
        check(all(row["node"] == "z" for row in r2.trials) and len(r2.trials) == 1, "a container with no children runs nothing")
    d3 = base({"t": {"events": [{"on": "s"}, {"wait_s": 0.05}, {"off": "s"}]}},
              [{"name": "nr", "trial": "t", "rules": ["reps 3"]}])
    r3 = Run(ctx, "no_response", d3)
    check(r3.rc == 0 and len(r3.trials) == 3 and all(row["outcome"] == "" for row in r3.trials), "no response: outcome empty, %s" % r3.err)


def t_loop(ctx):
    d = base({"t": tmpl(key_answer="$a")},
             [{"name": "pr", "trial": "t", "timeline_variables": "x.pstb", "loop": {"until_accuracy": 1.0, "max": 3}}])
    r = Run(ctx, "loop_wrong", d, tables={"x": "a\nleft\nright\n"}, args=["--rt", "0.2", "--participant", "p=0.0"])
    check(r.rc == 0 and len(r.trials) == 6, "loop with max: 3 iterations of 2 trials when never correct, got %d (%s)" % (len(r.trials), r.err))
    seeds = {row["trial_seed"] for row in r.trials[:2]} | {row["trial_seed"] for row in r.trials[2:4]}
    check(len(seeds) == 4, "loop: each iteration has its own node seed, so its own trial seeds")
    r2 = Run(ctx, "loop_right", d, tables={"x": "a\nleft\nright\n"}, args=["--rt", "0.2"])
    check(r2.rc == 0 and len(r2.trials) == 2, "loop: one iteration when correct, got %d (%s)" % (len(r2.trials), r2.err))
    d3 = base({"t": tmpl(key_answer="$a")},
              [{"name": "c", "loop": {"until_accuracy": 1.0, "max": 2}, "timeline":
                [{"name": "pr", "trial": "t", "timeline_variables": "x.pstb"}]}])
    r3 = Run(ctx, "loop_container", d3, tables={"x": "a\nleft\nright\n"}, args=["--rt", "0.2", "--participant", "p=0.0"])
    check(r3.rc == 0 and len(r3.trials) == 4, "loop on a container: 2 iterations, got %d (%s)" % (len(r3.trials), r3.err))


def t_requeue(ctx):
    d = base({"t": tmpl(key_answer="$a")},
             [{"name": "r", "trial": "t", "timeline_variables": "x.pstb", "requeue": "no_response", "rules": ["order sequential", "reps 1"]}])
    with open(os.path.join(ctx.out, "req.csv"), "w", newline="\n") as f:
        f.write("trial_index,response,rt_s,hold_s\n1,,0,0\n0,left,0.2,0.05\n2,left,0.2,0.05\n3,right,0.2,0.05\n")
    r = Run(ctx, "requeue", d, tables={"x": "a\nleft\nright\nleft\n"}, args=["--participant", "script:" + os.path.join(ctx.out, "req.csv")])
    check(r.rc == 0, "requeue: %s" % r.err)
    check(len(r.trials) == 4, "requeue: 3 + 1 trials, got %d" % len(r.trials))
    if len(r.trials) == 4:
        check(r.trials[1]["outcome"] == "timeout", "the timeout's row stays")
        check(r.trials[3]["requeued"] == "1" and r.trials[3]["outcome"] != "timeout", "the requeued copy reruns: %s" % r.trials[3])


def t_track_ends(ctx):
    methods = {"st": {"type": "stair", "start": 0.5, "n_up": 1, "n_down": 1, "step_type": "lin", "steps": [0.1], "stop_trials": 2}}
    d = base({"t": tmpl(events=[{"on": "s"}], key_answer="left", extra={"method_value": 1})},
             [{"name": "tr", "trial": "t", "timeline_variables": "x.pstb", "tracks": ["st"], "track_rate": 0.5, "rules": ["reps 2"]}],
             methods=methods)
    r = Run(ctx, "track_ends", d, tables={"x": "a\nleft\nright\n"}, args=["--rt", "0.2"])
    check(r.rc == 0, "track ends early: %s" % r.err)
    tracks = [row for row in r.trials if row["track"] == "0"]
    conds = [row for row in r.trials if row["track"] == "-1"]
    check(len(tracks) == 2 and len(conds) == 4, "track ended at stop_trials 2, schedule ran its 4: tracks %d, cond %d" % (len(tracks), len(conds)))
    check(len(r.trials) == 6, "6 trials, got %d" % len(r.trials))


def t_lookup_keys(ctx):
    # a lookup in a duration, a number, a color, a text-free stimulus key, a trigger code, a response deadline
    stim = {"s": {"kind": "shape", "w": {"by": "$size", "map": {"big": 100, "small": 10}}, "h": 10,
                  "color": {"by": "$size", "map": {"big": [1, 0, 0], "small": [0, 1, 0]}}},
            "t": {"kind": "shape", "w": 4, "h": 4}}
    d = base({"t": {
        "events": [{"on": "s"}, {"wait_s": {"by": "$size", "map": {"big": 0.1, "small": 0.2}}}, {"trigger": {"by": "$size", "map": {"big": 9, "small": 8}}, "role": "out"},
                   {"set": "s", "props": {"x": {"by": "$size", "map": {"big": 1.5, "small": 2.5}}}}, {"off": "s"}],
        "response": {"role": "r", "choices": ["left"], "from": "s", "duration_s": {"by": "$size", "map": {"big": 0.8, "small": 0.3}}},
        "after": [{"wait_s": {"by": "$responded", "map": {"true": 0.05, "false": 0.15}}}]}},
        [{"name": "l", "trial": "t", "timeline_variables": "x.pstb"}],
        stimuli=stim,
        devices={"r": {"direction": "in", "family": ["keyboard"], "required": True}, "out": {"direction": "out", "family": ["triggerbox"], "required": True}})
    r = Run(ctx, "lookup", d, tables={"x": "size\nbig\nsmall\n"}, args=["--participant", "silent"])
    check(r.rc == 0, "lookups: %s" % r.err)
    if r.rc == 0:
        codes = [int(e["code"]) for e in r.events if e["kind"] == "out"]
        check(sorted(codes) == [8, 9], "lookup as a trigger code: %s" % codes)
        ons = {e["trial_index"]: float(e["t_s"]) for e in r.events if e["kind"] == "onset"}
        sets = {e["trial_index"]: float(e["t_s"]) for e in r.events if e["kind"] == "set"}
        for row in r.trials:
            idx = row["trial_index"]
            want = 0.1 if row["size"] == "big" else 0.2
            check(abs(sets[idx] - ons[idx] - want) < 0.5 / 60.0 + 1e-6, "lookup duration %s: %g" % (row["size"], sets[idx] - ons[idx]))
    # a lookup in a texture key, a bool key
    d2 = base({"t": tmpl(key_answer="$a", extra={"columns": {"v": {"by": "$responded", "map": {"true": "$rt_s", "false": "none"}}}})},
              [{"name": "l", "trial": "t", "timeline_variables": "x.pstb"}], stimuli=None)
    r2 = Run(ctx, "lookup2", d2, tables={"x": "a\nleft\nright\n"}, args=["--rt", "0.25"])
    check(r2.rc == 0 and all(float(row["v"]) == 0.25 for row in r2.trials), "a lookup by a bool, giving a variable: %s" % r2.err)


def t_triggers(ctx):
    devs = {"r": {"direction": "in", "family": ["keyboard"], "required": True},
            "z_out": {"direction": "out", "family": ["triggerbox"], "required": True},
            "a_out": {"direction": "out", "family": ["triggerbox"], "required": True}}
    d = base({"t": {"events": [{"on": "s"}, {"trigger": 1, "role": "z_out"}, {"wait_s": 0.1}, {"trigger": 2, "role": "a_out"}, {"off": "s"}]}},
             [{"name": "l", "trial": "t", "rules": ["reps 2"]}], devices=devs)
    r = Run(ctx, "triggers", d)
    check(r.rc == 0, "triggers: %s" % r.err)
    if r.rc == 0:
        check(all(row["a_out_n"] == "1" and row["z_out_n"] == "1" for row in r.trials), "each role's count per trial")
        out = [(e["role"], e["code"]) for e in r.events if e["kind"] == "out"]
        check(out[:2] == [("z_out", "1"), ("a_out", "2")], "the roles keep their codes: %s" % out[:2])
        with open(os.path.join(r.data, "trials.csv")) as f:
            header = f.readline()
        check(header.index("a_out_n") < header.index("z_out_n"), "output-role columns come in the order of the role names")


def t_after_timing(ctx):
    # after starts at the later of the response's end and the events' end
    d = base({"t": {"events": [{"on": "s"}, {"wait_s": 1}, {"off": "s"}],
                    "response": {"role": "r", "choices": ["left"], "from": "s", "duration_s": 1.5, "key_answer": "left"},
                    "after": [{"on": "t"}, {"wait_s": 0.5}, {"off": "t"}]}},
             [{"name": "l", "trial": "t", "rules": ["reps 2"]}])
    r = Run(ctx, "after_timing", d, args=["--rt", "0.1"])
    check(r.rc == 0, "after: %s" % r.err)
    if r.rc == 0:
        ons = [e for e in r.events if e["stimulus"] == "t" and e["kind"] == "onset"]
        s_on = [e for e in r.events if e["stimulus"] == "s" and e["kind"] == "onset"]
        check(all(abs(float(a["t_s"]) - float(b["t_s"]) - 1.0) < 1.0 / 120 for a, b in zip(ons, s_on)),
              "with an early response, after starts at the end of the events: %s" % [(a["t_s"], b["t_s"]) for a, b in zip(ons, s_on)])
    d2 = base({"t": {"events": [{"on": "s"}], "response": {"role": "r", "choices": ["left"], "from": "s", "duration_s": 0.4, "response_ends_trial": False},
                     "after": [{"mark": "go"}]}},
              [{"name": "l", "trial": "t", "rules": ["reps 2"]}])
    r2 = Run(ctx, "after_window", d2, args=["--rt", "0.1"])
    check(r2.rc == 0, "after, response_ends_trial false: %s" % r2.err)
    if r2.rc == 0:
        marks = [float(e["t_s"]) for e in r2.events if e["text"] == "go"]
        ons = [float(e["t_s"]) for e in r2.events if e["kind"] == "onset" and e["stimulus"] == "s"]
        check(all(abs(m - o - 0.4) < 2.0 / 60 for m, o in zip(marks, ons)), "response_ends_trial false: after at the window's end: %s" % list(zip(marks, ons)))
    # a response with no deadline, and the participant silent, stops at the budget
    d3 = base({"t": {"events": [{"on": "s"}], "response": {"role": "r", "choices": ["left"], "from": "s"}}},
              [{"name": "l", "trial": "t"}])
    r3 = Run(ctx, "no_deadline", d3, args=["--participant", "silent", "--max-seconds", "5"])
    check(r3.rc != 0 and "budget" in r3.err, "no deadline, silent: stops at the budget: %s" % r3.err)


def t_outcomes(ctx):
    # correct=1 goes to ytr_update as 1; a timeout INVALID; an explicit outcome value
    d = base({"t": tmpl(key_answer="$a", extra={"outcome": "$correct"})},
             [{"name": "l", "trial": "t", "timeline_variables": "x.pstb", "rules": ["order sequential"]}])
    with open(os.path.join(ctx.out, "oc.csv"), "w", newline="\n") as f:
        f.write("trial_index,response,rt_s,hold_s\n0,left,0.2,0.05\n1,left,0.2,0.05\n2,,0,0\n")
    r = Run(ctx, "outcome_val", d, tables={"x": "a\nleft\nright\nleft\n"}, args=["--participant", "script:" + os.path.join(ctx.out, "oc.csv")])
    check(r.rc == 0, "outcome value: %s" % r.err)
    if r.rc == 0:
        check([row["outcome"] for row in r.trials] == ["correct", "wrong", "timeout"], "outcome text per $outcome: %s" % [row["outcome"] for row in r.trials])
        check([row["correct"] for row in r.trials] == ["1", "0", "0"], "correct column: %s" % [row["correct"] for row in r.trials])
    # no key_answer: the outcome column says responded
    d2 = base({"t": tmpl()}, [{"name": "l", "trial": "t", "timeline_variables": "x.pstb"}])
    r2 = Run(ctx, "no_key_answer", d2, tables={"x": "a\nleft\n"}, args=["--rt", "0.2"])
    check(r2.rc == 0 and r2.trials[0]["outcome"] == "responded" and r2.trials[0]["correct"] == "", "no key_answer: %s %s" % (r2.err, r2.trials[:1]))


def t_method_update(ctx):
    methods = {"q": {"type": "quest", "scale": "log10", "pf": "gumbel", "stop_trials": 8, "estimate": "mean",
                     "stim": [{"linspace": [-2, 0, 11]}], "param": [{"linspace": [-2, 0, 11]}, {"linspace": [1, 5, 5]}, 0.5, 0.02]}}
    d = base({"t": tmpl(key_answer="$a", extra={"method_value": "$level", "columns": {"mv": "$level", "est": "$estimate"}})},
             [{"name": "l", "trial": "t", "tracks": ["q"], "vars": {"a": {"choice": ["left", "right"]}}, "track_rate": 1}], methods=methods)
    r = Run(ctx, "method", d, args=["--rt", "0.2"])
    check(r.rc == 0, "method: %s" % r.err)
    # a quest with three stimulus axes: $level is one value, so it is refused
    # at load (the fuzz target found its update reading past one value)
    m3 = json.loads(json.dumps(methods))
    m3["q"]["stim"] = [{"linspace": [-2, 0, 3]}] * 3
    d3 = json.loads(json.dumps(d))
    d3["methods"] = m3
    r3 = Run(ctx, "method_3axes", d3, args=["--rt", "0.2"])
    check(r3.rc != 0 and "one stimulus axis" in r3.err and not os.path.exists(r3.data), "a quest with three stimulus axes is refused at load: %s" % r3.err)
    if r.rc == 0:
        check(len(r.trials) == 8, "quest ran to its 8 trials, got %d" % len(r.trials))
        levels = [float(row["level"]) for row in r.trials]
        check(all(1e-2 - 1e-12 <= l <= 1.0 + 1e-12 for l in levels), "$level in stimulus units (10^x over [-2,0]): %s" % levels)
        check(float(r.trials[-1]["estimate"]) > 0, "$estimate in stimulus units")
    # a stimulus value from $level, deg-free
    d2 = base({"t": tmpl(key_answer="$a", extra={"columns": {"shown": "$level"}})},
              [{"name": "l", "trial": "t", "tracks": ["q"], "vars": {"a": {"choice": ["left"]}}, "track_rate": 1}], methods=methods,
              stimuli={"s": {"kind": "shape", "w": "$level", "h": 1, "opacity": "$level"}, "t": {"kind": "shape"}})
    r2 = Run(ctx, "method2", d2, args=["--rt", "0.2"])
    check(r2.rc == 0, "a stimulus value from $level: %s" % r2.err)
    if r2.rc == 0:
        check(all(float(row["shown"]) == float(row["level"]) for row in r2.trials), "column equals level")
    # a timeout is not told to a track: it runs again
    methods2 = {"st": {"type": "stair", "start": 0.5, "n_up": 1, "n_down": 1, "step_type": "lin", "steps": [0.1], "stop_trials": 3}}
    d3 = base({"t": tmpl(key_answer="$a", extra={"method_value": "$level"})},
              [{"name": "l", "trial": "t", "tracks": ["st"], "vars": {"a": {"choice": ["left"]}}, "track_rate": 1}], methods=methods2)
    with open(os.path.join(ctx.out, "tm.csv"), "w", newline="\n") as f:
        f.write("trial_index,response,rt_s,hold_s\n0,left,0.2,0.05\n1,,0,0\n2,left,0.2,0.05\n3,left,0.2,0.05\n")
    r3 = Run(ctx, "track_timeout", d3, args=["--participant", "script:" + os.path.join(ctx.out, "tm.csv")])
    check(r3.rc == 0 and len(r3.trials) == 4 and r3.trials[1]["outcome"] == "timeout", "a timeout is not told to the track: %d trials %s" % (len(r3.trials), r3.err))


def t_seeds(ctx):
    # D8: two runs of the same seed are byte-identical; another seed differs; a skipped node moves nothing else
    d = base({"t": tmpl(key_answer="$a")}, [{"name": "first", "trial": "t", "timeline_variables": "x.pstb", "rules": ["order random", "reps 3"]},
                                            {"name": "second", "trial": "t", "timeline_variables": "x.pstb", "rules": ["order random", "reps 3"]}])
    table = {"x": "a\nleft\nright\nleft\n"}
    r1 = Run(ctx, "seed1", d, tables=table, args=["--rt", "0.2"])
    r2 = Run(ctx, "seed2", d, tables=table, args=["--rt", "0.2"])
    check(r1.rc == 0 and r2.rc == 0, "seeds: %s" % r1.err)
    for name in ("trials.csv", "events.csv", "frames.csv", "session.json"):
        check(same_file(os.path.join(r1.data, name), os.path.join(r2.data, name)), "seeds: %s differs between two runs" % name)
    # D8: skipping or looping a node shifts no other node's draws
    d2 = json.loads(json.dumps(d))
    d2["timeline"].insert(0, {"name": "zero", "if": "$off", "trial": "t", "timeline_variables": "x.pstb"})
    d2["session"] = {"fields": {"participant": {"type": "integer", "required": True}, "off": {"type": "integer", "required": True}}}
    r3 = Run(ctx, "seed_skip", d2, tables=table, args=["--rt", "0.2", "--field", "off=0"])
    r4 = Run(ctx, "seed_noskip", d2, tables=table, args=["--rt", "0.2", "--field", "off=1"])
    check(r3.rc == 0 and r4.rc == 0, "seeds, a skipped node: %s %s" % (r3.err, r4.err))
    for n in ("first", "second"):
        a = [row["condition"] for row in r3.trials if row["node"] == n]
        b = [row["condition"] for row in r4.trials if row["node"] == n]
        check(a == b, "a skipped node moves no other node's draws (%s)" % n)


def t_refused(ctx):
    d = base({"t": tmpl()}, [{"name": "l", "trial": "t", "timeline_variables": "missing.pstb"}])
    r = Run(ctx, "refused_table", d, tables={})
    check(r.rc != 0, "a missing table is refused (rc %d): %s" % (r.rc, r.err))
    d2 = base({"t": tmpl(events=[{"on": "s"}, {"trigger": 1, "role": "r"}])}, [{"name": "l", "trial": "t"}])
    r2 = Run(ctx, "refused_role", d2)
    check(r2.rc != 0 and "role" in r2.err, "a trigger on an input-only role: %s" % r2.err)
    # both set and to on one channel: the real ytl_check_ops() refuses
    d3 = base({"t": {"events": [{"on": "s"}, {"set": "s", "props": {"x": 1}}, {"to": "s", "props": {"x": 2}, "duration_s": 0.1}, {"off": "s"}]}}, [{"name": "l", "trial": "t"}])
    r3 = Run(ctx, "refused_ops", d3, args=["--participant", "silent"])
    check(r3.rc != 0 and "channel" in r3.err.lower(), "a set and a tween on one channel: the real check refuses it: %s" % r3.err)
    # the same across the events and after tables
    d3b = base({"t": {"events": [{"on": "s"}, {"set": "s", "props": {"x": 1}}, {"off": "s"}],
                       "after": [{"to": "s", "props": {"x": 2}, "duration_s": 0.1}]}}, [{"name": "l", "trial": "t"}])
    r3b = Run(ctx, "refused_ops2", d3b, args=["--participant", "silent"])
    check(r3b.rc != 0 and "channel" in r3b.err.lower(), "a set and a tween across events and after: %s" % r3b.err)
    # 'from' never shown, an impossible constraint (a dry open)
    d4 = base({"t": tmpl()}, [{"name": "l", "trial": "t", "timeline_variables": "x.pstb", "rules": ["order constrained", "max_run a=x 1", "reps 2"]}])
    r4 = Run(ctx, "refused_rule", d4, tables={"x": "a\nx\nx\nx\ny\n"})
    check(r4.rc != 0 and "max_run" in r4.err and not os.path.exists(r4.data), "an impossible constraint fails the dry open, before a data set: %s" % r4.err)


def t_sound(ctx):
    # a sound on the null device: the response's RT runs from the audio onset
    import struct
    wav = os.path.join(ctx.out, "tone.wav")
    samples = b"".join(struct.pack("<hh", 0, 0) for _ in range(4800))
    with open(wav, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(samples)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, 48000, 192000, 4, 16) + b"data" + struct.pack("<I", len(samples)) + samples)
    d = base({"t": tmpl(events=[{"on": "tone"}, {"wait_s": 0.5}, {"on": "s"}, {"wait_s": 0.1}, {"off": "s"}, {"off": "tone"}], key_answer="left")},
             [{"name": "l", "trial": "t", "rules": ["reps 2"]}],
             stimuli={"s": {"kind": "shape", "w": 1, "h": 1}, "t": {"kind": "shape"}, "tone": {"kind": "sound", "audio": "tone.wav"}})
    d["trials"]["t"]["response"]["from"] = "tone"
    sd = dict(Run.SOURCE_EXTRA)
    Run.SOURCE_EXTRA["audio"] = {"rate": 48000, "channels": 2}
    r = Run(ctx, "sound", d, pack_files=[{"name": "tone.wav", "kind": "audio", "source": "tone.wav"}], args=["--rt", "0.3"], copy_in=[wav])
    Run.SOURCE_EXTRA.clear()
    Run.SOURCE_EXTRA.update(sd)
    check(r.rc == 0, "a sound stimulus: %s" % r.err)
    if r.rc == 0:
        rts = [float(row["rt_s"]) for row in r.trials]
        check(all(abs(x - 0.3) < 1e-6 for x in rts) and all(row["onset_src"] == "2" for row in r.trials), "RT runs from the audio onset: %s %s" % (rts[:2], r.trials[0]["onset_src"]))


def t_unused_stim(ctx):
    # a stimulus the running template does not touch may name a variable of
    # another leaf only: its keys must not be evaluated here (2.5)
    d = base({"a": tmpl(key_answer="left"), "b": {"events": [{"on": "u"}, {"wait_s": 0.05}, {"off": "u"}]}},
             [{"name": "one", "trial": "a", "rules": ["reps 2"]}, {"name": "two", "trial": "b", "vars": {"z": 5}, "rules": ["reps 2"]}],
             stimuli={"s": {"kind": "shape", "w": 1, "h": 1}, "u": {"kind": "shape", "w": "$z", "h": 1}})
    r = Run(ctx, "unused_stim", d, args=["--rt", "0.2"])
    check(r.rc == 0 and len(r.trials) == 4, "a stimulus's variable exists only for its own template: %s" % r.err)


def t_record_data(ctx):
    # record_data false: no row, and nothing of its response leaks into the next
    d = base({"a": tmpl(key_answer="left"), "b": {"events": [{"on": "s"}, {"wait_s": 0.05}, {"off": "s"}]}},
             [{"name": "one", "trial": "a", "record_data": False, "rules": ["reps 2"]}, {"name": "two", "trial": "b", "rules": ["reps 2"]}])
    r = Run(ctx, "record_data", d, args=["--rt", "0.2"])
    check(r.rc == 0 and len(r.trials) == 2, "record_data false writes no row: %d (%s)" % (len(r.trials), r.err))
    check(all(row["response"] == "" and row["rt_s"] == "" for row in r.trials), "no value of an unrecorded trial reaches the next row: %s" % r.trials[:1])
    check([row["trial_index"] for row in r.trials] == ["2", "3"], "trial_index still counts the unrecorded trials")


def trace_rows(ctx, r):
    with open(os.path.join(r.dir, "trace.csv"), newline="") as f:
        for row in csv.DictReader(f):
            yield row


def t_fix_density(ctx):
    stim = {"s": {"kind": "shape", "w": 1, "h": 1},
            "r": {"kind": "rdk", "aperture": "circle", "w": 200, "h": 200, "density": 0.01, "speed": 10, "dot_size": 2, "color": [1, 1, 1]}}
    d = base({"t": {"events": [{"on": "r"}, {"wait_s": 0.1}, {"off": "r"}]}}, [{"name": "l", "trial": "t", "rules": ["reps 2"]}], stimuli=stim)
    r = Run(ctx, "fix_density", d)
    check(r.rc == 0, "density: %s" % r.err)
    if r.rc == 0:
        check(int(r.trials[0]["r_updates"]) > 1 and r.trials[0]["r_digest"] != "0000000000000000", "density moves dots: %s" % r.trials[0]["r_digest"])
        check(r.trials[0]["r_digest"] != r.trials[1]["r_digest"], "two trials, two fields")


def t_fix_key_reapply(ctx):
    d = base({"t": {"events": [{"on": "s"}, {"to": "s", "props": {"x": 50}, "duration_s": 0.1}, {"wait_s": 0.2}, {"off": "s"}]}},
             [{"name": "l", "trial": "t", "rules": ["reps 2"]}],
             stimuli={"s": {"kind": "shape", "x": 5, "w": 1, "h": 1}})
    r = Run(ctx, "fix_reapply", d, args=["--trace-channels", "trace.csv"])
    check(r.rc == 0, "reapply: %s" % r.err)
    rows = [x for x in trace_rows(ctx, r) if x["prop"] == "x"]
    t1 = [x for x in rows if x["trial_index"] == "1"]
    check(t1 and abs(float(t1[0]["value"]) - 5.0) < 1e-3, "trial 1 starts from the literal 5: %s" % (t1[0] if t1 else None))
    t0 = [x for x in rows if x["trial_index"] == "0"]
    check(t0 and abs(float(t0[-1]["value"]) - 50.0) < 1e-3, "trial 0 reached the tween's end")


def t_fix_key_var_reapply(ctx):
    # a key that holds a variable is read per trial (the channel's start)
    d = base({"t": {"events": [{"on": "s"}, {"to": "s", "props": {"x": 50}, "duration_s": 0.1}, {"wait_s": 0.2}, {"off": "s"}]}},
             [{"name": "l", "trial": "t", "timeline_variables": "x.pstb", "rules": ["order sequential"]}],
             stimuli={"s": {"kind": "shape", "x": "$p", "w": 1, "h": 1}})
    r = Run(ctx, "fix_reapply_var", d, tables={"x": "p\n3\n7\n"}, args=["--trace-channels", "trace.csv"])
    check(r.rc == 0, "reapply var: %s" % r.err)
    rows = [x for x in trace_rows(ctx, r) if x["prop"] == "x"]
    for t, want in (("0", 3.0), ("1", 7.0)):
        t_rows = [x for x in rows if x["trial_index"] == t]
        check(t_rows and abs(float(t_rows[0]["value"]) - want) < 1e-3, "trial %s starts from its own row value %g: %s" % (t, want, t_rows[0] if t_rows else None))


def t_fix_stale_rdk(ctx):
    stim = {"s": {"kind": "shape", "w": 1, "h": 1},
            "r": {"kind": "rdk", "aperture": "circle", "w": 50, "count": 5, "speed": 10, "dot_size": 2, "color": [1, 1, 1]}}
    d = base({"a": {"events": [{"on": "r"}, {"wait_s": 0.1}, {"off": "r"}]}, "b": {"events": [{"on": "s"}, {"wait_s": 0.05}, {"off": "s"}]}},
             [{"name": "one", "trial": "a"}, {"name": "two", "trial": "b"}], stimuli=stim)
    r = Run(ctx, "fix_stale", d)
    check(r.rc == 0 and len(r.trials) == 2, "stale rdk: %s" % r.err)
    check(r.trials[0]["r_updates"] != "" and r.trials[1]["r_updates"] == "" and r.trials[1]["r_digest"] == "", "a template with no rdk has empty rdk columns: %s" % r.trials[1]["r_updates"])
    st = r.read("streams/r.csv")
    check(all(x["trial_index"] == "0" for x in st) and st, "the stream holds only the rdk's own trial")


def t_fix_timeout_values(ctx):
    d = base({"t": {"events": [{"on": "s"}], "response": {"role": "r", "choices": ["left"], "from": "s", "duration_s": 0.2},
                    "after": [{"wait_s": {"by": "$response", "map": {"left": 0.05, "": 0.15}}}],
                    "columns": {"resp": "$response", "resp_time_s": "$rt_s"}}}, [{"name": "l", "trial": "t", "rules": ["reps 2"]}])
    r = Run(ctx, "fix_timeout", d, args=["--participant", "silent"])
    check(r.rc == 0 and len(r.trials) == 2, "a timeout does not abort: %s" % r.err)
    check(all(x["resp"] == "" and x["resp_time_s"] == "" and x["outcome"] == "timeout" for x in r.trials), "columns are empty on a timeout: %s" % r.trials[:1])
    d2 = json.loads(json.dumps(d))
    d2["trials"]["t"]["after"][0]["wait_s"]["map"] = {"left": 0.05}
    r2 = Run(ctx, "fix_timeout_refused", d2)
    check(r2.rc != 0 and "no key for" in r2.err, "a map on $response needs the empty key: %s" % r2.err)
    d3 = json.loads(json.dumps(d))
    d3["trials"]["t"]["after"][0] = {"wait_s": "$rt_s"}
    r3 = Run(ctx, "fix_rt_refused", d3)
    check(r3.rc != 0 and "rt_s" in r3.err, "$rt_s in a duration is refused: %s" % r3.err)


def t_fix_any_key(ctx):
    d = base({"t": {"events": [{"on": "s"}], "response": {"role": "r", "from": "s", "duration_s": 1}, "columns": {"got": "$response"}}},
             [{"name": "l", "trial": "t", "requeue": "no_response", "rules": ["reps 2"]}])
    r = Run(ctx, "fix_any", d, args=["--rt", "0.2"])
    check(r.rc == 0 and len(r.trials) == 2, "any key: %s" % r.err)
    check(all(x["outcome"] == "responded" and x["requeued"] == "0" for x in r.trials), "an any-key press is a response: %s" % r.trials[:1])
    check(all(x["got"].lower() == "space" and x["response"].lower() == "space" for x in r.trials), "the key's name is the response: %s" % r.trials[0]["response"])
    methods = {"st": {"type": "stair", "start": 0.5, "n_up": 1, "n_down": 1, "step_type": "lin", "steps": [0.1], "stop_trials": 2}}
    d2 = base({"t": {"events": [{"on": "s"}], "response": {"role": "r", "from": "s", "duration_s": 1, "key_answer": "space"}, "method_value": "$level"}},
              [{"name": "l", "trial": "t", "tracks": ["st"]}], methods=methods)
    r2 = Run(ctx, "fix_any_track", d2, args=["--rt", "0.2"])
    check(r2.rc == 0 and len(r2.trials) == 2 and all(x["correct"] == "1" for x in r2.trials), "an any-key press reaches the method: %d trials, %s" % (len(r2.trials), r2.err))


def t_fix_from_in_if(ctx):
    d = base({"t": {"events": [{"if": "$show", "steps": [{"on": "s"}]}], "response": {"role": "r", "from": "s", "choices": ["left"]}}},
             [{"name": "l", "trial": "t", "timeline_variables": "x.pstb", "rules": ["order sequential"]}])
    r = Run(ctx, "fix_from", d, tables={"x": "show\n1\n0\n"}, args=["--max-seconds", "5"])
    check(r.rc != 0 and "not shown on this trial" in r.err, "a from inside a false if stops with its name: %s" % r.err)


def t_fix_shown_twice(ctx):
    d = base({"t": {"events": [{"on": "s"}, {"wait_s": 0.1}, {"off": "s"}, {"wait_s": 0.1}, {"on": "s"}, {"wait_s": 0.1}, {"off": "s"}, {"wait_s": 0.1}], "record": ["s"]}},
             [{"name": "l", "trial": "t"}])
    r = Run(ctx, "fix_twice", d)
    check(r.rc == 0, "twice: %s" % r.err)
    if r.rc == 0:
        row = r.trials[0]
        ons = [float(e["t_s"]) for e in r.events if e["stimulus"] == "s" and e["kind"] == "onset"]
        offs = [float(e["t_s"]) for e in r.events if e["stimulus"] == "s" and e["kind"] == "offset"]
        check(int(row["s_frames"]) == 12, "frames over both showings: %s" % row["s_frames"])
        check(abs(float(row["s_onset_s"]) - ons[0]) < 1e-6, "_onset_s is the first onset")
        check(abs(float(row["s_offset_s"]) - offs[-1]) < 1e-6, "_offset_s is the last offset")


def t_fix_vars_data(ctx):
    d = base({"t": {"events": [{"on": "s"}, {"wait_s": 0.05}, {"off": "s"}]}},
             [{"name": "one", "trial": "t", "vars": {"same": "from_vars"}}, {"name": "two", "trial": "t", "data": {"same": "from_data"}}])
    r = Run(ctx, "fix_vars_data", d)
    check(r.rc == 0 and [x["same"] for x in r.trials] == ["from_vars", "from_data"], "a name that is vars in one leaf and data in another: %s %s" % ([x.get("same") for x in r.trials], r.err))
    # one name, a jitter in one leaf and text in another: the column is text
    d2 = base({"t": {"events": [{"on": "s"}, {"wait_s": 0.05}, {"off": "s"}]}},
              [{"name": "one", "trial": "t", "rules": ["jitter same_s uniform 0.1 0.1"]}, {"name": "two", "trial": "t", "vars": {"same_s": "words"}}])
    r2 = Run(ctx, "fix_vars_jitter", d2)
    check(r2.rc == 0 and [x["same_s"] for x in r2.trials] == ["0.1", "words"], "a column of two types holds text: %s %s" % ([x.get("same_s") for x in r2.trials], r2.err))


def make_wav(ctx):
    import struct
    wav = os.path.join(ctx.out, "tone.wav")
    samples = b"".join(struct.pack("<hh", 0, 0) for _ in range(4800))
    with open(wav, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(samples)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, 48000, 192000, 4, 16) + b"data" + struct.pack("<I", len(samples)) + samples)
    return wav


def t_fix_sound_twice(ctx):
    wav = make_wav(ctx)
    d = base({"t": {"events": [{"on": "tone"}, {"wait_s": 0.2}, {"off": "tone"}, {"wait_s": 0.1}, {"on": "tone"}, {"wait_s": 0.2}, {"off": "tone"}, {"wait_s": 0.1}]}},
             [{"name": "l", "trial": "t"}], stimuli={"s": {"kind": "shape", "w": 1, "h": 1}, "tone": {"kind": "sound", "audio": "tone.wav"}})
    sd = dict(Run.SOURCE_EXTRA)
    Run.SOURCE_EXTRA["audio"] = {"rate": 48000, "channels": 2}
    r = Run(ctx, "fix_sound", d, pack_files=[{"name": "tone.wav", "kind": "audio", "source": "tone.wav"}], copy_in=[wav])
    Run.SOURCE_EXTRA.clear()
    Run.SOURCE_EXTRA.update(sd)
    check(r.rc == 0, "sound twice: %s" % r.err)
    check(len([e for e in r.events if e["kind"] == "sound"]) == 2, "a sound plays at each onset: %d" % len([e for e in r.events if e["kind"] == "sound"]))


def t_fix_rdk_limit(ctx):
    # the step buffer is fixed and goes to the stream when full: a 20-minute
    # trial and one only a press ends run to their ends, and a buffer that
    # flushes every 7 steps writes what one that never flushes writes
    stim = {"s": {"kind": "shape", "w": 1, "h": 1},
            "r": {"kind": "rdk", "aperture": "circle", "w": 50, "count": 3, "speed": 10, "dot_size": 2, "color": [1, 1, 1]}}
    d = base({"t": {"events": [{"on": "r"}, {"wait_s": 1200}, {"off": "r"}]}}, [{"name": "l", "trial": "t"}], stimuli=stim)
    r = Run(ctx, "fix_rdk_long", d, args=["--max-seconds", "1300"])
    check(r.rc == 0 and len(r.trials) == 1 and int(r.trials[0]["r_updates"]) >= 72000, "a 20-minute rdk trial runs to its end: %s %s" % (r.err, r.trials[:1] and r.trials[0]["r_updates"]))
    d2 = base({"t": {"events": [{"on": "r"}, {"wait_s": 300}, {"off": "r"}]}}, [{"name": "l", "trial": "t", "rules": ["reps 2"]}], stimuli=stim)
    ra = Run(ctx, "fix_rdk_flush_7", d2, args=["--max-seconds", "1000", "--rdk-buffer", "7"])
    rb = Run(ctx, "fix_rdk_flush_none", d2, args=["--max-seconds", "1000", "--rdk-buffer", "100000"])
    check(ra.rc == 0 and rb.rc == 0 and len(ra.trials) == 2, "300 s rdk trials, flushed and not: %s %s" % (ra.err, rb.err))
    if ra.rc == 0 and rb.rc == 0:
        check(same_file(os.path.join(ra.data, "streams", "r.csv"), os.path.join(rb.data, "streams", "r.csv")), "the stream of a flushed run equals an unflushed one's, byte for byte")
        check([(x["r_digest"], x["r_updates"], x["r_seed"]) for x in ra.trials] == [(x["r_digest"], x["r_updates"], x["r_seed"]) for x in rb.trials] and int(ra.trials[0]["r_updates"]) >= 18000,
              "digest, updates and seed equal: %s" % [(x["r_digest"], x["r_updates"]) for x in ra.trials])
        check(ra.count("streams/r.csv") == sum(int(x["r_updates"]) for x in ra.trials), "every step is in the stream once")
    with open(os.path.join(ctx.out, "late.csv"), "w", newline="\n") as f:
        f.write("trial_index,response,rt_s,hold_s\n0,left,100,0.05\n")
    d3 = base({"t": {"events": [{"on": "r"}], "response": {"role": "r", "from": "r", "choices": ["left"]}}}, [{"name": "l", "trial": "t"}], stimuli=stim)
    r3 = Run(ctx, "fix_rdk_unbounded", d3, args=["--participant", "script:" + os.path.join(ctx.out, "late.csv"), "--max-seconds", "1000"])
    check(r3.rc == 0 and len(r3.trials) == 1 and int(r3.trials[0]["r_updates"]) >= 6000, "an rdk in a trial only a press ends runs to the press: %s %s" % (r3.err, r3.trials[:1] and r3.trials[0]["r_updates"]))


def t_fix_record_inherit(ctx):
    d = base({"t": {"events": [{"on": "s"}, {"wait_s": 0.05}, {"off": "s"}]}},
             [{"name": "c", "record_data": False, "timeline": [{"name": "l", "trial": "t"}]}, {"name": "k", "trial": "t"}])
    r = Run(ctx, "fix_rec", d)
    check(r.rc == 0 and [x["node"] for x in r.trials] == ["k"], "record_data false on a container reaches its children: %s" % [x["node"] for x in r.trials])


def t_fix_backward_at(ctx):
    d = base({"t": {"events": [{"on": "s"}, {"wait_s": 1}, {"off": "s"}, {"at_s": 0.1}, {"on": "t"}]}}, [{"name": "l", "trial": "t"}])
    r = Run(ctx, "fix_at", d)
    check(r.rc == 0, "backward at: %s" % r.err)
    offs = [e for e in r.events if e["stimulus"] == "s" and e["kind"] == "offset"]
    check(len(offs) == 1, "the step before the backward at_s is not dropped")
    check(float(r.trials[0]["time_elapsed_s"]) >= 0.99, "the end is at the latest time: %s" % r.trials[0]["time_elapsed_s"])
    # the wait alone schedules the latest time: nothing follows it
    dw = base({"t": {"events": [{"on": "s"}, {"wait_s": 1}, {"at_s": 0.1}, {"off": "s"}]}}, [{"name": "l", "trial": "t"}])
    rw = Run(ctx, "fix_at_wait", dw)
    check(rw.rc == 0 and float(rw.trials[0]["time_elapsed_s"]) >= 0.99, "a wait's end counts: %s" % (rw.trials[:1] or rw.err))
    # `after` starts at the latest scheduled time of events, and a tween's end counts
    d2 = base({"t": {"events": [{"on": "s"}, {"wait_s": 1}, {"off": "s"}, {"at_s": 0.1}, {"on": "t"}], "after": [{"mark": "go"}]}}, [{"name": "l", "trial": "t"}])
    r2 = Run(ctx, "fix_at2", d2)
    marks = [float(e["t_s"]) for e in r2.events if e["text"] == "go"]
    ons = [float(e["t_s"]) for e in r2.events if e["stimulus"] == "s" and e["kind"] == "onset"]
    check(r2.rc == 0 and marks and marks[0] - ons[0] >= 0.99, "after begins at the latest of events: %s %s" % (marks, ons))
    # 2.5: only `then` moves time; a `to` running at the end is cut there
    d3 = base({"t": {"events": [{"on": "s"}, {"to": "s", "props": {"x": 1}, "duration_s": 1}, {"off": "t"}, {"on": "t"}]}}, [{"name": "l", "trial": "t"}])
    r3 = Run(ctx, "fix_at3", d3)
    check(r3.rc == 0 and float(r3.trials[0]["time_elapsed_s"]) < 0.1, "a to tween does not move the end: %s %s" % (r3.trials[:1] and r3.trials[0]["time_elapsed_s"], r3.err))
    d3b = base({"t": {"events": [{"on": "s"}, {"to": "s", "props": {"opacity": 0}, "duration_s": 5}], "response": {"role": "r", "choices": ["left"], "from": "s", "duration_s": 2, "key_answer": "left"},
                     "after": [{"on": "t"}, {"wait_s": 0.05}, {"off": "t"}]}}, [{"name": "l", "trial": "t"}])
    r3b = Run(ctx, "fix_to_response", d3b, args=["--rt", "0.3"])
    check(r3b.rc == 0 and float(r3b.trials[0]["time_elapsed_s"]) < 0.5, "a to tween does not hold back after a response: %s" % (r3b.trials[:1] and r3b.trials[0]["time_elapsed_s"]))
    d3c = base({"t": {"events": [{"on": "s"}, {"then": "s", "props": {"x": 1}, "duration_s": 1}, {"off": "s"}]}}, [{"name": "l", "trial": "t"}])
    r3c = Run(ctx, "fix_then", d3c)
    check(r3c.rc == 0 and float(r3c.trials[0]["time_elapsed_s"]) >= 0.99, "a then moves time: %s" % (r3c.trials[:1] and r3c.trials[0]["time_elapsed_s"]))
    # a then's end holds back `after` past an early end of the response
    d3d = base({"t": {"events": [{"on": "s"}, {"then": "s", "props": {"x": 1}, "duration_s": 1}], "response": {"role": "r", "choices": ["left"], "from": "s", "duration_s": 0.1},
                     "after": [{"mark": "go"}]}}, [{"name": "l", "trial": "t"}])
    r3d = Run(ctx, "fix_then_after", d3d, args=["--participant", "silent"])
    marks = [float(e["t_s"]) for e in r3d.events if e["text"] == "go"]
    ons = [float(e["t_s"]) for e in r3d.events if e["stimulus"] == "s" and e["kind"] == "onset"]
    check(r3d.rc == 0 and marks and ons and marks[0] - ons[0] >= 0.99, "after starts at a then's end: %s %s %s" % (marks, ons, r3d.err))
    # a then in `after`: the trial ends at its end
    d3e = base({"t": {"events": [{"on": "s"}], "response": {"role": "r", "choices": ["left"], "from": "s", "duration_s": 0.1},
                     "after": [{"on": "t"}, {"then": "t", "props": {"x": 1}, "duration_s": 1}]}}, [{"name": "l", "trial": "t"}])
    r3e = Run(ctx, "fix_then_in_after", d3e, args=["--participant", "silent"])
    check(r3e.rc == 0 and float(r3e.trials[0]["time_elapsed_s"]) >= 1.09, "a then in after holds the end: %s %s" % (r3e.trials[:1] and r3e.trials[0]["time_elapsed_s"], r3e.err))
    # a forward at_s in `after`, and the end goes at the latest
    d4 = base({"t": {"events": [{"on": "s"}], "response": {"role": "r", "from": "s", "duration_s": 0.1, "choices": ["left"]}, "after": [{"at_s": 1.5}, {"mark": "late"}]}}, [{"name": "l", "trial": "t"}])
    r4 = Run(ctx, "fix_at4", d4, args=["--participant", "silent"])
    check(r4.rc == 0 and float(r4.trials[0]["time_elapsed_s"]) >= 1.49 and any(e["text"] == "late" for e in r4.events), "a forward at_s in after: %s %s" % (r4.trials[:1], r4.err))
    # a backward at_s inside `after`: the end stays at the latest time
    d5 = base({"t": {"events": [{"on": "s"}], "response": {"role": "r", "from": "s", "duration_s": 0.1, "choices": ["left"]},
                     "after": [{"on": "t"}, {"wait_s": 1}, {"off": "t"}, {"at_s": 0.2}]}}, [{"name": "l", "trial": "t"}])
    r5 = Run(ctx, "fix_at5", d5, args=["--participant", "silent"])
    check(r5.rc == 0 and float(r5.trials[0]["time_elapsed_s"]) >= 1.0 and any(e["stimulus"] == "t" and e["kind"] == "offset" for e in r5.events), "a backward at_s in after: %s %s" % (r5.trials[:1], r5.err))


def t_fix_refusals(ctx):
    methods = {"st": {"type": "stair", "start": 0.5, "n_up": 1, "n_down": 1, "step_type": "lin", "steps": [0.1], "stop_trials": 2}}
    no_resp = base({"t": {"events": [{"on": "s"}]}}, [{"name": "l", "trial": "t", "tracks": ["st"]}], methods=methods)
    r = Run(ctx, "fix_t13", no_resp)
    check(r.rc != 0 and "needs a template with a 'response'" in r.err, "a track on a template with no response: %s" % r.err)
    no_ka = base({"t": tmpl()}, [{"name": "l", "trial": "t", "tracks": ["st"]}], methods=methods)
    r = Run(ctx, "fix_t14", no_ka)
    check(r.rc != 0 and "key_answer" in r.err, "a track with no key_answer: %s" % r.err)
    txt = base({"t": tmpl(extra={"outcome": "good"})}, [{"name": "l", "trial": "t"}])
    r = Run(ctx, "fix_t18", txt)
    check(r.rc != 0 and "'outcome'" in r.err, "a non-integer outcome: %s" % r.err)
    mixed = base({"t": tmpl(key_answer="left", extra={"method_value": 1})}, [{"name": "l", "trial": "t", "tracks": ["st"], "track_rate": 0.5, "timeline_variables": "x.pstb"}],
                 methods=methods, stimuli={"s": {"kind": "shape", "w": "$level", "h": 1}, "t": {"kind": "shape"}})
    r = Run(ctx, "fix_t16", mixed, tables={"x": "a\nleft\n"})
    check(r.rc != 0 and "$level" in r.err, "$level in a number key on a mixed leaf: %s" % r.err)
    ok = base({"t": tmpl(key_answer="left", extra={"method_value": 1, "columns": {"lv": "$level", "mt": "$method"}})},
              [{"name": "l", "trial": "t", "tracks": ["st"], "track_rate": 0.5, "timeline_variables": "x.pstb"}], methods=methods)
    r = Run(ctx, "fix_t16b", ok, tables={"x": "a\nleft\n"}, args=["--rt", "0.2"])
    check(r.rc == 0, "$level and $method in columns on a mixed leaf: %s" % r.err)
    cond = [x for x in r.trials if x["track"] == "-1"]
    check(cond and all(x["lv"] == "" and x["mt"] == "" for x in cond), "empty on a condition trial")
    # a response-dependent key on a driven property (the channel owns the field)
    drv = base({"t": tmpl(key_answer="left", after=[{"on": "u"}, {"set": "u", "props": {"x": 2}}, {"off": "u"}])}, [{"name": "l", "trial": "t"}],
               stimuli={"s": {"kind": "shape", "w": 1, "h": 1}, "u": {"kind": "shape", "w": 1, "h": 1, "x": {"by": "$responded", "map": {"true": 1, "false": 2}}}})
    r = Run(ctx, "fix_after_driven", drv)
    check(r.rc != 0 and "depends on the response" in r.err, "a response key on a driven property is refused: %s" % r.err)


def t_fix_michelson_order(ctx):
    rig = os.path.join(ctx.out, "rig")
    shutil.rmtree(rig, ignore_errors=True)
    os.makedirs(rig)
    subprocess.check_call([ctx.make_rig, rig])
    stim = {"a_first": {"kind": "shape", "w": "$contrast_shown", "h": 1},
            "z_gabor": {"kind": "gabor", "sf": 1, "sigma": 10, "contrast": {"michelson": 0.2}}}
    d = base({"t": {"events": [{"on": ["a_first", "z_gabor"]}, {"wait_s": 0.05}, {"off": ["a_first", "z_gabor"]}], "columns": {"c": "$contrast_shown"}}},
             [{"name": "l", "trial": "t"}], stimuli=stim, display={"calibration": "required"})
    r = Run(ctx, "fix_michelson", d, args=["--rig", os.path.join(rig, "profile.json")])
    check(r.rc == 0 and float(r.trials[0]["c"]) > 0.1, "a stimulus sorted before the michelson reads $contrast_shown: %s" % r.err)


def t_fix_texture_vars(ctx):
    import zlib, struct
    def png(path, val):
        raw = b"".join(b"\x00" + bytes([val, val, val]) * 4 for _ in range(4))
        def chunk(t, c):
            x = struct.pack(">I", len(c)) + t + c
            return x + struct.pack(">I", zlib.crc32(t + c) & 0xffffffff)
        open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    for n, v in (("a", 10), ("b", 200)):
        png(os.path.join(ctx.out, n + ".png"), v)
    d = base({"t": {"events": [{"on": "p"}, {"wait_s": 0.05}, {"off": "p"}]}},
             [{"name": "l", "trial": "t", "vars": {"pic": {"choice": ["a.ysptex", "b.ysptex"]}}, "rules": ["reps 6"]}],
             stimuli={"p": {"kind": "image", "texture": "$pic", "w": 1, "h": 1}})
    res = [{"name": "a.ysptex", "kind": "texture", "source": "a.png", "primaries": "device"}, {"name": "b.ysptex", "kind": "texture", "source": "b.png", "primaries": "device"}]
    r = Run(ctx, "fix_tex", d, pack_files=res, copy_in=[os.path.join(ctx.out, "a.png"), os.path.join(ctx.out, "b.png")])
    check(r.rc == 0 and {x["pic"] for x in r.trials} == {"a.ysptex", "b.ysptex"}, "texture from vars: %s" % r.err)
    d2 = base({"t": {"events": [{"on": "p"}, {"wait_s": 0.05}, {"off": "p"}]}},
              [{"name": "l", "trial": "t", "timeline_variables": "x.pstb"}], stimuli={"p": {"kind": "image", "texture": "$im", "w": 1, "h": 1}})
    r2 = Run(ctx, "fix_tex2", d2, tables={"x": "im\na.ysptex\nb.ysptex\n"}, pack_files=res, copy_in=[os.path.join(ctx.out, "a.png"), os.path.join(ctx.out, "b.png")])
    check(r2.rc == 0 and len(r2.trials) == 2, "texture from a table column: %s" % r2.err)


def t_fix_walk_budget(ctx):
    # a tree in which no leaf can run is refused at load
    d = base({"t": tmpl()}, [{"name": "c", "repetitions": 1000000, "timeline": [{"name": "e", "repetitions": 3, "timeline": []}]}])
    r = Run(ctx, "fix_walk", d)
    check(r.rc != 0 and "no leaf that can run" in r.err and not os.path.exists(r.data), "no runnable leaf: refused at load: %s" % r.err)
    # many skipped leaves, then a trial: the walk yields frames and goes on
    kids = [{"name": "k%d" % i, "if": "$off", "trial": "t"} for i in range(200)]
    d2 = base({"t": {"events": [{"on": "s"}, {"wait_s": 0.02}, {"off": "s"}]}}, [{"name": "c", "repetitions": 5100, "timeline": kids}, {"name": "z", "trial": "t"}],
              session={"fields": {"participant": {"type": "integer", "required": True}, "off": {"type": "integer", "required": True}}})
    r2 = Run(ctx, "fix_walk_long", d2, args=["--field", "off=0"])
    check(r2.rc == 0 and len(r2.trials) == 1 and r2.trials[0]["node"] == "z", "1,020,000 skipped leaves, then a trial: %s" % r2.err)
    n_fr = r2.count("frames.csv")
    first = next(r2.rows("frames.csv"), {})
    check(n_fr > 100 and first.get("trial_index") == "", "the walk spreads over frames, which belong to no trial: %d" % n_fr)
    # leaf runs: the seed log keeps 65,536 and counts the rest; the session
    # neither stops nor is refused
    d3 = base({"t": {"events": []}}, [{"name": "c", "repetitions": 70000, "timeline": [{"name": "l", "trial": "t"}]}])
    r3 = Run(ctx, "fix_seeds", d3, args=["--max-seconds", "100000"])
    n3 = r3.count("trials.csv")
    check(r3.rc == 0 and n3 == 70000, "70000 leaf runs: %s %d" % (r3.err, n3))
    if r3.rc == 0:
        with open(os.path.join(r3.data, "session.json")) as f:
            sess = json.load(f)
        check(len(sess["node_seeds"]) == 65536 and sess["node_seeds_unlogged"] == 70000 - 65536, "the seed log: %d listed, %s counted" % (len(sess["node_seeds"]), sess.get("node_seeds_unlogged")))
    # container starts: bounded at load as leaf runs are
    d5 = base({"t": {"events": [{"wait_s": 0.02}]}}, [{"name": "c", "repetitions": 1000000, "timeline": [{"name": "c2", "repetitions": 1000000, "timeline": [{"name": "c3", "timeline": []}]}]},
                                                     {"name": "l", "trial": "t"}])
    r5 = Run(ctx, "fix_visits_over", d5)
    check(r5.rc != 0 and "starts its containers" in r5.err and "10000000" in r5.err and not os.path.exists(r5.data), "past the container bound: refused at load: %s" % r5.err)
    d6 = base({"t": {"events": [{"wait_s": 0.02}]}}, [{"name": "c", "repetitions": 4000, "timeline": [{"name": "c2", "repetitions": 1000, "timeline": []}]},
                                                     {"name": "l", "trial": "t"}])
    r6 = Run(ctx, "fix_visits_under", d6)
    check(r6.rc == 0 and len(r6.trials) == 1 and r6.count("frames.csv") > 900, "4,004,000 container starts, then the trial: %s" % r6.err)


def t_fix_data_set(ctx):
    long_v = "x" * 300
    huge_v = "y" * 5000
    d = base({"t": tmpl(key_answer="left", extra={"columns": {"seed": "$trial_seed", "opt": "$maybe"}})},
             [{"name": "l", "trial": "t", "vars": {"v1": "a"}, "data": {"d1": "b"}, "rules": ["reps 2", "jitter j_s uniform 0.1 0.2"]}],
             session={"fields": {"participant": {"type": "integer", "required": True}, "maybe": {"type": "integer", "required": False}}})
    r = Run(ctx, "fix_ds", d, args=["--rt", "0.2"])
    check(r.rc == 0, "data set: %s" % r.err)
    if r.rc == 0:
        check(any(e["kind"] == "response" and e["text"] == "left" for e in r.events), "a response row in events.csv")
        check(all(x["maybe"] == "" and x["opt"] == "" for x in r.trials), "an absent optional integer is empty: %s" % r.trials[0]["maybe"])
        check(r.trials[0]["seed"] == r.trials[0]["trial_seed"] and r.trials[0]["seed"].startswith("x"), "$trial_seed in columns is the x hex: %s" % r.trials[0]["seed"])
        with open(os.path.join(r.data, "trials.csv")) as f:
            h = f.readline().rstrip("\n").split(",")
        check(h.index("v1") < h.index("j_s") < h.index("trial_seed") < h.index("d1"), "item 3's order: vars, jitters, trial_seed, data: %s" % h[15:22])
        with open(os.path.join(r.data, "session.json")) as f:
            sess = json.load(f)
        check(len(sess["headers"]) >= 15 and "devices" in sess and sess["devices"][0]["bound"] == "sim", "session.json: headers and device rows: %d" % len(sess["headers"]))
    dl = base({"t": tmpl(extra={"columns": {"long": "$lt"}})}, [{"name": "l", "trial": "t", "vars": {"lt": long_v}}])
    r2 = Run(ctx, "fix_long", dl, args=["--rt", "0.2"])
    check(r2.rc == 0 and r2.trials[0]["long"] == long_v and r2.trials[0]["lt"] == long_v, "a 300-byte value is written whole: %s" % r2.err)
    dh = base({"t": tmpl(extra={"columns": {"long": "$lt"}})}, [{"name": "l", "trial": "t", "vars": {"lt": huge_v}}])
    r2b = Run(ctx, "fix_huge", dh, args=["--rt", "0.2"])
    check(r2b.rc != 0 and "5000" in r2b.err and not os.path.exists(r2b.data), "a value past 4096 bytes is refused at load: %s" % r2b.err)
    # an array in vars and data: a value with one text form, fed to the stimulus (item 2 of round 2)
    da = base({"t": {"events": [{"on": "s"}, {"wait_s": 0.02}, {"off": "s"}]}}, [{"name": "l", "trial": "t", "vars": {"col": [1, 0, 0.5]}, "data": {"arr": [2, 3]}}],
              stimuli={"s": {"kind": "shape", "w": 1, "h": 1, "color": "$col"}})
    ra = Run(ctx, "fix_array", da)
    check(ra.rc == 0 and ra.trials[0]["col"] == "[1,0,0.5]" and ra.trials[0]["arr"] == "[2,3]", "arrays in vars and data: %s %s" % (ra.err, ra.trials[:1] and (ra.trials[0]["col"], ra.trials[0]["arr"])))
    nm = "n" * 60
    dn = base({"t": {"events": [{"on": nm}, {"wait_s": 0.05}, {"off": nm}], "record": [nm]}}, [{"name": "l", "trial": "t"}], stimuli={nm: {"kind": "shape", "w": 1, "h": 1}})
    r3 = Run(ctx, "fix_longname", dn)
    check(r3.rc != 0 and "longer than 63" in r3.err, "a derived column past 63 bytes: %s" % r3.err)


def t_fix_check_all_leaves(ctx):
    # h_check_ops: the second leaf has the larger jitter; a negative column
    # duration on the second leaf only must be seen
    d = base({"t": {"events": [{"on": "s"}, {"wait_s": "$soa_s"}, {"off": "s"}]}},
             [{"name": "a", "trial": "t", "timeline_variables": "x.pstb"}, {"name": "b", "trial": "t", "timeline_variables": "y.pstb"}])
    r = Run(ctx, "fix_all_leaves", d, tables={"x": "soa_s\n0.1\n", "y": "soa_s\n-0.5\n"})
    check(r.rc != 0 and ("op" in r.err or "duration" in r.err) and not os.path.exists(r.data), "the second leaf's negative wait is seen at load: %s" % r.err)


def t_fix_release_order(ctx):
    # a release held past the end of its trial must not arrive after the
    # next trial's press
    d = base({"t": tmpl(key_answer="left", after=[{"wait_s": 0.01}])}, [{"name": "l", "trial": "t", "rules": ["reps 4"]}])
    with open(os.path.join(ctx.out, "hold.csv"), "w", newline="\n") as f:
        f.write("trial_index,response,rt_s,hold_s\n0,left,0.2,0.5\n1,left,0.2,0.5\n2,left,0.2,0.5\n3,left,0.2,0.5\n")
    r = Run(ctx, "fix_release", d, args=["--participant", "script:" + os.path.join(ctx.out, "hold.csv")])
    check(r.rc == 0 and all(float(x["rt_s"]) == 0.2 for x in r.trials), "held keys across trials: %s" % r.err)


def t_r2_release_other_template(ctx):
    # a key held past its trial's end: the release goes to the collector that
    # saw the press, so the next template's press counts
    with open(os.path.join(ctx.out, "hold_x.csv"), "w", newline="\n") as f:
        f.write("trial_index,response,rt_s,hold_s\n0,left,0.1,1.5\n1,left,0.1,0.05\n2,left,0.2,0.05\n")
    ta = {"events": [{"on": "s"}], "response": {"role": "r", "choices": ["left", "right"], "from": "s", "duration_s": 0.5, "key_answer": "left"}}
    tb = {"events": [{"on": "t"}], "response": {"role": "r", "choices": ["left", "right"], "from": "t", "duration_s": 0.5, "key_answer": "left"}}
    d = base({"a": ta, "b": tb}, [{"name": "one", "trial": "a"}, {"name": "two", "trial": "b"}, {"name": "three", "trial": "a"}])
    r = Run(ctx, "r2_release", d, args=["--participant", "script:" + os.path.join(ctx.out, "hold_x.csv")])
    check(r.rc == 0 and [x["outcome"] for x in r.trials] == ["correct", "correct", "correct"], "a held key does not cost the next template's press: %s %s" % (r.err, [x["outcome"] for x in r.trials]))


def t_r2_yield_draws_nothing(ctx):
    kids = [{"name": "k%d" % i, "if": "$off", "trial": "t"} for i in range(100)]
    d = base({"t": {"events": [{"on": "s"}, {"wait_s": 0.02}, {"off": "s"}]}},
             [{"name": "a", "trial": "t"}, {"name": "c", "repetitions": 300, "timeline": kids}, {"name": "z", "trial": "t"}],
             session={"fields": {"participant": {"type": "integer", "required": True}, "off": {"type": "integer", "required": True}}})
    r = Run(ctx, "r2_yield", d, args=["--field", "off=0", "--trace-channels", "trace.csv"])
    check(r.rc == 0 and len(r.trials) == 2, "yield: %s" % r.err)
    traced = {x["frame"] for x in trace_rows(ctx, r)}
    fr = r.read("frames.csv")
    no_trial = [x["frame"] for x in fr if x["trial_index"] == ""]
    check(len(no_trial) > 3 and not (traced & set(no_trial)), "frames of no trial draw nothing: %d such frames, %d of them drew" % (len(no_trial), len(traced & set(no_trial))))


def t_r2_low(ctx):
    d = base({"t": {"events": [{"on": "s"}, {"wait_s": 0.1}, {"off": "s"}, {"wait_s": 0.1}, {"on": "s"}, {"wait_s": 0.1}], "record": ["s"]}}, [{"name": "l", "trial": "t"}])
    r = Run(ctx, "r2_left_on", d)
    if r.rc == 0:
        ons = [float(e["t_s"]) for e in r.events if e["stimulus"] == "s" and e["kind"] == "onset"]
        check(abs(float(r.trials[0]["s_offset_s"]) - (ons[0] + float(r.trials[0]["time_elapsed_s"]))) < 1e-6, "a stimulus left on: its offset is the trial's end: %s" % r.trials[0]["s_offset_s"])
    check(r.rc == 0, "left on: %s" % r.err)
    d2 = base({"t": {"events": [{"wait_s": 0.02}]}}, [{"name": "l", "trial": "t", "vars": {"x": "v"}, "data": {"x": "d"}}])
    r2 = Run(ctx, "r2_same_node", d2)
    check(r2.rc != 0 and "both a 'vars' name and a 'data' column" in r2.err, "vars and data of one name on one node are refused: %s" % r2.err)
    d3 = base({"t": {"events": [{"wait_s": 0.02}]}}, [{"name": "a", "trial": "t", "timeline_variables": "x.pstb"}, {"name": "b", "trial": "t", "vars": {"a": "v"}}])
    r3 = Run(ctx, "r2_table_vars", d3, tables={"x": "a\nq\n"})
    check(r3.rc == 0 and [x["a"] for x in r3.trials] == ["q", "v"], "a table column here and a vars there share the column: %s %s" % (r3.err, [x.get("a") for x in r3.trials]))


def t_r2_relaxed(ctx):
    # ysp/experiment.h 0.1.2: definitions 0.1.1 refused run, on presses and timeouts
    def script(name, rows):
        path = os.path.join(ctx.out, name)
        with open(path, "w", newline="\n") as f:
            f.write("trial_index,response,rt_s,hold_s\n" + "".join("%d,%s,%s,%s\n" % ((i,) + r) for i, r in enumerate(rows)))
        return ["--participant", "script:" + path]
    press = lambda k, rt: (k, str(rt), "0.05")
    silent = ("", "0", "0")
    methods = {"st": {"type": "stair", "start": 0.5, "n_up": 1, "n_down": 1, "step_type": "lin", "steps": [0.1], "stop_trials": 3}}
    # time_elapsed_s counts from the session's start: a trial's length is the
    # step between rows (plus the one frame between trials)
    def el(r):
        t = [float(x["time_elapsed_s"]) for x in r.trials]
        return [b - a for a, b in zip([0.0] + t, t)]
    # outcome and method_value: lookups by $response with no "" key
    d = base({"t": tmpl(key_answer="left", extra={"outcome": {"by": "$response", "map": {"left": 1, "right": 0}},
                                                  "method_value": {"by": "$responded", "map": {"true": {"by": "$response", "map": {"left": "$level", "right": 0.2}}}}})},
             [{"name": "l", "trial": "t", "tracks": ["st"]}], methods=methods)
    r = Run(ctx, "r2_relax_rsp", d, args=script("rx1.csv", [press("left", 0.2), silent, press("right", 0.2), press("left", 0.2)]))
    check(r.rc == 0 and [x["outcome"] for x in r.trials] == ["correct", "timeout", "wrong", "correct"], "outcome and method_value by $response: %s %s" % (r.err, [x["outcome"] for x in r.trials]))
    # no deadline: $rt_s in a number key
    d = base({"t": {"events": [{"on": "s"}], "response": {"role": "r", "choices": ["left"], "from": "s", "duration_s": 0},
                    "after": [{"wait_s": "$rt_s"}, {"off": "s"}]}}, [{"name": "l", "trial": "t"}])
    r = Run(ctx, "r2_relax_nodl", d, args=script("rx2.csv", [press("left", 0.3)]))
    check(r.rc == 0 and abs(el(r)[0] - 0.6) < 0.05, "wait_s $rt_s with no deadline: %s %s" % (r.err, el(r)))
    # the "true" entry of a lookup by $responded reads $rt_s
    d = base({"t": tmpl(after=[{"wait_s": {"by": "$responded", "map": {"true": "$rt_s", "false": 0.5}}}])}, [{"name": "l", "trial": "t", "rules": ["reps 2"]}])
    r = Run(ctx, "r2_relax_byresp", d, args=script("rx3.csv", [press("left", 0.2), silent]))
    check(r.rc == 0 and len(r.trials) == 2 and abs(el(r)[0] - 0.4) < 0.05 and abs(el(r)[1] - 1.5) < 0.04, "a lookup by $responded reads $rt_s: %s %s" % (r.err, el(r)))
    # the steps of if $responded: a lookup by $response with no "" key
    d = base({"t": tmpl(after=[{"if": "$responded", "steps": [{"wait_s": {"by": "$response", "map": {"left": "$rt_s", "right": 0.1}}}]}])},
             [{"name": "l", "trial": "t", "rules": ["reps 2"]}])
    r = Run(ctx, "r2_relax_if", d, args=script("rx4.csv", [press("left", 0.3), silent]))
    check(r.rc == 0 and len(r.trials) == 2 and abs(el(r)[0] - 0.6) < 0.05 and abs(el(r)[1] - 1.0) < 0.04, "if $responded: %s %s" % (r.err, el(r)))
    # a mixed leaf: $level in a method's entry of a lookup by $method, also through vars
    d = base({"t": tmpl(key_answer="left", extra={"method_value": {"by": "$response", "map": {"left": "$level", "right": "$level"}},
                                                  "columns": {"wv": {"by": "$method", "map": {"st": "$level", "": "$n"}}, "hv": "$x"}})},
             [{"name": "l", "trial": "t", "tracks": ["st"], "track_rate": 0.5, "timeline_variables": "x.pstb",
               "vars": {"x": {"by": "$method", "map": {"st": "$level", "": 2}}}}], methods=methods,
             stimuli={"s": {"kind": "shape", "w": {"by": "$method", "map": {"st": "$level", "": "$n"}}, "h": "$x"}, "t": {"kind": "shape"}})
    r = Run(ctx, "r2_relax_level", d, tables={"x": "n\n7\n"}, args=["--rt", "0.2"])
    tr = [x for x in r.trials if x["track"] != "-1"]
    cd = [x for x in r.trials if x["track"] == "-1"]
    check(r.rc == 0 and tr and cd and all(x["wv"] == x["level"] and x["hv"] == x["level"] for x in tr) and all(x["wv"] == "7" and x["hv"] == "2" for x in cd),
          "$level by $method on a mixed leaf: %s %s" % (r.err, [(x.get("track"), x.get("wv"), x.get("hv")) for x in r.trials]))


def t_r4_leaf_loop_done_track(ctx):
    # a leaf loop whose track is done: a pass with no trials ends the loop,
    # so the session finishes promptly instead of spinning in one frame
    import time
    methods = {"st": {"type": "stair", "start": 0.5, "n_up": 1, "n_down": 1, "step_type": "lin", "steps": [0.1], "stop_trials": 3}}
    d = base({"t": tmpl(key_answer="left")},
             [{"name": "c", "repetitions": 1000, "timeline": [{"name": "l", "trial": "t", "tracks": ["st"], "loop": {"until_accuracy": 1, "max": 1000000}}]}], methods=methods)
    t0 = time.time()
    r = Run(ctx, "r4_loop_spin", d, args=["--participant", "p=0.0"])
    took = time.time() - t0
    check(r.rc == 0 and len(r.trials) == 3 and took < 30, "a done track's leaf loop ends: rc %d, %d trials, %.1f s: %s" % (r.rc, len(r.trials), took, r.err[:200]))
    if r.rc == 0:
        with open(os.path.join(r.data, "session.json")) as f:
            sess = json.load(f)
        # rep 1: the pass with 3 trials and the empty pass that ends the loop;
        # reps 2..1000: one empty open each
        opens = len(sess["node_seeds"]) + sess["node_seeds_unlogged"]
        check(opens == 1001 and sess["node_seeds_unlogged"] == 0, "leaf opens: %d listed, %s unlogged" % (len(sess["node_seeds"]), sess.get("node_seeds_unlogged")))


def t_r4_requeue_full(ctx):
    # a trial that never gets a response requeues until ysp/trials.h's
    # history is full; the session then stops with a named error
    d = base({"t": {"events": [{"on": "s"}], "response": {"role": "r", "choices": ["left"], "from": "s", "duration_s": 0.05}}},
             [{"name": "l", "trial": "t", "requeue": "no_response"}])
    r = Run(ctx, "r4_requeue_full", d, args=["--participant", "silent"])
    n = r.count("trials.csv")
    check(r.rc != 0 and "requeue: no_response" in r.err and "4096" in r.err and "session trial" in r.err and n >= 4000,
          "a requeue past the history stops by name: rc %d, %d rows: %s" % (r.rc, n, r.err[:300]))


def t_r5_wait_final(ctx):
    # a press after the plan's onset and before the flip record's (a landing
    # frame shown late, --test-plan-early): it is early against the final
    # onset, so it must not end the window, and `after` must not start on it
    # (ysp/response.h 0.9.0, yrsp_set_wait_final())
    late = 1 / 60.0
    with open(os.path.join(ctx.out, "early.csv"), "w", newline="\n") as f:
        f.write("trial_index,response,rt_s,hold_s\n0,left,-0.005,0.05\n")
    d = base({"t": {"events": [{"on": "s"}], "after": [{"mark": "go"}, {"wait_s": 0.1}],
                    "response": {"role": "r", "choices": ["left"], "from": "s", "duration_s": 1}}},
             [{"name": "l", "trial": "t"}])
    r = Run(ctx, "r5_wait_deadline", d, args=["--participant", "script:" + os.path.join(ctx.out, "early.csv"), "--max-trials", "1",
                                              "--test-plan-early", repr(late)])
    ok = r.rc == 0 and len(r.trials) == 1
    check(ok, "wait for the record, with a deadline: %s" % r.err)
    if ok:
        t = r.trials[0]
        onset = float([e for e in r.events if e["kind"] == "onset" and e["stimulus"] == "s"][0]["t_s"])
        go = float([e for e in r.events if e["kind"] == "mark" and e["text"] == "go"][0]["t_s"])
        check(t["outcome"] == "timeout" and t["n_early"] == "1" and not int(t["rsp_flags"]) & 0x4,
              "an early press in the gap is no response and does not end the window: %s" % {k: t[k] for k in ("outcome", "n_early", "rsp_flags")})
        check(go >= onset + 1.0 - 0.001, "after starts at the deadline from the final onset (%.4f s after it), not at the press" % (go - onset))
    # no deadline: the window stays open after the early press (the trial
    # would wait for another press; the budget ends the run)
    d2 = base({"t": {"events": [{"on": "s"}], "after": [{"mark": "go"}],
                     "response": {"role": "r", "choices": ["left"], "from": "s", "duration_s": 0}}},
              [{"name": "l", "trial": "t"}])
    r2 = Run(ctx, "r5_wait_nodeadline", d2, args=["--participant", "script:" + os.path.join(ctx.out, "early.csv"), "--max-trials", "1",
                                                  "--test-plan-early", repr(late), "--max-seconds", "3"])
    check(r2.rc != 0 and "budget" in r2.err and not any(e["kind"] == "mark" for e in r2.events),
          "no deadline: the early press does not end the window: rc %d %s" % (r2.rc, r2.err[:200]))
    # landing frames shown more frames late than the old 4-frame bound: the
    # record, whenever it comes, decides; a lookup by $response with no ""
    # key (duration 0's promise) is never read on the early press
    for plan_early in (0.05, 0.1):
        for name, aft in (("mark", [{"mark": "go"}]), ("lookup", [{"wait_s": {"by": "$response", "map": {"left": 0.05}}}])):
            d3 = base({"t": {"events": [{"on": "s"}], "after": aft,
                             "response": {"role": "r", "choices": ["left"], "from": "s", "duration_s": 0}}},
                      [{"name": "l", "trial": "t"}])
            r4 = Run(ctx, "r5_wait_late_%s_%g" % (name, plan_early), d3,
                     args=["--participant", "script:" + os.path.join(ctx.out, "early.csv"), "--max-trials", "1",
                           "--test-plan-early", repr(plan_early), "--max-seconds", "3"])
            check(r4.rc != 0 and "budget" in r4.err and not any(e["kind"] == "mark" for e in r4.events),
                  "a landing %g s late: the early press does not end the window (%s): rc %d %s" % (plan_early, name, r4.rc, r4.err[:200]))
    # the safety net (1 s) passed before the record: the press ends the
    # window on the plan, and the record in the same frame makes it early,
    # before `after` is built: ENDED_EARLY is back (named in player.md), and
    # the row and `after` hear the same, final result
    d4 = base({"t": {"events": [{"on": "s"}], "after": [{"mark": "go"}],
                     "response": {"role": "r", "choices": ["left"], "from": "s", "duration_s": 0}}},
              [{"name": "l", "trial": "t"}])
    r5 = Run(ctx, "r5_wait_net", d4, args=["--participant", "script:" + os.path.join(ctx.out, "early.csv"), "--max-trials", "1",
                                          "--test-plan-early", "1.2"])
    ok = r5.rc == 0 and len(r5.trials) == 1
    check(ok, "the safety net: %s" % r5.err[:200])
    if ok:
        t = r5.trials[0]
        f = int(t["rsp_flags"])
        check(t["response"] == "" and t["rt_s"] == "" and not f & 0x8000 and f & 0x4 and f & 0x80 and t["n_early"] == "1"
              and any(e["kind"] == "mark" for e in r5.events),
              "the safety net: the row is the final result: %s" % {k: t[k] for k in ("response", "rt_s", "rsp_flags", "n_early", "outcome")})
    # the same press after the final onset is a response
    with open(os.path.join(ctx.out, "late.csv"), "w", newline="\n") as f:
        f.write("trial_index,response,rt_s,hold_s\n0,left,0.005,0.05\n")
    r3 = Run(ctx, "r5_wait_valid", d2, args=["--participant", "script:" + os.path.join(ctx.out, "late.csv"), "--max-trials", "1",
                                             "--test-plan-early", repr(late)])
    check(r3.rc == 0 and len(r3.trials) == 1 and r3.trials[0]["response"] == "left" and abs(float(r3.trials[0]["rt_s"]) - 0.005) < 1e-6,
          "a press after the final onset is the response: %s %s" % (r3.trials[:1], r3.err[:200]))


def t_r6_allow_below(ctx):
    # --allow-below-tier takes role names: a flag where its value goes (it
    # would swallow that flag), an empty entry, or a name that is no role
    # of the definition is a bad argument (exit 2), never a silent no-op
    d = base({"t": tmpl()}, [{"name": "l", "trial": "t"}])
    for tag, val, word in (("flag", "--measure", "--measure"), ("trail", "r,", "empty"), ("lead", ",r", "empty"),
                           ("double", "r,,r", "empty"), ("unknown", "nosuch", "'nosuch'"), ("unknown2", "r,nosuch", "'nosuch'")):
        r = Run(ctx, "r6_allow_" + tag, d, args=["--max-trials", "1", "--allow-below-tier", val])
        check(r.rc == 2 and "--allow-below-tier" in r.err and word in r.err and not os.path.exists(r.data),
              "--allow-below-tier %s: exit %d: %s" % (val, r.rc, r.err[:200]))
    r = Run(ctx, "r6_allow_ok", d, args=["--max-trials", "1", "--allow-below-tier", "r"])
    check(r.rc == 0, "--allow-below-tier r: exit %d: %s" % (r.rc, r.err[:200]))

def v2base(trials, timeline, stimuli=None, roles=None, extra=None):
    d = base(trials, timeline, stimuli=stimuli, devices={})
    d["version"] = 2
    if roles:
        d["roles"] = roles
    if extra:
        d.update(extra)
    return d


def v2_rig(ctx, run_dir):
    rig = os.path.join(ctx.out, "v2_rig")
    if not os.path.exists(os.path.join(rig, "profile.json")):
        os.makedirs(rig, exist_ok=True)
        subprocess.run([ctx.make_rig, rig, "--view", "1024", "320"], check=True)
    return os.path.join(rig, "profile.json")


def write_samples(path, rows):
    with open(path, "w", newline="\n") as f:
        f.write("t_s,role,x,y\n")
        for t, role, x, y in rows:
            f.write("%.6f,%s,%g,%g\n" % (t, role, x, y))


def t_v2_states(ctx):
    # states, exits, flags, instant and shared exits, max_visits and over,
    # outcomes (D36) and requeue_outcomes, with no signal role: the chart
    # alone, its state rows in events.csv (3.15)
    t = {"flags": ["seen"],
         "states": [
             {"name": "a", "steps": [{"on": "s"}, {"flag": "seen", "value": True}, {"wait_s": 0.1}]},
             {"name": "b", "exits": [{"if": "$seen", "to": "c"}, {"to": "end", "outcome": "never"}]},
             {"name": "c", "steps": [{"wait_s": 0.05}], "exits": [{"after_s": 0.2, "to": "c", "outcome": "again"}],
              "max_visits": 2, "over": {"to": "end", "outcome": "too_many"}}],
         "exits": [{"after_s": 5, "to": "end", "in": ["c"]}],
         "requeue_outcomes": ["never"]}
    d = v2base({"t": t}, [{"name": "l", "trial": "t", "rules": ["reps 2"]}])
    r = Run(ctx, "v2_states", d)
    check(r.rc == 0, "v2 states: exit %d: %s" % (r.rc, r.err[:300]))
    if r.rc != 0:
        return
    check(len(r.trials) == 2 and all(x["outcome"] == "too_many" and x["seen"] == "1" for x in r.trials),
          "v2 states: outcome too_many and the flag set on both trials: %s" % [(x["outcome"], x.get("seen")) for x in r.trials])
    st = [e for e in r.events if e["kind"] == "state" and e["trial_index"] == "0"]
    names = [e["text"] for e in st]
    check(names == ["a", "b", "c", "c", "c"], "v2 states: the visits a, b, c, c and c by over: %s" % names)
    if len(st) == 5:
        check(st[0]["code"] == "-1" and st[1]["code"] == "0" and st[2]["code"] == "0" and st[3]["code"] == "0" and st[4]["code"] == "-2",
              "v2 states: codes -1 (steps), 0 (the if), 0 (after_s), 0, -2 (over): %s" % [e["code"] for e in st])
        # after_s 0.2 from the entry frame's onset: 12 frames at 60 Hz
        dt = float(st[3]["t_s"]) - float(st[2]["t_s"])
        check(abs(dt - 0.2) < 0.009, "v2 states: c's second visit 0.2 s after its first: %.6f" % dt)
        check(float(st[0]["end_s"]) - float(st[0]["t_s"]) > 0.09, "v2 states: a's row ends when its steps do: %s" % st[0])


def t_v2_until(ctx):
    # an events template with until (rule 9) and a timeout: no sample, so
    # the timeout ends the trial with "timeout"; a real run with a signal
    # role is refused by name before anything opens (G18)
    t = {"controls": {"near": {"control": "region", "of": "hand", "radius": 5, "outside": True}},
         "events": [{"on": "s"}, {"until": "near", "timeout_s": 0.5}, {"off": "s"}]}
    roles = {"hand": {"type": "position2d_relative", "units": "display_px"}}
    d = v2base({"t": t}, [{"name": "l", "trial": "t"}], roles=roles)
    r = Run(ctx, "v2_until", d, args=["--sim-gain", "hand=1"])
    check(r.rc == 0 and len(r.trials) == 1 and r.trials[0]["outcome"] == "timeout",
          "v2 until: the timeout's outcome: exit %d %s %s" % (r.rc, r.err[:200], [x.get("outcome") for x in r.trials]))
    if r.rc == 0 and r.trials:
        el = float(r.trials[0]["time_elapsed_s"])
        check(0.49 < el < 0.55, "v2 until: the trial ends 0.5 s in: %s" % el)
    r = Run(ctx, "v2_until_gainless", d)
    check(r.rc == 2 and "--sim-gain hand=" in r.err, "v2 until: a relative role needs --sim-gain: exit %d %s" % (r.rc, r.err[:200]))
    # the real run's refusal: before the writer, the screen and the devices
    r = Run(ctx, "v2_real", d)
    cmd = [ctx.player, r.pack, "--out", os.path.join(r.dir, "real"), "--field", "participant=3"]
    p2 = subprocess.run(cmd, cwd=r.dir, capture_output=True, text=True, timeout=60)
    # a player built without SDL (the mutation test's) has no real run; its
    # in-process check (mut_test.c, case_v2) sees the refusal instead
    if "built without SDL" not in p2.stderr:
        check(p2.returncode == 1 and "G18" in p2.stderr and "signal role 'hand'" in p2.stderr and not os.path.exists(os.path.join(r.dir, "real")),
              "v2: a real run with a signal role is refused by name: exit %d %s" % (p2.returncode, p2.stderr[:300]))
    # an event role below its min_tier in a version 2 definition: refused as
    # version 1 is, before the screen opens
    d2 = v2base({"t": tmpl()}, [{"name": "l", "trial": "t"}])
    d2["devices"] = {"r": {"direction": "in", "family": ["keyboard"], "required": True, "min_tier": 1}}
    r = Run(ctx, "v2_min_tier", d2, args=["--max-trials", "1"])
    cmd = [ctx.player, r.pack, "--out", os.path.join(r.dir, "real"), "--field", "participant=3"]
    p2 = subprocess.run(cmd, cwd=r.dir, capture_output=True, text=True, timeout=60)
    if "built without SDL" not in p2.stderr:
        check(p2.returncode != 0 and "tier" in p2.stderr and not os.path.exists(os.path.join(r.dir, "real")),
              "v2: a role below its min_tier is refused: exit %d %s" % (p2.returncode, p2.stderr[:300]))


def t_v2_hand(ctx):
    # a hand: a hit with a hold, a movement response (rsp_role), the column
    # forms, freeze_at, a sample and a frame trace, a walk and a uniform,
    # value forms on a stimulus, cm units
    roles = {"hand": {"type": "position2d_relative", "units": "display_cm"}}
    stim = {"s": {"kind": "shape", "w": 2, "h": 2, "x": "$ph"}, "dot": {"kind": "shape", "w": 0.5, "h": 0.5, "layer": 1},
            "goal": {"kind": "shape", "w": 2, "h": 2, "x": "$aim_x", "y": "$aim_y", "layer": 2}}
    t = {"controls": {
            "cur": {"control": "cursor", "role": "hand", "transform": {"rotation_deg": "$turn"}, "freeze_at": "out", "stimulus": "dot"},
            "home": {"control": "region", "of": "hand", "radius": 1, "stay_s": 0.2},
            "out": {"control": "region", "of": "hand", "radius": 5, "outside": True},
            "go": {"control": "movement", "of": "hand", "onset": 0.5, "from": "home"},
            "top": {"control": "peak", "of": "hand", "from": "home"},
            "near": {"control": "distance", "of": "hand", "scale": 2},
            "path": {"control": "trace", "of": ["hand"], "from": "home", "to": "out"},
            "fr": {"control": "trace", "of": ["cur", "near"], "rate": "frame", "from": "reach"}},
         "states": [
            {"name": "hold", "steps": [{"on": ["s", "dot"]}, {"set": "s", "props": {"w": {"signal": "near"}}}], "exits": [{"on": "home", "to": "reach"}]},
            {"name": "reach", "steps": [{"on": "goal"}, {"set": "goal", "props": {"visible": {"when": "out", "then": 0, "else": 1}}}],
             "exits": [{"on": "out", "to": "end"}]}],
         "response": {"choices": [{"signal": "go"}], "from": "goal"},
         "columns": {"out_x": {"hit": "out.x"}, "out_s": {"time": "out", "from": "goal"}, "top": {"signal": "top"},
                     "fast": {"when": {"signal": "top", "above": 10}, "then": 1, "else": 0}, "cur_end_y": {"signal": "cur.y"}}}
    tl = [{"name": "blk", "vars": {"turn": 90, "aim": {"walk2d": {"step": 3, "box": [[-5, -5], [5, 5]]}},
                                   "ph": {"uniform": [0, 1], "n": 1}},
           "timeline": [{"name": "l", "trial": "t", "rules": ["reps 2"]}]}]
    d = v2base({"t": t}, tl, stimuli=stim, roles=roles, extra={"display": {"units": "cm"}})
    smp = []
    # trial 0: still for 0.4 s, then right at 20 cm/s; trial 1 alike; counts of 0.01 cm
    for k in range(400):
        smp.append((0.001 * k, "hand", 0, 0))
    for k in range(1, 400):
        smp.append((0.4 + 0.001 * k, "hand", 2, 0))
    # trial 1: back to the start in one report, still, then right again
    smp.append((1.0, "hand", -798, 0))
    for k in range(1, 500):
        smp.append((1.0 + 0.001 * k, "hand", 0, 0))
    for k in range(1, 400):
        smp.append((1.5 + 0.001 * k, "hand", 2, 0))
    rig = v2_rig(ctx, None)
    sp = os.path.join(ctx.out, "v2_hand_samples.csv")
    write_samples(sp, smp)
    r = Run(ctx, "v2_hand", d, args=["--rig", rig, "--sim-gain", "hand=0.01", "--samples", sp, "--max-seconds", "30"])
    check(r.rc in (0, 1), "v2 hand: exit %d %s" % (r.rc, r.err[:300]))
    if not r.trials:
        check(False, "v2 hand: no rows: %s" % r.err[:300])
        return
    x = r.trials[0]
    check(x["rsp_role"] == "hand" and x["response"] == "go", "v2 hand: the movement's response and rsp_role: %s %s" % (x.get("response"), x.get("rsp_role")))
    check(x["out_x"] != "" and float(x["out_x"]) >= 5, "v2 hand: the hit's value of out: %s" % x.get("out_x"))
    check(x["out_s"] != "" and float(x["out_s"]) > 0, "v2 hand: a time column: %s" % x.get("out_s"))
    check(abs(float(x["top"]) - 20) < 1e-6 and x["fast"] == "1", "v2 hand: peak 20 cm/s, fast: %s %s" % (x.get("top"), x.get("fast")))
    # rotation_deg 90 counterclockwise on the screen, y down: x right becomes y up (negative)
    check(float(x["cur_end_y"]) <= -5 + 1e-9, "v2 hand: the cursor turned 90 deg ccw and frozen at the crossing: %s" % x.get("cur_end_y"))
    for c in ("aim_x", "aim_y", "aim_from_x", "aim_from_y", "aim_deg", "ph"):
        check(c in x and x[c] != "", "v2 hand: the variable column %s: %r" % (c, x.get(c)))
    check(0 <= float(x["ph"]) < 1 and x["ph"] == r.trials[1]["ph"], "v2 hand: a uniform of 1 is a number, one per leaf run (D29): %s" % [y.get("ph") for y in r.trials])
    check(abs(math.hypot(float(x["aim_x"]) - float(x["aim_from_x"]), float(x["aim_y"]) - float(x["aim_from_y"])) - 3) < 1e-9,
          "v2 hand: the walk's step: %s" % x)
    path = r.read(os.path.join("streams", "path.csv")) if hasattr(r, "read") else []
    check(len(path) > 10 and set(path[0]) >= {"trial_index", "t_s", "hand_x", "hand_y", "tier", "device"},
          "v2 hand: the sample trace's rows and columns: %d %s" % (len(path), list(path[0]) if path else None))
    fr = r.read(os.path.join("streams", "fr.csv"))
    check(len(fr) > 2 and set(fr[0]) >= {"trial_index", "frame", "planned_s", "t_s", "cur_x", "cur_y", "near"},
          "v2 hand: the frame trace's rows and columns: %d %s" % (len(fr), list(fr[0]) if fr else None))
    states = [e["text"] for e in r.events if e["kind"] == "state"]
    check(states[:2] == ["hold", "reach"], "v2 hand: the state rows: %s" % states[:4])
    # the trace's window opens at home's hit, 0.2 s held from the trial's
    # start: no row of trial 0 before it (3.7)
    hold = [e for e in r.events if e["kind"] == "state" and e["text"] == "hold" and e["trial_index"] == "0"]
    p0 = [float(x["t_s"]) for x in path if x["trial_index"] == "0"]
    check(hold and p0 and min(p0) >= float(hold[0]["t_s"]) + 0.199,
          "v2 hand: the trace starts at home's hit: first %s, hold %s" % (min(p0) if p0 else None, hold[0]["t_s"] if hold else None))


def t_v2_rule5(ctx):
    # rule 5: a hit starts a state whose timed exit is already due at the
    # frame's onset; the frame takes that exit too, so the trial ends on the
    # hit's frame, not one frame later. Also --profile: a build with no
    # trace ring refuses it before the data set exists (exit 2); a trace
    # ring build writes profile.csv
    t = {"controls": {"away": {"control": "region", "of": "hand", "radius": 1, "outside": True}},
         "states": [{"name": "a", "steps": [{"on": "s"}], "exits": [{"on": "away", "to": "b"}]},
                    {"name": "b", "exits": [{"after_s": 0.001, "to": "end", "outcome": "done_b"}]}]}
    roles = {"hand": {"type": "position2d_relative", "units": "display_px"}}
    d = v2base({"t": t}, [{"name": "l", "trial": "t"}], roles=roles)
    sp = os.path.join(ctx.out, "v2_rule5_samples.csv")
    # the jump at 0.505 s: inside the frame from 0.5 to 0.5167 s at 60 Hz
    write_samples(sp, [(0.0, "hand", 0, 0), (0.505, "hand", 9, 0)])
    r = Run(ctx, "v2_rule5", d, args=["--sim-gain", "hand=1", "--samples", sp])
    check(r.rc == 0 and len(r.trials) == 1 and r.trials[0]["outcome"] == "done_b", "v2 rule 5: exit %d %s %s" % (r.rc, r.err[:200], r.trials[:1]))
    if r.rc == 0 and r.trials:
        st = [e for e in r.events if e["kind"] == "state"]
        fa = [int(e["frame"]) for e in st if e["text"] == "b"]
        el = float(r.trials[0]["time_elapsed_s"])
        # the hit's frame is frame 31 (onset 0.5167 s); the trial ends there
        check(abs(el - 31 / 60.0) < 0.002, "v2 rule 5: the trial ends on the hit's frame: %.6f s, b's frames %s" % (el, fa))
    # a chain on one frame: a's hit starts b, b's timed exit c, c's control
    # (already true at its arm, entry now) d, d's timed exit e, e's control
    # the end. d's after_s ends past c's arm hit, so the frame's timed exits
    # (not its hits) take it; e's arm hit then needs the frame's second
    # round of hits (rule 5), else the trial ends a frame late
    ctl = {"away": t["controls"]["away"],
           "far": {"control": "region", "of": "hand", "radius": 0.5, "outside": True},
           "far2": {"control": "region", "of": "hand", "radius": 0.25, "outside": True}}
    t2 = {"controls": ctl,
          "states": [{"name": "a", "steps": [{"on": "s"}], "exits": [{"on": "away", "to": "b"}]},
                     {"name": "b", "exits": [{"after_s": 0.001, "to": "c"}]},
                     {"name": "c", "exits": [{"on": "far", "to": "d"}]},
                     {"name": "d", "exits": [{"after_s": 0.002, "to": "e"}]},
                     {"name": "e", "exits": [{"on": "far2", "to": "end", "outcome": "done_e"}]}]}
    d2 = v2base({"t": t2}, [{"name": "l", "trial": "t"}], roles=roles)
    r2 = Run(ctx, "v2_rule5b", d2, args=["--sim-gain", "hand=1", "--samples", sp])
    check(r2.rc == 0 and len(r2.trials) == 1 and r2.trials[0]["outcome"] == "done_e", "v2 rule 5b: exit %d %s %s" % (r2.rc, r2.err[:200], r2.trials[:1]))
    if r2.rc == 0 and r2.trials:
        el = float(r2.trials[0]["time_elapsed_s"])
        names = [e["text"] for e in r2.events if e["kind"] == "state"]
        check(abs(el - 31 / 60.0) < 0.002 and names == ["a", "b", "c", "d", "e"], "v2 rule 5b: the chain ends on the hit's frame: %.6f s %s" % (el, names))
    r = Run(ctx, "v2_profile", d, args=["--sim-gain", "hand=1", "--samples", sp, "--profile"])
    prof = os.path.join(r.data, "profile.csv")
    if r.rc == 2:
        check("YSP_TRACE_RING" in r.err and not os.path.exists(r.data), "--profile without a trace ring: %s" % r.err[:200])
    else:
        ok = r.rc == 0 and os.path.exists(prof)
        if ok:
            with open(prof) as f:
                head = f.readline().strip()
                body = f.read()
            ok = head == "frame,zone,t_ns,dur_ns,value" and ",pl.frame," in body and ",sig.frame," in body
        check(ok, "--profile in a trace ring build: exit %d %s" % (r.rc, r.err[:200]))


def v2_states_of(r, trial=None):
    return [(e["text"], e["code"]) for e in r.events if e["kind"] == "state" and (trial is None or e["trial_index"] == str(trial))]


def t_v2_review2(ctx):
    # the second review's items, each from its reproducer
    # (scratchpad p2rev/tools/t*.py); the item number is in each message
    dev = {"devices": {"r": {"direction": "in", "family": ["keyboard"], "required": True}}}
    hand = {"hand": {"type": "position2d_relative", "units": "display_px"}}
    far = {"control": "region", "of": "hand", "radius": 5, "outside": True}
    # 1 (F34): response_ends_trial's implicit exit does not make a state
    # one with exits: fix goes on to stim at its steps' end
    for rete in (None, False):
        resp = {"role": "r", "choices": ["left", "right"], "from": "s", "duration_s": 1}
        if rete is not None:
            resp["response_ends_trial"] = rete
        t = {"states": [{"name": "fix", "steps": [{"on": "t"}, {"wait_s": 0.5}, {"off": "t"}]}, {"name": "stim", "steps": [{"on": "s"}, {"wait_s": 1}]}],
             "response": resp}
        r = Run(ctx, "rv1_fix_stim_%s" % rete, v2base({"t": t}, [{"name": "l", "trial": "t", "rules": ["reps 2"]}], extra=dev),
                args=["--participant", "silent", "--max-seconds", "30"])
        check(r.rc == 0 and len(r.trials) == 2 and abs(float(r.trials[0]["time_elapsed_s"]) - 1.5) < 0.002,
              "1: a state with only the response's exit goes on (response_ends_trial %s): exit %d %s %s" % (rete, r.rc, r.err[:200], r.trials[:1]))
    # the response's from in a later state
    t = {"states": [{"name": "a", "steps": [{"on": "t"}, {"wait_s": 0.5}]}, {"name": "b", "steps": [{"on": "s"}, {"wait_s": 2}]}],
         "response": {"role": "r", "choices": ["left", "right"], "from": "s", "duration_s": 1.5, "key_answer": "left"},
         "columns": {"rt2_s": {"time": "response", "from": "s"}}}
    r = Run(ctx, "rv1_from_late", v2base({"t": t}, [{"name": "l", "trial": "t", "rules": ["reps 2"]}], extra=dev), args=["--rt", "0.3"])
    check(r.rc == 0 and len(r.trials) == 2 and r.trials[0]["outcome"] == "correct" and abs(float(r.trials[0]["time_elapsed_s"]) - 0.8) < 0.002
          and abs(float(r.trials[0]["rt2_s"]) - 0.3) < 1e-6, "1: the response from a later state ends it: exit %d %s %s" % (r.rc, r.err[:200], r.trials[:1]))
    # an exit on the response to b, and b with no exits of its own ends at its steps
    t = {"states": [{"name": "a", "steps": [{"on": "s"}], "exits": [{"on": "response", "to": "b"}, {"after_s": 2, "to": "end", "outcome": "slow"}]},
                    {"name": "b", "steps": [{"wait_s": 0.1}]}],
         "response": {"role": "r", "choices": ["left", "right"], "from": "s", "duration_s": 1}}
    r = Run(ctx, "rv1_resp_to_b", v2base({"t": t}, [{"name": "l", "trial": "t", "rules": ["reps 2"]}], extra=dev), args=["--rt", "0.3", "--max-seconds", "20"])
    check(r.rc == 0 and len(r.trials) == 2 and abs(float(r.trials[0]["time_elapsed_s"]) - 0.4) < 0.002 and v2_states_of(r, 0) == [("a", "0"), ("b", "-1")],
          "1: b after the response's exit ends at its steps: exit %d %s %s %s" % (r.rc, r.err[:200], r.trials[:1], v2_states_of(r, 0)))
    # 2: a column or a when test on a role no control reads
    smp = os.path.join(ctx.out, "rv2_samples.csv")
    write_samples(smp, [(0.0, "hand", 3, 4), (0.0, "pen", 30, 40), (0.05, "hand", 1, 1)])
    roles2 = {"hand": hand["hand"], "pen": {"type": "position2d", "units": "display_px"}}
    t = {"controls": {"pd": {"control": "distance", "of": "pen"}}, "states": [{"name": "a", "steps": [{"on": "s"}, {"wait_s": 0.1}]}],
         "columns": {"hx": {"signal": "hand.x"}, "hy": {"signal": "hand.y"}, "pdv": {"signal": "pd"},
                     "w": {"when": {"signal": "hand.x", "above": 2}, "then": 1, "else": 0}}}
    r = Run(ctx, "rv2_col_role", v2base({"t": t}, [{"name": "l", "trial": "t"}], roles=roles2), args=["--sim-gain", "hand=1", "--samples", smp])
    check(r.rc == 0 and r.trials and (r.trials[0]["hx"], r.trials[0]["hy"], r.trials[0]["pdv"], r.trials[0]["w"]) == ("4", "5", "50", "1"),
          "2: a role's column is its integrated value: exit %d %s %s" % (r.rc, r.err[:200], r.trials[:1]))
    t = {"states": [{"name": "a", "steps": [{"on": "s"}, {"wait_s": 0.1}]}], "columns": {"hx": {"signal": "hand.x"}}}
    r = Run(ctx, "rv2_col_only", v2base({"t": t}, [{"name": "l", "trial": "t"}], roles=roles2), args=["--sim-gain", "hand=1", "--samples", smp])
    check(r.rc == 0 and r.trials and r.trials[0]["hx"] == "4", "2: a column with no control at all: exit %d %s %s" % (r.rc, r.err[:200], r.trials[:1]))
    # 3: an events template keeps `after` (2.5), with and without until
    smp = os.path.join(ctx.out, "rv3_samples.csv")
    write_samples(smp, [(0.0, "hand", 0, 0), (0.2, "hand", 9, 0), (2.5, "hand", -9, 0), (2.7, "hand", 9, 0)])
    for nm, ev in (("until", [{"on": "t"}, {"until": "far", "timeout_s": 3}, {"off": "t"}, {"on": "s"}, {"wait_s": 1}]),
                   ("nountil", [{"on": "t"}, {"wait_s": 0.2}, {"off": "t"}, {"on": "s"}, {"wait_s": 1}])):
        t = {"controls": {"far": far}, "events": ev, "after": [{"mark": "fb"}, {"on": "t"}, {"wait_s": 0.25}],
             "response": {"role": "r", "choices": ["left", "right"], "from": "s", "duration_s": 1, "key_answer": "left"}}
        x = dict(dev)
        r = Run(ctx, "rv3_after_" + nm, v2base({"t": t}, [{"name": "l", "trial": "t", "rules": ["reps 2"]}], roles=hand, extra=x),
                args=["--rt", "0.3", "--sim-gain", "hand=1", "--samples", smp, "--max-seconds", "30"])
        marks = [e for e in r.events if e["kind"] == "mark" and e["text"] == "fb"]
        check(r.rc == 0 and len(r.trials) == 2 and len(marks) == 2 and abs(float(r.trials[0]["time_elapsed_s"]) - 1.45) < 0.002,
              "3: after runs after the events and the response (%s): exit %d %s %s" % (nm, r.rc, r.err[:200], [(y["outcome"], y["time_elapsed_s"]) for y in r.trials]))
        if nm == "nountil":
            # a response after the events' end: after starts at the response
            t["response"]["duration_s"] = 2
            r = Run(ctx, "rv3_after_late", v2base({"t": t}, [{"name": "l", "trial": "t"}], roles=hand, extra=x),
                    args=["--rt", "1.5", "--sim-gain", "hand=1", "--samples", smp, "--max-seconds", "30"])
            check(r.rc == 0 and len(r.trials) == 1 and abs(float(r.trials[0]["time_elapsed_s"]) - 1.95) < 0.002,
                  "3: after from a response later than the events: exit %d %s %s" % (r.rc, r.err[:200], [(y["outcome"], y["time_elapsed_s"]) for y in r.trials]))
    # 4 (F3): a state's after_s counts from its entry through an until
    smp = os.path.join(ctx.out, "rv4_samples.csv")
    write_samples(smp, [(0.0, "hand", 0, 0), (0.2, "hand", 9, 0)])
    t = {"controls": {"far": far}, "states": [{"name": "a", "steps": [{"on": "s"}, {"until": "far", "timeout_s": 5}, {"wait_s": 1.0}, {"off": "s"}],
                                              "exits": [{"after_s": 0.5, "to": "end", "outcome": "a_after"}]}]}
    r = Run(ctx, "rv4_until_after", v2base({"t": t}, [{"name": "l", "trial": "t"}], roles=hand), args=["--sim-gain", "hand=1", "--samples", smp])
    check(r.rc == 0 and r.trials and r.trials[0]["outcome"] == "a_after" and abs(float(r.trials[0]["time_elapsed_s"]) - 0.5) < 0.002,
          "4: after_s from the state's entry, not the until part's: %s %s" % (r.err[:200], r.trials[:1]))
    # 5 and 9: a recenter as a later state's first step arms its exits at
    # the call; a head recenter runs again on a later visit
    smp = os.path.join(ctx.out, "rv5_samples.csv")
    write_samples(smp, [(0.0, "hand", 0, 0), (0.1, "hand", 9, 0)])
    ctl = {"far": far, "hx": {"control": "distance", "of": "hand"}}
    t = {"controls": ctl, "states": [{"name": "a", "steps": [{"on": "s"}, {"wait_s": 0.2}]},
                                     {"name": "b", "steps": [{"recenter": "hand", "to": [0, 0]}],
                                      "exits": [{"on": "far", "to": "end", "outcome": "far_hit"}, {"after_s": 0.5, "to": "end", "outcome": "timeout_b"}]}],
         "columns": {"hx_end": {"signal": "hx"}}}
    r = Run(ctx, "rv5_recenter_later", v2base({"t": t}, [{"name": "l", "trial": "t"}], roles=hand), args=["--sim-gain", "hand=1", "--samples", smp])
    check(r.rc == 0 and r.trials and r.trials[0]["outcome"] == "timeout_b" and r.trials[0]["hx_end"] == "0",
          "5: the recenter first, then the exits arm: %s %s" % (r.err[:200], r.trials[:1]))
    t = {"controls": ctl, "states": [{"name": "a", "steps": [{"recenter": "hand", "to": [0, 0]}, {"on": "s"}],
                                      "exits": [{"on": "far", "to": "b"}, {"after_s": 1, "to": "end", "outcome": "no_far"}],
                                      "max_visits": 2, "over": {"to": "end", "outcome": "over"}},
                                     {"name": "b", "exits": [{"after_s": 0.1, "to": "a"}]}],
         "columns": {"hx_end": {"signal": "hx"}}}
    r = Run(ctx, "rv9_recenter_revisit", v2base({"t": t}, [{"name": "l", "trial": "t"}], roles=hand), args=["--sim-gain", "hand=1", "--samples", smp])
    check(r.rc == 0 and r.trials and r.trials[0]["outcome"] == "no_far" and r.trials[0]["hx_end"] == "0" and v2_states_of(r, 0) == [("a", "0"), ("b", "0"), ("a", "1")],
          "9: a's head recenter runs on its second visit: %s %s %s" % (r.err[:200], r.trials[:1], v2_states_of(r, 0)))
    # 6: a flag the last step sets is seen by the next state's instant if
    for nm, steps in (("nowait", [{"on": "s"}, {"flag": "f", "value": True}]), ("end", [{"on": "s"}, {"wait_s": 0.1}, {"flag": "f", "value": True}])):
        t = {"flags": ["f"], "states": [{"name": "a", "steps": steps},
                                        {"name": "b", "exits": [{"if": "$f", "to": "end", "outcome": "flag_seen"}, {"to": "end", "outcome": "flag_not_seen"}]}]}
        r = Run(ctx, "rv6_flag_" + nm, v2base({"t": t}, [{"name": "l", "trial": "t"}]))
        check(r.rc == 0 and r.trials and r.trials[0]["outcome"] == "flag_seen", "6: the flag at a's end (%s): %s %s" % (nm, r.err[:200], r.trials[:1]))
    # 7: a stimulus a hit's state shows: its frames from its landing frame
    smp = os.path.join(ctx.out, "rv7_samples.csv")
    write_samples(smp, [(0.0, "hand", 0, 0), (0.105, "hand", 9, 0)])
    # the hold ends between samples: the frame's ysig_frame() finds the hit,
    # and b's table lands at the frame's second evaluate (rule 5)
    held = dict(far, stay_s=0.05)
    t = {"controls": {"far": held}, "states": [{"name": "a", "steps": [{"on": "s"}], "exits": [{"on": "far", "to": "b"}]},
                                              {"name": "b", "steps": [{"on": "t"}, {"wait_s": 0.2}, {"off": "t"}, {"wait_s": 0.05}]}], "record": ["t"]}
    r = Run(ctx, "rv7_frames", v2base({"t": t}, [{"name": "l", "trial": "t"}], roles=hand), args=["--sim-gain", "hand=1", "--samples", smp])
    fr = {x["onset_s"]: int(x["frame"]) for x in r.rows("frames.csv")}
    x = r.trials[0] if r.trials else {}
    want = fr.get(x.get("t_offset_s"), -99) - fr.get(x.get("t_onset_s"), 0)
    check(r.rc == 0 and x and x["t_frames"] == str(want) and want >= 11, "7: t_frames is offset minus onset in frames: %s %s want %d" % (r.err[:200], {k: x.get(k) for k in ("t_onset_s", "t_offset_s", "t_frames")}, want))
    # 8: more than 16 time columns
    cols = {"c%02d_s" % k: {"time": "b", "from": "a"} for k in range(18)}
    t = {"states": [{"name": "a", "steps": [{"on": "s"}, {"wait_s": 0.1}]}, {"name": "b", "steps": [{"wait_s": 0.1}]}], "columns": cols}
    r = Run(ctx, "rv8_timecols", v2base({"t": t}, [{"name": "l", "trial": "t"}]))
    check(r.rc == 0 and r.trials and all(abs(float(r.trials[0][k] or "nan") - 0.1) < 1e-6 for k in cols), "8: 18 time columns: %s %s" % (r.err[:200], r.trials[:1]))
    # 11 (rule 8 over the parts): a tween of an earlier until part goes with the exit
    smp = os.path.join(ctx.out, "rv11_samples.csv")
    write_samples(smp, [(0.0, "hand", 3, 0), (0.1, "hand", 6, 0), (0.15, "hand", -9, 0)])
    near = {"control": "region", "of": "hand", "radius": 2}
    t = {"controls": {"far": far, "near": near},
         "states": [{"name": "a", "steps": [{"on": "s"}, {"to": "s", "props": {"x": 100}, "duration_s": 1}, {"until": "far", "timeout_s": 5}, {"wait_s": 2}],
                     "exits": [{"on": "near", "to": "b"}]},
                    {"name": "b", "steps": [{"wait_s": 0.5}]}]}
    r = Run(ctx, "rv11_rule8", v2base({"t": t}, [{"name": "l", "trial": "t"}], roles=hand), args=["--sim-gain", "hand=1", "--samples", smp, "--trace-channels", "tr.csv"])
    xs = []
    tp = os.path.join(r.dir, "tr.csv")
    if os.path.exists(tp):
        with open(tp) as f:
            for row in csv.DictReader(f):
                if row["stimulus"] == "s" and row["prop"] == "x":
                    xs.append(float(row["value"]))
    check(r.rc == 0 and xs and 5 < max(xs) < 30 and xs[-1] == xs[-2] == max(xs), "11: the tween stops at the exit from a later part: max %s last %s %s" % (max(xs) if xs else None, xs[-3:], r.err[:200]))
    # 12 and 13: records later than planned and frames late: time columns
    # and frame trace rows take the records', with the late flag
    smp = os.path.join(ctx.out, "rv12_samples.csv")
    write_samples(smp, [(0.0, "hand", 0, 0), (0.105, "hand", 9, 0), (1.0, "hand", -9, 0)])
    t = {"controls": {"far": far, "fr": {"control": "trace", "of": ["hand"], "rate": "frame"}},
         "states": [{"name": "a", "steps": [{"on": "s"}], "exits": [{"on": "far", "to": "b"}]}, {"name": "b", "steps": [{"on": "t"}, {"wait_s": 0.2}]}],
         "columns": {"tb_s": {"time": "b", "from": "far"}, "tt_s": {"time": "t", "from": "far"}}}
    d = v2base({"t": t}, [{"name": "l", "trial": "t", "rules": ["reps 2"]}], roles=hand)
    base_args = ["--sim-gain", "hand=1", "--samples", smp]
    r0 = Run(ctx, "rv12_plain", d, args=base_args)
    r1 = Run(ctx, "rv12_late", d, args=base_args + ["--test-record-late", "0.004"])
    ok = r0.rc == 0 and r1.rc == 0 and r0.trials and r1.trials
    if ok:
        for k in ("tb_s", "tt_s"):
            ok = ok and abs(float(r1.trials[0][k]) - float(r0.trials[0][k]) - 0.004) < 1e-6
    check(ok, "12: a time column takes the flip record's onset: %s %s / %s" % (r1.err[:200], r0.trials[:1], r1.trials[:1]))
    r2 = Run(ctx, "rv13_delay", d, args=base_args + ["--test-record-delay", "3"])
    rows = r2.read(os.path.join("streams", "fr.csv")) if r2.rc == 0 else []
    rows0 = r0.read(os.path.join("streams", "fr.csv")) if r0.rc == 0 else []
    check(r2.rc == 0 and rows and len(rows) == len(rows0) and all(x["t_s"] != "" for x in rows) and any(x["flags"] == "2" for x in rows)
          and [x["t_s"] for x in rows] == [x["t_s"] for x in rows0],
          "13: frame rows wait for their records past the next trial's start: %s %d/%d %s" % (r2.err[:200], len(rows), len(rows0), sorted(set(x["flags"] for x in rows))))
    # 14: the samples file and --sim-gain refuse what they cannot use, by name
    t = {"controls": {"far": far}, "states": [{"name": "a", "steps": [{"on": "s"}], "exits": [{"on": "far", "to": "end"}, {"after_s": 1, "to": "end"}]}]}
    d = v2base({"t": t}, [{"name": "l", "trial": "t"}], roles=hand)
    for nm, txt in (("bigt", "t_s,role,x,y\n0,hand,0,0\n9300000000,hand,9,0\n"), ("hugeval", "t_s,role,x,y\n0.1,hand,1e39,0\n"),
                    ("inf", "t_s,role,x,y\n0,hand,inf,nan\n"), ("big1", "t_s,role,x,y\n1e300,hand,9,0\n")):
        sp = os.path.join(ctx.out, "rv14_%s.csv" % nm)
        with open(sp, "w", newline="\n") as f:
            f.write(txt)
        r = Run(ctx, "rv14_" + nm, d, args=["--sim-gain", "hand=1", "--samples", sp])
        check(r.rc == 2 and "t_s must be 0 to" in r.err, "14: samples %s refused by name: exit %d %s" % (nm, r.rc, r.err[:200]))
    r = Run(ctx, "rv14_gain_name", d, args=["--sim-gain", "hand=1", "--sim-gain", "nosuch=1"])
    check(r.rc == 2 and "no signal role 'nosuch'" in r.err, "14: a --sim-gain for no role: exit %d %s" % (r.rc, r.err[:200]))
    r = Run(ctx, "rv14_v1_samples", base({"t": tmpl()}, [{"name": "l", "trial": "t"}]), args=["--samples", smp])
    check(r.rc == 2 and "no signal role" in r.err, "14: --samples for a definition with no signal role: exit %d %s" % (r.rc, r.err[:200]))
    # 10: outcome "correct" with no key_answer: the choice's index, as
    # version 1 (mut_test.c's case_v2 reads ytr_update()'s value; here the
    # run must end)
    t = {"states": [{"name": "a", "steps": [{"on": "s"}, {"wait_s": 1}]}], "outcome": "correct",
         "response": {"role": "r", "choices": ["left", "right"], "from": "s", "duration_s": 1}}
    r = Run(ctx, "rv10_outcome", v2base({"t": t}, [{"name": "l", "trial": "t"}], extra=dev), args=["--rt", "0.3"])
    check(r.rc == 0 and r.trials, "10: outcome correct with no key_answer runs: %s" % r.err[:200])


def trace_values(r, key):
    """--trace-channels tr.csv: frame -> value of stimulus.prop key."""
    out = {}
    tp = os.path.join(r.dir, "tr.csv")
    if os.path.exists(tp):
        with open(tp) as f:
            for row in csv.DictReader(f):
                if row["stimulus"] + "." + row["prop"] == key:
                    out[int(row["frame"])] = float(row["value"])
    return out


def t_v2_review3(ctx):
    # the re-review's items (scratchpad p3rev/tools), letter in each message
    dev = {"devices": {"r": {"direction": "in", "family": ["keyboard"], "required": True}}}
    hand = {"hand": {"type": "position2d_relative", "units": "display_px"}}
    far = {"control": "region", "of": "hand", "radius": 5, "outside": True}
    # B: `from` as the events' last step: version 2 gives version 1's rows
    resp = {"role": "r", "choices": ["left", "right"], "from": "s", "duration_s": 1, "key_answer": "left"}
    for part in (["--rt", "0.3"], ["--participant", "silent"]):
        rows = {}
        for ver in (1, 2):
            t = {"events": [{"on": "t"}, {"wait_s": 0.2}, {"off": "t"}, {"on": "s"}], "after": [{"mark": "fb"}, {"wait_s": 0.25}], "response": resp}
            d = v2base({"t": t}, [{"name": "l", "trial": "t", "rules": ["reps 2"]}], extra=dev)
            d["version"] = ver
            r = Run(ctx, "rvB_v%d_%s" % (ver, part[-1]), d, args=part + ["--max-seconds", "30"])
            rows[ver] = [(x["outcome"], x["response"], x["rt_s"], x["time_elapsed_s"]) for x in r.trials] if r.rc == 0 else r.err[:200]
        check(rows[1] == rows[2] and isinstance(rows[1], list) and len(rows[1]) == 2,
              "B: from as the last event step, v1 and v2 rows (%s): %s / %s" % (part[-1], rows[1], rows[2]))
    # D: a later state's [recenter, until C]: C arms after the recenter lands
    smp = os.path.join(ctx.out, "rvD_samples.csv")
    write_samples(smp, [(0.0, "hand", 0, 0), (0.1, "hand", 9, 0)])
    ctl = {"far": far, "hx": {"control": "distance", "of": "hand"}}
    t = {"controls": ctl, "states": [{"name": "a", "steps": [{"on": "s"}, {"wait_s": 0.2}]},
                                     {"name": "b", "steps": [{"recenter": "hand", "to": [0, 0]}, {"until": "far", "timeout_s": 1}, {"wait_s": 0.5}]}],
         "columns": {"hx_end": {"signal": "hx"}}}
    r = Run(ctx, "rvD_recenter_until", v2base({"t": t}, [{"name": "l", "trial": "t"}], roles=hand), args=["--sim-gain", "hand=1", "--samples", smp])
    check(r.rc == 0 and r.trials and r.trials[0]["outcome"] == "timeout" and abs(float(r.trials[0]["time_elapsed_s"]) - 1.2) < 0.002,
          "D: the until after a recenter arms at the call: %s %s" % (r.err[:200], r.trials[:1]))
    # E: events due by an exit's time happen: a flag, an off
    for nm, steps, exits in (("flag_same_frame", [{"on": "s"}, {"wait_s": 0.3}, {"flag": "f", "value": True}, {"wait_s": 1}], [{"after_s": 0.305, "to": "b"}]),
                             ("flag_at_entry", [{"flag": "f", "value": True}, {"on": "s"}, {"wait_s": 1}], [{"after_s": 0.001, "to": "b"}])):
        t = {"flags": ["f"], "states": [{"name": "a", "steps": steps, "exits": exits},
                                        {"name": "b", "exits": [{"if": "$f", "to": "end", "outcome": "seen"}, {"to": "end", "outcome": "not_seen"}]}]}
        r = Run(ctx, "rvE_" + nm, v2base({"t": t}, [{"name": "l", "trial": "t"}]))
        check(r.rc == 0 and r.trials and r.trials[0]["outcome"] == "seen", "E: a flag due by the exit (%s): %s %s" % (nm, r.err[:200], r.trials[:1]))
    t = {"states": [{"name": "a", "steps": [{"on": "s"}, {"wait_s": 0.3}, {"off": "s"}, {"wait_s": 1}], "exits": [{"after_s": 0.305, "to": "b"}]},
                    {"name": "b", "steps": [{"wait_s": 0.5}]}]}
    r = Run(ctx, "rvE_off", v2base({"t": t}, [{"name": "l", "trial": "t"}]), args=["--trace-channels", "tr.csv"])
    vis = trace_values(r, "s.visible")
    check(r.rc == 0 and vis.get(17) == 1 and vis.get(18) == 0 and vis.get(40) == 0, "E: the off due by the exit lands: %s %s" % (r.err[:200], [vis.get(k) for k in (17, 18, 40)]))
    # F: an events template's part change is no exit: its tweens run on
    smp = os.path.join(ctx.out, "rvF_samples.csv")
    write_samples(smp, [(0.0, "hand", 0, 0), (0.5, "hand", 9, 0)])
    for nm, ev, after in (("until_hit", [{"on": "s"}, {"to": "s", "props": {"x": 100}, "duration_s": 1}, {"until": "far", "timeout_s": 3}, {"on": "t"}, {"wait_s": 1.5}], None),
                          ("until_timeout", [{"on": "s"}, {"to": "s", "props": {"x": 100}, "duration_s": 2}, {"until": "far", "timeout_s": 0.3}, {"on": "t"}], [{"wait_s": 1}])):
        t = {"controls": {"far": far}, "events": ev}
        if after:
            t["after"] = after
        r = Run(ctx, "rvF_" + nm, v2base({"t": t}, [{"name": "l", "trial": "t"}], roles=hand), args=["--sim-gain", "hand=1", "--samples", smp, "--trace-channels", "tr.csv"])
        xs = trace_values(r, "s.x")
        check(r.rc == 0 and xs.get(60, 0) >= 49.9, "F: the tween runs on through the events' parts (%s): %s f60 %s" % (nm, r.err[:200], xs.get(60)))
    # G: frame traces' waiting rows sized from the definition
    many = {"d": {"control": "distance", "of": "hand"}}
    for k in range(9):
        many["tr%d" % k] = {"control": "trace", "of": ["d"], "rate": "frame"}
    smp = os.path.join(ctx.out, "rvG_samples.csv")
    write_samples(smp, [(0.01 * k, "hand", 1, 0) for k in range(100)])
    t = {"controls": many, "states": [{"name": "a", "steps": [{"on": "s"}, {"wait_s": 0.2}]}]}
    r = Run(ctx, "rvG_traces", v2base({"t": t}, [{"name": "l", "trial": "t", "rules": ["reps 2"]}], roles=hand),
            args=["--sim-gain", "hand=1", "--samples", smp, "--test-record-delay", "8"])
    check(r.rc == 0 and len(r.trials) == 2, "G: 9 frame traces, records 8 frames late: exit %d %s" % (r.rc, r.err[:200]))
    many = {"d": {"control": "distance", "of": "hand"}}
    for k in range(17):
        many["tr%d" % k] = {"control": "trace", "of": ["d"], "rate": "frame"}
    t = {"controls": many, "states": [{"name": "a", "steps": [{"on": "s"}, {"wait_s": 0.2}]}]}
    r = Run(ctx, "rvG_too_many", v2base({"t": t}, [{"name": "l", "trial": "t"}], roles=hand), args=["--sim-gain", "hand=1"])
    check(r.rc != 0 and "17 streams" in r.err and not os.path.exists(r.data), "G: 17 trace names refused at load, before the data set: exit %d %s" % (r.rc, r.err[:200]))
    # I: a set after a tween of one property refused at load; a tween after a
    # stopped one starts from the frozen value
    smp = os.path.join(ctx.out, "rvI_samples.csv")
    write_samples(smp, [(0.0, "hand", 0, 0), (0.5, "hand", 9, 0)])
    for nm, bsteps in (("set", [{"set": "s", "props": {"x": 7}}, {"wait_s": 1}]), ("tween", [{"to": "s", "props": {"x": 0}, "duration_s": 0.5}, {"wait_s": 1}])):
        t = {"controls": {"far": far}, "states": [{"name": "a", "steps": [{"on": "s"}, {"to": "s", "props": {"x": 100}, "duration_s": 1}, {"wait_s": 3}],
                                                  "exits": [{"on": "far", "to": "b"}]}, {"name": "b", "steps": bsteps}]}
        r = Run(ctx, "rvI_" + nm, v2base({"t": t}, [{"name": "l", "trial": "t"}], roles=hand), args=["--sim-gain", "hand=1", "--samples", smp, "--trace-channels", "tr.csv"])
        if nm == "set":
            check(r.rc != 0 and "is tweened (to, then) in one step list and set in another" in r.err, "I: a set after a tween refused at load: exit %d %s" % (r.rc, r.err[:200]))
        else:
            xs = trace_values(r, "s.x")
            check(r.rc == 0 and xs.get(29) is not None and xs.get(30) == xs.get(29) and xs.get(31, 99) < xs.get(29),
                  "I: the next tween starts from the frozen value: %s %s" % (r.err[:200], [xs.get(k) for k in (29, 30, 31)]))


def t_v2_f34(ctx):
    # F34, decided 2026-11-12 ("wait like version 1"): a press or the
    # deadline takes the response exits; states that reach end wait for an
    # open window; a from never shown opens none (p3rev/tools/v1r.py)
    dev = {"devices": {"r": {"direction": "in", "family": ["keyboard"], "required": True}}}
    resp = {"role": "r", "choices": ["left", "right"], "from": "s", "duration_s": 1, "key_answer": "left"}
    cases = (
        ("own_resp_exit", {"states": [{"name": "a", "steps": [{"on": "s"}], "exits": [{"on": "response", "to": "end"}]}], "response": resp},
         ("timeout", 1.0), ("correct", 0.5)),
        ("shared_resp_exit", {"states": [{"name": "a", "steps": [{"on": "s"}, {"wait_s": 0.2}]}, {"name": "b", "steps": [{"wait_s": 0.2}]}],
                              "exits": [{"on": "response", "to": "end", "in": ["a", "b"]}], "response": dict(resp, response_ends_trial=False)},
         ("timeout", 1.0), ("correct", 1.0)),
        ("window_open_at_end", {"states": [{"name": "a", "steps": [{"on": "s"}, {"wait_s": 0.2}]}], "response": dict(resp, duration_s=2)},
         ("timeout", 2.0), ("correct", 0.5)),
        ("states_outlast", {"states": [{"name": "a", "steps": [{"on": "s"}, {"wait_s": 1.5}]}], "response": dict(resp, duration_s=0.8, response_ends_trial=False)},
         ("timeout", 1.5), ("correct", 1.5)),
        ("deadline_exit_outcome", {"states": [{"name": "a", "steps": [{"on": "s"}], "exits": [{"on": "response", "to": "b", "outcome": "late_or_not"}]},
                                              {"name": "b", "steps": [{"wait_s": 0.1}]}], "response": resp},
         ("late_or_not", 1.1), ("late_or_not", 0.6)),
        ("deadline_latest_wins", {"states": [{"name": "a", "steps": [{"on": "s"}], "exits": [{"after_s": 0.1, "to": "b", "outcome": "early"}]},
                                             {"name": "b", "exits": [{"on": "response", "to": "end"}]}], "response": resp},
         ("timeout", 1.0), ("early", 0.5)),   # a press sets none: the earlier outcome stays (D36)
        ("from_never_shown", {"states": [{"name": "a", "steps": [{"on": "t"}], "exits": [{"after_s": 0.3, "to": "end"}, {"after_s": 5, "to": "b"}]},
                                         {"name": "b", "steps": [{"on": "s"}, {"wait_s": 1}]}], "response": dict(resp, duration_s=2)},
         ("timeout", 0.3), ("timeout", 0.3)),
    )
    for nm, t, silent, pressed in cases:
        d = v2base({"t": t}, [{"name": "l", "trial": "t", "rules": ["reps 2"]}], extra=dev)
        for part, want in ((["--participant", "silent"], silent), (["--rt", "0.5"], pressed)):
            r = Run(ctx, "f34_%s_%s" % (nm, part[-1]), d, args=part + ["--max-seconds", "20"])
            got = [(x["outcome"], float(x["time_elapsed_s"])) for x in r.trials[:1]] if r.rc == 0 else r.err[:200]
            check(r.rc == 0 and len(r.trials) == 2 and r.trials[0]["outcome"] == want[0] and abs(float(r.trials[0]["time_elapsed_s"]) - want[1]) < 0.002,
                  "F34 %s (%s): want %s, got %s" % (nm, part[-1], want, got))


def t_v2_review4(ctx):
    # the third re-review (p4rev/tools/r*.py), item in each message
    dev = {"devices": {"r": {"direction": "in", "family": ["keyboard"], "required": True}}}
    hand = {"hand": {"type": "position2d_relative", "units": "display_px"}}
    far = {"control": "region", "of": "hand", "radius": 5, "outside": True}
    resp = {"role": "r", "choices": ["left", "right"], "from": "s", "duration_s": 1, "key_answer": "left"}

    def one(nm, d, args, want_rows=None, rc=0):
        r = Run(ctx, nm, d, args=args + ["--max-seconds", "20"])
        got = [(x["outcome"], round(float(x["time_elapsed_s"]), 3)) for x in r.trials] if r.rc == 0 else r.err[:200]
        if want_rows is not None:
            check(r.rc == rc and got == want_rows, "%s: want %s, got exit %d %s" % (nm, want_rows, r.rc, got))
        return r
    # 1: a signal choice's window: duration_s and minimum_valid_rt_s apply,
    # and the trial ends with the window
    smp = os.path.join(ctx.out, "rv4_1.csv")
    rows = [(0.001 * k, "hand", 0, 0) for k in range(400)] + [(0.4 + 0.001 * k, "hand", 2, 0) for k in range(1, 400)]
    rows += [(1.0, "hand", -798, 0)] + [(1.0 + 0.001 * k, "hand", 0, 0) for k in range(1, 3000)]
    write_samples(smp, rows)
    still = os.path.join(ctx.out, "rv4_1still.csv")
    write_samples(still, [(0.001 * k, "hand", 0, 0) for k in range(3000)])
    for dur, ends, src, want in ((0.1, None, smp, [("timeout", 0.433)]), (1, None, smp, [("responded", 1.2)]), (1, True, smp, [("responded", 0.433)]),
                                 (1, False, still, [("slow", 1.2)]), (1, True, still, [("slow", 1.2)])):
        ctl = {"home": {"control": "region", "of": "hand", "radius": 1, "stay_s": 0.2}, "out": {"control": "region", "of": "hand", "radius": 50, "outside": True},
               "go": {"control": "movement", "of": "hand", "onset": 50, "from": "home"}}
        rsp = {"choices": [{"signal": "go"}], "from": "t", "duration_s": dur}
        if ends is not None:
            rsp["response_ends_trial"] = ends
        reach_exits = [{"on": "out", "to": "end"}] if src == smp else [{"after_s": 0.5, "to": "end", "outcome": "slow"}]
        t = {"controls": ctl, "states": [{"name": "hold", "steps": [{"on": "s"}], "exits": [{"on": "home", "to": "reach"}]},
                                         {"name": "reach", "steps": [{"on": "t"}], "exits": reach_exits}], "response": rsp}
        one("rv4_1_%s_%s_%s" % (dur, ends, "still" if src == still else "moves"), v2base({"t": t}, [{"name": "l", "trial": "t"}], roles=hand),
            ["--sim-gain", "hand=1", "--samples", src], want)
    # 2: persist with max_responses and no deadline waits for the press, as v1
    M = dict(resp, response_ends_trial=False, duration_s=0, max_responses=1)
    for ver, t in ((2, {"states": [{"name": "a", "steps": [{"on": "s"}, {"wait_s": 0.1}]}], "response": M}), (1, {"events": [{"on": "s"}, {"wait_s": 0.1}], "response": M})):
        d = v2base({"t": t}, [{"name": "l", "trial": "t", "rules": ["reps 2"]}], extra=dev)
        d["version"] = ver
        one("rv4_2_maxr_v%d" % ver, d, ["--rt", "0.3"], [("correct", 0.3), ("correct", 0.617)])
    # 3: persist, deadline, a press: the response exit at the window's end
    P1 = dict(resp, response_ends_trial=False)
    t = {"states": [{"name": "a", "steps": [{"on": "s"}, {"wait_s": 3}], "exits": [{"on": "response", "to": "b"}]}, {"name": "b", "steps": [{"wait_s": 0.5}]}], "response": P1}
    r = one("rv4_3_persist_exit", v2base({"t": t}, [{"name": "l", "trial": "t", "rules": ["reps 2"]}], extra=dev), ["--rt", "0.3"], [("correct", 1.5), ("correct", 3.017)])
    st = [(e["text"], round(float(e["t_s"]) - float(r.events[0]["t_s"]), 3)) for e in r.events if e["kind"] == "state" and e["trial_index"] == "0"] if r.rc == 0 else []
    check(st == [("a", 0.0), ("b", 1.0)], "3: b starts at the window's end: %s" % st)
    # 5 (F38): at the exit's time the exit wins; events before it land
    for nm, wait, want in (("tie", 0.3, 0.0), ("after", 0.301, 0.0), ("before", 0.299, 1.0)):
        t = {"states": [{"name": "a", "steps": [{"wait_s": wait}, {"on": "t"}, {"wait_s": 1}], "exits": [{"after_s": 0.3, "to": "b"}]}, {"name": "b", "steps": [{"wait_s": 0.5}]}]}
        r = Run(ctx, "rv4_5_" + nm, v2base({"t": t}, [{"name": "l", "trial": "t"}]), args=["--trace-channels", "tr.csv"])
        vis = trace_values(r, "t.visible")
        check(r.rc == 0 and vis.get(30) == want, "5: an on at %.3f against an exit at 0.3 (F38): frame 30 t.visible %s, want %s" % (wait, vis.get(30), want))
    # 6: a variable stay_s of 0 is entry now
    smp6 = os.path.join(ctx.out, "rv4_6.csv")
    write_samples(smp6, [(0.0, "hand", 9, 0)])
    for hold, want in (("$z_s", 0.2), ("$h_s", 0.25)):
        t = {"controls": {"far": dict(far, stay_s=hold)}, "states": [{"name": "a", "steps": [{"on": "s"}, {"wait_s": 0.2}, {"until": "far", "timeout_s": 1}]}]}
        d = v2base({"t": t}, [{"name": "l", "trial": "t", "vars": {"z_s": 0, "h_s": 0.05}}], roles=hand)
        one("rv4_6_" + hold[1:], d, ["--sim-gain", "hand=1", "--samples", smp6], [("done", want)])
    # 7 (F26): a variable stay_s that can be 0 in a cycle refused at load; a
    # session's 0 refused by the player; a signal_lost cycle refused at load
    cyc = lambda hold: {"controls": {"far": dict(far, stay_s=hold)}, "exits": [{"after_s": 3, "to": "end", "in": ["choose"]}],
                        "states": [{"name": "show", "steps": [{"on": "s"}, {"until": "far"}]}, {"name": "choose", "exits": [{"if": "$practice", "to": "show"}]}]}
    d = v2base({"t": cyc("$hold_s")}, [{"name": "l", "trial": "t", "rules": ["practice 2", "reps 2"], "vars": {"hold_s": {"choice": [0, 0.05]}}}], roles=hand)
    r = Run(ctx, "rv4_7_var", d, args=["--sim-gain", "hand=1"])
    check(r.rc != 0 and "stay_s variable can be 0" in r.err, "7: a cycle on a stay_s that can be 0 refused at load: %s" % r.err[:200])
    sess = {"fields": {"participant": {"type": "integer", "required": True}, "hold_s": {"type": "integer", "required": True}}}
    d = v2base({"t": cyc("$hold_s")}, [{"name": "l", "trial": "t", "rules": ["practice 1", "reps 1"]}], roles=hand, extra={"session": sess})
    r = Run(ctx, "rv4_7_session0", d, args=["--sim-gain", "hand=1", "--field", "hold_s=0"])
    check(r.rc == 1 and "stay_s '$hold_s' is 0 on this trial" in r.err, "7: a session's stay_s of 0 in a cycle refused by name: %s" % r.err[:200])
    d2 = v2base({"t": {"controls": {"lost": {"control": "signal_lost", "of": "hand", "after_s": 0.1}}, "exits": [{"after_s": 2, "to": "end", "in": ["b"]}],
                       "states": [{"name": "a", "exits": [{"on": "lost", "to": "b"}]}, {"name": "b", "exits": [{"if": "$practice", "to": "a"}]}]}},
                 [{"name": "l", "trial": "t", "rules": ["practice 1", "reps 1"]}], roles=hand)
    r = Run(ctx, "rv4_7_lost", d2, args=["--sim-gain", "hand=1"])
    check(r.rc != 0 and "signal_lost" in r.err and "F26" in r.err, "7: a signal_lost cycle refused at load: %s" % r.err[:200])
    # 8: more than 64 states run
    st = [{"name": "s%d" % k, "steps": [{"wait_s": 0.01}]} for k in range(65)]
    one("rv4_8_states65", v2base({"t": {"states": st}}, [{"name": "l", "trial": "t"}]), [], [("done", 0.65)])
    # 10: a response's end and an after_s exit at one time: list order
    t = {"states": [{"name": "a", "steps": [{"on": "s"}], "exits": [{"on": "response", "to": "end", "outcome": "rsp"}, {"after_s": 1.0, "to": "end", "outcome": "timed"}]}], "response": resp}
    one("rv4_10_tie_silent", v2base({"t": t}, [{"name": "l", "trial": "t"}], extra=dev), ["--participant", "silent"], [("rsp", 1.0)])
    t = {"states": [{"name": "a", "steps": [{"on": "s"}], "exits": [{"on": "response", "to": "end", "outcome": "rsp"}, {"after_s": 0.3, "to": "end", "outcome": "timed"}]}], "response": resp}
    one("rv4_10_tie_pressed", v2base({"t": t}, [{"name": "l", "trial": "t"}], extra=dev), ["--rt", "0.3"], [("rsp", 0.3)])
    t = {"states": [{"name": "a", "steps": [{"on": "s"}], "exits": [{"after_s": 1.0, "to": "b", "outcome": "timed"}]},
                    {"name": "b", "steps": [{"wait_s": 0.5}], "exits": [{"on": "response", "to": "end", "outcome": "rsp_b"}]}], "response": P1}
    one("rv4_10_after_then_rsp", v2base({"t": t}, [{"name": "l", "trial": "t"}], extra=dev), ["--participant", "silent"], [("rsp_b", 1.0)])
    # F37: a response that nothing ends refused at load, both versions
    for ver in (1, 2):
        t = {"events": [{"on": "s"}], "response": dict(resp, response_ends_trial=False, duration_s=0)}
        d = v2base({"t": t}, [{"name": "l", "trial": "t"}], extra=dev)
        d["version"] = ver
        r = Run(ctx, "rv4_f37_v%d" % ver, d)
        check(r.rc != 0 and "nothing ends this response" in r.err and "F37" in r.err, "F37 v%d: refused at load: %s" % (ver, r.err[:200]))


def t_v2_corpus(ctx):
    # the loader's valid but unusual version 2 files (tests/experiment/good)
    good = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "experiment", "good")
    rig = v2_rig(ctx, None)
    for name, extra, want in (("v2_retry", ["--max-seconds", "20"], "budget"), ("v2_forms", [], "ok"),
                              ("v2_outcomes", [], "ok"), ("v2_requeue_set", [], "ok"), ("v2_requeue_warn", [], "ok"),
                              ("v2_instant_shared", ["--samples", "pad", "--max-seconds", "20"], "budget")):
        with open(os.path.join(good, name + ".json")) as f:
            d = json.load(f)
        args = ["--rig", rig]
        for role in sorted(d.get("roles", {})):   # a gain for a role the file lacks is refused
            args += ["--sim-gain", role + "=1"]
        for k, v in enumerate(extra):
            if v == "pad":
                sp = os.path.join(ctx.out, "v2_pad_samples.csv")
                # the pad leaves the radius 0.6 s into each trial, then comes back
                rows = []
                for tr in range(12):
                    rows += [(tr * 2.0 + 0.6, "pad", 9, 0), (tr * 2.0 + 0.7, "pad", 0, 0)]
                write_samples(sp, rows)
                extra = extra[:k] + [sp] + extra[k + 1:]
        r = Run(ctx, name, d, args=args + extra)
        if want == "ok":
            check(r.rc == 0 and len(r.trials) > 0, "%s: exit %d, %d rows: %s" % (name, r.rc, len(r.trials), r.err[:300]))
        else:
            # v2_instant_shared: F26 (decided) gave its until stay_s 0.05; a
            # practice trial then cycles show and choose at each lift, and
            # the session runs to its budget rather than stopping at once
            states = [e for e in r.events if e["kind"] == "state"]
            check(r.rc == 1 and "budget" in r.err and len(states) > 4, "%s: runs until the budget: exit %d %s" % (name, r.rc, r.err[:300]))


TESTS = [t_nested, t_if_unless, t_empty_and_zero, t_loop, t_requeue, t_track_ends, t_lookup_keys, t_triggers, t_after_timing, t_outcomes,
         t_method_update, t_seeds, t_refused, t_unused_stim, t_record_data, t_fix_density, t_fix_key_reapply, t_fix_stale_rdk,
         t_fix_timeout_values, t_fix_any_key, t_fix_from_in_if, t_fix_shown_twice, t_fix_vars_data, t_fix_sound_twice, t_fix_rdk_limit,
         t_fix_record_inherit, t_fix_backward_at, t_fix_refusals, t_fix_michelson_order, t_fix_texture_vars, t_fix_walk_budget,
         t_fix_data_set, t_fix_check_all_leaves, t_fix_release_order, t_fix_key_var_reapply,
         t_r2_release_other_template, t_r2_yield_draws_nothing, t_r2_low, t_r2_relaxed, t_r4_leaf_loop_done_track, t_r4_requeue_full,
         t_r5_wait_final, t_r6_allow_below, t_v2_states, t_v2_until, t_v2_hand, t_v2_rule5, t_v2_review2, t_v2_review3, t_v2_f34, t_v2_review4, t_v2_corpus]


def main():
    global fails, cases
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--out", default=None)
    ap.add_argument("--only", default=None)
    ap.add_argument("--player", default=None, help="another player binary (the mutation test's)")
    a = ap.parse_args()
    a.bin = os.path.abspath(a.bin)

    class C:
        pass
    ctx = C()
    ctx.out = os.path.abspath(a.out or tempfile.mkdtemp(prefix="player_test"))
    os.makedirs(ctx.out, exist_ok=True)
    def find(name, dirs):
        # a multi-config build (MSVC) puts programs in Release/, with .exe
        for d in dirs:
            for sub in ("", "Release"):
                for ext in ("", ".exe"):
                    c = os.path.join(a.bin, d, sub, name + ext)
                    if os.path.isfile(c):
                        return c
        return ""
    ctx.make_rig = find("player_make_rig", ("", "player"))
    ctx.player = find("ysp_player", ("player",))
    ctx.ypak = find("ypak", ("pack",))
    for name in ("player", "ypak"):
        if not getattr(ctx, name):
            print("no %s under %s" % (name, a.bin))
            return 2
    if a.player:
        ctx.player = a.player
    for t in TESTS + [t_sound]:
        if a.only and t.__name__ != a.only:
            continue
        cases += 1
        print("%s" % t.__name__)
        t(ctx)
    print("%d cases, %d failures" % (cases, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
