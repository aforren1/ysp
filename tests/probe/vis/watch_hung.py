"""Watches a window by title every 50 ms: hung (IsHungAppWindow), a ghost in front.

    python watch_hung.py --title "ysp-vis probe" --seconds 60 --log watch.log

Windows marks a window whose thread pumps no messages for about 4 s as hung
and puts a "Ghost" window in front of it (docs/av_probe.md, section 3).
Writes when the window appears, the first time it is hung, the first ghost,
and a summary line. Ends when the window goes or after --seconds.
"""
import argparse
import ctypes
import time
from ctypes import wintypes

u = ctypes.windll.user32
u.FindWindowW.restype = wintypes.HWND
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.GetForegroundWindow.restype = wintypes.HWND
u.IsHungAppWindow.argtypes = [wintypes.HWND]
u.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
ap = argparse.ArgumentParser()
ap.add_argument("--title", default="ysp-vis probe")
ap.add_argument("--seconds", type=float, default=60.0)
ap.add_argument("--log", default="watch.log")
a = ap.parse_args()
t0 = time.perf_counter()
seen = hung = ghost = None
polls = hung_polls = 0
buf = ctypes.create_unicode_buffer(64)
with open(a.log, "w", newline="\n") as log:
    while time.perf_counter() - t0 < a.seconds:
        t = time.perf_counter() - t0
        hw = u.FindWindowW(None, a.title)
        if hw:
            polls += 1
            if seen is None:
                seen = t
                log.write("%.2f s: window found\n" % t)
            if u.IsHungAppWindow(hw):
                hung_polls += 1
                if hung is None:
                    hung = t
                    log.write("%.2f s: hung\n" % t)
            fg = u.GetForegroundWindow()
            if fg:
                u.GetClassNameW(fg, buf, 64)
                if buf.value == "Ghost" and ghost is None:
                    ghost = t
                    log.write("%.2f s: ghost in front\n" % t)
        elif seen is not None:
            break
        time.sleep(0.05)
    log.write("summary: window %s, polls %d, hung polls %d, first hung %s, ghost %s\n" % (
        "seen" if seen is not None else "never seen", polls, hung_polls,
        "never" if hung is None else "%.2f s" % hung, "never" if ghost is None else "%.2f s" % ghost))
