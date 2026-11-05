"""The ysp-av probe's frame loop in Python (docs/av_probe.md).

    python run_py.py --bundle DIR [--windowed] [--n N] [--seconds S]
                     [--gc on|off|freeze] [--csv PATH] [--idle S]
                     [--abort | --panic] [--audio null|default]
                     [--angle-dir DIR] [--preload DLL] [--modules]

DIR holds ysp_av_probe.pyd with SDL3.dll, libEGL.dll, libGLESv2.dll and
d3dcompiler_47.dll beside it, as a wheel would carry them. DIR goes on
sys.path only, never on PATH.
"""
import argparse
import gc
import os
import sys
import time

ap = argparse.ArgumentParser()
ap.add_argument("--bundle", required=True)
ap.add_argument("--windowed", action="store_true")
ap.add_argument("--n", type=int, default=1)
ap.add_argument("--seconds", type=float, default=5)
ap.add_argument("--gc", choices=["on", "off", "freeze"], default="on")
ap.add_argument("--csv")
ap.add_argument("--idle", type=float, default=0)
ap.add_argument("--idle-mode", choices=["sleep", "input", "pump"], default="sleep")
ap.add_argument("--abort", action="store_true")
ap.add_argument("--panic", action="store_true")
ap.add_argument("--audio")
ap.add_argument("--angle-dir")
ap.add_argument("--preload")
ap.add_argument("--modules", action="store_true")
ap.add_argument("--noghost", action="store_true", help="DisableProcessWindowsGhosting() before open")
ap.add_argument("--heap", type=int, default=100000, help="trial dicts made at setup")
a = ap.parse_args()

sys.path.insert(0, os.path.abspath(a.bundle))
import ysp_av_probe as av  # noqa: E402

if a.preload:
    print("preload", a.preload, av.preload(a.preload))
if a.modules:
    print("modules before open:\n" + av.modules())

# A heap like an experiment's: a trial list of dicts, made before the loop.
trials = [{"trial": i, "cond": i % 8, "ori": float(i % 180), "rt": None, "resp": None} for i in range(a.heap)]

av.elevate()
if a.noghost:
    av.noghost()
print(av.open(windowed=a.windowed, angle_dir=a.angle_dir, panic=a.panic))
log = []          # every record, kept as a user's script would keep it
try:
    av.setup(a.n)
    if a.audio:
        print("audio", a.audio, av.audio_open(a.audio == "null"))
    if a.modules:
        print("modules after open:\n" + av.modules())
    if a.gc == "off":
        gc.disable()
    elif a.gc == "freeze":
        gc.collect()
        gc.freeze()
    n = a.n
    ORI = av.P_ORI
    begin, evaluate, gfx_begin, draw, setp, gfx_end, flip_at = (
        av.begin, av.evaluate, av.gfx_begin, av.draw, av.set, av.gfx_end, av.flip_at)

    def frames(count=None, until=None):
        k = 0
        while count is None or k < count:
            if until is not None and av.now() >= until:
                return True
            f = begin()
            if f is None:
                return False
            index, onset, period, recs = f
            log.extend(recs)
            evaluate()
            gfx_begin()
            v = float(index % 360)
            for i in range(n):
                setp(i, ORI, v)
                draw(i)
            gfx_end()
            flip_at(onset)
            k += 1
        return True

    if a.abort or a.panic:
        frames(60)
        av.keys(3 if a.panic else 1, 500, 300)
        if a.panic:
            print("hanging the interpreter for 10 s; 3 presses follow", flush=True)
            time.sleep(10)
            print("panic did not end the process")
        else:
            ok = frames(until=av.now() + 5)
            print("abort: loop ended by abort:", not ok, "abort_info", av.abort_info(), "keys", av.keys_status())
    elif a.idle > 0:
        frames(120)
        first = av.counts()[0]
        av.watch_start()
        if a.idle_mode == "sleep":
            time.sleep(a.idle)
        elif a.idle_mode == "pump":
            # what a binding's wait() would do: pump every 50 ms
            end = time.monotonic() + a.idle
            while time.monotonic() < end:
                av.pump()
                time.sleep(0.05)
        else:
            input("idle: press Enter\n")
        print("idle %.1f s (%s), watcher:\n%s" % (a.idle, a.idle_mode, av.watch_stop()))
        av.watch_start()
        frames(120)
        print("after idle, watcher:\n" + av.watch_stop())
        print("first frame after idle: index", first)
    else:
        t0 = av.now()
        frames(until=t0 + a.seconds)
    print("gc counts", gc.get_count(), "stats", gc.get_stats()[2])
finally:
    av.close()
    if a.csv:
        tag = "host=python%d.%d gc=%s n=%d windowed=%d seconds=%.0f" % (
            sys.version_info[0], sys.version_info[1], a.gc, a.n, a.windowed, a.seconds)
        print("dumped", av.dump(a.csv, tag), "frames to", a.csv)
    print("frames %d records %d lost %d; records in the log %d" % (av.counts() + (len(log),)))
    av.free()
