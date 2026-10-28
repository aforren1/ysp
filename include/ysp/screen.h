/* ysp/screen.h - v0.10.4 - public domain single-header display library
 *
 *   The window, the GL ES 3.0 context, the display mode and the swap path
 *   of a stimulus display, with flip at a time: each frame learns the
 *   predicted onset of the flip it draws for, presents for a target time
 *   on the ysp/rt.h clock, and gets a record of the vblank it was shown on,
 *   the residual, the drops, the composition path and where the frame's
 *   time went. Also: display and mode lists, the mode whose refresh is a
 *   multiple of a video's rate, a photodiode patch, input timestamps on
 *   the ysp/rt.h clock, and several displays.
 *
 *   REQUIRES ysp/rt.h beside it (the clock, the waits, the event ring, the
 *   instrumentation macros), ysp/input.h (the input event, v0.4.0), and
 *   SDL3 (3.2 or later) to link. On Windows it
 *   also needs ANGLE's libEGL.dll and libGLESv2.dll at run time; nothing
 *   ANGLE or Khronos is needed to compile. Under Emscripten, SDL3's port
 *   and ASYNCIFY or JSPI (BACKENDS, WEB). Copy ysp/screen.h and ysp/rt.h.
 *
 *   Written in the single-header style of the stb / sokol libraries. C99 is
 *   the floor: it builds as C99, C11 and C++17, and in the C dialect MSVC
 *   compiles by default.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *   Do this:
 *
 *       #define YSP_SCREEN_IMPLEMENTATION
 *
 *   in *one* C or C++ file before including this header to create the
 *   implementation. Every other file just includes the header normally.
 *   ysp/rt.h's implementation comes with it, once, as with the other ysp
 *   headers. Link SDL3.
 *
 *   A trial of ysp/timeline.h on the display (examples/screen/hello.c runs
 *   it):
 *
 *       #define YSP_SCREEN_IMPLEMENTATION
 *       #include "ysp/screen.h"
 *
 *       static yscr_screen scr;                 // zeroed
 *       yscr_desc d = { 0 };                    // fullscreen, primary display
 *       d.ring = &ring;
 *       if (!yscr_open(&scr, &d)) die(yscr_error(&scr));
 *       yscr_frame f;
 *       int rc;
 *       while ((rc = yscr_begin(&scr, &f)) == YSCR_OK) {   // Shift+Esc ends it
 *           if (f.index == 0) ytl_anchor(&tl, TRIAL, f.onset, 0);
 *           ytl_frame tf = { f.onset, f.period, f.index };
 *           int n = ytl_evaluate(&tl, &tf, fired, 8);
 *           draw(ytl_values(&tl), n);
 *           if (yscr_flip(&scr) < 0) die(yscr_error(&scr));   // at f.onset
 *       }
 *       if (rc < 0) die(yscr_error(&scr));      // else YSCR_QUIT: an abort
 *       yscr_close(&scr);
 *
 *   The timeline evaluates at f.onset, the predicted onset of the flip the
 *   frame draws for, so what a frame shows is a function of when it
 *   appears. The measured record of each flip arrives one frame later, in
 *   f.last and in the ring. Every call that fails writes yscr_error()
 *   before it returns its code. In C99 and C++20 a compound literal does
 *   the desc in one line: yscr_open(&scr, &(yscr_desc){ .ring = &ring }).
 *
 *   Psychtoolbox and PsychoPy, for comparison:
 *
 *       vbl = Screen('Flip', win);                % PTB
 *       for i = 1:n
 *           Screen('FillRect', win, contrast(i) * 255, rect);
 *           [vbl, onset, ~, missed] = Screen('Flip', win, vbl + 0.5 * ifi);
 *       end
 *
 *       for frame in range(n):                    # PsychoPy
 *           grating.contrast = contrast[frame]
 *           grating.draw()
 *           win.flip()
 *
 *   Both draw frame i for "the next flip" and leave the caller to work out
 *   when that is. PTB's Flip blocks until the vblank and returns its time;
 *   yscr_wait_flip() does the same here, at the cost of one frame of
 *   pipelining.
 *
 *   SDL calls on the window go through yscr_window_call(), because on
 *   Windows a thread of the header's own owns the window (WINDOW THREAD):
 *
 *       static void title(void* t) { SDL_SetWindowTitle(yscr_window(&scr), (const char*)t); }
 *       static char text[] = "block 2";
 *       yscr_window_call(&scr, title, text);
 *
 *   Present at a later time, and mark the phases of a frame:
 *
 *       yscr_begin(&scr, &f);
 *       ytl_evaluate(...);
 *       yscr_mark(&scr, YSCR_PHASE_EVALUATE);
 *       upload_textures();
 *       yscr_mark(&scr, YSCR_PHASE_UPLOAD);
 *       draw(...);
 *       yscr_flip_at(&scr, f.onset + 3 * f.period, NULL);   // 3 frames later
 *
 *   ---------------------------------------------------------------------
 *   MODEL
 *   ---------------------------------------------------------------------
 *   TIME
 *     Every time is an int64_t count of nanoseconds on the ysp/rt.h clock
 *     (yrt_now_ns()), the unit ysp/timeline.h takes. A backend that
 *     stamps on another clock converts at the boundary: DXGI's vblank times
 *     are QueryPerformanceCounter values, the counter yrt_now_ns() reads,
 *     and yrt_ticks_to_ns() converts them with no correlation.
 *
 *   THE GRID
 *     A fixed-refresh display flips on a grid of vblanks. The header keeps
 *     the newest vblank the OS reported (time and count) and the period:
 *     the mode's rational period at first, then the period fitted from the
 *     OS's vblank times over at least 64 vblanks. The describe line shows
 *     the fitted refresh and its difference from the mode's (this laptop's
 *     panel: 60.0008 Hz). A flip's vblank is the grid's nearest to the
 *     OS's time, not the OS's count: on this laptop's DXGI_FLIP, while
 *     frames were held, the count lost about 1 vblank in 25, and 15% of
 *     the times were up to half a period off the vblanks, which the
 *     scanout (D3DKMTGetScanLine) showed steady on the grid; each frame
 *     flipped on the vblank nearest its time. An OS time more than 1% of a
 *     period off the grid sets YSCR_FLIP_GRID_UNSTABLE on its record, the
 *     record's onset is then the grid's vblank time, and the grid does not
 *     move to it: a grid moved there woke the next hold before the real
 *     vblank, and that frame was shown a vblank early (v0.4.2 and before).
 *     The grid moves to a new phase when 16 off-grid times in a row agree
 *     on it, as a changed display timing gives (DXGI's off-grid times
 *     agreed on at most 5 in a row). When 48 of the last 64 times are off
 *     the grid, the vblanks themselves move, or no grid fits: then the
 *     grid follows each time and takes the OS's count, as before v0.4.3,
 *     until 16 of 64 or fewer are off it (DXGI's held frames: at most 34
 *     of 64).
 *     After a gap (v0.10.1): when no OS time came for 0.5 s (the host
 *     waited between trials), the first begin() reads DWM's vblank time
 *     (DwmGetCompositionTimingInfo) and anchors the grid there, as an OS
 *     time, when it is within 1% of a period plus 100 ppm of the gap of
 *     the grid's vblank (at most a quarter period). Why: the OS's vblank
 *     times wander by tens of us over seconds, so a period fitted over the
 *     first seconds of a run was 1 to 41 ppm off, and across a gap of 10
 *     to 60 s the grid ended 0.2 to 2 ms off the vblanks; the next 17
 *     records were GRID_UNSTABLE with onsets that late. DWM's time sat
 *     within 4 us of the flip statistics' and is never older than a
 *     period (measured; docs/screen.md, "The grid after a gap"). The
 *     primary display only, at the screen's refresh: elsewhere the grid
 *     extrapolates as before. The describe line says
 *     gap_anchors=N(max move, refused M).
 *
 *   DEPTH
 *     The depth is the number of vblanks from a present call to the flip
 *     that shows it: 1 on an overlay or independent-flip path, 2 when the
 *     compositor copies the frame (measured, docs/screen.md). It comes
 *     from the path, never from misses (decided 2026-10-09: a depth that
 *     rises after misses adds a vblank of latency in the middle of a
 *     task):
 *       - open(), DXGI_FLIP: measured. Black frames until three flips in
 *         a row agree, at least 6 and at most 40, because a window that
 *         has just gone fullscreen is composed for a few frames.
 *       - open(), COMPOSITION: the path's. A present there waits for its
 *         target, so an on-time flip shows only that the depth was enough.
 *       - a path change (YSCR_EV_PATH): the new path's, at once. That is
 *         the depth open() measured on that path, else 1 for independent
 *         flip and a DXGI overlay, 2 for composed and for COMPOSITION's
 *         overlay frames (they come with the compositor's statistics and
 *         showed on the vblank after next). An unknown path keeps it.
 *       - a miss: none. It is a drop, flagged and counted; its tier is as
 *         before.
 *     desc.depth = N (1 to 8) pins it: no measurement, no path following.
 *     On a path that needs more, every flip shows a vblank late (each
 *     record dropped 1, and f.onset a period early); on a path that needs
 *     less, every frame waits the extra vblanks and shows on time (on
 *     DXGI_FLIP the hold wakes at the path's depth, not the pin). A loop
 *     whose GPU work per frame is close to a period can pin 2.
 *     desc.depth_learn (opt-in, not with a pin) is v0.4.3's rule: on
 *     DXGI_FLIP three flips in a row at a new depth change it, and one
 *     flip after a path change; on COMPOSITION three misses in a row
 *     raise it, and it falls only on evidence: per path, the slack of each
 *     flip (the planned vblank's time minus the return of the present
 *     call), on time and missed, must show on 4 flips in a row that one
 *     depth less would have been on time 8 times as often as a miss with
 *     that slack or more; a miss within 4 flips of a lowering clears that
 *     path's on-time counts. Measured (docs/screen.md, "Depth from the
 *     path"): with steady GPU work near a period it dropped 1 to 6 frames
 *     in 600 where the default dropped 3 to 57; after each short run of
 *     misses it moved the depth twice, and the frames after showed early
 *     (DXGI_FLIP) or a vblank apart (COMPOSITION).
 *     Every change writes a YSCR_EV_DEPTH ring record (old, new, reason,
 *     path, frame), and the describe line reads depth=2(path,3 changes).
 *     When the depth falls (composed to overlay), a flip planned at the
 *     old depth can show a vblank early (EARLY). The next frame is planned
 *     a vblank after the early one's plan, never on it, so the early frame
 *     stays on the screen two vblanks; f.onset and f.vblank always rise
 *     from one frame to the next. When the depth rises, a flip planned at
 *     the old depth is shown late (a drop), and the next frame's f.vblank
 *     is 2 on: begin() runs before that drop's record completes.
 *     When open() gets no OS time (a window covered at open, or
 *     COMPOSITION's first statistics late), the grid is a guess and onsets
 *     are tier 3; the first OS time replaces the guess.
 *
 *   SETTLE
 *     The start of a run is not settled when open's depth is measured:
 *     a window moves from the composed path to an overlay plane after
 *     0.5 to 0.7 s (1.7 to 1.9 s under decode load), a window covered at
 *     open gets off-grid times for seconds, and the first flips can be
 *     early, late or without a statistic (docs/screen.md, "Settling at
 *     open"). So open() presents black frames after the depth until all
 *     of these hold at once, like Psychtoolbox's sync tests:
 *       - an OS vblank time (no guessed grid);
 *       - desc.settle_flips (default 6) flips in a row, each with an OS
 *         statistic, on the grid, not early, not late, tier 1 (2 in a
 *         window), on one path and one depth;
 *       - the last 20 vblank intervals: SD at most 50 us and mean within
 *         10 us of the mode's period (120 opens in front: at most 13.5
 *         and 6.7 us; settled, about 1 us);
 *       - the minimum time from the open() call: 2.5 s in a window, none
 *         fullscreen (desc.settle_min_ns; < 0 removes it).
 *     The cap (desc.settle_max_ns) is 3 s fullscreen and 4 s in a window,
 *     from the open() call. At the cap, desc.settle decides: STRICT makes
 *     open() fail with an error that names the condition, WARN opens and
 *     reports it, AUTO (the default) is STRICT fullscreen and WARN in a
 *     window, OFF skips settling. The environment variable YSP_SETTLE
 *     (off, warn or strict) overrides desc.settle, so a developer can turn
 *     it off without a rebuild; any other value fails open(). The SIM
 *     backend and a presenter without OS vblank times skip it. An abort
 *     during settling ends it; the first begin() reports the abort.
 *     Every outcome is recorded, overrides and skips too: the describe
 *     line says settle=0.66s(24 flips), settle=FAILED(grid,flip41,2/6-
 *     clean,warn), settle=SKIPPED(env YSP_SETTLE=off) or ABORTED; a
 *     failure adds WARNING=not-settled; the ring gets one YSCR_EV_SETTLE
 *     record; yscr_settle_check() gives the numbers and a message.
 *     Settle frames are not records: they reach neither f.done nor the
 *     ring's flip records, and the first frame after open() is frame 0.
 *     A statistic for a present that already completed (COMPOSITION in a
 *     window repeated each flip as an overlay frame) changes no path.
 *     open() returns holding a free back buffer, so GL work before the
 *     first begin() (ygfx_prime) draws into a buffer no present holds.
 *
 *   FOREGROUND
 *     Windows gives the foreground (the keyboard) only to a program that
 *     the foreground program started or that had the last input. A
 *     program started by a scheduler, a remote shell, a test runner or
 *     any process in the background does not get it: measured, the OS
 *     refused 20 of 21 such opens, open() settled anyway, the window
 *     showed under the other program's window and the taskbar in the
 *     z-order, and the keys went to the other program (docs/screen.md,
 *     "The foreground"). desc.foreground decides what open() does:
 *       AUTO   (0, default) FORCE fullscreen, HONOR in a window.
 *       HONOR  ask once; take what the OS gives.
 *       FORCE  ask once; if the OS refuses, raise again with SDL's
 *              SDL_HINT_FORCE_RAISEWINDOW (it attaches to the foreground
 *              thread's input), then put the hint back as it was.
 *     The describe line says foreground=granted, forced or refused
 *     (refused adds WARNING=not-foreground), and so do YSCR_EV_SETTLE's
 *     u.u32[8] and yscr_settle_info.foreground (YSCR_FG_*). Measured, a
 *     forced raise left the program that had the foreground without it
 *     after close (9 of 9); so close() gives it back to that window when
 *     the raise was forced and the screen still has the foreground. A
 *     refused foreground does not fail open(); settling decides that.
 *     Other platforms and a screen without a window report n/a.
 *     The next screen of the process (v0.10.2): a close() while the
 *     screen has the foreground keeps the right for the process
 *     (AllowSetForegroundWindow), so the next open() gets the foreground
 *     as this one did. With the window thread each run of screens has a
 *     new thread, and Windows had refused the raise of every screen after
 *     the first one in a window (docs/screen.md, "The next screen's
 *     raise").
 *
 *   CONTEXT HOOK (v0.5.2)
 *     open() settles for 0.5 to 0.8 s fullscreen and 2.5 s in a window.
 *     desc.on_context lets other work use that time: open() calls it
 *     once, when the GL context exists (current on the calling thread),
 *     before the warm-up's first present. Give it work that runs on other
 *     threads, such as ysp/gfx.h's ygfx_open_start() (ANGLE compiles the
 *     programs on its own threads while open() settles), and let open()
 *     go on; finish the work after open() returns (ygfx_open()). Do not
 *     present, call begin() or close the screen in the hook. Work that
 *     holds the thread delays the warm-up and settling: open() adds the
 *     hook's time to the cap, so a slow hook cannot fail settling, but
 *     the minimum (2.5 s in a window) stays from the open() call. After
 *     the hook, open() binds the default framebuffer, so its black frames
 *     clear the back buffer whatever the hook bound. Every open that gets
 *     a context calls it, also SIM and settle OFF; an open refused before
 *     that does not. If open() fails after the hook, the context is gone:
 *     ygfx_close() then frees the gfx and deletes nothing. The describe
 *     line adds the hook's time to the settle token
 *     (settle=0.78s(25 flips,hook=0.015s)); yscr_settle_info.hook_ns
 *     has it. Measured on the Iris Xe, 160 opens (docs/screen.md,
 *     "Compiles during settling"): settling took the same time and the
 *     same presents with ysp/gfx.h's compiles running, and every open
 *     settled; the hook before the warm-up gave the compiles 0.2 s more
 *     than one after it.
 *
 *   PREDICTION
 *     yscr_begin() predicts the first vblank a present made now can
 *     reach: the vblank at or before now plus a margin (an eighth of a
 *     period, at most 1 ms), plus the depth, and never one at or before
 *     the last flip. f.onset is that vblank's time plus desc.onset_offset_ns.
 *
 *   FLIP AT A TIME
 *     yscr_flip_at(t) snaps t to a vblank with ysp/timeline.h's rule: the
 *     first vblank at or after t - lead x period (lead x period truncated to
 *     whole ns). The default lead is 0.5, the nearest vblank, because a
 *     predicted onset carries noise: with "at or after", a t one
 *     microsecond past a vblank goes a whole frame late. A flip and a
 *     timeline event at the same time land on the same frame.
 *     YSCR_LEAD_NONE is never early: the first vblank at or after t.
 *     When the snapped vblank is before the first one the present can
 *     still make, the flip goes to that one and its record has
 *     YSCR_FLIP_LATE_TARGET: the caller was late. When it is later, the
 *     header sleeps on the ysp_rt clock until just after vblank
 *     (planned - depth), and presents then; DXGI shows a frame at the first
 *     vblank it can, so this is what puts it on the planned one. The same
 *     wait applies when the call comes within the margin before a vblank,
 *     which the plan already put the flip after. (DXGI's
 *     own SyncInterval was measured as the other way to hold a frame and
 *     put 795 of 1800 held frames early; see docs/screen.md.)
 *
 *   THE RECORD (yscr_record)
 *     index     the frame number
 *     target    t as given to flip_at
 *     planned   the vblank time the flip was planned for, + offset
 *     onset     the time of the vblank it was shown on, + offset: the OS's
 *               vblank timestamp on DXGI, or the grid's vblank nearest
 *               it when it is off the grid (GRID_UNSTABLE), not a
 *               photon. The photodiode
 *               test (tests/loopback/) measures the difference, which goes
 *               in desc.onset_offset_ns.
 *     residual  onset - target
 *     dropped   vblanks between planned and shown: the system's lateness,
 *               not the caller's (that is LATE_TARGET)
 *     path      YSCR_PATH_COMPOSED, _OVERLAY or _INDEPENDENT on Windows,
 *               _BROWSER in a browser
 *     tier      how far the onset can be trusted (TIERS)
 *     flags     PENDING (not shown yet), ESTIMATED (no OS time for this
 *               present; onset is the planned vblank), LATE_TARGET,
 *               OCCLUDED (no statistic came: the window was covered),
 *               GRID_UNSTABLE (the OS's time was off the vblank grid;
 *               onset is the grid's vblank nearest it), EARLY (shown
 *               before the planned vblank), SKIPPED and CANCELED (never
 *               shown: a later present took the vblank; onset 0, residual
 *               0), ONSET_PLANNED (the onset is the vblank the system
 *               planned the frame for, not one it observed), BELOW_TIER
 *               (tier worse than desc.min_tier)
 *     phase_ns  where the frame's time went, below
 *     A record completes when the OS reports its flip, normally in the next
 *     begin(). DXGI reports only the newest flip, so the header reads the
 *     statistics again before a present and once per vblank while it holds
 *     a frame; a flip that is never reported still gets a record, flagged
 *     ESTIMATED. yscr_wait_flip() waits for all of them.
 *     f.last is the newest completed record. f.done and f.n_done list
 *     every record completed since the begin() before, in completion
 *     order (the ring's), whether it completed in begin(), in flip_at()
 *     while a frame was held, or in wait_flip(); the last one is f.last.
 *     The list is valid until the next begin(), and holds at most
 *     YSCR_MAX_DONE (16; f.done_lost counts any that did not fit). A
 *     record completed in close() reaches the ring and on_flip only.
 *
 *   PHASES
 *     phase_ns[YSCR_PHASE_EVALUATE, _SCRIPT, _DRAW, _UPLOAD] is the time
 *     charged by yscr_mark(); the time not marked before flip_at() goes
 *     to _DRAW. _SWAP is begin()'s wait for a slot plus the present call,
 *     not flip_at()'s planned wait for a later vblank. _GPU is
 *     YSCR_PHASE_UNKNOWN in v0.1: a GL_EXT_disjoint_timer_query per frame
 *     was built and measured, and it tracked the GPU work but cost about
 *     250 us of CPU per frame through ANGLE, so it was taken out
 *     (docs/screen.md). A frame that was late shows which phase took
 *     the time, with no tool attached.
 *
 *   THE RING
 *     With desc.ring set, each completed record is one yrt_event:
 *       source  YRT_SRC_SCREEN        kind  YSCR_EV_FLIP
 *       t_ns    onset                   aux   desc.display_index
 *       u.i64[0]  target
 *       u.u16[4]  dropped, at most 65535
 *       u.u16[5]  path in bits 0..2, flags in bits 3..12, tier in bits
 *                 13..15: YSCR_EV_PATH_OF(), _FLAGS_OF(), _TIER_OF()
 *       u.u32[3..8]  phase_ns[0..5]
 *       u.u32[9]  the frame index, low 32 bits (ytl_event.frame)
 *     Residual is t_ns - u.i64[0]. A flip never shown carries its planned
 *     vblank in t_ns, because the ring reads 0 as "now". Also
 *     YSCR_EV_PATH when the path
 *     changes (u.u16[0] old, u.u16[1] new) and YSCR_EV_MODE at open
 *     (u.i32[0..5] w, h, refresh_num, refresh_den, backend, and how AUTO
 *     chose it: YSCR_AUTO_*). Records arrive in completion order, which is
 *     not always frame order. YSCR_EV_DEPTH at open and at each change of
 *     depth (DEPTH). YSCR_EV_TEXT_INPUT, YSCR_EV_DEVICE, YSCR_EV_RAW_MICE
 *     and YSCR_EV_INPUT_LOST: INPUT. YSCR_EV_UNSYNCED: SYNC GUARD.
 *     YSCR_EV_VISIBILITY: a page hidden or shown (BACKENDS, WEB).
 *
 *   ONE FRAME IN FLIGHT
 *     begin() waits until the swap path takes a frame, so the frame drawn
 *     is never more than one present behind the screen. DXGI: a frame
 *     latency waitable object with SetMaximumFrameLatency(1) and 2 buffers.
 *     COMPOSITION: the previous present has a statistic that settles it
 *     (shown, skipped or canceled) and one of 3 buffers is free. Present at
 *     a time could queue several future frames; that is not done, so each
 *     frame is drawn against a fresh prediction and fresh input.
 *
 *   GL STATE (what a renderer on this header, such as ysp/gfx.h, relies on)
 *     begin() makes the context current on the calling thread with the back
 *     buffer as framebuffer 0. caps.mode.w and caps.mode.h are the back
 *     buffer's size in pixels, in a window too. On DXGI_FLIP and
 *     COMPOSITION, framebuffer 0 is ANGLE's pbuffer on a D3D11 texture, and
 *     its row 0 is the top row of the screen, not the bottom as in GL
 *     (measured, docs/gfx.md); a renderer that writes rows in screen
 *     order must flip them. flip_at() on DXGI_FLIP and COMPOSITION changes
 *     no GL state (the patch is a ClearView after ANGLE's flush); on a
 *     presenter without draws_patch, the GL patch puts back what it changes
 *     (PHOTODIODE PATCH). Nothing else here touches GL state, so a renderer
 *     sets what it needs once per frame, except the present callback:
 *     yscr_on_present(fn) calls fn inside flip_at(), after your drawing
 *     and before the header's flush and present, with the context current
 *     and framebuffer 0 bound. fn may draw with GL and change any state;
 *     the header puts nothing back for it. Two counters tell a renderer
 *     that caches GL state what happened behind its back:
 *       yscr_gl_epoch()       changes after every present callback: GL
 *                               state may differ from your cache. Drop the
 *                               cache and set state again.
 *       yscr_gl_generation()  changes when the context is new: every GL
 *                               object made before is gone (0 while
 *                               closed). Today only open() makes a context,
 *                               and a lost device ends the screen with
 *                               YSCR_ERR_LOST, so it is constant from
 *                               open to close.
 *     YSCR_HAS_GL_EPOCH is defined when both exist.
 *     Between flip() and the next begin() (v0.10.3), framebuffer 0 is a
 *     buffer that no present holds: GL work there (ygfx_prime(), uploads,
 *     a clear) does not wait for the screen, and shows nothing until a
 *     frame draws it. Read framebuffer 0 after your drawing and before
 *     flip() (ygfx_read_output()); after flip() what it holds is not
 *     defined. On COMPOSITION, when no buffer is free at the flip, no surface
 *     is current until bind() or begin() finds one: draws to framebuffer
 *     0 then do nothing (measured: a buffer was free at every present;
 *     docs/screen.md, "Between frames"; the case without one is tested
 *     through a hook since v0.10.4).
 *
 *   TIERS (yscr_tier, record.tier, caps.worst_tier, desc.min_tier)
 *     Each flip gets a tier from its backend, its path and the source of its
 *     onset, as measured on the one machine of STATUS:
 *       1  the OS observed the flip: DXGI_FLIP on the overlay or
 *          independent-flip path, COMPOSITION on independent flip
 *          (IndependentFlipFrame's displayed time sat within 0.1 ms of the
 *          scanout's own vblank, and moved with a late frame)
 *       2  good under conditions: DXGI_FLIP composed (DWM's vblank, one
 *          display), and any flip whose OS time was off the vblank grid
 *          (GRID_UNSTABLE: its onset is the grid's vblank)
 *       3  for task development only: an ESTIMATED or ONSET_PLANNED onset,
 *          such as every composed or overlay frame of COMPOSITION, whose
 *          time is DWM's plan for a vblank about 15 ms ahead
 *       YSCR_TIER_SIM  the simulated display
 *       YSCR_TIER_WEB  a browser (BACKENDS, WEB): the time a frame callback
 *          began, not an observed flip; the lowest tier
 *     A custom presenter may set the tier in each yscr_vblank; 0 takes the
 *     path's default. caps.worst_tier and the describe line give the worst
 *     tier since open. desc.min_tier (1 to 3) sets BELOW_TIER on each flip
 *     worse than it; the run goes on, and the caller decides what to do
 *     with those trials.
 *
 *   ---------------------------------------------------------------------
 *   BACKENDS
 *   ---------------------------------------------------------------------
 *   YSCR_BACKEND_AUTO (desc.backend 0)
 *     Under Emscripten, WEB. On Windows, COMPOSITION where it opens and the presentation factory reports
 *     independent flip for the window's output (Windows 11 x64), else
 *     DXGI_FLIP. Decided 2026-10-09 on 10-minute runs that showed
 *     COMPOSITION no worse (docs/screen.md, "AUTO"). The describe line
 *     says backend=composition(auto), or backend=dxgi_flip(auto,
 *     composition-refused:"why"), and YSCR_EV_MODE's u.i32[5] is
 *     YSCR_AUTO_NAMED, _COMPOSITION or _FALLBACK. A backend named in
 *     desc.backend is opened or fails, as before.
 *   YSCR_BACKEND_DXGI_FLIP (Windows 10 and 11)
 *     SDL3 makes the window, on the window thread (WINDOW THREAD). The
 *     header makes a D3D11 device on the adapter
 *     whose output shows the window (on a laptop with two GPUs, the one
 *     wired to the panel; any other means a copy between adapters and a
 *     composed path), a DXGI_SWAP_EFFECT_FLIP_DISCARD swapchain on the
 *     window, and an ANGLE display on that device
 *     (EGL_ANGLE_device_creation_d3d11). ANGLE renders into swapchain
 *     buffer 0 through a pbuffer (EGL_ANGLE_d3d_texture_client_buffer); in
 *     D3D11's flip model buffer 0 always names the current back buffer.
 *     The header does not use SDL's GL path on Windows: through it ANGLE
 *     makes a blt-model swapchain, which gives no vblank statistics, no
 *     frame latency control and no independent flip (docs/screen.md).
 *     libEGL.dll is loaded at run time from desc.angle_dir, else from the
 *     directory in the YSCR_ANGLE_DIR environment variable, else from the
 *     program's directory and the DLL search path; with a directory, the
 *     libGLESv2.dll beside it is the one used. The context is GL ES 3.0
 *     with an RGBA8 back buffer and no depth buffer. DXGI discards the back
 *     buffer at each present, so draw every pixel of every frame.
 *     A fullscreen screen is a borderless window over the display in its
 *     current mode, which DXGI promotes to an overlay plane or to
 *     independent flip; the record's path says which. A window is composed.
 *   YSCR_BACKEND_COMPOSITION (Windows 11, x64; open fails elsewhere)
 *     The same device, adapter choice and ANGLE context as DXGI_FLIP, but
 *     no swapchain: a presentation manager (IPresentationManager) shows our
 *     own 3 textures, each a client-buffer pbuffer for ANGLE, through a
 *     DirectComposition visual on the window. Each present carries a target
 *     time, so flip_at() never waits: measured, the system shows the frame
 *     on the vblank nearest the target, ties to the earlier one, which is
 *     ysp/timeline.h's rule, and the header passes the planned vblank's
 *     time itself. On independent flip the shortest lead that met its
 *     vblank was the next vblank; on the composed path, the one after. The
 *     target is QPC time in 100 ns units, not interrupt time, which is 20
 *     ms away from it on this machine (STATUS). Statistics: an independent
 *     flip reports its displayed time (tier 1); a composed or overlay frame
 *     reports a CompositionFrame, whose time the header reads from
 *     DComp's target statistics and flags ONSET_PLANNED (tier 3). A window
 *     that another window covers gets no statistic at all: such a flip
 *     completes after 3 periods as ESTIMATED and OCCLUDED. Without
 *     IPresentationSurface::SetSourceRect nothing showed; the header sets
 *     it. The header declares the COM interfaces itself, because dcomp.h
 *     is C++ only, MinGW-w64 has no presentation.h, and presentation.h's C
 *     declarations return SystemInterruptTime and LUID by value where the
 *     C++ methods return them through a hidden pointer;
 *     tests/compile/screen_com.cpp checks every slot against the SDK.
 *     AUTO picks it where it opens with independent flip.
 *     IDLE WAKE: measured, the first present after 67 ms or more without
 *     one (4 vblanks; 50 ms was fine) showed one vblank after its target,
 *     on independent flip, also with the CPU or the GPU busy and
 *     no present. A thread that waits on the display's vblank event
 *     (D3DKMTWaitForVerticalBlankEvent) while the screen is open removed
 *     it, so the header runs one; the describe line says idle_wake=on.
 *   YSCR_BACKEND_SIM
 *     No window, no GL: a vblank grid on the ysp_rt clock with
 *     desc.sim_period_ns (default 1/60 s), which shows every frame on the
 *     vblank it was planned for. For tests, CI and dry runs of a timeline.
 *     On the real clock it sleeps to each vblank, so a loaded host drops
 *     frames as a real display does. On ysp/rt.h's virtual clock
 *     (yrt_virtual_begin(), v0.8.0) the sleep jumps the clock to the
 *     vblank: no frame drops, the records are the same on every run, and a
 *     run takes the CPU time of its frames, not their display time. The
 *     describe line then says clock=virtual. Only SIM and CUSTOM open on
 *     the virtual clock; a real backend is refused.
 *   YSCR_BACKEND_CUSTOM
 *     desc.presenter and desc.presenter_ctx: your swap path (PRESENTER).
 *     AUTO leaves the window thread off for it; with desc.window_thread
 *     ON its window belongs to the window thread, and the presenter calls
 *     SDL on it only through yscr_window_call() (WINDOW THREAD).
 *   YSCR_BACKEND_WEB (Emscripten, v0.7.0; docs/web.md)
 *     SDL3's Emscripten port makes the window: a canvas, "#canvas" unless
 *     SDL's hint or window property names another. The header makes the
 *     WebGL2 context on it: no alpha, depth, stencil or multisampling, the
 *     drawing buffer not preserved, the high-performance GPU. Setting
 *     Module.yscrDesynchronized = true in the page asks for a
 *     desynchronized context (not measured; off by default).
 *     The frame loop stays a blocking loop. begin() awaits the next
 *     requestAnimationFrame callback, so build with -sASYNCIFY or -sJSPI;
 *     the frame is drawn in that callback's task and composited with that
 *     browser frame. A callback that ran more than half a period ago (the
 *     program set up after open(), or the browser ran it late) is not used:
 *     begin() waits for the next (late= in the describe line), because a
 *     draw there was replaced by the next callback's draw before it showed
 *     (measured in headless Chrome after a 17 ms setup).
 *     Timing. A browser reports when it began a frame, never when the
 *     display showed it. A record's onset is the first callback time after
 *     both the callback that drew the frame and the present call: the
 *     earliest vsync the frame can show at. Its flags have ONSET_PLANNED,
 *     its path is BROWSER and its tier YSCR_TIER_WEB. The compositor's delay
 *     after that vsync is in every onset and is not known here (one to
 *     three frames is usual; nothing here measured it). Callback times are
 *     performance.now() times, correlated to the ysp_rt clock at open, and
 *     the browser coarsens them: 100 us in Chrome and 1 ms in Firefox on a
 *     page that is not cross-origin isolated, 5 us and 20 us on one that
 *     is (measured, docs/web.md). They also jitter about the vsync grid, so
 *     the onsets are the grid's (GRID_UNSTABLE), and the describe line
 *     does not warn about off-grid times or about the period.
 *     caps.kind is YSCR_CALLBACK, caps.period_ns and the mode's refresh are
 *     the period measured at open, and caps.mode.w and h are the canvas's
 *     drawing buffer in device pixels. caps.native_target is set, so the
 *     core never sleeps: a wait on the browser's main thread is a busy
 *     loop that stops rendering.
 *     Settling. open() counts callbacks until the last 20 intervals are
 *     steady (none over 1.5 times their median, which is a skipped
 *     callback; SD at most 0.5 ms), for at least 0.25 s and at most 2 s,
 *     and takes their mean as the period. It never fails, whatever
 *     desc.settle says: a browser gives nothing to wait for. At the cap the
 *     describe line says settle=FAILED(spread,...,warn) and
 *     WARNING=not-settled. desc.settle OFF records SKIPPED; the period is
 *     measured all the same.
 *     Holds. flip_at() cannot hold a drawn frame, because the drawing
 *     buffer is not preserved: a frame shows at the next frame the browser
 *     composites, and a later target is recorded EARLY. Draw a frame in the
 *     begin() whose f.onset is its time, as ysp/timeline.h does. After a
 *     frame planned late shows on time all the same, the next frame is
 *     planned from begin()'s own prediction, not a vblank after the late
 *     plan.
 *     Fullscreen. A page enters fullscreen only from a user gesture, so the
 *     header never asks. desc.windowed false gives a canvas the size of the
 *     page's viewport at open; the page may enter fullscreen before
 *     (examples/web/site). desc.mode must stay zero. From v0.7.2 the header
 *     also keeps SDL from leaving the page's fullscreen: SDL's port applies
 *     a window's flags (not fullscreen) at SDL_CreateWindow, at
 *     SDL_DestroyWindow and when the page is shown again, and that called
 *     document.exitFullscreen() whenever the document was fullscreen. So
 *     the document's exitFullscreen does nothing during the create and the
 *     destroy, and SDL's visibilitychange handler is removed after the
 *     create (the header has its own; SDL sends no WINDOW_SHOWN or
 *     WINDOW_HIDDEN for the page on this backend). A browser's own exit
 *     (Esc, a switch of tab) is the page's to handle (docs/web.md).
 *     A hidden page gets no callbacks. begin() waits until the page is
 *     shown again (hidden= in the describe line); the frame in flight
 *     completes OCCLUDED, with no time; each change writes a
 *     YSCR_EV_VISIBILITY ring record at the next begin().
 *     Input: SDL's Emscripten events. SDL stamps an event when the page's
 *     handler ran (SDL_GetTicksNS), so a key pressed while a frame is drawn
 *     is stamped when that task ends. From v0.7.1 yscr_event_input() gives
 *     a key, mouse button, touch (finger) or pen press or release the DOM
 *     event's own time (Event.timeStamp, on the performance.now() clock;
 *     for input, when the OS gave it to the browser) where it finds it: a
 *     capture listener on the window keeps the newest 64 keydown, keyup
 *     and pointer events with the time their listener ran, which is in the
 *     same task as SDL's handler and just before it, and an SDL event takes
 *     the newest of its class (and pointer id, for a finger) whose listener
 *     ran at most 1 ms after SDL's stamp and 50 ms before it. Where none
 *     matches, SDL's stamp stays; dom_stamps=found/tried in the describe
 *     line counts them. yscr_poll()'s time and SDL's own events keep SDL's
 *     stamp. Touch comes as SDL's finger events from pointer events on the
 *     canvas (SDL 3.4.2 sets its touch-action to none): x and y 0..1 of
 *     the canvas's CSS box, finger id the pointerId + 1; SDL's mouse events
 *     made from touch are dropped by ysp/input.h as before. Not measured
 *     against a physical key or touch (docs/web.md).
 *     Not for a browser: triggers (they need -pthread and an isolated
 *     page; not tested), codes (drawn through GL, always at risk:
 *     CODE_RISK_COMPOSED), the gamma ramp, raw mice and the panic watchdog
 *     (panic=n/a).
 *   Not implemented; open() refuses them with a message:
 *     YSCR_BACKEND_GLX_OML (Linux X11),
 *     YSCR_BACKEND_WAYLAND (presentation-time), YSCR_BACKEND_METAL
 *     (macOS through ANGLE). On Linux and macOS only SIM and CUSTOM open.
 *
 *   ---------------------------------------------------------------------
 *   PRESENTER (the swap-path interface)
 *   ---------------------------------------------------------------------
 *   A yscr_presenter is a table of functions, the Presenter extension
 *   of the rig: open, close, acquire (wait for a slot, bind the context),
 *   present (queue the frame for a vblank count and time), completions
 *   (the flips since the last call, oldest first, each with its present id,
 *   vblank time on the ysp_rt clock, vblank count, path, tier and flags;
 *   a time of 0 means "none", and SKIPPED or CANCELED means "never
 *   shown"), gl_proc, bind
 *   and describe. The core owns the window, the prediction, the snap, the
 *   decision when to present, the patch (unless the presenter sets
 *   draws_patch and paints the rectangle in present), drops, records and
 *   the ring. The presenter owns its OS objects and converts its times to
 *   the ysp_rt clock. Version 2 adds: yscr_present_req.codes, n_codes and
 *   verify (drawn after the patch by a presenter with draws_patch); in the
 *   open struct n_codes, want_gpu_done and three counters a presenter that
 *   reads codes back writes; and gpu_done(), the highest present id whose
 *   GPU work has finished, called from the trigger worker thread. The core calls a presenter from the thread that calls
 *   begin and flip, never from two threads at once; acquire may block,
 *   present and completions must not block past their OS call. A presenter
 *   with native_target in its caps shows a frame on req->target_count
 *   itself; otherwise the core waits before present. The header's timing
 *   statements end at this boundary for a presenter that is not its own.
 *   tests/adapt/screen_test.c has a complete one.
 *
 *   ---------------------------------------------------------------------
 *   PHOTODIODE PATCH
 *   ---------------------------------------------------------------------
 *   desc.patch.on draws a square (desc.patch.size pixels, default 32) in
 *   desc.patch.corner, at the gray level yscr_set_patch() last set, on
 *   every frame, last, so it is on top. It is a scissored clear, never a
 *   draw call. On DXGI_FLIP and COMPOSITION it is
 *   ID3D11DeviceContext1::ClearView with one rectangle on the back buffer,
 *   after ANGLE's flush, and flip_at()
 *   changes no GL state at all. On a presenter without draws_patch the core
 *   clears through GL and changes, then restores, the draw framebuffer
 *   binding, GL_SCISSOR_TEST, the scissor box, the clear color, the color
 *   write mask and GL_RASTERIZER_DISCARD; nothing else. The patch is for
 *   the loopback test: toggle it with the stimulus, and a photodiode on
 *   that corner measures when the frame appeared.
 *
 *   ---------------------------------------------------------------------
 *   CODES (desc.codes, yscr_code, yscr_code_frames, yscr_code_row)
 *   ---------------------------------------------------------------------
 *   A code is a pixel value a device reads from the video signal, so the
 *   display itself times the trigger. desc.codes declares up to
 *   YSCR_MAX_CODES slots at open: a SOLID rectangle of one value, or a
 *   ROW of w pixels each with its own value (YSCR_CODE_ROW_PIXELS in all
 *   ROW slots together). Values are 0xBBGGRR, red in bits 0 to 7, exact
 *   device values (8 bits per channel). Each slot shows its rest value
 *   until you set it: yscr_code() for the next flip only,
 *   yscr_code_frames() for n flips or YSCR_CODE_HOLD, and
 *   yscr_code_row() for a ROW. Codes are drawn last, after your frame
 *   and after the patch, on every flip.
 *     Pixel Mode (VPixx VIEWPixx, VIEWPixx /EEG and /3D, DATAPixx and
 *       DATAPixx3, PROPixx): the device sets its digital outputs from the
 *       top-left pixel of each frame, bits 0 to 7 from red, 8 to 15 from
 *       green, 16 to 23 from blue, and holds them while the pixel stays.
 *       yscr_slot_pixel_mode() is that pixel; yscr_pixel_mode_bits()
 *       gives the value for 24 output bits. Turn the mode on with VPixx's
 *       own tools (Datapixx('EnablePixelMode') in Psychtoolbox).
 *     Pixel sync (VPixx): a register write on the device waits for a
 *       sequence of pixels on a chosen raster line (VPixx recommends at
 *       least 8). yscr_slot_psync() is where Psychtoolbox's PsychDataPixx
 *       draws it, 8 pixels on scanline 0 from x = 10, and
 *       yscr_psync_pattern() its values. The USB register side is not in
 *       this header.
 *     Bits# T-Lock packets fit a ROW slot; this header builds no packet.
 *   Drawing. DXGI_FLIP and COMPOSITION: a SOLID slot is one
 *   ID3D11DeviceContext1::ClearView; a ROW of up to 8 pixels is one
 *   ClearView per pixel, a longer one an UpdateSubresource (CODES in
 *   STATUS has the costs); after ANGLE's flush, so no GL state changes. On
 *   a presenter without draws_patch the core clears through GL, one
 *   scissored clear per SOLID slot and per ROW pixel, with the patch's
 *   save-and-restore list. open() refuses slots that overlap each other or
 *   the patch (the default top-left patch is on the Pixel Mode pixel: move
 *   it with desc.patch.corner), slots outside the display, and codes on a
 *   windowed screen, which is not at the display's origin.
 *   Exactness. At open, DXGI_FLIP and COMPOSITION draw all 256 values of
 *   each channel through both drawing paths into a test texture and read
 *   them back (selftest= in the describe line). desc.verify_codes = N
 *   copies the first row of each code to a staging texture every N flips
 *   and compares it one flip later (yscr_code_verify()). Both check the
 *   back buffer only. What happens after it, the header reports where it
 *   can, in record.code_risk (and YSCR_FLIP_CODE_AT_RISK in the flags):
 *     COMPOSED        this flip went through DWM
 *     ADVANCED_COLOR  HDR or auto color management is on (Windows
 *                     DisplayConfig); DWM and the display kernel convert
 *                     colors then
 *     GAMMA           the OS gamma ramp is not identity (OS GAMMA)
 *     SCALED          the mode is not the panel's preferred mode
 *     UNVERIFIED      the self test did not run (SIM, a custom presenter)
 *     VERIFY_FAILED   a read-back differed (sticky)
 *     STATE_UNKNOWN   the display state could not be read
 *   It cannot see Night Light, accessibility color filters, the MHC
 *   calibration pipeline (matrix and LUT at scanout), GPU dithering, or the
 *   display's own processing; the describe line says mhc=unknown and
 *   nightlight=unknown. Check on the rig with the device's own read-back
 *   (VPixx: the top line through vline). desc.codes_strict makes open()
 *   fail when a display-wide risk is set.
 *   The ring gets one YSCR_EV_CODE record per flip on a screen with
 *   codes: t_ns the onset, u.u32[0] the frame index, u.u16[2] code_risk,
 *   u.u16[3] the slot count, u.u32[2..9] the value of each slot (a ROW's
 *   first pixel).
 *
 *   ---------------------------------------------------------------------
 *   OS GAMMA (desc.keep_os_gamma)
 *   ---------------------------------------------------------------------
 *   A device value is only exact if nothing changes it after the back
 *   buffer. A fullscreen screen on Windows reads the display's gamma ramp
 *   at open (GetDeviceGammaRamp). If it is not the 8-bit identity, it sets
 *   the identity (SetDeviceGammaRamp) and reads it back. At close, at
 *   exit() (atexit) and when SDL posts its quit event, it puts back the
 *   ramp it read. A crash leaves the identity ramp until the next mode
 *   change, logoff or another program sets one. A window never touches
 *   the ramp; desc.keep_os_gamma = true leaves a fullscreen screen's ramp
 *   alone too. The panic watchdog puts the ramp back before it ends a hung
 *   process (PANIC). The describe line says os_gamma=identity (it was),
 *   set, refused (Windows did not take it), unreadable, or the same with
 *   (kept); anything but identity or set sets CODE_RISK_GAMMA.
 *   Microsoft documents limits on these calls: SetDeviceGammaRamp may
 *   return success and not set a ramp, other programs and the OS may
 *   overwrite it at any time, a display event resets it, and its behavior
 *   is undefined in HDR. The ramp is checked at open only: one read costs
 *   0.8 ms (measured), too much for the frame loop. Whether the ramp acts
 *   on frames shown by independent flip or on an overlay plane, and
 *   whether Night Light replaces it, was not measured (it needs light or
 *   a capture card); do not rely on it there. Out of reach of this header:
 *   the MHC calibration pipeline, Night Light, HDR and auto color
 *   management. Later work: CGSetDisplayTransferByTable (macOS), the RandR
 *   CRTC gamma (X11), and wlr-gamma-control on wlroots compositors
 *   (Wayland has no general protocol).
 *
 *   ---------------------------------------------------------------------
 *   TRIGGERS (desc.triggers, yscr_trigger, yscr_trigger_at)
 *   ---------------------------------------------------------------------
 *   desc.triggers declares up to YSCR_MAX_TRIGGERS channels: a callback,
 *   its context and an offset. yscr_trigger(s, channel, code), between
 *   begin() and flip_at(), runs that callback at this flip's planned
 *   vblank + desc.onset_offset_ns + the channel's offset, on a deadline
 *   worker (ysp/rt.h WORKER) that open() starts when desc.n_triggers > 0.
 *   The callback runs on that thread, elevated; keep it to a port write:
 *       static void ttl(void* port, const yscr_trigger_info* i) {
 *           ypar_pulse_async((ypar_port*)port, (uint8_t)i->code, YRT_MS(2));
 *       }
 *   ypar_pulse_async() writes the leading edge on the calling thread, here
 *   the worker at the deadline. yscr_trigger_at(s, channel, code, t)
 *   fires at time t, tied to no flip.
 *   When the header learns that a flip will be late, its triggers move:
 *     the caller was late    flip_at() planned a later vblank before the
 *                            trigger was armed (LATE_TARGET); no move
 *     the present returned   past the planned vblank's latch: the trigger
 *                            is armed for the vblank the frame can still
 *                            make (YSCR_TRIG_MOVED)
 *     the record completes   before the trigger fired, on another vblank:
 *                            it moves to that vblank (COMPOSITION's
 *                            composed path reports before the vblank)
 *     desc.trigger_fence     the worker checks a D3D11 fence 1 ms before
 *                            the deadline; GPU work not done means the
 *                            frame will miss, and the trigger moves one
 *                            vblank (YSCR_TRIG_GPU_MOVED). Off by
 *                            default (TRIGGERS in STATUS)
 *   A miss learned only from the statistic, after the trigger fired, is a
 *   mismatch: the result says FIRED_EARLY and by how many vblanks. A move
 *   never fires a trigger twice: a trigger the worker has taken cannot
 *   move, under one lock. close() stops the worker, which runs anything
 *   still armed at once with YSCR_TRIG_FLUSHED: a callback should not
 *   pulse then. A frame takes at most YSCR_MAX_JOBS triggers, and as
 *   many can be pending (YSCR_ERR_REFUSED beyond).
 *   Where the worker runs (desc.trigger_cpu). The rule: keep the worker
 *   off the CPUs that service the display's vblank DPC. A DPC runs ahead
 *   of every thread, at any priority, so a worker spinning on that CPU at
 *   the vblank waits for it. Which CPUs those are depends on the machine:
 *   the GPU, its driver, the interrupt affinity policy, the core types.
 *   This header knows one machine. On the laptop of STATUS (i7-1360P,
 *   Iris Xe) the DPC took 50 to 400 us from about 75 us before the vblank,
 *   2 to 3% of one P-core, on the P-core the worker spun on, and made
 *   about 1 trigger in 30 over 20 us late; on the E-cores it did not reach
 *   the worker (p99 1 to 3 us). So the default, on Windows on a hybrid
 *   CPU, is a hard affinity mask of the E-cores (with a soft E-core
 *   preference Windows still ran the worker on P-cores). That is this
 *   machine's answer, not a rule: on a CPU of one core type there are no
 *   E-cores and the default is ysp/rt.h's placement, and on another GPU or
 *   interrupt policy the E-cores may be the busy ones. Check it: run
 *   examples/screen/flipstats.c --trigger, which prints the late triggers
 *   per CPU, and read "% DPC Time" per processor (typeperf) during a run.
 *   k > 0 pins the worker to logical CPU k - 1 (a pinned worker cannot
 *   leave a busy CPU: measured up to 48 ms late); -1 leaves it where
 *   ysp/rt.h puts it (the P-cores of a hybrid CPU). The describe line
 *   says where: worker=TIME_CRITICAL/E-cores:cpus=0xff00, .../pinned:
 *   cpus=0x..., or .../ysp_rt. Later: at open, sample the DPC count of
 *   each CPU for about 0.5 s and place the worker away from the busiest.
 *   desc.trigger_spin_ns is the worker's spin window (ysp/rt.h WAITS).
 *   After a flip trigger fires, the worker arms itself 50 us before the
 *   next vblank's deadline, so the next frame's trigger needs no wake-up
 *   from the frame thread; when no trigger comes, that costs the worker
 *   one spin window for nothing.
 *   The ring gets one YSCR_EV_TRIGGER record per trigger when its flip
 *   has completed and it fired or was canceled: t_ns the fired time (the
 *   deadline if not fired), u.i64[0] the deadline, u.i64[1] the flip's
 *   onset, u.u32[4] the code, u.u32[5] the frame index, u.u16[12] the
 *   channel, u.u16[13] the flags, u.i32[7] the mismatch in vblanks,
 *   u.i32[8] fired minus deadline in ns.
 *
 *   ---------------------------------------------------------------------
 *   AFTER FLIP (yscr_on_flip)
 *   ---------------------------------------------------------------------
 *   yscr_on_flip(s, fn, ctx) calls fn with each completed record, oldest
 *   first, and the results of its triggers, on the frame thread, wherever
 *   records complete: begin(), wait_flip(), close(). For network markers
 *   (stamp them with the record's onset, not with "now"), logs, and
 *   trigger mismatches. It is late by design, by how long the record takes
 *   to complete (TRIGGERS in STATUS); on COMPOSITION's composed path it
 *   runs before the frame is on the screen, because the statistic is DWM's
 *   plan (ONSET_PLANNED). A trigger not fired yet when its record
 *   completes has YSCR_TRIG_PENDING. fn's time counts in begin().
 *
 *   ---------------------------------------------------------------------
 *   NATIVE HANDLES (yscr_native)
 *   ---------------------------------------------------------------------
 *   yscr_native(s, &out) gives a DXGI_FLIP or COMPOSITION screen's
 *   D3D11 device, its immediate context, ANGLE's EGL display and the LUID
 *   of the adapter the device is on, for a library that shares textures
 *   with the screen (ysp/video.h's GPU path). Any other backend or
 *   platform: YSCR_ERR_NOT_IMPLEMENTED and zeros.
 *     Lifetime  the pointers are borrowed: no reference is added. They are
 *               valid from open() to close(); keep none past close(), and
 *               release any object you made on the device before it.
 *     Threads   the device is free-threaded (created without
 *               D3D11_CREATE_DEVICE_SINGLETHREADED): another thread may
 *               create resources on it. The immediate context is the frame
 *               thread's, shared with ANGLE: use it only on the frame
 *               thread, between begin() and flip_at(), and leave no state
 *               bound in it, because ANGLE caches the context's state and
 *               does not know of your calls.
 *     Video     desc.d3d11_video makes the device with
 *               D3D11_CREATE_DEVICE_VIDEO_SUPPORT and turns on multithread
 *               protection (ID3D11Multithread::SetMultithreadProtected), so
 *               a decoder thread can use the device's video interfaces and
 *               its calls on the immediate context take turns with the
 *               frame thread's and ANGLE's. out.video and out.mt_protected
 *               say what the device has, read from the device. An adapter
 *               without D3D11 video support makes open() fail with a
 *               message; there is no fallback. The frame thread may queue
 *               ID3D11DeviceContext4::Wait() on a fence the decoder
 *               signals, between begin() and flip_at(): it holds back the
 *               GPU queue, not the thread. Cost, measured with no decoder
 *               on the device: no difference in the header's CPU time per
 *               frame (STATUS). With a decoder, each immediate-context call
 *               takes a lock the decoder may hold; ysp/video.h measures
 *               that. Off by default.
 *
 *   ---------------------------------------------------------------------
 *   WINDOW THREAD (desc.window_thread, yscr_window_call; v0.10.0)
 *   ---------------------------------------------------------------------
 *   On Windows a thread of the header's own makes every window, reads its
 *   messages and makes every SDL video call. The frame thread (the one that
 *   calls begin() and flip_at()) keeps the D3D11 device, the swap path and
 *   ANGLE. Why: Windows marks a window hung when its thread reads no
 *   message for 4.2 to 4.6 s (measured), and puts a "Ghost" window in
 *   front of it. A host that waits between trials (Python's time.sleep()
 *   or input(), Octave's pause, C's Sleep()) did that, and the first 2 to
 *   20 frames after the wait went wrong. With the window thread no window
 *   was marked hung and the first frame after a wait of 5 to 30 s was on
 *   time; the frame loop's misses, residuals, paths and tiers did not
 *   change (measured; docs/screen.md, "The window thread").
 *   desc.window_thread:
 *     AUTO  (0, default) ON for DXGI_FLIP and COMPOSITION. OFF on the web,
 *           with YRT_NO_THREADS, for a custom presenter, and when the
 *           program started SDL's video on its own thread before the first
 *           open() (SDL made that thread its main thread).
 *     ON    the window thread, or open() fails with a message.
 *     OFF   the frame thread owns the window, as in v0.9.0.
 *   One mode per process, because SDL has one main thread: while screens
 *   with a window are open, open() refuses ON or OFF when it differs from
 *   theirs, and AUTO takes theirs (a custom presenter's AUTO beside the
 *   window thread fails). The describe line says window_thread=on(auto),
 *   on, off, off(auto,backend), off(auto,platform), off(auto,other-screen),
 *   off(auto,sdl-video-up), or n/a for a screen with no window.
 *   The thread: one per process, started by the first open() that needs
 *   it and stopped by the last close(); every screen's window is on it.
 *   It waits in MsgWaitForMultipleObjectsEx for a command or a message, at
 *   most 100 ms (4 ms while a screen has gamepads: SDL polls XInput in its
 *   pump), then calls SDL_PumpEvents(). The 100 ms wake is the heartbeat
 *   that tells the panic watchdog the thread answers (PANIC).
 *   What changes for the program:
 *     - SDL calls on yscr_window() go through yscr_window_call(s, fn,
 *       ctx): fn(ctx) runs on the window thread and the call returns when
 *       fn returns (an empty call: 6.5 to 11.4 us median, 27 to 44 us p99,
 *       measured).
 *       Without the window thread fn runs at once on the calling thread,
 *       so the same code runs either way. Do not call SDL on the window
 *       from your own thread: SDL's video calls belong to SDL's main
 *       thread, which is the window thread. That includes the title, the
 *       cursor, the mouse grab, text input, more SDL windows (make them in
 *       a window call, so the window thread reads their messages), Win32
 *       windows of your own, SDL_PumpEvents(), SDL_PollEvent() and Dear
 *       ImGui's SDL3 backend (docs/screen.md, "Dear ImGui"). Inside fn a
 *       yscr_window_call() runs at once. fn must not wait for the frame
 *       thread, which waits for fn.
 *     - Read events with yscr_poll(). begin() no longer pumps; the window
 *       thread pumps as messages come, so mouse, focus and close events
 *       reach SDL's queue while the host waits.
 *     - SDL keeps the string of a text, IME, drop or clipboard event in the
 *       memory of the thread that takes the event, and frees it at that
 *       thread's next pump. The frame thread does not pump, so after
 *       yscr_poll() returned such an event, the next begin() or close()
 *       calls SDL_CleanupTLS() on the calling thread (without it each typed
 *       character held 2 SDL allocations, measured). That also frees the
 *       strings SDL returned earlier on that thread (SDL_GetKeyName() and
 *       the like): copy one you keep.
 *     - The panic releases the display, and the process lives, while the
 *       window thread answers (PANIC).
 *   Commands and their time limit. The frame thread waits for the window
 *   thread only for a command (open, close, text input, a gamepad plugged
 *   in or out, the display lists, a window call), never in a frame. A
 *   command the window thread does not take within 5 s is withdrawn
 *   before it runs, and the call fails (YSCR_ERR_TIMEOUT; open() with a
 *   message): Windows calls a window hung after 5 s without a message
 *   read, so the thread is hung. Once taken, open()'s command and a window
 *   call run to their end, because they use the caller's memory (open()
 *   waits as long as v0.9.0's open() did on the same driver call).
 *   close()'s command works on a copy of what it needs: close() waits at
 *   most 5 s more, then returns with "the window thread did not answer" in
 *   yscr_error() and the window left until the process ends, so close()
 *   never hangs the host. After that open() refuses a window, because
 *   SDL's video belongs to the stuck thread. yscr_displays(), yscr_modes()
 *   and yscr_mode_multiple() run on the window thread while it runs (they
 *   are SDL video calls), else on the calling thread, as before.
 *   What stays on other threads: the frame thread reads two window flags
 *   each frame (SDL_TextInputActive(), SDL_GetWindowRelativeMouseMode():
 *   plain reads in SDL 3.4.0) and the HWND. DXGI and DirectComposition
 *   present from the frame thread to the window another thread of the
 *   process owns (measured at tier 1 on the overlay and independent-flip
 *   paths). SDL's raw keyboard thread stamps keys as before. DXGI's rule
 *   "never have the message-pump thread wait on the render thread" holds:
 *   the window thread never waits on the frame thread, and the header
 *   makes no exclusive fullscreen swapchain.
 *   Other platforms: macOS keeps Cocoa windows on the process's main thread
 *   (not planned here); the X11 and Wayland backends are stubs; the web has
 *   one thread.
 *
 *   ---------------------------------------------------------------------
 *   INPUT
 *   ---------------------------------------------------------------------
 *   yscr_poll() is the next SDL event plus its time on the ysp_rt clock.
 *   It takes the event with SDL_PollEvent(), or, with the window thread,
 *   with SDL_PeepEvents(), which does not pump (WINDOW THREAD). The time:
 *   SDL's event timestamp (SDL_GetTicksNS() units) moved onto the ysp_rt
 *   clock through a yrt_correlate() made at open and again every 10 s.
 *   The correlation is exact to its width (0.1 us here); the event's
 *   own timestamp is what SDL gives.
 *   On Windows the header turns on SDL's raw keyboard path
 *   (SDL_HINT_WINDOWS_RAW_KEYBOARD, at default priority, so your own setting
 *   of the hint wins) and stops SDL text input, which would route keys
 *   through the message path. On the raw path SDL reads Raw Input on its
 *   own thread and stamps a key when it arrives: measured, keys sent with
 *   SendInput() were stamped 0.18 to 0.67 ms after the call. That
 *   stamp does not include the keyboard's scan, its USB polling interval
 *   (1 to 8 ms on common keyboards) or the HID stack, which were not
 *   measured. With the raw path off, SDL uses the message time, which
 *   counts in the system tick: measured, keys came out stamped from 6.2 ms
 *   before to 12.1 ms after the call. On X11 the server's 1 ms
 *   time, on Wayland the compositor's. Keyboards are not response boxes;
 *   use ysp/serial.h for reaction times.
 *   With the raw path on, SDL 3.4 also sends a key from the window message
 *   when the message carries no scan code (read in SDL 3.4.0's source):
 *   keys injected by virtual-key code (SendInput with wVk only), keys with
 *   no scan code (some media keys), some on-screen keyboards and remote
 *   tools. Measured with SendInput (examples/screen/input.c --reports, 518
 *   taps): a virtual-key tap gave two press-release pairs, the second
 *   -0.1 to 34.2 ms after the first; a 100 ms virtual-key hold gave one
 *   press and a repeat; scan-code injection gave one report, tapped or
 *   held. Keys from a keyboard carry scan codes, so they most likely
 *   report once; screen_input --hand checks keys pressed by hand.
 *   yscr_event_input() drops the second reports (v0.4.1, "Second key
 *   reports" below); SDL's own event stream keeps them.
 *   caps.raw_keyboard says whether keys come on the raw path now: on
 *   Windows, with a window, the hint on and text input off.
 *   Devices (v0.3.4). Keyboards on the raw path are per device: a key
 *   event's `which` is the keyboard's Raw Input handle, so two keyboards
 *   give two ids (ysp/response.h keeps them apart). Keys on the message
 *   path (text input on, the hint off, keys with no scan code) and keys
 *   from SendInput have which 0. Mice: SDL reads Raw Input for mice only in
 *   relative mode; in its absolute mode, the one ysp/screen.h uses, every
 *   mouse comes as one pointer, which 0, stamped with the message time
 *   (tier 3 in ysp/response.h). The abort and panic keys count every
 *   keyboard on purpose: the watchdog's low-level hook sees the system's
 *   keys, not a device. A screen with a window and desc.ring writes a
 *   YSCR_EV_DEVICE record for each device: t_ns the time it was logged,
 *   aux desc.display_index, u.u32[0] YSCR_DEV_KEYBOARD, _MOUSE, _GAMEPAD,
 *   _TOUCH or _PEN, u.u32[1] YSCR_DEV_PRESENT (listed at open), _ADDED
 *   or _REMOVED, u.u64[1] the id (SDL's: the keyboard's or mouse's `which`,
 *   a gamepad's instance id, a touch device's touchID, a pen's id), and its
 *   name in u.bytes[16..39] (YSCR_DEV_NAME_OF(e): NUL-terminated, cut at
 *   23 bytes; empty for a removal). Keyboards, mice and touch devices are
 *   listed at open, gamepads too with desc.gamepads; hot-plug comes from
 *   SDL's ADDED and REMOVED events as they pass through yscr_poll();
 *   pens and touch devices SDL did not list get a record when first used.
 *   One record per change: a table of 32 devices per screen drops SDL's
 *   ADDED events for devices it listed already (a full table logs every
 *   report). The ids are Windows handles on Windows: a keyboard unplugged
 *   and plugged in again gets a new id, and ids are not stable across
 *   sessions; map them through the names in the same log.
 *   Raw mice (desc.raw_mice, v0.3.5, Windows). One thread per process
 *   registers Raw Input for mice (usage page 1, usage 2) for a message-only
 *   window of its own, at time-critical priority, as SDL's thread does for
 *   the keyboard, and stamps each report when GetRawInputBuffer() returns
 *   (the reports of one call share the stamp). From v0.4.0 each report
 *   goes over the input bridge as ysp/input.h events (yin_from_mouse():
 *   presses, releases, a movement, a wheel; device the Raw Input handle,
 *   SDL's mouse id and the YSCR_EV_DEVICE id, 0 for injected input), so
 *   yscr_poll() and yscr_event_input() deliver them with everything
 *   else; a mouse SDL does not list gets YIN_UNLISTED. Movement is in
 *   counts, unaccelerated (no pointer ballistics: the system cursor moves
 *   by other rules). A tablet, remote desktop or a VM sends positions
 *   instead (YIN_AXIS_ABSOLUTE, 0..1 of the primary display or the
 *   virtual desktop). yscr_poll_mouse() is DEPRECATED (v0.4.0, goes in
 *   v0.5): it reads the raw mouse records straight from the bridge's
 *   store, one event of a report per call (v0.3.5 gave a whole report),
 *   and their doorbells then decode as already taken. u.u32[1] of the
 *   YSCR_EV_RAW_MICE records counts the bridge's overwritten records.
 *   SDL keeps its own mouse events: the system cursor,
 *   clicks on other windows, the focus, Dear ImGui and an operator console
 *   work as before.
 *   Focus: registered without RIDEV_INPUTSINK, so reports come only while
 *   this process is in front. A participant's clicks while another program
 *   has the focus are not taken; an operator window of this process (an
 *   ImGui panel included) keeps the process in front. RIDEV_INPUTSINK was
 *   measured working and is not used: it would take every click on the
 *   desktop.
 *   Devices: SDL lists the mice it recognizes; Windows listed the same 3
 *   here. A precision touchpad's cursor, injected input and remote desktop
 *   come with no device handle: device 0, which cannot be told apart from
 *   each other (a touchpad is a digitizer; its fingers are a touch source,
 *   not a mouse). A report from a device SDL does not list gets
 *   YSCR_MOUSE_UNLISTED and, the first time, a YSCR_EV_DEVICE record
 *   (added, named "raw, not in SDL's list"); filter on the flag.
 *   Relative mode: Raw Input registration is per process and per usage.
 *   SDL's relative mode takes the mice while on and removes the
 *   registration for the whole process when it ends (measured). open()
 *   refuses raw_mice while any SDL window is in relative mode. Later, a
 *   guard in begin() reads the window's relative mode every frame (23 ns
 *   measured) and the registration when that changes and every 30th frame
 *   (GetRegisteredRawInputDevices, 0.4 to 11 us measured; 291 ns per frame
 *   on average): while relative mode is on, no raw reports come (SDL's
 *   mouse events carry the device then) and a YSCR_EV_RAW_MICE record
 *   says SUSPENDED; when it ends, or when other code took the mice, the
 *   reader registers again and the record says REGISTERED. Each screen with
 *   raw_mice shares the one reader; the last to close stops it. On other
 *   platforms, and with no window, open() refuses raw_mice. Measured with
 *   injected input only (examples/screen/input.c --mice is the hand test
 *   for two physical mice): reports stamped 0.15 to 0.40 ms after
 *   SendInput (means of three runs), no report lost or doubled with SDL's
 *   raw keyboard running.
 *   The input bridge (v0.4.0). One loop for every input: a producer on any
 *   thread (desc.raw_mice's reader, a response-box reader, an eye tracker)
 *   calls yscr_push_input(&e) with a ysp/input.h event stamped on the
 *   ysp_rt clock. The header stores it (a store of YSCR_INPUT_STORE
 *   records, 4096 unless defined before the implementation, one per
 *   process) and pushes one SDL event of type yscr_input_event_type()
 *   (registered at the first open with SDL), its user.code the record's
 *   sequence number: a doorbell. yscr_poll() returns doorbells in SDL's
 *   stream like any event; ImGui and other SDL consumers see the stream
 *   unchanged (they ignore an event type they do not know). Then
 *       while (yscr_poll(&scr, &ev, &t)) {
 *           ImGui_ImplSDL3_ProcessEvent(&ev);                  // optional
 *           if (yscr_event_input(&scr, &ev, &in)) yrsp_feed(&rsp, &in);
 *       }
 *   yscr_event_input() gives a doorbell's own record (keyed by the
 *   sequence, so a doorbell decoded late, out of order or never does not
 *   shift any other) and converts SDL's keyboard, mouse, touch, pen and
 *   gamepad events through ysp/input.h's adapter, with the screen's raw
 *   keyboard state and the ysp_rt time. A bridged event keeps the
 *   producer's stamp; SDL's push time is never used for it.
 *   Order: one producer's events come in its push order. Against SDL's own
 *   events, a doorbell sits where SDL queued it, so the stream is arrival
 *   order to within one pump of SDL's queue; every event carries its
 *   time, and ysp/response.h reads times, not order.
 *   Overflow: a record nobody decodes stays until the store wraps; then the
 *   newest overwrites it (counted), and its doorbell, if decoded later,
 *   returns false and counts as lost. Overwriting the oldest, not refusing
 *   the newest, keeps a skipped doorbell from holding its slot forever.
 *   SDL's queue takes 65,535 events (measured); a doorbell it refuses is
 *   counted, its record kept, and yscr_poll() pushes it again, oldest
 *   first, when SDL's queue is empty, so it arrives late but arrives.
 *   begin() writes a YSCR_EV_INPUT_LOST ring record when the counts of
 *   overwritten records, refused doorbells or lost decodes move (u.u32[0]
 *   to [3]: overwritten, refused, lost, still waiting; totals);
 *   yscr_get_input_stats() reads them. yscr_push_input() is for any
 *   thread; yscr_poll() and yscr_event_input() for one thread.
 *   Measured (AC, the timing guard, a 320 x 200 window): 4 producer
 *   threads at about 1 kHz each, 8000 events, every one decoded once and
 *   in each producer's order, none lost; yscr_push_input() 1.9 to 2.6 us
 *   with a window (SDL_PushEvent calls the event watches and wakes the
 *   queue), yscr_event_input() on a doorbell 74 to 81 ns, the doorbell's
 *   whole poll and decode 410 to 424 ns (SDL_PollEvent pumps the window's
 *   messages). 70,000 pushes with no poll: SDL refused 4465 doorbells, the
 *   store kept the newest 4096 records, all decoded over the next 3
 *   frames, and 65,535 doorbells of overwritten records counted lost. Raw
 *   mice through the bridge: 30 of 30 injected moves, with SDL's keys in
 *   the same loop.
 *   Second key reports (v0.4.1). yscr_event_input() returns false for a
 *   second report of a key press, so every consumer of the loop above
 *   gets one press per key, not only ysp/response.h's collector. The rule,
 *   on keyboard events from SDL (either path) and from doorbells alike:
 *     - a key-down that is not an OS repeat, of the same key (scancode)
 *       as a key-down kept less than desc.key_dedup_ns (0 = 50 ms) before
 *       or after it, from the same keyboard or with either device 0 (the
 *       message path and injected input have device 0), is dropped;
 *     - after such a drop, the first key-up of that key that finds no
 *       kept key-down still down is dropped too, so the consumer sees one
 *       down and one up; the kept press's own key-up passes, whichever
 *       comes first, and a key-up is paired with a kept key-down of its
 *       own device before one with device 0;
 *     - a key-up with device 0 is dropped first where a press kept with a
 *       keyboard's id has a dropped report pending (v0.4.2): the kept
 *       press's own key-up has that id, so this one is the second
 *       report's, also while another keyboard holds the same key;
 *     - an OS repeat (YIN_REPEAT) passes: SDL's key state marks the second
 *       report of a held key so, and a consumer's held-key rule reads it;
 *     - a key-down with the same time, device and key as the one kept is
 *       that report decoded again and passes, so two calls on one SDL
 *       event give the event twice.
 *   Why 50 ms: the largest measured gap was 34.2 ms (518 virtual-key
 *   taps), and one finger does not press one key twice within 50 ms
 *   (stated, not measured here). The first report to arrive is kept, not
 *   the earliest stamp: SDL queues the raw report first, and the message
 *   report's stamp is tick-quantized (-0.1 ms is in the table). Each drop
 *   counts in yscr_get_input_stats() (key_doubles, key_double_ups) and,
 *   on a screen with desc.ring, writes a YSCR_EV_KEY_DOUBLE record for a
 *   dropped key-down: t_ns the dropped report's time, aux
 *   desc.display_index, u.i64[0] its time minus the kept report's (ns),
 *   u.u32[2] the scancode, u.u32[3] its device, u.u32[4] the kept
 *   report's device. The table holds the 16 keys pressed last, for the
 *   process (SDL's queue is one stream for every window); s NULL uses 50
 *   ms. desc.key_dedup_ns < 0 turns it off for that screen's calls:
 *   every report, as SDL gives it. yscr_key_filter() applies the same
 *   rule, and the same table, to an event that did not come from
 *   yscr_event_input() (your own adapter, a simulated participant); never
 *   call it on an event yscr_event_input() returned. SDL's own events,
 *   read by Dear ImGui or by SDL_PollEvent(), are not changed. Measured
 *   with the core test only: no key was injected through it in a window.
 *   Gamepads (desc.gamepads): open() starts SDL_INIT_GAMEPAD and opens each
 *   gamepad; yscr_poll() opens one on SDL_EVENT_GAMEPAD_ADDED and closes
 *   it on SDL_EVENT_GAMEPAD_REMOVED (so read events with yscr_poll(), not
 *   SDL_PollEvent()); close() closes them and stops the subsystem. Up to 8
 *   gamepads. A screen with no window (SIM) does not start it. The stamps
 *   of gamepad events depend on SDL's driver for the pad (XInput is
 *   polled; Raw Input, HIDAPI and GameInput are not) and were not
 *   measured. Off by default: open() starts SDL's video subsystem only.
 *   Text input (yscr_text_input): a text box needs SDL text input
 *   (SDL_EVENT_TEXT_INPUT, and SDL_EVENT_TEXT_EDITING for an input method's
 *   composition), which open() stops. yscr_text_input(s, true, x, y, w,
 *   h) starts it on the screen's window and puts the IME candidate window
 *   by the rectangle x, y, w, h (window pixels, the caret's line); false
 *   stops it. Call it again while on to move the rectangle with the caret.
 *   While it is on, keys go through the message path: their times are not
 *   the raw path's (see above), so a typed response is untimed. Each change
 *   of on or off writes a YSCR_EV_TEXT_INPUT ring record (t_ns the call,
 *   aux desc.display_index, u.u32[0] 1 on or 0 off, u.i32[1..4] x, y, w, h,
 *   u.u32[5] 0), and every flip planned while it is on has
 *   YSCR_FLIP_TEXT_INPUT in its record (not in the ring's mode word,
 *   which holds 10 flag bits). On a screen with no window (SIM) only the
 *   record and the flag change.
 *   The state is observed, not only commanded: another library can start
 *   text input on the window (Dear ImGui's SDL3 backend does, on whichever
 *   window has the focus; docs/imgui_probe.md measured its keys 11.4 ms
 *   late). flip_at() reads SDL_TextInputActive() and, when it differs from
 *   the last logged state, writes the same record with u.u32[5] 1
 *   (external; t_ns when flip_at() saw it, the rectangle 0) before it sets
 *   the flip's flag. A change between two flips is seen at the next one,
 *   so the flag can be a frame late; caps.raw_keyboard reads the state at
 *   once.
 *
 *   ---------------------------------------------------------------------
 *   ABORT (desc.abort_keys, yscr_request_abort, f.abort)
 *   ---------------------------------------------------------------------
 *   An abort asks the frame loop to stop; the header never stops it. Each
 *   abort is reported once: the next begin() returns YSCR_QUIT with
 *   f.abort (the reasons, ORed), f.abort_presses and f.abort_ns (the first
 *   one's time), and every other field of f 0. The begin() after that
 *   starts a frame again. The caller decides: save and close, or ask the
 *   operator and go on. The header calls no exit() and closes no window.
 *     YSCR_ABORT_KEY      the abort combination, Shift+Esc by default.
 *                           Esc alone does nothing, so a participant who
 *                           hits Esc does not end the session. A held key
 *                           is one press: repeats do not count
 *     YSCR_ABORT_CLOSE    a close request for a screen's window: the
 *                           close button, the taskbar, Task Manager's "End
 *                           task"
 *     YSCR_ABORT_ALT_F4   Alt+F4. The header sets
 *                           SDL_HINT_WINDOWS_CLOSE_ON_ALT_F4 to "0" (at
 *                           default priority), so it is not a close request
 *     YSCR_ABORT_QUIT     SDL's quit event: Ctrl+C in the console, a
 *                           logoff, the last window closed (with CLOSE)
 *     YSCR_ABORT_REQUEST  yscr_request_abort(), from any thread: an
 *                           operator console, a response-box button
 *     YSCR_ABORT_PANIC    the panic released the display (PANIC, with
 *                           the window thread): from then on every
 *                           begin() reports it, and the program may only
 *                           close
 *   desc.abort_keys sets the combination: .key is an SDL keycode (0 =
 *   Esc); .mods are the YSCR_MOD_* that must be held (0 = Shift;
 *   YSCR_MOD_NONE = the key alone, for development; more modifiers held
 *   still count); .off turns the key off (the other reasons stay). There is
 *   one combination per process: open() refuses a desc that differs from
 *   an open screen's. The describe line says abort=shift+esc.
 *   The header sees each event as SDL queues it (SDL_AddEventWatch), so
 *   an abort counts although your code read the event first with
 *   yscr_poll() or SDL_PollEvent(). An SDL event filter that drops the
 *   event hides it. Reports of one press within 50 ms (SDL's two, INPUT,
 *   and the panic watchdog's) are one press. The aborts wait in a log of
 *   16 per process; a screen opened later does not see the earlier ones.
 *   A group reports an abort in every f[i], and no member begins a frame.
 *   The ring gets one YSCR_EV_ABORT record per abort, when begin()
 *   reports it: t_ns the press (or the request), aux desc.display_index,
 *   u.u32[0] the reason, u.u32[1] who saw it (1 the watchdog's hook, 2
 *   injected input, 4 SDL), u.u32[2] the key, u.u32[3] the modifiers,
 *   u.i64[2] when begin() reported it.
 *
 *   ---------------------------------------------------------------------
 *   PANIC (desc.panic): Shift+Esc 3 times when the program hangs
 *   ---------------------------------------------------------------------
 *   An abort works only while the frame loop calls begin(). A program that
 *   hangs in fullscreen leaves a fullscreen window, a switched display
 *   mode and the OS gamma ramp the header set. desc.panic arms a watchdog
 *   for that: a thread with a low-level keyboard hook (WH_KEYBOARD_LL),
 *   which sees the abort combination whatever the frame thread does. The
 *   hook passes every key on, so SDL still sees the same keys, and press 1
 *   is an abort either way.
 *   The rule: desc.panic_presses (0 = 3) presses within
 *   desc.panic_window_ns (0 = 2 s), with a window of this process in
 *   front (or the "Ghost" window Windows puts in front of a hung one after
 *   about 5 s, measured), and no abort reported by a begin() for
 *   desc.panic_grace_ns (0 = 10 s). A responsive loop reports press 1
 *   within a frame, so presses 2 and 3 are more aborts, and a program that
 *   saves after press 1 is not killed. A program outside its frame loop
 *   (loading, saving) for longer than the grace time looks hung.
 *   With the window thread (v0.10.0), the panic depends on who answers.
 *   While the window thread answers (its heartbeat is newer than 1 s),
 *   the panic releases the display and the process lives: the window
 *   thread minimizes every screen's window (SDL leaves fullscreen, which
 *   puts back a switched mode) and shows the cursor; the watchdog puts
 *   back the gamma ramps, writes YSCR_EV_PANIC with u.u32[1] 1 (released)
 *   and adds an abort with YSCR_ABORT_PANIC. The next begin() reports it
 *   with the presses (f.abort 0x21), and every begin() after that returns
 *   YSCR_QUIT with f.abort YSCR_ABORT_PANIC (WARNING=panic-released in
 *   the describe line): the program may only close. No call gets the
 *   session back, and open() refuses a window until every screen has
 *   closed. panic_fn does not run: the program saves after its next
 *   begin(). Why (decided 2026-10-26): the frame thread may only be
 *   waiting (MATLAB's pause, Python's input()), the window, the cursor,
 *   the mode and the gamma are what the operator needs back, and a false
 *   release costs a minimized window and an abort, not a session. Measured
 *   (examples/screen/abort.c): the release took 7.7 ms on the window
 *   thread, the program lived, and its next begin() reported 0x21 with 3
 *   presses. When the window thread does not answer (no heartbeat for 1
 *   s, or the release not begun within 1 s or not done within 5 s), the panic ends the process as
 *   follows; tests/adapt/screen_window_test.c and abort.c's "kill" case
 *   block the window thread to run that path.
 *   The end of the process, on the watchdog thread: it removes the hook,
 *   so no key of the session waits on it; writes a YSCR_EV_PANIC record
 *   to each armed screen's ring (t_ns the panic, u.u32[0] the presses
 *   needed, u.u32[1] 0, u.i64[1] the last report); puts back each gamma ramp the header set; calls
 *   desc.panic_fn(desc.panic_ctx) on a thread of its own, at most 1 s; and
 *   ends the process with TerminateProcess(YSCR_PANIC_EXIT_CODE = 99).
 *   Without the window thread (OFF) every panic ends the process so.
 *   Windows puts back a display mode the screen switched as the process
 *   ends (measured: 1.2 s after press 3, teardown included).
 *   ChangeDisplaySettingsExW from the watchdog was measured too: it waited
 *   over 1 s on the hung window and brought the mode back no sooner, so
 *   the watchdog does not call it.
 *   Not exit(): exit runs atexit handlers and DLL detach, which can wait
 *   on locks the hung thread holds, and only the end of the process takes
 *   down a window and a swapchain that a hung thread owns.
 *   What another thread can put back: the gamma ramps (GDI, by the
 *   display's name) and ring records. What it cannot: the window,
 *   fullscreen and the display mode (a call on a window, or a display
 *   change, waits for the hung thread; the end of the process ends them);
 *   the D3D11 device, ANGLE and
 *   SDL (not safe while the frame thread is inside them); data not yet
 *   written (panic_fn may save some, but runs while the frame thread holds
 *   what it holds, so it must take no lock the frame loop uses); hardware
 *   a trigger left set (panic_fn may reset it).
 *   When it is armed: DXGI_FLIP and COMPOSITION in fullscreen, on
 *   Windows. A window has no ramp or mode to put back and its user has the
 *   desktop: the describe line says panic=idle(windowed); on SIM, CUSTOM
 *   and other platforms, panic=n/a. open() refuses desc.panic with
 *   abort_keys.off and with a key the hook cannot name (it takes Esc, F1
 *   to F12, a letter or a digit). The first armed screen's numbers hold
 *   until the last armed screen closes.
 *   Cost: while it is armed, every key of the session goes through the
 *   watchdog thread, which otherwise sleeps in GetMessage. Windows skips a
 *   hook that takes longer than LowLevelHooksTimeout (at most 1 s); this
 *   hook does a few compares and takes no lock the frame thread holds. A
 *   test hook that stalled for 1.5 s once was still called afterwards, so
 *   the header does not install it again. Keys on SDL's raw path were not
 *   stamped later with the hook armed (STATUS).
 *   Measured (examples/screen/abort.c; a window armed through a test
 *   seam, and fullscreen in a switched mode): see STATUS.
 *   Later, X11: the watchdog opens its own display connection and selects
 *   XI_RawKeyPress and XI_RawKeyRelease on the root window (XInput 2.1),
 *   which arrive whatever has the focus and are not consumed. It checks
 *   _NET_ACTIVE_WINDOW, puts back the CRTC gamma through RandR on its own
 *   connection (and the mode, unless the X server does as the process
 *   ends, to be measured), and ends with _exit(). Wayland has no global key
 *   events: no panic there; the compositor undoes a dead client's
 *   fullscreen.
 *
 *   ---------------------------------------------------------------------
 *   WINDOW ICON (desc.icon_rgba, desc.icon_sdl)
 *   ---------------------------------------------------------------------
 *   open() gives each window the header's own icon, Escher's impossible
 *   cube, drawn for this header (public domain, like the rest), at 16, 32
 *   and 48 pixels: SDL_SetWindowIcon with alternate images, so Windows
 *   takes the size its display scale needs. It is stored as a 5-color
 *   palette and runs of 4-bit indices (1312 bytes in all), decoded at
 *   open. desc.icon_rgba (icon_w x icon_h RGBA, top row first, 1 to 256 a
 *   side) sets yours; desc.icon_sdl keeps SDL's.
 *
 *   ---------------------------------------------------------------------
 *   DISPLAYS AND MODES
 *   ---------------------------------------------------------------------
 *   yscr_displays() and yscr_modes() list what SDL reports, the
 *   refresh as a rational (refresh_num / refresh_den Hz) and as a period.
 *   desc.mode all zero opens in the desktop mode and never switches. A
 *   desc.mode with any field set must match a mode yscr_modes() lists
 *   (a refresh is compared as a rational), or open() fails: there is no
 *   nearest-mode fallback. A matching mode other than the desktop's
 *   switches the display for the life of the screen. yscr_caps.mode is
 *   the mode read back from the OS after the switch. For a video at 23.976
 *   or 59.94 Hz, yscr_mode_multiple() finds the mode whose refresh is a
 *   multiple of it, and its error in ppm.
 *
 *   ---------------------------------------------------------------------
 *   MULTIPLE DISPLAYS
 *   ---------------------------------------------------------------------
 *   One yscr_screen per display (desc.display), each with its own device,
 *   context, swap path, grid and records; desc.display_index tells their
 *   records apart. yscr_begin_group() and yscr_flip_group_at() run a
 *   frame on all of them from one thread: a DXGI wait is a kernel object,
 *   so waiting on each in turn costs the longest wait, not the sum. Two
 *   displays that flip together is a claim about hardware: only genlock
 *   on a workstation GPU makes it, and two photodiodes measure it. The
 *   group calls do not synchronize anything; each record has its own onset.
 *
 *   ---------------------------------------------------------------------
 *   VARIABLE REFRESH
 *   ---------------------------------------------------------------------
 *   A capability, never a default. yscr_caps.vrr_capable reports whether
 *   DXGI allows tearing presents, which a variable-refresh panel needs and
 *   which does not prove the panel has it. desc.vrr makes open() fail:
 *   v0.1 does not run variable refresh, which needs the photodiode
 *   interval sweep and the luminance-versus-interval sweep on that panel
 *   first. The header always presents with a sync interval of 1 and never
 *   with tearing. A driver that varies the refresh on its own shows up as
 *   off-grid times (YSCR_FLIP_GRID_UNSTABLE) and as a warning in the
 *   describe line; once most times are off the grid, the grid follows
 *   them (THE GRID).
 *
 *   ---------------------------------------------------------------------
 *   SYNC GUARD
 *   ---------------------------------------------------------------------
 *   A driver panel can force vsync off for every program: AMD Software
 *   "Wait for Vertical Refresh: Always off", NVIDIA Control Panel
 *   "Vertical sync: Off", Intel Graphics "Vertical Sync: Speed", and on
 *   Linux Mesa's vblank_mode=0. Whether a panel overrides a flip-model
 *   Present(1) is not measured (docs/screen.md, "Driver-forced vsync
 *   off"). If one does, frames tear or are discarded, and the onsets are
 *   not measurements. The guard looks at every flip that has an OS time
 *   (after open's black frames, and not while the window is occluded) for
 *   evidence that is impossible under vsync:
 *     - same refresh: its vblank count is not after the previous flip's,
 *       or its time is less than half a period after it;
 *     - no wait: it flipped in the refresh its present call was made in
 *       (the count is not after the count at the call, and the time is
 *       before the call: DXGI's count lost vblanks while frames were
 *       held, so it can be one behind the grid's count at the call);
 *     - torn: its time is more than a quarter period before the vblank
 *       its count names, on the grid it was planned with.
 *   It fires when 32 of the last 64 such flips have evidence (FLIPS), or
 *   when, over a block of at least 64 refreshes between such flips, more
 *   presents completed (shown, estimated or skipped) than the refreshes
 *   plus 8, the frames that can be in flight (RATE). The rate catches a
 *   driver that discards frames without tearing: their statistics are
 *   missing, so they complete as ESTIMATED and look shown.
 *   Legitimate cases give no evidence: a held frame or a drop shows fewer
 *   frames than refreshes; COMPOSITION's present at a time and the SIM
 *   backend show each frame on a vblank of its own after its present;
 *   DXGI's off-grid times of held frames (GRID_UNSTABLE) gave a torn
 *   flip now and then, never many in a row. One stale count or
 *   jittered time is one flip of 32. tests/adapt/screen_test.c plays a
 *   forced-off driver four ways (each fires) and the legitimate paths
 *   (none fires). On this laptop's DXGI_FLIP, with load, misses and held
 *   frames, at most 3 of 64 flips had evidence; on COMPOSITION, none
 *   (docs/screen.md, "Driver-forced vsync off").
 *   When it fires, once per open: the screen is untimed. Every record
 *   from the flip that fired it on carries YSCR_FLIP_UNSYNCED and tier 3
 *   (so desc.min_tier flags it, and caps.worst_tier is 3). The ring gets
 *   one YSCR_EV_UNSYNCED record; the describe line says
 *   WARNING=not-vsynced(driver-setting?); yscr_sync_check() returns the
 *   rule, the frame, the evidence counts and a message to show the
 *   operator. The run goes on: only the caller knows what to do with its
 *   trials. Limit: a driver that tears but reports the next vblank's
 *   count and time gives no evidence; examples/screen/sync_check.c shows
 *   tearing to the eye.
 *
 *   ---------------------------------------------------------------------
 *   PARAMETERS
 *   ---------------------------------------------------------------------
 *   yscr_params() returns a table of the desc fields a designer sets:
 *   name, type, range, the value a zero field means, unit, one line of
 *   text and, for an enum, its values: "name=value" pairs joined by ',',
 *   in value order. A name is the C constant's suffix in lower case
 *   ("dxgi_flip" is YSCR_BACKEND_DXGI_FLIP, "top_left" YSCR_TOP_LEFT).
 *   backend lists every yscr_backend; open() refuses the stubs (BACKENDS).
 *
 *   ---------------------------------------------------------------------
 *   MEMORY AND THREADS
 *   ---------------------------------------------------------------------
 *   The handle holds everything; nothing is allocated after open(), and
 *   the frame loop makes no allocation of its own (measured: 0 C runtime
 *   heap calls from the header in a debug build). The handle is about 22
 *   KB, most of it the code rows and the depth evidence. One thread calls
 *   begin, flip and the rest for a screen, so every call on a screen that
 *   fails writes yscr_error() before it returns its code. The calls for
 *   any thread take no screen (yscr_push_input(), yscr_request_abort()). open() and close() of different
 *   screens must not run at the same time, because ANGLE's libEGL is
 *   loaded once per process and never unloaded, and the window thread
 *   takes one command at a time. The threads open() starts: once per
 *   process, the window thread (WINDOW THREAD) for the first screen with a
 *   window, which the last such close() stops; the trigger worker
 *   (TRIGGERS) when desc.n_triggers > 0, which close() stops; on
 *   COMPOSITION the idle-wake waiter (BACKENDS), which close() stops; once
 *   per process, the raw mouse reader (INPUT) and the panic watchdog
 *   (PANIC), which the last screen that uses them stops. The abort state
 *   is one per process (ABORT). Raise the frame thread with
 *   yrt_thread_elevate() before open.
 *
 *   ---------------------------------------------------------------------
 *   BUILDING
 *   ---------------------------------------------------------------------
 *   Link SDL3 (3.2 or later); with CMake, find_package(SDL3) and link
 *   SDL3::SDL3. The implementation includes <SDL3/SDL.h>, and on Windows
 *   <d3d11_1.h> and <dxgi1_5.h> from the Windows SDK or MinGW-w64. It
 *   links nothing else: d3d11.dll, dxgi.dll, user32.dll and libEGL.dll are
 *   loaded at run time. It does not include SDL_main.h, so your main() is
 *   yours. Put libEGL.dll and libGLESv2.dll from an ANGLE build beside the
 *   program, or name their directory in desc.angle_dir or YSCR_ANGLE_DIR.
 *   Link libm on Linux, and include ysp/screen.h (or ysp/rt.h) before any
 *   system header in the implementation file: ysp/rt.h sets the
 *   feature-test macro that glibc reads at the first one.
 *   Under Emscripten link SDL3's port and suspend the frame loop:
 *       emcc -O2 -sUSE_SDL=3 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 \
 *            -sASYNCIFY -sASYNCIFY_STACK_SIZE=65536 -DYRT_NO_THREADS ...
 *   (-sJSPI in place of -sASYNCIFY where every target browser has JSPI;
 *   examples/web/build.sh has the demos' flags and docs/web.md the sizes).
 *   Define YSCR_NO_SDL to build without SDL: the core, SIM and CUSTOM
 *   only, with no window, no input and no GL. tests/adapt/screen_test.c
 *   builds this way.
 *   Define YSCR_API to change the linkage of every function.
 *   Hot paths carry ysp/rt.h's instrumentation macros: zones yscr.begin,
 *   yscr.wait, yscr.pump, yscr.stats, yscr.flip, yscr.patch,
 *   yscr.present and yscr.hold; a frame mark when a flip's record
 *   completes; plots of the residual and the depth.
 *
 *   LICENSE: public domain / MIT-0, see end of file.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.10.4 - COMPOSITION closes the 5 events its open got (the
 *          presentation manager's statistics and lost events, the 3
 *          buffers' available events): each open and close left 5 handles.
 *          An id from yscr_displays() names its display after SDL's video
 *          starts again (SDL numbers the displays anew each time, so an id
 *          listed with no screen open was invalid at the next call or
 *          open). x86: the 64-bit atomic stores use compare-exchange
 *          (MSVC x86 has no InterlockedExchange64), and COMPOSITION
 *          refuses x86 as it refuses ARM64. A test hook makes the case of
 *          no free buffer after a present happen (YSCR__COMP_IS_AVAILABLE).
 *   v0.10.3 - GL STATE between frames, COMPOSITION: after a present the
 *          shown buffer stayed framebuffer 0, and a draw into it between
 *          flip() and the next begin() (ygfx_prime()) waited until a newer
 *          present or a covered window released it: more than 90 s in
 *          front. flip() now leaves a buffer that no present holds bound
 *          (one that is free; none free: no surface). See STATUS.
 *   v0.10.2 - FOREGROUND, the next screen: with the window thread, the
 *          second and later screens that one process opened after a
 *          close got no foreground in a window (refused: the window opened
 *          behind, and where another window covered it every record was
 *          OCCLUDED at tier 3), and fullscreen only by a forced raise.
 *          Windows let the thread that had the foreground window raise
 *          the next one, and the window thread ends at the last close.
 *          close() now calls AllowSetForegroundWindow for its own process
 *          while the screen has the foreground. Measured: STATUS.
 *          Found by the ysp-vis real-window tests.
 *   v0.10.1 - THE GRID after a gap: when no OS time came for 0.5 s, the
 *          first begin() anchors the grid on DWM's vblank time, so the
 *          records after a wait between trials are not GRID_UNSTABLE.
 *          Measured (docs/screen.md, "The grid after a gap"): gaps of 5 to
 *          60 s, COMPOSITION and DXGI_FLIP, window thread ON and OFF: 17
 *          GRID_UNSTABLE records with onsets up to 1.98 ms late in 29 of 40
 *          runs before, none in 20 of 20 after, the first OS time after
 *          the gap within 2 us of the grid. The describe line's
 *          gap_anchors=. Fixed (PANIC, with the window thread): a release
 *          in a switched display mode ended the process, because
 *          minimizing put the desktop mode back first (1.2 to 1.4 s,
 *          measured) and the watchdog waited 1 s; it now waits 1 s for the
 *          release to begin and 5 s for it to end, reading the hook's
 *          messages meanwhile.
 *   v0.10.0 - The window thread (WINDOW THREAD; decided 2026-10-26 on
 *          docs/winthread_probe.md): on Windows a thread of the header's own
 *          makes the windows, reads their messages and makes every SDL
 *          video call, so a host that waits between frames no longer
 *          leaves a hung window and a ghost in front of it.
 *          desc.window_thread (AUTO, ON, OFF; AUTO is on for DXGI_FLIP and
 *          COMPOSITION, OFF is v0.9.0's path); yscr_window_call() runs SDL
 *          calls on the window there; the describe line's window_thread=.
 *          begin() no longer pumps and yscr_poll() takes events without a
 *          pump; yscr_displays() and yscr_modes() run on the window thread
 *          while it runs; a command not taken in 5 s is withdrawn, and
 *          close() returns after 5 s more. The panic releases the display
 *          (the windows minimized, the gamma put back, the process alive)
 *          while the window thread answers, and ends the process only when
 *          it does not: the new abort reason YSCR_ABORT_PANIC (0x20), from
 *          every begin() after a release; YSCR_EV_PANIC's u.u32[1] 1 for a
 *          release. Text and drop strings SDL keeps for the frame thread
 *          are freed at the next begin(). Breaking: SDL calls on
 *          yscr_window() from the program's own thread (the examples' title
 *          and cursor calls, Dear ImGui's SDL3 backend) must go through
 *          yscr_window_call(), or set desc.window_thread OFF.
 *   v0.9.0 - PARAMETERS: yscr_param gains a last field, values: for an
 *          enum row the names and values of its C enum or YSCR_ macro set
 *          ("top_left=0,top_right=1,..."), NULL for every other row.
 *          backend, patch.corner and settle list them, and backend lists
 *          every yscr_backend (min and max cannot say which values exist).
 *          The doc text no longer lists them. key_dedup_ns's default
 *          is 5e7 (50 ms, what a zero field means; was 0). ysp/gfx.h's and
 *          ysp/video.h's tables are yscr_param too. A positional
 *          initializer of a yscr_param gets values NULL.
 *   v0.8.0 - BACKENDS, SIM on ysp/rt.h's virtual clock (rt.h v0.9.0): the
 *          presenter's sleep to a vblank jumps the clock there, so a
 *          --sim run drops no frame and gives the same records on every
 *          run. The describe line says clock=virtual. open() refuses
 *          every backend but SIM and CUSTOM on the virtual clock. The
 *          examples' --sim checks that allowed whole-frame slips on a
 *          loaded CI runner are exact again. A record's phases are costs
 *          and read yrt_real_ns(), so they stay real on the virtual clock
 *          (they read 0 for DRAW and the jump for SWAP through
 *          yrt_now_ns()). The real clock: no change.
 *   v0.7.2 - BACKENDS, WEB: open() and close() no longer leave a
 *          fullscreen the page entered (Fullscreen, in BACKENDS, WEB). A
 *          program that yielded between the page's request and open() (the
 *          sound demo: its sound's open awaits callbacks) lost fullscreen
 *          when SDL made the window, 2.2 to 3.7 s into the run in Chrome 154
 *          and Firefox 157, and every demo lost it when the page was shown
 *          again after hidden. Nothing changes on the other backends.
 *   v0.7.1 - BACKENDS, WEB: yscr_event_input() stamps a key, mouse
 *          button, touch or pen press or release with the DOM event's own
 *          time where it finds it (Input, in BACKENDS, WEB); the describe
 *          line adds dpr= and dom_stamps=found/tried. Nothing changes on
 *          the other backends.
 *   v0.7.0 - YSCR_BACKEND_WEB (BACKENDS, WEB; docs/web.md): under
 *          Emscripten, SDL3's canvas and the header's WebGL2 context,
 *          frames paced by requestAnimationFrame through an awaited
 *          callback (ASYNCIFY or JSPI), records from the callback times as
 *          estimates (YSCR_TIER_WEB, YSCR_PATH_BROWSER, ONSET_PLANNED),
 *          the period measured at open, settling that warns and never
 *          fails, YSCR_EV_VISIBILITY ring records. AUTO picks it there.
 *          New enum values only; nothing changes on Windows.
 *   v0.6.0 - The ysp conventions (README.md, Conventions); breaks the API.
 *          Every call on a screen that returns a negative code writes
 *          yscr_error() first: yscr_begin(), yscr_flip_at(), yscr_flip(),
 *          yscr_wait_flip(), the code and trigger calls, yscr_native(),
 *          yscr_text_input(), and yscr_begin_group() and
 *          yscr_flip_group_at() into the screen at fault (the first when
 *          none). A specific message stays; a call that wrote none gets
 *          "ysp_screen: <call>: <code's text>". Before, a timeout or a
 *          lost device in begin() left the message empty. yscr_native()
 *          takes a non-const screen, so it can write the message.
 *          desc.panic_window_ms and panic_grace_ms are int64_t
 *          panic_window_ns and panic_grace_ns; desc.trigger_spin_ns is
 *          int64_t. yscr_params() names the new fields. The time
 *          parameters of flip_at, trigger_at and flip_group_at are t_ns
 *          (a name only: they were int64_t ns). USAGE builds as
 *          C99 and C++17. Nothing else changed.
 *   v0.5.2 - desc.on_context (CONTEXT HOOK; decided 2026-10-10): open()
 *          calls it once when the GL context exists, before the warm-up
 *          and settling, so the caller's work on other threads overlaps
 *          them; ysp/gfx.h v0.11.1's ygfx_open_start() submits its
 *          programs there. Its time is added to the settle cap; the
 *          describe line (settle=...,hook=0.015s) and
 *          yscr_settle_info.hook_ns report it. Measured on the Iris Xe
 *          (docs/screen.md, "Compiles during settling"), cold program
 *          cache, on AC: gfx ready 2.57 s after the open() call in a
 *          window (4.79 to 4.83 s without), 2.46 to 2.47 s fullscreen
 *          (2.87 to 2.91 s); on battery 3.41 s (5.37 to 5.40) and 3.46
 *          to 3.47 s (3.85 to 3.89); a warm cache 24 to 51 ms earlier.
 *          Settling took the same time and presents with the compiles
 *          running; every one of 260 opens settled.
 *   v0.5.1 - The foreground (FOREGROUND; decided 2026-10-10): desc.foreground
 *          AUTO forces the raise fullscreen (SDL_HINT_FORCE_RAISEWINDOW)
 *          and honors the OS policy in a window; HONOR and FORCE. The
 *          describe line (foreground=granted, forced or refused, and
 *          WARNING=not-foreground), YSCR_EV_SETTLE's u.u32[8] and
 *          yscr_settle_info.foreground say how the window got it. A
 *          forced raise gives the foreground back at close to the window
 *          that had it. Measured from a background launch (docs/screen.md,
 *          "The foreground"): the OS refused 20 of 21, open() still
 *          settled, and the keys went to the other program; forced,
 *          25 of 25 got it. Fixed
 *          (IDLE WAKE): on COMPOSITION the first frame after 67 ms or
 *          more without a present showed one vblank late (72 of 72 after
 *          a gap); a thread now waits on the display's vblank event
 *          while the screen is open: 0 of 36. DXGI_FLIP was not
 *          affected (0 of 54). Fixed: open() returned with the back
 *          buffer it had just presented bound, so a draw before the
 *          first begin() (ygfx_prime) waited forever on COMPOSITION;
 *          open() now holds a free slot, as the warm-up did.
 *   v0.5.0 - open() settles the start of the run (SETTLE; decided
 *          2026-10-09): black frames until an OS time, 6 clean flips on
 *          one path and depth, the spread of the last 20 vblank intervals
 *          and, in a window, 2.5 s; capped at 3 s (4 s in a window). At
 *          the cap AUTO fails open() fullscreen and warns in a window;
 *          desc.settle, settle_flips, settle_min_ns, settle_max_ns and the
 *          YSP_SETTLE variable change it. Every outcome is in the describe
 *          line, a YSCR_EV_SETTLE ring record and yscr_settle_check().
 *          Measured before (docs/screen.md, "Settling at open"): every bad
 *          record, path and depth change of 120 opens came in the first
 *          0.66 s. Fixed: COMPOSITION in a window reported each flip a
 *          second time as an overlay frame, and the path and depth flapped
 *          on frames 0 to 5 (9 to 13 depth changes per open); a statistic
 *          for a completed present now changes no path.
 *   v0.4.4 - The depth comes from the path, never from misses (DEPTH;
 *          decided 2026-10-09): measured at open on DXGI_FLIP (never above
 *          the smallest depth 2 of open's flips showed), the path's on
 *          COMPOSITION (independent flip 1, composed and overlay 2), the
 *          new path's at each path change. A miss is a drop and nothing
 *          more. On DXGI_FLIP a flip that was not held and showed early
 *          lowers it to what it showed, since only that proves it too high.
 *          desc.depth pins it; desc.depth_learn keeps v0.4.3's rule
 *          (opt-in: with steady GPU work near a period it dropped 1 to 6
 *          frames in 600 where the default dropped 3 to 57). Each change
 *          writes a YSCR_EV_DEPTH ring record and the describe line counts
 *          them. Measured on v0.4.3's stress runs: no early flip after
 *          heavy frames (3 per run before); every early flip and depth
 *          change left is in a run's first 6 frames.
 *          AUTO picks COMPOSITION where it opens with independent flip,
 *          else DXGI_FLIP (BACKENDS; decided 2026-10-09 on 10-minute runs:
 *          equal prediction p99, drops and tiers, no early flip); the
 *          describe line and YSCR_EV_MODE's u.i32[5] say what AUTO did.
 *          Fixed: when open() got no OS time (a window covered at open;
 *          COMPOSITION after a session unlock), the guessed grid stayed:
 *          every record tier 3 and the sync guard fired on on-time flips.
 *          The first OS time now replaces the guess.
 *   v0.4.3 - Fixed: held frames shown a vblank early on DXGI_FLIP (THE
 *          GRID; docs/screen.md, "Held frames a vblank early"). While
 *          frames were held, DXGI's refresh count lost about 1 vblank in
 *          25 and 15% of its vblank times were up to half a period off
 *          the vblanks, which D3DKMTGetScanLine showed steady. The grid
 *          moved to those times, so the next hold woke before the real
 *          vblank: 26 to 72 of 1200 held frames were shown a vblank
 *          early in fullscreen, idle, in every version since v0.2.0, and
 *          the lost counts flagged about 145 more EARLY that were on time.
 *          Now a flip's vblank is the grid's nearest to the OS's time;
 *          only times on the grid move it, or 16 in a row that agree on a
 *          new phase; when 48 of 64 times are off it, it follows them as
 *          before. A record whose OS time is off the grid has the grid's
 *          vblank time as onset (GRID_UNSTABLE, tier 2). Measured: 0 early
 *          in 1200 held frames fullscreen; with load, misses and holds,
 *          3 of 3600 (from the depth rule, not the grid).
 *   v0.4.2 - The sync guard (SYNC GUARD): a driver setting that forces
 *          vsync off is detected from the flips. When 32 of the last 64
 *          flips with an OS time came two in one refresh, flipped with no
 *          vblank after their present, or tore, or a block of 64 or more
 *          refreshes saw more presents complete than its refreshes plus
 *          8, the screen is untimed: from that flip
 *          on every record is tier 3 with the new flag YSCR_FLIP_UNSYNCED,
 *          a YSCR_EV_UNSYNCED ring record marks the flip, the describe
 *          line warns, and yscr_sync_check() gives the evidence and a
 *          message that names the AMD, NVIDIA, Intel and Mesa settings.
 *          Fixed (INPUT, "Second key reports"): a device-0 key-up of a
 *          dropped report could take another keyboard's held key, so
 *          key_double_ups missed it and that keyboard's own key-up passed
 *          too; a device-0 key-up now goes first to a press kept with a
 *          keyboard's id that has a dropped report pending, and a key-up
 *          pairs with its own device's press before one with device 0.
 *   v0.4.1 - yscr_event_input() drops SDL 3.4's second key reports (INPUT,
 *          "Second key reports"): a key-down of the same key within 50 ms
 *          of the last one kept, from the same keyboard or with either
 *          device 0, and that report's key-up. It was ysp/response.h's
 *          rule R3, which only its collector got. Counted in
 *          yscr_input_stats (key_doubles, key_double_ups) and logged as a
 *          YSCR_EV_KEY_DOUBLE ring record each; desc.key_dedup_ns sets
 *          the window or turns the filter off; yscr_key_filter() applies
 *          it to events from elsewhere.
 *   v0.4.0 - The input bridge (INPUT, "The input bridge"): any thread hands
 *          ysp/input.h events to yscr_push_input(); each one is stored
 *          and announced by an SDL event (a doorbell) in SDL's queue, so
 *          yscr_poll() returns keyboard, mouse, raw mice, response boxes
 *          and eye trackers in one stream, and yscr_event_input() turns
 *          any input event of that stream (a doorbell or SDL's own) into a
 *          yin_event. Raw mice go over the bridge; yscr_poll_mouse()
 *          is deprecated and reads them back for one more version.
 *          Breaking: ysp/screen.h requires ysp/input.h beside it (types and
 *          inline adapters only); yscr_mouse_event is ysp/input.h's
 *          yin_mouse_report. Defaults unchanged: no producer, no doorbell.
 *   v0.3.5 - Raw mice (INPUT, "Raw mice"): desc.raw_mice reads every
 *          mouse's Raw Input on a thread of its own, per device and
 *          stamped when read, and yscr_poll_mouse() hands the reports
 *          out (buttons, wheel, unaccelerated deltas). A guard registers
 *          again after SDL's relative mode took the mice, and logs it.
 *          Off by default: mice come through SDL as before.
 *   v0.3.4 - The input devices in the ring (INPUT, "Devices"): a
 *          YSCR_EV_DEVICE record for each keyboard, mouse, touch device
 *          and (with desc.gamepads) gamepad SDL lists at open, and for each
 *          one added or removed later (seen in yscr_poll()), and for a
 *          pen or touch device the first time it is used: kind, id and
 *          name, so a data file's device column maps to a device.
 *   v0.3.3 - Input for ysp/response.h (INPUT): desc.gamepads starts SDL's
 *          gamepad subsystem and opens each gamepad, so its events come
 *          through yscr_poll() (off by default, as before);
 *          caps.raw_keyboard says whether keys come on SDL's raw path. The
 *          Text input is observed, not only commanded: flip_at() reads
 *          SDL_TextInputActive(), so text input that another library
 *          started (Dear ImGui's SDL3 backend) gets the ring record (with
 *          u.u32[5] 1, external) and the flip flag too. The
 *          INPUT text on SDL 3.4's double key report is narrowed to what
 *          was measured: it comes from keys whose window message has no
 *          scan code (virtual-key injection), not from every key. Reports
 *          of one abort press within 50 ms (was 30) are one press: the
 *          second report came up to 34.2 ms later, and a tool that injects
 *          by virtual key could otherwise turn two presses into a panic.
 *   v0.3.2 - yscr_text_input() (INPUT, "Text input"): starts or stops SDL
 *          text input on a screen's window and sets the IME area, for a
 *          text box. Each change writes a YSCR_EV_TEXT_INPUT ring record,
 *          and every flip planned while it is on carries
 *          YSCR_FLIP_TEXT_INPUT, so analysis sees when keys were off the
 *          raw timed path. Off at open, as before.
 *   v0.3.1 - Aborts (ABORT): Shift+Esc, a close request, Alt+F4, SDL's
 *          quit event and yscr_request_abort() each make one begin()
 *          return YSCR_QUIT with f.abort, f.abort_presses and f.abort_ns,
 *          and a YSCR_EV_ABORT ring record; the header sees them through
 *          an SDL event watch, also when the caller reads the events first.
 *          The panic watchdog (PANIC, desc.panic, off by default): Shift+Esc
 *          3 times in 2 s while the frame loop reports no abort puts back
 *          the gamma ramps and ends the process (exit code 99); Windows
 *          puts back a switched mode as it ends. The window icon
 *          (WINDOW ICON): Escher's impossible cube at 16, 32 and 48
 *          pixels; desc.icon_rgba, desc.icon_sdl.
 *          desc.d3d11_video (NATIVE HANDLES): a D3D11 device with video
 *          support and multithread protection; yscr_native_info gains
 *          video and mt_protected. yscr_begin_group() reports an abort on
 *          every member and begins none. close(), exit and the watchdog
 *          claim a gamma entry atomically, so one of them puts it back.
 *          Breaking: Esc alone no longer
 *          ends the loop; desc.no_esc_quit is gone (desc.abort_keys);
 *          yscr_frame gains abort, abort_presses and abort_ns;
 *          yscr_presenter_open gains d3d11_video (the presenter version
 *          stays 2).
 *   v0.3.0 - Flip hooks for triggers. Codes (CODES): exact device values in
 *          the frame, per flip or held, as rectangles or rows, drawn last,
 *          with presets for VPixx Pixel Mode and pixel sync, a self test at
 *          open, an optional read-back, record.code_risk and a CODE ring
 *          record. Triggers (TRIGGERS): callbacks on a deadline worker at
 *          a flip's planned vblank, armed when the present returns, moved
 *          when the header learns the flip will be late, a TRIGGER ring
 *          record, and an optional GPU fence check; the worker runs on the
 *          E-cores of a hybrid CPU on Windows and arms itself for the next
 *          vblank after a flip trigger. After flip (AFTER
 *          FLIP): yscr_on_flip(); yscr_frame.done lists every
 *          record completed since the last begin(). yscr_native()
 *          (NATIVE HANDLES). Fixed: after a flip shown early when the
 *          depth fell, begin() planned the next frame for the same vblank
 *          (DEPTH). Before present (GL STATE):
 *          yscr_on_present(), yscr_gl_epoch(), yscr_gl_generation().
 *          OS GAMMA: a fullscreen screen on Windows takes the OS gamma ramp
 *          to identity and gives it back at close and at exit. Breaking:
 *          yscr_record gains code_risk; yscr_frame gains done, n_done
 *          and done_lost; yscr_present_req, the presenter
 *          and its open struct gain fields (YSCR_PRESENTER_VERSION 2).
 *   v0.2.0 - The Windows 11 composition swapchain (YSCR_BACKEND_COMPOSITION)
 *          with present at a target time. Tiers: each flip record carries
 *          yscr_tier, caps.worst_tier and the describe line report the
 *          worst since open, and desc.min_tier flags flips below it. New
 *          flags SKIPPED, CANCELED, ONSET_PLANNED and BELOW_TIER. Breaking:
 *          yscr_record.path and yscr_vblank.path are uint8_t and a tier
 *          byte follows each; the ring's mode word is path in bits 0..2,
 *          flags in bits 3..12 and the tier in bits 13..15 (read it with
 *          YSCR_EV_PATH_OF, _FLAGS_OF and _TIER_OF). On Windows the raw
 *          keyboard path is on (SDL_HINT_WINDOWS_RAW_KEYBOARD, at default
 *          priority) and SDL text input is stopped. AUTO stays DXGI_FLIP.
 *          The depth on a backend that holds frames to their target is
 *          raised by misses and lowered only on slack evidence, per path,
 *          never by a try (DEPTH); begin() never predicts a vblank a flip
 *          in flight already has. Fixed: on DXGI_FLIP a flip_at() call
 *          within the margin before a vblank presented at once, and the
 *          frame showed a vblank before the one planned (EARLY set); it now
 *          waits for that vblank.
 *   v0.1.0 - first release: the DXGI flip swap path (Windows 10 and 11)
 *          through ANGLE, the simulated display, the presenter interface,
 *          prediction, the snap, the per-flip record, phases, the patch,
 *          groups, input restamping, mode lists and the parameter table.
 *          The other swap paths are stubs that refuse to open.
 *
 *   STATUS: v0.10.4 (docs/screen.md, "Handles, no free buffer, display
 *   ids"): on the laptop below, 10 opens and closes a backend gained 40
 *   handles (Event 40) on COMPOSITION before, ON and OFF, and 0 after on
 *   COMPOSITION and DXGI_FLIP; the no-free-buffer case (a hook) in front:
 *   no surface, a draw and a read return at once and do nothing, the next
 *   begin() runs (16 of 16 checks); an id listed with no screen opens its
 *   display after SDL's video restarted (one display). MSVC x86 compile
 *   checks and the core test pass; no x86 window was opened.
 *   v0.10.3 (GL between frames; docs/screen.md, "Between
 *   frames"): on the laptop below, AC, COMPOSITION in front, a prime or a
 *   clear and read of framebuffer 0 after flip() waited more than 90 s
 *   before (4 of 4 C runs, 3 of 3 Python) and took 0.7 to 102 ms after
 *   (7 of 7 primes, 24 of 24 window-test checks; Python 9 of 9). The park
 *   costs 13 to 16 us at p50, 97 us at p99, after the present call; no
 *   drop and every record tier 1 in 1800-frame runs before and after.
 *   tests/adapt/screen_window_test.c --only between; 1 new mutant, killed.
 *   v0.10.2 (the next screen's raise; docs/screen.md, "The next
 *   screen's raise"): on the laptop below, AC, screens one after another
 *   in one process with the window thread ON, every screen after the first
 *   was refused in a window before (7 of 7 in C, every record OCCLUDED; 4
 *   of 4 in Python) and granted after (8 of 8 in C, 6 of 6 in Python, no
 *   OCCLUDED or PATH_UNKNOWN record); OFF was granted before and after.
 *   tests/adapt/screen_window_test.c --only sequence: 17 checks (windowed,
 *   fullscreen, two at once); 1 new mutant, killed (11 checks fail).
 *   v0.10.1 (the grid after a gap; docs/screen.md, "The grid
 *   after a gap"): 60 runs with a gap of 5 to 60 s on the laptop below,
 *   AC, both backends, window thread ON and OFF; the core test plays a
 *   0.5 ms move during a 30 s gap with and without the system's vblank,
 *   a vblank too far off, and a short gap; 4 new mutants, all killed.
 *   examples/screen/abort.c --mode-test released a switched mode with the
 *   window thread (1.4 to 1.5 s, the process alive).
 *   v0.10.0 (the window thread; docs/screen.md, "The window
 *   thread"): measured on the laptop below, AC, no photodiode; the numbers
 *   and their conditions are in docs/screen.md. tests/adapt/screen_test.c
 *   checks the mode rule and a released screen without SDL;
 *   tests/adapt/screen_window_test.c runs the thread on real windows (a
 *   host that sleeps, window calls from 3 threads, the display lists, text
 *   input and its memory, the release, a blocked window thread ended by
 *   the panic, two screens, open and close cycles, a fullscreen change
 *   while open, gamepads (none attached), raw mice, triggers, and a mode
 *   switch); examples/screen/abort.c sends the keys for the release and
 *   the end; 19 new mutants, all killed.
 *   v0.7.2 (WEB fullscreen; docs/web.md): every demo of
 *   examples/web ran with fullscreen asked in headed Chrome 154 and
 *   Firefox 157, isolated and not; each page stayed fullscreen until it
 *   left fullscreen itself at the end (before: av.html lost it when SDL
 *   made the window in all four runs). A page shown again after a scripted
 *   hidden state kept fullscreen (before: it left). JavaScript only: no
 *   native test or mutant.
 *   v0.7.1 (WEB input; docs/web.md): scripted keys (desktop) and
 *   taps (Pixel 7 emulation) in Chrome 154 got the DOM's time in
 *   yscr_event_input() (dom_stamps in the describe line, the self-test's
 *   table); a run of av_sync_mt opened this backend with -pthread. No
 *   physical key or touch measured; the matching is JavaScript, with no
 *   native test or mutant.
 *   v0.7.0 (WEB; docs/web.md): built with emsdk 6.0.12 and SDL3's
 *   port 3.4.2, ASYNCIFY, YRT_NO_THREADS; the demos of examples/web ran in
 *   Chrome 154 (headless and headed), Edge 155 (headless) and Firefox 154
 *   to 157 (headless) on the laptop below. Chrome headed on its 60 Hz
 *   panel, isolated, 5 runs of 600 frames: callback intervals SD 47 to 99
 *   us, p1 16.51 to 16.55 ms, p99 16.78 to 16.84 ms, no frame dropped, the
 *   program's time per frame 35 us p50. The browser's clock step: 100 us
 *   (Chrome) and 1 ms (Firefox) not isolated, 5 us and 20 us isolated, also
 *   when a service worker isolated the page. JSPI against ASYNCIFY: 27 %
 *   less wasm and 15 to 20 us less per frame. No light was measured: the
 *   compositor's delay after a web onset is unknown. tests/adapt/
 *   screen_test.c checks the presenter's two pure rules (the onset after a
 *   late task, settling); the presenter itself runs only in a page.
 *   v0.6.0: tests/adapt/screen_test.c checks a message after each
 *   failing call on a screen and the ns panic fields; the rest below is
 *   v0.4.4's and the later versions' as stated. v0.4.4 (the depth from the path, AUTO = COMPOSITION;
 *   docs/screen.md, "Depth from the path" and "AUTO, decided"). Two swap paths run, DXGI_FLIP and COMPOSITION, on one
 *   machine: a Windows 11 25H2 laptop whose Intel Iris Xe drives a 1920 x
 *   1200 panel at 60.0008 Hz (60 Hz is its only rate), with the ANGLE that
 *   ships in Docker Desktop's Electron front end (2.1.23876, git
 *   fffbc739779a; not a pinned build) and SDL3 3.4.0. Built with MSVC 19.4
 *   under /W4 /WX as C11 and C++17 and with MinGW-w64 gcc 16.1 under
 *   -Werror, and run from the MSVC build. No photodiode has been attached:
 *   tests/loopback/screen_loopback.c is built and has never run, so the
 *   delay from a reported vblank to light is UNMEASURED, and
 *   desc.onset_offset_ns stays 0 until it is.
 *   DXGI_FLIP (v0.1.0, unchanged; docs/screen.md has the tables):
 *   fullscreen on the overlay path, the onset was within 2.3 us of the
 *   prediction at p99 over 7200 frames, no frame dropped or late; in a
 *   plain window (composed) for 10 minutes, 5.7 us p99, no drop. In v0.2
 *   runs a window kept on top moved between the overlay and the composed
 *   path every 5 to 11 s; per minute that gave 27 to 67 drops, 9 to 15
 *   flips a vblank early (each flagged EARLY) and 13 to 96 flips with no
 *   statistic, and twice the waitable object freed no slot for 333 ms
 *   (begin() returned YSCR_ERR_TIMEOUT). The cause was not found.
 *   COMPOSITION (new; Windows 11, x64): in a window kept on top every flip
 *   but 2 to 9 per run was an independent flip, tier 1 (the rest were
 *   overlay frames, tier 3). For 10 minutes (36000 frames): 2.3 us
 *   p99 and 8.1 us max from the prediction, 12 drops, no flip without a
 *   statistic. Six 1-minute runs that alternated with DXGI_FLIP: 0 to 24
 *   drops for COMPOSITION against 27 to 67, 0 to 15 flips without a
 *   statistic against 13 to 96, no early flip against 9 to 15, 2.1 to 2.5
 *   us p99 against 4.5 to 5.4 us (the two DXGI_FLIP runs without a
 *   timeout). These window runs, and the 10-minute run, came before the
 *   depth rule that lowers only on evidence and before the fix for a flip
 *   called inside the margin before a vblank, which showed one vblank
 *   early on DXGI_FLIP. That fault may explain some of DXGI_FLIP's early
 *   flips above. None of them was run again.
 *   Fullscreen on AC, 1 minute each on the quiet machine: v0.2
 *   DXGI_FLIP 2.33 us p99, COMPOSITION 2.36 us, a v0.1 control 2.32 us, no
 *   drop. (While another program ran load threads on the machine, both
 *   backends gave 75 to 110 us p99 and 21 to 29 drops in fullscreen.) From
 *   the present call's return to the reported onset (scanout start, not
 *   light): 15.96 ms p50 on DXGI_FLIP overlay, 15.63 ms on COMPOSITION
 *   independent flip, every frame on the next vblank; a composed DXGI_FLIP
 *   frame took a vblank more (32.6 ms). Holds 0 to 4 vblanks ahead: 1800
 *   of 1800 fullscreen and 2979 of 2998 in a window on the vblank asked for,
 *   19 late, 0 early. Overruns of 17 and 20 ms: 84 of 84 flagged
 *   LATE_TARGET with the draw phase charged, no miss without a flag. The
 *   header's CPU time per frame in the 10-minute window run: begin() 21.8
 *   us mean (SDL_PumpEvents() 11.0 us, statistics 7.9 us), flip_at() 1.8
 *   us; that misses the 20 us bar, on the OS calls. The present call costs
 *   more than on DXGI_FLIP: 530 to 1230 us mean against 380 to 730 us
 *   in the alternating runs. No C runtime heap call per frame in a debug
 *   build (2 once per run, made by the runtime for an OS component's C++
 *   exception on the frame thread). With one frame in flight, the next
 *   frame waits for the last one's statistic, and the loop used about 99%
 *   of vblanks
 *   in a window (16.85 ms mean frame time).
 *   AUTO (v0.4.4) picks COMPOSITION where it opens with independent
 *   flip. 10 minutes each, idle, AC: fullscreen 1.92 us p99 against
 *   DXGI_FLIP's 1.88, in a window 1.82 against 1.78; 0 and 1 drops on
 *   both; no early flip against 4 (DXGI_FLIP's window, at the start).
 *   Against it: its present call costs about twice as much (463 against
 *   222 us mean), it needs Windows 11 x64, its composed onset is tier 3
 *   where DXGI_FLIP's is tier 2, and a covered window gets no statistic.
 *   What the onset is (measured against the scanout's own vblank entry,
 *   polled with D3DKMTGetScanLine): an independent flip's DisplayedTime
 *   was 13 us after it at p50 and 77 us at p99, came 0.3 ms later, and
 *   moved with a late frame: an observation, tier 1. A composed frame's
 *   time from DComp's target statistics also sat on the vblank (26 us p50)
 *   and moved with a late frame, but came about 15 ms BEFORE that vblank:
 *   it is DWM's plan, so it carries YSCR_FLIP_ONSET_PLANNED and tier 3.
 *   A second window loading the GPU did not make DWM miss a frame (its own
 *   counters stayed 0), so whether a DWM miss shows in the record is
 *   UNMEASURED: on COMPOSITION, a composed onset is DWM's claim. Present
 *   targets and displayed times are QPC time in 100 ns units here (QPC
 *   runs at 10 MHz); interrupt time is 20.03 ms away and was wrong. On a
 *   machine whose QPC runs at another rate this is unverified.
 *   Tiers, from these runs: DXGI_FLIP overlay and COMPOSITION independent
 *   flip are tier 1; DXGI_FLIP composed is tier 2 (DWM's vblank, measured
 *   against nothing else); a flip whose OS time is off the grid (up to
 *   half a period; the scanout's vblanks stayed on it) is tier 2, with the
 *   grid's vblank time as onset; COMPOSITION composed or
 *   overlay frames and every ESTIMATED flip are tier 3.
 *   Input: SDL's tick clock correlates to the ysp_rt clock within 0.1 us.
 *   Keys sent with SendInput(): on the raw path, stamped 0.18 to 0.67 ms
 *   after the call (mean 0.39 ms, 199 keys); through the message loop,
 *   from 6.2 ms before to 12.1 ms after it.
 *   The core (prediction, snap, drops, depth, estimated, skipped and
 *   canceled records, tiers, the ring record, phases, groups, errors, the
 *   mode picker, preemption, codes, triggers and their moves, the
 *   after-flip and present callbacks, aborts, the panic rule, the icon
 *   data) is checked without SDL or a display by
 *   tests/adapt/screen_test.c against a scripted swap path on a
 *   virtual clock, so the host's load cannot change a result, on MSVC,
 *   MinGW gcc 16.1, gcc 11.4 (WSL2; also as C99 at -O3, as C++17, and
 *   under ASan and UBSan, and 8 of 8 runs with a busy loop on its CPU) and
 *   clang (emcc, run in node); 43 deliberate mutations of the header
 *   each make it fail (v0.3.1's 12 and v0.3.2's 2 checked with MinGW
 *   only), and the 9 of v0.4.3 (eg- in tests/mutate/screen.toml). v0.3.2's text input: its records and flag on the scripted path
 *   (MSVC, MinGW); its SDL calls by hand in examples/layout/gfx_layout.c.
 *   CODES and TRIGGERS (v0.3.0, measured on battery, Balanced plan; the
 *   tables are in docs/screen.md): the open-time self test passed on
 *   DXGI_FLIP and COMPOSITION, and 1800 code read-backs in fullscreen
 *   differed 0 times. Drawing a 1-pixel and an 8-pixel code cost 3.8 us
 *   mean on the GPU context with one ClearView per pixel, 5.3 us with
 *   UpdateSubresource, so rows of up to 8 pixels use ClearView. This
 *   laptop runs auto color management (color=wcg in the describe line),
 *   so every code here is at risk (CODE_RISK_ADVANCED_COLOR); no device
 *   has read one. A trigger fired p50 1.0 to 2.1 us, p99 59 to 300 us and
 *   at most 0.6 to 7.9 ms after its deadline (TIME_CRITICAL worker; the
 *   larger numbers in a window), on the P-cores where ysp/rt.h puts the
 *   worker. The cause, measured: the display's vblank DPC, 50 to 400 us
 *   from about 75 us before the vblank, ran on the P-core the worker spun
 *   on; the trigger lock and the spin window were not the cause. The
 *   worker now runs on the E-cores of a hybrid CPU: this machine's way
 *   to keep it off the vblank DPC's CPU, not a rule (TRIGGERS). On AC, 1
 *   minute each: fullscreen p99 1.6 to 2.6 us on both backends (31 and 29
 *   us before); in a window 1.0 to 3.2 us (32 to 169 us before). The
 *   tail is the OS's and stays: 1 to 15 triggers a minute fired over 200
 *   us late (1 to 22 before), the largest 0.35 to 43 ms (0.37 to 12.6 ms
 *   before), so the 200 us bar for the largest is missed. Without a
 *   display, examples/rt/jitter.c on the same cores woke up to 2.6 and
 *   32.6 ms late with the same 1.2 ms spin window (on the P-cores, 3.0
 *   ms and 20 us). With 3 frames of 25 ms GPU work every
 *   300 frames: without the GPU check 72 of 72 triggers fired a frame
 *   early; with desc.trigger_fence 140 of 142 moved in time and 0 moved
 *   in error, and no move in 10800 idle frames. The fence stays off by
 *   default until it runs on more than one GPU. Arming a trigger cost the
 *   frame thread about 8 us on battery and 3.8 us on AC in flip_at(), most
 *   of it the worker's wake-up; the worker now arms itself for the next
 *   vblank after each flip trigger, so a trigger every frame arms with no
 *   wake-up: 0.6 to 0.9 us mean on AC, flip_at() 1.7 to 2.6 us in all.
 *   The OS gamma ramp: this laptop's is the identity, so
 *   open() set nothing; setting the same ramp again and reading it back
 *   was exact; one GetDeviceGammaRamp costs 0.8 ms, so it is read at open
 *   only. Whether the ramp acts on independent-flip and overlay frames,
 *   and under Night Light, was not measured.
 *   ABORT, PANIC, ICON and d3d11_video (v0.3.1, AC, keys sent with
 *   SendInput; docs/screen.md has the tables): Shift+Esc gave one
 *   abort per press, stamped 0.12 to 0.70 ms after the call, also when the
 *   loop read its events before begin(); Esc alone gave none; a held
 *   Shift+Esc gave one. SDL 3.4 reports a key injected by virtual-key code
 *   twice on the raw path (INPUT); the header counts one. A hung program ended 28 to 85 ms after
 *   the third press, after its panic_fn ran and a gamma entry was put
 *   back; after a 7 s hang, with Windows' ghost window in front, too; a
 *   held Shift+Esc did not panic; a responsive loop got 5 aborts for 5
 *   presses and no panic. Fullscreen at 1680 x 1050 on the 1920 x 1200
 *   desktop, hung: the desktop mode was back 1.2 s after press 3, as the
 *   process ended. With the watchdog's hook armed, keys on the raw path
 *   were stamped no later (p50 0.30 to
 *   0.31 ms against 0.35 to 0.46 ms, 3 interleaved runs of 199 keys). The
 *   header's CPU time per frame, begin() without its wait, 3 interleaved
 *   rounds of 1800 frames in a window on top: 22.5 to 25.2 us mean with
 *   d3d11_video off, 19.6 to 26.3 us with it on, 20.0 to 29.5 us for the
 *   v0.3.0 header; flip_at() 1.5 to 2.2 us in all. No difference shows
 *   beyond the spread between rounds. Alt+F4 was not sent (if the focus
 *   moved, it would close another program); the core test checks it
 *   through the feed. The icons reach the window at 16 and 32 pixels as
 *   drawn (read back, 96 dpi).
 *   tests/compile/screen_com.cpp checks every COM slot and struct the
 *   header declares against the Windows SDK (MSVC; MinGW for DComp only).
 *   NOT done: any rate but 60 Hz, any other GPU, Windows 10 (COMPOSITION
 *   needs 11), ARM64 and x86 (COMPOSITION refuses them: the hidden-pointer returns
 *   are checked only on x64), two physical displays, variable refresh
 *   (refused), macOS, X11 and Wayland swap paths (stubs). Linux runs the
 *   core and the simulated display only.
 *   Outside this STATUS block and docs/screen.md, a number in this
 *   header is a measurement only where the text says "measured".
 */
#ifndef YSP_SCREEN_H_INCLUDED
#define YSP_SCREEN_H_INCLUDED

#define YSCR_VERSION_MAJOR 0
#define YSCR_VERSION_MINOR 10
#define YSCR_VERSION_PATCH 4
#define YSCR_VERSION_STRING "0.10.4"

#include "ysp/rt.h"
#include "ysp/input.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YSCR_API
#define YSCR_API extern
#endif

/* SDL types appear only behind pointers, so a translation unit that calls
 * the API needs no SDL include path. */
struct SDL_Window;
union SDL_Event;

/* --- codes -------------------------------------------------------------- */

#define YSCR_OK                    0
#define YSCR_QUIT                  1  /* yscr_begin: an abort; f.abort says why (ABORT) */
#define YSCR_ERR_ARG             (-1)
#define YSCR_ERR_CLOSED          (-2)  /* the screen is not open           */
#define YSCR_ERR_TIMEOUT         (-3)  /* the swap path freed no slot      */
#define YSCR_ERR_LOST            (-4)  /* the device or window was lost    */
#define YSCR_ERR_ORDER           (-5)  /* begin and flip out of turn       */
#define YSCR_ERR_NOT_IMPLEMENTED (-6)
#define YSCR_ERR_REFUSED         (-7)  /* a request done only on purpose   */

#define YSCR_LEAD_NONE (-1.0)   /* desc.lead: never before the target     */

/* Phases of a frame, for yscr_mark() and yscr_record.phase_ns[]. */
#define YSCR_PHASE_EVALUATE 0   /* timeline evaluate                      */
#define YSCR_PHASE_SCRIPT   1   /* script callback                        */
#define YSCR_PHASE_DRAW     2   /* draw submission; unmarked time too     */
#define YSCR_PHASE_UPLOAD   3   /* texture upload                         */
#define YSCR_PHASE_SWAP     4   /* begin's wait for a slot + present call */
#define YSCR_PHASE_GPU      5   /* GPU time: always UNKNOWN in v0.1       */
#define YSCR_N_PHASES       6
#define YSCR_PHASE_UNKNOWN  0xFFFFFFFFu

/* Flip record flags. */
#define YSCR_FLIP_PENDING       0x001u /* not shown yet                    */
#define YSCR_FLIP_ESTIMATED     0x002u /* no OS statistic for this present;
                                          * onset = the planned vblank      */
#define YSCR_FLIP_LATE_TARGET   0x004u /* t was behind the next reachable
                                          * vblank when flip_at ran         */
#define YSCR_FLIP_OCCLUDED      0x008u /* the OS said the window is hidden */
#define YSCR_FLIP_GRID_UNSTABLE 0x010u /* this vblank was off the grid     */
#define YSCR_FLIP_EARLY         0x020u /* shown before the planned vblank  */
#define YSCR_FLIP_SKIPPED       0x040u /* never shown: a later present took
                                          * its vblank; onset 0             */
#define YSCR_FLIP_CANCELED      0x080u /* never shown: canceled; onset 0  */
#define YSCR_FLIP_ONSET_PLANNED 0x100u /* onset is the vblank the system
                                          * planned, not one it observed    */
#define YSCR_FLIP_BELOW_TIER    0x200u /* tier worse than desc.min_tier   */
#define YSCR_FLIP_CODE_AT_RISK  0x400u /* record.code_risk is not 0; not in
                                          * the ring's mode word (CODES)    */
#define YSCR_FLIP_TEXT_INPUT    0x800u /* planned while text input was on:
                                          * keys were off the raw path; not
                                          * in the ring's mode word (INPUT) */
#define YSCR_FLIP_UNSYNCED     0x1000u /* the sync guard fired: flips are not
                                          * synced to the vblank, the screen
                                          * is untimed (tier 3); not in the
                                          * ring's mode word (SYNC GUARD)   */

/* How far a flip's onset can be trusted, from its backend, path and the
 * source of its time (TIERS in the manual). Lower is better. */
typedef enum yscr_tier {
    YSCR_TIER_UNKNOWN = 0,
    YSCR_TIER_1       = 1,  /* an OS vblank time for a flip it observed   */
    YSCR_TIER_2       = 2,  /* good under stated conditions only          */
    YSCR_TIER_3       = 3,  /* runs for development; onset not a measurement */
    YSCR_TIER_SIM     = 4,  /* the simulated display                      */
    YSCR_TIER_WEB     = 5   /* a browser's frame callback: an estimate with
                             * the compositor's delay unknown; the lowest  */
} yscr_tier;

/* The ring record's mode word, u.u16[5]. */
#define YSCR_EV_PATH_OF(w)  ((unsigned)(w) & 0x7u)
#define YSCR_EV_FLAGS_OF(w) (((unsigned)(w) >> 3) & 0x3FFu)
#define YSCR_EV_TIER_OF(w)  ((unsigned)(w) >> 13)

/* Ring record kinds under YRT_SRC_SCREEN. */
#define YSCR_EV_FLIP      1u
#define YSCR_EV_PATH      2u
#define YSCR_EV_MODE      3u
#define YSCR_EV_REPRESENT 4u   /* reserved for variable refresh           */
#define YSCR_EV_TRIGGER   5u   /* one at-onset trigger (TRIGGERS)         */
#define YSCR_EV_CODE      6u   /* the codes in one flip (CODES)           */
#define YSCR_EV_ABORT     7u   /* one abort, as begin() reports it (ABORT) */
#define YSCR_EV_PANIC     8u   /* the watchdog ends the process, or releases
                                * the display (u.u32[1] 1; PANIC)           */
#define YSCR_EV_TEXT_INPUT 9u  /* text input on or off (INPUT)            */
#define YSCR_EV_DEVICE    10u  /* an input device present, added, removed (INPUT) */

/* YSCR_EV_DEVICE: u.u32[0] the kind, u.u32[1] the change. */
#define YSCR_DEV_KEYBOARD 1u
#define YSCR_DEV_MOUSE    2u
#define YSCR_DEV_GAMEPAD  3u
#define YSCR_DEV_TOUCH    4u
#define YSCR_DEV_PEN      5u
#define YSCR_DEV_PRESENT  0u   /* there at open                            */
#define YSCR_DEV_ADDED    1u
#define YSCR_DEV_REMOVED  2u
#define YSCR_DEV_NAME_OF(e) ((const char*)(e)->u.bytes + 16)   /* NUL-terminated, at most 23 */

/* YSCR_EV_RAW_MICE: the raw mouse reader's state changed (INPUT, "Raw
 * mice"). u.u32[0] one of these, u.u32[1] the events lost so far. */
#define YSCR_EV_RAW_MICE        11u
#define YSCR_RAW_MICE_ON         1u   /* registered at open                 */
#define YSCR_RAW_MICE_REGISTERED 2u   /* registered again after a loss     */
#define YSCR_RAW_MICE_SUSPENDED  3u   /* SDL relative mode holds the mice  */

/* desc.raw_mice's report is ysp/input.h's yin_mouse_report (v0.4.0). */
#define YSCR_MOUSE_LEFT            YIN_MOUSE_LEFT
#define YSCR_MOUSE_RIGHT           YIN_MOUSE_RIGHT
#define YSCR_MOUSE_MIDDLE          YIN_MOUSE_MIDDLE
#define YSCR_MOUSE_X1              YIN_MOUSE_X1
#define YSCR_MOUSE_X2              YIN_MOUSE_X2
#define YSCR_MOUSE_ABSOLUTE        YIN_MOUSE_ABSOLUTE
#define YSCR_MOUSE_VIRTUAL_DESKTOP YIN_MOUSE_VIRTUAL_DESKTOP
#define YSCR_MOUSE_UNLISTED        YIN_MOUSE_UNLISTED
typedef yin_mouse_report yscr_mouse_event;

/* YSCR_EV_INPUT_LOST: the input bridge lost or delayed events since the
 * last record (INPUT, "The input bridge"). u.u32[0] records overwritten
 * before they were decoded, [1] doorbells SDL refused, [2] decodes that
 * found no record, [3] doorbells waiting to be pushed again; all totals. */
#define YSCR_EV_INPUT_LOST 12u

/* YSCR_EV_KEY_DOUBLE: one dropped second report of a key press (INPUT,
 * "Second key reports"). t_ns the report's time, u.i64[0] its time minus
 * the kept report's (ns), u.u32[2] the scancode, u.u32[3] its device,
 * u.u32[4] the kept report's device. */
#define YSCR_EV_KEY_DOUBLE 13u

/* YSCR_EV_UNSYNCED: the sync guard fired, once per open (SYNC GUARD).
 * t_ns the onset of the flip that fired it, aux desc.display_index,
 * u.u32[0] the rule (YSCR_SYNC_RULE_*), u.u32[1] flips with evidence in
 * the window, u.u32[2] the window's flips, u.u32[3] presents and u.u32[4]
 * refreshes of the rate block (0 for the flip rule), u.i64[3] the frame
 * index of that flip. */
#define YSCR_EV_UNSYNCED 14u
#define YSCR_SYNC_RULE_FLIPS 1u   /* at least 32 of the last 64 flips     */
#define YSCR_SYNC_RULE_RATE  2u   /* more presents than refreshes + 8     */

/* YSCR_EV_DEPTH: the depth changed (DEPTH). t_ns when, aux
 * desc.display_index, u.u16[0] the old depth (0 at open), u.u16[1] the new,
 * u.u16[2] the reason (YSCR_DEPTH_*), u.u16[3] the path, u.i64[1] the
 * frame whose flip record brought the change (-1 at open). */
#define YSCR_EV_DEPTH 15u

/* YSCR_EV_MODE's u.i32[5]: how the backend was chosen (BACKENDS). */
#define YSCR_AUTO_NAMED       0   /* desc.backend named it                 */
#define YSCR_AUTO_COMPOSITION 1   /* AUTO, and COMPOSITION opened          */
#define YSCR_AUTO_FALLBACK    2   /* AUTO, COMPOSITION refused: DXGI_FLIP  */
#define YSCR_DEPTH_OPEN    1u   /* measured at open, or the path's at open */
#define YSCR_DEPTH_PATH    2u   /* the path changed                       */
#define YSCR_DEPTH_PIN     3u   /* desc.depth, at open                    */
#define YSCR_DEPTH_LEARNER 4u   /* desc.depth_learn: misses or evidence   */
#define YSCR_DEPTH_EARLY   5u   /* DXGI_FLIP: a flip not held showed early */

/* --- settling at open (SETTLE) ------------------------------------------- */

/* desc.settle, and the YSP_SETTLE environment variable (off, warn,
 * strict), which overrides it. */
#define YSCR_SETTLE_AUTO   0   /* strict fullscreen; warn in a window      */
#define YSCR_SETTLE_STRICT 1   /* open() fails and names the condition     */
#define YSCR_SETTLE_WARN   2   /* open() succeeds; describe and ring say so */
#define YSCR_SETTLE_OFF    3   /* no settling; recorded as SKIPPED          */

/* yscr_settle_info.result; YSCR_EV_SETTLE's u.u16[0]. */
#define YSCR_SETTLED        1u
#define YSCR_SETTLE_FAILED  2u   /* the cap came first                       */
#define YSCR_SETTLE_SKIPPED 3u   /* off, SIM, or no OS vblank times          */
#define YSCR_SETTLE_ABORTED 4u   /* an abort came; begin() reports it         */

/* Where the mode came from: u.u16[2]. */
#define YSCR_SETTLE_SRC_DEFAULT 0u   /* desc.settle AUTO                     */
#define YSCR_SETTLE_SRC_DESC    1u   /* desc.settle named it                 */
#define YSCR_SETTLE_SRC_ENV     2u   /* YSP_SETTLE                           */
#define YSCR_SETTLE_SRC_BACKEND 3u   /* SIM, or a presenter without OS times */

/* The condition not met at the cap, or the last that broke a run of clean
 * flips: u.u16[3]. */
#define YSCR_SETTLE_C_NONE     0u
#define YSCR_SETTLE_C_ANCHOR   1u   /* no OS vblank time yet: a guessed grid */
#define YSCR_SETTLE_C_STATS    2u   /* no OS statistic, or occluded          */
#define YSCR_SETTLE_C_EARLY    3u
#define YSCR_SETTLE_C_LATE     4u   /* dropped, or a late target            */
#define YSCR_SETTLE_C_GRID     5u   /* an OS time off the grid               */
#define YSCR_SETTLE_C_TIER     6u   /* above 1 fullscreen, above 2 a window  */
#define YSCR_SETTLE_C_PATH     7u
#define YSCR_SETTLE_C_DEPTH    8u
#define YSCR_SETTLE_C_SPREAD   9u   /* the last 20 vblank intervals          */
#define YSCR_SETTLE_C_UNSYNCED 10u  /* the sync guard fired                  */

/* YSCR_EV_SETTLE: open's settling, once per open, at its end (also when
 * skipped). t_ns then, aux desc.display_index, u.u16[0] the result, [1]
 * the mode applied (STRICT, WARN or OFF), [2] the source, [3] the
 * condition, u.i64[1] ns from the open() call, u.u32[4] presents since the
 * open() call, [5] clean flips in a row at the end, [6] the spread's SD in
 * ns (0xFFFFFFFF: fewer than 20 intervals), u.i32[7] the mean interval
 * minus the mode's period, ns, u.u32[8] how the window got the foreground
 * (YSCR_FG_*, FOREGROUND), [9] desc.foreground as applied (HONOR or
 * FORCE; 0 without a window). */
#define YSCR_EV_SETTLE 16u

/* YSCR_EV_VISIBILITY: the page was hidden or shown (BACKENDS, WEB). t_ns
 * the DOM event's time, aux desc.display_index, u.u32[0] 1 visible, 0
 * hidden; written by the begin() after the change. */
#define YSCR_EV_VISIBILITY 17u

/* desc.window_thread (WINDOW THREAD, v0.10.0). */
#define YSCR_WINDOW_THREAD_AUTO 0  /* on for DXGI_FLIP and COMPOSITION         */
#define YSCR_WINDOW_THREAD_ON   1  /* on, or open() fails                      */
#define YSCR_WINDOW_THREAD_OFF  2  /* the frame thread owns the window (0.9.0) */

/* desc.foreground (FOREGROUND). */
#define YSCR_FOREGROUND_AUTO  0   /* FORCE fullscreen, HONOR in a window */
#define YSCR_FOREGROUND_HONOR 1   /* take what the OS policy gives       */
#define YSCR_FOREGROUND_FORCE 2   /* raise again with SDL's force hint   */

/* How the window got the foreground: yscr_settle_info.foreground and
 * YSCR_EV_SETTLE's u.u32[8]. */
#define YSCR_FG_NA      0u   /* no window (SIM, CUSTOM), or not Windows      */
#define YSCR_FG_GRANTED 1u   /* the OS gave it at the first raise            */
#define YSCR_FG_FORCED  2u   /* the OS refused; the forced raise took it     */
#define YSCR_FG_REFUSED 3u   /* another window has it: keys go there         */

/* What open's settling did (SETTLE), from yscr_settle_check(). */
typedef struct yscr_settle_info {
    uint32_t result;          /* YSCR_SETTLED ...; 0 when not open          */
    uint32_t mode;            /* applied: STRICT, WARN or OFF               */
    uint32_t source;          /* YSCR_SETTLE_SRC_*                          */
    uint32_t condition;       /* YSCR_SETTLE_C_*                            */
    int64_t  ns;              /* from the open() call to the result         */
    int32_t  flips;           /* presents since the open() call             */
    int32_t  run;             /* clean flips in a row at the end            */
    int32_t  need;            /* the run asked for (desc.settle_flips)      */
    uint32_t foreground;      /* YSCR_FG_* (FOREGROUND)                     */
    int64_t  min_ns, max_ns;  /* the minimum and the cap applied            */
    int64_t  spread_sd_ns;    /* -1: fewer than 20 intervals                */
    int64_t  spread_mean_ns;  /* mean interval minus the mode's period      */
    int64_t  hook_ns;         /* desc.on_context's time; added to the cap  */
    char     message[256];    /* what happened, for the operator            */
} yscr_settle_info;

/* What the sync guard saw (SYNC GUARD), from yscr_sync_check(). Counts are
 * of flips with an OS time since open, after open's own black frames. */
typedef struct yscr_sync_info {
    int32_t  untimed;        /* 1: the guard fired; stays until close       */
    uint32_t rule;           /* YSCR_SYNC_RULE_* that fired it; 0           */
    int64_t  fired_index;    /* frame index of the flip that fired it; -1   */
    int32_t  window;         /* flips in the window, at most 64             */
    int32_t  evidence;       /* of them, flips with evidence of no sync     */
    int32_t  peak;           /* the most evidence one window held since
                              * open: the margin to 32                      */
    uint32_t observed;       /* flips with an OS time, total                */
    uint32_t same_refresh;   /* evidence: two flips in one refresh          */
    uint32_t no_wait;        /* evidence: shown in the refresh it was
                              * presented in, with no vblank between        */
    uint32_t torn;           /* evidence: over a quarter period before the
                              * vblank its count names                      */
    uint32_t block_presents; /* the rate block so far: presents completed   */
    uint32_t block_refreshes;/* ... and refreshes between its flips         */
    char     message[512];   /* "" until it fires; then the reason and the
                              * driver settings that force vsync off        */
} yscr_sync_info;

/* The bridge's counters, totals since the process started. */
typedef struct yscr_input_stats {
    uint32_t stored;        /* events stored by yscr_push_input() and raw mice */
    uint32_t overwritten;   /* records overwritten before anyone decoded them  */
    uint32_t refused;       /* doorbells SDL refused (its queue full)          */
    uint32_t pending;       /* refused doorbells not pushed again yet          */
    uint32_t lost;          /* doorbells decoded after their record was gone   */
    uint32_t key_doubles;   /* second key-downs dropped (v0.4.1)               */
    uint32_t key_double_ups;/* their key-ups dropped                           */
} yscr_input_stats;

/* --- abort and panic (ABORT, PANIC) --------------------------------------- */

/* yscr_frame.abort: why begin() returned YSCR_QUIT. Several can be set. */
#define YSCR_ABORT_KEY     0x01u /* the abort combination (desc.abort_keys)  */
#define YSCR_ABORT_CLOSE   0x02u /* the window's close request: close button,
                                    * taskbar, Task Manager's "End task"     */
#define YSCR_ABORT_ALT_F4  0x04u /* Alt+F4                                   */
#define YSCR_ABORT_QUIT    0x08u /* SDL's quit event: Ctrl+C in the console,
                                    * logoff, the last window closed          */
#define YSCR_ABORT_REQUEST 0x10u /* yscr_request_abort()                   */
#define YSCR_ABORT_PANIC   0x20u /* the panic released the display: close
                                    * the screen (PANIC; v0.10.0)             */

/* yscr_abort_keys.mods. Either side's key counts. */
#define YSCR_MOD_SHIFT 0x01u
#define YSCR_MOD_CTRL  0x02u
#define YSCR_MOD_ALT   0x04u
#define YSCR_MOD_GUI   0x08u
#define YSCR_MOD_NONE  0x80u     /* the key alone: no modifier needed        */

#define YSCR_KEY_ESCAPE 0x1Bu    /* SDLK_ESCAPE, so this header needs no SDL */

/* The exit code of a process the panic watchdog ends. */
#define YSCR_PANIC_EXIT_CODE 99

/* The abort combination. Zero: Shift+Esc. */
typedef struct yscr_abort_keys {
    bool     off;   /* no key aborts; close, Alt+F4, quit and requests still do */
    uint32_t key;   /* an SDL keycode (SDL_Keycode); 0 = YSCR_KEY_ESCAPE      */
    uint32_t mods;  /* YSCR_MOD_* that must all be held; 0 = YSCR_MOD_SHIFT */
} yscr_abort_keys;

typedef void (*yscr_panic_fn)(void* ctx);

/* desc.on_context (CONTEXT HOOK): once per open, when the GL context
 * exists, before the warm-up and settling. */
struct yscr_screen;
typedef void (*yscr_context_fn)(void* ctx, struct yscr_screen* s);

/* Patch corners. */
#define YSCR_TOP_LEFT     0
#define YSCR_TOP_RIGHT    1
#define YSCR_BOTTOM_LEFT  2
#define YSCR_BOTTOM_RIGHT 3
#define YSCR_PATCH_DEFAULT_SIZE 32

typedef enum yscr_backend {
    YSCR_BACKEND_AUTO = 0,       /* COMPOSITION where it runs, else DXGI_FLIP */
    YSCR_BACKEND_DXGI_FLIP,      /* Windows 10 and 11                      */
    YSCR_BACKEND_COMPOSITION,    /* Windows 11 composition swapchain       */
    YSCR_BACKEND_GLX_OML,        /* Linux X11: stub                        */
    YSCR_BACKEND_WAYLAND,        /* stub                                   */
    YSCR_BACKEND_METAL,          /* macOS: stub                            */
    YSCR_BACKEND_WEB,            /* Emscripten: WebGL2, requestAnimationFrame */
    YSCR_BACKEND_SIM,            /* no window, a vblank grid on ysp_rt     */
    YSCR_BACKEND_CUSTOM          /* desc.presenter                         */
} yscr_backend;

typedef enum yscr_kind {
    YSCR_FIXED_GRID = 1,
    YSCR_CONTINUOUS = 2,
    YSCR_CALLBACK   = 3
} yscr_kind;

typedef enum yscr_path {
    YSCR_PATH_UNKNOWN     = 0,
    YSCR_PATH_COMPOSED    = 1,
    YSCR_PATH_OVERLAY     = 2,
    YSCR_PATH_INDEPENDENT = 3,
    YSCR_PATH_SIMULATED   = 4,
    YSCR_PATH_BROWSER     = 5   /* the browser's compositor (BACKENDS, WEB) */
} yscr_path;

/* A display mode. The refresh is a rational in Hz. */
typedef struct yscr_mode {
    int32_t  w, h;           /* pixels                                      */
    int32_t  refresh_num;    /* 0 = unknown                                  */
    int32_t  refresh_den;
    int64_t  period_ns;      /* refresh_den * 1e9 / refresh_num, rounded     */
    uint32_t format;         /* SDL pixel format                             */
    float    density;
} yscr_mode;

typedef struct yscr_display_info {
    uint32_t    id;          /* SDL_DisplayID                                */
    char        name[64];
    int32_t     x, y, w, h;  /* desktop bounds                               */
    yscr_mode desktop;
    bool        primary;
} yscr_display_info;

typedef struct yscr_caps {
    yscr_kind    kind;
    yscr_backend backend;
    int64_t  period_ns;        /* FIXED_GRID, CALLBACK: the mode's period     */
    int64_t  min_interval_ns;  /* CONTINUOUS                                  */
    int64_t  max_interval_ns;
    bool     native_target;    /* the OS takes a target vblank or time        */
    bool     hw_onset;         /* onsets are OS vblank timestamps             */
    bool     vrr_capable;      /* necessary, not sufficient                   */
    int32_t  max_in_flight;    /* 1                                           */
    yscr_mode mode;          /* the mode obtained, read back from the OS    */
    yscr_tier worst_tier;    /* the worst tier of any flip since open       */
    bool     raw_keyboard;     /* keys come on SDL's raw path now (INPUT)     */
} yscr_caps;

typedef struct yscr_patch {
    bool    on;
    int32_t size;              /* pixels, square; 0 = YSCR_PATCH_DEFAULT_SIZE */
    int32_t corner;            /* YSCR_TOP_LEFT .. YSCR_BOTTOM_RIGHT        */
} yscr_patch;

/* --- codes: exact device values in the frame (CODES) ---------------------- */

#define YSCR_MAX_CODES        8
#define YSCR_CODE_ROW_PIXELS  1024   /* all ROW slots together             */
#define YSCR_CODE_HOLD        (-1)   /* frames: until changed              */

typedef enum yscr_code_kind {
    YSCR_CODE_SOLID = 0,     /* a rectangle of one value                     */
    YSCR_CODE_ROW   = 1      /* w pixels of one row, each its own value      */
} yscr_code_kind;

/* A code slot, declared at open. Values are 0xBBGGRR: red in bits 0 to 7. */
typedef struct yscr_code_slot {
    int32_t  kind;             /* yscr_code_kind                             */
    int32_t  x, y;             /* display pixels, top-left origin              */
    int32_t  w, h;             /* ROW: w pixels; h is taken as 1               */
    uint32_t rest;             /* the value between codes, every pixel         */
} yscr_code_slot;

/* One code as the swap path draws it this flip. */
typedef struct yscr_code_draw {
    int32_t         x, y, w, h;
    uint32_t        value;     /* SOLID                                        */
    const uint32_t* px;        /* ROW: w values; NULL for SOLID                */
} yscr_code_draw;

/* record.code_risk: why a code pixel may not reach the display exactly. */
#define YSCR_CODE_RISK_COMPOSED       0x0001u /* this flip went through DWM */
#define YSCR_CODE_RISK_ADVANCED_COLOR 0x0002u /* HDR or auto color management */
#define YSCR_CODE_RISK_GAMMA          0x0004u /* the OS ramp is not identity  */
#define YSCR_CODE_RISK_SCALED         0x0008u /* mode is not the panel's own  */
#define YSCR_CODE_RISK_UNVERIFIED     0x0010u /* the open-time self test did not run */
#define YSCR_CODE_RISK_VERIFY_FAILED  0x0020u /* a read-back differed (sticky) */
#define YSCR_CODE_RISK_STATE_UNKNOWN  0x0040u /* the display state was not readable */

/* --- triggers at onset (TRIGGERS) ----------------------------------------- */

#define YSCR_MAX_TRIGGERS 8
#define YSCR_MAX_JOBS     16

/* yscr_trigger_info.flags and yscr_trigger_result.flags */
#define YSCR_TRIG_MOVED       0x0001u /* armed for a later vblank than planned */
#define YSCR_TRIG_FIRED_EARLY 0x0002u /* fired, then the frame was shown later */
#define YSCR_TRIG_FIRED_LATE  0x0004u /* fired, then the frame was shown earlier */
#define YSCR_TRIG_WORKER_LATE 0x0008u /* fired over 1 ms after its deadline    */
#define YSCR_TRIG_CANCELED    0x0010u /* never fired: its flip was not shown   */
#define YSCR_TRIG_FLUSHED     0x0020u /* run early by close()                  */
#define YSCR_TRIG_NOT_SHOWN   0x0040u /* its flip was skipped or canceled      */
#define YSCR_TRIG_PENDING     0x0080u /* not fired yet when reported           */
#define YSCR_TRIG_GPU_MOVED   0x0100u /* moved: the GPU had not finished       */
#define YSCR_TRIG_ESTIMATED   0x0200u /* its flip has no OS time               */

typedef struct yscr_trigger_info {
    int64_t  deadline_ns;      /* planned vblank + onset offset + channel offset */
    int64_t  fired_ns;         /* the clock as the callback starts            */
    int64_t  woke_ns;          /* the clock as ysp/rt.h's worker entered the
                                * job, after its spin: woke_ns - deadline_ns
                                * is ysp/rt.h's own lateness                */
    int64_t  lock_ns;          /* then the wait for the trigger lock         */
    int64_t  frame;            /* flip index; -1 for yscr_trigger_at()      */
    uint32_t code;
    uint16_t channel;
    uint16_t flags;            /* MOVED, GPU_MOVED, FLUSHED                   */
} yscr_trigger_info;

typedef void (*yscr_trigger_fn)(void* ctx, const yscr_trigger_info* info);

typedef struct yscr_trigger_desc {
    yscr_trigger_fn fn;
    void*       ctx;
    int64_t     offset_ns;     /* added to the planned vblank + onset offset  */
    const char* name;
} yscr_trigger_desc;

typedef struct yscr_trigger_result {
    int64_t  deadline_ns;
    int64_t  fired_ns;         /* 0 = not fired                               */
    int64_t  onset_ns;         /* the flip's onset; 0 = not shown             */
    int64_t  frame;
    uint32_t code;
    uint16_t channel;
    uint16_t flags;
    int32_t  mismatch;         /* vblank shown minus vblank fired for         */
    int32_t  reserved_;
} yscr_trigger_result;

/* What yscr_on_present()'s callback gets. */
typedef struct yscr_present_info {
    int64_t index;             /* frame number                                */
    int64_t onset;             /* begin()'s predicted onset                   */
    int32_t w, h;              /* drawable pixels                             */
    int32_t rows_top_down;     /* 1: GL row 0 is the top of the screen        */
    int32_t reserved_;
} yscr_present_info;

/* The record of one flip. */
typedef struct yscr_record {
    int64_t  index;      /* frame number                                      */
    int64_t  target;     /* t passed to flip_at, RT ns                         */
    int64_t  planned;    /* the vblank time the flip was planned for (+offset) */
    int64_t  onset;      /* estimated onset, RT ns (+offset); 0 while pending */
    int64_t  residual;   /* onset - target                                    */
    uint32_t dropped;    /* vblanks between planned and actual                */
    uint8_t  path;       /* yscr_path                                       */
    uint8_t  tier;       /* yscr_tier                                       */
    uint16_t flags;      /* YSCR_FLIP_*                                     */
    uint32_t phase_ns[YSCR_N_PHASES];
    uint16_t code_risk;  /* YSCR_CODE_RISK_*; 0 on a screen with no codes   */
    uint16_t reserved_;
} yscr_record;

typedef void (*yscr_flip_fn)(void* ctx, const yscr_record* r,
                               const yscr_trigger_result* trig, int n_trig);
typedef void (*yscr_present_fn)(void* ctx, const yscr_present_info* info);

/* What the frame loop draws for. */
typedef struct yscr_frame {
    int64_t onset;       /* predicted onset of this frame's flip, RT ns       */
    int64_t period;      /* ns; 0 when there is no fixed period                */
    int64_t index;       /* frame number, 0 for the first                     */
    int64_t vblank;      /* the vblank count it is planned for                */
    const yscr_record* last; /* newest completed flip record; NULL before one */
    /* Every flip record completed since the begin() before this one, in
     * completion order (records complete in begin(), flip_at(),
     * wait_flip()); last is the final one. Valid until the next begin(). */
    const yscr_record* done;
    int32_t  n_done;
    uint32_t done_lost;  /* records that did not fit (YSCR_MAX_DONE), since open */
    /* Set only when begin() returns YSCR_QUIT (every other field is then
     * 0): the aborts since the last report (ABORT). */
    uint32_t abort;          /* YSCR_ABORT_*                               */
    int32_t  abort_presses;  /* presses of the abort combination in it       */
    int64_t  abort_ns;       /* the first one's time, RT ns                  */
} yscr_frame;

/* The records one yscr_frame can carry in done. One frame in flight
 * completes at most a few per begin(); more means begin() was not called
 * while many flips completed. */
#define YSCR_MAX_DONE 16

/* Native handles of a swap path on Windows (yscr_native()). */
typedef struct yscr_native_info {
    void*    d3d11_device;   /* ID3D11Device*, the device ANGLE renders with  */
    void*    d3d11_context;  /* its immediate ID3D11DeviceContext*            */
    void*    egl_display;    /* ANGLE's EGLDisplay                            */
    uint32_t luid_low;       /* the adapter's LUID (DXGI_ADAPTER_DESC.AdapterLuid) */
    int32_t  luid_high;
    int32_t  video;          /* 1: made with D3D11_CREATE_DEVICE_VIDEO_SUPPORT */
    int32_t  mt_protected;   /* 1: multithread protection is on               */
} yscr_native_info;

/* --- presenter (the swap-path interface) ------------------------------- */

/* A GL or EGL entry point. Cast it to the function's own type to call it;
 * ISO C converts between function pointer types, not to and from void*. */
typedef void (*yscr_proc)(void);

#define YSCR_PRESENTER_VERSION 2

typedef struct yscr_vblank {
    uint64_t present_id;     /* the present it completes; 0 = a bare vblank */
    int64_t  t_ns;           /* vblank time on the ysp_rt clock              */
    int64_t  count;          /* vblank counter                               */
    uint8_t  path;           /* yscr_path                                  */
    uint8_t  tier;           /* yscr_tier; 0 = from the path               */
    uint16_t flags;          /* OCCLUDED, SKIPPED, CANCELED, ONSET_PLANNED;
                              * t_ns 0 = no time (the core estimates)       */
    uint32_t reserved_;
} yscr_vblank;

typedef struct yscr_present_req {
    uint64_t present_id;     /* from the core, increasing                    */
    int64_t  target_count;   /* the vblank to show on; 0 = the next one      */
    int64_t  target_ns;      /* the same vblank as a time                    */
    int32_t  hold;           /* 1                                            */
    int32_t  patch_on;       /* paint the patch, when draws_patch            */
    int32_t  patch_x, patch_y, patch_w, patch_h;   /* pixels, top-left origin */
    float    patch_value;    /* gray, 0..1                                   */
    int32_t  verify;         /* 1: read the codes back (draws_patch)         */
    const yscr_code_draw* codes;      /* drawn after the patch, when draws_patch */
    int32_t  n_codes;
    int32_t  reserved_;
} yscr_present_req;

typedef struct yscr_presenter_open {
    struct SDL_Window*  window;    /* NULL when the presenter needs none     */
    uint32_t            display;
    const yscr_mode*  mode;
    const char*         angle_dir;
    int64_t             sim_period_ns;
    int32_t             n_codes;   /* code slots: run the code self test      */
    int32_t             want_gpu_done;  /* desc.trigger_fence: set up gpu_done */
    /* Written by a presenter that draws codes and reads them back: */
    uint32_t*           code_checked;  /* codes compared                     */
    uint32_t*           code_failed;   /* codes that differed                */
    int32_t*            code_selftest; /* 1 passed, -1 failed, 0 not run     */
    int32_t             d3d11_video;   /* desc.d3d11_video                   */
} yscr_presenter_open;

typedef struct yscr_presenter {
    uint32_t    version;           /* YSCR_PRESENTER_VERSION               */
    const char* name;
    bool        needs_window;      /* the core creates the SDL window        */
    bool        waits_block;       /* acquire() blocks the thread            */
    bool        draws_patch;       /* present() paints the patch itself      */
    int   (*open)(void* ctx, const yscr_presenter_open* in, yscr_caps* caps,
                  char* err, size_t err_cap);
    void  (*close)(void* ctx);
    int   (*acquire)(void* ctx, int64_t deadline_ns, yscr_vblank* newest);
    int   (*present)(void* ctx, const yscr_present_req* req);
    int   (*completions)(void* ctx, yscr_vblank* out, int cap);
    yscr_proc (*gl_proc)(void* ctx, const char* name); /* may be NULL        */
    void  (*bind)(void* ctx);                         /* may be NULL           */
    int   (*describe)(void* ctx, char* buf, size_t cap); /* may be NULL        */
    /* The highest present_id whose GPU work has finished, or 0 if unknown.
     * Called from the trigger worker thread; may be NULL. */
    uint64_t (*gpu_done)(void* ctx);
} yscr_presenter;

/* --- open ---------------------------------------------------------------- */

typedef struct yscr_desc {
    uint32_t       display;          /* SDL_DisplayID; 0 = primary            */
    yscr_mode    mode;             /* zero = the desktop mode                */
    bool           windowed;
    int32_t        window_w, window_h;  /* windowed; 0 = 800 x 600            */
    yscr_backend backend;          /* 0 = AUTO                               */
    double         lead;             /* 0 = 0.5; YSCR_LEAD_NONE              */
    yrt_ring*    ring;             /* flip records; NULL = none              */
    uint32_t       display_index;    /* aux of every record                    */
    yscr_patch   patch;
    bool           show_cursor;
    yscr_abort_keys abort_keys;    /* zero: Shift+Esc (ABORT)                */
    bool           vrr;              /* refused                                */
    int32_t        min_tier;         /* 0 = off; else flag flips worse than it */
    int64_t        onset_offset_ns;
    int64_t        sim_period_ns;    /* BACKEND_SIM; 0 = 1e9 / 60              */
    const char*    angle_dir;
    const yscr_presenter* presenter;   /* BACKEND_CUSTOM                     */
    void*          presenter_ctx;
    /* codes (CODES) */
    yscr_code_slot codes[YSCR_MAX_CODES];
    int32_t        n_codes;
    int32_t        verify_codes;     /* read the codes back every N flips; 0 = off */
    bool           codes_strict;     /* open() fails when codes are at risk   */
    /* the OS gamma ramp (OS GAMMA) */
    bool           keep_os_gamma;    /* fullscreen: leave the OS ramp alone   */
    /* triggers at onset (TRIGGERS) */
    bool           trigger_fence;    /* move a trigger when the GPU is late   */
    int32_t        trigger_cpu;      /* 0 = default: E-cores on a hybrid CPU (TRIGGERS);
                                      * k > 0 = logical CPU k - 1; -1 = ysp/rt.h's */
    int64_t        trigger_spin_ns;  /* the worker's spin window, ns; 0 = ysp_rt's default */
    const yscr_trigger_desc* triggers;  /* copied at open                   */
    int32_t        n_triggers;
    /* the panic watchdog (PANIC); Windows, fullscreen */
    bool           panic;            /* arm it; off by default                 */
    int32_t        panic_presses;    /* abort presses that panic; 0 = 3        */
    int64_t        panic_window_ns;  /* ... within this time; 0 = 2 s          */
    int64_t        panic_grace_ns;   /* no panic this long after begin()
                                      * reported an abort; 0 = 10 s           */
    yscr_panic_fn panic_fn;        /* last words, on another thread; NULL = none */
    void*          panic_ctx;
    /* the window icon (WINDOW ICON) */
    const uint8_t* icon_rgba;        /* icon_w x icon_h RGBA, top row first;
                                      * NULL = the header's impossible cube   */
    int32_t        icon_w, icon_h;   /* 1 to 256                               */
    bool           icon_sdl;         /* keep SDL's own icon                    */
    /* the D3D11 device for a video decoder (NATIVE HANDLES) */
    bool           d3d11_video;      /* VIDEO_SUPPORT and multithread protection */
    /* input (INPUT) */
    bool           gamepads;         /* start SDL's gamepad subsystem and open
                                      * every gamepad                         */
    bool           raw_mice;         /* read every mouse's Raw Input on a
                                      * thread: per device, raw-timed (Windows) */
    int64_t        key_dedup_ns;     /* yscr_event_input() drops a key's second
                                      * report within this time; 0 = 50 ms,
                                      * < 0 = off, at most 1 s (INPUT)       */
    /* the depth (DEPTH) */
    int32_t        depth;            /* 0 = from the path; 1 to 8 pins it    */
    bool           depth_learn;      /* misses raise it (opt-in; not with a pin) */
    /* settling at open (SETTLE) */
    int32_t        settle;           /* YSCR_SETTLE_*; 0 = AUTO; YSP_SETTLE overrides */
    int32_t        settle_flips;     /* clean flips in a row; 0 = 6; at most 64 */
    int64_t        settle_min_ns;    /* from the open() call; 0 = 2.5 s in a
                                      * window, none fullscreen; < 0 = none   */
    int64_t        settle_max_ns;    /* the cap from the open() call; 0 = 3 s
                                      * fullscreen, 4 s in a window; <= 60 s  */
    /* the foreground (FOREGROUND) */
    int32_t        foreground;       /* YSCR_FOREGROUND_*; 0 = AUTO          */
    /* work that overlaps settling (CONTEXT HOOK) */
    yscr_context_fn on_context;     /* once, before the warm-up; NULL = none */
    void*          on_context_ctx;
    /* the window's own thread (WINDOW THREAD) */
    int32_t        window_thread;    /* YSCR_WINDOW_THREAD_*; 0 = AUTO         */
} yscr_desc;

/* Private. One flip between flip_at() and its completion. */
typedef struct yscr__pend {
    yscr_record rec;
    uint64_t    id;
    int64_t     planned_count;
    int64_t     count_at_present;
    int64_t     t_ret;            /* when the present call returned         */
    int64_t     t_call;           /* when it was made                       */
    uint32_t    code_vals[YSCR_MAX_CODES];  /* for the CODE record        */
    int32_t     asap;
    int32_t     held;             /* planned after the first vblank it could make */
    int32_t     used;
} yscr__pend;

#define YSCR__MAX_PEND 8
#define YSCR__MAX_PADS 8
#define YSCR__MAX_DEVS 32
/* Slack: the planned vblank's time minus the present call's return, in
 * bins of a 32nd of a period over 4 periods, per path. */
#define YSCR__SLACK_BINS  128
#define YSCR__SLACK_PATHS 5
#define YSCR__SLACK_DECAY 4096   /* flips between halvings of the counts  */
/* Off-grid OS times in a row that agree with the first of them on a new
 * phase, and so move the grid there. DXGI's off-grid times agreed so on
 * at most 5 in a row (docs/screen.md, "Held frames a vblank early"). */
#define YSCR__REGRID_N 16
#define YSCR__BACKEND_WORDS 192

/* Private. One at-onset trigger job. */
typedef struct yscr__job {
    int32_t  state;            /* 0 free, 1 armed, 2 firing, 3 fired, 4 canceled */
    uint16_t channel, flags;
    uint32_t code;
    int32_t  mismatch;
    uint64_t pend_id;          /* the flip's present id; 0 = trigger_at        */
    int64_t  frame;
    int64_t  count;            /* the vblank it fires for                      */
    int64_t  deadline;
    int64_t  wake;             /* deadline, or the GPU check before it         */
    int64_t  fired;
    int64_t  onset;
    int64_t  period;           /* for a move the worker makes                  */
    int32_t  flip_done;
    int32_t  check;            /* 1: check the GPU at wake, then fire          */
} yscr__job;

/* The screen. Caller-allocated and zeroed; every field is private. */
typedef struct yscr_screen {
    int                     open;
    int                     begun;
    int                     slot_held;
    int                     warming;
    int                     pr_open;
    int                     polled;
    yscr_backend          backend;
    const yscr_presenter* pr;
    void*                   pr_ctx;
    struct SDL_Window*      window;
    int                     sdl_video;
    int                     cursor_hidden;
    int                     text_input;   /* text input on, as last logged    */
    int                     sdl_gamepad;  /* desc.gamepads: the subsystem is up */
    void*                   pads[YSCR__MAX_PADS];   /* SDL_Gamepad*, opened here */
    uint64_t                dev_id[YSCR__MAX_DEVS]; /* the devices logged, for
                                                       * one record per change */
    uint8_t                 dev_kind[YSCR__MAX_DEVS];
    uint32_t                rm_unlisted[16];          /* raw mice SDL does not list */
    int32_t                 raw_mice;                 /* desc.raw_mice: a reader user */
    int32_t                 rm_state;                 /* 1 while relative mode holds the mice */
    int32_t                 rm_rel, rm_frames;        /* the guard's last relative mode, frames */
    int64_t                 key_dedup_ns;             /* desc.key_dedup_ns: 0 = 50 ms, < 0 off */
    yscr_caps             caps;
    yrt_ring*             ring;
    uint32_t                display_index;
    double                  lead;
    int64_t                 lead_ns;
    int64_t                 offset;
    yscr_patch            patch;
    float                   patch_value;
    /* abort and panic */
    int32_t                 abort_seen;   /* the last abort log entry reported */
    int32_t                 abort_user;   /* holds the process's abort state   */
    int32_t                 abort_slot;   /* + 1: in the window and panic table */
    int32_t                 panic_armed;
    void*                   hwnd;         /* the window's HWND, for the watchdog */
    void*                   win_icon[2];  /* HICONs the header set: small, big */
    /* the grid */
    int                     have_anchor;
    int64_t                 vb_t, vb_count;
    int64_t                 ref_t, ref_count;
    int64_t                 count_bias;   /* the grid's vblank number minus the OS's count */
    int64_t                 cand_t;       /* an off-grid time: a new phase may start there */
    int32_t                 cand_n;       /* off-grid times since that agree with it */
    uint64_t                grid_hist;    /* a bit per OS time, newest in bit 0: 1 = off the grid */
    int32_t                 grid_n;       /* bits in grid_hist, at most 64 */
    int32_t                 grid_follow;  /* 1: most times are off it, so it follows them */
    double                  period_f;
    double                  nominal_f;
    int64_t                 margin_ns;
    int32_t                 depth, depth_cand, depth_votes, depth_need;
    int32_t                 show_depth;   /* where a present on this path shows; = depth unless pinned */
    int32_t                 depth_pin;    /* desc.depth; 0 = from the path */
    int32_t                 depth_learn;  /* desc.depth_learn */
    uint32_t                depth_changes;   /* after open's */
    int32_t                 adopt_left;   /* queued flips a path change's window survives */
    int32_t                 last_obs, obs_streak;
    uint8_t                 warm_seen[9]; /* open's flips per depth shown, on the last path */
    int32_t                 depth_of[YSCR__SLACK_PATHS];   /* 0 = not known yet */
    int32_t                 lower_streak, fresh_lower;
    uint32_t                slack_n;
    uint16_t                slack_ok[YSCR__SLACK_PATHS][YSCR__SLACK_BINS];
    uint16_t                slack_miss[YSCR__SLACK_PATHS][YSCR__SLACK_BINS];
    uint8_t                 path;
    uint32_t                unstable;
    int32_t                 anchor_guessed;   /* open() got no OS time: the grid is a guess */
    bool                    hw_onset_pr;      /* the presenter's caps.hw_onset */
    /* the sync guard (SYNC GUARD) */
    uint64_t                sync_hist;    /* a bit per observed flip, newest
                                           * in bit 0: 1 = evidence        */
    int32_t                 sync_win;     /* bits in sync_hist, at most 64 */
    int32_t                 sync_peak;    /* the most bits set at once     */
    int32_t                 sync_off;     /* 1: fired                      */
    uint32_t                sync_rule;
    uint32_t                sync_obs, sync_same, sync_nowait, sync_torn;
    uint32_t                sync_p, sync_r;   /* the rate block            */
    int32_t                 sync_have_prev;
    int64_t                 sync_prev_count, sync_prev_t;
    int64_t                 sync_index;
    uint32_t                sync_fire[3]; /* evidence, presents, refreshes
                                           * when it fired                 */
    int32_t                 min_tier;
    int32_t                 worst_tier;
    int64_t                 prev_shown;
    int64_t                 last_planned; /* the vblank flip_at() last planned */
    yscr_record           fin[YSCR_MAX_DONE];   /* completed since begin() returned */
    yscr_record           fin_out[YSCR_MAX_DONE]; /* the list the frame shows */
    int32_t                 n_fin, n_fin_out;
    uint32_t                fin_lost;
    /* this frame */
    int64_t                 index;
    int64_t                 pred_count;
    int64_t                 mark_t;
    uint32_t                acc[YSCR_N_PHASES];
    uint64_t                next_id;
    yscr__pend            pend[YSCR__MAX_PEND];
    yscr_record           last;
    int                     have_last;
    /* input */
    int64_t                 sdl_rt, sdl_ticks, sdl_corr_t;
    uint64_t                sdl_width;
    /* GL, through the presenter */
    yscr_proc             gl[16];
    /* hooks */
    yscr_flip_fn          on_flip;
    void*                   on_flip_ctx;
    yscr_present_fn       on_present;
    void*                   on_present_ctx;
    uint32_t                gl_epoch;
    uint32_t                gl_generation;
    /* codes */
    int32_t                 n_codes;
    yscr_code_slot        code_slot[YSCR_MAX_CODES];
    uint32_t                code_val[YSCR_MAX_CODES];
    int32_t                 code_left[YSCR_MAX_CODES];  /* flips; -1 hold; 0 rest */
    int32_t                 code_off[YSCR_MAX_CODES];   /* ROW: start in code_px */
    int32_t                 code_buf;                     /* which frame buffer   */
    uint32_t                code_px[YSCR_CODE_ROW_PIXELS];
    uint32_t                code_frame_px[2][YSCR_CODE_ROW_PIXELS];
    yscr_code_draw        code_draw[2][YSCR_MAX_CODES];
    int32_t                 verify_every, code_selftest;
    uint32_t                code_checked, code_failed;
    uint16_t                code_risk;    /* display-wide part                  */
    char                    os_color[160]; /* for the describe line             */
    int32_t                 gamma_owned;
    /* triggers */
    int32_t                 n_trig, trig_on, trig_fence, worker_on, trig_cpu;
    char                    trig_cores;   /* 'E' E-cores, 'N' pinned, 0 ysp/rt.h's choice */
    uint64_t                trig_mask;    /* the worker's CPUs when trig_cores is set */
    int64_t                 trig_spin;
    yscr_trigger_desc     trig[YSCR_MAX_TRIGGERS];
    yscr__job             job[YSCR_MAX_JOBS];
    int32_t                 n_req;
    uint32_t                req_code[YSCR_MAX_JOBS];
    uint16_t                req_ch[YSCR_MAX_JOBS];
    int64_t                 armed_wake;
    int64_t                 woke;           /* the worker's wake, for its callback */
    uint32_t                trig_lost;      /* no free job at arm time         */
    uint64_t                lock_mem[16];   /* a CRITICAL_SECTION or a mutex   */
    int32_t                 lock_ok;
#if !defined(YRT_NO_THREADS)
    yrt_worker            worker;
#endif
    char                    error[256];
    uint64_t                backend_mem[YSCR__BACKEND_WORDS];
    int32_t                 auto_pick;    /* YSCR_AUTO_*: what AUTO did */
    char                    auto_why[96]; /* why AUTO fell back to DXGI_FLIP */
    /* settling at open (SETTLE) */
    uint64_t                done_id;      /* the newest present completed: a
                                           * statistic for an older one is stale */
    int32_t                 settling;     /* 1 while open() presents settle frames */
    uint32_t                st_result, st_mode, st_src, st_cond;
    int32_t                 st_need, st_run, st_tier, st_window, st_have, st_depth, st_flips, st_nts;
    int32_t                 st_bad;       /* the present whose flip set st_cond  */
    uint8_t                 st_path;
    int64_t                 st_t0, st_min, st_max, st_ns, st_sd, st_mean;
    int64_t                 st_ts[21];    /* the newest OS vblank times, oldest first */
    char                    st_env[16];   /* YSP_SETTLE as read                  */
    /* the foreground (FOREGROUND) and the vblank waiter (IDLE WAKE) */
    uint32_t                fg, fg_mode;  /* YSCR_FG_*; HONOR or FORCE applied  */
    void*                   fg_prev;      /* the foreground window before open  */
    void*                   wake;         /* the waiter's state; NULL = none     */
    int32_t                 hooked;       /* desc.on_context ran (CONTEXT HOOK)  */
    int64_t                 hook_ns;      /* ... for this long                   */
    /* the window thread (WINDOW THREAD) */
    int32_t                 wt;           /* 1: the window thread owns the window */
    int32_t                 wt_mode;      /* YSCR_WINDOW_THREAD_ON or _OFF as applied */
    int32_t                 wt_why;       /* AUTO's reason for off: YSCR__WT_WHY_* */
    int32_t                 wt_user;      /* holds the process's window-thread mode */
    /* the grid after a gap (THE GRID) */
    uint32_t                gap_anchors;  /* gaps the system's vblank closed */
    uint32_t                gap_refused;  /* ... it was too far off the grid */
    int64_t                 gap_shift_max;/* the largest move, ns            */
    int64_t                 gap_asked;    /* vb_t when the system was asked  */
} yscr_screen;

/* --- API ----------------------------------------------------------------- */

/* YSCR_VERSION_STRING of the implementation that was compiled. */
YSCR_API const char* yscr_version(void);

/* Static text for a YSCR_* code ("ok" for 0 and other positive values). */
YSCR_API const char* yscr_strerror(int code);

/* The connected displays: fills up to cap entries and returns the count, or
 * a negative code. Starts SDL's video subsystem for the call when no screen
 * holds it, which takes about 0.1 s: call it at setup, not in a trial. */
YSCR_API int yscr_displays(yscr_display_info* out, int cap);

/* The fullscreen modes of a display (0 = primary), in SDL's order: largest
 * and fastest first. Returns the count, which can be larger than cap. */
YSCR_API int yscr_modes(uint32_t display, yscr_mode* out, int cap);

/* The mode whose refresh is an integer multiple of num/den Hz (24000/1001
 * for NTSC film), at w x h (0 x 0 = the desktop size). Of the modes within
 * tol_ppm (0 = 200 ppm) of a multiple, the one with the highest refresh.
 * Returns the multiple (>= 1) and fills *out and *err_ppm (the refresh's
 * error against the exact multiple). Returns YSCR_ERR_REFUSED when no
 * mode qualifies, with the nearest mode and its error in *out and *err_ppm.
 * Nothing switches mode: open with desc.mode = *out to do that. */
YSCR_API int yscr_mode_multiple(uint32_t display, int32_t num, int32_t den,
                                    int32_t w, int32_t h, double tol_ppm,
                                    yscr_mode* out, double* err_ppm);

/* The same rule on a list you give (w or h 0 = any size). For a designer
 * that has no display, and for tests. */
YSCR_API int yscr_mode_multiple_in(const yscr_mode* modes, int n,
                                       int32_t num, int32_t den, int32_t w, int32_t h,
                                       double tol_ppm, yscr_mode* out, double* err_ppm);

/* Opens the window, the context and the swap path, presents black frames
 * to find the vblank grid and the depth and to settle the display (SETTLE:
 * 0.5 to 0.8 s fullscreen, 2.5 s in a window), and returns true. Calls
 * desc.on_context once before it settles (CONTEXT HOOK). On false, yscr_error() says why. The handle must be zeroed
 * or closed. Refuses desc.vrr, a desc.mode that is not a listed mode of the
 * display, a lead outside 0, (0, 1) or YSCR_LEAD_NONE, and a backend that
 * v0.1 does not have. */
YSCR_API bool        yscr_open(yscr_screen* s, const yscr_desc* desc);

/* Lets the last flip complete (its record reaches the ring), then closes
 * everything open() opened. Safe on a closed or zeroed handle. */
YSCR_API void        yscr_close(yscr_screen* s);

/* The message of the last call on s that failed (open() clears it):
 * every call that returns a negative code writes it first. "" after a
 * successful open. */
YSCR_API const char* yscr_error(const yscr_screen* s);
YSCR_API bool        yscr_is_open(const yscr_screen* s);

/* The capability kind, period, mode obtained and the rest; zeroes when the
 * screen is not open. */
YSCR_API void        yscr_get_caps(const yscr_screen* s, yscr_caps* out);

/* One line for the log: backend, adapter, ANGLE version, mode, measured
 * refresh, path, depth, lead, and warnings. Returns snprintf's count. */
YSCR_API int         yscr_describe(const yscr_screen* s, char* buf, size_t cap);

/* The sync guard's state (SYNC GUARD): whether flips have shown that they
 * are not synced to the vblank (a driver setting that forces vsync off),
 * the evidence, and a message for the operator. Zeroes (fired_index -1)
 * when the screen is not open. */
YSCR_API void        yscr_sync_check(const yscr_screen* s, yscr_sync_info* out);

/* What open's settling did (SETTLE): the result, the condition, the times
 * and a message for the operator. Zeroes when the screen is not open. */
YSCR_API void        yscr_settle_check(const yscr_screen* s, yscr_settle_info* out);

/* Starts a frame. Without the window thread, pumps SDL's events (read
 * them with yscr_poll(); WINDOW THREAD). Waits
 * until the swap path takes a frame (at most one is in flight), completes
 * the records of the flips that happened, binds the context and its back
 * buffer, and fills *f with the predicted onset of the first vblank this
 * frame can make. Returns YSCR_OK; YSCR_QUIT, without starting a frame,
 * once for each abort since the last report (ABORT): the abort
 * combination (Shift+Esc unless desc.abort_keys), a close request, Alt+F4,
 * SDL's quit event or yscr_request_abort(). f.abort says which; every
 * other field of *f is 0. The next begin() starts a frame: you decide how
 * to stop. Aborts read with yscr_poll() or SDL_PollEvent() before begin()
 * count too. YSCR_ERR_ORDER after a begin without a flip;
 * YSCR_ERR_TIMEOUT or YSCR_ERR_LOST from the swap path. */
YSCR_API int  yscr_begin(yscr_screen* s, yscr_frame* f);

/* Ends a phase of this frame (YSCR_PHASE_EVALUATE .. _UPLOAD): the time
 * since begin() or the previous mark goes to that phase. Optional; ignored
 * outside a frame. */
YSCR_API void yscr_mark(yscr_screen* s, int phase);

/* Ends the frame: draws the patch, and presents the frame for the vblank
 * t_ns (the ysp_rt clock) snaps to (see FLIP AT A TIME), never before the
 * first one it can make. Waits on the ysp_rt clock first when that vblank is later. Does not wait
 * for the flip: *out (may be NULL) gets index, target, planned, phases and
 * YSCR_FLIP_PENDING, and the complete record arrives with a later
 * begin() (f.last), with yscr_wait_flip(), and in desc.ring. Returns
 * YSCR_OK, YSCR_ERR_ORDER without a begin, or the swap path's error. */
YSCR_API int  yscr_flip_at(yscr_screen* s, int64_t t_ns, yscr_record* out);

/* yscr_flip_at() at the onset begin() predicted for this frame. */
YSCR_API int  yscr_flip(yscr_screen* s);

/* Blocks until every flip so far has its record, as Psychtoolbox's Flip
 * does, and copies the newest into *out (may be NULL). Call it between
 * frames, not between begin() and flip. YSCR_ERR_ORDER when no frame was
 * flipped yet. */
YSCR_API int  yscr_wait_flip(yscr_screen* s, yscr_record* out);

/* The patch's gray level for this and the next frames, clamped to 0..1. */
YSCR_API void yscr_set_patch(yscr_screen* s, float v);

/* --- codes (CODES) ---------------------------------------------------------
 * A value for code slot `slot` (desc.codes[slot]), 0xBBGGRR, drawn on the
 * next `frames` flips and then the slot's rest value. yscr_code() is one
 * flip; YSCR_CODE_HOLD holds it until the next call; 0 returns to rest
 * now. yscr_code_row() sets the n pixels of a ROW slot (the rest of the
 * row takes the rest value). They return YSCR_OK or YSCR_ERR_ARG. */
YSCR_API int yscr_code(yscr_screen* s, int slot, uint32_t rgb);
YSCR_API int yscr_code_frames(yscr_screen* s, int slot, uint32_t rgb, int frames);
YSCR_API int yscr_code_row(yscr_screen* s, int slot, const uint32_t* px, int n, int frames);
/* VPixx Pixel Mode: the display's top-left pixel; digital out bits 0 to 7
 * are red, 8 to 15 green, 16 to 23 blue, so the code is the 24-bit value. */
YSCR_API yscr_code_slot yscr_slot_pixel_mode(void);
YSCR_API uint32_t yscr_pixel_mode_bits(uint32_t ttl24);   /* the code for 24 output bits */
/* VPixx pixel sync as Psychtoolbox's PsychDataPixx draws it: 8 pixels on
 * scanline 0 from x = 10. yscr_psync_pattern() fills its 8 values. */
YSCR_API yscr_code_slot yscr_slot_psync(void);
YSCR_API void yscr_psync_pattern(uint32_t out[8], uint8_t counter);
/* The display-wide code risk (YSCR_CODE_RISK_*) and the read-back counts. */
YSCR_API uint16_t yscr_code_risk(const yscr_screen* s);
YSCR_API void yscr_code_verify(const yscr_screen* s, uint32_t* checked, uint32_t* failed);

/* --- triggers (TRIGGERS) ---------------------------------------------------
 * Fires desc.triggers[channel] for this frame's flip, at its planned vblank
 * + desc.onset_offset_ns + the channel's offset, on the screen's deadline
 * worker. Call between begin() and flip_at(). YSCR_ERR_REFUSED when the
 * frame already has YSCR_MAX_JOBS triggers or the queue is full. */
YSCR_API int yscr_trigger(yscr_screen* s, int channel, uint32_t code);
/* The same at time t_ns on the ysp_rt clock, not tied to a flip. */
YSCR_API int yscr_trigger_at(yscr_screen* s, int channel, uint32_t code, int64_t t_ns);

/* --- hooks -----------------------------------------------------------------
 * After flip: fn gets each completed record, oldest first, with the results
 * of its triggers, on the frame thread (AFTER FLIP). NULL removes it. */
YSCR_API void yscr_on_flip(yscr_screen* s, yscr_flip_fn fn, void* ctx);
/* Before present: fn may draw into the back buffer with GL (PRESENT
 * CALLBACK). NULL removes it. */
YSCR_API void yscr_on_present(yscr_screen* s, yscr_present_fn fn, void* ctx);
/* Changes after every yscr_on_present() callback, so a renderer that
 * caches GL state knows to drop it. */
YSCR_API uint32_t yscr_gl_epoch(const yscr_screen* s);
/* Changes when the GL context is new: every GL object made before is gone.
 * 0 while closed. Today only open() makes a context, so it is constant
 * from open to close (a lost device ends the screen: YSCR_ERR_LOST). */
YSCR_API uint32_t yscr_gl_generation(const yscr_screen* s);
#define YSCR_HAS_GL_EPOCH 1

/* --- native handles ---------------------------------------------------------
 * The D3D11 device and immediate context, ANGLE's EGL display and the
 * adapter LUID of a DXGI_FLIP or COMPOSITION screen (NATIVE HANDLES).
 * YSCR_OK; YSCR_ERR_NOT_IMPLEMENTED on any other backend and platform,
 * YSCR_ERR_CLOSED on a closed screen; out is zeroed then. */
YSCR_API int yscr_native(yscr_screen* s, yscr_native_info* out);

/* begin() and flip_at() on n screens. Each screen keeps its own grid, so
 * f[i].onset differ unless the displays are genlocked; flip_group_at snaps
 * t_ns (the ysp_rt clock) on each grid. YSCR_ERR_NOT_IMPLEMENTED for a swap path whose wait
 * blocks a thread (none in v0.1). An abort pending is reported in every
 * f[i] with YSCR_QUIT, and no member begins a frame. */
YSCR_API int  yscr_begin_group(yscr_screen* const* s, int n, yscr_frame* f);
YSCR_API int  yscr_flip_group_at(yscr_screen* const* s, int n, int64_t t_ns);

/* A GL ES 3.0 or EGL entry point of this screen's context; NULL on SIM.
 * Cast it to the function's type. */
YSCR_API yscr_proc yscr_gl_proc(const yscr_screen* s, const char* name);

/* The SDL window; NULL on SIM and on presenters that need none. With the
 * window thread (WINDOW THREAD), call SDL on it only inside
 * yscr_window_call(). */
YSCR_API struct SDL_Window* yscr_window(const yscr_screen* s);

/* Runs fn(ctx) on the thread that owns the screens' windows and waits for
 * it to return (WINDOW THREAD): the one door for SDL video calls (title,
 * cursor, mouse grab, more windows, Dear ImGui's SDL3 backend). Without a
 * window thread it runs fn here. s may be NULL: the process's window
 * thread. Any thread; calls run one at a time. YSCR_OK; YSCR_ERR_ARG for
 * a NULL fn; YSCR_ERR_TIMEOUT when the window thread took no call for
 * 5 s (fn did not run). */
typedef void (*yscr_window_fn)(void* ctx);
YSCR_API int yscr_window_call(yscr_screen* s, yscr_window_fn fn, void* ctx);

/* Makes this screen's context current on this thread, with its back
 * buffer. begin() does it; call it to draw to one screen of a group. */
YSCR_API void  yscr_bind(yscr_screen* s);

/* The next SDL event, and its time on the ysp_rt clock in *t_rt (may be
 * NULL): SDL_PollEvent(), or with the window thread SDL_PeepEvents(),
 * which does not pump (WINDOW THREAD). See INPUT for what the time is
 * worth. */
YSCR_API bool    yscr_poll(yscr_screen* s, union SDL_Event* ev, int64_t* t_rt);

/* The next raw mouse report (desc.raw_mice), oldest first: true and *out,
 * or false when none is waiting. Any screen drains the one process-wide
 * queue; call it from one thread, once a frame until false (INPUT, "Raw
 * mice"). A device SDL does not list gets YSCR_MOUSE_UNLISTED and, the
 * first time, a YSCR_EV_DEVICE record. */
YSCR_API bool    yscr_poll_mouse(yscr_screen* s, yscr_mouse_event* out);

/* The input bridge (INPUT, "The input bridge"). Any thread: store *e (its
 * t, the producer's ysp_rt stamp, is kept as given) and push one doorbell,
 * an SDL event of type yscr_input_event_type(), into SDL's queue.
 * YSCR_OK (also when SDL refused the doorbell: it is pushed again
 * later), YSCR_ERR_ARG, YSCR_ERR_CLOSED before the first screen with
 * SDL opened. */
YSCR_API int      yscr_push_input(const yin_event* e);

/* The doorbell's SDL event type; 0 before the first screen with SDL. */
YSCR_API uint32_t yscr_input_event_type(void);

/* One SDL event from yscr_poll() as a yin_event: a doorbell gives its
 * stored event (keyed by the doorbell's sequence number, user.code); SDL's
 * keyboard, mouse, touch, pen and gamepad events go through ysp/input.h's
 * adapter, with this screen's raw-keyboard state and the ysp_rt time.
 * false for any other event, a doorbell already decoded, a doorbell
 * whose record was overwritten (counted as lost), or a second report of a
 * key press or its key-up (counted; INPUT, "Second key reports"). s may
 * be NULL. */
YSCR_API bool     yscr_event_input(yscr_screen* s, const union SDL_Event* ev, yin_event* out);

/* The same key filter on an event that did not come from
 * yscr_event_input(), which applies it already: your own SDL adapter, a
 * simulated participant. false: drop it. Counted and logged like the
 * bridge's drops. s may be NULL (a 50 ms window, no ring record). */
YSCR_API bool     yscr_key_filter(yscr_screen* s, const yin_event* e);

/* The bridge's counters. */
YSCR_API void     yscr_get_input_stats(yscr_input_stats* out);

/* An SDL_GetTicksNS() value on the ysp_rt clock; 0 on a screen with no
 * window. */
YSCR_API int64_t yscr_restamp(const yscr_screen* s, uint64_t sdl_ticks_ns);

/* Starts (on) or stops SDL text input on the screen's window, with the IME
 * area at x, y, w, h (window pixels); a ring record on each change and
 * YSCR_FLIP_TEXT_INPUT on the flips planned while on (INPUT, "Text
 * input"). Off at open. YSCR_OK, YSCR_ERR_ARG, YSCR_ERR_CLOSED, or
 * YSCR_ERR_LOST when SDL refuses (yscr_error() says why). */
YSCR_API int     yscr_text_input(yscr_screen* s, bool on, int x, int y, int w, int h);

/* Asks every open screen to abort: the next begin() of each returns
 * YSCR_QUIT with YSCR_ABORT_REQUEST. Any thread; no screen needed (a
 * network command, a response box button). Not a panic. */
YSCR_API void    yscr_request_abort(void);

/* One desc field a designer sets. */
typedef struct yscr_param {
    const char* name;     /* the desc field, dotted for nested fields       */
    const char* type;     /* "u32", "i32", "i64", "f64", "bool", "enum"      */
    double      min, max; /* inclusive                                      */
    double      def;      /* the value a zero field means                   */
    const char* unit;     /* "", "px", "ns", "ms", "Hz", "frame"            */
    const char* doc;      /* one line                                       */
    const char* values;   /* v0.9, "enum": "name=value" pairs joined by ',',
                           * in value order; NULL for the other types       */
} yscr_param;

/* The table of desc fields a designer sets; *n gets the count. The pointer,
 * the presenter and the ANGLE path are not in it. */
YSCR_API const yscr_param* yscr_params(int* n);

#ifdef __cplusplus
}
#endif

#endif /* YSP_SCREEN_H_INCLUDED */

/* ======================================================================= *
 *                            IMPLEMENTATION                               *
 * ======================================================================= */
#ifdef YSP_SCREEN_IMPLEMENTATION
#ifndef YSP_SCREEN_IMPLEMENTATION_GUARD
#define YSP_SCREEN_IMPLEMENTATION_GUARD

#ifndef YSP_RT_IMPLEMENTATION_GUARD
    #define YSP_RT_IMPLEMENTATION
    #include "ysp/rt.h"
#endif
#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#elif !defined(YRT_NO_THREADS)
    #include <pthread.h>
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>

#if !defined(YSCR_NO_SDL)
    #include <SDL3/SDL.h>
    #include "ysp/input.h"   /* again, now with SDL: yin_from_sdl */
#endif

#if defined(__EMSCRIPTEN__) && !defined(YSCR_NO_SDL)
    #define YSCR__WEB 1
    #include <emscripten.h>
    #include <emscripten/html5.h>
#endif

#if defined(_WIN32) && !defined(YSCR_NO_SDL)
    #define YSCR__DXGI 1
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <d3d11_4.h>
    #include <dxgi1_5.h>
    #include <dwmapi.h>   /* the types only: dwmapi.dll is loaded at run time */
    #if !defined(YRT_NO_THREADS)
        #define YSCR__WT 1   /* the window thread (WINDOW THREAD) */
    #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque GL and EGL handles, so no Khronos header is needed. */
typedef void*        yscr__EGLDisplay;
typedef void*        yscr__EGLContext;
typedef void*        yscr__EGLSurface;
typedef void*        yscr__EGLConfig;
typedef int32_t      yscr__EGLint;
typedef unsigned int yscr__GLenum;

#if defined(_WIN32)
    #define YSCR__APIENTRY __stdcall
#else
    #define YSCR__APIENTRY
#endif

typedef void (YSCR__APIENTRY *yscr__glClearColor_fn)(float, float, float, float);
typedef void (YSCR__APIENTRY *yscr__glClear_fn)(unsigned int);
typedef void (YSCR__APIENTRY *yscr__glScissor_fn)(int, int, int, int);
typedef void (YSCR__APIENTRY *yscr__glEnable_fn)(yscr__GLenum);
typedef unsigned char (YSCR__APIENTRY *yscr__glIsEnabled_fn)(yscr__GLenum);
typedef void (YSCR__APIENTRY *yscr__glGetIntegerv_fn)(yscr__GLenum, int*);
typedef void (YSCR__APIENTRY *yscr__glGetFloatv_fn)(yscr__GLenum, float*);
typedef void (YSCR__APIENTRY *yscr__glGetBooleanv_fn)(yscr__GLenum, unsigned char*);
typedef void (YSCR__APIENTRY *yscr__glColorMask_fn)(unsigned char, unsigned char, unsigned char, unsigned char);
typedef void (YSCR__APIENTRY *yscr__glBindFramebuffer_fn)(yscr__GLenum, unsigned int);
typedef const unsigned char* (YSCR__APIENTRY *yscr__glGetString_fn)(yscr__GLenum);
typedef void (YSCR__APIENTRY *yscr__glFlush_fn)(void);

enum {
    YSCR__GL_CLEARCOLOR, YSCR__GL_CLEAR, YSCR__GL_SCISSOR, YSCR__GL_ENABLE,
    YSCR__GL_DISABLE, YSCR__GL_ISENABLED, YSCR__GL_GETINTEGERV,
    YSCR__GL_GETFLOATV, YSCR__GL_GETBOOLEANV, YSCR__GL_COLORMASK,
    YSCR__GL_BINDFRAMEBUFFER
};

#define YSCR__GL_COLOR_BUFFER_BIT     0x4000u
#define YSCR__GL_SCISSOR_TEST         0x0C11u
#define YSCR__GL_SCISSOR_BOX          0x0C10u
#define YSCR__GL_COLOR_CLEAR_VALUE    0x0C22u
#define YSCR__GL_COLOR_WRITEMASK      0x0C23u
#define YSCR__GL_RASTERIZER_DISCARD   0x8C89u
#define YSCR__GL_DRAW_FRAMEBUFFER     0x8CA9u
#define YSCR__GL_DRAW_FB_BINDING      0x8CA6u
#define YSCR__GL_VERSION              0x1F02u

/* --- small helpers ------------------------------------------------------- */

/* Test-only seam, not API: tests/adapt/screen_test.c defines these
 * before the implementation to run the core on a virtual clock, so no
 * check there depends on how the host schedules the test. */
#ifndef YSCR__NOW
#define YSCR__NOW() (yrt_now_ns())
#endif
#ifndef YSCR__SLEEP_UNTIL
#define YSCR__SLEEP_UNTIL(t, spin) yrt_sleep_until((int64_t)(t), (spin))
#endif
/* The phases are costs: the real clock, also on ysp/rt.h's virtual clock,
 * where a draw read with yrt_now_ns() takes 0 and a wait is the jump. */
#ifndef YSCR__COST_NOW
#define YSCR__COST_NOW() (yrt_real_ns())
#endif
/* Whether SDL text input is on for the screen's window now: another
 * library (a GUI) can start it, so the header reads it instead of trusting
 * its own calls. With no window, the state yscr_text_input() set. */
#ifndef YSCR__TEXT_INPUT_ACTIVE
#if !defined(YSCR_NO_SDL)
#define YSCR__TEXT_INPUT_ACTIVE(s) ((s)->window ? (SDL_TextInputActive((s)->window) ? 1 : 0) : (s)->text_input)
#else
#define YSCR__TEXT_INPUT_ACTIVE(s) ((s)->text_input)
#endif
#endif
static void yscr__text_input_set(yscr_screen* s, int on, int external, int x, int y, int w, int h);
/* Called once when the sync guard fires; a test counts the fires. */
#ifndef YSCR__ON_UNSYNCED
#define YSCR__ON_UNSYNCED(s) ((void)(s))
#endif

/* One YSCR_EV_DEVICE record per change of a device: present at open,
 * added, removed. The table makes a device that SDL reports twice (its
 * ADDED events for devices already there) one record; a full table still
 * logs. Pure: tests/adapt/screen_test.c calls it without SDL. */
static void yscr__device_log(yscr_screen* s, uint32_t kind, uint32_t change, uint64_t id, const char* name);
/* examples/screen/abort.c sets it to 1 to arm the panic watchdog in a
 * window, so its test needs no fullscreen. */
#ifndef YSCR__PANIC_WINDOWED
#define YSCR__PANIC_WINDOWED 0
#endif
static int64_t yscr__now(void) { return YSCR__NOW(); }

static void yscr__device_log(yscr_screen* s, uint32_t kind, uint32_t change, uint64_t id, const char* name) {
    yrt_event ev;
    int i, at = -1, free_i = -1;
    size_t n;
    for (i = 0; i < YSCR__MAX_DEVS; i++) {
        if (s->dev_kind[i] == kind && s->dev_id[i] == id) at = i;
        else if (!s->dev_kind[i] && free_i < 0) free_i = i;
    }
    if (change == YSCR_DEV_REMOVED) {
        if (at < 0) return;
        s->dev_kind[at] = 0;
        s->dev_id[at] = 0;
    } else {
        if (at >= 0) return;
        if (free_i >= 0) { s->dev_kind[free_i] = (uint8_t)kind; s->dev_id[free_i] = id; }
    }
    if (!s->ring) return;
    memset(&ev, 0, sizeof ev);
    ev.source = (uint16_t)YRT_SRC_SCREEN;
    ev.kind = (uint16_t)YSCR_EV_DEVICE;
    ev.t_ns = (uint64_t)yscr__now();
    ev.aux = s->display_index;
    ev.u.u32[0] = kind;
    ev.u.u32[1] = change;
    ev.u.u64[1] = id;
    n = name ? strlen(name) : 0;
    if (n > 23) n = 23;
    if (n) memcpy(ev.u.bytes + 16, name, n);
    yrt_ring_push(s->ring, &ev);
}


static void yscr__push_code(yscr_screen* s, const yscr__pend* p);
static void yscr__trig_flip_done(yscr_screen* s, const yscr__pend* p, int64_t shown);
static void yscr__trig_emit(yscr_screen* s);

/* The trigger lock: frame thread and trigger worker. Stored in the handle's
 * lock_mem, so the public header needs no OS type. */
#if defined(YRT_NO_THREADS)
static int  yscr__lock_init(yscr_screen* s) { (void)s; return 1; }
static void yscr__lock_free(yscr_screen* s) { (void)s; }
static void yscr__lock(yscr_screen* s) { (void)s; }
static void yscr__unlock(yscr_screen* s) { (void)s; }
#elif defined(_WIN32)
typedef char yscr__cs_fits[sizeof(CRITICAL_SECTION) <= sizeof(((yscr_screen*)0)->lock_mem) ? 1 : -1];
static int  yscr__lock_init(yscr_screen* s) { InitializeCriticalSection((CRITICAL_SECTION*)(void*)s->lock_mem); return 1; }
static void yscr__lock_free(yscr_screen* s) { DeleteCriticalSection((CRITICAL_SECTION*)(void*)s->lock_mem); }
/* A wait for the lock is a zone, so a trace shows who held whom up. */
static void yscr__lock(yscr_screen* s) {
    CRITICAL_SECTION* cs = (CRITICAL_SECTION*)(void*)s->lock_mem;
    if (!s->lock_ok || TryEnterCriticalSection(cs)) return;
    {
        YRT_ZONE(z_wait, "yscr.lockwait");
        EnterCriticalSection(cs);
        YRT_ZONE_END(z_wait);
    }
}
static void yscr__unlock(yscr_screen* s) { if (s->lock_ok) LeaveCriticalSection((CRITICAL_SECTION*)(void*)s->lock_mem); }
#else
typedef char yscr__mx_fits[sizeof(pthread_mutex_t) <= sizeof(((yscr_screen*)0)->lock_mem) ? 1 : -1];
static int  yscr__lock_init(yscr_screen* s) { return pthread_mutex_init((pthread_mutex_t*)(void*)s->lock_mem, NULL) == 0; }
static void yscr__lock_free(yscr_screen* s) { pthread_mutex_destroy((pthread_mutex_t*)(void*)s->lock_mem); }
static void yscr__lock(yscr_screen* s) {
    pthread_mutex_t* m = (pthread_mutex_t*)(void*)s->lock_mem;
    if (!s->lock_ok || pthread_mutex_trylock(m) == 0) return;
    {
        YRT_ZONE(z_wait, "yscr.lockwait");
        pthread_mutex_lock(m);
        YRT_ZONE_END(z_wait);
    }
}
static void yscr__unlock(yscr_screen* s) { if (s->lock_ok) pthread_mutex_unlock((pthread_mutex_t*)(void*)s->lock_mem); }
#endif

static void yscr__set_error(char* buf, size_t cap, const char* fmt, ...) {
    va_list ap;
    if (!buf || cap == 0) return;
    va_start(ap, fmt);
    vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
}

static void yscr__copy(char* dst, size_t cap, const char* src) {
    size_t n;
    if (!dst || cap == 0) return;
    if (!src) src = "";
    n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static uint32_t yscr__sat32(int64_t v) {
    if (v < 0) return 0;
    if (v > (int64_t)0xFFFFFFFEu) return 0xFFFFFFFEu;
    return (uint32_t)v;
}

/* --- abort (ABORT) ---------------------------------------------------------
 * One state per process: SDL's event watch (on the frame thread or on SDL's
 * raw-input thread), the panic watchdog's keyboard hook and
 * yscr_request_abort() write it from any thread, and each screen's begin()
 * reads it. */
#if defined(_WIN32)
static int32_t yscr__a_inc(volatile int32_t* p) { return (int32_t)InterlockedIncrement((volatile LONG*)p); }
static int32_t yscr__a_load(volatile int32_t* p) { return (int32_t)InterlockedCompareExchange((volatile LONG*)p, 0, 0); }
static void yscr__a_store(volatile int32_t* p, int32_t v) { (void)InterlockedExchange((volatile LONG*)p, (LONG)v); }
static int yscr__a_cas(volatile int32_t* p, int32_t want, int32_t v) {
    return InterlockedCompareExchange((volatile LONG*)p, (LONG)v, (LONG)want) == (LONG)want;
}
static int64_t yscr__a_load64(volatile int64_t* p) { return (int64_t)InterlockedCompareExchange64((volatile LONG64*)p, 0, 0); }
/* x86 MSVC has no InterlockedExchange64 (CI's 32-bit build, v0.10.4);
 * its 64-bit compare-exchange (cmpxchg8b) is the one 64-bit atomic there. */
static void yscr__a_store64(volatile int64_t* p, int64_t v) {
#if defined(_M_IX86) || defined(__i386__)
    LONG64 old = *(volatile LONG64*)p;
    for (;;) {
        LONG64 seen = InterlockedCompareExchange64((volatile LONG64*)p, (LONG64)v, old);
        if (seen == old) break;
        old = seen;
    }
#else
    (void)InterlockedExchange64((volatile LONG64*)p, (LONG64)v);
#endif
}
#else
static int32_t yscr__a_inc(volatile int32_t* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int32_t yscr__a_load(volatile int32_t* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void yscr__a_store(volatile int32_t* p, int32_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static int yscr__a_cas(volatile int32_t* p, int32_t want, int32_t v) {
    return __atomic_compare_exchange_n(p, &want, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static int64_t yscr__a_load64(volatile int64_t* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void yscr__a_store64(volatile int64_t* p, int64_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
#endif

/* yscr__abort_entry.flags: who saw it */
#define YSCR__AB_HOOK     0x1u   /* the panic watchdog's keyboard hook         */
#define YSCR__AB_INJECTED 0x2u   /* the key came from SendInput or the like    */
#define YSCR__AB_SDL      0x4u   /* SDL's event watch                          */
#define YSCR__ABORT_LOG   16
/* Reports of one key press: the hook sees it as Windows reads it, SDL's
 * raw path 0.1 to 0.7 ms later, and SDL's message path, which with the raw
 * keyboard on reports a key with no scan code a second time (INPUT): up to
 * 17.1 ms later in 517 of 518 virtual-key taps, 34.2 ms in one. Remote
 * desktop and assistive tools inject by virtual key, so at 30 ms two
 * presses through one could count as four and meet the panic rule. A
 * person cannot press twice within 50 ms. */
#define YSCR__AB_SAME_NS  50000000

typedef struct yscr__abort_entry {
    volatile int32_t seq;      /* the entry's number, written last; 0 while written */
    uint32_t reason, flags, key, mods;
    int64_t  t;                /* RT ns                                          */
} yscr__abort_entry;

static struct {
    yscr__abort_entry log[YSCR__ABORT_LOG];
    volatile int32_t seq;      /* the last entry's number                        */
    volatile int32_t lock;     /* writers of last_t and press[]                  */
    volatile int64_t ack;      /* when a begin() last reported an abort          */
    volatile int64_t sdl_off;  /* RT ns minus SDL ticks                          */
    int32_t  users;            /* open screens                                   */
    int32_t  off;
    uint32_t key, mods;        /* the combination; mods 0 = the key alone        */
    int64_t  last_t;           /* the last press counted                         */
    int64_t  press[8];         /* the last presses, for the panic rule           */
    int32_t  n_press;
    int32_t  sdl_held;         /* the abort key, as SDL's events say          */
    int32_t  presses;          /* the panic rule; 0 = no watchdog armed          */
    int64_t  window_ns, grace_ns;
} yscr__ab;

/* The panic released the display (PANIC, v0.10.0): every later begin() of a
 * screen whose window the window thread owns returns YSCR_QUIT with
 * YSCR_ABORT_PANIC, until the last such screen closes. */
static volatile int32_t yscr__released;
/* The panic released the display of this screen: begin() returns
 * YSCR_QUIT from now on. */
static int yscr__released_on(const yscr_screen* s) { return s->wt && yscr__a_load(&yscr__released); }

/* With the window thread, SDL keeps the string of a text, IME, drop or
 * clipboard event in the memory of the thread that takes the event, and
 * frees it at that thread's next pump (SDL_FreeTemporaryMemory, SDL 3.4.0's
 * SDL_events.c). The frame thread never pumps, so yscr_poll() notes such
 * an event and the next begin() or close() frees the thread's SDL memory. */
static int yscr__tls_dirty;
/* Test seam: 0 keeps the strings, to measure what they cost
 * (examples/screen/window_thread.c --text). */
#ifndef YSCR__TLS_CLEANUP
#define YSCR__TLS_CLEANUP 1
#endif

/* The window thread's mode (WINDOW THREAD): one per process while screens
 * with a window are open, because SDL has one main thread. */
#define YSCR__WT_WHY_NONE     0   /* AUTO chose on                              */
#define YSCR__WT_WHY_NAMED    1   /* desc.window_thread ON or OFF               */
#define YSCR__WT_WHY_BACKEND  2   /* AUTO: not DXGI_FLIP or COMPOSITION         */
#define YSCR__WT_WHY_PLATFORM 3   /* AUTO: not Windows, or YRT_NO_THREADS       */
#define YSCR__WT_WHY_PROCESS  4   /* AUTO: the mode of the screens already open */
#define YSCR__WT_WHY_SDL      5   /* AUTO: SDL video was up on another thread   */
static int32_t yscr__wt_mode_now;    /* YSCR_WINDOW_THREAD_ON or _OFF; 0 = none open */
static int32_t yscr__wt_mode_users;  /* open screens with a window                  */

/* The mode for a screen with a window: YSCR_WINDOW_THREAD_ON or _OFF and
 * *why, or 0 with *err when open() must fail. want is desc.window_thread;
 * native: DXGI_FLIP or COMPOSITION; available: the build has the window
 * thread; process: the open screens' mode (0 none); sdl_elsewhere: SDL's
 * video is up and no screen holds it, so another thread is SDL's main
 * thread. Pure: tests/adapt/screen_test.c calls it. */
static int32_t yscr__wt_decide(int32_t want, int native, int available, int32_t process, int sdl_elsewhere,
                               int32_t* why, const char** err) {
    int32_t natural, nwhy;
    *err = NULL;
    *why = YSCR__WT_WHY_NAMED;
    if (want == YSCR_WINDOW_THREAD_ON) {
        if (!available) *err = "desc.window_thread ON needs Windows and threads (not the web, not YRT_NO_THREADS)";
        else if (process == YSCR_WINDOW_THREAD_OFF) *err = "desc.window_thread differs from another open screen's: one mode per process";
        else if (sdl_elsewhere) *err = "desc.window_thread ON: SDL's video is up on another thread, which SDL made its main thread";
        return *err ? 0 : YSCR_WINDOW_THREAD_ON;
    }
    if (want == YSCR_WINDOW_THREAD_OFF) {
        if (process == YSCR_WINDOW_THREAD_ON) *err = "desc.window_thread differs from another open screen's: one mode per process";
        return *err ? 0 : YSCR_WINDOW_THREAD_OFF;
    }
    natural = YSCR_WINDOW_THREAD_ON;
    nwhy = YSCR__WT_WHY_NONE;
    if (!available) { natural = YSCR_WINDOW_THREAD_OFF; nwhy = YSCR__WT_WHY_PLATFORM; }
    else if (!native) { natural = YSCR_WINDOW_THREAD_OFF; nwhy = YSCR__WT_WHY_BACKEND; }
    else if (sdl_elsewhere) { natural = YSCR_WINDOW_THREAD_OFF; nwhy = YSCR__WT_WHY_SDL; }
    *why = nwhy;
    if (process == 0 || process == natural) return natural;
    if (process == YSCR_WINDOW_THREAD_OFF) { *why = YSCR__WT_WHY_PROCESS; return YSCR_WINDOW_THREAD_OFF; }
    /* the window thread runs, and AUTO would leave this window off it */
    *err = nwhy == YSCR__WT_WHY_BACKEND
         ? "desc.window_thread AUTO is off for this backend, and another open screen has the window thread: "
           "set desc.window_thread ON (call SDL on the window only through yscr_window_call) or close that screen"
         : "desc.window_thread AUTO cannot join the window thread of the open screens";
    return 0;
}

static int yscr__win_call(yscr_screen* s, yscr_window_fn fn, void* ctx);
#if defined(YSCR__WT)
static int yscr__wt_running(void);
static int yscr__wt_call(yscr_window_fn fn, void* ctx);
#else
/* No window thread in this build: everything runs on the caller's thread. */
static int yscr__wt_running(void) { return 0; }
static int yscr__wt_call(yscr_window_fn fn, void* ctx) { fn(ctx); return YSCR_OK; }
#endif


static int32_t yscr__abort_push(uint32_t reason, uint32_t flags, uint32_t key, uint32_t mods, int64_t t) {
    int32_t n = yscr__a_inc(&yscr__ab.seq);
    yscr__abort_entry* e = &yscr__ab.log[(uint32_t)n % YSCR__ABORT_LOG];
    yscr__a_store(&e->seq, 0);
    e->reason = reason;
    e->flags = flags;
    e->key = key;
    e->mods = mods;
    e->t = t;
    yscr__a_store(&e->seq, n);
    return n;
}

/* The panic rule on a press at t: enough presses within the window, and no
 * begin() has reported an abort for the grace time, so the frame loop looks
 * hung. A loop that reported press 1 is saving, and must not be killed. */
static int yscr__panic_due(int64_t t) {
    int i, n = 0;
    int64_t ack = yscr__a_load64(&yscr__ab.ack);
    yscr__ab.press[(uint32_t)yscr__ab.n_press++ % 8u] = t;
    if (yscr__ab.presses <= 0) return 0;
    for (i = 0; i < 8; i++)
        if (yscr__ab.press[i] && yscr__ab.press[i] <= t && t - yscr__ab.press[i] < yscr__ab.window_ns) n++;
    return n >= yscr__ab.presses && (ack == 0 || t - ack > yscr__ab.grace_ns);
}

/* A press is a key-down after an up: Windows repeats key-downs while a key
 * is held, and a held combination must not count as several presses. */
/* --- raw mice: the queue, the decoder, the guard (pure; INPUT, "Raw mice") ------- */

/* The input bridge's store (INPUT, "The input bridge"). Many producers,
 * one decoder: a record goes in slot seq % size, and its doorbell carries
 * seq, so a doorbell finds its own record or learns it is gone; a record
 * whose doorbell nobody decodes is overwritten when the store wraps,
 * instead of holding its slot forever. */
#ifndef YSCR_INPUT_STORE
#define YSCR_INPUT_STORE 4096   /* records, a power of two */
#endif
#define YSCR__IN_MASK ((uint32_t)YSCR_INPUT_STORE - 1u)
#define YSCR__IN_SEQ  0x7FFFFFFFu   /* sequences stay positive: -1 marks a write */
typedef struct yscr__in_slot {
    volatile int32_t seq;     /* the record's sequence, written last; -1 while written */
    volatile int32_t state;   /* 0 empty, 1 stored, 2 decoded                 */
    volatile int32_t bell;    /* 1: SDL refused its doorbell                   */
    int32_t          origin;  /* 1: desc.raw_mice's reader                     */
    yin_event      e;
} yscr__in_slot;
/* One key of the second-report filter (INPUT, "Second key reports"). */
#define YSCR__KEYS          16
#define YSCR__KEY_DEDUP_NS  50000000
typedef struct yscr__key {
    int64_t  t;               /* the kept key-down's time                      */
    uint32_t device, control;
    uint8_t  used, down;      /* down: the kept key-down's key-up not seen yet */
    uint8_t  dropped;         /* dropped key-downs whose key-up is to drop     */
    uint8_t  reserved_;
} yscr__key;
static struct {
    volatile int32_t next;    /* the next sequence, unmasked                   */
    volatile int32_t overwritten, refused, pending, lost;
    volatile int32_t key_doubles, key_double_ups;
    uint32_t type;            /* the doorbell's SDL type; 0 = no SDL yet       */
    uint32_t mouse_next;      /* yscr_poll_mouse()'s scan; the frame thread  */
    uint32_t logged[4];       /* the counters the last record showed           */
    yscr__key key[YSCR__KEYS];/* the key filter's table; the frame thread      */
    uint32_t key_next;        /* the slot the next new key takes               */
    yscr__in_slot slot[YSCR_INPUT_STORE];
} yscr__in;

static void yscr__a_dec(volatile int32_t* p) {
    int32_t v;
    do { v = yscr__a_load(p); } while (!yscr__a_cas(p, v, v - 1));
}

/* Any thread. Returns the record's sequence. */
static int32_t yscr__in_store(const yin_event* e, int32_t origin) {
    uint32_t raw = (uint32_t)yscr__a_inc(&yscr__in.next) - 1u;
    int32_t seq = (int32_t)(raw & YSCR__IN_SEQ);
    yscr__in_slot* sl = &yscr__in.slot[raw & YSCR__IN_MASK];
    if (yscr__a_load(&sl->state) == 1) yscr__a_inc(&yscr__in.overwritten);
    if (yscr__a_cas(&sl->bell, 1, 0)) yscr__a_dec(&yscr__in.pending);
    yscr__a_store(&sl->seq, -1);
    sl->e = *e;
    sl->origin = origin;
    yscr__a_store(&sl->state, 1);
    yscr__a_store(&sl->seq, seq);
    return seq;
}

/* Test-only seam, not API: tests/adapt/screen_test.c rings its own
 * doorbells. 1 when SDL took the doorbell. */
#ifndef YSCR__DOORBELL
#if !defined(YSCR_NO_SDL)
static int yscr__doorbell(int32_t seq) {
    SDL_Event ev;
    if (!yscr__in.type) return 0;
    memset(&ev, 0, sizeof ev);
    ev.type = yscr__in.type;
    ev.user.code = seq;
    return SDL_PushEvent(&ev) ? 1 : 0;
}
#define YSCR__DOORBELL(seq) yscr__doorbell(seq)
#else
#define YSCR__DOORBELL(seq) ((void)(seq), 0)
#endif
#endif

static int yscr__in_push(const yin_event* e, int32_t origin) {
    int32_t seq = yscr__in_store(e, origin);
    if (!YSCR__DOORBELL(seq)) {
        yscr__in_slot* sl = &yscr__in.slot[(uint32_t)seq & YSCR__IN_MASK];
        yscr__a_inc(&yscr__in.refused);
        if (yscr__a_load(&sl->seq) == seq && yscr__a_cas(&sl->bell, 0, 1)) yscr__a_inc(&yscr__in.pending);
    }
    return 0;
}

/* The frame thread: the record of doorbell seq. 1 found (and marked
 * decoded), 0 gone (overwritten), -1 decoded already. */
static int yscr__in_take(int32_t seq, yin_event* out, int32_t* origin) {
    yscr__in_slot* sl;
    yin_event copy;
    int32_t o;
    if (seq < 0) return 0;
    sl = &yscr__in.slot[(uint32_t)seq & YSCR__IN_MASK];
    if (yscr__a_load(&sl->seq) != seq) return 0;
    copy = sl->e;
    o = sl->origin;
    if (yscr__a_load(&sl->seq) != seq) return 0;           /* torn: rewritten meanwhile */
    if (!yscr__a_cas(&sl->state, 1, 2)) return yscr__a_load(&sl->seq) == seq ? -1 : 0;
    if (yscr__a_load(&sl->seq) != seq) return 0;
    if (yscr__a_cas(&sl->bell, 1, 0)) yscr__a_dec(&yscr__in.pending);
    *out = copy;
    if (origin) *origin = o;
    return 1;
}

/* The frame thread, when SDL's queue is empty: push the doorbells SDL
 * refused again, oldest first. Returns how many SDL took. */
static int yscr__in_rebell(void) {
    uint32_t raw, i, start;
    int pushed = 0, waiting = 0;
    if (yscr__a_load(&yscr__in.pending) <= 0) return 0;
    raw = (uint32_t)yscr__a_load(&yscr__in.next);
    start = raw > YSCR_INPUT_STORE ? raw - YSCR_INPUT_STORE : 0;
    for (i = start; i != raw; i++) {
        yscr__in_slot* sl = &yscr__in.slot[i & YSCR__IN_MASK];
        int32_t seq = (int32_t)(i & YSCR__IN_SEQ);
        if (!yscr__a_load(&sl->bell) || yscr__a_load(&sl->state) != 1 || yscr__a_load(&sl->seq) != seq) continue;
        if (!YSCR__DOORBELL(seq)) { waiting = 1; break; }
        if (yscr__a_cas(&sl->bell, 1, 0)) yscr__a_dec(&yscr__in.pending);
        pushed++;
    }
    if (!pushed && !waiting) yscr__a_store(&yscr__in.pending, 0);   /* nothing left */
    return pushed;
}

/* A mouse SDL does not list: say so on every event, log it the first time. */
static int yscr__unlisted(yscr_screen* s, uint32_t device, int64_t t) {
    int i, free_i = -1;
    if (!s || !device) return 0;
    for (i = 0; i < YSCR__MAX_DEVS; i++)
        if (s->dev_kind[i] == YSCR_DEV_MOUSE && s->dev_id[i] == device) return 0;
    for (i = 0; i < 16; i++) {
        if (s->rm_unlisted[i] == device) return 1;
        if (!s->rm_unlisted[i] && free_i < 0) free_i = i;
    }
    if (free_i >= 0) s->rm_unlisted[free_i] = device;
    if (s->ring) {
        yrt_event ev;
        memset(&ev, 0, sizeof ev);
        ev.source = (uint16_t)YRT_SRC_SCREEN;
        ev.kind = (uint16_t)YSCR_EV_DEVICE;
        ev.t_ns = (uint64_t)t;
        ev.aux = s->display_index;
        ev.u.u32[0] = YSCR_DEV_MOUSE;
        ev.u.u32[1] = YSCR_DEV_ADDED;
        ev.u.u64[1] = device;
        memcpy(ev.u.bytes + 16, "raw, not in SDL's list", 22);
        yrt_ring_push(s->ring, &ev);
    }
    return 1;
}

/* A decoded doorbell: the record, with the unlisted flag on a raw mouse. */
static int yscr__in_decode(yscr_screen* s, int32_t seq, yin_event* out) {
    int r = yscr__in_take(seq, out, NULL);
    if (r == 0) yscr__a_inc(&yscr__in.lost);
    if (r <= 0) return 0;
    if (out->kind == YIN_KIND_MOUSE && yscr__unlisted(s, out->device, out->t)) out->flags |= YIN_UNLISTED;
    return 1;
}

/* Two reports can be of one press when their devices are equal or one is
 * 0: SDL gives the raw path the keyboard's handle, the message path and
 * injected input 0. Two keyboards are two presses. */
static int yscr__key_dev(uint32_t a, uint32_t b) { return a == b || a == 0 || b == 0; }

/* The second-report filter (INPUT, "Second key reports"): 1 keeps e, 0
 * drops it (counted, and logged on s's ring). Keyboard presses and
 * releases only. The frame thread. */
static int yscr__key_keep(yscr_screen* s, const yin_event* e) {
    int64_t win = s && s->key_dedup_ns ? s->key_dedup_ns : YSCR__KEY_DEDUP_NS;
    yscr__key* k;
    int i;
    if (e->kind != YIN_KIND_KEYBOARD || win <= 0) return 1;
    if (e->type == YIN_RELEASE) {
        yscr__key* any = NULL;
        /* A device-0 key-up cannot be the key-up of a press kept with a
         * keyboard's id, so where such a press has a dropped report
         * pending, it is that report's. Else it could take another
         * keyboard's held key, and that keyboard's own key-up would pass
         * too (found by examples/response/trial_two_keyboards.c, v0.4.2). */
        if (e->device == 0)
            for (i = 0; i < YSCR__KEYS; i++) {
                k = &yscr__in.key[i];
                if (!k->used || !k->dropped || !k->device || k->control != e->control) continue;
                k->dropped--;
                yscr__a_inc(&yscr__in.key_double_ups);
                return 0;
            }
        /* the kept press's key-up first, whichever report it came from;
         * the same device before device 0, so table order cannot pair a
         * key-up with another keyboard's press */
        for (i = 0; i < YSCR__KEYS; i++) {
            k = &yscr__in.key[i];
            if (!k->used || !k->down || k->control != e->control || !yscr__key_dev(k->device, e->device)) continue;
            if (k->device == e->device) { any = k; break; }
            if (!any) any = k;
        }
        if (any) {
            any->down = 0;
            return 1;
        }
        for (i = 0; i < YSCR__KEYS; i++) {
            k = &yscr__in.key[i];
            if (!k->used || !k->dropped || k->control != e->control || !yscr__key_dev(k->device, e->device)) continue;
            k->dropped--;
            yscr__a_inc(&yscr__in.key_double_ups);
            return 0;
        }
        return 1;
    }
    if (e->type != YIN_PRESS || (e->flags & YIN_REPEAT)) return 1;
    for (i = 0; i < YSCR__KEYS; i++) {
        int64_t dt;
        k = &yscr__in.key[i];
        if (!k->used || k->control != e->control || !yscr__key_dev(k->device, e->device)) continue;
        dt = e->t - k->t;
        if (dt == 0 && k->device == e->device) return 1;      /* the kept report again */
        if (dt >= win || dt <= -win) continue;
        if (k->dropped < 255) k->dropped++;
        yscr__a_inc(&yscr__in.key_doubles);
        if (s && s->ring) {
            yrt_event ev;
            memset(&ev, 0, sizeof ev);
            ev.source = (uint16_t)YRT_SRC_SCREEN;
            ev.kind = (uint16_t)YSCR_EV_KEY_DOUBLE;
            ev.t_ns = (uint64_t)e->t;
            ev.aux = s->display_index;
            ev.u.i64[0] = dt;
            ev.u.u32[2] = e->control;
            ev.u.u32[3] = e->device;
            ev.u.u32[4] = k->device;
            yrt_ring_push(s->ring, &ev);
        }
        return 0;
    }
    /* kept: this keyboard's slot for the key, else the oldest */
    k = NULL;
    for (i = 0; i < YSCR__KEYS && !k; i++)
        if (yscr__in.key[i].used && yscr__in.key[i].control == e->control && yscr__in.key[i].device == e->device)
            k = &yscr__in.key[i];
    if (!k) {
        k = &yscr__in.key[yscr__in.key_next];
        yscr__in.key_next = (yscr__in.key_next + 1u) % YSCR__KEYS;
    }
    k->t = e->t;
    k->device = e->device;
    k->control = e->control;
    k->used = k->down = 1;
    k->dropped = 0;
    return 1;
}

/* At begin(): one YSCR_EV_INPUT_LOST record when a counter moved. */
static void yscr__in_log(yscr_screen* s) {
    uint32_t v[4];
    yrt_event ev;
    v[0] = (uint32_t)yscr__a_load(&yscr__in.overwritten);
    v[1] = (uint32_t)yscr__a_load(&yscr__in.refused);
    v[2] = (uint32_t)yscr__a_load(&yscr__in.lost);
    v[3] = (uint32_t)yscr__a_load(&yscr__in.pending);
    if (v[0] == yscr__in.logged[0] && v[1] == yscr__in.logged[1] && v[2] == yscr__in.logged[2]) {
        yscr__in.logged[3] = v[3];   /* only the waiting count fell */
        return;
    }
    memcpy(yscr__in.logged, v, sizeof v);
    if (!s->ring) return;
    memset(&ev, 0, sizeof ev);
    ev.source = (uint16_t)YRT_SRC_SCREEN;
    ev.kind = (uint16_t)YSCR_EV_INPUT_LOST;
    ev.t_ns = (uint64_t)YSCR__NOW();
    ev.aux = s->display_index;
    memcpy(&ev.u.u32[0], v, sizeof v);
    yrt_ring_push(s->ring, &ev);
}

/* RAWMOUSE's flags and button flags, as winuser.h defines them; repeated
 * here so the decoder and its test need no Windows header. */
#define YSCR__RI_MOVE_ABSOLUTE   0x0001u
#define YSCR__RI_VIRTUAL_DESKTOP 0x0002u
#define YSCR__RI_WHEEL           0x0400u
#define YSCR__RI_HWHEEL          0x0800u

/* One RAWMOUSE (usFlags, usButtonFlags, usButtonData, lLastX, lLastY). */
static void yscr__mouse_decode(uint16_t flags, uint16_t bflags, uint16_t bdata, int32_t lx, int32_t ly,
                                 uint32_t device, int64_t t, yscr_mouse_event* e) {
    /* down, up bit pairs: left, right, middle, button 4, button 5 */
    static const uint8_t button[5] = { YSCR_MOUSE_LEFT, YSCR_MOUSE_RIGHT, YSCR_MOUSE_MIDDLE,
                                       YSCR_MOUSE_X1, YSCR_MOUSE_X2 };
    int i;
    memset(e, 0, sizeof *e);
    e->t = t;
    e->device = device;
    e->dx = lx;
    e->dy = ly;
    for (i = 0; i < 5; i++) {
        if (bflags & (1u << (2 * i))) e->down |= button[i];
        if (bflags & (2u << (2 * i))) e->up |= button[i];
    }
    if (bflags & YSCR__RI_WHEEL) e->wheel = (int16_t)bdata;
    if (bflags & YSCR__RI_HWHEEL) e->hwheel = (int16_t)bdata;
    if (flags & YSCR__RI_MOVE_ABSOLUTE) e->flags |= YSCR_MOUSE_ABSOLUTE;
    if (flags & YSCR__RI_VIRTUAL_DESKTOP) e->flags |= YSCR_MOUSE_VIRTUAL_DESKTOP;
}

/* The registration guard, once a frame: SDL's relative mode takes the
 * mouse registration while it is on, and removes it for the process when
 * it ends (measured). Returns 0, YSCR_RAW_MICE_SUSPENDED when relative
 * mode took the mice, or YSCR_RAW_MICE_REGISTERED when the reader must
 * register again. *suspended is the screen's state. */
static int yscr__rm_guard(int32_t* suspended, int relative, int registered) {
    if (relative) {
        if (*suspended) return 0;
        *suspended = 1;
        return (int)YSCR_RAW_MICE_SUSPENDED;
    }
    *suspended = 0;
    return registered ? 0 : (int)YSCR_RAW_MICE_REGISTERED;
}

static void yscr__rm_log(yscr_screen* s, uint32_t what) {
    yrt_event ev;
    if (!s->ring) return;
    memset(&ev, 0, sizeof ev);
    ev.source = (uint16_t)YRT_SRC_SCREEN;
    ev.kind = (uint16_t)YSCR_EV_RAW_MICE;
    ev.t_ns = (uint64_t)YSCR__NOW();
    ev.aux = s->display_index;
    ev.u.u32[0] = what;
    ev.u.u32[1] = (uint32_t)yscr__a_load(&yscr__in.overwritten);
    yrt_ring_push(s->ring, &ev);
}

static int yscr__abort_edge(int32_t* held, int down) {
    int press = down && !*held;
    *held = down ? 1 : 0;
    return press;
}

/* A key-down that is not a repeat, at RT time t, with mods (YSCR_MOD_*)
 * held, from the source in flags. Returns 1 when the panic rule is met. */
static int yscr__abort_key(uint32_t key, uint32_t mods, int64_t t, uint32_t flags) {
    int panic = 0;
    if (yscr__ab.users <= 0 || yscr__ab.off || key != yscr__ab.key) return 0;
    if ((mods & yscr__ab.mods) != yscr__ab.mods) return 0;
    while (!yscr__a_cas(&yscr__ab.lock, 0, 1)) { }
    if (!(yscr__ab.last_t && t - yscr__ab.last_t < YSCR__AB_SAME_NS && yscr__ab.last_t - t < YSCR__AB_SAME_NS)) {
        yscr__ab.last_t = t;
        yscr__abort_push(YSCR_ABORT_KEY, flags, key, mods, t);
        panic = yscr__panic_due(t);
    }
    yscr__a_store(&yscr__ab.lock, 0);
    return panic;
}

/* The panic rule's numbers (desc.panic_*); presses 0 turns it off. */
static void yscr__panic_config(int32_t presses, int64_t window_ns, int64_t grace_ns) {
    yscr__ab.presses = presses;
    yscr__ab.window_ns = window_ns;
    yscr__ab.grace_ns = grace_ns;
    memset(yscr__ab.press, 0, sizeof yscr__ab.press);
}

static const char* yscr__abort_open(yscr_screen* s, const yscr_desc* d) {
    uint32_t key = d->abort_keys.key ? d->abort_keys.key : YSCR_KEY_ESCAPE;
    uint32_t m = d->abort_keys.mods;
    int32_t off = d->abort_keys.off ? 1 : 0;
    if (m & ~(YSCR_MOD_SHIFT | YSCR_MOD_CTRL | YSCR_MOD_ALT | YSCR_MOD_GUI | YSCR_MOD_NONE))
        return "desc.abort_keys.mods has bits that are not YSCR_MOD_*";
    m = m == 0 ? YSCR_MOD_SHIFT : (m & YSCR_MOD_NONE) ? 0u : m;
    if (yscr__ab.users > 0 && (yscr__ab.key != key || yscr__ab.mods != m || yscr__ab.off != off))
        return "desc.abort_keys differs from another open screen's: there is one combination per process";
    yscr__ab.key = key;
    yscr__ab.mods = m;
    yscr__ab.off = off;
    yscr__ab.users++;
    s->abort_user = 1;
    s->abort_seen = yscr__a_load(&yscr__ab.seq);   /* not the aborts of before */
    return NULL;
}

/* Keys the panic watchdog's hook can name from an SDL keycode: Esc, F1 to
 * F12 (SDLK_F1 is 0x4000003A), a letter, a digit. */
static int yscr__vk_known(uint32_t key) {
    return key == YSCR_KEY_ESCAPE || (key >= 0x4000003Au && key <= 0x40000045u) ||
           (key >= 'a' && key <= 'z') || (key >= '0' && key <= '9');
}

static void yscr__abort_close(yscr_screen* s) {
    if (s->abort_user && --yscr__ab.users == 0) yscr__panic_config(0, 0, 0);
    s->abort_user = 0;
}

static int yscr__abort_pending(const yscr_screen* s) {
    return yscr__a_load(&yscr__ab.seq) != s->abort_seen;
}

/* Reports the log entries after the screen's last one: f gets the reasons,
 * the ring one YSCR_EV_ABORT record each. Returns 1 if there were any. */
static int yscr__abort_take(yscr_screen* s, yscr_frame* f) {
    int32_t cur = yscr__a_load(&yscr__ab.seq), k;
    uint32_t mask = 0;
    int32_t presses = 0;
    int64_t first = 0, now;
    if (cur == s->abort_seen) return 0;
    now = yscr__now();
    for (k = s->abort_seen + 1; k - cur <= 0; k++) {
        yscr__abort_entry* e = &yscr__ab.log[(uint32_t)k % YSCR__ABORT_LOG];
        yscr__abort_entry c;
        int32_t s1 = yscr__a_load(&e->seq);
        if (s1 - k < 0) break;              /* still being written: the next begin() */
        c.reason = e->reason; c.flags = e->flags; c.key = e->key; c.mods = e->mods; c.t = e->t;
        if (s1 != k || yscr__a_load(&e->seq) != k) continue;   /* written over: lost */
        mask |= c.reason;
        if (c.reason & YSCR_ABORT_KEY) presses++;
        if (!first) first = c.t ? c.t : now;
        if (s->ring) {
            yrt_event ev;
            memset(&ev, 0, sizeof ev);
            ev.source = (uint16_t)YRT_SRC_SCREEN;
            ev.kind = (uint16_t)YSCR_EV_ABORT;
            ev.t_ns = (uint64_t)(c.t ? c.t : now);
            ev.aux = s->display_index;
            ev.u.u32[0] = c.reason;
            ev.u.u32[1] = c.flags;
            ev.u.u32[2] = c.key;
            ev.u.u32[3] = c.mods;
            ev.u.i64[2] = now;
            yrt_ring_push(s->ring, &ev);
        }
    }
    s->abort_seen = k - 1;
    if (!mask) return 0;
    yscr__a_store64(&yscr__ab.ack, now);
    memset(f, 0, sizeof *f);
    f->abort = mask;
    f->abort_presses = presses;
    f->abort_ns = first;
    return 1;
}

/* Open screens with a window, for the close request's window id and for
 * the panic watchdog. Written by open() and close(), read on other threads
 * only to compare ids and, in a panic, to find rings and switched modes. */
#define YSCR__SLOTS 8
static struct { yscr_screen* s; uint32_t win; } yscr__slot[YSCR__SLOTS];

static void yscr__slot_take(yscr_screen* s, uint32_t win) {
    int i;
    for (i = 0; i < YSCR__SLOTS; i++)
        if (!yscr__slot[i].s) { yscr__slot[i].win = win; yscr__slot[i].s = s; s->abort_slot = i + 1; return; }
}

static void yscr__slot_free(yscr_screen* s) {
    if (s->abort_slot) { yscr__slot[s->abort_slot - 1].s = NULL; yscr__slot[s->abort_slot - 1].win = 0; }
    s->abort_slot = 0;
}

YSCR_API void yscr_request_abort(void) {
    yscr__abort_push(YSCR_ABORT_REQUEST, 0, 0, 0, yscr__now());
}

/* --- the default window icon (WINDOW ICON) ---------------------------------
 * Escher's impossible cube, drawn for this header: public domain like the
 * rest. Each byte is a run, (length - 1) << 4 | palette index; index 0 is
 * transparent. */
static const uint8_t yscr__icon_pal[5][4] = {
    {0,0,0,0}, {38,42,56,255}, {232,234,240,255}, {150,162,186,255}, {84,96,126,255}
};
static const uint8_t yscr__icon_rle[1292] = {
    0x40,0x03,0x01,0x13,0x90,0x23,0x11,0x43,0x40,0x13,0x01,0x03,0x01,0x00,0x41,0x13,0x01,0x00,0x13,0x01,0x00,0x03,0x01,0x50,
    0x03,0x11,0x00,0x23,0x00,0x03,0x01,0x30,0x13,0x01,0x03,0x01,0x00,0x21,0x00,0x03,0x01,0x00,0x33,0x01,0x00,0x03,0x01,0x00,
    0x03,0x01,0x10,0x03,0x01,0x00,0x01,0x13,0x01,0x10,0x03,0x01,0x00,0x03,0x01,0x10,0x03,0x01,0x10,0x01,0x03,0x01,0x10,0x03,
    0x01,0x00,0x03,0x01,0x10,0x03,0x01,0x20,0x03,0x01,0x10,0x03,0x01,0x00,0x03,0x01,0x10,0x03,0x01,0x20,0x03,0x01,0x10,0x03,
    0x01,0x00,0x03,0x01,0x00,0x43,0x00,0x03,0x01,0x10,0x03,0x01,0x00,0x03,0x01,0x03,0x41,0x00,0x03,0x01,0x00,0x13,0x01,0x00,
    0x03,0x11,0x50,0x03,0x01,0x00,0x13,0x10,0x13,0x60,0x03,0x01,0x13,0x01,0x10,0x11,0x43,0x10,0x03,0x01,0x03,0x01,0x40,0x31,
    0x33,0x11,0x20,0xa0,0x21,0xf0,0xb0,0x01,0x02,0x03,0x51,0xf0,0x40,0x21,0x02,0x03,0x01,0x42,0x41,0xe0,0x01,0x12,0x01,0x02,
    0x03,0x01,0x43,0x42,0x41,0x80,0x01,0x02,0x13,0x01,0x02,0x03,0x41,0x53,0x32,0x11,0x60,0x01,0x02,0x13,0x11,0x02,0x03,0x01,
    0x30,0x41,0x33,0x01,0x12,0x03,0x01,0x20,0x11,0x02,0x13,0x01,0x00,0x01,0x02,0x03,0x01,0x80,0x31,0x02,0x23,0x01,0x10,0x11,
    0x02,0x03,0x11,0x10,0x01,0x02,0x03,0x01,0x90,0x01,0x12,0x03,0x11,0x03,0x01,0x00,0x01,0x12,0x41,0x00,0x01,0x02,0x03,0x01,
    0x80,0x01,0x02,0x13,0x11,0x02,0x03,0x01,0x00,0x01,0x13,0x42,0x00,0x01,0x02,0x03,0x01,0x70,0x01,0x02,0x13,0x01,0x00,0x01,
    0x02,0x03,0x01,0x00,0x21,0x43,0x00,0x01,0x02,0x03,0x01,0x00,0x31,0x00,0x11,0x02,0x13,0x01,0x10,0x01,0x02,0x03,0x01,0x00,
    0x01,0x02,0x03,0x41,0x00,0x01,0x02,0x03,0x01,0x00,0x32,0x11,0x02,0x03,0x11,0x20,0x01,0x02,0x03,0x01,0x00,0x01,0x02,0x03,
    0x01,0x40,0x01,0x02,0x03,0x01,0x00,0x33,0x01,0x02,0x03,0x01,0x40,0x01,0x02,0x03,0x01,0x00,0x01,0x02,0x03,0x01,0x40,0x01,
    0x02,0x03,0x01,0x00,0x21,0x03,0x01,0x02,0x03,0x01,0x40,0x01,0x02,0x03,0x01,0x00,0x01,0x02,0x03,0x01,0x40,0x01,0x02,0x03,
    0x01,0x30,0x11,0x02,0x03,0x01,0x40,0x01,0x02,0x03,0x01,0x00,0x01,0x02,0x03,0x01,0x40,0x01,0x02,0x03,0x01,0x40,0x01,0x02,
    0x03,0x01,0x40,0x01,0x02,0x03,0x01,0x00,0x01,0x02,0x03,0x01,0x40,0x01,0x02,0x03,0x01,0x40,0x01,0x02,0x03,0x01,0x40,0x01,
    0x02,0x03,0x01,0x00,0x01,0x02,0x03,0x01,0x40,0x01,0x02,0x03,0x01,0x40,0x01,0x02,0x03,0x01,0x40,0x01,0x02,0x03,0x01,0x00,
    0x01,0x02,0x03,0x01,0x40,0x01,0x02,0x03,0x01,0x40,0x01,0x02,0x03,0x01,0x40,0x01,0x02,0x03,0x01,0x00,0x01,0x02,0x03,0x01,
    0x40,0x01,0x02,0x03,0x41,0x00,0x01,0x02,0x03,0x01,0x40,0x01,0x02,0x03,0x01,0x00,0x01,0x02,0x03,0x01,0x30,0x01,0x02,0x23,
    0x32,0x00,0x01,0x02,0x03,0x01,0x40,0x01,0x02,0x03,0x01,0x00,0x01,0x02,0x03,0x01,0x10,0x11,0x02,0x13,0x01,0x43,0x00,0x01,
    0x02,0x03,0x01,0x00,0x41,0x02,0x03,0x01,0x00,0x01,0x02,0x03,0x01,0x00,0x01,0x12,0x03,0x11,0x00,0x41,0x00,0x01,0x02,0x03,
    0x01,0x00,0x32,0x11,0x03,0x01,0x00,0x01,0x02,0x03,0x11,0x02,0x13,0x01,0x80,0x01,0x02,0x03,0x01,0x00,0x23,0x01,0x12,0x03,
    0x01,0x00,0x01,0x02,0x03,0x01,0x02,0x13,0x01,0x90,0x01,0x02,0x03,0x01,0x00,0x21,0x02,0x13,0x20,0x31,0x13,0x01,0xa0,0x01,
    0x02,0x03,0x01,0x00,0x11,0x02,0x13,0x01,0x20,0x01,0x12,0x41,0x90,0x01,0x02,0x03,0x11,0x12,0x03,0x11,0x40,0x13,0x42,0x41,
    0x40,0x01,0x02,0x03,0x01,0x02,0x13,0x01,0x60,0x01,0x53,0x42,0x31,0x00,0x01,0x02,0x03,0x01,0x13,0x01,0x80,0x41,0x53,0x32,
    0x11,0x02,0x03,0x01,0x03,0x01,0xe0,0x41,0x43,0x02,0x01,0x02,0x03,0x11,0xf0,0x40,0x41,0x23,0x01,0x90,0xf0,0xf0,0xf0,0xf0,
    0x00,0x01,0xf0,0xf0,0xd0,0x01,0x02,0x41,0xf0,0xf0,0x60,0x11,0x62,0x41,0xf0,0xf0,0x00,0x01,0x32,0x04,0x33,0x42,0x41,0xf0,
    0xa0,0x01,0x32,0x14,0x83,0x42,0x41,0xf0,0x40,0x01,0x32,0x24,0xd3,0x42,0x41,0xd0,0x11,0x22,0x44,0x31,0xe3,0x42,0x11,0xa0,
    0x01,0x32,0x34,0x11,0x04,0x01,0x10,0x41,0x83,0x11,0x32,0x04,0x01,0x90,0x01,0x32,0x34,0x11,0x03,0x04,0x01,0x60,0x41,0x23,
    0x11,0x32,0x14,0x01,0x80,0x01,0x32,0x24,0x11,0x23,0x04,0x01,0xb0,0x31,0x32,0x24,0x01,0x70,0x01,0x32,0x01,0x14,0x01,0x00,
    0x01,0x23,0x04,0x01,0xc0,0x11,0x22,0x44,0x01,0x50,0x11,0x32,0x31,0x10,0x01,0x23,0x04,0x01,0xb0,0x01,0x32,0x54,0x01,0x50,
    0x01,0x23,0x52,0x11,0x33,0x04,0x01,0xa0,0x01,0x32,0x34,0x11,0x04,0x01,0x50,0x01,0x73,0x22,0x33,0x04,0x11,0x80,0x01,0x32,
    0x24,0x11,0x13,0x04,0x01,0x50,0x01,0xe3,0x14,0x02,0x41,0x20,0x01,0x22,0x34,0x11,0x23,0x04,0x01,0x50,0x01,0xe3,0x14,0x03,
    0x42,0x21,0x22,0x34,0x01,0x00,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x41,0x63,0x14,0x53,0x42,0x34,0x01,0x10,0x01,0x23,0x04,
    0x01,0x50,0x01,0x23,0x04,0x01,0x20,0x21,0x33,0x14,0x93,0x24,0x11,0x20,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,
    0x01,0x23,0x14,0x01,0x83,0x14,0x01,0x40,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x00,0x41,
    0x33,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,
    0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,
    0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,
    0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,
    0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,
    0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x02,0x41,0x00,0x01,0x23,0x04,0x01,
    0x50,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x40,0x01,0x22,0x04,0x13,0x42,0x11,0x23,0x04,0x01,0x50,0x01,0x23,0x04,
    0x01,0x50,0x01,0x23,0x04,0x01,0x20,0x11,0x22,0x14,0x63,0x11,0x23,0x04,0x31,0x20,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,
    0x01,0x10,0x01,0x32,0x24,0x63,0x11,0x23,0x04,0x11,0x12,0x31,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x01,0x00,0x01,0x32,0x24,
    0x21,0x43,0x11,0x23,0x04,0x11,0x13,0x22,0x01,0x23,0x04,0x01,0x50,0x01,0x23,0x04,0x11,0x32,0x24,0x01,0x20,0x61,0x23,0x04,
    0x11,0x33,0x01,0x12,0x13,0x04,0x01,0x50,0x01,0x23,0x04,0x11,0x12,0x34,0x01,0x80,0x11,0x23,0x04,0x11,0x13,0x11,0x32,0x14,
    0x01,0x50,0x01,0x23,0x04,0x11,0x02,0x34,0x01,0xa0,0x01,0x23,0x04,0x41,0x32,0x24,0x01,0x50,0x01,0x23,0x04,0x11,0x34,0x01,
    0xb0,0x01,0x23,0x04,0x01,0x10,0x01,0x32,0x24,0x11,0x50,0x01,0x23,0x14,0x41,0xc0,0x01,0x23,0x04,0x01,0x00,0x01,0x32,0x24,
    0x01,0x70,0x01,0x23,0x32,0x31,0xb0,0x01,0x23,0x04,0x11,0x22,0x34,0x01,0x80,0x01,0x53,0x42,0x41,0x60,0x01,0x23,0x04,0x11,
    0x12,0x34,0x01,0x90,0x01,0xa3,0x42,0x41,0x10,0x01,0x23,0x04,0x11,0x02,0x34,0x01,0xa0,0x11,0xe3,0x42,0x21,0x23,0x04,0x01,
    0x02,0x24,0x11,0xd0,0x41,0xe3,0x12,0x33,0x44,0x01,0xf0,0x40,0x41,0xf3,0x34,0x01,0xf0,0xa0,0x41,0xa3,0x24,0x01,0xf0,0xf0,
    0x00,0x41,0x53,0x04,0x11,0xf0,0xf0,0x60,0x41,0x03,0x01,0xf0,0xf0,0xd0,0x01,0xf0,0xf0,0xf0,0xf0,0x00
};
/* side, offset and length of each size in yscr__icon_rle */
static const uint16_t yscr__icon_size[3][3] = { { 16, 0, 147 }, { 32, 147, 497 }, { 48, 644, 648 } };

/* Size k of the icon (0: 16, 1: 32, 2: 48 pixels) as RGBA rows of pitch
 * bytes. Returns 1 when the runs fill the square exactly. */
static int yscr__icon_decode(int k, uint8_t* px, int pitch) {
    int side = yscr__icon_size[k][0], i, n = 0;
    const uint8_t* r = yscr__icon_rle + yscr__icon_size[k][1];
    for (i = 0; i < yscr__icon_size[k][2]; i++) {
        int run = (r[i] >> 4) + 1, j;
        const uint8_t* c = yscr__icon_pal[r[i] & 15u];
        if ((r[i] & 15u) >= sizeof yscr__icon_pal / sizeof yscr__icon_pal[0] || n + run > side * side) return 0;
        for (j = 0; j < run; j++, n++) memcpy(px + (n / side) * pitch + (n % side) * 4, c, 4);
    }
    return n == side * side;
}

YSCR_API const char* yscr_version(void) { return YSCR_VERSION_STRING; }

YSCR_API const char* yscr_strerror(int code) {
    switch (code) {
    case YSCR_OK:                  return "ok";
    case YSCR_QUIT:                return "quit";
    case YSCR_ERR_ARG:             return "bad argument";
    case YSCR_ERR_CLOSED:          return "screen not open";
    case YSCR_ERR_TIMEOUT:         return "the swap path freed no slot in time";
    case YSCR_ERR_LOST:            return "device or window lost";
    case YSCR_ERR_ORDER:           return "begin and flip out of turn";
    case YSCR_ERR_NOT_IMPLEMENTED: return "not implemented";
    case YSCR_ERR_REFUSED:         return "refused";
    default:                         return code > 0 ? "ok" : "unknown error";
    }
}

/* --- modes ----------------------------------------------------------------- */

YSCR_API int yscr_mode_multiple_in(const yscr_mode* modes, int n,
                                       int32_t num, int32_t den, int32_t w, int32_t h,
                                       double tol_ppm, yscr_mode* out, double* err_ppm) {
    int i, best = -1, nearest = -1, best_k = 0;
    double rate, best_f = 0.0, nearest_err = 1e300;
    if (!modes || n < 0 || num <= 0 || den <= 0 || tol_ppm < 0) return YSCR_ERR_ARG;
    if (tol_ppm == 0) tol_ppm = 200.0;
    rate = (double)num / (double)den;
    for (i = 0; i < n; i++) {
        const yscr_mode* m = &modes[i];
        double f, k, e;
        if (m->refresh_num <= 0 || m->refresh_den <= 0) continue;
        if ((w && m->w != w) || (h && m->h != h)) continue;
        f = (double)m->refresh_num / (double)m->refresh_den;
        k = floor(f / rate + 0.5);
        if (k < 1) k = 1;
        e = (f / (k * rate) - 1.0) * 1e6;
        if (fabs(e) < nearest_err) { nearest_err = fabs(e); nearest = i; }
        if (fabs(e) <= tol_ppm && f > best_f) { best_f = f; best = i; best_k = (int)k; }
    }
    if (best >= 0) {
        double f = (double)modes[best].refresh_num / (double)modes[best].refresh_den;
        if (out) *out = modes[best];
        if (err_ppm) *err_ppm = (f / (best_k * rate) - 1.0) * 1e6;
        return best_k;
    }
    if (nearest >= 0) {
        double f = (double)modes[nearest].refresh_num / (double)modes[nearest].refresh_den;
        double k = floor(f / rate + 0.5);
        if (k < 1) k = 1;
        if (out) *out = modes[nearest];
        if (err_ppm) *err_ppm = (f / (k * rate) - 1.0) * 1e6;
    }
    return YSCR_ERR_REFUSED;
}

#if !defined(YSCR_NO_SDL)

static int64_t yscr__period_of(int32_t num, int32_t den) {
    if (num <= 0 || den <= 0) return 0;
    return ((int64_t)den * 1000000000 + num / 2) / num;
}

static void yscr__mode_from_sdl(const SDL_DisplayMode* m, yscr_mode* out) {
    memset(out, 0, sizeof *out);
    out->w = m->w;
    out->h = m->h;
    out->refresh_num = m->refresh_rate_numerator;
    out->refresh_den = m->refresh_rate_denominator;
    if (out->refresh_num <= 0 && m->refresh_rate > 0) {
        out->refresh_num = (int32_t)(m->refresh_rate * 1000.0f + 0.5f);
        out->refresh_den = 1000;
    }
    out->period_ns = yscr__period_of(out->refresh_num, out->refresh_den);
    out->format = (uint32_t)m->format;
    out->density = m->pixel_density;
}

/* SDL's video subsystem is reference counted, so a call that needs it holds
 * it only for its own duration. */
static bool yscr__video_up(void) { return SDL_InitSubSystem(SDL_INIT_VIDEO); }
static void yscr__video_down(void) { SDL_QuitSubSystem(SDL_INIT_VIDEO); }
static void yscr__pad_open(yscr_screen* s, SDL_JoystickID id);
static void yscr__pad_close(yscr_screen* s, SDL_JoystickID id, bool all);
static void yscr__devices_at_open(yscr_screen* s);
static void yscr__device_event(yscr_screen* s, const SDL_Event* ev);

/* SDL numbers the displays anew at each start of its video, and
 * yscr_displays() without a screen starts and stops it for the call, so its
 * ids were stale at the next call (v0.10.4; id 1, then 3 at open). SDL
 * never gives an id out twice in a process, so an id that is not current
 * names the display yscr_displays() listed under it: the current one with
 * the same name and bounds, else the only one with the same bounds or the
 * same name. SDL video calls run on one thread (WINDOW THREAD), so the
 * list needs no lock. */
typedef struct { uint32_t id; char name[64]; int32_t x, y, w, h; } yscr__listed_display;
static yscr__listed_display yscr__listed[16];
static unsigned yscr__n_listed;
static void yscr__display_listed(const yscr_display_info* d) {
    unsigned i, n = yscr__n_listed < 16 ? yscr__n_listed : 16;
    yscr__listed_display* e;
    for (i = 0; i < n && yscr__listed[i].id != d->id; i++) { }
    e = i < n ? &yscr__listed[i] : &yscr__listed[yscr__n_listed++ % 16];
    e->id = d->id;
    memcpy(e->name, d->name, sizeof e->name);
    e->x = d->x; e->y = d->y; e->w = d->w; e->h = d->h;
}
static SDL_DisplayID yscr__display_id(uint32_t display) {
    const yscr__listed_display* e = NULL;
    SDL_DisplayID* ids;
    SDL_DisplayID out = (SDL_DisplayID)display, by_rect = 0, by_name = 0;
    int i, n = 0, n_rect = 0, n_name = 0;
    unsigned k, n_listed = yscr__n_listed < 16 ? yscr__n_listed : 16;
    if (!display) return SDL_GetPrimaryDisplay();
    for (k = 0; k < n_listed; k++) if (yscr__listed[k].id == display) e = &yscr__listed[k];
    if (!e) return out;   /* not from yscr_displays(): SDL judges it */
    ids = SDL_GetDisplays(&n);
    for (i = 0; ids && i < n; i++) {
        SDL_Rect r;
        int same_rect, same_name;
        if (ids[i] == (SDL_DisplayID)display) { SDL_free(ids); return out; }   /* current */
        same_rect = SDL_GetDisplayBounds(ids[i], &r) && r.x == e->x && r.y == e->y && r.w == e->w && r.h == e->h;
        same_name = SDL_GetDisplayName(ids[i]) && !strncmp(SDL_GetDisplayName(ids[i]), e->name, sizeof e->name - 1);
        if (same_rect && same_name) { out = ids[i]; n_rect = n_name = -1; break; }
        if (same_rect) { by_rect = ids[i]; n_rect++; }
        if (same_name) { by_name = ids[i]; n_name++; }
    }
    if (n_rect == 1) out = by_rect;
    else if (n_rect == 0 && n_name == 1) out = by_name;
    SDL_free(ids);
    return out;
}

/* yscr_displays(), yscr_modes() and the desktop size for
 * yscr_mode_multiple(): SDL video calls, so on the window thread while it
 * runs (WINDOW THREAD); else here, with SDL's video up for the call. */
typedef struct { int what; yscr_display_info* d; yscr_mode* m; int cap; uint32_t display; int32_t w, h; int rc; } yscr__dmcall;
static int yscr__displays_here(yscr_display_info* out, int cap);
static int yscr__modes_here(uint32_t display, yscr_mode* out, int cap);
static void yscr__dm_cb(void* p) {
    yscr__dmcall* a = (yscr__dmcall*)p;
    if (a->what == 0) a->rc = yscr__displays_here(a->d, a->cap);
    else if (a->what == 1) a->rc = yscr__modes_here(a->display, a->m, a->cap);
    else if (!yscr__video_up()) a->rc = YSCR_ERR_LOST;
    else {
        const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(yscr__display_id(a->display));
        if (dm) { a->w = dm->w; a->h = dm->h; }
        yscr__video_down();
        a->rc = YSCR_OK;
    }
}
static int yscr__dm(yscr__dmcall* a) {
    int rc = yscr__wt_call(yscr__dm_cb, a);
    return rc < 0 ? rc : a->rc;
}

YSCR_API int yscr_displays(yscr_display_info* out, int cap) {
    yscr__dmcall a;
    if (cap < 0 || (cap > 0 && !out)) return YSCR_ERR_ARG;
    memset(&a, 0, sizeof a);
    a.what = 0; a.d = out; a.cap = cap;
    return yscr__dm(&a);
}

YSCR_API int yscr_modes(uint32_t display, yscr_mode* out, int cap) {
    yscr__dmcall a;
    if (cap < 0 || (cap > 0 && !out)) return YSCR_ERR_ARG;
    memset(&a, 0, sizeof a);
    a.what = 1; a.m = out; a.cap = cap; a.display = display;
    return yscr__dm(&a);
}

static int yscr__displays_here(yscr_display_info* out, int cap) {
    int i, n = 0, count = 0;
    SDL_DisplayID* ids;
    SDL_DisplayID primary;
    if (!yscr__video_up()) return YSCR_ERR_LOST;
    ids = SDL_GetDisplays(&n);
    primary = SDL_GetPrimaryDisplay();
    for (i = 0; ids && i < n; i++) {
        if (count < cap) {
            yscr_display_info* d = &out[count];
            SDL_Rect r;
            const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(ids[i]);
            memset(d, 0, sizeof *d);
            d->id = (uint32_t)ids[i];
            yscr__copy(d->name, sizeof d->name, SDL_GetDisplayName(ids[i]));
            if (SDL_GetDisplayBounds(ids[i], &r)) { d->x = r.x; d->y = r.y; d->w = r.w; d->h = r.h; }
            if (dm) yscr__mode_from_sdl(dm, &d->desktop);
            d->primary = ids[i] == primary;
            yscr__display_listed(d);
        }
        count++;
    }
    SDL_free(ids);
    yscr__video_down();
    return count;
}

static int yscr__modes_here(uint32_t display, yscr_mode* out, int cap) {
    int i, n = 0;
    SDL_DisplayMode** modes;
    if (!yscr__video_up()) return YSCR_ERR_LOST;
    modes = SDL_GetFullscreenDisplayModes(yscr__display_id(display), &n);
    if (!modes) { yscr__video_down(); return YSCR_ERR_ARG; }
    for (i = 0; i < n && i < cap; i++) yscr__mode_from_sdl(modes[i], &out[i]);
    SDL_free(modes);
    yscr__video_down();
    return n;
}

YSCR_API int yscr_mode_multiple(uint32_t display, int32_t num, int32_t den,
                                    int32_t w, int32_t h, double tol_ppm,
                                    yscr_mode* out, double* err_ppm) {
    yscr_mode list[256];
    int n = yscr_modes(display, list, 256), rc;
    if (n < 0) return n;
    if (n > 256) n = 256;
    if (w == 0 && h == 0) {
        yscr__dmcall a;
        memset(&a, 0, sizeof a);
        a.what = 2; a.display = display;
        if ((rc = yscr__dm(&a)) < 0) return rc;
        w = a.w; h = a.h;
    }
    rc = yscr_mode_multiple_in(list, n, num, den, w, h, tol_ppm, out, err_ppm);
    return rc;
}

#else /* YSCR_NO_SDL */

YSCR_API int yscr_displays(yscr_display_info* out, int cap) {
    (void)out; (void)cap; return YSCR_ERR_NOT_IMPLEMENTED;
}
YSCR_API int yscr_modes(uint32_t display, yscr_mode* out, int cap) {
    (void)display; (void)out; (void)cap; return YSCR_ERR_NOT_IMPLEMENTED;
}
YSCR_API int yscr_mode_multiple(uint32_t display, int32_t num, int32_t den,
                                    int32_t w, int32_t h, double tol_ppm,
                                    yscr_mode* out, double* err_ppm) {
    (void)display; (void)num; (void)den; (void)w; (void)h; (void)tol_ppm;
    (void)out; (void)err_ppm;
    return YSCR_ERR_NOT_IMPLEMENTED;
}

#endif /* YSCR_NO_SDL */

/* --- SIM presenter --------------------------------------------------------- */

typedef struct yscr__sim {
    int64_t       t0, period;
    uint64_t      pend_id;
    int64_t       pend_count;
    int           has_pend;
    yscr_vblank done;
    int           has_done;
} yscr__sim;

typedef char yscr__sim_fits[sizeof(yscr__sim) <= sizeof(((yscr_screen*)0)->backend_mem) ? 1 : -1];

static int64_t yscr__sim_count(const yscr__sim* m, int64_t t) {
    int64_t d = t - m->t0;
    return d >= 0 ? d / m->period : -((-d + m->period - 1) / m->period);
}

static int yscr__sim_open(void* ctx, const yscr_presenter_open* in, yscr_caps* caps,
                            char* err, size_t err_cap) {
    yscr__sim* m = (yscr__sim*)ctx;
    (void)err; (void)err_cap;
    memset(m, 0, sizeof *m);
    m->period = in->sim_period_ns > 0 ? in->sim_period_ns : 16666667;
    m->t0 = yscr__now();
    caps->kind = YSCR_FIXED_GRID;
    caps->period_ns = m->period;
    caps->native_target = true;
    caps->hw_onset = true;
    caps->max_in_flight = 1;
    caps->mode.refresh_num = 1000000000;
    caps->mode.refresh_den = (int32_t)(m->period > 0x7fffffff ? 0x7fffffff : m->period);
    caps->mode.period_ns = m->period;
    return YSCR_OK;
}

static void yscr__sim_close(void* ctx) { (void)ctx; }

static int yscr__sim_acquire(void* ctx, int64_t deadline_ns, yscr_vblank* newest) {
    yscr__sim* m = (yscr__sim*)ctx;
    int64_t c;
    (void)deadline_ns;
    if (m->has_pend) {
        int64_t t = m->t0 + m->pend_count * m->period;
        if (t > yscr__now()) YSCR__SLEEP_UNTIL((int64_t)t, YRT_DEFAULT_SPIN_NS);
        m->done.present_id = m->pend_id;
        m->done.t_ns = t;
        m->done.count = m->pend_count;
        m->done.path = YSCR_PATH_SIMULATED;
        m->done.flags = 0;
        m->has_done = 1;
        m->has_pend = 0;
    }
    c = yscr__sim_count(m, yscr__now());
    newest->present_id = 0;
    newest->count = c;
    newest->t_ns = m->t0 + c * m->period;
    newest->path = YSCR_PATH_SIMULATED;
    newest->flags = 0;
    return YSCR_OK;
}

static int yscr__sim_present(void* ctx, const yscr_present_req* req) {
    yscr__sim* m = (yscr__sim*)ctx;
    int64_t next = yscr__sim_count(m, yscr__now()) + 1;
    m->pend_id = req->present_id;
    m->pend_count = req->target_count > next ? req->target_count : next;
    m->has_pend = 1;
    return YSCR_OK;
}

static int yscr__sim_completions(void* ctx, yscr_vblank* out, int cap) {
    yscr__sim* m = (yscr__sim*)ctx;
    if (!m->has_done || cap < 1) return 0;
    out[0] = m->done;
    m->has_done = 0;
    return 1;
}

static int yscr__sim_describe(void* ctx, char* buf, size_t cap) {
    yscr__sim* m = (yscr__sim*)ctx;
    return snprintf(buf, cap, "sim_period=%lldns", (long long)m->period);
}

static const yscr_presenter yscr__sim_presenter = {
    YSCR_PRESENTER_VERSION, "sim", false, false, false,
    yscr__sim_open, yscr__sim_close, yscr__sim_acquire, yscr__sim_present,
    yscr__sim_completions, NULL, NULL, yscr__sim_describe, NULL
};

/* --- web presenter (Emscripten: WebGL2, requestAnimationFrame) ------------ */

/* The state and the two rules below compile everywhere, so the core test
 * checks them without a browser; the presenter itself is Emscripten's. */

/* Settling on the frame callbacks (BACKENDS, WEB): the last 20 intervals
 * between callback times, none over 1.5 times their median (a skipped
 * callback), SD at most 0.5 ms; at least 0.25 s, at most 2 s. Never a
 * failure: a browser gives no control to wait for. */
#define YSCR__WEB_N        20
#define YSCR__WEB_SD_NS    500000.0
#define YSCR__WEB_MIN_NS   250000000LL
#define YSCR__WEB_MAX_NS   2000000000LL
#define YSCR__WEB_WAIT_MS  250   /* a callback wait; a hidden page has none */

typedef struct yscr__web {
    int32_t     ctx;              /* EMSCRIPTEN_WEBGL_CONTEXT_HANDLE         */
    int32_t     fresh;            /* in a frame callback, nothing presented  */
    int32_t     has_pend, has_done, desync, coi, settled;
    int32_t     n_iv, settle_frames;
    uint64_t    pend_id;
    int64_t     off_ns;           /* ysp_rt ns minus performance.now() in ns */
    int64_t     corr_ns;          /* that correlation's width                */
    int64_t     period;           /* measured, ns                            */
    int64_t     t_last, c_last;   /* the newest frame callback               */
    int64_t     settle_ns, sd_ns, t_open;
    int64_t     hidden_ns;
    int64_t     pend_t;           /* when the pending present was made      */
    uint32_t    callbacks, hidden_waits, skipped, late;
    uint32_t    dom_found, dom_tried; /* input stamped with the DOM's time   */
    int64_t     iv[YSCR__WEB_N];
    yscr_vblank done;
    char        canvas[64];
} yscr__web;

typedef char yscr__web_fits[sizeof(yscr__web) <= sizeof(((yscr_screen*)0)->backend_mem) ? 1 : -1];

/* The vblank a present shows on: the first after both the frame callback
 * that completed it and the present call. A task that ran past a vsync
 * delays the next callback, whose time is then the vsync it missed, before
 * the present (measured in headless Chrome: 1.3 ms before it after a 17 ms
 * setup), so the frame cannot have shown there. c_cb and t_cb are that
 * callback's count and time. */
static int64_t yscr__web_shown(int64_t c_cb, int64_t t_cb, int64_t pend_t, int64_t period) {
    int64_t c_pres = c_cb + (int64_t)floor((double)(pend_t - t_cb) / (double)period);
    return c_pres + 1 > c_cb ? c_pres + 1 : c_cb;
}

/* The intervals of open's callbacks: 1 when the last YSCR__WEB_N are
 * steady (SETTLE's rule for the web, above); sets the period and the SD. */
static int yscr__web_steady(yscr__web* w) {
    int64_t sorted[YSCR__WEB_N], med;
    double m = 0, v = 0;
    int i, j;
    if (w->n_iv < YSCR__WEB_N) return 0;
    memcpy(sorted, w->iv, sizeof sorted);
    for (i = 1; i < YSCR__WEB_N; i++)
        for (j = i; j > 0 && sorted[j - 1] > sorted[j]; j--) { int64_t x = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = x; }
    med = sorted[YSCR__WEB_N / 2];
    for (i = 0; i < YSCR__WEB_N; i++) m += (double)w->iv[i];
    m /= YSCR__WEB_N;
    for (i = 0; i < YSCR__WEB_N; i++) v += ((double)w->iv[i] - m) * ((double)w->iv[i] - m);
    v = sqrt(v / YSCR__WEB_N);
    w->sd_ns = (int64_t)llround(v);
    if (sorted[YSCR__WEB_N - 1] * 2 > med * 3) {   /* a skipped callback: the median is the period */
        w->period = med;
        return 0;
    }
    w->period = (int64_t)llround(m);
    return v <= YSCR__WEB_SD_NS;
}

#if defined(YSCR__WEB)

EM_JS_DEPS(yscr__web_deps, "$UTF8ToString,$GL")

/* The next frame callback's time (performance.now() ms), or -1 when none
 * came in timeout_ms: the page is hidden. The await suspends the program
 * (JSPI or ASYNCIFY), so the blocking frame loop runs in the callback's
 * task: what it draws is composited with that frame. */
EM_ASYNC_JS(double, yscr__web_raf, (int timeout_ms), {
    return await new Promise(function(resolve) {
        var done = false, to = 0, id = 0;
        id = requestAnimationFrame(function(t) {
            /* its time and when it ran, for a page's own statistics */
            if (!done) { done = true; clearTimeout(to); Module['yscrRafRun'] = performance.now(); Module['yscrRafT'] = t; resolve(t); }
        });
        to = setTimeout(function() { if (!done) { done = true; cancelAnimationFrame(id); resolve(-1); } }, timeout_ms);
    });
})

EM_JS(double, yscr__web_perf_ms, (void), { return performance.now(); })

/* A WebGL2 context on the canvas. Created here, not through SDL, so the
 * attributes are the header's: no alpha, depth, stencil or multisampling
 * (gfx.h draws to its own targets), the drawing buffer not preserved (the
 * compositor takes it at each frame), the high-performance GPU. desync is
 * Module.yscrDesynchronized, off by default (BACKENDS, WEB). */
EM_JS(int, yscr__web_gl_create, (const char* sel, int desync), {
    var c = document.querySelector(UTF8ToString(sel));
    if (!c) return 0;
    var a = { alpha: false, depth: false, stencil: false, antialias: false, premultipliedAlpha: true,
              preserveDrawingBuffer: false, powerPreference: 'high-performance',
              failIfMajorPerformanceCaveat: false, majorVersion: 2, minorVersion: 0,
              enableExtensionsByDefault: 1, desynchronized: !!desync };
    var h = GL.createContext(c, a);
    if (h) GL.makeContextCurrent(h);
    return h;
})

/* 0 crossOriginIsolated, 1 visible now, 2 Module.yscrDesynchronized,
 * 3 visibility changes so far (the listener counts from the first open),
 * 4 and 5 the viewport's width and height in CSS pixels, 6 the
 * devicePixelRatio times 100. */
EM_JS(int, yscr__web_query, (int what), {
    if (!Module['yscrVis']) {
        Module['yscrVis'] = { changes: 0, log: [] };
        document.addEventListener('visibilitychange', function(e) {
            var v = Module['yscrVis'];
            v.changes++;
            if (v.log.length < 64) v.log.push([e.timeStamp, document.visibilityState === 'visible' ? 1 : 0]);
        });
    }
    if (what === 0) return (typeof crossOriginIsolated !== 'undefined' && crossOriginIsolated) ? 1 : 0;
    if (what === 1) return document.visibilityState === 'visible' ? 1 : 0;
    if (what === 2) return Module['yscrDesynchronized'] ? 1 : 0;
    if (what === 4) return window.innerWidth;
    if (what === 5) return window.innerHeight;
    if (what === 6) return Math.round((window.devicePixelRatio || 1) * 100);
    return Module['yscrVis'].changes;
})

/* The page's fullscreen is the page's (BACKENDS, WEB). SDL's port leaves
 * it when it applies a window's flags: at SDL_CreateWindow and at
 * SDL_DestroyWindow whenever the document is fullscreen, which it is when
 * the program yielded between the page's request and open() (av_sync: the
 * sound's open awaits its callbacks; Chrome 154 and Firefox 157 left
 * fullscreen 1.4 to 3 s into the run). While on is 1, the document's
 * exitFullscreen does nothing; 0 gives the browser's back. */
EM_JS(void, yscr__web_fs_hold, (int on), {
    var d = document, h = Module['yscrFsHold'];
    if (on && !h) {
        var no = function() { return Promise.resolve(); };
        h = Module['yscrFsHold'] = { exit: d.hasOwnProperty('exitFullscreen'), wexit: d.hasOwnProperty('webkitExitFullscreen') };
        if (!h.exit) Object.defineProperty(d, 'exitFullscreen', { value: no, configurable: true, writable: true });
        if (!h.wexit) Object.defineProperty(d, 'webkitExitFullscreen', { value: no, configurable: true, writable: true });
    } else if (!on && h) {
        if (!h.exit) delete d.exitFullscreen;
        if (!h.wexit) delete d.webkitExitFullscreen;
        Module['yscrFsHold'] = null;
    }
})

/* The DOM's own times of the input events SDL turns into events (INPUT,
 * BACKENDS WEB). SDL's Emscripten port stamps an event when its handler
 * runs; Event.timeStamp is when the browser made the event, on the
 * performance.now() clock, and for input it is the time the OS gave the
 * browser. These capture listeners on the window run in the same task as
 * SDL's handler, before it, so the record whose run time is just before an
 * SDL event's stamp is that event's. A record: class (1 key down, 2 key
 * up, 3 pointer down, 4 pointer up or cancel), the pointer id or -1, the
 * DOM's time, the time the listener ran. The newest 64 are kept. */
EM_JS(void, yscr__web_in_init, (void), {
    if (Module['yscrIn']) return;
    var r = Module['yscrIn'] = { log: [], head: 0 };
    function add(cls, id, e) {
        var x = [cls, id, e.timeStamp, performance.now()];
        if (r.log.length < 64) r.log.push(x); else { r.log[r.head] = x; r.head = (r.head + 1) & 63; }
    }
    window.addEventListener('keydown', function(e) { add(1, -1, e); }, true);
    window.addEventListener('keyup', function(e) { add(2, -1, e); }, true);
    window.addEventListener('pointerdown', function(e) { add(3, e.pointerId, e); }, true);
    window.addEventListener('pointerup', function(e) { add(4, e.pointerId, e); }, true);
    window.addEventListener('pointercancel', function(e) { add(4, e.pointerId, e); }, true);
    /* a second mouse button while one is held comes as a move */
    window.addEventListener('pointermove', function(e) {
        var bit = [1, 4, 2, 8, 16][e.button];
        if (e.button >= 0 && bit) add((e.buttons & bit) ? 3 : 4, e.pointerId, e);
    }, true);
})

/* The DOM time (ms) of the newest record of class cls (and pointer id, when
 * id >= 0) whose listener ran at most 1 ms after t_ms and at most 50 ms
 * before it, or -1. A time that is not a performance.now() time (more than
 * 1 s before the listener ran, or after it) is not used. */
EM_JS(double, yscr__web_in_find, (int cls, int id, double t_ms), {
    var r = Module['yscrIn'], best = null, i, x;
    if (!r) return -1;
    for (i = 0; i < r.log.length; i++) {
        x = r.log[i];
        if (x[0] !== cls || (id >= 0 && x[1] !== id) || x[3] > t_ms + 1 || t_ms - x[3] > 50) continue;
        if (!best || x[3] > best[3]) best = x;
    }
    if (!best || !(best[2] <= best[3] + 0.001) || best[3] - best[2] > 1000) return -1;
    return best[2];
})

/* The oldest visibility change not taken yet: its DOM time (ms) in *t and
 * 1 visible or 0 hidden, or -1 when there is none. */
EM_JS(int, yscr__web_vis_take, (double* t), {
    var v = Module['yscrVis'];
    if (!v || !v.log.length) return -1;
    var e = v.log.shift();
    HEAPF64[t >> 3] = e[0];
    return e[1];
})

static uint64_t yscr__web_perf_ns(void) { return (uint64_t)(yscr__web_perf_ms() * 1e6); }

static int64_t yscr__web_to_rt(const yscr__web* w, double ms) { return (int64_t)llround(ms * 1e6) + w->off_ns; }

/* A press or release from SDL gets the DOM event's own time in place of
 * SDL's handler time, when its record is found (yscr__web_in_find). */
static void yscr__web_dom_stamp(yscr__web* w, const SDL_Event* ev, yin_event* out) {
    int cls, id = -1;
    double ms;
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:            cls = 1; break;
    case SDL_EVENT_KEY_UP:              cls = 2; break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_PEN_DOWN:            cls = 3; break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_PEN_UP:              cls = 4; break;
    case SDL_EVENT_FINGER_DOWN:         cls = 3; id = (int)ev->tfinger.fingerID - 1; break;   /* SDL: pointerId + 1 */
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:     cls = 4; id = (int)ev->tfinger.fingerID - 1; break;
    default: return;
    }
    w->dom_tried++;
    ms = yscr__web_in_find(cls, id, (double)(out->t - w->off_ns) * 1e-6);
    if (ms < 0) return;
    out->t = yscr__web_to_rt(w, ms);
    w->dom_found++;
}

/* Waits for the next frame callback. While the page is hidden none come:
 * the pending present completes with no time (OCCLUDED), and the wait goes
 * on until the page is shown again, so a hidden page pauses the loop. */
static void yscr__web_next(yscr__web* w) {
    double ms;
    int64_t t, k, t_hide = 0;
    for (;;) {
        ms = yscr__web_raf(YSCR__WEB_WAIT_MS);
        if (ms >= 0) break;
        if (!t_hide) t_hide = yscr__now();
        w->hidden_waits++;
        if (w->has_pend) {
            memset(&w->done, 0, sizeof w->done);
            w->done.present_id = w->pend_id;
            w->done.path = YSCR_PATH_BROWSER;
            w->done.flags = YSCR_FLIP_OCCLUDED;
            w->has_done = 1;
            w->has_pend = 0;
        }
    }
    if (t_hide) w->hidden_ns += yscr__now() - t_hide;
    t = yscr__web_to_rt(w, ms);
    k = w->period > 0 && w->callbacks ? (int64_t)llround((double)(t - w->t_last) / (double)w->period) : 1;
    if (k < 1) k = 1;
    if (k > 1 && !t_hide) w->skipped += (uint32_t)(k - 1);
    w->c_last += k;
    w->t_last = t;
    w->callbacks++;
    w->fresh = 1;
    if (w->has_pend) {
        int64_t shown = yscr__web_shown(w->c_last, w->t_last, w->pend_t, w->period);
        memset(&w->done, 0, sizeof w->done);
        w->done.present_id = w->pend_id;
        w->done.t_ns = t + (shown - w->c_last) * w->period;
        w->done.count = shown;
        w->done.path = YSCR_PATH_BROWSER;
        w->done.tier = YSCR_TIER_WEB;
        w->done.flags = YSCR_FLIP_ONSET_PLANNED;
        w->has_done = 1;
        w->has_pend = 0;
    }
}

static int yscr__web_open(void* ctx, const yscr_presenter_open* in, yscr_caps* caps, char* err, size_t err_cap) {
    yscr__web* w = (yscr__web*)ctx;
    yrt_corr_desc cd;
    yrt_corr c;
    int cw = 0, ch = 0;
    const char* sel;
    memset(w, 0, sizeof *w);
    w->t_open = yscr__now();
    if (!in->window) { yscr__set_error(err, err_cap, "ysp_screen: web: no window"); return YSCR_ERR_ARG; }
    sel = SDL_GetStringProperty(SDL_GetWindowProperties(in->window), SDL_PROP_WINDOW_EMSCRIPTEN_CANVAS_ID_STRING, "#canvas");
    yscr__copy(w->canvas, sizeof w->canvas, sel);
    w->coi = yscr__web_query(0);
    w->desync = yscr__web_query(2);
    yscr__web_in_init();
    w->ctx = yscr__web_gl_create(w->canvas, w->desync);
    if (w->ctx <= 0) {
        yscr__set_error(err, err_cap, "ysp_screen: web: no WebGL2 context on %s (the browser or its GPU has no WebGL2)", w->canvas);
        return YSCR_ERR_LOST;
    }
    /* performance.now() is the frame callbacks' clock; yrt_now_ns() is
     * Emscripten's clock_gettime, which may count from another origin */
    memset(&cd, 0, sizeof cd);
    cd.read = yscr__web_perf_ns;
    if (yrt_correlate(&cd, &c) > 0) {
        w->off_ns = c.rt_ns - (int64_t)c.other;
        w->corr_ns = c.width_ns;
    }
    /* Settle: count callbacks until their intervals are steady. */
    for (;;) {
        int64_t prev = w->t_last, el;
        yscr__web_next(w);
        if (w->callbacks > 1) {
            if (w->n_iv == YSCR__WEB_N) memmove(w->iv, w->iv + 1, sizeof(int64_t) * (YSCR__WEB_N - 1));
            else w->n_iv++;
            w->iv[w->n_iv - 1] = w->t_last - prev;
        }
        el = yscr__now() - w->t_open;
        w->settled = yscr__web_steady(w);
        if ((w->settled && el >= YSCR__WEB_MIN_NS) || el >= YSCR__WEB_MAX_NS) break;
    }
    w->settle_ns = yscr__now() - w->t_open;
    w->settle_frames = (int32_t)w->callbacks;
    if (w->period <= 0) w->period = 16666667;
    w->skipped = 0;
    w->c_last = 0;   /* the grid's count starts here */
    emscripten_get_canvas_element_size(w->canvas, &cw, &ch);
    caps->kind = YSCR_CALLBACK;
    caps->period_ns = w->period;
    caps->native_target = true;   /* the core never sleeps: the main thread must not block */
    caps->hw_onset = false;
    caps->max_in_flight = 1;
    caps->mode.w = cw;
    caps->mode.h = ch;
    caps->mode.refresh_num = (int32_t)llround(1e12 / (double)w->period);
    caps->mode.refresh_den = 1000;
    caps->mode.period_ns = w->period;
    return YSCR_OK;
}

static void yscr__web_close(void* ctx) {
    yscr__web* w = (yscr__web*)ctx;
    if (w->ctx > 0) emscripten_webgl_destroy_context((EMSCRIPTEN_WEBGL_CONTEXT_HANDLE)w->ctx);
    w->ctx = 0;
}

/* A frame is drawn early in a frame callback's task, so it is composited
 * with that frame. A callback that has run more than half a period ago
 * (open() held it while the program set up, or the browser ran the
 * callback late) is not used: a draw there was replaced before it showed,
 * by the next frame's draw in the next callback (measured in headless
 * Chrome after a 17 ms setup). So the wait goes on, at most 4 callbacks. */
static void yscr__web_sync(yscr__web* w) {
    int i;
    if (w->fresh && yscr__now() - w->t_last <= w->period / 2) return;
    for (i = 0; i < 4; i++) {
        yscr__web_next(w);
        if (yscr__now() - w->t_last <= w->period / 2) return;
        w->late++;
    }
}

static int yscr__web_acquire(void* ctx, int64_t deadline_ns, yscr_vblank* newest) {
    yscr__web* w = (yscr__web*)ctx;
    (void)deadline_ns;   /* a hidden page waits past it: see yscr__web_next */
    yscr__web_sync(w);
    emscripten_webgl_make_context_current((EMSCRIPTEN_WEBGL_CONTEXT_HANDLE)w->ctx);
    memset(newest, 0, sizeof *newest);
    newest->t_ns = w->t_last;
    newest->count = w->c_last;
    newest->path = YSCR_PATH_BROWSER;
    return YSCR_OK;
}

/* The browser shows what was drawn at the next frame it composites, after
 * the program yields. A target later than that cannot be held: the drawing
 * buffer is not preserved, so the record says EARLY (BACKENDS, WEB). */
static int yscr__web_present(void* ctx, const yscr_present_req* req) {
    yscr__web* w = (yscr__web*)ctx;
    w->pend_id = req->present_id;
    w->pend_t = yscr__now();
    w->has_pend = 1;
    w->fresh = 0;
    return YSCR_OK;
}

static int yscr__web_completions(void* ctx, yscr_vblank* out, int cap) {
    yscr__web* w = (yscr__web*)ctx;
    if (!w->has_done || cap < 1) return 0;
    out[0] = w->done;
    w->has_done = 0;
    return 1;
}

static yscr_proc yscr__web_gl_proc(void* ctx, const char* name) {
    (void)ctx;
    return (yscr_proc)emscripten_webgl_get_proc_address(name);
}

static void yscr__web_bind(void* ctx) {
    yscr__web* w = (yscr__web*)ctx;
    yscr__web_sync(w);
    emscripten_webgl_make_context_current((EMSCRIPTEN_WEBGL_CONTEXT_HANDLE)w->ctx);
}

static int yscr__web_describe(void* ctx, char* buf, size_t cap) {
    yscr__web* w = (yscr__web*)ctx;
    yrt_clock_info ci;
    yrt_get_clock_info(&ci);
    return snprintf(buf, cap, "raf_period=%.4fms raf_settle=%s%.2fs(%d callbacks,sd=%.0fus) coi=%d clock_step=%lldns "
                    "corr=%lldns desync=%d skipped=%u late=%u hidden=%u(%.1fs) vis_changes=%d canvas=%s dpr=%.2f "
                    "dom_stamps=%u/%u",
                    (double)w->period * 1e-6, w->settled ? "" : "UNSTEADY,", (double)w->settle_ns * 1e-9,
                    (int)w->settle_frames, (double)w->sd_ns * 1e-3, w->coi, (long long)ci.resolution_ns,
                    (long long)w->corr_ns, w->desync, (unsigned)w->skipped, (unsigned)w->late, (unsigned)w->hidden_waits,
                    (double)w->hidden_ns * 1e-9, yscr__web_query(3), w->canvas, (double)yscr__web_query(6) * 0.01,
                    (unsigned)w->dom_found, (unsigned)w->dom_tried);
}

static const yscr_presenter yscr__web_presenter = {
    YSCR_PRESENTER_VERSION, "web", true, true, false,
    yscr__web_open, yscr__web_close, yscr__web_acquire, yscr__web_present,
    yscr__web_completions, yscr__web_gl_proc, yscr__web_bind, yscr__web_describe, NULL
};

#endif /* YSCR__WEB */

/* --- DXGI flip presenter (Windows 10 and 11) ------------------------------ */

#if defined(YSCR__DXGI)

/* COM from C and from C++ without CINTERFACE, so the implementation file
 * may include other COM headers in either language. */
#ifdef __cplusplus
    #define YSCR__CALL(o, m, ...) ((o)->m(__VA_ARGS__))
    #define YSCR__CALL0(o, m)     ((o)->m())
    #define YSCR__IID(x)          (x)
#else
    #define YSCR__CALL(o, m, ...) ((o)->lpVtbl->m((o), __VA_ARGS__))
    #define YSCR__CALL0(o, m)     ((o)->lpVtbl->m(o))
    #define YSCR__IID(x)          (&(x))
#endif
#define YSCR__RELEASE(o) do { if (o) { YSCR__CALL0((o), Release); (o) = NULL; } } while (0)

/* Own copies of the IIDs, so nothing links dxguid. */
static const IID yscr__IID_IDXGIFactory1 = {0x770aae78,0xf26f,0x4dba,{0xa8,0x29,0x25,0x3c,0x83,0xd1,0xb3,0x87}};
static const IID yscr__IID_IDXGIFactory2 = {0x50c83a1c,0xe072,0x4c48,{0x87,0xb0,0x36,0x30,0xfa,0x36,0xa6,0xd0}};
static const IID yscr__IID_IDXGIFactory5 = {0x7632e1f5,0xee65,0x4dca,{0x87,0xfd,0x84,0xcd,0x75,0xf8,0x83,0x8d}};
static const IID yscr__IID_IDXGIDevice   = {0x54ec77fa,0x1377,0x44e6,{0x8c,0x32,0x88,0xfd,0x5f,0x44,0xc8,0x4c}};
static const IID yscr__IID_IDXGISwapChain2 = {0xa8be2ac4,0x199f,0x4946,{0xb3,0x31,0x79,0x59,0x9f,0xb9,0x8d,0xe7}};
static const IID yscr__IID_IDXGISwapChainMedia = {0xdd95b90b,0xf05f,0x4f6a,{0xbd,0x65,0x25,0xbf,0xb2,0x64,0xbd,0x84}};
static const IID yscr__IID_ID3D11DeviceContext1 = {0xbb2c6faa,0xb5fb,0x4082,{0x8e,0x6b,0x38,0x8b,0x8c,0xfa,0x90,0xe1}};
static const IID yscr__IID_ID3D11Texture2D = {0x6f15aaf2,0xd208,0x4e89,{0x9a,0xb4,0x48,0x95,0x35,0xd3,0x4f,0x9c}};

static const IID yscr__IID_ID3D11Device5 = {0x8ffde202,0xa0e7,0x45df,{0x9e,0x01,0xe8,0x37,0x80,0x1b,0x5e,0xa0}};
static const IID yscr__IID_ID3D11DeviceContext4 = {0x917600da,0xf58c,0x4c33,{0x98,0xd8,0x3e,0x15,0xb3,0x90,0xfa,0x24}};
static const IID yscr__IID_ID3D11Fence = {0xaffde9d1,0x1df7,0x4bb7,{0x8a,0x34,0x0f,0x46,0x25,0x1d,0xab,0x80}};
/* ID3D10Multithread has the same IID and the same slots. */
static const IID yscr__IID_ID3D11Multithread = {0x9b7e4e00,0x342c,0x4106,{0xa1,0x9f,0x4f,0x27,0x04,0xf6,0x89,0xf0}};

/* desc.d3d11_video: a device a decoder can share (NATIVE HANDLES). */
static UINT yscr__device_flags(const yscr_presenter_open* in) {
    return (UINT)D3D11_CREATE_DEVICE_BGRA_SUPPORT | (in->d3d11_video ? (UINT)D3D11_CREATE_DEVICE_VIDEO_SUPPORT : 0u);
}

static void yscr__device_error(const yscr_presenter_open* in, HRESULT hr, char* err, size_t err_cap) {
    if (in->d3d11_video)
        yscr__set_error(err, err_cap, "ysp_screen: desc.d3d11_video: D3D11CreateDevice with VIDEO_SUPPORT failed "
                          "0x%08lx; the adapter may have no D3D11 video support", (unsigned long)hr);
    else
        yscr__set_error(err, err_cap, "ysp_screen: D3D11CreateDevice 0x%08lx", (unsigned long)hr);
}

/* A decoder thread then shares the immediate context with the frame
 * thread; protection takes a lock on each call of either. */
static void yscr__device_protect(const yscr_presenter_open* in, ID3D11Device* dev) {
    ID3D11Multithread* mt = NULL;
    if (!in->d3d11_video) return;
    if (SUCCEEDED(YSCR__CALL(dev, QueryInterface, YSCR__IID(yscr__IID_ID3D11Multithread), (void**)&mt)) && mt) {
        YSCR__CALL(mt, SetMultithreadProtected, TRUE);
        YSCR__RELEASE(mt);
    }
}

/* Codes, the read-back and the GPU fence: the same on both Windows swap
 * paths, so one copy. */
#define YSCR__STAGE_W 8192
typedef struct yscr__d3dcodes {
    ID3D11DeviceContext4*   dctx4;
    ID3D11Fence*            fence;
    ID3D11Texture2D*        staging;         /* YSCR__STAGE_W x 1, read back */
    const yscr_code_draw* staged;          /* what was copied last present   */
    int32_t                 n_staged;
    uint32_t*               checked;
    uint32_t*               failed;
} yscr__d3dcodes;

/* Rows of up to this many pixels are drawn one ClearView per pixel; longer
 * ones with one UpdateSubresource (measured: see CODES in STATUS). */
static int yscr__row_clear_max = 8;

static void yscr__d3d_color(uint32_t v, float c[4]) {
    /* code / 255 in double, rounded once to float: the conversion back to
     * UNORM8 rounds to the code (checked at open by the self test) */
    c[0] = (float)((double)(v & 0xFFu) / 255.0);
    c[1] = (float)((double)((v >> 8) & 0xFFu) / 255.0);
    c[2] = (float)((double)((v >> 16) & 0xFFu) / 255.0);
    c[3] = 1.0f;
}

static void yscr__d3d_draw_codes(ID3D11DeviceContext* dc, ID3D11DeviceContext1* dc1, ID3D11RenderTargetView* rtv,
                                   ID3D11Resource* tex, const yscr_code_draw* codes, int n) {
    int i, k;
    for (i = 0; i < n; i++) {
        const yscr_code_draw* c = &codes[i];
        D3D11_RECT r;
        float col[4];
        if (!c->px) {
            r.left = c->x; r.top = c->y; r.right = c->x + c->w; r.bottom = c->y + c->h;
            yscr__d3d_color(c->value, col);
            YSCR__CALL(dc1, ClearView, (ID3D11View*)rtv, col, &r, 1);
        } else if (c->w <= yscr__row_clear_max || !tex) {
            for (k = 0; k < c->w; k++) {
                r.left = c->x + k; r.top = c->y; r.right = c->x + k + 1; r.bottom = c->y + 1;
                yscr__d3d_color(c->px[k], col);
                YSCR__CALL(dc1, ClearView, (ID3D11View*)rtv, col, &r, 1);
            }
        } else {
            uint32_t row[YSCR_CODE_ROW_PIXELS];
            D3D11_BOX b;
            int w = c->w < YSCR_CODE_ROW_PIXELS ? c->w : YSCR_CODE_ROW_PIXELS;
            for (k = 0; k < w; k++) row[k] = 0xFF000000u | (c->px[k] & 0xFFFFFFu);   /* RGBA8 in memory */
            b.left = (UINT)c->x; b.right = (UINT)(c->x + w); b.top = (UINT)c->y; b.bottom = (UINT)(c->y + 1);
            b.front = 0; b.back = 1;
            YSCR__CALL(dc, UpdateSubresource, tex, 0, &b, row, (UINT)(w * 4), 0);
        }
    }
}

/* The read-back: the first row of each code is copied to a staging texture
 * on one present and mapped on the next, when the GPU has long finished,
 * so the frame loop never waits on it. */
static void yscr__d3d_verify_check(ID3D11DeviceContext* dc, yscr__d3dcodes* q) {
    D3D11_MAPPED_SUBRESOURCE m;
    int i, k, x = 0;
    if (!q->staged || !q->staging) return;
    if (SUCCEEDED(YSCR__CALL(dc, Map, (ID3D11Resource*)q->staging, 0, D3D11_MAP_READ, 0, &m))) {
        const uint32_t* row = (const uint32_t*)m.pData;
        for (i = 0; i < q->n_staged; i++) {
            const yscr_code_draw* c = &q->staged[i];
            int w = c->px ? c->w : (c->w < 64 ? c->w : 64), bad = 0;
            for (k = 0; k < w && x + k < YSCR__STAGE_W; k++) {
                uint32_t want = c->px ? c->px[k] : c->value;
                if ((row[x + k] & 0xFFFFFFu) != (want & 0xFFFFFFu)) bad = 1;
            }
            x += w;
            if (q->checked) (*q->checked)++;
            if (bad && q->failed) (*q->failed)++;
        }
        YSCR__CALL(dc, Unmap, (ID3D11Resource*)q->staging, 0);
    }
    q->staged = NULL;
}

static void yscr__d3d_verify_copy(ID3D11DeviceContext* dc, yscr__d3dcodes* q, ID3D11Resource* tex,
                                    const yscr_code_draw* codes, int n) {
    int i, x = 0;
    if (!q->staging || !tex || n < 1) return;
    for (i = 0; i < n; i++) {
        const yscr_code_draw* c = &codes[i];
        int w = c->px ? c->w : (c->w < 64 ? c->w : 64);
        D3D11_BOX b;
        if (x + w > YSCR__STAGE_W) w = YSCR__STAGE_W - x;
        if (w <= 0) break;
        b.left = (UINT)c->x; b.right = (UINT)(c->x + w); b.top = (UINT)c->y; b.bottom = (UINT)(c->y + 1);
        b.front = 0; b.back = 1;
        YSCR__CALL(dc, CopySubresourceRegion, (ID3D11Resource*)q->staging, 0, (UINT)x, 0, 0, tex, 0, &b);
        x += w;
    }
    q->staged = codes;
    q->n_staged = n;
}

/* Open: the fence, the staging texture, and the self test: every value of
 * every channel through both drawing paths, read back. */
static void yscr__d3d_codes_open(ID3D11Device* dev, ID3D11DeviceContext* dc, ID3D11DeviceContext1* dc1,
                                   const yscr_presenter_open* in, yscr__d3dcodes* q) {
    ID3D11Device5* dev5 = NULL;
    memset(q, 0, sizeof *q);
    q->checked = in->code_checked;
    q->failed = in->code_failed;
    if (in->want_gpu_done) {
        YSCR__CALL(dev, QueryInterface, YSCR__IID(yscr__IID_ID3D11Device5), (void**)&dev5);
        YSCR__CALL(dc, QueryInterface, YSCR__IID(yscr__IID_ID3D11DeviceContext4), (void**)&q->dctx4);
        if (dev5 && q->dctx4)
            YSCR__CALL(dev5, CreateFence, 0, D3D11_FENCE_FLAG_NONE, YSCR__IID(yscr__IID_ID3D11Fence), (void**)&q->fence);
        YSCR__RELEASE(dev5);
        if (!q->fence) YSCR__RELEASE(q->dctx4);
    }
    if (in->n_codes > 0 && dc1) {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D *t = NULL, *st = NULL;
        ID3D11RenderTargetView* rv = NULL;
        memset(&td, 0, sizeof td);
        td.Width = YSCR__STAGE_W; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        YSCR__CALL(dev, CreateTexture2D, &td, NULL, &q->staging);
        /* self test on a 256 x 2 target: row 0 by ClearView, row 1 by
         * UpdateSubresource, each pixel a different value per channel */
        td.Width = 256; td.Height = 2; td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        YSCR__CALL(dev, CreateTexture2D, &td, NULL, &t);
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.BindFlags = 0;
        YSCR__CALL(dev, CreateTexture2D, &td, NULL, &st);
        if (t) YSCR__CALL(dev, CreateRenderTargetView, (ID3D11Resource*)t, NULL, &rv);
        if (t && st && rv && in->code_selftest) {
            uint32_t px[256];
            yscr_code_draw d[2];
            D3D11_MAPPED_SUBRESOURCE m;
            int i, bad = 0, keep = yscr__row_clear_max;
            for (i = 0; i < 256; i++)
                px[i] = (uint32_t)i | ((uint32_t)(255 - i) << 8) | ((uint32_t)((i * 37 + 11) & 255) << 16);
            d[0].x = 0; d[0].y = 0; d[0].w = 256; d[0].h = 1; d[0].value = 0; d[0].px = px;
            d[1] = d[0]; d[1].y = 1;
            yscr__row_clear_max = 256;   /* row 0 by ClearView           */
            yscr__d3d_draw_codes(dc, dc1, rv, (ID3D11Resource*)t, &d[0], 1);
            yscr__row_clear_max = 0;     /* row 1 by UpdateSubresource   */
            yscr__d3d_draw_codes(dc, dc1, rv, (ID3D11Resource*)t, &d[1], 1);
            yscr__row_clear_max = keep;
            YSCR__CALL(dc, CopyResource, (ID3D11Resource*)st, (ID3D11Resource*)t);
            if (SUCCEEDED(YSCR__CALL(dc, Map, (ID3D11Resource*)st, 0, D3D11_MAP_READ, 0, &m))) {
                const unsigned char* b = (const unsigned char*)m.pData;
                int y;
                for (y = 0; y < 2; y++) {
                    const uint32_t* row = (const uint32_t*)(b + (size_t)y * m.RowPitch);
                    for (i = 0; i < 256; i++) if ((row[i] & 0xFFFFFFu) != px[i]) bad++;
                }
                YSCR__CALL(dc, Unmap, (ID3D11Resource*)st, 0);
                *in->code_selftest = bad ? -1 : 1;
            } else {
                *in->code_selftest = -1;
            }
        }
        YSCR__RELEASE(rv);
        YSCR__RELEASE(st);
        YSCR__RELEASE(t);
    }
}

static void yscr__d3d_codes_close(yscr__d3dcodes* q) {
    YSCR__RELEASE(q->staging);
    YSCR__RELEASE(q->fence);
    YSCR__RELEASE(q->dctx4);
    q->staged = NULL;
}

/* After ANGLE's flush: the patch, then the codes, the read-back, and the
 * fence that tells the trigger worker this present's GPU work is done. */
static void yscr__d3d_finish_frame(ID3D11DeviceContext* dc, ID3D11DeviceContext1* dc1, ID3D11RenderTargetView* rtv,
                                     ID3D11Resource* tex, yscr__d3dcodes* q, const yscr_present_req* req) {
    yscr__d3d_verify_check(dc, q);
    if (req->patch_on && dc1 && rtv) {
        D3D11_RECT r;
        float c[4];
        r.left = req->patch_x; r.top = req->patch_y;
        r.right = req->patch_x + req->patch_w; r.bottom = req->patch_y + req->patch_h;
        c[0] = c[1] = c[2] = req->patch_value; c[3] = 1.0f;
        YSCR__CALL(dc1, ClearView, (ID3D11View*)rtv, c, &r, 1);
    }
    if (req->n_codes > 0 && dc1 && rtv) {
        YRT_ZONE(z_codes, "yscr.codes");
        yscr__d3d_draw_codes(dc, dc1, rtv, tex, req->codes, req->n_codes);
        if (req->verify) yscr__d3d_verify_copy(dc, q, tex, req->codes, req->n_codes);
        YRT_ZONE_END(z_codes);
    }
    if (q->fence) YSCR__CALL(q->dctx4, Signal, q->fence, req->present_id);
}

static uint64_t yscr__d3d_gpu_done(yscr__d3dcodes* q) {
    return q->fence ? (uint64_t)YSCR__CALL0(q->fence, GetCompletedValue) : 0;
}

typedef HRESULT (WINAPI *yscr__D3D11CreateDevice_fn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT (WINAPI *yscr__CreateDXGIFactory1_fn)(REFIID, void**);
typedef HMONITOR (WINAPI *yscr__MonitorFromWindow_fn)(HWND, DWORD);

/* EGL entry points, loaded once per process from ANGLE's libEGL. */
typedef yscr_proc (YSCR__APIENTRY *yscr__eglGetProcAddress_fn)(const char*);
typedef void* (YSCR__APIENTRY *yscr__eglCreateDeviceANGLE_fn)(yscr__EGLint, void*, const intptr_t*);
typedef unsigned (YSCR__APIENTRY *yscr__eglReleaseDeviceANGLE_fn)(void*);
typedef yscr__EGLDisplay (YSCR__APIENTRY *yscr__eglGetPlatformDisplayEXT_fn)(unsigned, void*, const yscr__EGLint*);
typedef unsigned (YSCR__APIENTRY *yscr__eglInitialize_fn)(yscr__EGLDisplay, yscr__EGLint*, yscr__EGLint*);
typedef unsigned (YSCR__APIENTRY *yscr__eglTerminate_fn)(yscr__EGLDisplay);
typedef unsigned (YSCR__APIENTRY *yscr__eglChooseConfig_fn)(yscr__EGLDisplay, const yscr__EGLint*, yscr__EGLConfig*, yscr__EGLint, yscr__EGLint*);
typedef yscr__EGLContext (YSCR__APIENTRY *yscr__eglCreateContext_fn)(yscr__EGLDisplay, yscr__EGLConfig, yscr__EGLContext, const yscr__EGLint*);
typedef unsigned (YSCR__APIENTRY *yscr__eglDestroyContext_fn)(yscr__EGLDisplay, yscr__EGLContext);
typedef yscr__EGLSurface (YSCR__APIENTRY *yscr__eglCreatePbufferFromClientBuffer_fn)(yscr__EGLDisplay, unsigned, void*, yscr__EGLConfig, const yscr__EGLint*);
typedef unsigned (YSCR__APIENTRY *yscr__eglDestroySurface_fn)(yscr__EGLDisplay, yscr__EGLSurface);
typedef unsigned (YSCR__APIENTRY *yscr__eglMakeCurrent_fn)(yscr__EGLDisplay, yscr__EGLSurface, yscr__EGLSurface, yscr__EGLContext);
typedef yscr__EGLint (YSCR__APIENTRY *yscr__eglGetError_fn)(void);

#define YSCR__EGL_NONE                 0x3038
#define YSCR__EGL_RED_SIZE             0x3024
#define YSCR__EGL_GREEN_SIZE           0x3023
#define YSCR__EGL_BLUE_SIZE            0x3022
#define YSCR__EGL_ALPHA_SIZE           0x3021
#define YSCR__EGL_SURFACE_TYPE         0x3033
#define YSCR__EGL_PBUFFER_BIT          0x0001
#define YSCR__EGL_RENDERABLE_TYPE      0x3040
#define YSCR__EGL_OPENGL_ES3_BIT       0x0040
#define YSCR__EGL_CONTEXT_MAJOR        0x3098
#define YSCR__EGL_CONTEXT_MINOR        0x30FB
#define YSCR__EGL_D3D11_DEVICE_ANGLE   0x33A1
#define YSCR__EGL_D3D_TEXTURE_ANGLE    0x33A3
#define YSCR__EGL_PLATFORM_DEVICE_EXT  0x313F

typedef struct yscr__egl_api {
    HMODULE lib;
    int     tried;
    yscr__eglGetProcAddress_fn               GetProcAddress;
    yscr__eglCreateDeviceANGLE_fn            CreateDeviceANGLE;
    yscr__eglReleaseDeviceANGLE_fn           ReleaseDeviceANGLE;
    yscr__eglGetPlatformDisplayEXT_fn        GetPlatformDisplayEXT;
    yscr__eglInitialize_fn                   Initialize;
    yscr__eglTerminate_fn                    Terminate;
    yscr__eglChooseConfig_fn                 ChooseConfig;
    yscr__eglCreateContext_fn                CreateContext;
    yscr__eglDestroyContext_fn               DestroyContext;
    yscr__eglCreatePbufferFromClientBuffer_fn CreatePbufferFromClientBuffer;
    yscr__eglDestroySurface_fn               DestroySurface;
    yscr__eglMakeCurrent_fn                  MakeCurrent;
    yscr__eglGetError_fn                     GetError;
} yscr__egl_api;

/* Process-wide: libEGL is loaded once and never unloaded, because ANGLE
 * keeps per-process state that a reload would not restore. Open screens
 * from one thread. */
static yscr__egl_api yscr__egl;

static yscr_proc yscr__egl_sym(const char* name) {
    yscr_proc p = yscr__egl.GetProcAddress ? yscr__egl.GetProcAddress(name) : NULL;
    if (!p && yscr__egl.lib) p = (yscr_proc)GetProcAddress(yscr__egl.lib, name);
    return p;
}

static bool yscr__egl_load(const char* dir, char* err, size_t cap) {
    if (yscr__egl.lib) return true;
    {
        wchar_t path[MAX_PATH];
        HMODULE lib = NULL;
        int n = 0;
        if (dir && dir[0]) n = MultiByteToWideChar(CP_UTF8, 0, dir, -1, path, MAX_PATH - 16) - 1;
        else n = (int)GetEnvironmentVariableW(L"YSCR_ANGLE_DIR", path, MAX_PATH - 16);
        if (n > 0 && n < MAX_PATH - 16) {
            static const wchar_t name[] = L"\\libEGL.dll";
            memcpy(path + n, name, sizeof name);
            /* The libGLESv2.dll beside it, not one from PATH. */
            lib = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        }
        if (!lib && !(dir && dir[0])) lib = LoadLibraryW(L"libEGL.dll");
        if (!lib) {
            yscr__set_error(err, cap, "ysp_screen: ANGLE's libEGL.dll not found (desc.angle_dir, "
                              "YSCR_ANGLE_DIR, the program's directory); error %lu",
                              (unsigned long)GetLastError());
            return false;
        }
        yscr__egl.lib = lib;
    }
    yscr__egl.GetProcAddress = (yscr__eglGetProcAddress_fn)(yscr_proc)GetProcAddress(yscr__egl.lib, "eglGetProcAddress");
    yscr__egl.CreateDeviceANGLE = (yscr__eglCreateDeviceANGLE_fn)yscr__egl_sym("eglCreateDeviceANGLE");
    yscr__egl.ReleaseDeviceANGLE = (yscr__eglReleaseDeviceANGLE_fn)yscr__egl_sym("eglReleaseDeviceANGLE");
    yscr__egl.GetPlatformDisplayEXT = (yscr__eglGetPlatformDisplayEXT_fn)yscr__egl_sym("eglGetPlatformDisplayEXT");
    yscr__egl.Initialize = (yscr__eglInitialize_fn)yscr__egl_sym("eglInitialize");
    yscr__egl.Terminate = (yscr__eglTerminate_fn)yscr__egl_sym("eglTerminate");
    yscr__egl.ChooseConfig = (yscr__eglChooseConfig_fn)yscr__egl_sym("eglChooseConfig");
    yscr__egl.CreateContext = (yscr__eglCreateContext_fn)yscr__egl_sym("eglCreateContext");
    yscr__egl.DestroyContext = (yscr__eglDestroyContext_fn)yscr__egl_sym("eglDestroyContext");
    yscr__egl.CreatePbufferFromClientBuffer = (yscr__eglCreatePbufferFromClientBuffer_fn)yscr__egl_sym("eglCreatePbufferFromClientBuffer");
    yscr__egl.DestroySurface = (yscr__eglDestroySurface_fn)yscr__egl_sym("eglDestroySurface");
    yscr__egl.MakeCurrent = (yscr__eglMakeCurrent_fn)yscr__egl_sym("eglMakeCurrent");
    yscr__egl.GetError = (yscr__eglGetError_fn)yscr__egl_sym("eglGetError");
    if (!yscr__egl.CreateDeviceANGLE || !yscr__egl.GetPlatformDisplayEXT || !yscr__egl.Initialize ||
        !yscr__egl.ChooseConfig || !yscr__egl.CreateContext || !yscr__egl.MakeCurrent ||
        !yscr__egl.CreatePbufferFromClientBuffer || !yscr__egl.GetError) {
        yscr__set_error(err, cap, "ysp_screen: the libEGL.dll found is not ANGLE with "
                          "EGL_ANGLE_device_creation_d3d11 and EGL_ANGLE_d3d_texture_client_buffer");
        yscr__egl.lib = NULL;
        return false;
    }
    return true;
}

typedef struct yscr__dxgi {
    HWND                 hwnd;
    ID3D11Device*        dev;
    ID3D11DeviceContext* dctx;
    ID3D11DeviceContext1* dctx1;
    ID3D11RenderTargetView* rtv;
    ID3D11Texture2D*     tex;      /* buffer 0: the current back buffer      */
    yscr__d3dcodes     q;
    IDXGISwapChain1*     sc;
    IDXGISwapChainMedia* media;
    HANDLE               waitable;
    void*                edev;
    yscr__EGLDisplay   dpy;
    yscr__EGLContext   ctx;
    yscr__EGLSurface   pb;
    yscr__glFlush_fn   flush;
    UINT                 last_reported;
    UINT                 map_dxgi[8];
    uint64_t             map_id[8];
    int                  map_next;
    int64_t              period_ns;
    uint8_t              path;
    uint16_t             occluded;
    int                  w, h;
    char                 adapter[64];
    char                 angle[64];
} yscr__dxgi;

typedef char yscr__dxgi_fits[sizeof(yscr__dxgi) <= sizeof(((yscr_screen*)0)->backend_mem) ? 1 : -1];

static void yscr__dxgi_close(void* ctx);

static int yscr__dxgi_open(void* vctx, const yscr_presenter_open* in, yscr_caps* caps,
                             char* err, size_t err_cap) {
    yscr__dxgi* d = (yscr__dxgi*)vctx;
    HMODULE d3d11 = NULL, dxgi = NULL, user32 = NULL;
    yscr__D3D11CreateDevice_fn create_device;
    yscr__CreateDXGIFactory1_fn create_factory;
    yscr__MonitorFromWindow_fn monitor_from_window;
    IDXGIFactory1* fac1 = NULL;
    IDXGIFactory2* fac2 = NULL;
    IDXGIFactory5* fac5 = NULL;
    IDXGIAdapter1* pick = NULL;
    IDXGISwapChain2* sc2 = NULL;
    ID3D11Texture2D* tex = NULL;
    HMONITOR mon;
    HRESULT hr;
    UINT i, j;
    DXGI_SWAP_CHAIN_DESC1 sd;
    D3D_FEATURE_LEVEL levels[4];
    yscr__EGLint attrs[16], n_cfg = 0, egl_major = 0, egl_minor = 0;
    yscr__EGLConfig cfg = NULL;
    yscr__glGetString_fn get_string;

    memset(d, 0, sizeof *d);
    if (!in->window) { yscr__set_error(err, err_cap, "ysp_screen: no window"); return YSCR_ERR_ARG; }
    d->hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(in->window),
                                           SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!d->hwnd) { yscr__set_error(err, err_cap, "ysp_screen: SDL gave no HWND"); return YSCR_ERR_LOST; }
    SDL_GetWindowSizeInPixels(in->window, &d->w, &d->h);
    d->period_ns = in->mode && in->mode->period_ns > 0 ? in->mode->period_ns : 16666667;


    d3d11 = LoadLibraryW(L"d3d11.dll");
    dxgi = LoadLibraryW(L"dxgi.dll");
    user32 = LoadLibraryW(L"user32.dll");
    create_device = d3d11 ? (yscr__D3D11CreateDevice_fn)(yscr_proc)GetProcAddress(d3d11, "D3D11CreateDevice") : NULL;
    create_factory = dxgi ? (yscr__CreateDXGIFactory1_fn)(yscr_proc)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
    monitor_from_window = user32 ? (yscr__MonitorFromWindow_fn)(yscr_proc)GetProcAddress(user32, "MonitorFromWindow") : NULL;
    if (!create_device || !create_factory || !monitor_from_window) {
        yscr__set_error(err, err_cap, "ysp_screen: d3d11.dll or dxgi.dll missing");
        return YSCR_ERR_NOT_IMPLEMENTED;
    }
    hr = create_factory(YSCR__IID(yscr__IID_IDXGIFactory1), (void**)&fac1);
    if (FAILED(hr)) { yscr__set_error(err, err_cap, "ysp_screen: CreateDXGIFactory1 0x%08lx", (unsigned long)hr); return YSCR_ERR_LOST; }

    /* The adapter whose output holds the window: on a hybrid laptop that is
     * the one wired to the panel, and any other forces a cross-adapter copy
     * and composition. */
    mon = monitor_from_window(d->hwnd, 1 /* MONITOR_DEFAULTTOPRIMARY */);
    for (i = 0; !pick; i++) {
        IDXGIAdapter1* ad = NULL;
        if (YSCR__CALL(fac1, EnumAdapters1, i, &ad) != S_OK) break;
        for (j = 0; ; j++) {
            IDXGIOutput* o = NULL;
            DXGI_OUTPUT_DESC od;
            if (YSCR__CALL(ad, EnumOutputs, j, &o) != S_OK) break;
            YSCR__CALL(o, GetDesc, &od);
            YSCR__RELEASE(o);
            if (od.Monitor == mon) { pick = ad; break; }
        }
        if (pick != ad) YSCR__RELEASE(ad);
    }
    if (pick) {
        DXGI_ADAPTER_DESC1 ad;
        YSCR__CALL(pick, GetDesc1, &ad);
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, d->adapter, (int)sizeof d->adapter - 1, NULL, NULL);
    }
    levels[0] = D3D_FEATURE_LEVEL_11_1; levels[1] = D3D_FEATURE_LEVEL_11_0;
    levels[2] = D3D_FEATURE_LEVEL_10_1; levels[3] = D3D_FEATURE_LEVEL_10_0;
    hr = create_device((IDXGIAdapter*)pick, pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL,
                       yscr__device_flags(in), levels, 4, D3D11_SDK_VERSION, &d->dev, NULL, &d->dctx);
    if (hr == E_INVALIDARG)   /* 11.1 unknown to the runtime */
        hr = create_device((IDXGIAdapter*)pick, pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL,
                           yscr__device_flags(in), levels + 1, 3, D3D11_SDK_VERSION, &d->dev, NULL, &d->dctx);
    YSCR__RELEASE(pick);
    if (FAILED(hr)) {
        YSCR__RELEASE(fac1);
        yscr__device_error(in, hr, err, err_cap);
        return YSCR_ERR_LOST;
    }
    yscr__device_protect(in, d->dev);
    {   /* the factory that owns the device's adapter */
        IDXGIDevice* dd = NULL;
        IDXGIAdapter* a = NULL;
        YSCR__CALL(d->dev, QueryInterface, YSCR__IID(yscr__IID_IDXGIDevice), (void**)&dd);
        if (dd) YSCR__CALL(dd, GetAdapter, &a);
        if (a) YSCR__CALL(a, GetParent, YSCR__IID(yscr__IID_IDXGIFactory2), (void**)&fac2);
        YSCR__RELEASE(a);
        YSCR__RELEASE(dd);
    }
    YSCR__RELEASE(fac1);
    if (!fac2) {
        yscr__dxgi_close(d);
        yscr__set_error(err, err_cap, "ysp_screen: no DXGI 1.2 factory (Windows 8 or later needed)");
        return YSCR_ERR_NOT_IMPLEMENTED;
    }
    if (SUCCEEDED(YSCR__CALL(fac2, QueryInterface, YSCR__IID(yscr__IID_IDXGIFactory5), (void**)&fac5)) && fac5) {
        BOOL tearing = FALSE;
        if (SUCCEEDED(YSCR__CALL(fac5, CheckFeatureSupport, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, (UINT)sizeof tearing)))
            caps->vrr_capable = tearing != FALSE;
        YSCR__RELEASE(fac5);
    }

    memset(&sd, 0, sizeof sd);
    sd.Width = (UINT)d->w;
    sd.Height = (UINT)d->h;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_NONE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    hr = YSCR__CALL(fac2, CreateSwapChainForHwnd, (IUnknown*)d->dev, d->hwnd, &sd, NULL, NULL, &d->sc);
    if (SUCCEEDED(hr)) YSCR__CALL(fac2, MakeWindowAssociation, d->hwnd, DXGI_MWA_NO_ALT_ENTER);
    YSCR__RELEASE(fac2);
    if (FAILED(hr)) {
        yscr__dxgi_close(d);
        yscr__set_error(err, err_cap, "ysp_screen: CreateSwapChainForHwnd (flip discard) 0x%08lx", (unsigned long)hr);
        return YSCR_ERR_LOST;
    }
    YSCR__CALL(d->sc, QueryInterface, YSCR__IID(yscr__IID_IDXGISwapChain2), (void**)&sc2);
    if (!sc2) {
        yscr__dxgi_close(d);
        yscr__set_error(err, err_cap, "ysp_screen: no IDXGISwapChain2 (Windows 8.1 or later needed)");
        return YSCR_ERR_NOT_IMPLEMENTED;
    }
    /* One frame in flight: the waitable object signals when a slot frees. */
    YSCR__CALL(sc2, SetMaximumFrameLatency, 1);
    d->waitable = YSCR__CALL0(sc2, GetFrameLatencyWaitableObject);
    YSCR__RELEASE(sc2);
    YSCR__CALL(d->sc, QueryInterface, YSCR__IID(yscr__IID_IDXGISwapChainMedia), (void**)&d->media);

    if (!yscr__egl_load(in->angle_dir, err, err_cap)) { yscr__dxgi_close(d); return YSCR_ERR_NOT_IMPLEMENTED; }
    d->edev = yscr__egl.CreateDeviceANGLE(YSCR__EGL_D3D11_DEVICE_ANGLE, d->dev, NULL);
    d->dpy = d->edev ? yscr__egl.GetPlatformDisplayEXT(YSCR__EGL_PLATFORM_DEVICE_EXT, d->edev, NULL) : NULL;
    if (!d->dpy || !yscr__egl.Initialize(d->dpy, &egl_major, &egl_minor)) {
        int e = yscr__egl.GetError();
        yscr__dxgi_close(d);
        yscr__set_error(err, err_cap, "ysp_screen: ANGLE display on the D3D11 device failed (EGL 0x%x)", e);
        return YSCR_ERR_LOST;
    }
    attrs[0] = YSCR__EGL_RED_SIZE; attrs[1] = 8; attrs[2] = YSCR__EGL_GREEN_SIZE; attrs[3] = 8;
    attrs[4] = YSCR__EGL_BLUE_SIZE; attrs[5] = 8; attrs[6] = YSCR__EGL_ALPHA_SIZE; attrs[7] = 8;
    attrs[8] = YSCR__EGL_SURFACE_TYPE; attrs[9] = YSCR__EGL_PBUFFER_BIT;
    attrs[10] = YSCR__EGL_RENDERABLE_TYPE; attrs[11] = YSCR__EGL_OPENGL_ES3_BIT;
    attrs[12] = YSCR__EGL_NONE;
    if (!yscr__egl.ChooseConfig(d->dpy, attrs, &cfg, 1, &n_cfg) || n_cfg < 1) {
        yscr__dxgi_close(d);
        yscr__set_error(err, err_cap, "ysp_screen: no RGBA8 ES 3.0 pbuffer config");
        return YSCR_ERR_LOST;
    }
    attrs[0] = YSCR__EGL_CONTEXT_MAJOR; attrs[1] = 3; attrs[2] = YSCR__EGL_CONTEXT_MINOR; attrs[3] = 0;
    attrs[4] = YSCR__EGL_NONE;
    d->ctx = yscr__egl.CreateContext(d->dpy, cfg, NULL, attrs);
    /* D3D11 flip model exposes only buffer 0, and it always names the
     * current back buffer, so one pbuffer follows the rotation. */
    YSCR__CALL(d->sc, GetBuffer, 0, YSCR__IID(yscr__IID_ID3D11Texture2D), (void**)&tex);
    /* The patch is a ClearView with a rectangle on the same buffer, after
     * ANGLE's flush: a scissored clear that never touches GL state. */
    YSCR__CALL(d->dctx, QueryInterface, YSCR__IID(yscr__IID_ID3D11DeviceContext1), (void**)&d->dctx1);
    if (tex && d->dctx1) YSCR__CALL(d->dev, CreateRenderTargetView, (ID3D11Resource*)tex, NULL, &d->rtv);
    attrs[0] = YSCR__EGL_NONE;
    d->pb = (d->ctx && tex) ? yscr__egl.CreatePbufferFromClientBuffer(d->dpy, YSCR__EGL_D3D_TEXTURE_ANGLE, tex, cfg, attrs) : NULL;
    d->tex = tex;   /* kept: a ROW code is an UpdateSubresource on it */
    tex = NULL;
    if (!d->ctx || !d->pb || !yscr__egl.MakeCurrent(d->dpy, d->pb, d->pb, d->ctx)) {
        int e = yscr__egl.GetError();
        yscr__dxgi_close(d);
        yscr__set_error(err, err_cap, "ysp_screen: ES 3.0 context on the swapchain buffer failed (EGL 0x%x)", e);
        return YSCR_ERR_LOST;
    }
    d->flush = (yscr__glFlush_fn)yscr__egl_sym("glFlush");
    get_string = (yscr__glGetString_fn)yscr__egl_sym("glGetString");
    if (get_string) {
        const char* v = (const char*)get_string(YSCR__GL_VERSION);
        const char* a = v ? strstr(v, "ANGLE ") : NULL;
        yscr__copy(d->angle, sizeof d->angle, a ? a + 6 : (v ? v : "?"));
        {   /* "2.1.23876 git hash: fffbc739779a)" to "2.1.23876/fffbc739779a" */
            char* p = strstr(d->angle, " git hash: ");
            if (p) { memmove(p + 1, p + 11, strlen(p + 11) + 1); *p = '/'; }
            p = strchr(d->angle, ')');
            if (p) *p = '\0';
        }
    }
    caps->kind = YSCR_FIXED_GRID;
    caps->hw_onset = true;
    caps->native_target = false;
    caps->max_in_flight = 1;
    d->path = YSCR_PATH_UNKNOWN;
    yscr__d3d_codes_open(d->dev, d->dctx, d->dctx1, in, &d->q);
    return YSCR_OK;
}

static void yscr__dxgi_close(void* vctx) {
    yscr__dxgi* d = (yscr__dxgi*)vctx;
    if (d->dpy) {
        yscr__egl.MakeCurrent(d->dpy, NULL, NULL, NULL);
        if (d->pb) yscr__egl.DestroySurface(d->dpy, d->pb);
        if (d->ctx) yscr__egl.DestroyContext(d->dpy, d->ctx);
        if (yscr__egl.Terminate) yscr__egl.Terminate(d->dpy);
    }
    if (d->edev && yscr__egl.ReleaseDeviceANGLE) yscr__egl.ReleaseDeviceANGLE(d->edev);
    if (d->waitable) CloseHandle(d->waitable);
    yscr__d3d_codes_close(&d->q);
    YSCR__RELEASE(d->tex);
    YSCR__RELEASE(d->rtv);
    YSCR__RELEASE(d->dctx1);
    YSCR__RELEASE(d->media);
    YSCR__RELEASE(d->sc);
    YSCR__RELEASE(d->dctx);
    YSCR__RELEASE(d->dev);
    d->dpy = NULL; d->pb = NULL; d->ctx = NULL; d->edev = NULL; d->waitable = NULL;
}

static void yscr__dxgi_bind(void* vctx) {
    yscr__dxgi* d = (yscr__dxgi*)vctx;
    yscr__egl.MakeCurrent(d->dpy, d->pb, d->pb, d->ctx);
}

static int yscr__dxgi_acquire(void* vctx, int64_t deadline_ns, yscr_vblank* newest) {
    yscr__dxgi* d = (yscr__dxgi*)vctx;
    int64_t left = deadline_ns - yscr__now();
    DWORD ms = left <= 0 ? 0 : (DWORD)((left + 999999) / 1000000);
    DWORD rc = WaitForSingleObjectEx(d->waitable, ms, FALSE);
    newest->t_ns = 0;
    yscr__dxgi_bind(d);
    return rc == WAIT_OBJECT_0 ? YSCR_OK : YSCR_ERR_TIMEOUT;
}

static int yscr__dxgi_present(void* vctx, const yscr_present_req* req) {
    yscr__dxgi* d = (yscr__dxgi*)vctx;
    UINT count = 0, hold = req->hold < 1 ? 1u : (req->hold > 4 ? 4u : (UINT)req->hold);
    HRESULT hr;
    if (d->flush) d->flush();
    yscr__d3d_finish_frame(d->dctx, d->dctx1, d->rtv, (ID3D11Resource*)d->tex, &d->q, req);
    hr = YSCR__CALL(d->sc, Present, hold, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) return YSCR_ERR_LOST;
    d->occluded = hr == DXGI_STATUS_OCCLUDED;
    if (FAILED(hr)) return YSCR_ERR_LOST;
    YSCR__CALL(d->sc, GetLastPresentCount, &count);
    d->map_dxgi[d->map_next & 7] = count;
    d->map_id[d->map_next & 7] = req->present_id;
    d->map_next++;
    return YSCR_OK;
}

static int yscr__dxgi_completions(void* vctx, yscr_vblank* out, int cap) {
    yscr__dxgi* d = (yscr__dxgi*)vctx;
    DXGI_FRAME_STATISTICS st;
    UINT present_count, present_refresh, sync_refresh;
    int64_t sync_qpc;
    int k;
    uint8_t path = d->path;
    if (cap < 1) return 0;
    if (d->media) {
        DXGI_FRAME_STATISTICS_MEDIA sm;
        if (FAILED(YSCR__CALL(d->media, GetFrameStatisticsMedia, &sm))) return 0;
        present_count = sm.PresentCount;
        present_refresh = sm.PresentRefreshCount;
        sync_refresh = sm.SyncRefreshCount;
        sync_qpc = (int64_t)sm.SyncQPCTime.QuadPart;
        switch (sm.CompositionMode) {
        case DXGI_FRAME_PRESENTATION_MODE_COMPOSED: path = YSCR_PATH_COMPOSED; break;
        case DXGI_FRAME_PRESENTATION_MODE_OVERLAY:  path = YSCR_PATH_OVERLAY; break;
        case DXGI_FRAME_PRESENTATION_MODE_NONE:     path = YSCR_PATH_INDEPENDENT; break;
        default:                                    path = YSCR_PATH_UNKNOWN; break;
        }
    } else {
        if (FAILED(YSCR__CALL(d->sc, GetFrameStatistics, &st))) return 0;
        present_count = st.PresentCount;
        present_refresh = st.PresentRefreshCount;
        sync_refresh = st.SyncRefreshCount;
        sync_qpc = (int64_t)st.SyncQPCTime.QuadPart;
    }
    if (present_count == 0 || present_count == d->last_reported) return 0;
    d->last_reported = present_count;
    d->path = path;
    out[0].present_id = 0;
    for (k = 0; k < 8; k++)
        if (d->map_dxgi[k] == present_count && d->map_id[k]) { out[0].present_id = d->map_id[k]; break; }
    /* SyncQPCTime is the vblank of SyncRefreshCount. The present's own
     * vblank is the same one unless the statistic was read late; then the
     * mode's period backs it out, and the core's grid check bounds it. */
    out[0].count = (int64_t)present_refresh;
    /* SyncQPCTime is QPC, the counter yrt_now_ns() reads: the same clock,
     * converted by the same arithmetic, with no correlation. */
    out[0].t_ns = yrt_ticks_to_ns(sync_qpc) -
                  (int64_t)(int32_t)(sync_refresh - present_refresh) * d->period_ns;
    out[0].flags = 0;
    out[0].path = path;
    if (d->occluded) out[0].flags |= YSCR_FLIP_OCCLUDED;
    out[0].reserved_ = 0;
    return 1;
}

static int yscr__dxgi_describe(void* vctx, char* buf, size_t cap) {
    yscr__dxgi* d = (yscr__dxgi*)vctx;
    return snprintf(buf, cap, "adapter=\"%s\" angle=%s", d->adapter, d->angle);
}

static yscr_proc yscr__dxgi_gl_proc(void* vctx, const char* name) {
    (void)vctx;
    return yscr__egl_sym(name);
}

static uint64_t yscr__dxgi_gpu_done(void* vctx) { return yscr__d3d_gpu_done(&((yscr__dxgi*)vctx)->q); }

static const yscr_presenter yscr__dxgi_presenter = {
    YSCR_PRESENTER_VERSION, "dxgi_flip", true, false, true,
    yscr__dxgi_open, yscr__dxgi_close, yscr__dxgi_acquire, yscr__dxgi_present,
    yscr__dxgi_completions, yscr__dxgi_gl_proc, yscr__dxgi_bind, yscr__dxgi_describe,
    yscr__dxgi_gpu_done
};

/* --- composition swapchain presenter (Windows 11) ------------------------- */

/* Our own declarations of the presentation and DirectComposition
 * interfaces: dcomp.h is C++ only, MinGW-w64 has no presentation.h, and the
 * SDK's C declarations return SystemInterruptTime and LUID by value, which
 * is not how a C++ method returns an aggregate on x64 (a hidden pointer
 * after `this`). Each table holds every slot in the SDK's order up to the
 * last one called; tests/compile/screen_com.cpp checks them against
 * the SDK with spy objects. */
typedef struct yscr__SIT { uint64_t value; } yscr__SIT;
typedef struct yscr__PT { float M11, M12, M21, M22, M31, M32; } yscr__PT;
typedef struct yscr__CFDI {               /* CompositionFrameDisplayInstance */
    LUID     displayAdapterLUID;
    UINT     displayVidPnSourceId;
    UINT     displayUniqueId;
    LUID     renderAdapterLUID;
    int      instanceKind;                  /* 0 composed, 1 scanout, 2 intermediate */
    yscr__PT finalTransform;
    unsigned char requiredCrossAdapterCopy;
    int      colorSpace;
} yscr__CFDI;

/* DirectComposition's frame statistics (dcomptypes.h), Windows 11. */
typedef struct yscr__CFStats { uint64_t startTime, targetTime, framePeriod; } yscr__CFStats;
typedef struct yscr__CTargetId {
    LUID displayAdapterLuid;
    LUID renderAdapterLuid;
    UINT vidPnSourceId, vidPnTargetId, uniqueId;
} yscr__CTargetId;
typedef struct yscr__CStats { UINT presentCount, refreshCount, virtualRefreshCount; uint64_t time; } yscr__CStats;
typedef struct yscr__CTargetStats {
    UINT outstandingPresents;
    uint64_t presentTime;
    uint64_t vblankDuration;
    yscr__CStats presentedStats;
    yscr__CStats completedStats;
} yscr__CTargetStats;
typedef HRESULT (WINAPI *yscr__DCompositionGetStatistics_fn)(uint64_t frame, yscr__CFStats* fs, UINT n,
                                                              yscr__CTargetId* ids, UINT* actual);
typedef HRESULT (WINAPI *yscr__DCompositionGetTargetStatistics_fn)(uint64_t frame, const yscr__CTargetId* id,
                                                                    yscr__CTargetStats* ts);

#define YSCR__IUNK_SLOTS \
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void* self, const IID* iid, void** out); \
    ULONG   (STDMETHODCALLTYPE *AddRef)(void* self); \
    ULONG   (STDMETHODCALLTYPE *Release)(void* self)

typedef struct yscr__PFactoryV {          /* IPresentationFactory */
    YSCR__IUNK_SLOTS;
    unsigned char (STDMETHODCALLTYPE *IsPresentationSupported)(void* self);                     /* 3 */
    unsigned char (STDMETHODCALLTYPE *IsPresentationSupportedWithIndependentFlip)(void* self);  /* 4 */
    HRESULT (STDMETHODCALLTYPE *CreatePresentationManager)(void* self, void** manager);         /* 5 */
} yscr__PFactoryV;

typedef struct yscr__PManagerV {          /* IPresentationManager */
    YSCR__IUNK_SLOTS;
    HRESULT  (STDMETHODCALLTYPE *AddBufferFromResource)(void* self, void* resource, void** buffer);  /* 3 */
    HRESULT  (STDMETHODCALLTYPE *CreatePresentationSurface)(void* self, HANDLE h, void** surface);   /* 4 */
    uint64_t (STDMETHODCALLTYPE *GetNextPresentId)(void* self);                                      /* 5 */
    HRESULT  (STDMETHODCALLTYPE *SetTargetTime)(void* self, yscr__SIT target);                     /* 6 */
    HRESULT  (STDMETHODCALLTYPE *SetPreferredPresentDuration)(void* self, yscr__SIT d, yscr__SIT tol); /* 7 */
    HRESULT  (STDMETHODCALLTYPE *ForceVSyncInterrupt)(void* self, unsigned char force);              /* 8 */
    HRESULT  (STDMETHODCALLTYPE *Present)(void* self);                                               /* 9 */
    HRESULT  (STDMETHODCALLTYPE *GetPresentRetiringFence)(void* self, const IID* iid, void** fence);/* 10 */
    HRESULT  (STDMETHODCALLTYPE *CancelPresentsFrom)(void* self, uint64_t id);                       /* 11 */
    HRESULT  (STDMETHODCALLTYPE *GetLostEvent)(void* self, HANDLE* h);                               /* 12 */
    HRESULT  (STDMETHODCALLTYPE *GetPresentStatisticsAvailableEvent)(void* self, HANDLE* h);         /* 13 */
    HRESULT  (STDMETHODCALLTYPE *EnablePresentStatisticsKind)(void* self, int kind, unsigned char on);/* 14 */
    HRESULT  (STDMETHODCALLTYPE *GetNextPresentStatistics)(void* self, void** stats);                /* 15 */
} yscr__PManagerV;

typedef struct yscr__PBufferV {           /* IPresentationBuffer */
    YSCR__IUNK_SLOTS;
    HRESULT (STDMETHODCALLTYPE *GetAvailableEvent)(void* self, HANDLE* h);                      /* 3 */
    HRESULT (STDMETHODCALLTYPE *IsAvailable)(void* self, unsigned char* available);             /* 4 */
} yscr__PBufferV;

typedef struct yscr__PSurfaceV {          /* IPresentationSurface : IPresentationContent */
    YSCR__IUNK_SLOTS;
    void    (STDMETHODCALLTYPE *SetTag)(void* self, UINT_PTR tag);                               /* 3 */
    HRESULT (STDMETHODCALLTYPE *SetBuffer)(void* self, void* buffer);                            /* 4 */
    HRESULT (STDMETHODCALLTYPE *SetColorSpace)(void* self, int color_space);                     /* 5 */
    HRESULT (STDMETHODCALLTYPE *SetAlphaMode)(void* self, int alpha_mode);                       /* 6 */
    HRESULT (STDMETHODCALLTYPE *SetSourceRect)(void* self, const RECT* r);                       /* 7 */
} yscr__PSurfaceV;

#define YSCR__STATS_SLOTS \
    YSCR__IUNK_SLOTS; \
    uint64_t (STDMETHODCALLTYPE *GetPresentId)(void* self);                                       /* 3 */ \
    int      (STDMETHODCALLTYPE *GetKind)(void* self)                                             /* 4 */

typedef struct yscr__PStatsV { YSCR__STATS_SLOTS; } yscr__PStatsV;

typedef struct yscr__PStatusStatsV {      /* IPresentStatusPresentStatistics */
    YSCR__STATS_SLOTS;
    uint64_t (STDMETHODCALLTYPE *GetCompositionFrameId)(void* self);                              /* 5 */
    int      (STDMETHODCALLTYPE *GetPresentStatus)(void* self);                                   /* 6 */
} yscr__PStatusStatsV;

typedef struct yscr__CompStatsV {         /* ICompositionFramePresentStatistics */
    YSCR__STATS_SLOTS;
    UINT_PTR (STDMETHODCALLTYPE *GetContentTag)(void* self);                                      /* 5 */
    uint64_t (STDMETHODCALLTYPE *GetCompositionFrameId)(void* self);                              /* 6 */
    void     (STDMETHODCALLTYPE *GetDisplayInstanceArray)(void* self, UINT* n, const yscr__CFDI** a); /* 7 */
} yscr__CompStatsV;

typedef struct yscr__IFlipStatsV {        /* IIndependentFlipFramePresentStatistics */
    YSCR__STATS_SLOTS;
    LUID*        (STDMETHODCALLTYPE *GetOutputAdapterLUID)(void* self, LUID* ret);                /* 5 */
    UINT         (STDMETHODCALLTYPE *GetOutputVidPnSourceId)(void* self);                         /* 6 */
    UINT_PTR     (STDMETHODCALLTYPE *GetContentTag)(void* self);                                  /* 7 */
    yscr__SIT* (STDMETHODCALLTYPE *GetDisplayedTime)(void* self, yscr__SIT* ret);             /* 8 */
    yscr__SIT* (STDMETHODCALLTYPE *GetPresentDuration)(void* self, yscr__SIT* ret);           /* 9 */
} yscr__IFlipStatsV;

typedef struct yscr__DCDeviceV {          /* IDCompositionDevice */
    YSCR__IUNK_SLOTS;
    HRESULT (STDMETHODCALLTYPE *Commit)(void* self);                                             /* 3 */
    void*   unused_4_;                      /* WaitForCommitCompletion */
    void*   unused_5_;                      /* GetFrameStatistics */
    HRESULT (STDMETHODCALLTYPE *CreateTargetForHwnd)(void* self, HWND h, BOOL topmost, void** target); /* 6 */
    HRESULT (STDMETHODCALLTYPE *CreateVisual)(void* self, void** visual);                        /* 7 */
    void*   unused_8_;                      /* CreateSurface */
    void*   unused_9_;                      /* CreateVirtualSurface */
    HRESULT (STDMETHODCALLTYPE *CreateSurfaceFromHandle)(void* self, HANDLE h, void** surface);  /* 10 */
} yscr__DCDeviceV;

typedef struct yscr__DCTargetV {          /* IDCompositionTarget */
    YSCR__IUNK_SLOTS;
    HRESULT (STDMETHODCALLTYPE *SetRoot)(void* self, void* visual);                              /* 3 */
} yscr__DCTargetV;

typedef struct yscr__DCVisualV {          /* IDCompositionVisual */
    YSCR__IUNK_SLOTS;
    /* 3..14: SetOffsetX x2, SetOffsetY x2, SetTransform x2, SetTransformParent,
     * SetEffect, SetBitmapInterpolationMode, SetBorderMode, SetClip x2. Two
     * slots per overload pair whatever the compiler's order within it. */
    void*   unused_3_14_[12];
    HRESULT (STDMETHODCALLTYPE *SetContent)(void* self, void* content);                          /* 15 */
} yscr__DCVisualV;

/* A COM object as C sees it: a pointer to its table. */
#define YSCR__OBJ(T) struct { const T* lpVtbl; }
typedef YSCR__OBJ(yscr__PFactoryV)     yscr__PFactory;
typedef YSCR__OBJ(yscr__PManagerV)     yscr__PManager;
typedef YSCR__OBJ(yscr__PBufferV)      yscr__PBuffer;
typedef YSCR__OBJ(yscr__PSurfaceV)     yscr__PSurface;
typedef YSCR__OBJ(yscr__PStatsV)       yscr__PStats;
typedef YSCR__OBJ(yscr__PStatusStatsV) yscr__PStatusStats;
typedef YSCR__OBJ(yscr__CompStatsV)    yscr__CompStats;
typedef YSCR__OBJ(yscr__IFlipStatsV)   yscr__IFlipStats;
typedef YSCR__OBJ(yscr__DCDeviceV)     yscr__DCDevice;
typedef YSCR__OBJ(yscr__DCTargetV)     yscr__DCTarget;
typedef YSCR__OBJ(yscr__DCVisualV)     yscr__DCVisual;
typedef YSCR__OBJ(yscr__PStatsV)       yscr__Unknown;   /* Release only */

#define YSCR__C(o, m, ...) ((o)->lpVtbl->m((o), __VA_ARGS__))
#define YSCR__C0(o, m)     ((o)->lpVtbl->m(o))
#define YSCR__CREL(o)      do { if (o) { (o)->lpVtbl->Release(o); (o) = NULL; } } while (0)

static const IID yscr__IID_IPresentationFactory = {0x8fb37b58,0x1d74,0x4f64,{0xa4,0x9c,0x1f,0x97,0xa8,0x0a,0x2e,0xc0}};
static const IID yscr__IID_IPresentStatusPresentStatistics = {0xc9ed2a41,0x79cb,0x435e,{0x96,0x4e,0xc8,0x55,0x30,0x55,0x42,0x0c}};
static const IID yscr__IID_ICompositionFramePresentStatistics = {0xab41d127,0xc101,0x4c0a,{0x91,0x1d,0xf9,0xf2,0xe9,0xd0,0x8e,0x64}};
static const IID yscr__IID_IIndependentFlipFramePresentStatistics = {0x8c93be27,0xad94,0x4da0,{0x8f,0xd4,0x24,0x13,0x13,0x2d,0x12,0x4e}};
static const IID yscr__IID_IDCompositionDevice = {0xc37ea93a,0xe7aa,0x450d,{0xb1,0x6f,0x97,0x46,0xcb,0x04,0x07,0xf3}};

#define YSCR__KIND_STATUS 1
#define YSCR__KIND_COMPOSITION 2
#define YSCR__KIND_IFLIP 3
#define YSCR__STATUS_QUEUED 0
#define YSCR__STATUS_SKIPPED 1
#define YSCR__STATUS_CANCELED 2

typedef HRESULT (WINAPI *yscr__CreatePresentationFactory_fn)(void* d3d_device, const IID* iid, void** out);
typedef HRESULT (WINAPI *yscr__DCompositionCreateDevice_fn)(void* dxgi_device, const IID* iid, void** out);
typedef HRESULT (WINAPI *yscr__DCompositionCreateSurfaceHandle_fn)(DWORD access, void* security, HANDLE* out);

#define YSCR__COMP_BUFFERS 3
#define YSCR__COMP_DONE 8

typedef struct yscr__comp {
    HWND                  hwnd;
    ID3D11Device*         dev;
    ID3D11DeviceContext*  dctx;
    ID3D11DeviceContext1* dctx1;
    yscr__PFactory*     pf;
    yscr__PManager*     pm;
    yscr__PSurface*     ps;
    HANDLE                surface_handle;
    yscr__DCDevice*     dc;
    yscr__DCTarget*     target;
    yscr__DCVisual*     visual;
    yscr__Unknown*      dsurf;
    HANDLE                stat_event, lost_event;
    yscr__DCompositionGetStatistics_fn       get_stats;
    yscr__DCompositionGetTargetStatistics_fn get_target_stats;
    ID3D11Texture2D*      tex[YSCR__COMP_BUFFERS];
    yscr__PBuffer*      buf[YSCR__COMP_BUFFERS];
    HANDLE                avail[YSCR__COMP_BUFFERS];
    ID3D11RenderTargetView* rtv[YSCR__COMP_BUFFERS];
    yscr__EGLSurface    pb[YSCR__COMP_BUFFERS];
    int                   cur;
    int                   parked;           /* between frames: 1 a free buffer bound, -1 none */
    void*                 edev;
    yscr__EGLDisplay    dpy;
    yscr__EGLContext    ctx;
    yscr__glFlush_fn    flush;
    uint64_t              sys_id[8], core_id[8];
    int                   map_next;
    uint64_t              last_sys_id;      /* the newest present */
    int                   last_resolved;    /* it has a statistic that settles it */
    int64_t               last_target_ns;
    yscr_vblank         done[YSCR__COMP_DONE];
    int                   n_done;
    int64_t               ref_t, ref_count; /* vblank count from times */
    int                   have_ref;
    int64_t               period_ns;
    uint8_t               path;
    int                   w, h;
    yscr__d3dcodes      q;
    char                  adapter[64];
    char                  angle[64];
} yscr__comp;

typedef char yscr__comp_fits[sizeof(yscr__comp) <= sizeof(((yscr_screen*)0)->backend_mem) ? 1 : -1];

static uint64_t yscr__comp_core_id(const yscr__comp* c, uint64_t sys) {
    int k;
    for (k = 0; k < 8; k++) if (c->sys_id[k] == sys && c->core_id[k]) return c->core_id[k];
    return 0;
}

static int64_t yscr__comp_count(yscr__comp* c, int64_t t) {
    int64_t k;
    if (!c->have_ref) { c->ref_t = t; c->ref_count = 1000; c->have_ref = 1; return c->ref_count; }
    k = c->ref_count + (int64_t)llround((double)(t - c->ref_t) / (double)c->period_ns);
    c->ref_t = t;
    c->ref_count = k;
    return k;
}

static void yscr__comp_push(yscr__comp* c, const yscr_vblank* v) {
    if (c->n_done == YSCR__COMP_DONE) {   /* never with one present in flight */
        memmove(c->done, c->done + 1, sizeof c->done[0] * (YSCR__COMP_DONE - 1));
        c->n_done--;
    }
    c->done[c->n_done++] = *v;
}

/* Read every statistic waiting. The two time sources differ in what they
 * are (docs/screen.md): an independent flip's DisplayedTime is the
 * vblank of the flip, a composed frame has only DWM's frame target. */
static void yscr__comp_drain(yscr__comp* c) {
    for (;;) {
        yscr__PStats* st = NULL;
        yscr_vblank v;
        uint64_t sys;
        int kind;
        if (FAILED(YSCR__C(c->pm, GetNextPresentStatistics, (void**)&st)) || !st) break;
        sys = YSCR__C0(st, GetPresentId);
        kind = YSCR__C0(st, GetKind);
        memset(&v, 0, sizeof v);
        v.present_id = yscr__comp_core_id(c, sys);
        if (kind == YSCR__KIND_STATUS) {
            yscr__PStatusStats* s2 = NULL;
            YSCR__C(st, QueryInterface, &yscr__IID_IPresentStatusPresentStatistics, (void**)&s2);
            if (s2) {
                int status = YSCR__C0(s2, GetPresentStatus);
                if (status == YSCR__STATUS_SKIPPED || status == YSCR__STATUS_CANCELED) {
                    v.flags = (uint16_t)(status == YSCR__STATUS_SKIPPED ? YSCR_FLIP_SKIPPED : YSCR_FLIP_CANCELED);
                    v.path = c->path;
                    yscr__comp_push(c, &v);
                    if (sys == c->last_sys_id) c->last_resolved = 1;
                }
                YSCR__CREL(s2);
            }
        } else if (kind == YSCR__KIND_IFLIP) {
            yscr__IFlipStats* s3 = NULL;
            YSCR__C(st, QueryInterface, &yscr__IID_IIndependentFlipFramePresentStatistics, (void**)&s3);
            if (s3) {
                yscr__SIT shown;
                shown.value = 0;
                s3->lpVtbl->GetDisplayedTime(s3, &shown);
                /* QPC in 100 ns units, measured, not interrupt time: the
                 * value sits on the D3DKMT vblank, 20 ms from
                 * QueryInterruptTimePrecise on this machine. */
                v.t_ns = (int64_t)shown.value * 100;
                v.count = yscr__comp_count(c, v.t_ns);
                v.path = (uint8_t)YSCR_PATH_INDEPENDENT;
                v.tier = (uint8_t)YSCR_TIER_1;
                c->path = v.path;
                yscr__comp_push(c, &v);
                if (sys == c->last_sys_id) c->last_resolved = 1;
                YSCR__CREL(s3);
            }
        } else if (kind == YSCR__KIND_COMPOSITION) {
            yscr__CompStats* s4 = NULL;
            YSCR__C(st, QueryInterface, &yscr__IID_ICompositionFramePresentStatistics, (void**)&s4);
            if (s4) {
                UINT n = 0;
                const yscr__CFDI* inst = NULL;
                s4->lpVtbl->GetDisplayInstanceArray(s4, &n, &inst);
                v.path = (uint8_t)((n && inst[0].instanceKind == 1) ? YSCR_PATH_OVERLAY : YSCR_PATH_COMPOSED);
                /* DWM's own time for the vblank it composed this frame for:
                 * it sits on the hardware vblank and follows a late frame to
                 * the vblank used, but it is reported about 15 ms before
                 * that vblank, so it is DWM's plan, not an observation
                 * (docs/screen.md). */
                v.t_ns = 0;
                if (n && c->get_stats && c->get_target_stats) {
                    uint64_t frame = YSCR__C0(s4, GetCompositionFrameId);
                    yscr__CFStats fs;
                    yscr__CTargetId ids[8];
                    UINT k, nid = 0;
                    if (SUCCEEDED(c->get_stats(frame, &fs, 8, ids, &nid))) {
                        for (k = 0; k < nid && k < 8; k++) {
                            yscr__CTargetStats ts;
                            if (ids[k].displayAdapterLuid.LowPart != inst[0].displayAdapterLUID.LowPart ||
                                ids[k].displayAdapterLuid.HighPart != inst[0].displayAdapterLUID.HighPart ||
                                ids[k].vidPnSourceId != inst[0].displayVidPnSourceId) continue;
                            if (SUCCEEDED(c->get_target_stats(frame, &ids[k], &ts)) && ts.presentedStats.time)
                                v.t_ns = yrt_ticks_to_ns((int64_t)ts.presentedStats.time);
                            break;
                        }
                    }
                }
                v.flags = YSCR_FLIP_ONSET_PLANNED;
                v.tier = (uint8_t)YSCR_TIER_3;
                if (v.t_ns) v.count = yscr__comp_count(c, v.t_ns);
                c->path = v.path;
                yscr__comp_push(c, &v);
                if (sys == c->last_sys_id) c->last_resolved = 1;
                YSCR__CREL(s4);
            }
        }
        YSCR__CREL(st);
    }
}

static void yscr__comp_close(void* vctx);

/* Set by open() around AUTO's try of COMPOSITION; open() calls are never
 * concurrent (MEMORY AND THREADS). */
static int yscr__comp_auto;

#if defined(_M_ARM64) || defined(__aarch64__) || defined(_M_IX86) || defined(__i386__)
static int yscr__comp_open(void* vctx, const yscr_presenter_open* in, yscr_caps* caps,
                             char* err, size_t err_cap) {
    memset(vctx, 0, sizeof(yscr__comp));
    (void)in; (void)caps;
    yscr__set_error(err, err_cap, "ysp_screen: COMPOSITION is x64 only: the struct-return ABI it depends on "
                      "differs on ARM64 and x86 and has not been tested there");
    return YSCR_ERR_NOT_IMPLEMENTED;
}
#else
static int yscr__comp_open(void* vctx, const yscr_presenter_open* in, yscr_caps* caps,
                             char* err, size_t err_cap) {
    yscr__comp* c = (yscr__comp*)vctx;
    HMODULE d3d11, dcomp, user32;
    yscr__D3D11CreateDevice_fn create_device;
    yscr__CreatePresentationFactory_fn create_pf;
    yscr__DCompositionCreateDevice_fn create_dc;
    yscr__DCompositionCreateSurfaceHandle_fn create_sh;
    yscr__CreateDXGIFactory1_fn create_factory;
    yscr__MonitorFromWindow_fn monitor_from_window;
    IDXGIFactory1* fac1 = NULL;
    IDXGIAdapter1* pick = NULL;
    IDXGIDevice* dxdev = NULL;
    D3D_FEATURE_LEVEL levels[2];
    HRESULT hr;
    UINT i, j;
    yscr__EGLint attrs[16], n_cfg = 0, egl_major = 0, egl_minor = 0;
    yscr__EGLConfig cfg = NULL;
    RECT src;

    memset(c, 0, sizeof *c);
    if (!in->window) { yscr__set_error(err, err_cap, "ysp_screen: no window"); return YSCR_ERR_ARG; }
    c->hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(in->window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    SDL_GetWindowSizeInPixels(in->window, &c->w, &c->h);
    c->period_ns = in->mode && in->mode->period_ns > 0 ? in->mode->period_ns : 16666667;
    d3d11 = LoadLibraryW(L"d3d11.dll");
    dcomp = LoadLibraryW(L"dcomp.dll");
    user32 = LoadLibraryW(L"user32.dll");
    create_device = d3d11 ? (yscr__D3D11CreateDevice_fn)(yscr_proc)GetProcAddress(d3d11, "D3D11CreateDevice") : NULL;
    create_pf = dcomp ? (yscr__CreatePresentationFactory_fn)(yscr_proc)GetProcAddress(dcomp, "CreatePresentationFactory") : NULL;
    create_dc = dcomp ? (yscr__DCompositionCreateDevice_fn)(yscr_proc)GetProcAddress(dcomp, "DCompositionCreateDevice") : NULL;
    create_sh = dcomp ? (yscr__DCompositionCreateSurfaceHandle_fn)(yscr_proc)GetProcAddress(dcomp, "DCompositionCreateSurfaceHandle") : NULL;
    c->get_stats = dcomp ? (yscr__DCompositionGetStatistics_fn)(yscr_proc)GetProcAddress(dcomp, "DCompositionGetStatistics") : NULL;
    c->get_target_stats = dcomp ? (yscr__DCompositionGetTargetStatistics_fn)(yscr_proc)GetProcAddress(dcomp, "DCompositionGetTargetStatistics") : NULL;
    create_factory = (yscr__CreateDXGIFactory1_fn)(yscr_proc)GetProcAddress(LoadLibraryW(L"dxgi.dll"), "CreateDXGIFactory1");
    monitor_from_window = user32 ? (yscr__MonitorFromWindow_fn)(yscr_proc)GetProcAddress(user32, "MonitorFromWindow") : NULL;
    if (!c->hwnd || !create_device || !create_pf || !create_dc || !create_sh || !create_factory || !monitor_from_window) {
        yscr__set_error(err, err_cap, "ysp_screen: the composition swapchain needs Windows 11 (dcomp.dll "
                          "CreatePresentationFactory)");
        return YSCR_ERR_NOT_IMPLEMENTED;
    }
    if (SUCCEEDED(create_factory(YSCR__IID(yscr__IID_IDXGIFactory1), (void**)&fac1))) {
        HMONITOR mon = monitor_from_window(c->hwnd, 1);
        for (i = 0; !pick; i++) {
            IDXGIAdapter1* ad = NULL;
            if (YSCR__CALL(fac1, EnumAdapters1, i, &ad) != S_OK) break;
            for (j = 0; ; j++) {
                IDXGIOutput* o = NULL;
                DXGI_OUTPUT_DESC od;
                if (YSCR__CALL(ad, EnumOutputs, j, &o) != S_OK) break;
                YSCR__CALL(o, GetDesc, &od);
                YSCR__RELEASE(o);
                if (od.Monitor == mon) { pick = ad; break; }
            }
            if (pick != ad) YSCR__RELEASE(ad);
        }
        YSCR__RELEASE(fac1);
    }
    if (pick) {
        DXGI_ADAPTER_DESC1 ad;
        YSCR__CALL(pick, GetDesc1, &ad);
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, c->adapter, (int)sizeof c->adapter - 1, NULL, NULL);
    }
    levels[0] = D3D_FEATURE_LEVEL_11_1; levels[1] = D3D_FEATURE_LEVEL_11_0;
    hr = create_device((IDXGIAdapter*)pick, pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL,
                       yscr__device_flags(in), levels, 2, D3D11_SDK_VERSION, &c->dev, NULL, &c->dctx);
    YSCR__RELEASE(pick);
    if (FAILED(hr)) { yscr__device_error(in, hr, err, err_cap); return YSCR_ERR_LOST; }
    yscr__device_protect(in, c->dev);
    YSCR__CALL(c->dctx, QueryInterface, YSCR__IID(yscr__IID_ID3D11DeviceContext1), (void**)&c->dctx1);

    hr = create_pf(c->dev, &yscr__IID_IPresentationFactory, (void**)&c->pf);
    if (FAILED(hr) || !YSCR__C0(c->pf, IsPresentationSupported)) {
        yscr__comp_close(c);
        yscr__set_error(err, err_cap, "ysp_screen: this GPU and driver do not support the composition swapchain");
        return YSCR_ERR_NOT_IMPLEMENTED;
    }
    /* AUTO takes COMPOSITION only where its frames can reach the tier 1
     * path; a composed onset there is DWM's plan (tier 3). */
    if (yscr__comp_auto && !YSCR__C0(c->pf, IsPresentationSupportedWithIndependentFlip)) {
        yscr__comp_close(c);
        yscr__set_error(err, err_cap, "ysp_screen: no independent flip for the composition swapchain on this output");
        return YSCR_ERR_NOT_IMPLEMENTED;
    }
    caps->vrr_capable = false;
    if (FAILED(YSCR__C(c->pf, CreatePresentationManager, (void**)&c->pm)) ||
        FAILED(create_sh(0x0003 /* COMPOSITIONOBJECT_ALL_ACCESS */, NULL, &c->surface_handle)) ||
        FAILED(YSCR__C(c->pm, CreatePresentationSurface, c->surface_handle, (void**)&c->ps))) {
        yscr__comp_close(c);
        yscr__set_error(err, err_cap, "ysp_screen: presentation manager or surface failed");
        return YSCR_ERR_LOST;
    }
    YSCR__CALL(c->dev, QueryInterface, YSCR__IID(yscr__IID_IDXGIDevice), (void**)&dxdev);
    hr = dxdev ? create_dc(dxdev, &yscr__IID_IDCompositionDevice, (void**)&c->dc) : E_FAIL;
    YSCR__RELEASE(dxdev);
    if (FAILED(hr) ||
        FAILED(YSCR__C(c->dc, CreateTargetForHwnd, c->hwnd, TRUE, (void**)&c->target)) ||
        FAILED(YSCR__C(c->dc, CreateVisual, (void**)&c->visual)) ||
        FAILED(YSCR__C(c->dc, CreateSurfaceFromHandle, c->surface_handle, (void**)&c->dsurf)) ||
        FAILED(YSCR__C(c->visual, SetContent, (void*)c->dsurf)) ||
        FAILED(YSCR__C(c->target, SetRoot, (void*)c->visual)) ||
        FAILED(YSCR__C0(c->dc, Commit))) {
        yscr__comp_close(c);
        yscr__set_error(err, err_cap, "ysp_screen: DirectComposition target or visual failed");
        return YSCR_ERR_LOST;
    }
    YSCR__C(c->pm, EnablePresentStatisticsKind, YSCR__KIND_STATUS, 1);
    YSCR__C(c->pm, EnablePresentStatisticsKind, YSCR__KIND_COMPOSITION, 1);
    YSCR__C(c->pm, EnablePresentStatisticsKind, YSCR__KIND_IFLIP, 1);
    YSCR__C(c->pm, GetPresentStatisticsAvailableEvent, &c->stat_event);
    YSCR__C(c->pm, GetLostEvent, &c->lost_event);
    YSCR__C(c->ps, SetAlphaMode, (int)DXGI_ALPHA_MODE_IGNORE);
    YSCR__C(c->ps, SetColorSpace, (int)DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    /* Without a source rectangle nothing shows and every other present is
     * skipped (measured; ntrviewer-hr and StreamLight found the same). */
    src.left = 0; src.top = 0; src.right = c->w; src.bottom = c->h;
    YSCR__C(c->ps, SetSourceRect, &src);

    if (!yscr__egl_load(in->angle_dir, err, err_cap)) { yscr__comp_close(c); return YSCR_ERR_NOT_IMPLEMENTED; }
    c->edev = yscr__egl.CreateDeviceANGLE(YSCR__EGL_D3D11_DEVICE_ANGLE, c->dev, NULL);
    c->dpy = c->edev ? yscr__egl.GetPlatformDisplayEXT(YSCR__EGL_PLATFORM_DEVICE_EXT, c->edev, NULL) : NULL;
    if (!c->dpy || !yscr__egl.Initialize(c->dpy, &egl_major, &egl_minor)) {
        yscr__comp_close(c);
        yscr__set_error(err, err_cap, "ysp_screen: ANGLE display on the D3D11 device failed");
        return YSCR_ERR_LOST;
    }
    attrs[0] = YSCR__EGL_RED_SIZE; attrs[1] = 8; attrs[2] = YSCR__EGL_GREEN_SIZE; attrs[3] = 8;
    attrs[4] = YSCR__EGL_BLUE_SIZE; attrs[5] = 8; attrs[6] = YSCR__EGL_ALPHA_SIZE; attrs[7] = 8;
    attrs[8] = YSCR__EGL_SURFACE_TYPE; attrs[9] = YSCR__EGL_PBUFFER_BIT;
    attrs[10] = YSCR__EGL_RENDERABLE_TYPE; attrs[11] = YSCR__EGL_OPENGL_ES3_BIT;
    attrs[12] = YSCR__EGL_NONE;
    if (!yscr__egl.ChooseConfig(c->dpy, attrs, &cfg, 1, &n_cfg) || n_cfg < 1) {
        yscr__comp_close(c);
        yscr__set_error(err, err_cap, "ysp_screen: no RGBA8 ES 3.0 pbuffer config");
        return YSCR_ERR_LOST;
    }
    attrs[0] = YSCR__EGL_CONTEXT_MAJOR; attrs[1] = 3; attrs[2] = YSCR__EGL_CONTEXT_MINOR; attrs[3] = 0;
    attrs[4] = YSCR__EGL_NONE;
    c->ctx = yscr__egl.CreateContext(c->dpy, cfg, NULL, attrs);
    for (i = 0; i < YSCR__COMP_BUFFERS; i++) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = (UINT)c->w; td.Height = (UINT)c->h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        /* what AddBufferFromResource takes: displayable, shared by NT handle */
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | 0x100000 /* SHARED_DISPLAYABLE */;
        attrs[0] = YSCR__EGL_NONE;
        if (FAILED(YSCR__CALL(c->dev, CreateTexture2D, &td, NULL, &c->tex[i])) ||
            FAILED(YSCR__C(c->pm, AddBufferFromResource, (void*)c->tex[i], (void**)&c->buf[i])) ||
            FAILED(YSCR__C(c->buf[i], GetAvailableEvent, &c->avail[i])) ||
            !c->ctx ||
            !(c->pb[i] = yscr__egl.CreatePbufferFromClientBuffer(c->dpy, YSCR__EGL_D3D_TEXTURE_ANGLE, c->tex[i], cfg, attrs))) {
            yscr__comp_close(c);
            yscr__set_error(err, err_cap, "ysp_screen: composition buffer %u failed", i);
            return YSCR_ERR_LOST;
        }
        if (c->dctx1) YSCR__CALL(c->dev, CreateRenderTargetView, (ID3D11Resource*)c->tex[i], NULL, &c->rtv[i]);
    }
    c->flush = (yscr__glFlush_fn)yscr__egl_sym("glFlush");
    {
        yscr__glGetString_fn gs = (yscr__glGetString_fn)yscr__egl_sym("glGetString");
        const char* v;
        yscr__egl.MakeCurrent(c->dpy, c->pb[0], c->pb[0], c->ctx);
        v = gs ? (const char*)gs(YSCR__GL_VERSION) : NULL;
        if (v) {
            const char* a = strstr(v, "ANGLE ");
            char* p;
            yscr__copy(c->angle, sizeof c->angle, a ? a + 6 : v);
            p = strstr(c->angle, " git hash: ");
            if (p) { memmove(p + 1, p + 11, strlen(p + 11) + 1); *p = '/'; }
            p = strchr(c->angle, ')');
            if (p) *p = '\0';
        }
    }
    caps->kind = YSCR_FIXED_GRID;
    caps->native_target = true;
    caps->hw_onset = true;
    caps->max_in_flight = 1;
    c->last_resolved = 1;
    yscr__d3d_codes_open(c->dev, c->dctx, c->dctx1, in, &c->q);
    return YSCR_OK;
}
#endif

static void yscr__comp_close(void* vctx) {
    yscr__comp* c = (yscr__comp*)vctx;
    int i;
    if (c->pm && c->last_sys_id) YSCR__C(c->pm, CancelPresentsFrom, c->last_sys_id + 1);
    yscr__d3d_codes_close(&c->q);
    if (c->dpy) {
        yscr__egl.MakeCurrent(c->dpy, NULL, NULL, NULL);
        for (i = 0; i < YSCR__COMP_BUFFERS; i++) if (c->pb[i]) yscr__egl.DestroySurface(c->dpy, c->pb[i]);
        if (c->ctx) yscr__egl.DestroyContext(c->dpy, c->ctx);
        if (yscr__egl.Terminate) yscr__egl.Terminate(c->dpy);
    }
    if (c->edev && yscr__egl.ReleaseDeviceANGLE) yscr__egl.ReleaseDeviceANGLE(c->edev);
    for (i = 0; i < YSCR__COMP_BUFFERS; i++) {
        YSCR__RELEASE(c->rtv[i]);
        YSCR__CREL(c->buf[i]);
        YSCR__RELEASE(c->tex[i]);
    }
    YSCR__CREL(c->dsurf);
    YSCR__CREL(c->visual);
    YSCR__CREL(c->target);
    YSCR__CREL(c->dc);
    YSCR__CREL(c->ps);
    YSCR__CREL(c->pm);
    YSCR__CREL(c->pf);
    if (c->surface_handle) CloseHandle(c->surface_handle);
    /* The Get*Event calls return a new handle each; releasing the manager
     * and the buffers does not close them (5 handles an open before
     * v0.10.4, measured). */
    for (i = 0; i < YSCR__COMP_BUFFERS; i++) if (c->avail[i]) CloseHandle(c->avail[i]);
    if (c->stat_event) CloseHandle(c->stat_event);
    if (c->lost_event) CloseHandle(c->lost_event);
    YSCR__RELEASE(c->dctx1);
    YSCR__RELEASE(c->dctx);
    YSCR__RELEASE(c->dev);
    memset(c, 0, sizeof *c);
}

/* Between frames no presented buffer stays bound (v0.10.3). A shown
 * buffer is released only by a newer present or a covered window, and a
 * draw into it waited until then: ygfx_prime() between flip() and begin()
 * in front, more than 90 s (docs/screen.md, "Between frames"). So after a
 * present framebuffer 0 is a buffer no present holds; with none free,
 * no surface (draws to framebuffer 0 do nothing; reads of it fail). */
/* Whether buffer k is free. A free buffer was found at every present in
 * testing, so the window test replaces this to make the case without one
 * happen (tests/adapt/screen_window_test.c, "nofree"). */
#ifndef YSCR__COMP_IS_AVAILABLE
#define YSCR__COMP_IS_AVAILABLE(c, k, ok) YSCR__C((c)->buf[k], IsAvailable, (ok))
#endif
static void yscr__comp_park(yscr__comp* c) {
    int i;
    for (i = 1; i < YSCR__COMP_BUFFERS; i++) {
        unsigned char ok = 0;
        int k = (c->cur + i) % YSCR__COMP_BUFFERS;
        if (SUCCEEDED(YSCR__COMP_IS_AVAILABLE(c, k, &ok)) && ok) {
            c->cur = k;
            c->parked = 1;
            yscr__egl.MakeCurrent(c->dpy, c->pb[k], c->pb[k], c->ctx);
            return;
        }
    }
    c->parked = -1;
    yscr__egl.MakeCurrent(c->dpy, NULL, NULL, c->ctx);
}

static void yscr__comp_bind(void* vctx) {
    yscr__comp* c = (yscr__comp*)vctx;
    if (c->parked < 0) yscr__comp_park(c);   /* a buffer may be free now */
    else yscr__egl.MakeCurrent(c->dpy, c->pb[c->cur], c->pb[c->cur], c->ctx);
}

/* One frame in flight: wait until the newest present is settled by a
 * statistic, then for a free buffer. A present that gets no statistic in
 * time was not shown (the window was covered, measured): it completes as
 * OCCLUDED and the frame loop goes on. */
static int yscr__comp_acquire(void* vctx, int64_t deadline_ns, yscr_vblank* newest) {
    yscr__comp* c = (yscr__comp*)vctx;
    HANDLE h[2];
    int i;
    newest->t_ns = 0;
    yscr__comp_drain(c);
    if (!c->last_resolved) {
        int64_t give_up = c->last_target_ns ? c->last_target_ns + 3 * c->period_ns : yscr__now() + 3 * c->period_ns;
        if (give_up > deadline_ns) give_up = deadline_ns;
        h[0] = c->stat_event; h[1] = c->lost_event;
        while (!c->last_resolved) {
            int64_t left = give_up - yscr__now();
            DWORD rc;
            if (left <= 0) {
                yscr_vblank v;
                memset(&v, 0, sizeof v);
                v.present_id = yscr__comp_core_id(c, c->last_sys_id);
                v.flags = YSCR_FLIP_OCCLUDED;
                v.path = c->path;
                yscr__comp_push(c, &v);
                c->last_resolved = 1;
                break;
            }
            rc = WaitForMultipleObjects(2, h, FALSE, (DWORD)((left + 999999) / 1000000));
            if (rc == WAIT_OBJECT_0 + 1) return YSCR_ERR_LOST;
            yscr__comp_drain(c);
        }
    }
    for (;;) {
        DWORD rc;
        int64_t left;
        /* the parked buffer first: it is current already */
        for (i = 0; i < YSCR__COMP_BUFFERS; i++) {
            unsigned char ok = 0;
            int k = (c->cur + (c->parked > 0 ? 0 : 1) + i) % YSCR__COMP_BUFFERS;
            if (SUCCEEDED(YSCR__COMP_IS_AVAILABLE(c, k, &ok)) && ok) {
                c->cur = k;
                c->parked = 0;
                yscr__comp_bind(c);
                return YSCR_OK;
            }
        }
        left = deadline_ns - yscr__now();
        if (left <= 0) return YSCR_ERR_TIMEOUT;
        rc = WaitForMultipleObjects(YSCR__COMP_BUFFERS, c->avail, FALSE, (DWORD)((left + 999999) / 1000000));
        if (rc == WAIT_FAILED) return YSCR_ERR_LOST;
    }
}

static int yscr__comp_present(void* vctx, const yscr_present_req* req) {
    yscr__comp* c = (yscr__comp*)vctx;
    yscr__SIT target;
    uint64_t sys;
    if (c->flush) c->flush();
    yscr__d3d_finish_frame(c->dctx, c->dctx1, c->rtv[c->cur], (ID3D11Resource*)c->tex[c->cur], &c->q, req);
    if (FAILED(YSCR__C(c->ps, SetBuffer, (void*)c->buf[c->cur]))) return YSCR_ERR_LOST;
    /* QPC time in 100 ns units (measured: interrupt time is 20 ms off on
     * this machine and puts every frame 1.2 periods early). The target
     * persists across presents, so it is set on every one. */
    target.value = req->target_count ? (uint64_t)(req->target_ns / 100) : 0;
    if (FAILED(YSCR__C(c->pm, SetTargetTime, target))) return YSCR_ERR_LOST;
    sys = YSCR__C0(c->pm, GetNextPresentId);
    if (FAILED(YSCR__C0(c->pm, Present))) return YSCR_ERR_LOST;
    c->sys_id[c->map_next & 7] = sys;
    c->core_id[c->map_next & 7] = req->present_id;
    c->map_next++;
    c->last_sys_id = sys;
    c->last_resolved = 0;
    c->last_target_ns = req->target_count ? req->target_ns : 0;
    yscr__comp_park(c);
    return YSCR_OK;
}

static int yscr__comp_completions(void* vctx, yscr_vblank* out, int cap) {
    yscr__comp* c = (yscr__comp*)vctx;
    int n;
    yscr__comp_drain(c);
    n = c->n_done < cap ? c->n_done : cap;
    memcpy(out, c->done, sizeof out[0] * (size_t)n);
    memmove(c->done, c->done + n, sizeof c->done[0] * (size_t)(c->n_done - n));
    c->n_done -= n;
    return n;
}

static int yscr__comp_describe(void* vctx, char* buf, size_t cap) {
    yscr__comp* c = (yscr__comp*)vctx;
    return snprintf(buf, cap, "adapter=\"%s\" angle=%s", c->adapter, c->angle);
}

static uint64_t yscr__comp_gpu_done(void* vctx) { return yscr__d3d_gpu_done(&((yscr__comp*)vctx)->q); }

static const yscr_presenter yscr__comp_presenter = {
    YSCR_PRESENTER_VERSION, "composition", true, false, true,
    yscr__comp_open, yscr__comp_close, yscr__comp_acquire, yscr__comp_present,
    yscr__comp_completions, yscr__dxgi_gl_proc, yscr__comp_bind, yscr__comp_describe,
    yscr__comp_gpu_done
};


/* --- the display's color state and the OS gamma ramp (Windows) ------------ */

/* Own layouts of the display config structs (wingdi.h, Windows 7 and
 * later), so neither the SDK version nor a _WIN32_WINNT another header set
 * decides whether this compiles. tests/compile/screen_com.cpp checks
 * them against the SDK. */
typedef struct yscr__dci_header {
    UINT32 type, size;
    LUID   adapterId;
    UINT32 id;
} yscr__dci_header;
typedef struct yscr__dc_path {      /* DISPLAYCONFIG_PATH_INFO           */
    LUID   src_adapter;
    UINT32 src_id, src_mode, src_status;
    LUID   tgt_adapter;
    UINT32 tgt_id;
    UINT32 tgt_rest[9];
    UINT32 flags;
} yscr__dc_path;
typedef struct yscr__dc_mode {      /* DISPLAYCONFIG_MODE_INFO           */
    UINT32 bytes[16];
} yscr__dc_mode;
typedef struct yscr__dc_source_name {
    yscr__dci_header header;
    WCHAR  viewGdiDeviceName[32];
} yscr__dc_source_name;
typedef struct yscr__dc_preferred {
    yscr__dci_header header;
    UINT32 width, height;
    UINT32 pad_;           /* targetMode holds a UINT64: 8-byte aligned */
    UINT32 targetMode[12];
} yscr__dc_preferred;
typedef struct yscr__aci {
    yscr__dci_header header;
    UINT32 value;                /* bit 1: advanced color enabled; bit 2: wide color enforced */
    UINT32 colorEncoding;
    UINT32 bitsPerColorChannel;
} yscr__aci;
typedef struct yscr__aci2 {
    yscr__dci_header header;
    UINT32 value;
    UINT32 colorEncoding;
    UINT32 bitsPerColorChannel;
    UINT32 activeColorMode;      /* 0 SDR, 1 WCG (auto color management), 2 HDR */
} yscr__aci2;
#define YSCR__DCI_SOURCE_NAME      1
#define YSCR__DCI_PREFERRED_MODE   3
#define YSCR__DCI_ADVANCED_COLOR   9
#define YSCR__DCI_ADVANCED_COLOR_2 15

typedef LONG (WINAPI *yscr__GetDisplayConfigBufferSizes_fn)(UINT32, UINT32*, UINT32*);
typedef LONG (WINAPI *yscr__QueryDisplayConfig_fn)(UINT32, UINT32*, yscr__dc_path*, UINT32*,
                                                     yscr__dc_mode*, void*);
typedef LONG (WINAPI *yscr__DisplayConfigGetDeviceInfo_fn)(yscr__dci_header*);
typedef BOOL (WINAPI *yscr__GetMonitorInfoW_fn)(HMONITOR, LPMONITORINFO);
typedef BOOL (WINAPI *yscr__GammaRamp_fn)(HDC, LPVOID);
typedef HDC  (WINAPI *yscr__CreateDCW_fn)(LPCWSTR, LPCWSTR, LPCWSTR, const DEVMODEW*);
typedef BOOL (WINAPI *yscr__DeleteDC_fn)(HDC);
typedef HHOOK (WINAPI *yscr__SetWindowsHookExW_fn)(int, HOOKPROC, HINSTANCE, DWORD);
typedef BOOL (WINAPI *yscr__UnhookWindowsHookEx_fn)(HHOOK);
typedef LRESULT (WINAPI *yscr__CallNextHookEx_fn)(HHOOK, int, WPARAM, LPARAM);
typedef BOOL (WINAPI *yscr__GetMessageW_fn)(LPMSG, HWND, UINT, UINT);
typedef BOOL (WINAPI *yscr__PeekMessageW_fn)(LPMSG, HWND, UINT, UINT, UINT);
typedef BOOL (WINAPI *yscr__RegisterRawInputDevices_fn)(PCRAWINPUTDEVICE, UINT, UINT);
typedef UINT (WINAPI *yscr__GetRegisteredRawInputDevices_fn)(PRAWINPUTDEVICE, PUINT, UINT);
typedef UINT (WINAPI *yscr__GetRawInputBuffer_fn)(PRAWINPUT, PUINT, UINT);
typedef ATOM (WINAPI *yscr__RegisterClassW_fn)(const WNDCLASSW*);
typedef HWND (WINAPI *yscr__CreateWindowExW_fn)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU,
                                                   HINSTANCE, LPVOID);
typedef BOOL (WINAPI *yscr__DestroyWindow_fn)(HWND);
typedef LRESULT (WINAPI *yscr__DefWindowProcW_fn)(HWND, UINT, WPARAM, LPARAM);
typedef DWORD (WINAPI *yscr__MsgWaitForMultipleObjects_fn)(DWORD, const HANDLE*, BOOL, DWORD, DWORD);
typedef DWORD (WINAPI *yscr__MsgWaitForMultipleObjectsEx_fn)(DWORD, const HANDLE*, DWORD, DWORD, DWORD);
typedef BOOL (WINAPI *yscr__PostMessageW_fn)(HWND, UINT, WPARAM, LPARAM);
typedef BOOL (WINAPI *yscr__PostThreadMessageW_fn)(DWORD, UINT, WPARAM, LPARAM);
typedef SHORT (WINAPI *yscr__GetAsyncKeyState_fn)(int);
typedef HWND (WINAPI *yscr__GetForegroundWindow_fn)(void);
typedef BOOL (WINAPI *yscr__SetForegroundWindow_fn)(HWND);
typedef BOOL (WINAPI *yscr__AllowSetForegroundWindow_fn)(DWORD);
typedef BOOL (WINAPI *yscr__IsWindow_fn)(HWND);
typedef DWORD (WINAPI *yscr__GetWindowThreadProcessId_fn)(HWND, LPDWORD);
typedef BOOL (WINAPI *yscr__IsHungAppWindow_fn)(HWND);
typedef HICON (WINAPI *yscr__CreateIconIndirect_fn)(PICONINFO);
typedef BOOL (WINAPI *yscr__DestroyIcon_fn)(HICON);
typedef LRESULT (WINAPI *yscr__SendMessageW_fn)(HWND, UINT, WPARAM, LPARAM);
typedef UINT (WINAPI *yscr__GetDpiForWindow_fn)(HWND);
typedef int (WINAPI *yscr__GetSystemMetricsForDpi_fn)(int, UINT);
typedef HBITMAP (WINAPI *yscr__CreateDIBSection_fn)(HDC, const BITMAPINFO*, UINT, void**, HANDLE, DWORD);
typedef HBITMAP (WINAPI *yscr__CreateBitmap_fn)(int, int, UINT, UINT, const void*);
typedef BOOL (WINAPI *yscr__DeleteObject_fn)(HGDIOBJ);
typedef int (WINAPI *yscr__GetClassNameW_fn)(HWND, LPWSTR, int);

/* gdi32 and user32 by name, so nothing extra is linked (MinGW links no
 * gdi32 by default). */
static struct yscr__winapi {
    int tried;
    yscr__GetDisplayConfigBufferSizes_fn sizes;
    yscr__QueryDisplayConfig_fn          query;
    yscr__DisplayConfigGetDeviceInfo_fn  info;
    yscr__GetMonitorInfoW_fn             monitor_info;
    yscr__MonitorFromWindow_fn           monitor_from_window;
    yscr__GammaRamp_fn                   get_ramp, set_ramp;
    yscr__CreateDCW_fn                   create_dc;
    yscr__DeleteDC_fn                    delete_dc;
    /* the panic watchdog */
    yscr__SetWindowsHookExW_fn           set_hook;
    yscr__UnhookWindowsHookEx_fn         unhook;
    yscr__CallNextHookEx_fn              next_hook;
    yscr__GetMessageW_fn                 get_message;
    yscr__PeekMessageW_fn                peek_message;
    yscr__PostThreadMessageW_fn          post_thread;
    yscr__GetAsyncKeyState_fn            key_state;
    yscr__GetForegroundWindow_fn         foreground;
    yscr__SetForegroundWindow_fn         set_foreground;
    yscr__AllowSetForegroundWindow_fn    allow_foreground;
    yscr__IsWindow_fn                    is_window;
    yscr__GetWindowThreadProcessId_fn    window_pid;
    yscr__IsHungAppWindow_fn             is_hung;
    yscr__GetClassNameW_fn               class_name;
    /* the window icons */
    yscr__CreateIconIndirect_fn          create_icon;
    yscr__DestroyIcon_fn                 destroy_icon;
    yscr__SendMessageW_fn                send_message;
    yscr__GetDpiForWindow_fn             window_dpi;
    yscr__GetSystemMetricsForDpi_fn      metrics_dpi;
    yscr__CreateDIBSection_fn            create_dib;
    yscr__CreateBitmap_fn                create_bitmap;
    yscr__DeleteObject_fn                delete_object;
    /* the raw mouse reader */
    yscr__RegisterRawInputDevices_fn     raw_register;
    yscr__GetRegisteredRawInputDevices_fn raw_registered;
    yscr__GetRawInputBuffer_fn           raw_buffer;
    yscr__RegisterClassW_fn              register_class;
    yscr__CreateWindowExW_fn             create_window;
    yscr__DestroyWindow_fn               destroy_window;
    yscr__DefWindowProcW_fn              def_proc;
    yscr__MsgWaitForMultipleObjects_fn   msg_wait;
    yscr__MsgWaitForMultipleObjectsEx_fn msg_wait_ex;
    yscr__PostMessageW_fn                post_message;
} yscr__win;

static void yscr__win_load(void) {
    HMODULE u, g;
    if (yscr__win.tried) return;
    yscr__win.tried = 1;
    u = LoadLibraryW(L"user32.dll");
    g = LoadLibraryW(L"gdi32.dll");
    if (u) {
        yscr__win.sizes = (yscr__GetDisplayConfigBufferSizes_fn)(yscr_proc)GetProcAddress(u, "GetDisplayConfigBufferSizes");
        yscr__win.query = (yscr__QueryDisplayConfig_fn)(yscr_proc)GetProcAddress(u, "QueryDisplayConfig");
        yscr__win.info = (yscr__DisplayConfigGetDeviceInfo_fn)(yscr_proc)GetProcAddress(u, "DisplayConfigGetDeviceInfo");
        yscr__win.monitor_info = (yscr__GetMonitorInfoW_fn)(yscr_proc)GetProcAddress(u, "GetMonitorInfoW");
        yscr__win.monitor_from_window = (yscr__MonitorFromWindow_fn)(yscr_proc)GetProcAddress(u, "MonitorFromWindow");
        yscr__win.set_hook = (yscr__SetWindowsHookExW_fn)(yscr_proc)GetProcAddress(u, "SetWindowsHookExW");
        yscr__win.unhook = (yscr__UnhookWindowsHookEx_fn)(yscr_proc)GetProcAddress(u, "UnhookWindowsHookEx");
        yscr__win.next_hook = (yscr__CallNextHookEx_fn)(yscr_proc)GetProcAddress(u, "CallNextHookEx");
        yscr__win.get_message = (yscr__GetMessageW_fn)(yscr_proc)GetProcAddress(u, "GetMessageW");
        yscr__win.peek_message = (yscr__PeekMessageW_fn)(yscr_proc)GetProcAddress(u, "PeekMessageW");
        yscr__win.post_thread = (yscr__PostThreadMessageW_fn)(yscr_proc)GetProcAddress(u, "PostThreadMessageW");
        yscr__win.key_state = (yscr__GetAsyncKeyState_fn)(yscr_proc)GetProcAddress(u, "GetAsyncKeyState");
        yscr__win.foreground = (yscr__GetForegroundWindow_fn)(yscr_proc)GetProcAddress(u, "GetForegroundWindow");
        yscr__win.set_foreground = (yscr__SetForegroundWindow_fn)(yscr_proc)GetProcAddress(u, "SetForegroundWindow");
        yscr__win.allow_foreground = (yscr__AllowSetForegroundWindow_fn)(yscr_proc)GetProcAddress(u, "AllowSetForegroundWindow");
        yscr__win.is_window = (yscr__IsWindow_fn)(yscr_proc)GetProcAddress(u, "IsWindow");
        yscr__win.window_pid = (yscr__GetWindowThreadProcessId_fn)(yscr_proc)GetProcAddress(u, "GetWindowThreadProcessId");
        yscr__win.is_hung = (yscr__IsHungAppWindow_fn)(yscr_proc)GetProcAddress(u, "IsHungAppWindow");
        yscr__win.class_name = (yscr__GetClassNameW_fn)(yscr_proc)GetProcAddress(u, "GetClassNameW");
        yscr__win.create_icon = (yscr__CreateIconIndirect_fn)(yscr_proc)GetProcAddress(u, "CreateIconIndirect");
        yscr__win.destroy_icon = (yscr__DestroyIcon_fn)(yscr_proc)GetProcAddress(u, "DestroyIcon");
        yscr__win.send_message = (yscr__SendMessageW_fn)(yscr_proc)GetProcAddress(u, "SendMessageW");
        yscr__win.window_dpi = (yscr__GetDpiForWindow_fn)(yscr_proc)GetProcAddress(u, "GetDpiForWindow");
        yscr__win.metrics_dpi = (yscr__GetSystemMetricsForDpi_fn)(yscr_proc)GetProcAddress(u, "GetSystemMetricsForDpi");
        yscr__win.raw_register = (yscr__RegisterRawInputDevices_fn)(yscr_proc)GetProcAddress(u, "RegisterRawInputDevices");
        yscr__win.raw_registered = (yscr__GetRegisteredRawInputDevices_fn)(yscr_proc)GetProcAddress(u, "GetRegisteredRawInputDevices");
        yscr__win.raw_buffer = (yscr__GetRawInputBuffer_fn)(yscr_proc)GetProcAddress(u, "GetRawInputBuffer");
        yscr__win.register_class = (yscr__RegisterClassW_fn)(yscr_proc)GetProcAddress(u, "RegisterClassW");
        yscr__win.create_window = (yscr__CreateWindowExW_fn)(yscr_proc)GetProcAddress(u, "CreateWindowExW");
        yscr__win.destroy_window = (yscr__DestroyWindow_fn)(yscr_proc)GetProcAddress(u, "DestroyWindow");
        yscr__win.def_proc = (yscr__DefWindowProcW_fn)(yscr_proc)GetProcAddress(u, "DefWindowProcW");
        yscr__win.msg_wait = (yscr__MsgWaitForMultipleObjects_fn)(yscr_proc)GetProcAddress(u, "MsgWaitForMultipleObjects");
        yscr__win.msg_wait_ex = (yscr__MsgWaitForMultipleObjectsEx_fn)(yscr_proc)GetProcAddress(u, "MsgWaitForMultipleObjectsEx");
        yscr__win.post_message = (yscr__PostMessageW_fn)(yscr_proc)GetProcAddress(u, "PostMessageW");
    }
    if (g) {
        yscr__win.get_ramp = (yscr__GammaRamp_fn)(yscr_proc)GetProcAddress(g, "GetDeviceGammaRamp");
        yscr__win.set_ramp = (yscr__GammaRamp_fn)(yscr_proc)GetProcAddress(g, "SetDeviceGammaRamp");
        yscr__win.create_dc = (yscr__CreateDCW_fn)(yscr_proc)GetProcAddress(g, "CreateDCW");
        yscr__win.delete_dc = (yscr__DeleteDC_fn)(yscr_proc)GetProcAddress(g, "DeleteDC");
        yscr__win.create_dib = (yscr__CreateDIBSection_fn)(yscr_proc)GetProcAddress(g, "CreateDIBSection");
        yscr__win.create_bitmap = (yscr__CreateBitmap_fn)(yscr_proc)GetProcAddress(g, "CreateBitmap");
        yscr__win.delete_object = (yscr__DeleteObject_fn)(yscr_proc)GetProcAddress(g, "DeleteObject");
    }
}

/* An 8-bit identity: every entry's high byte is its index. */
static int yscr__ramp_identity(const WORD* r) {   /* 3 x 256, red first */
    int i;
    for (i = 0; i < 768; i++) if ((r[i] >> 8) != (i & 255)) return 0;
    return 1;
}

/* Ramps this process set, to restore at close, at exit and on SDL's quit. */
#define YSCR__GAMMA_MAX 8
static struct yscr__gamma_entry {
    volatile int32_t used;
    WCHAR dev[32];
    WORD  saved[3][256];
} yscr__gamma[YSCR__GAMMA_MAX];
static int yscr__gamma_atexit, yscr__gamma_watch;

/* close(), exit and the panic watchdog's thread can each get here: the
 * exchange lets one of them restore an entry. */
static void yscr__gamma_restore(int k) {
    HDC dc;
    if (k < 0 || k >= YSCR__GAMMA_MAX) return;
    if (InterlockedExchange((volatile LONG*)&yscr__gamma[k].used, 0) == 0) return;
    if (!yscr__win.create_dc || !yscr__win.set_ramp) return;
    dc = yscr__win.create_dc(yscr__gamma[k].dev, NULL, NULL, NULL);
    if (!dc) return;
    yscr__win.set_ramp(dc, yscr__gamma[k].saved);
    yscr__win.delete_dc(dc);
}

static void yscr__gamma_restore_all(void) {
    int k;
    for (k = 0; k < YSCR__GAMMA_MAX; k++) yscr__gamma_restore(k);
}

static bool SDLCALL yscr__gamma_quit_watch(void* ud, SDL_Event* e) {
    (void)ud;
    if (e && e->type == SDL_EVENT_QUIT) yscr__gamma_restore_all();
    return true;
}

static void yscr__copy_w(char* dst, size_t cap, const WCHAR* w) {
    size_t i;
    for (i = 0; i + 1 < cap && w[i]; i++) dst[i] = (char)(w[i] < 128 ? w[i] : '?');
    dst[i] = '\0';
}

/* Read what the header can of the display's color path, and take the OS
 * gamma ramp for a fullscreen screen unless desc.keep_os_gamma. */
static void yscr__win_display_state(yscr_screen* s, const yscr_desc* desc) {
    HWND hwnd;
    HMONITOR mon;
    MONITORINFOEXW mi;
    yscr__dc_path paths[32];
    yscr__dc_mode modes[64];
    UINT32 np = 32, nm = 64, i;
    int found = 0, cm = -1, bpc = 0, pw = 0, ph = 0;
    const char* gamma = "kept";
    char dev[40];
    s->code_risk = 0;
    yscr__win_load();
    hwnd = s->window ? (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(s->window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL) : NULL;
    mon = (hwnd && yscr__win.monitor_from_window) ? yscr__win.monitor_from_window(hwnd, MONITOR_DEFAULTTONEAREST) : NULL;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!mon || !yscr__win.monitor_info || !yscr__win.monitor_info(mon, (LPMONITORINFO)&mi)) {
        s->code_risk |= YSCR_CODE_RISK_STATE_UNKNOWN;
        yscr__copy(s->os_color, sizeof s->os_color, "os_color=unknown");
        return;
    }
    yscr__copy_w(dev, sizeof dev, mi.szDevice);
    /* advanced color and the panel's own mode, through the display config */
    if (yscr__win.query && yscr__win.info &&
        yscr__win.query(2u /* QDC_ONLY_ACTIVE_PATHS */, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS) {
        for (i = 0; i < np && !found; i++) {
            yscr__dc_source_name src;
            memset(&src, 0, sizeof src);
            src.header.type = YSCR__DCI_SOURCE_NAME;
            src.header.size = sizeof src;
            src.header.adapterId = paths[i].src_adapter;
            src.header.id = paths[i].src_id;
            if (yscr__win.info(&src.header) != ERROR_SUCCESS || wcscmp(src.viewGdiDeviceName, mi.szDevice) != 0) continue;
            found = 1;
            {
                yscr__aci2 a2;
                yscr__aci a1;
                yscr__dc_preferred pm;
                memset(&a2, 0, sizeof a2);
                a2.header.type = YSCR__DCI_ADVANCED_COLOR_2;
                a2.header.size = sizeof a2;
                a2.header.adapterId = paths[i].tgt_adapter;
                a2.header.id = paths[i].tgt_id;
                if (yscr__win.info(&a2.header) == ERROR_SUCCESS) {
                    cm = (int)a2.activeColorMode;
                    bpc = (int)a2.bitsPerColorChannel;
                } else {
                    memset(&a1, 0, sizeof a1);
                    a1.header.type = YSCR__DCI_ADVANCED_COLOR;
                    a1.header.size = sizeof a1;
                    a1.header.adapterId = paths[i].tgt_adapter;
                    a1.header.id = paths[i].tgt_id;
                    if (yscr__win.info(&a1.header) == ERROR_SUCCESS) {
                        cm = (a1.value & 2u) ? 2 : ((a1.value & 4u) ? 1 : 0);
                        bpc = (int)a1.bitsPerColorChannel;
                    }
                }
                memset(&pm, 0, sizeof pm);
                pm.header.type = YSCR__DCI_PREFERRED_MODE;
                pm.header.size = sizeof pm;
                pm.header.adapterId = paths[i].tgt_adapter;
                pm.header.id = paths[i].tgt_id;
                if (yscr__win.info(&pm.header) == ERROR_SUCCESS) { pw = (int)pm.width; ph = (int)pm.height; }
            }
        }
    }
    if (cm < 0) s->code_risk |= YSCR_CODE_RISK_STATE_UNKNOWN;
    if (cm > 0) s->code_risk |= YSCR_CODE_RISK_ADVANCED_COLOR;
    if (!desc->windowed && pw && ph && (pw != s->caps.mode.w || ph != s->caps.mode.h)) s->code_risk |= YSCR_CODE_RISK_SCALED;
    /* the OS ramp: fullscreen only, never a window's, which shares the
     * display with everything else */
    if (!desc->windowed && !desc->keep_os_gamma) {
        HDC dc = yscr__win.create_dc ? yscr__win.create_dc(mi.szDevice, NULL, NULL, NULL) : NULL;
        WORD cur[3][256], id[3][256], back[3][256];
        int k, c;
        gamma = "unreadable";
        if (dc && yscr__win.get_ramp && yscr__win.get_ramp(dc, cur)) {
            if (yscr__ramp_identity(&cur[0][0])) {
                gamma = "identity";
            } else {
                for (c = 0; c < 3; c++) for (k = 0; k < 256; k++) id[c][k] = (WORD)(k * 257);
                for (k = 0; k < YSCR__GAMMA_MAX && yscr__gamma[k].used; k++) { }
                gamma = "refused";
                if (k < YSCR__GAMMA_MAX && yscr__win.set_ramp) {
                    /* registered before the set, so an exit at any point
                     * puts the user's ramp back */
                    memcpy(yscr__gamma[k].saved, cur, sizeof cur);
                    memcpy(yscr__gamma[k].dev, mi.szDevice, sizeof yscr__gamma[k].dev);
                    (void)InterlockedExchange((volatile LONG*)&yscr__gamma[k].used, 1);
                    if (!yscr__gamma_atexit) { yscr__gamma_atexit = 1; atexit(yscr__gamma_restore_all); }
                    if (!yscr__gamma_watch) { yscr__gamma_watch = SDL_AddEventWatch(yscr__gamma_quit_watch, NULL) ? 1 : 0; }
                    if (yscr__win.set_ramp(dc, id) && yscr__win.get_ramp(dc, back) && yscr__ramp_identity(&back[0][0])) {
                        s->gamma_owned = k + 1;
                        gamma = "set";
                    } else {
                        yscr__gamma_restore(k);   /* Windows may accept and ignore a ramp */
                    }
                }
            }
        }
        if (dc) yscr__win.delete_dc(dc);
        if (strcmp(gamma, "identity") != 0 && strcmp(gamma, "set") != 0) s->code_risk |= YSCR_CODE_RISK_GAMMA;
    } else {
        /* not ours to change: report it */
        HDC dc = yscr__win.create_dc ? yscr__win.create_dc(mi.szDevice, NULL, NULL, NULL) : NULL;
        WORD cur[3][256];
        if (dc && yscr__win.get_ramp && yscr__win.get_ramp(dc, cur)) {
            gamma = yscr__ramp_identity(&cur[0][0]) ? "identity(kept)" : "not-identity(kept)";
            if (!yscr__ramp_identity(&cur[0][0])) s->code_risk |= YSCR_CODE_RISK_GAMMA;
        } else {
            gamma = "unreadable(kept)";
            s->code_risk |= YSCR_CODE_RISK_GAMMA;
        }
        if (dc) yscr__win.delete_dc(dc);
    }
    snprintf(s->os_color, sizeof s->os_color, "display=%s color=%s wire_bpc=%d panel=%dx%d os_gamma=%s mhc=unknown "
             "nightlight=unknown", dev, cm == 0 ? "sdr" : cm == 1 ? "wcg" : cm == 2 ? "hdr" : "unknown", bpc, pw, ph, gamma);
}

static void yscr__win_gamma_release(yscr_screen* s) {
    int k, any = 0;
    if (s->gamma_owned) yscr__gamma_restore(s->gamma_owned - 1);
    s->gamma_owned = 0;
    for (k = 0; k < YSCR__GAMMA_MAX; k++) any |= yscr__gamma[k].used;
    if (!any && yscr__gamma_watch) { SDL_RemoveEventWatch(yscr__gamma_quit_watch, NULL); yscr__gamma_watch = 0; }
}

/* Size k of the header's icon as an HICON, or NULL. */
static HICON yscr__win_icon(int k) {
    int side = yscr__icon_size[k][0], i;
    BITMAPINFO bi;
    void* bits = NULL;
    HBITMAP color, mask;
    ICONINFO ii;
    HICON ic = NULL;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = side;
    bi.bmiHeader.biHeight = -side;   /* top row first */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    color = yscr__win.create_dib(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    mask = yscr__win.create_bitmap(side, side, 1, 1, NULL);
    if (color && mask && bits && yscr__icon_decode(k, (uint8_t*)bits, side * 4)) {
        uint8_t* p = (uint8_t*)bits;
        for (i = 0; i < side * side; i++) { uint8_t t = p[i * 4]; p[i * 4] = p[i * 4 + 2]; p[i * 4 + 2] = t; }   /* BGRA */
        memset(&ii, 0, sizeof ii);
        ii.fIcon = TRUE;
        ii.hbmColor = color;
        ii.hbmMask = mask;
        ic = yscr__win.create_icon(&ii);
    }
    if (color) yscr__win.delete_object(color);
    if (mask) yscr__win.delete_object(mask);
    return ic;
}

/* SDL gives a window one icon at its base size, 32 pixels, for the title
 * bar too, where Windows shrinks it (measured, docs/screen.md); the
 * header's icon has a size drawn for each. The smallest size at least as
 * large as the window's icon metric. */
static void yscr__win_icons(yscr_screen* s) {
    HWND hwnd = (HWND)s->hwnd;
    UINT dpi;
    int which, k;
    yscr__win_load();
    if (!hwnd || !yscr__win.create_icon || !yscr__win.send_message || !yscr__win.create_dib ||
        !yscr__win.create_bitmap || !yscr__win.delete_object) return;
    dpi = yscr__win.window_dpi ? yscr__win.window_dpi(hwnd) : 96;
    for (which = 0; which < 2; which++) {
        int metric = which == 0 ? SM_CXSMICON : SM_CXICON;
        int want = yscr__win.metrics_dpi ? yscr__win.metrics_dpi(metric, dpi) : (which == 0 ? 16 : 32);
        HICON ic;
        for (k = 0; k < 2 && yscr__icon_size[k][0] < want; k++) { }
        ic = yscr__win_icon(k);
        if (!ic) continue;
        yscr__win.send_message(hwnd, WM_SETICON, which == 0 ? ICON_SMALL : ICON_BIG, (LPARAM)ic);
        s->win_icon[which] = ic;
    }
}

/* --- the window thread (WINDOW THREAD) -------------------------------------
 * One thread per process makes every window, runs its messages and makes
 * every SDL video call, so a host that idles between frames (time.sleep,
 * input(), Octave's pause) never leaves a window without a pump: Windows
 * marks a window hung after about 4.2 s without one and puts a ghost in
 * front (docs/winthread_probe.md). The frame thread keeps the D3D11 device,
 * the swap path and ANGLE. What keeps the two free of deadlocks (DXGI
 * overview, "Multithread considerations"): the window thread never waits on
 * the frame thread; the frame thread waits on it only for a command, never
 * in a frame. SDL makes the thread that starts its video subsystem its main
 * thread (SDL.c, SDL_InitSubSystem), so this thread starts and stops it. */
#if defined(YSCR__WT)
/* A command the thread has not taken in 5 s is withdrawn before it runs:
 * by the OS's own rule a window whose thread reads no message for 5 s is
 * hung. */
#define YSCR__WT_TAKE_MS  5000u
/* close()'s command, once taken: the destroy of a fullscreen window in a
 * switched mode put the desktop mode back in 1.2 s (measured). */
#define YSCR__WT_CLOSE_MS 5000u
/* The heartbeat: the thread wakes at least this often, so the panic tells a
 * live thread (a beat within 1 s) from a hung one. 4 ms while gamepads are
 * open: SDL polls XInput in its pump. */
#define YSCR__WT_BEAT_MS  100u
#define YSCR__WT_PADS_MS  4u
#define YSCR__WT_ALIVE_NS 1000000000LL

enum { YSCR__WTC_IDLE = 0, YSCR__WTC_POSTED, YSCR__WTC_RUNNING, YSCR__WTC_DONE, YSCR__WTC_CANCELED };

static struct {
    HANDLE           thread;
    DWORD            tid;
    HANDLE           cmd_ev;        /* auto-reset: a command, a release, the stop */
    HANDLE           done_ev;       /* auto-reset: the command ran                */
    CRITICAL_SECTION lock;          /* one command at a time; never deleted       */
    int              lock_ok;
    yscr_window_fn   fn;
    void*            ctx;
    volatile LONG    state;         /* YSCR__WTC_*                                */
    volatile LONG    quit;
    volatile LONG    video;         /* screens whose SDL video it holds: it pumps */
    volatile LONG    pads;          /* screens with gamepads                      */
    volatile int64_t beat;          /* the loop's last pass, rt ns                */
    volatile LONG    release_req;   /* panics that asked for a release            */
    volatile LONG    release_start; /* ... the last one it began                  */
    volatile LONG    release_done;  /* ... the last one it served                 */
    volatile int64_t t_release;     /* when the last release ran, rt ns           */
    volatile LONG    stuck;         /* a command or the stop timed out            */
    int              users;         /* screens whose window it owns               */
    SDL_Window*      wins[YSCR__SLOTS];   /* its windows; this thread only        */
} yscr__wt;

static int yscr__wt_on_thread(void) { return yscr__wt.thread && GetCurrentThreadId() == yscr__wt.tid; }

/* The panic's release (PANIC): the display goes back to the operator and
 * the process lives. Minimized, not destroyed: the frame thread still holds
 * a swap path on each window. SDL leaves fullscreen when a fullscreen
 * window is minimized, which puts back a switched mode. */
static void yscr__wt_do_release(LONG req) {
    int i;
    InterlockedExchange(&yscr__wt.release_start, req);
    for (i = 0; i < YSCR__SLOTS; i++) if (yscr__wt.wins[i]) SDL_MinimizeWindow(yscr__wt.wins[i]);
    SDL_ShowCursor();
    yscr__a_store64(&yscr__wt.t_release, yscr__now());
    InterlockedExchange(&yscr__wt.release_done, req);
}

static DWORD WINAPI yscr__wt_main(LPVOID arg) {
    MSG m;
    (void)arg;
    yscr__win.peek_message(&m, NULL, WM_USER, WM_USER, PM_NOREMOVE);   /* the queue, before ready */
    yscr__a_store64(&yscr__wt.beat, yscr__now());
    SetEvent(yscr__wt.done_ev);
    while (!yscr__wt.quit) {
        LONG req;
        DWORD r = yscr__win.msg_wait_ex(1, &yscr__wt.cmd_ev, yscr__wt.pads ? YSCR__WT_PADS_MS : YSCR__WT_BEAT_MS,
                                        QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0 &&
            InterlockedCompareExchange(&yscr__wt.state, YSCR__WTC_RUNNING, YSCR__WTC_POSTED) == YSCR__WTC_POSTED) {
            yscr__wt.fn(yscr__wt.ctx);
            InterlockedExchange(&yscr__wt.state, YSCR__WTC_DONE);
            SetEvent(yscr__wt.done_ev);
        }
        req = InterlockedCompareExchange(&yscr__wt.release_req, 0, 0);
        if (req != yscr__wt.release_done) yscr__wt_do_release(req);
        if (yscr__wt.video > 0) SDL_PumpEvents();
        yscr__a_store64(&yscr__wt.beat, yscr__now());
    }
    SDL_CleanupTLS();   /* SDL's per-thread memory of this thread */
    return 0;
}

/* Runs fn(ctx) on the window thread and waits; on that thread, or with no
 * thread, runs it here. A command not taken in 5 s is withdrawn
 * (YSCR_ERR_TIMEOUT; fn did not run). Once taken: run_ms 0 waits as long as
 * fn runs, because fn uses the caller's memory; else at most run_ms, then
 * the thread is left behind (stuck) and YSCR_ERR_TIMEOUT. */
static int yscr__wt_run(yscr_window_fn fn, void* ctx, DWORD run_ms) {
    if (!yscr__wt.thread || yscr__wt_on_thread()) { fn(ctx); return YSCR_OK; }
    if (yscr__wt.stuck) return YSCR_ERR_LOST;
    EnterCriticalSection(&yscr__wt.lock);
    yscr__wt.fn = fn;
    yscr__wt.ctx = ctx;
    InterlockedExchange(&yscr__wt.state, YSCR__WTC_POSTED);
    SetEvent(yscr__wt.cmd_ev);
    if (WaitForSingleObject(yscr__wt.done_ev, YSCR__WT_TAKE_MS) != WAIT_OBJECT_0) {
        if (InterlockedCompareExchange(&yscr__wt.state, YSCR__WTC_CANCELED, YSCR__WTC_POSTED) == YSCR__WTC_POSTED) {
            LeaveCriticalSection(&yscr__wt.lock);
            return YSCR_ERR_TIMEOUT;
        }
        if (WaitForSingleObject(yscr__wt.done_ev, run_ms ? run_ms : INFINITE) != WAIT_OBJECT_0) {
            InterlockedExchange(&yscr__wt.stuck, 1);
            LeaveCriticalSection(&yscr__wt.lock);
            return YSCR_ERR_TIMEOUT;
        }
    }
    InterlockedExchange(&yscr__wt.state, YSCR__WTC_IDLE);
    LeaveCriticalSection(&yscr__wt.lock);
    return YSCR_OK;
}

/* A screen that uses the thread; the first starts it. NULL or why not. */
static const char* yscr__wt_acquire(void) {
    if (yscr__wt.stuck)
        return "the window thread did not answer at an earlier close; SDL's video belongs to it until the process ends";
    if (yscr__wt.users > 0) { yscr__wt.users++; return NULL; }
    yscr__win_load();
    if (!yscr__win.msg_wait_ex || !yscr__win.peek_message) return "window thread: user32.dll lacks MsgWaitForMultipleObjectsEx";
    if (!yscr__wt.lock_ok) { InitializeCriticalSection(&yscr__wt.lock); yscr__wt.lock_ok = 1; }
    yscr__wt.cmd_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    yscr__wt.done_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    yscr__wt.quit = 0;
    yscr__wt.state = YSCR__WTC_IDLE;
    yscr__wt.video = yscr__wt.pads = 0;
    yscr__wt.release_req = yscr__wt.release_start = yscr__wt.release_done = 0;
    memset(yscr__wt.wins, 0, sizeof yscr__wt.wins);
    yscr__wt.thread = yscr__wt.cmd_ev && yscr__wt.done_ev ? CreateThread(NULL, 0, yscr__wt_main, NULL, 0, &yscr__wt.tid) : NULL;
    if (!yscr__wt.thread || WaitForSingleObject(yscr__wt.done_ev, YSCR__WT_TAKE_MS) != WAIT_OBJECT_0) {
        if (yscr__wt.thread) {   /* its loop never ran: nothing of SDL's is on it */
            InterlockedExchange(&yscr__wt.quit, 1);
            SetEvent(yscr__wt.cmd_ev);
            WaitForSingleObject(yscr__wt.thread, YSCR__WT_TAKE_MS);
            CloseHandle(yscr__wt.thread);
        }
        if (yscr__wt.cmd_ev) CloseHandle(yscr__wt.cmd_ev);
        if (yscr__wt.done_ev) CloseHandle(yscr__wt.done_ev);
        yscr__wt.thread = yscr__wt.cmd_ev = yscr__wt.done_ev = NULL;
        yscr__wt.tid = 0;
        return "window thread: CreateThread failed, or the thread did not start in 5 s";
    }
    yscr__wt.users = 1;
    return NULL;
}

/* The last screen stops the thread. One that does not stop in 5 s is left
 * behind with its handles (stuck), and close() returns. */
static void yscr__wt_release(void) {
    if (yscr__wt.users <= 0 || --yscr__wt.users > 0) return;
    if (yscr__wt.stuck) return;
    InterlockedExchange(&yscr__wt.quit, 1);
    SetEvent(yscr__wt.cmd_ev);
    if (WaitForSingleObject(yscr__wt.thread, YSCR__WT_CLOSE_MS) != WAIT_OBJECT_0) {
        InterlockedExchange(&yscr__wt.stuck, 1);
        return;
    }
    CloseHandle(yscr__wt.thread);
    CloseHandle(yscr__wt.cmd_ev);
    CloseHandle(yscr__wt.done_ev);
    yscr__wt.thread = yscr__wt.cmd_ev = yscr__wt.done_ev = NULL;
    yscr__wt.tid = 0;
    yscr__a_store(&yscr__released, 0);   /* the next open is a new session */
}

/* The thread answered within the last second: its heartbeat is newer. */
static int yscr__wt_alive(void) {
    return yscr__wt.thread && !yscr__wt.stuck &&
           yscr__now() - yscr__a_load64(&yscr__wt.beat) < YSCR__WT_ALIVE_NS;
}

static int yscr__wt_running(void) { return yscr__wt.thread != NULL; }
static int yscr__wt_call(yscr_window_fn fn, void* ctx) { return yscr__wt_run(fn, ctx, 0); }
#endif /* YSCR__WT */

/* --- the panic watchdog (PANIC) ------------------------------------------- */

static struct {
    HANDLE          thread;
    DWORD           tid;
    HHOOK           hook;
    HANDLE          ready;
    volatile LONG   panicking;
    int             armed;          /* screens armed                           */
    UINT            vk;
    int32_t         key_down;       /* the hook's own state of the key: repeats */
    yscr_panic_fn fn;
    void*           ctx;
    volatile int64_t t_panic;       /* when the panic began, for a test        */
} yscr__wd;

/* A key the hook can name: Esc, F1 to F12, a letter or a digit. */
static UINT yscr__vk_of(uint32_t key) {
    if (!yscr__vk_known(key)) return 0;
    if (key == YSCR_KEY_ESCAPE) return VK_ESCAPE;
    if (key >= 0x4000003Au && key <= 0x40000045u) return VK_F1 + (key - 0x4000003Au);   /* SDLK_F1..F12 */
    if (key >= 'a' && key <= 'z') return 'A' + (key - 'a');
    if (key >= '0' && key <= '9') return key;
    return 0;
}

/* Is the foreground window this process's? About 5 s into a hang Windows
 * puts a "Ghost" window of another process in front of a hung one
 * (measured, docs/screen.md), so a ghost counts while one of the
 * screens' windows is hung. */
static int yscr__wd_ours(void) {
    HWND h = yscr__win.foreground();
    DWORD pid = 0;
    WCHAR cls[8];
    int i;
    if (!h) return 0;
    yscr__win.window_pid(h, &pid);
    if (pid == GetCurrentProcessId()) return 1;
    if (!yscr__win.class_name || !yscr__win.is_hung || yscr__win.class_name(h, cls, 8) != 5 ||
        wcscmp(cls, L"Ghost") != 0) return 0;
    for (i = 0; i < YSCR__SLOTS; i++) {
        yscr_screen* s = yscr__slot[i].s;
        if (s && s->hwnd && yscr__win.is_hung((HWND)s->hwnd)) return 1;
    }
    return 0;
}

/* Every key of the session passes through here while the watchdog is
 * armed, and Windows skips a hook that takes longer than
 * LowLevelHooksTimeout: no lock but the abort state's, no I/O, no wait. */
static LRESULT CALLBACK yscr__wd_hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && lp) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lp;
        if (k->vkCode == yscr__wd.vk) {
            if (yscr__abort_edge(&yscr__wd.key_down, wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) && yscr__wd_ours()) {
                uint32_t m = 0;
                if (yscr__win.key_state(VK_SHIFT) < 0) m |= YSCR_MOD_SHIFT;
                if (yscr__win.key_state(VK_CONTROL) < 0) m |= YSCR_MOD_CTRL;
                if (yscr__win.key_state(VK_MENU) < 0) m |= YSCR_MOD_ALT;
                if (yscr__win.key_state(VK_LWIN) < 0 || yscr__win.key_state(VK_RWIN) < 0) m |= YSCR_MOD_GUI;
                if (yscr__abort_key(yscr__ab.key, m, yscr__now(),
                                      YSCR__AB_HOOK | ((k->flags & LLKHF_INJECTED) ? YSCR__AB_INJECTED : 0u)))
                    yscr__win.post_thread(yscr__wd.tid, WM_APP, 0, 0);
            }
        }
    }
    return yscr__win.next_hook(NULL, code, wp, lp);
}

static DWORD WINAPI yscr__wd_last_words(LPVOID arg) {
    (void)arg;
    if (yscr__wd.fn) yscr__wd.fn(yscr__wd.ctx);
    return 0;
}

/* The caller's last words run on their own thread, for at most 1 s: they
 * may wait on something the hung thread holds. */
static void yscr__wd_bounded(LPTHREAD_START_ROUTINE fn) {
    HANDLE h = CreateThread(NULL, 0, fn, NULL, 0, NULL);
    if (h) { WaitForSingleObject(h, 1000); CloseHandle(h); }
}

/* The frame loop looks hung: put back the gamma ramps, then end the
 * process. TerminateProcess, not exit(): exit runs atexit handlers and DLL
 * detach, which can wait on locks the hung thread holds, and only the end
 * of the process takes down a window and a swapchain that another thread
 * owns. Windows puts back a switched display mode as the process ends
 * (measured: ChangeDisplaySettingsExW from here blocked over 1 s on the
 * hung window and the mode came back no sooner). */
static void yscr__panic_records(int64_t now, uint32_t released) {
    int i;
    for (i = 0; i < YSCR__SLOTS; i++) {
        yscr_screen* s = yscr__slot[i].s;
        if (s && s->panic_armed == 1 && s->ring) {
            yrt_event ev;
            memset(&ev, 0, sizeof ev);
            ev.source = (uint16_t)YRT_SRC_SCREEN;
            ev.kind = (uint16_t)YSCR_EV_PANIC;
            ev.t_ns = (uint64_t)now;
            ev.aux = s->display_index;
            ev.u.u32[0] = (uint32_t)yscr__ab.presses;
            ev.u.u32[1] = released;
            ev.u.i64[1] = yscr__a_load64(&yscr__ab.ack);
            yrt_ring_push(s->ring, &ev);
        }
    }
}

static void yscr__panic(void) {
    int64_t now = yscr__now();
    if (InterlockedCompareExchange(&yscr__wd.panicking, 1, 0) != 0) return;
    yscr__wd.t_panic = now;
#if defined(YSCR__WT)
    /* Release, not end, while the window thread answers (decided
     * 2026-10-26): the frame thread may only be idle (MATLAB's pause,
     * Python's input()), and only the windows, the cursor, the mode and the
     * gamma ramps are the operator's to get back. The program learns it at
     * its next begin() and may only close. */
    if (yscr__wt_alive()) {
        LONG req = InterlockedIncrement(&yscr__wt.release_req);
        int k, quit = 0;
        SetEvent(yscr__wt.cmd_ev);
        /* 1 s for the window thread to begin the release, then up to 5 s
         * for it to end: minimizing a fullscreen window in a switched mode
         * puts the desktop mode back first, which took 1.2 to 1.4 s
         * (measured; v0.10.0's 1 s in all ended the process). The hook
         * runs on this thread, so its messages are read meanwhile; a later
         * press is the same panic. */
        for (k = 0; k < 500 && yscr__wt.release_done != req && (k < 100 || yscr__wt.release_start == req); k++) {
            MSG m;
            while (yscr__win.peek_message(&m, NULL, 0, 0, PM_REMOVE)) if (m.message == WM_QUIT) quit = 1;
            Sleep(10);
        }
        if (quit) yscr__win.post_thread(yscr__wd.tid, WM_QUIT, 0, 0);
        if (yscr__wt.release_done == req) {
            yscr__gamma_restore_all();
            yscr__panic_records(now, 1);
            yscr__abort_push(YSCR_ABORT_PANIC, YSCR__AB_HOOK, 0, 0, now);
            yscr__a_store(&yscr__released, 1);
            InterlockedExchange(&yscr__wd.panicking, 0);
            return;
        }
    }
#endif
    /* no key of the session waits on this thread from here */
    if (yscr__wd.hook) { yscr__win.unhook(yscr__wd.hook); yscr__wd.hook = NULL; }
    yscr__panic_records(now, 0);
    yscr__gamma_restore_all();
    if (yscr__wd.fn) yscr__wd_bounded(yscr__wd_last_words);
    TerminateProcess(GetCurrentProcess(), YSCR_PANIC_EXIT_CODE);
}

static DWORD WINAPI yscr__wd_main(LPVOID arg) {
    MSG m;
    HMODULE self = NULL;
    (void)arg;
    yscr__win.peek_message(&m, NULL, WM_USER, WM_USER, PM_NOREMOVE);   /* the queue, before ready */
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void*)&yscr__wd, &self);
    yscr__wd.hook = yscr__win.set_hook(WH_KEYBOARD_LL, yscr__wd_hook, self, 0);
    SetEvent(yscr__wd.ready);
    if (!yscr__wd.hook) return 1;
    while (yscr__win.get_message(&m, NULL, 0, 0) > 0)
        if (m.message == WM_APP) yscr__panic();
    if (yscr__wd.hook) { yscr__win.unhook(yscr__wd.hook); yscr__wd.hook = NULL; }
    return 0;
}


/* --- the raw mouse reader (desc.raw_mice; INPUT, "Raw mice") ------------------------
 * One per process: Raw Input registration is per process and per usage, so
 * a second registration for mice would take the first one's. SDL's own
 * thread keeps the keyboard (usage 6); this one takes mice (usage 2), flags
 * 0: input only while this process is in front (a participant's clicks are
 * not taken while another program has the focus; an operator window in
 * this process, ImGui's included, keeps the focus in the process). */
#define YSCR__RM_REGISTER (WM_APP + 0x51)
static int yscr__rm_still_ours(void);
static struct {
    volatile int32_t users;
    volatile int32_t registered;   /* the reader's registration is in place */
    HANDLE thread, quit, ready;
    HWND   hwnd;
    UINT   offset;                 /* RAWINPUTHEADER as GetRawInputBuffer lays it */
    uint64_t buf[2048];            /* 16 KB, 8-byte aligned for GetRawInputBuffer */
} yscr__rm;

static LRESULT CALLBACK yscr__rm_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    return yscr__win.def_proc(h, msg, w, l);
}

static int yscr__rm_register(void) {
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 1;
    rid.usUsage = 2;
    rid.dwFlags = 0;
    rid.hwndTarget = yscr__rm.hwnd;
    return yscr__win.raw_register(&rid, 1, sizeof rid) ? 1 : 0;
}

/* Every report waiting, stamped when GetRawInputBuffer() returned: the
 * reports of one call share a stamp (SDL interpolates mouse times across a
 * call; an interpolated time would be a guess). */
static void yscr__rm_drain(void) {
    for (;;) {
        UINT size = (UINT)sizeof yscr__rm.buf, count, i;
        RAWINPUT* in = (RAWINPUT*)(void*)yscr__rm.buf;
        int64_t t;
        count = yscr__win.raw_buffer(in, &size, sizeof(RAWINPUTHEADER));
        t = YSCR__NOW();
        if (count == 0 || count == (UINT)-1) return;
        for (i = 0; i < count; i++) {
            if (in->header.dwType == RIM_TYPEMOUSE) {
                const RAWMOUSE* m = (const RAWMOUSE*)(const void*)((const BYTE*)in + yscr__rm.offset);
                yscr_mouse_event e;
                yin_event ev[12];
                int k, n;
                yscr__mouse_decode(m->usFlags, m->usButtonFlags, m->usButtonData, m->lLastX, m->lLastY,
                                     (uint32_t)(uintptr_t)in->header.hDevice, t, &e);
                /* onto the bridge: one doorbell per event */
                n = yin_from_mouse(&e, ev, 12);
                for (k = 0; k < n; k++) yscr__in_push(&ev[k], 1);
            }
            /* NEXTRAWINPUTBLOCK, which MinGW's headers lack (no QWORD) */
            in = (RAWINPUT*)(void*)(((uintptr_t)in + in->header.dwSize + 7u) & ~(uintptr_t)7u);
        }
    }
}

static DWORD WINAPI yscr__rm_main(LPVOID arg) {
    WNDCLASSW wc;
    MSG m;
    (void)arg;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = yscr__rm_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ysp_screen_raw_mice";
    yscr__win.register_class(&wc);   /* fails harmlessly when a reader ran before */
    yscr__rm.hwnd = yscr__win.create_window(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                                                 wc.hInstance, NULL);
    if (yscr__rm.hwnd && yscr__rm_register()) yscr__a_store(&yscr__rm.registered, 1);
    /* the raw keyboard's thread runs at this priority too (SDL) */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetEvent(yscr__rm.ready);
    if (!yscr__a_load(&yscr__rm.registered)) return 1;
    for (;;) {
        DWORD w = yscr__win.msg_wait(1, &yscr__rm.quit, FALSE, INFINITE, QS_RAWINPUT | QS_POSTMESSAGE);
        if (w != WAIT_OBJECT_0 + 1) break;
        yscr__rm_drain();
        /* only the reader's own requests: WM_INPUT stays for the buffer */
        while (yscr__win.peek_message(&m, NULL, YSCR__RM_REGISTER, YSCR__RM_REGISTER, PM_REMOVE))
            yscr__a_store(&yscr__rm.registered, yscr__rm_register());
    }
    {
        RAWINPUTDEVICE rid;
        rid.usUsagePage = 1;
        rid.usUsage = 2;
        rid.dwFlags = RIDEV_REMOVE;
        rid.hwndTarget = NULL;
        if (yscr__rm_still_ours()) yscr__win.raw_register(&rid, 1, sizeof rid);
    }
    yscr__win.destroy_window(yscr__rm.hwnd);
    yscr__rm.hwnd = NULL;
    return 0;
}

/* Whether the process's mouse registration still targets the reader's
 * window (relative mode and other code can take it). */
static int yscr__rm_still_ours(void) {
    RAWINPUTDEVICE list[16];
    UINT n = 16, i, got;
    if (!yscr__rm.hwnd) return 0;
    got = yscr__win.raw_registered(list, &n, sizeof list[0]);
    if (got == (UINT)-1) return 1;   /* more than 16 registrations: do not guess */
    for (i = 0; i < got; i++)
        if (list[i].usUsagePage == 1 && list[i].usUsage == 2) return list[i].hwndTarget == yscr__rm.hwnd;
    return 0;
}

static const char* yscr__rm_start(void) {
    BOOL wow = FALSE;
    if (yscr__a_inc(&yscr__rm.users) > 1) return NULL;
    yscr__win_load();
    if (!yscr__win.raw_register || !yscr__win.raw_registered || !yscr__win.raw_buffer ||
        !yscr__win.register_class || !yscr__win.create_window || !yscr__win.destroy_window ||
        !yscr__win.def_proc || !yscr__win.msg_wait || !yscr__win.peek_message || !yscr__win.post_message) {
        yscr__a_store(&yscr__rm.users, 0);
        return "desc.raw_mice: user32.dll lacks the Raw Input calls";
    }
    /* a 32-bit process on 64-bit Windows gets 64-bit headers (SDL does the same) */
    yscr__rm.offset = (UINT)sizeof(RAWINPUTHEADER);
    if (IsWow64Process(GetCurrentProcess(), &wow) && wow) yscr__rm.offset += 8;
    yscr__a_store(&yscr__rm.registered, 0);
    yscr__rm.quit = CreateEventW(NULL, TRUE, FALSE, NULL);
    yscr__rm.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    yscr__rm.thread = (yscr__rm.quit && yscr__rm.ready) ? CreateThread(NULL, 0, yscr__rm_main, NULL, 0, NULL) : NULL;
    if (!yscr__rm.thread || WaitForSingleObject(yscr__rm.ready, 2000) != WAIT_OBJECT_0 ||
        !yscr__a_load(&yscr__rm.registered)) {
        if (yscr__rm.thread) {
            SetEvent(yscr__rm.quit);
            WaitForSingleObject(yscr__rm.thread, 2000);
            CloseHandle(yscr__rm.thread);
        }
        if (yscr__rm.quit) CloseHandle(yscr__rm.quit);
        if (yscr__rm.ready) CloseHandle(yscr__rm.ready);
        yscr__rm.thread = yscr__rm.quit = yscr__rm.ready = NULL;
        yscr__a_store(&yscr__rm.users, 0);
        return "desc.raw_mice: Raw Input registration for mice was refused";
    }
    return NULL;
}

static void yscr__rm_stop(void) {
    if (yscr__a_load(&yscr__rm.users) <= 0) return;
    if (InterlockedDecrement((volatile LONG*)&yscr__rm.users) > 0) return;
    SetEvent(yscr__rm.quit);
    WaitForSingleObject(yscr__rm.thread, 2000);
    CloseHandle(yscr__rm.thread);
    CloseHandle(yscr__rm.quit);
    CloseHandle(yscr__rm.ready);
    yscr__rm.thread = yscr__rm.quit = yscr__rm.ready = NULL;
}

/* The guard's Windows side: is the registration still the reader's, and
 * a request to register again. */
static int yscr__rm_check_registered(void) { return yscr__rm_still_ours(); }
static void yscr__rm_request(void) {
    if (yscr__rm.hwnd) yscr__win.post_message(yscr__rm.hwnd, YSCR__RM_REGISTER, 0, 0);
}

/* The first armed screen starts the watchdog, with its desc's numbers. */
static const char* yscr__wd_arm(yscr_screen* s, const yscr_desc* d) {
    if (yscr__wd.armed == 0) {
        yscr__win_load();
        if (!yscr__win.set_hook || !yscr__win.unhook || !yscr__win.next_hook || !yscr__win.get_message ||
            !yscr__win.peek_message || !yscr__win.post_thread || !yscr__win.key_state ||
            !yscr__win.foreground || !yscr__win.window_pid)
            return "desc.panic: user32.dll lacks the keyboard hook calls";
        yscr__wd.vk = yscr__vk_of(yscr__ab.key);
        yscr__wd.key_down = 0;
        yscr__wd.fn = d->panic_fn;
        yscr__wd.ctx = d->panic_ctx;
        yscr__wd.panicking = 0;
        yscr__wd.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
        yscr__wd.thread = yscr__wd.ready ? CreateThread(NULL, 0, yscr__wd_main, NULL, 0, &yscr__wd.tid) : NULL;
        if (!yscr__wd.thread || WaitForSingleObject(yscr__wd.ready, 2000) != WAIT_OBJECT_0 || !yscr__wd.hook) {
            if (yscr__wd.thread) { WaitForSingleObject(yscr__wd.thread, 2000); CloseHandle(yscr__wd.thread); }
            if (yscr__wd.ready) CloseHandle(yscr__wd.ready);
            yscr__wd.thread = yscr__wd.ready = NULL;
            return "desc.panic: the low-level keyboard hook was refused";
        }
        CloseHandle(yscr__wd.ready);
        yscr__wd.ready = NULL;
        /* it sleeps in GetMessage; raised so a busy frame thread cannot hold
         * up the session's keys behind it */
        SetThreadPriority(yscr__wd.thread, THREAD_PRIORITY_HIGHEST);
        yscr__panic_config(d->panic_presses ? d->panic_presses : 3, d->panic_window_ns ? d->panic_window_ns : YRT_S(2),
                             d->panic_grace_ns ? d->panic_grace_ns : YRT_S(10));
    }
    yscr__wd.armed++;
    s->panic_armed = 1;
    return NULL;
}

static void yscr__wd_disarm(yscr_screen* s) {
    if (s->panic_armed != 1) return;
    s->panic_armed = 0;
    if (--yscr__wd.armed > 0) return;
    yscr__panic_config(0, 0, 0);
    yscr__win.post_thread(yscr__wd.tid, WM_QUIT, 0, 0);
    WaitForSingleObject(yscr__wd.thread, 2000);
    CloseHandle(yscr__wd.thread);
    yscr__wd.thread = NULL;
}

#endif /* YSCR__DXGI */

/* --- the core -------------------------------------------------------------- */

static int64_t yscr__time_of(const yscr_screen* s, int64_t c) {
    return s->vb_t + (int64_t)llround((double)(c - s->vb_count) * s->period_f);
}

/* The count of the last vblank at or before t. */
static int64_t yscr__count_at(const yscr_screen* s, int64_t t) {
    int64_t c = s->vb_count + (int64_t)floor((double)(t - s->vb_t) / s->period_f);
    while (yscr__time_of(s, c + 1) <= t) c++;
    while (yscr__time_of(s, c) > t) c--;
    return c;
}

/* The vblank t lands on: the first one at or after t - lead x period, the
 * timeline's rule, so a flip and an event at one time land together. */
static int64_t yscr__snap(const yscr_screen* s, int64_t t) {
    int64_t edge = t - s->lead_ns;
    int64_t c = yscr__count_at(s, edge);
    if (yscr__time_of(s, c) < edge) c++;
    return c;
}

static void yscr__update_lead(yscr_screen* s) {
    s->lead_ns = s->lead < 0 ? 0 : (int64_t)(s->lead * s->period_f);
    s->margin_ns = (int64_t)(s->period_f / 8);
    if (s->margin_ns > 1000000) s->margin_ns = 1000000;
}

static void yscr__push(yscr_screen* s, const yrt_event* ev) {
    if (s->ring) yrt_ring_push(s->ring, ev);
}

static void yscr__push_flip(yscr_screen* s, const yscr_record* r) {
    yrt_event ev;
    int i;
    if (!s->ring) return;
    memset(&ev, 0, sizeof ev);
    /* A flip never shown has no onset; the ring takes 0 for "now", so the
     * record carries the planned vblank and its SKIPPED or CANCELED flag. */
    ev.t_ns = (uint64_t)(r->onset ? r->onset : r->planned);
    ev.source = (uint16_t)YRT_SRC_SCREEN;
    ev.kind = (uint16_t)YSCR_EV_FLIP;
    ev.aux = s->display_index;
    ev.u.i64[0] = r->target;
    ev.u.u16[4] = (uint16_t)(r->dropped > 65535u ? 65535u : r->dropped);
    ev.u.u16[5] = (uint16_t)((r->path & 0x7u) | ((uint32_t)(r->flags & 0x3FFu) << 3) |
                             ((uint32_t)(r->tier & 0x7u) << 13));
    for (i = 0; i < YSCR_N_PHASES; i++) ev.u.u32[3 + i] = r->phase_ns[i];
    ev.u.u32[9] = (uint32_t)(r->index & 0xFFFFFFFF);
    yscr__push(s, &ev);
}

/* --- the foreground (FOREGROUND) and the vblank waiter (IDLE WAKE) ------- */

#if defined(YSCR__DXGI)
typedef struct { WCHAR DeviceName[32]; UINT hAdapter; LUID AdapterLuid; UINT VidPnSourceId; } yscr__kmt_open;
typedef struct { UINT hAdapter; UINT hDevice; UINT VidPnSourceId; } yscr__kmt_wait;
typedef struct { UINT hAdapter; } yscr__kmt_close;
typedef LONG (APIENTRY *yscr__kmt_open_fn)(yscr__kmt_open*);
typedef LONG (APIENTRY *yscr__kmt_wait_fn)(const yscr__kmt_wait*);
typedef LONG (APIENTRY *yscr__kmt_close_fn)(const yscr__kmt_close*);
typedef struct yscr__wake_t {
    volatile int32_t   stop;
    yscr__kmt_wait     w;
    yscr__kmt_wait_fn  wait;
    yscr__kmt_close_fn close;
    HANDLE             thread;
} yscr__wake_t;

static DWORD WINAPI yscr__wake_main(LPVOID p) {
    yscr__wake_t* k = (yscr__wake_t*)p;
    /* A failed wait (the display off) would spin: back off instead. */
    while (!yscr__a_load(&k->stop)) if (k->wait(&k->w) != 0) Sleep(4);
    return 0;
}

/* COMPOSITION showed the first present after 4 idle vblanks one vblank
 * late; a waiter on the vblank event keeps that from happening (IDLE
 * WAKE). Without the entry points or the adapter the screen runs as
 * before, and the describe line says idle_wake=off. */
static void yscr__wake_start(yscr_screen* s) {
    HMODULE g = GetModuleHandleW(L"gdi32.dll");
    yscr__kmt_open_fn op = g ? (yscr__kmt_open_fn)(yscr_proc)GetProcAddress(g, "D3DKMTOpenAdapterFromGdiDisplayName") : NULL;
    yscr__kmt_wait_fn wt = g ? (yscr__kmt_wait_fn)(yscr_proc)GetProcAddress(g, "D3DKMTWaitForVerticalBlankEvent") : NULL;
    yscr__kmt_close_fn cl = g ? (yscr__kmt_close_fn)(yscr_proc)GetProcAddress(g, "D3DKMTCloseAdapter") : NULL;
    HMONITOR mon;
    MONITORINFOEXW mi;
    yscr__kmt_open o;
    yscr__wake_t* k;
    yscr__win_load();
    if (!op || !wt || !cl || !s->hwnd || !yscr__win.monitor_from_window || !yscr__win.monitor_info) return;
    mon = yscr__win.monitor_from_window((HWND)s->hwnd, MONITOR_DEFAULTTONEAREST);
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!mon || !yscr__win.monitor_info(mon, (LPMONITORINFO)&mi)) return;
    memset(&o, 0, sizeof o);
    memcpy(o.DeviceName, mi.szDevice, sizeof o.DeviceName);
    if (op(&o) != 0) return;
    k = (yscr__wake_t*)calloc(1, sizeof *k);
    if (k) {
        k->w.hAdapter = o.hAdapter;
        k->w.VidPnSourceId = o.VidPnSourceId;
        k->wait = wt;
        k->close = cl;
        k->thread = CreateThread(NULL, 0, yscr__wake_main, k, 0, NULL);
    }
    if (!k || !k->thread) {
        yscr__kmt_close c;
        c.hAdapter = o.hAdapter;
        cl(&c);
        free(k);
        return;
    }
    s->wake = k;
}

static void yscr__wake_stop(yscr_screen* s) {
    yscr__wake_t* k = (yscr__wake_t*)s->wake;
    if (!k) return;
    s->wake = NULL;
    yscr__a_store(&k->stop, 1);
    /* The wait returns at the next vblank. With the display off it may
     * not return: then the thread keeps its state and the adapter. */
    if (WaitForSingleObject(k->thread, 250) == WAIT_OBJECT_0) {
        yscr__kmt_close c;
        c.hAdapter = k->w.hAdapter;
        k->close(&c);
        CloseHandle(k->thread);
        free(k);
    } else {
        CloseHandle(k->thread);
    }
}
#endif

/* How the window got the foreground (FOREGROUND); raises it again with
 * SDL's force hint when the mode is FORCE and the OS refused. */
static uint32_t yscr__foreground(yscr_screen* s) {
#if defined(YSCR__DXGI)
    HWND h = (HWND)s->hwnd;
    if (!s->window || !h || !yscr__win.foreground) return YSCR_FG_NA;
    if (yscr__win.foreground() == h) return YSCR_FG_GRANTED;
    if (s->fg_mode != YSCR_FOREGROUND_FORCE) return YSCR_FG_REFUSED;
    {   /* SDL reads the hint at the raise; the caller's value goes back */
        const char* v = SDL_GetHint(SDL_HINT_FORCE_RAISEWINDOW);
        char keep[16];
        int had = v != NULL;
        if (had) yscr__copy(keep, sizeof keep, v);
        SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");
        SDL_RaiseWindow(s->window);
        if (had) SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, keep);
        else SDL_ResetHint(SDL_HINT_FORCE_RAISEWINDOW);
    }
    return yscr__win.foreground() == h ? YSCR_FG_FORCED : YSCR_FG_REFUSED;
#else
    (void)s;
    return YSCR_FG_NA;
#endif
}
/* The core test replaces it. */
#ifndef YSCR__FOREGROUND
#define YSCR__FOREGROUND(s) yscr__foreground(s)
#endif

/* --- settling at open (SETTLE) -------------------------------------------- */

/* The spread rule's numbers (docs/screen.md, "Settling at open"): over the
 * first 20 intervals from open's first OS time the SD was at most 13.5 us
 * and the mean at most 6.7 us off the period in 120 opens with the window
 * in front; once settled the SD is about 1 us. The drift after a covered
 * COMPOSITION open gave 170 to 1800 us. */
#define YSCR__SETTLE_N       20
#define YSCR__SETTLE_SD_NS   50000.0
#define YSCR__SETTLE_MEAN_NS 10000.0

static const char* const yscr__settle_cname[11] = {
    "none", "anchor", "statistics", "early", "late", "grid", "tier", "path", "depth", "spread", "unsynced"
};

/* An OS vblank time from open's black frames, for the spread. */
static void yscr__settle_time(yscr_screen* s, int64_t t) {
    if (s->st_nts == 21) {
        memmove(s->st_ts, s->st_ts + 1, sizeof(int64_t) * 20);
        s->st_nts = 20;
    }
    s->st_ts[s->st_nts++] = t;
}

/* The last 20 intervals between OS times, each divided by the vblanks it
 * spans: 1 when their SD and the mean's distance from the mode's period
 * are within the rule. Sets st_sd (-1: too few) and st_mean. */
static int yscr__settle_spread(yscr_screen* s) {
    double iv[YSCR__SETTLE_N], m = 0, v = 0;
    int i, n = 0;
    s->st_sd = -1;
    s->st_mean = 0;
    for (i = 1; i < s->st_nts && n < YSCR__SETTLE_N; i++) {
        int64_t d = s->st_ts[i] - s->st_ts[i - 1];
        int64_t k = (int64_t)llround((double)d / s->nominal_f);
        if (k >= 1) iv[n++] = (double)d / (double)k;
    }
    if (n < YSCR__SETTLE_N) return 0;
    for (i = 0; i < n; i++) m += iv[i];
    m /= n;
    for (i = 0; i < n; i++) v += (iv[i] - m) * (iv[i] - m);
    v = sqrt(v / n);
    s->st_sd = (int64_t)llround(v);
    s->st_mean = (int64_t)llround(m - s->nominal_f);
    return v <= YSCR__SETTLE_SD_NS && fabs(m - s->nominal_f) <= YSCR__SETTLE_MEAN_NS;
}

/* One settle frame's record: clean, or the condition it breaks. A run of
 * st_need clean flips on one path and one depth is half the rule; the
 * spread and the minimum time are the rest (yscr__settle). */
static void yscr__settle_flip(yscr_screen* s, const yscr__pend* p, const yscr_record* r) {
    uint32_t c = YSCR_SETTLE_C_NONE;
    uint16_t f = r->flags;
    if (s->sync_off || (f & YSCR_FLIP_UNSYNCED)) c = YSCR_SETTLE_C_UNSYNCED;
    else if (f & (YSCR_FLIP_ESTIMATED | YSCR_FLIP_OCCLUDED | YSCR_FLIP_SKIPPED | YSCR_FLIP_CANCELED)) c = YSCR_SETTLE_C_STATS;
    else if (s->anchor_guessed) c = YSCR_SETTLE_C_ANCHOR;
    else if (f & YSCR_FLIP_EARLY) c = YSCR_SETTLE_C_EARLY;
    else if (r->dropped || (f & YSCR_FLIP_LATE_TARGET)) c = YSCR_SETTLE_C_LATE;
    else if (f & YSCR_FLIP_GRID_UNSTABLE) c = YSCR_SETTLE_C_GRID;
    else if (r->tier < YSCR_TIER_1 || r->tier > s->st_tier) c = YSCR_SETTLE_C_TIER;
    else if (s->st_have && r->path != s->st_path) c = YSCR_SETTLE_C_PATH;
    else if (s->st_have && s->depth != s->st_depth) c = YSCR_SETTLE_C_DEPTH;
    if (c != YSCR_SETTLE_C_STATS) {   /* a flip with no statistic has no path */
        s->st_path = r->path;
        s->st_depth = s->depth;
        s->st_have = 1;
    }
    if (c) {
        s->st_run = 0;
        s->st_cond = c;
        s->st_bad = (int32_t)p->id;
    } else {
        s->st_run++;
    }
}

/* The tier of a flip: the presenter's, else its path's, and never 1 for a
 * time that is an estimate or a plan (TIERS in the manual). */
static uint8_t yscr__tier(const yscr_screen* s, const yscr_record* r) {
    int t = r->tier;
    if (s->backend == YSCR_BACKEND_SIM) return YSCR_TIER_SIM;
    /* no flip in a browser is observed: the frame callback's time is not
     * the scanout's, and the compositor's delay after it is unknown */
    if (s->backend == YSCR_BACKEND_WEB) return YSCR_TIER_WEB;
    if (!t) {
        switch (r->path) {
        case YSCR_PATH_OVERLAY:
        case YSCR_PATH_INDEPENDENT: t = s->caps.hw_onset ? YSCR_TIER_1 : YSCR_TIER_3; break;
        case YSCR_PATH_COMPOSED:    t = s->caps.hw_onset ? YSCR_TIER_2 : YSCR_TIER_3; break;
        case YSCR_PATH_SIMULATED:   t = YSCR_TIER_SIM; break;
        default:                      t = YSCR_TIER_3; break;
        }
    }
    if ((r->flags & (YSCR_FLIP_ESTIMATED | YSCR_FLIP_ONSET_PLANNED)) && t < YSCR_TIER_3) t = YSCR_TIER_3;
    /* not synced to the vblank: no onset is a measurement (SYNC GUARD) */
    if (s->sync_off && t < YSCR_TIER_3) t = YSCR_TIER_3;
    /* the OS's time was off the vblank grid (measured on this laptop's
     * held frames, while the scanout's vblanks stayed on it): the onset
     * is the grid's vblank, not an OS observation of it */
    if ((r->flags & YSCR_FLIP_GRID_UNSTABLE) && t < YSCR_TIER_2) t = YSCR_TIER_2;
    return (uint8_t)t;
}

static void yscr__finish(yscr_screen* s, yscr__pend* p, int64_t shown) {
    yscr_record* r = &p->rec;
    r->flags = (uint16_t)(r->flags & ~YSCR_FLIP_PENDING);
    r->residual = (r->flags & (YSCR_FLIP_SKIPPED | YSCR_FLIP_CANCELED)) ? 0 : r->onset - r->target;
    if (shown > s->prev_shown) s->prev_shown = shown;
    if (p->id > s->done_id) s->done_id = p->id;
    p->used = 0;
    if (s->warming) return;
    /* the rate block counts every present that completed, shown or not,
     * from its first flip with a time on; a hidden window's presents may
     * not be paced, so it starts again */
    if (r->flags & YSCR_FLIP_OCCLUDED) {
        s->sync_p = s->sync_r = 0;
        s->sync_have_prev = 0;
    } else if (s->sync_have_prev) {
        s->sync_p++;
    }
    if (s->sync_off) r->flags |= YSCR_FLIP_UNSYNCED;
    if (r->index < 0) {   /* open's settle frames: judged, never reported */
        if (!(r->flags & (YSCR_FLIP_SKIPPED | YSCR_FLIP_CANCELED))) r->tier = yscr__tier(s, r);
        if (s->settling) yscr__settle_flip(s, p, r);
        return;
    }
    if (!(r->flags & (YSCR_FLIP_SKIPPED | YSCR_FLIP_CANCELED))) {
        r->tier = yscr__tier(s, r);
        if (r->tier > s->worst_tier) s->worst_tier = r->tier;
        if (s->min_tier && r->tier > s->min_tier) r->flags |= YSCR_FLIP_BELOW_TIER;
    }
    if (s->n_codes > 0) {
        r->code_risk = yscr_code_risk(s);
        if (r->path == YSCR_PATH_COMPOSED || r->path == YSCR_PATH_BROWSER) r->code_risk |= YSCR_CODE_RISK_COMPOSED;
        if (r->code_risk) r->flags |= YSCR_FLIP_CODE_AT_RISK;
    }
    yscr__push_flip(s, r);
    yscr__push_code(s, p);
    if (s->trig_on || s->on_flip) yscr__trig_flip_done(s, p, shown);
    YRT_FRAME_MARK();
    YRT_PLOT("yscr residual us", (double)r->residual / 1000.0);
    s->last = *r;
    s->have_last = 1;
    if (s->n_fin < YSCR_MAX_DONE) s->fin[s->n_fin++] = *r;
    else {   /* keep the newest */
        memmove(s->fin, s->fin + 1, sizeof(yscr_record) * (YSCR_MAX_DONE - 1));
        s->fin[YSCR_MAX_DONE - 1] = *r;
        s->fin_lost++;
    }
}

static void yscr__estimate(yscr_screen* s, yscr__pend* p) {
    p->rec.onset = yscr__time_of(s, p->planned_count) + s->offset;
    p->rec.flags |= YSCR_FLIP_ESTIMATED;
    p->rec.dropped = 0;
    yscr__finish(s, p, p->planned_count);
}

/* How far d is from a whole number of periods, in ns. */
static double yscr__phase_off(const yscr_screen* s, int64_t d) {
    double r = (double)d - (double)llround((double)d / s->period_f) * s->period_f;
    return r < 0 ? -r : r;
}

/* The grid's vblank nearest t, and whether t is within 1% of a period of
 * it: an OS time on the grid. */
static int yscr__on_grid(const yscr_screen* s, int64_t t, int64_t* nc) {
    *nc = s->vb_count + (int64_t)llround((double)(t - s->vb_t) / s->period_f);
    return yscr__phase_off(s, t - yscr__time_of(s, *nc)) <= s->period_f / 100;
}

/* restart: the display's timing changed, so the fit starts again. */
static void yscr__anchor(yscr_screen* s, int64_t t, int64_t count, int restart) {
    if (!s->have_anchor || restart) {
        s->vb_t = t; s->vb_count = count;
        s->ref_t = t; s->ref_count = count;
        if (restart) {
            s->period_f = s->nominal_f;
            yscr__update_lead(s);
        }
        s->have_anchor = 1;
        return;
    }
    if (count < s->vb_count) return;
    s->vb_t = t; s->vb_count = count;
    if (count - s->ref_count >= 64) {
        s->period_f = (double)(t - s->ref_t) / (double)(count - s->ref_count);
        yscr__update_lead(s);
    }
}

/* --- the grid after a gap (THE GRID, v0.10.1) ---------------------------------
 * With no OS time for a while (the host waited between trials) the grid
 * extrapolates its period across the gap. Measured on this laptop's panel
 * (docs/screen.md, "The grid after a gap"): the period fitted over the
 * first 2 s of a run was 1 to 41 ppm off the long-run rate, because the
 * OS's vblank times wander by tens of us over seconds, so after a gap of
 * 10 to 60 s the grid was 0.2 to 2 ms off the vblanks and the next 17
 * records had GRID_UNSTABLE. DWM's vblank time (DwmGetCompositionTimingInfo)
 * sat within 4 us of the flip statistics' times, before, during and after
 * a gap, and is never older than a period. So the first begin() after a
 * gap reads it and anchors the grid there, as an OS time: the phase
 * follows the vblanks, and the period gains the gap as baseline. */
#define YSCR__GAP_NS 500000000   /* no OS time for 0.5 s: a gap */

/* DWM's last vblank, for the screen's display: 1 and *t, else 0. DWM's
 * timing is the primary display's, so a screen elsewhere gets none, nor
 * one whose display refreshes at another rate. */
#if defined(YSCR__DXGI)
typedef HRESULT (WINAPI *yscr__DwmGetCompositionTimingInfo_fn)(HWND, DWM_TIMING_INFO*);
static int yscr__dwm_vblank(const yscr_screen* s, int64_t* t) {
    static yscr__DwmGetCompositionTimingInfo_fn get;
    static int tried;
    DWM_TIMING_INFO ti;
    MONITORINFO mi;
    HMONITOR mon;
    double p;
    if (!tried) {
        HMODULE m = LoadLibraryW(L"dwmapi.dll");
        tried = 1;
        get = m ? (yscr__DwmGetCompositionTimingInfo_fn)(yscr_proc)GetProcAddress(m, "DwmGetCompositionTimingInfo") : NULL;
    }
    yscr__win_load();
    if (!get || !s->hwnd || !yscr__win.monitor_from_window || !yscr__win.monitor_info) return 0;
    mon = yscr__win.monitor_from_window((HWND)s->hwnd, MONITOR_DEFAULTTONEAREST);
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!mon || !yscr__win.monitor_info(mon, &mi) || !(mi.dwFlags & MONITORINFOF_PRIMARY)) return 0;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = sizeof ti;
    if (get(NULL, &ti) != S_OK || !ti.qpcVBlank || !ti.qpcRefreshPeriod) return 0;
    p = (double)yrt_ticks_to_ns((int64_t)ti.qpcRefreshPeriod);
    if (p < s->period_f * 0.995 || p > s->period_f * 1.005) return 0;
    /* qpcVBlank is the next vblank's time, up to a period ahead (measured):
     * the last one is a period before it */
    *t = yrt_ticks_to_ns((int64_t)ti.qpcVBlank);
    if (*t > yscr__now()) *t -= (int64_t)llround(p);
    return 1;
}
#endif
/* The system's last vblank time for the screen: 1 and *t, else 0. The core
 * test replaces it. */
#ifndef YSCR__SYS_VBLANK
#if defined(YSCR__DXGI)
#define YSCR__SYS_VBLANK(s, t) yscr__dwm_vblank((s), (t))
#else
#define YSCR__SYS_VBLANK(s, t) ((void)(s), (void)(t), 0)
#endif
#endif

/* After a gap, before the prediction, once per gap: the system's vblank,
 * when it is newer than the grid's, fresh (within 2 periods of now), and
 * within what the period's error and the times' wander can explain (1% of
 * a period, plus 100 ppm of the time since the grid's last OS time, at
 * most a quarter period), becomes the grid's newest OS time. */
static void yscr__gap_anchor(yscr_screen* s, int64_t now) {
    int64_t t = 0, nc, off, span;
    double bound;
    if (!s->have_anchor || s->anchor_guessed || s->grid_follow || now - s->vb_t < YSCR__GAP_NS) return;
    if (s->gap_asked == s->vb_t) return;   /* once per gap: a refusal stands */
    s->gap_asked = s->vb_t;
    if (!YSCR__SYS_VBLANK(s, &t) || t <= s->vb_t || t > now || now - t > (int64_t)(2 * s->period_f)) return;
    span = t - s->vb_t;
    nc = s->vb_count + (int64_t)llround((double)span / s->period_f);
    off = t - yscr__time_of(s, nc);
    bound = s->period_f / 100 + (double)span * 1e-4;
    if (bound > s->period_f / 4) bound = s->period_f / 4;
    if ((double)(off < 0 ? -off : off) > bound) { s->gap_refused++; return; }
    s->gap_anchors++;
    if ((off < 0 ? -off : off) > s->gap_shift_max) s->gap_shift_max = off < 0 ? -off : off;
    yscr__anchor(s, t, nc, 0);
}

static int yscr__slack_bin(const yscr_screen* s, int64_t slack) {
    double b = (double)slack * 32.0 / s->period_f;
    if (b < 0) return 0;
    if (b >= YSCR__SLACK_BINS - 1) return YSCR__SLACK_BINS - 1;
    return (int)b;
}

/* The depth a path shows at when open() did not measure it there: 1 where
 * the scanout reads the frame itself, 2 where the compositor copies it
 * (docs/screen.md, "Depth"). COMPOSITION's overlay frames come with the
 * compositor's statistics and showed on the vblank after next (2 of 2);
 * on a backend that holds frames, a depth too large costs latency only,
 * where one too small drops every frame. Unknown: no change. */
static int32_t yscr__path_table(const yscr_screen* s, int path) {
    switch (path) {
    case YSCR_PATH_COMPOSED:    return 2;
    case YSCR_PATH_OVERLAY:     return s->caps.native_target ? 2 : 1;
    case YSCR_PATH_INDEPENDENT:
    case YSCR_PATH_SIMULATED:   return 1;
    default:                    return 0;
    }
}

static int32_t yscr__path_depth(const yscr_screen* s, int path) {
    int32_t d;
    if (path < YSCR__SLACK_PATHS && s->depth_of[path]) return s->depth_of[path];
    d = yscr__path_table(s, path);
    return d ? d : s->show_depth;
}

/* A change of depth changes the time from a present to its flip, so the
 * ring gets one record per change and the describe line counts them: a
 * task can see where its latency moved. show is where a present shows on
 * the path; a pin holds the planned depth, and a DXGI hold still wakes at
 * show (FLIP AT A TIME). */
static void yscr__depth_set(yscr_screen* s, int32_t show, uint16_t why, int64_t t, int64_t frame) {
    int32_t old = s->depth, d;
    yrt_event ev;
    if (show < 1) show = 1;
    if (show > 8) show = 8;
    s->show_depth = show;
    d = s->depth_pin ? s->depth_pin : show;
    if (d == old) return;   /* open() sets old to 0, so its record always comes */
    s->depth = d;
    if (old) s->depth_changes++;
    YRT_PLOT("yscr depth", (double)d);
    memset(&ev, 0, sizeof ev);
    ev.t_ns = (uint64_t)t;
    ev.source = (uint16_t)YRT_SRC_SCREEN;
    ev.kind = (uint16_t)YSCR_EV_DEPTH;
    ev.aux = s->display_index;
    ev.u.u16[0] = (uint16_t)old;
    ev.u.u16[1] = (uint16_t)d;
    ev.u.u16[2] = why;
    ev.u.u16[3] = s->path;
    ev.u.i64[1] = frame;
    yscr__push(s, &ev);
}

/* A present with a target waits for it, so an on-time flip shows only that
 * the depth it was given was enough: at depth 2 every flip "measured" 2,
 * and COMPOSITION ran at half rate after one miss. A try at one less drops
 * a frame whenever it fails. So the depth is lowered only on evidence: the
 * slack a present at one less would have had (this flip's slack minus a
 * period) has been seen on time, on this path, at least 8 times as often
 * as a miss was seen with as much slack or more, on 4 flips in a row. A
 * miss soon after a lowering refutes that evidence: the counts of on-time
 * flips on that path are cleared, so the same evidence cannot drop a
 * second frame. Three misses in a row raise the depth by one. */
static void yscr__native_depth(yscr_screen* s, yscr__pend* p, int64_t count) {
    int path = p->rec.path < YSCR__SLACK_PATHS ? p->rec.path : 0;
    int64_t slack = yscr__time_of(s, p->planned_count) - p->t_ret;
    int bin = yscr__slack_bin(s, slack);
    int k;
    if (count < p->planned_count) return;   /* early: says nothing */
    if (++s->slack_n >= YSCR__SLACK_DECAY) {   /* old evidence fades */
        int q;
        s->slack_n = 0;
        for (q = 0; q < YSCR__SLACK_PATHS; q++)
            for (k = 0; k < YSCR__SLACK_BINS; k++) { s->slack_ok[q][k] >>= 1; s->slack_miss[q][k] >>= 1; }
    }
    if (count > p->planned_count) {
        if (s->slack_miss[path][bin] < 0xFFFF) s->slack_miss[path][bin]++;
        s->lower_streak = 0;
        if (!p->asap) return;   /* a held flip says nothing about the depth */
        if (s->fresh_lower > 0) {   /* the evidence was wrong: back at once */
            memset(s->slack_ok[path], 0, sizeof s->slack_ok[path]);
            s->fresh_lower = 0;
            s->depth_votes = 0;
            yscr__depth_set(s, s->depth + 1, YSCR_DEPTH_LEARNER, p->rec.onset, p->rec.index);
        } else if (++s->depth_votes >= 3) {
            s->depth_votes = 0;
            if (s->depth < 8) yscr__depth_set(s, s->depth + 1, YSCR_DEPTH_LEARNER, p->rec.onset, p->rec.index);
        } else {
            return;
        }
        s->depth_of[path] = s->depth;
        return;
    }
    if (s->slack_ok[path][bin] < 0xFFFF) s->slack_ok[path][bin]++;
    s->depth_votes = 0;
    if (s->fresh_lower > 0) s->fresh_lower--;
    if (!p->asap || s->depth <= 1 || p->planned_count - p->count_at_present < s->depth) return;
    {
        int lb = yscr__slack_bin(s, slack - (int64_t)s->period_f);
        uint32_t ok = 0, miss = 0;
        for (k = 0; k <= lb; k++) ok += s->slack_ok[path][k];
        for (k = lb; k < YSCR__SLACK_BINS; k++) miss += s->slack_miss[path][k];
        if (slack - (int64_t)s->period_f > 0 && ok >= 8u * (1u + miss)) s->lower_streak++;
        else s->lower_streak = 0;
    }
    if (s->lower_streak >= 4) {
        s->lower_streak = 0;
        yscr__depth_set(s, s->depth - 1, YSCR_DEPTH_LEARNER, p->rec.onset, p->rec.index);
        s->depth_of[path] = s->depth;
        s->fresh_lower = 4;
    }
}

static int yscr__popcount64(uint64_t x) {
    int n = 0;
    for (; x; x &= x - 1) n++;
    return n;
}

static void yscr__sync_fire(yscr_screen* s, uint32_t rule, const yscr__pend* p, int64_t t, int ev) {
    yrt_event e;
    int k;
    s->sync_off = 1;
    s->sync_rule = rule;
    /* a completion with no record left: the oldest record still to come
     * is the first one flagged */
    s->sync_index = s->index;
    if (p) s->sync_index = p->rec.index;
    else
        for (k = 0; k < YSCR__MAX_PEND; k++)
            if (s->pend[k].used && s->pend[k].rec.index < s->sync_index) s->sync_index = s->pend[k].rec.index;
    s->sync_fire[0] = (uint32_t)ev;
    s->sync_fire[1] = s->sync_p;
    s->sync_fire[2] = s->sync_r;
    memset(&e, 0, sizeof e);
    e.t_ns = (uint64_t)(t + s->offset);
    e.source = (uint16_t)YRT_SRC_SCREEN;
    e.kind = (uint16_t)YSCR_EV_UNSYNCED;
    e.aux = s->display_index;
    e.u.u32[0] = rule;
    e.u.u32[1] = (uint32_t)ev;
    e.u.u32[2] = (uint32_t)s->sync_win;
    if (rule == YSCR_SYNC_RULE_RATE) { e.u.u32[3] = s->sync_p; e.u.u32[4] = s->sync_r; }
    e.u.i64[3] = s->sync_index;
    yscr__push(s, &e);
    YSCR__ON_UNSYNCED(s);
}

/* The sync guard (SYNC GUARD), on a flip with an OS time. Under vsync a
 * vblank shows at most one new frame, and a frame presented in one
 * refresh flips at a vblank after it, so each piece of evidence below is
 * impossible there; one can still come from a stale count or a jittered
 * time, so the guard wants half of 64 flips, or a rate over 64 refreshes.
 * DXGI's held frames on this laptop had times off the grid (docs/screen.md,
 * "Held frames a vblank early"), a quarter period early on a few flips,
 * never on many in a row. */
static void yscr__sync_flip(yscr_screen* s, const yscr__pend* p, int64_t count, int64_t t, uint16_t vflags) {
    int ev = 0, n;
    if (s->warming) return;
    if (vflags & YSCR_FLIP_OCCLUDED) {   /* a hidden window may not be paced */
        s->sync_p = s->sync_r = 0;
        s->sync_have_prev = 0;
        return;
    }
    s->sync_obs++;
    if (s->sync_have_prev) {
        if (count <= s->sync_prev_count || (double)(t - s->sync_prev_t) < s->period_f / 2) {
            ev = 1;
            s->sync_same++;
        }
        if (count > s->sync_prev_count) s->sync_r += (uint32_t)(count - s->sync_prev_count);
    }
    /* a completion whose present was estimated already has no plan. Both
     * the count and the time: DXGI's count can be a vblank behind the
     * grid's, and the time alone cannot tell a vblank during the call from
     * one before it */
    if (p && count <= p->count_at_present && t < p->t_call) {
        ev = 1;
        s->sync_nowait++;
    }
    /* torn: before the vblank its count names, on the grid it was planned
     * with (a grid that follows the flips, as when most times are off
     * it, moves with a steady tear) */
    if (p && (double)(p->rec.planned - s->offset - t) + (double)(count - p->planned_count) * s->period_f > s->period_f / 4) {
        ev = 1;
        s->sync_torn++;
    }
    if (!s->sync_have_prev || count > s->sync_prev_count) s->sync_prev_count = count;
    s->sync_prev_t = t;
    s->sync_have_prev = 1;
    s->sync_hist = (s->sync_hist << 1) | (uint64_t)ev;
    if (s->sync_win < 64) s->sync_win++;
    n = yscr__popcount64(s->sync_hist);
    if (n > s->sync_peak) s->sync_peak = n;
    if (!s->sync_off && n >= 32) yscr__sync_fire(s, YSCR_SYNC_RULE_FLIPS, p, t, n);
    if (s->sync_r >= 64) {
        /* presents that completed between the block's flips, against the
         * refreshes: queued and estimated presents shift a few across the
         * block's edges, at most the frames in flight (8) */
        if (!s->sync_off && s->sync_p > s->sync_r + YSCR__MAX_PEND) yscr__sync_fire(s, YSCR_SYNC_RULE_RATE, p, t, n);
        s->sync_p = s->sync_r = 0;
    }
}

static void yscr__complete(yscr_screen* s, const yscr_vblank* v) {
    int i;
    yscr__pend* p = NULL;
    int64_t count = v->count, t = v->t_ns, onset;
    bool unstable = false, guessed = false, stale;
    for (i = 0; i < YSCR__MAX_PEND; i++) {
        yscr__pend* q = &s->pend[i];
        if (!q->used) continue;
        if (q->id == v->present_id) p = q;
    }
    /* Older presents the statistics skipped: estimate them, oldest first. */
    for (;;) {
        yscr__pend* oldest = NULL;
        for (i = 0; i < YSCR__MAX_PEND; i++) {
            yscr__pend* q = &s->pend[i];
            if (q->used && v->present_id && q->id < v->present_id && (!oldest || q->id < oldest->id)) oldest = q;
        }
        if (!oldest) break;
        yscr__estimate(s, oldest);
    }
    /* A statistic for a present that already completed is stale: in a
     * window, COMPOSITION reported each independent flip a second time,
     * as an overlay frame planned for the next vblank, and the path and
     * the depth flapped 1, 2, 1 on 6 frames of every open (9 to 13 depth
     * changes; docs/screen.md, "Settling at open"). It moves no path. */
    stale = !p && v->present_id && v->present_id <= s->done_id;
    if (t && !stale && (s->warming || s->settling)) yscr__settle_time(s, t);
    if (v->path != s->path && !stale) {
        yrt_event ev;
        memset(&ev, 0, sizeof ev);
        ev.t_ns = (uint64_t)t;
        ev.source = (uint16_t)YRT_SRC_SCREEN;
        ev.kind = (uint16_t)YSCR_EV_PATH;
        ev.aux = s->display_index;
        ev.u.u16[0] = s->path;
        ev.u.u16[1] = v->path;
        yscr__push(s, &ev);
        s->path = v->path;
        if (!s->warming && !s->depth_learn) {
            /* the path sets the depth, and only the path: a miss never
             * does (DEPTH) */
            yscr__depth_set(s, yscr__path_depth(s, v->path), YSCR_DEPTH_PATH, t ? t : yscr__now(), p ? p->rec.index : -1);
        } else if (s->caps.native_target) {
            /* the depth this path had last time; a path seen first keeps
             * the current one until misses show otherwise */
            if (v->path < YSCR__SLACK_PATHS && s->depth_of[v->path])
                yscr__depth_set(s, s->depth_of[v->path], YSCR_DEPTH_PATH, t ? t : yscr__now(), p ? p->rec.index : -1);
            s->depth_votes = 0;
            s->lower_streak = 0;
            s->fresh_lower = 0;
        } else {
            s->depth_need = 1;   /* a new path may have a new depth: adopt it */
            s->adopt_left = 3;
            memset(s->warm_seen, 0, sizeof s->warm_seen);
        }
    }
    if (v->flags & (YSCR_FLIP_SKIPPED | YSCR_FLIP_CANCELED)) {
        /* never shown: no onset, no anchor, no drop */
        if (!p) return;
        p->rec.onset = 0;
        p->rec.path = v->path;
        p->rec.flags |= (uint16_t)(v->flags & (YSCR_FLIP_SKIPPED | YSCR_FLIP_CANCELED));
        p->rec.dropped = 0;
        yscr__finish(s, p, s->prev_shown);
        return;
    }
    if (t == 0) {   /* no time for this flip */
        if (!p) return;
        p->rec.flags |= (uint16_t)(v->flags & YSCR_FLIP_OCCLUDED);
        p->rec.path = v->path;
        yscr__estimate(s, p);
        return;
    }
    /* The vblank is the grid's nearest to the OS's time, not the OS's
     * count: on this laptop's DXGI_FLIP, while frames were held, the
     * refresh count lost about 1 vblank in 25 and 15% of the times were
     * up to half a period off the vblanks, which D3DKMTGetScanLine showed
     * steady on the grid. Each frame still flipped on the vblank nearest
     * its time. A grid moved to such a time woke the next hold early, and
     * that frame did show a vblank early (docs/screen.md, "Held frames a
     * vblank early"). So only times on the grid move it, or 16 in a row
     * that agree on a new phase, as a changed display timing gives. When
     * 48 of the last 64 times are off the grid, the vblanks themselves
     * move (or the grid is wrong): the grid then follows each time and
     * takes the OS's count, as before v0.4.3, until 16 of 64 or fewer
     * are off it. DXGI's held frames put at most 34 of 64 off it. */
    if (s->anchor_guessed) {
        /* open() got no OS time, so the grid is a guess: a window covered
         * at open, or COMPOSITION's first statistics late (seen for about
         * 0.7 s after a session unlock), made every record tier 3 and fired
         * the sync guard. The first OS time replaces the guess, numbered
         * as its nearest guessed vblank, so the flips planned on the guess
         * keep their numbers. */
        int64_t nc = s->vb_count + (int64_t)llround((double)(t - s->vb_t) / s->period_f);
        s->anchor_guessed = 0;
        guessed = true;   /* its plan was made on the guess: no evidence */
        s->caps.hw_onset = s->hw_onset_pr;
        s->count_bias = nc - v->count;
        s->cand_n = 0;
        s->grid_hist = 0;
        s->grid_n = 0;
        s->grid_follow = 0;
        yscr__anchor(s, t, nc, 1);
    }
    onset = t;
    if (!s->have_anchor) {
        yscr__anchor(s, t, count, 0);
    } else {
        int64_t nc;
        int on = yscr__on_grid(s, t, &nc), n_off;
        s->grid_hist = (s->grid_hist << 1) | (uint64_t)!on;
        if (s->grid_n < 64) s->grid_n++;
        n_off = yscr__popcount64(s->grid_hist);
        if (!s->grid_follow && s->grid_n >= 64 && n_off >= 48) s->grid_follow = 1;
        else if (s->grid_follow && n_off <= 16) s->grid_follow = 0;
        if (!on) {
            unstable = true;
            s->unstable++;
        }
        if (s->grid_follow) {
            int64_t d = t - yscr__time_of(s, v->count + s->count_bias);
            count = v->count + s->count_bias;
            s->cand_n = 0;
            yscr__anchor(s, t, count, (double)(d < 0 ? -d : d) > s->period_f / 4);   /* a jump: fit again */
        } else if (on) {
            s->count_bias = nc - v->count;
            s->cand_n = 0;
            count = nc;
            yscr__anchor(s, t, count, 0);
        } else {
            if (s->cand_n > 0 && yscr__phase_off(s, t - s->cand_t) <= s->period_f / 100) s->cand_n++;
            else { s->cand_t = t; s->cand_n = 1; }
            count = nc;
            if (s->cand_n > YSCR__REGRID_N) {
                s->count_bias = nc - v->count;
                s->cand_n = 0;
                yscr__anchor(s, t, count, 1);
            } else {
                onset = yscr__time_of(s, nc);
            }
        }
    }
    /* the guard reads the OS's own time, and its count in the grid's numbers */
    if (!guessed) yscr__sync_flip(s, p, v->count + s->count_bias, t, v->flags);
    if (!p) return;
    p->rec.onset = onset + s->offset;
    p->rec.path = v->path;
    p->rec.tier = v->tier;
    p->rec.flags |= (uint16_t)(v->flags & YSCR_FLIP_ONSET_PLANNED);
    if (unstable) p->rec.flags |= YSCR_FLIP_GRID_UNSTABLE;
    if (v->flags & YSCR_FLIP_OCCLUDED) p->rec.flags |= YSCR_FLIP_OCCLUDED;
    if (count < p->planned_count) {
        p->rec.flags |= YSCR_FLIP_EARLY;
        p->rec.dropped = 0;
    } else {
        p->rec.dropped = (uint32_t)(count - p->planned_count);
    }
    /* DXGI shows a present at the first vblank it can, so a flip that was
     * not held and showed early, at a time on the grid, proves the depth
     * too high: the depth falls to what it showed. The video worker's
     * windowed runs: the path moved from composed to overlay 5 flips
     * before DXGI reported it, and open() learned 3 on the composed path
     * in 3 of 9 runs (108 to 116 early flips). Only a lower depth comes
     * from it, so no latency is added. The path keeps at least its table
     * depth: the report may lag the flip that showed early. */
    if (!s->caps.native_target && !s->warming && !s->depth_learn && count < p->planned_count && !unstable &&
        !p->held) {
        int32_t obs = (int32_t)(count - p->count_at_present);
        if (obs >= 1 && obs < s->show_depth) {
            int path = s->path < YSCR__SLACK_PATHS ? s->path : 0;
            int32_t keep = yscr__path_table(s, path);
            if (keep < obs) keep = obs;
            yscr__depth_set(s, obs, YSCR_DEPTH_EARLY, onset, p->rec.index);
            if (s->depth_of[path] > keep) s->depth_of[path] = keep;
        }
    }
    /* Depth: vblanks from the present call to the flip. */
    if (s->caps.native_target) {
        int32_t obs = (int32_t)(count - p->count_at_present);
        if (obs == s->last_obs) s->obs_streak++;
        else { s->last_obs = obs; s->obs_streak = 1; }
        if (!s->warming && s->depth_learn) yscr__native_depth(s, p, count);
    } else if (p->asap && (s->warming || s->depth_learn)) {
        /* open() measures the depth here; after it only the learner does */
        int32_t obs = (int32_t)(count - p->count_at_present);
        if (obs == s->last_obs) s->obs_streak++;
        else { s->last_obs = obs; s->obs_streak = 1; }
        if (s->warming && obs >= 1 && obs <= 8 && s->warm_seen[obs] < 255) s->warm_seen[obs]++;
        if (obs >= 1 && obs <= 8 && obs != s->depth) {
            if (obs == s->depth_cand) s->depth_votes++;
            else { s->depth_cand = obs; s->depth_votes = 1; }
            if (s->depth_votes >= s->depth_need) {
                if (s->warming) s->depth = s->show_depth = obs;
                else yscr__depth_set(s, obs, s->depth_need == 1 ? YSCR_DEPTH_PATH : YSCR_DEPTH_LEARNER, onset, p->rec.index);
                s->depth_votes = 0;
                s->depth_need = 3;
            }
        } else if (obs == s->depth) {
            /* Shown on the vblank after the flip before it, it may have
             * waited behind that flip: then its obs says "this depth or
             * less", and must not close a path change's one-vote window
             * (at a fall from 2 to 1 it did, and 3 flips showed early). */
            if (s->depth_need == 1 && s->adopt_left > 0 && count == s->prev_shown + 1) {
                if (--s->adopt_left == 0) s->depth_need = 3;
            } else {
                s->depth_votes = 0;
                s->depth_need = 3;
            }
        }
    }
    yscr__finish(s, p, count);
}

static void yscr__drain(yscr_screen* s) {
    yscr_vblank v[4];
    int i, n;
    memset(v, 0, sizeof v);   /* a presenter that leaves a field unset means 0 */
    n = s->pr->completions(s->pr_ctx, v, 4);
    for (i = 0; i < n; i++) yscr__complete(s, &v[i]);
}

/* DXGI reports only the newest flip, so a flip that happened since the
 * last read must be read before the next present can hide it. Only then,
 * because each read is an OS call. */
static int yscr__overdue(const yscr_screen* s, int64_t now) {
    int i;
    for (i = 0; i < YSCR__MAX_PEND; i++)
        if (s->pend[i].used && yscr__time_of(s, s->pend[i].planned_count) <= now) return 1;
    return 0;
}

static void yscr__drain_overdue(yscr_screen* s) {
    if (yscr__overdue(s, yscr__now())) yscr__drain(s);
}

static int yscr__pending(const yscr_screen* s) {
    int i, n = 0;
    for (i = 0; i < YSCR__MAX_PEND; i++) n += s->pend[i].used != 0;
    return n;
}

static void yscr__gl_load(yscr_screen* s) {
    static const char* const names[11] = {
        "glClearColor", "glClear", "glScissor", "glEnable", "glDisable", "glIsEnabled",
        "glGetIntegerv", "glGetFloatv", "glGetBooleanv", "glColorMask", "glBindFramebuffer"
    };
    int i;
    memset(s->gl, 0, sizeof s->gl);
    if (!s->pr->gl_proc) return;
    for (i = 0; i < 11; i++) s->gl[i] = s->pr->gl_proc(s->pr_ctx, names[i]);
    for (i = 0; i <= YSCR__GL_BINDFRAMEBUFFER; i++)
        if (!s->gl[i]) { memset(s->gl, 0, sizeof s->gl); return; }
}

static void yscr__clear_black(yscr_screen* s) {
    if (!s->gl[YSCR__GL_CLEAR]) return;
    ((yscr__glClearColor_fn)s->gl[YSCR__GL_CLEARCOLOR])(0.0f, 0.0f, 0.0f, 1.0f);
    ((yscr__glClear_fn)s->gl[YSCR__GL_CLEAR])(YSCR__GL_COLOR_BUFFER_BIT);
}

/* The patch square in pixels, top-left origin. */
static void yscr__patch_rect(const yscr_screen* s, int* x, int* y, int* size) {
    int w = s->caps.mode.w, h = s->caps.mode.h;
    *size = s->patch.size > 0 ? s->patch.size : YSCR_PATCH_DEFAULT_SIZE;
    *x = (s->patch.corner == YSCR_TOP_RIGHT || s->patch.corner == YSCR_BOTTOM_RIGHT) ? w - *size : 0;
    *y = (s->patch.corner == YSCR_BOTTOM_LEFT || s->patch.corner == YSCR_BOTTOM_RIGHT) ? h - *size : 0;
}

/* A scissored clear on the default framebuffer, so the patch costs no draw
 * call and leaves the caller's pipeline alone; the five pieces of state it
 * touches are put back. */
static void yscr__draw_patch(yscr_screen* s) {
    int box[4], fb = 0, size, x, y;
    float cc[4];
    unsigned char mask[4], scissor, discard;
    yscr__glEnable_fn en = (yscr__glEnable_fn)s->gl[YSCR__GL_ENABLE];
    yscr__glEnable_fn dis = (yscr__glEnable_fn)s->gl[YSCR__GL_DISABLE];
    yscr__glIsEnabled_fn is = (yscr__glIsEnabled_fn)s->gl[YSCR__GL_ISENABLED];
    yscr__glGetIntegerv_fn geti = (yscr__glGetIntegerv_fn)s->gl[YSCR__GL_GETINTEGERV];
    yscr__glBindFramebuffer_fn bind = (yscr__glBindFramebuffer_fn)s->gl[YSCR__GL_BINDFRAMEBUFFER];
    yscr__glColorMask_fn cmask = (yscr__glColorMask_fn)s->gl[YSCR__GL_COLORMASK];
    float v = s->patch_value;
    if (!s->patch.on || !s->gl[YSCR__GL_CLEAR]) return;
    yscr__patch_rect(s, &x, &y, &size);
    y = s->caps.mode.h - y - size;   /* GL's origin is the bottom left */
    scissor = is(YSCR__GL_SCISSOR_TEST);
    discard = is(YSCR__GL_RASTERIZER_DISCARD);
    geti(YSCR__GL_SCISSOR_BOX, box);
    geti(YSCR__GL_DRAW_FB_BINDING, &fb);
    ((yscr__glGetFloatv_fn)s->gl[YSCR__GL_GETFLOATV])(YSCR__GL_COLOR_CLEAR_VALUE, cc);
    ((yscr__glGetBooleanv_fn)s->gl[YSCR__GL_GETBOOLEANV])(YSCR__GL_COLOR_WRITEMASK, mask);
    if (fb) bind(YSCR__GL_DRAW_FRAMEBUFFER, 0);
    if (!scissor) en(YSCR__GL_SCISSOR_TEST);
    if (discard) dis(YSCR__GL_RASTERIZER_DISCARD);
    cmask(1, 1, 1, 1);
    ((yscr__glScissor_fn)s->gl[YSCR__GL_SCISSOR])(x, y, size, size);
    ((yscr__glClearColor_fn)s->gl[YSCR__GL_CLEARCOLOR])(v, v, v, 1.0f);
    ((yscr__glClear_fn)s->gl[YSCR__GL_CLEAR])(YSCR__GL_COLOR_BUFFER_BIT);
    ((yscr__glClearColor_fn)s->gl[YSCR__GL_CLEARCOLOR])(cc[0], cc[1], cc[2], cc[3]);
    ((yscr__glScissor_fn)s->gl[YSCR__GL_SCISSOR])(box[0], box[1], box[2], box[3]);
    cmask(mask[0], mask[1], mask[2], mask[3]);
    if (discard) en(YSCR__GL_RASTERIZER_DISCARD);
    if (!scissor) dis(YSCR__GL_SCISSOR_TEST);
    if (fb) bind(YSCR__GL_DRAW_FRAMEBUFFER, (unsigned int)fb);
}


/* --- codes ------------------------------------------------------------------ */

static int yscr__rects_meet(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

/* Checks and copies desc.codes once the mode is known. 0, or a message. */
static const char* yscr__codes_open(yscr_screen* s, const yscr_desc* desc) {
    int i, k, off = 0;
    if (desc->n_codes < 0 || desc->n_codes > YSCR_MAX_CODES) return "desc.n_codes must be 0 to YSCR_MAX_CODES";
    if (desc->n_codes == 0) return NULL;
    if (desc->windowed && s->pr->needs_window)
        return "codes need a fullscreen screen: a device reads display pixels, and a window is not at the display origin";
    for (i = 0; i < desc->n_codes; i++) {
        yscr_code_slot c = desc->codes[i];
        if (c.kind == YSCR_CODE_ROW) c.h = 1;
        if ((c.kind != YSCR_CODE_SOLID && c.kind != YSCR_CODE_ROW) || c.w < 1 || c.h < 1 || c.x < 0 || c.y < 0 ||
            (s->caps.mode.w > 0 && (c.x + c.w > s->caps.mode.w || c.y + c.h > s->caps.mode.h)))   /* SIM has no pixels */
            return "a code slot is empty, of an unknown kind, or outside the display";
        if (c.kind == YSCR_CODE_ROW) {
            if (off + c.w > YSCR_CODE_ROW_PIXELS) return "ROW code slots hold more than YSCR_CODE_ROW_PIXELS pixels";
            s->code_off[i] = off;
            for (k = 0; k < c.w; k++) s->code_px[off + k] = c.rest & 0xFFFFFFu;
            off += c.w;
        }
        for (k = 0; k < i; k++) {
            const yscr_code_slot* o = &s->code_slot[k];
            if (yscr__rects_meet(c.x, c.y, c.w, c.h, o->x, o->y, o->w, o->h)) return "two code slots overlap";
        }
        if (desc->patch.on) {
            int px, py, ps;
            yscr__patch_rect(s, &px, &py, &ps);
            if (yscr__rects_meet(c.x, c.y, c.w, c.h, px, py, ps, ps))
                return "a code slot overlaps the photodiode patch (move the patch: desc.patch.corner)";
        }
        s->code_slot[i] = c;
        s->code_val[i] = c.rest & 0xFFFFFFu;
        s->code_left[i] = 0;
    }
    s->n_codes = desc->n_codes;
    s->verify_every = desc->verify_codes > 0 ? desc->verify_codes : 0;
    return NULL;
}

/* This flip's code list, in one of two buffers, so the list the swap path
 * read back on the previous present is still whole on this one. */
static const yscr_code_draw* yscr__codes_build(yscr_screen* s, uint32_t* vals) {
    int i, b = s->code_buf;
    yscr_code_draw* d = s->code_draw[b];
    uint32_t* px = s->code_frame_px[b];
    for (i = 0; i < s->n_codes; i++) {
        const yscr_code_slot* c = &s->code_slot[i];
        int active = s->code_left[i] != 0;
        d[i].x = c->x; d[i].y = c->y; d[i].w = c->w; d[i].h = c->h;
        if (c->kind == YSCR_CODE_ROW) {
            int k, off = s->code_off[i];
            for (k = 0; k < c->w; k++) px[off + k] = active ? s->code_px[off + k] : (c->rest & 0xFFFFFFu);
            d[i].px = px + off;
            d[i].value = 0;
            vals[i] = px[off];
        } else {
            d[i].px = NULL;
            d[i].value = active ? s->code_val[i] : (c->rest & 0xFFFFFFu);
            vals[i] = d[i].value;
        }
    }
    return d;
}

/* After a present that went out: one flip less for each timed code. */
static void yscr__codes_advance(yscr_screen* s) {
    int i;
    for (i = 0; i < s->n_codes; i++) if (s->code_left[i] > 0) s->code_left[i]--;
    s->code_buf ^= 1;
}

/* Codes through GL, for a presenter that does not draw them: one
 * scissored clear per SOLID slot and per ROW pixel, with the patch's
 * save-and-restore list. glClearColor's float converts to the code. */
static void yscr__gl_codes(yscr_screen* s, const yscr_code_draw* d, int n) {
    int box[4], fb = 0, i, k, h = s->caps.mode.h;
    float cc[4];
    unsigned char mask[4], scissor, discard;
    yscr__glEnable_fn en = (yscr__glEnable_fn)s->gl[YSCR__GL_ENABLE];
    yscr__glEnable_fn dis = (yscr__glEnable_fn)s->gl[YSCR__GL_DISABLE];
    yscr__glIsEnabled_fn is = (yscr__glIsEnabled_fn)s->gl[YSCR__GL_ISENABLED];
    yscr__glGetIntegerv_fn geti = (yscr__glGetIntegerv_fn)s->gl[YSCR__GL_GETINTEGERV];
    yscr__glBindFramebuffer_fn bind = (yscr__glBindFramebuffer_fn)s->gl[YSCR__GL_BINDFRAMEBUFFER];
    yscr__glColorMask_fn cmask = (yscr__glColorMask_fn)s->gl[YSCR__GL_COLORMASK];
    yscr__glScissor_fn sc = (yscr__glScissor_fn)s->gl[YSCR__GL_SCISSOR];
    yscr__glClearColor_fn ccol = (yscr__glClearColor_fn)s->gl[YSCR__GL_CLEARCOLOR];
    yscr__glClear_fn clr = (yscr__glClear_fn)s->gl[YSCR__GL_CLEAR];
    if (n < 1 || !clr) return;
    scissor = is(YSCR__GL_SCISSOR_TEST);
    discard = is(YSCR__GL_RASTERIZER_DISCARD);
    geti(YSCR__GL_SCISSOR_BOX, box);
    geti(YSCR__GL_DRAW_FB_BINDING, &fb);
    ((yscr__glGetFloatv_fn)s->gl[YSCR__GL_GETFLOATV])(YSCR__GL_COLOR_CLEAR_VALUE, cc);
    ((yscr__glGetBooleanv_fn)s->gl[YSCR__GL_GETBOOLEANV])(YSCR__GL_COLOR_WRITEMASK, mask);
    if (fb) bind(YSCR__GL_DRAW_FRAMEBUFFER, 0);
    if (!scissor) en(YSCR__GL_SCISSOR_TEST);
    if (discard) dis(YSCR__GL_RASTERIZER_DISCARD);
    cmask(1, 1, 1, 1);
    for (i = 0; i < n; i++) {
        int cnt = d[i].px ? d[i].w : 1;
        for (k = 0; k < cnt; k++) {
            uint32_t v = d[i].px ? d[i].px[k] : d[i].value;
            if (d[i].px) sc(d[i].x + k, h - d[i].y - 1, 1, 1);
            else sc(d[i].x, h - d[i].y - d[i].h, d[i].w, d[i].h);   /* GL's origin is the bottom left */
            ccol((float)((double)(v & 0xFFu) / 255.0), (float)((double)((v >> 8) & 0xFFu) / 255.0),
                 (float)((double)((v >> 16) & 0xFFu) / 255.0), 1.0f);
            clr(YSCR__GL_COLOR_BUFFER_BIT);
        }
    }
    ccol(cc[0], cc[1], cc[2], cc[3]);
    sc(box[0], box[1], box[2], box[3]);
    cmask(mask[0], mask[1], mask[2], mask[3]);
    if (discard) en(YSCR__GL_RASTERIZER_DISCARD);
    if (!scissor) dis(YSCR__GL_SCISSOR_TEST);
    if (fb) bind(YSCR__GL_DRAW_FRAMEBUFFER, (unsigned int)fb);
}

/* One CODE record per flip on a screen with codes: what was in the frame. */
static void yscr__push_code(yscr_screen* s, const yscr__pend* p) {
    yrt_event ev;
    int i;
    if (!s->ring || s->n_codes < 1) return;
    memset(&ev, 0, sizeof ev);
    ev.t_ns = (uint64_t)(p->rec.onset ? p->rec.onset : p->rec.planned);
    ev.source = (uint16_t)YRT_SRC_SCREEN;
    ev.kind = (uint16_t)YSCR_EV_CODE;
    ev.aux = s->display_index;
    ev.u.u32[0] = (uint32_t)(p->rec.index & 0xFFFFFFFF);
    ev.u.u16[2] = p->rec.code_risk;
    ev.u.u16[3] = (uint16_t)s->n_codes;
    for (i = 0; i < s->n_codes && i < 8; i++) ev.u.u32[2 + i] = p->code_vals[i];
    yscr__push(s, &ev);
}

/* --- triggers ---------------------------------------------------------------- */

#define YSCR__J_FREE     0
#define YSCR__J_ARMED    1
#define YSCR__J_FIRING   2
#define YSCR__J_FIRED    3
#define YSCR__J_CANCELED 4
#define YSCR__FENCE_GUARD_NS 1000000   /* the GPU check runs this long before */
#define YSCR__WORKER_LATE_NS 1000000
/* The worker re-arms itself this long before the next vblank's deadline
 * after a flip trigger fires, so the frame thread's arm for that vblank
 * finds the worker armed early enough and needs no submit (TRIGGERS in
 * STATUS: the submit was most of the arming cost). Wider than the drift
 * of the grid from one frame to the next. */
#define YSCR__SPEC_EARLY_NS 50000

static void yscr__trig_run(yscr_screen* s, int64_t now, int flushed);

#if !defined(YRT_NO_THREADS) && !defined(YSCR__ARM)
#define YSCR__REAL_WORKER 1
static void yscr__trig_job(void* ctx, const yrt_job_info* info) {
    yscr_screen* s = (yscr_screen*)ctx;
    s->woke = (int64_t)info->at_ns;
    yscr__trig_run(s, yscr__now(), info->flushed ? 1 : 0);
}
#if defined(_WIN32)
/* The logical CPUs of group 0 below the highest efficiency class: the
 * E-cores of a hybrid CPU. 0 on a CPU with one class, or without CPU sets. */
static DWORD_PTR yscr__ecore_mask(void) {
    typedef BOOL (WINAPI *info_fn)(void*, ULONG, PULONG, HANDLE, ULONG);
    unsigned char buf[256 * 32];   /* SYSTEM_CPU_SET_INFORMATION is 32 bytes */
    ULONG len = (ULONG)sizeof buf, off;
    BYTE top = 0, eff;
    DWORD_PTR mask = 0;
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    info_fn info = k ? (info_fn)(void (*)(void))GetProcAddress(k, "GetSystemCpuSetInformation") : NULL;
    if (!info || !info(buf, len, &len, GetCurrentProcess(), 0)) return 0;
    for (off = 0; off + 32 <= len && *(DWORD*)(void*)(buf + off) >= 32; off += *(DWORD*)(void*)(buf + off))
        if (*(DWORD*)(void*)(buf + off + 4) == 0 && buf[off + 18] > top) top = buf[off + 18];
    for (off = 0; off + 32 <= len && *(DWORD*)(void*)(buf + off) >= 32; off += *(DWORD*)(void*)(buf + off)) {
        eff = buf[off + 18];
        if (*(DWORD*)(void*)(buf + off + 4) == 0 && *(WORD*)(void*)(buf + off + 12) == 0 &&
            buf[off + 14] < 8 * sizeof(DWORD_PTR) && eff < top)
            mask |= (DWORD_PTR)1 << buf[off + 14];
    }
    return mask;
}
#endif
static bool yscr__worker_on_start(void* ctx, char* err, size_t cap) {
    yscr_screen* s = (yscr_screen*)ctx;
    if (s->trig_cpu > 0) {
        if (yrt_thread_pin(s->trig_cpu - 1) < 0) {
            snprintf(err, cap, "ysp_screen: the trigger worker could not be pinned to CPU %d", s->trig_cpu - 1);
            return false;
        }
        s->trig_cores = 'N';
        s->trig_mask = (uint64_t)1 << ((s->trig_cpu - 1) & 63);
        return true;
    }
#if defined(_WIN32)
    /* Off the P-cores: there the display's vblank DPC took the CPU the
     * worker spun on, 50 to 400 us at the deadline (TRIGGERS in STATUS).
     * A hard mask, because Windows ran the worker on P-cores with E-core
     * CPU sets selected; a set of cores, not one, so a busy core does not
     * hold it. */
    if (s->trig_cpu == 0) {
        typedef BOOL (WINAPI *sel_fn)(HANDLE, const ULONG*, ULONG);
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        sel_fn sel = k ? (sel_fn)(void (*)(void))GetProcAddress(k, "SetThreadSelectedCpuSets") : NULL;
        DWORD_PTR m = yscr__ecore_mask();
        if (m && SetThreadAffinityMask(GetCurrentThread(), m)) {
            /* ysp/rt.h's P-core preference must not compete with the mask */
            if (sel) (void)sel(GetCurrentThread(), NULL, 0);
            s->trig_cores = 'E';
            s->trig_mask = (uint64_t)m;
        }
    }
#endif
    return true;
}
static int yscr__worker_start(yscr_screen* s) {
    yrt_worker_desc wd;
    memset(&wd, 0, sizeof wd);
    wd.on_start = yscr__worker_on_start;
    wd.start_ctx = s;
    wd.spin_ns = s->trig_spin;
    memset(&s->worker, 0, sizeof s->worker);
    return yrt_worker_open(&s->worker, &wd) ? 1 : 0;
}
static void yscr__worker_stop(yscr_screen* s) { yrt_worker_close(&s->worker); }
static void yscr__worker_arm(yscr_screen* s, int64_t t) {
    yrt_worker_submit(&s->worker, (int64_t)(t > 0 ? t : 0), yscr__trig_job, s);
}
#define YSCR__ARM(s, t)        yscr__worker_arm((s), (t))
#define YSCR__WORKER_START(s)  yscr__worker_start(s)
#define YSCR__WORKER_STOP(s)   yscr__worker_stop(s)
#elif !defined(YSCR__ARM)
#define YSCR__ARM(s, t)        ((void)(s), (void)(t))
#define YSCR__WORKER_START(s)  ((void)(s), 0)
#define YSCR__WORKER_STOP(s)   ((void)(s))
#endif

/* Under the lock: keep the worker armed no later than the earliest wake.
 * An earlier wake is enough: the worker then fires nothing and re-arms for
 * the real one itself, off the frame thread. spec is a wake to arm when no
 * job is armed (0 for none). */
static void yscr__trig_rearm_spec(yscr_screen* s, int64_t spec) {
    int64_t w = INT64_MAX;
    int i;
    for (i = 0; i < YSCR_MAX_JOBS; i++)
        if (s->job[i].state == YSCR__J_ARMED && s->job[i].wake < w) w = s->job[i].wake;
    if (w == INT64_MAX && spec > 0) w = spec;
    if (w != INT64_MAX && w < s->armed_wake) {
        YRT_ZONE(z_sub, "yscr.trigsubmit");
        s->armed_wake = w;
        YSCR__ARM(s, w);
        YRT_ZONE_END(z_sub);
    }
}
static void yscr__trig_rearm(yscr_screen* s) { yscr__trig_rearm_spec(s, 0); }

static void yscr__trig_set_count(yscr_screen* s, yscr__job* j, int64_t count) {
    j->count = count;
    j->deadline = yscr__time_of(s, count) + s->offset + s->trig[j->channel].offset_ns;
    j->wake = j->check ? j->deadline - YSCR__FENCE_GUARD_NS : j->deadline;
}

/* The worker's callback, and the test's: fire what is due. A job chosen
 * here is FIRING under the lock, so a move from the frame thread cannot
 * touch it; the callbacks run with the lock released. */
static void yscr__trig_run(yscr_screen* s, int64_t now, int flushed) {
    int idx[YSCR_MAX_JOBS], n = 0, i, k;
    yscr_trigger_info info[YSCR_MAX_JOBS];
    int64_t fired[YSCR_MAX_JOBS], woke = s->woke ? s->woke : now, t_lock, spec = 0;
    s->woke = 0;
    yscr__lock(s);
    t_lock = yscr__now() - now;
    s->armed_wake = INT64_MAX;
    for (i = 0; i < YSCR_MAX_JOBS; i++) {
        yscr__job* j = &s->job[i];
        if (j->state != YSCR__J_ARMED) continue;
        if (!flushed && j->wake > now) continue;
        if (j->check && !flushed) {
            uint64_t done = s->pr && s->pr->gpu_done ? s->pr->gpu_done(s->pr_ctx) : UINT64_MAX;
            if (done < j->pend_id) {   /* not finished: the frame will miss */
                j->count++;
                j->deadline += j->period;
                j->wake = j->deadline - YSCR__FENCE_GUARD_NS;
                j->flags |= (uint16_t)(YSCR_TRIG_MOVED | YSCR_TRIG_GPU_MOVED);
                continue;
            }
            j->check = 0;
            j->wake = j->deadline;
            if (j->wake > now) continue;
        }
        j->state = YSCR__J_FIRING;
        if (flushed) j->flags |= YSCR_TRIG_FLUSHED;
        k = n++;
        while (k > 0 && s->job[idx[k - 1]].deadline > j->deadline) { idx[k] = idx[k - 1]; k--; }
        idx[k] = i;
    }
    for (k = 0; k < n; k++) {
        const yscr__job* j = &s->job[idx[k]];
        info[k].deadline_ns = j->deadline;
        info[k].fired_ns = 0;
        info[k].woke_ns = woke;
        info[k].lock_ns = t_lock;
        info[k].frame = j->frame;
        info[k].code = j->code;
        info[k].channel = j->channel;
        info[k].flags = j->flags;
    }
    yscr__unlock(s);
    for (k = 0; k < n; k++) {
        const yscr_trigger_desc* t = &s->trig[info[k].channel];
        fired[k] = yscr__now();
        info[k].fired_ns = fired[k];
        if (t->fn) t->fn(t->ctx, &info[k]);
    }
    yscr__lock(s);
    for (k = 0; k < n; k++) {
        yscr__job* j = &s->job[idx[k]];
        j->fired = fired[k];
        j->state = YSCR__J_FIRED;
        if (!flushed && fired[k] - j->deadline > YSCR__WORKER_LATE_NS) j->flags |= YSCR_TRIG_WORKER_LATE;
        /* a flip trigger: the next frame's trigger is likely one period on */
        if (!flushed && j->pend_id) {
            int64_t w = j->deadline + j->period - YSCR__SPEC_EARLY_NS;
            if (s->trig_fence && s->pr && s->pr->gpu_done) w -= YSCR__FENCE_GUARD_NS;
            if (w > spec) spec = w;
        }
    }
    yscr__trig_rearm_spec(s, spec);
    yscr__unlock(s);
}

/* After the present returned: arm this frame's triggers. A present that
 * returned past the planned vblank's latch cannot be shown on it, so its
 * triggers go to the first vblank it can still make (MOVED). */
static void yscr__trig_arm_flip(yscr_screen* s, const yscr__pend* p) {
    int64_t c = p->planned_count, e;
    uint16_t moved = 0;
    int r, i;
    if (s->n_req < 1) return;
    e = yscr__count_at(s, p->t_ret + s->margin_ns) + s->show_depth;
    if (e > c) { c = e; moved = YSCR_TRIG_MOVED; }
    yscr__lock(s);
    for (r = 0; r < s->n_req; r++) {
        yscr__job* j = NULL;
        for (i = 0; i < YSCR_MAX_JOBS; i++) if (s->job[i].state == YSCR__J_FREE) { j = &s->job[i]; break; }
        if (!j) { s->trig_lost++; continue; }
        memset(j, 0, sizeof *j);
        j->state = YSCR__J_ARMED;
        j->channel = s->req_ch[r];
        j->code = s->req_code[r];
        j->pend_id = p->id;
        j->frame = p->rec.index;
        j->period = (int64_t)llround(s->period_f);
        j->flags = moved;
        j->check = s->trig_fence && s->pr->gpu_done != NULL;
        yscr__trig_set_count(s, j, c);
    }
    s->n_req = 0;
    yscr__trig_rearm(s);
    yscr__unlock(s);
}

/* Final jobs (fired or canceled, and their flip known) go to the ring and
 * free their slot. */
static void yscr__trig_emit(yscr_screen* s) {
    yscr__job out[YSCR_MAX_JOBS];
    int i, n = 0;
    if (!s->trig_on) return;
    yscr__lock(s);
    for (i = 0; i < YSCR_MAX_JOBS; i++) {
        yscr__job* j = &s->job[i];
        if ((j->state == YSCR__J_FIRED || j->state == YSCR__J_CANCELED) && j->flip_done) {
            out[n++] = *j;
            j->state = YSCR__J_FREE;
        }
    }
    yscr__unlock(s);
    for (i = 0; i < n; i++) {
        yrt_event ev;
        if (!s->ring) break;
        memset(&ev, 0, sizeof ev);
        ev.t_ns = (uint64_t)(out[i].fired ? out[i].fired : out[i].deadline);
        ev.source = (uint16_t)YRT_SRC_SCREEN;
        ev.kind = (uint16_t)YSCR_EV_TRIGGER;
        ev.aux = s->display_index;
        ev.u.i64[0] = out[i].deadline;
        ev.u.i64[1] = out[i].onset;
        ev.u.u32[4] = out[i].code;
        ev.u.u32[5] = (uint32_t)(out[i].frame & 0xFFFFFFFF);
        ev.u.u16[12] = out[i].channel;
        ev.u.u16[13] = out[i].flags;
        ev.u.i32[7] = out[i].mismatch;
        ev.u.i32[8] = out[i].fired ? (int32_t)yscr__sat32(out[i].fired - out[i].deadline) : 0;
        yscr__push(s, &ev);
    }
}

/* A flip's record is complete: settle its triggers against the vblank it
 * was shown on, then the after-flip callback. A trigger not fired yet moves
 * to that vblank; one already fired is a mismatch. */
static void yscr__trig_flip_done(yscr_screen* s, const yscr__pend* p, int64_t shown) {
    yscr_trigger_result res[YSCR_MAX_JOBS];
    const yscr_record* r = &p->rec;
    int i, n = 0, moved = 0;
    if (s->trig_on) {
        yscr__lock(s);
        for (i = 0; i < YSCR_MAX_JOBS; i++) {
            yscr__job* j = &s->job[i];
            if (j->state == YSCR__J_FREE || j->pend_id != p->id || j->flip_done) continue;
            j->flip_done = 1;
            if (r->flags & (YSCR_FLIP_SKIPPED | YSCR_FLIP_CANCELED)) {
                j->flags |= YSCR_TRIG_NOT_SHOWN;
                if (j->state == YSCR__J_ARMED) { j->state = YSCR__J_CANCELED; j->flags |= YSCR_TRIG_CANCELED; }
            } else {
                j->onset = r->onset;
                if (r->flags & YSCR_FLIP_ESTIMATED) {
                    j->flags |= YSCR_TRIG_ESTIMATED;
                } else if (shown != j->count) {
                    if (j->state == YSCR__J_ARMED) {
                        yscr__trig_set_count(s, j, shown);
                        j->flags |= YSCR_TRIG_MOVED;
                        moved = 1;
                    } else {
                        j->mismatch = (int32_t)(shown - j->count);
                        j->flags |= (uint16_t)(shown > j->count ? YSCR_TRIG_FIRED_EARLY : YSCR_TRIG_FIRED_LATE);
                    }
                }
            }
            res[n].deadline_ns = j->deadline;
            res[n].fired_ns = j->state == YSCR__J_FIRED ? j->fired : 0;
            res[n].onset_ns = (r->flags & (YSCR_FLIP_SKIPPED | YSCR_FLIP_CANCELED)) ? 0 : r->onset;
            res[n].frame = j->frame;
            res[n].code = j->code;
            res[n].channel = j->channel;
            res[n].flags = (uint16_t)(j->flags | ((j->state == YSCR__J_ARMED || j->state == YSCR__J_FIRING)
                                                  ? YSCR_TRIG_PENDING : 0));
            res[n].mismatch = j->mismatch;
            res[n].reserved_ = 0;
            n++;
        }
        if (moved) yscr__trig_rearm(s);
        yscr__unlock(s);
    }
    if (s->on_flip) s->on_flip(s->on_flip_ctx, r, res, n);
    yscr__trig_emit(s);
}

/* Present a few black frames at open, so the first begin() has a vblank
 * anchor and a measured depth, not a guess. */
static void yscr__warmup(yscr_screen* s) {
    int i;
    s->warming = 1;
    /* At least 6 frames, and on until three flips in a row agree with the
     * depth: a window that has just gone fullscreen is composed for a few
     * frames before it gets an overlay or independent flip. */
    for (i = 0; i < 40; i++) {
        /* a backend that holds a frame to its target learns its depth from
         * misses after open, so only the anchor and a steady path matter */
        if (i >= 6 && s->obs_streak >= 3 && (s->caps.native_target || s->last_obs == s->depth)) break;
        yscr_present_req req;
        yscr_vblank newest;
        int64_t now = yscr__now();
        newest.t_ns = 0;
        if (s->pr->acquire(s->pr_ctx, now + 200000000, &newest) < 0) break;
        yscr__drain(s);
        if (newest.t_ns && !s->have_anchor) yscr__anchor(s, newest.t_ns, newest.count, 0);
        yscr__clear_black(s);
        memset(&req, 0, sizeof req);
        req.present_id = ++s->next_id;
        req.hold = 1;
        {
            yscr__pend* p = &s->pend[i % YSCR__MAX_PEND];
            if (p->used) yscr__estimate(s, p);
            memset(p, 0, sizeof *p);
            p->used = 1;
            p->id = req.present_id;
            p->rec.index = -1 - i;
            p->rec.flags = YSCR_FLIP_PENDING;
            if (s->have_anchor) {
                p->count_at_present = yscr__count_at(s, yscr__now());
                p->planned_count = p->count_at_present + s->depth;
                p->asap = 1;
            } else {
                p->planned_count = 0;
            }
            req.target_count = s->have_anchor ? p->planned_count : 0;
        }
        if (s->pr->present(s->pr_ctx, &req) < 0) break;
    }
    /* Wait for the last statistics, as a frame loop would. */
    {
        int64_t end = yscr__now() + (int64_t)(4 * s->period_f) + 50000000;
        yscr_vblank newest;
        newest.t_ns = 0;
        if (s->pr->acquire(s->pr_ctx, end, &newest) == YSCR_OK) s->slot_held = 1;
        while (yscr__pending(s) > 0 && yscr__now() < end) {
            yscr__drain(s);
            if (yscr__pending(s) == 0) break;
            YSCR__SLEEP_UNTIL((int64_t)(yscr__now() + 200000), 0);
        }
    }
    for (i = 0; i < YSCR__MAX_PEND; i++) s->pend[i].used = 0;
    s->warming = 0;
    s->have_last = 0;
    memset(&s->last, 0, sizeof s->last);
    if (!s->have_anchor) {   /* the presenter gives no vblank times */
        s->vb_t = yscr__now();
        s->vb_count = 0;
        s->ref_t = s->vb_t;
        s->ref_count = 0;
        s->have_anchor = 1;
        s->anchor_guessed = 1;
        s->hw_onset_pr = s->caps.hw_onset;
        s->caps.hw_onset = false;
    }
    {   /* The depth at open: measured where a present shows at the first
         * vblank it can; the path's where the system holds it to its
         * target, since an on-time flip there shows only that the depth
         * was enough (DEPTH). */
        int32_t d = s->depth, k;
        /* A flip that waited behind the one before it shows "this depth or
         * less"; three such in a row gave 3 on the composed path (the video
         * worker, 3 of 9 windowed runs). So never more than the smallest
         * depth 2 of open's flips on this path showed. */
        for (k = 1; k < d && !s->caps.native_target && !s->depth_learn; k++)
            if (s->warm_seen[k] >= 2) { d = k; break; }
        if (s->caps.native_target && !s->depth_learn) d = yscr__path_depth(s, s->path);
        else if (!s->caps.native_target && s->path < YSCR__SLACK_PATHS) s->depth_of[s->path] = d;
        s->depth = 0;
        yscr__depth_set(s, d, s->depth_pin ? YSCR_DEPTH_PIN : YSCR_DEPTH_OPEN, yscr__now(), -1);
    }
}

/* The settle= token of the describe line (SETTLE). */
static void yscr__settle_token(const yscr_screen* s, char* b, size_t cap) {
    const char* mode = s->st_mode == YSCR_SETTLE_STRICT ? "strict" : s->st_mode == YSCR_SETTLE_WARN ? "warn" : "off";
    const char* src = s->st_src == YSCR_SETTLE_SRC_ENV ? ":env" : s->st_src == YSCR_SETTLE_SRC_DESC ? ":desc" : "";
    char why[64], hk[32] = "";
    if (s->hooked) snprintf(hk, sizeof hk, ",hook=%.3fs", (double)s->hook_ns * 1e-9);
    if (s->st_cond == YSCR_SETTLE_C_SPREAD && s->st_sd >= 0)
        snprintf(why, sizeof why, "spread,sd=%.1fus,mean%+.1fus", (double)s->st_sd * 1e-3, (double)s->st_mean * 1e-3);
    else if (s->st_cond == YSCR_SETTLE_C_SPREAD)
        snprintf(why, sizeof why, "spread,fewer-than-%d-intervals", YSCR__SETTLE_N);
    else
        snprintf(why, sizeof why, "%s,flip%d,%d/%d-clean", yscr__settle_cname[s->st_cond < 11 ? s->st_cond : 0],
                 (int)s->st_bad, (int)s->st_run, (int)s->st_need);
    switch (s->st_result) {
    case YSCR_SETTLED:
        snprintf(b, cap, "%.2fs(%d flips%s%s%s%s)", (double)s->st_ns * 1e-9, (int)s->st_flips,
                 s->st_src ? "," : "", s->st_src ? mode : "", src, hk);
        break;
    case YSCR_SETTLE_FAILED:
        snprintf(b, cap, "FAILED(%s,%s%s)", why, mode, src);
        break;
    case YSCR_SETTLE_ABORTED:
        snprintf(b, cap, "ABORTED(%.2fs)", (double)s->st_ns * 1e-9);
        break;
    case YSCR_SETTLE_SKIPPED:
        if (s->st_src == YSCR_SETTLE_SRC_ENV) snprintf(b, cap, "SKIPPED(env YSP_SETTLE=%s)", s->st_env);
        else if (s->st_src == YSCR_SETTLE_SRC_DESC) snprintf(b, cap, "SKIPPED(desc)");
        else snprintf(b, cap, "SKIPPED(%s)", s->backend == YSCR_BACKEND_SIM ? "sim" : "no-os-vblank-times");
        break;
    default:
        snprintf(b, cap, "none");
        break;
    }
}

/* The sentence for the operator (yscr_settle_check, and open()'s error). */
static void yscr__settle_message(const yscr_screen* s, char* b, size_t cap) {
    char tok[96];
    yscr__settle_token(s, tok, sizeof tok);
    if (s->st_result != YSCR_SETTLE_FAILED) {
        snprintf(b, cap, "settle=%s", tok);
    } else if (s->backend == YSCR_BACKEND_WEB) {
        snprintf(b, cap, "the browser's frame callbacks did not settle in %.1f s (warn): the last %d intervals had an "
                 "SD of %.1f us (limit 500), or a callback was skipped; the run goes on", (double)s->st_ns * 1e-9,
                 YSCR__SETTLE_N, (double)s->st_sd * 1e-3);
    } else if (s->st_cond == YSCR_SETTLE_C_SPREAD) {
        snprintf(b, cap, "the display did not settle in %.1f s (%s, %s): spread: the last %d vblank intervals had an SD of "
                 "%.1f us (limit %.0f) and a mean %+.1f us from the mode's period (limit %.0f)",
                 (double)s->st_max * 1e-9, s->st_window ? "window" : "fullscreen",
                 s->st_mode == YSCR_SETTLE_STRICT ? "strict" : "warn", YSCR__SETTLE_N, (double)s->st_sd * 1e-3,
                 YSCR__SETTLE_SD_NS * 1e-3, (double)s->st_mean * 1e-3, YSCR__SETTLE_MEAN_NS * 1e-3);
    } else {
        snprintf(b, cap, "the display did not settle in %.1f s (%s, %s): %s at present %d; %d of %d clean flips "
                 "in a row at the cap%s",
                 (double)s->st_max * 1e-9, s->st_window ? "window" : "fullscreen",
                 s->st_mode == YSCR_SETTLE_STRICT ? "strict" : "warn",
                 yscr__settle_cname[s->st_cond < 11 ? s->st_cond : 0], (int)s->st_bad, (int)s->st_run, (int)s->st_need,
                 /* COMPOSITION gives a covered window no statistic */
                 s->st_cond == YSCR_SETTLE_C_STATS || s->st_cond == YSCR_SETTLE_C_ANCHOR
                     ? " (is the window covered, or the program in the background?)" : "");
    }
}

static void yscr__settle_push(yscr_screen* s) {
    yrt_event ev;
    memset(&ev, 0, sizeof ev);
    ev.source = (uint16_t)YRT_SRC_SCREEN;
    ev.kind = (uint16_t)YSCR_EV_SETTLE;
    ev.aux = s->display_index;
    ev.u.u16[0] = (uint16_t)s->st_result;
    ev.u.u16[1] = (uint16_t)s->st_mode;
    ev.u.u16[2] = (uint16_t)s->st_src;
    ev.u.u16[3] = (uint16_t)s->st_cond;
    ev.u.i64[1] = s->st_ns;
    ev.u.u32[4] = (uint32_t)s->st_flips;
    ev.u.u32[5] = (uint32_t)s->st_run;
    ev.u.u32[6] = s->st_sd < 0 ? 0xFFFFFFFFu : (uint32_t)(s->st_sd > 0xFFFFFFFELL ? 0xFFFFFFFELL : s->st_sd);
    ev.u.i32[7] = (int32_t)(s->st_mean > INT32_MAX ? INT32_MAX : s->st_mean < -INT32_MAX ? -INT32_MAX : s->st_mean);
    ev.u.u32[8] = s->fg;
    ev.u.u32[9] = s->fg ? s->fg_mode : 0u;
    yscr__push(s, &ev);
}

/* Black frames after the warm-up until the start of the run has settled
 * (SETTLE): st_need clean flips in a row on one path and one depth, the
 * spread of the last 20 intervals, and the minimum time; or the cap. An
 * abort ends it at once and stays pending, so the caller's first begin()
 * reports it. Returns 0, or < 0 when the swap path failed. */
static int yscr__settle(yscr_screen* s) {
    yscr_frame f;
    int rc = 0;
    s->settling = 1;
    s->index = -1;
    for (;;) {
        int64_t el = yscr__now() - s->st_t0;
        int spread = yscr__settle_spread(s);
        if (yscr__a_load(&yscr__ab.seq) != s->abort_seen) { s->st_result = YSCR_SETTLE_ABORTED; break; }
        if (s->st_run >= s->st_need && spread && el >= s->st_min) {
            s->st_result = YSCR_SETTLED;
            s->st_cond = YSCR_SETTLE_C_NONE;
            break;
        }
        if (el >= s->st_max) {
            s->st_result = YSCR_SETTLE_FAILED;
            if (s->st_run >= s->st_need) s->st_cond = YSCR_SETTLE_C_SPREAD;
            else if (!s->st_cond) s->st_cond = s->anchor_guessed ? YSCR_SETTLE_C_ANCHOR : YSCR_SETTLE_C_STATS;
            break;
        }
        rc = yscr_begin(s, &f);
        if (rc == YSCR_QUIT) { s->st_result = YSCR_SETTLE_ABORTED; rc = 0; break; }
        if (rc < 0) break;
        yscr__clear_black(s);
        rc = yscr_flip_at(s, f.onset, NULL);
        s->index = -1;
        if (rc < 0) break;
    }
    /* Hold a slot, as the warm-up does: after the last settle flip the bound
     * back buffer is the one just presented, and a draw into it before the
     * first begin() (ygfx_prime()) waited forever on COMPOSITION. */
    if (rc >= 0 && !s->slot_held) {
        yscr_vblank newest;
        newest.t_ns = 0;
        if (s->pr->acquire(s->pr_ctx, yscr__now() + (int64_t)(8 * s->period_f) + 200000000, &newest) == YSCR_OK)
            s->slot_held = 1;
    }
    s->settling = 0;
    s->index = 0;
    s->st_ns = yscr__now() - s->st_t0;
    s->st_flips = (int32_t)s->next_id;
    s->depth_changes = 0;   /* the describe line counts the run's */
    return rc < 0 ? rc : 0;
}

/* YSP_SETTLE, read once per open; 0 when unset. The core test replaces it. */
#ifndef YSCR__GETENV
static int yscr__getenv(const char* name, char* out, size_t cap) {
#if defined(_WIN32)
    DWORD n = GetEnvironmentVariableA(name, out, (DWORD)cap);
    if (n == 0 || n >= cap) { out[0] = 0; return n >= cap ? -1 : 0; }
    return 1;
#else
    const char* v = getenv(name);
    if (!v) { out[0] = 0; return 0; }
    if (strlen(v) >= cap) { out[0] = 0; return -1; }
    memcpy(out, v, strlen(v) + 1);
    return 1;
#endif
}
#define YSCR__GETENV(name, out, cap) yscr__getenv((name), (out), (cap))
#endif

/* desc.settle and its fields, YSP_SETTLE over them; the error, or NULL. */
static const char* yscr__settle_desc(yscr_screen* s, const yscr_desc* d) {
    int env, mode = d->settle;
    if (d->settle < 0 || d->settle > YSCR_SETTLE_OFF) return "ysp_screen: desc.settle must be a YSCR_SETTLE_* value";
    if (d->settle_flips < 0 || d->settle_flips > 64) return "ysp_screen: desc.settle_flips must be 0 (6) to 64";
    if (d->settle_max_ns < 0 || d->settle_max_ns > 60000000000LL || d->settle_min_ns > 60000000000LL)
        return "ysp_screen: desc.settle_max_ns must be 0 (the default) to 60 s, and settle_min_ns at most 60 s";
    if (d->foreground < 0 || d->foreground > YSCR_FOREGROUND_FORCE)
        return "ysp_screen: desc.foreground must be a YSCR_FOREGROUND_* value";
    s->fg_mode = (uint32_t)(d->foreground ? d->foreground : d->windowed ? YSCR_FOREGROUND_HONOR : YSCR_FOREGROUND_FORCE);
    s->st_src = d->settle ? YSCR_SETTLE_SRC_DESC : YSCR_SETTLE_SRC_DEFAULT;
    env = YSCR__GETENV("YSP_SETTLE", s->st_env, sizeof s->st_env);
    if (env) {
        if (env > 0 && !strcmp(s->st_env, "off")) mode = YSCR_SETTLE_OFF;
        else if (env > 0 && !strcmp(s->st_env, "warn")) mode = YSCR_SETTLE_WARN;
        else if (env > 0 && !strcmp(s->st_env, "strict")) mode = YSCR_SETTLE_STRICT;
        else return "ysp_screen: the environment variable YSP_SETTLE must be off, warn or strict";
        s->st_src = YSCR_SETTLE_SRC_ENV;
    }
    if (mode == YSCR_SETTLE_AUTO) mode = d->windowed ? YSCR_SETTLE_WARN : YSCR_SETTLE_STRICT;
    s->st_mode = (uint32_t)mode;
    s->st_window = d->windowed ? 1 : 0;
    s->st_tier = d->windowed ? YSCR_TIER_2 : YSCR_TIER_1;
    s->st_need = d->settle_flips ? d->settle_flips : 6;
    /* A window: the system moved one from the composed path to an overlay
     * plane 1.7 to 1.9 s after open under decode load (docs/video.md), so
     * 2.5 s. The cap: every fullscreen open with the window in front settled
     * by 0.8 s; a window's cap is 1.5 s past its minimum, above the slowest
     * window seen (2.95 s, covered at open). */
    s->st_min = d->settle_min_ns < 0 ? 0 : d->settle_min_ns ? d->settle_min_ns : d->windowed ? 2500000000LL : 0;
    s->st_max = d->settle_max_ns ? d->settle_max_ns : d->windowed ? 4000000000LL : 3000000000LL;
    if (mode != YSCR_SETTLE_OFF && s->st_min >= s->st_max)
        return "ysp_screen: the settle minimum must be below the cap (a window's minimum is 2.5 s, "
               "desc.settle_min_ns < 0 removes it)";
    return NULL;
}

#if !defined(YSCR_NO_SDL)
static void yscr__restamp_correlate(yscr_screen* s) {
    yrt_corr_desc d;
    yrt_corr c;
    memset(&d, 0, sizeof d);
    d.read = SDL_GetTicksNS;
    if (yrt_correlate(&d, &c) > 0 && (s->sdl_width == 0 || (uint64_t)c.width_ns <= 2 * s->sdl_width)) {
        s->sdl_rt = (int64_t)c.rt_ns;
        s->sdl_ticks = (int64_t)c.other;
        s->sdl_width = c.width_ns;
        yscr__a_store64(&yscr__ab.sdl_off, s->sdl_rt - s->sdl_ticks);
    }
    s->sdl_corr_t = yscr__now();
}

static uint32_t yscr__mods_of(SDL_Keymod m) {
    return ((m & SDL_KMOD_SHIFT) ? YSCR_MOD_SHIFT : 0u) | ((m & SDL_KMOD_CTRL) ? YSCR_MOD_CTRL : 0u) |
           ((m & SDL_KMOD_ALT) ? YSCR_MOD_ALT : 0u) | ((m & SDL_KMOD_GUI) ? YSCR_MOD_GUI : 0u);
}

/* SDL calls this as it queues each event, on the thread that queues it, so
 * the header sees every abort even when the caller reads the events before
 * begin(). It may run on SDL's raw-input thread: atomics only. */
static bool SDLCALL yscr__abort_watch(void* ud, SDL_Event* e) {
    int64_t t;
    int i;
    (void)ud;
    if (!e) return true;
    t = (int64_t)e->common.timestamp + yscr__a_load64(&yscr__ab.sdl_off);
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if ((uint32_t)e->key.key == yscr__ab.key) {
            if (yscr__abort_edge(&yscr__ab.sdl_held, e->type == SDL_EVENT_KEY_DOWN) &&
                yscr__abort_key((uint32_t)e->key.key, yscr__mods_of(e->key.mod), t, YSCR__AB_SDL)) {
#if defined(YSCR__DXGI)
                if (yscr__wd.thread) yscr__win.post_thread(yscr__wd.tid, WM_APP, 0, 0);
#endif
            }
        } else if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && e->key.key == SDLK_F4 && (e->key.mod & SDL_KMOD_ALT)) {
            yscr__abort_push(YSCR_ABORT_ALT_F4, YSCR__AB_SDL, (uint32_t)e->key.key, yscr__mods_of(e->key.mod), t);
        }
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        for (i = 0; i < YSCR__SLOTS; i++)
            if (yscr__slot[i].s && yscr__slot[i].win == (uint32_t)e->window.windowID) {
                yscr__abort_push(YSCR_ABORT_CLOSE, YSCR__AB_SDL, 0, 0, t);
                break;
            }
        break;
    case SDL_EVENT_QUIT:
        yscr__abort_push(YSCR_ABORT_QUIT, YSCR__AB_SDL, 0, 0, t);
        break;
    default:
        break;
    }
    return true;
}
static int yscr__abort_windows;   /* open screens with a window: the watch's users */

/* The window icon: desc.icon_rgba, or the header's at 32 pixels with 16 and
 * 48 for other scales. SDL copies the surfaces. */
static const char* yscr__set_icon(yscr_screen* s, const yscr_desc* d) {
    SDL_Surface* base = NULL;
    int k, ok = 1;
    if (d->icon_sdl) return NULL;
    if (d->icon_rgba) {
        base = SDL_CreateSurfaceFrom(d->icon_w, d->icon_h, SDL_PIXELFORMAT_RGBA32, (void*)(uintptr_t)d->icon_rgba, d->icon_w * 4);
        ok = base != NULL;
    } else {
        SDL_Surface* alt[3] = { NULL, NULL, NULL };
        for (k = 0; k < 3; k++) {
            alt[k] = SDL_CreateSurface(yscr__icon_size[k][0], yscr__icon_size[k][0], SDL_PIXELFORMAT_RGBA32);
            ok = ok && alt[k] && yscr__icon_decode(k, (uint8_t*)alt[k]->pixels, alt[k]->pitch);
        }
        base = alt[1];
        if (ok) ok = SDL_AddSurfaceAlternateImage(base, alt[0]) && SDL_AddSurfaceAlternateImage(base, alt[2]);
        if (alt[0]) SDL_DestroySurface(alt[0]);
        if (alt[2]) SDL_DestroySurface(alt[2]);
    }
    if (ok) ok = SDL_SetWindowIcon(s->window, base);
    if (base) SDL_DestroySurface(base);
    return ok ? NULL : "the window icon";
}
#endif

/* desc.raw_mice at open: a window on Windows, no SDL relative mode on any
 * window (it would take the registration), then the process's reader. */
#if defined(YSCR__DXGI) && !defined(YSCR_NO_SDL)
/* 1 when any SDL window is in relative mouse mode. SDL's window list is
 * its main thread's (WINDOW THREAD). */
static void yscr__rel_any_cb(void* p) {
    int i, n = 0;
    SDL_Window** wins = SDL_GetWindows(&n);
    *(int*)p = 0;
    for (i = 0; wins && i < n; i++) if (SDL_GetWindowRelativeMouseMode(wins[i])) *(int*)p = 1;
    SDL_free(wins);
}
#endif

static const char* yscr__rm_open(yscr_screen* s) {
#if defined(YSCR__DXGI) && !defined(YSCR_NO_SDL)
    const char* why;
    int rel = 0;
    if (!s->window) return "desc.raw_mice needs a window";
    if (yscr__win_call(s, yscr__rel_any_cb, &rel) < 0) return "desc.raw_mice: the window thread did not answer";
    if (rel)
        return "desc.raw_mice: SDL relative mouse mode is on for a window; both read mice through one "
               "Raw Input registration";
    why = yscr__rm_start();
    if (why) return why;
    s->raw_mice = 1;
    s->rm_state = 0;
    yscr__rm_log(s, YSCR_RAW_MICE_ON);
    return NULL;
#else
#if defined(YSCR__DXGI)
    (void)&yscr__rm_start; (void)&yscr__rm_check_registered; (void)&yscr__rm_request;
#endif
    (void)s;
    return "desc.raw_mice is Windows only, with a window";
#endif
}

/* Once a frame (begin()): the registration guard (INPUT, "Raw mice"). Relative
 * mode is read every frame (23 ns measured); the registration (0.4 to 11
 * us measured, GetRegisteredRawInputDevices) when relative mode changes
 * and every 30th frame, which also catches other code taking the mice. */
static void yscr__rm_check(yscr_screen* s) {
#if defined(YSCR__DXGI) && !defined(YSCR_NO_SDL)
    int rel = s->window && SDL_GetWindowRelativeMouseMode(s->window) ? 1 : 0;
    int act, due = rel != s->rm_rel || ++s->rm_frames >= 30;
    s->rm_rel = rel;
    if (!due) return;
    s->rm_frames = 0;
    act = yscr__rm_guard(&s->rm_state, rel, rel ? 1 : yscr__rm_check_registered());
    if (act == (int)YSCR_RAW_MICE_REGISTERED) yscr__rm_request();
    if (act) yscr__rm_log(s, (uint32_t)act);
#else
    /* the reader and its guard run on Windows with SDL only; the core
     * test calls these directly */
    (void)&yscr__rm_guard; (void)&yscr__mouse_decode; (void)&yscr__rm_log;
    (void)s;
#endif
}

/* desc.on_context (CONTEXT HOOK), before the warm-up: the caller submits
 * work that runs on other threads (ANGLE's compiles) while open() settles;
 * before the warm-up it had 0.2 s more (measured, docs/screen.md). The
 * hook's own time is added to the cap, so a slow hook cannot fail
 * settling; the minimum stays from the open() call, as the window's move
 * to an overlay counts from its creation. */
static void yscr__context_hook(yscr_screen* s, const yscr_desc* desc) {
    int64_t t;
    if (!desc->on_context) return;
    t = yscr__now();
    desc->on_context(desc->on_context_ctx, s);
    s->hook_ns = yscr__now() - t;
    s->hooked = 1;
    s->st_max += s->hook_ns;
    /* settle frames clear the back buffer: the hook may have bound a target */
    if (s->gl[YSCR__GL_BINDFRAMEBUFFER])
        ((yscr__glBindFramebuffer_fn)s->gl[YSCR__GL_BINDFRAMEBUFFER])(YSCR__GL_DRAW_FRAMEBUFFER, 0);
}

#if !defined(YSCR_NO_SDL)
/* What the window part of close() needs, taken from the screen by value, so
 * close() can return while the window thread still runs it (WINDOW THREAD:
 * close must not hang the host). */
typedef struct yscr__wclose {
    SDL_Window* window;
    void*       icon[2];
    void*       pads[YSCR__MAX_PADS];
    int32_t     cursor_hidden, gamepad, video, on_wt;
} yscr__wclose;

static void yscr__wclose_take(yscr_screen* s, yscr__wclose* c) {
    int k;
    memset(c, 0, sizeof *c);
    c->window = s->window;
    for (k = 0; k < 2; k++) { c->icon[k] = s->win_icon[k]; s->win_icon[k] = NULL; }
    for (k = 0; k < YSCR__MAX_PADS; k++) { c->pads[k] = s->pads[k]; s->pads[k] = NULL; }
    c->cursor_hidden = s->cursor_hidden;
    c->gamepad = s->sdl_gamepad;
    c->video = s->sdl_video;
    c->on_wt = s->wt;
    s->sdl_gamepad = 0;
}

/* The window part of close(), on the thread that made the window. */
static void yscr__window_close(void* p) {
    yscr__wclose* c = (yscr__wclose*)p;
    int k;
    if (c->cursor_hidden) SDL_ShowCursor();
#if defined(YSCR__WEB)
    if (c->window) {   /* the page leaves its fullscreen itself */
        yscr__web_fs_hold(1);
        SDL_DestroyWindow(c->window);
        yscr__web_fs_hold(0);
    }
#else
    if (c->window) SDL_DestroyWindow(c->window);
#endif
#if defined(YSCR__DXGI)
    /* after the window that showed them */
    for (k = 0; k < 2; k++) if (c->icon[k] && yscr__win.destroy_icon) yscr__win.destroy_icon((HICON)c->icon[k]);
#endif
    if (c->gamepad) {
        for (k = 0; k < YSCR__MAX_PADS; k++) if (c->pads[k]) SDL_CloseGamepad((SDL_Gamepad*)c->pads[k]);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
    if (c->video) yscr__video_down();
    (void)k;
}
#endif

#if !defined(YSCR_NO_SDL)
/* The window part of open(): on the window thread when there is one
 * (WINDOW THREAD), else on the caller's. A failure leaves the message in
 * s->error; the caller closes, never this function, which may run on the
 * thread that close() would wait for. */
static bool yscr__window_open(yscr_screen* s, const yscr_desc* desc, yscr_mode* want, yscr_presenter_open* in) {
    SDL_DisplayID id;
    const SDL_DisplayMode* dm;
    SDL_DisplayMode match;
    bool explicit_mode = desc->mode.w || desc->mode.h || desc->mode.refresh_num;
    SDL_PropertiesID props;
    /* The raw-input keyboard: stamped when Raw Input delivers the key,
     * not with the message time, which counts in the system tick and was
     * measured up to 15.7 ms early. Default priority, so a caller's own
     * setting of the hint wins. */
    SDL_SetHintWithPriority(SDL_HINT_WINDOWS_RAW_KEYBOARD, "1", SDL_HINT_DEFAULT);
    /* SDL would turn Alt+F4 into a close request; the header reports it
     * as its own reason (ABORT). */
    SDL_SetHintWithPriority(SDL_HINT_WINDOWS_CLOSE_ON_ALT_F4, "0", SDL_HINT_DEFAULT);
    if (!yscr__video_up()) {
        yscr__set_error(s->error, sizeof s->error, "ysp_screen: SDL video: %s", SDL_GetError());
        return false;
    }
    s->sdl_video = 1;
    /* the input bridge's doorbell, once per process (SDL keeps it) */
    if (!yscr__in.type) {
        Uint32 type = SDL_RegisterEvents(1);
        if (type) yscr__in.type = type;
    }
    if (desc->gamepads) {
        int k, n_pads = 0;
        SDL_JoystickID* ids;
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            yscr__set_error(s->error, sizeof s->error, "ysp_screen: SDL gamepads: %s", SDL_GetError());
            return false;
        }
        s->sdl_gamepad = 1;
        ids = SDL_GetGamepads(&n_pads);
        for (k = 0; ids && k < n_pads; k++) yscr__pad_open(s, ids[k]);
        SDL_free(ids);
    }
    id = yscr__display_id(desc->display);
    dm = SDL_GetDesktopDisplayMode(id);
    if (!dm) {
        yscr__set_error(s->error, sizeof s->error, "ysp_screen: display %u: %s", (unsigned)desc->display, SDL_GetError());
        return false;
    }
    match = *dm;
    if (explicit_mode) {
        /* An exact listed mode or nothing: no implicit conversion. */
        int i, n = 0, found = 0;
        SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(id, &n);
        for (i = 0; modes && i < n; i++) {
            const SDL_DisplayMode* m = modes[i];
            if ((desc->mode.w && m->w != desc->mode.w) || (desc->mode.h && m->h != desc->mode.h)) continue;
            if (desc->mode.refresh_num &&
                (int64_t)m->refresh_rate_numerator * (desc->mode.refresh_den ? desc->mode.refresh_den : 1) !=
                (int64_t)desc->mode.refresh_num * m->refresh_rate_denominator) continue;
            match = *m; found = 1; break;
        }
        SDL_free(modes);
        if (!found) {
            yscr__copy(s->error, sizeof s->error, "ysp_screen: desc.mode is not a mode of this display "
                         "(yscr_modes lists them)");
            return false;
        }
    }
#if defined(YSCR__DXGI)
    yscr__win_load();   /* the window a forced raise gives the foreground back to */
    s->fg_prev = yscr__win.foreground ? (void*)yscr__win.foreground() : NULL;
#endif
#if defined(YSCR__WEB)
    /* A page goes fullscreen only from a user gesture (a button), so
     * the header never asks: a "fullscreen" screen is a canvas the size
     * of the page's viewport at open, which the page may have made
     * fullscreen before (BACKENDS, WEB). */
    if (!desc->windowed) {
        match.w = yscr__web_query(4);
        match.h = yscr__web_query(5);
    }
#endif
    props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "ysp_screen");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, (Sint64)SDL_WINDOWPOS_CENTERED_DISPLAY(id));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, (Sint64)SDL_WINDOWPOS_CENTERED_DISPLAY(id));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER,
                          desc->windowed ? (desc->window_w ? desc->window_w : 800) : match.w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER,
                          desc->windowed ? (desc->window_h ? desc->window_h : 600) : match.h);
#if defined(YSCR__WEB)
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, false);
#else
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, !desc->windowed);
#endif
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
#if defined(YSCR__WEB)
    yscr__web_fs_hold(1);
    s->window = SDL_CreateWindowWithProperties(props);
    yscr__web_fs_hold(0);
    /* SDL's visibility handler re-applies the window's flags when the
     * page is shown again, which leaves the page's fullscreen; the
     * header has its own listener (yscr__web_query) */
    if (s->window) emscripten_set_visibilitychange_callback(NULL, 0, NULL);
#else
    s->window = SDL_CreateWindowWithProperties(props);
#endif
    SDL_DestroyProperties(props);
    if (!s->window) {
        yscr__set_error(s->error, sizeof s->error, "ysp_screen: window: %s", SDL_GetError());
        return false;
    }
    if (!desc->windowed && explicit_mode &&
        (match.w != dm->w || match.h != dm->h || match.refresh_rate_numerator != dm->refresh_rate_numerator)) {
        if (!SDL_SetWindowFullscreenMode(s->window, &match)) {
            yscr__set_error(s->error, sizeof s->error, "ysp_screen: mode switch: %s", SDL_GetError());
            return false;
        }
    }
    {
        const char* e = yscr__set_icon(s, desc);
        if (e) {
            yscr__set_error(s->error, sizeof s->error, "ysp_screen: %s: %s", e, SDL_GetError());
            return false;
        }
#if defined(YSCR__DXGI)
        if (!desc->icon_sdl && !desc->icon_rgba) {
            s->hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(s->window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
            yscr__win_icons(s);
        }
#endif
    }
    if (!desc->windowed) SDL_SyncWindow(s->window);
    SDL_RaiseWindow(s->window);
    /* SDL routes keys through the message path while text input runs. */
    SDL_StopTextInput(s->window);
    if (!desc->windowed && !desc->show_cursor) { SDL_HideCursor(); s->cursor_hidden = 1; }
    {   /* the mode obtained, from the OS, not from the request */
        const SDL_DisplayMode* got = desc->windowed ? SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(s->window))
                                                    : SDL_GetWindowFullscreenMode(s->window);
        if (!got) got = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(s->window));
        if (got) yscr__mode_from_sdl(got, want);
        {
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(s->window, &pw, &ph);
            want->w = pw; want->h = ph;
        }
    }
    in->window = s->window;
    yscr__restamp_correlate(s);
    s->hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(s->window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    yscr__slot_take(s, (uint32_t)SDL_GetWindowID(s->window));
    if (s->abort_slot && yscr__abort_windows++ == 0 && !SDL_AddEventWatch(yscr__abort_watch, NULL)) {
        yscr__set_error(s->error, sizeof s->error, "ysp_screen: SDL_AddEventWatch: %s", SDL_GetError());
        return false;
    }
    return true;
}
#endif

/* Runs fn(ctx) on the thread that owns the screen's window (WINDOW THREAD). */
static int yscr__win_call(yscr_screen* s, yscr_window_fn fn, void* ctx) {
#if defined(YSCR__WT)
    if (s && s->wt) return yscr__wt_run(fn, ctx, 0);
#else
    (void)s;
#endif
    fn(ctx);
    return YSCR_OK;
}

#if !defined(YSCR_NO_SDL)
typedef struct { yscr_screen* s; const yscr_desc* d; yscr_mode* want; yscr_presenter_open* in; bool ok; } yscr__wopen;

static void yscr__window_open_cb(void* p) {
    yscr__wopen* w = (yscr__wopen*)p;
    w->ok = yscr__window_open(w->s, w->d, w->want, w->in);
#if defined(YSCR__WT)
    if (yscr__wt_on_thread()) {   /* what the thread's loop and a release need */
        yscr_screen* s = w->s;
        int i;
        if (s->sdl_video) InterlockedIncrement(&yscr__wt.video);
        if (s->sdl_gamepad) InterlockedIncrement(&yscr__wt.pads);
        for (i = 0; s->window && i < YSCR__SLOTS; i++)
            if (!yscr__wt.wins[i]) { yscr__wt.wins[i] = s->window; break; }
    }
#endif
}

/* The window part of close(), as a command: on the window thread it also
 * takes the window off the thread's lists. */
static void yscr__window_close_cb(void* p) {
#if defined(YSCR__WT)
    yscr__wclose* c = (yscr__wclose*)p;
    if (yscr__wt_on_thread()) {
        int i;
        for (i = 0; c->window && i < YSCR__SLOTS; i++) if (yscr__wt.wins[i] == c->window) yscr__wt.wins[i] = NULL;
        if (c->gamepad) InterlockedDecrement(&yscr__wt.pads);
        if (c->video) InterlockedDecrement(&yscr__wt.video);
    }
#endif
    yscr__window_close(p);
}

/* open()'s window: the window thread's mode, the thread for the first
 * screen that uses it, and the window, made there or here. NULL, or why
 * not (s->error may hold SDL's message already). */
static const char* yscr__wt_open(yscr_screen* s, const yscr_desc* desc, yscr_mode* want, yscr_presenter_open* in) {
    yscr__wopen w;
    const char* e;
    int native = s->backend == YSCR_BACKEND_DXGI_FLIP || s->backend == YSCR_BACKEND_COMPOSITION;
    int avail = 0, elsewhere;
#if defined(YSCR__WT)
    avail = 1;
#endif
    if (yscr__a_load(&yscr__released) && yscr__wt_mode_users > 0)
        return "the panic released the display (PANIC): close every screen before the next open";
    elsewhere = yscr__wt_mode_users == 0 && SDL_WasInit(SDL_INIT_VIDEO) != 0;
    s->wt_mode = yscr__wt_decide(desc->window_thread, native, avail, yscr__wt_mode_users ? yscr__wt_mode_now : 0,
                                 elsewhere, &s->wt_why, &e);
    if (e) return e;
    yscr__wt_mode_now = s->wt_mode;
    yscr__wt_mode_users++;
    s->wt_user = 1;
    w.s = s; w.d = desc; w.want = want; w.in = in; w.ok = false;
#if defined(YSCR__WT)
    if (s->wt_mode == YSCR_WINDOW_THREAD_ON) {
        e = yscr__wt_acquire();
        if (e) return e;
        s->wt = 1;
        if (yscr__wt_run(yscr__window_open_cb, &w, 0) < 0) return "window thread: it took no command in 5 s";
        return w.ok ? NULL : "the window";
    }
#endif
    yscr__window_open_cb(&w);
    return w.ok ? NULL : "the window";
}

/* close()'s window, on the thread that made it. With the window thread
 * close() waits at most 5 s for the command to be taken and 5 s for it to
 * run, then returns with the thread left behind: close must not hang the
 * host (a driver call that never returns). The command works on a copy,
 * so the screen may be freed while it runs. */
static void yscr__wt_close(yscr_screen* s) {
    yscr__wclose c;
    yscr__wclose_take(s, &c);
#if defined(YSCR__WT)
    if (s->wt) {
        static yscr__wclose held;   /* one close at a time (MEMORY AND THREADS) */
        int rc = YSCR_ERR_LOST;
        if (!yscr__wt.stuck) {
            held = c;
            rc = yscr__wt_run(yscr__window_close_cb, &held, YSCR__WT_CLOSE_MS);
        }
        if (rc < 0)
            yscr__copy(s->error, sizeof s->error, "ysp_screen: close: the window thread did not answer in 5 s; "
                       "its window stays until the process ends");
        yscr__wt_release();
        s->wt = 0;
    } else
#endif
    yscr__window_close_cb(&c);
    if (s->wt_user) {
        s->wt_user = 0;
        if (--yscr__wt_mode_users == 0) yscr__wt_mode_now = 0;
    }
}

static void yscr__fg_cb(void* p) { yscr_screen* s = (yscr_screen*)p; s->fg = YSCR__FOREGROUND(s); }
static void yscr__devices_cb(void* p) { yscr__devices_at_open((yscr_screen*)p); }
#endif

YSCR_API bool yscr_open(yscr_screen* s, const yscr_desc* desc) {
    yscr_presenter_open in;
    yscr_mode want;
    int rc, hw_onset;
    int64_t t_open = yscr__now();
    if (!s) return false;
    if (s->open) { yscr__copy(s->error, sizeof s->error, "ysp_screen: already open"); return false; }
    memset(s, 0, sizeof *s);
    s->st_t0 = t_open;
    if (!desc) { yscr__copy(s->error, sizeof s->error, "ysp_screen: NULL desc"); return false; }
    {
        const char* e = yscr__settle_desc(s, desc);
        if (e) { yscr__copy(s->error, sizeof s->error, e); return false; }
    }
    if (desc->vrr) {
        yscr__copy(s->error, sizeof s->error, "ysp_screen: variable refresh is refused until the panel "
                     "has passed the photodiode interval sweep and the luminance-versus-interval sweep");
        return false;
    }
    if (!(desc->lead == 0 || desc->lead == YSCR_LEAD_NONE || (desc->lead > 0 && desc->lead < 1))) {
        yscr__copy(s->error, sizeof s->error, "ysp_screen: desc.lead must be 0, in (0, 1) or YSCR_LEAD_NONE");
        return false;
    }
    if (desc->depth < 0 || desc->depth > 8 || (desc->depth && desc->depth_learn)) {
        yscr__copy(s->error, sizeof s->error, "ysp_screen: desc.depth must be 0 (from the path) or 1 to 8, "
                     "and not set with desc.depth_learn");
        return false;
    }
    if (desc->min_tier < 0 || desc->min_tier > YSCR_TIER_3) {
        yscr__copy(s->error, sizeof s->error, "ysp_screen: desc.min_tier must be 0 (off) or 1 to 3");
        return false;
    }
    if (desc->patch.size < 0 || desc->patch.corner < 0 || desc->patch.corner > 3 ||
        desc->sim_period_ns < 0 || desc->window_w < 0 || desc->window_h < 0) {
        yscr__copy(s->error, sizeof s->error, "ysp_screen: a negative size, period or corner in desc");
        return false;
    }
    if (desc->icon_rgba && (desc->icon_w < 1 || desc->icon_w > 256 || desc->icon_h < 1 || desc->icon_h > 256)) {
        yscr__copy(s->error, sizeof s->error, "ysp_screen: desc.icon_w and icon_h must be 1 to 256 with desc.icon_rgba");
        return false;
    }
    if (desc->panic && (desc->panic_presses < 0 || desc->panic_window_ns < 0 || desc->panic_grace_ns < 0)) {
        yscr__copy(s->error, sizeof s->error, "ysp_screen: a negative desc.panic_presses, panic_window_ns or panic_grace_ns");
        return false;
    }
    if (desc->key_dedup_ns > 1000000000) {
        yscr__copy(s->error, sizeof s->error, "ysp_screen: desc.key_dedup_ns must be at most 1 s (< 0 turns the key filter off)");
        return false;
    }
    if (desc->window_thread < YSCR_WINDOW_THREAD_AUTO || desc->window_thread > YSCR_WINDOW_THREAD_OFF) {
        yscr__copy(s->error, sizeof s->error, "ysp_screen: desc.window_thread must be 0 (AUTO), 1 (ON) or 2 (OFF)");
        return false;
    }
    if (desc->panic && desc->abort_keys.off) {
        yscr__copy(s->error, sizeof s->error, "ysp_screen: desc.panic needs the abort combination (desc.abort_keys.off is set)");
        return false;
    }
    if (desc->panic && !yscr__vk_known(desc->abort_keys.key ? desc->abort_keys.key : YSCR_KEY_ESCAPE)) {
        yscr__copy(s->error, sizeof s->error, "ysp_screen: desc.panic takes Esc, F1 to F12, a letter or a digit "
                     "as desc.abort_keys.key");
        return false;
    }
#if !defined(YSCR__WEB)
    (void)&yscr__web_shown; (void)&yscr__web_steady;   /* the core test calls them */
#endif
    s->backend = desc->backend;
    s->auto_pick = YSCR_AUTO_NAMED;
    if (s->backend == YSCR_BACKEND_AUTO) {
#if defined(YSCR__DXGI)
        /* COMPOSITION, decided 2026-10-09 on the 10-minute runs (BACKENDS,
         * docs/screen.md "AUTO"); DXGI_FLIP where it does not open */
        s->backend = YSCR_BACKEND_COMPOSITION;
        s->auto_pick = YSCR_AUTO_COMPOSITION;
#elif defined(YSCR__WEB)
        s->backend = YSCR_BACKEND_WEB;
#else
        yscr__copy(s->error, sizeof s->error, "ysp_screen: no display backend on this platform "
                     "(Linux X11, Wayland and macOS are stubs); BACKEND_SIM runs without a display");
        return false;
#endif
    }
    /* VIRTUAL CLOCK: a display's flips happen on real time, so a frame
     * loop on virtual time would predict and record nonsense */
    if (yrt_is_virtual() && s->backend != YSCR_BACKEND_SIM && s->backend != YSCR_BACKEND_CUSTOM) {
        yscr__copy(s->error, sizeof s->error, "ysp_screen: the clock is virtual (yrt_virtual_begin); only "
                     "BACKEND_SIM and BACKEND_CUSTOM run on it");
        return false;
    }
    switch (s->backend) {
    case YSCR_BACKEND_SIM:
        s->pr = &yscr__sim_presenter;
        s->pr_ctx = s->backend_mem;
        break;
#if defined(YSCR__DXGI)
    case YSCR_BACKEND_DXGI_FLIP:
        s->pr = &yscr__dxgi_presenter;
        s->pr_ctx = s->backend_mem;
        break;
    case YSCR_BACKEND_COMPOSITION:
        s->pr = &yscr__comp_presenter;
        s->pr_ctx = s->backend_mem;
        break;
#endif
#if defined(YSCR__WEB)
    case YSCR_BACKEND_WEB:
        s->pr = &yscr__web_presenter;
        s->pr_ctx = s->backend_mem;
        break;
#endif
    case YSCR_BACKEND_CUSTOM:
        if (!desc->presenter || desc->presenter->version != YSCR_PRESENTER_VERSION ||
            !desc->presenter->open || !desc->presenter->acquire || !desc->presenter->present ||
            !desc->presenter->completions) {
            yscr__copy(s->error, sizeof s->error, "ysp_screen: desc.presenter missing, of another version, "
                         "or without open, acquire, present or completions");
            return false;
        }
        s->pr = desc->presenter;
        s->pr_ctx = desc->presenter_ctx;
        break;
    default:
        yscr__set_error(s->error, sizeof s->error, "ysp_screen: backend %d is not implemented on this platform "
                          "(DXGI_FLIP and COMPOSITION on Windows, WEB under Emscripten, SIM and CUSTOM are)", (int)s->backend);
        return false;
    }

    {
        const char* e = yscr__abort_open(s, desc);
        if (e) {
            yscr__set_error(s->error, sizeof s->error, "ysp_screen: %s", e);
            s->pr = NULL;
            return false;
        }
    }
    memset(&want, 0, sizeof want);
    memset(&in, 0, sizeof in);
    in.d3d11_video = desc->d3d11_video ? 1 : 0;
    in.display = desc->display;
    in.angle_dir = desc->angle_dir;
    in.sim_period_ns = desc->sim_period_ns;
    in.mode = &want;
    in.n_codes = desc->n_codes;
    in.want_gpu_done = desc->trigger_fence ? 1 : 0;
    in.code_checked = &s->code_checked;
    in.code_failed = &s->code_failed;
    in.code_selftest = &s->code_selftest;
    s->caps.backend = s->backend;

#if !defined(YSCR_NO_SDL)
    if (s->pr->needs_window) {
        const char* e = yscr__wt_open(s, desc, &want, &in);
        if (e) {
            if (!s->error[0]) yscr__set_error(s->error, sizeof s->error, "ysp_screen: %s", e);
            yscr_close(s);
            return false;
        }
    }
#endif
    if (!s->abort_slot) yscr__slot_take(s, 0);

#if defined(YSCR__DXGI)
    yscr__comp_auto = s->auto_pick == YSCR_AUTO_COMPOSITION;
#endif
    rc = s->pr->open(s->pr_ctx, &in, &s->caps, s->error, sizeof s->error);
#if defined(YSCR__DXGI)
    yscr__comp_auto = 0;
    if (rc == YSCR_ERR_NOT_IMPLEMENTED && s->auto_pick == YSCR_AUTO_COMPOSITION) {
        /* not Windows 11 x64, or no support on this GPU or output */
        const char* why = strncmp(s->error, "ysp_screen: ", 12) ? s->error : s->error + 12;
        yscr__copy(s->auto_why, sizeof s->auto_why, why);
        s->error[0] = 0;
        s->backend = YSCR_BACKEND_DXGI_FLIP;
        s->auto_pick = YSCR_AUTO_FALLBACK;
        s->pr = &yscr__dxgi_presenter;
        memset(s->backend_mem, 0, sizeof s->backend_mem);
        rc = s->pr->open(s->pr_ctx, &in, &s->caps, s->error, sizeof s->error);
    }
#endif
    s->pr_open = rc >= 0;
    if (rc < 0) {
        if (!s->error[0]) yscr__set_error(s->error, sizeof s->error, "ysp_screen: %s open: %s", s->pr->name, yscr_strerror(rc));
        s->pr = NULL;
        yscr_close(s);
        return false;
    }
    s->caps.backend = s->backend;
    hw_onset = s->caps.hw_onset;
#if !defined(YSCR_NO_SDL)
    yscr__win_call(s, yscr__fg_cb, s);
#else
    s->fg = YSCR__FOREGROUND(s);
#endif
#if defined(YSCR__DXGI)
    if (s->backend == YSCR_BACKEND_COMPOSITION) yscr__wake_start(s);
#endif
    if (s->caps.mode.period_ns == 0) s->caps.mode = want;
    if (s->caps.period_ns == 0) s->caps.period_ns = s->caps.mode.period_ns;
    if (s->caps.period_ns <= 0) s->caps.period_ns = 16666667;
    if (s->caps.kind == 0) s->caps.kind = YSCR_FIXED_GRID;
    s->open = 1;
    s->ring = desc->ring;
    s->display_index = desc->display_index;
#if !defined(YSCR_NO_SDL)
    if (s->window) yscr__win_call(s, yscr__devices_cb, s);
#endif
    if (desc->raw_mice) {
        const char* why = yscr__rm_open(s);
        if (why) {
            yscr__set_error(s->error, sizeof s->error, "ysp_screen: %s", why);
            yscr_close(s);
            return false;
        }
    }
    s->lead = desc->lead == 0 ? 0.5 : desc->lead;
    s->offset = desc->onset_offset_ns;
    s->key_dedup_ns = desc->key_dedup_ns;
    s->patch = desc->patch;
    s->nominal_f = (double)s->caps.period_ns;
    s->period_f = s->nominal_f;
    s->depth = 1;
    s->show_depth = 1;
    s->depth_pin = desc->depth;
    s->depth_learn = desc->depth_learn;
    s->sync_index = -1;
    s->depth_need = 1;
    s->path = YSCR_PATH_UNKNOWN;
    s->min_tier = desc->min_tier;
    yscr__update_lead(s);
    yscr__gl_load(s);
    {
        static uint32_t generation;
        s->gl_generation = ++generation;
    }
    s->lock_ok = yscr__lock_init(s);
    s->armed_wake = INT64_MAX;
    {
        const char* e = yscr__codes_open(s, desc);
        if (!e && (desc->n_triggers < 0 || desc->n_triggers > YSCR_MAX_TRIGGERS || (desc->n_triggers && !desc->triggers)))
            e = "desc.n_triggers must be 0 to YSCR_MAX_TRIGGERS, with desc.triggers";
        if (!e && desc->n_triggers > 0) {
            int i;
            for (i = 0; i < desc->n_triggers; i++) s->trig[i] = desc->triggers[i];
            s->n_trig = desc->n_triggers;
            s->trig_fence = desc->trigger_fence ? 1 : 0;
            s->trig_cpu = desc->trigger_cpu > 0 ? desc->trigger_cpu : desc->trigger_cpu < 0 ? -1 : 0;
            s->trig_spin = desc->trigger_spin_ns;
            s->worker_on = YSCR__WORKER_START(s);
            if (!s->worker_on) e = "the trigger worker did not start (built with YRT_NO_THREADS?)";
            s->trig_on = s->worker_on;
        }
#if defined(YSCR__DXGI)
        if (!e && (s->backend == YSCR_BACKEND_DXGI_FLIP || s->backend == YSCR_BACKEND_COMPOSITION)) {
            yscr__win_display_state(s, desc);
            if (desc->codes_strict && s->n_codes &&
                (yscr_code_risk(s) & ~(uint16_t)YSCR_CODE_RISK_COMPOSED))
                e = "desc.codes_strict: the display may change code pixels (see the describe line)";
        }
        /* Armed after the gamma ramp is taken, so a panic finds it. A
         * window has no ramp or mode to put back, and its user has the
         * desktop. */
        if (!e && desc->panic && (s->backend == YSCR_BACKEND_DXGI_FLIP || s->backend == YSCR_BACKEND_COMPOSITION)) {
            if (!desc->windowed || YSCR__PANIC_WINDOWED) e = yscr__wd_arm(s, desc);
            else s->panic_armed = 2;
        }
#endif
        if (!e && desc->panic && !s->panic_armed) s->panic_armed = 3;
        if (e) {
            yscr__set_error(s->error, sizeof s->error, "ysp_screen: %s", e);
            yscr_close(s);
            return false;
        }
    }
    {
        yrt_event ev;
        memset(&ev, 0, sizeof ev);
        ev.source = (uint16_t)YRT_SRC_SCREEN;
        ev.kind = (uint16_t)YSCR_EV_MODE;
        ev.aux = s->display_index;
        ev.u.i32[0] = s->caps.mode.w;
        ev.u.i32[1] = s->caps.mode.h;
        ev.u.i32[2] = s->caps.mode.refresh_num;
        ev.u.i32[3] = s->caps.mode.refresh_den;
        ev.u.i32[4] = (int32_t)s->backend;
        ev.u.i32[5] = s->auto_pick;
        yscr__push(s, &ev);
    }
    yscr__context_hook(s, desc);
    yscr__warmup(s);
    /* Settling (SETTLE): skipped where it cannot run or was turned off,
     * and recorded either way. */
#if defined(YSCR__WEB)
    if (s->backend == YSCR_BACKEND_WEB && s->st_mode != YSCR_SETTLE_OFF) {
        /* the presenter's own rule, on the frame callbacks at open: a
         * browser gives nothing to wait for, so it warns, never fails */
        const yscr__web* w = (const yscr__web*)(const void*)s->backend_mem;
        s->st_result = w->settled ? YSCR_SETTLED : YSCR_SETTLE_FAILED;
        s->st_mode = YSCR_SETTLE_WARN;
        s->st_src = YSCR_SETTLE_SRC_BACKEND;
        s->st_cond = w->settled ? YSCR_SETTLE_C_NONE : YSCR_SETTLE_C_SPREAD;
        s->st_sd = w->sd_ns;
        s->st_mean = 0;
        s->st_ns = w->settle_ns;
        s->st_flips = w->settle_frames;
        s->st_run = w->settled ? YSCR__WEB_N : 0;
        s->st_need = YSCR__WEB_N;
    } else
#endif
    if (s->backend == YSCR_BACKEND_SIM || !hw_onset) {
        s->st_result = YSCR_SETTLE_SKIPPED;
        s->st_src = YSCR_SETTLE_SRC_BACKEND;
    } else if (s->st_mode == YSCR_SETTLE_OFF) {
        s->st_result = YSCR_SETTLE_SKIPPED;
    } else if ((rc = yscr__settle(s)) < 0) {
        yscr__set_error(s->error, sizeof s->error, "ysp_screen: %s present during settling: %s", s->pr->name,
                        yscr_strerror(rc));
        yscr_close(s);
        return false;
    }
    if (s->st_result == YSCR_SETTLE_SKIPPED) {
        s->st_ns = yscr__now() - s->st_t0;
        s->st_flips = (int32_t)s->next_id;
    }
    yscr__settle_push(s);
    if (s->st_result == YSCR_SETTLE_FAILED && s->st_mode == YSCR_SETTLE_STRICT) {
        char m[224];
        yscr__settle_message(s, m, sizeof m);
        yscr__set_error(s->error, sizeof s->error, "ysp_screen: %s. YSP_SETTLE=warn opens anyway.", m);
        yscr_close(s);
        return false;
    }
    return true;
}

YSCR_API void yscr_close(yscr_screen* s) {
    if (!s) return;
    if (s->raw_mice) {
#if defined(YSCR__DXGI)
        yscr__rm_stop();
#endif
        s->raw_mice = 0;
    }
    if (s->pr && s->pr_open && s->open) {
        int64_t end = yscr__now() + 100000000;
        yscr_vblank newest;
        /* Let the last flip complete, so its record reaches the ring. */
        while (yscr__pending(s) > 0 && yscr__now() < end) {
            if (!s->slot_held && s->pr->acquire(s->pr_ctx, end, &newest) == YSCR_OK) s->slot_held = 1;
            yscr__drain(s);
            if (yscr__pending(s) == 0) break;
            YSCR__SLEEP_UNTIL((int64_t)(yscr__now() + 500000), 0);
        }
        {
            int i;
            for (i = 0; i < YSCR__MAX_PEND; i++) if (s->pend[i].used) yscr__estimate(s, &s->pend[i]);
        }
    }
    if (s->worker_on) {
        YSCR__WORKER_STOP(s);
        yscr__trig_run(s, yscr__now(), 1);   /* anything still armed */
        s->worker_on = 0;
    }
    if (s->trig_on) {
        int i;
        for (i = 0; i < YSCR_MAX_JOBS; i++) s->job[i].flip_done = 1;
        yscr__trig_emit(s);
        s->trig_on = 0;
    }
    if (s->lock_ok) { yscr__lock_free(s); s->lock_ok = 0; }
#if defined(YSCR__DXGI)
    yscr__wd_disarm(s);
    yscr__win_gamma_release(s);
    yscr__wake_stop(s);
    /* The next screen's raise (v0.10.2): Windows let the thread that had the
     * foreground window raise the next one, and with the window thread the
     * next open's window belongs to a new thread, so its raise was refused
     * and the window opened behind (runs 2 and 3 of 3 in a window: every
     * record OCCLUDED, tier 3). While this window has the foreground, the
     * process keeps the right for its next window; as before, input
     * the user gives another program takes it away. */
    if (s->hwnd && yscr__win.foreground && yscr__win.allow_foreground && yscr__win.foreground() == (HWND)s->hwnd)
        yscr__win.allow_foreground(GetCurrentProcessId());
    /* A forced raise took the foreground from another window: give it back
     * while this one still has it (measured: else the system picked a
     * third program in 9 of 9 forced opens). */
    if (s->fg == YSCR_FG_FORCED && s->fg_prev && s->hwnd && yscr__win.foreground && yscr__win.is_window &&
        yscr__win.set_foreground && yscr__win.foreground() == (HWND)s->hwnd && yscr__win.is_window((HWND)s->fg_prev))
        yscr__win.set_foreground((HWND)s->fg_prev);
#endif
    s->fg = 0;
    s->fg_prev = NULL;
    s->panic_armed = 0;
#if !defined(YSCR_NO_SDL)
    if (s->abort_slot && yscr__slot[s->abort_slot - 1].win && --yscr__abort_windows == 0)
        SDL_RemoveEventWatch(yscr__abort_watch, NULL);
#endif
    yscr__slot_free(s);
    yscr__abort_close(s);
    if (s->pr && s->pr_open && s->pr->close) s->pr->close(s->pr_ctx);
    s->pr = NULL;
    s->pr_open = 0;
#if !defined(YSCR_NO_SDL)
    yscr__wt_close(s);
    if (yscr__tls_dirty && YSCR__TLS_CLEANUP) {
        yscr__tls_dirty = 0;
        SDL_CleanupTLS();
    }
#endif
    s->window = NULL;
    s->sdl_video = 0;
    s->cursor_hidden = 0;
    s->open = 0;
    s->begun = 0;
}

YSCR_API const char* yscr_error(const yscr_screen* s) { return s ? s->error : "ysp_screen: NULL screen"; }
YSCR_API bool yscr_is_open(const yscr_screen* s) { return s && s->open; }

/* Whether keys come on SDL's raw path now: the hint can change at any
 * time, and text input routes keys through the message path (INPUT). */
static bool yscr__raw_keyboard(const yscr_screen* s) {
#if !defined(YSCR_NO_SDL) && defined(_WIN32)
    return s->window && !YSCR__TEXT_INPUT_ACTIVE(s) && SDL_GetHintBoolean(SDL_HINT_WINDOWS_RAW_KEYBOARD, false);
#else
    (void)s;
    return false;
#endif
}

YSCR_API void yscr_get_caps(const yscr_screen* s, yscr_caps* out) {
    if (!out) return;
    if (!s || !s->open) { memset(out, 0, sizeof *out); return; }
    *out = s->caps;
    out->worst_tier = (yscr_tier)s->worst_tier;
    out->raw_keyboard = yscr__raw_keyboard(s);
}

static const char* yscr__path_name(uint16_t p) {
    switch (p) {
    case YSCR_PATH_COMPOSED: return "composed";
    case YSCR_PATH_OVERLAY: return "overlay";
    case YSCR_PATH_INDEPENDENT: return "independent";
    case YSCR_PATH_SIMULATED: return "simulated";
    case YSCR_PATH_BROWSER: return "browser";
    default: return "unknown";
    }
}

YSCR_API void yscr_sync_check(const yscr_screen* s, yscr_sync_info* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->fired_index = -1;
    if (!s || !s->open) return;
    out->untimed = s->sync_off;
    out->rule = s->sync_rule;
    out->fired_index = s->sync_index;
    out->window = s->sync_win;
    out->evidence = yscr__popcount64(s->sync_hist);
    out->peak = s->sync_peak;
    out->observed = s->sync_obs;
    out->same_refresh = s->sync_same;
    out->no_wait = s->sync_nowait;
    out->torn = s->sync_torn;
    out->block_presents = s->sync_p;
    out->block_refreshes = s->sync_r;
    if (!s->sync_off) return;
    {
        char why[96];
        if (s->sync_rule == YSCR_SYNC_RULE_RATE)
            snprintf(why, sizeof why, "%u presents completed in %u refreshes", (unsigned)s->sync_fire[1],
                     (unsigned)s->sync_fire[2]);
        else
            snprintf(why, sizeof why, "%u of 64 flips came two in one refresh, without a vblank wait, or torn",
                     (unsigned)s->sync_fire[0]);
        snprintf(out->message, sizeof out->message,
                 "ysp_screen: flips are not synced to the vblank (%s), so from frame %lld the screen is untimed "
                 "(tier 3, YSCR_FLIP_UNSYNCED). A driver setting may force vsync off: AMD \"Wait for Vertical "
                 "Refresh: Always off\", NVIDIA \"Vertical sync: Off\", Intel \"Vertical Sync: Speed\", Mesa "
                 "vblank_mode=0. Set it to the application's choice.",
                 why, (long long)s->sync_index);
    }
}

/* The describe line's window_thread token (WINDOW THREAD). */
static const char* yscr__wt_name(const yscr_screen* s) {
    if (s->wt_mode == YSCR_WINDOW_THREAD_ON) return s->wt_why == YSCR__WT_WHY_NAMED ? "on" : "on(auto)";
    if (s->wt_mode != YSCR_WINDOW_THREAD_OFF) return "n/a";
    switch (s->wt_why) {
    case YSCR__WT_WHY_NAMED: return "off";
    case YSCR__WT_WHY_BACKEND: return "off(auto,backend)";
    case YSCR__WT_WHY_PLATFORM: return "off(auto,platform)";
    case YSCR__WT_WHY_PROCESS: return "off(auto,other-screen)";
    case YSCR__WT_WHY_SDL: return "off(auto,sdl-video-up)";
    default: return "off(auto)";
    }
}

static const char* yscr__fg_name(uint32_t fg) {
    return fg == YSCR_FG_GRANTED ? "granted" : fg == YSCR_FG_FORCED ? "forced" : fg == YSCR_FG_REFUSED ? "refused" : "n/a";
}

YSCR_API int yscr_describe(const yscr_screen* s, char* buf, size_t cap) {
    char extra[448], abort_s[40], settle_s[96];
    double hz, ppm;
    if (!s || !buf || cap == 0) return YSCR_ERR_ARG;
    if (!s->open) return snprintf(buf, cap, "ysp_screen: closed");
    yscr__settle_token(s, settle_s, sizeof settle_s);
    extra[0] = '\0';
    if (s->pr->describe) s->pr->describe(s->pr_ctx, extra, sizeof extra);
    hz = s->period_f > 0 ? 1e9 / s->period_f : 0;
    ppm = (s->period_f / s->nominal_f - 1.0) * 1e6;
    if (s->n_codes > 0 || s->n_trig > 0 || s->os_color[0]) {
        size_t n = strlen(extra);
        uint16_t risk = yscr_code_risk(s);
        if (s->os_color[0]) n += (size_t)snprintf(extra + n, n < sizeof extra ? sizeof extra - n : 0, " %s", s->os_color);
        if (s->n_codes > 0 && n < sizeof extra)
            n += (size_t)snprintf(extra + n, sizeof extra - n, " codes=%d selftest=%s%s", s->n_codes,
                                  s->code_selftest > 0 ? "pass" : s->code_selftest < 0 ? "FAIL" : "not-run",
                                  risk ? " WARNING=codes-at-risk" : "");
        if (s->n_trig > 0 && n < sizeof extra) {
            char cpus[32] = "";
            /* chosen before the call: a directive inside macro arguments is undefined (clang) */
#if defined(YSCR__REAL_WORKER)
            const char* wpol = yrt_policy_name(yrt_worker_policy(&s->worker));
#else
            const char* wpol = "test";
#endif
            if (s->trig_cores) snprintf(cpus, sizeof cpus, ":cpus=0x%llx", (unsigned long long)s->trig_mask);
            snprintf(extra + n, sizeof extra - n, " triggers=%d worker=%s%s%s fence=%s", s->n_trig, wpol,
                     s->trig_cores == 'E' ? "/E-cores" : s->trig_cores == 'N' ? "/pinned" : "/ysp_rt",
                     cpus, !s->trig_fence ? "off" : (s->pr->gpu_done ? "on" : "unavailable"));
        }
    }
    if (s->gap_anchors || s->gap_refused) {   /* THE GRID: gaps the system's vblank closed */
        size_t n = strlen(extra);
        if (n < sizeof extra)
            snprintf(extra + n, sizeof extra - n, " gap_anchors=%u(max %.0fus, refused %u)", (unsigned)s->gap_anchors,
                     (double)s->gap_shift_max / 1e3, (unsigned)s->gap_refused);
    }
    {   /* the abort combination, as an operator reads it */
        uint32_t k = yscr__ab.key;
        size_t n = 0;
        if (yscr__ab.off) n = (size_t)snprintf(abort_s, sizeof abort_s, "off");
        else {
            n += (size_t)snprintf(abort_s + n, sizeof abort_s - n, "%s%s%s%s",
                                  (yscr__ab.mods & YSCR_MOD_CTRL) ? "ctrl+" : "", (yscr__ab.mods & YSCR_MOD_ALT) ? "alt+" : "",
                                  (yscr__ab.mods & YSCR_MOD_SHIFT) ? "shift+" : "", (yscr__ab.mods & YSCR_MOD_GUI) ? "gui+" : "");
            if (k == YSCR_KEY_ESCAPE) snprintf(abort_s + n, sizeof abort_s - n, "esc");
            else if (k >= 0x4000003Au && k <= 0x40000045u) snprintf(abort_s + n, sizeof abort_s - n, "f%u", (unsigned)(k - 0x4000003Au + 1));
            else if (k > 32 && k < 127) snprintf(abort_s + n, sizeof abort_s - n, "%c", (char)k);
            else snprintf(abort_s + n, sizeof abort_s - n, "key0x%x", (unsigned)k);
        }
    }
    return snprintf(buf, cap, "ysp_screen %s: backend=%s%s%s%s %s%s mode=%dx%d@%d/%d measured=%.4fHz(%+.0fppm) "
                    "path=%s depth=%d(%s,%u changes) settle=%s foreground=%s%s window_thread=%s lead=%.2f worst_tier=%d abort=%s "
                    "panic=%s%s%s%s%s%s%s",
                    YSCR_VERSION_STRING, s->pr->name,
                    s->auto_pick == YSCR_AUTO_COMPOSITION ? "(auto)"
                        : s->auto_pick == YSCR_AUTO_FALLBACK ? "(auto,composition-refused:\"" : "",
                    s->auto_pick == YSCR_AUTO_FALLBACK ? s->auto_why : "",
                    s->auto_pick == YSCR_AUTO_FALLBACK ? "\")" : "", extra,
                    yrt_is_virtual() ? " clock=virtual" : "", s->caps.mode.w, s->caps.mode.h,
                    s->caps.mode.refresh_num, s->caps.mode.refresh_den, hz, ppm,
                    yscr__path_name(s->path), s->depth,
                    s->depth_pin ? "pin" : s->depth_learn ? "learner" : "path", (unsigned)s->depth_changes,
                    settle_s, yscr__fg_name(s->fg),
                    s->backend != YSCR_BACKEND_COMPOSITION ? "" : s->wake ? " idle_wake=on" : " idle_wake=off",
                    yscr__wt_name(s), s->lead < 0 ? -1.0 : s->lead, s->worst_tier, abort_s,
                    s->panic_armed == 1 ? "armed" : s->panic_armed == 2 ? "idle(windowed)" : s->panic_armed == 3 ? "n/a" : "off",
                    s->sync_off ? " WARNING=not-vsynced(driver-setting?)" : "",
                    /* a browser's callback times jitter about the grid, and its
                     * "mode" is the period open() measured (BACKENDS, WEB) */
                    s->unstable && s->backend != YSCR_BACKEND_WEB ? " WARNING=off-grid-vblanks" : "",
                    (ppm > 200 || ppm < -200) && s->backend != YSCR_BACKEND_WEB ? " WARNING=period-differs-from-mode" : "",
                    s->st_result == YSCR_SETTLE_FAILED ? " WARNING=not-settled" : "",
                    s->fg == YSCR_FG_REFUSED ? " WARNING=not-foreground" : "",
                    yscr__released_on(s) ? " WARNING=panic-released" : "");
}

YSCR_API void yscr_settle_check(const yscr_screen* s, yscr_settle_info* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!s || !s->open) return;
    out->result = s->st_result;
    out->mode = s->st_mode;
    out->source = s->st_src;
    out->condition = s->st_cond;
    out->ns = s->st_ns;
    out->flips = s->st_flips;
    out->run = s->st_run;
    out->need = s->st_need;
    out->foreground = s->fg;
    out->min_ns = s->st_min;
    out->max_ns = s->st_max;
    out->spread_sd_ns = s->st_sd;
    out->spread_mean_ns = s->st_mean;
    out->hook_ns = s->hook_ns;
    yscr__settle_message(s, out->message, sizeof out->message);
}

static void yscr__pump(yscr_screen* s) {
#if !defined(YSCR_NO_SDL)
    if (yscr__tls_dirty && YSCR__TLS_CLEANUP) {
        yscr__tls_dirty = 0;
        SDL_CleanupTLS();
    }
    /* The window thread pumps: SDL_PumpEvents belongs to SDL's main thread
     * (SDL_events.c asserts it), which that thread is. */
    if (s->window && !s->polled && !s->wt) {   /* yscr_poll() pumped already this frame */
        YRT_ZONE(z_pump, "yscr.pump");
        SDL_PumpEvents();
        YRT_ZONE_END(z_pump);
    }
#endif
    s->polled = 0;
}

static int yscr__begin(yscr_screen* s, yscr_frame* f) {
    int64_t t0, now, wait, prev_pred, c0;
    int rc;
    YRT_ZONE(z_begin, "yscr.begin");
    if (!s || !f) { YRT_ZONE_END(z_begin); return YSCR_ERR_ARG; }
    if (!s->open) { YRT_ZONE_END(z_begin); return YSCR_ERR_CLOSED; }
    if (s->begun) { YRT_ZONE_END(z_begin); return YSCR_ERR_ORDER; }
    yscr__pump(s);
#if defined(YSCR__WEB)
    if (s->backend == YSCR_BACKEND_WEB) {
        double ms;
        int vis;
        while ((vis = yscr__web_vis_take(&ms)) >= 0) {
            yrt_event ev;
            memset(&ev, 0, sizeof ev);
            ev.source = (uint16_t)YRT_SRC_SCREEN;
            ev.kind = (uint16_t)YSCR_EV_VISIBILITY;
            ev.t_ns = (uint64_t)yscr__web_to_rt((const yscr__web*)(const void*)s->backend_mem, ms);
            ev.aux = s->display_index;
            ev.u.u32[0] = (uint32_t)vis;
            yscr__push(s, &ev);
        }
    }
#endif
    if (s->raw_mice) yscr__rm_check(s);
    yscr__in_log(s);
    if (yscr__abort_take(s, f)) { YRT_ZONE_END(z_begin); return YSCR_QUIT; }
    if (yscr__released_on(s)) {   /* only close() is left */
        memset(f, 0, sizeof *f);
        f->abort = YSCR_ABORT_PANIC;
        YRT_ZONE_END(z_begin);
        return YSCR_QUIT;
    }
    t0 = yscr__now();
    c0 = YSCR__COST_NOW();
    if (!s->slot_held) {
        yscr_vblank newest;
        YRT_ZONE(z_wait, "yscr.wait");
        newest.t_ns = 0;
        rc = s->pr->acquire(s->pr_ctx, t0 + (int64_t)(8 * s->period_f) + 200000000, &newest);
        YRT_ZONE_END(z_wait);
        if (rc < 0) { YRT_ZONE_END(z_begin); return rc; }
        if (newest.t_ns) {   /* on the grid only, in its numbers (yscr__complete) */
            int64_t nc;
            if (!s->have_anchor) yscr__anchor(s, newest.t_ns, newest.count, 0);
            else if (yscr__on_grid(s, newest.t_ns, &nc) && nc > s->vb_count) {
                s->count_bias = nc - newest.count;
                yscr__anchor(s, newest.t_ns, nc, 0);
            }
        }
    } else if (s->pr->bind) {
        s->pr->bind(s->pr_ctx);
    }
    s->slot_held = 0;
    wait = YSCR__COST_NOW() - c0;
    {
        YRT_ZONE(z_stats, "yscr.stats");
        yscr__drain(s);
        YRT_ZONE_END(z_stats);
    }
    if (yscr__pending(s) >= 3) {   /* statistics stopped coming */
        int i;
        for (i = 0; i < YSCR__MAX_PEND; i++) if (s->pend[i].used) yscr__estimate(s, &s->pend[i]);
    }
    yscr__trig_emit(s);
#if !defined(YSCR_NO_SDL)
    if (s->window && t0 - s->sdl_corr_t > 10000000000LL) yscr__restamp_correlate(s);
#endif
    now = yscr__now();
    yscr__gap_anchor(s, now);
    prev_pred = s->pred_count;
    s->pred_count = yscr__count_at(s, now + s->margin_ns) + s->depth;
    if (s->pred_count <= s->prev_shown) s->pred_count = s->prev_shown + 1;
    /* Never the vblank the last frame was planned for, even when that
     * frame is done: one shown a vblank early (EARLY, when the composed
     * path's depth falls) has left its planned vblank, and the next frame
     * planned there got the same onset (docs/screen.md). In a browser a
     * frame planned late shows at the next frame all the same, so that
     * rule planned every later frame a vblank ahead of where it showed
     * (measured, headless Chrome); there the onsets rise from begin to
     * begin instead. */
    if (s->backend == YSCR_BACKEND_WEB) {
        if (s->pred_count <= prev_pred) s->pred_count = prev_pred + 1;
    } else if (s->pred_count <= s->last_planned) {
        s->pred_count = s->last_planned + 1;
    }
    {   /* one frame per vblank, as flip_at() plans it */
        int i;
        for (i = 0; i < YSCR__MAX_PEND; i++)
            if (s->pend[i].used && s->pend[i].planned_count >= s->pred_count) s->pred_count = s->pend[i].planned_count + 1;
    }
    f->onset = yscr__time_of(s, s->pred_count) + s->offset;
    f->period = (int64_t)llround(s->period_f);
    f->index = s->index;
    f->vblank = s->pred_count;
    f->last = s->have_last ? &s->last : NULL;
    memcpy(s->fin_out, s->fin, sizeof(yscr_record) * (size_t)s->n_fin);
    s->n_fin_out = s->n_fin;
    s->n_fin = 0;
    f->done = s->fin_out;
    f->n_done = s->n_fin_out;
    f->done_lost = s->fin_lost;
    memset(s->acc, 0, sizeof s->acc);
    s->acc[YSCR_PHASE_SWAP] = yscr__sat32(wait);
    s->acc[YSCR_PHASE_GPU] = YSCR_PHASE_UNKNOWN;
    s->mark_t = YSCR__COST_NOW();
    s->begun = 1;
    YRT_ZONE_END(z_begin);
    return YSCR_OK;
}

YSCR_API void yscr_mark(yscr_screen* s, int phase) {
    int64_t now;
    if (!s || !s->begun || phase < 0 || phase >= YSCR_PHASE_GPU) return;
    now = YSCR__COST_NOW();
    s->acc[phase] = yscr__sat32((int64_t)s->acc[phase] + (now - s->mark_t));
    s->mark_t = now;
}

static int yscr__flip_at(yscr_screen* s, int64_t t, yscr_record* out) {
    int64_t now, tg, count_t, earliest, planned, t_call, c_call;
    yscr__pend* p = NULL;
    yscr_present_req req;
    const yscr_code_draw* codes = NULL;
    uint32_t code_vals[YSCR_MAX_CODES];
    int i, rc;
    uint16_t flags = YSCR_FLIP_PENDING;
    YRT_ZONE(z_flip, "yscr.flip");
    if (!s) { YRT_ZONE_END(z_flip); return YSCR_ERR_ARG; }
    if (!s->open) { YRT_ZONE_END(z_flip); return YSCR_ERR_CLOSED; }
    if (!s->begun) { YRT_ZONE_END(z_flip); return YSCR_ERR_ORDER; }
    now = YSCR__COST_NOW();
    s->acc[YSCR_PHASE_DRAW] = yscr__sat32((int64_t)s->acc[YSCR_PHASE_DRAW] + (now - s->mark_t));
    now = yscr__now();
    if (s->on_present) {
        yscr_present_info pi;
        memset(&pi, 0, sizeof pi);
        pi.index = s->index;
        pi.onset = yscr__time_of(s, s->pred_count) + s->offset;
        pi.w = s->caps.mode.w;
        pi.h = s->caps.mode.h;
        pi.rows_top_down = s->backend == YSCR_BACKEND_DXGI_FLIP || s->backend == YSCR_BACKEND_COMPOSITION;
        s->on_present(s->on_present_ctx, &pi);
        s->gl_epoch++;
    }
    {
        YRT_ZONE(z_patch, "yscr.patch");
        if (s->n_codes > 0) codes = yscr__codes_build(s, code_vals);
        if (!s->pr->draws_patch) {
            yscr__draw_patch(s);
            if (codes) yscr__gl_codes(s, codes, s->n_codes);
        }
        YRT_ZONE_END(z_patch);
    }

    tg = t - s->offset;
    count_t = yscr__snap(s, tg);
    earliest = yscr__count_at(s, yscr__now() + s->margin_ns) + s->depth;
    if (earliest <= s->prev_shown) earliest = s->prev_shown + 1;
    if (earliest <= s->last_planned && s->backend != YSCR_BACKEND_WEB) earliest = s->last_planned + 1;
    for (i = 0; i < YSCR__MAX_PEND; i++)   /* one frame per vblank */
        if (s->pend[i].used && s->pend[i].planned_count >= earliest) earliest = s->pend[i].planned_count + 1;
    planned = count_t;
    if (count_t < earliest) { planned = earliest; flags |= YSCR_FLIP_LATE_TARGET; }

    /* Wait until the present can no longer show early: DXGI shows a frame
     * at the first vblank it can. That covers a later vblank asked for, and
     * also a call within the margin before a vblank: the margin planned the
     * flip after that vblank, and a present made before it showed a vblank
     * early (EARLY set; a preempted frame loop hit this). */
    if (!s->caps.native_target) {
        int64_t guard = (int64_t)(s->period_f / 8);
        int64_t wake;
        if (guard > 500000) guard = 500000;
        wake = yscr__time_of(s, planned - s->show_depth) + guard;
        if (wake > yscr__now()) {
            YRT_ZONE(z_hold, "yscr.hold");
            /* DXGI keeps only the newest flip's statistic, and it lags the
             * vblank by up to about 2 ms on a composed path, so a long wait
             * reads it once per vblank or the next present would hide it. */
            for (;;) {
                int64_t now2 = yscr__now();
                int64_t poll = yscr__time_of(s, yscr__count_at(s, now2)) + 2500000;
                if (poll <= now2) poll = yscr__time_of(s, yscr__count_at(s, now2) + 1) + 2500000;
                if (poll + 1000000 >= wake) break;
                YSCR__SLEEP_UNTIL((int64_t)poll, 0);
                yscr__drain_overdue(s);
            }
            YSCR__SLEEP_UNTIL((int64_t)wake, YRT_DEFAULT_SPIN_NS);
            YRT_ZONE_END(z_hold);
        }
    }

    for (i = 0; i < YSCR__MAX_PEND; i++) if (!s->pend[i].used) { p = &s->pend[i]; break; }
    if (!p) {   /* never with one frame in flight; keep the newest */
        yscr__pend* oldest = &s->pend[0];
        for (i = 1; i < YSCR__MAX_PEND; i++) if (s->pend[i].id < oldest->id) oldest = &s->pend[i];
        yscr__estimate(s, oldest);
        p = oldest;
    }
    memset(p, 0, sizeof *p);
    p->used = 1;
    p->id = ++s->next_id;
    p->planned_count = planned;
    s->last_planned = planned;
    p->rec.index = s->index;
    p->rec.target = t;
    p->rec.planned = yscr__time_of(s, planned) + s->offset;
    {   /* observed, not only commanded: a GUI library may have started
         * it. SDL_TextInputActive() measured 21 ns a call. */
        int ti = YSCR__TEXT_INPUT_ACTIVE(s);
        if (ti != s->text_input) yscr__text_input_set(s, ti, 1, 0, 0, 0, 0);
    }
    if (s->text_input) flags |= YSCR_FLIP_TEXT_INPUT;
    p->rec.flags = flags;
    /* A present without a target shows at the first vblank it can, held
     * or not, so every one measures the depth; one with a target only
     * when the target was the first vblank it could make. */
    p->asap = planned == earliest || !s->caps.native_target;
    p->held = planned > earliest;

    memset(&req, 0, sizeof req);
    req.present_id = p->id;
    req.target_count = planned;
    req.target_ns = yscr__time_of(s, planned);
    req.hold = 1;
    if (s->patch.on && s->pr->draws_patch) {
        int px, py, ps;
        yscr__patch_rect(s, &px, &py, &ps);
        req.patch_on = 1;
        req.patch_x = px; req.patch_y = py; req.patch_w = ps; req.patch_h = ps;
        req.patch_value = s->patch_value;
    }
    if (codes && s->pr->draws_patch) {
        req.codes = codes;
        req.n_codes = s->n_codes;
        req.verify = s->verify_every > 0 && (s->index % s->verify_every) == 0;
    }
    if (codes) memcpy(p->code_vals, code_vals, sizeof(uint32_t) * (size_t)s->n_codes);
    yscr__drain_overdue(s);
    c_call = YSCR__COST_NOW();
    t_call = yscr__now();
    p->t_call = t_call;
    p->count_at_present = yscr__count_at(s, t_call);
    {
        YRT_ZONE(z_present, "yscr.present");
        rc = s->pr->present(s->pr_ctx, &req);
        YRT_ZONE_END(z_present);
    }
    p->t_ret = yscr__now();
    s->acc[YSCR_PHASE_SWAP] = yscr__sat32((int64_t)s->acc[YSCR_PHASE_SWAP] + (YSCR__COST_NOW() - c_call));
    memcpy(p->rec.phase_ns, s->acc, sizeof s->acc);
    if (rc < 0) {
        p->used = 0;
        s->begun = 0;
        s->n_req = 0;
        s->index++;
        YRT_ZONE_END(z_flip);
        return rc;
    }
    if (s->n_codes > 0) yscr__codes_advance(s);
    if (s->n_req > 0) {
        YRT_ZONE(z_arm, "yscr.trigarm");
        yscr__trig_arm_flip(s, p);
        YRT_ZONE_END(z_arm);
    }
    if (out) *out = p->rec;
    s->begun = 0;
    s->index++;
    YRT_ZONE_END(z_flip);
    return YSCR_OK;
}

static int yscr__flip(yscr_screen* s) {
    if (!s) return YSCR_ERR_ARG;
    if (!s->open) return YSCR_ERR_CLOSED;
    if (!s->begun) return YSCR_ERR_ORDER;
    return yscr_flip_at(s, yscr__time_of(s, s->pred_count) + s->offset, NULL);
}

static int yscr__wait_flip(yscr_screen* s, yscr_record* out) {
    int64_t end;
    if (!s) return YSCR_ERR_ARG;
    if (!s->open) return YSCR_ERR_CLOSED;
    if (s->begun) return YSCR_ERR_ORDER;
    end = yscr__now() + (int64_t)(8 * s->period_f) + 200000000;
    while (yscr__pending(s) > 0) {
        if (!s->slot_held) {
            yscr_vblank newest;
            int rc;
            newest.t_ns = 0;
            rc = s->pr->acquire(s->pr_ctx, end, &newest);
            if (rc < 0) return rc;
            s->slot_held = 1;
        }
        yscr__drain(s);
        if (yscr__pending(s) == 0) break;
        if (yscr__now() >= end) {
            int i;
            for (i = 0; i < YSCR__MAX_PEND; i++) if (s->pend[i].used) yscr__estimate(s, &s->pend[i]);
            break;
        }
        YSCR__SLEEP_UNTIL((int64_t)(yscr__now() + 100000), 0);
    }
    if (out) {
        if (!s->have_last) return YSCR_ERR_ORDER;
        *out = s->last;
    }
    return YSCR_OK;
}

YSCR_API void yscr_set_patch(yscr_screen* s, float v) {
    if (!s) return;
    s->patch_value = v < 0 ? 0.0f : (v > 1 ? 1.0f : v);
}

/* --- codes, triggers and hooks: the API -------------------------------------- */

static int yscr__code_frames(yscr_screen* s, int slot, uint32_t rgb, int frames) {
    if (!s || !s->open || slot < 0 || slot >= s->n_codes || frames < YSCR_CODE_HOLD) return YSCR_ERR_ARG;
    if (s->code_slot[slot].kind == YSCR_CODE_ROW) {
        int k, off = s->code_off[slot];
        for (k = 0; k < s->code_slot[slot].w; k++) s->code_px[off + k] = rgb & 0xFFFFFFu;
    }
    s->code_val[slot] = rgb & 0xFFFFFFu;
    s->code_left[slot] = frames;
    return YSCR_OK;
}

static int yscr__code(yscr_screen* s, int slot, uint32_t rgb) {
    return yscr_code_frames(s, slot, rgb, 1);
}

static int yscr__code_row(yscr_screen* s, int slot, const uint32_t* px, int n, int frames) {
    int k, off;
    const yscr_code_slot* c;
    if (!s || !s->open || slot < 0 || slot >= s->n_codes || !px || n < 0 || frames < YSCR_CODE_HOLD) return YSCR_ERR_ARG;
    c = &s->code_slot[slot];
    if (c->kind != YSCR_CODE_ROW || n > c->w) return YSCR_ERR_ARG;
    off = s->code_off[slot];
    for (k = 0; k < c->w; k++) s->code_px[off + k] = (k < n ? px[k] : c->rest) & 0xFFFFFFu;
    s->code_left[slot] = frames;
    return YSCR_OK;
}

YSCR_API yscr_code_slot yscr_slot_pixel_mode(void) {
    yscr_code_slot c;
    memset(&c, 0, sizeof c);
    c.kind = YSCR_CODE_SOLID;
    c.w = 1; c.h = 1;
    return c;
}

YSCR_API uint32_t yscr_pixel_mode_bits(uint32_t ttl24) { return ttl24 & 0xFFFFFFu; }

YSCR_API yscr_code_slot yscr_slot_psync(void) {
    yscr_code_slot c;
    memset(&c, 0, sizeof c);
    c.kind = YSCR_CODE_ROW;
    c.x = 10; c.y = 0; c.w = 8; c.h = 1;
    return c;
}

YSCR_API void yscr_psync_pattern(uint32_t out[8], uint8_t counter) {
    /* red, green, blue, yellow, magenta, cyan, white, then the counter in
     * green: Psychtoolbox's PsychDataPixx sequence */
    static const uint32_t seq[7] = { 0x0000FFu, 0x00FF00u, 0xFF0000u, 0x00FFFFu, 0xFF00FFu, 0xFFFF00u, 0xFFFFFFu };
    int i;
    if (!out) return;
    for (i = 0; i < 7; i++) out[i] = seq[i];
    out[7] = (uint32_t)counter << 8;
}

YSCR_API uint16_t yscr_code_risk(const yscr_screen* s) {
    uint16_t r;
    if (!s || !s->open) return 0;
    r = s->code_risk;
    if (s->code_selftest <= 0) r |= YSCR_CODE_RISK_UNVERIFIED;
    if (s->code_failed) r |= YSCR_CODE_RISK_VERIFY_FAILED;
    return r;
}

YSCR_API void yscr_code_verify(const yscr_screen* s, uint32_t* checked, uint32_t* failed) {
    if (checked) *checked = s ? s->code_checked : 0;
    if (failed) *failed = s ? s->code_failed : 0;
}

static int yscr__trigger(yscr_screen* s, int channel, uint32_t code) {
    if (!s || !s->open || channel < 0 || channel >= s->n_trig) return YSCR_ERR_ARG;
    if (!s->begun) return YSCR_ERR_ORDER;
    if (s->n_req >= YSCR_MAX_JOBS) return YSCR_ERR_REFUSED;
    s->req_ch[s->n_req] = (uint16_t)channel;
    s->req_code[s->n_req] = code;
    s->n_req++;
    return YSCR_OK;
}

static int yscr__trigger_at(yscr_screen* s, int channel, uint32_t code, int64_t t) {
    yscr__job* j = NULL;
    int i;
    if (!s || !s->open || channel < 0 || channel >= s->n_trig) return YSCR_ERR_ARG;
    yscr__lock(s);
    for (i = 0; i < YSCR_MAX_JOBS; i++) if (s->job[i].state == YSCR__J_FREE) { j = &s->job[i]; break; }
    if (!j) { yscr__unlock(s); return YSCR_ERR_REFUSED; }
    memset(j, 0, sizeof *j);
    j->state = YSCR__J_ARMED;
    j->channel = (uint16_t)channel;
    j->code = code;
    j->frame = -1;
    j->flip_done = 1;   /* no flip to wait for */
    j->deadline = t + s->trig[channel].offset_ns;
    j->wake = j->deadline;
    yscr__trig_rearm(s);
    yscr__unlock(s);
    return YSCR_OK;
}

YSCR_API void yscr_on_flip(yscr_screen* s, yscr_flip_fn fn, void* ctx) {
    if (!s) return;
    s->on_flip = fn;
    s->on_flip_ctx = ctx;
}

YSCR_API void yscr_on_present(yscr_screen* s, yscr_present_fn fn, void* ctx) {
    if (!s) return;
    s->on_present = fn;
    s->on_present_ctx = ctx;
}

YSCR_API uint32_t yscr_gl_epoch(const yscr_screen* s) { return s ? s->gl_epoch : 0; }
YSCR_API uint32_t yscr_gl_generation(const yscr_screen* s) { return s && s->open ? s->gl_generation : 0; }

static int yscr__native(const yscr_screen* s, yscr_native_info* out) {
    if (!out) return YSCR_ERR_ARG;
    memset(out, 0, sizeof *out);
    if (!s || !s->open) return YSCR_ERR_CLOSED;
#if defined(YSCR__DXGI)
    if (s->backend == YSCR_BACKEND_DXGI_FLIP || s->backend == YSCR_BACKEND_COMPOSITION) {
        ID3D11Device* dev;
        IDXGIDevice* xd = NULL;
        IDXGIAdapter* ad = NULL;
        if (s->backend == YSCR_BACKEND_DXGI_FLIP) {
            const yscr__dxgi* d = (const yscr__dxgi*)(const void*)s->backend_mem;
            dev = d->dev;
            out->d3d11_context = d->dctx;
            out->egl_display = d->dpy;
        } else {
            const yscr__comp* c = (const yscr__comp*)(const void*)s->backend_mem;
            dev = c->dev;
            out->d3d11_context = c->dctx;
            out->egl_display = c->dpy;
        }
        out->d3d11_device = dev;
        if (dev) {   /* asked of the device, not of the desc */
            ID3D11Multithread* mt = NULL;
            out->video = (YSCR__CALL0(dev, GetCreationFlags) & (UINT)D3D11_CREATE_DEVICE_VIDEO_SUPPORT) ? 1 : 0;
            if (out->d3d11_context &&
                SUCCEEDED(YSCR__CALL((ID3D11DeviceContext*)out->d3d11_context, QueryInterface,
                                       YSCR__IID(yscr__IID_ID3D11Multithread), (void**)&mt)) && mt) {
                out->mt_protected = YSCR__CALL0(mt, GetMultithreadProtected) ? 1 : 0;
                YSCR__RELEASE(mt);
            }
        }
        /* asked of the device, so it is the adapter the device is on even
         * when open() found no adapter for the monitor */
        if (dev && SUCCEEDED(YSCR__CALL(dev, QueryInterface, YSCR__IID(yscr__IID_IDXGIDevice), (void**)&xd))) {
            if (SUCCEEDED(YSCR__CALL(xd, GetAdapter, &ad))) {
                DXGI_ADAPTER_DESC desc;
                if (SUCCEEDED(YSCR__CALL(ad, GetDesc, &desc))) {
                    out->luid_low = (uint32_t)desc.AdapterLuid.LowPart;
                    out->luid_high = (int32_t)desc.AdapterLuid.HighPart;
                }
                YSCR__RELEASE(ad);
            }
            YSCR__RELEASE(xd);
        }
        return YSCR_OK;
    }
#endif
    return YSCR_ERR_NOT_IMPLEMENTED;
}

/* A group's failure, in the screen it names (the first when none). */
static int yscr__group_fail(yscr_screen* const* s, int n, int i, int rc, const char* what) {
    if (s && i < n && s[i]) yscr__set_error(s[i]->error, sizeof s[i]->error, "ysp_screen: %s: screen %d of the group: %s",
                                            what, i, yscr_strerror(rc));
    return rc;
}

YSCR_API int yscr_begin_group(yscr_screen* const* s, int n, yscr_frame* f) {
    int i, rc;
    if (!s || !f || n < 1) return yscr__group_fail(s, n, 0, YSCR_ERR_ARG, "begin_group");
    for (i = 0; i < n; i++) {
        if (!s[i]) return yscr__group_fail(s, n, 0, YSCR_ERR_ARG, "begin_group");
        if (!s[i]->open) return yscr__group_fail(s, n, i, YSCR_ERR_CLOSED, "begin_group");
        if (s[i]->pr->waits_block) return yscr__group_fail(s, n, i, YSCR_ERR_NOT_IMPLEMENTED, "begin_group");
        if (s[i]->begun) return yscr__group_fail(s, n, i, YSCR_ERR_ORDER, "begin_group");
    }
    /* SDL's pump is the process's: once for the group, then every member
     * sees the same aborts before any of them begins a frame. */
    for (i = 0; i < n && !s[i]->window; i++) { }
    if (i < n) yscr__pump(s[i]);
    for (i = 0; i < n; i++) s[i]->polled = 1;
    for (i = 0; i < n && !yscr__abort_pending(s[i]) && !yscr__released_on(s[i]); i++) { }
    if (i < n) {
        for (i = 0; i < n; i++)
            if (!yscr__abort_take(s[i], &f[i])) {
                memset(&f[i], 0, sizeof f[i]);
                if (yscr__released_on(s[i])) f[i].abort = YSCR_ABORT_PANIC;
            }
        return YSCR_QUIT;
    }
    /* Each wait is on its own kernel object, so waiting in turn costs the
     * longest wait, not their sum. */
    for (i = 0; i < n; i++) {
        rc = yscr_begin(s[i], &f[i]);
        if (rc != YSCR_OK) return rc;
    }
    return YSCR_OK;
}

YSCR_API int yscr_flip_group_at(yscr_screen* const* s, int n, int64_t t) {
    int i, rc, first = YSCR_OK;
    if (!s || n < 1) return yscr__group_fail(s, n, 0, YSCR_ERR_ARG, "flip_group_at");
    for (i = 0; i < n; i++) {
        if (!s[i]) return yscr__group_fail(s, n, 0, YSCR_ERR_ARG, "flip_group_at");
        if (s[i]->pr && s[i]->pr->bind) s[i]->pr->bind(s[i]->pr_ctx);
        rc = yscr_flip_at(s[i], t, NULL);
        if (rc < 0 && first == YSCR_OK) first = rc;
    }
    return first;
}

YSCR_API yscr_proc yscr_gl_proc(const yscr_screen* s, const char* name) {
    if (!s || !s->open || !name || !s->pr->gl_proc) return NULL;
    return s->pr->gl_proc(s->pr_ctx, name);
}

YSCR_API struct SDL_Window* yscr_window(const yscr_screen* s) { return s ? s->window : NULL; }

YSCR_API void yscr_bind(yscr_screen* s) {
    if (s && s->open && s->pr->bind) s->pr->bind(s->pr_ctx);
}

#if !defined(YSCR_NO_SDL)
YSCR_API int64_t yscr_restamp(const yscr_screen* s, uint64_t sdl_ticks_ns) {
    if (!s || !s->sdl_width) return 0;
    return s->sdl_rt + ((int64_t)sdl_ticks_ns - s->sdl_ticks);
}

/* The input devices SDL lists at open (YSCR_EV_DEVICE). SDL's lists are
 * its allocations, made once here, not in the frame loop. */
static void yscr__devices_at_open(yscr_screen* s) {
    int i, n = 0;
    SDL_KeyboardID* kb = SDL_GetKeyboards(&n);
    for (i = 0; kb && i < n; i++)
        yscr__device_log(s, YSCR_DEV_KEYBOARD, YSCR_DEV_PRESENT, kb[i], SDL_GetKeyboardNameForID(kb[i]));
    SDL_free(kb);
    {
        SDL_MouseID* m = SDL_GetMice(&n);
        for (i = 0; m && i < n; i++)
            yscr__device_log(s, YSCR_DEV_MOUSE, YSCR_DEV_PRESENT, m[i], SDL_GetMouseNameForID(m[i]));
        SDL_free(m);
    }
    {
        SDL_TouchID* tch = SDL_GetTouchDevices(&n);
        for (i = 0; tch && i < n; i++)
            yscr__device_log(s, YSCR_DEV_TOUCH, YSCR_DEV_PRESENT, tch[i], SDL_GetTouchDeviceName(tch[i]));
        SDL_free(tch);
    }
    if (s->sdl_gamepad) {
        SDL_JoystickID* g = SDL_GetGamepads(&n);
        for (i = 0; g && i < n; i++)
            yscr__device_log(s, YSCR_DEV_GAMEPAD, YSCR_DEV_PRESENT, g[i], SDL_GetGamepadNameForID(g[i]));
        SDL_free(g);
    }
}

/* Hot-plug, and devices SDL does not list (pens, touch devices it sees
 * first in an event): a record the first time. Names are SDL's pointers. */
static void yscr__device_event(yscr_screen* s, const SDL_Event* ev) {
    switch (ev->type) {
    case SDL_EVENT_KEYBOARD_ADDED:
        yscr__device_log(s, YSCR_DEV_KEYBOARD, YSCR_DEV_ADDED, ev->kdevice.which,
                           SDL_GetKeyboardNameForID(ev->kdevice.which));
        break;
    case SDL_EVENT_KEYBOARD_REMOVED:
        yscr__device_log(s, YSCR_DEV_KEYBOARD, YSCR_DEV_REMOVED, ev->kdevice.which, NULL);
        break;
    case SDL_EVENT_MOUSE_ADDED:
        yscr__device_log(s, YSCR_DEV_MOUSE, YSCR_DEV_ADDED, ev->mdevice.which,
                           SDL_GetMouseNameForID(ev->mdevice.which));
        break;
    case SDL_EVENT_MOUSE_REMOVED:
        yscr__device_log(s, YSCR_DEV_MOUSE, YSCR_DEV_REMOVED, ev->mdevice.which, NULL);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        yscr__device_log(s, YSCR_DEV_GAMEPAD, YSCR_DEV_ADDED, ev->gdevice.which,
                           SDL_GetGamepadNameForID(ev->gdevice.which));
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        yscr__device_log(s, YSCR_DEV_GAMEPAD, YSCR_DEV_REMOVED, ev->gdevice.which, NULL);
        break;
    case SDL_EVENT_PEN_PROXIMITY_IN:
        yscr__device_log(s, YSCR_DEV_PEN, YSCR_DEV_ADDED, ev->pproximity.which, "pen");
        break;
    case SDL_EVENT_FINGER_DOWN:
        yscr__device_log(s, YSCR_DEV_TOUCH, YSCR_DEV_ADDED, ev->tfinger.touchID,
                           SDL_GetTouchDeviceName(ev->tfinger.touchID));
        break;
    default:
        break;
    }
}

/* desc.gamepads: SDL sends a gamepad's events only while it is open. */
static void yscr__pad_open(yscr_screen* s, SDL_JoystickID id) {
    int i, free_i = -1;
    SDL_Gamepad* g;
    for (i = 0; i < YSCR__MAX_PADS; i++) {
        if (s->pads[i] && SDL_GetGamepadID((SDL_Gamepad*)s->pads[i]) == id) return;
        if (!s->pads[i] && free_i < 0) free_i = i;
    }
    if (free_i < 0) return;
    g = SDL_OpenGamepad(id);
    if (g) s->pads[free_i] = g;
}

static void yscr__pad_close(yscr_screen* s, SDL_JoystickID id, bool all) {
    int i;
    for (i = 0; i < YSCR__MAX_PADS; i++)
        if (s->pads[i] && (all || SDL_GetGamepadID((SDL_Gamepad*)s->pads[i]) == id)) {
            SDL_CloseGamepad((SDL_Gamepad*)s->pads[i]);
            s->pads[i] = NULL;
        }
}

typedef struct { yscr_screen* s; const SDL_Event* ev; } yscr__padev;
/* Gamepads open and close on SDL's main thread: the window thread's. */
static void yscr__pad_event_cb(void* p) {
    yscr__padev* a = (yscr__padev*)p;
    if (a->ev->type == SDL_EVENT_GAMEPAD_ADDED) yscr__pad_open(a->s, a->ev->gdevice.which);
    else yscr__pad_close(a->s, a->ev->gdevice.which, false);
}

static void yscr__device_event_cb(void* p) {
    yscr__padev* a = (yscr__padev*)p;
    yscr__device_event(a->s, a->ev);
}

/* One event from SDL's queue without a pump (WINDOW THREAD): any thread may
 * take events; SDL_PollEvent pumps, which belongs to SDL's main thread. */
static bool yscr__take(SDL_Event* ev) {
    if (SDL_PeepEvents(ev, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) <= 0) return false;
    if ((ev->type >= SDL_EVENT_TEXT_EDITING && ev->type <= SDL_EVENT_TEXT_INPUT) ||
        ev->type == SDL_EVENT_TEXT_EDITING_CANDIDATES || ev->type == SDL_EVENT_CLIPBOARD_UPDATE ||
        (ev->type >= SDL_EVENT_DROP_FILE && ev->type <= SDL_EVENT_DROP_POSITION))
        yscr__tls_dirty = 1;
    return true;
}

YSCR_API bool yscr_poll(yscr_screen* s, union SDL_Event* ev, int64_t* t_rt) {
    if (!ev) return false;
    if (s) s->polled = 1;
    /* doorbells SDL refused go in again once its queue is empty */
    if (yscr__wt_running()) {
        if (!yscr__take(ev) && (!yscr__in_rebell() || !yscr__take(ev))) return false;
    } else if (!SDL_PollEvent(ev) && (!yscr__in_rebell() || !SDL_PollEvent(ev))) return false;
    if (t_rt) *t_rt = yscr_restamp(s, ev->common.timestamp);
    if (s && s->sdl_gamepad && (ev->type == SDL_EVENT_GAMEPAD_ADDED || ev->type == SDL_EVENT_GAMEPAD_REMOVED)) {
        yscr__padev a;
        a.s = s;
        a.ev = ev;
        yscr__win_call(s, yscr__pad_event_cb, &a);
    }
    if (s && s->window) {
        if (s->wt && (ev->type == SDL_EVENT_KEYBOARD_ADDED || ev->type == SDL_EVENT_MOUSE_ADDED ||
                      ev->type == SDL_EVENT_GAMEPAD_ADDED || ev->type == SDL_EVENT_FINGER_DOWN)) {
            yscr__padev a;   /* the names are SDL's main thread's */
            a.s = s;
            a.ev = ev;
            yscr__win_call(s, yscr__device_event_cb, &a);
        } else yscr__device_event(s, ev);
    }
    return true;
}
#else
YSCR_API int64_t yscr_restamp(const yscr_screen* s, uint64_t sdl_ticks_ns) {
    (void)s; (void)sdl_ticks_ns; return 0;
}
YSCR_API bool yscr_poll(yscr_screen* s, union SDL_Event* ev, int64_t* t_rt) {
    /* the key feed and the icon serve SDL's event watch and its window;
     * without SDL only tests/adapt/screen_test.c calls them */
    (void)&yscr__abort_key; (void)&yscr__icon_decode; (void)&yscr__abort_edge;
    (void)&yscr__device_log; (void)&yscr__in_rebell;
    /* the window thread's rule; no window without SDL */
    (void)&yscr__wt_decide; (void)&yscr__wt_running; (void)yscr__wt_mode_now; (void)yscr__wt_mode_users;
    (void)yscr__tls_dirty;
    (void)s; (void)ev; (void)t_rt; return false;
}
#endif

/* One YSCR_EV_TEXT_INPUT record per change of on or off, not per caret
 * move: the log needs the intervals. external: the change was observed
 * (another library started or stopped text input), not made here. */
static void yscr__text_input_set(yscr_screen* s, int on, int external, int x, int y, int w, int h) {
    yrt_event ev;
    if (s->text_input == on) return;
    s->text_input = on;
    if (!s->ring) return;
    memset(&ev, 0, sizeof ev);
    ev.source = (uint16_t)YRT_SRC_SCREEN;
    ev.kind = (uint16_t)YSCR_EV_TEXT_INPUT;
    ev.t_ns = (uint64_t)yscr__now();
    ev.aux = s->display_index;
    ev.u.u32[0] = (uint32_t)on;
    ev.u.i32[1] = x; ev.u.i32[2] = y; ev.u.i32[3] = w; ev.u.i32[4] = h;
    ev.u.u32[5] = (uint32_t)external;
    yrt_ring_push(s->ring, &ev);
}

/* Deprecated (v0.4.0): raw mice come through the bridge. This reads the
 * raw mouse records straight from the store, oldest first, one event of a
 * report at a time, so their doorbells then decode as taken. */
YSCR_API bool yscr_poll_mouse(yscr_screen* s, yscr_mouse_event* out) {
    static const uint8_t button[6] = { 0, YSCR_MOUSE_LEFT, YSCR_MOUSE_MIDDLE, YSCR_MOUSE_RIGHT,
                                       YSCR_MOUSE_X1, YSCR_MOUSE_X2 };
    uint32_t raw, i;
    if (!out) return false;
    raw = (uint32_t)yscr__a_load(&yscr__in.next);
    i = yscr__in.mouse_next;
    if (raw - i > YSCR_INPUT_STORE) i = raw - YSCR_INPUT_STORE;
    for (; i != raw; i++) {
        yscr__in_slot* sl = &yscr__in.slot[i & YSCR__IN_MASK];
        int32_t seq = (int32_t)(i & YSCR__IN_SEQ), origin = 0;
        yin_event e;
        if (sl->origin != 1 || yscr__a_load(&sl->state) != 1 || yscr__a_load(&sl->seq) != seq) continue;
        if (yscr__in_take(seq, &e, &origin) != 1 || origin != 1) continue;
        yscr__in.mouse_next = i + 1;
        memset(out, 0, sizeof *out);
        out->t = e.t;
        out->device = e.device;
        if (e.type == YIN_PRESS && e.control <= 5) out->down = button[e.control];
        else if (e.type == YIN_RELEASE && e.control <= 5) out->up = button[e.control];
        else if (e.control == YIN_AXIS_DELTA) { out->dx = (int32_t)e.x; out->dy = (int32_t)e.y; }
        else if (e.control == YIN_AXIS_ABSOLUTE) {
            out->dx = (int32_t)(e.x * 65535.0f + 0.5f);
            out->dy = (int32_t)(e.y * 65535.0f + 0.5f);
            out->flags |= YSCR_MOUSE_ABSOLUTE;
        } else if (e.control == YIN_AXIS_WHEEL) {
            out->wheel = (int16_t)(e.value * 120.0f);
            out->hwheel = (int16_t)(e.x * 120.0f);
        }
        if (yscr__unlisted(s, out->device, out->t)) out->flags |= YSCR_MOUSE_UNLISTED;
        return true;
    }
    yscr__in.mouse_next = raw;
    return false;
}

YSCR_API int yscr_push_input(const yin_event* e) {
    if (!e) return YSCR_ERR_ARG;
    if (!yscr__in.type) return YSCR_ERR_CLOSED;
    return yscr__in_push(e, 0);
}

YSCR_API uint32_t yscr_input_event_type(void) { return yscr__in.type; }

YSCR_API void yscr_get_input_stats(yscr_input_stats* out) {
    if (!out) return;
    out->stored = (uint32_t)yscr__a_load(&yscr__in.next);
    out->overwritten = (uint32_t)yscr__a_load(&yscr__in.overwritten);
    out->refused = (uint32_t)yscr__a_load(&yscr__in.refused);
    out->pending = (uint32_t)yscr__a_load(&yscr__in.pending);
    out->lost = (uint32_t)yscr__a_load(&yscr__in.lost);
    out->key_doubles = (uint32_t)yscr__a_load(&yscr__in.key_doubles);
    out->key_double_ups = (uint32_t)yscr__a_load(&yscr__in.key_double_ups);
}

YSCR_API bool yscr_key_filter(yscr_screen* s, const yin_event* e) {
    return e && yscr__key_keep(s, e) != 0;
}

YSCR_API bool yscr_event_input(yscr_screen* s, const union SDL_Event* ev, yin_event* out) {
    if (!ev || !out) return false;
#if !defined(YSCR_NO_SDL)
    if (yscr__in.type && ev->type == yscr__in.type)
        return yscr__in_decode(s, ev->user.code, out) != 0 && yscr__key_keep(s, out);
    {
        yin_sdl_ctx ctx;
        ctx.raw_keyboard = s ? yscr__raw_keyboard(s) : true;
        ctx.keep_synthetic = false;
        if (!yin_from_sdl(ev, yscr_restamp(s, ev->common.timestamp), &ctx, out)) return false;
#if defined(YSCR__WEB)
        if (s && s->open && s->backend == YSCR_BACKEND_WEB)
            yscr__web_dom_stamp((yscr__web*)(void*)s->backend_mem, ev, out);
#endif
        return yscr__key_keep(s, out);
    }
#else
    (void)s;
    (void)&yscr__in_decode;
    return false;
#endif
}

#if !defined(YSCR_NO_SDL)
typedef struct { yscr_screen* s; bool on; SDL_Rect r; int rc; } yscr__ti;
/* SDL's text input calls belong to its main thread (WINDOW THREAD). */
static void yscr__text_input_cb(void* p) {
    yscr__ti* a = (yscr__ti*)p;
    yscr_screen* s = a->s;
    a->rc = YSCR_OK;
    if (a->on) {
        if (!SDL_SetTextInputArea(s->window, &a->r, 0) || (!SDL_TextInputActive(s->window) && !SDL_StartTextInput(s->window))) {
            yscr__set_error(s->error, sizeof s->error, "ysp_screen: text input: %s", SDL_GetError());
            a->rc = YSCR_ERR_LOST;
        }
    } else if (SDL_TextInputActive(s->window) && !SDL_StopTextInput(s->window)) {
        yscr__set_error(s->error, sizeof s->error, "ysp_screen: text input: %s", SDL_GetError());
        a->rc = YSCR_ERR_LOST;
    }
}
#endif

static int yscr__text_input(yscr_screen* s, bool on, int x, int y, int w, int h) {
    if (!s) return YSCR_ERR_ARG;
    if (!s->open) return YSCR_ERR_CLOSED;
    if (w < 0 || h < 0) return YSCR_ERR_ARG;
#if !defined(YSCR_NO_SDL)
    if (s->window) {
        yscr__ti a;
        int rc;
        a.s = s; a.on = on; a.r.x = x; a.r.y = y; a.r.w = w; a.r.h = h; a.rc = YSCR_OK;
        rc = yscr__win_call(s, yscr__text_input_cb, &a);
        if (rc < 0) return rc;
        if (a.rc != YSCR_OK) return a.rc;
    }
#endif
    yscr__text_input_set(s, on ? 1 : 0, 0, x, y, w, h);
    return YSCR_OK;
}

/* --- the calls on a screen: a failure always leaves a message --------------- */

/* Hides the message during a call, so yscr__done() can tell whether the
 * call wrote one; one byte each way, in the frame loop too. */
static char yscr__hold(yscr_screen* s) {
    char c0 = 0;
    if (s) { c0 = s->error[0]; s->error[0] = '\0'; }
    return c0;
}

/* A failure that wrote nothing gets the call's name and the code's text;
 * a success leaves the message of the last failure as it was. */
static int yscr__done(yscr_screen* s, char c0, int rc, const char* what) {
    if (!s) return rc;
    if (rc < 0) {
        if (!s->error[0]) yscr__set_error(s->error, sizeof s->error, "ysp_screen: %s: %s", what, yscr_strerror(rc));
    } else if (!s->error[0]) {
        s->error[0] = c0;
    }
    return rc;
}

YSCR_API int yscr_begin(yscr_screen* s, yscr_frame* f) {
    char c0 = yscr__hold(s);
    return yscr__done(s, c0, yscr__begin(s, f), "begin");
}

YSCR_API int yscr_flip_at(yscr_screen* s, int64_t t, yscr_record* out) {
    char c0 = yscr__hold(s);
    return yscr__done(s, c0, yscr__flip_at(s, t, out), "flip_at");
}

YSCR_API int yscr_flip(yscr_screen* s) {
    char c0 = yscr__hold(s);
    return yscr__done(s, c0, yscr__flip(s), "flip");
}

YSCR_API int yscr_wait_flip(yscr_screen* s, yscr_record* out) {
    char c0 = yscr__hold(s);
    return yscr__done(s, c0, yscr__wait_flip(s, out), "wait_flip");
}

YSCR_API int yscr_code_frames(yscr_screen* s, int slot, uint32_t rgb, int frames) {
    char c0 = yscr__hold(s);
    return yscr__done(s, c0, yscr__code_frames(s, slot, rgb, frames), "code");
}

YSCR_API int yscr_code(yscr_screen* s, int slot, uint32_t rgb) {
    char c0 = yscr__hold(s);
    return yscr__done(s, c0, yscr__code(s, slot, rgb), "code");
}

YSCR_API int yscr_code_row(yscr_screen* s, int slot, const uint32_t* px, int n, int frames) {
    char c0 = yscr__hold(s);
    return yscr__done(s, c0, yscr__code_row(s, slot, px, n, frames), "code_row");
}

YSCR_API int yscr_trigger(yscr_screen* s, int channel, uint32_t code) {
    char c0 = yscr__hold(s);
    return yscr__done(s, c0, yscr__trigger(s, channel, code), "trigger");
}

YSCR_API int yscr_trigger_at(yscr_screen* s, int channel, uint32_t code, int64_t t) {
    char c0 = yscr__hold(s);
    return yscr__done(s, c0, yscr__trigger_at(s, channel, code, t), "trigger_at");
}

YSCR_API int yscr_native(yscr_screen* s, yscr_native_info* out) {
    char c0 = yscr__hold(s);
    return yscr__done(s, c0, yscr__native(s, out), "native");
}

YSCR_API int yscr_window_call(yscr_screen* s, yscr_window_fn fn, void* ctx) {
    char c0 = yscr__hold(s);
    int rc;
    if (!fn) rc = YSCR_ERR_ARG;
    else if (s) rc = yscr__win_call(s, fn, ctx);
    else rc = yscr__wt_call(fn, ctx);
    return yscr__done(s, c0, rc, "window_call");
}

YSCR_API int yscr_text_input(yscr_screen* s, bool on, int x, int y, int w, int h) {
    char c0 = yscr__hold(s);
    return yscr__done(s, c0, yscr__text_input(s, on, x, y, w, h), "text_input");
}

YSCR_API const yscr_param* yscr_params(int* n) {
    static const yscr_param table[] = {
        { "display",         "u32",  0, 4294967295.0, 0, "",      "SDL display id; 0 = the primary display", NULL },
        { "mode.w",          "i32",  0, 65535, 0, "px",           "mode width; 0 = the desktop mode", NULL },
        { "mode.h",          "i32",  0, 65535, 0, "px",           "mode height; 0 = the desktop mode", NULL },
        { "mode.refresh_num","i32",  0, 2147483647.0, 0, "Hz",    "refresh numerator; 0 = the desktop mode", NULL },
        { "mode.refresh_den","i32",  0, 2147483647.0, 1, "",      "refresh denominator", NULL },
        { "windowed",        "bool", 0, 1, 0, "",                 "a window instead of borderless fullscreen", NULL },
        { "window_w",        "i32",  0, 65535, 800, "px",         "window width", NULL },
        { "window_h",        "i32",  0, 65535, 600, "px",         "window height", NULL },
        { "backend",         "enum", 0, 8, 0, "",                 "the presenting path; open() refuses the stubs glx_oml, wayland, metal",
          "auto=0,dxgi_flip=1,composition=2,glx_oml=3,wayland=4,metal=5,web=6,sim=7,custom=8" },
        { "lead",            "f64", -1, 0.999, 0.5, "frame",      "snap window; -1 = never before the target", NULL },
        { "display_index",   "u32",  0, 4294967295.0, 0, "",      "display number in the flip records", NULL },
        { "patch.on",        "bool", 0, 1, 0, "",                 "draw the photodiode patch", NULL },
        { "patch.size",      "i32",  0, 4096, YSCR_PATCH_DEFAULT_SIZE, "px", "patch side", NULL },
        { "patch.corner",    "enum", 0, 3, 0, "",                 "the patch's corner", "top_left=0,top_right=1,bottom_left=2,bottom_right=3" },
        { "show_cursor",     "bool", 0, 1, 0, "",                 "keep the cursor visible in fullscreen", NULL },
        { "abort_keys.off",  "bool", 0, 1, 0, "",                 "no key combination aborts the frame loop", NULL },
        { "abort_keys.key",  "u32",  0, 4294967295.0, 27, "",     "SDL keycode of the abort combination; 0 = Esc", NULL },
        { "abort_keys.mods", "u32",  0, 255, 1, "",               "modifiers held: 1 shift, 2 ctrl, 4 alt, 8 gui, 128 none; 0 = shift", NULL },
        { "panic",           "bool", 0, 1, 0, "",                 "arm the panic watchdog in fullscreen (Windows)", NULL },
        { "panic_presses",   "i32",  0, 16, 3, "",                "abort presses that end a hung program", NULL },
        { "panic_window_ns", "i64",  0, 6e10, 2e9, "ns",          "the time those presses must fall in", NULL },
        { "panic_grace_ns",  "i64",  0, 6e11, 1e10, "ns",         "no panic this long after the frame loop reported an abort", NULL },
        { "icon_sdl",        "bool", 0, 1, 0, "",                 "keep SDL's window icon instead of the header's", NULL },
        { "d3d11_video",     "bool", 0, 1, 0, "",                 "D3D11 device with video support and multithread protection", NULL },
        { "gamepads",        "bool", 0, 1, 0, "",                 "start SDL's gamepad subsystem and open every gamepad", NULL },
        { "raw_mice",        "bool", 0, 1, 0, "",                 "read each mouse's Raw Input on a thread: per device, raw-timed (Windows)", NULL },
        { "key_dedup_ns",    "i64", -1, 1e9, 5e7, "ns",           "drop a key's second report within this time; 0 = 50 ms, -1 = off", NULL },
        { "onset_offset_ns", "i64", -1e9, 1e9, 0, "ns",           "added to every onset; from the photodiode test", NULL },
        { "min_tier",        "i32",  0, 3, 0, "",                 "flag flips whose tier is worse; 0 = off", NULL },
        { "depth",           "i32",  0, 8, 0, "vblank",           "vblanks from a present to its flip; 0 = from the path, else pinned", NULL },
        { "depth_learn",     "bool", 0, 1, 0, "",                 "let misses raise the depth (adds latency; not with a pin)", NULL },
        { "settle",          "enum", 0, 3, 0, "",                 "settling at open; auto: strict fullscreen, warn in a window",
          "auto=0,strict=1,warn=2,off=3" },
        { "settle_flips",    "i32",  0, 64, 6, "frame",           "clean flips in a row that settle the start", NULL },
        { "settle_min_ns",   "i64", -1, 6e10, 0, "ns",            "settle no sooner; 0 = 2.5 s in a window, none fullscreen; -1 = none", NULL },
        { "settle_max_ns",   "i64",  0, 6e10, 3000000000.0, "ns", "the settle cap from the open() call; 0 = 3 s fullscreen, 4 s window", NULL },
        { "sim_period_ns",   "i64",  0, 1e10, 16666667, "ns",     "frame period of the simulated display", NULL }
    };
    if (n) *n = (int)(sizeof table / sizeof table[0]);
    return table;
}

#ifdef __cplusplus
}
#endif

#endif /* YSP_SCREEN_IMPLEMENTATION_GUARD */
#endif /* YSP_SCREEN_IMPLEMENTATION */

/* ------------------------------------------------------------------------
 * This software is available under the MIT-0 (MIT No Attribution) license.
 *
 * Copyright (c) 2026 ysp contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY.
 * ------------------------------------------------------------------------ */
