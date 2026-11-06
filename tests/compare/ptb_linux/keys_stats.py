"""Psychtoolbox KbQueue stamps against the kernel's evdev time.

    uv run --no-project python keys_stats.py KEYS.csv KEYS_log.csv LABEL

KEYS.csv from ptb_keys.m (GetSecs, CLOCK_REALTIME), KEYS_log.csv from
uinput_keys.c (w0 in both clocks, the kernel's time in CLOCK_MONOTONIC).
"""
import csv,sys,statistics as st
p=[float(r['time']) for r in csv.DictReader(open(sys.argv[1]))]
L=list(csv.DictReader(open(sys.argv[2])))
assert len(p)==len(L), (len(p), len(L))
d=[]
for t,r in zip(p,L):
    off=int(r['w0_real_ns'])-int(r['w0_mono_ns'])
    d.append((t*1e9-off-int(r['kernel_mono_ns']))/1e6)
s=sorted(d); n=len(s)
print("%-10s PTB - kernel  n %d  min %.3f  mean %.3f  p50 %.3f  p99 %.3f  max %.3f ms"%(sys.argv[3],n,s[0],st.mean(s),s[n//2],s[-(-n*99//100)-1],s[-1]))
