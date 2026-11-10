#!/usr/bin/env python3
"""The player in a real window, with a participant made of a uinput keyboard.

Not in ctest: it needs an X display, /dev/uinput, and the keyboard rule of
tools/linux/70-ysp-input.rules (docs/screen.md, "Kernel key times"). It
changes the display's gamma ramp for the abort test and puts it back.

    python3 tests/player/real_test.py --bin BUILD --out DIR [--only NAME]
                                      [--trials N] [--windowed]

Cases (--only takes one name):
  rdk afc2 stroop oddball   each worked example for N trials (default 30):
                            the data set has the columns of --sim; each
                            response is the key sent, its time inside the
                            write() that sent it; frames dropped and the
                            residuals are reported. oddball's trigger role
                            is a TriggerBox on a pty pair: the bytes read
                            are the codes events.csv says went out.
  abort                     Shift+Esc in the middle: exit 3, the rows of
                            the finished trials, session.json "aborted",
                            the mode and the gamma ramp back
  panic                     a frame loop that hangs (--hang-at-trial, a
                            test option), then Shift+Esc 3 times: the
                            watchdog ends the process (99), the rows of
                            the finished trials are on disk, the ramp back
  refuse                    display.min_tier in a window, a required role
                            with no device, a rig file that does not exist

The participant: a --sim run of the same seed gives each trial's key (the
correct one) and how long the trial takes before its `from` stimulus and
after the response. A real trial is then timed from the participant's own
last press, so the error of one trial does not add to the next. Every
seventh trial is silent (a timeout). A key goes out only while the
player's window has the input focus (_NET_ACTIVE_WINDOW's _NET_WM_PID).
"""
import argparse
import csv
import fcntl
import json
import os
import random
import select
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
fails = 0

# linux/input-event-codes.h
EV_SYN, EV_KEY = 0, 1
KEYS = {"left": 105, "right": 106, "r": 19, "g": 34, "b": 48, "esc": 1, "shift": 42}
# linux/uinput.h
UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_DEV_SETUP = 0x405C5503
UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502


def check(cond, msg):
    global fails
    if not cond:
        fails += 1
        print("  FAIL: %s" % msg, flush=True)
    return cond


class Keyboard:
    """A uinput keyboard with the keys the definitions name, Shift and Esc."""

    def __init__(self):
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        # udev takes a device for a keyboard (ID_INPUT_KEYBOARD, which the
        # rule's ACL needs) when it has keys 1 to 31; only the keys of KEYS
        # are ever sent
        for k in sorted(set(range(1, 32)) | set(KEYS.values())):
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, k)
        setup = struct.pack("HHHH80sI", 3, 0x1234, 0x5678, 1, b"ysp-player-test", 0)
        fcntl.ioctl(self.fd, UI_DEV_SETUP, setup)
        fcntl.ioctl(self.fd, UI_DEV_CREATE)

    def _ev(self, typ, code, value):
        return struct.pack("llHHi", 0, 0, typ, code, value)

    def key(self, name, value):
        """One key event and its SYN in one write(); returns the
        CLOCK_MONOTONIC ns just before and just after it (ysp_rt's clock)."""
        buf = self._ev(EV_KEY, KEYS[name], value) + self._ev(EV_SYN, 0, 0)
        t0 = time.monotonic_ns()
        os.write(self.fd, buf)
        t1 = time.monotonic_ns()
        return t0, t1

    def close(self):
        try:
            fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        finally:
            os.close(self.fd)


def focus_pid():
    try:
        w = subprocess.run(["xprop", "-root", "_NET_ACTIVE_WINDOW"], capture_output=True, text=True, timeout=2).stdout
        wid = w.split("#")[-1].split(",")[0].strip()
        if not wid.startswith("0x") or int(wid, 16) == 0:
            return None
        p = subprocess.run(["xprop", "-id", wid, "_NET_WM_PID"], capture_output=True, text=True, timeout=2).stdout
        return int(p.strip().split()[-1])
    except (ValueError, IndexError, subprocess.SubprocessError):
        return None


def xrandr_state():
    out = subprocess.run(["xrandr", "--verbose", "--current"], capture_output=True, text=True).stdout
    keep = []
    for line in out.splitlines():
        s = line.strip()
        if " connected" in line or s.startswith("Gamma:") or s.startswith("Brightness:") or "*current" in s:
            keep.append(s)
    return keep


def output_name():
    for line in subprocess.run(["xrandr", "--current"], capture_output=True, text=True).stdout.splitlines():
        if " connected" in line:
            return line.split()[0]
    return None


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def header(path):
    with open(path, newline="") as f:
        return f.readline().strip()


class Ctx:
    def __init__(self, a):
        self.bin = os.path.abspath(a.bin)
        self.out = os.path.abspath(a.out)
        self.player = os.path.join(self.bin, "player", "ysp_player")
        self.make_rig = os.path.join(self.bin, "player", "player_make_rig")
        self.ypak = os.path.join(self.bin, "pack", "ypak")
        self.trials = a.trials
        self.windowed = a.windowed
        self.font = a.font


def build_pack(ctx, name):
    src = os.path.join(HERE, name)
    work = os.path.join(ctx.out, name + "_src")
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(src, work)
    if name == "stroop":
        os.makedirs(os.path.join(work, "fonts"), exist_ok=True)
        shutil.copyfile(ctx.font, os.path.join(work, "fonts", "NotoSans-Regular.ttf"))
    pack = os.path.join(ctx.out, name + ".ysppak")
    subprocess.check_call([ctx.ypak, "build", "pack.json", "-o", pack], cwd=work, stdout=subprocess.DEVNULL)
    with open(os.path.join(work, "pack.json")) as f:
        entry = json.load(f)["experiment"]["source"]
    with open(os.path.join(work, entry)) as f:
        definition = json.load(f)
    return pack, definition


def make_rig(ctx, name, trig_port=None):
    rig = os.path.join(ctx.out, "rig_" + name)
    shutil.rmtree(rig, ignore_errors=True)
    os.makedirs(rig)
    subprocess.check_call([ctx.make_rig, rig])
    path = os.path.join(rig, "profile.json")
    if trig_port:
        with open(path) as f:
            prof = json.load(f)
        prof["roles"]["trig"] = {"family": "triggerbox", "key": "port:" + trig_port}
        with open(path, "w", newline="\n") as f:
            json.dump(prof, f, indent=2, sort_keys=True)
            f.write("\n")
    return path


def the_response(definition):
    t = list(definition["trials"].values())[0]
    r = t["response"]
    return r["from"], float(r.get("duration_s", 0)), float(r.get("minimum_valid_rt_s", 0))


def key_of(definition):
    """The response column's name of each choice to the key that makes it."""
    r = list(definition["trials"].values())[0]["response"]
    out = {}
    for c in r["choices"]:
        if isinstance(c, str):
            out[c] = c
        else:
            out[c.get("name", c["key"])] = c["key"]
    return out


def plan_from_sim(ctx, name, pack, rig, n):
    """Each trial's key and its times before `from` and after the response,
    from a --sim run of the same seed with the correct participant."""
    d = os.path.join(ctx.out, name + "_sim")
    shutil.rmtree(d, ignore_errors=True)
    cmd = [ctx.player, pack, "--out", d, "--sim", "--seed", "11", "--field", "participant=1", "--max-trials", str(n)]
    if rig:
        cmd += ["--rig", rig]
    subprocess.check_call(cmd, stdout=subprocess.DEVNULL)
    return d, read_csv(os.path.join(d, "trials.csv")), read_csv(os.path.join(d, "events.csv")), read_csv(os.path.join(d, "frames.csv"))


def timing_plan(trials, events, frames, frm, duration):
    first = {}
    for fr in frames:
        if fr["trial_index"] != "" and fr["trial_index"] not in first:
            first[fr["trial_index"]] = float(fr["onset_s"])
    onset = {}
    resp = {}
    for e in events:
        if e["kind"] == "onset" and e["stimulus"] == frm and e["trial_index"] not in onset:
            onset[e["trial_index"]] = float(e["t_s"])
        if e["kind"] == "response":
            resp[e["trial_index"]] = float(e["t_s"])
    plan = []
    for i, row in enumerate(trials):
        ti = row["trial_index"]
        nxt = str(int(ti) + 1)
        pre = onset[ti] - first[ti]
        end = resp.get(ti, onset[ti] + duration)
        post = (first[nxt] - end) if nxt in first else 0.5
        plan.append({"key": row["response"], "pre": pre, "post": post})
    return plan


class Pty:
    """tests/loopback/pty_pair.py: two pseudo-terminals, bytes copied
    between them. The player's TriggerBox writes to one; this reads the
    other."""

    def __init__(self):
        self.p = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "loopback", "pty_pair.py")], stdout=subprocess.PIPE, text=True)
        pid, a, b = self.p.stdout.readline().split()
        self.a, self.b = a, b
        self.fd = os.open(b, os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY)
        self.data = bytearray()
        self.stop = False
        self.t = threading.Thread(target=self._read, daemon=True)
        self.t.start()

    def _read(self):
        while not self.stop:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try:
                    self.data += os.read(self.fd, 4096)
                except BlockingIOError:
                    pass

    def close(self):
        if self.stop:
            return
        time.sleep(0.3)
        self.stop = True
        self.t.join()
        os.close(self.fd)
        self.p.send_signal(signal.SIGTERM)
        self.p.wait(timeout=5)


def run_real(ctx, name, pack, plan, rig, extra=(), silent_every=7, abort_after=None, panic_after=None, rt_range=(0.35, 0.6)):
    """Runs the player for real and plays the participant. Returns the exit
    code, the data folder, the presses sent and the player's output."""
    d = os.path.join(ctx.out, name + "_real")
    shutil.rmtree(d, ignore_errors=True)
    cmd = [ctx.player, pack, "--out", d, "--seed", "11", "--field", "participant=1", "--max-trials", str(len(plan)), "--measure"]
    if rig:
        cmd += ["--rig", rig]
    if ctx.windowed:
        cmd += ["--windowed"]
    cmd += list(extra)
    rng = random.Random(5)
    kb = Keyboard()
    time.sleep(1.5)   # X adds a new keyboard about a second after its node (docs/screen.md)
    sent = []
    out_lines = []
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        line = p.stdout.readline()
        out_lines.append(line)
        if "running" not in line:
            p.wait(timeout=30)
            out_lines += p.stdout.readlines()
            return p.returncode, d, sent, "".join(out_lines)
        t_next = time.monotonic() + 0.03   # the first frame follows the line
        focused = False
        for i, tr in enumerate(plan):
            target = t_next + tr["pre"]
            if abort_after is not None and i == abort_after:
                time.sleep(max(0.0, target + 0.2 - time.monotonic()))
                if not check(focus_pid() == p.pid, "%s: the player's window has no focus; no Shift+Esc sent" % name):
                    break
                kb.key("shift", 1)
                t0, t1 = kb.key("esc", 1)
                kb.key("esc", 0)
                kb.key("shift", 0)
                sent.append({"trial": i, "key": "shift+esc", "t0": t0, "t1": t1})
                break
            if panic_after is not None and i == panic_after:
                time.sleep(max(0.0, target + 1.0 - time.monotonic()))
                if not check(focus_pid() == p.pid, "%s: the player's window has no focus; no Shift+Esc sent" % name):
                    break
                for k in range(3):
                    kb.key("shift", 1)
                    t0, t1 = kb.key("esc", 1)
                    kb.key("esc", 0)
                    kb.key("shift", 0)
                    sent.append({"trial": i, "key": "shift+esc", "t0": t0, "t1": t1})
                    time.sleep(0.3)
                break
            silent = silent_every and i % silent_every == silent_every - 1
            if silent:
                t_next = target + plan[i]["_duration"] + tr["post"]
                continue
            rt = rng.uniform(*rt_range)
            t_press = target + rt
            time.sleep(max(0.0, t_press - time.monotonic()))
            if p.poll() is not None:
                break
            if not focused:
                focused = focus_pid() == p.pid
                if not check(focused, "%s: the player's window has no focus; no key sent" % name):
                    break
            t0, t1 = kb.key(tr["key"], 1)
            time.sleep(0.08)
            kb.key(tr["key"], 0)
            sent.append({"trial": i, "key": tr["key"], "name": tr.get("name", tr["key"]), "t0": t0, "t1": t1})
            t_next = t0 / 1e9 + tr["post"]
        try:
            p.wait(timeout=60)
        except subprocess.TimeoutExpired:
            pass
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()
            out = output_name()
            if out:
                subprocess.run(["xrandr", "--output", out, "--mode", "1920x1080", "--rate", "60", "--gamma", "1:1:1"])
            print("  the player did not end: killed, and the mode and ramp put back with xrandr", flush=True)
        kb.close()
    out_lines += p.stdout.readlines()
    return p.returncode, d, sent, "".join(out_lines)


def frame_report(name, d):
    frames = read_csv(os.path.join(d, "frames.csv"))
    dropped = sum(int(f["dropped"] or 0) for f in frames)
    res = [float(f["residual_s"]) for f in frames if f["residual_s"]]
    tiers = {}
    for f in frames:
        tiers[f["tier"]] = tiers.get(f["tier"], 0) + 1
    late = [f for f in frames if int(f["dropped"] or 0) > 0]
    res_sorted = sorted(abs(x) for x in res)
    p99 = res_sorted[int(len(res_sorted) * 0.99)] if res_sorted else 0
    print("  %s: %d frames, %d dropped, residual |max| %.1f us, |p99| %.1f us, tiers %s" %
          (name, len(frames), dropped, max(res_sorted) * 1e6 if res_sorted else 0, p99 * 1e6, tiers), flush=True)
    for f in late[:10]:
        print("    drop: frame %s trial %s dropped %s flags %s" % (f["frame"], f["trial_index"], f["dropped"], f["flags"]))
    return dropped


def check_data(name, sim_dir, d, plan, sent, frm, duration, min_rt):
    for f in ("trials.csv", "frames.csv", "events.csv"):
        check(header(os.path.join(sim_dir, f)) == header(os.path.join(d, f)), "%s: %s has other columns than --sim's" % (name, f))
    rows = read_csv(os.path.join(d, "trials.csv"))
    events = read_csv(os.path.join(d, "events.csv"))
    by_trial = {s["trial"]: s for s in sent}
    resp = {e["trial_index"]: e for e in events if e["kind"] == "response"}
    ok = 0
    for row in rows:
        i = int(row["trial_index"])
        s = by_trial.get(i)
        if s is None:
            check(row["response"] == "" and row["outcome"] == "timeout", "%s trial %d: no key sent, but response '%s' outcome '%s'" % (name, i, row["response"], row["outcome"]))
            continue
        e = resp.get(str(i))
        if not check(e is not None, "%s trial %d: key %s sent, no response event (rsp_flags %s, n_early %s)" % (name, i, s["key"], row["rsp_flags"], row["n_early"])):
            continue
        t = round(float(e["t_s"]) * 1e9)
        rt = float(row["rt_s"])
        # the kernel's time of the key is inside the write() that sent it
        # (to its 1 us truncation): docs/screen.md, "Linux key tiers by stamp"
        check(s["t0"] - 1000 <= t <= s["t1"] + 1000,
              "%s trial %d: the response's time is %.1f us from the write() [0, %.1f]" % (name, i, (t - s["t0"]) / 1e3, (s["t1"] - s["t0"]) / 1e3))
        check(row["response"] == s["name"], "%s trial %d: response '%s', key sent '%s' (%s)" % (name, i, row["response"], s["key"], s["name"]))
        check(row["rsp_tier"] == "0", "%s trial %d: rsp_tier %s, not 0 (the kernel's time, UNKNOWN)" % (name, i, row["rsp_tier"]))
        check(min_rt <= rt and (duration == 0 or rt < duration), "%s trial %d: rt %.3f outside the window" % (name, i, rt))
        check(row["correct"] in ("1", "true", "True"), "%s trial %d: the key sent was the answer, correct is '%s'" % (name, i, row["correct"]))
        ok += 1
    print("  %s: %d rows, %d keys sent, %d responses checked" % (name, len(rows), len(sent), ok), flush=True)
    return rows


def case_example(ctx, name):
    print("== %s" % name, flush=True)
    pack, definition = build_pack(ctx, name)
    frm, duration, min_rt = the_response(definition)
    pty = Pty() if name == "oddball" else None
    try:
        needs_rig = definition.get("display", {}).get("units") == "deg" or name == "oddball"
        rig = make_rig(ctx, name, pty.a if pty else None) if needs_rig else None
        sim_dir, trials, events, frames = plan_from_sim(ctx, name, pack, rig, ctx.trials)
        plan = timing_plan(trials, events, frames, frm, duration)
        keys = key_of(definition)
        for tr in plan:
            tr["_duration"] = duration
            tr["name"] = tr["key"]
            tr["key"] = keys[tr["key"]]
        t0 = time.monotonic()
        rc, d, sent, out = run_real(ctx, name, pack, plan, rig)
        took = time.monotonic() - t0
        print("  %s: exit %s after %.1f s; %s" % (name, rc, took, out.strip().replace("\n", " | ")), flush=True)
        if not check(rc == 0, "%s: exit %s" % (name, rc)):
            return
        frame_report(name, d)
        rows = check_data(name, sim_dir, d, plan, sent, frm, duration, min_rt)
        if pty:
            check_bytes(name, d, pty, rows)
    finally:
        if pty:
            pty.close()


def check_bytes(name, d, pty, rows):
    """The TriggerBox's bytes: a 0 after it opens, each code and the 0 that
    ends its pulse, and 0xFF at its close (ysp/box.h)."""
    pty.close()
    data = bytes(pty.data)
    events = read_csv(os.path.join(d, "events.csv"))
    outs = [e for e in events if e["kind"] == "out"]
    codes = [int(e["code"]) for e in outs]
    want = bytearray([0])
    for c in codes:
        want += bytes([c, 0])
    want += b"\xff"
    check(data == bytes(want), "%s: the pty got %d bytes %s, events.csv's codes give %d %s" % (name, len(data), data[:24].hex(), len(want), bytes(want[:24]).hex()))
    per = {}
    for e in outs:
        per.setdefault(e["trial_index"], []).append(int(e["code"]))
    for row in rows:
        ti = row["trial_index"]
        got = per.get(ti, [])
        want_t = [int(row["code"])] + ([101 if row["response"] == "left" else 102] if row["response"] else [])
        check(sorted(got) == sorted(want_t), "%s trial %s: codes %s, the definition gives %s" % (name, ti, got, want_t))
        check(row["trig_n"] == str(len(want_t)) and row["trig_failed"] == "0", "%s trial %s: trig_n %s trig_failed %s" % (name, ti, row["trig_n"], row["trig_failed"]))
        lag = float(row["trig_lag_max_s"] or 0)
        check(0 <= lag < 0.0167 + (1.0 if row["response"] else 0), "%s trial %s: trig_lag_max_s %s" % (name, ti, row["trig_lag_max_s"]))
    flips = [float(e["t_s"]) - float(e["deadline_s"]) for e in outs if int(e["flags"]) & 0x20]
    if flips:
        flips.sort()
        print("  %s: %d bytes on the pty; %d codes at the flip, write start minus deadline p50 %.1f us, max %.1f us" %
              (name, len(data), len(flips), flips[len(flips) // 2] * 1e6, flips[-1] * 1e6), flush=True)


def case_abort(ctx, kind):
    print("== %s" % kind, flush=True)
    name = "afc2"
    pack, definition = build_pack(ctx, name)
    frm, duration, min_rt = the_response(definition)
    rig = make_rig(ctx, kind)
    sim_dir, trials, events, frames = plan_from_sim(ctx, name, pack, rig, 12)
    plan = timing_plan(trials, events, frames, frm, duration)
    for tr in plan:
        tr["_duration"] = duration
        tr["name"] = tr["key"]
    out = output_name()
    before = xrandr_state()
    # a ramp that is not the identity, so that "put back" means something:
    # a fullscreen screen sets the identity (ysp/screen.h, OS GAMMA)
    subprocess.check_call(["xrandr", "--output", out, "--gamma", "0.9:0.9:0.9"])
    changed = xrandr_state()
    try:
        if kind == "abort":
            rc, d, sent, outp = run_real(ctx, kind, pack, plan, rig, abort_after=6)
            want_rc = 3
        else:
            rc, d, sent, outp = run_real(ctx, kind, pack, plan, rig, extra=["--hang-at-trial", "6"], panic_after=6)
            want_rc = 99
        after = xrandr_state()
    finally:
        subprocess.check_call(["xrandr", "--output", out, "--gamma", "1:1:1"])
    print("  %s: exit %s; %s" % (kind, rc, outp.strip().replace("\n", " | ")), flush=True)
    check(rc == want_rc, "%s: exit %s, not %d" % (kind, rc, want_rc))
    check(after == changed, "%s: the display after the run %s, before it %s" % (kind, after, changed))
    rows = read_csv(os.path.join(d, "trials.csv"))
    check([int(r["trial_index"]) for r in rows] == list(range(6)), "%s: rows of trials %s, not 0 to 5" % (kind, [r["trial_index"] for r in rows]))
    for r in rows:
        empty = [k for k in ("time_elapsed_s", "tier_worst", "frames_dropped", "onset_tier", "onset_residual_s", "outcome") if r[k] == ""]
        check(not empty, "%s trial %s: empty %s" % (kind, r["trial_index"], empty))
    with open(os.path.join(d, "session.json")) as f:
        s = json.load(f)
    if kind == "abort":
        check(s.get("closed") is True, "abort: session.json closed %s" % s.get("closed"))
        ab = s.get("aborted", {})
        check(ab.get("reasons") == ["key"] and ab.get("trial_index") == 6 and ab.get("trials_written") == 6, "abort: session.json aborted %s" % ab)
    else:
        check(s.get("closed") is False, "panic: session.json closed %s (the process ended in the panic)" % s.get("closed"))
        check(os.path.exists(os.path.join(d, "panic.json")), "panic: no panic.json")
    print("  %s: before %s; ramp set to %s; after %s" % (kind, before, changed, after), flush=True)


def case_refuse(ctx):
    print("== refuse", flush=True)
    pack, definition = build_pack(ctx, "afc2")
    rig = make_rig(ctx, "refuse")
    # display.min_tier 1 in a window (tier 2 at best)
    work = os.path.join(ctx.out, "tier1_src")
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(os.path.join(HERE, "rdk"), work)
    with open(os.path.join(work, "experiment.json")) as f:
        dd = json.load(f)
    dd["display"] = {"min_tier": 1}
    with open(os.path.join(work, "experiment.json"), "w", newline="\n") as f:
        json.dump(dd, f)
    tier_pack = os.path.join(ctx.out, "tier1.ysppak")
    subprocess.check_call([ctx.ypak, "build", "pack.json", "-o", tier_pack], cwd=work, stdout=subprocess.DEVNULL)

    def run(args, tag):
        out = os.path.join(ctx.out, "refuse_" + tag)
        shutil.rmtree(out, ignore_errors=True)
        r = subprocess.run([ctx.player] + args + ["--out", out, "--field", "participant=1"], capture_output=True, text=True, timeout=60)
        return r.returncode, (r.stdout + r.stderr).strip(), os.path.exists(out)

    rc, msg, made = run([tier_pack, "--windowed"], "tier")
    print("  min_tier: exit %d: %s" % (rc, msg[:300]), flush=True)
    check(rc == 1 and "min_tier is 1" in msg and not made, "min_tier 1 in a window: exit %d, folder made %s" % (rc, made))
    odd, _ = build_pack(ctx, "oddball")
    rc, msg, made = run([odd, "--rig", rig], "role")
    print("  unbound role: exit %d: %s" % (rc, msg[:300]), flush=True)
    check(rc == 1 and "'trig'" in msg and not made, "a required role with no device: exit %d" % rc)
    # no rig at all: the loader has no rig to check roles against, so the
    # player's own check names it
    work = os.path.join(ctx.out, "norig_src")
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(os.path.join(HERE, "rdk"), work)
    with open(os.path.join(work, "experiment.json")) as f:
        dd = json.load(f)
    dd["devices"]["trig"] = {"direction": "out", "family": ["triggerbox"], "required": True}
    with open(os.path.join(work, "experiment.json"), "w", newline="\n") as f:
        json.dump(dd, f)
    norig_pack = os.path.join(ctx.out, "norig.ysppak")
    subprocess.check_call([ctx.ypak, "build", "pack.json", "-o", norig_pack], cwd=work, stdout=subprocess.DEVNULL)
    rc, msg, made = run([norig_pack], "norig")
    print("  unbound role, no rig: exit %d: %s" % (rc, msg[:300]), flush=True)
    check(rc == 1 and "'trig'" in msg and not made, "a required role with no rig: exit %d" % rc)
    rc, msg, made = run([pack, "--rig", os.path.join(ctx.out, "no_such_rig", "profile.json")], "rig")
    print("  bad rig: exit %d: %s" % (rc, msg[:300]), flush=True)
    check(rc == 1 and "no rig profile" in msg and not made, "a rig file that does not exist: exit %d" % rc)
    rc, msg, made = run([pack, "--rig", rig, "--participant", "correct"], "participant")
    check(rc == 2, "--participant without --sim: exit %d" % rc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default=None)
    ap.add_argument("--trials", type=int, default=30)
    ap.add_argument("--windowed", action="store_true")
    ap.add_argument("--font", default="/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf")
    a = ap.parse_args()
    if not os.environ.get("DISPLAY"):
        print("SKIP: no DISPLAY")
        return 0
    ctx = Ctx(a)
    os.makedirs(ctx.out, exist_ok=True)
    cases = ["rdk", "afc2", "stroop", "oddball", "abort", "panic", "refuse"]
    for c in cases:
        if a.only and c != a.only:
            continue
        if c in ("abort", "panic"):
            case_abort(ctx, c)
        elif c == "refuse":
            case_refuse(ctx)
        else:
            case_example(ctx, c)
    print("%d failures" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
