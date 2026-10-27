"""Least-squares periods of the OS's vblank times over windows of a gapdiag run
(the wander table of docs/screen.md, "The cause").
    uv run --no-project python seg.py CSV..."""
import csv, sys
from gapan import fit, NOM
for p in sys.argv[1:]:
    rows=[r for r in csv.DictReader(open(p))]
    comp=sorted([(float(r["t"])) for r in rows if r["src"]=="0" and float(r["t"])>0])
    t0=comp[0]
    pts=[(round((t-t0)/NOM),t) for t in comp]
    print(p)
    # per-window LS period (ppm vs mode) over 1 s windows for the first 10 s, then 10 s windows
    edges=[0,1,2,3,4,5,7.5,10,20,30,40,50,60,70,80,90,100]
    for i in range(len(edges)-1):
        seg=[(k,t) for k,t in pts if edges[i]*1e9<=t-t0<edges[i+1]*1e9]
        if len(seg)>20:
            a,b=fit(seg); res=max(abs(t-(a+b*k)) for k,t in seg)/1e3
            print("  %5.1f-%5.1f s: n %4d period %+.1f ppm, max residual %.1f us"%(edges[i],edges[i+1],len(seg),(b/NOM-1)*1e6,res))
