#!/usr/bin/env python3
"""Fetch and build the natives of ysp-vis (Python ysp.screen, MEX ysp_screen) in third_party/vis/.

    uv run --no-project python tools/vendor_vis.py                         # verify (default)
    uv run --no-project python tools/vendor_vis.py --write --angle          # ANGLE's two DLLs
    uv run --no-project python tools/vendor_vis.py --write --sdl msvc       # static SDL3, MSVC
    uv run --no-project python tools/vendor_vis.py --write --sdl mingw --mingw-bin DIR
    uv run --no-project python tools/vendor_vis.py --write --pl-mpeg        # pl_mpeg.h (MPEG-1)
    uv run --no-project python tools/vendor_vis.py --write --layout msvc    # text layout, MSVC
    uv run --no-project python tools/vendor_vis.py --write --layout mingw --mingw-bin DIR
    uv run --no-project python tools/vendor_vis.py --write --angle --sdl clang --pl-mpeg --layout clang   # macOS
    uv run --no-project python tools/vendor_vis.py --write --sdl gcc --pl-mpeg --layout gcc               # Linux
    ... --cache DIR       keep the downloads between runs (CI)

What it makes on Windows:

    third_party/vis/sdl3/msvc/    include/SDL3/, lib/SDL3-static.lib (/MD): the
                                  Python wheel and the MATLAB MEX files
    third_party/vis/sdl3/mingw/   include/SDL3/, lib/libSDL3.a: the Octave MEX
                                  files (build it with Octave's own gcc,
                                  mingw64/bin of the Octave install)
    third_party/vis/sdl3/LICENSE.txt     SDL's license (zlib)
    third_party/vis/angle/        libEGL.dll, libGLESv2.dll, LICENSE.ANGLE.txt
    third_party/vis/pl_mpeg/      pl_mpeg.h (MIT, its license in the file):
                                  ysp/video.h's MPEG-1 decoder, the pin of
                                  CMakeLists.txt's YSP_FETCH_PL_MPEG
    third_party/vis/layout/msvc/  lib/ (static, /MD) and include/ysp/layout.h:
                                  pack/layout.c with Skribidi, HarfBuzz,
                                  SheenBidi, libunibreak and budouxc, built by
                                  tools/vis_layout/CMakeLists.txt from the
                                  trees tools/vendor_layout.py writes (run it
                                  first): the text of ysp.gfx and ysp_gfx
    third_party/vis/layout/mingw/ the same with Octave's gcc
    third_party/vis/layout/LICENSE.*.txt   their license files

What it makes on macOS (arm64, macOS 14 and later: the METAL backend):

    third_party/vis/sdl3/clang/   include/SDL3/, lib/libSDL3.a, built with
                                  Apple clang for MACOSX_DEPLOYMENT_TARGET
                                  14.0 (the Python wheel and the MATLAB MEX
                                  files)
    third_party/vis/angle/        libEGL.dylib, libGLESv2.dylib (from the
                                  Electron zip for darwin-arm64, unchanged),
                                  LICENSE.ANGLE.txt
    third_party/vis/layout/clang/ the text layout, Apple clang, 14.0
    third_party/vis/pl_mpeg/      as on Windows

What it makes on Linux (x86_64, X11: the GLX_OML backend):

    third_party/vis/sdl3/gcc/     include/SDL3/, lib/libSDL3.a: SDL3 3.4.18,
                                  not 3.4.0 (before 3.4.18 SDL's X11 event
                                  handler can end the process on an X error,
                                  docs/screen.md), position-independent, with
                                  the X11 video driver and its XRandR, XInput2
                                  and XFixes parts (the build refuses without
                                  them) and without Wayland (ysp/screen.h's
                                  Wayland backend is a stub). Needs the X11
                                  development packages (Debian: libx11-dev
                                  libxext-dev libxrandr-dev libxi-dev
                                  libxfixes-dev libxcursor-dev libxss-dev)
    third_party/vis/layout/gcc/   the text layout, position-independent
    third_party/vis/pl_mpeg/      as on Windows

No ANGLE on Linux: GLX_OML makes a GL ES 3.0 context through the system's
libGL, which ysp/screen.h loads at run time with libX11 and libxcb.

Why static SDL3: a module that imports SDL3.dll binds to any SDL3.dll
already loaded in the process (docs/av_probe.md, section 5). Why ANGLE from
the Electron release: CI's screen job takes ANGLE from the same zip, so the
wheel ships the build CI tests (docs/bindings.md, ysp-vis).

Every download is checked by SHA-256, and so is each DLL taken from the
zip; nothing else is accepted. The SDL3 libraries are built here, so their
bytes depend on the compiler: a stamp file beside each records the tarball
hash and the compiler. Standard library only.

A download that breaks off is resumed where it stopped (an HTTP range
request), up to DOWNLOAD_ATTEMPTS times with a growing pause: the 136 MB
Electron zip failed twice in a row on 2026-10-27 with IncompleteRead and a
connection reset. The part file is kept in the cache folder, or in a
temporary folder without --cache. Only the complete file's SHA-256 decides.
"""
import argparse
import hashlib
import html
import http.client
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "third_party", "vis")

SDL_VERSION = "3.4.0"
SDL_URL = "https://github.com/libsdl-org/SDL/releases/download/release-%s/SDL3-%s.tar.gz" % (SDL_VERSION, SDL_VERSION)
SDL_SHA256 = "082cbf5f429e0d80820f68dc2b507a94d4cc1b4e70817b119bbb8ec6a69584b8"   # GitHub's digest of the asset

# The pin of .github/workflows/ci.yml's screen job (ELECTRON_VERSION).
ELECTRON_VERSION = "v38.8.6"   # ANGLE 2.1.25848 (git 85cc0e0b9cc3), as GL_VERSION reports it
ANGLE_VERSION = "2.1.25848 (git 85cc0e0b9cc3)"
ELECTRON_URL = "https://github.com/electron/electron/releases/download/%s/electron-%s-win32-x64.zip" % (
    ELECTRON_VERSION, ELECTRON_VERSION)
ELECTRON_SHA256 = "366ae2b4aa9e6bc89b98c8b5831d46303e45cd49b2970c2b3921de5f2fcfc2e1"
ANGLE_FILES = {
    "libEGL.dll": "09dc89e7a401f272d29ef445f1825f4ef4422c6844892a6d8a56108b47a69504",
    "libGLESv2.dll": "e5d96fb54dfbbf676b0b37c7915bcd907ff28f2ba63b49a72736a43ba3939a4e",
}

# macOS: ANGLE from the Electron release for darwin-arm64. 43.8.3 is the
# Electron of the METAL backend's runs (docs/screen_macos_probe.md); those
# used VS Code's copy, which Microsoft builds and signs itself, so its bytes
# differ. This zip's build (ANGLE 2.1.28042, git b4f8ea825136, as the
# describe line reports it) passed tests/adapt/screen_metal_test.c 42 of 42
# on 2026-11-05 (docs/bindings.md, ysp-vis). The two files are ad hoc
# signed by the linker and are installed unchanged, so their SHA-256 holds
# beside the module: ysp/screen.h opens libEGL by full path and libEGL
# opens libGLESv2 from its own folder, so no install name is used.
ELECTRON_MAC_VERSION = "v43.8.3"
ANGLE_MAC_VERSION = "2.1.28042 (git b4f8ea825136)"
ELECTRON_MAC_URL = "https://github.com/electron/electron/releases/download/%s/electron-%s-darwin-arm64.zip" % (
    ELECTRON_MAC_VERSION, ELECTRON_MAC_VERSION)
ELECTRON_MAC_SHA256 = "08e693ae36cfd230105aabe70c626b1a243c13534a529c35a80c1b87cb8da7b4"   # the release's SHASUMS256.txt
ANGLE_MAC_DIR = "Electron.app/Contents/Frameworks/Electron Framework.framework/Versions/A/Libraries/"
ANGLE_MAC_FILES = {
    "libEGL.dylib": "86f4ff17a99bae62e45f2b64b8ae3b1e2d6bf1f9f6e567d2e72e2df5b85f517d",
    "libGLESv2.dylib": "79c7eab0ca9a0411e5731fc6b65db8fa304ed0b5e353ee97b58196305f0c8aef",
}
# ysp's floor (README; CMakePresets.json macos-base, ci.yml).
MACOS_TARGET = "14.0"

# Linux: SDL3 3.4.18, the version ysp/screen.h asks for on X11 (BACKENDS,
# GLX_OML) and the pin of ci.yml's screen job. Windows and macOS keep 3.4.0.
SDL_LINUX_VERSION = "3.4.18"
SDL_LINUX_URL = "https://github.com/libsdl-org/SDL/releases/download/release-%s/SDL3-%s.tar.gz" % (
    SDL_LINUX_VERSION, SDL_LINUX_VERSION)
SDL_LINUX_SHA256 = "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3"   # GitHub's digest of the asset
# What SDL's X11 driver must have been built with: XRandR lists the displays
# and modes (yscr_displays(), yscr_modes()); XInput2 and XFixes give the
# relative mouse and the grab. Without the development packages SDL's
# configure leaves them out without an error.
SDL_LINUX_NEED = ["SDL_VIDEO_DRIVER_X11", "SDL_VIDEO_DRIVER_X11_XRANDR", "SDL_VIDEO_DRIVER_X11_XINPUT2",
                  "SDL_VIDEO_DRIVER_X11_XFIXES"]

# pl_mpeg has no release tags: the pin is a commit and the file's SHA-256,
# the same as CMakeLists.txt's YSP_FETCH_PL_MPEG and tools/vendor_pack.py.
PL_MPEG_COMMIT = "c871f2be022ece7ef4f64230b4fb8e1fb9eb6023"
PL_MPEG_URL = "https://raw.githubusercontent.com/phoboslab/pl_mpeg/%s/pl_mpeg.h" % PL_MPEG_COMMIT
PL_MPEG_SHA256 = "3a8cb30c83c2a1147719c30fe0c8b93da2987aa43140077c575b39aaa75fc2c9"

DOWNLOAD_ATTEMPTS = 8
DOWNLOAD_TIMEOUT = 60      # s without a byte before a read gives up
DOWNLOAD_PAUSE_MAX = 30    # s between attempts, at most

# The system libraries SDL3.dll imports itself (SDL's sdl3.pc, Libs.private):
# a module that links the static library links these too.
SDL_SYSTEM_LIBS = ["user32", "gdi32", "winmm", "imm32", "ole32", "oleaut32", "version", "uuid", "advapi32",
                   "setupapi", "shell32", "dinput8"]
# macOS: what libSDL3.a needs, as the clang build's sdl3.pc gives it (its
# Libs line after -lSDL3: a static-only build has no Libs.private); the
# build refuses a list that differs. setup.py and build.m link with the same.
SDL_MAC_LINK = ["-framework", "CoreMedia", "-framework", "CoreVideo", "-framework", "Cocoa",
                "-weak_framework", "UniformTypeIdentifiers", "-framework", "IOKit", "-framework", "ForceFeedback",
                "-framework", "Carbon", "-framework", "CoreAudio", "-framework", "AudioToolbox",
                "-framework", "AVFoundation", "-framework", "Foundation", "-framework", "GameController",
                "-framework", "Metal", "-framework", "QuartzCore", "-weak_framework", "CoreHaptics",
                "-lpthread", "-lm"]


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fetch_resumable(url, part, attempts=DOWNLOAD_ATTEMPTS, pause_max=DOWNLOAD_PAUSE_MAX):
    """Downloads url into the file part, resuming what part already holds.

    Each attempt asks for the bytes after those in part (Range); a server that
    answers 200 instead of 206 sends the whole file, and part starts over.
    Returns when the server's length is reached; exits after `attempts`
    attempts that each broke off.
    """
    last = None
    for attempt in range(1, attempts + 1):
        have = os.path.getsize(part) if os.path.exists(part) else 0
        headers = {"User-Agent": "ysp-vendor-vis"}
        if have:
            headers["Range"] = "bytes=%d-" % have
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=DOWNLOAD_TIMEOUT) as r:
                total = None
                if have and r.status == 206:
                    m = re.match(r"bytes (\d+)-\d+/(\d+|\*)", r.headers.get("Content-Range", ""))
                    if not m or int(m.group(1)) != have:
                        have = 0   # not the range asked for: start over
                    elif m.group(2) != "*":
                        total = int(m.group(2))
                else:
                    have = 0       # 200: the whole file
                if total is None and have == 0 and r.headers.get("Content-Length"):
                    total = int(r.headers["Content-Length"])
                with open(part, "ab" if have else "wb") as f:
                    while True:
                        b = r.read(1 << 20)
                        if not b:
                            break
                        f.write(b)
                got = os.path.getsize(part)
                if total is not None and got < total:
                    raise http.client.IncompleteRead(b"", total - got)
                return
        except urllib.error.HTTPError as e:
            if e.code == 416 and have:
                return     # part holds the whole file already: the hash decides
            if e.code not in (408, 429) and e.code < 500:
                sys.exit("error: %s: HTTP %d" % (url, e.code))
            last = e
        except (http.client.HTTPException, urllib.error.URLError, OSError) as e:
            # IncompleteRead, a reset connection, a timeout
            last = e
        if attempt < attempts:
            pause = min(2 ** attempt, pause_max)
            print("  attempt %d of %d broke off at %d bytes (%s: %s); resuming in %d s"
                  % (attempt, attempts, os.path.getsize(part) if os.path.exists(part) else 0,
                     type(last).__name__, last, pause))
            time.sleep(pause)
    sys.exit("error: %s did not download in %d attempts (last: %s: %s)" % (url, attempts, type(last).__name__, last))


def download(url, want, cache):
    name = url.rsplit("/", 1)[1]
    if cache:
        p = os.path.join(cache, name)
        if os.path.exists(p):
            with open(p, "rb") as f:
                data = f.read()
            if sha256(data) == want:
                return data
    print("downloading %s" % url)
    work = cache or tempfile.mkdtemp(prefix="ysp-vendor-vis-")
    os.makedirs(work, exist_ok=True)
    part = os.path.join(work, name + ".part")
    try:
        fetch_resumable(url, part)
        with open(part, "rb") as f:
            data = f.read()
        got = sha256(data)
        if got != want:
            os.remove(part)   # the next run starts over, not from these bytes
            sys.exit("error: %s has SHA-256 %s, expected %s; refused" % (url, got, want))
        if cache:
            os.replace(part, os.path.join(cache, name))
    finally:
        if not cache:
            shutil.rmtree(work, ignore_errors=True)
    return data


# ---- pl_mpeg -------------------------------------------------------------------

def write_pl_mpeg(cache):
    data = download(PL_MPEG_URL, PL_MPEG_SHA256, cache)
    dest = os.path.join(OUT, "pl_mpeg")
    os.makedirs(dest, exist_ok=True)
    with open(os.path.join(dest, "pl_mpeg.h"), "wb") as f:
        f.write(data)
    # The repository has no LICENSE file at the pin; the header's own lines
    # are the notice, and the wheel and the MEX directory ship them.
    head = data.decode("utf-8", "replace").split("\n")[1:5]
    with open(os.path.join(dest, "LICENSE.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("pl_mpeg.h, commit %s (%s), compiled into ysp-vis.\nIts notice, as the file states it:\n\n%s\n\n"
                "SPDX MIT: Permission is hereby granted, free of charge, to any person obtaining a copy of this\n"
                "software and associated documentation files (the \"Software\"), to deal in the Software without\n"
                "restriction, including without limitation the rights to use, copy, modify, merge, publish,\n"
                "distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the\n"
                "Software is furnished to do so, subject to the following conditions:\n\n"
                "The above copyright notice and this permission notice shall be included in all copies or\n"
                "substantial portions of the Software.\n\n"
                "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING\n"
                "BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND\n"
                "NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,\n"
                "DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
                "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.\n"
                % (PL_MPEG_COMMIT, PL_MPEG_URL, "\n".join(head).strip()))
    print("ok: pl_mpeg %s in %s" % (PL_MPEG_COMMIT[:7], os.path.relpath(dest, ROOT)))


# ---- ANGLE ---------------------------------------------------------------------

def angle_notice(chromium_html):
    """ANGLE's license as LICENSES.chromium.html of the same zip carries it."""
    text = chromium_html.decode("utf-8", "replace")
    m = re.search(r'<span class="title">Almost Native Graphics Layer Engine</span>.*?<pre>(.*?)</pre>', text, re.S)
    if not m:
        sys.exit("error: the Electron zip's LICENSES.chromium.html has no ANGLE section")
    return html.unescape(m.group(1))


def write_angle(cache):
    if sys.platform == "darwin":
        return write_angle_mac(cache)
    data = download(ELECTRON_URL, ELECTRON_SHA256, cache)
    dest = os.path.join(OUT, "angle")
    os.makedirs(dest, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for name, want in ANGLE_FILES.items():
            b = z.read(name)
            if sha256(b) != want:
                sys.exit("error: %s in the Electron zip has SHA-256 %s, expected %s" % (name, sha256(b), want))
            with open(os.path.join(dest, name), "wb") as f:
                f.write(b)
        notice = angle_notice(z.read("LICENSES.chromium.html"))
    with open(os.path.join(dest, "LICENSE.ANGLE.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("ANGLE %s, libEGL.dll and libGLESv2.dll from the Electron %s release for win32-x64\n"
                "(%s).\n\nANGLE's license, as that release's LICENSES.chromium.html gives it. ANGLE\n"
                "compiles code of other projects into libGLESv2.dll; their notices are in\n"
                "LICENSES.chromium.html of the same release.\n\n%s" % (ANGLE_VERSION, ELECTRON_VERSION,
                                                                       ELECTRON_URL, notice))
    print("ok: ANGLE %s in %s" % (ANGLE_VERSION, os.path.relpath(dest, ROOT)))


def write_angle_mac(cache):
    data = download(ELECTRON_MAC_URL, ELECTRON_MAC_SHA256, cache)
    dest = os.path.join(OUT, "angle")
    os.makedirs(dest, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for name, want in ANGLE_MAC_FILES.items():
            b = z.read(ANGLE_MAC_DIR + name)
            if sha256(b) != want:
                sys.exit("error: %s in the Electron zip has SHA-256 %s, expected %s" % (name, sha256(b), want))
            p = os.path.join(dest, name)
            with open(p, "wb") as f:
                f.write(b)
            os.chmod(p, 0o755)
        notice = angle_notice(z.read("LICENSES.chromium.html"))
    with open(os.path.join(dest, "LICENSE.ANGLE.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("ANGLE %s, libEGL.dylib and libGLESv2.dylib from the Electron %s release for darwin-arm64\n"
                "(%s).\n\nANGLE's license, as that release's LICENSES.chromium.html gives it. ANGLE\n"
                "compiles code of other projects into libGLESv2.dylib; their notices are in\n"
                "LICENSES.chromium.html of the same release.\n\n%s" % (ANGLE_MAC_VERSION, ELECTRON_MAC_VERSION,
                                                                         ELECTRON_MAC_URL, notice))
    print("ok: ANGLE %s in %s" % (ANGLE_MAC_VERSION, os.path.relpath(dest, ROOT)))


# ---- SDL3 ----------------------------------------------------------------------

def find_ninja():
    p = shutil.which("ninja")
    if p:
        return p
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if os.path.exists(vswhere):
        out = subprocess.run([vswhere, "-latest", "-products", "*", "-property", "installationPath"],
                             capture_output=True, text=True).stdout.strip()
        if out:
            p = os.path.join(out, "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "Ninja", "ninja.exe")
            if os.path.exists(p):
                return p
    return None


def find_vcvars():
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.exists(vswhere):
        return None
    out = subprocess.run([vswhere, "-latest", "-products", "*", "-requires",
                          "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                         capture_output=True, text=True).stdout.strip()
    p = os.path.join(out, "VC", "Auxiliary", "Build", "vcvars64.bat") if out else ""
    return p if p and os.path.exists(p) else None


def run(cmds, env=None, vcvars=None):
    """Runs each command; with vcvars, all of them in one cmd.exe after vcvars64.bat."""
    if vcvars:
        fd, bat = tempfile.mkstemp(suffix=".bat")
        with os.fdopen(fd, "w") as f:
            f.write('@echo off\r\ncall "%s" >nul || exit /b 1\r\n' % vcvars)
            for c in cmds:
                f.write(subprocess.list2cmdline(c) + " || exit /b 1\r\n")
        try:
            r = subprocess.run(["cmd", "/c", bat], env=env)
        finally:
            os.remove(bat)
        if r.returncode:
            sys.exit("error: the build failed (exit %d)" % r.returncode)
        return
    for c in cmds:
        r = subprocess.run(c, env=env)
        if r.returncode:
            sys.exit("error: %s failed (exit %d)" % (c[0], r.returncode))


def compiler_id(toolchain, mingw_bin, env):
    if toolchain == "msvc":
        return "msvc"
    if toolchain == "clang":
        v = subprocess.run(["clang", "--version"], capture_output=True, text=True).stdout.splitlines()
        return "%s, MACOSX_DEPLOYMENT_TARGET %s, arm64" % (v[0] if v else "clang", MACOS_TARGET)
    if toolchain == "gcc":
        v = subprocess.run(["gcc", "--version"], capture_output=True, text=True).stdout.splitlines()
        g = subprocess.run(["ldd", "--version"], capture_output=True, text=True).stdout.splitlines()
        return "%s, %s" % (v[0] if v else "gcc", g[0] if g else "glibc")
    return subprocess.run([os.path.join(mingw_bin, "gcc.exe"), "-dumpfullversion"], capture_output=True, text=True,
                          env=env).stdout.strip()


def sdl_pc_libs(prefix):
    """What the build's sdl3.pc links besides libSDL3.a itself (Libs and
    Libs.private), with -Wl, spellings split into plain linker arguments."""
    with open(os.path.join(prefix, "lib", "pkgconfig", "sdl3.pc"), encoding="utf-8") as f:
        text = f.read()
    out = []
    for m in re.finditer(r"^Libs(?:\.private)?:(.*)$", text, re.M):
        for x in m.group(1).split():
            if x.startswith("-L") or x == "-lSDL3":
                continue
            out += x[4:].split(",") if x.startswith("-Wl,") else [x]
    return out


def check_sdl_mac_link(prefix):
    got = sdl_pc_libs(prefix)
    if got != SDL_MAC_LINK:
        sys.exit("error: SDL3's sdl3.pc links %r; tools/vendor_vis.py's SDL_MAC_LINK is %r: update it"
                 % (got, SDL_MAC_LINK))


def sdl_pin(toolchain):
    """(version, url, sha256) of the SDL3 source a toolchain builds."""
    if toolchain == "gcc":
        return SDL_LINUX_VERSION, SDL_LINUX_URL, SDL_LINUX_SHA256
    return SDL_VERSION, SDL_URL, SDL_SHA256


def check_sdl_linux(build):
    """Refuses an SDL build whose X11 driver lacks a part SDL_LINUX_NEED names.
    SDL_build_config.h stays in the build tree; it is not installed."""
    found = [os.path.join(d, "SDL_build_config.h") for d, _, files in os.walk(build) if "SDL_build_config.h" in files]
    if len(found) != 1:
        sys.exit("error: %d SDL_build_config.h in SDL3's build tree, expected 1" % len(found))
    with open(found[0], encoding="utf-8") as f:
        text = f.read()
    missing = [n for n in SDL_LINUX_NEED if not re.search(r"^#define %s\b" % n, text, re.M)]
    if missing:
        sys.exit("error: SDL3 was built without %s: install the X11 development packages (the docstring)"
                 % ", ".join(missing))


def write_sdl(toolchain, cache, mingw_bin, jobs):
    version, url, want = sdl_pin(toolchain)
    data = download(url, want, cache)
    prefix = os.path.join(OUT, "sdl3", toolchain)
    work = tempfile.mkdtemp(prefix="ysp-sdl3-%s-" % toolchain)
    try:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as t:
            if hasattr(tarfile, "data_filter"):
                t.extractall(work, filter="data")
            else:
                t.extractall(work)
        src = os.path.join(work, "SDL3-%s" % version)
        with open(os.path.join(src, "LICENSE.txt"), "rb") as f:
            lic = f.read()
        os.makedirs(os.path.join(OUT, "sdl3"), exist_ok=True)
        with open(os.path.join(OUT, "sdl3", "LICENSE.txt"), "wb") as f:
            f.write(lic)
        build = os.path.join(work, "build")
        ninja = find_ninja()
        if not ninja:
            sys.exit("error: ninja not found (PATH, or Visual Studio's CMake extension)")
        cfg = ["cmake", "-S", src, "-B", build, "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + ninja,
               "-DCMAKE_BUILD_TYPE=Release", "-DSDL_SHARED=OFF", "-DSDL_STATIC=ON", "-DSDL_TEST_LIBRARY=OFF",
               "-DSDL_TESTS=OFF", "-DSDL_EXAMPLES=OFF", "-DSDL_INSTALL_DOCS=OFF", "-DCMAKE_INSTALL_PREFIX=" + prefix]
        bld = ["cmake", "--build", build, "-j", str(jobs)]
        ins = ["cmake", "--install", build]
        env = dict(os.environ)
        if os.path.isdir(prefix):
            shutil.rmtree(prefix)
        if toolchain == "msvc":
            # /MD, the C runtime of Python extensions and MEX files.
            cfg += ["-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL", "-DCMAKE_C_COMPILER=cl"]
            vcvars = None if shutil.which("cl") else find_vcvars()
            if not shutil.which("cl") and not vcvars:
                sys.exit("error: no MSVC: run from a developer prompt or install Visual Studio's C++ tools")
            run([cfg, bld, ins], env=env, vcvars=vcvars)
        elif toolchain == "clang":
            # Apple clang for ysp's floor, arm64 (the zip's ANGLE is arm64).
            cfg += ["-DCMAKE_C_COMPILER=clang", "-DCMAKE_OSX_DEPLOYMENT_TARGET=" + MACOS_TARGET,
                    "-DCMAKE_OSX_ARCHITECTURES=arm64"]
            env["MACOSX_DEPLOYMENT_TARGET"] = MACOS_TARGET
            run([cfg, bld, ins], env=env)
            check_sdl_mac_link(prefix)
        elif toolchain == "gcc":
            # Position-independent: it is linked into a shared object (the
            # extension, the MEX file). SDL loads libX11 and its extensions
            # at run time (SDL_X11_SHARED, its default), so the module links
            # no X library.
            cfg += ["-DCMAKE_C_COMPILER=gcc", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON", "-DSDL_X11=ON",
                    "-DSDL_WAYLAND=OFF"]
            run([cfg, bld, ins], env=env)
            check_sdl_linux(build)
        else:
            if not mingw_bin:
                sys.exit("error: --mingw-bin DIR: the mingw64/bin of the gcc that builds the MEX files "
                         "(Octave's own, for Octave)")
            env["PATH"] = mingw_bin + os.pathsep + env.get("PATH", "")
            # Forward slashes: CMake reads a backslash in a path as an escape.
            mb = mingw_bin.replace("\\", "/")
            cfg += ["-DCMAKE_C_COMPILER=%s/gcc.exe" % mb, "-DCMAKE_CXX_COMPILER=%s/g++.exe" % mb,
                    "-DCMAKE_RC_COMPILER=%s/windres.exe" % mb]
            run([cfg, bld, ins], env=env)
        cc = compiler_id(toolchain, mingw_bin, env)
        link = SDL_MAC_LINK if toolchain == "clang" else sdl_pc_libs(prefix) if toolchain == "gcc" else SDL_SYSTEM_LIBS
        with open(os.path.join(prefix, "STAMP.txt"), "w", newline="\n") as f:
            f.write("SDL3 %s static, %s\nsource %s\nsha256 %s\ncompiler %s\nlink also: %s\n" % (
                version, toolchain, url, want, cc, " ".join(link)))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("ok: static SDL3 %s (%s) in %s" % (version, toolchain, os.path.relpath(prefix, ROOT)))


# ---- text layout (pack/layout.c and third_party/'s layout stack) -------------

# The license files of the layout stack (third_party/README.md), shipped
# beside the module as ysp_vis-LICENSE.<name>.txt.
LAYOUT_LICENSES = [
    ("Skribidi", ["skribidi/LICENSE"]),
    ("HarfBuzz", ["harfbuzz/COPYING", "harfbuzz/src/ms-use/COPYING"]),
    ("SheenBidi", ["sheenbidi/LICENSE"]),
    ("libunibreak", ["libunibreak/LICENCE"]),
    ("budouxc", ["budouxc/LICENSE", "budouxc/LICENSE-BudouX"]),
]
# In link order (MinGW's ld resolves left to right).
LAYOUT_LIBS = ["ysp_vis_layout", "ysp_skribidi", "ysp_harfbuzz", "ysp_sheenbidi", "ysp_unibreak", "ysp_budoux"]


def layout_source_hash():
    """The SHA-256 of third_party/MANIFEST.sha256 and the patches: what the
    libraries were built from (the stamp records it; verify compares)."""
    tp = os.path.join(ROOT, "third_party")
    h = hashlib.sha256()
    names = ["MANIFEST.sha256"] + sorted("patches/" + n for n in os.listdir(os.path.join(tp, "patches")))
    for n in names:
        with open(os.path.join(tp, n), "rb") as f:
            h.update(n.encode() + b"\0" + f.read())
    return h.hexdigest()


def write_layout(toolchain, mingw_bin, jobs):
    tp = os.path.join(ROOT, "third_party")
    if not os.path.exists(os.path.join(tp, "harfbuzz", "src", "harfbuzz.cc")):
        sys.exit("error: third_party/ has no layout sources: run tools/vendor_layout.py --write first")
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "vendor_layout.py")], capture_output=True, text=True)
    if r.returncode:
        sys.exit("error: tools/vendor_layout.py does not verify third_party/:\n%s%s" % (r.stdout, r.stderr))
    prefix = os.path.join(OUT, "layout", toolchain)
    work = tempfile.mkdtemp(prefix="ysp-layout-%s-" % toolchain)
    try:
        ninja = find_ninja()
        if not ninja:
            sys.exit("error: ninja not found (PATH, or Visual Studio's CMake extension)")
        cfg = ["cmake", "-S", os.path.join(ROOT, "tools", "vis_layout"), "-B", work, "-G", "Ninja",
               "-DCMAKE_MAKE_PROGRAM=" + ninja, "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_INSTALL_PREFIX=" + prefix]
        bld = ["cmake", "--build", work, "-j", str(jobs)]
        ins = ["cmake", "--install", work]
        env = dict(os.environ)
        if os.path.isdir(prefix):
            shutil.rmtree(prefix)
        if toolchain == "msvc":
            cfg += ["-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl"]
            vcvars = None if shutil.which("cl") else find_vcvars()
            if not shutil.which("cl") and not vcvars:
                sys.exit("error: no MSVC: run from a developer prompt or install Visual Studio's C++ tools")
            run([cfg, bld, ins], env=env, vcvars=vcvars)
            cc = "msvc"
        elif toolchain == "clang":
            cfg += ["-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
                    "-DCMAKE_OSX_DEPLOYMENT_TARGET=" + MACOS_TARGET, "-DCMAKE_OSX_ARCHITECTURES=arm64"]
            env["MACOSX_DEPLOYMENT_TARGET"] = MACOS_TARGET
            run([cfg, bld, ins], env=env)
            cc = compiler_id(toolchain, mingw_bin, env)
        elif toolchain == "gcc":
            cfg += ["-DCMAKE_C_COMPILER=gcc", "-DCMAKE_CXX_COMPILER=g++", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"]
            run([cfg, bld, ins], env=env)
            cc = compiler_id(toolchain, mingw_bin, env)
        else:
            if not mingw_bin:
                sys.exit("error: --mingw-bin DIR: the mingw64/bin of the gcc that builds the MEX files "
                         "(Octave's own, for Octave)")
            env["PATH"] = mingw_bin + os.pathsep + env.get("PATH", "")
            mb = mingw_bin.replace("\\", "/")
            cfg += ["-DCMAKE_C_COMPILER=%s/gcc.exe" % mb, "-DCMAKE_CXX_COMPILER=%s/g++.exe" % mb]
            run([cfg, bld, ins], env=env)
            cc = subprocess.run([os.path.join(mingw_bin, "gcc.exe"), "-dumpfullversion"], capture_output=True,
                                text=True, env=env).stdout.strip()
        with open(os.path.join(prefix, "STAMP.txt"), "w", newline="\n") as f:
            f.write("ysp-vis text layout, static, %s\nthird_party sha256 %s\ncompiler %s\nlink, in order: %s\n" % (
                toolchain, layout_source_hash(), cc, " ".join(LAYOUT_LIBS)))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    lic = os.path.join(OUT, "layout")
    for name, files in LAYOUT_LICENSES:
        parts = []
        for rel in files:
            with open(os.path.join(tp, rel), "r", encoding="utf-8", errors="replace") as f:
                parts.append("---- third_party/%s ----\n\n%s" % (rel, f.read().replace("\r\n", "\n")))
        with open(os.path.join(lic, "LICENSE.%s.txt" % name), "w", encoding="utf-8", newline="\n") as f:
            f.write("%s, compiled into the text layout of ysp-vis (third_party/README.md has the pin).\n\n%s" % (
                name, "\n".join(parts)))
    print("ok: text layout (%s) in %s" % (toolchain, os.path.relpath(prefix, ROOT)))


# ---- verify ------------------------------------------------------------------

def verify():
    bad = 0
    angle = {} if sys.platform.startswith("linux") else ANGLE_MAC_FILES if sys.platform == "darwin" else ANGLE_FILES
    for name, want in angle.items():
        p = os.path.join(OUT, "angle", name)
        if not os.path.exists(p):
            print("missing: %s (--write --angle)" % os.path.relpath(p, ROOT))
            bad += 1
            continue
        with open(p, "rb") as f:
            got = sha256(f.read())
        if got != want:
            print("differs: %s has SHA-256 %s, expected %s" % (os.path.relpath(p, ROOT), got, want))
            bad += 1
    p = os.path.join(OUT, "pl_mpeg", "pl_mpeg.h")
    if not os.path.exists(p):
        print("missing: %s (--write --pl-mpeg)" % os.path.relpath(p, ROOT))
        bad += 1
    else:
        with open(p, "rb") as f:
            got = sha256(f.read())
        if got != PL_MPEG_SHA256:
            print("differs: %s has SHA-256 %s, expected %s" % (os.path.relpath(p, ROOT), got, PL_MPEG_SHA256))
            bad += 1
        else:
            print("ok: %s" % os.path.relpath(p, ROOT))
    for tc, lib in (("msvc", "SDL3-static.lib"), ("mingw", "libSDL3.a"), ("clang", "libSDL3.a"), ("gcc", "libSDL3.a")):
        p = os.path.join(OUT, "sdl3", tc, "lib", lib)
        stamp = os.path.join(OUT, "sdl3", tc, "STAMP.txt")
        if not os.path.exists(p):
            print("missing: %s (--write --sdl %s)" % (os.path.relpath(p, ROOT), tc))
            continue   # one toolchain is enough for one host
        with open(stamp) as f:
            if sdl_pin(tc)[2] not in f.read():
                print("stale: %s was built from another SDL3 source" % os.path.relpath(p, ROOT))
                bad += 1
                continue
        print("ok: %s" % os.path.relpath(p, ROOT))
    for tc, lib in (("msvc", "ysp_harfbuzz.lib"), ("mingw", "libysp_harfbuzz.a"), ("clang", "libysp_harfbuzz.a"),
                    ("gcc", "libysp_harfbuzz.a")):
        p = os.path.join(OUT, "layout", tc, "lib", lib)
        stamp = os.path.join(OUT, "layout", tc, "STAMP.txt")
        if not os.path.exists(p):
            print("missing: %s (--write --layout %s)" % (os.path.relpath(p, ROOT), tc))
            continue   # one toolchain is enough for one host
        if os.path.exists(os.path.join(ROOT, "third_party", "MANIFEST.sha256")):
            with open(stamp) as f:
                if layout_source_hash() not in f.read():
                    print("stale: %s was built from another third_party/ (MANIFEST.sha256 or patches/)"
                          % os.path.relpath(p, ROOT))
                    bad += 1
                    continue
        print("ok: %s" % os.path.relpath(p, ROOT))
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--write", action="store_true", help="fetch and build into third_party/vis/")
    ap.add_argument("--angle", action="store_true", help="with --write: ANGLE's DLLs from the Electron zip")
    ap.add_argument("--sdl", default="", help="with --write: msvc, mingw or msvc,mingw; clang on macOS; gcc on Linux")
    ap.add_argument("--pl-mpeg", action="store_true", help="with --write: pl_mpeg.h, the MPEG-1 decoder")
    ap.add_argument("--layout", default="", help="with --write: the text layout, msvc, mingw or msvc,mingw; "
                                                 "clang on macOS; gcc on Linux (after tools/vendor_layout.py --write)")
    ap.add_argument("--mingw-bin", help="the mingw64/bin whose gcc builds the Octave MEX files")
    ap.add_argument("--cache", help="a folder that keeps the downloads between runs")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    linux = sys.platform.startswith("linux")
    if sys.platform not in ("win32", "darwin") and not linux:
        sys.exit("ysp-vis natives: Windows, macOS and Linux only")
    tcs = ("clang",) if sys.platform == "darwin" else ("gcc",) if linux else ("msvc", "mingw")
    if a.write:
        if a.angle and linux:
            sys.exit("error: --angle: no ANGLE on Linux (GLX_OML uses the system's libGL)")
        if a.angle:
            write_angle(a.cache)
        if a.pl_mpeg:
            write_pl_mpeg(a.cache)
        for tc in [t for t in a.sdl.split(",") if t]:
            if tc not in tcs:
                sys.exit("error: --sdl takes %s here" % " or ".join(tcs))
            write_sdl(tc, a.cache, a.mingw_bin, a.jobs)
        for tc in [t for t in a.layout.split(",") if t]:
            if tc not in tcs:
                sys.exit("error: --layout takes %s here" % " or ".join(tcs))
            write_layout(tc, a.mingw_bin, a.jobs)
    return 1 if verify() else 0


if __name__ == "__main__":
    sys.exit(main())
