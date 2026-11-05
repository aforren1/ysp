import os
from setuptools import setup, Extension

HERE = os.path.abspath(os.path.dirname(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

# ysp/pack.h lives in include/ under the repo root; that copy always wins in a
# dev tree so a stale staged copy cannot shadow it. Isolated builds (sdist /
# cibuildwheel see only this directory) get the headers staged in include/ysp/
# next to this file first. ysp/pack.h requires ysp/json.h beside it (for
# ypak_runs_key()'s numbers), so both are staged.
HEADER_DIR = os.path.join(REPO_ROOT, "include") if os.path.exists(os.path.join(REPO_ROOT, "include", "ysp", "pack.h")) else os.path.join(HERE, "include")

# Target the CPython 3.10+ stable ABI (Limited API): one .abi3.so works across
# Python versions. The macro must match Py_LIMITED_API in the C source.
PY_LIMITED = 0x030A0000

# Dotted name: the extension is built as ysp/pack.abi3.so. There is no
# ysp/__init__.py anywhere, so `ysp` is a PEP 420 implicit namespace package and
# the ysp-* distributions install together or separately.
ext = Extension(
    name="ysp.pack",
    sources=["ysp_pack_ext.c"],
    include_dirs=[HEADER_DIR],
    define_macros=[("Py_LIMITED_API", hex(PY_LIMITED))],
    py_limited_api=True,
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
    # the ysp/ directory (kept for in-place builds) as a package.
    packages=[],
    # Build a cp310-abi3 wheel rather than a version-specific one.
    options={"bdist_wheel": {"py_limited_api": "cp310"}},
)
