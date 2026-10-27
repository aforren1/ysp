# ysp: notes for Claude sessions

Single-header, public-domain C libraries for psychophysics rigs.
`include/ysp/<name>.h` (prefix `y<x>_`, see the Prefix column in
README.md), `pack/` (text layout and the pack tool, not single headers),
`examples/<lib>/`, `tests/` (`adapt/` unit tests, `compile/` C and C++
checks, `fuzz/`, `mutate/` mutation lists, `loopback/` hardware loopbacks),
`docs/<name>.md` (one manual per header, with design notes and
measurements), `firmware/` (Teensy and RP2040 sketches).

Plans: `docs/rig_spec.md` (display, audio, video, pack, player,
designer; section 11 is the order of work, 13 the open questions) and
`docs/devices_spec.md`. Design notes in a header's doc win where they
disagree with the spec. Read the relevant sections before proposing
anything; some options were measured and reversed, and the specs say why.

## Rules

- No commits, pushes, stashes, resets or other git state changes unless
  the user says so. Read-only git is fine.
  Standing exception (2026-10-10): agents may create git worktrees and
  local branches and commit to those branches. Never commit to, merge
  into or rebase `main`, and never push, without the user's say-so.
- Be terse and critical. US English. No em dashes, no emojis.
- Docs follow ASD-STE100 and the Google developer style guide, with
  Diataxis structure. Code comments say why, not how.
- Don't guess, measure. State numbers with the machine and conditions.
  Delete options that measure no better.
- Minimize allocations in hot loops; be cache-aware. A trial's frame
  work fits in one 16 ms frame, or the knob is named.
- Durations in experiment definitions, scripts and data files are
  seconds. C APIs use int64 ns.
- No Rust dependencies. Headers in `include/ysp` stay dependency-free
  (C99 floor, compile as C++).
- Prefer `uv` for Python (`uv run --no-project python ...`) and `gh`
  for GitHub. Write LF line endings (Python: binary mode or
  `newline='\n'`; CRLF breaks the mutation lists' anchors).
- A platform's timing tier comes from a photodiode or line-in run on
  that platform, never from CI or from a vendor's claim.

## Changing a header

Bump the version in the header's first line, its `*_VERSION` macro and
its changelog; update `docs/<name>.md`, its row in the README Headers
table (version, status word), its entry in `docs/status.md`, the tests,
the compile checks, and its mutation list
(`uv run tests/mutate/mutate.py tests/mutate/<name>.toml --check`, then
run the mutants you add). Parsers of untrusted input get a fuzz target.
A change to a struct's layout or to a function a bindings/share table
carries bumps at least the minor version (consumers accept another patch).
CI is `.github/workflows/ci.yml`.

## Building

`CMakePresets.json`: `msvc`, `msvc-full` (SDL3 and ANGLE), `mingw`,
`linux`, `clang`, `sanitizers`, `nothreads-cxx`. Third-party sources are
fetched and hash-checked, not committed:
`uv run --no-project python tools/vendor_layout.py --write` (Skribidi,
HarfBuzz and others, for `YSP_BUILD_LAYOUT`) and `tools/vendor_pack.py`
(lodepng, for `YSP_BUILD_PACK`).

Timing measurements run one at a time on a quiet machine: a second
measuring job's load once showed up as a false regression. On the
Windows development machine a shared guard script serializes them; on
other machines, run nothing else while measuring.

## Linux X11 work

`ysp/screen.h`'s X11 and Wayland backends are stubs. Start with the
probe (`docs/screen_x11_probe.md`; `sh tests/probe/screen_x11/run.sh`
from the repo root, in the X session): run it, read its results, then design
the backend from them and from `docs/rig_spec.md` 4.2 (GLX sync control,
separate X screens) and `docs/screen.md` (flip records, paths, depth,
slack, flip_at). Do not assume what the driver does; the probe measures it.
