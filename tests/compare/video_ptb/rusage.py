"""Run a command; write its wall time, CPU times and peak RSS from wait4's rusage.

    python3 -I rusage.py OUT.txt CMD [ARGS...]

The child's exit code passes through. CPU counts all its threads."""
import os, subprocess, sys, time
out, cmd = sys.argv[1], sys.argv[2:]
t0 = time.monotonic()
p = subprocess.Popen(cmd)
_, st, ru = os.wait4(p.pid, 0)
wall = time.monotonic() - t0
with open(out, "w", newline="\n") as f:
    f.write("wall %.1f s; user %.1f s; sys %.1f s; cpu %.0f%% of a core; peak RSS %.0f MB\n"
            % (wall, ru.ru_utime, ru.ru_stime, 100 * (ru.ru_utime + ru.ru_stime) / wall, ru.ru_maxrss / 1024))
sys.exit(os.waitstatus_to_exitcode(st))
