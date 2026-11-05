# Builds the probe extension (abi3, cp38+). Not a distribution: see README.md.
#   SDL3_ROOT=C:/sdk/SDL3-3.4.0 MINIAUDIO_DIR=... python setup.py build_ext --build-lib OUT
import os
from setuptools import setup, Extension

HERE = os.path.abspath(os.path.dirname(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
SDL = os.environ["SDL3_ROOT"]
MA = os.environ["MINIAUDIO_DIR"]

ext = Extension(
    name="ysp_av_probe",
    sources=["ysp_av_probe.c"],
    include_dirs=[os.path.join(ROOT, "include"), os.path.join(SDL, "include"), MA],
    define_macros=[("Py_LIMITED_API", "0x03080000")],
    py_limited_api=True,
    library_dirs=[os.path.join(SDL, "lib", "x64")],
    libraries=["SDL3", "user32"],
)
setup(name="ysp-av-probe", version="0", ext_modules=[ext], packages=[],
      options={"bdist_wheel": {"py_limited_api": "cp38"}})
