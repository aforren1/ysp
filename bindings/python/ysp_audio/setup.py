import hashlib
import os
import sys
import urllib.request
from setuptools import setup, Extension

HERE = os.path.abspath(os.path.dirname(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

# ysp/audio.h (with ysp/rt.h) lives in include/ under the repo root; that copy
# always wins in a dev tree so a stale staged copy cannot shadow it. Isolated
# builds (sdist / cibuildwheel see only this directory) get the headers staged
# in include/ysp/ next to this file first.
HEADER_DIR = os.path.join(REPO_ROOT, "include") if os.path.exists(os.path.join(REPO_ROOT, "include", "ysp", "audio.h")) else os.path.join(HERE, "include")

# The shared table (bindings/share/ysp_audio_api.h): the same rule, staged in
# include/share/.
SHARE_DIR = os.path.join(REPO_ROOT, "bindings", "share") if os.path.exists(os.path.join(REPO_ROOT, "bindings", "share", "ysp_audio_api.h")) else os.path.join(HERE, "include", "share")

# miniaudio.h 0.11.25, the pin of CMakeLists.txt's YSP_FETCH_MINIAUDIO. From
# YSP_MINIAUDIO_DIR, else staged in include/ (CI copies it there), else
# downloaded there and checked against the SHA-256 value.
MA_URL = "https://raw.githubusercontent.com/mackron/miniaudio/0.11.25/miniaudio.h"
MA_SHA256 = "ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89"


def miniaudio_dir():
    for d in (os.environ.get("YSP_MINIAUDIO_DIR"), os.path.join(HERE, "include")):
        if d and os.path.exists(os.path.join(d, "miniaudio.h")):
            with open(os.path.join(d, "miniaudio.h"), "rb") as f:
                if hashlib.sha256(f.read()).hexdigest() != MA_SHA256:
                    sys.exit("ysp-audio: %s/miniaudio.h is not miniaudio 0.11.25 (SHA-256 differs)" % d)
            return d
    d = os.path.join(HERE, "include")
    os.makedirs(d, exist_ok=True)
    data = urllib.request.urlopen(MA_URL, timeout=60).read()
    if hashlib.sha256(data).hexdigest() != MA_SHA256:
        sys.exit("ysp-audio: the downloaded miniaudio.h has another SHA-256 value; refused")
    with open(os.path.join(d, "miniaudio.h"), "wb") as f:
        f.write(data)
    return d


extra_compile_args = []
extra_link_args = []
libraries = []
if sys.platform != "win32":
    # miniaudio's device thread and ysp/rt.h's threads; miniaudio loads its
    # backends with dlopen. Windows links nothing: miniaudio loads its DLLs.
    extra_compile_args.append("-pthread")
    extra_link_args.append("-pthread")
    libraries += ["m"] + (["dl"] if sys.platform.startswith("linux") else [])

# Target the CPython 3.10+ stable ABI (Limited API): one .abi3.so works across
# Python versions. The macro must match Py_LIMITED_API in the C source.
PY_LIMITED = 0x030A0000

ext = Extension(
    name="ysp.audio",
    sources=["ysp_audio_ext.c"],
    include_dirs=[HEADER_DIR, SHARE_DIR, miniaudio_dir()],
    define_macros=[("Py_LIMITED_API", hex(PY_LIMITED))],
    py_limited_api=True,
    libraries=libraries,
    extra_compile_args=extra_compile_args,
    extra_link_args=extra_link_args,
)

# Metadata lives in pyproject.toml only; this call carries the C extension and
# the abi3 wheel tag.
setup(
    ext_modules=[ext],
    packages=[],
    options={"bdist_wheel": {"py_limited_api": "cp310"}},
)
