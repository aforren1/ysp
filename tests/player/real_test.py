#!/usr/bin/env python3
"""The player in a real window, with a participant made of a uinput keyboard.

Not in ctest: it needs an X display, /dev/uinput, and the keyboard rule of
tools/linux/70-ysp-input.rules (docs/screen.md, "Kernel key times"). It
changes the display's gamma ramp for the abort test and puts it back.

    python3 tests/player/real_test.py --bin BUILD --out DIR [--only NAME]
                                      [--trials N] [--windowed] [--player EXE]

Cases (--only takes one name):
  rdk afc2 stroop oddball   each worked example for N trials (default 30):
                            the data set has the columns of --sim; each
                            response is the key sent, its time inside the
                            write() that sent it; frames dropped and the
                            residuals are reported. oddball's trigger role
                            is a TriggerBox on a pty pair: the bytes read
                            are the codes events.csv says went out.
  rule                      oddball with its response role on a LINE board
                            emulated on a second pty pair: the presses
                            come over the input bridge, the response's
                            code goes out from the board's reader thread
                            (a ydev_rule), and the rows have no
                            SENT_MISMATCH
  abort                     Shift+Esc in the middle: exit 3, the rows of
                            the finished trials, session.json "aborted",
                            the mode and the gamma ramp back
  panic                     a frame loop that hangs (--hang-at-trial, a
                            test option), then Shift+Esc 3 times: the
                            watchdog ends the process (99), the rows of
                            the finished trials are on disk, the ramp back
  refuse                    display.min_tier in a window, a required role
                            with no device, a rig file that does not exist
  alloc                     the four examples (oddball with its pty
                            TriggerBox) through player_alloc_test --real:
                            no heap call on the frame thread inside
                            pl_frame(), keys and codes included

The participant: a --sim run of the same seed gives each trial's key (the
correct one) and how long the trial takes before its `from` stimulus and
after the response. A real trial is then timed from the participant's own
last press, so the error of one trial does not add to the next. Every
seventh trial is silent (a timeout). A key goes out only while the
player's window has the input focus (_NET_ACTIVE_WINDOW's _NET_WM_PID).
"""
import argparse
import csv
import decimal
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


def restore_display():
    """After a player that had to be killed: X keeps a ramp and a mode
    after the process ends (docs/screen.md)."""
    out = output_name()
    if out:
        subprocess.run(["xrandr", "--output", out, "--mode", "1920x1080", "--rate", "60", "--gamma", "1:1:1"])
    print("  the player did not end: killed, and the mode and ramp put back with xrandr", flush=True)


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
        self.player = a.player or os.path.join(self.bin, "player", "ysp_player")
        self.make_rig = os.path.join(self.bin, "player", "player_make_rig")
        self.ypak = os.path.join(self.bin, "pack", "ypak")
        self.trials = a.trials
        self.windowed = a.windowed
        self.font = a.font
        # the player's own options that a case's roles need (set by main())
        self.odd_extra = []
        self.line_extra = []


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


def make_rig(ctx, name, trig_port=None, resp_port=None):
    rig = os.path.join(ctx.out, "rig_" + name)
    shutil.rmtree(rig, ignore_errors=True)
    os.makedirs(rig)
    subprocess.check_call([ctx.make_rig, rig])
    path = os.path.join(rig, "profile.json")
    if trig_port:
        with open(path) as f:
            prof = json.load(f)
        prof["roles"]["trig"] = {"family": "triggerbox", "key": "port:" + trig_port}
        if resp_port:
            prof["roles"]["resp"] = {"family": "line", "key": "port:" + resp_port}
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


class LineBoard(Pty):
    """A board of the ysp line protocol (ysp/box.h, YBOX_LINE) on a pty
    pair: it answers "i" and "q", sends a sync every 100 ms, and a button's
    edges on press(). Its counter is CLOCK_MONOTONIC in microseconds, so a
    press's time is known on the host's clock as well."""

    def __init__(self):
        self.p = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "loopback", "pty_pair.py")], stdout=subprocess.PIPE, text=True)
        pid, a, b = self.p.stdout.readline().split()
        self.a, self.b = a, b
        self.fd = os.open(b, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
        self.lock = threading.Lock()
        self.data = bytearray()
        self.stop = False
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _send(self, line):
        with self.lock:
            os.write(self.fd, (line + "\n").encode())

    def _run(self):
        buf = b""
        last_sync = 0.0
        while not self.stop:
            r, _, _ = select.select([self.fd], [], [], 0.02)
            if r:
                try:
                    buf += os.read(self.fd, 4096)
                except BlockingIOError:
                    pass
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.strip().decode("ascii", "replace")
                    if line == "i":
                        self._send("I ysp-line 1 test resp")
                    elif line.startswith("q "):
                        self._send("Q %s %d" % (line[2:], time.monotonic_ns() // 1000))
            if time.monotonic() - last_sync >= 0.1:
                last_sync = time.monotonic()
                self._send("S %d" % (time.monotonic_ns() // 1000))

    def press(self, ch):
        t = time.monotonic_ns() // 1000
        self._send("E %d %d 1" % (t, ch))
        time.sleep(0.08)
        self._send("E %d %d 0" % (time.monotonic_ns() // 1000, ch))
        return t * 1000, t * 1000


def run_real(ctx, name, pack, plan, rig, extra=(), silent_every=7, abort_after=None, panic_after=None, rt_range=(0.35, 0.6), board=None):
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
            if board:
                # a button of the LINE board: no focus needed, the press is
                # the board's own time
                t0, t1 = board.press(tr["button"])
            else:
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
            restore_display()
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


def ns(text):
    """A seconds column to integer nanoseconds, exactly (9 decimals)."""
    return int((decimal.Decimal(text) * 1000000000).to_integral_value())


def odd_code(row):
    """The oddball's response code for a row; None when its template sends
    none (rule2's template) or nothing was pressed."""
    if not row["response"] or row["trial_type"] != "oddball":
        return None
    return 101 if row["response"] == "left" else 102


def check_bytes(name, d, pty, rows, resp_code=odd_code):
    """The TriggerBox's bytes: a 0 after it opens, each code and the 0 that
    ends its pulse, and 0xFF at its close (ysp/box.h). Each row's counts
    are its out rows': `trig_n` their number, `trig_failed` the failed ones
    (flag 0x8), `trig_lag_max_s` exactly their largest write start minus
    deadline."""
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
        per.setdefault(e["trial_index"], []).append(e)
    for row in rows:
        ti = row["trial_index"]
        got = per.get(ti, [])
        rc = resp_code(row)
        want_t = [int(row["code"])] + ([rc] if rc is not None else [])
        check(sorted(int(e["code"]) for e in got) == sorted(want_t), "%s trial %s: codes %s, the definition gives %s" % (name, ti, [int(e["code"]) for e in got], want_t))
        failed = sum(1 for e in got if int(e["flags"]) & 0x8)
        check(row["trig_n"] == str(len(got)) and row["trig_failed"] == str(failed),
              "%s trial %s: trig_n %s trig_failed %s, events.csv has %d codes, %d failed" % (name, ti, row["trig_n"], row["trig_failed"], len(got), failed))
        lag = max((ns(e["t_s"]) - ns(e["deadline_s"]) for e in got), default=0)
        check(row["trig_lag_max_s"] != "" and ns(row["trig_lag_max_s"]) == lag,
              "%s trial %s: trig_lag_max_s %s, events.csv's largest lag %.9f" % (name, ti, row["trig_lag_max_s"], lag / 1e9))
    flips = [float(e["t_s"]) - float(e["deadline_s"]) for e in outs if int(e["flags"]) & 0x20]
    if flips:
        flips.sort()
        print("  %s: %d bytes on the pty; %d codes at the flip, write start minus deadline p50 %.1f us, max %.1f us" %
              (name, len(data), len(flips), flips[len(flips) // 2] * 1e6, flips[-1] * 1e6), flush=True)


def oddball_variant(ctx, tag, edit):
    work = os.path.join(ctx.out, tag + "_src")
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(os.path.join(HERE, "oddball"), work)
    with open(os.path.join(work, "experiment.json")) as f:
        dd = json.load(f)
    edit(dd)
    with open(os.path.join(work, "experiment.json"), "w", newline="\n") as f:
        json.dump(dd, f)
    pack = os.path.join(ctx.out, tag + ".ysppak")
    subprocess.check_call([ctx.ypak, "build", "pack.json", "-o", pack], cwd=work, stdout=subprocess.DEVNULL)
    return pack, dd


def case_noafter(ctx):
    """oddball with no `after`: the trial ends on the frame of the response,
    so the frame loop's code is written in the trial's last frame and its
    device record comes after the trial ended. The row must still count it
    (a review's finding, 2026-11-10)."""
    print("== noafter", flush=True)

    def edit(dd):
        del dd["trials"]["oddball"]["after"]
    pack, dd = oddball_variant(ctx, "noafter", edit)
    frm, duration, min_rt = the_response(dd)
    pty = Pty()
    try:
        rig = make_rig(ctx, "noafter", pty.a)
        sim_dir, trials, events, frames = plan_from_sim(ctx, "noafter", pack, rig, ctx.trials)
        plan = timing_plan(trials, events, frames, frm, duration)
        keys = key_of(dd)
        for tr in plan:
            tr["_duration"] = duration
            tr["name"] = tr["key"]
            tr["key"] = keys[tr["key"]]
        rc, d, sent, out = run_real(ctx, "noafter", pack, plan, rig, extra=ctx.odd_extra)
        print("  noafter: exit %s" % rc, flush=True)
        if check(rc == 0, "noafter: exit %s: %s" % (rc, out.strip()[-300:])):
            check_bytes("noafter", d, pty, read_csv(os.path.join(d, "trials.csv")))
    finally:
        pty.close()


def case_lost(ctx):
    """The oddball with its 3rd code at a flip never written
    (--test-lose-code 3), as a canceled trigger: that trial counts it failed,
    and every later record still goes to its own trial."""
    print("== lost", flush=True)
    pack, dd = build_pack(ctx, "oddball")
    frm, duration, min_rt = the_response(dd)
    pty = Pty()
    try:
        rig = make_rig(ctx, "lost", pty.a)
        sim_dir, trials, events, frames = plan_from_sim(ctx, "oddball", pack, rig, ctx.trials)
        plan = timing_plan(trials, events, frames, frm, duration)
        keys = key_of(dd)
        for tr in plan:
            tr["_duration"] = duration
            tr["name"] = tr["key"]
            tr["key"] = keys[tr["key"]]
        rc, d, sent, out = run_real(ctx, "lost", pack, plan, rig, extra=ctx.odd_extra + ["--test-lose-code", "3"])
        print("  lost: exit %s" % rc, flush=True)
        if not check(rc == 0, "lost: exit %s: %s" % (rc, out.strip()[-300:])):
            return
        rows = read_csv(os.path.join(d, "trials.csv"))
        events = read_csv(os.path.join(d, "events.csv"))
        lost = rows[2]
        flips = [e for e in events if e["kind"] == "out" and int(e["flags"]) & 0x20]
        check(not any(e["trial_index"] == "2" for e in flips), "lost: trial 2 has a code at the flip in events.csv")
        check(lost["trig_failed"] == "1", "lost: trial 2 trig_failed %s, not 1" % lost["trig_failed"])
        for e in flips:
            check(int(e["code"]) == int(rows[int(e["trial_index"])]["code"]), "lost: a code at the flip in trial %s is %s, its row's %s" %
                  (e["trial_index"], e["code"], rows[int(e["trial_index"])]["code"]))
        for row in rows:
            if row["trial_index"] != "2":
                check(row["trig_failed"] == "0", "lost: trial %s trig_failed %s" % (row["trial_index"], row["trig_failed"]))
        print("  lost: %d codes at the flip for %d trials" % (len(flips), len(rows)), flush=True)
    finally:
        pty.close()


def line_edit(dd):
    dd["devices"]["resp"]["family"] = ["line", "keyboard"]


def case_rule2(ctx):
    """A LINE board's rule and a template on the same role with no response
    trigger, whose trials come first: none of its presses may send a code
    (a review's finding, 2026-11-10)."""
    print("== rule2", flush=True)

    def edit(dd):
        line_edit(dd)
        t = json.loads(json.dumps(dd["trials"]["oddball"]))
        del t["response"]["trigger"]
        dd["trials"]["notrig"] = t
        first = json.loads(json.dumps(dd["timeline"][0]))
        first["name"] = "first"
        first["trial"] = "notrig"
        dd["timeline"] = [first, dd["timeline"][0]]
    pack, dd = oddball_variant(ctx, "rule2", edit)
    frm, duration, min_rt = the_response(dd)
    trig, board = Pty(), LineBoard()
    try:
        rig = make_rig(ctx, "rule2", trig.a, board.a)
        sim_dir, trials, events, frames = plan_from_sim(ctx, "rule2", pack, rig, ctx.trials)
        plan = timing_plan(trials, events, frames, frm, duration)
        buttons = {v: int(k) for k, v in dd["devices"]["resp"]["buttons"].items()}
        for tr in plan:
            tr["_duration"] = duration
            tr["name"] = tr["key"]
            tr["button"] = buttons[tr["key"]]
        rc, d, sent, out = run_real(ctx, "rule2", pack, plan, rig, board=board, extra=ctx.line_extra)
        print("  rule2: exit %s" % rc, flush=True)
        if check(rc == 0, "rule2: exit %s: %s" % (rc, out.strip()[-300:])):
            rows = read_csv(os.path.join(d, "trials.csv"))
            check(any(r["trial_type"] == "notrig" and r["response"] for r in rows), "rule2: no pressed trial of the template with no trigger")
            check_bytes("rule2", d, trig, rows)
    finally:
        trig.close()
        board.close()


def case_fewer(ctx):
    """Two templates on a LINE role whose rules differ in their choices:
    refused at the start, naming both (a review's finding, 2026-11-10)."""
    print("== fewer", flush=True)

    def edit(dd):
        line_edit(dd)
        t = json.loads(json.dumps(dd["trials"]["oddball"]))
        t["response"]["choices"] = ["left"]
        t["response"]["trigger"]["code"] = {"by": "$response", "map": {"left": 101}}
        del t["response"]["key_answer"]
        dd["trials"]["one"] = t
        first = json.loads(json.dumps(dd["timeline"][0]))
        first["name"] = "first"
        first["trial"] = "one"
        dd["timeline"] = [first, dd["timeline"][0]]
    pack, dd = oddball_variant(ctx, "fewer", edit)
    trig, board = Pty(), LineBoard()
    try:
        rig = make_rig(ctx, "fewer", trig.a, board.a)
        out = os.path.join(ctx.out, "fewer_real")
        shutil.rmtree(out, ignore_errors=True)
        # a player that starts runs one silent trial (about 3 s) and ends
        p = subprocess.Popen([ctx.player, pack, "--out", out, "--field", "participant=1", "--rig", rig, "--max-trials", "1"] + ctx.line_extra,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            msg = p.communicate(timeout=60)[0].strip()
        except subprocess.TimeoutExpired:
            p.kill()
            msg = p.communicate()[0].strip()
            restore_display()
        print("  fewer: exit %d: %s" % (p.returncode, msg[:300]), flush=True)
        check(p.returncode == 1 and "'one'" in msg and "'oddball'" in msg and not os.path.exists(out),
              "fewer: two rules with other choices on one role: exit %d" % p.returncode)
    finally:
        trig.close()
        board.close()


def case_rule(ctx):
    """oddball with its response role on a LINE board: the response's code
    goes out from the board's reader thread (a ydev_rule, 2.6), and the
    board's presses come over the screen's input bridge."""
    print("== rule", flush=True)
    work = os.path.join(ctx.out, "rule_src")
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(os.path.join(HERE, "oddball"), work)
    with open(os.path.join(work, "experiment.json")) as f:
        dd = json.load(f)
    dd["devices"]["resp"]["family"] = ["line", "keyboard"]
    with open(os.path.join(work, "experiment.json"), "w", newline="\n") as f:
        json.dump(dd, f)
    pack = os.path.join(ctx.out, "rule.ysppak")
    subprocess.check_call([ctx.ypak, "build", "pack.json", "-o", pack], cwd=work, stdout=subprocess.DEVNULL)
    frm, duration, min_rt = the_response(dd)
    trig, board = Pty(), LineBoard()
    try:
        rig = make_rig(ctx, "rule", trig.a, board.a)
        sim_dir, trials, events, frames = plan_from_sim(ctx, "rule", pack, rig, ctx.trials)
        plan = timing_plan(trials, events, frames, frm, duration)
        buttons = {v: int(k) for k, v in dd["devices"]["resp"]["buttons"].items()}
        for tr in plan:
            tr["_duration"] = duration
            tr["name"] = tr["key"]
            tr["button"] = buttons[tr["key"]]
        rc, d, sent, out = run_real(ctx, "rule", pack, plan, rig, board=board)
        print("  rule: exit %s; %s" % (rc, out.strip().replace("\n", " | ")[:4000]), flush=True)
        if not check(rc == 0, "rule: exit %s" % rc):
            return
        frame_report("rule", d)
        rows = read_csv(os.path.join(d, "trials.csv"))
        evs = read_csv(os.path.join(d, "events.csv"))
        resp = {e["trial_index"]: e for e in evs if e["kind"] == "response"}
        rule_outs = {e["trial_index"]: e for e in evs if e["kind"] == "out" and int(e["flags"]) & 0x200}
        by_trial = {x["trial"]: x for x in sent}
        lags = []
        for row in rows:
            i = int(row["trial_index"])
            x = by_trial.get(i)
            if x is None:
                check(row["response"] == "", "rule trial %d: no press, response '%s'" % (i, row["response"]))
                continue
            e = resp.get(str(i))
            if not check(e is not None, "rule trial %d: no response event" % i):
                continue
            t = round(float(e["t_s"]) * 1e9)
            # the board's time through the device clock fit: within its
            # bracket (1.3 ms for LINE, ysp/device.h TIME)
            check(abs(t - x["t0"]) < 1500000, "rule trial %d: the response is %.1f us from the board's press" % (i, (t - x["t0"]) / 1e3))
            check(row["response"] == x["name"], "rule trial %d: response '%s', button %s" % (i, row["response"], x["name"]))
            check(int(row["rsp_flags"]) & 0x400 == 0, "rule trial %d: SENT_MISMATCH (rsp_flags %s)" % (i, row["rsp_flags"]))
            o = rule_outs.get(str(i))
            if check(o is not None, "rule trial %d: no code from the rule" % i):
                check(int(o["code"]) == (101 if x["name"] == "left" else 102), "rule trial %d: code %s" % (i, o["code"]))
                lags.append(float(o["t_s"]) - float(o["deadline_s"]))
        check_bytes("rule", d, trig, rows)
        if lags:
            lags.sort()
            print("  rule: %d codes from the rule, write start minus the press p50 %.1f us, max %.1f us" %
                  (len(lags), lags[len(lags) // 2] * 1e6, lags[-1] * 1e6), flush=True)
    finally:
        trig.close()
        board.close()


def case_alloc(ctx):
    print("== alloc", flush=True)
    keep = ctx.player
    ctx.player = os.path.join(ctx.bin, "player", "player_alloc_test")
    try:
        for name in ("rdk", "afc2", "stroop", "oddball"):
            pack, definition = build_pack(ctx, name)
            frm, duration, min_rt = the_response(definition)
            pty = Pty() if name == "oddball" else None
            try:
                needs_rig = definition.get("display", {}).get("units") == "deg" or name == "oddball"
                rig = make_rig(ctx, "alloc_" + name, pty.a if pty else None) if needs_rig else None
                ctx.player = keep
                sim_dir, trials, events, frames = plan_from_sim(ctx, name, pack, rig, 12)
                ctx.player = os.path.join(ctx.bin, "player", "player_alloc_test")
                plan = timing_plan(trials, events, frames, frm, duration)
                keys = key_of(definition)
                for tr in plan:
                    tr["_duration"] = duration
                    tr["name"] = tr["key"]
                    tr["key"] = keys[tr["key"]]
                rc, d, sent, out = run_real(ctx, "alloc_" + name, pack, plan, rig, extra=["--real"])
                print("  %s: exit %s, %d keys; %s" % (name, rc, len(sent), out.strip().splitlines()[-1] if out.strip() else ""), flush=True)
                check(rc == 0 and "inside pl_frame: 0" in out, "%s: heap calls on the frame thread: %s" % (name, out.strip()[-200:]))
            finally:
                if pty:
                    pty.close()
    finally:
        ctx.player = keep


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
    ap.add_argument("--player", default=None, help="another player binary (the mutation test's)")
    a = ap.parse_args()
    if not os.environ.get("DISPLAY"):
        print("SKIP: no DISPLAY")
        return 0
    ctx = Ctx(a)
    os.makedirs(ctx.out, exist_ok=True)
    cases = ["rdk", "afc2", "stroop", "oddball", "noafter", "lost", "rule", "rule2", "fewer", "abort", "panic", "refuse", "alloc"]
    for c in cases:
        if a.only and c != a.only:
            continue
        if c in ("abort", "panic"):
            case_abort(ctx, c)
        elif c == "refuse":
            case_refuse(ctx)
        elif c == "alloc":
            case_alloc(ctx)
        elif c == "rule":
            case_rule(ctx)
        elif c == "noafter":
            case_noafter(ctx)
        elif c == "lost":
            case_lost(ctx)
        elif c == "rule2":
            case_rule2(ctx)
        elif c == "fewer":
            case_fewer(ctx)
        else:
            case_example(ctx, c)
    print("%d failures" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
