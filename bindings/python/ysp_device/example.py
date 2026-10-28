"""ysp.device on simulated boards: a response box, a trigger box, and a
response rule that sends the button's code at the press.

    python example.py            # simulated boards, no hardware
    python example.py --list     # the serial ports and their match keys

With hardware, leave out sim= and give the board's match key, or open a
role from the rig profile: ysp.device.Device(**ysp.rigfile.load().desc("resp")).
"""
import math
import sys

import ysp.device as yd

if "--list" in sys.argv:
    for p in yd.list_ports():
        print(p["key"], p["name"], p["description"])
    sys.exit(0)

# The boards: a line board (buttons on channels 1 and 2) and a TriggerBox.
box_sim = yd.Sim("line")
trig_sim = yd.Sim("triggerbox")

trig = yd.Device("trig", "triggerbox", trig_sim.key, sim=trig_sim, pulse=0.005)
if not trig.wait_running(3.0):
    sys.exit("trig: " + trig.error)

# A code at the press, written on the box's reader thread, not a frame later.
rule = yd.Rule(trig, [{"kind": yd.KIND_BOX, "control": 1, "code": 101},
                      {"kind": yd.KIND_BOX, "control": 2, "code": 102}])
box = yd.Device("resp", "line", box_sim.key, sim=box_sim, rule=rule)
if not box.wait_running(3.0):
    sys.exit("resp: " + box.error)
print("resp:", box.stats()["ident"], "role index", box.device, "tier", box.source()["tier"])

for trial in range(3):
    onset = yd.now() + 0.05
    trig.pulse(10 + trial, 0.002)           # the stimulus code
    rule.arm(onset + 0.1, onset + 1.5)      # [min rt, deadline)
    box_sim.press(onset + 0.3 + 0.1 * trial, 0.08, channel=1 + trial % 2)   # the participant
    events = []
    while len(events) < 2:
        events += box.wait(1.0)
    press = events[0]
    info = rule.info()
    written = (info["t_before"] - info["t_event"]) * 1e6
    print(f"trial {trial:d}: button {press.control:d}, rt {press.t - onset:.4f} s, unc {press.unc * 1e6:.1f} us; "
          f"code {info['code']:d} written {written:.1f} us after the press")
    rule.disarm()

st = box.stats()
print(f"box: {st['events']:d} events, {st['answers']:d} timer queries answered, fit ppm {st['fit']['ppm']:.1f}, "
      f"dropped {st['queue_dropped']:d}")
box.close()      # the input first, then the rule, then the output
rule.close()
trig.close()
print("trig wrote:", " ".join(d.hex() for _, d in trig_sim.writes()))
