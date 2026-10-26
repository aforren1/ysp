"""ysp.rigfile: a rig profile made, checked and read back; a role's device
opened from it when ysp-device is installed.

    python example.py            # a profile in a temporary folder
    python example.py --user     # the user's own profile (read only)
"""
import os
import sys
import tempfile

import ysp.rigfile as yrig

if "--user" in sys.argv:
    try:
        prof = yrig.load()
    except yrig.Missing as e:
        sys.exit("no rig profile: %s (make one with rigfile_profile --new NAME)" % e.args[0])
    print("rig", prof.rig, "written", prof.written, "hash", prof.hash)
    for note in prof.notes:
        print("note:", note)
    for name in prof.roles:
        r = prof.role(name)
        print(name, r["family"], r["key"], "tier", prof.source(name)["tier"])
    sys.exit(0)

prof = yrig.Profile("booth-2")
prof.bind("resp", "line", "serial:16C0:0483:B1:")
prof.bind("trig", "triggerbox", "serial:0403:6001:TB0123:")
prof.set_view(570, 518.4, 1920)
print(prof.text)
print("ppd %.4f, hash %s" % (prof.ppd, prof.hash))

with tempfile.TemporaryDirectory() as d:
    path = os.path.join(d, "profile.json")
    prof.save_file(path)
    again = yrig.load_file(path)
    print("read back:", again.roles, "same hash" if again.hash == prof.hash else "OTHER HASH", again.notes)
    print("resp tier:", again.source("resp")["tier"], "-", again.source("resp")["note"])

try:
    import ysp.device as yd
except ImportError:
    sys.exit(0)
sim = yd.Sim("line", key="serial:16C0:0483:B1:1-1")
with yd.Device(**prof.desc("resp"), sim=sim) as dev:
    print("resp opened from the profile:", dev.wait_running(3.0), dev.key, "tier", prof.source(dev)["tier"])
