import os
import sys
from setuptools import setup, Extension

HERE = os.path.abspath(os.path.dirname(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

# The headers live in include/ under the repository root, and that copy wins in
# a development tree, so a stale staged copy cannot shadow it. An isolated build
# (an sdist, cibuildwheel) sees only this directory and gets them staged in
# include/ysp/ next to this file: ysp/rigfile.h and the headers it includes
# (device.h, json.h, rt.h, input.h, box.h, serial.h).
HEADER_DIR = (os.path.join(REPO_ROOT, "include")
              if os.path.exists(os.path.join(REPO_ROOT, "include", "ysp", "rigfile.h"))
              else os.path.join(HERE, "include"))

extra_compile_args = []
extra_link_args = []
libraries = []
if sys.platform == "win32":
    # ysp/serial.h lists ports with SetupAPI and the registry; a #pragma
    # reaches MSVC only, so name them for MinGW too.
    libraries += ["setupapi", "advapi32"]
else:
    # ysp/device.h, compiled in with ysp/rigfile.h, has threads
    extra_compile_args.append("-pthread")
    extra_link_args.append("-pthread")

PY_LIMITED = 0x03080000

# Dotted name: built as ysp/rigfile.abi3.so. `ysp` is a PEP 420 namespace (no
# __init__.py), shared with the other ysp-* distributions.
ext = Extension(
    name="ysp.rigfile",
    sources=["ysp_rigfile_ext.c"],
    include_dirs=[HEADER_DIR],
    define_macros=[("Py_LIMITED_API", hex(PY_LIMITED))],
    py_limited_api=True,
    libraries=libraries,
    extra_compile_args=extra_compile_args,
    extra_link_args=extra_link_args,
)

# Metadata lives in pyproject.toml only; this call carries the extension and
# the abi3 wheel tag.
setup(
    ext_modules=[ext],
    packages=[],
    options={"bdist_wheel": {"py_limited_api": "cp38"}},
)
