/* ysp/rt.h - v0.9.0 - public domain single-header real-time timing library
 *
 *   The clock, the waits, the scheduling ladder, the one-shot deadline
 *   worker, the background-compute pump, the event ring, the clock
 *   correlation, the device clock fit, the per-user folders and the
 *   instrumentation macros that a psychophysics rig needs, factored out of the transport headers so experiment code and
 *   future transports share one clock base and one set of timing claims.
 *
 *   Written in the single-header style of the stb / sokol libraries. It is
 *   the base the other ysp headers stand on: ysp/parallel.h and ysp/serial.h
 *   both include it and both run their trailing edges on the worker below.
 *
 *   Targets Windows, Linux and macOS, and WebAssembly through Emscripten
 *   (node, or a browser worker) with the scheduling ladder collapsed; see
 *   WEBASSEMBLY. C99 is the floor: it builds as C99, C11 and C++17, and in
 *   the C dialect MSVC compiles by default. Nothing but the OS.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *   Do this:
 *
 *       #define YSP_RT_IMPLEMENTATION
 *
 *   in *one* C or C++ file before including this header to create the
 *   implementation. Every other file just includes the header normally.
 *
 *       #define YSP_RT_IMPLEMENTATION
 *       #include "ysp/rt.h"
 *
 *       yrt_policy pol = yrt_thread_elevate(NULL);   // NULL = defaults
 *       yrt_report rep;
 *       char line[192];
 *       yrt_report_get(&rep, pol);
 *       yrt_describe(&rep, line, sizeof line);
 *       puts(line);                      // log this next to your timings
 *
 *       int64_t t = yrt_now_ns();
 *       for (int i = 0; i < 100; i++) {
 *           t += YRT_MS(10);                        // a 10 ms frame
 *           int64_t late = yrt_sleep_until(t, YRT_DEFAULT_SPIN_NS);
 *           do_frame(i, late);
 *       }
 *
 *   ---------------------------------------------------------------------
 *   CLOCK
 *   ---------------------------------------------------------------------
 *   yrt_now_ns() reads a monotonic clock:
 *   CLOCK_MONOTONIC on Linux and macOS, QueryPerformanceCounter on Windows,
 *   converted with integer arithmetic split so that a TSC-rate counter
 *   cannot overflow the multiply. The clock never steps and never goes
 *   backwards; its zero is arbitrary, so only differences mean anything.
 *
 *   It returns int64_t, the type of every time in ysp, so the difference of
 *   two times is signed. Every header times against it: ysp/serial.h and
 *   ysp/parallel.h time their pulses on it, ysp/screen.h and ysp/audio.h map
 *   their onsets to it. Times from two headers taken in the same process are
 *   directly comparable; subtract them. They are not comparable across a
 *   reboot or with wall-clock time.
 *
 *   UNITS. Every time in a ysp C API is an int64_t count of nanoseconds, a
 *   field or parameter whose unit the name does not already say ends in
 *   _ns, and YRT_S(), YRT_MS() and YRT_US() write one from another unit:
 *   ytl_wait(&q, YRT_MS(500)). A bare 0.5 where nanoseconds are expected
 *   converts to 0; MSVC warns (C4244 at its default /W3), and gcc warns
 *   with -Wfloat-conversion, which the repository's CMake build turns on.
 *   Seconds stay in files, scripts and the Python and MATLAB bindings.
 *
 *   yrt_ticks_to_ns() is the conversion yrt_now_ns() applies to the raw
 *   counter, as a function. On Windows, give it a QueryPerformanceCounter
 *   value that another API reports (DXGI's SyncQPCTime, a WASAPI QPC
 *   position). The result is the ysp_rt time of that counter value, bit for
 *   bit, so you do not need a correlation for it. On the other platforms
 *   the counter already counts nanoseconds and the function returns its
 *   argument.
 *
 *   yrt_get_clock_info() reports what the OS says the clock is worth
 *   (clock_getres on POSIX, 1 s / QueryPerformanceFrequency on Windows) and
 *   whether a Windows high-resolution waitable timer could be created. The
 *   resolution is the tick the clock counts in, not the cost of reading it
 *   and not the accuracy of any wait.
 *
 *   ---------------------------------------------------------------------
 *   WAITS
 *   ---------------------------------------------------------------------
 *   yrt_sleep_until(deadline_ns, spin_ns) is the workhorse. It hands the
 *   bulk of the wait to the OS, wakes spin_ns before the deadline, then
 *   spins on the clock to the deadline itself, and returns how late it
 *   actually woke in nanoseconds. Log that number; it is the only honest
 *   statement this header can make about its own accuracy on your machine.
 *
 *     Linux:   clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME). Absolute, so
 *              a signal restart cannot add the elapsed time back on. Its
 *              accuracy is bounded below by the thread's timer slack (50 us
 *              by default; see yrt_thread_set_timer_slack) and above by
 *              whatever else the scheduler is doing.
 *     macOS:   no absolute clock_nanosleep exists, so the wait is a relative
 *              nanosleep recomputed from the clock on each iteration. A
 *              short wake does not shorten the total.
 *     Windows: a waitable timer created with
 *              CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Win10 1803+). If that
 *              is refused, the fallback timer only fires on the system tick,
 *              about 15.6 ms, and the spin window is WIDENED to a full tick
 *              so the deadline is still met, trading CPU for accuracy
 *              exactly as ysp/parallel.h does. That widening is the only
 *              time this function ignores your spin_ns.
 *
 *   spin_ns = 0 leaves the entire wait to the OS. On Windows even the
 *   high-resolution timer may fire up to about a millisecond late; on Linux
 *   the floor is the thread's timer slack. A spin window buys accuracy with a
 *   busy core: the thread burns CPU for spin_ns of every wait. Pick it from
 *   measurement, not from this comment, and run examples/rt/jitter.c on the
 *   rig to see what each window is worth there.
 *
 *   yrt_spin_until() is the pure-spin floor: no syscall, a pause
 *   instruction in the loop so a sibling hyperthread is not starved. It is
 *   as accurate as the clock and as expensive as a full core.
 *
 *   yrt_sleep_ns() is the relative convenience wrapper. It, and a zeroed
 *   yrt_worker_desc.spin_ns, take YRT_DEFAULT_SPIN_NS as the window. That
 *   default is NOT the same on every platform, because the OS wait it has to
 *   cover is not the same:
 *
 *     Windows: about 1.2 ms, because the high-resolution waitable timer may
 *              fire that late. A narrower window leaves a median lateness the
 *              OS wait itself cannot remove, so the spin is the only thing
 *              that closes it. ysp/parallel.h spins the last ~1.14 ms of a
 *              pulse for the same reason.
 *     Linux,
 *     macOS:   200 us, which covers the default 50 us timer slack and the
 *              ordinary scheduling delay behind it. A wider window here would
 *              burn CPU for nothing.
 *
 *   Both are starting points, not measurements, and a rig with a different
 *   power plan, chipset or background load will want a different number.
 *   examples/rt/jitter.c sweeps several windows, marks this platform's
 *   default, and prints the wake latency distribution of each: pick yours
 *   from that output and pass it to yrt_sleep_until() or to
 *   yrt_worker_desc.spin_ns.
 *
 *   No wait in this header allocates. On Windows each thread that calls
 *   yrt_sleep_until() creates one waitable timer on first use and reuses
 *   it; yrt_thread_cleanup() releases it for a thread that is about to
 *   end and will not wait again.
 *
 *   ---------------------------------------------------------------------
 *   VIRTUAL CLOCK (v0.9.0)
 *   ---------------------------------------------------------------------
 *   yrt_virtual_begin(t0_ns) puts the whole process on a virtual clock, so
 *   a simulated run gives the same records and output on any host, loaded
 *   or not. Call it before anything reads the clock (the examples call it
 *   right after they parse --sim):
 *
 *       if (sim) yrt_virtual_begin(0);   // starts at YRT_VIRTUAL_T0_NS
 *
 *   From then on yrt_now_ns() reads the virtual time, and the time moves
 *   only when the thread that called yrt_virtual_begin() (the DRIVER)
 *   waits: yrt_sleep_until(), yrt_sleep_ns() and yrt_spin_until() on the
 *   driver jump the clock to their deadline at once and return 0. Between
 *   two waits the clock stands still, so a frame's own work takes no time.
 *   On any other thread those waits block, in real 100 us steps, until the
 *   driver has moved the clock to the deadline. A yrt_worker job runs once
 *   the clock passes its deadline, the same way, and its at_ns reads the
 *   virtual clock. Event ring stamps (t_ns 0) are virtual too.
 *
 *   What moves with the clock runs as a VIRTUAL TIMER on the driver:
 *   yrt_virtual_timer(fn, ctx, at_ns) calls fn(ctx, t) when a wait of the
 *   driver passes at_ns, with the clock set to that time, and fn returns
 *   its next time or ends. Timers run in time order, ties in slot order (a
 *   timer takes the lowest free slot), so a run repeats exactly.
 *   YRT_VIRTUAL_TIMERS (16) can be set at once; set and cancel them on the
 *   driver only. A timer must not wait: it runs inside a wait already (a
 *   wait there moves the clock and runs no timer).
 *
 *   What uses it: ysp/screen.h's BACKEND_SIM sleeps to its vblank grid,
 *   which on the virtual clock jumps there, so no frame drops; ysp/audio.h's
 *   BACKEND_NULL becomes a device on a virtual timer (one block per period,
 *   at its own time), and both refuse a real device; ysp/video.h decodes
 *   inline. Real time stays where it is the point: the instrumentation
 *   zones (a cost is real CPU time), yrt_correlate(), and the timeout of
 *   yrt_pump_wait().
 *
 *   Limits. A driver that polls for another thread's real progress
 *   (poll, sleep 1 ms, until a timeout) sees the timeout pass at once: on
 *   the virtual clock a wait is free. Work that only a real thread does (a
 *   decode thread, a capture writer) is not paced by the virtual clock, so
 *   its results can still differ between runs; make it inline, or a
 *   virtual timer. yrt_virtual_end() returns to the real clock.
 *
 *   COSTS. Between two waits the virtual clock stands still, so a cost
 *   read with yrt_now_ns() is 0, and one across a wait is the jump. A
 *   time that feeds a plan or a record reads yrt_now_ns(); a cost (a draw,
 *   a decode, a build, a CPU phase) reads yrt_real_ns(), the real counter
 *   on either clock. The headers do so: ysp/screen.h's phases, ysp/gfx.h's
 *   program and glyph build times, ysp/video.h's decode and upload times.
 *
 *   Cost on the real clock: yrt_now_ns() and the waits test one flag
 *   first. See docs/rt.md, "Virtual clock", for the measurement.
 *
 *   ---------------------------------------------------------------------
 *   SCHEDULING
 *   ---------------------------------------------------------------------
 *   yrt_thread_elevate() raises the CALLING thread as far up this ladder
 *   as the OS and this process's privileges allow, and reports the rung that
 *   stuck. Asking is not getting: every rung can be refused, nothing fails,
 *   and the return value is the only way to know what you have.
 *
 *     YRT_POLICY_DEADLINE         Linux SCHED_DEADLINE, through the raw
 *                                   sched_setattr syscall (glibc has no
 *                                   wrapper). Needs CAP_SYS_NICE, and the
 *                                   kernel also refuses it when the thread's
 *                                   affinity is not the full root domain,
 *                                   which is why yrt_thread_pin() and this
 *                                   rung are mutually exclusive.
 *     YRT_POLICY_TIME_CONSTRAINT  macOS THREAD_TIME_CONSTRAINT_POLICY via
 *                                   thread_policy_set: the same "runtime in
 *                                   a period by a deadline" reservation,
 *                                   under a different kernel. Its own rung
 *                                   because it is neither SCHED_DEADLINE nor
 *                                   SCHED_FIFO and should not be logged as
 *                                   either.
 *     YRT_POLICY_FIFO             SCHED_FIFO at priority 80, or the system
 *                                   maximum when that is lower. 80 is above
 *                                   the kernel's threaded IRQ handlers (50)
 *                                   and below migration and the watchdog
 *                                   (99). Needs CAP_SYS_NICE on Linux.
 *     YRT_POLICY_TIME_CRITICAL    Windows THREAD_PRIORITY_TIME_CRITICAL.
 *                                   On every rung it gets, the Windows
 *                                   thread is also opted out of EcoQoS; see
 *                                   POWER THROTTLING below.
 *     YRT_POLICY_NORMAL           Nothing was granted. On Linux the timer
 *                                   slack is still dropped to 1 ns (the kernel
 *                                   rounds up to its own floor), which is free
 *                                   and removes 50 us of deliberate lateness
 *                                   from every wait.
 *
 *   One rung sits BELOW that floor and is not part of this ladder:
 *   YRT_POLICY_BELOW_NORMAL, which only yrt_pump asks for and only when
 *   yrt_pump_desc.below_normal is set. yrt_thread_elevate() never lowers a
 *   thread, and nothing here ever moves a thread down the ladder it climbed.
 *
 *   The reservation (yrt_sched_deadline: runtime, deadline, period in ns)
 *   means the same thing here as in ypar_desc.sched and yser_desc.sched:
 *   runtime is the CPU budget guaranteed and capped per period, the relative
 *   deadline orders EDF priority, and runtime/period is the bandwidth the
 *   kernel must admit. All-zero means the library defaults, 1 ms in 10 ms.
 *   yrt_sched_normalize() applies the same validation the transport
 *   headers apply at open time: a zero period becomes the deadline, and
 *   0 < runtime <= deadline <= period must hold.
 *
 *   The rest of the setup calls each do one thing and say whether it took:
 *
 *     yrt_thread_set_timer_slack()  Linux PR_SET_TIMERSLACK. A normal
 *                                     thread's timers are allowed to fire
 *                                     50 us late so the kernel can batch
 *                                     wakeups. Set it to 1 ns (rounded up to
 *                                     the kernel's floor) and that deliberate
 *                                     lateness is gone. No-op elsewhere.
 *     yrt_thread_pin()              Bind the calling thread to one CPU.
 *                                     PINNING DEFEATS SCHED_DEADLINE: the
 *                                     kernel's admission test refuses a
 *                                     deadline task whose affinity is not
 *                                     the root domain, so pin OR take the
 *                                     DEADLINE rung, not both. Pinning plus
 *                                     an isolated core (isolcpus, nohz_full)
 *                                     is the other way to build a quiet
 *                                     thread. macOS has no true pinning, only
 *                                     an affinity hint, so this returns false
 *                                     there. On a hybrid CPU the number does
 *                                     not tell you the kind of core: ask
 *                                     yrt_cpu_core_type() first, and read
 *                                     pin= in the report line. See CORE
 *                                     TYPES.
 *     yrt_process_lock_memory()     POSIX mlockall(MCL_CURRENT|MCL_FUTURE):
 *                                     no page of this process is paged out,
 *                                     so no wait ends in a major fault. Needs
 *                                     RLIMIT_MEMLOCK headroom or CAP_IPC_LOCK.
 *                                     WINDOWS HAS NO EQUIVALENT. VirtualLock
 *                                     and SetProcessWorkingSetSize pin
 *                                     individual pages into a working set
 *                                     whose size the memory manager still
 *                                     owns; neither is mlockall. This
 *                                     function does nothing and returns false
 *                                     on Windows rather than claim otherwise.
 *     yrt_timer_resolution_begin()  Windows timeBeginPeriod(1), loaded from
 *                                     winmm at runtime so the header keeps
 *                                     its "no libraries" promise. It lowers
 *                                     the system tick so ordinary waits get
 *                                     finer. Since Windows 10 2004 the effect
 *                                     is per-process, not global, so it no
 *                                     longer slows the whole machine down and
 *                                     no longer helps another process. The
 *                                     high-resolution waitable timer that
 *                                     yrt_sleep_until() uses makes it
 *                                     largely unnecessary; it still matters
 *                                     for Sleep() and for the fallback timer.
 *                                     It also opts the process out of the
 *                                     Windows 11 timer throttling below.
 *                                     Pair with yrt_timer_resolution_end().
 *                                     No-op elsewhere.
 *
 *   POWER THROTTLING (Windows 11). When every window of a process is
 *   minimized or hidden, Windows may do two things to it:
 *
 *     Timer resolution  It no longer honors timeBeginPeriod(). Sleep() and
 *                       a waitable timer without the high-resolution flag
 *                       wake on the 15.6 ms tick again. Measured on this
 *                       repository's test laptop, minimized: 13 ms late
 *                       instead of 0.5 ms. yrt_timer_resolution_begin()
 *                       therefore also calls SetProcessInformation() with
 *                       PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,
 *                       which kept the 0.5 ms. yrt_sleep_until(), the
 *                       worker and the pump use the high-resolution timer,
 *                       which this throttling does not touch: no state
 *                       changed their lateness.
 *     EcoQoS            It runs the process's threads on efficiency cores.
 *                       Minimized, 99 to 100% of the measured threads ran on
 *                       E-cores, and the same computation took about 1.25
 *                       times as long; wake lateness did not change. So
 *                       yrt_thread_elevate() and the pump thread call
 *                       SetThreadInformation() with
 *                       THREAD_POWER_THROTTLING_EXECUTION_SPEED off, for the
 *                       calling thread only. A worker with no_elevate and
 *                       every thread ysp/rt.h does not touch are left to the
 *                       OS.
 *
 *   Both calls are looked up at run time, so the header still builds and
 *   runs on Windows versions and with MinGW headers that lack them; there a
 *   call does nothing, and the report says n/a. Neither call is undone: the
 *   opt-outs last for the life of the process and the thread.
 *   yrt_timer_resolution_end() does not undo the first one.
 *
 *   CORE TYPES. A hybrid CPU has performance cores (P) and efficiency cores
 *   (E). yrt_cpu_core_type(cpu) says which one a logical CPU is. On
 *   Windows 10 and later it reads the EfficiencyClass of
 *   GetSystemCpuSetInformation(): the highest class is P, any lower class
 *   is E, one class everywhere is YRT_CORE_UNIFORM. Everywhere else it
 *   returns YRT_CORE_UNKNOWN. Linux on Intel hybrid lists the two kinds
 *   in /sys/devices/cpu_core/cpus and /sys/devices/cpu_atom/cpus, but WSL2
 *   hides them (it shows 8 identical vCPUs on the test laptop's 4 P-cores
 *   and 8 E-cores), so this header does not read them; check those files
 *   yourself before you pin on a Linux rig.
 *
 *   On Windows, yrt_thread_elevate() also prefers the P-cores for the
 *   calling thread, with SetThreadSelectedCpuSets() and the CPU sets of the
 *   highest EfficiencyClass. It does this on a hybrid CPU only. The opt-out
 *   from EcoQoS alone does not keep a thread on a P-core: with 8 busy
 *   threads on the test laptop, an elevated thread still woke on an E-core
 *   55 to 90% of the time, and a fixed computation took 9 to 25% longer
 *   than on a P-core. With the preference it ran on P-cores every time.
 *   The preference is soft: Windows still runs the thread elsewhere when
 *   no P-core is available. yrt_thread_pin() removes it, because a pin
 *   is the explicit choice. The pump does not get it: at normal priority on
 *   busy P-cores, its wake p99 rose to 14.5 ms in one run of four. Every
 *   thread ysp/rt.h does not touch is left alone.
 *
 *   yrt_report_get() and yrt_describe() put the whole outcome on one
 *   line, for the log file next to the timing data:
 *
 *     ysp_rt: policy=TIME_CRITICAL clock_res=100ns slack=n/a hires_timer=yes
 *             mlock=no timer_res=yes timer_throttle=off ecoqos=off pin=none
 *             cores=P
 *
 *   timer_throttle=off means the process is opted out of the timer
 *   throttling, and ecoqos=off that the CALLING thread is opted out of
 *   EcoQoS; "on" means Windows may still apply it, and "n/a" means the OS
 *   has no such setting (every platform but Windows 10 1709 and later).
 *   pin= is the CPU yrt_thread_pin() bound the calling thread to, with
 *   its kind (P, E, uniform or ?), or none. cores=P means the calling
 *   thread prefers the P-cores; cores=any means it does not.
 *
 *   Jitter numbers without that line are unlabeled. Print it.
 *
 *   ---------------------------------------------------------------------
 *   WORKER
 *   ---------------------------------------------------------------------
 *   yrt_worker is one thread that runs one callback at one absolute
 *   deadline. The caller allocates the handle, yrt_worker_open() spawns
 *   the thread, and the thread elevates ITSELF up the ladder above and
 *   publishes the rung it got before open() returns, so the policy is known
 *   before the first trial and no trial pays for thread creation.
 *
 *     yrt_worker_submit(w, deadline_ns, fn, ctx)
 *         One pending job per worker. A submit while a job is pending
 *         REPLACES it, earlier deadline or later, and the worker re-evaluates
 *         at once. There is no queue: a rig that needs two independent
 *         deadlines starts two workers.
 *     yrt_worker_cancel(w)
 *         Drops the pending job.
 *     yrt_worker_close(w)
 *         Runs a pending job IMMEDIATELY (a flush, with info->flushed set)
 *         and then joins. This is what the transport headers do when they
 *         close with a pulse in flight: a trailing edge that never lands is
 *         worse than an early one.
 *
 *   The callback runs ON THE WORKER THREAD, at the worker's policy, WITHOUT
 *   the worker's lock held. That is deliberate and it is what makes the
 *   callback usable: it may call yrt_worker_submit() on its own worker to
 *   re-arm a periodic job, it may block, and it cannot deadlock against a
 *   submit from another thread. The price is that a callback which outlives
 *   its own deadline overlaps the next one, and that two calls to a
 *   non-reentrant routine from the callback and from the main thread need
 *   the caller's own lock. Nothing in the callback's data is protected by
 *   ysp/rt.h.
 *
 *   Nothing allocates after yrt_worker_open() returns. The handle carries
 *   its own OS objects, the job is three fields, and submit, cancel and the
 *   worker's own wait are all allocation-free.
 *
 *   A submit or cancel issued inside the worker's final spin window is seen
 *   AFTER the spin ends, not during: the worker releases its lock to spin, so
 *   the call does not block, but the job it was racing has already been
 *   chosen. The race is exactly as wide as the worker's spin window, so it is
 *   wider on Windows than on Linux and macOS when the desc leaves that window
 *   at YRT_DEFAULT_SPIN_NS. See WAITS.
 *
 *   yrt_worker_desc.on_start runs ON THE WORKER THREAD, after the thread
 *   elevates itself and before yrt_worker_open() returns, and a false
 *   return fails the open. It is for state the OS keeps PER THREAD, which
 *   the thread that calls open() cannot set up on the worker's behalf: the
 *   parallel port's DIRECT backend grants itself ioperm()/iopl() there,
 *   because those are per-thread on Linux and a refused grant has to fail
 *   the open, not the first trigger.
 *
 *   Every submit stamps the job with a sequence number, returns it, and
 *   hands the same number to the callback as yrt_job_info.seq;
 *   yrt_worker_pending() reports the seq of the job now pending, or 0. A
 *   job that has to know whether it is still the one its caller is waiting
 *   for can compare the two, without a token of its own. The numbers are
 *   positive, increasing and unique per worker; nothing else about them is
 *   promised.
 *
 *   Both transport headers build their trailing edges on this worker, and
 *   both keep a small mutex and a token of their own beside it. That is not
 *   duplication: the worker runs the callback with ITS lock released (see
 *   above), so serializing the register or the byte stream is the
 *   transport's job, and a job already inside its final spin window cannot
 *   be canceled, so "a plain write cancels a pending trailing edge" needs a
 *   token the callback rechecks under the transport's own lock. See the
 *   THREADING section of ysp/serial.h and the async block of ysp/parallel.h
 *   for each one. Everything ELSE uses this worker as it stands: a stimulus
 *   onset, a frame boundary, a response window, a new transport you write.
 *
 *   ---------------------------------------------------------------------
 *   PUMP
 *   ---------------------------------------------------------------------
 *   yrt_pump is the other half of the worker's job, and the opposite shape.
 *   The worker runs a SHORT callback AT a deadline, as high up the scheduling
 *   ladder as it can get. The pump runs a LONG callback BETWEEN deadlines, at
 *   normal priority or below it, and never climbs.
 *
 *   What it is for: an adaptive method whose inference does not fit a frame.
 *   A QUEST+ selection over a large grid, a Gaussian-process refit, a
 *   hyperparameter fit; see docs/adapt.md, "Inference on a thread". The
 *   trial loop submits the response and returns to drawing; the pump runs
 *   update and next; the loop asks "is the next stimulus ready?" when the
 *   inter-trial interval ends. That turns a frame budget from a hard
 *   constraint into a contract the code can check, and it makes background
 *   fitting free: on_idle runs one fit step whenever nothing is queued.
 *
 *       yrt_pump_desc d = {          // unset fields are 0: the defaults
 *           .msg_size = sizeof(trial_result),
 *           .capacity = 8,
 *           .on_msg   = infer,         // runs on the pump thread
 *           .on_idle  = fit_one_step,  // optional; true = call me again
 *           .ctx      = &model,
 *       };
 *       if (!yrt_pump_open(&pump, &d)) die(yrt_pump_error(&pump));
 *
 *       int seq = yrt_pump_submit(&pump, &result);   // < 0 is an error
 *       ...                                           // draw some frames
 *       if (yrt_pump_done_seq(&pump) >= (uint32_t)seq) {
 *           yrt_pump_lock(&pump);                   // read the publication
 *           next_level = model.published_level;
 *           yrt_pump_unlock(&pump);
 *       }
 *
 *   THE RING. msg_size bytes times capacity messages, either a buffer the
 *   caller owns or, when desc.ring is NULL, an inline buffer inside the
 *   handle (YRT_PUMP_INLINE_BYTES, 4096 by default). Nothing allocates,
 *   ever. yrt_pump_submit() COPIES the message into the ring and returns its
 *   sequence number; when the ring is full it returns YRT_ERR_FULL and
 *   copies nothing, so the caller still owns the message it tried to send and
 *   may retry it on the next frame. That is the honest failure: a pump that
 *   silently grew a queue would trade a visible error for an invisible
 *   unbounded latency. A message being handled still occupies its slot until
 *   on_msg returns, because on_msg reads it in place; so a full ring means
 *   capacity messages accepted and at most one of them in flight.
 *
 *   Submit is safe from several threads at once: it takes the ring's mutex.
 *   The ring has exactly ONE consumer, the pump thread, and on_msg is called
 *   for each message in submit order, one at a time, never concurrently with
 *   itself and never with either lock held. The order across two threads that
 *   submit at the same instant is the order the mutex granted them, which is
 *   the only order there is to have.
 *
 *   DONE_SEQ AND WAIT. yrt_pump_done_seq() is the highest seq whose on_msg
 *   has RETURNED. It is one atomic load, no lock, cheap enough for a frame
 *   loop to poll every frame. It is published after the callback returns, so
 *   `done_seq >= seq` means the work for that message is finished and whatever
 *   it published is readable. yrt_pump_wait() is the blocking form with a
 *   relative timeout, for the end of an inter-trial interval: it returns 0
 *   when the pump has passed the seq and YRT_ERR_TIMEOUT when the timeout
 *   ran out first. Poll in a frame loop; wait in an interval you are willing
 *   to lengthen.
 *
 *   THE LOCK. yrt_pump_lock() / yrt_pump_unlock() guard a struct the
 *   callbacks write and the caller reads: the proposal, a posterior summary,
 *   a progress counter. It is a SEPARATE mutex from the ring's, and that is
 *   the whole point. on_msg runs without it held and is expected to take it
 *   only around its publish, at the end, for the few instructions a struct
 *   copy costs. So a caller may hold it for as long as it likes without
 *   stalling the pump's inference, the ring, a submit, or a done_seq poll; the
 *   only thing that waits is a publish. Nothing else in the pump takes it, and
 *   the pump never reads what you put under it.
 *
 *   IDLE. on_idle runs on the pump thread, with no lock held, whenever the
 *   ring is empty. Return true and it is called again (after a re-check of the
 *   ring, so a submit is never starved by a busy idle callback); return false,
 *   or leave on_idle NULL, and the thread blocks on a condition variable until
 *   the next submit or the close. It must return promptly, a few milliseconds:
 *   a close cannot join the thread until it does, and a message cannot be
 *   dispatched until it does. One gradient step, not a whole fit.
 *
 *   PRIORITY. The pump runs at normal priority, or below it when
 *   desc.below_normal is set: THREAD_PRIORITY_BELOW_NORMAL on Windows, nice +5
 *   on the pump thread alone on Linux (setpriority(PRIO_PROCESS, tid), with
 *   the kernel task id from the gettid syscall, because Linux nice is
 *   per-task and PRIO_PROCESS with a thread id is how you reach one thread),
 *   and a Mach THREAD_PRECEDENCE_POLICY importance of -16 on macOS. Failing to
 *   lower is not an error; yrt_pump_policy() then reports
 *   YRT_POLICY_NORMAL instead of YRT_POLICY_BELOW_NORMAL, and those two
 *   are the only values it ever returns while the pump runs. On Windows the
 *   pump thread is also opted out of EcoQoS, below normal or not: priority
 *   decides who runs first, and EcoQoS decides on which core and how fast;
 *   minimized, an inference took about 1.25 times as long on the efficiency
 *   cores. See POWER THROTTLING.
 *
 *   The pump NEVER elevates. It does not call yrt_thread_elevate() and has
 *   no desc.sched. A thread that may hold a CPU for 30 ms must not be able to
 *   preempt the frame loop or a deadline worker, and a SCHED_FIFO thread that
 *   runs a Cholesky is a machine you cannot get the mouse back on. Inference
 *   is work that must FINISH; a trailing edge is work that must finish ON
 *   TIME. Only the second one belongs on the ladder. desc.pin_cpu is there for
 *   the other half of the same idea: pin the pump to a core the frame loop
 *   does not use and its cache footprint stops being the frame loop's problem.
 *
 *   CLOSE. yrt_pump_close() refuses new submits first (they get
 *   YRT_ERR_STOPPED), then DRAINS: every message already in the ring is
 *   still delivered to on_msg, in order, before the thread is joined. A
 *   submitted response must not be lost because the session ended. Set
 *   desc.drop_on_stop and it discards the queue instead, keeping only the
 *   message already in flight, which cannot be un-run; those dropped messages
 *   never reach done_seq, so a yrt_pump_wait() on one of them returns
 *   YRT_ERR_STOPPED. Drain when the messages are data; drop when they are
 *   requests for a result nobody will read.
 *
 *   Both close forms join the thread, so on_msg and on_idle are guaranteed not
 *   to be running when yrt_pump_close() returns. That is what makes it safe
 *   to free or reuse the context and the ring afterwards. Close is safe on a
 *   zeroed handle and on one already closed.
 *
 *   yrt_pump_wait() is SAFE AGAINST A CONCURRENT CLOSE AT ANY POINT, and
 *   against a restart. A thread may sit in a loop of short waits while
 *   another closes and opens the pump as often as it likes; no wait touches a
 *   destroyed mutex, none hangs past its timeout, and each returns 0 (the seq
 *   was reached), YRT_ERR_TIMEOUT or YRT_ERR_STOPPED. The mechanism: a
 *   wait registers itself in an atomic counter before it looks at anything
 *   else, and backs out if the pump's mutexes are already gone; a close marks
 *   them gone, lets every registered waiter out, and destroys nothing until
 *   the counter is zero. So a binding needs no wait slicing and no deferral
 *   of its own. The other calls keep the ordinary rule: do not race a submit,
 *   a lock or an open with a close. One cost follows from the counter: a
 *   thread that calls yrt_pump_wait() in a tight loop on a pump that is
 *   stopping keeps the counter above zero part of the time, and the stop
 *   waits in 100 us polls until it catches a zero. The test's hammer does
 *   exactly that, with two such threads, and its 300 open-and-close cycles
 *   took 0.4 to 4.3 s in all across Linux, Windows and node, thread creation
 *   included; a caller that sleeps between waits never sees it.
 *
 *   ---------------------------------------------------------------------
 *   EVENT RING
 *   ---------------------------------------------------------------------
 *   yrt_ring holds fixed 64-byte records, yrt_event, in memory you
 *   supply. Any thread pushes. One thread drains. It is the log primitive
 *   of the rig: the flip record, the audio onset, the video frame decision,
 *   restamped input and trace zones all go through a ring, and the drain
 *   writes them out in whatever format you choose.
 *
 *       static unsigned char mem[YRT_RING_BYTES(4096)];   // 256 KB + 64
 *       static yrt_ring ring;
 *       if (!yrt_ring_open(&ring, &(yrt_ring_desc){
 *               .memory = mem, .bytes = sizeof mem }))
 *           die(yrt_ring_error(&ring));
 *
 *       // any thread, an audio callback included; t_ns 0 = now, tid 0 = me
 *       yrt_ring_push(&ring, &(yrt_event){ .source = MY_SRC,
 *                                             .kind = ONSET,
 *                                             .u.i64 = { start_frame } });
 *
 *       // the frame thread, once per frame, after the flip
 *       yrt_event ev[64];
 *       for (int n; (n = yrt_ring_drain(&ring, ev, 64)) > 0; )
 *           for (int i = 0; i < n; i++) write_csv_row(&ev[i]);
 *
 *   examples/rt/ring_csv.c is the whole program. In C++17, zero a desc and
 *   an event and set their fields one by one.
 *
 *   THE RECORD. seq (written by the ring), tid, t_ns, source, kind, aux and
 *   a 40-byte payload with several views (i64, u64, f64, u32, u16, text,
 *   bytes). A zero t_ns gets the time of the push. A zero tid gets
 *   yrt_thread_id(). The source says who pushed the record. The kind says
 *   what it is, and its meaning belongs to the source. ysp/rt.h assigns the
 *   sources 1 to 255 to the ysp headers (YRT_SRC_*); an extension host
 *   assigns 256 to 32767; 32768 and up are yours. Records come out in the
 *   order the ring took them, which is each thread's push order. That is
 *   NOT time order: a record can carry a time in the future (an estimated
 *   onset) or in the past. Sort a batch by t_ns if you need time order.
 *
 *   WAIT-FREE PUSH. A push is a fixed sequence: one fetch-add on a count of
 *   used slots, one on the ticket counter, a copy of 60 bytes, and one
 *   release store that publishes the record. There is no lock, no retry
 *   loop, no allocation and, with t_ns and tid set, no system call. No
 *   thread ever waits for another inside a push, whatever its priority and
 *   whatever the other thread is doing. That holds where a fetch-add is one
 *   instruction: x86-64, ARMv8.1 and later with LSE atomics, and
 *   WebAssembly. On an ARMv8.0 core, and with MSVC on ARM64 below
 *   /arch:armv8.1, every atomic add is a retry loop in hardware, so the push
 *   is lock-free there, not wait-free.
 *
 *   A FULL RING refuses the new record. The push returns YRT_ERR_FULL,
 *   writes nothing and counts the refusal. The next drain puts a
 *   YRT_KIND_LOSS record first in its output (source YRT_SRC_RT), with
 *   the number lost since the previous drain in u.u64[0] and the total in
 *   u.u64[1]. A writer that copies every record out therefore cannot omit a
 *   loss. The ring keeps what it already holds and refuses what is new, so
 *   every record that comes out is whole and every gap is counted.
 *   yrt_ring_dropped() gives the total for a status line. With P threads
 *   inside a push at the same instant, a push can be refused when the ring
 *   is within P - 1 records of full.
 *
 *   A STALLED PRODUCER STALLS THE DRAIN. Records come out in ticket order,
 *   so the drain stops at a record whose producer took a ticket and has not
 *   yet written it. A push holds a ticket for a few dozen instructions. But
 *   a normal-priority producer that the OS deschedules inside that window
 *   holds it until it runs again, for a scheduler quantum or longer on a
 *   busy machine. During that time the drain returns nothing, the ring
 *   fills behind the stalled record, and then the ring refuses the next
 *   pushes, from every thread. The audio callback's records are then the
 *   ones that are lost, although the audio thread never waited. A producer
 *   that dies inside a push stops the drain for good. Size the ring for the
 *   longest stall you expect at the record rate you expect: capacity /
 *   rate is the stall the ring absorbs. For example, 4096 records at 6000
 *   records a second absorb a 0.68 s stall (arithmetic, not a measurement).
 *   Watch for LOSS records in the log, and give the ring more memory if any
 *   appear.
 *
 *   MEMORY. You own the memory and the handle; nothing allocates.
 *   YRT_RING_BYTES(n) is the size for n records. The ring aligns the
 *   start to 64 bytes and uses the largest power of two of slots that
 *   fits. yrt_ring_open() zeroes the memory, which also brings every page
 *   in before the first push. There is no close. Stop every producer before
 *   you free the memory or the handle, and detach the ring from the trace
 *   (yrt_trace_set_ring(NULL)) if it is attached.
 *
 *   ONE DRAINER. Call yrt_ring_drain() from one thread at a time. The
 *   ring does not detect a second drainer. A drain copies records out,
 *   frees their slots once per batch, and stops early when it reaches an
 *   unfinished record, so a short count does not mean an empty ring.
 *
 *   The ring is not a threading feature: it exists in a YRT_NO_THREADS
 *   build, because other libraries' threads still push.
 *
 *   ---------------------------------------------------------------------
 *   CLOCK CORRELATION
 *   ---------------------------------------------------------------------
 *   Every time in the rig is on the ysp_rt clock. A library that stamps
 *   with another clock (SDL's tick clock, Windows interrupt time, an audio
 *   device's frame count, an eye tracker) is converted at the boundary, and
 *   yrt_correlate() gives the pair of readings to convert with:
 *
 *       yrt_corr c;
 *       yrt_correlate(&(yrt_corr_desc){ .read = SDL_GetTicksNS }, &c);
 *       uint64_t t = c.rt_ns + (ev.common.timestamp - c.other);
 *
 *   PLAIN MODE reads the ysp_rt clock, the other clock, and the ysp_rt clock
 *   again, `tries` times (16 by default), and keeps the try with the
 *   smallest gap between the two ysp_rt reads. The other read happened
 *   inside that gap, so rt_ns is its middle and width_ns is the gap. The
 *   true ysp_rt time of `other` is within width_ns / 2 of rt_ns. The width
 *   is never 0: it is at least the ysp_rt clock's tick (the QPC period on
 *   Windows, clock_getres() on POSIX, the measured step under Emscripten),
 *   so a weight of 1 / width^2 is always finite. Plain mode cannot see the
 *   other clock's own tick: a 1 ms clock read in a 100 ns bracket is still
 *   wrong by up to 1 ms.
 *
 *   EDGE MODE (.edge = true) is for a coarse clock. It reads in a tight
 *   loop until the value changes, and brackets the change: from the start
 *   of the last read that saw the old value to the end of the first read
 *   that saw the new one. `other` is the new value, at the moment it began.
 *   It keeps the tightest of `tries` edges (4 by default) and spins the
 *   whole time, so the worst case is `tries` ticks of the other clock: 4 x
 *   15.6 ms = 62.5 ms for GetTickCount64 at the default Windows tick, and
 *   4 x 1 ms for a millisecond clock. timeout_ns (100 ms by default) ends
 *   the wait with YRT_ERR_TIMEOUT when the clock does not change at all.
 *   Correlate between trials, not inside a frame.
 *
 *   THE CLOCKS THE RIG NAMES:
 *     SDL3 ticks       .read = SDL_GetTicksNS. Plain mode. SDL2's
 *                      SDL_GetTicks counts milliseconds; use edge mode.
 *     Interrupt time   .read = yrt_interrupt_time_100ns, 100 ns units,
 *                      plain mode, Windows 10 and later; 0 elsewhere. It is
 *                      the clock GetTickCount64() and window message times
 *                      count in (in coarser steps; use edge mode for those).
 *                      It includes time the machine spent asleep.
 *     QPC values       No correlation: yrt_ticks_to_ns() converts them
 *                      exactly. See CLOCK.
 *     An audio device  A frame count, not a time. Read a position that the
 *                      API reports from any thread (WASAPI's
 *                      IAudioClock::GetPosition) through .read_ctx with the
 *                      device in .ctx. Or stamp the callback's own frame
 *                      count with yrt_now_ns() at callback entry and use
 *                      that pair as it is. To turn frames into time you also
 *                      need the device's true rate, which only repeated pairs
 *                      over a long time give: ysp/audio.h fits it itself,
 *                      and DEVICE CLOCK FIT below does it for other devices.
 *
 *   Push a correlation into the log as a YRT_KIND_CLOCK record, so the
 *   analysis can convert the other clock's stamps again later.
 *
 *   ---------------------------------------------------------------------
 *   DEVICE CLOCK FIT (yrt_fit)
 *   ---------------------------------------------------------------------
 *   A device with its own clock (a Cedrus XID box's ms timer, a
 *   microcontroller's micros(), an eye tracker) stamps its events with
 *   that clock. yrt_fit maps those stamps to the ysp_rt clock. Feed it
 *   pairs: the device's counter and the ysp_rt time that goes with it.
 *
 *       static yrt_fit fit;                  // about 40 KB: not on a stack
 *       yrt_fit_open(&fit, &(yrt_fit_desc){ .mode = YRT_FIT_LATE,
 *                                                .ns_per_tick = 1000.0,
 *                                                .tick_bits = 32 });
 *       // the reader thread, for each sync frame:
 *       yrt_fit_add(&fit, frame_us, (int64_t)t_read, 0);
 *       // for each event the device stamped:
 *       ev.t = yrt_fit_map(&fit, event_us);
 *
 *   MODES (desc.mode), by what the pair's host time is worth:
 *     LATE      The host time is when the reader got the bytes: late by
 *               the device's send, the USB poll, a bridge chip's latency
 *               timer, the driver and the thread's wake-up, never early.
 *               The map is the line under every point with the smallest
 *               summed gap, the edge of the lower convex hull that spans
 *               the mean x (Moon, Skelly and Towsley, INFOCOM 1999), as
 *               ysp/audio.h fits callback times. It finds the least late
 *               deliveries, so the map is the device's time plus the
 *               path's MINIMUM delay, which only a loopback measures.
 *     BRACKET   The device's time was read inside a bracket: a timer query
 *               sent at t0 and answered by t1, or a register latched by a
 *               write between t0 and t1. host_ns = t1, width_ns = t1 - t0.
 *               Brackets wider than max_width_ns (1.3 ms by default, as
 *               Psychtoolbox's DataPixx sync) are refused; the rest are
 *               fitted as LATE pairs, so per bucket the pair with the
 *               smallest t1 minus device time wins (PsychDataPixx's
 *               filter). The map is late by 0 to the width of the pairs it
 *               rests on (info.width_ns). Probing (how often, a random
 *               sub-millisecond wait between tries to leave the USB frame's
 *               phase) is the caller's.
 *     UNBIASED  The host time scatters both ways around the truth: a
 *               position report the API stamped at the source. Bucket
 *               means, least squares.
 *   THE WINDOW. One point per bucket of host time (desc.bucket_ns, 200
 *   ms): the least late pair (LATE, BRACKET) or the mean (UNBIASED).
 *   YRT_FIT_POINTS (1024) points, 205 s at 200 ms; the oldest leaves.
 *   The map is anchored at the newest point, so a recent event is mapped
 *   over a short distance. Until the points span desc.slope_span_ns (5 s)
 *   the slope stays nominal (ns_per_tick) and the offset comes from the
 *   last second's points, because a slope from a shorter span was worse
 *   than the drift it corrects (ysp/audio.h measured it); after that the
 *   slope is fitted, and info.ppm says how far the device's crystal is
 *   from nominal. A refit happens at every new bucket, at every pair
 *   while the window has 16 points or fewer, and when a LATE or BRACKET
 *   pair lies under the current line; otherwise a pair costs about 0.1 us
 *   (STATUS).
 *   WRAP. desc.tick_bits = 32 for a uint32 counter: XID's ms timer wraps
 *   after 49.7 days, micros() after 71.6 minutes. yrt_fit_add() unwraps
 *   a counter by the time the map expects at the pair's host time;
 *   yrt_fit_map() and yrt_fit_unwrap() unwrap against the newest pair,
 *   so map an event within half a wrap period of it (35 minutes for
 *   micros()). Pass the counter as the device sent it.
 *   RESTART. A pair farther than desc.restart_ns (50 ms) from the map is
 *   held, not used (YRT_FIT_PENDING). Held pairs that agree with each
 *   other at the nominal tick for desc.restart_hold_ns (1 s) are a new
 *   clock: the device's timer was reset (XID's e5), the board rebooted.
 *   The fit starts a new epoch from them (YRT_FIT_RESTART, info.epoch),
 *   with the slope nominal again. A normal pair ends the hold, and the
 *   held pairs count as rejected: a burst of late pairs after an OS stall
 *   arrives within milliseconds, so it never holds for a second. Map an
 *   event with the epoch it belongs to: an event stamped before a restart
 *   and mapped after it maps wrong. The nominal tick must be right to
 *   restart_ns over the longest gap between pairs in the first
 *   slope_span_ns, or the first pairs look like a new clock.
 *   ERROR. Synthetic devices in STATUS: a map within tens of us of the
 *   truth for dense LATE pairs, within 2 ns of the estimator's exact
 *   value. Few pairs behind a coarse timer (a press every few seconds
 *   behind a 16 ms FTDI latency timer) give ms errors for minutes; such a
 *   device needs BRACKET probes or a lower latency timer. info.spread_ns is
 *   the p99 distance of the points from the map (above it for LATE and
 *   BRACKET): a stamp's uncertainty for ysp/input.h's unc_us, not an
 *   error bound of the map; only a loopback gives that.
 *   RECORDS. With desc.ring, each closed bucket's point is a
 *   YRT_KIND_CLOCK record (the device's unwrapped ticks, the width, the
 *   epoch; aux = desc.clock_id) and each refit at a new bucket a
 *   YRT_KIND_FIT record (the anchor, ns per tick, the spread, the
 *   epoch, the points). An analysis can fit the whole session again from
 *   the CLOCK records, with the points after each event too, which an
 *   online fit cannot see. At most one of each per bucket.
 *   THREADS. A fit belongs to one thread (the device's reader); it takes
 *   no lock and allocates nothing. Exists in a YRT_NO_THREADS build.
 *
 *   ---------------------------------------------------------------------
 *   PER-USER FOLDERS (yrt_user_dir)
 *   ---------------------------------------------------------------------
 *   Where a rig keeps files that belong to the user of the machine and not
 *   to an experiment: a program cache, a rig profile, a calibration.
 *
 *       char dir[512];
 *       if (yrt_user_dir(YRT_DIR_CONFIG, "rig", dir, sizeof dir) == YRT_OK)
 *           ... open dir/profile.json, after making dir if it is missing
 *
 *   KINDS. YRT_DIR_CACHE: disposable files that a program can make again
 *   (compiled programs); the OS or the user may delete them. YRT_DIR_CONFIG:
 *   files to keep (a rig profile, device bindings). The base folder:
 *                 CACHE                         CONFIG
 *     Windows     %LOCALAPPDATA%                %APPDATA%
 *     Linux       $XDG_CACHE_HOME, if absolute,  $XDG_CONFIG_HOME, if
 *                 else $HOME/.cache             absolute, else $HOME/.config
 *     macOS       $HOME/Library/Caches          $HOME/Library/Application Support
 *     the web     none                          none
 *   then ysp, then subdir: "%LOCALAPPDATA%\ysp\progcache",
 *   "$HOME/.config/ysp/rig". subdir is NULL or "" for the ysp folder itself,
 *   or names separated by '/' (Windows gets '\'); an empty name, "." or
 *   "..", a backslash, a colon or a control character is refused.
 *   RULES. Absolute paths only: a variable that is missing or relative is
 *   no folder, and no other folder is tried. On Windows the variable is
 *   read with the wide API and given as UTF-8 (a user name with accents
 *   works); a drive and a root, or a UNC path. On POSIX each part of the
 *   path that exists, from the root, is refused when all users can write
 *   it (/tmp is 1777) or when another user than this one or root owns it
 *   (a folder planted under /tmp before the first run): a program cache
 *   that someone else can write can be given an entry with a correct
 *   hash, and the driver parses it. On the web there is no folder. The
 *   function makes no folder and writes no file; it reads the environment
 *   and, on POSIX, the owner and mode of the parts that exist.
 *   RESULT. YRT_OK and the path in out (no trailing separator), or
 *   YRT_ERR_ARG with out empty: NULL out or cap 0 (out untouched then), an
 *   unknown kind, a refused subdir, a missing or refused base, a path that
 *   does not fit in cap bytes with its NUL. Then use no folder: never fall
 *   back to a shared one.
 *
 *   ---------------------------------------------------------------------
 *   INSTRUMENTATION
 *   ---------------------------------------------------------------------
 *   Macros that mark what the code is doing, for a profiler or for the log:
 *
 *     YRT_ZONE(z, "name")      begin a zone; declares a local named z
 *     YRT_ZONE_VALUE(z, n)     attach a number to it
 *     YRT_ZONE_END(z)          end it
 *     YRT_FRAME_MARK()         the flip
 *     YRT_FRAME_MARK_NAMED(s)  the flip of a second display
 *     YRT_PLOT("name", x)      one point of a numeric plot
 *     YRT_MESSAGE("text")      a message, a string literal
 *     YRT_MESSAGEF(fmt, ...)   a formatted message
 *     YRT_THREAD_NAME("name")  name the calling thread
 *     YRT_THREAD_INIT("name")  name it and set up its per-thread state
 *     YRT_ZONE_SCOPED("name")  C++ only: a zone that ends with the scope
 *
 *       YRT_ZONE(z, "draw");
 *       draw_stimuli();
 *       YRT_ZONE_END(z);
 *       YRT_FRAME_MARK();
 *
 *   C has no destructors, so a zone is a pair. YRT_ZONE is a declaration:
 *   put it at block scope, as a statement of its own, not as the body of an
 *   unbraced if. Every path out of the zone needs its YRT_ZONE_END. Every
 *   name is a string literal, because the trace keeps the pointer. Do not
 *   give a macro an argument with a side effect: what it does depends on
 *   the mode.
 *
 *   THE MODE is one define for the whole program, on the compiler command
 *   line, because the first include of this header decides it:
 *
 *     (none)            Every macro is ((void)0) or ((void)sizeof(x)). No
 *                       code, and no argument is evaluated. sizeof is what
 *                       keeps a value computed only for a plot from being an
 *                       unused variable under -Werror.
 *     YRT_TRACE_RING  The macros push records into the trace ring set by
 *                       yrt_trace_set_ring(). One ZONE record per zone, at
 *                       its end, with the begin time, the duration and the
 *                       value; a zone left by an early return is not
 *                       recorded. FRAME, PLOT, MESSAGE (at most 39
 *                       characters) and THREAD_NAME push one record each.
 *                       With no ring attached a macro does one pointer load
 *                       and reads no clock. A rig without Tracy still gets
 *                       its zones in its log.
 *     YRT_TRACY       The macros call Tracy's C API (<tracy/TracyC.h>, or
 *                       the header YRT_TRACY_HEADER names). Define
 *                       TRACY_ENABLE too, or the build stops: without it
 *                       TracyC.h compiles every zone away and the profiler
 *                       shows nothing. Compile and link Tracy's
 *                       TracyClient.cpp into the program. The frame mark
 *                       also plots "yrt_ns", the ysp_rt time, so a
 *                       capture can be put on the rig's clock.
 *
 *   YRT_TRACY and YRT_TRACE_RING together stop the build.
 *   YRT_TRACE_WEB is reserved for a browser backend that does not exist
 *   yet, and also stops the build.
 *
 *   A REAL-TIME THREAD calls YRT_THREAD_INIT once, at its start, before it
 *   is on a deadline. Tracy allocates a queue for each thread on its first
 *   event, and the ring mode asks the OS for the thread id on its first
 *   record; both belong at the start, not in the first audio callback. The
 *   worker and the pump do it for their own threads before their start
 *   functions return.
 *
 *   What ysp/rt.h instruments itself: a "yrt.worker.job" zone around
 *   each worker job (value: its lateness in ns), "yrt.pump.msg" (value:
 *   the seq) and "yrt.pump.idle" around the pump's callbacks, and
 *   "yrt.sleep" around the wait in yrt_sleep_until(). The mode of the
 *   translation unit that defines YSP_RT_IMPLEMENTATION decides whether
 *   these exist. The trace-ring functions are always compiled in, so any
 *   other translation unit may use YRT_TRACE_RING whatever that mode is.
 *
 *   ---------------------------------------------------------------------
 *   WEBASSEMBLY
 *   ---------------------------------------------------------------------
 *   Under Emscripten (__EMSCRIPTEN__) the header compiles as a subset of its
 *   POSIX path. What a wasm module can and cannot have, call by call:
 *
 *     Clock    clock_gettime(CLOCK_MONOTONIC), which Emscripten backs with
 *              performance.now() in a browser and the high-resolution timer
 *              in node. Browsers COARSEN performance.now() to between 5 us
 *              and 100 us (by browser and version) unless the page is
 *              cross-origin isolated (COOP and COEP headers), which also
 *              enables threads; an isolated page gets a finer clock, how much
 *              finer being the browser's decision, which is why it is
 *              measured below and not stated here. Every
 *              read is a call out to JavaScript, so it costs far more than a
 *              native read. Emscripten's clock_getres() reports 1 ns whatever
 *              the host does, so on this platform, and only here,
 *              yrt_get_clock_info() MEASURES the smallest step two reads
 *              can see and reports that instead. Log it: it is the
 *              resolution of every timestamp you take.
 *     Waits    yrt_sleep_until() is the relative nanosleep loop macOS uses,
 *              then the spin. In a pthread (a Web Worker) or on node's main
 *              thread that is a real blocking wait on a futex. On a
 *              BROWSER'S MAIN THREAD it is not usable: blocking there freezes
 *              rendering and input, and Emscripten turns a main-thread sleep
 *              into a busy-wait. The browser frame loop belongs to
 *              requestAnimationFrame, which is where a stimulus is drawn; the
 *              waits in this header are for worker threads, node, and tools.
 *              A build WITHOUT -pthread has no futex at all, and every sleep
 *              in it is a busy-wait even under node.
 *     Ladder   Collapsed. A wasm thread is a Web Worker and no page can ask
 *              for its priority, so yrt_thread_elevate() returns
 *              YRT_POLICY_NORMAL without trying (Emscripten's scheduling
 *              stubs report success and would be believed otherwise);
 *              yrt_thread_pin(), yrt_thread_set_timer_slack(),
 *              yrt_process_lock_memory() and
 *              yrt_timer_resolution_begin() return false; and
 *              yrt_describe() ends its line with "platform=wasm
 *              ladder=none" so a log cannot mistake NORMAL for a refusal. The
 *              pump's below-normal rung is YRT_POLICY_NORMAL here.
 *     Threads  The worker and the pump need -pthread, and in a browser the
 *              page must be cross-origin isolated for SharedArrayBuffer to
 *              exist. Built without -pthread they still compile and link
 *              (Emscripten provides pthread stubs), and open() fails with a
 *              message that names the missing flag; YRT_NO_THREADS drops
 *              them as on every other platform. With -pthread, build the
 *              program with -sPROXY_TO_PTHREAD=1 so main() itself runs on a
 *              pthread and may block, and -sEXIT_RUNTIME=1 so its return code
 *              reaches node. This is the configuration the tests run in:
 *
 *       emcc -O2 -pthread -sPROXY_TO_PTHREAD=1 -sEXIT_RUNTIME=1 -Iinclude \
 *            -o rt_pump.js examples/rt/pump.c && node rt_pump.js
 *
 *              Node 24 runs threaded modules with no extra flag. The
 *              adaptive headers' tests also want -sSTACK_SIZE=16MB,
 *              -sINITIAL_MEMORY=64MB and -sALLOW_MEMORY_GROWTH=1; with
 *              -pthread, emcc warns that memory growth is slower for
 *              JavaScript code (-Wpthreads-mem-growth), which does not touch
 *              anything here and can be silenced.
 *
 *   What this buys a browser experiment is the pump: the frame loop stays in
 *   requestAnimationFrame on the main thread, a QUEST+ or GP update runs on
 *   a pump thread, and the frame callback polls yrt_pump_done_seq() exactly
 *   as a native loop does. No timing claim in this header survives the trip
 *   to a browser unchanged; run examples/rt/jitter.c under the runtime you
 *   will use and log its ysp_rt: line.
 *
 *     Ring     With -pthread the ring's atomics are wasm's, and a push from
 *              a pthread or an AudioWorklet thread is wait-free as on a
 *              native core. Without -pthread there is no shared memory and
 *              emcc turns the atomics into plain loads and stores, which is
 *              correct for the one thread there is. yrt_thread_id() is
 *              the pthread handle. yrt_correlate()'s width floor is the
 *              measured clock step, taken once per process (up to 5 ms).
 *
 *   ---------------------------------------------------------------------
 *   BUILDING
 *   ---------------------------------------------------------------------
 *   POSIX builds link -pthread: the worker needs it, and so does the
 *   scheduling ladder, which raises the calling thread through
 *   pthread_setschedparam even when the worker is compiled out. On glibc
 *   older than 2.17 also link -lrt for clock_gettime. macOS needs nothing
 *   extra. Windows needs no import library: winmm is loaded at runtime and
 *   only by yrt_timer_resolution_begin().
 *
 *   Define YRT_NO_THREADS to drop the worker and the pump. That removes
 *   yrt_worker, yrt_pump and every yrt_worker_* / yrt_pump_*
 *   declaration, so code that uses them must guard the same way. The clock,
 *   the waits, the whole scheduling section, the event ring, the clock
 *   correlation, the instrumentation macros, the YRT_ERR_* codes and
 *   yrt_strerror() stay: none of them starts a thread.
 *
 *   Define YRT_TRACE_RING or YRT_TRACY, for the whole program, to turn
 *   the instrumentation macros on; see INSTRUMENTATION. YRT_TRACY needs
 *   TRACY_ENABLE, Tracy's public/ directory on the include path and
 *   TracyClient.cpp in the build:
 *
 *       c++ -O2 -DTRACY_ENABLE -DTRACY_ON_DEMAND -Itracy/public \
 *           -c tracy/public/TracyClient.cpp
 *       cc -O2 -pthread -DYRT_TRACY -DTRACY_ENABLE -DTRACY_ON_DEMAND -Iinclude \
 *           -Itracy/public -c app.c
 *       c++ -pthread -o app app.o TracyClient.o -ldl
 *
 *   Define YRT_PUMP_INLINE_BYTES to size the ring a pump gets when
 *   yrt_pump_desc.ring is NULL (4096 by default). It sits inside every
 *   yrt_pump handle whether or not it is used, so a program that always
 *   supplies its own ring can set it to 1 and a program with 8 KB messages
 *   raises it. It must be the same in every translation unit that sees a
 *   yrt_pump.
 *
 *   Define YRT_API to override the default `extern` linkage.
 *
 *       cc -O2 -pthread -Iinclude -o rt_jitter examples/rt/jitter.c
 *       cl /O2 /Iinclude examples\rt\jitter.c
 *       emcc -O2 -pthread -sPROXY_TO_PTHREAD=1 -sEXIT_RUNTIME=1 -Iinclude \
 *            -o rt_jitter.js examples/rt/jitter.c     # then: node rt_jitter.js
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.9.0 - VIRTUAL CLOCK: yrt_virtual_begin(), yrt_virtual_end(),
 *          yrt_is_virtual(), yrt_virtual_timer(), yrt_virtual_cancel(),
 *          YRT_VIRTUAL_T0_NS, YRT_VIRTUAL_TIMERS and YRT_ERR_STATE;
 *          yrt_real_ns(), the real clock for costs on either clock. The
 *          examples' --sim runs compared against the real clock and failed
 *          on loaded CI runners (a dropped frame moved an onset); on the
 *          virtual clock they give the same output on every run. The real
 *          clock: yrt_now_ns() and the waits test one flag first.
 *   v0.8.0 - The ysp conventions (README.md, Conventions); breaks the API.
 *          Every time is int64_t ns: yrt_now_ns(), yrt_sleep_until(),
 *          yrt_sleep_ns(), yrt_spin_until(), yrt_worker_submit(),
 *          yrt_pump_wait(), yrt_event.t_ns, the zone's dur_ns, yrt_corr,
 *          yrt_job_info, yrt_sched_deadline, yrt_clock_info and
 *          yrt_report. yrt_now_us() is gone. Unit macros YRT_NS(),
 *          YRT_US(), YRT_MS(), YRT_S(), YRT_TO_MS() and YRT_TO_S().
 *          Open and close: yrt_worker_start/stop() and
 *          yrt_pump_start/stop() are yrt_worker_open/close() and
 *          yrt_pump_open/close(); yrt_fit_init() is yrt_fit_open(), which
 *          returns bool with the reason in yrt_fit_error(), and
 *          yrt_fit_close() is new; yrt_ring_close() is new.
 *          yrt_sched_normalize(), yrt_thread_set_timer_slack(),
 *          yrt_thread_pin(), yrt_process_lock_memory() and
 *          yrt_timer_resolution_begin() return 0 or a negative code
 *          (YRT_ERR_UNSUPPORTED, YRT_ERR_REFUSED, YRT_ERR_ARG) instead of
 *          bool.
 *   v0.7.0 - Per-user folders: yrt_user_dir(YRT_DIR_CACHE or
 *          YRT_DIR_CONFIG, subdir, out, cap) gives <base>/ysp/<subdir>,
 *          the base from the platform's per-user cache or config location,
 *          checked as ysp/gfx.h's ygfx_default_cache_dir() checked its
 *          folder (that function is now a wrapper of this one). See
 *          PER-USER FOLDERS. Nothing else changed.
 *   v0.6.0 - The device clock fit: yrt_fit maps a device's own counter
 *          (a response box's ms timer, a board's micros(), a tracker's
 *          clock) to the ysp_rt clock from (ticks, host time) pairs. Three
 *          estimators: LATE (the lower envelope, for stamps that are late
 *          and never early), BRACKET (the least late of bracketed reads,
 *          wide brackets refused) and UNBIASED (least squares). Counter
 *          unwrap, a restart on a clock reset, a fixed window of 1024
 *          points, no allocation. With a ring it logs the thinned pairs
 *          (YRT_KIND_CLOCK) and each fit (YRT_KIND_FIT, new) for an
 *          offline refit. YRT_SRC_DEVICE (12) reserved for ysp/device.h.
 *          See DEVICE CLOCK FIT. Nothing else changed.
 *   v0.5.0 - The event ring: yrt_ring, a wait-free ring of 64-byte
 *          yrt_event records in memory the caller supplies, pushed from
 *          any number of threads (an audio callback included) and drained
 *          by one. A full ring refuses the new record, counts it, and the
 *          next drain reports the count in band as a YRT_KIND_LOSS
 *          record. See EVENT RING.
 *          yrt_correlate(): the ysp_rt clock against another clock read
 *          between two of its own reads, the tightest of N tries, with the
 *          bracket's width; an edge mode for coarse clocks.
 *          yrt_interrupt_time_100ns(), the Windows interrupt-time clock,
 *          and yrt_ticks_to_ns(), the exact conversion yrt_now_ns()
 *          applies to a QPC value. yrt_thread_id(). See CLOCK
 *          CORRELATION.
 *          The instrumentation macros (YRT_ZONE, YRT_FRAME_MARK,
 *          YRT_PLOT, YRT_MESSAGE and the rest): nothing by default, the
 *          Tracy C API with YRT_TRACY, records in a trace ring with
 *          YRT_TRACE_RING. The worker, the pump and yrt_sleep_until()
 *          are instrumented. See INSTRUMENTATION.
 *          YRT_OK, the YRT_ERR_* codes and yrt_strerror() are now
 *          outside the YRT_NO_THREADS guard, because the ring and the
 *          correlation use them; a no-threads build gains them and loses
 *          nothing. YRT_ERR_FULL's text is "ring full, not taken", for
 *          the pump and the event ring alike.
 *          Windows 11 power throttling: yrt_timer_resolution_begin() also
 *          opts the process out of the timer throttling a minimized process
 *          gets, and yrt_thread_elevate() and the pump thread opt their
 *          thread out of EcoQoS. yrt_report gains timer_throttle_off,
 *          ecoqos_off and throttle_known, and the yrt_describe() line ends
 *          in timer_throttle= and ecoqos= on every platform. See POWER
 *          THROTTLING under SCHEDULING.
 *          yrt_cpu_core_type() and yrt_core_type_name(): P-core or
 *          E-core, from Windows' EfficiencyClass. yrt_thread_elevate()
 *          prefers the P-cores on a hybrid CPU (Windows), and
 *          yrt_thread_pin() records what it pinned. yrt_report gains
 *          pinned_cpu, pinned_core and pcores_preferred, and the line
 *          gains pin= and cores=. A 256-byte buffer now always holds the
 *          line. See CORE TYPES under SCHEDULING.
 *   v0.4.1 - MinGW: the worker and pump threads start with the x87 control
 *          word of the thread that started them, not Windows' default, so
 *          x87 libm code (mingw-w64's msvcrt exp and pow) gives the same bits
 *          on the pump as on the main thread.
 *   v0.4.0 - An Emscripten branch, so the pump, the worker and the adaptive
 *          headers' async layers run in node and in a browser worker. It is
 *          the POSIX path with the Linux- and Mach-only calls taken out: the
 *          scheduling ladder collapses to YRT_POLICY_NORMAL, pinning,
 *          timer slack, memory locking and timer resolution report false,
 *          sleeps are the relative nanosleep loop, and
 *          yrt_get_clock_info() measures the clock's step instead of
 *          trusting Emscripten's clock_getres(). yrt_describe() adds
 *          "platform=wasm ladder=none" there and nowhere else. A worker or
 *          pump started in a module built without -pthread fails with a
 *          message naming the flag. No other platform changes behavior. See
 *          WEBASSEMBLY.
 *   v0.3.1 - yrt_pump_wait() is safe against a concurrent yrt_pump_stop()
 *          or yrt_pump_start() at ANY point, not only once it has parked.
 *          v0.3 checked for a live pump and then took its mutex, and a stop
 *          landing between the two destroyed the mutex under it; a binding
 *          had to slice its waits and defer its stop to cover that. A wait now
 *          registers in an atomic counter before it looks, a stop closes the
 *          door, releases every registered waiter and destroys nothing until
 *          the counter reads zero, and a restart never plain-writes the
 *          fields a racing wait reads. A wait racing a DRAINING stop now also
 *          waits for the drain, so a seq already in the queue returns 0
 *          instead of YRT_ERR_STOPPED. See STOP under PUMP.
 *          YRT_VERSION_MAJOR / _MINOR / _PATCH / _STRING and
 *          yrt_version(), so a binding reads its version from the header.
 *   v0.3 - yrt_pump, a background-compute worker: one thread, a fixed-size
 *          message ring, a callback per message in submit order, an idle
 *          callback for spare work, a "done through seq" the frame loop polls
 *          or waits on, and a lock the caller shares with the callbacks. It is
 *          the opposite end of yrt_worker: that one runs a short job AT a
 *          deadline at the top of the scheduling ladder, this one runs a long
 *          job BETWEEN deadlines at or below normal priority. See PUMP.
 *          Two new return codes, YRT_ERR_FULL and YRT_ERR_TIMEOUT, and a
 *          new bottom rung YRT_POLICY_BELOW_NORMAL, appended to
 *          yrt_policy so every existing value keeps its number. Only the
 *          pump ever returns it; yrt_thread_elevate() never lowers.
 *          yrt_strerror() names both new codes, and the YRT_ERR_STOPPED
 *          text now says "worker or pump" because both use it.
 *          The worker is unchanged.
 *   v0.2 - yrt_worker_desc.on_start, a hook that runs ON THE WORKER THREAD
 *          after elevation and before yrt_worker_start() publishes
 *          readiness, and fails the start when it returns false. The
 *          parallel port's DIRECT backend needs exactly that: Linux grants
 *          port I/O permission per THREAD, so the grant has to happen on the
 *          worker and a refusal has to fail ypar_open() rather than surface
 *          one trial later.
 *          yrt_job_info.seq and yrt_worker_pending(). Every submit
 *          stamps the job with a sequence number and RETURNS it, so a job
 *          can tell whether it is still the one a caller is waiting for.
 *          yrt_worker_submit() therefore returns a positive seq on success
 *          instead of YRT_OK; failures are still negative YRT_ERR_*, so
 *          the test is `rc < 0`, not `rc != YRT_OK`.
 *          The Linux feature-test macro is now _DEFAULT_SOURCE, and it is
 *          set on the FIRST include of this header rather than only in an
 *          implementation translation unit. Including ysp/rt.h before a
 *          transport header no longer hides what that transport needs
 *          (CRTSCTS, realpath) behind strict POSIX.
 *          The compile-time assertions fall back to a negative-array-size
 *          typedef, and _Alignof to a struct offset, before C11. MSVC's
 *          default C dialect, which is what setuptools and mex compile with,
 *          now builds this header without /std:c11.
 *   v0.1 - first release.
 *
 *   STATUS: v0.7.0. yrt_user_dir() (v0.7.0) is built with warnings as errors
 *   by MSVC, MinGW gcc and gcc on WSL2, and by emcc in CI's wasm job;
 *   tests/adapt/rt_test.c checks both kinds against environments it sets
 *   (Windows: non-ASCII, UNC, relative, missing, a trailing slash, the exact
 *   fit; POSIX: XDG absolute and relative, HOME, /tmp refused; the web:
 *   always refused) and the subfolder rules, and ysp/gfx.h's test of
 *   ygfx_default_cache_dir() passes unchanged. Its mutants are in
 *   tests/mutate/rt.toml.
 *   The device clock fit (v0.6.0) is built with warnings
 *   as errors by MSVC 19.44 (/W4 /WX, C11 and C++17), MinGW-w64 gcc 16.1
 *   (C99, C11, C++17) and gcc 11.4 on WSL2 (C99, C++17), and its tests in
 *   tests/adapt/rt_test_fit.h pass on all three, also under ASan and
 *   UBSan on WSL2. They run seeded synthetic devices against the true line
 *   (computed in long double) and against a brute-force envelope and the
 *   closed form of least squares (both within 2 ns). Map minus truth at
 *   each new pair's tick, over four seed sets, from 10 s on and (in
 *   brackets) from 205 s on, when the window is full: LATE, a ms timer at
 *   100 pairs a second behind a 1 ms USB frame, an exponential tail, 16 ms
 *   bursts and 60 to 200 ms stalls, -75 to +240 us (-47 to +91); LATE, a
 *   us counter that wraps, -12 to +46 us (-5 to +18); BRACKET, 0.4 to 3 ms
 *   brackets every 100 ms, -327 to +628 us (-42 to +110); UNBIASED, SD
 *   115 us, -23 to +22 us (-5 to +7); LATE, a press every 2 to 4 s behind a
 *   16 ms latency timer, up to +8.7 ms at 2 min and -1.4 to +3.2 ms from
 *   205 s. Before the slope (the first 5 s), a 500 ppm clock: -541 to +160
 *   us at the pair, -641 to +60 us 200 ms ahead. Rates within 0.9 ppm of
 *   the truth at the end of each run. A counter reset gives
 *   one restart 1.0 to 1.3 s later; a 300 ms stall gives none. With a full
 *   window a pair costs 0.1 us at p99 without a refit, and a refit 9 to 23
 *   us at p50 (up to 50 us max; one per 200 ms bucket), measured with gcc
 *   and MSVC on the laptop below. 17 of 17 mutants are caught
 *   (tests/mutate/rt.toml). No device has fed it: every number here is
 *   synthetic.
 *   v0.5.0's event ring, the correlation and the macros (v0.5.0)
 *   are built with warnings as errors by MSVC 19.44 (/W4 /WX, its default C
 *   dialect, /std:c11 and /std:c++17), by MinGW-w64 gcc 16.1 (C99, C11,
 *   C++17) on Windows 11, and by gcc 11.4 on WSL2 (C99, C11, C++17), in all
 *   three macro modes; also by MSVC for 32-bit x86, and by emcc 6.0.10 (C11
 *   and C++17, with and without -pthread, with YRT_NO_THREADS and with
 *   YRT_TRACE_RING). YRT_TRACY is built and run against Tracy v0.14.1
 *   by MSVC and gcc. tests/adapt/rt_test.c, which runs in
 *   YRT_TRACE_RING mode, passes on all of them (under node with -pthread)
 *   and is clean under ThreadSanitizer and ASan+UBSan on Linux. It checks open and sizing, order, seq, the stamps,
 *   a full ring and its LOSS record, and a stress run of 2, 4 and 8
 *   producers pushing 20,000 records each into 256 slots against one
 *   drainer, where every record carries words derived from (producer,
 *   index): no torn record, no duplicate, per-producer order, no seq gap,
 *   and drained + refused equal to pushed exactly, with the LOSS records
 *   adding up to the refusals. Three deliberately broken copies of the
 *   header are caught by it: publishing before the copy (every run, and
 *   by TSan), no admission check (every run), and freeing the slots before
 *   copying them out (2 runs of 3). The trace test checks every macro and
 *   the zone and thread name the worker emits; the correlation test checks
 *   a known offset, a 1 ms clock's edge within half the width, the
 *   timeout, and on Windows that interrupt time runs at the QPC rate and
 *   that yrt_ticks_to_ns() brackets yrt_now_ns() and matches a 128-bit
 *   reference. examples/rt/ring_bench.c measured, on a 16-thread Windows 11
 *   machine (MSVC and MinGW) and in WSL2 on the same machine (8 vCPUs),
 *   per push: 11 to 16 ns from one thread with the stamps set (14 to 37 ns
 *   in WSL2) and 37 to 43 ns with t_ns and tid stamped by the ring; 0.4 to
 *   0.6 us mean with 8 threads pushing back-to-back on Windows (1.5 us in
 *   WSL2, where 9 threads share 8 vCPUs). A TIME_CRITICAL producer pushing
 *   once a millisecond while 7 others hammered took 1.5 to 2.2 us at the
 *   99.9th percentile, clock reads included, against 115 to 553 us for the
 *   same records behind a lock. Drain: 1.5 to 6 ns per record. A zone in
 *   YRT_TRACE_RING mode costs 52 to 95 ns with a ring attached and under
 *   1 ns over an empty loop without one (471 ns attached under node, where
 *   one clock read costs 190 ns); the default mode is the empty
 *   loop; Tracy v0.14.1 costs 2 to 10 ns per zone on demand and not
 *   connected, 65 to 90 ns buffering. Correlation: a 100 ns width (one QPC
 *   tick, the floor) against QPC and interrupt time on Windows and 41 to
 *   46 ns against CLOCK_MONOTONIC_RAW in WSL2, 1 to 2 us per call; an edge
 *   of GetTickCount64 to 100 to 200 ns, in 62 ms for 4 edges. The
 *   head-of-line stall in EVENT RING is real: with producers at 1.1 million
 *   records a second, drains waited up to 5 ms on a producer descheduled
 *   inside its push, and the elevated producer's pushes were refused (up
 *   to 684 of 5000); with a stall injected at 6000 records a second the
 *   refusals began between 0.6 and 0.8 s, as 4096 / 6000 predicts. As root
 *   under WSL2 the bench's worker obtained YRT_POLICY_DEADLINE. The
 *   tables are in docs/rt.md.
 *   POWER THROTTLING was measured on an i7-1360P laptop (4 P-cores, 8
 *   E-cores) under Windows 11 25H2 build 26200, on AC power, by a probe
 *   that owns a window, visible, minimized and covered by a second
 *   process's window, with 12 s to settle. Minimized, timeBeginPeriod(1)
 *   waits of 1 and 2 ms woke 13 to 14 ms late (3 of 3 runs); with the
 *   IGNORE_TIMER_RESOLUTION opt-out, 0.44 to 0.76 ms. The high-resolution
 *   timer that yrt_sleep_until(), the worker and the pump use woke 0.26 to
 *   0.46 ms late (p50) in every state, with or without either opt-out.
 *   Minimized, 99 to 100% of the threads ran on E-cores; one computation
 *   alternated on one thread took 1.12 to 1.22 ms opted out of EcoQoS
 *   against 1.42 to 1.60 ms not (2 runs), and 7 to 12% less while visible.
 *   SetProcessInformation and SetThreadInformation returned success for
 *   every mask, also in a 32-bit process, where GetThreadInformation cannot
 *   read the state back (ERROR_INVALID_PARAMETER) and the report uses what
 *   this header set. CORE TYPES was measured on the same laptop with 8
 *   spinning threads of a second process loading all cores, alternating
 *   the setting every 200 ms on the same threads for 30 s, two runs each
 *   visible and minimized. With the EcoQoS opt-out alone, the elevated
 *   frame thread and the worker ran on E-cores 55 to 90% of the time;
 *   steered to every P-core CPU set, 0%, their fixed computation was 9 to
 *   25% faster at p50, and their wake p99 was lower in 7 of 8 pairs (by 3
 *   to 15%, and 9% higher in the eighth). Splitting the P-cores so the two
 *   elevated threads never share a physical core measured the same as
 *   steering both to all of them. The pump, steered, was 8 to 20% faster
 *   but its wake p99 was 14.5 ms in 1 run of 4 against 0.14 ms. Without
 *   the load every thread already ran on P-cores 92 to 100% of the time.
 *   Covering the window throttled the timer in 1 of 19 runs
 *   only, and battery power was not available to test.
 *   The Windows path is built and measured on Windows 11 by
 *   examples/rt/jitter.c, and also builds in MSVC's default (pre-C11) C mode,
 *   which is what the Python and MEX bindings compile with. The Linux path is
 *   built as C11, as C++17 and with YRT_NO_THREADS (warnings as errors) and
 *   run under a WSL2 kernel, including a ThreadSanitizer run of the worker's
 *   submit, replace, cancel, flush, seq and on_start paths. The macOS path
 *   has not been compiled or run at all; CI compiles it.
 *   The pump is built and run on Windows 11 (MSVC /W4 /WX) and on the same
 *   WSL2 kernel as C11, C99 and C++17, clean under ThreadSanitizer and under
 *   AddressSanitizer with UndefinedBehaviorSanitizer, by
 *   tests/adapt/rt_test.c (order, seq, done_seq, wait, ERR_FULL,
 *   on_idle, drain, drop, the publish lock over a few thousand messages, the
 *   inline and the caller-supplied ring, on_start refusal, a stop releasing a
 *   blocked wait, a zeroed handle, a double stop, a submit after stop) and by
 *   examples/rt/pump.c. The v0.3.1 wait-versus-stop guarantee is checked by a
 *   hammer in the same test (two threads in a loop of 0.3 ms waits while the
 *   main thread stops and restarts the pump 300 times, idle, draining and
 *   dropping), clean under ThreadSanitizer on Linux and run on Windows; a
 *   copy of the header whose stop skips the waiter drain is caught by the
 *   same TSan run, so the clean result is not an instrumentation accident.
 *   Both platforms returned YRT_POLICY_BELOW_NORMAL, and on Linux the nice
 *   value moved on the pump thread only (+5) with the calling thread's left
 *   alone, which is what the per-thread claim in PUMP rests on.
 *   The Emscripten path is built with emcc 6.0.10 as C11 and C++17 with
 *   warnings as errors, with -pthread, without it and with YRT_NO_THREADS,
 *   and run under node 24 with -pthread -sPROXY_TO_PTHREAD=1: the whole of
 *   tests/adapt/rt_test.c passes there, wait-versus-stop hammer
 *   included, as do examples/rt/jitter.c, examples/rt/pump.c, and the
 *   QUEST+ and GP test suites with their async layers on. Under node the
 *   clock claims 1 ns and shows a smallest step of 0.26 us on a pthread and
 *   0.48 us on the main thread, where one read costs 0.6 us and 3.4 us. Over
 *   five rt_jitter runs of 2 ms waits on a pthread, on a loaded development
 *   machine running node inside Docker, the median wake was 118 to 251 us
 *   late with no spin window, 0.1 to 47 us with the default 200 us, and
 *   0.1 us with 1 ms; the p99 ranged from 0.1 ms to 20 ms and belongs to the
 *   host, not to wasm. Measure on the machine you will use.
 *   No browser has run any of it: the WEBASSEMBLY notes on the main thread,
 *   cross-origin isolation and coarsened clocks are the platform's
 *   documented rules, not measurements.
 *   The macOS pump, like the rest of the macOS path, has not been compiled or
 *   run here at all, so its relative condition wait and its
 *   THREAD_PRECEDENCE_POLICY rung are written, not tested; CI compiles them and
 *   a rig should print what yrt_pump_policy() reports before believing it.
 *   The top of the ladder is barely exercised: the test hosts run without
 *   CAP_SYS_NICE, so the tests see YRT_POLICY_TIME_CRITICAL (Windows) and
 *   YRT_POLICY_NORMAL. YRT_POLICY_DEADLINE was obtained once, by
 *   rt_ring_bench run as root under WSL2 (v0.5.0), and nothing timed there
 *   is a claim about a real kernel; YRT_POLICY_FIFO and
 *   YRT_POLICY_TIME_CONSTRAINT have never been obtained. Treat the
 *   real-time rungs as written, not as tested, and check what
 *   yrt_describe() reports on your rig.
 *   Outside this STATUS block, no number in this header is a measurement.
 *   Every latency statement is a bound or a pointer at rt_jitter.
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, see end of file.
 */
#ifndef YSP_RT_H_INCLUDED
#define YSP_RT_H_INCLUDED

/* The version of this header, for a binding's __version__ and for a log line.
 * The string always matches the three numbers. */
#define YRT_VERSION_MAJOR  0
#define YRT_VERSION_MINOR  9
#define YRT_VERSION_PATCH  0
#define YRT_VERSION_STRING "0.9.0"

/* Feature-test macro for clock_nanosleep() and mlockall() in the Linux
 * implementation. Defined here, before the first system header, so it takes
 * effect when this header is the translation unit's first include (the usual
 * case). If you include other system headers before this one, define
 * _DEFAULT_SOURCE yourself.
 *
 * _DEFAULT_SOURCE rather than the _POSIX_C_SOURCE 200809L this header's own
 * code needs, and set on the FIRST include rather than only in an
 * implementation translation unit. Both for the same reason: a translation
 * unit may include this header without implementing it and then implement
 * ysp/serial.h, and an explicit _POSIX_C_SOURCE makes glibc drop __USE_MISC
 * for the WHOLE unit, taking CRTSCTS, realpath, strdup and usleep with it.
 * _DEFAULT_SOURCE is a superset of what this header needs and is the same
 * macro both transport headers ask for, so every include order agrees.
 *
 * Linux and Emscripten only, deliberately. Setting a feature macro on macOS
 * drops __DARWIN_C_LEVEL out of __DARWIN_C_FULL, which hides exactly what the
 * Darwin path needs: pthread_mach_thread_np and
 * pthread_cond_timedwait_relative_np. Emscripten's libc is musl, which under
 * -std=c11 shows nothing past ISO C (not even clock_gettime) unless a feature
 * macro asks, and _DEFAULT_SOURCE is the one it honors the same way glibc
 * does. */
#if (defined(__linux__) || defined(__EMSCRIPTEN__)) && !defined(_DEFAULT_SOURCE) && \
    !defined(_GNU_SOURCE) && !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE)
    #define _DEFAULT_SOURCE 1
#endif

/* CONDITION_VARIABLE and CreateWaitableTimerEx are Vista; raise the target
 * only when the user set it lower, and only in the implementation. */
#if defined(YSP_RT_IMPLEMENTATION) && defined(_WIN32)
    #if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
        #undef _WIN32_WINNT
        #define _WIN32_WINNT 0x0600
    #endif
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YRT_API
#define YRT_API extern
#endif

/* YRT_VERSION_STRING of the implementation that was compiled, which is not
 * always the header a caller included: a binding that links a prebuilt object
 * can compare the two. Static storage; never NULL. */
YRT_API const char* yrt_version(void);

/* --- clock -------------------------------------------------------------- */

/* Monotonic nanoseconds. CLOCK_MONOTONIC on Linux and macOS,
 * QueryPerformanceCounter on Windows. The zero point is arbitrary; only
 * differences are meaningful. This is the clock every deadline in ysp is
 * expressed in. Signed, like every other time in ysp, so a difference such
 * as `onset - yrt_now_ns()` is negative when the onset is past, instead of
 * wrapping. Counts from the OS's own zero (boot, or a process-wide origin),
 * so it stays positive for 292 years. */
YRT_API int64_t yrt_now_ns(void);

/* Units. Every time in a ysp C API is an int64_t count of nanoseconds; these
 * macros write one from another unit, rounded to the nearest nanosecond and
 * exact for whole numbers (to 2^53 ns, 104 days): .duration_ns = YRT_MS(200).
 * YRT_TO_S and YRT_TO_MS go back to a double for a person to read. Constant
 * expressions, so they work in a static initializer. The headers that do not
 * include this one (ysp/timeline.h, ysp/trials.h, ysp/response.h,
 * ysp/rdk.h) define the same macros under the same guard. */
#ifndef YRT_S
#define YRT_NS(x)    ((int64_t)(x))
#define YRT_US(x)    ((int64_t)((x) * 1e3 + ((x) < 0 ? -0.5 : 0.5)))
#define YRT_MS(x)    ((int64_t)((x) * 1e6 + ((x) < 0 ? -0.5 : 0.5)))
#define YRT_S(x)     ((int64_t)((x) * 1e9 + ((x) < 0 ? -0.5 : 0.5)))
#define YRT_TO_MS(ns) ((double)(ns) / 1e6)
#define YRT_TO_S(ns)  ((double)(ns) / 1e9)
#endif

/* The conversion yrt_now_ns() applies to the platform's raw counter, for a
 * timestamp an API hands over in that counter's units. On Windows `ticks` is
 * a QueryPerformanceCounter value (DXGI's SyncQPCTime, a WASAPI QPC
 * position, a raw LARGE_INTEGER), and the result is bit-identical to what
 * yrt_now_ns() returns for that counter value, so such a stamp is on the
 * ysp_rt clock exactly, with no correlation and no width. On Linux, macOS
 * and Emscripten the clock is clock_gettime(CLOCK_MONOTONIC), whose native
 * unit is already the nanosecond, and this returns `ticks` unchanged. A
 * negative `ticks` converts as minus the conversion of its magnitude, so a
 * difference of two counter values converts too. */
YRT_API int64_t yrt_ticks_to_ns(int64_t ticks);

/* The calling thread's OS id: GetCurrentThreadId() on Windows, the kernel
 * task id (gettid) on Linux, pthread_threadid_np() truncated to 32 bits on
 * macOS, and the pthread handle under Emscripten. The same number Tracy, a
 * debugger and a Chrome trace show. On POSIX the first call on each thread
 * makes a syscall and later calls read a thread-local copy, so a real-time
 * thread should call it (or YRT_THREAD_INIT) once before it matters. */
YRT_API uint32_t yrt_thread_id(void);

/* Windows interrupt time in 100 ns units, from QueryInterruptTimePrecise()
 * (Windows 10 and later), which is loaded at run time so the header still
 * links nothing. It is the clock GetTickCount64() and window message times
 * count in, at a finer grain; it includes time the machine spent asleep.
 * Returns 0 where the call does not exist and on every other platform. Its
 * signature fits yrt_corr_desc.read; see CLOCK CORRELATION. */
YRT_API uint64_t yrt_interrupt_time_100ns(void);

/* What the OS says the timing facilities are worth. Filled by
 * yrt_get_clock_info(); every field is a property of the machine, not of
 * any one thread. */
typedef struct yrt_clock_info {
    int64_t  resolution_ns; /* the tick the clock counts in: clock_getres() on
                             * POSIX, 1 s / QueryPerformanceFrequency() on
                             * Windows, and on Emscripten the larger of
                             * clock_getres() and the smallest step two reads
                             * actually show (see WEBASSEMBLY). Not the cost
                             * of reading the clock and not the accuracy of a
                             * wait. */
    bool hires_timer;       /* Windows: a CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
                             * timer could be created (Win10 1803+). False
                             * means every OS wait is quantized to the ~15.6 ms
                             * system tick and yrt_sleep_until() spins the
                             * last tick instead. Always true on POSIX, where
                             * the sleep syscalls already work at the clock's
                             * own resolution. */
} yrt_clock_info;

/* Fill `out` with the clock and timer properties above. Cheap, but it creates
 * and destroys a waitable timer on Windows, so do not call it in a hot loop. */
YRT_API void yrt_get_clock_info(yrt_clock_info* out);

/* --- waits -------------------------------------------------------------- */

/* Spin window yrt_sleep_ns() and a zeroed yrt_worker_desc use: that much
 * of the tail of a wait is spun on the clock rather than left to the OS. The
 * value differs by platform because the OS wait it has to cover differs; WAITS
 * says why. Measure it on the rig with examples/rt/jitter.c before you trust
 * it, and pass your own window rather than editing this one. */
#if defined(_WIN32)
    #define YRT_DEFAULT_SPIN_NS ((int64_t)1200000)
#else
    #define YRT_DEFAULT_SPIN_NS ((int64_t)200000)
#endif

/* Wait until the absolute monotonic time `deadline_ns`, then return how late
 * the wake actually was, in nanoseconds (0 if it landed on or before the
 * deadline). A deadline already in the past returns immediately with the
 * lateness so far; it never waits a whole clock period.
 *
 * The OS is given everything up to `spin_ns` before the deadline and the
 * remainder is spun on the clock with a pause instruction. spin_ns = 0 leaves
 * the whole wait to the OS. On Windows WITHOUT a high-resolution waitable
 * timer the window is widened to one system tick (~15.6 ms) regardless of
 * spin_ns, because the fallback timer cannot fire any finer and overshooting
 * the deadline is the worse failure.
 *
 * Callable from any thread, including several at once. Allocates nothing; on
 * Windows it creates one waitable timer per calling thread on first use (see
 * yrt_thread_cleanup). A negative deadline or spin_ns counts as 0. */
YRT_API int64_t yrt_sleep_until(int64_t deadline_ns, int64_t spin_ns);

/* Wait `ns` nanoseconds from now with YRT_DEFAULT_SPIN_NS as the spin
 * window, and return how late the wake was. The deadline is taken on entry,
 * so the call's own overhead comes out of the wait, not on top of it. For a
 * sequence of frames use yrt_sleep_until() against a running absolute
 * deadline instead, so wake latency cannot accumulate. */
YRT_API int64_t yrt_sleep_ns(int64_t ns);

/* Spin on the clock until `deadline_ns` and return how late the loop exited.
 * No syscall, no sleep, a pause instruction per iteration so a sibling
 * hyperthread keeps its share of the core. As accurate as the clock, and it
 * costs a whole CPU for the duration: this is the floor the other waits are
 * measured against, not a general-purpose sleep. */
YRT_API int64_t yrt_spin_until(int64_t deadline_ns);

/* Release the per-thread resources ysp/rt.h created for the CALLING thread
 * (today: the Windows waitable timer yrt_sleep_until() caches). Optional.
 * A thread that exits without calling it leaks one timer handle until the
 * process ends, which is why a long-lived experiment thread need not bother
 * and a worker pool should. No-op on POSIX. */
YRT_API void yrt_thread_cleanup(void);

/* --- virtual clock (v0.9.0) --------------------------------------------- */

/* The time a virtual clock starts at when yrt_virtual_begin() gets 0:
 * 1000 s, far from 0, which several headers read as "no time". */
#define YRT_VIRTUAL_T0_NS ((int64_t)1000000000000LL)
/* Timers a virtual clock holds at once. */
#define YRT_VIRTUAL_TIMERS 16

/* A virtual timer: called on the driver thread with the clock at its time.
 * Return the next time to call it (later than now_ns), or any other value
 * to end it. */
typedef int64_t (*yrt_virtual_fn)(void* ctx, int64_t now_ns);

/* Put the whole process on a virtual clock that starts at t0_ns (0 =
 * YRT_VIRTUAL_T0_NS). See VIRTUAL CLOCK. The calling thread becomes the
 * driver: its waits move the clock. Returns 0, or YRT_ERR_STATE when
 * the clock is already virtual. Call it before anything reads the clock. */
YRT_API int yrt_virtual_begin(int64_t t0_ns);

/* Back to the real clock, and drop every virtual timer. Times read before
 * and after are on different clocks. Safe when the clock is real. */
YRT_API void yrt_virtual_end(void);

/* True while the clock is virtual. */
YRT_API bool yrt_is_virtual(void);

/* The real clock, also while the clock is virtual: yrt_now_ns()'s counter
 * before VIRTUAL CLOCK. For a cost (how long a draw, a decode or a build
 * took), never for a time that feeds a plan or a record: those read
 * yrt_now_ns(). On the real clock the two return the same counter. */
YRT_API int64_t yrt_real_ns(void);

/* Call fn(ctx, t) on the driver thread when the virtual clock reaches
 * at_ns (at once, at the next wait of the driver, when at_ns is already
 * past). Returns the timer's id (0 or more), YRT_ERR_ARG for a NULL fn,
 * YRT_ERR_FULL when YRT_VIRTUAL_TIMERS are set, or YRT_ERR_STATE when
 * the clock is real or the caller is not the driver thread. Driver
 * thread only, timer callbacks included. */
YRT_API int yrt_virtual_timer(yrt_virtual_fn fn, void* ctx, int64_t at_ns);

/* End a timer before its time. Safe on an id that has ended. Driver
 * thread only. */
YRT_API void yrt_virtual_cancel(int id);

/* --- scheduling --------------------------------------------------------- */

/* A real-time reservation, in nanoseconds, with the same meaning as
 * ypar_desc.sched and yser_desc.sched: Linux SCHED_DEADLINE, macOS
 * THREAD_TIME_CONSTRAINT_POLICY. These do not set any deadline the caller
 * waits on; they govern how quickly and how predictably the thread gets the
 * CPU once it wakes.
 *
 *   runtime_ns  - CPU budget guaranteed (and capped) per period.
 *   deadline_ns - relative deadline; smaller = higher EDF priority = lower
 *                 wake latency, but harder to admit alongside other RT tasks.
 *   period_ns   - replenishment interval; reserved bandwidth = runtime/period.
 *
 * All three zero means the YRT_DEFAULT_RT_* values below. Otherwise
 * 0 < runtime_ns <= deadline_ns <= period_ns must hold, with a zero period
 * read as "same as deadline". See yrt_sched_normalize(). */
typedef struct yrt_sched_deadline {
    int64_t runtime_ns;
    int64_t deadline_ns;
    int64_t period_ns;
} yrt_sched_deadline;

#define YRT_DEFAULT_RT_RUNTIME_NS  ((int64_t)1000000)   /*  1 ms */
#define YRT_DEFAULT_RT_DEADLINE_NS ((int64_t)10000000)  /* 10 ms */
#define YRT_DEFAULT_RT_PERIOD_NS   ((int64_t)10000000)  /* 10 ms */

/* Apply the defaults and the validation described above to `rt`, in place.
 * All-zero becomes the YRT_DEFAULT_RT_* reservation; a zero period_ns
 * becomes deadline_ns. Returns 0, or YRT_ERR_ARG, leaving `rt` alone, when
 * the result would violate 0 < runtime <= deadline <= period, so a
 * half-filled struct fails loudly instead of silently degrading to the next
 * rung down. A NULL `rt` returns YRT_ERR_ARG. */
YRT_API int yrt_sched_normalize(yrt_sched_deadline* rt);

/* A rung of the scheduling ladder. See SCHEDULING in the manual for what each
 * one costs to get and what it buys. Log the value you obtained with your
 * timing data; the same jitter histogram means different things at different
 * rungs. */
typedef enum yrt_policy {
    YRT_POLICY_NONE = 0,       /* nothing was attempted, or a worker is not
                                  * running (YRT_NO_THREADS, open failed) */
    YRT_POLICY_DEADLINE,       /* Linux SCHED_DEADLINE                     */
    YRT_POLICY_TIME_CONSTRAINT,/* macOS THREAD_TIME_CONSTRAINT_POLICY      */
    YRT_POLICY_FIFO,           /* SCHED_FIFO, priority 80 or system max    */
    YRT_POLICY_TIME_CRITICAL,  /* Windows THREAD_PRIORITY_TIME_CRITICAL    */
    YRT_POLICY_NORMAL,         /* no real-time policy was granted          */
    YRT_POLICY_BELOW_NORMAL    /* deliberately below normal: only a
                                  * yrt_pump with desc.below_normal asks for
                                  * this, and nothing else in this header ever
                                  * moves a thread DOWN. Appended rather than
                                  * placed in ladder order so every value
                                  * above keeps the number it had in v0.2.  */
} yrt_policy;

/* Short static name of a policy ("DEADLINE", "FIFO", ...), for log lines. */
YRT_API const char* yrt_policy_name(yrt_policy p);

/* Raise the CALLING thread as far up the ladder as this OS and this process's
 * privileges allow and return the rung that stuck. `rt` is the reservation
 * for the DEADLINE and TIME_CONSTRAINT rungs; NULL means the defaults. An
 * `rt` that yrt_sched_normalize() rejects skips those rungs and starts at
 * FIFO, because a malformed reservation the kernel would refuse anyway is not
 * worth a syscall.
 *
 * Never fails: the bottom rung is YRT_POLICY_NORMAL, where the only thing
 * done is dropping the Linux timer slack to 1 ns, which needs no privilege.
 * Nothing here is undone for you; a thread stays elevated until it exits.
 *
 * On Windows it also opts the calling thread out of EcoQoS (efficiency
 * cores at a low clock) and, on a hybrid CPU, prefers the performance
 * cores for it, whatever rung it gets; see POWER THROTTLING and CORE
 * TYPES.
 *
 * On Linux, call this BEFORE yrt_thread_pin(): the kernel refuses
 * SCHED_DEADLINE to a thread whose affinity is not the root domain, and it
 * refuses to narrow the affinity of a thread that already has it. */
YRT_API yrt_policy yrt_thread_elevate(const yrt_sched_deadline* rt);

/* Ask the kernel to stop deliberately firing this thread's timers late.
 * Linux only (PR_SET_TIMERSLACK): a normal thread's default is 50000 ns, and
 * every clock_nanosleep in this header pays it. `ns` is clamped to at least 1;
 * the kernel keeps its own floor. Returns 0 if it was applied,
 * YRT_ERR_UNSUPPORTED on every other platform and YRT_ERR_REFUSED when the
 * call was refused. Real-time policies ignore timer slack entirely, so this
 * only matters at YRT_POLICY_NORMAL, where yrt_thread_elevate() already
 * does it for you. */
YRT_API int yrt_thread_set_timer_slack(int64_t ns);

/* Bind the calling thread to logical CPU `cpu`. Returns 0 if the OS applied
 * it, YRT_ERR_ARG for a CPU number out of range, YRT_ERR_REFUSED when the
 * OS said no.
 *
 * PINNING DEFEATS SCHED_DEADLINE. The kernel's admission control requires a
 * deadline task to be schedulable over the whole root domain, so a pinned
 * thread cannot hold YRT_POLICY_DEADLINE and an already-deadline thread
 * cannot be pinned. Choose: a deadline reservation on a normal machine, or a
 * FIFO thread pinned to a core the kernel was told to leave alone (isolcpus,
 * nohz_full, irqaffinity). Do not expect both.
 *
 * macOS has no thread pinning, only THREAD_AFFINITY_POLICY, which is a
 * cache-locality hint the scheduler may ignore; this returns
 * YRT_ERR_UNSUPPORTED there rather than pretend. */
YRT_API int yrt_thread_pin(int cpu);

/* What kind of core a logical CPU is, on a CPU with more than one kind. */
typedef enum yrt_core_type {
    YRT_CORE_UNKNOWN = 0,     /* the OS does not say (see SCHEDULING)       */
    YRT_CORE_UNIFORM,         /* every core of this machine is the same kind */
    YRT_CORE_PERFORMANCE,     /* a P-core: the highest efficiency class     */
    YRT_CORE_EFFICIENCY       /* an E-core: any lower class                 */
} yrt_core_type;

/* The kind of logical CPU `cpu`, numbered as yrt_thread_pin() numbers it.
 * Windows 10 and later: from GetSystemCpuSetInformation()'s
 * EfficiencyClass. Everywhere else, and for a CPU that does not exist,
 * YRT_CORE_UNKNOWN: see SCHEDULING for what Linux exposes. On a hybrid CPU
 * the number alone does not tell you the kind; ask before you pin. */
YRT_API yrt_core_type yrt_cpu_core_type(int cpu);

/* "P", "E", "uniform" or "?", for a log line. */
YRT_API const char* yrt_core_type_name(yrt_core_type t);

/* Lock this PROCESS's pages into RAM so no wait can end in a page fault:
 * mlockall(MCL_CURRENT|MCL_FUTURE) on POSIX, which needs RLIMIT_MEMLOCK
 * headroom or CAP_IPC_LOCK. Returns 0 if it was applied, YRT_ERR_REFUSED
 * if it was not.
 *
 * Returns YRT_ERR_UNSUPPORTED and does nothing on Windows and WebAssembly.
 * Windows has no equivalent: VirtualLock and SetProcessWorkingSetSize
 * operate on pages and on a working set the memory manager still owns, and
 * calling them would buy a claim this header cannot honor. Process-wide, so
 * call it once from the main thread. */
YRT_API int yrt_process_lock_memory(void);

/* Windows: timeBeginPeriod(1), raising this process's timer resolution to
 * 1 ms. Returns 0 if it was applied, YRT_ERR_REFUSED when winmm refused it,
 * YRT_ERR_UNSUPPORTED without winmm. winmm is loaded at run time, so the
 * header still links against nothing.
 *
 * Since Windows 10 2004 this is per-process, not machine-wide, so it neither
 * slows other processes down nor speeds them up, and since the
 * high-resolution waitable timer arrived in Win10 1803 yrt_sleep_until()
 * rarely needs it. It still shortens Sleep() and the fallback waitable timer,
 * which is what an older machine or a third-party library in the same process
 * will be using. Pair every call with yrt_timer_resolution_end(): the OS
 * reference-counts them. Process-wide; call from one thread. No-op and
 * YRT_ERR_UNSUPPORTED elsewhere.
 *
 * On success it also opts the process out of Windows 11's timer throttling
 * (PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION), without which a
 * minimized or hidden process loses the raised resolution; see POWER
 * THROTTLING. yrt_report.timer_throttle_off says whether that took. */
YRT_API int yrt_timer_resolution_begin(void);

/* Undo one yrt_timer_resolution_begin(). No-op elsewhere, and no-op if the
 * resolution was never raised. */
YRT_API void yrt_timer_resolution_end(void);

/* --- report ------------------------------------------------------------- */

/* Everything ysp/rt.h obtained or measured about the current thread and
 * process, in one struct, so it can go in the log next to the timing data.
 * Filled by yrt_report_get(). */
typedef struct yrt_report {
    yrt_policy policy;            /* the rung the caller obtained; the OS
                                     * cannot be asked about every rung, so
                                     * the caller supplies this one          */
    int64_t  clock_res_ns;          /* yrt_clock_info.resolution_ns        */
    int64_t  timer_slack_ns;        /* Linux PR_GET_TIMERSLACK               */
    bool     timer_slack_known;     /* false where the OS has no such notion */
    bool     hires_timer;           /* yrt_clock_info.hires_timer          */
    bool     memory_locked;         /* yrt_process_lock_memory() succeeded */
    bool     timer_resolution_raised; /* yrt_timer_resolution_begin() did  */
    /* Windows 11 power throttling. False on every other platform.      */
    bool     timer_throttle_off;  /* the process told Windows to keep its
                                   * raised timer resolution while its
                                   * windows are minimized or hidden      */
    bool     ecoqos_off;          /* the CALLING thread is opted out of
                                   * EcoQoS (efficiency cores, low clock)  */
    bool     throttle_known;      /* Windows 10 1709 or later: the two
                                   * fields above mean something          */
    /* Core placement of the CALLING thread. */
    int      pinned_cpu;          /* what yrt_thread_pin() bound it to on
                                   * this thread, or -1                   */
    yrt_core_type pinned_core;  /* yrt_cpu_core_type(pinned_cpu)      */
    bool     pcores_preferred;    /* yrt_thread_elevate() steered it to
                                   * the performance cores (Windows, a
                                   * hybrid CPU)                          */
} yrt_report;

/* Fill `out` with what can be queried right now, plus the `policy` the caller
 * obtained from yrt_thread_elevate() or yrt_worker_policy(). The two
 * process-wide flags reflect calls made through this header in this process;
 * if something else locked memory or raised the timer resolution, they stay
 * false. Reads the calling thread's timer slack, so call it from the thread
 * the report is about. */
YRT_API void yrt_report_get(yrt_report* out, yrt_policy policy);

/* Write a one-line, log-ready summary of `r` into `buf` (NUL-terminated,
 * truncated to fit) and return the number of characters written, not counting
 * the NUL. 256 bytes always hold the whole line. The lines
 * yrt_report_get() fills are shorter than 192 bytes on every platform
 * tested; a line that does not fit is cut, never overrun. The line looks
 * like:
 *
 *   ysp_rt: policy=DEADLINE clock_res=1ns slack=50000ns hires_timer=yes
 *           mlock=yes timer_res=no timer_throttle=n/a ecoqos=n/a pin=none
 *           cores=any
 *
 * (on one line). Print it once per session, beside the jitter numbers. */
YRT_API int yrt_describe(const yrt_report* r, char* buf, size_t cap);

/* --- return codes ------------------------------------------------------- */

/* Return codes of every call that returns int: a count, a job seq, or 0 when
 * >= 0; one of these when negative. yrt_strerror() names them. YRT_OK is
 * what "no error, nothing to report" means; do not compare against it,
 * compare against 0. Outside the YRT_NO_THREADS guard since v0.5.0, because
 * the event ring and the clock correlation use them and neither is a
 * threading feature. */
#define YRT_OK           0
#define YRT_ERR_ARG    (-1)  /* null handle, null callback, null message  */
#define YRT_ERR_STOPPED (-2) /* the worker or pump is not running         */
#define YRT_ERR_FULL    (-3) /* yrt_pump_submit, yrt_ring_push: the
                                * ring is full and NOTHING was taken        */
#define YRT_ERR_TIMEOUT (-4) /* yrt_pump_wait: the timeout expired;
                                * yrt_correlate: the clock never ticked   */
#define YRT_ERR_UNSUPPORTED (-5) /* this platform has no such facility    */
#define YRT_ERR_REFUSED (-6) /* the OS refused the request               */
#define YRT_ERR_STATE   (-7) /* v0.9.0: the clock is real, or the caller is
                                * not the virtual clock's driver thread    */

/* Static description of a YRT_ERR_* code ("ok" for values >= 0). */
YRT_API const char* yrt_strerror(int code);

/* --- event ring --------------------------------------------------------- */

/* Where a zone is in the source, for the trace. One static instance per zone,
 * declared by YRT_ZONE; a zone record points at it, so it must outlive the
 * drain (string literals and statics do). The fields are in the order and of
 * the types of Tracy's ___tracy_source_location_data. */
typedef struct yrt_srcloc {
    const char* name;
    const char* function;
    const char* file;
    uint32_t    line;
    uint32_t    color;
} yrt_srcloc;

/* The 40 bytes a record carries. Its meaning is the record's (source, kind):
 * pick the view that fits. Named rather than anonymous because anonymous
 * unions are C11. The zone and plot views are ysp/rt.h's own kinds; their
 * pointers are only meaningful inside this process. */
typedef union yrt_payload {
    int64_t       i64[5];
    uint64_t      u64[5];
    double        f64[5];
    int32_t       i32[10];
    uint32_t      u32[10];
    float         f32[10];
    uint16_t      u16[20];
    char          text[40];   /* NUL-terminated in ysp/rt.h's own kinds    */
    unsigned char bytes[40];
    struct { const yrt_srcloc* loc; int64_t dur_ns; uint64_t value; } zone;
    struct { const char* name; double value; } plot;  /* PLOT and FRAME     */
} yrt_payload;

/* One record: 64 bytes, a cache line, on every target (the implementation
 * asserts it). Build one with a designated initializer; every field left 0
 * gets its default. */
typedef struct yrt_event {
    uint32_t seq;      /* written by the ring: the ring's ticket + 1. What you
                        * put here is ignored. Consecutive records differ by
                        * 1 (mod 2^32) in the order the ring took them. A
                        * refused push takes no ticket, so a gap never means
                        * loss; the LOSS record does (see EVENT RING).      */
    uint32_t tid;      /* the pushing thread; 0 = yrt_thread_id()         */
    int64_t  t_ns;     /* on the ysp_rt clock; 0 = stamped at the push      */
    uint16_t source;   /* who: a YRT_SRC_* number                          */
    uint16_t kind;     /* what: its meaning belongs to the source            */
    uint32_t aux;      /* the producer's: a code, an index, a count          */
    yrt_payload u;
} yrt_event;

/* Sources. A record's kind means nothing without its source. 1 to 255 are
 * the ysp headers', assigned here so no two collide; 256 to 32767 belong to
 * extensions, which the host assigns at load; 32768 to 65535 are yours.
 * Numbers for headers that do not exist yet are reserved, so a log written
 * today keeps its meaning when they arrive. */
#define YRT_SRC_NONE       0u
#define YRT_SRC_RT         1u
#define YRT_SRC_SCREEN     2u
#define YRT_SRC_AUDIO      3u
#define YRT_SRC_VIDEO      4u
#define YRT_SRC_TIMELINE   5u
#define YRT_SRC_SERIAL     6u
#define YRT_SRC_PARALLEL   7u
#define YRT_SRC_INPUT      8u
#define YRT_SRC_TRIALS     9u
#define YRT_SRC_NET       10u
#define YRT_SRC_GFX       11u
#define YRT_SRC_DEVICE    12u   /* ysp/device.h, planned (docs/devices_spec.md) */
#define YRT_SRC_EXTENSION 256u   /* first extension source           */
#define YRT_SRC_USER    32768u   /* first source for your own program */

/* Kinds under YRT_SRC_RT. */
#define YRT_KIND_ZONE        1u  /* t_ns = begin; u.zone = {loc, dur, value} */
#define YRT_KIND_FRAME       2u  /* t_ns = mark; u.plot.name = NULL or the
                                    * YRT_FRAME_MARK_NAMED name             */
#define YRT_KIND_PLOT        3u  /* u.plot = {name, value}                   */
#define YRT_KIND_MESSAGE     4u  /* u.text, at most 39 chars and a NUL       */
#define YRT_KIND_THREAD_NAME 5u  /* u.text: the name of thread `tid`         */
#define YRT_KIND_LOSS        6u  /* made by the drain: u.u64[0] records lost
                                    * since the previous drain, u.u64[1] lost
                                    * in all; t_ns = the drain; seq = 0       */
#define YRT_KIND_CLOCK       7u  /* a correlation, pushed by whoever made
                                    * it: u.u64[0] = yrt_corr.other,
                                    * u.u64[1] = width_ns, t_ns = rt_ns, aux =
                                    * the caller's number for the clock.
                                    * yrt_fit (DEVICE CLOCK FIT) pushes one
                                    * per closed bucket: u.u64[0] the device's
                                    * unwrapped ticks, u.u64[1] the pair's
                                    * width, u.u64[2] the fit's epoch       */
#define YRT_KIND_FIT         8u  /* a device clock fit (DEVICE CLOCK FIT):
                                    * t_ns = the newest pair's host time,
                                    * u.i64[0] host ns at the anchor,
                                    * u.u64[1] unwrapped ticks at the anchor,
                                    * u.f64[2] ns per tick, u.i64[3] the
                                    * spread (p99, ns), u.u32[8] the epoch,
                                    * u.u32[9] the points; aux = the clock    */

/* Bytes of desc.memory that hold exactly n records: n slots of 64 bytes and
 * up to 64 bytes lost to aligning the start. */
#define YRT_RING_BYTES(n) ((size_t)(n) * 64u + 64u)

/* Ring description. Both fields are required. */
typedef struct yrt_ring_desc {
    void*  memory;     /* the records' storage, owned by the caller and
                        * alive while anything may push or drain. Any
                        * alignment; the ring aligns it to 64 itself.     */
    size_t bytes;      /* its size. The ring holds the largest power of
                        * two of 64-byte slots that fits, at least 2 and
                        * at most 2^24: YRT_RING_BYTES(n) gives n when n
                        * is a power of two.                               */
} yrt_ring_desc;

/* Ring handle. The caller allocates it and treats every field as opaque.
 * Four groups, each starting 128 bytes after the last, so no two share a
 * cache line from any 8-byte-aligned address: `used`, which every push and
 * every drain batch read-modify-write; the ticket counter and what a push
 * reads; the drainer's own fields; and the cold error text. `used` and
 * `head` were measured on one line and on two, and two pushed more records
 * per second at every producer count (docs/rt.md). */
typedef struct yrt_ring {
    /* --- every push and every drain batch --- */
    uint32_t       used;     /* claimed and not yet drained, plus the brief
                              * increments of pushes being refused          */
    unsigned char  pad0_[128 - sizeof(uint32_t)];
    /* --- every push --- */
    uint32_t       head;     /* the next ticket                             */
    uint32_t       dropped;  /* refused pushes in all, wrapping              */
    uint32_t       mask;     /* capacity - 1; 0 = not open                   */
    uint32_t       spare_;   /* keeps `slots` at the same offset everywhere */
    unsigned char* slots;    /* 64-aligned, inside desc.memory               */
    unsigned char  pad1_[128 - 4 * sizeof(uint32_t) - sizeof(unsigned char*)];
    /* --- the drainer --- */
    uint64_t       lost;     /* refused pushes reported in LOSS records      */
    uint32_t       tail;     /* the next ticket to drain                     */
    uint32_t       dropped_seen;
    unsigned char  pad2_[128 - sizeof(uint64_t) - 2 * sizeof(uint32_t)];
    /* --- cold --- */
    char           error[128];
} yrt_ring;

/* Lay the ring out in desc.memory and zero it, which also touches every page
 * now rather than in the first push from an audio callback. Returns false
 * with a message in yrt_ring_error() for a NULL desc, NULL memory, or room
 * for fewer than 2 records. Must not run while anything pushes or drains;
 * on an open ring it starts over, discarding what was in it. */
YRT_API bool yrt_ring_open(yrt_ring* r, const yrt_ring_desc* desc);

/* Detach the ring from desc.memory: a push or drain after it returns
 * YRT_ERR_ARG. The ring owns nothing, so this frees nothing. Stop the
 * producers (and detach it from the trace) first; a push racing the close
 * may still write. Safe on a zeroed or closed ring and after a failed
 * open. */
YRT_API void yrt_ring_close(yrt_ring* r);

/* Last yrt_ring_open() message for this handle ("" if none). */
YRT_API const char* yrt_ring_error(const yrt_ring* r);

/* Copy `ev` into the ring. A zero ev->t_ns is stamped with yrt_now_ns() and
 * a zero ev->tid with yrt_thread_id(), both before the slot is claimed; the
 * ring writes ev->seq itself.
 *
 * Returns 0, YRT_ERR_FULL when the ring had no room (nothing was written,
 * and the refusal is counted and reported to the drainer), or YRT_ERR_ARG
 * for a NULL pointer or a ring that was never opened.
 *
 * WAIT-FREE: any thread, any number of threads at once, an audio callback
 * included. No lock, no loop, no allocation, no system call when t_ns and tid
 * are set (a zero tid costs one on a POSIX thread's first push). See EVENT
 * RING for what "full" means with several producers. */
YRT_API int yrt_ring_push(yrt_ring* r, const yrt_event* ev);

/* Copy up to `cap` records into `out`, oldest ticket first, and free their
 * slots. Stops at the first record whose producer has not finished writing
 * it. When pushes were refused since the previous drain, out[0] is a
 * YRT_KIND_LOSS record that says how many. Returns the count written, 0
 * when there is nothing, or YRT_ERR_ARG. ONE drainer at a time: two threads
 * draining the same ring at once is not detected and corrupts it. */
YRT_API int yrt_ring_drain(yrt_ring* r, yrt_event* out, int cap);

/* Records the ring holds, or 0 when it is not open. */
YRT_API uint32_t yrt_ring_capacity(const yrt_ring* r);

/* Pushes refused since the ring was opened, wrapping at 2^32. */
YRT_API uint32_t yrt_ring_dropped(const yrt_ring* r);

/* --- clock correlation -------------------------------------------------- */

/* How to read the other clock, and how hard to look. Zero-initialize it and
 * set `read` or `read_ctx`; exactly one of the two. */
typedef struct yrt_corr_desc {
    uint64_t (*read)(void);          /* SDL_GetTicksNS, yrt_interrupt_time_100ns,
                                      * anything of this shape               */
    uint64_t (*read_ctx)(void* ctx); /* the same with a context: a device    */
    void*    ctx;
    int      tries;       /* reads (edges in edge mode) to take the tightest
                           * of; 0 = 16, or 4 in edge mode                   */
    bool     edge;        /* the other clock is coarse: time a CHANGE of its
                           * value instead of one read. Spins until it has
                           * seen `tries` changes; see CLOCK CORRELATION    */
    int64_t  timeout_ns;  /* edge mode: give up after this long; 0 = 100 ms */
} yrt_corr_desc;

/* One correspondence between the two clocks. */
typedef struct yrt_corr {
    int64_t  rt_ns;       /* the ysp_rt time at which...                     */
    uint64_t other;       /* ...the other clock read this, in its own units  */
    int64_t  width_ns;    /* the bracket around rt_ns: the true time is
                           * within width_ns / 2 of it. Never 0: at least the
                           * ysp_rt clock's tick. In plain mode the other
                           * clock's own tick comes on top.                  */
    int      tries;       /* reads or edges taken                            */
} yrt_corr;

/* Read the other clock between two ysp_rt reads, `tries` times, and keep the
 * tightest pair. Returns the number of tries taken (> 0), YRT_ERR_ARG for a
 * NULL pointer or not exactly one reader, or YRT_ERR_TIMEOUT when edge
 * mode saw no change in timeout_ns. Allocates nothing; spins, so call it
 * between trials, not inside a frame. */
YRT_API int yrt_correlate(const yrt_corr_desc* desc, yrt_corr* out);

/* --- device clock fit --------------------------------------------------- */

/* Points the fit keeps: one per bucket, so 205 s of 200 ms buckets. Fixed,
 * because the struct's size must agree in every translation unit. */
#define YRT_FIT_POINTS 1024

/* What the pairs' host times are worth; see DEVICE CLOCK FIT. */
typedef enum yrt_fit_mode {
    YRT_FIT_LATE     = 0, /* host stamps are late, never early: the lower
                             * envelope (USB reports read by a thread)      */
    YRT_FIT_BRACKET  = 1, /* the device time was read inside a bracket:
                             * host_ns is the bracket's end, width_ns its
                             * length; the least late pair per bucket, wide
                             * brackets refused                            */
    YRT_FIT_UNBIASED = 2  /* stamps scatter both ways around the truth:
                             * bucket means and least squares              */
} yrt_fit_mode;

/* Zero-initialize, then set ns_per_tick (required) and what you need. */
typedef struct yrt_fit_desc {
    int         mode;            /* yrt_fit_mode                              */
    double      ns_per_tick;     /* the device clock's nominal tick: 1e6 for a
                                  * ms timer, 1e3 for micros(). Required, > 0 */
    uint32_t    tick_bits;       /* the device counter's width: 1 to 63 wrap
                                  * (32 for a uint32 timer); 0 or 64 = none    */
    uint32_t    clock_id;        /* `aux` of the CLOCK and FIT records          */
    int64_t     bucket_ns;       /* one point per bucket; 0 = 200 ms            */
    int64_t     slope_span_ns;   /* the slope stays nominal until the points
                                  * span this; 0 = 5 s                         */
    int64_t     restart_ns;      /* a pair this far from the map is held as a
                                  * possible restart; 0 = 50 ms                */
    int64_t     restart_hold_ns; /* held pairs that agree with each other for
                                  * this long start a new epoch; 0 = 1 s       */
    int64_t     max_width_ns;    /* BRACKET: wider pairs are refused; 0 = 1.3 ms */
    yrt_ring* ring;            /* CLOCK and FIT records; NULL = none          */
} yrt_fit_desc;

/* yrt_fit_add()'s result bits. */
#define YRT_FIT_POINT    0x01  /* the pair is a point, or improved one     */
#define YRT_FIT_REFIT    0x02  /* the map changed                          */
#define YRT_FIT_RESTART  0x04  /* a new epoch started (a clock reset)      */
#define YRT_FIT_REJECTED 0x08  /* not used: too wide or out of order, or
                                  * held pairs that a normal pair proved to
                                  * be outliers                             */
#define YRT_FIT_PENDING  0x10  /* held as a possible restart               */

typedef struct yrt_fit_info {
    int64_t  t0_ns;        /* the map's anchor: host ns at...                */
    uint64_t ticks0;       /* ...these unwrapped device ticks                */
    double   ns_per_tick;  /* the fitted tick                                */
    double   ppm;          /* the fitted tick against the nominal one        */
    int64_t  spread_ns;    /* p99 distance of the points from the map:
                            * above it for LATE and BRACKET, either side for
                            * UNBIASED                                       */
    int64_t  width_ns;     /* BRACKET: the widest bracket the map rests on   */
    int64_t  span_ns;      /* host time from the oldest point to the newest  */
    uint32_t points;       /* in the window                                  */
    uint32_t pairs;        /* added since init                               */
    uint32_t epoch;        /* restarts since init                            */
    uint32_t rejected;
    uint32_t pending;      /* pairs held now                                 */
    bool     ready;        /* a map exists                                   */
    bool     slope;        /* the slope is fitted, not nominal               */
} yrt_fit_info;

/* The fit. About 40 KB; treat every field as opaque. */
typedef struct yrt_fit {
    yrt_fit_desc d;
    uint64_t modulus;           /* 2^tick_bits; 0 = no wrap                    */
    uint64_t base_ticks;        /* unwrapped ticks of the epoch's origin       */
    int64_t  base_host;
    uint64_t last_ticks;        /* unwrapped ticks of the newest pair used     */
    int64_t  bucket;            /* the newest point's bucket                   */
    int64_t  fx, fy;            /* the map: host = fy + (x - fx) * k          */
    double   k;
    int64_t  spread_ns, width_ns;
    double   ux, uy;            /* UNBIASED: the newest bucket's sums, from
                                 * its first pair...                           */
    int64_t  ux0, uy0;
    uint32_t un;                /* ...and its count                            */
    int32_t  head, n;
    int32_t  have, slope;
    uint32_t epoch, rejected, pairs;
    int32_t  pend_n;
    uint64_t pend_t[8];         /* held pairs: raw ticks, host, width          */
    int64_t  pend_y[8];
    int64_t  pend_w[8];
    int64_t  px[YRT_FIT_POINTS];  /* ticks since base_ticks                  */
    int64_t  py[YRT_FIT_POINTS];  /* host ns                                 */
    int64_t  pw[YRT_FIT_POINTS];  /* bracket width                           */
    int32_t  hull[YRT_FIT_POINTS];
    int64_t  scratch[YRT_FIT_POINTS];
    char     error[128];
} yrt_fit;

/* Start a fit. False, with the reason in yrt_fit_error(), for a NULL desc,
 * an unknown mode, ns_per_tick <= 0 or tick_bits > 64. Allocates nothing.
 * Opening an open fit starts it over. */
YRT_API bool yrt_fit_open(yrt_fit* f, const yrt_fit_desc* desc);

/* Forget the fit. Holds no resource, so this only zeroes the handle; safe on
 * a zeroed or closed handle and after a failed open. */
YRT_API void yrt_fit_close(yrt_fit* f);

/* The last yrt_fit_open() message ("" if none). */
YRT_API const char* yrt_fit_error(const yrt_fit* f);

/* One pair: the device's counter as it reported it (wrapped), the host time
 * of the pair on the ysp_rt clock, and for BRACKET the bracket's length
 * (host_ns is its end; 0 otherwise). Pairs in arrival order. Returns
 * YRT_FIT_* bits, or YRT_ERR_ARG. One thread at a time; a refit costs
 * O(YRT_FIT_POINTS). */
YRT_API int yrt_fit_add(yrt_fit* f, uint64_t ticks, int64_t host_ns, int64_t width_ns);

/* The ysp_rt time of a device counter value (wrapped as reported, unwrapped
 * against the newest pair), from the current epoch's map. 0 before the
 * first pair. */
YRT_API int64_t  yrt_fit_map(const yrt_fit* f, uint64_t ticks);

/* A wrapped counter value unwrapped against the newest pair used. */
YRT_API uint64_t yrt_fit_unwrap(const yrt_fit* f, uint64_t ticks);

YRT_API void     yrt_fit_get(const yrt_fit* f, yrt_fit_info* out);

/* --- per-user folders ---------------------------------------------------- */

/* Which per-user folder; see PER-USER FOLDERS. */
typedef enum yrt_dir_kind {
    YRT_DIR_CACHE  = 0,   /* disposable: %LOCALAPPDATA%, ~/.cache, ~/Library/Caches */
    YRT_DIR_CONFIG = 1    /* kept: %APPDATA%, ~/.config, ~/Library/Application Support */
} yrt_dir_kind;

/* <base>/ysp/<subdir> for this user, into out (cap bytes with the NUL).
 * YRT_OK, or YRT_ERR_ARG with out empty. Makes no folder. */
YRT_API int yrt_user_dir(int kind, const char* subdir, char* out, size_t cap);

/* --- instrumentation ---------------------------------------------------- */

/* The trace ring the YRT_TRACE_RING macros write into, for headers and
 * threads that have no ring pointer of their own. NULL (the default) detaches
 * it, and the macros then skip their clock reads. Detach, and stop every
 * thread that traces, before freeing the ring. */
YRT_API void        yrt_trace_set_ring(yrt_ring* r);
YRT_API yrt_ring* yrt_trace_ring(void);

/* An open zone in YRT_TRACE_RING mode. Declared by YRT_ZONE. */
typedef struct yrt_zone {
    int64_t             t0_ns;  /* 0 = no trace ring at the begin       */
    const yrt_srcloc* loc;
    uint64_t            value;  /* YRT_ZONE_VALUE                     */
} yrt_zone;

/* The functions the YRT_TRACE_RING macros expand to. Call the macros, not
 * these. They are the only entry points the trace needs, all plain C, so an
 * extension host can hand them over as function pointers. */
YRT_API yrt_zone yrt__zone_begin(const yrt_srcloc* loc);
YRT_API void yrt__zone_end(const yrt_zone* z);
YRT_API void yrt__trace_frame(const char* name);
YRT_API void yrt__trace_plot(const char* name, double value);
YRT_API void yrt__trace_message(const char* text, size_t len);
YRT_API void yrt__trace_thread_name(const char* name);
#if defined(__GNUC__) || defined(__clang__)
YRT_API void yrt__trace_messagef(const char* fmt, ...)
    __attribute__((format(printf, 1, 2)));
/* Declared and never defined: the default mode names it inside sizeof, so a
 * YRT_MESSAGEF format is checked even when nothing is compiled. */
int yrt__messagef_check(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#else
YRT_API void yrt__trace_messagef(const char* fmt, ...);
int yrt__messagef_check(const char* fmt, ...);
#endif

#if defined(YRT_TRACE_WEB)
    #error "ysp_rt: YRT_TRACE_WEB is a reserved name; the web backend is not written yet"
#endif
#if defined(YRT_TRACY) && defined(YRT_TRACE_RING)
    #error "ysp_rt: define YRT_TRACY or YRT_TRACE_RING, not both"
#endif

#if defined(YRT_TRACE_RING)

    #define YRT_ZONE(z, name) \
        static const yrt_srcloc yrt__sl_##z = \
            { name, __func__, __FILE__, (uint32_t)__LINE__, 0u }; \
        yrt_zone z = yrt__zone_begin(&yrt__sl_##z)
    #define YRT_ZONE_END(z)            yrt__zone_end(&(z))
    #define YRT_ZONE_VALUE(z, v)       ((void)((z).value = (uint64_t)(v)))
    #define YRT_FRAME_MARK()           yrt__trace_frame(NULL)
    #define YRT_FRAME_MARK_NAMED(name) yrt__trace_frame(name)
    #define YRT_PLOT(name, v)          yrt__trace_plot((name), (double)(v))
    #define YRT_MESSAGE(text)          yrt__trace_message((text), sizeof(text) - 1)
    #define YRT_MESSAGEF(...)          yrt__trace_messagef(__VA_ARGS__)
    #define YRT_THREAD_NAME(name)      yrt__trace_thread_name(name)
    #define YRT_THREAD_INIT(name) \
        do { yrt__trace_thread_name(name); (void)yrt_thread_id(); } while (0)

#elif defined(YRT_TRACY)

    #if !defined(TRACY_ENABLE)
        #error "ysp_rt: YRT_TRACY needs TRACY_ENABLE, or TracyC.h compiles every zone away"
    #endif
    #ifdef YRT_TRACY_HEADER
        #include YRT_TRACY_HEADER
    #else
        #include <tracy/TracyC.h>
    #endif
    #include <stdio.h>
    #include <stdarg.h>

    #define YRT_ZONE(z, name)          TracyCZoneN(z, name, 1)
    #define YRT_ZONE_END(z)            ___tracy_emit_zone_end(z)
    #define YRT_ZONE_VALUE(z, v)       ___tracy_emit_zone_value((z), (uint64_t)(v))
    #define YRT_FRAME_MARK()           yrt__tracy_frame_mark()
    #define YRT_FRAME_MARK_NAMED(name) ___tracy_emit_frame_mark(name)
    #define YRT_PLOT(name, v)          ___tracy_emit_plot((name), (double)(v))
    #define YRT_MESSAGE(text)          do { TracyCMessageL(text); } while (0)
    #define YRT_MESSAGEF(...)          yrt__tracy_messagef(__VA_ARGS__)
    #define YRT_THREAD_NAME(name)      ___tracy_set_thread_name(name)
    #define YRT_THREAD_INIT(name) \
        do { ___tracy_set_thread_name(name); \
             { TracyCZoneN(yrt__init_zone, "yrt thread init", 1); \
               ___tracy_emit_zone_end(yrt__init_zone); } } while (0)

    /* The frame mark carries the ysp_rt time as a plot point, so a Tracy
     * capture can be put on the rig's clock: Tracy stamps the plot with its
     * own clock, and the value is ours. A double holds the nanoseconds
     * exactly for 104 days of uptime. */
    static inline void yrt__tracy_frame_mark(void) {
        ___tracy_emit_frame_mark(0);
        ___tracy_emit_plot("yrt_ns", (double)yrt_now_ns());
    }
    #if defined(__GNUC__) || defined(__clang__)
    static inline void yrt__tracy_messagef(const char* fmt, ...)
        __attribute__((format(printf, 1, 2)));
    #endif
    static inline void yrt__tracy_messagef(const char* fmt, ...) {
        char buf[128];
        va_list ap;
        int n;
        va_start(ap, fmt);
        n = vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
        TracyCMessage(buf, (size_t)n);
    }

#else /* nothing: no code, no argument evaluated */

    #define YRT_ZONE(z, name)          ((void)0)
    #define YRT_ZONE_END(z)            ((void)0)
    #define YRT_ZONE_VALUE(z, v)       ((void)sizeof(v))
    #define YRT_FRAME_MARK()           ((void)0)
    #define YRT_FRAME_MARK_NAMED(name) ((void)0)
    #define YRT_PLOT(name, v)          ((void)sizeof(v))
    #define YRT_MESSAGE(text)          ((void)0)
    #define YRT_MESSAGEF(...)          ((void)sizeof(yrt__messagef_check(__VA_ARGS__)))
    #define YRT_THREAD_NAME(name)      ((void)0)
    #define YRT_THREAD_INIT(name)      ((void)0)

#endif

/* --- worker ------------------------------------------------------------- */

#ifndef YRT_NO_THREADS

/* What the worker thread knows about the job it is running. Valid only for
 * the duration of the callback. */
typedef struct yrt_job_info {
    int64_t  deadline_ns; /* the deadline this job was submitted for        */
    int64_t  at_ns;       /* the clock when the worker entered the callback */
    int64_t  late_ns;     /* at_ns - deadline_ns; negative only on a flush  */
    uint32_t seq;         /* what yrt_worker_submit() returned for this
                           * job. Positive, increasing and unique within one
                           * worker; a job that must know whether it is the
                           * one a caller is still waiting for compares this
                           * with the number that caller kept.             */
    bool     flushed;     /* run early by yrt_worker_close(), not by its
                           * deadline. A trailing edge, a cleanup write or a
                           * "the trial ended" hook still has to happen; a
                           * job that is only meaningful on time should check
                           * this and return. */
} yrt_job_info;

/* The job. Runs on the worker thread, at the worker's policy, with the
 * worker's lock NOT held: it may submit to its own worker to re-arm, and it
 * may block, at the cost of overlapping the next deadline. Keep it short and
 * allocation-free; everything this header promises about jitter ends where
 * this function begins. */
typedef void (*yrt_job_fn)(void* ctx, const yrt_job_info* info);

/* Optional start hook. Runs ONCE, on the worker thread, after the thread has
 * elevated itself and before yrt_worker_open() returns. Return true to let
 * the worker run. Return false and the open FAILS: the thread exits, no job
 * can be submitted, and `err` (or a fixed message when the hook wrote none)
 * is what yrt_worker_error() reports.
 *
 * This exists for state the OS keeps per THREAD, which the thread calling
 * yrt_worker_open() cannot set up on the worker's behalf. Write at most
 * `err_cap` bytes into `err`, NUL-terminated; snprintf() is the expected
 * tool. Keep it short: yrt_worker_open() is blocked until it returns. */
typedef bool (*yrt_start_fn)(void* ctx, char* err, size_t err_cap);

/* Worker description. Zero-initialize it and set only what you need. */
typedef struct yrt_worker_desc {
    yrt_sched_deadline sched; /* reservation for the DEADLINE and
                                 * TIME_CONSTRAINT rungs; all-zero = defaults */
    yrt_start_fn on_start;    /* per-thread setup, on the worker thread;
                                 * NULL = none. False fails the open.        */
    void*    start_ctx;         /* passed to on_start. Separate from the ctx a
                                 * job carries, which belongs to one submit.  */
    int64_t  spin_ns;           /* spin window for the worker's own wait;
                                 * 0 = YRT_DEFAULT_SPIN_NS. See WAITS.      */
    bool     no_elevate;        /* skip the ladder entirely and run the worker
                                 * as a normal thread. For a shared machine, a
                                 * profiling run, or a bug hunt where an RT
                                 * thread that spins would lock the box up.    */
} yrt_worker_desc;

/* Worker handle. The caller allocates it (stack, or a struct the experiment
 * owns) and treats every field as opaque. The layout is ordered by what a
 * submit touches: the job fields and the lock share the first cache lines,
 * because a submit takes the lock and then writes the job, and the 256-byte
 * error buffer is cold and sits last so it cannot push the lock away from the
 * job. Must be zeroed or closed before yrt_worker_open(); a fresh handle
 * has no valid running flag, so open() cannot detect and stop a worker
 * already running in it. */
typedef struct yrt_worker {
    /* --- hot: read or written on every submit and every wake --- */
    uint64_t     deadline_ns;
    yrt_job_fn fn;
    void*        ctx;
    uint32_t     spin_ns;
    uint32_t     gen;      /* job generation, so the worker can tell after a
                            * spin whether the job it chose is still the one.
                            * It is also the job's public seq: submit returns
                            * it and the callback sees it.                   */
    int          has_job;
    int          quit;
    int          ready;
    yrt_policy policy;   /* the rung the worker thread got; final once
                            * yrt_worker_open() returns true             */
    int          running;  /* read and written atomically: it is the one
                            * field a submit consults before it dares touch
                            * the lock, so it cannot be guarded by the lock */
    /* OS objects (mutex/critical section, condition variable, thread, timer),
     * stored inline so the worker owns no heap, and kept next to the hot
     * fields because the lock is the first thing a submit touches. The
     * implementation asserts at compile time that they fit and are aligned. */
    union { void* p[32]; uint64_t u[32]; double d[32]; } os;
    /* --- cold: setup, teardown, diagnostics --- */
    yrt_sched_deadline sched;
    yrt_start_fn on_start;
    void*        start_ctx;
    int          start_failed; /* the hook refused; written before the ready
                                * handshake, read by open() after it        */
    bool         no_elevate;
    char         error[256];
} yrt_worker;

/* Start `w`'s thread per `desc` (NULL = all defaults). Returns true once the
 * thread is up, has run desc.on_start and has published w->policy, so both
 * are final before the first trial. On failure returns false and leaves a
 * message in yrt_worker_error(); a refused on_start is one such failure,
 * and the thread is joined before this returns. This is the only worker call
 * that writes that message, and the only one that must not run while another
 * thread is using the handle. */
YRT_API bool yrt_worker_open(yrt_worker* w, const yrt_worker_desc* desc);

/* Close the worker: join its thread. A pending job is run IMMEDIATELY
 * first, with yrt_job_info.flushed set, so work that must happen is never
 * dropped just because the session ended. Safe on a zeroed or already-closed
 * handle. Must not race another call on the same handle. */
YRT_API void yrt_worker_close(yrt_worker* w);

/* Install `fn(ctx, ...)` to run at the absolute monotonic time
 * `deadline_ns`, replacing any pending job (earlier or later; the worker
 * re-evaluates at once). A deadline already past runs as soon as the worker
 * gets the CPU.
 *
 * Returns this job's sequence number, always POSITIVE, which the callback
 * also sees as yrt_job_info.seq; on failure a negative YRT_ERR_ARG or
 * YRT_ERR_STOPPED. Test `rc < 0`, not `rc != YRT_OK`. Callable from any
 * thread, including from a job on this same worker. Allocates nothing. */
YRT_API int yrt_worker_submit(yrt_worker* w, int64_t deadline_ns,
                                  yrt_job_fn fn, void* ctx);

/* Drop the pending job, if any. Returns 1 if a job was canceled, 0 if there
 * was none, or a negative YRT_ERR_* code. A job already inside its final
 * spin window, or already running, cannot be canceled; see WORKER. */
YRT_API int yrt_worker_cancel(yrt_worker* w);

/* The seq of the job waiting for its deadline, or 0 when none is pending or
 * the worker is not running. A job that has already been dispatched is no
 * longer pending, so this cannot be used to decide whether a callback will
 * still run; it answers "is something armed", not "is something done". Takes
 * the worker's lock, so it is not for a hot loop. */
YRT_API uint32_t yrt_worker_pending(const yrt_worker* w);

/* The rung the worker thread obtained. YRT_POLICY_NONE before a successful
 * start and after a stop. */
YRT_API yrt_policy yrt_worker_policy(const yrt_worker* w);

/* Last yrt_worker_open() message for this handle ("" if none). */
YRT_API const char* yrt_worker_error(const yrt_worker* w);

/* True while the worker thread is running. */
YRT_API bool yrt_worker_is_running(const yrt_worker* w);

/* --- pump --------------------------------------------------------------- */

/* Size of the ring a pump gets when yrt_pump_desc.ring is NULL. It lives
 * inside every yrt_pump handle, used or not, so override it (before the
 * include, in every translation unit that sees the handle) when your messages
 * or your capacity do not fit, or when you always supply your own ring and
 * want the 4 KB back. See BUILDING. */
#ifndef YRT_PUMP_INLINE_BYTES
#define YRT_PUMP_INLINE_BYTES 4096
#endif

/* One message, on the pump thread, in submit order, with NEITHER of the pump's
 * locks held. `msg` points AT THE RING SLOT and is valid only until this
 * returns; copy what you need to keep. `seq` is what yrt_pump_submit()
 * returned for it, and yrt_pump_done_seq() reports it once this function has
 * returned. Take yrt_pump_lock() around the publish at the end, not around
 * the work; see PUMP. */
typedef void (*yrt_msg_fn)(void* ctx, const void* msg, uint32_t seq);

/* Spare-time work, on the pump thread, with neither lock held, called whenever
 * the ring is empty. Return true to be called again, false to let the thread
 * block until the next submit or the close. MUST RETURN PROMPTLY, in a few
 * milliseconds: a queued message and yrt_pump_close() both wait behind it.
 * One step of a resumable fit is the intended shape. */
typedef bool (*yrt_idle_fn)(void* ctx);

/* Pump description. Zero-initialize it and set only what you need; msg_size,
 * capacity and on_msg have no useful default and are required. */
typedef struct yrt_pump_desc {
    size_t        msg_size;   /* bytes per message, > 0. Every submit copies
                               * exactly this many.                          */
    uint32_t      capacity;   /* messages the ring holds, > 0. A message being
                               * handled still holds its slot.               */
    void*         ring;       /* at least msg_size * capacity bytes, owned by
                               * the caller and alive until the pump stops;
                               * NULL uses the handle's inline buffer, which
                               * must be big enough (YRT_PUMP_INLINE_BYTES).
                               * The size of a buffer you supply cannot be
                               * checked here; get it right.                 */
    yrt_msg_fn  on_msg;     /* required                                    */
    yrt_idle_fn on_idle;    /* NULL = the thread just blocks when idle      */
    void*         ctx;        /* passed to on_msg and on_idle                */
    yrt_start_fn on_start;  /* per-thread setup, on the pump thread, after
                               * the priority and the pin and before
                               * yrt_pump_open() returns; NULL = none.
                               * False fails the open. Same contract as
                               * yrt_worker_desc.on_start.                 */
    void*    start_ctx;       /* passed to on_start                          */
    bool     below_normal;    /* run the thread BELOW normal priority. Default
                               * is normal. The pump never goes above it; see
                               * PUMP.                                       */
    int      pin_cpu;         /* logical CPU to pin the thread to. 0 and
                               * negative mean no pinning, so a zeroed desc
                               * does not pin: the repository's "a zero field
                               * means default" rule wins over the usual -1
                               * sentinel, and CPU 0 is not where background
                               * work belongs anyway (it takes the timer tick
                               * and most interrupts). To pin there, call
                               * yrt_thread_pin(0) from on_start, which runs
                               * on the pump thread.                          */
    bool     drop_on_stop;    /* stop discards the queue instead of draining
                               * it. Default drains; see PUMP.               */
} yrt_pump_desc;

/* Pump handle. The caller allocates it and treats every field as opaque; it
 * owns no heap. The layout is ordered by what a frame loop touches: the done
 * seq first, because polling it every frame is the hot path and it is read
 * without any lock, then the ring's bookkeeping and the callbacks, then the OS
 * objects, and the error buffer and the inline ring last, where the cold bytes
 * cannot push the hot ones out of a line. Must be zeroed or closed before
 * yrt_pump_open(); a fresh handle has no valid running flag, so start()
 * cannot detect a pump already running in it. */
typedef struct yrt_pump {
    /* --- the door: fields another thread may touch at ANY moment, including
     * while yrt_pump_close() tears the pump down and yrt_pump_open()
     * builds it again. Every access is atomic and yrt_pump_open() never
     * memsets them, so a yrt_pump_wait() racing a close or a reopen touches
     * nothing a plain store is writing. `done` is also the frame loop's poll,
     * which is why the door is first. --- */
    int      done;          /* highest seq whose on_msg returned            */
    int      running;       /* submits are accepted                         */
    int      locks_live;    /* the mutexes exist and a new waiter may take
                             * them. Outlives `running`: the callbacks still
                             * publish during a drain, after a close has
                             * already refused new submits                  */
    int      waiters;       /* yrt_pump_wait()s registered on this pump. A
                             * stop destroys nothing until it reads 0       */
    /* --- hot: the ring --- */
    unsigned char* ring;    /* desc.ring, or inline_ring below              */
    size_t   msg_size;
    uint32_t capacity;
    uint32_t head;          /* slot to hand to on_msg next                  */
    uint32_t tail;          /* slot the next submit fills                   */
    uint32_t count;         /* slots occupied, including one in flight      */
    uint32_t head_seq;      /* seq of the message at head                   */
    uint32_t seq;           /* last seq handed out by a submit              */
    int      busy;          /* on_msg is running, so head's slot is in use
                             * and is not "pending" any more                */
    int      quit;
    int      exited;        /* the pump thread has left its loop: a waiter
                             * whose seq is not done by now never will be  */
    int      ready;
    yrt_msg_fn  on_msg;
    yrt_idle_fn on_idle;
    void*    ctx;
    /* OS objects (the ring mutex and its condition variable, the wait
     * condition variable, the caller's publish mutex, the thread), inline so
     * the pump owns no heap. The implementation asserts at compile time that
     * they fit and are aligned. */
    union { void* p[40]; uint64_t u[40]; double d[40]; } os;
    /* --- cold: setup, teardown, diagnostics --- */
    yrt_start_fn on_start;
    void*    start_ctx;
    int      start_failed;  /* the hook refused; written before the ready
                             * handshake, read by open() after it          */
    int      drop_on_stop;
    int      pin_cpu;
    bool     below_normal;
    yrt_policy policy;
    char     error[256];
    /* Last, and the largest thing here: the ring a desc without one uses. */
    unsigned char inline_ring[YRT_PUMP_INLINE_BYTES];
} yrt_pump;

/* Start `p`'s thread per `desc`. Returns true once the thread is up, has set
 * its priority and affinity, has run desc.on_start and has published
 * p->policy, so all of that is final before the first submit. On failure
 * returns false and leaves a message in yrt_pump_error(): a NULL desc, a
 * missing on_msg, a zero msg_size or capacity, a ring the handle's inline
 * buffer is too small for, a refused on_start, or an OS object that could not
 * be created. This is the only pump call that writes that message. It must
 * not run while another thread submits, locks or stops the same handle; a
 * yrt_pump_wait() on another thread is allowed (see that function). */
YRT_API bool yrt_pump_open(yrt_pump* p, const yrt_pump_desc* desc);

/* Refuse further submits, deliver what is already queued (or discard it, with
 * desc.drop_on_stop), and join the thread. on_msg and on_idle are not running
 * when this returns, so the ring and the ctx can be reused or freed. Safe on a
 * zeroed or already-closed handle. Must not race an open, a submit, a
 * yrt_pump_lock() or another close on the same handle. It MAY race
 * yrt_pump_wait() from any thread at any point: see that function. */
YRT_API void yrt_pump_close(yrt_pump* p);

/* Copy `msg` (desc.msg_size bytes) into the ring and return its sequence
 * number, always POSITIVE and increasing, which on_msg also receives and
 * yrt_pump_done_seq() reports once on_msg has returned.
 *
 * Negative on failure: YRT_ERR_ARG for a null handle or message,
 * YRT_ERR_STOPPED when the pump is not running, YRT_ERR_FULL when the ring
 * has no free slot. ON YRT_ERR_FULL NOTHING WAS COPIED and the caller still
 * owns its message: retry it next frame, or drop it deliberately. Test
 * `rc < 0`, not `rc != YRT_OK`.
 *
 * Callable from any thread and from several at once (it takes the ring's
 * mutex); the resulting order is the order the mutex granted. Allocates
 * nothing. The seq space is 1..INT32_MAX and wraps to 1 after that, so a seq
 * kept across the wrap is meaningless; at a thousand messages a second that is
 * twenty-five days of submitting. */
YRT_API int yrt_pump_submit(yrt_pump* p, const void* msg);

/* Messages queued and not yet handed to on_msg (the one in flight is not
 * counted), or 0 when the pump is not running. Takes the ring's mutex, so it
 * is for a log line or an assertion, not a frame loop: poll
 * yrt_pump_done_seq() there instead. */
YRT_API int yrt_pump_pending(const yrt_pump* p);

/* The highest seq whose on_msg has RETURNED, or 0 before the first one. One
 * atomic load, no lock, no syscall: this is the call a frame loop makes.
 * `yrt_pump_done_seq(p) >= (uint32_t)seq` is the "is my result ready?" test,
 * and it is true only after the callback returned, so whatever that callback
 * published under yrt_pump_lock() is there to be read. */
YRT_API uint32_t yrt_pump_done_seq(const yrt_pump* p);

/* Block until yrt_pump_done_seq() reaches `seq`, or until `timeout_ns` of
 * monotonic time has passed. Returns YRT_OK (0) when the pump has passed the
 * seq, YRT_ERR_TIMEOUT when the timeout ran out first, YRT_ERR_ARG for a
 * null handle, and YRT_ERR_STOPPED when the pump is not running and never
 * reached that seq (a seq it DID reach still returns 0 after a stop).
 *
 * timeout_ns is relative and 0 means "poll, do not block"; there is no
 * infinite form, so pass a timeout you are willing to wait and treat
 * YRT_ERR_TIMEOUT as the answer. An enormous timeout standing in for
 * "forever" is safe: the wait is taken a second at a time internally. This is
 * the inter-trial-interval call; a frame loop polls yrt_pump_done_seq()
 * instead.
 *
 * Callable from any thread, several at once, and CONCURRENTLY WITH
 * yrt_pump_close() AND yrt_pump_open() AT ANY POINT: before the close, in
 * the middle of it, after it, or across a close and a reopen. It never touches
 * an OS object the close has destroyed or the open has not finished building,
 * and the close does not destroy anything until every wait that got in has left.
 * A wait racing a close returns 0 if the pump reached the seq (a draining close
 * still delivers the queue, so a seq already submitted usually is reached),
 * YRT_ERR_STOPPED if it did not, and never hangs past its timeout. A wait
 * racing a reopen may see either pump; the seq numbers of the new one start
 * again at 1, so a seq kept across a reopen is the caller's to retire. */
YRT_API int yrt_pump_wait(yrt_pump* p, uint32_t seq, int64_t timeout_ns);

/* Take and release the pump's PUBLISH mutex, which is not the ring's: it
 * guards nothing inside the pump and exists only so the caller and the
 * callbacks can share a struct of results. on_msg runs WITHOUT it held and
 * should take it only around its publish, for the few instructions a struct
 * copy costs, so holding it in the caller stalls nothing but that publish, not
 * the inference, not a submit, not a done_seq poll. Recursive locking is not
 * supported. A lock on a pump that is not running is a no-op, which keeps a
 * caller's read-the-results path working after a close. */
YRT_API void yrt_pump_lock(yrt_pump* p);
YRT_API void yrt_pump_unlock(yrt_pump* p);

/* True while the pump thread is running. */
YRT_API bool yrt_pump_is_running(const yrt_pump* p);

/* Last yrt_pump_open() message for this handle ("" if none). */
YRT_API const char* yrt_pump_error(const yrt_pump* p);

/* What the pump thread runs at: YRT_POLICY_NORMAL, or
 * YRT_POLICY_BELOW_NORMAL when desc.below_normal was set AND the OS granted
 * the drop. YRT_POLICY_NONE before a successful open and after a close.
 * Never anything higher: the pump does not climb the ladder. */
YRT_API yrt_policy yrt_pump_policy(const yrt_pump* p);

#endif /* YRT_NO_THREADS */

#ifdef __cplusplus
} /* extern "C" */
#endif

/* C++ only: a zone that ends when the scope does. C has no destructors, so
 * this is not defined there and costs C nothing. */
#ifdef __cplusplus
    #define YRT__PASTE2(a, b) a##b
    #define YRT__PASTE(a, b)  YRT__PASTE2(a, b)
    #if defined(YRT_TRACE_RING)
        struct yrt__scoped_zone {
            yrt_zone z;
            explicit yrt__scoped_zone(const yrt_srcloc* l) : z(yrt__zone_begin(l)) {}
            ~yrt__scoped_zone() { yrt__zone_end(&z); }
            yrt__scoped_zone(const yrt__scoped_zone&) = delete;
            yrt__scoped_zone& operator=(const yrt__scoped_zone&) = delete;
        };
        #define YRT_ZONE_SCOPED(name) \
            static const yrt_srcloc YRT__PASTE(yrt__ssl_, __LINE__) = \
                { name, __func__, __FILE__, (uint32_t)__LINE__, 0u }; \
            yrt__scoped_zone YRT__PASTE(yrt__sz_, __LINE__)(&YRT__PASTE(yrt__ssl_, __LINE__))
    #elif defined(YRT_TRACY)
        struct yrt__scoped_zone {
            TracyCZoneCtx z;
            explicit yrt__scoped_zone(const ___tracy_source_location_data* l)
                : z(___tracy_emit_zone_begin(l, 1)) {}
            ~yrt__scoped_zone() { ___tracy_emit_zone_end(z); }
            yrt__scoped_zone(const yrt__scoped_zone&) = delete;
            yrt__scoped_zone& operator=(const yrt__scoped_zone&) = delete;
        };
        #define YRT_ZONE_SCOPED(name) \
            static const ___tracy_source_location_data YRT__PASTE(yrt__ssl_, __LINE__) = \
                { name, __func__, __FILE__, (uint32_t)__LINE__, 0u }; \
            yrt__scoped_zone YRT__PASTE(yrt__sz_, __LINE__)(&YRT__PASTE(yrt__ssl_, __LINE__))
    #else
        #define YRT_ZONE_SCOPED(name) ((void)0)
    #endif
#endif

#endif /* YSP_RT_H_INCLUDED */

/* ======================================================================= *
 *                            IMPLEMENTATION                               *
 * ======================================================================= */
#ifdef YSP_RT_IMPLEMENTATION
#ifndef YSP_RT_IMPLEMENTATION_GUARD
#define YSP_RT_IMPLEMENTATION_GUARD

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* The implementation keeps unsigned clock arithmetic, where a deadline before
 * the clock's zero saturates instead of wrapping; the public calls take and
 * return int64_t and convert at the boundary. */
static uint64_t yrt__now(void);
static uint64_t yrt__spin_until(uint64_t deadline_ns);
static uint64_t yrt__sleep_until(uint64_t deadline_ns, uint64_t spin_ns);

/* Pick the concrete platform once. */
#if defined(_WIN32)
    #define YRT__WINDOWS 1
#elif defined(__EMSCRIPTEN__)
    /* Before the Linux test on purpose: Emscripten is a POSIX subset with its
     * own rules (no scheduling ladder, no affinity, no mlockall, no absolute
     * clock_nanosleep promise), and none of the Linux-only syscalls below
     * exist in a wasm module. See WEBASSEMBLY. */
    #define YRT__POSIX 1
    #define YRT__EMSCRIPTEN 1
#elif defined(__linux__)
    #define YRT__POSIX 1
    #define YRT__LINUX 1
#elif defined(__APPLE__)
    #define YRT__POSIX 1
    #define YRT__DARWIN 1
#else
    #error "ysp_rt: unsupported platform (need Windows, Linux, macOS or Emscripten)"
#endif

/* The third form covers MSVC's legacy C dialect, which is what setuptools and
 * MATLAB's mex compile with unless they are told otherwise. It has neither
 * _Static_assert nor _Alignof: a typedef whose array bound goes negative fails
 * the same build with a worse message, and a char in front of the type puts
 * the type's alignment in the second member's offset. */
#if defined(__cplusplus)
    #define YRT__STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    #define YRT__STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
    #define YRT__CAT2(a, b) a##b
    #define YRT__CAT(a, b)  YRT__CAT2(a, b)
    #define YRT__STATIC_ASSERT(cond, msg) \
        typedef char YRT__CAT(yrt__assert_, __LINE__)[(cond) ? 1 : -1]
#endif

/* YRT__ALIGN_PROBE(T) declares what the pre-C11 YRT__ALIGNOF(T) measures,
 * and is a bare forward declaration where the language has an alignof of its
 * own. The helper struct is NAMED after its type rather than written inline,
 * because an unnamed type defined inside offsetof() is MSVC's C4116 and is
 * not usable twice. */
#if defined(__cplusplus)
    #define YRT__ALIGN_PROBE(T) struct yrt__align_probe_##T
    #define YRT__ALIGNOF(T)     alignof(T)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    #define YRT__ALIGN_PROBE(T) struct yrt__align_probe_##T
    #define YRT__ALIGNOF(T)     _Alignof(T)
#else
    #define YRT__ALIGN_PROBE(T) struct yrt__align_probe_##T { char yrt__c; T yrt__v; }
    #define YRT__ALIGNOF(T)     offsetof(struct yrt__align_probe_##T, yrt__v)
#endif

/* The pause / yield hint: tells the core this is a spin-wait, so it stops
 * speculating down the loop and stops starving a sibling hyperthread. Not a
 * sleep and not a barrier; on a core that has no such hint it compiles away. */
#if defined(_MSC_VER)
    #include <intrin.h>
    #if defined(_M_ARM64) || defined(_M_ARM)
        #define YRT__PAUSE() __yield()
    #else
        #define YRT__PAUSE() _mm_pause()
    #endif
#elif defined(__i386__) || defined(__x86_64__)
    #define YRT__PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__) || defined(__arm__)
    #define YRT__PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
    #define YRT__PAUSE() ((void)0)
#endif

/* The Windows system tick with no high-resolution timer. Rounded up from the
 * usual 15.625 ms so a wait that must spin the last tick spins a whole one. */
#define YRT__TICK_NS 16000000ull

#if defined(__cplusplus)
    #define YRT__THREAD_LOCAL thread_local
#elif defined(_MSC_VER)
    #define YRT__THREAD_LOCAL __declspec(thread)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    #define YRT__THREAD_LOCAL _Thread_local
#else
    #define YRT__THREAD_LOCAL __thread
#endif

/* The event ring's atomics. Separate from the pump's flag helpers because
 * the ring exists in a YRT_NO_THREADS build too: other libraries' threads
 * (an audio callback) push into it. One fetch-add is one instruction on
 * x86-64, on ARMv8.1+ with LSE and in WebAssembly, which is what makes the
 * push wait-free there. On MSVC x86/x64 a volatile access has acquire or
 * release semantics (/volatile:ms) and the barrier only stops the compiler;
 * ARM64 has no such default, so it gets the explicit load-acquire and
 * store-release instructions. */
#if defined(_MSC_VER)
static uint32_t yrt__rfadd(uint32_t* p, uint32_t v) {
    return (uint32_t)_InterlockedExchangeAdd((volatile long*)p, (long)v);
}
    #if defined(_M_ARM64)
static uint32_t yrt__rload(const uint32_t* p) {
    return (uint32_t)__ldar32((unsigned __int32 volatile*)(uintptr_t)p);
}
static void yrt__rstore(uint32_t* p, uint32_t v) {
    __stlr32((unsigned __int32 volatile*)p, v);
}
static void* yrt__pload(void* const* p) {
    return (void*)(uintptr_t)__ldar64((unsigned __int64 volatile*)(uintptr_t)p);
}
    #else
static uint32_t yrt__rload(const uint32_t* p) {
    uint32_t v = *(const volatile uint32_t*)p;
    _ReadWriteBarrier();
    return v;
}
static void yrt__rstore(uint32_t* p, uint32_t v) {
    _ReadWriteBarrier();
    *(volatile uint32_t*)p = v;
}
/* A plain load, not an interlocked one: every traced zone reads this
 * pointer, and an interlocked read would take the line exclusive and make
 * every tracing thread fight over it. */
static void* yrt__pload(void* const* p) {
    void* v = *(void* const volatile*)p;
    _ReadWriteBarrier();
    return v;
}
    #endif
static void yrt__pstore(void** p, void* v) {
    (void)_InterlockedExchangePointer((void* volatile*)p, v);
}
#elif defined(__GNUC__) || defined(__clang__)
static uint32_t yrt__rfadd(uint32_t* p, uint32_t v) {
    return __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL);
}
static uint32_t yrt__rload(const uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void yrt__rstore(uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static void* yrt__pload(void* const* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void yrt__pstore(void** p, void* v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
#else
#error "ysp_rt: the event ring needs atomic fetch-add (GCC, Clang or MSVC builtins)"
#endif

/* Core placement of the calling thread, for yrt_report_get(): the CPU
 * yrt_thread_pin() bound it to plus one (0 = none), and whether
 * yrt_thread_elevate() steered it to the performance cores. */
static YRT__THREAD_LOCAL int yrt__pinned_plus1 = 0;
static YRT__THREAD_LOCAL int yrt__pcores_set = 0;

/* The record is one cache line on every target, wasm32 and its 4-byte
 * pointers included, and the publish word comes first so a push can copy
 * the other 60 bytes in one piece and then publish. */
YRT__STATIC_ASSERT(sizeof(yrt_payload) == 40, "yrt_payload must be 40 bytes");
YRT__STATIC_ASSERT(sizeof(yrt_event) == 64, "yrt_event must be 64 bytes");
YRT__STATIC_ASSERT(offsetof(yrt_event, seq) == 0, "yrt_event.seq must be first");
YRT__STATIC_ASSERT(offsetof(yrt_event, t_ns) == 8, "yrt_event.t_ns at 8");
YRT__STATIC_ASSERT(offsetof(yrt_event, u) == 24, "yrt_event.u at 24");
YRT__STATIC_ASSERT(offsetof(yrt_ring, head) == 128, "ring: head 128 bytes past used");
YRT__STATIC_ASSERT(offsetof(yrt_ring, lost) == 256, "ring: drainer 128 bytes past head");
YRT__STATIC_ASSERT(offsetof(yrt_ring, error) == 384, "ring: cold 128 bytes past drainer");

/* Process-wide outcomes, for yrt_report_get(). Both are set by functions
 * the manual says to call once from one thread, so a plain static is honest
 * here and a lock would only hide misuse. */
static bool yrt__mem_locked = false;
static bool yrt__timer_res_raised = false;

/* ======================================================================= *
 *  WINDOWS
 * ======================================================================= */
#if defined(YRT__WINDOWS)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Split the multiply: a TSC-rate counter (GHz) times 1e9 overflows 64 bits
 * within hours of uptime if done in one step. The one conversion both
 * yrt_now_ns() and yrt_ticks_to_ns() use, so the two agree to the bit. */
static uint64_t yrt__qpc_to_ns(uint64_t t, uint64_t fr) {
    return (t / fr) * 1000000000ull + (t % fr) * 1000000000ull / fr;
}

static uint64_t yrt__now(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return yrt__qpc_to_ns((uint64_t)c.QuadPart, (uint64_t)f.QuadPart);
}

int64_t yrt_ticks_to_ns(int64_t ticks) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    if (ticks < 0)
        return -(int64_t)yrt__qpc_to_ns(0u - (uint64_t)ticks, (uint64_t)f.QuadPart);
    return (int64_t)yrt__qpc_to_ns((uint64_t)ticks, (uint64_t)f.QuadPart);
}

/* The tick yrt_now_ns() counts in, without yrt_get_clock_info()'s
 * waitable timer, for yrt_correlate()'s floor on a bracket's width. */
static uint64_t yrt__clock_step_ns(void) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart > 0 ? (1000000000ull + (uint64_t)f.QuadPart - 1) / (uint64_t)f.QuadPart : 1;
}

uint32_t yrt_thread_id(void) {
    return (uint32_t)GetCurrentThreadId();
}

/* QueryInterruptTimePrecise is Windows 10 only and lives in an API set, not
 * in kernel32's import library, so it is looked up once. A racing first call
 * on two threads looks it up twice and stores the same pointer. */
typedef VOID (WINAPI *yrt__qitp_fn)(PULONGLONG);
static void* yrt__qitp = NULL;      /* the function, as an integer */
static uint32_t yrt__qitp_tried = 0;

uint64_t yrt_interrupt_time_100ns(void) {
    /* Through uintptr_t both ways: ISO C has no conversion between object
     * and function pointers, and MinGW's -Wpedantic says so. */
    yrt__qitp_fn fn = (yrt__qitp_fn)(uintptr_t)yrt__pload(&yrt__qitp);
    ULONGLONG t = 0;
    if (!fn && !yrt__rload(&yrt__qitp_tried)) {
        HMODULE k = GetModuleHandleW(L"kernelbase.dll");
        fn = k ? (yrt__qitp_fn)(void (*)(void))GetProcAddress(k, "QueryInterruptTimePrecise")
               : NULL;
        yrt__pstore(&yrt__qitp, (void*)(uintptr_t)fn);
        yrt__rstore(&yrt__qitp_tried, 1u);
    }
    if (!fn) return 0;
    fn(&t);
    return (uint64_t)t;
}

/* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, spelled out because the SDK only
 * defines it when the target version is Win10 1803 or newer. */
#define YRT__TIMER_HIRES 0x00000002

/* Create the best waitable timer this machine offers. *hires tells which one
 * came back, which decides whether a caller must spin a whole system tick. */
static HANDLE yrt__timer_create(bool* hires) {
    HANDLE h = CreateWaitableTimerExW(NULL, NULL, YRT__TIMER_HIRES,
                                      TIMER_ALL_ACCESS);
    *hires = (h != NULL);
    if (!h) h = CreateWaitableTimerW(NULL, FALSE, NULL);
    return h;
}

/* One waitable timer per thread that waits. Creating a kernel object inside
 * every yrt_sleep_until() would put a syscall in the path the function
 * exists to keep short. */
typedef struct yrt__tls {
    HANDLE timer;
    bool   hires;
    bool   tried;
} yrt__tls;

static YRT__THREAD_LOCAL yrt__tls yrt__tls_state;

static yrt__tls* yrt__tls_get(void) {
    yrt__tls* t = &yrt__tls_state;
    if (!t->tried) {
        t->tried = true;
        t->timer = yrt__timer_create(&t->hires);
    }
    return t;
}

void yrt_thread_cleanup(void) {
    yrt__tls* t = &yrt__tls_state;
    if (t->timer) CloseHandle(t->timer);
    t->timer = NULL;
    t->hires = false;
    t->tried = false;
}

/* Hand the wait to the OS until an absolute monotonic time. Returns as soon
 * as the timer fires; the caller re-reads the clock and decides what is left. */
static void yrt__coarse_wait_until(HANDLE timer, uint64_t until_ns) {
    uint64_t now = yrt__now();
    if (until_ns <= now) return;
    uint64_t delta = until_ns - now;
    if (timer) {
        LARGE_INTEGER due; /* negative = relative, 100 ns units */
        due.QuadPart = -(LONGLONG)(delta / 100ull);
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    /* No timer at all: Sleep is coarse, but it is still better than spinning
     * out an arbitrarily long wait. */
    Sleep((DWORD)(delta / 1000000ull));
}

void yrt_get_clock_info(yrt_clock_info* out) {
    if (!out) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    uint64_t fr = (uint64_t)f.QuadPart;
    out->resolution_ns = fr ? (int64_t)((1000000000ull + fr - 1) / fr) : 0;
    bool hires = false;
    HANDLE h = yrt__timer_create(&hires);
    if (h) CloseHandle(h);
    out->hires_timer = hires;
}

static uint64_t yrt__sleep_until(uint64_t deadline_ns, uint64_t spin_ns) {
    yrt__tls* t = yrt__tls_get();
    uint64_t spin = spin_ns;
    /* The fallback timer only fires on the system tick, so the coarse wait
     * has to stop a whole tick early and spin the rest. Overshooting the
     * deadline by 15 ms is worse than burning 15 ms of one core. */
    if (!t->hires && spin < YRT__TICK_NS) spin = YRT__TICK_NS;
    uint64_t now = yrt__now();
    if (now >= deadline_ns) return now - deadline_ns;
    YRT_ZONE(z, "yrt.sleep");
    uint64_t wake = (deadline_ns > spin) ? deadline_ns - spin : 0;
    if (now < wake) yrt__coarse_wait_until(t->timer, wake);
    uint64_t late = yrt__spin_until(deadline_ns);
    YRT_ZONE_END(z);
    return late;
}

int yrt_thread_set_timer_slack(int64_t ns) {
    (void)ns;
    return YRT_ERR_UNSUPPORTED; /* Windows has no per-thread timer slack to set. */
}

/* CPU sets (Windows 10 and later), looked up at run time like the other
 * newer calls. The record layout is SYSTEM_CPU_SET_INFORMATION's, spelled
 * out because older SDKs and MinGW's headers lack the type. */
typedef struct yrt__cpuset {
    DWORD   size;
    DWORD   type;        /* 0 = CpuSetInformation */
    DWORD   id;
    WORD    group;
    BYTE    lp;          /* LogicalProcessorIndex within the group */
    BYTE    core;
    BYTE    llc;
    BYTE    numa;
    BYTE    eff;         /* EfficiencyClass: higher is faster */
    BYTE    flags;
    DWORD   reserved;
    DWORD64 tag;
} yrt__cpuset;
YRT__STATIC_ASSERT(sizeof(yrt__cpuset) == 32, "SYSTEM_CPU_SET_INFORMATION is 32 bytes");
typedef BOOL (WINAPI *yrt__cpusetinfo_fn)(void*, ULONG, PULONG, HANDLE, ULONG);
typedef BOOL (WINAPI *yrt__selcpus_fn)(HANDLE, const ULONG*, ULONG);

typedef struct yrt__topo {
    BYTE  eff[64];       /* group 0, by logical processor */
    bool  have[64];
    BYTE  top, bottom;   /* highest and lowest class anywhere */
    ULONG pids[256];     /* the CPU set ids of the highest class */
    int   npids;
} yrt__topo;

/* False when the OS has no CPU sets or the machine has more than 256. */
static bool yrt__topology(yrt__topo* t) {
    unsigned char buf[256 * sizeof(yrt__cpuset)];
    ULONG len = (ULONG)sizeof(buf), off;
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    yrt__cpusetinfo_fn info = k ? (yrt__cpusetinfo_fn)(void (*)(void))
        GetProcAddress(k, "GetSystemCpuSetInformation") : NULL;
    memset(t, 0, sizeof(*t));
    t->bottom = 255;
    if (!info || !info(buf, len, &len, GetCurrentProcess(), 0)) return false;
    for (off = 0; off + sizeof(yrt__cpuset) <= len; ) {
        yrt__cpuset c;
        memcpy(&c, buf + off, sizeof(c));
        if (c.size < sizeof(c)) break;
        if (c.type == 0) {
            if (c.eff > t->top) t->top = c.eff;
            if (c.eff < t->bottom) t->bottom = c.eff;
            if (c.group == 0 && c.lp < 64) { t->eff[c.lp] = c.eff; t->have[c.lp] = true; }
        }
        off += c.size;
    }
    for (off = 0; off + sizeof(yrt__cpuset) <= len; ) {
        yrt__cpuset c;
        memcpy(&c, buf + off, sizeof(c));
        if (c.size < sizeof(c)) break;
        if (c.type == 0 && c.eff == t->top && t->npids < 256) t->pids[t->npids++] = c.id;
        off += c.size;
    }
    return t->bottom != 255;
}

static yrt__selcpus_fn yrt__selcpus(void) {
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    return k ? (yrt__selcpus_fn)(void (*)(void))GetProcAddress(k, "SetThreadSelectedCpuSets")
             : NULL;
}

yrt_core_type yrt_cpu_core_type(int cpu) {
    yrt__topo t;
    if (cpu < 0 || cpu >= 64 || !yrt__topology(&t) || !t.have[cpu]) return YRT_CORE_UNKNOWN;
    if (t.top == t.bottom) return YRT_CORE_UNIFORM;
    return t.eff[cpu] == t.top ? YRT_CORE_PERFORMANCE : YRT_CORE_EFFICIENCY;
}

/* Prefer the performance cores for the calling thread, on a hybrid CPU
 * only. A preference, not a binding: the CPU sets are soft, and Windows
 * still runs the thread elsewhere when they are unavailable. */
static void yrt__prefer_pcores(void) {
    yrt__topo t;
    yrt__selcpus_fn sel = yrt__selcpus();
    if (!sel || !yrt__topology(&t) || t.top == t.bottom || t.npids == 0) return;
    if (sel(GetCurrentThread(), t.pids, (ULONG)t.npids)) yrt__pcores_set = 1;
}

int yrt_thread_pin(int cpu) {
    if (cpu < 0 || cpu >= (int)(8 * sizeof(DWORD_PTR))) return YRT_ERR_ARG;
    DWORD_PTR mask = (DWORD_PTR)1 << cpu;
    if (SetThreadAffinityMask(GetCurrentThread(), mask) == 0) return YRT_ERR_REFUSED;
    /* The pin is the explicit choice: drop a P-core preference that could
     * disagree with it. */
    {
        yrt__selcpus_fn sel = yrt__selcpus();
        if (sel && sel(GetCurrentThread(), NULL, 0)) yrt__pcores_set = 0;
    }
    yrt__pinned_plus1 = cpu + 1;
    return 0;
}

int yrt_process_lock_memory(void) {
    /* Deliberately empty. See the declaration: nothing Windows offers is
     * mlockall, and a partial measure logged as a success is worse than a
     * documented refusal. */
    return YRT_ERR_UNSUPPORTED;
}

/* winmm is loaded at run time so this header still links against nothing.
 * timeBeginPeriod is called once per process here, so the load cost is not
 * on any timing path. */
typedef UINT (WINAPI *yrt__timeperiod_fn)(UINT); /* MMRESULT, which
 * lives in mmsystem.h; this header does not include it. */
static HMODULE yrt__winmm = NULL;
static yrt__timeperiod_fn yrt__time_end = NULL;

/* Windows 11 power throttling, through SetProcessInformation and
 * SetThreadInformation, looked up at run time: both are Windows 8 calls and
 * the throttling classes are Windows 10 1709 and later, and older SDKs and
 * MinGW's headers do not have the names. A refused call is not an error; the
 * report says what took. The constants and the struct layout are the SDK's
 * (processthreadsapi.h). */
#define YRT__PT_VERSION     1u
#define YRT__PT_EXEC_SPEED  0x1u   /* EcoQoS: efficiency cores, low clock */
#define YRT__PT_IGNORE_TIMER 0x4u  /* keep the timer resolution when hidden */
#define YRT__PROCESS_POWER_THROTTLING 4
#define YRT__THREAD_POWER_THROTTLING  3
typedef struct yrt__pt_state { ULONG version, control, state; } yrt__pt_state;
typedef BOOL (WINAPI *yrt__setinfo_fn)(HANDLE, int, LPVOID, DWORD);

static yrt__setinfo_fn yrt__k32(const char* name) {
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    return k ? (yrt__setinfo_fn)(void (*)(void))GetProcAddress(k, name) : NULL;
}

/* Set when this header opted the calling thread out of EcoQoS. A 32-bit
 * process under WOW64 can set the thread's state but not read it back
 * (GetThreadInformation fails with ERROR_INVALID_PARAMETER, measured on
 * build 26200), so the report falls back to this. */
static YRT__THREAD_LOCAL int yrt__ecoqos_set = 0;

/* Take the throttling in `mask` away from the calling thread or the whole
 * process: control says "this process decides", state 0 says "never". */
static bool yrt__throttle_off(bool thread, ULONG mask) {
    yrt__setinfo_fn set = yrt__k32(thread ? "SetThreadInformation" : "SetProcessInformation");
    yrt__pt_state s;
    s.version = YRT__PT_VERSION;
    s.control = mask;
    s.state   = 0;
    if (!set) return false;
    if (!set(thread ? GetCurrentThread() : GetCurrentProcess(),
             thread ? YRT__THREAD_POWER_THROTTLING : YRT__PROCESS_POWER_THROTTLING,
             &s, (DWORD)sizeof(s)))
        return false;
    if (thread && (mask & YRT__PT_EXEC_SPEED)) yrt__ecoqos_set = 1;
    return true;
}

/* Whether the calling thread is opted out of EcoQoS: read back from the OS
 * where it can say, so the report also sees a thread someone else set up,
 * and otherwise what this header did. -1 when the OS has no such setting. */
static int yrt__thread_ecoqos_off(void) {
    yrt__setinfo_fn get = yrt__k32("GetThreadInformation");
    yrt__pt_state s;
    s.version = YRT__PT_VERSION;
    s.control = 0;
    s.state   = 0;
    if (get && get(GetCurrentThread(), YRT__THREAD_POWER_THROTTLING, &s, (DWORD)sizeof(s)))
        return ((s.control & YRT__PT_EXEC_SPEED) && !(s.state & YRT__PT_EXEC_SPEED)) ? 1 : 0;
    if (yrt__ecoqos_set) return 1;
    return yrt__k32("SetThreadInformation") ? 0 : -1;
}

/* Process-wide and set by the call the manual says to make once from one
 * thread, like yrt__timer_res_raised. */
static bool yrt__timer_throttle_off = false;

int yrt_timer_resolution_begin(void) {
    if (yrt__timer_res_raised) return 0;
    if (!yrt__winmm) {
        /* System32 only: this is a system DLL and must not be picked up from
         * a data directory next to the experiment. */
        yrt__winmm = LoadLibraryExA("winmm.dll", NULL,
                                      LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!yrt__winmm) return YRT_ERR_UNSUPPORTED;
    }
    /* FARPROC to the real signature by way of void (*)(void): ISO C has no
     * conversion between object and function pointers, so a void* detour is
     * a pedantic error on MinGW, and GCC exempts only the generic function
     * type from -Wcast-function-type. */
    yrt__timeperiod_fn begin = (yrt__timeperiod_fn)(void (*)(void))
        GetProcAddress(yrt__winmm, "timeBeginPeriod");
    yrt__time_end = (yrt__timeperiod_fn)(void (*)(void))
        GetProcAddress(yrt__winmm, "timeEndPeriod");
    if (!begin || !yrt__time_end) return YRT_ERR_UNSUPPORTED;
    if (begin(1) != 0 /*TIMERR_NOERROR*/) return YRT_ERR_REFUSED;
    yrt__timer_res_raised = true;
    /* Windows 11 drops a raised resolution while the process's windows are
     * minimized or hidden, and Sleep() and the fallback timer then wake on
     * the 15.6 ms tick again (measured: 13 ms late instead of 0.5 ms; see
     * docs/rt.md). This is the documented opt-out. */
    yrt__timer_throttle_off = yrt__throttle_off(false, YRT__PT_IGNORE_TIMER);
    return 0;
}

void yrt_timer_resolution_end(void) {
    if (!yrt__timer_res_raised || !yrt__time_end) return;
    yrt__time_end(1);
    yrt__timer_res_raised = false;
}

yrt_policy yrt_thread_elevate(const yrt_sched_deadline* rt) {
    /* Windows has no reservation to hand a thread, so the parameters are
     * accepted and ignored rather than refused: the same call site works on
     * every platform and only the returned rung differs. */
    (void)rt;
    /* Whatever the priority: a minimized process's threads otherwise run on
     * efficiency cores, where the same computation took about 1.25 times as
     * long (measured; docs/rt.md). It needs no privilege. */
    (void)yrt__throttle_off(true, YRT__PT_EXEC_SPEED);
    /* With the P-cores busy, an elevated thread opted out of EcoQoS still
     * woke on an E-core 55 to 90% of the time, and its work took 9 to 25%
     * longer there (measured; docs/rt.md). */
    yrt__prefer_pcores();
    if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
        return YRT_POLICY_TIME_CRITICAL;
    return YRT_POLICY_NORMAL;
}

#endif /* YRT__WINDOWS */

/* ======================================================================= *
 *  POSIX (Linux, macOS, and Emscripten as a subset)
 * ======================================================================= */
#if defined(YRT__POSIX)

#include <time.h>
#include <errno.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#if !defined(YRT__EMSCRIPTEN)
    /* mlockall() and setpriority(): nothing in a wasm module can lock pages or
     * renice a thread, and the Emscripten stubs would report success. */
    #include <sys/mman.h>
    #include <sys/resource.h>
#endif

#if defined(YRT__LINUX)
    #include <sys/syscall.h>
    #include <sys/prctl.h>
    #ifndef SCHED_DEADLINE
    #define SCHED_DEADLINE 6
    #endif
    #ifndef PR_SET_TIMERSLACK
    #define PR_SET_TIMERSLACK 29
    #endif
    #ifndef PR_GET_TIMERSLACK
    #define PR_GET_TIMERSLACK 30
    #endif
/* syscall()'s prototype lives behind _DEFAULT_SOURCE in <unistd.h>; declare
 * it directly so this header does not force a feature-test macro onto the
 * user's translation unit. */
extern long syscall(long, ...);
#endif

#if defined(YRT__DARWIN)
    #include <mach/mach.h>
    #include <mach/mach_time.h>
    #include <mach/thread_policy.h>
    #include <mach/thread_act.h>
#endif

static uint64_t yrt__now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int64_t yrt_ticks_to_ns(int64_t ticks) {
    return ticks;   /* clock_gettime counts in nanoseconds already */
}

#if defined(YRT__EMSCRIPTEN)
/* Emscripten's clock_getres answers 1 ns whatever the host does, and the host
 * decides: performance.now() is coarsened to 5 us to 100 us in a browser
 * page that is not cross-origin isolated, and each read is a call out to
 * JavaScript. So measure the smallest step two reads can see, over 64 steps
 * or 5 ms. Under node the step is the cost of a read, which a single
 * preemption can inflate, hence 64 samples rather than a few. 0 when the
 * clock did not move at all in 5 ms. */
static uint64_t yrt__measure_step_ns(void) {
    uint64_t start = yrt__now(), prev = start, now, step = UINT64_MAX;
    int changes = 0;
    while (changes < 64 && (now = yrt__now()) - start < 5000000ull) {
        if (now != prev) {
            if (now - prev < step) step = now - prev;
            prev = now;
            changes++;
        }
    }
    return changes > 0 ? step : 0;
}
static uint32_t yrt__step_cache = 0;   /* ns; 0 = not measured yet */
#endif

/* The tick yrt_now_ns() counts in, for yrt_correlate()'s floor on a
 * bracket's width: clock_getres() natively, the measured step under
 * Emscripten (once per process; it costs up to 5 ms). */
static uint64_t yrt__clock_step_ns(void) {
    struct timespec res;
    uint64_t r = 1;
    if (clock_getres(CLOCK_MONOTONIC, &res) == 0)
        r = (uint64_t)res.tv_sec * 1000000000ull + (uint64_t)res.tv_nsec;
#if defined(YRT__EMSCRIPTEN)
    {
        uint64_t s = yrt__rload(&yrt__step_cache);
        if (s == 0) {
            s = yrt__measure_step_ns();
            if (s == 0 || s > 0xffffffffull) s = 0xffffffffull;
            yrt__rstore(&yrt__step_cache, (uint32_t)s);
        }
        if (s > r) r = s;
    }
#endif
    return r ? r : 1;
}

/* The kernel's id for this thread, cached: on Linux gettid is a syscall every
 * time, and a traced zone asks for it twice. */
static YRT__THREAD_LOCAL uint32_t yrt__tid_cache = 0;

uint32_t yrt_thread_id(void) {
    uint32_t id = yrt__tid_cache;
    if (id) return id;
#if defined(YRT__LINUX)
    id = (uint32_t)syscall(SYS_gettid);
#elif defined(YRT__DARWIN)
    {
        uint64_t t = 0;
        pthread_threadid_np(NULL, &t);
        id = (uint32_t)t;
    }
#else
    id = (uint32_t)(uintptr_t)pthread_self();
#endif
    if (id == 0) id = 1;   /* 0 means "fill it in" to yrt_ring_push */
    yrt__tid_cache = id;
    return id;
}

uint64_t yrt_interrupt_time_100ns(void) {
    return 0;   /* a Windows clock */
}

static void yrt__ns_to_ts(uint64_t ns, struct timespec* ts) {
    ts->tv_sec  = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
}

void yrt_thread_cleanup(void) {
    /* POSIX waits need no per-thread object; the entry point exists so
     * portable shutdown code does not have to be #ifdef'd. */
}

static void yrt__coarse_wait_until(uint64_t until_ns) {
#if defined(YRT__DARWIN) || defined(YRT__EMSCRIPTEN)
    /* Darwin has no absolute clock_nanosleep, and Emscripten's sleeps are
     * relative waits on a shared-memory futex (Atomics.wait) whatever the call
     * says, so the remainder is recomputed from the clock each time round: an
     * early or interrupted wake cannot shorten the total, and a late one is
     * not paid twice. */
    for (;;) {
        uint64_t now = yrt__now();
        if (now >= until_ns) return;
        struct timespec rel;
        yrt__ns_to_ts(until_ns - now, &rel);
        if (nanosleep(&rel, NULL) == 0) return;
        if (errno != EINTR) return;
    }
#else
    /* Absolute, so a signal restart does not add the elapsed time back on.
     * clock_nanosleep returns the error rather than setting errno. */
    struct timespec ts;
    yrt__ns_to_ts(until_ns, &ts);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) { }
#endif
}

void yrt_get_clock_info(yrt_clock_info* out) {
    if (!out) return;
    struct timespec res;
    if (clock_getres(CLOCK_MONOTONIC, &res) == 0)
        out->resolution_ns = (int64_t)res.tv_sec * 1000000000 + (int64_t)res.tv_nsec;
    else
        out->resolution_ns = 0;
#if defined(YRT__EMSCRIPTEN)
    {
        /* The only platform where this measures instead of asking; see
         * yrt__measure_step_ns() for why. */
        uint64_t step = yrt__measure_step_ns();
        if ((int64_t)step > out->resolution_ns) out->resolution_ns = (int64_t)step;
    }
#endif
    /* The POSIX sleep syscalls already work at the clock's own resolution;
     * there is no second-class timer to fall back to. */
    out->hires_timer = true;
}

static uint64_t yrt__sleep_until(uint64_t deadline_ns, uint64_t spin_ns) {
    uint64_t now = yrt__now();
    if (now >= deadline_ns) return now - deadline_ns;
    YRT_ZONE(z, "yrt.sleep");
    uint64_t wake = (deadline_ns > spin_ns) ? deadline_ns - spin_ns : 0;
    if (now < wake) yrt__coarse_wait_until(wake);
    uint64_t late = yrt__spin_until(deadline_ns);
    YRT_ZONE_END(z);
    return late;
}

int yrt_thread_set_timer_slack(int64_t ns) {
#if defined(YRT__LINUX)
    unsigned long v = (ns < 1) ? 1UL : (unsigned long)ns;
    return prctl(PR_SET_TIMERSLACK, v, 0UL, 0UL, 0UL) == 0 ? 0 : YRT_ERR_REFUSED;
#else
    (void)ns;
    return YRT_ERR_UNSUPPORTED; /* Darwin has no timer slack to set. */
#endif
}

int yrt_thread_pin(int cpu) {
#if defined(YRT__LINUX)
    /* The raw syscall, with the mask built by hand, so this compiles without
     * _GNU_SOURCE and the CPU_SET macros it hides. Thread id 0 is "me": on
     * Linux affinity is a per-thread property. */
    if (cpu < 0 || cpu >= 1024) return YRT_ERR_ARG;
    unsigned long mask[1024 / (8 * sizeof(unsigned long))];
    memset(mask, 0, sizeof(mask));
    size_t bits = 8 * sizeof(unsigned long);
    mask[(size_t)cpu / bits] |= 1UL << ((size_t)cpu % bits);
    if (syscall(SYS_sched_setaffinity, 0, (unsigned)sizeof(mask), mask) != 0) return YRT_ERR_REFUSED;
    yrt__pinned_plus1 = cpu + 1;
    return 0;
#else
    (void)cpu;
    return YRT_ERR_UNSUPPORTED; /* Darwin: affinity sets are a hint, not a binding. */
#endif
}

yrt_core_type yrt_cpu_core_type(int cpu) {
    /* Not read on POSIX: Linux on Intel hybrid lists the kinds in
     * /sys/devices/cpu_core/cpus and /sys/devices/cpu_atom/cpus, but WSL2,
     * the only Linux this header is tested on, hides them, and an untested
     * parser is not a claim. See SCHEDULING. */
    (void)cpu;
    return YRT_CORE_UNKNOWN;
}

int yrt_process_lock_memory(void) {
#if defined(YRT__EMSCRIPTEN)
    /* A wasm heap is one ArrayBuffer the JavaScript engine owns; there is
     * nothing to lock and no call that could. Emscripten's mlockall stub
     * returns 0, which would be logged as a lock that never happened. */
    return YRT_ERR_UNSUPPORTED;
#else
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) return YRT_ERR_REFUSED;
    yrt__mem_locked = true;
    return 0;
#endif
}

int yrt_timer_resolution_begin(void) {
    return YRT_ERR_UNSUPPORTED; /* POSIX has no global timer tick to raise. */
}

void yrt_timer_resolution_end(void) {
}

#if defined(YRT__LINUX)
/* glibc has no wrapper or type for sched_setattr; declare both here. The
 * kernel identifies the ABI version by the struct's own size field, so this
 * must stay exactly the 48-byte VER0 layout. */
struct yrt__sched_attr {
    uint32_t size;
    uint32_t sched_policy;
    uint64_t sched_flags;
    int32_t  sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime;
    uint64_t sched_deadline;
    uint64_t sched_period;
};
YRT__STATIC_ASSERT(sizeof(struct yrt__sched_attr) == 48,
                     "sched_attr must match the kernel's VER0 layout");

static int yrt__setattr(struct yrt__sched_attr* a) {
#if defined(SYS_sched_setattr)
    return (int)syscall(SYS_sched_setattr, 0, a, 0u);
#elif defined(__x86_64__)
    return (int)syscall(314 /* __NR_sched_setattr */, 0, a, 0u);
#elif defined(__aarch64__)
    return (int)syscall(274 /* __NR_sched_setattr */, 0, a, 0u);
#else
    (void)a; errno = ENOSYS; return -1;
#endif
}
#endif /* YRT__LINUX */

#if defined(YRT__DARWIN)
/* Ask the Mach scheduler for the same shape of reservation SCHED_DEADLINE
 * describes: `computation` of CPU inside `constraint` of every `period`.
 * Mach counts in absolute time units, not nanoseconds, and the ratio is not
 * 1 on every Apple machine. */
static bool yrt__darwin_time_constraint(const yrt_sched_deadline* rt) {
    mach_timebase_info_data_t tb;
    if (mach_timebase_info(&tb) != KERN_SUCCESS || tb.numer == 0) return false;
    /* ns * denom / numer, split so a long period cannot overflow. */
    #define YRT__NS2ABS(ns) \
        (uint32_t)(((ns) / 1000000ull) * ((uint64_t)tb.denom * 1000000ull) / tb.numer \
                 + ((ns) % 1000000ull) * (uint64_t)tb.denom / tb.numer)
    thread_time_constraint_policy_data_t pol;
    pol.period      = YRT__NS2ABS(rt->period_ns);
    pol.computation = YRT__NS2ABS(rt->runtime_ns);
    pol.constraint  = YRT__NS2ABS(rt->deadline_ns);
    /* Non-preemptible: the point of the rung is that nothing gets between the
     * wake and the deadline. The kernel caps how much it will honor. */
    pol.preemptible = FALSE;
    #undef YRT__NS2ABS
    /* pthread_mach_thread_np borrows the port; mach_thread_self would hand
     * back a reference this function would then have to deallocate. */
    return thread_policy_set(pthread_mach_thread_np(pthread_self()),
                             THREAD_TIME_CONSTRAINT_POLICY,
                             (thread_policy_t)&pol,
                             THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
}
#endif /* YRT__DARWIN */

yrt_policy yrt_thread_elevate(const yrt_sched_deadline* rt) {
#if defined(YRT__EMSCRIPTEN)
    /* The ladder collapses: a wasm thread is a Web Worker, scheduled by the
     * browser or node like any other, with no priority a page can ask for.
     * Emscripten's pthread_setschedparam stub returns success, so the FIFO
     * attempt below would report a rung that does not exist. */
    (void)rt;
    return YRT_POLICY_NORMAL;
#else
    yrt_sched_deadline r;
    memset(&r, 0, sizeof(r));
    if (rt) r = *rt;
    bool have_rt = yrt_sched_normalize(&r) == 0;

#if defined(YRT__LINUX)
    if (have_rt) {
        struct yrt__sched_attr a;
        memset(&a, 0, sizeof(a));
        a.size           = (uint32_t)sizeof(a);
        a.sched_policy   = SCHED_DEADLINE;
        a.sched_runtime  = r.runtime_ns;
        a.sched_deadline = r.deadline_ns;
        a.sched_period   = r.period_ns;
        if (yrt__setattr(&a) == 0) return YRT_POLICY_DEADLINE;
    }
#elif defined(YRT__DARWIN)
    if (have_rt && yrt__darwin_time_constraint(&r))
        return YRT_POLICY_TIME_CONSTRAINT;
#else
    (void)have_rt;
#endif

    /* FIFO 80: above the kernel's threaded IRQ handlers (50), so a wake is
     * not queued behind a busy NIC, but below migration and the watchdog
     * (99), which must still be able to preempt a runaway spin. */
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = 80;
    int top = sched_get_priority_max(SCHED_FIFO);
    if (top > 0 && sp.sched_priority > top) sp.sched_priority = top;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0)
        return YRT_POLICY_FIFO;

    /* No privilege for any of that. Dropping the timer slack costs nothing
     * and removes 50 us of deliberate lateness from every wait this thread
     * takes, which is the single biggest win available to an unprivileged
     * thread on Linux. */
    (void)yrt_thread_set_timer_slack(1);
    return YRT_POLICY_NORMAL;
#endif /* YRT__EMSCRIPTEN */
}

#endif /* YRT__POSIX */

/* ======================================================================= *
 *  PLATFORM-INDEPENDENT
 * ======================================================================= */

const char* yrt_version(void) { return YRT_VERSION_STRING; }

/* --- virtual clock ------------------------------------------------------- */

/* One driver thread writes the time; any thread reads it. A 64-bit load
 * must not tear on a 32-bit target, hence the helpers. */
#if defined(_MSC_VER) && defined(_M_ARM64)
static int64_t yrt__ld64(const int64_t* p) { return (int64_t)__ldar64((unsigned __int64 volatile*)(uintptr_t)p); }
static void yrt__st64(int64_t* p, int64_t v) { __stlr64((unsigned __int64 volatile*)p, (unsigned __int64)v); }
#elif defined(_MSC_VER) && defined(_M_IX86)
static int64_t yrt__ld64(const int64_t* p) { return _InterlockedCompareExchange64((volatile __int64*)(uintptr_t)p, 0, 0); }
static void yrt__st64(int64_t* p, int64_t v) {
    int64_t o = *(volatile int64_t*)p;
    while (_InterlockedCompareExchange64((volatile __int64*)p, v, o) != o) o = *(volatile int64_t*)p;
}
#elif defined(_MSC_VER)
static int64_t yrt__ld64(const int64_t* p) { int64_t v = *(const volatile int64_t*)p; _ReadWriteBarrier(); return v; }
static void yrt__st64(int64_t* p, int64_t v) { _ReadWriteBarrier(); *(volatile int64_t*)p = v; }
#else
static int64_t yrt__ld64(const int64_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void yrt__st64(int64_t* p, int64_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
#endif

typedef struct yrt__vtimer {
    yrt_virtual_fn fn;
    void*          ctx;
    int64_t        at;
} yrt__vtimer;

/* The flag is the one word every yrt_now_ns() reads; the rest is cold. */
static uint32_t yrt__v_on = 0;
static uint32_t yrt__v_driver = 0;
static int64_t  yrt__v_now = 0;
static int      yrt__v_busy = 0;   /* inside a timer: a wait there must not recurse */
static yrt__vtimer yrt__v_t[YRT_VIRTUAL_TIMERS];

/* The clock every public time reads: virtual when on. yrt__now() stays
 * the real counter, for what measures real time (zones, correlation, the
 * OS waits themselves). */
static int64_t yrt__clock(void) {
    if (yrt__rload(&yrt__v_on)) return yrt__ld64(&yrt__v_now);
    return (int64_t)yrt__now();
}

static int yrt__v_is_driver(void) { return yrt_thread_id() == yrt__rload(&yrt__v_driver); }

/* The driver's wait: run every timer due by `to` in time order (ties in
 * slot order, so a run repeats exactly), then stand at `to`. */
static void yrt__v_advance(int64_t to) {
    for (; !yrt__v_busy;) {
        int i, k = -1;
        int64_t at = 0;
        for (i = 0; i < YRT_VIRTUAL_TIMERS; i++)
            if (yrt__v_t[i].fn && (k < 0 || yrt__v_t[i].at < at)) { k = i; at = yrt__v_t[i].at; }
        if (k < 0 || at > to) break;
        if (at > yrt__ld64(&yrt__v_now)) yrt__st64(&yrt__v_now, at);
        {
            yrt_virtual_fn fn = yrt__v_t[k].fn;
            int64_t now = yrt__ld64(&yrt__v_now), next;
            yrt__v_busy = 1;
            next = fn(yrt__v_t[k].ctx, now);
            yrt__v_busy = 0;
            /* The callback may have canceled or replaced its own slot. */
            if (yrt__v_t[k].fn == fn && yrt__v_t[k].at == at) {
                if (next > now) yrt__v_t[k].at = next;
                else yrt__v_t[k].fn = NULL;
            }
        }
    }
    if (to > yrt__ld64(&yrt__v_now)) yrt__st64(&yrt__v_now, to);
}

/* A wait on the virtual clock. The driver moves the clock; any other
 * thread sleeps in real 100 us steps until the driver has moved it far
 * enough, or until the clock is real again. */
static int64_t yrt__v_wait(int64_t deadline) {
    int64_t now = yrt__ld64(&yrt__v_now);
    if (now >= deadline) return now - deadline;
    if (yrt__v_is_driver()) { yrt__v_advance(deadline); return 0; }
    while (yrt__rload(&yrt__v_on)) {
        now = yrt__ld64(&yrt__v_now);
        if (now >= deadline) return now - deadline;
        (void)yrt__sleep_until(yrt__now() + 100000u, 0);
    }
    return 0;
}

int yrt_virtual_begin(int64_t t0_ns) {
    if (yrt__rload(&yrt__v_on)) return YRT_ERR_STATE;
    memset(yrt__v_t, 0, sizeof yrt__v_t);
    yrt__st64(&yrt__v_now, t0_ns > 0 ? t0_ns : YRT_VIRTUAL_T0_NS);
    yrt__rstore(&yrt__v_driver, yrt_thread_id());
    yrt__rstore(&yrt__v_on, 1u);
    return 0;
}

void yrt_virtual_end(void) {
    yrt__rstore(&yrt__v_on, 0u);
    memset(yrt__v_t, 0, sizeof yrt__v_t);
}

bool yrt_is_virtual(void) { return yrt__rload(&yrt__v_on) != 0; }

int64_t yrt_real_ns(void) { return (int64_t)yrt__now(); }

int yrt_virtual_timer(yrt_virtual_fn fn, void* ctx, int64_t at_ns) {
    int i;
    if (!fn) return YRT_ERR_ARG;
    if (!yrt__rload(&yrt__v_on) || !yrt__v_is_driver()) return YRT_ERR_STATE;
    for (i = 0; i < YRT_VIRTUAL_TIMERS; i++)
        if (!yrt__v_t[i].fn) {
            yrt__v_t[i].ctx = ctx;
            yrt__v_t[i].at = at_ns;
            yrt__v_t[i].fn = fn;
            return i;
        }
    return YRT_ERR_FULL;
}

void yrt_virtual_cancel(int id) {
    if (id < 0 || id >= YRT_VIRTUAL_TIMERS || !yrt__v_is_driver()) return;
    yrt__v_t[id].fn = NULL;
}

int64_t yrt_now_ns(void) {
    if (yrt__rload(&yrt__v_on)) return yrt__ld64(&yrt__v_now);
    return (int64_t)yrt__now();
}

static uint64_t yrt__spin_until(uint64_t deadline_ns) {
    uint64_t now = yrt__now();
    while (now < deadline_ns) {
        YRT__PAUSE();
        now = yrt__now();
    }
    return now - deadline_ns;
}

/* A deadline before the clock's zero is already past. */
static uint64_t yrt__unsigned(int64_t t) { return t > 0 ? (uint64_t)t : 0u; }

int64_t yrt_sleep_until(int64_t deadline_ns, int64_t spin_ns) {
    if (yrt__rload(&yrt__v_on)) return yrt__v_wait(deadline_ns);
    return (int64_t)yrt__sleep_until(yrt__unsigned(deadline_ns), yrt__unsigned(spin_ns));
}

int64_t yrt_spin_until(int64_t deadline_ns) {
    if (yrt__rload(&yrt__v_on)) return yrt__v_wait(deadline_ns);
    return (int64_t)yrt__spin_until(yrt__unsigned(deadline_ns));
}

int64_t yrt_sleep_ns(int64_t ns) {
    if (yrt__rload(&yrt__v_on)) return yrt__v_wait(yrt__ld64(&yrt__v_now) + (ns > 0 ? ns : 0));
    /* The deadline is taken here, so the call's own overhead comes out of the
     * wait instead of being added to it. */
    return (int64_t)yrt__sleep_until(yrt__now() + yrt__unsigned(ns),
                                     (uint64_t)YRT_DEFAULT_SPIN_NS);
}

int yrt_sched_normalize(yrt_sched_deadline* rt) {
    if (!rt) return YRT_ERR_ARG;
    if (rt->runtime_ns == 0 && rt->deadline_ns == 0 && rt->period_ns == 0) {
        rt->runtime_ns  = YRT_DEFAULT_RT_RUNTIME_NS;
        rt->deadline_ns = YRT_DEFAULT_RT_DEADLINE_NS;
        rt->period_ns   = YRT_DEFAULT_RT_PERIOD_NS;
        return 0;
    }
    int64_t period = rt->period_ns ? rt->period_ns : rt->deadline_ns;
    if (rt->runtime_ns <= 0 || rt->deadline_ns <= 0 ||
        rt->runtime_ns > rt->deadline_ns || rt->deadline_ns > period)
        return YRT_ERR_ARG;
    rt->period_ns = period;
    return 0;
}

const char* yrt_core_type_name(yrt_core_type t) {
    switch (t) {
        case YRT_CORE_UNIFORM:     return "uniform";
        case YRT_CORE_PERFORMANCE: return "P";
        case YRT_CORE_EFFICIENCY:  return "E";
        default:                     return "?";
    }
}

const char* yrt_policy_name(yrt_policy p) {
    switch (p) {
        case YRT_POLICY_NONE:            return "NONE";
        case YRT_POLICY_DEADLINE:        return "DEADLINE";
        case YRT_POLICY_TIME_CONSTRAINT: return "TIME_CONSTRAINT";
        case YRT_POLICY_FIFO:            return "FIFO";
        case YRT_POLICY_TIME_CRITICAL:   return "TIME_CRITICAL";
        case YRT_POLICY_NORMAL:          return "NORMAL";
        case YRT_POLICY_BELOW_NORMAL:    return "BELOW_NORMAL";
        default:                           return "?";
    }
}

void yrt_report_get(yrt_report* out, yrt_policy policy) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->policy = policy;
    yrt_clock_info ci;
    memset(&ci, 0, sizeof(ci));
    yrt_get_clock_info(&ci);
    out->clock_res_ns = ci.resolution_ns;
    out->hires_timer  = ci.hires_timer;
    out->memory_locked = yrt__mem_locked;
    out->timer_resolution_raised = yrt__timer_res_raised;
    out->pinned_cpu  = yrt__pinned_plus1 - 1;
    out->pinned_core = out->pinned_cpu >= 0 ? yrt_cpu_core_type(out->pinned_cpu)
                                            : YRT_CORE_UNKNOWN;
    out->pcores_preferred = yrt__pcores_set != 0;
#if defined(YRT__WINDOWS)
    {
        int eco = yrt__thread_ecoqos_off();
        out->throttle_known     = eco >= 0;
        out->ecoqos_off         = eco > 0;
        out->timer_throttle_off = yrt__timer_throttle_off;
    }
#endif
#if defined(YRT__LINUX)
    /* PR_GET_TIMERSLACK returns the value itself, so a negative result is the
     * error and 0 is a legitimate (if unusual) answer. */
    int slack = prctl(PR_GET_TIMERSLACK, 0UL, 0UL, 0UL, 0UL);
    if (slack >= 0) {
        out->timer_slack_ns    = (int64_t)slack;
        out->timer_slack_known = true;
    }
#endif
}

int yrt_describe(const yrt_report* r, char* buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    if (!r) { buf[0] = '\0'; return 0; }
    char slack[32];
    if (r->timer_slack_known)
        snprintf(slack, sizeof(slack), "%lldns",
                 (long long)r->timer_slack_ns);
    else
        snprintf(slack, sizeof(slack), "n/a");
#if defined(YRT__EMSCRIPTEN)
    /* Said on the line itself, so a log read later cannot mistake NORMAL for
     * "a ladder was tried and refused": on wasm there is no ladder. */
    const char* platform = " platform=wasm ladder=none";
#else
    const char* platform = "";
#endif
    /* "off" means opted out of the throttling, which is what a rig wants;
     * "on" means Windows may still apply it. */
    const char* tt = !r->throttle_known ? "n/a" : r->timer_throttle_off ? "off" : "on";
    const char* eq = !r->throttle_known ? "n/a" : r->ecoqos_off ? "off" : "on";
    /* The pin with the kind of core it is on, because on a hybrid CPU the
     * number alone does not say. Bounded so a report filled by hand cannot
     * push the line past the 192 bytes the manual promises. */
    char pin[24];
    if (r->pinned_cpu >= 0 && r->pinned_cpu < 100000)
        snprintf(pin, sizeof(pin), "%d/%s", r->pinned_cpu, yrt_core_type_name(r->pinned_core));
    else
        snprintf(pin, sizeof(pin), "none");
    int n = snprintf(buf, cap,
                     "ysp_rt: policy=%s clock_res=%lldns slack=%s "
                     "hires_timer=%s mlock=%s timer_res=%s "
                     "timer_throttle=%s ecoqos=%s pin=%s cores=%s%s",
                     yrt_policy_name(r->policy),
                     (long long)r->clock_res_ns,
                     slack,
                     r->hires_timer ? "yes" : "no",
                     r->memory_locked ? "yes" : "no",
                     r->timer_resolution_raised ? "yes" : "no",
                     tt, eq, pin, r->pcores_preferred ? "P" : "any", platform);
    /* snprintf reports what it WOULD have written; the caller wants what is
     * in the buffer. */
    if (n < 0) { buf[0] = '\0'; return 0; }
    return (n >= (int)cap) ? (int)cap - 1 : n;
}

const char* yrt_strerror(int code) {
    switch (code) {
        case YRT_ERR_ARG:     return "bad argument";
        case YRT_ERR_STOPPED: return "worker or pump not running";
        case YRT_ERR_FULL:    return "ring full, not taken";
        case YRT_ERR_TIMEOUT: return "timed out";
        case YRT_ERR_UNSUPPORTED: return "not on this platform";
        case YRT_ERR_REFUSED: return "refused by the OS";
        case YRT_ERR_STATE:   return "the clock is real, or not the driver thread";
        default:                return code >= 0 ? "ok" : "unknown error";
    }
}

/* ======================================================================= *
 *  EVENT RING
 *
 *  Many producers, one drainer, fixed 64-byte records in caller memory.
 *  Two counters on the producers' line: `used` admits a push only while a
 *  slot is free, and `head` hands out tickets. With `head` alone, a producer
 *  that found the ring full would already hold a ticket it could not fill,
 *  and the drainer could not tell that hole from a producer still writing.
 *  `used` lets a refused push leave without a ticket. Each slot's first word
 *  is ticket + 1 once its record is complete.
 * ======================================================================= */

bool yrt_ring_open(yrt_ring* r, const yrt_ring_desc* desc) {
    uintptr_t base, aligned;
    size_t pad, avail;
    uint32_t cap = 0;
    if (!r) return false;
    memset(r, 0, sizeof(*r));
    if (!desc || !desc->memory) {
        snprintf(r->error, sizeof(r->error), "ring: desc.memory is required");
        return false;
    }
    base = (uintptr_t)desc->memory;
    aligned = (base + 63u) & ~(uintptr_t)63u;
    pad = (size_t)(aligned - base);
    avail = desc->bytes > pad ? (desc->bytes - pad) / 64u : 0u;
    if (avail > ((size_t)1 << 24)) avail = (size_t)1 << 24;
    if (avail >= 2u) {
        cap = 2u;
        while ((size_t)cap * 2u <= avail) cap *= 2u;
    }
    if (cap < 2u) {
        snprintf(r->error, sizeof(r->error),
                 "ring: desc.bytes (%lu) holds fewer than 2 records; size it "
                 "with YRT_RING_BYTES(n)", (unsigned long)desc->bytes);
        return false;
    }
    /* Zeroed, so no stale word can pass for a published ticket, and touched,
     * so the first push from a callback does not take a page fault. */
    memset((void*)aligned, 0, (size_t)cap * 64u);
    r->slots = (unsigned char*)aligned;
    r->mask  = cap - 1u;
    return true;
}

void yrt_ring_close(yrt_ring* r) {
    if (!r) return;
    r->slots = NULL;
    r->mask  = 0u;
}

const char* yrt_ring_error(const yrt_ring* r) {
    return r ? r->error : "null ring handle";
}

uint32_t yrt_ring_capacity(const yrt_ring* r) {
    return (r && r->slots) ? r->mask + 1u : 0u;
}

uint32_t yrt_ring_dropped(const yrt_ring* r) {
    return r ? yrt__rload(&r->dropped) : 0u;
}

int yrt_ring_push(yrt_ring* r, const yrt_event* ev) {
    unsigned char* s;
    int64_t t_ns;
    uint32_t tid, n, t;
    if (!r || !ev || !r->slots) return YRT_ERR_ARG;
    /* Before the claim: between its ticket and its publish a push holds up
     * the drainer, so nothing optional happens in that window. */
    t_ns = ev->t_ns ? ev->t_ns : yrt__clock();
    tid  = ev->tid ? ev->tid : yrt_thread_id();
    n = yrt__rfadd(&r->used, 1u);
    if (n > r->mask) {
        /* Full. The increment is undone rather than checked first, so the
         * push stays one fetch-add with no retry; a push racing this window
         * may be refused one record early, which the manual states. */
        (void)yrt__rfadd(&r->used, 0xffffffffu);
        (void)yrt__rfadd(&r->dropped, 1u);
        return YRT_ERR_FULL;
    }
    t = yrt__rfadd(&r->head, 1u);
    s = r->slots + (size_t)(t & r->mask) * 64u;
    memcpy(s + 4, &tid, sizeof(tid));
    memcpy(s + 8, &t_ns, sizeof(t_ns));
    memcpy(s + 16, (const unsigned char*)ev + 16, 48);
    yrt__rstore((uint32_t*)(void*)s, t + 1u);
    return 0;
}

int yrt_ring_drain(yrt_ring* r, yrt_event* out, int cap) {
    uint32_t tail, got = 0, d;
    int k = 0;
    if (!r || !out || !r->slots) return YRT_ERR_ARG;
    if (cap <= 0) return 0;
    d = yrt__rload(&r->dropped);
    if (d != r->dropped_seen) {
        /* In band, first, so a writer that only copies records out cannot
         * leave the loss out of its file. */
        uint32_t delta = d - r->dropped_seen;
        r->dropped_seen = d;
        r->lost += delta;
        memset(&out[0], 0, sizeof(out[0]));
        out[0].t_ns   = yrt__clock();
        out[0].tid    = yrt_thread_id();
        out[0].source = (uint16_t)YRT_SRC_RT;
        out[0].kind   = (uint16_t)YRT_KIND_LOSS;
        out[0].u.u64[0] = delta;
        out[0].u.u64[1] = r->lost;
        k = 1;
    }
    tail = r->tail;
    while (k < cap) {
        const unsigned char* s = r->slots + (size_t)((tail + got) & r->mask) * 64u;
        if (yrt__rload((const uint32_t*)(const void*)s) != tail + got + 1u) break;
        memcpy(&out[k], s, sizeof(yrt_event));
        k++;
        got++;
    }
    if (got) {
        r->tail = tail + got;
        /* After the copies, with release: this is what lets a producer reuse
         * the slots, once per batch rather than once per record. */
        (void)yrt__rfadd(&r->used, 0u - got);
    }
    return k;
}

/* ======================================================================= *
 *  TRACE (YRT_TRACE_RING)
 * ======================================================================= */

static void* yrt__trace_ptr = NULL;

void yrt_trace_set_ring(yrt_ring* r) { yrt__pstore(&yrt__trace_ptr, r); }

yrt_ring* yrt_trace_ring(void) { return (yrt_ring*)yrt__pload(&yrt__trace_ptr); }

yrt_zone yrt__zone_begin(const yrt_srcloc* loc) {
    yrt_zone z;
    z.loc = loc;
    z.value = 0;
    /* No ring, no clock read: an untraced build that happens to be compiled
     * in ring mode pays one pointer load per zone. */
    z.t0_ns = yrt__pload(&yrt__trace_ptr) ? (int64_t)yrt__now() : 0;
    return z;
}

static void yrt__trace_event(yrt_event* e, uint16_t kind) {
    memset(e, 0, sizeof(*e));
    e->source = (uint16_t)YRT_SRC_RT;
    e->kind   = kind;
}

void yrt__zone_end(const yrt_zone* z) {
    yrt_ring* r;
    yrt_event e;
    if (!z || !z->t0_ns) return;
    r = (yrt_ring*)yrt__pload(&yrt__trace_ptr);
    if (!r) return;
    yrt__trace_event(&e, (uint16_t)YRT_KIND_ZONE);
    e.t_ns = z->t0_ns;
    e.u.zone.loc    = z->loc;
    e.u.zone.dur_ns = (int64_t)yrt__now() - z->t0_ns;
    e.u.zone.value  = z->value;
    (void)yrt_ring_push(r, &e);
}

void yrt__trace_frame(const char* name) {
    yrt_event e;
    yrt_ring* r = (yrt_ring*)yrt__pload(&yrt__trace_ptr);
    if (!r) return;
    yrt__trace_event(&e, (uint16_t)YRT_KIND_FRAME);
    e.u.plot.name = name;
    (void)yrt_ring_push(r, &e);
}

void yrt__trace_plot(const char* name, double value) {
    yrt_event e;
    yrt_ring* r = (yrt_ring*)yrt__pload(&yrt__trace_ptr);
    if (!r) return;
    yrt__trace_event(&e, (uint16_t)YRT_KIND_PLOT);
    e.u.plot.name  = name;
    e.u.plot.value = value;
    (void)yrt_ring_push(r, &e);
}

static void yrt__trace_text(uint16_t kind, const char* text, size_t len) {
    yrt_event e;
    yrt_ring* r = (yrt_ring*)yrt__pload(&yrt__trace_ptr);
    if (!r) return;
    yrt__trace_event(&e, kind);
    if (text) {
        if (len > sizeof(e.u.text) - 1) len = sizeof(e.u.text) - 1;
        memcpy(e.u.text, text, len);
    }
    (void)yrt_ring_push(r, &e);
}

void yrt__trace_message(const char* text, size_t len) {
    yrt__trace_text((uint16_t)YRT_KIND_MESSAGE, text, len);
}

void yrt__trace_thread_name(const char* name) {
    yrt__trace_text((uint16_t)YRT_KIND_THREAD_NAME, name, name ? strlen(name) : 0);
}

void yrt__trace_messagef(const char* fmt, ...) {
    yrt_event e;
    va_list ap;
    yrt_ring* r = (yrt_ring*)yrt__pload(&yrt__trace_ptr);
    if (!r || !fmt) return;
    yrt__trace_event(&e, (uint16_t)YRT_KIND_MESSAGE);
    va_start(ap, fmt);
    (void)vsnprintf(e.u.text, sizeof(e.u.text), fmt, ap);
    va_end(ap);
    (void)yrt_ring_push(r, &e);
}

/* ======================================================================= *
 *  CLOCK CORRELATION
 * ======================================================================= */

static uint64_t yrt__corr_read(const yrt_corr_desc* d) {
    return d->read ? d->read() : d->read_ctx(d->ctx);
}

int yrt_correlate(const yrt_corr_desc* d, yrt_corr* out) {
    uint64_t best_w = UINT64_MAX, best_rt = 0, best_o = 0, a, b, v, w, step;
    int tries, taken = 0;
    if (!d || !out) return YRT_ERR_ARG;
    if ((d->read == NULL) == (d->read_ctx == NULL)) return YRT_ERR_ARG;
    tries = d->tries > 0 ? d->tries : (d->edge ? 4 : 16);
    if (!d->edge) {
        for (taken = 0; taken < tries; taken++) {
            a = yrt__now();
            v = yrt__corr_read(d);
            b = yrt__now();
            w = b - a;
            if (w < best_w) { best_w = w; best_rt = a + w / 2u; best_o = v; }
        }
    } else {
        /* The value changed somewhere between the start of the last read
         * that still saw the old value and the end of the first that saw
         * the new one. */
        uint64_t timeout = d->timeout_ns > 0 ? (uint64_t)d->timeout_ns : 100000000ull;
        uint64_t a_old = yrt__now();
        uint64_t v_old = yrt__corr_read(d);
        uint64_t end = a_old + timeout;
        while (taken < tries) {
            a = yrt__now();
            v = yrt__corr_read(d);
            b = yrt__now();
            if (v != v_old) {
                w = b - a_old;
                if (w < best_w) { best_w = w; best_rt = a_old + w / 2u; best_o = v; }
                taken++;
            }
            a_old = a;
            v_old = v;
            if (b >= end) break;
        }
        if (taken == 0) return YRT_ERR_TIMEOUT;
    }
    /* A 100 ns counter can read the same tick on both sides, and a width of
     * 0 would claim an exact match and break a 1 / width^2 weight. */
    step = yrt__clock_step_ns();
    out->rt_ns    = (int64_t)best_rt;
    out->other    = best_o;
    out->width_ns = (int64_t)(best_w < step ? step : best_w);
    out->tries    = taken;
    return taken;
}

/* ======================================================================= *
 *  DEVICE CLOCK FIT
 *
 *  Points are kept as x = device ticks since the epoch's first pair and y =
 *  host ns, one per bucket of host time, in a ring of YRT_FIT_POINTS. A
 *  refit works on doubles relative to the oldest point, with the nominal
 *  slope taken out, so the numbers stay small: 205 s of a us clock is 2e8.
 *  The map is anchored at the newest point, so a map read for a recent
 *  event multiplies a small tick difference.
 * ======================================================================= */

/* Before the slope is fitted, the offset comes from the last second only:
 * the drift the nominal slope ignores then costs at most a second's worth
 * (100 us at 100 ppm). ysp/audio.h measured the same rule. */
#define YRT__FIT_OFFSET_NS 1000000000LL
/* While the window is this small a refit is cheap, and every pair can
 * still move the offset by a lot, so each pair refits. */
#define YRT__FIT_EAGER     16

static int64_t yrt__fit_round(double v) {
    return v >= 0.0 ? (int64_t)(v + 0.5) : -(int64_t)(-v + 0.5);
}

/* Floor division for a positive divisor: a pair stamped before the epoch's
 * first pair falls in a negative bucket, not in bucket 0. */
static int64_t yrt__floordiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if (a % b != 0 && a < 0) q--;
    return q;
}

/* The unwrapped value of a wrapped counter nearest to ref. Differences are
 * taken modulo 2^64, so a value just before an unwrapped 0 becomes a
 * negative x, which the map handles as any other x. */
static uint64_t yrt__fit_unwrap_to(const yrt_fit* f, uint64_t ticks, uint64_t ref) {
    uint64_t m = f->modulus, u;
    int64_t  d, half;
    if (m == 0) return ticks;
    ticks &= m - 1u;
    u = (ref & ~(m - 1u)) | ticks;
    d = (int64_t)(u - ref);
    half = (int64_t)(m / 2u);
    if (d > half) u -= m;
    else if (d < -half) u += m;
    return u;
}

static int64_t yrt__fit_at(const yrt_fit* f, int64_t x) {
    return f->fy + yrt__fit_round((double)(x - f->fx) * f->k);
}

/* The unwrapped ticks the map expects at host time y: the reference that
 * picks a wrapped counter's period. */
static uint64_t yrt__fit_expect(const yrt_fit* f, int64_t y) {
    int64_t x = f->fx + yrt__fit_round((double)(y - f->fy) / f->k);
    return f->base_ticks + (uint64_t)x;
}

/* The kth smallest of v[0..n) (Hoare's selection), reordering v. */
static int64_t yrt__fit_select(int64_t* v, int32_t n, int32_t kth) {
    int32_t lo = 0, hi = n - 1;
    while (lo < hi) {
        int64_t pivot = v[lo + (hi - lo) / 2], t;
        int32_t i = lo, j = hi;
        while (i <= j) {
            while (v[i] < pivot) i++;
            while (v[j] > pivot) j--;
            if (i <= j) { t = v[i]; v[i] = v[j]; v[j] = t; i++; j--; }
        }
        if (kth <= j) hi = j;
        else if (kth >= i) lo = i;
        else break;
    }
    return v[kth];
}

static void yrt__fit_record(yrt_fit* f, uint16_t kind, int32_t i) {
    yrt_event ev;
    if (!f->d.ring) return;
    memset(&ev, 0, sizeof ev);
    ev.source = (uint16_t)YRT_SRC_RT;
    ev.kind   = kind;
    ev.aux    = f->d.clock_id;
    if (kind == YRT_KIND_CLOCK) {
        ev.t_ns     = (uint64_t)f->py[i];
        ev.u.u64[0] = f->base_ticks + (uint64_t)f->px[i];
        ev.u.u64[1] = (uint64_t)f->pw[i];
        ev.u.u64[2] = f->epoch;
    } else {
        ev.t_ns     = (uint64_t)f->py[(f->head + f->n - 1) % YRT_FIT_POINTS];
        ev.u.i64[0] = f->fy;
        ev.u.u64[1] = f->base_ticks + (uint64_t)f->fx;
        ev.u.f64[2] = f->k;
        ev.u.i64[3] = f->spread_ns;
        ev.u.u32[8] = f->epoch;
        ev.u.u32[9] = (uint32_t)f->n;
    }
    (void)yrt_ring_push(f->d.ring, &ev);
}

/* LATE and BRACKET: the line under every point with the smallest summed gap,
 * which is the edge of the lower convex hull that spans the mean x (Moon,
 * Skelly and Towsley, INFOCOM 1999), the estimator ysp/audio.h measured for
 * callback times. UNBIASED: least squares. Before the points span
 * slope_span_ns the slope stays nominal, because a slope from a short span
 * was worse than the drift it corrects (ysp/audio.h). */
static void yrt__fit_refit(yrt_fit* f) {
    const int32_t N = YRT_FIT_POINTS;
    int32_t n = f->n, i, j = 0, h = 0, lo, hi = -1, kth;
    int32_t i0 = f->head, il = (f->head + n - 1) % N;
    int64_t x0 = f->px[i0], y0 = f->py[i0], from;
    double  kn = f->d.ns_per_tick, a, b = 0.0, sx = 0.0, xm, xl, r;
    bool    envelope = f->d.mode != YRT_FIT_UNBIASED;
    int32_t* hull = f->hull;
#define YRT__X(q) ((double)(f->px[((q) + i0) % N] - x0))
#define YRT__Y(q) ((double)(f->py[((q) + i0) % N] - y0) - YRT__X(q) * kn)
    for (i = 0; i < n; i++) sx += YRT__X(i);
    xm = sx / n;
    /* the offset alone, from the last second */
    from = f->py[il] - YRT__FIT_OFFSET_NS;
    lo = n - 1;
    a = YRT__Y(n - 1);
    {
        double sum = 0.0;
        int32_t m = 0;
        for (i = 0; i < n; i++) {
            if (f->py[(i + i0) % N] < from) continue;
            if (YRT__Y(i) < a) { a = YRT__Y(i); lo = i; }
            sum += YRT__Y(i);
            m++;
        }
        if (!envelope) a = sum / m;
    }
    f->slope = 0;
    if (n >= 2 && f->py[il] - y0 >= f->d.slope_span_ns) {
        if (!envelope) {
            double sxx = 0.0, sxy = 0.0, sy = 0.0, dx;
            for (i = 0; i < n; i++) sy += YRT__Y(i);
            for (i = 0; i < n; i++) {
                dx = YRT__X(i) - xm;
                sxx += dx * dx;
                sxy += dx * (YRT__Y(i) - sy / n);
            }
            if (sxx > 0.0) {
                b = sxy / sxx;
                a = sy / n - b * xm;
                f->slope = 1;
            }
        } else {
            for (i = 0; i < n; i++) {
                while (h >= 2) {
                    double x1 = YRT__X(hull[h - 2]), y1 = YRT__Y(hull[h - 2]);
                    double x2 = YRT__X(hull[h - 1]), y2 = YRT__Y(hull[h - 1]);
                    double x3 = YRT__X(i), y3 = YRT__Y(i);
                    if ((x2 - x1) * (y3 - y1) - (y2 - y1) * (x3 - x1) <= 0.0) h--; else break;
                }
                hull[h++] = i;
            }
            while (j + 1 < h - 1 && YRT__X(hull[j + 1]) <= xm) j++;
            if (h >= 2 && YRT__X(hull[j + 1]) > YRT__X(hull[j])) {
                double x1 = YRT__X(hull[j]), y1 = YRT__Y(hull[j]);
                double x2 = YRT__X(hull[j + 1]), y2 = YRT__Y(hull[j + 1]);
                b = (y2 - y1) / (x2 - x1);
                a = y1 - b * x1;
                lo = hull[j];
                hi = hull[j + 1];
                f->slope = 1;
            }
        }
    }
    /* the spread: the p99 distance above the line (envelope) or either side */
    for (i = 0; i < n; i++) {
        r = YRT__Y(i) - a - b * YRT__X(i);
        if (!envelope && r < 0.0) r = -r;
        f->scratch[i] = yrt__fit_round(r);
    }
    kth = (99 * n + 99) / 100 - 1;
    f->spread_ns = yrt__fit_select(f->scratch, n, kth);
    f->width_ns = 0;
    if (f->d.mode == YRT_FIT_BRACKET) {
        f->width_ns = f->pw[(lo + i0) % N];
        if (hi >= 0 && f->pw[(hi + i0) % N] > f->width_ns) f->width_ns = f->pw[(hi + i0) % N];
    }
    xl = YRT__X(n - 1);
    f->fx = x0 + (int64_t)xl;
    f->fy = y0 + yrt__fit_round(a + b * xl + xl * kn);
    f->k = kn + b;
    f->have = 1;
#undef YRT__X
#undef YRT__Y
}

/* A pair the map accepts: a new point, a better point for the newest
 * bucket, or nothing. `below`: the pair lies under the current line, which
 * the envelope must then go through. */
static int yrt__fit_point(yrt_fit* f, uint64_t u, int64_t y, int64_t w, bool below) {
    const int32_t N = YRT_FIT_POINTS;
    int64_t x = (int64_t)(u - f->base_ticks);
    int64_t bk = yrt__floordiv(y - f->base_host, f->d.bucket_ns);
    int32_t i;
    bool closed = false, changed = false, first = !f->have;
    int rc = 0;
    if (f->n > 0 && bk < f->bucket) { f->rejected++; return YRT_FIT_REJECTED; }
    f->last_ticks = u;
    if (f->n > 0 && bk == f->bucket) {
        i = (f->head + f->n - 1) % N;
        if (f->d.mode != YRT_FIT_UNBIASED) {
            /* the less late of the two, at the nominal tick */
            if ((double)(y - f->py[i]) < (double)(x - f->px[i]) * f->d.ns_per_tick) {
                f->px[i] = x; f->py[i] = y; f->pw[i] = w;
                changed = true;
            }
        } else {
            f->ux += (double)(x - f->ux0);
            f->uy += (double)(y - f->uy0);
            f->un++;
            f->px[i] = f->ux0 + yrt__fit_round(f->ux / f->un);
            f->py[i] = f->uy0 + yrt__fit_round(f->uy / f->un);
            changed = true;
        }
    } else {
        if (f->n > 0) {
            yrt__fit_record(f, (uint16_t)YRT_KIND_CLOCK, (f->head + f->n - 1) % N);
            closed = true;
        }
        if (f->n == N) { f->head = (f->head + 1) % N; f->n--; }
        i = (f->head + f->n) % N;
        f->n++;
        f->px[i] = x; f->py[i] = y; f->pw[i] = w;
        f->bucket = bk;
        f->ux0 = x; f->uy0 = y; f->ux = 0.0; f->uy = 0.0; f->un = 1;
        changed = true;
    }
    if (changed) rc |= YRT_FIT_POINT;
    if (first || closed || (changed && (below || f->n <= YRT__FIT_EAGER ||
                                        (f->d.mode == YRT_FIT_UNBIASED && !f->slope)))) {
        yrt__fit_refit(f);
        rc |= YRT_FIT_REFIT;
        if (first || closed) yrt__fit_record(f, (uint16_t)YRT_KIND_FIT, 0);
    }
    return rc;
}

static int yrt__fit_add1(yrt_fit* f, uint64_t ticks, int64_t y, int64_t w);

/* A pair far from the map is held. Held pairs that agree with each other at
 * the nominal tick for restart_hold_ns are a new clock (a reset, a replug):
 * a new epoch starts from them. A burst of late pairs after a stall arrives
 * within milliseconds, so it never holds long enough; a normal pair clears
 * the hold. Eight are kept: the first, which times the hold, and the newest
 * seven. */
static int yrt__fit_hold(yrt_fit* f, uint64_t ticks, int64_t y, int64_t w) {
    int32_t i, n;
    uint64_t t[8], u0, u;
    int64_t  yy[8], ww[8], o, omin = 0, omax = 0;
    double   kn = f->d.ns_per_tick;
    int      rc = YRT_FIT_PENDING;
    if (f->pend_n == 8) {
        /* keep the first held pair: the hold is timed from it */
        for (i = 2; i < 8; i++) {
            f->pend_t[i - 1] = f->pend_t[i]; f->pend_y[i - 1] = f->pend_y[i];
            f->pend_w[i - 1] = f->pend_w[i];
        }
        f->pend_n--;
        f->rejected++;
    }
    f->pend_t[f->pend_n] = ticks; f->pend_y[f->pend_n] = y; f->pend_w[f->pend_n] = w;
    f->pend_n++;
    n = f->pend_n;
    if (n < 2 || f->pend_y[n - 1] - f->pend_y[0] < f->d.restart_hold_ns) return rc;
    u0 = f->modulus ? (f->pend_t[0] & (f->modulus - 1u)) : f->pend_t[0];
    for (i = 1; i < n; i++) {
        u = yrt__fit_unwrap_to(f, f->pend_t[i],
                                 u0 + (uint64_t)yrt__fit_round((double)(f->pend_y[i] - f->pend_y[0]) / kn));
        o = (f->pend_y[i] - f->pend_y[0]) - yrt__fit_round((double)(int64_t)(u - u0) * kn);
        if (o < omin) omin = o;
        if (o > omax) omax = o;
    }
    if (omax - omin > f->d.restart_ns) return rc;
    for (i = 0; i < n; i++) { t[i] = f->pend_t[i]; yy[i] = f->pend_y[i]; ww[i] = f->pend_w[i]; }
    f->pend_n = 0;
    f->n = 0; f->head = 0; f->have = 0; f->slope = 0; f->un = 0;
    f->epoch++;
    rc = YRT_FIT_RESTART;
    for (i = 0; i < n; i++) rc |= yrt__fit_add1(f, t[i], yy[i], ww[i]);
    return rc & ~YRT_FIT_PENDING;
}

static int yrt__fit_add1(yrt_fit* f, uint64_t ticks, int64_t y, int64_t w) {
    uint64_t u;
    int64_t  res;
    int      rc = 0;
    if (!f->have) {
        f->base_ticks = f->modulus ? (ticks & (f->modulus - 1u)) : ticks;
        f->base_host = y;
        return yrt__fit_point(f, f->base_ticks, y, w, true);
    }
    u = yrt__fit_unwrap_to(f, ticks, yrt__fit_expect(f, y));
    res = y - yrt__fit_at(f, (int64_t)(u - f->base_ticks));
    if (res > f->d.restart_ns || res < -f->d.restart_ns) return yrt__fit_hold(f, ticks, y, w);
    if (f->pend_n) {
        f->rejected += (uint32_t)f->pend_n;
        f->pend_n = 0;
        rc |= YRT_FIT_REJECTED;
    }
    /* below the line matters to the envelope only: UNBIASED pairs are below
     * it half the time, and a refit for each would cost 10 to 20 us */
    return rc | yrt__fit_point(f, u, y, w, res < 0 && f->d.mode != YRT_FIT_UNBIASED);
}

bool yrt_fit_open(yrt_fit* f, const yrt_fit_desc* d) {
    if (!f) return false;
    if (!d) { snprintf(f->error, sizeof f->error, "ysp_rt: fit: NULL desc"); return false; }
    if (d->mode < YRT_FIT_LATE || d->mode > YRT_FIT_UNBIASED) {
        snprintf(f->error, sizeof f->error, "ysp_rt: fit: mode %d is not a yrt_fit_mode", d->mode);
        return false;
    }
    if (!(d->ns_per_tick > 0.0) || d->tick_bits > 64u) {
        snprintf(f->error, sizeof f->error,
                 "ysp_rt: fit: ns_per_tick must be > 0 and tick_bits at most 64 (got %g, %u)",
                 d->ns_per_tick, (unsigned)d->tick_bits);
        return false;
    }
    memset(f, 0, sizeof *f);
    f->d = *d;
    if (f->d.bucket_ns <= 0)       f->d.bucket_ns = 200000000LL;
    if (f->d.slope_span_ns <= 0)   f->d.slope_span_ns = 5000000000LL;
    if (f->d.restart_ns <= 0)      f->d.restart_ns = 50000000LL;
    if (f->d.restart_hold_ns <= 0) f->d.restart_hold_ns = 1000000000LL;
    if (f->d.max_width_ns <= 0)    f->d.max_width_ns = 1300000LL;
    f->modulus = (d->tick_bits > 0u && d->tick_bits < 64u) ? (1ull << d->tick_bits) : 0u;
    f->k = d->ns_per_tick;
    return true;
}

void yrt_fit_close(yrt_fit* f) {
    /* A zero ns_per_tick is what every other fit call reads as "not open". */
    if (f) memset(f, 0, sizeof *f);
}

const char* yrt_fit_error(const yrt_fit* f) {
    return f ? f->error : "ysp_rt: NULL fit";
}

int yrt_fit_add(yrt_fit* f, uint64_t ticks, int64_t host_ns, int64_t width_ns) {
    if (!f || !(f->d.ns_per_tick > 0.0)) return YRT_ERR_ARG;
    f->pairs++;
    if (f->d.mode == YRT_FIT_BRACKET && (width_ns < 0 || width_ns > f->d.max_width_ns)) {
        f->rejected++;
        return YRT_FIT_REJECTED;
    }
    return yrt__fit_add1(f, ticks, host_ns, width_ns);
}

uint64_t yrt_fit_unwrap(const yrt_fit* f, uint64_t ticks) {
    if (!f) return ticks;
    return yrt__fit_unwrap_to(f, ticks, f->last_ticks);
}

int64_t yrt_fit_map(const yrt_fit* f, uint64_t ticks) {
    if (!f || !f->have) return 0;
    return yrt__fit_at(f, (int64_t)(yrt_fit_unwrap(f, ticks) - f->base_ticks));
}

void yrt_fit_get(const yrt_fit* f, yrt_fit_info* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!f) return;
    out->pairs    = f->pairs;
    out->epoch    = f->epoch;
    out->rejected = f->rejected;
    out->pending  = (uint32_t)f->pend_n;
    out->points   = (uint32_t)f->n;
    if (!f->have) return;
    out->ready       = true;
    out->slope       = f->slope != 0;
    out->t0_ns       = f->fy;
    out->ticks0      = f->base_ticks + (uint64_t)f->fx;
    out->ns_per_tick = f->k;
    out->ppm         = (f->k / f->d.ns_per_tick - 1.0) * 1e6;
    out->spread_ns   = f->spread_ns;
    out->width_ns    = f->width_ns;
    out->span_ns     = f->py[(f->head + f->n - 1) % YRT_FIT_POINTS] - f->py[f->head];
}

/* ======================================================================= *
 *  PER-USER FOLDERS
 * ======================================================================= */
#if defined(YRT__POSIX) && !defined(YRT__EMSCRIPTEN)
#include <stdlib.h>
#include <sys/stat.h>
#endif

/* Names separated by '/': no empty name (so no leading, trailing or double
 * slash), no "." or "..", no backslash or colon (a second root on
 * Windows), no control character. */
static bool yrt__subdir_ok(const char* s) {
    size_t i, start = 0, n, len;
    if (!s) return true;
    n = strlen(s);
    for (i = 0; i <= n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (i < n && (c < 0x20u || c == '\\' || c == ':')) return false;
        if (i == n || c == '/') {
            len = i - start;
            if (n > 0 && len == 0) return false;
            if (len == 1 && s[start] == '.') return false;
            if (len == 2 && s[start] == '.' && s[start + 1] == '.') return false;
            start = i + 1;
        }
    }
    return true;
}

#if !defined(YRT__EMSCRIPTEN)
/* Appends sep "ysp" [sep subdir] to the n bytes in out; 0 when it does not
 * fit, else the new length. */
static size_t yrt__dir_tail(char* out, size_t n, size_t cap, const char* subdir, char sep) {
    size_t m = subdir ? strlen(subdir) : 0, i;
    size_t need = n + 4u + (m ? 1u + m : 0u);
    if (need + 1u > cap) return 0;
    out[n] = sep;
    memcpy(out + n + 1, "ysp", 3);
    n += 4;
    if (m) {
        out[n++] = sep;
        for (i = 0; i < m; i++) out[n++] = subdir[i] == '/' ? sep : subdir[i];
    }
    out[n] = '\0';
    return n;
}
#endif

int yrt_user_dir(int kind, const char* subdir, char* out, size_t cap) {
    if (!out || cap == 0) return YRT_ERR_ARG;
    out[0] = '\0';
    if ((kind != YRT_DIR_CACHE && kind != YRT_DIR_CONFIG) || !yrt__subdir_ok(subdir)) return YRT_ERR_ARG;
#if defined(YRT__EMSCRIPTEN)
    return YRT_ERR_ARG;                   /* no per-user folder on the web */
#elif defined(YRT__WINDOWS)
    {
        wchar_t w[512];
        DWORD k = GetEnvironmentVariableW(kind == YRT_DIR_CACHE ? L"LOCALAPPDATA" : L"APPDATA", w, 512);
        int m;
        /* Absolute only: a drive and a root, or a UNC path. */
        if (k == 0 || k >= 512 || !((k >= 3 && w[1] == L':' && (w[2] == L'\\' || w[2] == L'/')) || (w[0] == L'\\' && w[1] == L'\\')))
            return YRT_ERR_ARG;
        while (k > 3 && (w[k - 1] == L'\\' || w[k - 1] == L'/')) w[--k] = L'\0';
        m = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)(cap < 0x7FFFFFFF ? cap : 0x7FFFFFFF), NULL, NULL);
        if (m <= 1 || yrt__dir_tail(out, (size_t)m - 1u, cap, subdir, '\\') == 0) { out[0] = '\0'; return YRT_ERR_ARG; }
        return YRT_OK;
    }
#else
    {
        const char* base = NULL;
        size_t i, n, end;
        uid_t me = geteuid();
#if !defined(YRT__DARWIN)
        base = getenv(kind == YRT_DIR_CACHE ? "XDG_CACHE_HOME" : "XDG_CONFIG_HOME");
        if (!base || base[0] != '/') base = NULL;     /* the XDG spec: a relative one is ignored */
#endif
        if (!base) {
#if defined(YRT__DARWIN)
            const char* sub = kind == YRT_DIR_CACHE ? "/Library/Caches" : "/Library/Application Support";
#else
            const char* sub = kind == YRT_DIR_CACHE ? "/.cache" : "/.config";
#endif
            const char* home = getenv("HOME");
            size_t ns;
            if (!home || home[0] != '/') return YRT_ERR_ARG;
            n = strlen(home);
            ns = strlen(sub);
            if (n + ns + 1u > cap) return YRT_ERR_ARG;
            memcpy(out, home, n + 1);
            while (n > 1 && out[n - 1] == '/') out[--n] = '\0';
            memcpy(out + n, sub, ns + 1);
            n += ns;
        } else {
            n = strlen(base);
            if (n + 1u > cap) return YRT_ERR_ARG;
            memcpy(out, base, n + 1);
        }
        while (n > 1 && out[n - 1] == '/') out[--n] = '\0';
        end = yrt__dir_tail(out, n, cap, subdir, '/');
        if (end == 0) { out[0] = '\0'; return YRT_ERR_ARG; }
        /* Each part of the path that exists, from the root: refused when all
         * users can write it (/tmp is 1777) or another user owns it (a
         * folder planted under /tmp before the first run). Root may own. */
        for (i = 1; i <= end; i++) {
            if (out[i] == '/' || i == end) {
                struct stat st;
                char c = out[i];
                bool bad;
                out[i] = '\0';
                bad = stat(out, &st) == 0 && ((st.st_mode & S_IWOTH) != 0 || (st.st_uid != me && st.st_uid != 0));
                out[i] = c;
                if (bad) { out[0] = '\0'; return YRT_ERR_ARG; }
            }
        }
        return YRT_OK;
    }
#endif
}

/* ======================================================================= *
 *  DEADLINE WORKER
 *
 *  One thread, one pending job, one absolute deadline. The thread elevates
 *  itself and publishes the rung before yrt_worker_open() returns, so the
 *  policy is known before the first trial and no trial pays for thread
 *  creation. Everything the worker needs lives in the caller's handle, so
 *  nothing allocates once it is running.
 * ======================================================================= */
#ifndef YRT_NO_THREADS

/* Appended to a failed thread creation. A wasm module built without -pthread
 * links Emscripten's pthread stubs, so everything compiles and the first
 * pthread_create() says only "Not supported"; the cause is a build flag and
 * the message should say which. */
#if defined(YRT__EMSCRIPTEN) && !defined(__EMSCRIPTEN_PTHREADS__)
    #define YRT__THREAD_HINT " (this wasm module was built without -pthread; " \
                               "see WEBASSEMBLY)"
#else
    #define YRT__THREAD_HINT ""
#endif

/* `running` is the one field that crosses threads outside the worker lock:
 * a job may call yrt_worker_submit() at the instant yrt_worker_close()
 * clears the flag, and a flag whose whole job is to say whether the lock is
 * still there cannot itself be guarded by that lock. One integer, read and
 * written atomically, is the whole synchronization. */
#if defined(_MSC_VER)
static int yrt__flag_load(const int* p) { return (int)*(const volatile LONG*)p; }
static void yrt__flag_store(int* p, int v) {
    (void)InterlockedExchange((volatile LONG*)p, (LONG)v);
}
#elif defined(__GNUC__) || defined(__clang__)
static int yrt__flag_load(const int* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void yrt__flag_store(int* p, int v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
#else
/* No atomics available: volatile keeps the compiler from caching the flag,
 * which is all any real 32-bit aligned load or store needs here. */
static int yrt__flag_load(const int* p) { return *(const volatile int*)p; }
static void yrt__flag_store(int* p, int v) { *(volatile int*)p = v; }
#endif

/* Sequentially consistent forms, for the one place that needs a store-load
 * handshake between two threads (the pump's waiter registration against its
 * stop, a Dekker pattern). Acquire/release is not enough there: each side
 * stores one flag and then loads the other's, and only a total order
 * guarantees that at least one of them sees the other. The Interlocked calls
 * are full barriers on MSVC, and a compare-exchange of 0 with 0 is its
 * sequentially consistent load. */
#if defined(_MSC_VER)
static int yrt__sc_load(const int* p) {
    return (int)InterlockedCompareExchange((volatile LONG*)(uintptr_t)p, 0, 0);
}
static void yrt__sc_store(int* p, int v) {
    (void)InterlockedExchange((volatile LONG*)p, (LONG)v);
}
static int yrt__sc_add(int* p, int v) {
    return (int)InterlockedExchangeAdd((volatile LONG*)p, (LONG)v) + v;
}
#elif defined(__GNUC__) || defined(__clang__)
static int yrt__sc_load(const int* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void yrt__sc_store(int* p, int v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static int yrt__sc_add(int* p, int v) { return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
#else
#error "ysp_rt: the pump needs atomic read-modify-write; define YRT_NO_THREADS"
#endif

/* The generation is also the job's public seq, which yrt_worker_submit()
 * returns as an int, so it is kept inside 1..INT32_MAX: 0 means "no job" to
 * yrt_worker_pending() and a negative return means an error. Wrapping keeps
 * the counter unique for long enough that no session can see a repeat, and
 * the generation check only needs "different", not "greater". */
static uint32_t yrt__next_gen(uint32_t g) {
    return (g >= 0x7fffffffu) ? 1u : g + 1u;
}

yrt_policy yrt_worker_policy(const yrt_worker* w) {
    return w ? w->policy : YRT_POLICY_NONE;
}

const char* yrt_worker_error(const yrt_worker* w) {
    return w ? w->error : "null worker handle";
}

bool yrt_worker_is_running(const yrt_worker* w) {
    return w && yrt__flag_load(&w->running) != 0;
}

#if defined(YRT__POSIX)
/* ---- POSIX: pthreads, PI mutex, CLOCK_MONOTONIC condvar --------------- */

typedef struct yrt__wstate {
    pthread_t       thread;
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
} yrt__wstate;

#else
/* ---- Windows: critical section, condition variable, waitable timer ---- */

typedef struct yrt__wstate {
    CRITICAL_SECTION   cs;
    CONDITION_VARIABLE cv;
    HANDLE thread;
    HANDLE timer;   /* the coarse wait                                    */
    HANDLE wakeup;  /* auto-reset: a submit happened during a timed wait  */
    bool   hires;
    unsigned short fpcw; /* the starting thread's x87 control word, MinGW */
} yrt__wstate;

/* A thread Windows creates starts with the x87 control word at 0x27F (53-bit
 * precision), but mingw-w64's msvcrt startup code runs fninit on the main
 * thread and leaves it at 0x37F (64-bit), and that CRT's exp and pow are x87
 * code, so the same exp() on the pump thread can differ from the main
 * thread's by an ulp (measured: exp(-3.2171) and pow(12.5, 1.37) with GCC
 * 16.1 / mingw-w64 14 msvcrt). The new thread takes the control word of the
 * thread that started it, so work moved onto it computes what it computed
 * before. MSVC and UCRT math are SSE2, where this word is not read. */
#if defined(__MINGW32__) && (defined(__x86_64__) || defined(__i386__))
static unsigned short yrt__fpcw_get(void) {
    unsigned short cw;
    __asm__ __volatile__("fnstcw %0" : "=m"(cw));
    return cw;
}
static void yrt__fpcw_set(unsigned short cw) {
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
}
#else
static unsigned short yrt__fpcw_get(void) { return 0; }
static void yrt__fpcw_set(unsigned short cw) { (void)cw; }
#endif

#endif

YRT__ALIGN_PROBE(yrt__wstate);
YRT__STATIC_ASSERT(sizeof(yrt__wstate) <= sizeof(((yrt_worker*)0)->os),
                     "yrt_worker.os is too small for this platform's OS objects");
YRT__STATIC_ASSERT(YRT__ALIGNOF(yrt__wstate) <= 8,
                     "yrt_worker.os is not aligned enough for this platform");

/* The OS objects live inside the caller's handle; this is the one place that
 * knows it. */
static yrt__wstate* yrt__w(yrt_worker* w) {
    return (yrt__wstate*)(void*)&w->os;
}

static void yrt__worker_lock(yrt_worker* w);
static void yrt__worker_unlock(yrt_worker* w);

/* Run a desc.on_start on the thread that will use what it sets up, which is
 * the whole point of it: ioperm(), iopl() and the rest of what an OS keeps per
 * thread can only be asked for from that thread. Called before the ready
 * handshake, so the message it leaves is published to the start() call by the
 * same lock that publishes the ready flag. Shared by the worker and the pump;
 * `what` is the prefix for the message a silent refusal gets. */
static bool yrt__run_on_start(yrt_start_fn fn, void* ctx, char* err,
                                size_t err_cap, const char* what) {
    char msg[256];
    if (!fn) return true;
    msg[0] = '\0';
    if (fn(ctx, msg, sizeof(msg))) return true;
    /* A hook that refuses without saying why still must not fail silently. */
    if (msg[0] == '\0')
        snprintf(err, err_cap, "%s: desc.on_start refused", what);
    else
        snprintf(err, err_cap, "%s", msg);
    return false;
}

static bool yrt__worker_on_start(yrt_worker* w) {
    return yrt__run_on_start(w->on_start, w->start_ctx, w->error,
                               sizeof(w->error), "worker");
}

/* Take the pending job and run it with the lock RELEASED. Called with the
 * lock held and returns with it held. Dropping the lock is what lets the
 * callback submit to this same worker and block without deadlocking; see
 * WORKER for what the caller owes in exchange. */
static void yrt__worker_run(yrt_worker* w, bool flushed) {
    yrt_job_fn fn = w->fn;
    void* ctx = w->ctx;
    yrt_job_info info;
    info.deadline_ns = (int64_t)w->deadline_ns;
    info.flushed = flushed;
    /* The generation the submit stamped this job with, read before the bump
     * that retires it. */
    info.seq = w->gen;
    w->has_job = 0;
    w->gen = yrt__next_gen(w->gen);
    yrt__worker_unlock(w);
    info.at_ns = yrt__clock();
    info.late_ns = info.at_ns - info.deadline_ns;
    if (fn) {
        YRT_ZONE(z, "yrt.worker.job");
        YRT_ZONE_VALUE(z, info.late_ns);
        fn(ctx, &info);
        YRT_ZONE_END(z);
    }
    yrt__worker_lock(w);
}

#if defined(YRT__POSIX)

static void yrt__worker_lock(yrt_worker* w)   { pthread_mutex_lock(&yrt__w(w)->mtx); }
static void yrt__worker_unlock(yrt_worker* w) { pthread_mutex_unlock(&yrt__w(w)->mtx); }

/* Wait for a submit, a cancel or a quit. Lock held on entry and on return. */
static void yrt__worker_wait_idle(yrt_worker* w) {
    yrt__wstate* s = yrt__w(w);
    pthread_cond_wait(&s->cv, &s->mtx);
}

/* Wait until `until_ns` or until something changes, whichever comes first.
 * Lock held on entry and on return; the caller re-evaluates from the top, so
 * a spurious wake costs one clock read. */
static void yrt__worker_wait_until(yrt_worker* w, uint64_t until_ns) {
    yrt__wstate* s = yrt__w(w);
#if defined(YRT__DARWIN)
    /* Darwin has no pthread_condattr_setclock, so an absolute wait would run
     * against CLOCK_REALTIME and step with the wall clock. Wait relative to
     * the monotonic clock instead. */
    uint64_t now = yrt__now();
    if (until_ns <= now) return;
    struct timespec rel;
    yrt__ns_to_ts(until_ns - now, &rel);
    pthread_cond_timedwait_relative_np(&s->cv, &s->mtx, &rel);
#else
    struct timespec ts;
    yrt__ns_to_ts(until_ns, &ts);
    pthread_cond_timedwait(&s->cv, &s->mtx, &ts);
#endif
}

static void yrt__worker_wake(yrt_worker* w) {
    pthread_cond_signal(&yrt__w(w)->cv);
}

static void yrt__worker_loop(yrt_worker* w);

static void* yrt__worker_main(void* arg) {
    yrt_worker* w = (yrt_worker*)arg;
    yrt_policy pol = w->no_elevate ? YRT_POLICY_NORMAL
                                     : yrt_thread_elevate(&w->sched);
    /* Before the handshake, so a tracer allocates its per-thread state now
     * and not inside the first job. */
    YRT_THREAD_INIT("yrt worker");
    /* After the elevation, so a hook can see the rung it is running at, and
     * before the handshake, so a refusal fails the start instead of a job. */
    bool ok = yrt__worker_on_start(w);
    yrt__worker_lock(w);
    /* Publish the rung and release yrt_worker_open(), which waits for it
     * so w->policy is final when start returns. */
    w->policy = ok ? pol : YRT_POLICY_NONE;
    w->start_failed = ok ? 0 : 1;
    w->ready = 1;
    pthread_cond_broadcast(&yrt__w(w)->cv);
    if (ok) yrt__worker_loop(w);
    yrt__worker_unlock(w);
    return NULL;
}

bool yrt_worker_open(yrt_worker* w, const yrt_worker_desc* desc) {
    if (!w) return false;
    yrt_worker_desc d;
    memset(&d, 0, sizeof(d));
    if (desc) d = *desc;
    if (yrt__flag_load(&w->running)) return true;

    memset(w, 0, sizeof(*w));
    w->sched = d.sched;
    if (yrt_sched_normalize(&w->sched) < 0) {
        snprintf(w->error, sizeof(w->error),
                 "invalid desc.sched: require 0 < runtime_ns <= deadline_ns <= "
                 "period_ns (period_ns 0 means same as deadline_ns)");
        return false;
    }
    w->spin_ns    = (uint32_t)(d.spin_ns > 0 && d.spin_ns < (int64_t)UINT32_MAX ? d.spin_ns : YRT_DEFAULT_SPIN_NS);
    w->no_elevate = d.no_elevate;
    w->on_start   = d.on_start;
    w->start_ctx  = d.start_ctx;
    w->policy     = YRT_POLICY_NONE;

    yrt__wstate* s = yrt__w(w);
    /* Priority inheritance: a normal-priority submitter holds the mutex for a
     * few instructions, and without PI the real-time worker would wait behind
     * whatever preempted that submitter. */
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    int mtx_rc = pthread_mutex_init(&s->mtx, &ma);
    pthread_mutexattr_destroy(&ma);
    if (mtx_rc != 0) {
        snprintf(w->error, sizeof(w->error), "worker: mutex init: %s", strerror(mtx_rc));
        return false;
    }
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
#if !defined(YRT__DARWIN)
    /* Absolute timed waits run against the same clock the deadlines are in. */
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
#endif
    int cv_rc = pthread_cond_init(&s->cv, &ca);
    pthread_condattr_destroy(&ca);
    if (cv_rc != 0) {
        pthread_mutex_destroy(&s->mtx);
        snprintf(w->error, sizeof(w->error), "worker: cond init: %s", strerror(cv_rc));
        return false;
    }
    /* pthread_create returns the error; it does not set errno. */
    int rc = pthread_create(&s->thread, NULL, yrt__worker_main, w);
    if (rc != 0) {
        pthread_cond_destroy(&s->cv);
        pthread_mutex_destroy(&s->mtx);
        snprintf(w->error, sizeof(w->error), "worker: thread create: %s%s",
                 strerror(rc), YRT__THREAD_HINT);
        return false;
    }
    pthread_mutex_lock(&s->mtx);
    while (!w->ready) pthread_cond_wait(&s->cv, &s->mtx);
    int refused = w->start_failed;
    pthread_mutex_unlock(&s->mtx);
    if (refused) {
        /* The thread is on its way out and never set `running`, so nothing
         * can be in the lock; join it before the objects it used go away.
         * w->error already holds the hook's message. */
        pthread_join(s->thread, NULL);
        pthread_cond_destroy(&s->cv);
        pthread_mutex_destroy(&s->mtx);
        return false;
    }
    yrt__flag_store(&w->running, 1);
    return true;
}

void yrt_worker_close(yrt_worker* w) {
    if (!w || !yrt__flag_load(&w->running)) return;
    yrt__wstate* s = yrt__w(w);
    /* Clear `running` first: a submit racing this teardown (a contract
     * violation, but a cheap one to survive) must be refused rather than
     * reach for a mutex that is about to be destroyed. */
    yrt__flag_store(&w->running, 0);
    pthread_mutex_lock(&s->mtx);
    w->quit = 1;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mtx);
    pthread_join(s->thread, NULL);
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->mtx);
    w->policy = YRT_POLICY_NONE;
}

int yrt_worker_submit(yrt_worker* w, int64_t deadline_ns,
                        yrt_job_fn fn, void* ctx) {
    if (!w || !fn) return YRT_ERR_ARG;
    if (!yrt__flag_load(&w->running)) return YRT_ERR_STOPPED;
    yrt__wstate* s = yrt__w(w);
    pthread_mutex_lock(&s->mtx);
    w->deadline_ns = yrt__unsigned(deadline_ns);
    w->fn  = fn;
    w->ctx = ctx;
    w->has_job = 1;
    w->gen = yrt__next_gen(w->gen);
    uint32_t seq = w->gen;
    yrt__worker_wake(w);
    pthread_mutex_unlock(&s->mtx);
    return (int)seq;
}

int yrt_worker_cancel(yrt_worker* w) {
    if (!w) return YRT_ERR_ARG;
    if (!yrt__flag_load(&w->running)) return YRT_ERR_STOPPED;
    yrt__wstate* s = yrt__w(w);
    pthread_mutex_lock(&s->mtx);
    int had = w->has_job;
    w->has_job = 0;
    w->gen = yrt__next_gen(w->gen);
    yrt__worker_wake(w);
    pthread_mutex_unlock(&s->mtx);
    return had ? 1 : 0;
}

#else /* YRT__WINDOWS */

static void yrt__worker_lock(yrt_worker* w)   { EnterCriticalSection(&yrt__w(w)->cs); }
static void yrt__worker_unlock(yrt_worker* w) { LeaveCriticalSection(&yrt__w(w)->cs); }

static void yrt__worker_wait_idle(yrt_worker* w) {
    yrt__wstate* s = yrt__w(w);
    SleepConditionVariableCS(&s->cv, &s->cs, INFINITE);
}

/* Wait until `until_ns` or until a submit wakes us. The condition variable
 * cannot be waited on together with a waitable timer, so the timed wait uses
 * the timer plus an auto-reset event that every submit sets. A submit that
 * arrives while the worker is between the unlock and the wait leaves the
 * event signaled, so a wakeup is never lost; the cost is one spurious loop. */
static void yrt__worker_wait_until(yrt_worker* w, uint64_t until_ns) {
    yrt__wstate* s = yrt__w(w);
    yrt__worker_unlock(w);
    uint64_t now = yrt__now();
    if (until_ns > now) {
        uint64_t delta = until_ns - now;
        LARGE_INTEGER due; /* negative = relative, 100 ns units */
        due.QuadPart = -(LONGLONG)(delta / 100ull);
        if (s->timer && SetWaitableTimer(s->timer, &due, 0, NULL, NULL, FALSE)) {
            HANDLE h[2];
            h[0] = s->timer;
            h[1] = s->wakeup;
            (void)WaitForMultipleObjects(2, h, FALSE, INFINITE);
        } else {
            (void)WaitForSingleObject(s->wakeup, (DWORD)(delta / 1000000ull));
        }
    }
    yrt__worker_lock(w);
}

static void yrt__worker_wake(yrt_worker* w) {
    yrt__wstate* s = yrt__w(w);
    WakeConditionVariable(&s->cv); /* an idle worker */
    SetEvent(s->wakeup);           /* a worker in a timed wait */
}

static void yrt__worker_loop(yrt_worker* w);

static DWORD yrt__worker_thread(yrt_worker* w);

static DWORD WINAPI yrt__worker_main(LPVOID arg) {
    /* First, before anything computes on this thread. */
    yrt__fpcw_set(yrt__w((yrt_worker*)arg)->fpcw);
    return yrt__worker_thread((yrt_worker*)arg);
}

static DWORD yrt__worker_thread(yrt_worker* w) {
    yrt_policy pol = w->no_elevate ? YRT_POLICY_NORMAL
                                     : yrt_thread_elevate(&w->sched);
    /* Before the handshake, so a tracer allocates its per-thread state now
     * and not inside the first job. */
    YRT_THREAD_INIT("yrt worker");
    /* See the POSIX twin: after the elevation, before the handshake. */
    bool ok = yrt__worker_on_start(w);
    yrt__worker_lock(w);
    /* Publish the rung and release yrt_worker_open(). */
    w->policy = ok ? pol : YRT_POLICY_NONE;
    w->start_failed = ok ? 0 : 1;
    w->ready = 1;
    WakeAllConditionVariable(&yrt__w(w)->cv);
    if (ok) yrt__worker_loop(w);
    yrt__worker_unlock(w);
    return 0;
}

bool yrt_worker_open(yrt_worker* w, const yrt_worker_desc* desc) {
    if (!w) return false;
    yrt_worker_desc d;
    memset(&d, 0, sizeof(d));
    if (desc) d = *desc;
    if (yrt__flag_load(&w->running)) return true;

    memset(w, 0, sizeof(*w));
    w->sched = d.sched;
    if (yrt_sched_normalize(&w->sched) < 0) {
        snprintf(w->error, sizeof(w->error),
                 "invalid desc.sched: require 0 < runtime_ns <= deadline_ns <= "
                 "period_ns (period_ns 0 means same as deadline_ns)");
        return false;
    }
    w->spin_ns    = (uint32_t)(d.spin_ns > 0 && d.spin_ns < (int64_t)UINT32_MAX ? d.spin_ns : YRT_DEFAULT_SPIN_NS);
    w->no_elevate = d.no_elevate;
    w->on_start   = d.on_start;
    w->start_ctx  = d.start_ctx;
    w->policy     = YRT_POLICY_NONE;

    yrt__wstate* s = yrt__w(w);
    InitializeCriticalSection(&s->cs);
    InitializeConditionVariable(&s->cv);
    s->wakeup = CreateEventA(NULL, FALSE, FALSE, NULL);
    s->timer  = yrt__timer_create(&s->hires);
    if (!s->wakeup || !s->timer) {
        snprintf(w->error, sizeof(w->error),
                 "worker: timer/event create failed (err %lu)",
                 (unsigned long)GetLastError());
        if (s->wakeup) CloseHandle(s->wakeup);
        if (s->timer)  CloseHandle(s->timer);
        DeleteCriticalSection(&s->cs);
        return false;
    }
    s->fpcw = yrt__fpcw_get();
    s->thread = CreateThread(NULL, 0, yrt__worker_main, w, 0, NULL);
    if (!s->thread) {
        snprintf(w->error, sizeof(w->error), "worker: thread create failed (err %lu)",
                 (unsigned long)GetLastError());
        CloseHandle(s->wakeup);
        CloseHandle(s->timer);
        DeleteCriticalSection(&s->cs);
        return false;
    }
    EnterCriticalSection(&s->cs);
    while (!w->ready) SleepConditionVariableCS(&s->cv, &s->cs, INFINITE);
    int refused = w->start_failed;
    LeaveCriticalSection(&s->cs);
    if (refused) {
        /* See the POSIX twin: join before the objects go away. w->error
         * already holds the hook's message. */
        WaitForSingleObject(s->thread, INFINITE);
        CloseHandle(s->thread);
        CloseHandle(s->timer);
        CloseHandle(s->wakeup);
        DeleteCriticalSection(&s->cs);
        return false;
    }
    yrt__flag_store(&w->running, 1);
    return true;
}

void yrt_worker_close(yrt_worker* w) {
    if (!w || !yrt__flag_load(&w->running)) return;
    yrt__wstate* s = yrt__w(w);
    /* Clear `running` first; see the POSIX twin. */
    yrt__flag_store(&w->running, 0);
    EnterCriticalSection(&s->cs);
    w->quit = 1;
    LeaveCriticalSection(&s->cs);
    WakeAllConditionVariable(&s->cv);
    SetEvent(s->wakeup);
    WaitForSingleObject(s->thread, INFINITE);
    CloseHandle(s->thread);
    CloseHandle(s->timer);
    CloseHandle(s->wakeup);
    DeleteCriticalSection(&s->cs);
    w->policy = YRT_POLICY_NONE;
}

int yrt_worker_submit(yrt_worker* w, int64_t deadline_ns,
                        yrt_job_fn fn, void* ctx) {
    if (!w || !fn) return YRT_ERR_ARG;
    if (!yrt__flag_load(&w->running)) return YRT_ERR_STOPPED;
    yrt__wstate* s = yrt__w(w);
    EnterCriticalSection(&s->cs);
    w->deadline_ns = yrt__unsigned(deadline_ns);
    w->fn  = fn;
    w->ctx = ctx;
    w->has_job = 1;
    w->gen = yrt__next_gen(w->gen);
    uint32_t seq = w->gen;
    LeaveCriticalSection(&s->cs);
    yrt__worker_wake(w);
    return (int)seq;
}

int yrt_worker_cancel(yrt_worker* w) {
    if (!w) return YRT_ERR_ARG;
    if (!yrt__flag_load(&w->running)) return YRT_ERR_STOPPED;
    yrt__wstate* s = yrt__w(w);
    EnterCriticalSection(&s->cs);
    int had = w->has_job;
    w->has_job = 0;
    w->gen = yrt__next_gen(w->gen);
    LeaveCriticalSection(&s->cs);
    yrt__worker_wake(w);
    return had ? 1 : 0;
}

#endif /* platform */

uint32_t yrt_worker_pending(const yrt_worker* w) {
    if (!w || !yrt__flag_load(&w->running)) return 0;
    /* The lock is what makes the pair (has_job, gen) consistent, and taking
     * it is a mutation the const in the signature cannot express: the handle
     * the CALLER sees is unchanged. */
    yrt_worker* m = (yrt_worker*)(uintptr_t)w;
    yrt__worker_lock(m);
    uint32_t seq = m->has_job ? m->gen : 0u;
    yrt__worker_unlock(m);
    return seq;
}

/* The loop itself is platform-independent; only the four wait/wake helpers
 * above differ. Entered and left with the lock held. */
static void yrt__worker_loop(yrt_worker* w) {
    while (!w->quit) {
        if (!w->has_job) {
            yrt__worker_wait_idle(w);
            continue;
        }
        uint64_t deadline = w->deadline_ns;
        uint32_t gen = w->gen;
        uint64_t spin = w->spin_ns;
#if defined(YRT__WINDOWS)
        /* Without a high-resolution timer the coarse wait cannot land closer
         * than a system tick, so the last tick is spun. See yrt_sleep_until. */
        if (!yrt__w(w)->hires && spin < YRT__TICK_NS) spin = YRT__TICK_NS;
#endif
        uint64_t now;
        if (yrt__rload(&yrt__v_on)) {
            /* VIRTUAL CLOCK: the job runs once the driver has moved the
             * clock to its deadline; the worker looks in real 100 us steps. */
            if (yrt__clock() < (int64_t)deadline) {
                yrt__worker_wait_until(w, yrt__now() + 100000u);
                continue;
            }
            yrt__worker_run(w, false);
            continue;
        }
        now = yrt__now();
        uint64_t wake = (deadline > spin) ? deadline - spin : 0;
        if (now < wake) {
            /* Wakes on the deadline OR on a submit, then re-evaluates from
             * the top, so a replacement job takes effect at once whether its
             * deadline is earlier or later than the one it replaced. */
            yrt__worker_wait_until(w, wake);
            continue;
        }
        if (now < deadline) {
            /* The lock is released for the spin so a submit from another
             * thread does not block behind it; the generation check is what
             * makes that safe. */
            yrt__worker_unlock(w);
            yrt__spin_until(deadline);
            yrt__worker_lock(w);
            if (w->quit || !w->has_job || w->gen != gen) continue;
        }
        yrt__worker_run(w, false);
    }
    /* Run a pending job immediately rather than waiting out its deadline:
     * yrt_worker_close() is tearing the session down, and work that has to
     * happen (a trailing edge, a cleanup write) is better early than never. */
    if (w->has_job) yrt__worker_run(w, true);
}

/* ======================================================================= *
 *  COMPUTE PUMP
 *
 *  One thread, a fixed ring of messages, one callback per message in submit
 *  order, and an idle callback for the gaps. The same shape as the worker
 *  above and the same private primitives, upside down: the worker exists to
 *  hit a deadline and climbs the scheduling ladder to do it, the pump exists
 *  to finish a long computation without disturbing anything and deliberately
 *  stays at or below normal priority. See PUMP in the manual.
 * ======================================================================= */

/* The pump's only scheduling call. Lowering is optional and a refusal is not
 * an error: the pump runs at normal priority and yrt_pump_policy() says so.
 * There is no path in here that raises. */
static yrt_policy yrt__pump_priority(bool below_normal) {
#if defined(YRT__WINDOWS)
    /* Below normal is about who runs first, not about which core: a
     * minimized process's pump otherwise runs on efficiency cores, where the
     * same computation took about 1.25 times as long, below normal or not
     * (measured; docs/rt.md). */
    (void)yrt__throttle_off(true, YRT__PT_EXEC_SPEED);
#endif
    if (!below_normal) return YRT_POLICY_NORMAL;
#if defined(YRT__WINDOWS)
    if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL))
        return YRT_POLICY_BELOW_NORMAL;
#elif defined(YRT__LINUX)
    /* Linux nice is per TASK, and a task is what the rest of the world calls a
     * thread, so PRIO_PROCESS with the KERNEL THREAD ID reaches this thread
     * alone. PRIO_PROCESS with getpid() would move every thread in the
     * process, the frame loop included, which is the opposite of the point.
     * The id comes from the gettid syscall because glibc had no wrapper for it
     * until 2.30 and this header supports older ones. +5 is one step out of
     * the way, not the bottom: a pump that never runs never finishes. */
    if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5) == 0)
        return YRT_POLICY_BELOW_NORMAL;
#elif defined(YRT__DARWIN)
    /* Mach has no per-thread nice; precedence is the knob, and a negative
     * importance is the whole of "get out of the way". */
    thread_precedence_policy_data_t pp;
    pp.importance = -16;
    if (thread_policy_set(pthread_mach_thread_np(pthread_self()),
                          THREAD_PRECEDENCE_POLICY, (thread_policy_t)&pp,
                          THREAD_PRECEDENCE_POLICY_COUNT) == KERN_SUCCESS)
        return YRT_POLICY_BELOW_NORMAL;
#endif
    return YRT_POLICY_NORMAL;
}

#if defined(YRT__POSIX)

typedef struct yrt__pstate {
    pthread_t       thread;
    pthread_mutex_t mtx;    /* the ring, and the ready handshake            */
    pthread_cond_t  cv;     /* a submit, a stop, or the ready flag          */
    pthread_cond_t  donecv; /* done_seq advanced, or the pump is going away */
    pthread_mutex_t pub;    /* yrt_pump_lock(): the CALLER's, not ours    */
} yrt__pstate;

#else /* YRT__WINDOWS */

/* No waitable timer and no auto-reset event here, unlike the worker's state:
 * the pump waits for work, never for a deadline, so a condition variable is
 * the whole of it. */
typedef struct yrt__pstate {
    CRITICAL_SECTION   cs;
    CONDITION_VARIABLE cv;
    CONDITION_VARIABLE donecv;
    CRITICAL_SECTION   pub;
    HANDLE             thread;
    unsigned short     fpcw;   /* see yrt__fpcw_get()                     */
} yrt__pstate;

#endif

YRT__ALIGN_PROBE(yrt__pstate);
YRT__STATIC_ASSERT(sizeof(yrt__pstate) <= sizeof(((yrt_pump*)0)->os),
                     "yrt_pump.os is too small for this platform's OS objects");
YRT__STATIC_ASSERT(YRT__ALIGNOF(yrt__pstate) <= 8,
                     "yrt_pump.os is not aligned enough for this platform");

static yrt__pstate* yrt__p(yrt_pump* p) {
    return (yrt__pstate*)(void*)&p->os;
}

static bool yrt__pump_on_start(yrt_pump* p) {
    return yrt__run_on_start(p->on_start, p->start_ctx, p->error,
                               sizeof(p->error), "pump");
}

/* ---- the six primitives that differ by platform ----------------------- */
#if defined(YRT__POSIX)

static void yrt__pump_lock_ring(yrt_pump* p)   { pthread_mutex_lock(&yrt__p(p)->mtx); }
static void yrt__pump_unlock_ring(yrt_pump* p) { pthread_mutex_unlock(&yrt__p(p)->mtx); }
static void yrt__pump_wake(yrt_pump* p)        { pthread_cond_signal(&yrt__p(p)->cv); }
static void yrt__pump_wait_work(yrt_pump* p) {
    yrt__pstate* s = yrt__p(p);
    pthread_cond_wait(&s->cv, &s->mtx);
}
static void yrt__pump_done_wake(yrt_pump* p) {
    pthread_cond_broadcast(&yrt__p(p)->donecv);
}
/* Lock held on entry and on return; the caller re-evaluates from the top, so a
 * spurious wake costs one clock read. */
static void yrt__pump_done_wait(yrt_pump* p, uint64_t until_ns) {
    yrt__pstate* s = yrt__p(p);
#if defined(YRT__DARWIN)
    /* Darwin has no pthread_condattr_setclock, so an absolute wait would run
     * against CLOCK_REALTIME and step with the wall clock. */
    uint64_t now = yrt__now();
    struct timespec rel;
    if (until_ns <= now) return;
    yrt__ns_to_ts(until_ns - now, &rel);
    pthread_cond_timedwait_relative_np(&s->donecv, &s->mtx, &rel);
#else
    struct timespec ts;
    yrt__ns_to_ts(until_ns, &ts);
    pthread_cond_timedwait(&s->donecv, &s->mtx, &ts);
#endif
}

#else /* YRT__WINDOWS */

static void yrt__pump_lock_ring(yrt_pump* p)   { EnterCriticalSection(&yrt__p(p)->cs); }
static void yrt__pump_unlock_ring(yrt_pump* p) { LeaveCriticalSection(&yrt__p(p)->cs); }
static void yrt__pump_wake(yrt_pump* p)        { WakeConditionVariable(&yrt__p(p)->cv); }
static void yrt__pump_wait_work(yrt_pump* p) {
    yrt__pstate* s = yrt__p(p);
    SleepConditionVariableCS(&s->cv, &s->cs, INFINITE);
}
static void yrt__pump_done_wake(yrt_pump* p) {
    WakeAllConditionVariable(&yrt__p(p)->donecv);
}
static void yrt__pump_done_wait(yrt_pump* p, uint64_t until_ns) {
    yrt__pstate* s = yrt__p(p);
    uint64_t now = yrt__now();
    DWORD ms = 0;
    if (until_ns > now) {
        /* Rounded UP: a sub-millisecond remainder must still wait, or the
         * caller's loop turns into a spin for the last tick of its timeout. */
        uint64_t d = (until_ns - now + 999999ull) / 1000000ull;
        ms = (d >= INFINITE) ? INFINITE - 1 : (DWORD)d;
    }
    (void)SleepConditionVariableCS(&s->donecv, &s->cs, ms);
}

#endif /* platform */

void yrt_pump_lock(yrt_pump* p) {
    /* A no-op before the first start and after the last stop, so a caller's
     * read-the-results path needs no guard of its own. `locks_live` rather
     * than `running` because the callbacks keep publishing through a drain,
     * which happens after a stop has already cleared `running`. */
    if (!p || !yrt__flag_load(&p->locks_live)) return;
#if defined(YRT__POSIX)
    pthread_mutex_lock(&yrt__p(p)->pub);
#else
    EnterCriticalSection(&yrt__p(p)->pub);
#endif
}

void yrt_pump_unlock(yrt_pump* p) {
    if (!p || !yrt__flag_load(&p->locks_live)) return;
#if defined(YRT__POSIX)
    pthread_mutex_unlock(&yrt__p(p)->pub);
#else
    LeaveCriticalSection(&yrt__p(p)->pub);
#endif
}

bool yrt_pump_is_running(const yrt_pump* p) {
    return p && yrt__flag_load(&p->running) != 0;
}

const char* yrt_pump_error(const yrt_pump* p) {
    return p ? p->error : "null pump handle";
}

yrt_policy yrt_pump_policy(const yrt_pump* p) {
    return p ? p->policy : YRT_POLICY_NONE;
}

uint32_t yrt_pump_done_seq(const yrt_pump* p) {
    /* One atomic load. No lock, no syscall: this is what a frame loop calls. */
    return p ? (uint32_t)yrt__flag_load(&p->done) : 0u;
}

/* The loop is platform-independent; only the primitives above differ. Entered
 * and left with the ring lock held. */
static void yrt__pump_loop(yrt_pump* p) {
    for (;;) {
        /* A stop that DROPS outranks the queue; a stop that drains does not.
         * That one condition is the whole of drop_on_stop. */
        if (p->count > 0 && !(p->quit && p->drop_on_stop)) {
            const void* slot = p->ring + (size_t)p->head * p->msg_size;
            uint32_t seq = p->head_seq;
            /* The slot stays occupied while on_msg reads it in place, so a
             * producer cannot overwrite it; `busy` is what keeps it out of
             * yrt_pump_pending(), which counts what has NOT been handed
             * over yet. */
            p->busy = 1;
            yrt__pump_unlock_ring(p);
            {
                YRT_ZONE(z, "yrt.pump.msg");
                YRT_ZONE_VALUE(z, seq);
                p->on_msg(p->ctx, slot, seq);
                YRT_ZONE_END(z);
            }
            yrt__pump_lock_ring(p);
            p->busy = 0;
            p->head = (p->head + 1u == p->capacity) ? 0u : p->head + 1u;
            p->count--;
            p->head_seq = yrt__next_gen(seq);
            /* Published only now, after the callback RETURNED: a caller that
             * sees this seq must be able to read what the callback published
             * before it returned. */
            yrt__flag_store(&p->done, (int)seq);
            yrt__pump_done_wake(p);
            continue;
        }
        if (p->quit) return;
        if (p->on_idle) {
            bool again;
            yrt__pump_unlock_ring(p);
            {
                YRT_ZONE(z, "yrt.pump.idle");
                again = p->on_idle(p->ctx);
                YRT_ZONE_END(z);
            }
            yrt__pump_lock_ring(p);
            /* Re-check the ring before idling again, so a submit that arrived
             * while on_idle ran is never starved by a callback that always
             * says yes. */
            if (again) continue;
        }
        if (p->count > 0 || p->quit) continue;
        yrt__pump_wait_work(p);
    }
}

int yrt_pump_submit(yrt_pump* p, const void* msg) {
    uint32_t seq;
    if (!p || !msg) return YRT_ERR_ARG;
    if (!yrt__flag_load(&p->running)) return YRT_ERR_STOPPED;
    yrt__pump_lock_ring(p);
    if (p->count >= p->capacity) {
        /* Nothing was copied, so the caller still owns the message. An
         * unbounded queue would trade this visible error for an invisible
         * latency; see PUMP. */
        yrt__pump_unlock_ring(p);
        return YRT_ERR_FULL;
    }
    memcpy(p->ring + (size_t)p->tail * p->msg_size, msg, p->msg_size);
    p->tail = (p->tail + 1u == p->capacity) ? 0u : p->tail + 1u;
    p->seq = yrt__next_gen(p->seq);
    seq = p->seq;
    /* count == 0 means nothing queued AND nothing in flight, so this message
     * is the one the consumer will take next and its seq is the head's. */
    if (p->count == 0) p->head_seq = seq;
    p->count++;
    yrt__pump_wake(p);
    yrt__pump_unlock_ring(p);
    return (int)seq;
}

int yrt_pump_pending(const yrt_pump* p) {
    uint32_t n;
    yrt_pump* m;
    if (!p || !yrt__flag_load(&p->running)) return 0;
    /* The lock is what makes (count, busy) consistent, and taking it is a
     * mutation the const in the signature cannot express: the handle the
     * CALLER sees is unchanged. */
    m = (yrt_pump*)(uintptr_t)p;
    yrt__pump_lock_ring(m);
    n = m->count - (uint32_t)(m->busy ? 1 : 0);
    yrt__pump_unlock_ring(m);
    return (int)n;
}

int yrt_pump_wait(yrt_pump* p, uint32_t seq, int64_t timeout_ns) {
    uint64_t now, until, tmo = yrt__unsigned(timeout_ns);
    int rc = YRT_ERR_TIMEOUT;
    if (!p) return YRT_ERR_ARG;
    /* The lock-free answer first: a seq already passed is the common case and
     * a stopped pump that reached it is still a yes. */
    if ((uint32_t)yrt__flag_load(&p->done) >= seq) return YRT_OK;
    /* Register BEFORE looking at whether the mutexes exist, and look with a
     * sequentially consistent load. yrt__pump_close_door() does the mirror
     * image (clear locks_live, then read waiters), so either this wait sees
     * the door closed and never touches a mutex, or the stop sees this wait
     * counted and destroys nothing until it leaves. Checking first and
     * registering second is the race this replaces. */
    (void)yrt__sc_add(&p->waiters, 1);
    if (!yrt__sc_load(&p->locks_live)) {
        rc = ((uint32_t)yrt__flag_load(&p->done) >= seq) ? YRT_OK
                                                          : YRT_ERR_STOPPED;
        (void)yrt__sc_add(&p->waiters, -1);
        return rc;
    }
    now = yrt__now();
    until = (tmo > UINT64_MAX - now) ? UINT64_MAX : now + tmo;
    yrt__pump_lock_ring(p);
    for (;;) {
        uint64_t slice;
        if ((uint32_t)yrt__flag_load(&p->done) >= seq) { rc = YRT_OK; break; }
        /* `exited`, not `running`: a draining stop has already refused new
         * submits but is still delivering the queue, and a seq in that queue
         * is still going to be reached. */
        if (p->exited) { rc = YRT_ERR_STOPPED; break; }
        now = yrt__now();
        if (now >= until) { rc = YRT_ERR_TIMEOUT; break; }
        /* One second at a time, and the loop re-checks: a caller who passes an
         * enormous timeout as a stand-in for "forever" must not land on an
         * absolute timespec the OS rejects, which would turn this wait into a
         * spin. One extra wake per second of waiting is the whole cost. */
        slice = now + 1000000000ull;
        if (slice > until) slice = until;
        yrt__pump_done_wait(p, slice);
    }
    yrt__pump_unlock_ring(p);
    /* Last, after the unlock: the moment this reaches zero the stop may
     * destroy the mutex just released. */
    (void)yrt__sc_add(&p->waiters, -1);
    return rc;
}

/* Validate the desc and lay the ring out. Split from the platform starts
 * because it is the same work on both and it is all that can fail before an
 * OS object exists. */
static bool yrt__pump_setup(yrt_pump* p, const yrt_pump_desc* d) {
    if (!d->on_msg || d->msg_size == 0 || d->capacity == 0) {
        snprintf(p->error, sizeof(p->error),
                 "pump: desc.on_msg, desc.msg_size and desc.capacity are all "
                 "required (got %s, %lu, %lu)",
                 d->on_msg ? "on_msg" : "no on_msg",
                 (unsigned long)d->msg_size, (unsigned long)d->capacity);
        return false;
    }
    if (d->ring) {
        /* The size of a buffer the caller owns cannot be checked from here;
         * the declaration says so. */
        p->ring = (unsigned char*)d->ring;
    } else {
        /* Divided rather than multiplied: msg_size * capacity can overflow,
         * and this test is the only thing between a large desc and a write
         * past the inline buffer. */
        if (d->msg_size > sizeof(p->inline_ring) / d->capacity) {
            snprintf(p->error, sizeof(p->error),
                     "pump: desc asks for %lu x %lu bytes of ring and the "
                     "inline buffer is %lu; pass desc.ring or raise "
                     "YRT_PUMP_INLINE_BYTES",
                     (unsigned long)d->capacity, (unsigned long)d->msg_size,
                     (unsigned long)sizeof(p->inline_ring));
            return false;
        }
        p->ring = p->inline_ring;
    }
    p->msg_size     = d->msg_size;
    p->capacity     = d->capacity;
    p->on_msg       = d->on_msg;
    p->on_idle      = d->on_idle;
    p->ctx          = d->ctx;
    p->on_start     = d->on_start;
    p->start_ctx    = d->start_ctx;
    p->below_normal = d->below_normal;
    p->pin_cpu      = d->pin_cpu;
    p->drop_on_stop = d->drop_on_stop ? 1 : 0;
    p->policy       = YRT_POLICY_NONE;
    return true;
}

/* Tell every waiter the pump thread is gone, whether it ran or never did. A
 * waiter checks `exited` under the ring mutex before it parks, so after this
 * nothing can park, and anything already parked is woken. */
static void yrt__pump_mark_exited(yrt_pump* p) {
    yrt__pump_lock_ring(p);
    p->exited = 1;
    yrt__pump_done_wake(p);
    yrt__pump_unlock_ring(p);
}

/* Close the door on new waiters and let the registered ones out, so the
 * caller may destroy the mutexes. Must follow yrt__pump_mark_exited(), or a
 * registered waiter could park with nothing left to wake it.
 *
 * The store to locks_live and the load of waiters are both sequentially
 * consistent, mirroring the registration in yrt_pump_wait(): of the two
 * store-then-load pairs, at least one sees the other's store. A waiter that
 * arrives later sees the door closed and backs out touching only the atomic
 * fields at the head of the handle. */
static void yrt__pump_close_door(yrt_pump* p) {
    yrt__sc_store(&p->locks_live, 0);
    while (yrt__sc_load(&p->waiters) > 0) {
        /* A waiter between its registration and its first lock still finds
         * the mutex alive, sees `exited` and leaves; this broadcast is for one
         * that parked before the pump thread's own. Not a timing path. */
        yrt__pump_lock_ring(p);
        yrt__pump_done_wake(p);
        yrt__pump_unlock_ring(p);
        (void)yrt__sleep_until(yrt__now() + 100000ull, 0);
    }
}

/* Clear everything but the atomic door at the head of the handle. A
 * yrt_pump_wait() racing a restart may be touching those fields right now,
 * so a plain memset of them would be a data race even though the values would
 * already be what start wants; `done` is reset with an atomic store instead.
 * The rest is only ever touched under the mutexes or by the pump thread, and
 * start runs when neither exists. */
static void yrt__pump_reset(yrt_pump* p) {
    size_t head = offsetof(yrt_pump, ring);
    memset((unsigned char*)p + head, 0, sizeof(*p) - head);
    yrt__sc_store(&p->done, 0);
}

#if defined(YRT__POSIX)

static void* yrt__pump_main(void* arg) {
    yrt_pump* p = (yrt_pump*)arg;
    yrt_policy pol = yrt__pump_priority(p->below_normal);
    bool ok;
    if (p->pin_cpu > 0) (void)yrt_thread_pin(p->pin_cpu);
    YRT_THREAD_INIT("yrt pump");
    /* After the priority and the pin, so a hook sees the thread it will run
     * on as it will be, and before the handshake, so a refusal fails the
     * start instead of a message. */
    ok = yrt__pump_on_start(p);
    yrt__pump_lock_ring(p);
    p->policy = ok ? pol : YRT_POLICY_NONE;
    p->start_failed = ok ? 0 : 1;
    p->ready = 1;
    pthread_cond_broadcast(&yrt__p(p)->cv);
    if (ok) yrt__pump_loop(p);
    /* Still under the lock: a waiter checks this before it parks, so none can
     * park after it and every one parked now is woken. */
    p->exited = 1;
    yrt__pump_done_wake(p);
    yrt__pump_unlock_ring(p);
    return NULL;
}

bool yrt_pump_open(yrt_pump* p, const yrt_pump_desc* desc) {
    yrt_pump_desc d;
    yrt__pstate* s;
    pthread_mutexattr_t ma;
    pthread_condattr_t ca;
    int rc_mtx, rc_pub, rc_cv, rc_done, rc_thread, refused;
    if (!p) return false;
    memset(&d, 0, sizeof(d));
    if (desc) d = *desc;
    if (yrt__flag_load(&p->running)) return true;

    yrt__pump_reset(p);
    if (!yrt__pump_setup(p, &d)) return false;

    s = yrt__p(p);
    /* Priority inheritance on BOTH mutexes, and the inversion runs the other
     * way round from the worker's: the pump is the LOW-priority thread here,
     * so without PI a frame loop that calls yrt_pump_submit() or
     * yrt_pump_lock() would wait for a preempted pump to be scheduled again
     * before it could have the lock back. */
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    rc_mtx = pthread_mutex_init(&s->mtx, &ma);
    rc_pub = rc_mtx ? 0 : pthread_mutex_init(&s->pub, &ma);
    pthread_mutexattr_destroy(&ma);
    if (rc_mtx != 0 || rc_pub != 0) {
        if (rc_mtx == 0) pthread_mutex_destroy(&s->mtx);
        snprintf(p->error, sizeof(p->error), "pump: mutex init: %s",
                 strerror(rc_mtx ? rc_mtx : rc_pub));
        return false;
    }
    pthread_condattr_init(&ca);
#if !defined(YRT__DARWIN)
    /* Absolute timed waits run against the clock yrt_pump_wait()'s timeout
     * is measured in. */
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
#endif
    rc_cv   = pthread_cond_init(&s->cv, &ca);
    rc_done = rc_cv ? 0 : pthread_cond_init(&s->donecv, &ca);
    pthread_condattr_destroy(&ca);
    if (rc_cv != 0 || rc_done != 0) {
        if (rc_cv == 0) pthread_cond_destroy(&s->cv);
        pthread_mutex_destroy(&s->pub);
        pthread_mutex_destroy(&s->mtx);
        snprintf(p->error, sizeof(p->error), "pump: cond init: %s",
                 strerror(rc_cv ? rc_cv : rc_done));
        return false;
    }
    /* Before the thread exists, because on_idle may call yrt_pump_lock() on
     * its very first invocation, which can happen before this function
     * returns. Sequentially consistent because it also opens the door to
     * waiters; see yrt__pump_close_door(). */
    yrt__sc_store(&p->locks_live, 1);
    /* pthread_create returns the error; it does not set errno. */
    rc_thread = pthread_create(&s->thread, NULL, yrt__pump_main, p);
    if (rc_thread != 0) {
        /* No thread will ever set `exited`, so do it here: a wait that got in
         * through the open door must not be left parked. */
        yrt__pump_mark_exited(p);
        yrt__pump_close_door(p);
        pthread_cond_destroy(&s->donecv);
        pthread_cond_destroy(&s->cv);
        pthread_mutex_destroy(&s->pub);
        pthread_mutex_destroy(&s->mtx);
        snprintf(p->error, sizeof(p->error), "pump: thread create: %s%s",
                 strerror(rc_thread), YRT__THREAD_HINT);
        return false;
    }
    pthread_mutex_lock(&s->mtx);
    while (!p->ready) pthread_cond_wait(&s->cv, &s->mtx);
    refused = p->start_failed;
    pthread_mutex_unlock(&s->mtx);
    if (refused) {
        /* The thread is on its way out and never set `running`, so nothing can
         * be in the lock; join it before the objects it used go away.
         * p->error already holds the hook's message. */
        pthread_join(s->thread, NULL);   /* the thread set `exited` */
        yrt__pump_close_door(p);
        pthread_cond_destroy(&s->donecv);
        pthread_cond_destroy(&s->cv);
        pthread_mutex_destroy(&s->pub);
        pthread_mutex_destroy(&s->mtx);
        return false;
    }
    yrt__flag_store(&p->running, 1);
    return true;
}

void yrt_pump_close(yrt_pump* p) {
    yrt__pstate* s;
    if (!p || !yrt__flag_load(&p->running)) return;
    s = yrt__p(p);
    /* Clear `running` first: no submit is accepted from here on, and a submit
     * racing this teardown must be refused rather than reach for a mutex that
     * is about to be destroyed. The queue is still delivered; `running` is
     * about the door, not about the work. */
    yrt__flag_store(&p->running, 0);
    pthread_mutex_lock(&s->mtx);
    p->quit = 1;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mtx);
    /* The thread sets `exited` and wakes every waiter on its way out, after
     * the drain, so a wait for a seq still in the queue gets its 0. */
    pthread_join(s->thread, NULL);
    /* After the join: no callback is running, so the publish mutex has no user
     * left, and closing the door both makes yrt_pump_lock() the documented
     * no-op and waits out every yrt_pump_wait() that got in. Only then is
     * anything destroyed. */
    yrt__pump_close_door(p);
    pthread_cond_destroy(&s->donecv);
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->pub);
    pthread_mutex_destroy(&s->mtx);
    p->policy = YRT_POLICY_NONE;
}

#else /* YRT__WINDOWS */

static DWORD yrt__pump_thread(yrt_pump* p);

static DWORD WINAPI yrt__pump_main(LPVOID arg) {
    /* First, before anything computes on this thread; see yrt__fpcw_get(). */
    yrt__fpcw_set(yrt__p((yrt_pump*)arg)->fpcw);
    return yrt__pump_thread((yrt_pump*)arg);
}

static DWORD yrt__pump_thread(yrt_pump* p) {
    yrt_policy pol = yrt__pump_priority(p->below_normal);
    bool ok;
    if (p->pin_cpu > 0) (void)yrt_thread_pin(p->pin_cpu);
    YRT_THREAD_INIT("yrt pump");
    /* See the POSIX twin: after the priority and the pin, before the
     * handshake. */
    ok = yrt__pump_on_start(p);
    yrt__pump_lock_ring(p);
    p->policy = ok ? pol : YRT_POLICY_NONE;
    p->start_failed = ok ? 0 : 1;
    p->ready = 1;
    WakeAllConditionVariable(&yrt__p(p)->cv);
    if (ok) yrt__pump_loop(p);
    /* See the POSIX twin. */
    p->exited = 1;
    yrt__pump_done_wake(p);
    yrt__pump_unlock_ring(p);
    return 0;
}

bool yrt_pump_open(yrt_pump* p, const yrt_pump_desc* desc) {
    yrt_pump_desc d;
    yrt__pstate* s;
    int refused;
    if (!p) return false;
    memset(&d, 0, sizeof(d));
    if (desc) d = *desc;
    if (yrt__flag_load(&p->running)) return true;

    yrt__pump_reset(p);
    if (!yrt__pump_setup(p, &d)) return false;

    s = yrt__p(p);
    /* Critical sections and condition variables cannot fail to initialize and
     * need no handle, so there is nothing to unwind before CreateThread. */
    InitializeCriticalSection(&s->cs);
    InitializeCriticalSection(&s->pub);
    InitializeConditionVariable(&s->cv);
    InitializeConditionVariable(&s->donecv);
    /* Before the thread exists; see the POSIX twin. */
    yrt__sc_store(&p->locks_live, 1);
    s->fpcw = yrt__fpcw_get();
    s->thread = CreateThread(NULL, 0, yrt__pump_main, p, 0, NULL);
    if (!s->thread) {
        DWORD err = GetLastError();
        yrt__pump_mark_exited(p);
        yrt__pump_close_door(p);
        snprintf(p->error, sizeof(p->error), "pump: thread create failed (err %lu)",
                 (unsigned long)err);
        DeleteCriticalSection(&s->pub);
        DeleteCriticalSection(&s->cs);
        return false;
    }
    EnterCriticalSection(&s->cs);
    while (!p->ready) SleepConditionVariableCS(&s->cv, &s->cs, INFINITE);
    refused = p->start_failed;
    LeaveCriticalSection(&s->cs);
    if (refused) {
        /* See the POSIX twin: join before the objects go away. p->error
         * already holds the hook's message. */
        WaitForSingleObject(s->thread, INFINITE);   /* it set `exited` */
        CloseHandle(s->thread);
        yrt__pump_close_door(p);
        DeleteCriticalSection(&s->pub);
        DeleteCriticalSection(&s->cs);
        return false;
    }
    yrt__flag_store(&p->running, 1);
    return true;
}

void yrt_pump_close(yrt_pump* p) {
    yrt__pstate* s;
    if (!p || !yrt__flag_load(&p->running)) return;
    s = yrt__p(p);
    /* Clear `running` first; see the POSIX twin. */
    yrt__flag_store(&p->running, 0);
    EnterCriticalSection(&s->cs);
    p->quit = 1;
    WakeAllConditionVariable(&s->cv);
    LeaveCriticalSection(&s->cs);
    /* See the POSIX twin: the thread marks `exited` after the drain, the join
     * waits for that, and close_door waits out every registered wait before
     * anything is deleted. */
    WaitForSingleObject(s->thread, INFINITE);
    CloseHandle(s->thread);
    yrt__pump_close_door(p);
    DeleteCriticalSection(&s->pub);
    DeleteCriticalSection(&s->cs);
    p->policy = YRT_POLICY_NONE;
}

#endif /* platform */

#endif /* YRT_NO_THREADS */

#endif /* YSP_RT_IMPLEMENTATION_GUARD */
#endif /* YSP_RT_IMPLEMENTATION */

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
