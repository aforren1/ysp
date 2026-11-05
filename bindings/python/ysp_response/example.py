"""ysp.response on a scripted participant: a keyboard trial as a PsychoPy
frame loop would run it, a response box, and an offline rescore.

    python example.py

No window and no keyboard: the presses are scripted, in the shape that
PsychoPy's Keyboard.getKeys() returns them (name, tDown, duration). Every
time is in seconds on one clock; in PsychoPy that is the clock of
win.flip() and of the keyboard's tDown.
"""
import types

import ysp.response as yr

FRAME = 1 / 60.0


def key_press(name, t_down, duration):
    return types.SimpleNamespace(name=name, tDown=t_down, duration=duration)


# 1. A keyboard trial: F or J, a window of 1.5 s from the stimulus onset,
#    presses before 0.1 s are anticipations (jsPsych's html-keyboard-response).
rsp = yr.Collector(["f", "j"], duration=1.5, min_rt=0.1)
script = [key_press("j", 10.43, 0.05), key_press("j", 10.95, 0.08)]   # an anticipation, then the response
t_fix, t_stim = 10.0, 10.4
rsp.arm(t_fix)                                   # at the fixation's flip
t = t_fix
while True:
    t += FRAME                                   # t = win.flip()
    if abs(t - t_stim) < FRAME / 2:
        rsp.set_onset(t, src="flip", tier=yr.TIER_UNKNOWN)   # the stimulus's flip
    for k in [k for k in script if k.tDown <= t]:            # keys = kb.getKeys(waitRelease=False)
        script.remove(k)
        rsp.key(k.tDown, k.name)
        if k.duration is not None:
            rsp.key(k.tDown + k.duration, k.name, down=False)
    if rsp.update(t) == yr.ENDED:
        break
r = rsp.finish()
print(f"keyboard: response {r.response}, rt {r.rt:.3f} s, held {r.rt_key_duration:.3f} s, onset tier {r.onset_tier:d}, "
      f"{r.n_anticipations:d} anticipation(s)")
print("  data row:", rsp.data_row())

# 2. A response box: events from ysp.box or ysp.device go to feed() as they are
#    (any 16-field Event in ysp's order), here made by hand. The source entry
#    states how the box is stamped, so the result carries the stamp's quality.
box = yr.Collector([{"name": "left", "kind": "box", "control": 1}, {"name": "right", "kind": "box", "control": 2}],
                   duration=2.0,
                   sources=[{"kind": "box", "tier": yr.TIER_1, "lo": -0.0002, "hi": 0.0002,
                             "note": "device clock fit, checked with a loopback"}])
box.arm(20.0)
box.set_onset(20.25, src="flip", tier=yr.TIER_1)
events = [yr.event(20.6123, kind="box", control=2, unc=0.00004),
          yr.event(20.6890, kind="box", type=yr.RELEASE, control=2)]
box.feed_many(events)                            # once per frame: everything the reader queued
r = box.finish()
print(f"box: response {r.response}, rt {r.rt:.4f} s, stamp tier {r.stamp_tier:d}, "
      f"bounds {r.stamp_lo:+.4f}..{r.stamp_hi:+.4f} s")

# 3. Offline: the same logged events of a trial scored again under other rules.
log = [yr.event(30.43, control="f"), yr.event(30.47, control="f", type=yr.RELEASE),
       yr.event(30.62, control="j"), yr.event(30.70, control="j", type=yr.RELEASE)]
for min_rt in (0.02, 0.1, 0.25):
    r = yr.Collector(["f", "j"], duration=1.5, min_rt=min_rt).score(log, 30.4, arm=30.0)
    rt = "none" if r.rt is None else f"{r.rt:.3f}"
    print(f"rescore with min_rt {min_rt:.2f} s: response {r.response}, rt {rt} s, {r.n_anticipations:d} anticipation(s)")
