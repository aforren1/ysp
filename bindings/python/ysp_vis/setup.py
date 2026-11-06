"""ysp-vis: one extension, ysp._vis, for ysp.screen, ysp.gfx and ysp.video. Windows, macOS (arm64), Linux X11.

The natives come from tools/vendor_vis.py (docs/bindings.md, ysp-vis):

    uv run --no-project python tools/vendor_layout.py --write
    uv run --no-project python tools/vendor_vis.py --write --angle --sdl msvc --pl-mpeg --layout msvc    # Windows
    uv run --no-project python tools/vendor_vis.py --write --angle --sdl clang --pl-mpeg --layout clang  # macOS
    uv run --no-project python tools/vendor_vis.py --write --sdl gcc --pl-mpeg --layout gcc              # Linux

A static SDL3 (third_party/vis/sdl3/<toolchain>) and the static text layout
(third_party/vis/layout/<toolchain>: pack/layout.c, Skribidi, HarfBuzz,
SheenBidi, libunibreak, budouxc) are linked in, and ANGLE's libEGL and
libGLESv2 (third_party/vis/angle: .dll on Windows, .dylib on macOS) go into
the wheel beside the extension, unchanged, with every license notice.

On Linux (X11, the GLX_OML backend) there is no ANGLE: ysp/screen.h loads
the system's libGL, libX11 and libxcb at run time, and SDL3 is 3.4.18
(tools/vendor_vis.py says why). MP4 decoding through FFmpeg (ysp/video.h,
YVID_FFMPEG) is compiled in when pkg-config finds FFmpeg's development
files, or not when YSP_VIS_FFMPEG=0. ysp/video.h then loads the FFmpeg
libraries of the major versions it was built against at run time (BUILDING
in ysp/video.h), so such a build decodes MP4 only on a system with those.
"""
import os
import shutil
import subprocess
import sys

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

HERE = os.path.abspath(os.path.dirname(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
MAC = sys.platform == "darwin"
LINUX = sys.platform.startswith("linux")

if sys.platform != "win32" and not MAC and not LINUX:
    sys.exit("ysp-vis: Windows, macOS and Linux (X11) only")

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


TOOLCHAIN = "clang" if MAC else "gcc" if LINUX else "msvc"
DLL = ".dylib" if MAC else ".dll"
HEADER_DIR = pick(os.path.join(REPO_ROOT, "include"), os.path.join(HERE, "include"), os.path.join("ysp", "screen.h"))
SHARE_DIR = pick(os.path.join(REPO_ROOT, "bindings", "share"), os.path.join(HERE, "include", "share"), "ysp_vis_core.h")
NATIVES = pick(os.path.join(REPO_ROOT, "third_party", "vis"), os.path.join(HERE, "natives"),
               os.path.join("sdl3", TOOLCHAIN, "lib", "libSDL3.a") if LINUX else os.path.join("angle", "libEGL" + DLL))
SDL = os.path.join(NATIVES, "sdl3", TOOLCHAIN)
ANGLE = os.path.join(NATIVES, "angle")
PL_MPEG = os.path.join(NATIVES, "pl_mpeg")   # MPEG-1 for ysp/video.h
LAYOUT = os.path.join(NATIVES, "layout")      # text: pack/layout and its stack, static
LAYOUT_TC = os.path.join(LAYOUT, TOOLCHAIN)
# In link order (tools/vendor_vis.py, LAYOUT_LIBS).
LAYOUT_LIBS = ["ysp_vis_layout", "ysp_skribidi", "ysp_harfbuzz", "ysp_sheenbidi", "ysp_unibreak", "ysp_budoux"]
LAYOUT_LICENSES = ["Skribidi", "HarfBuzz", "SheenBidi", "libunibreak", "budouxc"]

SDL_LIB = "SDL3-static.lib" if TOOLCHAIN == "msvc" else "libSDL3.a"
LAYOUT_PROBE = "ysp_harfbuzz.lib" if TOOLCHAIN == "msvc" else "libysp_harfbuzz.a"
NEEDS = [os.path.join(SDL, "lib", SDL_LIB), os.path.join(PL_MPEG, "pl_mpeg.h"), os.path.join(LAYOUT_TC, "lib", LAYOUT_PROBE)]
if not LINUX:
    NEEDS += [os.path.join(ANGLE, "libEGL" + DLL), os.path.join(ANGLE, "libGLESv2" + DLL)]
for need in NEEDS:
    if not os.path.exists(need):
        sys.exit("ysp-vis: %s is missing: run tools/vendor_layout.py --write, then tools/vendor_vis.py --write "
                 "%s--sdl %s --pl-mpeg --layout %s (from the repository root)"
                 % (need, "" if LINUX else "--angle ", TOOLCHAIN, TOOLCHAIN))

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
NATIVE_FILES = ([] if LINUX else [
    (os.path.join(ANGLE, "libEGL" + DLL), "libEGL" + DLL),
    (os.path.join(ANGLE, "libGLESv2" + DLL), "libGLESv2" + DLL),
    (os.path.join(ANGLE, "LICENSE.ANGLE.txt"), "ysp_vis-LICENSE.ANGLE.txt"),
]) + [
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


define_macros = [("Py_LIMITED_API", "0x030A0000")]
include_dirs = [HEADER_DIR, SHARE_DIR, os.path.join(SDL, "include"), PL_MPEG, os.path.join(LAYOUT_TC, "include")]


def ffmpeg_cflags():
    """FFmpeg's include flags from pkg-config, or None (YSP_VIS_FFMPEG=0, or not found)."""
    if os.environ.get("YSP_VIS_FFMPEG", "") == "0":
        return None
    try:
        r = subprocess.run(["pkg-config", "--cflags", "libavformat", "libavcodec", "libavutil"],
                           capture_output=True, text=True)
    except OSError:
        return None
    if r.returncode:
        if os.environ.get("YSP_VIS_FFMPEG", "") == "1":
            sys.exit("ysp-vis: YSP_VIS_FFMPEG=1, but pkg-config finds no FFmpeg development files:\n" + r.stderr)
        return None
    return r.stdout.split()


if MAC:
    libraries = LAYOUT_LIBS + ["SDL3", "c++"]
    # after the Python build's own -mmacosx-version-min (11.0 in uv's CPython), so 14.0 wins
    extra_link_args = SDL_MAC_LINK + ["-mmacosx-version-min=14.0"]
    extra_compile_args = ["-mmacosx-version-min=14.0"]
elif LINUX:
    # SDL3's own list is -pthread -lm (tools/vendor_vis.py, the stamp);
    # HarfBuzz needs libstdc++'s operator new; dl for the run-time loads of
    # glibc before 2.34. --exclude-libs keeps the static SDL3 and the layout
    # stack out of the module's exported symbols, so a libSDL3.so another
    # module loaded first cannot take their calls.
    libraries = LAYOUT_LIBS + ["SDL3", "stdc++", "m", "dl"]
    extra_link_args = ["-pthread", "-Wl,--exclude-libs,ALL"]
    extra_compile_args = ["-pthread"]
    ff = ffmpeg_cflags()
    if ff is not None:
        define_macros.append(("YVID_FFMPEG", "1"))
        extra_compile_args += ff
        print("ysp-vis: YVID_FFMPEG on (pkg-config: %s)" % (" ".join(ff) or "default paths"))
    else:
        print("ysp-vis: YVID_FFMPEG off: MP4 is refused; frame sequences and MPEG-1 play")
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
    include_dirs=include_dirs,
    define_macros=define_macros,
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
