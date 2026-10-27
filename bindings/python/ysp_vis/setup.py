"""ysp-vis: one extension, ysp._vis, for ysp.screen, ysp.gfx and ysp.video. Windows only.

The natives come from tools/vendor_vis.py (docs/bindings.md, ysp-vis):

    uv run --no-project python tools/vendor_layout.py --write
    uv run --no-project python tools/vendor_vis.py --write --angle --sdl msvc --pl-mpeg --layout msvc

A static SDL3 (third_party/vis/sdl3/msvc) and the static text layout
(third_party/vis/layout/msvc: pack/layout.c, Skribidi, HarfBuzz, SheenBidi,
libunibreak, budouxc) are linked in, and ANGLE's libEGL.dll and
libGLESv2.dll (third_party/vis/angle) go into the wheel beside the
extension, with every license notice.
"""
import os
import shutil
import sys

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

HERE = os.path.abspath(os.path.dirname(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

if sys.platform != "win32":
    sys.exit("ysp-vis: Windows only in this release (ysp/screen.h's other backends are stubs)")

# The repository's copies win in a development tree, so a stale staged copy
# cannot shadow them; an isolated build (cibuildwheel, an sdist) gets them
# staged next to this file (include/ysp, include/share, natives/).
def pick(repo_path, staged_path, probe):
    return repo_path if os.path.exists(os.path.join(repo_path, probe)) else staged_path


HEADER_DIR = pick(os.path.join(REPO_ROOT, "include"), os.path.join(HERE, "include"), os.path.join("ysp", "screen.h"))
SHARE_DIR = pick(os.path.join(REPO_ROOT, "bindings", "share"), os.path.join(HERE, "include", "share"), "ysp_vis_core.h")
NATIVES = pick(os.path.join(REPO_ROOT, "third_party", "vis"), os.path.join(HERE, "natives"),
               os.path.join("angle", "libEGL.dll"))
SDL = os.path.join(NATIVES, "sdl3", "msvc")
ANGLE = os.path.join(NATIVES, "angle")
PL_MPEG = os.path.join(NATIVES, "pl_mpeg")   # MPEG-1 for ysp/video.h
LAYOUT = os.path.join(NATIVES, "layout")      # text: pack/layout and its stack, static
LAYOUT_MSVC = os.path.join(LAYOUT, "msvc")
# In link order (tools/vendor_vis.py, LAYOUT_LIBS).
LAYOUT_LIBS = ["ysp_vis_layout", "ysp_skribidi", "ysp_harfbuzz", "ysp_sheenbidi", "ysp_unibreak", "ysp_budoux"]
LAYOUT_LICENSES = ["Skribidi", "HarfBuzz", "SheenBidi", "libunibreak", "budouxc"]

for need in (os.path.join(SDL, "lib", "SDL3-static.lib"), os.path.join(ANGLE, "libEGL.dll"),
             os.path.join(ANGLE, "libGLESv2.dll"), os.path.join(PL_MPEG, "pl_mpeg.h"),
             os.path.join(LAYOUT_MSVC, "lib", "ysp_harfbuzz.lib")):
    if not os.path.exists(need):
        sys.exit("ysp-vis: %s is missing: run tools/vendor_layout.py --write, then tools/vendor_vis.py --write "
                 "--angle --sdl msvc --pl-mpeg --layout msvc (from the repository root)" % need)

# The system libraries SDL3.dll imports itself (tools/vendor_vis.py, SDL_SYSTEM_LIBS).
SDL_SYSTEM_LIBS = ["user32", "gdi32", "winmm", "imm32", "ole32", "oleaut32", "version", "uuid", "advapi32",
                   "setupapi", "shell32", "dinput8"]

# Beside the extension in the wheel: ysp/screen.h loads libEGL.dll from the
# module's own directory (desc.angle_dir), and libEGL loads libGLESv2.dll
# from its own.
NATIVE_FILES = [
    (os.path.join(ANGLE, "libEGL.dll"), "libEGL.dll"),
    (os.path.join(ANGLE, "libGLESv2.dll"), "libGLESv2.dll"),
    (os.path.join(ANGLE, "LICENSE.ANGLE.txt"), "ysp_vis-LICENSE.ANGLE.txt"),
    (os.path.join(NATIVES, "sdl3", "LICENSE.txt"), "ysp_vis-LICENSE.SDL3.txt"),
    (os.path.join(PL_MPEG, "LICENSE.txt"), "ysp_vis-LICENSE.pl_mpeg.txt"),
] + [(os.path.join(LAYOUT, "LICENSE.%s.txt" % n), "ysp_vis-LICENSE.%s.txt" % n) for n in LAYOUT_LICENSES]


class BuildExt(build_ext):
    def run(self):
        super().run()
        out = os.path.dirname(self.get_ext_fullpath("ysp._vis"))
        for src, name in NATIVE_FILES:
            shutil.copyfile(src, os.path.join(out, name))
        # An editable install builds in place; the DLLs go there too.


ext = Extension(
    name="ysp._vis",
    sources=["ysp_vis_ext.c"],
    depends=["vis_gfx.c", "vis_text.c", "vis_video.c"],   # ysp.gfx and ysp.video, included by ysp_vis_ext.c
    include_dirs=[HEADER_DIR, SHARE_DIR, os.path.join(SDL, "include"), PL_MPEG, os.path.join(LAYOUT_MSVC, "include")],
    define_macros=[("Py_LIMITED_API", "0x03080000")],
    py_limited_api=True,
    library_dirs=[os.path.join(SDL, "lib"), os.path.join(LAYOUT_MSVC, "lib")],
    libraries=LAYOUT_LIBS + ["SDL3-static"] + SDL_SYSTEM_LIBS,
)

# Metadata lives in pyproject.toml; this call carries the extension, the
# thin modules, the natives and the abi3 tag.
setup(
    ext_modules=[ext],
    packages=["ysp"],
    cmdclass={"build_ext": BuildExt},
    options={"bdist_wheel": {"py_limited_api": "cp38"}},
)
