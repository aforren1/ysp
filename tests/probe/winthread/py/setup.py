# Builds the probe extension (abi3, cp38+) against the probe copy of
# ysp/screen.h (tests/probe/winthread/include). Not a distribution: see README.md.
#   SDL3_ROOT=C:/sdk/SDL3-3.4.0 MINIAUDIO_DIR=... python setup.py build_ext --build-lib OUT
#   SDL3_STATIC=1: SDL3_ROOT holds include/ and lib/SDL3-static.lib (README.md)
import os
from setuptools import setup, Extension

HERE = os.path.abspath(os.path.dirname(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
SDL = os.environ["SDL3_ROOT"]
MA = os.environ["MINIAUDIO_DIR"]
STATIC = os.environ.get("SDL3_STATIC") == "1"

if STATIC:
    # the system libraries SDL3.dll imports itself
    libs = ["SDL3-static", "user32", "gdi32", "winmm", "imm32", "ole32", "oleaut32", "version", "uuid",
            "advapi32", "setupapi", "shell32", "dinput8"]
    libdirs = [os.path.join(SDL, "lib")]
else:
    libs = ["SDL3", "user32"]
    libdirs = [os.path.join(SDL, "lib", "x64")]

ext = Extension(
    name="ysp_av_probe",
    sources=["ysp_av_probe.c"],
    include_dirs=[os.path.join(HERE, "..", "include"), os.path.join(ROOT, "include"), os.path.join(SDL, "include"), MA],
    define_macros=[("Py_LIMITED_API", "0x03080000")],
    py_limited_api=True,
    library_dirs=libdirs,
    libraries=libs,
)
setup(name="ysp-av-probe", version="0", ext_modules=[ext], packages=[],
      options={"bdist_wheel": {"py_limited_api": "cp38"}})
