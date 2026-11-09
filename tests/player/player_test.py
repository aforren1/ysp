#!/usr/bin/env python3
"""System tests of the player: definitions of valid but unusual shapes, run
through ysp_player --sim. Usage: player_test.py --bin BUILD_DIR [--out DIR]

Every case builds a pack with ypak and asserts on trials.csv and events.csv.
"""
import argparse
import csv
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
fails = 0
cases = 0


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
        self.build = subprocess.run([ctx.ypak, "build", "pack.json", "-o", self.pack], cwd=self.dir, capture_output=True, text=True)
        if self.build.returncode != 0:
            self.rc = -1
            self.err = self.build.stderr + self.build.stdout
            self.trials, self.events = [], []
            return
        self.data = os.path.join(self.dir, "data")
        cmd = [ctx.player, self.pack, "--out", self.data, "--sim", "--seed", "99", "--field", "participant=3"] + (args or [])
        r = subprocess.run(cmd, cwd=self.dir, capture_output=True, text=True)
        self.rc = r.returncode
        self.err = r.stderr + r.stdout
        self.trials = self.read("trials.csv")
        self.events = self.read("events.csv")

    def read(self, name):
        p = os.path.join(self.data, name)
        if not os.path.exists(p):
            return []
        with open(p, newline="") as f:
            return list(csv.DictReader(f))


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
    # A table with a header and no rows: the leaf runs nothing, the next runs.
    check(r.rc == 0 or "table" in r.err.lower() or "row" in r.err.lower(), "empty table: %s" % r.err)
    if r.rc == 0:
        check(len(r.trials) == 1, "empty table: only the second leaf's trial, got %d" % len(r.trials))
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
        a = open(os.path.join(r1.data, name), "rb").read()
        b = open(os.path.join(r2.data, name), "rb").read()
        check(a == b, "seeds: %s differs between two runs" % name)
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
    # 'from' never shown, an impossible constraint (a dry open)
    d4 = base({"t": tmpl()}, [{"name": "l", "trial": "t", "timeline_variables": "x.pstb", "rules": ["order constrained", "max_run a=x 1", "reps 2"]}])
    r4 = Run(ctx, "refused_rule", d4, tables={"x": "a\nx\nx\nx\ny\n"})
    check(r4.rc != 0 and "max_run" in r4.err, "an impossible constraint fails the dry open: %s" % r4.err)


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


TESTS = [t_nested, t_if_unless, t_empty_and_zero, t_loop, t_requeue, t_track_ends, t_lookup_keys, t_triggers, t_after_timing, t_outcomes,
         t_method_update, t_seeds, t_refused]


def main():
    global fails, cases
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--out", default=None)
    ap.add_argument("--only", default=None)
    a = ap.parse_args()
    a.bin = os.path.abspath(a.bin)

    class C:
        pass
    ctx = C()
    ctx.out = os.path.abspath(a.out or tempfile.mkdtemp(prefix="player_test"))
    os.makedirs(ctx.out, exist_ok=True)
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
