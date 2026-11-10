#!/usr/bin/env python3
"""Generate a corpus for tests/fuzz/player_fuzz.c: every key the format
defines in a valid definition, so token mutations start from all of them.

Usage: player_corpus.py --ypak YPAK --out DIR [--font FONT]

Writes DIR/fuzz.ysppak (tables, textures, a sound, glyph runs) and
DIR/seeds/*.json, one definition per key and per value class. The keys come
from the rows of ysp/experiment.h, so a new key is covered when it is added.
"""
import argparse
import itertools
import json
import os
import re
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

KINDS = ["shape", "grating", "gabor", "noise", "image", "text", "artwork", "rdk", "sound", "video"]
ROW = re.compile(r'\{ YEXP__S_(\w+), 0x([0-9a-f]+), YEXP__T_(\w+), ([^,]+), "([^"]+)", (NULL|"(?:[^"\\]|\\.)*"), (NULL|"[^"]*") \}')


def rows():
    text = open(os.path.join(ROOT, "include", "ysp", "experiment.h"), encoding="utf-8").read()
    out = []
    for m in ROW.finditer(text):
        sec, kinds, typ, flags, key, dflt, values = m.groups()
        dflt = None if dflt == "NULL" else dflt[1:-1].replace('\\"', '"')
        values = None if values == "NULL" else values[1:-1]
        out.append((sec, int(kinds, 16), typ, key, dflt, values))
    return out


RESP = {"role": "r", "choices": ["left", "right"], "from": "s", "duration_s": 0.3, "key_answer": "$a"}


def base(stimuli=None, trials=None, timeline=None, **kw):
    d = {"format": "ysp-experiment", "version": 1, "name": "f",
         "devices": {"r": {"direction": "in", "family": ["keyboard"], "required": True},
                     "o": {"direction": "out", "family": ["triggerbox"], "pulse_s": 0.002, "required": True},
                     "b": {"direction": "in", "family": ["xid"], "buttons": {"1": "left", "2": "right"}, "required": False}},
         "stimuli": stimuli or {"s": {"kind": "shape", "w": 2, "h": 2}, "t": {"kind": "shape", "w": 1, "h": 1, "layer": 1}},
         "trials": trials or {"x": {"events": [{"on": "s"}, {"wait_s": 0.05}], "response": dict(RESP), "after": [{"off": "s"}]}},
         "timeline": timeline or [{"name": "m", "trial": "x", "timeline_variables": "x.pstb", "rules": ["reps 1"]}]}
    d.update(kw)
    return d


def sample(typ, key, values, dflt):
    if key in ("kind",):
        return []
    if typ == "ENUM":
        return [v.split("=")[0] for v in values.split(",")]
    if typ in ("NUM",):
        return [0.5, "$v", {"by": "$a", "map": {"left": 0.25, "right": 0.75}}]
    if typ == "INT":
        return [1, "$n"]
    if typ == "DUR":
        return [0.05, "$d_s", {"by": "$a", "map": {"left": 0.1, "right": 0.2}}]
    if typ == "BOOL":
        return [True, "$flag"]
    if typ == "VEC3":
        return [[0.2, 0.3, 0.4], {"by": "$a", "map": {"left": [0, 0, 0], "right": [1, 1, 1]}}]
    if typ == "TEXT":
        return ["HELLO", "$a"]
    return []


def write(seeds, name, d):
    seeds[name] = d


def stim_seeds(seeds, all_rows):
    lit = {"shape": {}, "grating": {"sf": 1}, "gabor": {"sf": 1, "sigma": 5}, "noise": {"check": 2},
           "image": {"texture": "a.ysptex"}, "text": {"font": "fonts/F.ttf", "size": 40, "width": 1000, "text": "HELLO"},
           "rdk": {"count": 8, "w": 50, "speed": 5, "dot_size": 2, "color": [1, 1, 1]}, "sound": {"audio": "tone.wav"}, "artwork": {"artwork": "x"}, "video": {"video": "x"}}
    for i, kind in enumerate(KINDS):
        for sec, kinds, typ, key, dflt, values in all_rows:
            if sec != "STIM" or not (kinds & (1 << i)) or key == "kind":
                continue
            if kind in ("artwork", "video") and key != "x":
                continue   # refused by name; one seed of each is enough
            for j, val in enumerate(sample(typ, key, values, dflt)):
                if key in ("texture", "artwork", "audio", "video", "font"):
                    continue
                st = dict(lit[kind])
                st["kind"] = kind
                st[key] = val
                d = base(stimuli={"s": {"kind": "shape", "w": 2, "h": 2}, "st": st})
                d["trials"]["x"]["events"] = [{"on": "s"}, {"on": "st"}, {"wait_s": 0.05}, {"off": "st"}]
                write(seeds, "stim_%s_%s_%d" % (kind, key, j), d)


def method_seeds(seeds, all_rows):
    M = {"STAIR": {"type": "stair", "steps": [0.1], "stop_trials": 3, "start": 0.5},
         "QUEST": {"type": "quest", "stim": [{"linspace": [-2, 0, 5]}], "param": [{"linspace": [-2, 0, 5]}, {"linspace": [1, 3, 3]}, 0.5, 0.02], "stop_trials": 3},
         "AEP": {"type": "aep", "lo": [0], "hi": [1], "stop_trials": 3}}
    for sec, base_m in M.items():
        for r in all_rows:
            if r[0] != sec or r[3] in ("type", "steps", "stim", "param", "lo", "hi"):
                continue
            # a method is opened before any row exists: literals only
            for j, val in enumerate(sample(r[2], r[3], r[5], r[4])[:1 if r[2] != "ENUM" else 99]):
                m = dict(base_m)
                m[r[3]] = val
                d = base(methods={"m": m},
                         trials={"x": {"events": [{"on": "s"}, {"wait_s": 0.05}], "response": dict(RESP, key_answer="left"),
                                       "method_value": "$level", "after": [{"off": "s"}]}},
                         timeline=[{"name": "m", "trial": "x", "tracks": ["m"]}])
                write(seeds, "method_%s_%s_%d" % (sec.lower(), r[3], j), d)


def step_seeds(seeds):
    S = {
        "on_list": [{"on": ["s", "t"], "stagger_s": 0.02}, {"off": ["s", "t"]}],
        "at": [{"on": "s"}, {"wait_s": 0.2}, {"at_s": 0.1}, {"off": "s"}, {"wait_s": 0.2}],
        "set": [{"on": "s"}, {"set": "s", "props": {"x": 1.5}}, {"wait_s": 0.05}, {"off": "s"}],
        "to": [{"on": "s"}, {"to": "s", "props": {"x": 1.5, "opacity": 0.5}, "duration_s": 0.05, "delay_s": 0.01, "ease": "cosine"}, {"wait_s": 0.1}, {"off": "s"}],
        "then": [{"on": "s"}, {"then": "s", "props": {"y": 2}, "duration_s": 0.05, "ease": "cubic_out"}, {"off": "s"}],
        "color": [{"on": "t"}, {"set": "t", "props": {"color": [0.1, 0.2, 0.3]}}, {"off": "t"}],
        "trigger": [{"on": "s"}, {"trigger": 7, "role": "o"}, {"trigger": "$n", "role": "o"}, {"wait_s": 0.05}, {"off": "s"}],
        "mark": [{"on": "s"}, {"mark": "here"}, {"wait_s": 0.05}, {"mark": "there"}, {"off": "s"}],
        "if": [{"if": "$flag", "steps": [{"on": "s"}]}, {"unless": "$flag", "steps": [{"on": "t"}, {"off": "t"}]}, {"on": "s"}, {"off": "s"}],
        "ifpractice": [{"if": "$first_in_block", "steps": [{"on": "t"}]}, {"on": "s"}, {"wait_s": 0.02}, {"off": "s"}, {"off": "t"}],
    }
    S["wait_table"] = [{"on": "s"}, {"wait_s": "$d_s"}, {"at_s": {"by": "$a", "map": {"left": 0.05, "right": 0.1}}}, {"off": "s"}]
    for name, st in S.items():
        d = base()
        d["trials"]["x"]["events"] = st
        d["trials"]["x"]["response"] = dict(RESP)
        write(seeds, "step_" + name, d)
    resp_variants = {
        "no_choice": {"role": "r", "from": "s", "duration_s": 0.3},
        "persist": dict(RESP, response_ends_trial=False, max_responses=2),
        "min_rt": dict(RESP, minimum_valid_rt_s=0.1, duration_s="$d_s"),
        "held": dict(RESP, allow_held_key=True, wait_for_key_release=True),
        "names": dict(RESP, choices=[{"key": "left", "name": "L"}, {"key": "right", "name": "R"}]),
        "button": dict(RESP, role="b"),
        "trigger": dict(RESP, trigger={"role": "o", "code": {"by": "$response", "map": {"left": 9, "right": 10}}}),
        "no_deadline": {"role": "r", "from": "s", "choices": ["left"]},
    }
    for name, r in resp_variants.items():
        d = base()
        d["trials"]["x"]["response"] = r
        d["trials"]["x"]["after"] = [{"wait_s": 0.02}, {"off": "s"}]
        write(seeds, "resp_" + name, d)
    tt = {
        "columns": {"events": [{"on": "s"}], "response": dict(RESP), "columns": {"a1": "$a", "lvl": "$responded", "seed": "$trial_seed", "o": "$outcome"}},
        "record": {"events": [{"on": "s"}, {"on": "t"}, {"wait_s": 0.05}, {"off": "s"}, {"on": "s"}, {"wait_s": 0.05}, {"off": "s"}, {"off": "t"}], "response": dict(RESP), "record": ["s", "t"]},
        "outcome_val": {"events": [{"on": "s"}], "response": dict(RESP), "outcome": "$correct"},
        "outcome_int": {"events": [{"on": "s"}], "response": dict(RESP), "outcome": "$n"},
        "no_resp": {"events": [{"on": "s"}, {"wait_s": 0.02}, {"off": "s"}], "after": [{"on": "t"}, {"off": "t"}]},
        "lookup_response": {"events": [{"on": "s"}], "response": dict(RESP), "after": [{"wait_s": {"by": "$response", "map": {"left": 0.01, "right": 0.02, "": 0.03}}}, {"off": "s"}]},
    }
    for name, t in tt.items():
        write(seeds, "tmpl_" + name, base(trials={"x": t}))


def node_seeds(seeds):
    meths = {"st": {"type": "stair", "steps": [0.1], "stop_trials": 4, "start": 0.5}}
    tr = {"x": {"events": [{"on": "s"}, {"wait_s": 0.02}], "response": dict(RESP, key_answer="left"), "method_value": "$level", "after": [{"off": "s"}]}}
    N = {
        "nested": [{"name": "c", "timeline_variables": "x.pstb", "repetitions": 2, "data": {"tag": "$a"}, "timeline": [{"name": "l", "trial": "x", "rules": ["reps 1"], "data": {"in": 1}}]}],
        "if": [{"name": "a", "if": "$flag", "trial": "x", "timeline_variables": "x.pstb"}, {"name": "b", "unless": "$flag", "trial": "x", "timeline_variables": "x.pstb"}],
        "loop": [{"name": "a", "trial": "x", "timeline_variables": "x.pstb", "loop": {"until_accuracy": 0.5, "max": 3}}],
        "loop_c": [{"name": "c", "loop": {"until_accuracy": 1.0, "max": 2}, "timeline": [{"name": "a", "trial": "x", "timeline_variables": "x.pstb"}]}],
        "requeue": [{"name": "a", "trial": "x", "timeline_variables": "x.pstb", "requeue": "no_response"}],
        "record_off": [{"name": "c", "record_data": False, "timeline": [{"name": "a", "trial": "x", "timeline_variables": "x.pstb"}]}],
        "vars": [{"name": "a", "trial": "x", "timeline_variables": "x.pstb", "vars": {"w_v": 3, "ch": {"choice": ["p", "q"]}}}],
        "empty_c": [{"name": "c", "timeline": []}, {"name": "a", "trial": "x", "timeline_variables": "x.pstb"}],
    }
    for name, t in N.items():
        write(seeds, "node_" + name, base(timeline=t))
    write(seeds, "node_rules_all", base(timeline=[{"name": "a", "trial": "x", "timeline_variables": "x.pstb", "rules": [
        "order constrained", "reps 2", "max_run a 2", "practice 1", "warmup 1", "block_size 2", "jitter j_s uniform 0.01 0.02", "var w choice p q"]}]))
    write(seeds, "node_if_field", base(timeline=[{"name": "a", "if": "$participant", "trial": "x", "timeline_variables": "x.pstb"},
                                                 {"name": "b", "unless": "$participant", "trial": "x", "timeline_variables": "x.pstb"}]))
    write(seeds, "node_vars_dur", base(trials={"x": {"events": [{"on": "s"}, {"wait_s": "$wv_s"}], "response": dict(RESP), "after": [{"off": "s"}]}},
                                       timeline=[{"name": "m", "trial": "x", "timeline_variables": "x.pstb", "vars": {"wv_s": {"choice": [0.05, 0.1]}}}]))
    write(seeds, "node_shadow", base(timeline=[{"name": "c", "vars": {"ch": {"choice": ["p", "q"]}}, "timeline": [{"name": "a", "trial": "x", "timeline_variables": "x.pstb", "vars": {"ch": {"choice": ["r", "s"]}}}]}]))
    write(seeds, "image_lookup", base(stimuli={"s": {"kind": "image", "texture": {"by": "$a", "map": {"left": "a.ysptex", "right": "b.ysptex"}}}}))
    tc = dict(tr["x"], columns={"rt2_s": "$rt_s", "es": "$estimate", "lv": "$level", "cor": "$correct"})
    write(seeds, "track_columns", base(methods=meths, trials={"x": tc}, timeline=[{"name": "a", "trial": "x", "tracks": ["st"]}]))
    write(seeds, "node_groups", base(timeline=[{"name": "a", "trial": "x", "timeline_variables": "x.pstb", "rules": ["groups a blocked", "order random"]}]))
    write(seeds, "node_rate_display", base(timeline=[{"name": "a", "trial": "x", "timeline_variables": "x.pstb", "rules": ["jitter j_s uniform 0.01 0.05 rate=display"]}]))
    write(seeds, "track_round", base(methods=meths, trials=tr, timeline=[{"name": "a", "trial": "x", "tracks": ["st"], "interleave": "round_robin"}]))
    write(seeds, "track_table", base(methods=meths, trials={"x": dict(tr["x"], columns={"l": "$level", "m": "$method"})},
                                     timeline=[{"name": "a", "trial": "x", "tracks": ["st"], "track_rate": 0.5, "timeline_variables": "x.pstb"}]))
    write(seeds, "display", base(display={"units": "px", "background": [0.2, 0.2, 0.2], "calibration": "optional", "min_tier": 1}))
    write(seeds, "deg", base(display={"units": "deg"}))
    write(seeds, "session", base(session={"fields": {"participant": {"type": "integer", "required": True}, "extra": {"type": "string", "required": False}, "num": {"type": "integer", "required": False}}}))
    write(seeds, "web", base(web={"allowed": True, "exclusion": {"max": 1}}))
    write(seeds, "sound_from", base(stimuli={"s": {"kind": "sound", "audio": "tone.wav"}}, trials={"x": {"events": [{"on": "s"}, {"wait_s": 0.05}], "response": dict(RESP), "after": [{"off": "s"}]}}))
    write(seeds, "rdk_density", base(stimuli={"s": {"kind": "rdk", "density": 0.002, "w": 80, "h": 80, "aperture": "rect", "speed": 5, "dot_size": 2, "color": [1, 1, 1]}}))
    write(seeds, "text_var", base(stimuli={"s": {"kind": "text", "font": "fonts/F.ttf", "size": 40, "width": 1000, "text": "$a"}}))
    write(seeds, "image_var", base(stimuli={"s": {"kind": "image", "texture": "$img"}}))
    write(seeds, "image_choice", base(stimuli={"s": {"kind": "image", "texture": "$pic"}}, timeline=[{"name": "m", "trial": "x", "timeline_variables": "x.pstb", "vars": {"pic": {"choice": ["a.ysptex", "b.ysptex"]}}}]))


def make_pack(a, out):
    src = os.path.join(out, "src")
    os.makedirs(os.path.join(src, "fonts"), exist_ok=True)
    open(os.path.join(src, "x.csv"), "w", newline="\n").write("a,n,v,d_s,img,flag\nleft,1,0.1,0.1,a.ysptex,1\nright,2,0.2,0.2,b.ysptex,0\n")
    def png(p, val):
        raw = b"".join(b"\x00" + bytes([val] * 3) * 4 for _ in range(4))
        def chunk(t, d):
            c = struct.pack(">I", len(d)) + t + d
            return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
        open(p, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    png(os.path.join(src, "a.png"), 40)
    png(os.path.join(src, "b.png"), 200)
    samples = b"\x00\x00\x00\x00" * 4800
    open(os.path.join(src, "tone.wav"), "wb").write(b"RIFF" + struct.pack("<I", 36 + len(samples)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, 48000, 192000, 4, 16) + b"data" + struct.pack("<I", len(samples)) + samples)
    res = [{"name": "x.pstb", "kind": "table", "source": "x.csv"},
           {"name": "a.ysptex", "kind": "texture", "source": "a.png", "primaries": "device"},
           {"name": "b.ysptex", "kind": "texture", "source": "b.png", "primaries": "device"},
           {"name": "tone.wav", "kind": "audio", "source": "tone.wav"}]
    pack_def = base(stimuli={"s": {"kind": "shape", "w": 1, "h": 1}})
    if a.font:
        with open(a.font, "rb") as f:
            open(os.path.join(src, "fonts", "F.ttf"), "wb").write(f.read())
        res += [{"kind": "font", "source": "fonts/F.ttf", "license": "OFL-1.1"}, {"kind": "curveset", "from": "fonts/F.ttf", "glyphs": "used"}]
        pack_def = base(stimuli={"s": {"kind": "text", "font": "fonts/F.ttf", "size": 40, "width": 1000, "text": {"by": "$a", "map": {"left": "HELLO", "right": "right"}}}})
    json.dump(pack_def, open(os.path.join(src, "experiment.json"), "w", newline="\n"))
    json.dump({"format": "ysp-pack-source", "version": 1, "experiment": {"entry": "experiment/main.json", "source": "experiment.json", "media_type": "application/vnd.ysp.experiment"},
               "audio": {"rate": 48000, "channels": 2}, "resources": res}, open(os.path.join(src, "pack.json"), "w", newline="\n"))
    subprocess.check_call([a.ypak, "build", "pack.json", "-o", os.path.join(out, "fuzz.ysppak")], cwd=src)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ypak", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--font")
    a = ap.parse_args()
    a.ypak = os.path.abspath(a.ypak)
    a.out = os.path.abspath(a.out)
    if not a.font:
        for f in ("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
            if os.path.exists(f):
                a.font = f
                break
    seeds_dir = os.path.join(a.out, "seeds")
    os.makedirs(seeds_dir, exist_ok=True)
    make_pack(a, a.out)
    all_rows = rows()
    seeds = {}
    stim_seeds(seeds, all_rows)
    method_seeds(seeds, all_rows)
    step_seeds(seeds)
    node_seeds(seeds)
    for name, d in seeds.items():
        with open(os.path.join(seeds_dir, name + ".json"), "w", newline="\n") as f:
            json.dump(d, f)
    print("%d seeds, %d rows of keys" % (len(seeds), len(all_rows)))


if __name__ == "__main__":
    sys.exit(main())
