"""ysp.box on a capture made here: decode a line board's bytes, read in
chunks, as an analysis would decode a raw capture of a session.

    python example.py

No device is needed: the bytes are made below, split where a serial read
could split them.
"""
import ysp.box as box

capture = [   # (read time in seconds on the ysp_rt clock, the bytes of that read)
    (10.000, b"I ysp-line 1 teensy41 left\nS 1000000\n"),
    (10.050, b"E 1049000 1 1\nE 10"),       # a line split across two reads
    (10.070, b"62000 1 0\nQ 7 1070500\n"),
    (10.100, b"S 1100000\nO 1100200 5\n"),
]

d = box.Decoder("line", kind=box.KIND_BOX, device=1)
pairs = []
for t, chunk in capture:
    r = d.decode(chunk, t)
    for e in r.events:
        what = {box.PRESS: "press", box.RELEASE: "release", box.SAMPLE: "sample"}[e.type]
        print("read at %.3f s: %s of channel %d at board tick %d" % (e.t, what, e.control, e.ticks))
    for o in r.outputs:
        print("the board set its outputs to %d at tick %d" % (o.code, o.ticks))
    for s in r.text:
        print("identity:", s)
    pairs += [(t, p.ticks, p.probe) for p in r.pairs]

print("clock pairs (read time, board ticks, probe):", pairs)
print("one tick is %g s; %d frames, %d garbage bytes" % (box.tick("line"), d.frames, d.garbage))

# The bytes a host writes: a trigger code on a Brain Products TriggerBox and
# a 2 ms pulse that a line board times itself.
print("triggerbox set 12:", box.encode("triggerbox", "set", 12))
print("line pulse 5 for 2 ms:", box.encode("line", "pulse", 5, 0.002))
