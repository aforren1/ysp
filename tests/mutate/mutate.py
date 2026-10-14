# /// script
# requires-python = ">=3.11"
# ///
"""Mutation runner for the ysp headers.

Each mutant is one literal edit of a header, kept in a TOML file next to
this script. The runner copies the headers and tests/adapt into a work
directory, applies one mutant to the copy, builds the named test against
the copy, runs it and compares the result with an unmutated baseline.
It never writes to the repository: other people build against the
in-tree headers while a run is in progress.

    uv run tests/mutate/mutate.py                 every *.toml here
    uv run tests/mutate/mutate.py color.toml      one list
    uv run tests/mutate/mutate.py gfx.toml --only v04-06 v04-07
    uv run tests/mutate/mutate.py --check         anchors only, no builds

See tests/mutate/README.md.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import tomllib
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
WIN = os.name == "nt"
QUIET = os.environ.get("YSP_QUIET_FLAG", "C:/tmp/psy-quiet" if WIN else "/tmp/psy-quiet")
LOCK = os.environ.get("YSP_MEASURE_LOCK", "C:/tmp/psy-measure.lock" if WIN else "/tmp/psy-measure.lock")
EXE = ".exe" if WIN else ""

_print_lock = threading.Lock()


def say(*a):
    with _print_lock:
        print(*a, flush=True)


def wait_quiet():
    # The quiet flag means another person needs the machine idle (no CPU or
    # GPU work at all); it is theirs to create and remove, never ours.
    noted = False
    while os.path.exists(QUIET):
        if not noted:
            say("quiet flag %s present: waiting" % QUIET)
            noted = True
        time.sleep(60)


def wait_idle():
    # Between mutants, also yield to a timing run: our builds would load
    # the CPU while someone holds the measurement lock.
    noted = False
    while True:
        wait_quiet()
        if not os.path.exists(LOCK):
            return
        if not noted:
            say("measurement lock %s held: pausing between mutants" % LOCK)
            noted = True
        time.sleep(30)


# --- toolchains --------------------------------------------------------------

def find_gcc():
    g = os.environ.get("YSP_MUT_GCC")
    if g:
        return g
    if WIN and os.path.exists(r"C:\tmp\winlibs\mingw64\bin\gcc.exe"):
        return r"C:\tmp\winlibs\mingw64\bin\gcc.exe"
    return shutil.which("gcc") or "gcc"


_msvc_env = None
_msvc_lock = threading.Lock()


def msvc_env():
    # vcvars64.bat takes seconds; run it once and reuse its environment.
    global _msvc_env
    with _msvc_lock:
        if _msvc_env is not None:
            return _msvc_env
        bat = os.environ.get("YSP_MUT_VCVARS")
        if not bat:
            vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                                   "Microsoft Visual Studio", "Installer", "vswhere.exe")
            r = subprocess.run([vswhere, "-latest", "-products", "*", "-requires",
                                "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property",
                                "installationPath"], capture_output=True, text=True)
            root = r.stdout.strip().splitlines()[0] if r.stdout.strip() else ""
            bat = os.path.join(root, "VC", "Auxiliary", "Build", "vcvars64.bat")
        r = subprocess.run('cmd /s /c ""%s" >nul && set"' % bat, capture_output=True, text=True, shell=True)
        env = {}
        for line in r.stdout.splitlines():
            k, sep, v = line.partition("=")
            if sep:
                env[k] = v
        if "INCLUDE" not in env:
            raise SystemExit("MSVC environment not found (set YSP_MUT_VCVARS to vcvars64.bat)")
        _msvc_env = env
        return env


def build(test, inc, src_dir, out_dir, tag):
    """Builds test['source'] against the headers in inc; returns (exe, error)."""
    src = os.path.join(src_dir, os.path.basename(test["source"]))
    exe = os.path.join(out_dir, tag + EXE)
    if os.path.exists(exe):
        os.remove(exe)
    wait_quiet()
    if test.get("compiler", "gcc") == "msvc":
        cmd = ["cl", "/nologo"] + test.get("flags", []) + ["/I" + inc, src, "/Fe" + exe,
                                                           "/Fo" + out_dir + os.sep, "/link", "/nologo"] + test.get("libs", [])
        env = msvc_env()
        # CreateProcess searches the parent's PATH, not the one in env.
        path = next((v for k, v in env.items() if k.upper() == "PATH"), "")
        cmd[0] = shutil.which("cl", path=path) or "cl"
    else:
        gcc = find_gcc()
        cmd = [gcc] + [f.replace("{repo}", REPO) for f in test.get("flags", [])] + ["-I" + inc, "-o", exe, src] + \
              [f.replace("{repo}", REPO) for f in test.get("libs", [])]
        env = dict(os.environ)
        env["PATH"] = os.path.dirname(gcc) + os.pathsep + env.get("PATH", "")
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, cwd=out_dir)
    if r.returncode != 0 or not os.path.exists(exe):
        msg = [l for l in (r.stdout + r.stderr).splitlines() if "error" in l.lower()]
        return None, (msg[0] if msg else (r.stdout + r.stderr).strip()[:300])
    return exe, None


def run_env(test, mutant):
    env = dict(os.environ)
    if test.get("compiler", "gcc") != "msvc":
        env["PATH"] = os.path.dirname(find_gcc()) + os.pathsep + env.get("PATH", "")
    for k in test.get("unset", []):
        env.pop(k, None)
    for k, v in test.get("env_default", {}).items():
        env.setdefault(k, v.replace("{repo}", REPO))
    for k, v in list(test.get("env", {}).items()) + list(mutant.get("env", {}).items()):
        env[k] = v.replace("{repo}", REPO)
    return env


FAIL_RE = re.compile(r"FAIL(?: at)? line \d+")


def fail_keys(text, ignore):
    """The failing checks, by line, without the measured values that vary."""
    keys = set()
    for line in text.splitlines():
        if "FAIL" not in line or any(re.search(p, line) for p in ignore):
            continue
        m = FAIL_RE.search(line)
        keys.add(line[:m.end()] if m else re.sub(r"[-+]?\d[\d.e+-]*", "#", line))
    return keys


def run(exe, test, mutant, cwd):
    wait_quiet()
    env = run_env(test, mutant)
    try:
        r = subprocess.run([exe], capture_output=True, text=True, env=env, cwd=cwd,
                           timeout=test.get("timeout", 600), errors="replace")
    except subprocess.TimeoutExpired:
        return None, set(), ""
    out = r.stdout + r.stderr
    return r.returncode, fail_keys(out, test.get("ignore", [])), out


# --- mutants -----------------------------------------------------------------

def edits(m):
    yield m["find"], m["replace"], m.get("count", 1), m.get("file")
    for e in m.get("also", []):
        yield e["find"], e["replace"], e.get("count", 1), e.get("file")


def apply(m, default_file, read):
    """Returns {file: mutated text}, or an error string when an anchor does
    not match the expected number of times."""
    out = {}
    for find, repl, count, f in edits(m):
        f = f or default_file
        text = out.get(f) if f in out else read(f)
        n = text.count(find)
        if n != count:
            return "anchor in %s found %d times, want %d" % (f, n, count)
        out[f] = text.replace(find, repl)
    return out


def load(path):
    with open(path, "rb") as fh:
        d = tomllib.load(fh)
    d["path"] = path
    for m in d.get("mutant", []):
        if m.get("test") not in d.get("test", {}):
            raise SystemExit("%s: mutant %s names unknown test %r" % (path, m.get("id"), m.get("test")))
    return d


def repo_file(f):
    """A mutated file's path in the repository: a header under include/
    ("ysp/pack.h"), or a pack tool source under pack/ ("import.c")."""
    p = os.path.join(REPO, "include", f)
    return p if os.path.exists(p) else os.path.join(REPO, "pack", f)


def snapshot(work):
    """Copies the headers, the pack tool's C sources and tests/adapt once,
    so a file someone edits during the run cannot mix two versions into
    one result."""
    hdr = os.path.join(work, "hdr")
    tdir = os.path.join(work, "test")
    os.makedirs(hdr, exist_ok=True)
    os.makedirs(tdir, exist_ok=True)
    os.makedirs(os.path.join(hdr, "ysp"), exist_ok=True)
    for f in os.listdir(os.path.join(REPO, "include", "ysp")):
        if f.endswith(".h"):
            shutil.copy2(os.path.join(REPO, "include", "ysp", f), os.path.join(hdr, "ysp"))
    # pack/*.c beside the headers' folder: a test that includes "import.c"
    # finds the copy, mutated or not, before any -I of the repository
    for f in os.listdir(os.path.join(REPO, "pack")):
        if f.endswith(".c"):
            shutil.copy2(os.path.join(REPO, "pack", f), hdr)
    for f in os.listdir(os.path.join(REPO, "pack", "ysp")):
        if f.endswith(".h"):
            shutil.copy2(os.path.join(REPO, "pack", "ysp", f), os.path.join(hdr, "ysp"))
    adapt = os.path.join(REPO, "tests", "adapt")
    for f in os.listdir(adapt):
        if f.endswith((".h", ".c")):
            shutil.copy2(os.path.join(adapt, f), tdir)
    return hdr, tdir


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("lists", nargs="*", help="mutant files (default: every *.toml in tests/mutate)")
    ap.add_argument("--only", nargs="+", default=[], help="mutant ids to run")
    ap.add_argument("--check", action="store_true", help="check the anchors against the tree; build nothing")
    ap.add_argument("--jobs", "-j", type=int, default=1, help="mutants built and run at once (default 1)")
    ap.add_argument("--work", help="work directory (default: a new temporary directory)")
    ap.add_argument("--keep", action="store_true", help="keep the work directory")
    a = ap.parse_args()

    paths = a.lists or sorted(os.path.join(HERE, f) for f in os.listdir(HERE) if f.endswith(".toml"))
    paths = [p if os.path.exists(p) else os.path.join(HERE, p) for p in paths]
    lists = [load(p) for p in paths]

    if a.check:
        bad = 0
        for L in lists:
            for m in L.get("mutant", []):
                if a.only and m["id"] not in a.only:
                    continue
                r = apply(m, L["header"], lambda f: open(repo_file(f), encoding="utf-8").read())
                if isinstance(r, str):
                    bad += 1
                    say("%s %s: %s" % (os.path.basename(L["path"]), m["id"], r))
        say("anchors: %d not matching" % bad)
        return 1 if bad else 0

    work = a.work or tempfile.mkdtemp(prefix="ysp-mutate-")
    os.makedirs(work, exist_ok=True)
    hdr, tdir = snapshot(work)
    texts = {}

    def read(f):
        if f not in texts:
            # Text mode: anchors use "\n" whether git checked out CRLF or LF.
            with open(os.path.join(hdr, f), encoding="utf-8") as fh:
                texts[f] = fh.read()
        return texts[f]

    # One header directory per job; a mutant writes its files there and
    # puts the snapshot back afterwards.
    jobs = max(1, a.jobs)
    slots = []
    for j in range(jobs):
        d = os.path.join(work, "j%d" % j)
        if os.path.exists(d):
            shutil.rmtree(d)
        shutil.copytree(hdr, d)
        slots.append(d)
    free = list(slots)
    free_lock = threading.Lock()

    baselines = {}
    base_lock = threading.Lock()

    def baseline(L, tname, m):
        test = L["test"][tname]
        key = (L["path"], tname, tuple(sorted(m.get("env", {}).items())))
        with base_lock:
            if key in baselines:
                return baselines[key]
            wait_idle()
            bkey = (L["path"], tname)
            if bkey not in baselines:
                d = os.path.join(work, "base-" + tname)
                os.makedirs(d, exist_ok=True)
                exe, err = build(test, hdr, tdir, d, "t")
                baselines[bkey] = (exe, err)
            exe, err = baselines[bkey]
            if not exe:
                res = (None, None, None, err)
            else:
                t0 = time.time()
                rc, keys, _ = run(exe, test, m, os.path.dirname(exe))
                res = (rc, keys, time.time() - t0, None)
                note = "" if rc == 0 else " (%d failing checks kept as the baseline)" % len(keys)
                env = " ".join("%s=%s" % kv for kv in m.get("env", {}).items())
                say("  baseline %s %s: exit %s, %.0f s%s" % (tname, env, rc, res[2], note))
            baselines[key] = res
            return res

    def one(L, m):
        test = L["test"][m["test"]]
        r = apply(m, L["header"], read)
        if isinstance(r, str):
            return "no-match", r
        brc, bkeys, _, berr = baseline(L, m["test"], m)
        if berr is not None:
            return "skipped", "baseline does not build: " + berr
        wait_idle()
        with free_lock:
            d = free.pop()
        try:
            for f, t in r.items():
                with open(os.path.join(d, f), "w", encoding="utf-8", newline="\n") as fh:
                    fh.write(t)
            exe, err = build(test, d, tdir, d, "t")
            if not exe:
                return "build-failed", err
            rc, keys, out = run(exe, test, m, d)
            if rc is None:
                return "timeout", "after %s s" % test.get("timeout", 600)
            new = sorted(keys - bkeys)
            if new:
                return "killed", "%d new failing checks; first: %s" % (len(new), new[0])
            if rc != brc:
                return "killed", "exit %d (baseline %d)" % (rc, brc)
            return "survived", ""
        finally:
            for f in r:
                shutil.copy2(os.path.join(hdr, f), os.path.join(d, f))
            with free_lock:
                free.append(d)

    total_bad = 0
    t_all = time.time()
    for L in lists:
        name = os.path.basename(L["path"])
        ms = [m for m in L.get("mutant", []) if not a.only or m["id"] in a.only]
        if not ms:
            continue
        say("== %s (%s): %d mutants" % (name, L["header"], len(ms)))
        t0 = time.time()
        results = {}

        def task(m):
            st, detail = one(L, m)
            results[m["id"]] = (st, detail)
            exp = m.get("expect", "killed")
            ok = (st == "killed" or st == "timeout") if exp == "killed" else st == "survived"
            flag = "" if ok else "  <-- expected %s" % exp
            say("  %-10s %-12s %s%s%s" % (m["id"], st, m["name"], (": " + detail) if detail else "", flag))
            return ok

        with ThreadPoolExecutor(max_workers=jobs) as ex:
            oks = list(ex.map(task, ms))
        total_bad += oks.count(False)
        counts = {}
        for st, _ in results.values():
            counts[st] = counts.get(st, 0) + 1
        say("  %s: %s; %d of %d as expected; %.0f s" % (
            name, ", ".join("%s %d" % kv for kv in sorted(counts.items())), oks.count(True), len(oks), time.time() - t0))
    say("total %.0f s" % (time.time() - t_all))
    if not a.keep and not a.work:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if total_bad else 0


if __name__ == "__main__":
    sys.exit(main())
