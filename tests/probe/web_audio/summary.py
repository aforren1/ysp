"""summary.py FILE... [--all] [--events] [--from]: the opens in each file
of RESULT lines (open.mjs), and how many were silent. An open is silent
when the program has no audio, or when no context it kept (one with
callbacks or a tap) carried a sample above 0.005. An open where the driver
failed (no page result: a timeout or a detached frame) is counted apart,
not as silent. Each file's last line also gives the AudioContexts made per
page (every new AudioContext(), also one closed at once) and used per page
(with callbacks or a tap), and the range of the trials' sound minus plan.
--all prints every open, not only the silent ones; --events adds the
probe's event log of a silent open; --from counts where the contexts were
made (probe.js's call sites)."""
import collections
import json
import sys

files = [a for a in sys.argv[1:] if not a.startswith("--")]


def counts(xs):
    c = collections.Counter(xs)
    return ", ".join(f"{k}: {c[k]}" for k in sorted(c)) or "-"


for fn in files:
    rows = [json.loads(l[l.index("{"):]) for l in open(fn, encoding="utf-8") if l.startswith("RESULT")]
    silent = failed = 0
    made, used_n, amp = [], [], []
    sites = collections.Counter()
    for j in rows:
        if j.get("exception") or not j.get("done"):
            failed += 1
            print(f"DRIVER {j.get('exception') or 'no result'}")
            continue
        ctx = j.get("ctx") or []
        used = [c for c in ctx if c["procs"] or c["polls"]]
        made.append(len(ctx))
        used_n.append(len(used))
        amp += j.get("amp") or []
        for c in ctx:
            sites[c.get("from") or "?"] += 1
        ok = j.get("have_audio") == 1 and any(c["loud"] for c in used)
        silent += not ok
        if ok and "--all" not in sys.argv:
            continue
        last = used[-1] if used else {}
        print(("OK     " if ok else "SILENT ") + f"{j.get('ua')} path={j.get('path')} have_audio={j.get('have_audio')} "
              f"callbacks={j.get('callbacks')} reports={j.get('reports')} trials with a sound={j.get('audio_ok')}/{j.get('trials')} "
              f"contexts made={len(ctx)} used={len(used)} last=[{last.get('states')}] {j.get('noaudio') or ''}")
        if not ok and "--events" in sys.argv:
            print("\n".join(j.get("events") or []))
    if "--from" in sys.argv:
        for k, v in sites.most_common():
            print(f"  {v:4d} {k}")
    rng = f"{min(amp):+.3f} to {max(amp):+.3f} ms over {len(amp)} trials" if amp else "-"
    print(f"{fn}: {len(rows) - failed} opens, {silent} silent; {failed} driver failures; "
          f"contexts made per page {{{counts(made)}}}, used {{{counts(used_n)}}}; sound minus plan {rng}")
