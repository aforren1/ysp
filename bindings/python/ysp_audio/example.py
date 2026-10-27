"""ysp.audio: a tone at a time on the ysp_rt clock, and where it landed.

    python example.py            # miniaudio's null device: no sound
    python example.py --device   # the OS default output (-40 dBFS tones)

In PsychoPy, t is the flip time you want the sound on, on the clock the
flips are stamped with: put that clock on ysp_rt time (ysp.rt) or give the
sound a time from yau.now(). In Psychtoolbox the MEX function ysp_audio is
the same.
"""
import sys

import ysp.audio as yau

backend = "auto" if "--device" in sys.argv else "null"
print("outputs:", [d["name"] for d in yau.devices(backend=backend)])

with yau.Audio(backend=backend) as a:
    print(a.describe())
    beep = a.tone(1000, 0.1, yau.db(-40), ramp=0.005, id=1)

    # A sound must be handed over at least `a.lead` before its time.
    t = yau.now() + a.lead + 0.05
    for k in range(5):
        sid = a.play_at(beep, t + 0.25 * k)
    r = a.wait(sid)                                    # the last one; GIL released
    print("onset %.6f s, residual %d ns, tier %d, flags 0x%x"
          % (r.onset, r.residual_ns, r.tier, r.flags))
    for k in range(sid - 4, sid + 1):
        r = a.result(k)
        print("  play %d: start frame %d, residual %+6d ns%s"
              % (k, r.start_frame, r.residual_ns, " LATE" if r.flags & yau.LATE else ""))
