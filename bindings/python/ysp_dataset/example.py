"""A simulated session written as a ysp data set: what a PsychoPy script
does with ysp.dataset. The flips and the key presses are made up here; in
PsychoPy, onset_s is win.flip()'s time stamp and t_s a key's time on the
same clock.

    python example.py [folder]

writes <folder>/P07_<time> (a temporary folder by default) and its zip.
"""
import os
import random
import sys
import tempfile
import time

import ysp.dataset as yds

PERIOD = 1 / 60.0

base = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp()
folder = os.path.join(base, time.strftime("P07_%Y%m%dT%H%M%SZ", time.gmtime()))

cols = [
    ("participant", "const", "P07"),
    ("trial_index", "int"),
    ("contrast", "double"),
    ("side", "string"),
    ("response", "string"),
    ("correct", "bool"),
    ("rt_s", "time"),
]
session = {"experiment": "gabor-2afc", "seed": 20261021,
           "fields": {"participant": "P07", "age": 31, "handedness": "right"},
           "versions": {"ysp.dataset": yds.__version__, "python": sys.version.split()[0]}}

rng = random.Random(20261021)
with yds.Dataset(folder, cols, session=session) as ds:
    dots = ds.stream("dots", [("t_s", "time"), ("coherence", "double", 3)])
    ds.write_clock(role="box", kind="pair", t_s=time.perf_counter(), ticks=123456789, width_s=0.00002)
    t = time.perf_counter()
    frame = 0
    for trial in range(10):
        contrast = rng.choice([0.05, 0.1, 0.2])
        side = rng.choice(["left", "right"])
        onset = None
        for k in range(30):                      # 0.5 s of frames
            t += PERIOD
            ds.write_frame(frame=frame, trial_index=trial, target_s=t, onset_s=t + rng.gauss(0, 0.0002))
            if k == 6:
                onset = t
                ds.write_event(t_s=t, trial_index=trial, kind="onset", stimulus="gabor", frame=frame)
            ds.write_stream(dots, t_s=t, coherence=0.5)
            frame += 1
        rt = 0.25 + rng.random() * 0.2
        response = side if rng.random() < 0.8 else ("left" if side == "right" else "right")
        ds.write_event(t_s=onset + rt, trial_index=trial, kind="keyboard", role="kb", text=response)
        ds.write_trial(trial_index=trial, contrast=contrast, side=side, response=response,
                       correct=response == side, rt_s=rt)
        if trial == 4:                           # a live change, between trials
            ds.session_push("changes", {"trial_index": trial, "key": "contrast_step", "value": 0.5})
    rows = ds.finish()
    zip_bytes = ds.zip()

with open(folder + ".zip", "wb") as f:
    f.write(zip_bytes)
print("wrote", folder)
print("rows", rows)
print("zip", len(zip_bytes), "bytes")
with open(os.path.join(folder, "trials.csv"), encoding="utf-8") as f:
    print(f.read().splitlines()[:3])
