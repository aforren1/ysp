# ysp.color Python binding

A CPython extension for the [ysp/color.h](../../../include/ysp/color.h)
single-header library: the display calibration, conversions through it,
gamut questions, gamut mapping and a participant's luminance. It uses the
plain CPython C API and has no dependencies. It is built against the
**Limited API / stable ABI** (`Py_LIMITED_API = 0x030A0000`), so one
`ysp/color.abi3.so` works on CPython 3.10 and later.

The distribution is `ysp-color` and the module is `ysp.color`. `ysp` is a
[PEP 420](https://peps.python.org/pep-0420/) implicit namespace package (no
`__init__.py`), so `ysp-color` installs alone or next to the other `ysp-*`
distributions.

## How to install

From this directory:

```sh
uv pip install .                    # build and install an abi3 wheel
# or, in place for development:
python setup.py build_ext --inplace
```

In a development tree, setup.py finds `ysp/color.h` in the repository's
`include/` directory.
An isolated build (an sdist or a cibuildwheel run) sees only this directory.
Copy the header into `include/ysp/` next to `setup.py` first:

```sh
mkdir -p include/ysp && cp ../../../include/ysp/color.h include/ysp/
uv build --sdist
```

## How to convert a color

```python
import ysp.color as ycol

cal = ycol.Calibration.load(open("rig.yspcal", "rb").read())
cx = ycol.Context(cal, (0.5, 0.5, 0.5))          # the background

rgb, g = cx.to_rgb(ycol.dkl(elev=0, azim=90, contrast=0.1))
if not g.in_gamut:
    print("out by", g.distance, "; at most", g.scale, "of it fits")

d, g = cx.to_dir(ycol.dkl(azim=0, contrast=0.05))   # a grating's direction
k = cx.max_scale(ycol.dkl(azim=90, contrast=1), symmetric=True)
lab, g, flags = cx.convert(ycol.hex("#3366cc"), "cielab")
values, flags = cx.convert_n(rgb_array, "rgb", "oklch")   # N x 3 in, memoryview('d') out
```

A color is a `Color` namedtuple `(space, a, b, c)`: the space's name and its
three values in the header's order. Each space has a constructor with the
header's field names: `rgb`, `device`, `srgb`, `display_p3`, `rec2020`,
`xyz`, `xyy`, `cielab`, `cielch`, `cieluv`, `cielchuv`, `oklab`, `oklch`,
`lms`, `cone`, `dkl(elev, azim, contrast)`, `dkl_cart(lum, lm, s)`,
`mb(l, s, lum)`, and `hex("#3366cc")`. Any `(space, a, b, c)` tuple works
too. "cielab" is the CIE's Lab about the display's white; CSS's `lab()` has
a D50 white and is not the same.

A conversion the calibration cannot support raises `RefusedError` with the
reason ("needs the primaries' spectra"). A color outside the gamut is not an
error: the result is unclipped, and `Gamut` says by how much.

## How to set a contrast in light

A contrast threshold is a Michelson contrast of luminance, the display's
black included. `michelson()` gives ysp/gfx.h's contrast for it, the output
codes of the peak and the trough, and the contrast those codes show:

```python
sh = cx.michelson(0.03)               # dir (0, 0, 0): the background's
gabor_contrast = sh.scene             # ysp/gfx.h's contrast
staircase.update(sh.shown, correct)   # what the 8-bit codes showed
if not sh.gamut.in_gamut:
    print("past the gamut; the largest is", sh.max)
```

A contrast past the gamut is not clipped: `scene` is the request,
`gamut` says by how much it is outside, and `shown` is what the clamped
codes give. A direction with no luminance raises `RangeError`.

## How to measure a participant's luminance

```python
lum = ycol.Lum(participant="P01", method="hfp", freq_hz=15)
a, b = cx.lum_pair(azim=0, elev=elev_at_null, contrast=0.1)
lum.add(a, b, bg=(0.5, 0.5, 0.5), sd=0.8, n=6)
lum.derive(cal)
open("P01.ysplum", "wb").write(lum.save())
cx_p01 = ycol.Context(cal, (0.5, 0.5, 0.5), lum=lum)   # DKL with P01's isoluminance
```

## Reference

| Name | What it is |
|---|---|
| `Calibration()`, `.load(data)`, `.nominal(xy, white_Y, gamma)` | a calibration: empty, from a `.yspcal` file, or from stated primaries |
| `.add(gun, level, Y, x=0, y=0)`, `.set_spectra(start, step, r, g, b, black=None, nominal=False)`, `.derive()`, `.check()`, `.save()`, `.describe()` | ysp/color.h's calibration calls |
| `.flags`, `.crc`, `.rgb_to_xyz`, `.rgb_to_lms`, `.black_xyz`, `.black_lms`, `.lut`, `.n_readings`, `.white_err` | what derive() made |
| `Context(cal, background, *, cones="ss2", lum=None, white=None, src_white_Y=0, adapt="none")` | the conversion context |
| `.to_rgb(c)`, `.to_dir(c)`, `.from_rgb(rgb, space)`, `.convert(c, space)`, `.convert_n(values, src, dst)` | conversions |
| `.max_scale(c, symmetric=False)`, `.ring(plane, fixed, n, symmetric=False)`, `.map(c, method)` | gamut questions and mapping |
| `.michelson(m, dir=(0, 0, 0), bits=8)` | a Michelson contrast in light as ysp/gfx.h's contrast, and the contrast the output codes show |
| `.set_background(bg)`, `.lum_pair(azim, elev, contrast)`, `.describe()`, `.id`, `.can` | the rest |
| `Lum(cones, method, participant, field_deg, ecc_deg, freq_hz, fit_s)`, `.stated(w, cones)`, `.load(data)` | a participant's luminance |
| `.add(a, b, bg=None, sd=0, n=1)`, `.derive(cal)`, `.check(cal=None)`, `.save()`, `.describe()`, `.w`, `.resid` | its calls |
| `Gamut(status, in_gamut, below, above, margin, distance, scale, kept, why)` | the gamut of a result |
| `Shown(gain, scene, max, shown, Y_bg, Y_peak, Y_trough, peak, trough, gamut)` | `michelson()`'s result |
| `Error`, `ArgumentError`, `RefusedError`, `FormatError`, `RangeError` | the last four are also `ValueError` |

The header's manual is the full reference.
