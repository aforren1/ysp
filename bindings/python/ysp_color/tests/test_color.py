"""Tests for ysp.color: the binding's surface against ysp/color.h's own
numbers (tests/adapt/color_test.c checks the header itself)."""
import array
import math
import os
import re

import pytest

import ysp.color as ycol

SRGB_XY = [(0.64, 0.33), (0.30, 0.60), (0.15, 0.06), (0.3127, 0.3290)]


def gaussian_spectra():
    r, g, b = [], [], []
    for i in range(401):
        nm = 380 + i
        r.append(0.010 * math.exp(-0.5 * ((nm - 612) / 14.0) ** 2))
        g.append(0.012 * math.exp(-0.5 * ((nm - 545) / 24.0) ** 2))
        b.append(0.014 * math.exp(-0.5 * ((nm - 455) / 12.0) ** 2))
    return r, g, b


@pytest.fixture(scope="module")
def cal():
    c = ycol.Calibration.nominal(SRGB_XY, 80.0, 2.2)
    c.set_spectra(380, 1, *gaussian_spectra(), nominal=True)
    c.derive()
    return c


@pytest.fixture(scope="module")
def cx(cal):
    return ycol.Context(cal, (0.5, 0.5, 0.5))


def test_version_matches_pyproject():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    text = open(os.path.join(here, "pyproject.toml")).read()
    assert re.search(r'^version = "([^"]+)"', text, re.M).group(1) == ycol.__version__
    assert ycol.__version__.startswith(ycol.version() + ".")


def test_calibration_round_trip(cal):
    data = cal.save()
    assert len(data) == 62528
    back = ycol.Calibration.load(data)
    assert back.save() == data and back.crc == cal.crc
    assert back.flags & ycol.CAL_NOMINAL and back.flags & ycol.CAL_SPECTRA_NOMINAL
    assert "NOMINAL" in back.describe()
    bad = bytearray(data)
    bad[1000] ^= 1
    with pytest.raises(ycol.FormatError, match="CRC"):
        ycol.Calibration.load(bytes(bad))
    lut = cal.lut
    assert lut.format == "f" and len(lut) == 3 * 4096 and lut[0] == 0.0 and lut[4095] == 1.0


def test_readings_refused():
    c = ycol.Calibration()
    c.add(ycol.GUN_BLACK, 0, 0.5)
    for gun in range(3):
        c.add(gun, 0.5, 10)
        c.add(gun, 0.75, 9)
        c.add(gun, 1.0, 30)
    with pytest.raises(ycol.RangeError, match="0.75"):
        c.derive()


def test_conversions_and_gamut(cx):
    c = ycol.dkl(elev=0, azim=90, contrast=0.1)
    assert c == ycol.Color("dkl", 0.0, 90.0, 0.1)
    rgb, g = cx.to_rgb(c)
    assert g.status == 0 and g.in_gamut
    back = cx.from_rgb(rgb, "dkl")
    assert back.space == "dkl" and abs(back.c - 0.1) < 1e-12 and abs(back.b - 90) < 1e-9
    d, g = cx.to_dir(ycol.dkl(azim=0, contrast=0.05))
    assert all(abs(d[k] - (rgb[k] - 0.5)) < 1 for k in range(3))
    k = cx.max_scale(ycol.dkl(azim=90, contrast=1), symmetric=True)
    _, g = cx.to_dir(ycol.dkl(azim=90, contrast=k * 0.999))
    assert g.in_gamut
    _, g = cx.to_dir(ycol.dkl(azim=90, contrast=k * 1.01))
    assert not g.in_gamut and g.distance > 0
    lab, g, flags = cx.convert(ycol.hex("#3366cc"), "cielab")
    assert lab.space == "cielab" and 40 < lab.a < 50
    assert ycol.hex_format(cx.convert(ycol.hex("#3366cc"), "srgb")[0]) == "#3366cc"
    _, _, flags = cx.convert(ycol.xyz(30, 30, 30), "lms")
    assert flags & ycol.F_VIA_DEVICE


def test_srgb_primaries_in_gamut_on_nominal(cx):
    for h in ("#ff0000", "#00ff00", "#0000ff", "#ffffff", "#ff8800"):
        _, g = cx.to_rgb(ycol.hex(h))
        assert g.in_gamut, h


def test_convert_n_matches_single(cx):
    vals = array.array("d", [0.2, 0.4, 0.6, 0.9, 0.1, 0.3, 0.5, 0.5, 0.5])
    out, flags = cx.convert_n(vals, "rgb", "oklch")
    assert out.format == "d" and len(out) == 9 and len(flags) == 3
    for i in range(3):
        one, _, _ = cx.convert(ycol.rgb(*vals[3 * i:3 * i + 3]), "oklch")
        assert list(out[3 * i:3 * i + 3]) == [one.a, one.b, one.c]
    out, flags = cx.convert_n([(2.0, 0, 0)], "device", "rgb")
    assert flags[0] == 2 and math.isnan(out[0])


def test_refusals():
    lumo = ycol.Calibration()
    lumo.add(ycol.GUN_BLACK, 0, 0.2)
    for gun in range(3):
        for k in range(1, 9):
            lumo.add(gun, k / 8, 0.2 + 60 * (k / 8) ** 2.2)
    lumo.derive()
    cx = ycol.Context(lumo, (0.5, 0.5, 0.5))
    with pytest.raises(ycol.RefusedError, match="chromaticities"):
        cx.to_rgb(ycol.cielab(50, 0, 0))
    with pytest.raises(ycol.RefusedError, match="spectra"):
        cx.to_rgb(ycol.dkl(azim=90, contrast=0.1))
    with pytest.raises(ycol.ArgumentError):
        ycol.hex("#12345")
    with pytest.raises(ycol.ArgumentError):
        cx.to_rgb(("lab", 50, 0, 0))   # "cielab", not CSS's "lab"
    assert isinstance(ycol.RefusedError("x"), ValueError)


def test_map_and_ring(cx):
    rgb, g = cx.map(ycol.oklch(0.7, 0.3, 150), "chroma_oklch")
    assert not g.in_gamut and 0 < g.kept < 1
    _, g2 = cx.to_rgb(ycol.rgb(*rgb))
    assert g2.in_gamut
    with pytest.raises(ycol.RangeError, match="gray"):
        cx.map(ycol.oklch(1.3, 0.1, 30), "chroma_oklch")
    ring = cx.ring("dkl", 0.0, 8, symmetric=True)
    assert len(ring) == 8 and abs(ring[0] - ring[4]) < 1e-12
    assert ring[2] == cx.max_scale(ycol.dkl(azim=90, contrast=1), symmetric=True)


def test_lum_fit(cal, cx):
    w_true = (0.75, 0.30, 0.0)
    m = cx.rgb_to_lms
    u, v = [0.08, -0.06, 0.01], [0.03, 0.03, 0.03]
    mu = [sum(m[3 * i + j] * u[j] for j in range(3)) for i in range(3)]
    mv = [sum(m[3 * i + j] * v[j] for j in range(3)) for i in range(3)]
    t = -sum(w_true[i] * mu[i] for i in range(3)) / sum(w_true[i] * mv[i] for i in range(3))
    d = [u[k] + t * v[k] for k in range(3)]
    lum = ycol.Lum(participant="P01", method="hfp", freq_hz=15)
    lum.add([0.5 + x for x in d], [0.5 - x for x in d], bg=(0.5, 0.5, 0.5), sd=0.01, n=5)
    lum.derive(cal)
    n1 = math.sqrt(sum(x * x for x in lum.w))
    n2 = math.sqrt(sum(x * x for x in w_true))
    assert all(abs(lum.w[k] / n1 - w_true[k] / n2) < 1e-9 for k in range(3))
    back = ycol.Lum.load(lum.save())
    back.check(cal)
    assert back.participant == "P01" and "HFP" in back.describe()
    c2 = ycol.Context(cal, (0.5, 0.5, 0.5), lum=lum)
    assert c2.id != cx.id and "P01" in c2.describe()
    a, b = c2.lum_pair(0, 10, 0.05)
    assert all(abs((a[k] - 0.5) + (b[k] - 0.5)) < 1e-15 for k in range(3))
    with pytest.raises(ycol.RefusedError, match="SS10"):
        ycol.Context(cal, (0.5, 0.5, 0.5), lum=ycol.Lum.stated((0.7, 0.33, 0), cones="ss10"))


def test_tables_and_helpers(cal):
    spaces = ycol.spaces()
    assert [s["name"] for s in spaces][:3] == ["rgb", "device", "xyz"]
    assert ycol.SPACE_DKL == 16 and ycol.SPACE_DISPLAY_P3 == 12
    assert ycol.format(ycol.dkl(0, 90, 0.1)) == "dkl(0 90 0.10000000000000001)"
    assert ycol.cone_luminance("ss10")[:2] == (0.69283932, 0.34967567)
    l, m, s = ycol.cone_fundamentals(440)
    assert abs(l / 4.02563e-2 - 1) < 1e-7
    assert ycol.output_code(cal, (0.0, 1.0, 2.0)) == (0, 255, 255)
    assert ycol.GAMUT_EPS == 2.0 ** -20


def test_michelson():
    # ysp/color.h v0.4.0: a Michelson contrast in light, gfx's contrast and
    # the codes; a nominal display at 100 cd/m2 with a black of 0.5 cd/m2
    cal = ycol.Calibration()
    cal.add(ycol.GUN_BLACK, 0, 0.5)
    for gun in range(3):
        for k in range(1, 17):
            cal.add(gun, k / 16, 0.5 + 33.0 * (k / 16) ** 2.2)
    cal.derive()
    cx = ycol.Context(cal, (0.5, 0.5, 0.5))
    y_bg = 0.5 + 0.5 * 3 * 33.0
    sh = cx.michelson(0.05)
    assert isinstance(sh, ycol.Shown) and sh.gamut.in_gamut
    assert abs(sh.Y_bg - y_bg) < 1e-4
    assert abs(sh.gain - (y_bg - 0.5) / y_bg) < 1e-6
    assert abs(sh.scene - 0.05 / sh.gain) < 1e-15
    assert abs(sh.shown - (sh.Y_peak - sh.Y_trough) / (sh.Y_peak + sh.Y_trough)) < 1e-15
    assert abs(sh.shown - 0.05) < 0.007 and sh.peak[0] > sh.trough[0] and len(sh.trough) == 3
    # past the gamut: not clipped, reported; the clamped codes show the edge
    past = cx.michelson(sh.max * 1.5)
    assert not past.gamut.in_gamut and past.peak == (255, 255, 255) and past.trough == (0, 0, 0)
    assert abs(past.scene - 1.5) < 1e-12 and abs(past.shown - sh.max) < 1e-12
    # 0 shows 0; 16 bits; the opposite phase
    assert cx.michelson(0.0).shown == 0.0
    assert cx.michelson(0.05, bits=16).peak[0] > 255
    neg = cx.michelson(-0.05)
    assert neg.shown == -sh.shown and neg.peak == sh.trough
    # a colored dir, and an isoluminant one refused
    red = cx.michelson(0.02, dir=(0.2, 0, 0))
    assert red.peak[1] == red.trough[1] and red.gain < sh.gain
    with pytest.raises(ycol.RangeError, match="no luminance"):
        cx.michelson(0.02, dir=(1, -1, 0))
    with pytest.raises(ycol.ArgumentError, match="bits"):
        cx.michelson(0.02, bits=7)
