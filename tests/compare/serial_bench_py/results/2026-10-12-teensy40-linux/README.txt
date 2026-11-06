Linux runs of serial_bench.py and the C floor (tests/compare/serial_bench/raw_posix.c),
2026-10-12, the machine and board of
tests/compare/serial_pulse/results/2026-10-12-teensy40-linux/conditions.txt.
run1, run2: serial_bench.py, API order reversed in run 2, 13:17 to 13:24;
ysp-serial 0.6.1 built from this tree, psychtoolbox 3.0.22.2, pyserial 3.5,
CPython 3.13.15 (uv). --set absent_port="/dev/ttyACM250".
raw1, raw2: raw_posix forward and "rev", 13:50 to 13:52, after its stream probes
were spaced 2 to 3 ms apart (with 0 to 1 ms its stream fit had 2 points
and no slope).
