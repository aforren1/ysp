"""summary.py FILE... [--all] [--events]: the opens in each file of
RESULT lines (open.mjs), and how many were silent. An open is silent when
the program has no audio, or when no context it kept (one with callbacks
or a tap) carried a sample above 0.005. --all prints every open, not only
the silent ones; --events adds the probe's event log of a silent open."""
import json
import sys

files = [a for a in sys.argv[1:] if not a.startswith("--")]
for fn in files:
    rows = [json.loads(l[l.index("{"):]) for l in open(fn, encoding="utf-8") if l.startswith("RESULT")]
    silent = 0
    for j in rows:
        used = [c for c in (j.get("ctx") or []) if c["procs"] or c["polls"]]
        ok = j.get("have_audio") == 1 and any(c["loud"] for c in used)
        silent += not ok
        if ok and "--all" not in sys.argv:
            continue
        last = used[-1] if used else {}
        print(("OK     " if ok else "SILENT ") + f"{j.get('ua')} path={j.get('path')} have_audio={j.get('have_audio')} "
              f"callbacks={j.get('callbacks')} reports={j.get('reports')} trials with a sound={j.get('audio_ok')}/{j.get('trials')} "
              f"contexts={len(used)} last=[{last.get('states')}] {j.get('noaudio') or ''}{j.get('exception') or ''}")
        if not ok and "--events" in sys.argv:
            print("\n".join(j.get("events") or []))
    print(f"{fn}: {len(rows)} opens, {silent} silent")
