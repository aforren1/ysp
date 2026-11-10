#!/usr/bin/env python3
"""System tests of the player: definitions of valid but unusual shapes, run
through ysp_player --sim. Usage: player_test.py --bin BUILD_DIR [--out DIR]

Every case builds a pack with ypak and asserts on trials.csv and events.csv.
"""
import argparse
import csv
import itertools
import json
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


TESTS = [t_nested, t_if_unless, t_empty_and_zero, t_loop, t_requeue, t_track_ends, t_lookup_keys, t_triggers, t_after_timing, t_outcomes,
         t_method_update, t_seeds, t_refused, t_unused_stim, t_record_data, t_fix_density, t_fix_key_reapply, t_fix_stale_rdk,
         t_fix_timeout_values, t_fix_any_key, t_fix_from_in_if, t_fix_shown_twice, t_fix_vars_data, t_fix_sound_twice, t_fix_rdk_limit,
         t_fix_record_inherit, t_fix_backward_at, t_fix_refusals, t_fix_michelson_order, t_fix_texture_vars, t_fix_walk_budget,
         t_fix_data_set, t_fix_check_all_leaves, t_fix_release_order, t_fix_key_var_reapply,
         t_r2_release_other_template, t_r2_yield_draws_nothing, t_r2_low, t_r2_relaxed]


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
    ctx.make_rig = ""
    for c in (os.path.join(a.bin, "player_make_rig"), os.path.join(a.bin, "player", "player_make_rig")):
        if os.path.exists(c):
            ctx.make_rig = c
    for name, d in (("player", "player"), ("ypak", "pack")):
        ok = False
        for candidate in (os.path.join(a.bin, d, "ysp_" + name if name == "player" else name), os.path.join(a.bin, d, name)):
            if os.path.exists(candidate):
                setattr(ctx, name if name != "player" else "player", candidate)
                ok = True
                break
        if not ok:
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
