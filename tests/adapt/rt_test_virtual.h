/* rt_test_virtual.h - the virtual clock of ysp/rt.h (VIRTUAL CLOCK),
 * included by rt_test.c after its harness. The test needs real waits while
 * the clock is virtual, so it uses the implementation's own yrt__now() and
 * yrt__sleep_until() (the same translation unit). */

/* Every call of a timer, in order: which timer, and the clock it saw. */
typedef struct vt_log {
    int     who[32];
    int64_t at[32];
    int     n;
} vt_log;
static vt_log g_vt;

typedef struct vt_timer {
    int     id, slot, cancel_slot, budget;
    int64_t step;           /* the next call this much later; 0 ends it */
} vt_timer;

static int64_t vt_fn(void* ctx, int64_t now) {
    vt_timer* t = (vt_timer*)ctx;
    if (g_vt.n < 32) { g_vt.who[g_vt.n] = t->id; g_vt.at[g_vt.n] = now; g_vt.n++; }
    CHECK(yrt_now_ns() == now, "a timer sees the clock at its own time");
    if (t->cancel_slot >= 0) { yrt_virtual_cancel(t->cancel_slot); t->cancel_slot = -1; }
    if (t->budget > 0 && --t->budget == 0) return 0;
    return t->step ? now + t->step : 0;
}

/* A timer that waits, which the manual forbids: the wait moves the clock
 * and runs no timer, so it cannot recurse into itself. */
static int64_t vt_waits(void* ctx, int64_t now) {
    int64_t* seen = (int64_t*)ctx;
    (void)yrt_sleep_until(now + YRT_MS(1), 0);
    *seen = yrt_now_ns();
    return now + YRT_MS(5);
}

#if !defined(YRT_NO_THREADS)
/* The flags are release stores and acquire loads (the ring's atomics), so
 * the fields written before a flag are visible after it (ThreadSanitizer). */
typedef struct vt_job {
    uint32_t ran, in_job, done;
    int64_t at, deadline, woke, wait_to, lateness;
} vt_job;

static void vt_job_fn(void* ctx, const yrt_job_info* info) {
    vt_job* j = (vt_job*)ctx;
    j->at = info->at_ns;
    j->deadline = info->deadline_ns;
    yrt__rstore(&j->ran, 1u);
    if (j->wait_to) {
        yrt__rstore(&j->in_job, 1u);
        j->lateness = yrt_sleep_until(j->wait_to, 0);   /* not the driver: blocks */
        j->woke = yrt_now_ns();
        yrt__rstore(&j->done, 1u);
    }
}

/* Real time, while the clock is virtual: the test's own polls. */
static int vt_real_wait(uint32_t* flag, int64_t ms) {
    uint64_t end = yrt__now() + (uint64_t)ms * 1000000u;
    while (!yrt__rload(flag) && yrt__now() < end) (void)yrt__sleep_until(yrt__now() + 200000u, 0);
    return yrt__rload(flag) != 0;
}
#endif

static void test_virtual(void) {
    vt_timer a, b, c, d;
    int64_t t0, t;
    int i, ids[YRT_VIRTUAL_TIMERS];
    memset(&g_vt, 0, sizeof g_vt);

    CHECK(!yrt_is_virtual(), "the clock starts real");
    CHECK_I(yrt_virtual_timer(vt_fn, &a, 0), YRT_ERR_STATE, "no timer on the real clock");
    CHECK(strcmp(yrt_strerror(YRT_ERR_STATE), "unknown error") != 0, "YRT_ERR_STATE has a text");
    yrt_virtual_end();   /* safe when real */
    CHECK(!yrt_is_virtual(), "end on the real clock changes nothing");

    CHECK_I(yrt_virtual_begin(0), 0, "begin");
    CHECK(yrt_is_virtual(), "virtual after begin");
    CHECK_I(yrt_virtual_begin(5), YRT_ERR_STATE, "a second begin is refused");
    t0 = yrt_now_ns();
    CHECK(t0 == YRT_VIRTUAL_T0_NS, "0 starts at YRT_VIRTUAL_T0_NS");
    {   /* the real read moves while the virtual clock stands still */
        int64_t r0 = yrt_real_ns(), r1;
        (void)yrt__sleep_until(yrt__now() + 2000000u, 0);   /* 2 ms of real time */
        r1 = yrt_real_ns();
        CHECK(r1 - r0 >= 2000000 && r1 - r0 < 2000000000, "yrt_real_ns() is the real clock on the virtual one");
        CHECK(llabs(r1 - (int64_t)yrt__now()) < YRT_MS(100), "yrt_real_ns() reads the counter yrt_now_ns() reads when real");
    }
    CHECK(yrt_now_ns() == t0, "the clock stands still between waits");
    {   /* a driver's wait jumps virtual time, not real time */
        int64_t r0 = yrt_real_ns();
        (void)yrt_sleep_until(t0 + YRT_S(10), 0);
        CHECK(yrt_real_ns() - r0 < YRT_S(1), "a 10 s virtual wait takes no real time");
        yrt_virtual_end();
        CHECK_I(yrt_virtual_begin(t0), 0, "begin at a given time");
        CHECK(yrt_now_ns() == t0, "t0_ns is the start");
    }

    /* the driver's waits jump the clock and are never late */
    CHECK_I(yrt_sleep_until(t0 + YRT_MS(5), YRT_DEFAULT_SPIN_NS), 0, "sleep_until is on time");
    CHECK(yrt_now_ns() == t0 + YRT_MS(5), "sleep_until moves the clock to its deadline");
    CHECK(yrt_sleep_until(t0 + YRT_MS(2), 0) == YRT_MS(3), "a past deadline returns its lateness");
    CHECK(yrt_now_ns() == t0 + YRT_MS(5), "a past deadline does not move the clock back");
    CHECK_I(yrt_sleep_ns(YRT_MS(1)), 0, "sleep_ns is on time");
    CHECK_I(yrt_spin_until(t0 + YRT_MS(7)), 0, "spin_until is on time");
    CHECK(yrt_now_ns() == t0 + YRT_MS(7), "sleep_ns and spin_until move the clock");

    /* timers: time order, ties in the order they were set */
    t = yrt_now_ns();
    memset(&a, 0, sizeof a); a.id = 1; a.step = YRT_MS(10); a.cancel_slot = -1;
    memset(&b, 0, sizeof b); b.id = 2; b.cancel_slot = -1;                         /* one shot */
    memset(&c, 0, sizeof c); c.id = 3; c.step = YRT_MS(10); c.cancel_slot = -1; c.budget = 2;
    a.slot = yrt_virtual_timer(vt_fn, &a, t + YRT_MS(10));
    b.slot = yrt_virtual_timer(vt_fn, &b, t + YRT_MS(15));
    c.slot = yrt_virtual_timer(vt_fn, &c, t + YRT_MS(10));
    CHECK(a.slot >= 0 && b.slot >= 0 && c.slot >= 0 && a.slot != b.slot && b.slot != c.slot, "three timers");
    CHECK_I(yrt_virtual_timer(NULL, &a, t), YRT_ERR_ARG, "a NULL fn");
    CHECK_I(yrt_sleep_until(t + YRT_MS(9), 0), 0, "a wait short of every timer");
    CHECK_I(g_vt.n, 0, "no timer before its time");
    (void)yrt_sleep_until(t + YRT_MS(35), 0);
    CHECK(yrt_now_ns() == t + YRT_MS(35), "the clock ends at the wait's deadline");
    {
        static const int want_who[] = { 1, 3, 2, 1, 3, 1 };
        static const int want_ms[]  = { 10, 10, 15, 20, 20, 30 };
        CHECK_I(g_vt.n, 6, "six calls by 35 ms");
        for (i = 0; i < 6 && i < g_vt.n; i++) {
            CHECK_I(g_vt.who[i], want_who[i], "the timer called");
            CHECK(g_vt.at[i] == t + YRT_MS(want_ms[i]), "the time of a call");
        }
    }
    /* c ended after its budget, b after its one call: their slots are free */
    yrt_virtual_cancel(a.slot);
    yrt_virtual_cancel(a.slot);   /* safe twice */
    g_vt.n = 0;
    (void)yrt_sleep_until(t + YRT_MS(60), 0);
    CHECK_I(g_vt.n, 0, "a canceled timer is not called");

    /* a timer that cancels another one due at the same time */
    t = yrt_now_ns();
    memset(&a, 0, sizeof a); a.id = 4; a.cancel_slot = -1;
    memset(&d, 0, sizeof d); d.id = 5; d.cancel_slot = -1;
    a.slot = yrt_virtual_timer(vt_fn, &a, t + YRT_MS(1));
    d.slot = yrt_virtual_timer(vt_fn, &d, t + YRT_MS(1));
    a.cancel_slot = d.slot;
    (void)yrt_sleep_until(t + YRT_MS(2), 0);
    CHECK(g_vt.n == 1 && g_vt.who[0] == 4, "a timer canceled by an earlier one is not called");

    {   /* a timer that waits does not recurse */
        int64_t seen = 0;
        int id;
        t = yrt_now_ns();
        id = yrt_virtual_timer(vt_waits, &seen, t + YRT_MS(1));
        CHECK(id >= 0, "a timer that waits");
        (void)yrt_sleep_until(t + YRT_MS(3), 0);
        CHECK(seen == t + YRT_MS(2) && yrt_now_ns() == t + YRT_MS(3), "a wait in a timer moves the clock, once");
        yrt_virtual_cancel(id);
    }

    /* a full table */
    for (i = 0; i < YRT_VIRTUAL_TIMERS; i++) ids[i] = yrt_virtual_timer(vt_fn, &b, t + YRT_S(100));
    CHECK(ids[YRT_VIRTUAL_TIMERS - 1] >= 0, "YRT_VIRTUAL_TIMERS timers at once");
    CHECK_I(yrt_virtual_timer(vt_fn, &b, t + YRT_S(100)), YRT_ERR_FULL, "one more is refused");
    for (i = 0; i < YRT_VIRTUAL_TIMERS; i++) yrt_virtual_cancel(ids[i]);

    {   /* an event ring stamp is virtual */
        static unsigned char mem[YRT_RING_BYTES(8)];
        yrt_ring r;
        yrt_ring_desc rd;
        yrt_event ev, out[4];
        memset(&r, 0, sizeof r);
        memset(&rd, 0, sizeof rd);
        rd.memory = mem;
        rd.bytes = sizeof mem;
        CHECK(yrt_ring_open(&r, &rd), "a ring");
        memset(&ev, 0, sizeof ev);
        CHECK(yrt_ring_push(&r, &ev) >= 0, "a push");
        CHECK(yrt_ring_drain(&r, out, 4) == 1 && out[0].t_ns == yrt_now_ns(), "t_ns 0 is the virtual time");
        yrt_ring_close(&r);
    }

#if !defined(YRT_NO_THREADS)
    {   /* a worker job runs once the driver passes its deadline, and a wait
         * on another thread blocks until the driver gets there */
        yrt_worker w;
        yrt_worker_desc wd;
        static vt_job j;
        memset(&w, 0, sizeof w);
        memset(&wd, 0, sizeof wd);
        memset(&j, 0, sizeof j);
        wd.no_elevate = true;
        CHECK(yrt_worker_open(&w, &wd), "a worker on the virtual clock");
        t = yrt_now_ns();
        j.wait_to = t + YRT_MS(80);
        CHECK(yrt_worker_submit(&w, t + YRT_MS(50), vt_job_fn, &j) > 0, "submit");
        CHECK(!vt_real_wait(&j.ran, 30), "no job before the clock reaches its deadline (30 ms real)");
        (void)yrt_sleep_until(t + YRT_MS(60), 0);
        CHECK(vt_real_wait(&j.in_job, 2000), "the job runs once the clock passed its deadline");
        CHECK(j.deadline == t + YRT_MS(50) && j.at == t + YRT_MS(60), "the job's deadline and at_ns are virtual");
        CHECK(!vt_real_wait(&j.done, 30), "the job's wait blocks while the clock stands still (30 ms real)");
        (void)yrt_sleep_until(t + YRT_MS(80), 0);
        CHECK(vt_real_wait(&j.done, 2000), "the job's wait ends once the driver reaches it");
        CHECK(j.woke == t + YRT_MS(80) && j.lateness == 0, "the other thread woke at its deadline");
        yrt_worker_close(&w);
    }
#endif

    yrt_virtual_end();
    CHECK(!yrt_is_virtual(), "real after end");
    t = yrt_now_ns();
    CHECK(llabs(t - (int64_t)yrt__now()) < YRT_MS(100), "yrt_now_ns() is the real clock again");
    CHECK_I(yrt_virtual_timer(vt_fn, &a, t), YRT_ERR_STATE, "no timer after end");
    printf("  virtual clock: ok\n");
}
