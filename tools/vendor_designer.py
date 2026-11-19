#!/usr/bin/env python3
"""Recreate or verify the designer's third-party files in third_party/.

    uv run --no-project python tools/vendor_designer.py            # verify (default)
    uv run --no-project python tools/vendor_designer.py --write    # fetch into third_party/
    uv run --no-project python tools/vendor_designer.py --write --cache DIR   # keep the tarballs between runs
    uv run --no-project python tools/vendor_designer.py --write --test-engine  # also the Test Engine

Two sets, each from a GitHub tarball at a full commit SHA, each file compared
byte for byte with the SHA-256 value here; nothing else is accepted:

    third_party/imgui/           Dear ImGui v1.93.0 (MIT): the designer's UI,
                                 and its SDL3 and OpenGL 3 / GL ES 3 backends
    third_party/skribidi-extra/  Skribidi's rasterizer, image atlas, canvas
                                 and layout cache (MIT), from the commit that
                                 tools/vendor_layout.py pins: the designer
                                 draws user text with them; the pack tool
                                 does not compile them, so vendor_layout.py
                                 leaves them out

The designer (CMake option YSP_BUILD_DESIGNER) needs them for its window and
its UI test; its core and core test do not. The pins are the ones the UI
spike measured (docs/designer_ui_probe.md).

One more set only with --test-engine, for the optional test layer (CMake
option YSP_DESIGNER_TEST_ENGINE, docs/designer.md, "UI tests with the Test
Engine"); nothing else needs it:

    third_party/imgui_test_engine/  Dear ImGui Test Engine, tag v1.93.0 (the
                                 tag for ImGui v1.93.0), under the Dear ImGui
                                 Test Engine License v1.04, not MIT: read
                                 its LICENSE.txt before you fetch it

Standard library only.
"""
import argparse
import hashlib
import io
import os
import sys
import tarfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SETS = [
    {
        "dest": "imgui",
        "repo": "ocornut/imgui",
        "commit": "c1aaac65e896a45f81f47a4d68f9959e0f86c859",   # tag v1.93.0
        "files": {
            "imgui.cpp": "9542082a1eaeffcab7b3d8069e707839ce1714d167005e9f273ac9f656b415ed",
            "imgui.h": "314462301948fbd5673f5ead6f9a53acf54767cbee57e186e69f8f8b022c7094",
            "imgui_internal.h": "61bc50172d907cc931159cdce48fbff1db616cda33251c9c3469e316056939e6",
            "imgui_draw.cpp": "fff3bb04ca11e32041144e83a37b89ec92f4d5c62d681eb7ef29ee6cd7663e84",
            "imgui_tables.cpp": "36635c1cf2ad1b7c88cdced456cceba3a1d02266d78a63d77e9cce92f4d48d61",
            "imgui_widgets.cpp": "d2d388f711688f6a5ce1ae3b30168c1bf4f954ead40235b0b2b9667452850c93",
            "imconfig.h": "fa91f606da783669ffd485f3f9e0cd0d92cddeac459efce4c2b9327cce7a3233",
            "imstb_rectpack.h": "889b396795202d1457560a797a7242e96f6f132d4b88ca2d69be58bf05e1771f",
            "imstb_textedit.h": "a985f5fa0ed97353d493b497961e9eef52082edcd045cf6954b69990ec9d0741",
            "imstb_truetype.h": "c51a0f7e7ea760f2366bd3752635ec58e21fccfec4a832501639990ba6ce0528",
            "LICENSE.txt": "173506a2d6f7fb67990d257fb2507f188690eca39060c39469ae7bef43aae2a3",
            "backends/imgui_impl_sdl3.cpp": "306060dffa276b574f0541194e8e079c1685c29c1095602a3972de0bb23171e7",
            "backends/imgui_impl_sdl3.h": "fa184f17d59d31e986df2e827ef964d7b3c28bcba7789643a4aaaed344cb868e",
            "backends/imgui_impl_opengl3.cpp": "3d8c324f414517c2393e1f34009933bf9ca0e0eee4d04f9db57257df5f1d9d2b",
            "backends/imgui_impl_opengl3.h": "ff9510fb5d13f4c13b8ae4b980777c9a01d65d60c3a5449b0f1f92c635c5469b",
            "backends/imgui_impl_opengl3_loader.h": "e83374501bb912b5216a1b191c620a78a4ff0824af1664061fc42c5d76cc9367",
        },
    },
    {
        "dest": "skribidi-extra",
        "repo": "memononen/skribidi",
        "commit": "dee63d6ba76aeddd49dea6d1b2508cf9aa391f46",   # tools/vendor_layout.py's pin
        "files": {
            "src/skb_rasterizer.c": "123cb10676fd826a907647cf51b957869b5ed9012e42b3eb80c7ffbfbbd1f311",
            "src/skb_image_atlas.c": "c8c2c697f9c69d6d8eb22355cc27f5d79900f700128d83d8afd890d68ef754c1",
            "src/skb_canvas.c": "60dde5681f04c0f3d4fc4c9be6bac0ff6f3ace7dc20a310b3be0148af2054417",
            "src/skb_layout_cache.c": "604e54e4bc6051f64a2e6717c2f9904521c3d728905cb3aff89c31ffbf19feba",
            "include/skribidi/skb_rasterizer.h": "b6f0338ebbe96919b2c7e87185b100d4738c5651fe471e9181ac95eab8390c02",
            "include/skribidi/skb_image_atlas.h": "5c5c1539a3c7ede224e5fad8e9a82f22b2eec7fa39ea00948b9614270a5f8c32",
            "include/skribidi/skb_layout_cache.h": "87fc41f3f7600d04bc57f3d2a88d58303448c35ccba51242c3ae5a0fcc37cb59",
        },
    },
    {
        # Only with --test-engine. Its license (LICENSE.txt, v1.04) is free for
        # some users only, so it is never fetched by default.
        "dest": "imgui_test_engine",
        "repo": "ocornut/imgui_test_engine",
        "commit": "16b6efbb393fdef0dcac9b0794a8ee476606a152",   # tag v1.93.0, made with ImGui's v1.93.0
        "subdir": "imgui_test_engine",
        "optional": True,
        "files": {
            "imgui_capture_tool.cpp": "9308f675d5343f1c22599d21e80f8a4e70aa19e2a79bdad10d3e914acd491810",
            "imgui_capture_tool.h": "580a824a816d983c9cf4bd830a5994e1d522ef3649bd548eee682a12379df589",
            "imgui_te_context.cpp": "624ac7eab399dab5cd67f2efef9366aeaacd4c4f1af5f34bf19192eb3c76f752",
            "imgui_te_context.h": "0015be86bce41b887c66923488a8d1ee745b19c7c52215de788e2fad296e9657",
            "imgui_te_coroutine.cpp": "a4ea7bf4aea394f56b1f588b70bd77f96f2331af26eee973295904ebbc08494a",
            "imgui_te_coroutine.h": "43db2cf9b3d39fdbce385c04ac49fc70a57e5c5ab4b682d5c71ae9654b784e82",
            "imgui_te_engine.cpp": "0d8e36251dbbec8ec817539d1b3a0a82aab60928bdb8a258ec6a5b656ff22af6",
            "imgui_te_engine.h": "8a7cd47c4381aa69b02e763dfc888e9a85d3865b4eff3e1a14a98bec71d3d960",
            "imgui_te_exporters.cpp": "84e1bc0daad1cde3e59cf9719f5dba2adcc43894cc720cbafa79ec5c6d8bcb29",
            "imgui_te_exporters.h": "1deaec6a0c94808e73101219c32f7dc643613e959f918f09f7a73551652fac9a",
            "imgui_te_imconfig.h": "d8ebb750cabaeb544a6e2350bb2704b9c16a06f52f228e9d030b06466835ebba",
            "imgui_te_internal.h": "4ea717e535b3e6e9e110c62cefd62f0835bb17603e6adb84d5279b370a6d40ce",
            "imgui_te_perftool.cpp": "f9a9cc78d08b855afcfdfcadeb78342934b23d1600804ad60d9b4a9d455fab7e",
            "imgui_te_perftool.h": "87900961b84d6744845f2b244f700f7dc2ea3181c3268a984ed195b596522b1a",
            "imgui_te_ui.cpp": "aa19db413b9e22dc323b40d45c8ac07cffbfbc64ccc051553e0c82cf8375f139",
            "imgui_te_ui.h": "1390d011a1ae6fa46dc5853a8342136c908330451cb45d8c57e2ce7fbd4a7eab",
            "imgui_te_utils.cpp": "038d307b4db68f84a4ab72029b089f3306125f656277aad40da277cb704f943d",
            "imgui_te_utils.h": "8c41e2df36cd45c44a98963a1fbd70f4f566e1882a17206cfaf009b19942eee8",
            "LICENSE.txt": "8d01df5085b3d7c055999188bfc743354b00658be309819eb0415239820b1835",
            "thirdparty/Str/Str.h": "704a91401a1004a121ce07b6c096c66398a05ae4ee35f3bdc2fc0d98ea19f852",
        },
    },
]


def tarball(s, cache):
    """The commit's tarball: from the cache when there, else downloaded."""
    name = s["repo"].replace("/", "_") + "-" + s["commit"] + ".tar.gz"
    if cache and os.path.exists(os.path.join(cache, name)):
        with open(os.path.join(cache, name), "rb") as f:
            return f.read()
    url = "https://codeload.github.com/%s/tar.gz/%s" % (s["repo"], s["commit"])
    with urllib.request.urlopen(url, timeout=120) as r:
        data = r.read()
    if cache:
        os.makedirs(cache, exist_ok=True)
        with open(os.path.join(cache, name), "wb") as f:
            f.write(data)
    return data


def extract(data, wanted, subdir=None):
    """path inside the repository (or inside its subdir) -> bytes, for the
    wanted paths only."""
    out = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as t:
        for m in t.getmembers():
            if not m.isfile():
                continue
            rel = m.name.split("/", 1)[1] if "/" in m.name else m.name   # the tarball's top folder
            if subdir:
                if not rel.startswith(subdir + "/"):
                    continue
                rel = rel[len(subdir) + 1:]
            if rel in wanted:
                out[rel] = t.extractfile(m).read()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--write", action="store_true", help="download and write the files into third_party/")
    ap.add_argument("--cache", help="a folder that keeps the downloaded tarballs between runs")
    ap.add_argument("--test-engine", action="store_true",
                    help="also the Dear ImGui Test Engine (its own license, not MIT: docs/designer.md)")
    a = ap.parse_args()
    bad = 0
    sets = [s for s in SETS if a.test_engine or not s.get("optional")]
    for s in sets:
        dest = os.path.join(ROOT, "third_party", s["dest"])
        got_files = extract(tarball(s, a.cache), set(s["files"]), s.get("subdir")) if a.write else {}
        for name, want in sorted(s["files"].items()):
            path = os.path.join(dest, name)
            if a.write:
                data = got_files.get(name)
                if data is None:
                    print("error: %s is not in %s@%s" % (name, s["repo"], s["commit"][:12]))
                    bad += 1
                    continue
                got = hashlib.sha256(data).hexdigest()
                if got != want:
                    print("error: %s from %s@%s has SHA-256 %s, expected %s" % (name, s["repo"], s["commit"][:12], got, want))
                    bad += 1
                    continue
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "wb") as f:
                    f.write(data)
            if not os.path.exists(path):
                print("missing: %s (run with --write)" % os.path.relpath(path, ROOT))
                bad += 1
                continue
            with open(path, "rb") as f:
                got = hashlib.sha256(f.read()).hexdigest()
            if got != want:
                print("differs: %s has SHA-256 %s, expected %s" % (os.path.relpath(path, ROOT), got, want))
                bad += 1
    if bad:
        return 1
    print("ok: " + ", ".join("%s %s (%d files)" % (s["dest"], s["commit"][:12], len(s["files"])) for s in sets))
    return 0


if __name__ == "__main__":
    sys.exit(main())
