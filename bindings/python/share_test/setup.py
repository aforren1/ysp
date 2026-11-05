"""The test consumer of the ysp.audio and ysp.timeline tables (not shipped).

    uv build --wheel bindings/python/share_test     # after ysp-audio and ysp-timeline
"""
import os
from setuptools import setup, Extension

HERE = os.path.abspath(os.path.dirname(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
INC = os.path.join(REPO_ROOT, "include") if os.path.exists(os.path.join(REPO_ROOT, "include", "ysp", "audio.h")) else os.path.join(HERE, "include")
SHARE = os.path.join(REPO_ROOT, "bindings", "share") if os.path.exists(os.path.join(REPO_ROOT, "bindings", "share")) else os.path.join(HERE, "include", "share")

setup(
    ext_modules=[Extension(
        name="ysp_share_test",
        sources=["ysp_share_test.c"],
        include_dirs=[INC, SHARE],
        define_macros=[("Py_LIMITED_API", "0x030A0000")],
        py_limited_api=True,
    )],
    packages=[],
    options={"bdist_wheel": {"py_limited_api": "cp310"}},
)
