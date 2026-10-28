"""The queue-ahead probe's Python driver (docs/queue_probe.md).

Loads probe_queue.dll through ctypes, so the host that sleeps between
calls is a real Python interpreter (time.sleep). One mode per run:

  uv run --no-project python tests/probe/queue/run_queue.py MODE BIN_DIR [--windowed] [--json OUT]

MODE: land, far, many, cancel, panic, sleep, cost, smoke.
BIN_DIR holds probe_queue.dll and SDL3.dll; --angle DIR the ANGLE DLLs.
"""
import argparse
import ctypes as C
import json
import os
import statistics
import sys
import time

FLAGS = {0x002: "ESTIMATED", 0x004: "LATE_TARGET", 0x008: "OCCLUDED", 0x010: "GRID_UNSTABLE",
         0x020: "EARLY", 0x040: "SKIPPED", 0x080: "CANCELED", 0x100: "ONSET_PLANNED",
         0x1000: "UNSYNCED"}
PATHS = {0: "unknown", 1: "composed", 2: "overlay", 3: "independent"}
QUIT = 1


class Rec(C.Structure):
    _fields_ = [("index", C.c_int64), ("target", C.c_int64), ("planned", C.c_int64), ("onset", C.c_int64),
                ("residual", C.c_int64), ("t_arrive", C.c_int64), ("dropped", C.c_uint32),
                ("flags", C.c_uint16), ("path", C.c_uint8), ("tier", C.c_uint8)]


class Sub(C.Structure):
    _fields_ = [("index", C.c_int64), ("f_onset", C.c_int64), ("target", C.c_int64), ("t_begin", C.c_int64),
                ("t_begun", C.c_int64), ("t_flipped", C.c_int64), ("cpu_ns", C.c_int64)]


class Probe:
    def __init__(self, bin_dir, angle):
        os.add_dll_directory(os.path.abspath(bin_dir))
        d = C.CDLL(os.path.join(os.path.abspath(bin_dir), "probe_queue.dll"))
        self.d = d
        i64, i32, p64 = C.c_int64, C.c_int, C.POINTER(C.c_int64)
        d.pq_error.restype = C.c_char_p
        d.pq_now.restype = i64
        d.pq_open.argtypes = [i32, i32, i32, C.c_char_p, i32, i64]
        d.pq_queue.restype = i64
        d.pq_queue.argtypes = [i32, i32, i32, i32, p64, C.POINTER(C.c_uint32)]
        d.pq_reserve.argtypes = [i32, p64]
        d.pq_mem.argtypes = [p64, p64]
        d.pq_alloc_test.argtypes = [i32, i32, p64, p64, p64]
        d.pq_records.argtypes = [C.POINTER(Rec), i32, i32]
        d.pq_subs.argtypes = [C.POINTER(Sub), i32, i32]
        d.pq_cancel_info.argtypes = [p64, p64, C.POINTER(C.c_int), p64]
        d.pq_keys_join.argtypes = [p64, C.POINTER(C.c_int), C.POINTER(C.c_int)]
        d.pq_watch_stop.argtypes = [p64]
        d.pq_panic_state.argtypes = [C.POINTER(C.c_int), p64, p64]
        d.pq_cancel_who.argtypes = [p64, C.POINTER(C.c_int), C.POINTER(C.c_int), C.POINTER(C.c_int)]
        d.pq_begin_abort.argtypes = [C.POINTER(C.c_uint32), C.POINTER(C.c_int32), p64, C.POINTER(C.c_int)]
        self.angle = angle.encode() if angle else None

    def err(self):
        return self.d.pq_error().decode()

    def open(self, windowed, panic=False, grace_ns=0):
        rc = self.d.pq_open(1 if windowed else 0, 1280, 800, self.angle, 1 if panic else 0, grace_ns)
        if rc:
            raise SystemExit("open: " + self.err())
        buf = C.create_string_buffer(4096)
        self.d.pq_describe(buf, 4096)
        return buf.value.decode()

    def now(self):
        return self.d.pq_now()

    def mem(self):
        a, b = C.c_int64(0), C.c_int64(0)
        self.d.pq_mem(C.byref(a), C.byref(b))
        return a.value, b.value

    def reserve(self, n):
        ns = C.c_int64(0)
        rc = self.d.pq_reserve(n, C.byref(ns))
        if rc:
            raise SystemExit(self.err())
        return ns.value

    def queue(self, k, spacing=1, lead=0, load=0):
        t0, ab = C.c_int64(0), C.c_uint32(0)
        self.collect()   # nothing in flight before a queue
        first = self.d.pq_queue(k, spacing, lead, load, C.byref(t0), C.byref(ab))
        if first < 0 and first != -100:
            raise SystemExit("queue: %d %s" % (first, self.err()))
        return first, t0.value, ab.value

    def frames(self, n, load=0):
        rc = self.d.pq_frames(n, load)
        if rc:
            raise SystemExit("frames: %d %s" % (rc, self.err()))

    def collect(self):
        return self.d.pq_collect()

    def records(self, start=0):
        n = self.d.pq_nrec() - start
        arr = (Rec * max(n, 1))()
        n = self.d.pq_records(arr, n, start)
        return [{f: getattr(arr[i], f) for f, _ in Rec._fields_} for i in range(n)]

    def subs(self, start=0):
        n = self.d.pq_nsub() - start
        arr = (Sub * max(n, 1))()
        n = self.d.pq_subs(arr, n, start)
        return [{f: getattr(arr[i], f) for f, _ in Sub._fields_} for i in range(n)]

    def cancel_info(self):
        a, b, n, s = C.c_int64(0), C.c_int64(0), C.c_int(0), C.c_int64(0)
        self.d.pq_cancel_info(C.byref(a), C.byref(b), C.byref(n), C.byref(s))
        return a.value, b.value, n.value, s.value

    def cancel_who(self):
        t, a, b, n = C.c_int64(0), C.c_int(0), C.c_int(0), C.c_int(0)
        self.d.pq_cancel_who(C.byref(t), C.byref(a), C.byref(b), C.byref(n))
        return t.value, a.value, b.value, n.value

    def begin_abort(self):
        ab, pr, t, n = C.c_uint32(0), C.c_int32(0), C.c_int64(0), C.c_int(0)
        rc = self.d.pq_begin_abort(C.byref(ab), C.byref(pr), C.byref(t), C.byref(n))
        return rc, ab.value, pr.value, t.value

    def keys_join(self):
        t, s, k = (C.c_int64 * 16)(), C.c_int(0), C.c_int(0)
        self.d.pq_keys_join(t, C.byref(s), C.byref(k))
        return [x for x in t if x], s.value, k.value

    def watch_stop(self):
        out = (C.c_int64 * 6)()
        self.d.pq_watch_stop(out)
        return dict(zip(["polls", "hung", "ghost", "fg", "iconic", "max_beat_gap_ns"], list(out)))

    def panic_state(self):
        r, a, b = C.c_int(0), C.c_int64(0), C.c_int64(0)
        self.d.pq_panic_state(C.byref(r), C.byref(a), C.byref(b))
        return r.value, a.value, b.value

    def close(self):
        self.d.pq_close()


def flagnames(f):
    return "|".join(n for b, n in FLAGS.items() if f & b) or "-"


def pct(v, p):
    v = sorted(v)
    if not v:
        return float("nan")
    k = min(len(v) - 1, max(0, int(round(p / 100 * (len(v) - 1)))))
    return v[k]


def summarize(recs, label):
    """On the vblank asked for: shown (no SKIPPED/CANCELED), dropped 0, not EARLY."""
    shown = [r for r in recs if not r["flags"] & (0x040 | 0x080)]
    on = [r for r in shown if r["dropped"] == 0 and not r["flags"] & 0x020 and not r["flags"] & 0x004]
    res = [r["residual"] / 1e3 for r in shown]
    paths, tiers, flags = {}, {}, {}
    for r in recs:
        paths[PATHS.get(r["path"], r["path"])] = paths.get(PATHS.get(r["path"], r["path"]), 0) + 1
        tiers[r["tier"]] = tiers.get(r["tier"], 0) + 1
        fn = flagnames(r["flags"] & ~0x001)
        flags[fn] = flags.get(fn, 0) + 1
    s = {"label": label, "n": len(recs), "shown": len(shown), "on_vblank": len(on),
         "late": sum(1 for r in shown if r["dropped"] > 0), "early": sum(1 for r in shown if r["flags"] & 0x020),
         "late_target": sum(1 for r in shown if r["flags"] & 0x004),
         "res_us_p50": pct(res, 50), "res_us_min": min(res) if res else float("nan"),
         "res_us_max": max(res) if res else float("nan"), "paths": paths, "tiers": tiers, "flags": flags}
    print("%-34s n=%4d shown=%4d on=%4d late=%d early=%d late_target=%d res_us p50=%+.1f [%+.1f, %+.1f] %s tiers=%s flags=%s"
          % (label, s["n"], s["shown"], s["on_vblank"], s["late"], s["early"], s["late_target"], s["res_us_p50"],
             s["res_us_min"], s["res_us_max"], paths, tiers, flags))
    return s


def wait_done(p, t_end_ns):
    """The host sleeps until the last queued vblank has passed."""
    left = (t_end_ns - p.now()) / 1e9
    if left > 0:
        time.sleep(left + 0.05)


def by_index(recs):
    return {r["index"]: r for r in recs}


def run_queue_once(p, k, spacing=1, lead=0, load=0, sleep_extra=0.0):
    p.collect()   # nothing in flight before the queue
    r0, s0 = p.d.pq_nrec(), p.d.pq_nsub()
    first, t0, ab = p.queue(k, spacing, lead, load)
    t_sub = p.now()
    period = 16_666_667
    wait_done(p, t0 + (k - 1) * spacing * period)
    if sleep_extra:
        time.sleep(sleep_extra)
    p.collect()
    recs = [r for r in p.records(r0) if r["index"] >= first and r["index"] < first + k]
    subs = p.subs(s0)
    return recs, subs, t_sub, t0


def mode_land(p, a, out):
    """Several queued target times in a row, against one held frame."""
    reps = a.reps
    p.frames(60)
    res = {}
    # baseline: one held frame, 0 to 4 vblanks ahead, one in flight (0.10.3)
    recs = []
    for i in range(reps * 30):
        recs += run_queue_once(p, 1, 1, i % 5)[0]
    res["held1"] = summarize(recs, "one held frame, lead 0-4")
    out["held1_records"] = recs
    for k in a.ks:
        ns = p.reserve(k)
        print("reserve %d: %.1f ms" % (k, ns / 1e6))
        if a.prime:
            p.collect()
            r0 = p.d.pq_nrec()
            p.frames(a.prime)   # one in flight: each buffer is presented once in turn
            p.collect()
            pr = p.records(r0)
            res["prime%d" % k] = summarize(pr, "  %d frames after reserve %d" % (a.prime, k))
            out["prime%d_records" % k] = pr
        recs, subsall = [], []
        for i in range(reps):
            r, s, _, _ = run_queue_once(p, k, 1, 0)
            recs += r
            subsall += s
            p.frames(10)
        res["K%d" % k] = summarize(recs, "queue K=%d, %d reps" % (k, reps))
        out["K%d_records" % k] = recs
        # predicted onsets: f.onset against the onset shown
        subs = {s["index"]: s for s in subsall}
        d = [r["onset"] - subs[r["index"]]["f_onset"] for r in recs if r["index"] in subs and r["onset"]]
        print("   f.onset vs onset: p50 %+.1f us, max |.| %.1f us" % (pct(d, 50) / 1e3, max(abs(x) for x in d) / 1e3 if d else -1))
    out["summary"] = res


def mode_far(p, a, out):
    """How far ahead: one frame, then a short queue, aimed far ahead."""
    p.reserve(8)
    p.frames(60)
    res = {}
    for lead in a.leads:
        recs = []
        for i in range(a.reps):
            r, s, t_sub, t0 = run_queue_once(p, 4, 1, lead)
            recs += r
        res["lead%d" % lead] = summarize(recs, "K=4 lead %d vblanks (%.2f s)" % (lead, lead / 60))
        out["lead%d_records" % lead] = recs
    out["summary"] = res


def mode_many(p, a, out):
    """How many: reserve n, queue n frames ahead in a row."""
    res = {}
    for k in a.ks:
        m0 = p.mem()
        ns = p.reserve(k)
        m1 = p.mem()
        print("reserve %d: %.1f ms, local memory %.1f -> %.1f MB" % (k, ns / 1e6, m0[0] / 2**20, m1[0] / 2**20))
        p.frames(30)
        recs = []
        for i in range(a.reps):
            r, s, _, _ = run_queue_once(p, k, 1, 0)
            recs += r
            p.frames(10)
        res["K%d" % k] = summarize(recs, "queue K=%d" % k)
        res["K%d" % k]["reserve_ms"] = ns / 1e6
        res["K%d" % k]["mem_mb"] = [m0[0] / 2**20, m1[0] / 2**20]
        out["K%d_records" % k] = recs
    out["summary"] = res


def mode_cancel(p, a, out):
    """Abort (Shift+Esc) mid-queue; the abort hook cancels the queue."""
    p.reserve(60)
    p.frames(60)
    bits = 1 | (4 if a.blank else 0)
    p.d.pq_cancel_on(bits)
    rows = []
    for rep in range(a.reps):
        r0, s0 = p.d.pq_nrec(), p.d.pq_nsub()
        n0 = p.cancel_info()[2]
        p.collect()
        p.d.pq_keys(1, a.delay_ms, 50, 1 if a.request else 0)
        first, t0, ab = p.queue(60, 1, 0, 0)
        t_q = p.now()
        time.sleep(1.3)   # the host sleeps; the queue would end within 1 s
        keys, sent, skipped = p.keys_join()
        p.collect()
        rc, abort, presses, abort_ns = p.begin_abort()
        p.d.pq_drain()
        c0, c1, n, blank = p.cancel_info()
        recs = [r for r in p.records(r0) if r["index"] >= first]
        nsub = p.d.pq_nsub() - s0
        shown = [r for r in recs if not r["flags"] & 0xC0]
        canceled = [r for r in recs if r["flags"] & 0x80]
        t_key = keys[0] if keys else 0
        last_on = max((r["onset"] for r in shown), default=0)
        after = [r for r in shown if r["onset"] > c1]
        row = {"rep": rep, "sent": sent, "skipped": skipped, "submitted": nsub, "abort_in_submit": ab,
               "begin_rc": rc, "abort": abort, "presses": presses, "cancels": n - n0,
               "key_to_cancel_us": (c0 - t_key) / 1e3 if t_key and n > n0 else None,
               "cancel_call_us": (c1 - c0) / 1e3 if n > n0 else None,
               "shown": len(shown), "canceled": len(canceled), "records": len(recs),
               "last_onset_minus_cancel_ms": (last_on - c1) / 1e6 if n > n0 else None,
               "shown_after_cancel": len(after),
               "blank_after_cancel_ms": (blank - c1) / 1e6 if blank > 1 else None,
               "flags": sorted({flagnames(r["flags"] & ~1) for r in recs})}
        ft, fw, lw, dropped = p.cancel_who()
        row.update({"first_who": fw, "last_who": lw, "not_presented_total": dropped})
        rows.append(row)
        print(json.dumps(row))
        p.frames(60)
    out["rows"] = rows


def mode_panic(p, a, out):
    """Three presses while the host sleeps mid-queue: the panic's release."""
    p.reserve(60)
    p.frames(60)
    p.d.pq_cancel_on(a.bits)
    p.d.pq_watch_start()
    p.collect()
    r0 = p.d.pq_nrec()
    p.d.pq_keys(3, a.delay_ms, 120, 0)
    first, t0, ab = p.queue(60, 1, 0, 0)
    time.sleep(3.0)
    keys, sent, skipped = p.keys_join()
    w = p.watch_stop()
    released, t_panic, t_rel = p.panic_state()
    p.collect()
    c0, c1, n, blank = p.cancel_info()
    recs = [r for r in p.records(r0) if r["index"] >= first]
    shown = [r for r in recs if not r["flags"] & 0xC0]
    last_on = max((r["onset"] for r in shown), default=0)
    rc, abort, presses, abort_ns = p.begin_abort()
    ft, fw, lw, dropped = p.cancel_who()
    row = {"bits": a.bits, "sent": sent, "key1_to_first_cancel_ms": (ft - keys[0]) / 1e6 if keys and ft else None,
           "first_who": fw, "last_who": lw, "not_presented": dropped, "skipped": skipped, "abort_in_submit": ab, "released": released,
           "key1_to_cancel_ms": (c0 - keys[0]) / 1e6 if keys and n else None,
           "key3_to_release_ms": (t_rel - keys[-1]) / 1e6 if keys and t_rel else None,
           "panic_to_release_ms": (t_rel - t_panic) / 1e6 if t_rel and t_panic else None,
           "cancel_minus_release_ms": (c0 - t_rel) / 1e6 if n and t_rel else None,
           "cancels": n, "shown": len(shown), "canceled": sum(1 for r in recs if r["flags"] & 0x80),
           "records": len(recs), "last_onset_minus_cancel_ms": (last_on - c1) / 1e6 if n else None,
           "shown_after_cancel": sum(1 for r in shown if n and r["onset"] > c1),
           "begin_rc": rc, "abort": abort, "presses": presses, "watch": w,
           "flags": sorted({flagnames(r["flags"] & ~1) for r in recs})}
    print(json.dumps(row))
    out["row"] = row


def mode_sleep(p, a, out):
    """An RSVP of 60 queued frames, then the host sleeps; then normal frames."""
    p.reserve(60)
    p.frames(60)
    rows = []
    for s in a.sleeps:
        r0, s0 = p.d.pq_nrec(), p.d.pq_nsub()
        p.d.pq_watch_start()
        first, t0, ab = p.queue(60, 1, 0, 0)
        t_sub = p.now()
        n_before = p.d.pq_nrec()
        time.sleep(s)
        w = p.watch_stop()
        n_during = p.d.pq_nrec() - n_before
        t_wake = p.now()
        p.collect()
        t_coll = p.now()
        recs = [r for r in p.records(r0) if first <= r["index"] < first + 60]
        st = summarize(recs, "RSVP 60, sleep %.1f s" % s)
        # the frames right after: one in flight, each for f.onset
        r1 = p.d.pq_nrec()
        p.frames(120)
        p.collect()
        after = p.records(r1)
        st_after = summarize(after, "  then 120 frames, one in flight")
        first10 = summarize(after[:10], "  first 10 of them")
        subs_after = p.subs(s0 + 60)
        row = {"sleep_s": s, "submit_ms": (t_sub - p.subs(s0)[0]["t_begin"]) / 1e6, "records_during_sleep": n_during,
               "collect_ms": (t_coll - t_wake) / 1e6, "arrive_after_onset_ms_p50":
               pct([(r["t_arrive"] - r["onset"]) / 1e6 for r in recs if r["onset"]], 50),
               "watch": w, "queue": st, "after": st_after, "after10": first10,
               "first_after_wait_ms": (subs_after[0]["t_begun"] - subs_after[0]["t_begin"]) / 1e6 if subs_after else None}
        print(json.dumps({k: v for k, v in row.items() if k not in ("queue", "after", "after10")}))
        rows.append(row)
        out["sleep%g_records" % s] = recs
    out["rows"] = rows


def mode_cost(p, a, out):
    """CPU per queued frame, memory per buffer, and RGBA16F's."""
    w, h = C.c_int(0), C.c_int(0)
    p.d.pq_size(C.byref(w), C.byref(h))
    print("back buffer %d x %d" % (w.value, h.value))
    res = {"w": w.value, "h": h.value}
    for fmt, name in ((28, "RGBA8"), (10, "RGBA16F")):
        for n in (8, 32, 62):
            b, af, ns = C.c_int64(0), C.c_int64(0), C.c_int64(0)
            made = p.d.pq_alloc_test(fmt, n, C.byref(b), C.byref(af), C.byref(ns))
            mb = (af.value - b.value) / 2**20
            print("alloc %-7s n=%2d made=%2d: +%.1f MB (%.2f MB each), %.1f ms" %
                  (name, n, made, mb, mb / max(made, 1), ns.value / 1e6))
            res["%s_%d" % (name, n)] = {"made": made, "mb": mb, "ms": ns.value / 1e6}
    p.frames(60)
    s0 = p.d.pq_nsub()
    p.frames(300, a.load)
    p.collect()
    one = p.subs(s0)
    cpu1 = [s["cpu_ns"] / 1e3 for s in one]
    wall1 = [(s["t_flipped"] - s["t_begun"]) / 1e3 for s in one]
    print("one in flight, 300 frames: CPU/frame p50 %.0f us p99 %.0f us; begin-return to flip-return p50 %.0f us" %
          (pct(cpu1, 50), pct(cpu1, 99), pct(wall1, 50)))
    res["one"] = {"cpu_us_p50": pct(cpu1, 50), "cpu_us_p99": pct(cpu1, 99), "draw_flip_us_p50": pct(wall1, 50)}
    for k in a.ks:
        m0 = p.mem()
        ns = p.reserve(k)
        m1 = p.mem()
        cpus, walls, totals = [], [], []
        for rep in range(a.reps):
            s0 = p.d.pq_nsub()
            r, s, t_sub, t0 = run_queue_once(p, k, 1, 0, a.load)
            cpus += [x["cpu_ns"] / 1e3 for x in s[:k]]
            walls += [(x["t_flipped"] - x["t_begun"]) / 1e3 for x in s[:k]]
            totals.append((s[k - 1]["t_flipped"] - s[0]["t_begin"]) / 1e6)
            p.frames(10)
        print("K=%2d: reserve %.1f ms, local %.0f -> %.0f MB; CPU/frame p50 %.0f us p99 %.0f us; "
              "submit all K p50 %.2f ms max %.2f ms" % (k, ns / 1e6, m0[0] / 2**20, m1[0] / 2**20, pct(cpus, 50),
                                                         pct(cpus, 99), pct(totals, 50), max(totals)))
        res["K%d" % k] = {"reserve_ms": ns / 1e6, "mem_mb": [m0[0] / 2**20, m1[0] / 2**20],
                          "cpu_us_p50": pct(cpus, 50), "cpu_us_p99": pct(cpus, 99),
                          "submit_ms_p50": pct(totals, 50), "submit_ms_max": max(totals)}
    out["summary"] = res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode")
    ap.add_argument("bin")
    ap.add_argument("--angle", default="C:/Program Files/Docker/Docker/frontend")
    ap.add_argument("--windowed", action="store_true")
    ap.add_argument("--json")
    ap.add_argument("--reps", type=int, default=10)
    ap.add_argument("--ks", type=lambda s: [int(x) for x in s.split(",")], default=[2, 4, 8, 16, 30, 60])
    ap.add_argument("--leads", type=lambda s: [int(x) for x in s.split(",")], default=[2, 10, 60, 300, 600])
    ap.add_argument("--sleeps", type=lambda s: [float(x) for x in s.split(",")], default=[1.5, 6.0])
    ap.add_argument("--delay-ms", type=int, default=300)
    ap.add_argument("--blank", action="store_true")
    ap.add_argument("--request", action="store_true", help="abort by yscr_request_abort when not in front")
    ap.add_argument("--bits", type=int, default=3)
    ap.add_argument("--load", type=int, default=0)
    ap.add_argument("--prime", type=int, default=0, help="land: frames one in flight after each reserve")
    a = ap.parse_args()
    p = Probe(a.bin, a.angle)
    panic = a.mode == "panic"
    desc = p.open(a.windowed, panic=panic, grace_ns=1_000_000 if panic else 0)
    print(desc)
    out = {"mode": a.mode, "windowed": a.windowed, "describe": desc, "argv": sys.argv}
    try:
        if a.mode == "smoke":
            p.reserve(4)
            p.frames(60)
            r, s, _, _ = run_queue_once(p, 4)
            summarize(r, "smoke K=4")
            for x in r:
                print(x)
        else:
            globals()["mode_" + a.mode](p, a, out)
    finally:
        buf = C.create_string_buffer(4096)
        p.d.pq_describe(buf, 4096)
        out["describe_end"] = buf.value.decode()
        print(out["describe_end"])
        p.close()
    if a.json:
        with open(a.json, "w", newline="\n") as f:
            json.dump(out, f)


if __name__ == "__main__":
    main()
