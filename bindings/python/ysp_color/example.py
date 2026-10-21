"""ysp.color on a nominal display: conversions, gamut questions, mapping.

    python example.py

The calibration is nominal (sRGB's primaries, gamma 2.2, Gaussian spectra),
so no number here is a measurement of light.
"""
import math

import ysp.color as ycol

r, g, b = [], [], []
for i in range(401):
    nm = 380 + i
    r.append(0.010 * math.exp(-0.5 * ((nm - 612) / 14.0) ** 2))
    g.append(0.012 * math.exp(-0.5 * ((nm - 545) / 24.0) ** 2))
    b.append(0.014 * math.exp(-0.5 * ((nm - 455) / 12.0) ** 2))

cal = ycol.Calibration.nominal([(0.64, 0.33), (0.30, 0.60), (0.15, 0.06), (0.3127, 0.3290)], 100.0, 2.2)
cal.set_spectra(380, 1, r, g, b, nominal=True)
cal.derive()
cx = ycol.Context(cal, (0.5, 0.5, 0.5))
print(cx.describe())

for h in ("#3366cc", "#ff8800", "#808080"):
    rgb, gam = cx.to_rgb(ycol.hex(h))
    lch, _, _ = cx.convert(ycol.hex(h), "oklch")
    print(h, "rgb %.4f %.4f %.4f" % rgb, "in gamut" if gam.in_gamut else "OUT", ycol.format(lch))

for azim in range(0, 360, 45):
    print("DKL azimuth %3d: largest contrast %.4f" % (azim, cx.max_scale(ycol.dkl(azim=azim, contrast=1), symmetric=True)))

for method in ("scale", "chroma_oklch", "chroma_cielch", "chroma_dkl", "clip"):
    rgb, gam = cx.map(ycol.oklch(0.7, 0.3, 150), method)
    print("%-13s rgb %.4f %.4f %.4f kept %.4f" % ((method,) + rgb + (gam.kept,)))
