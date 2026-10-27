#!/usr/bin/env python3
"""Fetch and build the natives of ysp-vis (Python ysp.screen, MEX ysp_screen) in third_party/vis/.

    uv run --no-project python tools/vendor_vis.py                         # verify (default)
    uv run --no-project python tools/vendor_vis.py --write --angle          # ANGLE's two DLLs
    uv run --no-project python tools/vendor_vis.py --write --sdl msvc       # static SDL3, MSVC
    uv run --no-project python tools/vendor_vis.py --write --sdl mingw --mingw-bin DIR
    uv run --no-project python tools/vendor_vis.py --write --pl-mpeg        # pl_mpeg.h (MPEG-1)
    ... --cache DIR       keep the downloads between runs (CI)

What it makes, Windows only (the other screen backends are stubs):

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
            sys.exit("error: the SDL3 build failed (exit %d)" % r.returncode)
        return
    for c in cmds:
        r = subprocess.run(c, env=env)
        if r.returncode:
            sys.exit("error: %s failed (exit %d)" % (c[0], r.returncode))


def write_sdl(toolchain, cache, mingw_bin, jobs):
    data = download(SDL_URL, SDL_SHA256, cache)
    prefix = os.path.join(OUT, "sdl3", toolchain)
    work = tempfile.mkdtemp(prefix="ysp-sdl3-%s-" % toolchain)
    try:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as t:
            if hasattr(tarfile, "data_filter"):
                t.extractall(work, filter="data")
            else:
                t.extractall(work)
        src = os.path.join(work, "SDL3-%s" % SDL_VERSION)
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
        cc = "msvc" if toolchain == "msvc" else subprocess.run(
            [os.path.join(mingw_bin, "gcc.exe"), "-dumpfullversion"], capture_output=True, text=True,
            env=env).stdout.strip()
        with open(os.path.join(prefix, "STAMP.txt"), "w", newline="\n") as f:
            f.write("SDL3 %s static, %s\nsource %s\nsha256 %s\ncompiler %s\nlink also: %s\n" % (
                SDL_VERSION, toolchain, SDL_URL, SDL_SHA256, cc, " ".join(SDL_SYSTEM_LIBS)))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("ok: static SDL3 %s (%s) in %s" % (SDL_VERSION, toolchain, os.path.relpath(prefix, ROOT)))


# ---- verify ------------------------------------------------------------------

def verify():
    bad = 0
    for name, want in ANGLE_FILES.items():
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
    for tc, lib in (("msvc", "SDL3-static.lib"), ("mingw", "libSDL3.a")):
        p = os.path.join(OUT, "sdl3", tc, "lib", lib)
        stamp = os.path.join(OUT, "sdl3", tc, "STAMP.txt")
        if not os.path.exists(p):
            print("missing: %s (--write --sdl %s)" % (os.path.relpath(p, ROOT), tc))
            continue   # one toolchain is enough for one host
        with open(stamp) as f:
            if SDL_SHA256 not in f.read():
                print("stale: %s was built from another SDL3 source" % os.path.relpath(p, ROOT))
                bad += 1
                continue
        print("ok: %s" % os.path.relpath(p, ROOT))
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--write", action="store_true", help="fetch and build into third_party/vis/")
    ap.add_argument("--angle", action="store_true", help="with --write: ANGLE's DLLs from the Electron zip")
    ap.add_argument("--sdl", default="", help="with --write: msvc, mingw or msvc,mingw")
    ap.add_argument("--pl-mpeg", action="store_true", help="with --write: pl_mpeg.h, the MPEG-1 decoder")
    ap.add_argument("--mingw-bin", help="the mingw64/bin whose gcc builds the Octave MEX files")
    ap.add_argument("--cache", help="a folder that keeps the downloads between runs")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    if sys.platform != "win32":
        sys.exit("ysp-vis natives: Windows only (the other screen backends are stubs)")
    if a.write:
        if a.angle:
            write_angle(a.cache)
        if a.pl_mpeg:
            write_pl_mpeg(a.cache)
        for tc in [t for t in a.sdl.split(",") if t]:
            if tc not in ("msvc", "mingw"):
                sys.exit("error: --sdl takes msvc, mingw or both")
            write_sdl(tc, a.cache, a.mingw_bin, a.jobs)
    return 1 if verify() else 0


if __name__ == "__main__":
    sys.exit(main())
