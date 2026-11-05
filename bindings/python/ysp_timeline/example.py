"""ysp.timeline on a simulated 60 Hz display: a trial built as a sequence,
evaluated at each frame's predicted onset, then run again by a rewind.

    python example.py

In PsychoPy, the onset is the next flip's predicted time on the clock that
win.flip() stamps, and the period is win.monitorFramePeriod; in
Psychtoolbox, vbl + ifi from Screen('Flip'). Nothing here reads a clock.
"""
import ysp.timeline as ytl

TRIAL = 1
period = 1 / 60
t0 = 100.0                                   # the first flip's onset, seconds

tl = ytl.Timeline(["fix", "grating", "contrast"])
dot = tl.field((0.0, 0.0))                   # a position the timeline writes

# GSAP: gsap.timeline().set(fix, {visible: 1}).set(fix, {visible: 0}, "+=0.5")
#   .set(grating, {visible: 1}, "<").to(grating, {contrast: 0.5, duration: 0.1,
#   ease: "sine.inOut"}, "<") ... and a dot that moves while the grating shows.
q = tl.seq(TRIAL)
q.on("fix").wait(0.5).off("fix").on("grating").trigger(12)
q.to("contrast", 0.5, duration=0.1, ease="cosine")
q.to(dot, (300, 0), duration=0.6, ease="power2.out")
q.wait(0.6).then("contrast", 0.0, duration=0.1, ease="cosine").off("grating")


def run_trial(first_frame):
    tl.anchor(TRIAL, t0 + first_frame * period)     # trial time 0 is this frame
    for i in range(first_frame, first_frame + 80):
        onset = t0 + i * period
        fired = tl.evaluate(onset, period, i)
        fix, grating, contrast = tl.values
        # draw(fix, grating, contrast, dot.value) would go here
        for e in fired:
            name = {ytl.ONSET: "onset", ytl.OFFSET: "offset", ytl.TRIGGER: "trigger"}.get(e.kind, e.kind)
            print(f"frame {e.frame:3d}  {name!s:<7} ch {e.target:d} code {e.code:2d}  {e.residual * 1e6:.1f} us late")
        if i - first_frame in (30, 33, 36, 60, 72):
            print(f"frame {i:3d}  contrast {contrast:.4f}  dot x {dot.value[0]:.2f}")


run_trial(0)
print("-- the same trial again, anchored at frame 200 --")
run_trial(200)
print(f"{len(tl.events(TRIAL)):d} events in the record")
