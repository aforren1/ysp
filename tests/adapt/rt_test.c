/* rt_test.c - self-checking test for yrt_pump, the event ring, the
 * trace macros, the clock correlation, the device clock fit
 * (rt_test_fit.h) and the per-user folders (rt_test_userdir.h) in
 * ysp/rt.h. No framework: it returns 0 when every check passed and 1 after
 * printing each failure. With YRT_TEST_FIT_ONLY it runs the fit and the
 * folders only, as tests/mutate/rt.toml does.
 *
 * The ring's stress test runs 2, 4 and 8 producers on deadline workers into
 * a 256-slot ring against one draining thread, and checks every record and
 * every push's outcome exactly: no torn record, no duplicate, each
 * producer's order, no seq gap, drained + refused = pushed, and the LOSS
 * records summing to the refusals. Run it under ThreadSanitizer too.
 *
 *     gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -pthread -Iinclude \
 *         -o pump_test tests/adapt/rt_test.c
 *     gcc ... -fsanitize=address,undefined ...
 *     gcc ... -fsanitize=thread ...
 *     cl /nologo /O2 /W4 /WX /Iinclude tests\adapt\rt_test.c
 *     emcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude \
 *         -pthread -sPROXY_TO_PTHREAD=1 -sEXIT_RUNTIME=1 \
 *         -o rt_test.js tests/adapt/rt_test.c && node rt_test.js
 *
 * Under Emscripten the below-normal check demands NORMAL (there is no
 * priority to lower), test_platform() checks the collapsed ladder, and
 * everything else, the hammer included, is the same test. A build without -pthread cannot start a thread and fails the first
 * start; that is the platform, not the pump.
 *
 * The ThreadSanitizer run is the point of the publish-lock check: the pump
 * writes a struct while the main thread reads it, several thousand times, and
 * the only thing between them is yrt_pump_lock(). A race there is a bug in
 * the header, not in the test. The same goes for the wait-versus-stop hammer:
 * two threads call yrt_pump_wait() in a loop while the main thread stops and
 * restarts the pump 300 times, which is the guarantee v0.3.1 makes. A copy of
 * the header whose stop skips the waiter drain fails that check under TSan.
 * On Linux kernels with high mmap entropy run the TSan binary under
 * `setarch $(uname -m) -R`, or TSan aborts before main.
 *
 * It lives in tests/adapt/ rather than tests/compile/ because the pump exists
 * for the adaptive-method headers (docs/adapt.md, "Inference on a thread")
 * and because it is a behavior test, not a compile check.
 *
 * Timing: the test spends about 1.5 s in deliberate sleeps, which is how it
 * gets deterministic answers out of a thread it does not control. Every margin
 * is at least a factor of ten, so a sanitizer's slowdown does not reach them.
 */
/* The whole test runs in trace-ring mode, so the zones ysp/rt.h puts in its
 * own worker and pump run here too, under the sanitizers. With no trace ring
 * attached (every test but test_trace) they cost a pointer load each. */
#define YRT_TRACE_RING
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- harness */

static int g_failures = 0;

static void fail(int line, const char* what) {
    fprintf(stderr, "ysp_rt_pump_test: FAIL at line %d: %s\n", line, what);
    g_failures++;
}

static void fail_i(int line, const char* what, long got, long want) {
    fprintf(stderr, "ysp_rt_pump_test: FAIL at line %d: %s (got %ld, want %ld)\n",
            line, what, got, want);
    g_failures++;
}

#define CHECK(cond, what)     do { if (!(cond)) fail(__LINE__, (what)); } while (0)
#define CHECK_I(got, want, what) do { long g_ = (long)(got), w_ = (long)(want); \
        if (g_ != w_) fail_i(__LINE__, (what), g_, w_); } while (0)

/* A pure OS wait, no spin window: the test sleeps for hundreds of
 * milliseconds at a time and must not burn a core doing it. */
static void nap_ms(unsigned ms) {
    (void)yrt_sleep_until(yrt_now_ns() + (int64_t)ms * 1000000, 0);
}

/* Submit, treating a full ring as back pressure rather than an error, which is
 * what a caller that has nothing else to do with the message does. */
static int submit_blocking(yrt_pump* p, const void* m) {
    int i;
    for (i = 0; i < 20000; i++) {
        int rc = yrt_pump_submit(p, m);
        if (rc != YRT_ERR_FULL) return rc;
        nap_ms(1);
    }
    return YRT_ERR_FULL;
}

/* ------------------------------------------------ order, seq, done_seq, wait */

#define ORDER_N 200

typedef struct order_msg {
    uint32_t tag;
    uint32_t pad[3];   /* a message wider than a word, so a short copy shows */
} order_msg;

typedef struct order_ctx {
    uint32_t seen_seq[ORDER_N];
    uint32_t seen_tag[ORDER_N];
    int      n;
    int      out_of_order;
    int      concurrent;   /* two on_msg calls overlapping: must never happen */
    int      in_msg;
    uint32_t last_seq;
} order_ctx;

static void order_on_msg(void* ctx, const void* msg, uint32_t seq) {
    order_ctx* o = (order_ctx*)ctx;
    const order_msg* m = (const order_msg*)msg;
    if (o->in_msg) o->concurrent++;
    o->in_msg = 1;
    if (seq != o->last_seq + 1u) o->out_of_order++;
    o->last_seq = seq;
    if (o->n < ORDER_N) {
        o->seen_seq[o->n] = seq;
        o->seen_tag[o->n] = m->tag;
        o->n++;
    }
    o->in_msg = 0;
}

/* One pass of the order test, so the inline ring and a caller-owned ring are
 * checked by the same code. `ring` NULL asks for the inline buffer. */
static void test_order(void* ring, const char* label) {
    yrt_pump p;
    yrt_pump_desc d;
    static order_ctx o;   /* static: 1.6 KB of arrays, and the pump thread
                           * writes it while main only reads it after the join */
    int i, rc = 0;
    memset(&o, 0, sizeof(o));
    memset(&p, 0, sizeof(p));
    memset(&d, 0, sizeof(d));
    d.msg_size = sizeof(order_msg);
    d.capacity = 8;
    d.ring     = ring;
    d.on_msg   = order_on_msg;
    d.ctx      = &o;
    if (!yrt_pump_open(&p, &d)) {
        fail(__LINE__, yrt_pump_error(&p));
        printf("  (%s)\n", label);
        return;
    }
    CHECK(yrt_pump_is_running(&p), "pump should be running after start");
    CHECK_I(yrt_pump_done_seq(&p), 0, "done_seq before any message");

    for (i = 0; i < ORDER_N; i++) {
        order_msg m;
        memset(&m, 0, sizeof(m));
        m.tag = (uint32_t)(i * 3 + 1);
        rc = submit_blocking(&p, &m);
        if (rc < 0) { fail(__LINE__, yrt_strerror(rc)); break; }
        CHECK_I(rc, i + 1, "submit should return a seq of 1, 2, 3, ...");
    }
    /* wait and done_seq must agree about the last message. */
    CHECK_I(yrt_pump_wait(&p, (uint32_t)rc, 5000000000LL), YRT_OK,
            "wait for the last seq");
    CHECK(yrt_pump_done_seq(&p) >= (uint32_t)rc,
          "done_seq must have passed the seq wait returned for");
    CHECK_I(yrt_pump_pending(&p), 0, "nothing should be pending after the wait");
    /* A seq that will never be reached must time out, not hang or lie. */
    CHECK_I(yrt_pump_wait(&p, (uint32_t)rc + 1000, 20000000LL),
            YRT_ERR_TIMEOUT, "wait for a seq never submitted");
    yrt_pump_close(&p);

    CHECK_I(o.n, ORDER_N, "every message must reach on_msg");
    CHECK_I(o.out_of_order, 0, "on_msg must see seq in submit order");
    CHECK_I(o.concurrent, 0, "on_msg must never run concurrently with itself");
    for (i = 0; i < o.n; i++) {
        if (o.seen_seq[i] != (uint32_t)(i + 1)) {
            fail_i(__LINE__, "seq at index", (long)o.seen_seq[i], (long)(i + 1));
            break;
        }
        if (o.seen_tag[i] != (uint32_t)(i * 3 + 1)) {
            fail_i(__LINE__, "payload at index", (long)o.seen_tag[i],
                   (long)(i * 3 + 1));
            break;
        }
    }
    printf("  order/seq/payload over %d messages, %s ring: ok\n", o.n, label);
}

/* --------------------------------------------------------------- ERR_FULL */

typedef struct slow_ctx {
    uint64_t hold_ns;    /* how long on_msg holds its slot                  */
    uint32_t hold_seq;   /* only this seq is held; 0 holds every one        */
    int      delivered;
} slow_ctx;

static void slow_on_msg(void* ctx, const void* msg, uint32_t seq) {
    slow_ctx* s = (slow_ctx*)ctx;
    (void)msg;
    s->delivered++;
    if (s->hold_seq == 0u || s->hold_seq == seq)
        (void)yrt_sleep_until(yrt_now_ns() + s->hold_ns, 0);
}

static void test_full(void) {
    yrt_pump p;
    yrt_pump_desc d;
    slow_ctx s;
    order_msg probe, probe_copy;
    int i, accepted = 0, rc = 0;
    memset(&s, 0, sizeof(s));
    s.hold_ns  = 400000000ull;  /* 400 ms: long enough that the whole fill
                                 * happens while the first slot is held */
    s.hold_seq = 1u;
    memset(&p, 0, sizeof(p));
    memset(&d, 0, sizeof(d));
    d.msg_size = sizeof(order_msg);
    d.capacity = 8;
    d.on_msg   = slow_on_msg;
    d.ctx      = &s;
    if (!yrt_pump_open(&p, &d)) { fail(__LINE__, yrt_pump_error(&p)); return; }

    memset(&probe, 0xa5, sizeof(probe));
    probe.tag = 0xdeadbeefu;
    probe_copy = probe;
    for (i = 0; i < 64; i++) {
        rc = yrt_pump_submit(&p, &probe);
        if (rc == YRT_ERR_FULL) break;
        if (rc < 0) { fail(__LINE__, yrt_strerror(rc)); break; }
        accepted++;
    }
    CHECK_I(rc, YRT_ERR_FULL, "a ring of 8 must refuse the ninth message");
    /* A message being handled still holds its slot, so a full ring is exactly
     * capacity messages accepted. */
    CHECK_I(accepted, 8, "accepted before the ring filled");
    CHECK(memcmp(&probe, &probe_copy, sizeof(probe)) == 0,
          "YRT_ERR_FULL must not touch the caller's message");
    CHECK(yrt_pump_pending(&p) > 0, "a full ring must report pending messages");
    yrt_pump_close(&p);                 /* drains the eight */
    CHECK_I(s.delivered, 8, "a drain must deliver every accepted message");
    printf("  ERR_FULL at capacity, message intact, drain of 8: ok\n");
}

/* ---------------------------------------------------------------- on_idle */

typedef struct idle_ctx {
    yrt_pump* p;
    int calls;
    int budget;
    int msgs;
} idle_ctx;

static bool idle_on_idle(void* ctx) {
    idle_ctx* k = (idle_ctx*)ctx;
    bool again;
    yrt_pump_lock(k->p);
    k->calls++;
    again = (k->budget > 0);
    if (again) k->budget--;
    yrt_pump_unlock(k->p);
    return again;
}

static void idle_on_msg(void* ctx, const void* msg, uint32_t seq) {
    idle_ctx* k = (idle_ctx*)ctx;
    (void)msg; (void)seq;
    yrt_pump_lock(k->p);
    k->msgs++;
    k->budget = 2;   /* there is more spare work now: two more idle calls */
    yrt_pump_unlock(k->p);
}

static int idle_read_calls(yrt_pump* p, idle_ctx* k) {
    int n;
    yrt_pump_lock(p);
    n = k->calls;
    yrt_pump_unlock(p);
    return n;
}

static void test_idle(void) {
    yrt_pump p;
    yrt_pump_desc d;
    idle_ctx k;
    uint32_t dummy = 1u;
    int c1, c2, c3;
    int rc;
    memset(&k, 0, sizeof(k));
    k.p = &p;
    k.budget = 3;   /* three "yes" answers, then one "no" */
    memset(&p, 0, sizeof(p));
    memset(&d, 0, sizeof(d));
    d.msg_size = sizeof(dummy);
    d.capacity = 4;
    d.on_msg   = idle_on_msg;
    d.on_idle  = idle_on_idle;
    d.ctx      = &k;
    if (!yrt_pump_open(&p, &d)) { fail(__LINE__, yrt_pump_error(&p)); return; }

    nap_ms(60);
    c1 = idle_read_calls(&p, &k);
    nap_ms(60);
    c2 = idle_read_calls(&p, &k);
    CHECK_I(c1, 4, "on_idle runs until it returns false (3 yes, 1 no)");
    CHECK_I(c2, c1, "on_idle must stop being called once it returned false");

    rc = yrt_pump_submit(&p, &dummy);
    CHECK(rc > 0, "submit while the pump is blocked on the idle condvar");
    CHECK_I(yrt_pump_wait(&p, (uint32_t)rc, 2000000000LL), YRT_OK,
            "a submit must wake a pump that is blocked");
    nap_ms(60);
    c3 = idle_read_calls(&p, &k);
    CHECK_I(c3, 7, "on_idle resumes after a message (2 yes, 1 no more)");
    yrt_pump_close(&p);
    CHECK_I(k.msgs, 1, "the one message must have been delivered");
    printf("  on_idle: 4 calls idle, stops on false, 3 more after a submit: ok\n");
}

/* ------------------------------------------------------------ stop: drain */

static void test_stop(bool drop, int expect, const char* label) {
    yrt_pump p;
    yrt_pump_desc d;
    slow_ctx s;
    uint32_t v;
    int i, submitted = 0;
    memset(&s, 0, sizeof(s));
    /* Drop needs the pump to still be inside the first message when stop
     * arrives; drain only needs the queue to be non-empty. */
    s.hold_ns  = drop ? 600000000ull : 5000000ull;
    s.hold_seq = 0u;
    memset(&p, 0, sizeof(p));
    memset(&d, 0, sizeof(d));
    d.msg_size     = sizeof(v);
    d.capacity     = 32;
    d.on_msg       = slow_on_msg;
    d.ctx          = &s;
    d.drop_on_stop = drop;
    if (!yrt_pump_open(&p, &d)) { fail(__LINE__, yrt_pump_error(&p)); return; }
    for (i = 0; i < 20; i++) {
        v = (uint32_t)i;
        if (yrt_pump_submit(&p, &v) > 0) submitted++;
    }
    CHECK_I(submitted, 20, "20 messages into a ring of 32");
    if (drop) {
        /* Wait until the pump has taken the first message and nothing more, so
         * the count after the stop is exactly "the one in flight". pending
         * counts what has NOT been handed over, so 19 is that state. */
        for (i = 0; i < 2000 && yrt_pump_pending(&p) != 19; i++) nap_ms(1);
        CHECK_I(yrt_pump_pending(&p), 19, "one in flight, nineteen queued");
    }
    yrt_pump_close(&p);
    CHECK_I(s.delivered, expect, label);
    CHECK_I(yrt_pump_pending(&p), 0, "pending is 0 once the pump is stopped");
    printf("  stop %s: %d of %d messages delivered\n",
           drop ? "with drop_on_stop" : "draining", s.delivered, submitted);
}

/* ------------------------------------------------- the publish lock (TSan) */

#define CK_N 4000

typedef struct ck_pub {
    uint32_t a, b, sum, seq;
} ck_pub;

typedef struct ck_ctx {
    yrt_pump* p;
    ck_pub      pub;
} ck_ctx;

static void ck_on_msg(void* ctx, const void* msg, uint32_t seq) {
    ck_ctx* k = (ck_ctx*)ctx;
    const uint32_t* v = (const uint32_t*)msg;
    uint32_t a = v[0], b = v[1];
    /* The "inference" runs with no lock held; only the publish takes it, which
     * is the discipline the manual asks of on_msg. */
    yrt_pump_lock(k->p);
    k->pub.a   = a;
    k->pub.b   = b;
    k->pub.sum = a + b;
    k->pub.seq = seq;
    yrt_pump_unlock(k->p);
}

static void test_publish_lock(void) {
    yrt_pump p;
    yrt_pump_desc d;
    static ck_ctx k;
    int i, reads = 0, torn = 0, seen = 0, rc = 0;
    memset(&k, 0, sizeof(k));
    k.p = &p;
    memset(&p, 0, sizeof(p));
    memset(&d, 0, sizeof(d));
    d.msg_size = 2 * sizeof(uint32_t);
    d.capacity = 16;
    d.on_msg   = ck_on_msg;
    d.ctx      = &k;
    if (!yrt_pump_open(&p, &d)) { fail(__LINE__, yrt_pump_error(&p)); return; }
    for (i = 0; i < CK_N; i++) {
        uint32_t v[2];
        ck_pub got;
        v[0] = (uint32_t)i;
        v[1] = (uint32_t)(i * 7 + 13);
        rc = submit_blocking(&p, v);
        if (rc < 0) { fail(__LINE__, yrt_strerror(rc)); break; }
        yrt_pump_lock(&p);
        got = k.pub;
        yrt_pump_unlock(&p);
        reads++;
        if (got.seq != 0u) {
            seen++;
            if (got.sum != got.a + got.b) torn++;
            if (got.b != got.a * 7u + 13u) torn++;
        }
    }
    CHECK_I(yrt_pump_wait(&p, (uint32_t)rc, 5000000000LL), YRT_OK,
            "wait for the last of the checksum messages");
    yrt_pump_close(&p);
    CHECK_I(reads, CK_N, "every read under the lock");
    CHECK_I(torn, 0, "the publish lock must never expose a half-written struct");
    CHECK(seen > CK_N / 10, "the main thread should see most publications");
    CHECK_I(k.pub.seq, (uint32_t)rc, "the last publication is the last message");
    printf("  publish lock: %d reads, %d saw a publication, %d torn\n",
           reads, seen, torn);
}

/* ------------------------------------------------- priority, on_start, edges */

static bool refuse_start(void* ctx, char* err, size_t cap) {
    (void)ctx;
    snprintf(err, cap, "test refuses to start");
    return false;
}

static bool count_start(void* ctx, char* err, size_t cap) {
    (void)err; (void)cap;
    *(int*)ctx += 1;
    return true;
}

static void no_op_msg(void* ctx, const void* msg, uint32_t seq) {
    (void)ctx; (void)msg; (void)seq;
}

static void test_priority_and_start(void) {
    yrt_pump p;
    yrt_pump_desc d;
    uint32_t v = 7u;
    int starts = 0;
    int rc;

    /* below_normal, plus a pin that may or may not be granted: neither is
     * allowed to fail the start. */
    memset(&p, 0, sizeof(p));
    memset(&d, 0, sizeof(d));
    d.msg_size     = sizeof(v);
    d.capacity     = 2;
    d.on_msg       = no_op_msg;
    d.below_normal = true;
    d.pin_cpu      = 1;
    d.on_start     = count_start;
    d.start_ctx    = &starts;
    if (!yrt_pump_open(&p, &d)) {
        fail(__LINE__, yrt_pump_error(&p));
    } else {
        yrt_policy pol = yrt_pump_policy(&p);
        CHECK(pol == YRT_POLICY_BELOW_NORMAL || pol == YRT_POLICY_NORMAL,
              "a pump reports NORMAL or BELOW_NORMAL and nothing else");
#if defined(__EMSCRIPTEN__)
        CHECK_I(pol, YRT_POLICY_NORMAL, "wasm has no priority to lower");
#endif
        CHECK_I(starts, 1, "on_start runs exactly once");
        printf("  below_normal start: policy=%s (a refusal is not an error)\n",
               yrt_policy_name(pol));
        rc = yrt_pump_submit(&p, &v);
        CHECK(rc > 0, "submit to a below-normal pump");
        CHECK_I(yrt_pump_wait(&p, (uint32_t)rc, 2000000000LL), YRT_OK,
                "a below-normal pump still runs its messages");
        yrt_pump_close(&p);
        CHECK_I(yrt_pump_policy(&p), YRT_POLICY_NONE,
                "policy is NONE after a stop");
    }

    /* A default start must be NORMAL, never anything higher: the pump does not
     * climb the ladder. */
    memset(&p, 0, sizeof(p));
    memset(&d, 0, sizeof(d));
    d.msg_size = sizeof(v);
    d.capacity = 2;
    d.on_msg   = no_op_msg;
    if (!yrt_pump_open(&p, &d)) fail(__LINE__, yrt_pump_error(&p));
    CHECK_I(yrt_pump_policy(&p), YRT_POLICY_NORMAL,
            "a default pump runs at normal priority");
    yrt_pump_close(&p);

    /* A refused on_start fails the start and leaves the hook's message. */
    memset(&p, 0, sizeof(p));
    memset(&d, 0, sizeof(d));
    d.msg_size = sizeof(v);
    d.capacity = 2;
    d.on_msg   = no_op_msg;
    d.on_start = refuse_start;
    CHECK(!yrt_pump_open(&p, &d), "a refused on_start must fail the start");
    CHECK(strstr(yrt_pump_error(&p), "test refuses") != NULL,
          "the hook's own message must reach yrt_pump_error");
    CHECK(!yrt_pump_is_running(&p), "nothing runs after a refused start");
    CHECK_I(yrt_pump_policy(&p), YRT_POLICY_NONE,
            "no policy after a refused start");
    CHECK_I(yrt_pump_submit(&p, &v), YRT_ERR_STOPPED,
            "submit after a refused start");
    yrt_pump_close(&p);   /* must be safe */
    printf("  priority, on_start and its refusal: ok\n");
}

static void test_edges(void) {
    yrt_pump p;
    yrt_pump_desc d;
    uint32_t v = 1u;
    int rc;

    /* A zeroed handle is the documented starting state. */
    memset(&p, 0, sizeof(p));
    CHECK(!yrt_pump_is_running(&p), "a zeroed handle is not running");
    CHECK_I(yrt_pump_done_seq(&p), 0, "a zeroed handle has no done_seq");
    CHECK_I(yrt_pump_pending(&p), 0, "a zeroed handle has nothing pending");
    CHECK_I(yrt_pump_policy(&p), YRT_POLICY_NONE, "a zeroed handle has no policy");
    CHECK_I(yrt_pump_submit(&p, &v), YRT_ERR_STOPPED, "submit to a zeroed handle");
    CHECK_I(yrt_pump_wait(&p, 1u, 0), YRT_ERR_STOPPED, "wait on a zeroed handle");
    yrt_pump_lock(&p);      /* a no-op, not a crash */
    yrt_pump_unlock(&p);
    yrt_pump_close(&p);      /* a no-op */
    CHECK_I(yrt_pump_submit(NULL, &v), YRT_ERR_ARG, "submit with a null handle");
    CHECK_I(yrt_pump_wait(NULL, 1u, 0), YRT_ERR_ARG, "wait with a null handle");
    yrt_pump_close(NULL);
    yrt_pump_lock(NULL);
    yrt_pump_unlock(NULL);

    /* Descs that cannot work must fail at start, with a message. */
    CHECK(!yrt_pump_open(&p, NULL), "a NULL desc has no on_msg");
    CHECK(yrt_pump_error(&p)[0] != '\0', "a failed start leaves a message");
    memset(&d, 0, sizeof(d));
    d.msg_size = sizeof(v);
    d.capacity = 4;
    CHECK(!yrt_pump_open(&p, &d), "on_msg is required");
    d.on_msg = no_op_msg;
    d.msg_size = 0;
    CHECK(!yrt_pump_open(&p, &d), "msg_size is required");
    d.msg_size = sizeof(v);
    d.capacity = 0;
    CHECK(!yrt_pump_open(&p, &d), "capacity is required");
    d.capacity = 4;
    d.msg_size = YRT_PUMP_INLINE_BYTES;   /* 4 x that does not fit inline */
    CHECK(!yrt_pump_open(&p, &d), "a ring too big for the inline buffer");
    CHECK(strstr(yrt_pump_error(&p), "YRT_PUMP_INLINE_BYTES") != NULL,
          "the message must name the macro that fixes it");

    /* Stop twice, and submit after a stop. */
    memset(&p, 0, sizeof(p));
    memset(&d, 0, sizeof(d));
    d.msg_size = sizeof(v);
    d.capacity = 4;
    d.on_msg   = no_op_msg;
    if (!yrt_pump_open(&p, &d)) { fail(__LINE__, yrt_pump_error(&p)); return; }
    rc = yrt_pump_submit(&p, &v);
    CHECK(rc > 0, "one message before the stop");
    CHECK_I(yrt_pump_wait(&p, (uint32_t)rc, 2000000000LL), YRT_OK, "wait");
    yrt_pump_close(&p);
    yrt_pump_close(&p);   /* the second one must be a no-op */
    CHECK(!yrt_pump_is_running(&p), "not running after a stop");
    CHECK_I(yrt_pump_submit(&p, &v), YRT_ERR_STOPPED, "submit after a stop");
    /* A seq the pump did reach is still a yes after the stop; one it never
     * reached is YRT_ERR_STOPPED, not a hang. */
    CHECK_I(yrt_pump_wait(&p, (uint32_t)rc, 0), YRT_OK,
            "a seq already done is done after a stop");
    CHECK_I(yrt_pump_wait(&p, (uint32_t)rc + 1u, 10000000LL), YRT_ERR_STOPPED,
            "a seq never reached, on a stopped pump");
    CHECK(yrt_pump_done_seq(&p) == (uint32_t)rc,
          "done_seq survives the stop, for the caller's last check");
    yrt_pump_lock(&p);   /* a no-op after the stop, per the manual */
    yrt_pump_unlock(&p);

    CHECK(strcmp(yrt_strerror(YRT_ERR_FULL), "unknown error") != 0,
          "yrt_strerror names YRT_ERR_FULL");
    CHECK(strcmp(yrt_strerror(YRT_ERR_TIMEOUT), "unknown error") != 0,
          "yrt_strerror names YRT_ERR_TIMEOUT");
    printf("  zeroed handle, bad descs, double stop, submit after stop: ok\n");
}

/* --------------------------------------- a stop releases a blocked wait */

/* The stop has to come from another thread, and ysp/rt.h already has one: a
 * deadline worker fires it while the main thread sits in yrt_pump_wait() for
 * a seq that will never arrive. This is the one exception the declaration of
 * yrt_pump_close() allows to its "do not race another call" rule. */
static void stop_job(void* ctx, const yrt_job_info* info) {
    (void)info;
    yrt_pump_close((yrt_pump*)ctx);
}

static void test_stop_releases_wait(void) {
    yrt_pump p;
    yrt_pump_desc d;
    yrt_worker w;
    yrt_worker_desc wd;
    int64_t t0, waited;
    int rc;
    memset(&p, 0, sizeof(p));
    memset(&d, 0, sizeof(d));
    d.msg_size = sizeof(uint32_t);
    d.capacity = 4;
    d.on_msg   = no_op_msg;
    if (!yrt_pump_open(&p, &d)) { fail(__LINE__, yrt_pump_error(&p)); return; }
    memset(&w, 0, sizeof(w));
    memset(&wd, 0, sizeof(wd));
    wd.no_elevate = true;   /* a test host is not a rig; do not ask for FIFO */
    if (!yrt_worker_open(&w, &wd)) {
        fail(__LINE__, yrt_worker_error(&w));
        yrt_pump_close(&p);
        return;
    }
    if (yrt_worker_submit(&w, yrt_now_ns() + 200000000LL, stop_job, &p) < 0)
        fail(__LINE__, "worker submit");
    t0 = yrt_now_ns();
    rc = yrt_pump_wait(&p, 99u, 10000000000LL);   /* 10 s it must not use */
    waited = yrt_now_ns() - t0;
    yrt_worker_close(&w);
    CHECK_I(rc, YRT_ERR_STOPPED, "a stop must release a blocked wait");
    CHECK(waited < 3000000000LL, "the wait must return with the stop, not the timeout");
    CHECK(!yrt_pump_is_running(&p), "the pump is stopped");
    yrt_pump_close(&p);   /* already stopped: a no-op */
    printf("  a stop releases a blocked wait after %.0f ms\n",
           (double)waited / 1e6);
}

/* ------------------------------------------ wait against stop, hammered */

/* The stop flag the main thread raises for the hammer threads. The test's own
 * atomics, so ThreadSanitizer judges the header and not a sloppy flag. */
#if defined(_MSC_VER)
static int t_load(const int* p) {
    return (int)InterlockedCompareExchange((volatile LONG*)(uintptr_t)p, 0, 0);
}
static void t_store(int* p, int v) { (void)InterlockedExchange((volatile LONG*)p, (LONG)v); }
#else
static int t_load(const int* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void t_store(int* p, int v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
#endif

#define HAMMER_CYCLES  300
#define HAMMER_THREADS 2

typedef struct hammer_ctx {
    yrt_pump* p;
    const int*  quit;
    long calls, ok, timeout, stopped, other;
} hammer_ctx;

/* Runs on a deadline worker for the whole test: ysp/rt.h's own thread is the
 * portable way to get a second one. Alternates a seq the pump reaches (1,
 * after a cycle's first message) with one it never will, with a timeout short
 * enough that most calls are in flight when a stop lands. */
static void hammer_job(void* ctx, const yrt_job_info* info) {
    hammer_ctx* h = (hammer_ctx*)ctx;
    (void)info;
    while (!t_load(h->quit)) {
        uint32_t seq = (h->calls & 1) ? 1u : 0x7fff0000u;
        int rc = yrt_pump_wait(h->p, seq, 300000LL);   /* 0.3 ms */
        h->calls++;
        if (rc == YRT_OK) h->ok++;
        else if (rc == YRT_ERR_TIMEOUT) h->timeout++;
        else if (rc == YRT_ERR_STOPPED) h->stopped++;
        else h->other++;
    }
}

static void test_hammer_wait_vs_stop(void) {
    static yrt_pump p;
    yrt_pump_desc d;
    yrt_worker w[HAMMER_THREADS];
    yrt_worker_desc wd;
    hammer_ctx h[HAMMER_THREADS];
    int quit = 0;
    int i, started = 0, restarts = 0;
    uint32_t v = 1u;
    long calls = 0, ok = 0, timeout = 0, stopped = 0, other = 0;
    int64_t t0 = yrt_now_ns();

    memset(&p, 0, sizeof(p));
    memset(&wd, 0, sizeof(wd));
    wd.no_elevate = true;
    for (i = 0; i < HAMMER_THREADS; i++) {
        memset(&h[i], 0, sizeof(h[i]));
        h[i].p = &p;
        h[i].quit = &quit;
        memset(&w[i], 0, sizeof(w[i]));
        if (!yrt_worker_open(&w[i], &wd)) {
            fail(__LINE__, yrt_worker_error(&w[i]));
            break;
        }
        started++;
        if (yrt_worker_submit(&w[i], 0, hammer_job, &h[i]) < 0)
            fail(__LINE__, "hammer submit");
    }

    /* Stop and restart under the waiters, in every shape a stop can have:
     * idle, with a queue to drain, with a queue to drop, and at once. */
    for (i = 0; i < HAMMER_CYCLES; i++) {
        memset(&d, 0, sizeof(d));
        d.msg_size     = sizeof(v);
        d.capacity     = 4;
        d.on_msg       = no_op_msg;
        d.drop_on_stop = (i % 3 == 2);
        if (!yrt_pump_open(&p, &d)) { fail(__LINE__, yrt_pump_error(&p)); break; }
        restarts++;
        if (i % 2) {
            (void)yrt_pump_submit(&p, &v);
            (void)yrt_pump_submit(&p, &v);
        }
        if (i % 5 == 0) nap_ms(1);
        yrt_pump_close(&p);
    }

    t_store(&quit, 1);
    for (i = 0; i < started; i++) yrt_worker_close(&w[i]);   /* joins */
    for (i = 0; i < started; i++) {
        calls += h[i].calls; ok += h[i].ok; timeout += h[i].timeout;
        stopped += h[i].stopped; other += h[i].other;
    }
    CHECK_I(restarts, HAMMER_CYCLES, "every start in the hammer must succeed");
    CHECK_I(other, 0, "a racing wait returns only 0, TIMEOUT or STOPPED");
    CHECK(calls > HAMMER_CYCLES, "the waiters must have been busy throughout");
    CHECK(stopped > 0, "some waits must have met a stopped pump");
    CHECK(ok + timeout > 0, "some waits must have met a running pump");
    CHECK(!yrt_pump_is_running(&p), "the pump ends stopped");
    printf("  wait vs stop/restart: %d cycles, %ld waits on %d threads "
           "(ok %ld, timeout %ld, stopped %ld), %.0f ms\n",
           restarts, calls, started, ok, timeout, stopped,
           (double)(yrt_now_ns() - t0) / 1e6);
}

/* ------------------------------------------------------------- event ring */

static unsigned char g_ring_mem[YRT_RING_BYTES(4096) + 1];

/* Open, sizes, order, the stamps, a full ring and its LOSS record. */
static void test_ring_basic(void) {
    yrt_ring r;
    yrt_ring_desc d;
    yrt_event ev, out[16];
    int64_t t0, t1;
    int i, n;

    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    memset(&ev, 0, sizeof(ev));
    CHECK(!yrt_ring_open(&r, NULL), "a NULL desc fails");
    CHECK(yrt_ring_error(&r)[0] != '\0', "a failed open leaves a message");
    CHECK(!yrt_ring_open(&r, &d), "NULL memory fails");
    d.memory = g_ring_mem;
    d.bytes = 64;   /* at most one slot after alignment */
    CHECK(!yrt_ring_open(&r, &d), "room for fewer than 2 records fails");
    CHECK(strstr(yrt_ring_error(&r), "YRT_RING_BYTES") != NULL,
          "the message must name the macro that sizes the memory");
    CHECK_I(yrt_ring_push(&r, &ev), YRT_ERR_ARG, "push to a ring that never opened");
    CHECK_I(yrt_ring_drain(&r, out, 16), YRT_ERR_ARG, "drain of a ring that never opened");
    CHECK_I(yrt_ring_capacity(&r), 0, "an unopened ring has no capacity");

    d.bytes = YRT_RING_BYTES(4096);
    CHECK(yrt_ring_open(&r, &d), "YRT_RING_BYTES(4096)");
    CHECK_I(yrt_ring_capacity(&r), 4096, "YRT_RING_BYTES(n) holds n");
    d.memory = g_ring_mem + 1;   /* misaligned on purpose */
    CHECK(yrt_ring_open(&r, &d), "misaligned memory");
    CHECK_I(yrt_ring_capacity(&r), 4096, "the alignment pad covers a misaligned start");
    CHECK(((uintptr_t)r.slots & 63u) == 0, "slots are 64-aligned");
    d.bytes = YRT_RING_BYTES(100);
    CHECK(yrt_ring_open(&r, &d), "100 records");
    CHECK_I(yrt_ring_capacity(&r), 64, "rounded down to a power of two");

    /* Order, seq, and the zero-means-stamp rule. */
    d.memory = g_ring_mem;
    d.bytes = YRT_RING_BYTES(8);
    CHECK(yrt_ring_open(&r, &d), "8 records");
    t0 = yrt_now_ns();
    for (i = 0; i < 5; i++) {
        memset(&ev, 0, sizeof(ev));
        ev.seq = 999u;                   /* ignored */
        ev.source = (uint16_t)YRT_SRC_USER;
        ev.kind = 3;
        ev.aux = (uint32_t)i;
        ev.u.i64[4] = -i;
        if (i == 4) { ev.t_ns = 12345u; ev.tid = 77u; }
        CHECK_I(yrt_ring_push(&r, &ev), 0, "push into room");
    }
    t1 = yrt_now_ns();
    n = yrt_ring_drain(&r, out, 16);
    CHECK_I(n, 5, "drain returns what was pushed");
    for (i = 0; i < n && i < 5; i++) {
        CHECK_I(out[i].seq, i + 1, "seq is ticket + 1, in push order");
        CHECK_I(out[i].aux, i, "records come out in push order");
        CHECK(out[i].u.i64[4] == -i, "payload intact");
        CHECK_I(out[i].source, YRT_SRC_USER, "source");
        if (i < 4) {
            CHECK(out[i].t_ns >= t0 && out[i].t_ns <= t1, "t_ns 0 is stamped at the push");
            CHECK(out[i].tid == yrt_thread_id(), "tid 0 is the pushing thread");
        }
    }
    CHECK(out[4].t_ns == 12345u && out[4].tid == 77u, "set stamps are kept");
    CHECK_I(yrt_ring_drain(&r, out, 16), 0, "an empty ring drains nothing");
    CHECK_I(yrt_ring_drain(&r, out, 0), 0, "cap 0 drains nothing");

    /* Full: 8 fit, 3 are refused, the drain reports them first, then the 8. */
    memset(&ev, 0, sizeof(ev));
    for (i = 0; i < 8; i++) {
        ev.aux = (uint32_t)(100 + i);
        CHECK_I(yrt_ring_push(&r, &ev), 0, "fill the ring");
    }
    for (i = 0; i < 3; i++)
        CHECK_I(yrt_ring_push(&r, &ev), YRT_ERR_FULL, "a full ring refuses");
    CHECK_I(yrt_ring_dropped(&r), 3, "every refusal is counted");
    n = yrt_ring_drain(&r, out, 4);
    CHECK_I(n, 4, "LOSS plus 3 records in a cap of 4");
    CHECK(out[0].source == YRT_SRC_RT && out[0].kind == YRT_KIND_LOSS, "LOSS first");
    CHECK(out[0].u.u64[0] == 3u && out[0].u.u64[1] == 3u, "LOSS counts the refusals");
    CHECK_I(out[0].seq, 0, "LOSS has no ticket");
    CHECK_I(out[1].aux, 100, "the oldest record follows the LOSS");
    CHECK_I(out[1].seq, 6, "tickets continue across a refusal");
    n = yrt_ring_drain(&r, out, 16);
    CHECK_I(n, 5, "the rest, and no second LOSS");
    CHECK_I(out[0].aux, 103, "in order");
    CHECK_I(yrt_ring_push(&r, &ev), 0, "room again after the drain");
    CHECK_I(yrt_ring_push(&r, NULL), YRT_ERR_ARG, "NULL event");
    CHECK_I(yrt_ring_drain(&r, NULL, 4), YRT_ERR_ARG, "NULL out");
    printf("  ring: open, sizes, order, stamps, full and LOSS: ok\n");
}

/* Many producers, one drainer, a ring small enough to fill. Every record
 * carries words derived from (producer, index), so a torn record cannot pass,
 * and every push's outcome is kept, so the accounting is checked exactly:
 * each push is drained once or refused once, never both and never neither. */
#define STRESS_M    20000
#define STRESS_PMAX 8

typedef struct stress_ctx {
    yrt_ring*    r;
    const int*     go;
    int            pid;
    int            done;
    uint32_t       tid;
    long           refused;
    unsigned char* refused_at;   /* STRESS_M flags */
} stress_ctx;

static unsigned char g_stress_refused[STRESS_PMAX][STRESS_M];
static unsigned char g_stress_seen[STRESS_PMAX][STRESS_M];

static uint64_t stress_word(int pid, uint32_t i, int k) {
    uint64_t x = ((uint64_t)(pid + 1) << 40) ^ ((uint64_t)i << 8) ^ (uint64_t)k;
    x *= 0x9E3779B97F4A7C15ull;
    return x ^ (x >> 29);
}

static void stress_job(void* ctx, const yrt_job_info* info) {
    stress_ctx* s = (stress_ctx*)ctx;
    yrt_event ev;
    uint32_t i;
    int k;
    (void)info;
    s->tid = yrt_thread_id();
    while (!t_load(s->go)) { }
    for (i = 0; i < STRESS_M; i++) {
        memset(&ev, 0, sizeof(ev));
        ev.source = (uint16_t)YRT_SRC_USER;
        ev.kind = (uint16_t)(s->pid + 1);
        ev.aux = i;
        ev.t_ns = (int64_t)i + 1;
        for (k = 0; k < 5; k++) ev.u.u64[k] = stress_word(s->pid, i, k);
        if (yrt_ring_push(s->r, &ev) == YRT_ERR_FULL) {
            s->refused_at[i] = 1;
            s->refused++;
        }
        /* Bursts with gaps, so the drainer keeps up most of the time and
         * the pushes interleave with drains rather than only with refusals. */
        if (i % 32u == 31u) (void)yrt_spin_until(yrt_now_ns() + 20000);
    }
    t_store(&s->done, 1);
}

static void test_ring_stress(int producers) {
    static unsigned char mem[YRT_RING_BYTES(256)];
    static yrt_event out[128];
    static yrt_worker w[STRESS_PMAX];
    static stress_ctx c[STRESS_PMAX];
    yrt_ring r;
    yrt_ring_desc d;
    yrt_worker_desc wd;
    uint32_t last_i[STRESS_PMAX], last_seq = 0;
    int have_last[STRESS_PMAX];
    int go = 0, started = 0, i, all_done = 0, naps = 0;
    long drained = 0, refused = 0, torn = 0, dup = 0, order = 0, seq_gap = 0, bad_tid = 0;
    long accounting = 0;
    uint64_t loss = 0;
    int64_t t0, t_nap;

    memset(&d, 0, sizeof(d));
    d.memory = mem;
    d.bytes = sizeof(mem);
    if (!yrt_ring_open(&r, &d)) { fail(__LINE__, yrt_ring_error(&r)); return; }
    memset(g_stress_refused, 0, sizeof(g_stress_refused));
    memset(g_stress_seen, 0, sizeof(g_stress_seen));
    memset(&wd, 0, sizeof(wd));
    wd.no_elevate = true;
    for (i = 0; i < producers; i++) {
        memset(&c[i], 0, sizeof(c[i]));
        c[i].r = &r;
        c[i].go = &go;
        c[i].pid = i;
        c[i].refused_at = g_stress_refused[i];
        have_last[i] = 0;
        last_i[i] = 0;
        memset(&w[i], 0, sizeof(w[i]));
        if (!yrt_worker_open(&w[i], &wd)) { fail(__LINE__, yrt_worker_error(&w[i])); break; }
        started++;
        if (yrt_worker_submit(&w[i], 0, stress_job, &c[i]) < 0) fail(__LINE__, "stress submit");
    }
    nap_ms(20);   /* every producer parked on `go` */
    t0 = t_nap = yrt_now_ns();
    t_store(&go, 1);
    for (;;) {
        int n, j, k;
        int was_done = all_done;
        if (!all_done) {
            all_done = 1;
            for (i = 0; i < started; i++) if (!t_load(&c[i].done)) all_done = 0;
        }
        n = yrt_ring_drain(&r, out, 128);
        for (j = 0; j < n; j++) {
            const yrt_event* e = &out[j];
            int pid;
            if (e->source == YRT_SRC_RT && e->kind == YRT_KIND_LOSS) {
                loss += e->u.u64[0];
                if (e->u.u64[1] != loss) torn++;
                continue;
            }
            if (e->seq != last_seq + 1u) seq_gap++;
            last_seq = e->seq;
            pid = (int)e->kind - 1;
            if (pid < 0 || pid >= started || e->aux >= STRESS_M) { torn++; continue; }
            for (k = 0; k < 5; k++)
                if (e->u.u64[k] != stress_word(pid, e->aux, k)) { torn++; break; }
            if (e->t_ns != (int64_t)e->aux + 1) torn++;
            if (e->tid != c[pid].tid) bad_tid++;
            if (g_stress_seen[pid][e->aux]) dup++;
            g_stress_seen[pid][e->aux] = 1;
            if (have_last[pid] && e->aux <= last_i[pid]) order++;
            have_last[pid] = 1;
            last_i[pid] = e->aux;
            drained++;
        }
        /* Fall behind for 1 ms in every 6, so the ring fills and refuses
         * some of the time and keeps up the rest. */
        if (!all_done && yrt_now_ns() - t_nap > 5000000) {
            nap_ms(1);
            t_nap = yrt_now_ns();
            naps++;
        }
        /* Done only after a drain that started with every producer finished
         * came back empty. */
        if (was_done && n == 0) break;
    }
    for (i = 0; i < started; i++) yrt_worker_close(&w[i]);
    for (i = 0; i < started; i++) {
        uint32_t j;
        refused += c[i].refused;
        for (j = 0; j < STRESS_M; j++)
            if (g_stress_seen[i][j] == g_stress_refused[i][j]) accounting++;
    }
    CHECK_I(started, producers, "every producer started");
    CHECK_I(torn, 0, "no torn record");
    CHECK_I(dup, 0, "no duplicate record");
    CHECK_I(order, 0, "each producer's records in its own order");
    CHECK_I(seq_gap, 0, "a refusal takes no ticket, so seq has no gaps");
    CHECK_I(bad_tid, 0, "tid 0 is filled with the pushing thread's id");
    CHECK_I(accounting, 0, "each push is drained once or refused once");
    CHECK_I(drained + refused, (long)producers * STRESS_M, "drained + refused = pushed");
    CHECK_I((long)loss, refused, "the LOSS records add up to the refusals");
    CHECK_I((long)yrt_ring_dropped(&r), refused, "yrt_ring_dropped is exact");
    CHECK(drained > 0 && refused > 0, "the run must both drain and refuse");
    printf("  ring stress, %d producers x %d into 256 slots: drained %ld, refused %ld, "
           "%d drainer naps, %.0f ms\n", producers, STRESS_M, drained, refused, naps,
           (double)(yrt_now_ns() - t0) / 1e6);
}

/* The trace macros in YRT_TRACE_RING mode (this file defines it), and the
 * zones ysp/rt.h's own worker emits. */
static void trace_job(void* ctx, const yrt_job_info* info) {
    (void)ctx; (void)info;
}

static void test_trace(void) {
    static unsigned char mem[YRT_RING_BYTES(256)];
    static yrt_event out[256];
    yrt_ring r;
    yrt_ring_desc d;
    yrt_worker w;
    yrt_worker_desc wd;
    int n, i, zones = 0, plots = 0, msgs = 0, frames = 0, names = 0;
    int worker_zone = 0, worker_name = 0;
    int64_t t0;

    memset(&d, 0, sizeof(d));
    d.memory = mem;
    d.bytes = sizeof(mem);
    if (!yrt_ring_open(&r, &d)) { fail(__LINE__, yrt_ring_error(&r)); return; }

    /* Detached: the macros write nothing anywhere. */
    CHECK(yrt_trace_ring() == NULL, "no trace ring by default");
    {
        YRT_ZONE(z0, "detached");
        YRT_PLOT("detached", 1.0);
        YRT_ZONE_END(z0);
    }

    yrt_trace_set_ring(&r);
    CHECK(yrt_trace_ring() == &r, "attached");
    t0 = yrt_now_ns();
    YRT_THREAD_NAME("test main");
    {
        YRT_ZONE(outer, "outer");
        YRT_ZONE_VALUE(outer, 99);
        {
            YRT_ZONE(inner, "inner");
            nap_ms(2);
            YRT_ZONE_END(inner);
        }
        YRT_PLOT("fill", 0.25);
        YRT_MESSAGE("trial start");
        YRT_MESSAGEF("trial %d of %d, and a tail long enough to truncate", 3, 40);
        YRT_FRAME_MARK();
        YRT_FRAME_MARK_NAMED("display 2");
        YRT_ZONE_END(outer);
    }
    memset(&w, 0, sizeof(w));
    memset(&wd, 0, sizeof(wd));
    wd.no_elevate = true;
    if (yrt_worker_open(&w, &wd)) {
        (void)yrt_worker_submit(&w, 0, trace_job, NULL);
        nap_ms(30);
        yrt_worker_close(&w);
    } else {
        fail(__LINE__, yrt_worker_error(&w));
    }
    yrt_trace_set_ring(NULL);

    n = yrt_ring_drain(&r, out, 256);
    for (i = 0; i < n; i++) {
        const yrt_event* e = &out[i];
        if (e->source != YRT_SRC_RT) continue;
        if (e->kind == YRT_KIND_ZONE) {
            const char* nm = e->u.zone.loc->name;
            zones++;
            if (strcmp(nm, "outer") == 0) {
                CHECK(e->u.zone.value == 99u, "zone value");
                CHECK(e->u.zone.dur_ns >= 2000000u, "outer contains the 2 ms nap");
                CHECK(e->t_ns >= t0, "zone t_ns is its begin");
                CHECK(strcmp(e->u.zone.loc->function, "test_trace") == 0, "srcloc function");
                CHECK(e->tid == yrt_thread_id(), "zone tid");
            }
            if (strcmp(nm, "inner") == 0)
                CHECK(e->u.zone.dur_ns >= 2000000u, "inner contains the nap");
            if (strcmp(nm, "yrt.worker.job") == 0 && e->tid != yrt_thread_id())
                worker_zone++;
            CHECK(strcmp(nm, "detached") != 0, "a detached zone is not recorded");
        } else if (e->kind == YRT_KIND_PLOT) {
            plots++;
            CHECK(strcmp(e->u.plot.name, "fill") == 0 && e->u.plot.value == 0.25, "plot");
        } else if (e->kind == YRT_KIND_MESSAGE) {
            msgs++;
            if (msgs == 2) {
                CHECK(strlen(e->u.text) == 39, "MESSAGEF truncates to 39 characters");
                CHECK(strncmp(e->u.text, "trial 3 of 40", 13) == 0, "MESSAGEF formats");
            } else {
                CHECK(strcmp(e->u.text, "trial start") == 0, "MESSAGE");
            }
        } else if (e->kind == YRT_KIND_FRAME) {
            frames++;
            if (frames == 2)
                CHECK(e->u.plot.name && strcmp(e->u.plot.name, "display 2") == 0,
                      "FRAME_MARK_NAMED carries its name");
        } else if (e->kind == YRT_KIND_THREAD_NAME) {
            names++;
            if (strcmp(e->u.text, "yrt worker") == 0) worker_name++;
        }
    }
    CHECK(zones >= 3, "outer, inner and the worker's job");
    CHECK_I(plots, 1, "one plot");
    CHECK_I(msgs, 2, "two messages");
    CHECK_I(frames, 2, "two frame marks");
    CHECK(names >= 2, "the main thread and the worker named themselves");
    CHECK_I(worker_zone, 1, "the worker traced its job on its own thread");
    CHECK_I(worker_name, 1, "YRT_THREAD_INIT on the worker thread");
    printf("  trace ring: %d records (%d zones), worker zone and name: ok\n", n, zones);
}

/* ------------------------------------------------------ clock correlation */

static uint64_t offset_clock(void* ctx) { return yrt_now_ns() + *(const uint64_t*)ctx; }
static uint64_t ms_clock(void* ctx) { (void)ctx; return yrt_now_ns() / 1000000; }
static uint64_t frozen_clock(void) { return 7u; }
static uint64_t rt_clock(void) { return (uint64_t)yrt_now_ns(); }

static uint64_t abs_diff(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

static void test_correlate(void) {
    yrt_corr_desc d;
    yrt_corr c;
    uint64_t off = 123456789u;
    int64_t t0, el, self_w;
    int rc;

    memset(&d, 0, sizeof(d));
    CHECK_I(yrt_correlate(&d, &c), YRT_ERR_ARG, "no reader");
    CHECK_I(yrt_correlate(NULL, &c), YRT_ERR_ARG, "NULL desc");
    d.read = rt_clock;
    d.read_ctx = offset_clock;
    CHECK_I(yrt_correlate(&d, &c), YRT_ERR_ARG, "two readers");
    d.read_ctx = NULL;
    CHECK_I(yrt_correlate(&d, NULL), YRT_ERR_ARG, "NULL out");

    /* The clock against itself: the read lies inside its own bracket. */
    CHECK_I(yrt_correlate(&d, &c), 16, "plain mode takes 16 tries by default");
    CHECK(c.width_ns > 0, "the width is never 0");
    CHECK(abs_diff(c.other, (uint64_t)c.rt_ns) <= (uint64_t)c.width_ns / 2u + 1u,
          "the clock against itself agrees within half the width");
    self_w = c.width_ns;

    /* A fixed offset is recovered within half the width. */
    memset(&d, 0, sizeof(d));
    d.read_ctx = offset_clock;
    d.ctx = &off;
    d.tries = 5;
    CHECK_I(yrt_correlate(&d, &c), 5, "tries is honored");
    CHECK(abs_diff(c.other - off, (uint64_t)c.rt_ns) <= (uint64_t)c.width_ns / 2u + 1u, "offset recovered");

    /* Edge mode on a 1 ms clock made from ours: the edge is exactly at
     * value * 1e6 on our clock, so it must lie within half the width. */
    memset(&d, 0, sizeof(d));
    d.read_ctx = ms_clock;
    d.edge = true;
    t0 = yrt_now_ns();
    rc = yrt_correlate(&d, &c);
    el = yrt_now_ns() - t0;
    CHECK_I(rc, 4, "edge mode takes 4 edges by default");
    CHECK(abs_diff(c.other * 1000000u, (uint64_t)c.rt_ns) <= (uint64_t)c.width_ns / 2u + 1u,
          "edge mode finds the tick boundary within half the width");
    CHECK(c.width_ns < 1000000u, "an edge is far tighter than the tick");
    CHECK(el < 50000000u, "4 edges of a 1 ms clock take about 4 ms");
    printf("  correlate: self width %llu ns, 1 ms edge width %llu ns, 4 edges in %.1f ms\n",
           (unsigned long long)self_w, (unsigned long long)c.width_ns, (double)el / 1e6);

    /* A clock that never moves: the timeout, not a hang. */
    memset(&d, 0, sizeof(d));
    d.read = frozen_clock;
    d.edge = true;
    d.timeout_ns = 2000000u;
    CHECK_I(yrt_correlate(&d, &c), YRT_ERR_TIMEOUT, "edge mode times out");

#if defined(_WIN32)
    {
        /* Interrupt time against QPC: two correlations 50 ms apart must agree
         * on the rate, and the width is never below the QPC tick. */
        yrt_corr a, b;
        double rate;
        CHECK(yrt_interrupt_time_100ns() != 0, "QueryInterruptTimePrecise exists (Win10+)");
        memset(&d, 0, sizeof(d));
        d.read = yrt_interrupt_time_100ns;
        CHECK(yrt_correlate(&d, &a) > 0, "correlate interrupt time");
        nap_ms(50);
        CHECK(yrt_correlate(&d, &b) > 0, "correlate interrupt time again");
        CHECK(a.width_ns >= 100u, "the width floor is the QPC tick");
        rate = (double)(b.other - a.other) * 100.0 / (double)(b.rt_ns - a.rt_ns);
        CHECK(rate > 0.999 && rate < 1.001, "interrupt time runs at the QPC rate");
        printf("  interrupt time: width %llu ns, rate %.6f\n",
               (unsigned long long)a.width_ns, rate);
    }
#else
    CHECK(yrt_interrupt_time_100ns() == 0, "interrupt time is a Windows clock");
#endif
}

/* -------------------------------------------- Windows power throttling */

#if defined(_WIN32)
/* What yrt_report_get() says about EcoQoS on a thread ysp/rt.h set up,
 * read on that thread from an on_start hook. */
static int g_eco_seen = -1;

static bool eco_probe(void* ctx, char* err, size_t cap) {
    yrt_report rep;
    (void)ctx; (void)err; (void)cap;
    yrt_report_get(&rep, YRT_POLICY_NONE);
    g_eco_seen = rep.throttle_known ? (rep.ecoqos_off ? 1 : 0) : -1;
    return true;
}

static void eco_msg(void* ctx, const void* msg, uint32_t seq) { (void)ctx; (void)msg; (void)seq; }
#endif

static void test_throttle(void) {
#if defined(_WIN32)
    yrt_worker w;
    yrt_worker_desc wd;
    yrt_pump p;
    yrt_pump_desc pd;
    yrt_report rep;
    char line[256];
    int worker_eco, plain_eco, pump_eco;

    memset(&w, 0, sizeof(w));
    memset(&wd, 0, sizeof(wd));
    wd.on_start = eco_probe;   /* elevates first, then runs the hook */
    g_eco_seen = -1;
    if (yrt_worker_open(&w, &wd)) yrt_worker_close(&w);
    worker_eco = g_eco_seen;
    wd.no_elevate = true;
    g_eco_seen = -1;
    memset(&w, 0, sizeof(w));
    if (yrt_worker_open(&w, &wd)) yrt_worker_close(&w);
    plain_eco = g_eco_seen;
    memset(&p, 0, sizeof(p));
    memset(&pd, 0, sizeof(pd));
    pd.msg_size = 4;
    pd.capacity = 1;
    pd.on_msg = eco_msg;
    pd.on_start = eco_probe;
    pd.below_normal = true;
    g_eco_seen = -1;
    if (yrt_pump_open(&p, &pd)) yrt_pump_close(&p);
    pump_eco = g_eco_seen;
    CHECK(worker_eco >= 0, "Windows 10 1709+ reports the thread's EcoQoS state");
    CHECK_I(worker_eco, 1, "an elevated worker thread is opted out of EcoQoS");
    CHECK_I(plain_eco, 0, "a no_elevate worker is left to the OS");
    CHECK_I(pump_eco, 1, "the pump thread is opted out of EcoQoS, below normal too");

    CHECK(yrt_timer_resolution_begin() == 0, "timeBeginPeriod(1)");
    yrt_report_get(&rep, YRT_POLICY_NORMAL);
    CHECK(rep.timer_throttle_off, "raising the resolution opts out of its throttling");
    yrt_describe(&rep, line, sizeof(line));
    CHECK(strstr(line, "timer_throttle=off") != NULL, "yrt_describe says so");
    CHECK(strstr(line, "ecoqos=on") != NULL, "the test's own thread was never elevated");
    yrt_timer_resolution_end();
    printf("  power throttling: worker %d, no_elevate worker %d, pump %d; %s\n",
           worker_eco, plain_eco, pump_eco, line);
#else
    yrt_report rep;
    char line[256];
    yrt_report_get(&rep, YRT_POLICY_NORMAL);
    yrt_describe(&rep, line, sizeof(line));
    CHECK(!rep.throttle_known, "power throttling is a Windows notion");
    CHECK(strstr(line, "timer_throttle=n/a ecoqos=n/a") != NULL, "yrt_describe says n/a");
    printf("  power throttling: n/a on this platform\n");
#endif
}

/* ------------------------------------------------------------ core types */

#if defined(_WIN32)
/* Run on an elevated worker thread before start() returns: what the report
 * says after yrt_thread_elevate(), then after a pin to an E-core. */
typedef struct core_probe {
    int e_cpu;
    bool pref_after_elevate, pref_after_pin, pinned_runs_there;
    int pinned_cpu;
    yrt_core_type pinned_core;
    char line[256];
} core_probe;

static bool core_probe_start(void* ctx, char* err, size_t cap) {
    core_probe* c = (core_probe*)ctx;
    yrt_report rep;
    (void)err; (void)cap;
    yrt_report_get(&rep, YRT_POLICY_NONE);
    c->pref_after_elevate = rep.pcores_preferred;
    if (c->e_cpu >= 0 && yrt_thread_pin(c->e_cpu) == 0) {
#if defined(_WIN32)
        int k;
        c->pinned_runs_there = true;
        for (k = 0; k < 50; k++) {
            if ((int)GetCurrentProcessorNumber() != c->e_cpu) c->pinned_runs_there = false;
            Sleep(1);
        }
#endif
        yrt_report_get(&rep, YRT_POLICY_NONE);
        c->pref_after_pin = rep.pcores_preferred;
        c->pinned_cpu = rep.pinned_cpu;
        c->pinned_core = rep.pinned_core;
        yrt_describe(&rep, c->line, sizeof(c->line));
    }
    return true;
}
#endif

static void test_core_types(void) {
    int cpu, np = 0, ne = 0, nu = 0, nq = 0, first_e = -1;
    yrt_report rep;
    yrt_report big;
    char line[256];
    int len;
    for (cpu = 0; cpu < 64; cpu++) {
        yrt_core_type t = yrt_cpu_core_type(cpu);
        if (t == YRT_CORE_PERFORMANCE) np++;
        else if (t == YRT_CORE_EFFICIENCY) { ne++; if (first_e < 0) first_e = cpu; }
        else if (t == YRT_CORE_UNIFORM) nu++;
        else nq++;
    }
    CHECK_I(np + ne + nu + nq, 64, "every CPU number gets an answer");
    CHECK_I(yrt_cpu_core_type(-1), YRT_CORE_UNKNOWN, "a CPU that does not exist");
    CHECK(strcmp(yrt_core_type_name(YRT_CORE_PERFORMANCE), "P") == 0, "names");
    CHECK(strcmp(yrt_core_type_name(YRT_CORE_EFFICIENCY), "E") == 0, "names");
    yrt_report_get(&rep, YRT_POLICY_NORMAL);
    CHECK_I(rep.pinned_cpu, -1, "the test's own thread was never pinned");
    CHECK(!rep.pcores_preferred, "nor elevated");
#if defined(_WIN32)
    CHECK(np + ne + nu > 0, "Windows 10+ names the kind of every CPU");
    CHECK(!(np && nu) && !(ne && nu), "a machine is hybrid or uniform, not both");
    if (ne > 0) {
        /* Hybrid: an elevated worker prefers the P-cores, and a pin to an
         * E-core drops the preference and holds. */
        yrt_worker w;
        yrt_worker_desc wd;
        static core_probe c;
        memset(&c, 0, sizeof(c));
        c.e_cpu = first_e;
        memset(&w, 0, sizeof(w));
        memset(&wd, 0, sizeof(wd));
        wd.on_start = core_probe_start;
        wd.start_ctx = &c;
        if (yrt_worker_open(&w, &wd)) yrt_worker_close(&w);
        CHECK(c.pref_after_elevate, "an elevated thread prefers the P-cores on a hybrid CPU");
        CHECK(!c.pref_after_pin, "a pin drops the preference");
        CHECK(c.pinned_runs_there, "the pinned thread runs on its E-core");
        CHECK_I(c.pinned_cpu, first_e, "the report has the pinned CPU");
        CHECK_I(c.pinned_core, YRT_CORE_EFFICIENCY, "and its kind");
        CHECK(strstr(c.line, " cores=any") != NULL, "the line says the preference is gone");
        printf("  core types: %d P, %d E; pinned worker: %s\n", np, ne, c.line);
    } else {
        printf("  core types: uniform machine (%d CPUs), no preference to test\n", nu);
    }
#else
    CHECK_I(np + ne + nu, 0, "only Windows names core kinds");
    printf("  core types: unknown here (%d CPUs asked)\n", nq);
#endif
    /* The longest line a report can give on this platform still fits in
     * 192 bytes; any report fits in 256. */
    memset(&big, 0, sizeof(big));
    yrt_report_get(&big, YRT_POLICY_TIME_CONSTRAINT);
    big.pinned_cpu = 1023;
    big.pinned_core = YRT_CORE_UNIFORM;
    big.memory_locked = big.timer_resolution_raised = big.hires_timer = true;
    big.pcores_preferred = false;
    len = yrt_describe(&big, line, sizeof(line));
    CHECK(len < 191, "a real report's line fits in 192 bytes");
    big.clock_res_ns = UINT64_MAX;
    big.timer_slack_known = true;
    big.timer_slack_ns = UINT64_MAX;
    big.pinned_cpu = 99999;
    len = yrt_describe(&big, line, sizeof(line));
    CHECK(len < 255, "any report's line fits in 256 bytes");
}

/* ----------------------------------------------------------- raw counter */

/* yrt_ticks_to_ns() must be the conversion yrt_now_ns() uses: a counter
 * read just before and just after yrt_now_ns() must convert to times that
 * bracket it, every second of counts must be exactly 1e9 ns, and a negative
 * count converts as a difference. */
static void test_ticks(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, a, b;
    int64_t now;
    int i, bad = 0;
    QueryPerformanceFrequency(&f);
    for (i = 0; i < 1000; i++) {
        QueryPerformanceCounter(&a);
        now = yrt_now_ns();
        QueryPerformanceCounter(&b);
        if (yrt_ticks_to_ns(a.QuadPart) > now || yrt_ticks_to_ns(b.QuadPart) < now) bad++;
        if (a.QuadPart == b.QuadPart && yrt_ticks_to_ns(a.QuadPart) != now) bad++;
    }
    CHECK_I(bad, 0, "yrt_ticks_to_ns(QPC) brackets yrt_now_ns(), and equals it on one tick");
    CHECK(yrt_ticks_to_ns(f.QuadPart * 3600) == 3600000000000ll, "an hour of counts is an hour");
    CHECK(yrt_ticks_to_ns(-f.QuadPart) == -1000000000ll, "negative counts convert");
#if defined(__SIZEOF_INT128__)
    {
        /* An independent reference in 128-bit arithmetic, over 40 days of
         * uptime at this machine's frequency. */
        __extension__ typedef unsigned __int128 u128;
        uint64_t x = 88172645463325252ull;
        int mism = 0;
        for (i = 0; i < 100000; i++) {
            int64_t t;
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            t = (int64_t)(x % ((uint64_t)f.QuadPart * 3456000ull));
            if ((u128)yrt_ticks_to_ns(t) != (u128)t * 1000000000u / (uint64_t)f.QuadPart)
                mism++;
        }
        CHECK_I(mism, 0, "yrt_ticks_to_ns matches a 128-bit reference");
    }
#endif
    printf("  ticks to ns: QPC at %lld Hz converts as yrt_now_ns() does\n",
           (long long)f.QuadPart);
#else
    CHECK(yrt_ticks_to_ns(123456789) == 123456789, "identity where the unit is the ns");
    CHECK(yrt_ticks_to_ns(-5) == -5, "identity for negatives");
    printf("  ticks to ns: identity (clock_gettime counts in ns)\n");
#endif
}

/* ---------------------------------------------------------------- version */

/* --------------------------------------------------------------- platform */

/* What the platform branch promises about the clock and the ladder. The
 * native half only reads (it must not elevate the test's own thread); the
 * Emscripten half calls every scheduling function, because there each one is
 * documented to refuse and none can change anything. */
static void test_platform(void) {
    yrt_clock_info ci;
    yrt_report rep;
    char line[256];
    memset(&ci, 0, sizeof(ci));
    yrt_get_clock_info(&ci);
    CHECK(ci.resolution_ns > 0, "the clock must report a resolution");
    yrt_report_get(&rep, YRT_POLICY_NORMAL);
    yrt_describe(&rep, line, sizeof(line));
#if defined(__EMSCRIPTEN__)
    CHECK_I(yrt_thread_elevate(NULL), YRT_POLICY_NORMAL,
            "wasm has no ladder: elevate returns NORMAL");
    CHECK(yrt_thread_pin(0) == YRT_ERR_UNSUPPORTED, "wasm cannot pin a thread");
    CHECK(yrt_thread_set_timer_slack(1) == YRT_ERR_UNSUPPORTED, "wasm has no timer slack");
    CHECK(yrt_process_lock_memory() == YRT_ERR_UNSUPPORTED, "wasm cannot lock memory");
    CHECK(yrt_timer_resolution_begin() == YRT_ERR_UNSUPPORTED, "wasm has no timer resolution");
    yrt_timer_resolution_end();
    CHECK(strstr(line, "platform=wasm ladder=none") != NULL,
          "yrt_describe must say the ladder is absent on wasm");
#else
    CHECK(strstr(line, "platform=") == NULL,
          "only the wasm line carries a platform field");
#endif
    printf("  platform: clock resolution %llu ns; %s\n",
           (unsigned long long)ci.resolution_ns, line);
}

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <sys/wait.h>
static int g_policy_seen = -1;
static bool policy_probe(void* ctx, char* err, size_t cap) {
    (void)ctx; (void)err; (void)cap;
    g_policy_seen = sched_getscheduler(0);
    return true;
}
static void policy_msg(void* ctx, const void* msg, uint32_t seq) { (void)ctx; (void)msg; (void)seq; }

/* Elevation below the FIFO 80 it asks for first, and what elevated threads
 * pass on. A user granted RLIMIT_RTPRIO 50 or less (limits.conf "rtprio")
 * must get SCHED_FIFO 1, not NORMAL; and a no_elevate worker or the
 * pump started by that FIFO thread must run SCHED_OTHER, not inherit FIFO.
 * In a child, so the lowered limit and the FIFO threads end with it. Exit
 * 0 pass, 1 to 4 a failed check, 5 skip (no grant), 6 skip (CAP_SYS_NICE). */
static void test_rtprio(void) {
    pid_t pid = fork();
    int status = 0, code;
    if (pid == 0) {
        struct rlimit rl;
        struct sched_param sp;
        yrt_worker w;
        yrt_worker_desc wd;
        yrt_pump pm;
        yrt_pump_desc pd;
        if (getrlimit(RLIMIT_RTPRIO, &rl) != 0 || rl.rlim_cur == 0) _exit(5);
        if (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > 10) rl.rlim_cur = 10;
        if (setrlimit(RLIMIT_RTPRIO, &rl) != 0) _exit(1);
        if (yrt_thread_elevate(NULL) != YRT_POLICY_FIFO) _exit(1);
        if (sched_getscheduler(0) != SCHED_FIFO || sched_getparam(0, &sp) != 0) _exit(1);
        if (sp.sched_priority == 80) _exit(6);
        if (sp.sched_priority != 1) _exit(1);
        memset(&w, 0, sizeof(w));
        memset(&wd, 0, sizeof(wd));
        wd.no_elevate = true;
        wd.on_start = policy_probe;
        g_policy_seen = -1;
        if (!yrt_worker_open(&w, &wd)) _exit(2);
        yrt_worker_close(&w);
        if (g_policy_seen != SCHED_OTHER) _exit(2);
        memset(&w, 0, sizeof(w));
        wd.no_elevate = false;
        g_policy_seen = -1;
        if (!yrt_worker_open(&w, &wd)) _exit(3);
        yrt_worker_close(&w);
        if (g_policy_seen != SCHED_FIFO) _exit(3);
        memset(&pm, 0, sizeof(pm));
        memset(&pd, 0, sizeof(pd));
        pd.msg_size = 4;
        pd.capacity = 1;
        pd.on_msg = policy_msg;
        pd.on_start = policy_probe;
        g_policy_seen = -1;
        if (!yrt_pump_open(&pm, &pd)) _exit(4);
        yrt_pump_close(&pm);
        _exit(g_policy_seen == SCHED_OTHER ? 0 : 4);
    }
    CHECK(pid > 0, "fork for the RLIMIT_RTPRIO checks");
    if (pid <= 0) return;
    waitpid(pid, &status, 0);
    code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    CHECK(code != 1, "with RLIMIT_RTPRIO 10, elevate must give SCHED_FIFO 1");
    CHECK(code != 2, "a no_elevate worker started by a FIFO thread must run SCHED_OTHER");
    CHECK(code != 3, "an elevating worker started by a FIFO thread must run SCHED_FIFO");
    CHECK(code != 4, "the pump started by a FIFO thread must run SCHED_OTHER");
    CHECK(code >= 0, "the RLIMIT_RTPRIO child must exit");
    printf("  rtprio: %s\n", code == 0 ? "FIFO 1 under RLIMIT_RTPRIO 10; workers and pump start SCHED_OTHER" :
           code == 5 ? "skipped (no rtprio granted)" : code == 6 ? "skipped (CAP_SYS_NICE: FIFO 80)" : "FAILED");
}
#endif

static void test_version(void) {
    char built[32];
    snprintf(built, sizeof(built), "%d.%d.%d", YRT_VERSION_MAJOR,
             YRT_VERSION_MINOR, YRT_VERSION_PATCH);
    CHECK(strcmp(built, YRT_VERSION_STRING) == 0,
          "YRT_VERSION_STRING must match the three numbers");
    CHECK(strcmp(yrt_version(), YRT_VERSION_STRING) == 0,
          "yrt_version() must return the header's string");
    printf("  version %s: ok\n", yrt_version());
}

#include "rt_test_fit.h"
#include "rt_test_userdir.h"

/* -------------------------------------------------------------------- main */

int main(void) {
    static unsigned char caller_ring[8 * sizeof(order_msg)];
    uint64_t t0 = yrt_now_ns();

    test_fit();
    test_user_dir();
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    test_rtprio();
#endif
#ifdef YRT_TEST_FIT_ONLY
    /* the mutation runs of tests/mutate/rt.toml: the fit, the per-user
     * folders and the rtprio checks alone, in seconds */
    printf("ysp_rt_pump_test: %s (%d failures, fit, folders and rtprio only)\n",
           g_failures ? "FAILED" : "all checks passed", g_failures);
    (void)caller_ring;
    return g_failures ? 1 : 0;
#endif

    printf("ysp_rt_pump_test: yrt_pump, %d-byte handle\n",
           (int)sizeof(yrt_pump));

    test_order(NULL, "inline");
    test_order(caller_ring, "caller");
    test_full();
    test_idle();
    test_stop(false, 20, "a drain must deliver every message");
    test_stop(true, 1, "drop_on_stop must keep only the message in flight");
    test_publish_lock();
    test_priority_and_start();
    test_stop_releases_wait();
    test_hammer_wait_vs_stop();
    test_edges();
    test_ring_basic();
    test_ring_stress(2);
    test_ring_stress(4);
    test_ring_stress(8);
    test_trace();
    test_correlate();
    test_ticks();
    test_throttle();
    test_core_types();
    test_platform();
    test_version();

    printf("ysp_rt_pump_test: %s (%d failures, %.1f s)\n",
           g_failures ? "FAILED" : "all checks passed", g_failures,
           (double)(yrt_now_ns() - t0) / 1e9);
    return g_failures ? 1 : 0;
}
