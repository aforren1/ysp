"""ysp-vis: one extension, ysp._vis, for ysp.screen, ysp.gfx and ysp.video. Windows and macOS (arm64).

The natives come from tools/vendor_vis.py (docs/bindings.md, ysp-vis):

    uv run --no-project python tools/vendor_layout.py --write
    uv run --no-project python tools/vendor_vis.py --write --angle --sdl msvc --pl-mpeg --layout msvc    # Windows
    uv run --no-project python tools/vendor_vis.py --write --angle --sdl clang --pl-mpeg --layout clang  # macOS

A static SDL3 (third_party/vis/sdl3/<toolchain>) and the static text layout
(third_party/vis/layout/<toolchain>: pack/layout.c, Skribidi, HarfBuzz,
SheenBidi, libunibreak, budouxc) are linked in, and ANGLE's libEGL and
libGLESv2 (third_party/vis/angle: .dll on Windows, .dylib on macOS) go into
the wheel beside the extension, unchanged, with every license notice.
Linux is refused: ysp/screen.h's X11 and Wayland backends are stubs.
"""
import os
import shutil
import sys

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

HERE = os.path.abspath(os.path.dirname(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
MAC = sys.platform == "darwin"

if sys.platform != "win32" and not MAC:
    sys.exit("ysp-vis: Windows and macOS only in this release (ysp/screen.h's X11 and Wayland backends are stubs)")

# macOS 14 is ysp's floor (README), and the static SDL3 and text layout are
# built for it (tools/vendor_vis.py); a lower target would tag the wheel for
# systems where the METAL backend refuses to open.
if MAC:
    target = os.environ.get("MACOSX_DEPLOYMENT_TARGET", "")
    if not target or tuple(int(x) for x in target.split(".")[:2]) < (14, 0):
        os.environ["MACOSX_DEPLOYMENT_TARGET"] = "14.0"


# The repository's copies win in a development tree, so a stale staged copy
# cannot shadow them; an isolated build (cibuildwheel, an sdist) gets them
# staged next to this file (include/ysp, include/share, natives/).
def pick(repo_path, staged_path, probe):
    return repo_path if os.path.exists(os.path.join(repo_path, probe)) else staged_path


TOOLCHAIN = "clang" if MAC else "msvc"
DLL = ".dylib" if MAC else ".dll"
HEADER_DIR = pick(os.path.join(REPO_ROOT, "include"), os.path.join(HERE, "include"), os.path.join("ysp", "screen.h"))
SHARE_DIR = pick(os.path.join(REPO_ROOT, "bindings", "share"), os.path.join(HERE, "include", "share"), "ysp_vis_core.h")
NATIVES = pick(os.path.join(REPO_ROOT, "third_party", "vis"), os.path.join(HERE, "natives"),
               os.path.join("angle", "libEGL" + DLL))
SDL = os.path.join(NATIVES, "sdl3", TOOLCHAIN)
ANGLE = os.path.join(NATIVES, "angle")
PL_MPEG = os.path.join(NATIVES, "pl_mpeg")   # MPEG-1 for ysp/video.h
LAYOUT = os.path.join(NATIVES, "layout")      # text: pack/layout and its stack, static
LAYOUT_TC = os.path.join(LAYOUT, TOOLCHAIN)
# In link order (tools/vendor_vis.py, LAYOUT_LIBS).
LAYOUT_LIBS = ["ysp_vis_layout", "ysp_skribidi", "ysp_harfbuzz", "ysp_sheenbidi", "ysp_unibreak", "ysp_budoux"]
LAYOUT_LICENSES = ["Skribidi", "HarfBuzz", "SheenBidi", "libunibreak", "budouxc"]

SDL_LIB = "libSDL3.a" if MAC else "SDL3-static.lib"
LAYOUT_PROBE = "libysp_harfbuzz.a" if MAC else "ysp_harfbuzz.lib"
for need in (os.path.join(SDL, "lib", SDL_LIB), os.path.join(ANGLE, "libEGL" + DLL),
             os.path.join(ANGLE, "libGLESv2" + DLL), os.path.join(PL_MPEG, "pl_mpeg.h"),
             os.path.join(LAYOUT_TC, "lib", LAYOUT_PROBE)):
    if not os.path.exists(need):
        sys.exit("ysp-vis: %s is missing: run tools/vendor_layout.py --write, then tools/vendor_vis.py --write "
                 "--angle --sdl %s --pl-mpeg --layout %s (from the repository root)" % (need, TOOLCHAIN, TOOLCHAIN))

# The system libraries SDL3.dll imports itself (tools/vendor_vis.py, SDL_SYSTEM_LIBS).
SDL_SYSTEM_LIBS = ["user32", "gdi32", "winmm", "imm32", "ole32", "oleaut32", "version", "uuid", "advapi32",
                   "setupapi", "shell32", "dinput8"]
# What libSDL3.a needs on macOS (tools/vendor_vis.py, SDL_MAC_LINK, which its
# build checks against SDL's sdl3.pc). ysp/screen.h and ysp/video.h load
# their own frameworks (Metal, QuartzCore, AVFoundation) at run time; HarfBuzz
# builds without exceptions and RTTI but needs libc++'s operator new.
SDL_MAC_LINK = ["-framework", "CoreMedia", "-framework", "CoreVideo", "-framework", "Cocoa",
                "-weak_framework", "UniformTypeIdentifiers", "-framework", "IOKit", "-framework", "ForceFeedback",
                "-framework", "Carbon", "-framework", "CoreAudio", "-framework", "AudioToolbox",
                "-framework", "AVFoundation", "-framework", "Foundation", "-framework", "GameController",
                "-framework", "Metal", "-framework", "QuartzCore", "-weak_framework", "CoreHaptics",
                "-lpthread", "-lm"]

# Beside the extension in the wheel: ysp/screen.h loads libEGL from the
# module's own directory (desc.angle_dir), and libEGL loads libGLESv2 from
# its own.
NATIVE_FILES = [
    (os.path.join(ANGLE, "libEGL" + DLL), "libEGL" + DLL),
    (os.path.join(ANGLE, "libGLESv2" + DLL), "libGLESv2" + DLL),
    (os.path.join(ANGLE, "LICENSE.ANGLE.txt"), "ysp_vis-LICENSE.ANGLE.txt"),
    (os.path.join(NATIVES, "sdl3", "LICENSE.txt"), "ysp_vis-LICENSE.SDL3.txt"),
    (os.path.join(PL_MPEG, "LICENSE.txt"), "ysp_vis-LICENSE.pl_mpeg.txt"),
] + [(os.path.join(LAYOUT, "LICENSE.%s.txt" % n), "ysp_vis-LICENSE.%s.txt" % n) for n in LAYOUT_LICENSES]


class BuildExt(build_ext):
    def run(self):
        super().run()
        out = os.path.dirname(self.get_ext_fullpath("ysp._vis"))
        for src, name in NATIVE_FILES:
            # copyfile, not copy2: the dylibs keep their bytes (and their
            # SHA-256, tools/vendor_vis.py) and get the mode of a new file
            shutil.copyfile(src, os.path.join(out, name))
            if name.endswith(DLL):
                os.chmod(os.path.join(out, name), 0o755)
        # An editable install builds in place; the DLLs go there too.


if MAC:
    libraries = LAYOUT_LIBS + ["SDL3", "c++"]
    # after the Python build's own -mmacosx-version-min (11.0 in uv's CPython), so 14.0 wins
    extra_link_args = SDL_MAC_LINK + ["-mmacosx-version-min=14.0"]
    extra_compile_args = ["-mmacosx-version-min=14.0"]
else:
    libraries = LAYOUT_LIBS + ["SDL3-static"] + SDL_SYSTEM_LIBS
    extra_link_args = []
    extra_compile_args = []

ext = Extension(
    name="ysp._vis",
    sources=["ysp_vis_ext.c"],
    # ysp.gfx and ysp.video, included by ysp_vis_ext.c, and the shared core:
    # a change to a header there rebuilds the extension
    depends=["vis_gfx.c", "vis_text.c", "vis_video.c"] + [os.path.join(SHARE_DIR, n) for n in (
        "ysp_vis_core.h", "ysp_vis_gfx.h", "ysp_vis_text.h", "ysp_vis_video.h", "ysp_share.h", "ysp_audio_api.h",
        "ysp_timeline_api.h")],
    include_dirs=[HEADER_DIR, SHARE_DIR, os.path.join(SDL, "include"), PL_MPEG, os.path.join(LAYOUT_TC, "include")],
    define_macros=[("Py_LIMITED_API", "0x030A0000")],
    py_limited_api=True,
    library_dirs=[os.path.join(SDL, "lib"), os.path.join(LAYOUT_TC, "lib")],
    libraries=libraries,
    extra_compile_args=extra_compile_args,
    extra_link_args=extra_link_args,
)

# Metadata lives in pyproject.toml; this call carries the extension, the
# thin modules, the natives and the abi3 tag.
setup(
    ext_modules=[ext],
    packages=["ysp"],
    cmdclass={"build_ext": BuildExt},
    options={"bdist_wheel": {"py_limited_api": "cp310"}},
)
