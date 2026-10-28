import os
import sys
from setuptools import setup, Extension

HERE = os.path.abspath(os.path.dirname(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

# ysp/aep.h lives in include/ under the repo root; that copy always wins in a dev tree so a
# stale staged copy cannot shadow it. Isolated builds (sdist / cibuildwheel see
# only this directory) get the header staged in include/ysp/ next to this file by CI. One
# directory serves both cases: the async layer makes ysp/aep.h include
# ysp/rt.h, and the two headers are always staged together.
HEADER_DIR = os.path.join(REPO_ROOT, "include") if os.path.exists(os.path.join(REPO_ROOT, "include", "ysp", "aep.h")) else os.path.join(HERE, "include")

extra_compile_args = []
extra_link_args = []
libraries = []
if sys.platform != "win32":
    # The async layer's pump thread uses pthreads on Linux/macOS, and the
    # library needs libm there.
    extra_compile_args.append("-pthread")
    extra_link_args.append("-pthread")
    libraries.append("m")

# YAEP_MAX_TRIALS caps desc.max_trials, so a session or a data set longer
# than 512 trials needs a build with a larger value:
# YSP_AEP_MAX_TRIALS=1024 pip install . The history and the matrices are
# allocated per handle from desc.max_trials, so the cap costs nothing until
# it is used.
max_trials_env = os.environ.get("YSP_AEP_MAX_TRIALS", "512").strip()
try:
    MAX_TRIALS = int(max_trials_env)
except ValueError:
    sys.exit(f"YSP_AEP_MAX_TRIALS must be an integer, not {max_trials_env!r}")
if not 16 <= MAX_TRIALS <= 65536:
    sys.exit(f"YSP_AEP_MAX_TRIALS must be in 16..65536, not {MAX_TRIALS}")

# Target the CPython 3.10+ stable ABI (Limited API): one .abi3.so works across
# Python versions. The macro must match Py_LIMITED_API in the C source.
PY_LIMITED = 0x030A0000

# Dotted name: the extension is built as ysp/aep.abi3.so. There is no
# ysp/__init__.py anywhere, so `ysp` is a PEP 420 implicit namespace package and
# ysp-aep installs next to the other ysp-* distributions without owning it.
ext = Extension(
    name="ysp.aep",
    sources=["ysp_aep_ext.c"],
    include_dirs=[HEADER_DIR],  # for "ysp/aep.h" and the "ysp/rt.h" it includes
    # YAEP_ASYNC compiles the async layer (and ysp/rt.h's pump) in; the binding
    # always ships it, because a Python trial loop is where it pays.
    define_macros=[("Py_LIMITED_API", hex(PY_LIMITED)), ("YAEP_ASYNC", "1"),
                   ("YAEP_MAX_TRIALS", str(MAX_TRIALS))],
    py_limited_api=True,
    libraries=libraries,
    extra_compile_args=extra_compile_args,
    extra_link_args=extra_link_args,
)

# Metadata (name, version, description, readme, requires-python, license) lives
# in pyproject.toml only. setuptools >= 77 ignores a setup.py copy of a field
# pyproject.toml also declares, and warns about it, so two sources of truth here
# would silently drift. This call carries only what pyproject.toml cannot say:
# the C extension and the abi3 wheel tag.
setup(
    ext_modules=[ext],
    # `ysp` is a PEP 420 namespace: no __init__.py, nothing to package. An
    # explicit empty list also stops setuptools auto-discovery from treating
    # the ysp/ directory (kept for in-place builds) or tests/ as a package.
    packages=[],
    # Build a cp310-abi3 wheel rather than a version-specific one.
    # build_ext decides whether to recompile by file dates alone, so an object
    # left in build/ from another YSP_AEP_MAX_TRIALS (or another header) would
    # be linked as is. One C file is cheap to compile; always compile it.
    options={"bdist_wheel": {"py_limited_api": "cp310"},
             "build_ext": {"force": True}},
)
