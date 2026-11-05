"""ysp.pack in an analysis script: open a pack, list it, read its definition,
and check every entry against the manifest.

    python example.py [PACK]

Without PACK it reads the repository's test fixture,
tests/pack/fixtures/small.ysppak.
"""
import hashlib
import json
import os
import sys

import ysp.pack as yp

here = os.path.dirname(os.path.abspath(__file__))
path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "..", "tests", "pack", "fixtures", "small.ysppak")

try:
    pk = yp.Pack(path)
except yp.Error as ex:   # the header's message: which rule, which entry
    sys.exit(f"cannot open {path}: {ex}")

with pk:
    print(pk.describe())
    for e in pk.entries():
        stream = "  (stream)" if e.stream else ""
        print(f"  {e.name!s:<24} {e.kind!s:<10} {e.size:8d} bytes{stream}")

    m = pk.manifest()
    if "tool" in m:
        print("tool:", m["tool"]["name"], m["tool"]["version"])
    if pk.experiment:
        definition = json.loads(pk.definition())
        print("experiment:", pk.experiment.name, "keys:", ", ".join(sorted(definition)))

    # The reader checks XXH64 hashes; the manifest also lists each entry's
    # SHA-256, which a methods section can cite. Check both.
    try:
        pk.verify()
    except yp.CorruptError as ex:
        sys.exit(f"corrupt: {ex}")
    for me in m["entries"]:
        data = pk.read(me["name"])
        ok = hashlib.sha256(data).hexdigest() == me["sha256"]
        print(f"  sha256 {'ok ' if ok else 'BAD'} {me['name']}")
    print("pack ID", pk.id)
