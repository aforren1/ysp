# X11 display-timing probe, 2026-10-09

This note describes a probe that measures display timing on a Linux X11
rig. Its results decided how the GLX backend of `ysp/screen.h`
(`YSCR_BACKEND_GLX_OML`, v0.5.0) is built. The plan that asks for
the backend is [rig_spec.md](rig_spec.md) section 4.2 ("Linux X11: GLX
sync control", separate X screens) and section 8 (timing model, tiers).
The Windows measurements that the probe mirrors are in
[screen.md](screen.md): depth, holding a frame, the onset as an
observation, path reports, slack.

The probe ran on one rig on 2026-10-09 (section 5). The backend and its
own measurements are in [screen.md](screen.md), "Linux X11: GLX_OML".
To add a rig, run the probe there and add its column to section 5.

Code: `tests/probe/screen_x11/`

- `screen_x11_probe.c`: the probe, one C file, about 3000 lines.
- `build.sh`: builds it with `cc` (or `CC=clang`).
- `run.sh`: one command that builds, records the system, runs the probe
  twice and packs the result.

CMake does not build the probe. It is Linux only.

## 1. Run it

### 1.1 Install the packages

Debian or Ubuntu (names checked on Ubuntu 22.04):

```sh
sudo apt install build-essential pkg-config libx11-dev libxcb1-dev \
    libxcb-present-dev libxcb-randr0-dev libgl-dev libvulkan-dev \
    mesa-utils x11-utils x11-xserver-utils vulkan-tools pciutils
# optional: clang; libsdl3-0 where the release packages it (Ubuntu 22.04 does not)
```

Fedora (names checked on packages.fedoraproject.org):

```sh
sudo dnf install gcc pkgconf-pkg-config libX11-devel libxcb-devel \
    libglvnd-devel mesa-libGL-devel vulkan-headers vulkan-loader \
    glx-utils xdpyinfo xrandr xset vulkan-tools SDL3
# optional: clang
```

Arch (names checked on archlinux.org):

```sh
sudo pacman -S --needed base-devel libx11 libxcb libglvnd mesa \
    vulkan-headers vulkan-icd-loader vulkan-radeon mesa-utils \
    xorg-xrandr xorg-xdpyinfo xorg-xset vulkan-tools sdl3
# optional: clang
```

Only libX11, libxcb (with its present and randr parts) and libGL are
linked. EGL, Vulkan, SDL3 and ANGLE are loaded at run time. When one is
missing, its section prints a message and the rest runs. The Vulkan
section also needs the Vulkan headers at build time.

### 1.2 Run

From the repository root, in a terminal on the X session:

```sh
sh tests/probe/screen_x11/run.sh
```

The script writes everything into `ysp_x11_probe_<date>_<time>/` in the
current folder and packs it into `ysp_x11_probe_<date>_<time>.tar.gz`.
Send that file back.

Before you start:

- Close programs that use the GPU. Keep the machine on AC power.
- Do not touch the mouse or keyboard during the run. The terminal keeps
  the keyboard focus, so Ctrl+C stops the probe early. The script then
  still makes the archive.
- The probe resets the X screen saver timer before each run. A DPMS
  setting can still blank a screen; `xset s off -dpms` turns both off
  until the next login.

What you see: each X screen goes dark gray. A 64 x 64 square at the top
left changes between black and white on every frame, for a photodiode.
There is no full-screen flicker.

How long: pass 1 (idle) is about 6 minutes with two X screens and about
3 minutes with one. Pass 2 (CPU and GPU load, fewer runs) is about 3
minutes. ANGLE adds about 2 minutes per screen.

### 1.3 Options

`run.sh` passes `PROBE_ARGS` to both passes. For example, with an ANGLE
build that has `libEGL.so` and `libGLESv2.so` in one folder:

```sh
PROBE_ARGS="--angle /path/to/angle/out" sh tests/probe/screen_x11/run.sh
```

To run the probe without the script, build it and call it:

```sh
sh tests/probe/screen_x11/build.sh              # makes tests/probe/screen_x11/build/screen_x11_probe
tests/probe/screen_x11/build/screen_x11_probe --help
```

| Option | Effect |
|---|---|
| `--out DIR` | Output folder (default `./ysp_x11_probe_<date>`) |
| `--frames N` | Swaps per run, default 600. Runs that hold 2 or 3 vblanks use N/2 and N/3, so each run takes about 10 s at 60 Hz |
| `--vblanks N` | Events in the vblank test, default 300 |
| `--screen N` | GLX and EGL tests on X screen N only, no two-screen phases |
| `--both` | Only the two-screen phases |
| `--no-both` | No two-screen phases |
| `--quick` | Fewer runs: `oml_k1`, `swap_wait`, `swap_pipe` |
| `--load N` | N busy threads for the whole run |
| `--gpu-heavy`, `--gpu-iters N` | A fragment shader with 400 (or N) dependent iterations per pixel every frame |
| `--late-every N` | Frame interval of the injected late frames, default 30 |
| `--wm` | Fullscreen through the window manager (`_NET_WM_STATE_FULLSCREEN`), not an override-redirect window |
| `--glx-compat` | A desktop GL context on GLX, not GL ES 3.0 |
| `--angle DIR` | Also run the EGL tests through ANGLE (Vulkan and GL back ends). `YSP_ANGLE_DIR` does the same |
| `--sleep-margin MS` | Time after the vblank at which a held frame's sleep ends, default 1.0 ms |
| `--no-rt` | Do not ask for `SCHED_FIFO` |
| `--skip-env`, `--skip-egl`, `--skip-sdl`, `--skip-vk` | Leave out a section |
| `--timeout S` | Stop after S seconds, default 1800 |

## 2. Output

| File | Contents |
|---|---|
| `idle/summary.txt`, `load/summary.txt` | The human-readable report: every section below, with one table row per run |
| `idle/swaps.csv` | One row per swap of every run (columns below) |
| `idle/vblank.csv` | One row per PresentNotifyMSC event: serial, target MSC, MSC, UST, the arrival on four clocks |
| `idle/clock.csv` | One row per sync-values read: UST, MSC, SBC, and the read time on four clocks |
| `idle/extensions.txt` | Full GLX, EGL and Vulkan extension lists |
| `system/` | `glxinfo`, `eglinfo`, `xrandr --verbose`, `xdpyinfo`, `xset q`, `vulkaninfo --summary`, `lspci -nnk`, the process list, the Xorg log, X and driconf configuration, amdgpu power settings |
| `bin/` | The binary and the source it was built from |
| `build.log` | The build command and the compiler's output |

`swaps.csv` columns. All times are nanoseconds on `CLOCK_MONOTONIC`.
UST values are raw, in the unit the source gives (microseconds for
Present and for Mesa's OML).

| Column | Meaning |
|---|---|
| `run`, `api`, `screen`, `method`, `k`, `frame` | Which run and frame |
| `late_injected` | 1 when the probe spun 1.25 periods before this swap |
| `gpu_iters` | Shader iterations of the GPU load, 0 when off |
| `target_msc` | The vblank the frame was meant for (empty for `swap_pipe`) |
| `msc_prev`, `ust_prev_ns` | The last shown frame's vblank, from which the target was planned |
| `t_wake_ns` | End of the hold (`sleep`, `vblwait`) |
| `t_call_ns`, `t_ret_ns` | Call and return of the swap call |
| `t_done_ns` | Return of `glXWaitForSbcOML`, or the arrival of the Present event |
| `sbc` | The probe's count of swaps on this drawable |
| `sync_kind`, `sync_ust`, `sync_msc`, `sync_sbc` | Kind 1: this swap's values from `glXWaitForSbcOML`. Kind 2: current values from `eglGetSyncValuesCHROMIUM` |
| `present_serial`, `present_mode`, `present_ust`, `present_msc`, `present_arrival_ns` | This swap's PresentCompleteNotify. Mode 0 copy, 1 flip, 2 skip, 3 suboptimal copy |
| `intel_type`, `intel_ust`, `intel_msc`, `intel_sbc`, `intel_arrival_ns` | GLX_INTEL_swap_event. Type 0x8180 exchange, 0x8181 copy, 0x8182 flip |

### 2.1 Table columns in summary.txt

| Column | Meaning |
|---|---|
| swaps, shown | Swaps made, and swaps with an onset (OML or a Present event that is not a skip) |
| Present mode | Counts of flip, copy, suboptimal copy, skip and no event |
| mode changes | How often the mode changed between frames |
| on target, late, early | Shown MSC against the target. "Late" also gives how many of the injected late frames were late |
| MSC step | The MSC difference between consecutive onsets: 0, 1, 2, 3 or more |
| swap call | Return minus call, microseconds |
| onset minus return | Latency from the swap call's return to the onset, milliseconds |
| known after onset | When the onset became known: `glXWaitForSbcOML` return, and Present event arrival, each minus the UST |
| grid residual | Distance of each onset from a straight-line fit of UST against MSC for that run |
| slack | The planned vblank's time minus the swap's return: the smallest on an on-time frame and the largest on a late frame. The deadline before a vblank lies between them |

## 3. What each section measures, and why the backend needs it

### 3.1 Environment (summary section 1)

X server vendor and release, screens, RandR CRTCs and outputs, mode
timings, providers, output properties, compositing manager, window
manager, 30-bit visuals, gamma ramp size, server extensions.

- The refresh rate is the dot clock divided by htotal x vtotal, to six
  decimals. The backend's fixed-grid capability needs the exact period.
  The vblank test compares it with the UST fit (ppm).
- The provider name tells the DDX: `modesetting` or the amdgpu driver.
  Their page-flip behavior differs (amdgpu has `TearFree` and
  `VariableRefresh` options).
- Output properties include `vrr_capable`, `TearFree`, `max bpc` and the
  EDID's monitor name. `max bpc` and the 30-bit visual count say whether
  10-bit output is possible.
- `_NET_WM_CM_Sn` has an owner when a compositing manager runs on screen
  n. A compositor forces the copy path unless it unredirects the
  window. The probe sets `_NET_WM_BYPASS_COMPOSITOR` on its fullscreen
  windows.
- DRI3 and Present present means Mesa presents through Present. Only
  then are Present events the per-frame path report.

### 3.2 GLX_OML_sync_control and the clocks (section 2)

- Presence of OML, swap control, `GLX_INTEL_swap_event`,
  `GLX_EXT_create_context_es2_profile`; the 10 bpc FBConfigs.
- Whether GLX gives a GL ES 3.0 context. rig_spec decides GL ES 3.0
  only. If GLX cannot make one, the backend uses EGL and takes its
  timing from Present instead of OML.
- `glXGetSyncValuesOML` 200 times with `clock_gettime` after each read,
  on `CLOCK_MONOTONIC`, `_RAW`, `REALTIME` and `BOOTTIME`. For each
  clock and unit (us, ns) the probe counts the reads where "read time
  minus UST" lies between 0 and about one period, and names the first
  clock that fits 95% of them. The backend can use a UST as a
  `ysp/rt.h` time without correlation only if it is
  `CLOCK_MONOTONIC`.
- `glXGetMscRateOML` and the MSC rate from the reads, against the mode.
- PresentNotifyMSC: 300 events queued at once, one per vblank. They
  give an independent vblank grid (the Linux analog of the scanline
  source in screen.md), the UST jitter about a straight-line fit, the
  same clock test, and the delay from the vblank to the event's
  arrival. That delay bounds how soon `yscr_begin()` can complete a
  record.

### 3.3 GLX swaps, fullscreen (section 3)

One override-redirect window covers the X screen: Xorg's Present
code flips a window only when it covers the whole screen (from the
Xorg source; the output control below checks it). Every run starts
after 10 swaps and a fresh vblank anchor. Runs:

| Run | What it does | Question |
|---|---|---|
| `oml_k1`, `oml_k2`, `oml_k3` | `glXSwapBuffersMscOML(target = last shown + k)`, then `glXWaitForSbcOML` | Does a target MSC hold a frame exactly? This is the scheduling primitive that `yscr_flip_at()` needs |
| `swap_wait` | `glXSwapBuffers`, swap interval 1, then wait | The depth: on which vblank does an unscheduled swap show? |
| `swap_pipe` | `glXSwapBuffers` with no wait | The queue the driver allows, the swap call's blocking, and latency without a wait |
| `sleep_k2` | Sleep until 1 ms after vblank target-1, then swap | The method DXGI_FLIP uses (screen.md, "Holding a frame"), for EGL where no target MSC exists |
| `vblwait_k2` | Wait for a PresentNotifyMSC event at target-1, then swap | The same hold with the server's vblank event as the wake |
| `oml_k1_late` | `oml_k1`, and every 30th frame spins 1.25 periods before its swap | Does the reported onset move with a late frame (an observation, tier 1) or stay on the plan (tier 3)? Are late frames flagged and no others? |

Each swap records OML's values for that swap, its PresentCompleteNotify
(the path: flip, copy, suboptimal copy, skip) and its INTEL event if the
extension exists. The serial check ("Present serial equals the swap
count") confirms that the probe matched each event to its swap.

Controls, after the fullscreen runs:

- A 640 x 480 window: the server must copy it. If the path report shows
  copy here and flip fullscreen, the report tells the paths apart.
- When one X screen has two or more active CRTCs: a window that covers
  only the first output. It shows whether a per-output window can flip,
  which rig_spec's "one window per output" design needs.

### 3.4 Present (section 4)

The Present columns of every run, and the vblank test. Mesa's DRI3
path presents with PresentPixmap on its own connection. The probe opens
a second connection and selects PresentCompleteNotify on the same
window; the server sends each selecting client its own event. If the
Present columns stay empty, Mesa did not use Present (DRI2) and the
summary says so.

### 3.5 EGL on X11 (section 5)

The same runs, without OML, through the system `libEGL.so.1` on
`EGL_PLATFORM_X11_KHR` with `EGL_PLATFORM_X11_SCREEN_KHR`, a GL ES 3.0
context and swap interval 1. It reports the EGL extensions that could
carry present timing: `EGL_CHROMIUM_sync_control`,
`EGL_ANDROID_get_frame_timestamps`, `EGL_ANDROID_presentation_time`,
`EGL_EXT_present_opaque`, and others. Its timing comes from the Present
events.

With `--angle DIR`, the same through ANGLE's Vulkan and GL back ends.
`GL_RENDERER` names the back end. ANGLE is optional on Linux; rig_spec
uses the native driver there.

### 3.6 Vulkan (section 6)

Extension listing only, no swapchain run: the device, the driver (RADV
expected), `VK_KHR_present_id`, `VK_KHR_present_wait`, their `2`
versions, `VK_GOOGLE_display_timing`, `VK_EXT_present_timing`,
`VK_EXT_acquire_xlib_display`, `VK_EXT_direct_mode_display`,
`VK_KHR_display`, the present modes and 10 bpc formats for an X11
window on the screen, and the displays that `VK_KHR_display` lists.

### 3.7 Two X screens (section 7)

Each screen gets its own X connection (`:0.N`) and thread, as a backend
that takes `desc.x_display` would. Four phases of `oml_k1` (or
`swap_wait` without OML), each about 10 s:

| Phase | Screen 0 | Screen 1 |
|---|---|---|
| A | timed | idle |
| B | idle | timed |
| C | timed | timed |
| D | timed | untimed: swap interval 0, as fast as it goes |

Compare screen 0's rows of A, C and D: a change in late frames, MSC
steps, grid residual or swap call time means one screen disturbs the
other. D is rig_spec 4.2's untimed operator screen. Phase C also
reports the onset offset between the screens and its drift: CRTCs that
are not locked drift.

### 3.8 SDL3 on a non-default screen (section 8)

Each variant runs in a child process, so an SDL crash costs only that
variant. It prints the displays SDL lists, the X screen the window is
on (from the X server, not only SDL's property), and whether a GL
context works:

| Variant | DISPLAY | SDL display | Context |
|---|---|---|---|
| 1, 2 | default | index 0 | GLX; EGL ES 3.0 |
| 3, 4 | `<display>.1` | index 0 | GLX; EGL ES 3.0 |
| 5, 6 | default | index 1 (on screen 1, if SDL lists it) | GLX; EGL ES 3.0 |

Expectation from reading SDL3's source (commit 0359c2d41, 2025-08-22),
not measured: SDL lists the RandR outputs of every X screen and creates
a window on its display's screen, but `X11_GL_CreateContext` picks its
FBConfig on `DefaultScreen()`. So variant 5 may fail or mismatch, while
variant 3 should work. The EGL path picks the visual on the window's
screen. The 2.0-era reports in rig_spec do not apply to SDL3 as is.

### 3.9 Load (section 9)

Pass 2 of `run.sh` runs `--quick` with one busy thread per CPU and the
heavy shader. Compare its late frames and grid residuals with pass 1.

## 4. What the results will decide

1. **Scheduling primitive.** If `oml_k2` and `oml_k3` show every frame
   on its target, `yscr_flip_at()` passes the planned vblank as
   `glXSwapBuffersMscOML`'s target. If not, the backend holds frames by
   `sleep` or by `vblwait`, whichever was exact.
2. **Context API.** GLX ES 3.0 with OML, or EGL ES 3.0 with Present
   events. This follows from item 1 and from whether GLX made an ES 3.0
   context.
3. **Onset source and tier.** OML's UST or Present's UST. Tier 1 needs:
   the UST is `CLOCK_MONOTONIC` (no correlation), it sits on the
   NotifyMSC grid, it moves with a late frame (`oml_k1_late`), and the
   path is flip. The arrival delay sets when a record can complete.
4. **Path detection.** If the Present mode is flip fullscreen and copy
   in the window control, `record.mode` takes it from Present per frame,
   and a copy frame gets a lower tier. INTEL_swap_event is the fallback
   where Present is absent.
5. **Depth and slack.** `swap_wait`'s MSC steps give the depth; the
   slack columns give the deadline before a vblank for the evidence
   rule of screen.md ("Depth on a backend that holds frames").
6. **Two-screen design.** Whether the backend opens its own X
   connection per screen (`desc.x_display`) or SDL3 can do it
   (section 8). Whether an untimed operator screen is safe beside a
   timed one (phase D). Whether one window per output can flip on a
   single X screen (the output control).
7. **10-bit output.** 30-bit visuals, 10 bpc FBConfigs or EGL configs,
   `max bpc`, Vulkan 10 bpc formats, and the gamma ramp size. On Xorg,
   30-bit visuals are normally offered only when the server runs at
   depth 30.
8. **VRR safety.** `_VARIABLE_REFRESH` on the window (Mesa sets it when
   its `adaptive_sync` driconf option is on), `vrr_capable`, and the
   Xorg log. The backend must keep variable refresh off by default
   (rig_spec 4.2).

## 5. Results

One rig: `run.sh` on 2026-10-09, archive
`ysp_x11_probe_20261009_081033.tar.gz` (not in the repository). Pass 1
idle, pass 2 with 20 busy threads and the heavy shader. Section 5.2's
NotifyMSC rows come from the fixed probe (section 6, "Defects found on
the rig"), run again the same day.

### 5.1 Conditions

| Item | Value |
|---|---|
| Date, kernel, distribution | 2026-10-09, Linux 6.11.5-1-liquorix-amd64, Debian 12 |
| X server, DDX (provider name) | X.Org 1.21.1 (release 12101007); amdgpu ("AMD Radeon RX 6600 @ pci:0000:03:00.0"). A second provider, "modesetting", is the Intel iGPU with no output connected |
| GPU, Mesa (GL_RENDERER, GL_VERSION) | AMD Radeon RX 6600 (navi23, LLVM 15.0.6, DRM 3.59); OpenGL ES 3.2 Mesa 22.3.6 |
| X screens; per screen: outputs, mode, refresh (dot clock) | 1 screen; DisplayPort-5 (HP E23 G4, HDMI subconnector), 1920 x 1080, 148.5 MHz / (2200 x 1125) = 60.000000 Hz |
| Compositing manager per screen | none (Xfwm4, compositor off) |
| DRI3, Present version | yes, Present 1.2 |
| `TearFree`, `VariableRefresh`, `vrr_capable`, `_VARIABLE_REFRESH` | auto; not set in any X config file; 0; 1 on every fullscreen window (Mesa), not set on the 640 x 480 window |
| `SCHED_FIFO` granted | yes, priority 10 |
| Power (AC), amdgpu power level | desktop on AC; `auto`, profile 3D_FULL_SCREEN |

### 5.2 Sync control and clocks, screen 0

| Item | Screen 0 |
|---|---|
| GLX_OML_sync_control, INTEL_swap_event | yes, yes |
| GLX GL ES 3.0 context | yes, direct (GLX_EXT_create_context_es2_profile) |
| UST clock and unit (OML) | CLOCK_MONOTONIC, us (195 of 200 reads in range) |
| UST clock and unit (Present) | the same: OML minus Present UST 0 us at most, in every run |
| Mode refresh; UST fit; difference ppm | 60.000000 Hz; 59.999302 Hz; 11.6 ppm |
| NotifyMSC grid residual p99 / max, us | 0.67 / 1.02 |
| NotifyMSC arrival after UST p50 / p99, us | -427 / -292: the event comes before its UST, which is the end of the vblank |
| glXGetSyncValuesOML call | 54.6 us p50, 91.5 us p99, 7.5 to 94.1 us (200 reads, clock.csv) |

### 5.3 Swap runs, screen 0

Pass 1, idle, GLX:

| run | swaps | shown | Present mode flip/copy/subopt/skip/none | on target | late | early | MSC step 0/1/2/3+ | swap call us p50/p99/max | onset minus return ms p50/p99/max | known after onset us, sync p50/p99; Present p50/p99 | grid residual us p99/max | slack ms: min on time; max late |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| oml_k1 | 600 | 600 | 600/0/0/0/0 | 600 | 0 | 0 | 0/599/0/0 | 44/94/122 | 17.00/17.09/17.09 | -396/-242; -393/-238 | 0.7/1.9 | 16.78; - |
| oml_k2 | 300 | 300 | 300/0/0/0/0 | 300 | 0 | 0 | 0/0/299/0 | 45/99/109 | 33.66/33.74/33.76 | -396/-231; -393/-223 | 1.1/2.2 | 33.42; - |
| oml_k3 | 200 | 200 | 200/0/0/0/0 | 200 | 0 | 0 | 0/0/0/199 | 36/84/111 | 50.35/50.41/50.42 | -410/-261; -408/-258 | 0.7/1.0 | 50.15; - |
| swap_wait | 600 | 600 | 600/0/0/0/0 | 600 | 0 | 0 | 0/599/0/0 | 36/112/234 | 17.01/17.07/17.09 | -406/-250; -402/-245 | 1.1/2.5 | 16.62; - |
| swap_pipe | 600 | 600 | 600/0/0/0/0 | - | - | - | 0/599/0/0 | 53/94/114 | 33.68/33.74/33.77 | -; -350/-197 | 0.7/1.2 | - |
| sleep_k2 | 300 | 300 | 300/0/0/0/0 | 300 | 0 | 0 | 0/0/299/0 | 77/162/206 | 15.58/15.65/15.65 | -403/-237; -400/-233 | 0.7/0.7 | 15.45; - |
| vblwait_k2 | 300 | 300 | 300/0/0/0/0 | 300 | 0 | 0 | 0/0/299/0 | 62/104/114 | 17.03/17.08/17.09 | -400/-258; -397/-254 | 0.7/2.4 | 16.85; - |
| oml_k1_late | 600 | 600 | 600/0/0/0/0 | 580 | 20 (20 of 20 injected) | 0 | 0/579/20/0 | 50/71/108 | 17.01/17.05/17.08 | -407/-383; -404/-379 | 0.7/2.5 | 16.91; -3.78 |
| window_swap_wait (640 x 480) | 300 | 300 | 0/300/0/0/0 | 300 | 0 | 0 | 0/299/0/0 | 31/63/67 | 16.94/17.08/17.09 | -330/415; -328/420 | 2.2/2.7 | 16.13; - |
| window_oml_k1 (640 x 480) | 300 | 300 | 0/300/0/0/0 | 300 | 0 | 0 | 0/299/0/0 | 46/67/76 | 16.89/17.05/17.08 | -274/419; -270/422 | 2.2/2.5 | 15.76; - |

In every GLX run the Present serial equaled the swap count on every
swap, and INTEL_swap_event agreed with Present's mode (flip fullscreen,
copy in the window).

Pass 1, EGL (`libEGL.so.1`, radeonsi): `swap_wait`, `swap_pipe`,
`sleep_k2`, `vblwait_k2` and `swap_wait_late` gave the same results as
GLX, every swap a flip. EGL has no target MSC, so it holds a frame only by
`sleep` or `vblwait`.

Pass 2, 20 busy threads and the heavy shader, GLX:

| run | swaps | shown | Present mode flip/copy/subopt/skip/none | on target | late | early | MSC step 0/1/2/3+ | swap call us p50/p99/max | onset minus return ms p50/p99/max | known after onset us, sync p50/p99; Present p50/p99 | grid residual us p99/max | slack ms: min on time; max late |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| oml_k1 | 600 | 600 | 600/0/0/0/0 | 600 | 0 | 0 | 0/599/0/0 | 24/35/47 | 17.02/17.09/17.09 | -383/1224; -383/1225 | 1.0/5.7 | 14.72; - |
| swap_wait | 600 | 600 | 600/0/0/0/0 | 600 | 0 | 0 | 0/599/0/0 | 22/38/79 | 16.94/17.09/17.09 | -302/1408; -301/1409 | 1.0/1.5 | 12.79; - |
| swap_pipe | 600 | 600 | 600/0/0/0/0 | - | - | - | 0/599/0/0 | 22/36/2566 | 33.57/33.76/33.77 | -; -229/1449 | 1.0/8.9 | - |
| window_swap_wait | 300 | 300 | 0/300/0/0/0 | 300 | 0 | 0 | 0/299/0/0 | 22/40/45 | 16.85/17.09/17.09 | -220/1689; -219/1690 | 0.8/0.8 | 14.07; - |
| window_oml_k1 | 300 | 300 | 0/300/0/0/0 | 300 | 0 | 0 | 0/299/0/0 | 21/35/42 | 16.91/17.09/17.10 | -269/1280; -268/1281 | 0.8/9.1 | 13.65; - |

Under load the event came up to 1.4 ms after the UST (p99), against
0.24 ms before it idle. No frame was late.

### 5.4 Two screens

Run 2 (2026-10-12, after a second monitor was added as X screen 1:
ZaphodHeads, DisplayPort-4 on `:0.0`, an "MD-2402 24" monitor, and
DisplayPort-5 on `:0.1`, the HP; both 1920 x 1080 at 60 Hz). The probe
with `--skip-egl --skip-vk` and SDL 3.4.18 on the library path:

| Phase | Screen 0 late / steps 2+ / grid p99 | Screen 1 late / steps 2+ / grid p99 | Notes |
|---|---|---|---|
| A | 0 / 0 / 0.7 us | | 600 of 600 on target |
| B | | 0 / 0 / 0.7 us | 600 of 600 on target |
| C | 0 / 0 / 0.9 us | 0 / 0 / 0.7 us | offset p50 0.0 us, min -50,000 us, drift -99.6 ppm: not explained |
| D | 0 / 0 / 1.1 us (max 8.9) | 28,410 swaps a second; flip 131,201, skip 158,576 | screen 0 undisturbed |

### 5.5 EGL, ANGLE, Vulkan, SDL3

| Item | Result |
|---|---|
| EGL: driver, CHROMIUM sync control, frame timestamps, Present path | radeonsi; EGL_CHROMIUM_sync_control yes (UST CLOCK_MONOTONIC us); EGL_ANDROID_get_frame_timestamps no; Present events yes, every fullscreen swap a flip |
| ANGLE: back ends that initialized, GL_RENDERER, Present path | not run (no `--angle`) |
| Vulkan: driver; present_id/wait (2); display_timing; present_timing; acquire_xlib_display | not measured: `vkCreateInstance` returned -9 (VK_ERROR_INCOMPATIBLE_DRIVER), no instance extension listed; no Vulkan driver installed |
| SDL3 variants 1 to 6 (RESULT lines) | run 1: no SDL3 on the rig (Debian 12 has no package). Run 2, SDL 3.4.18 built from source: SDL lists both X screens' displays; variants 1, 2 (default DISPLAY) on screen 0, GLX and EGL ES 3.0 ok; variants 3, 4 (`DISPLAY=:0.1`) on screen 1, both ok; variants 5, 6 (default DISPLAY, the display on screen 1 asked for) on screen 0: SDL puts every window on $DISPLAY's screen |

### 5.6 Decisions

1. **Scheduling primitive: the OML target.** `oml_k1`, `oml_k2` and
   `oml_k3` showed 1100 of 1100 frames on their target, 0 early, idle and
   under load. `yscr_flip_at()` passes the planned vblank to
   `glXSwapBuffersMscOML` and does not wait (`native_target`). `sleep_k2`
   and `vblwait_k2` were exact too, but cost a wait on the frame thread.
2. **Context API: GLX, GL ES 3.0.** GLX gave an ES 3.0 context, and only
   GLX has the target MSC.
3. **Onset source and tier: Present's UST, tier 1 on a flip.** The UST is
   CLOCK_MONOTONIC in us (no correlation), equal to OML's, within 0.67 us
   p99 of the NotifyMSC grid, and it moved with each of the 20 injected
   late frames. Idle, the event arrives 0.40 ms before the UST at p50 and
   at least 0.24 ms before it at p99, so a record completes at about its
   onset.
4. **Path detection: Present's mode.** Fullscreen was a flip on all 5300
   GLX swaps of both passes, the 640 x 480 window a copy on all 1200. A
   copy is tier 2. INTEL_swap_event agreed, and is not used.
5. **Depth and slack.** `swap_wait` showed MSC step 1 on 599 of 599:
   depth 1. The smallest on-time slack was 15.45 ms (`sleep_k2`), the
   largest late slack -3.78 ms, so the deadline before a vblank is between
   them; the core's slack evidence (screen.md, "Depth on a backend that
   holds frames") learns it.
6. **Two-screen design (run 2): one process per X screen.** Both screens
   timed at once lost nothing, and an untimed screen swapping 28,410
   times a second beside a timed one cost it nothing but grid residual
   (8.9 us max). SDL 3 opens one X screen, $DISPLAY's (variants 5 and 6),
   so `desc.x_display` sets DISPLAY before SDL starts, and an operator
   UI runs in another process or the browser console.
7. **10-bit output: not on this server.** 0 of 504 visuals are 30-bit
   (the server runs at depth 24), 0 FBConfigs or EGL configs have 10 bpc;
   `max bpc` is 16 and the gamma ramp has 1024 entries.
8. **VRR safety.** `vrr_capable` is 0 on this monitor, but Mesa set
   `_VARIABLE_REFRESH = 1` on every fullscreen window. The backend sets it
   to 0 after the first swap (screen.md).

## 6. Checked, and not verified

Checked on 2026-10-09 in WSL2 (Ubuntu 22.04, Mesa 23.2.1, gcc 11.4 and
clang 11), with a private two-screen Xvfb. No WSLg window was used, so
that no fullscreen window covered the Windows desktop during other
measurements. These runs check the code paths, not timing: Xvfb has a
simulated 60 Hz vblank and no display.

- Builds without warnings with `-Wall -Wextra -Wshadow -Wpedantic` on
  both compilers, with and without the Vulkan headers.
- `run.sh` end to end: build, system files, both passes, the archive.
- Under ASan and UBSan: no error in a full pass with two screens. The
  leak report lists Mesa's allocations and one deliberate one: the
  probe keeps each EGL library loaded.
- PresentNotifyMSC, the clock test and the two-screen phases ran.
  Xvfb's UST matched `CLOCK_MONOTONIC` in microseconds on 60 of 60
  events.
- Mesa 23.2's `eglGetMscRateANGLE` crashed on its software X11 path.
  The probe now calls it only on ANGLE. The first call of each optional
  sync query runs under a fault handler, and a context whose driver
  crashed is abandoned, not closed: after a crash, the driver's and
  Xlib's locks can still be held, and closing deadlocked.

Verified on the rig (section 5): DRI3 and Present events for GL swaps,
the serial check, the mode counts, a second client's Present selection
on Mesa's window, OML, INTEL_swap_event and swap interval control.

Not verified:

- SDL3 and ANGLE (neither was on the rig), the two-screen phases with
  real vblanks, and `--wm` with a real window manager. `ysp/screen.h`'s
  GLX backend does run fullscreen through SDL's window and the window
  manager (screen.md).
- That `XResetScreenSaver` also holds off DPMS: the rig's server has no
  DPMS extension.

### Defects found on the rig, fixed the same day

- **The NotifyMSC test.** The probe queued all 300 PresentNotifyMSC
  requests at once. The server held 128 and completed the other 172 at
  once with the current MSC (on targets 130 to 301 vblanks ahead). The
  summary then said "128 on the MSC asked for", gave an arrival of
  2.5 ms after the UST, and found no clock for the UST. The limit looks
  like the DRM file's 4096-byte event space (32 bytes per vblank event);
  this was not checked. The probe now queues 64 at a time: 300 of 300 on
  target.
- **The clock window.** A read or event counted as "in range" from
  -50 us after the UST. A UST is the start of scanout, the end of the
  vblank, and the events come from its start: 0.43 ms before the UST at
  p50 here. The window now starts at -2 ms.
