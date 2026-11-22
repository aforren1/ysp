/* worker_test.c - the designer's check thread (designer/core/worker.c;
 * docs/designer.md, "Plan of step 5", part T): newest wins for each kind of
 * job, a check before a waiting session job, a result marked with its
 * revision, a stale session dropped by the thread, the pack alive while a
 * session holds it after a rebuild, and a session closed on the thread
 * after another thread took it. The session is the stub behind the seam
 * (dsg_session_ops) until part A wires pl_sim_load(); its hooks here gate
 * the thread, so jobs pile up in a known order.
 *
 * Usage: test_designer_worker WORK. Also run under ThreadSanitizer. */
#include "dsg.h"
#include "doc.h"
#include "ysp/pack.h"
#include "ysp/rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef YSP_SOURCE_DIR
#define YSP_SOURCE_DIR "."
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char* work;
static const int64_t kWait = 60000000000LL;   /* 60 s: a sanitizer build is slow */

/* --- the stub session's hooks ---------------------------------------------- */

typedef struct hooks {
    dsg_worker*      w;
    volatile int32_t gate;          /* 1: a load waits until it is 0      */
    volatile int32_t in_load;       /* loads entered                      */
    volatile int32_t loads, closes;
    volatile int32_t fail_next;
    int64_t          checks_at_load[16];   /* stats.checks when each load began */
    int32_t          closes_at_load[16];   /* closes when each load began        */
    uint64_t         load_rev[16];
    uint64_t         close_rev[16];
    uint32_t         close_thread[16];
    uint8_t          close_sum[16];        /* the pack's bytes summed at the close */
    int32_t          n_impl;               /* stub sessions alive              */
} hooks;

static uint8_t pack_sum(const dsg_pack* pk) {
    size_t n = 0, i;
    const uint8_t* b = (const uint8_t*)dsg_pack_bytes(pk, &n);
    uint8_t s = 0;
    for (i = 0; b && i < n; i++) s = (uint8_t)(s + b[i]);
    return s;
}

static int h_load(void* ctx, dsg_session* s, const char* text, size_t n, char* err, size_t cap) {
    hooks* h = (hooks*)ctx;
    const int32_t k = dsg_atomic_add(&h->loads, 1) - 1;
    dsg_worker_stats st;
    memset(&st, 0, sizeof st);
    dsg_worker_get_stats(h->w, &st);
    if (k < 16) {
        h->checks_at_load[k] = st.checks;
        h->closes_at_load[k] = dsg_atomic_load(&h->closes);
        h->load_rev[k] = s->revision;
    }
    dsg_atomic_add(&h->in_load, 1);
    while (dsg_atomic_load(&h->gate)) yrt_sleep_ns(1000000);   /* a test gate, not the worker's wait */
    /* the stub reads the whole pack, as pl_sim_load() would (ASan) */
    if (s->pack) (void)pack_sum(s->pack);
    if (n == 0 || !text) {
        snprintf(err, cap, "no text");
        return -1;
    }
    if (dsg_atomic_load(&h->fail_next)) {
        dsg_atomic_add(&h->fail_next, -1);
        snprintf(err, cap, "the stub refused");
        return -1;
    }
    s->impl = malloc(64);   /* LeakSanitizer sees a session never closed */
    dsg_atomic_add(&h->n_impl, 1);
    return 0;
}

static void h_close(void* ctx, dsg_session* s) {
    hooks* h = (hooks*)ctx;
    const int32_t k = dsg_atomic_add(&h->closes, 1) - 1;
    if (k < 16) {
        h->close_rev[k] = s->revision;
        h->close_thread[k] = yrt_thread_id();
        h->close_sum[k] = s->pack ? pack_sum(s->pack) : (uint8_t)0;
    }
    if (s->impl) {
        free(s->impl);
        s->impl = NULL;
        dsg_atomic_add(&h->n_impl, -1);
    }
}

static void wait_in_load(hooks* h, int32_t k) {
    int i;
    for (i = 0; i < 60000 && dsg_atomic_load(&h->in_load) < k; i++) yrt_sleep_ns(1000000);
}

static volatile int32_t wakes;
static void on_wake(void* ctx) {
    (void)ctx;
    dsg_atomic_add(&wakes, 1);
}

/* --- the projects ----------------------------------------------------------- */

static int copy_file(const char* from, const char* to) {
    char* d;
    size_t n;
    char err[512];
    if (dsg_read_file(from, &d, &n, err, sizeof err)) { printf("  %s\n", err); return -1; }
    if (dsg_write_file(to, d, n, err, sizeof err)) { printf("  %s\n", err); free(d); return -1; }
    free(d);
    return 0;
}

static int project(const char* src_name, const char* name, dsg_project* p) {
    static const char* const files[] = { "pack.json", "experiment.json", "conditions.csv" };
    char src[DSG_PATH_MAX], dir[DSG_PATH_MAX], a[DSG_PATH_MAX], b[DSG_PATH_MAX], err[512];
    int i;
    snprintf(src, sizeof src, "%s/tests/player/%s", YSP_SOURCE_DIR, src_name);
    if (dsg_join(dir, sizeof dir, work, name) || dsg_make_dir(dir, err, sizeof err)) return -1;
    for (i = 0; i < 3; i++)
        if (dsg_join(a, sizeof a, src, files[i]) || dsg_join(b, sizeof b, dir, files[i]) || copy_file(a, b)) return -1;
    if (dsg_project_open(p, dir, err, sizeof err)) {
        printf("  %s\n", err);
        return -1;
    }
    return 0;
}

/* The definition with n spaces before its first brace's end: the same
 * definition, another text. */
static char* spaced(const dsg_project* p, int k, size_t* n) {
    char* t = (char*)malloc(p->text_len + (size_t)k + 1);
    if (!t) return NULL;
    memset(t, ' ', (size_t)k);
    memcpy(t + k, p->text, p->text_len);
    *n = p->text_len + (size_t)k;
    t[*n] = 0;
    return t;
}

int main(int argc, char** argv) {
    char err[512];
    dsg_project p1, p2;
    hooks h;
    dsg_worker_desc wd;
    dsg_worker* w;
    dsg_worker_stats st;
    const dsg_check_result* cr = NULL;
    dsg_session_result sr;
    dsg_session_settings set;
    dsg_pack* pk1 = NULL;
    uint8_t sum1 = 0;
    int i;
    if (argc < 2) {
        printf("usage: test_designer_worker WORK\n");
        return 2;
    }
    work = argv[1];
    memset(&p1, 0, sizeof p1);
    memset(&p2, 0, sizeof p2);
    if (dsg_make_dir(work, err, sizeof err) || project("rdk", "rdk_a", &p1) || project("rdk", "rdk_b", &p2)) {
        printf("FAIL: the work folder: %s\n", err);
        return 1;
    }
    memset(&h, 0, sizeof h);
    memset(&wd, 0, sizeof wd);
    memset(&st, 0, sizeof st);
    memset(&sr, 0, sizeof sr);
    memset(&set, 0, sizeof set);
    set.seed = 7;
    set.pin_row = -1;
    wd.wake = on_wake;
    wd.session.load = h_load;
    wd.session.close = h_close;
    wd.session.ctx = &h;
    w = dsg_worker_start(&wd, err, sizeof err);
    if (!w) {
        printf("FAIL: start: %s\n", err);
        return 1;
    }
    h.w = w;
    CHECK(!dsg_worker_take_check(w, &cr), "nothing to take at the start");

    /* 1. a result marked with its revision, and a wake-up for it */
    CHECK(dsg_worker_check(w, &p1, p1.text, p1.text_len, NULL, 7, NULL) == 0, "post");
    CHECK(dsg_worker_wait(w, kWait) == 0, "the check ran");
    CHECK(dsg_worker_take_check(w, &cr) && cr->revision == 7 && cr->report.status == DSG_CHECK_OK && cr->pack != NULL,
          "the result: revision %llu, status %d", cr ? (unsigned long long)cr->revision : 0ull, cr ? cr->report.status : -9);
    CHECK(cr && cr->report.built && cr->report.have_pack, "the first check built the pack");
    CHECK(dsg_atomic_load(&wakes) == 1, "one wake-up for one result (%d)", (int)dsg_atomic_load(&wakes));
    CHECK(!dsg_worker_take_check(w, &cr), "taken once");
    pk1 = cr ? cr->pack : NULL;
    sum1 = pack_sum(pk1);

    /* 2. newest wins for checks, a check before a waiting session job, and a
     *    finished session dropped by the thread when a newer job waits */
    dsg_atomic_add(&h.gate, 1);
    CHECK(dsg_worker_session(w, p1.text, p1.text_len, pk1, 20, &set) == 0, "post session 20");
    wait_in_load(&h, 1);
    CHECK(dsg_atomic_load(&h.in_load) == 1, "session 20 is loading");
    CHECK(dsg_worker_session_queued(w), "a session job runs: queued");
    CHECK(dsg_worker_session(w, p1.text, p1.text_len, pk1, 21, &set) == 0, "post session 21 behind it");
    for (i = 0; i < 10; i++) {
        size_t n = 0;
        char* t = spaced(&p1, i, &n);
        CHECK(t && dsg_worker_check(w, &p1, t, n, NULL, (uint64_t)(100 + i), NULL) == 0, "post check %d", 100 + i);
        free(t);   /* the post copied it */
    }
    dsg_atomic_add(&h.gate, -1);
    CHECK(dsg_worker_wait(w, kWait) == 0, "the burst ran");
    dsg_worker_get_stats(w, &st);
    CHECK(st.checks == 2 && st.checks_replaced == 9, "ten checks in a burst: one ran (%lld runs, %lld replaced)", (long long)st.checks,
          (long long)st.checks_replaced);
    CHECK(dsg_worker_take_check(w, &cr) && cr->revision == 109 && cr->report.status == DSG_CHECK_OK,
          "the result is the last text's (revision %llu)", cr ? (unsigned long long)cr->revision : 0ull);
    CHECK(h.loads == 2 && h.load_rev[1] == 21 && h.checks_at_load[1] == 2, "the check ran before session 21 (checks at its load: %lld)",
          (long long)h.checks_at_load[1]);
    CHECK(st.sessions_dropped == 1 && h.closes == 1 && h.close_rev[0] == 20, "session 20 was dropped on the thread (dropped %lld)",
          (long long)st.sessions_dropped);
    CHECK(h.close_thread[0] != yrt_thread_id(), "the drop closed it on the check thread");
    CHECK(h.closes_at_load[1] == 1, "before session 21 loaded: it was never posted (%d closes then)", (int)h.closes_at_load[1]);
    CHECK(dsg_worker_take_session(w, &sr) && sr.session && sr.revision == 21 && sr.session->revision == 21 && sr.session->pack == pk1,
          "session 21 arrived with its revision and its pack");
    CHECK(!dsg_worker_session_queued(w), "no session job left");
    CHECK(sr.session && sr.session->settings.seed == 7, "with its settings");

    /* 3. the pack lives while a session holds it after a rebuild */
    {
        dsg_session* s21 = sr.session;
        const dsg_check_result* c2 = NULL;
        CHECK(dsg_worker_check(w, &p2, p2.text, p2.text_len, NULL, 200, NULL) == 0, "another project: a rebuild");
        CHECK(dsg_worker_wait(w, kWait) == 0 && dsg_worker_take_check(w, &c2) && c2->report.built && c2->pack != pk1,
              "the check built a new pack");
        /* three more results rotate every result's slot, so only s21 holds
         * the first pack */
        for (i = 0; i < 3; i++) {
            dsg_worker_check(w, &p2, p2.text, p2.text_len, NULL, (uint64_t)(201 + i), NULL);
            dsg_worker_wait(w, kWait);
            dsg_worker_take_check(w, &c2);
        }
        CHECK(pack_sum(s21->pack) == sum1, "the old pack's bytes are whole (ASan: not freed)");
        {
            ypak_desc pd;
            ypak_pack pp;
            size_t sz = 0;
            memset(&pd, 0, sizeof pd);
            memset(&pp, 0, sizeof pp);
            pd.data = dsg_pack_bytes(s21->pack, &sz);
            pd.size = sz;
            CHECK(ypak_open(&pp, &pd), "and it opens as a pack");
            ypak_close(&pp);
        }
        CHECK(dsg_pack_live() == 2, "two packs live: the checker's and the session's (%d)", (int)dsg_pack_live());

        /* 4. closed on the thread, after this thread took it (detached) */
        dsg_worker_close(w, s21);
        CHECK(dsg_worker_wait(w, kWait) == 0 && h.closes == 2 && h.close_rev[1] == 21, "session 21 closed");
        CHECK(h.close_thread[1] != yrt_thread_id(), "on the check thread");
        CHECK(h.close_sum[1] == sum1, "its pack was still whole at the close");
        CHECK(dsg_pack_live() == 1, "the old pack is freed after the close (%d live)", (int)dsg_pack_live());
    }

    /* 5. newest wins for sessions, and the replaced jobs' packs go */
    {
        const dsg_check_result* c3 = NULL;
        dsg_pack* pk2;
        dsg_worker_check(w, &p2, p2.text, p2.text_len, NULL, 300, NULL);
        dsg_worker_wait(w, kWait);
        CHECK(dsg_worker_take_check(w, &c3) && c3->pack, "a result with the current pack");
        pk2 = c3 ? c3->pack : NULL;
        dsg_atomic_add(&h.gate, 1);
        dsg_worker_session(w, p2.text, p2.text_len, pk2, 301, &set);
        wait_in_load(&h, 3);
        for (i = 0; i < 4; i++) dsg_worker_session(w, p2.text, p2.text_len, pk2, (uint64_t)(302 + i), &set);
        dsg_atomic_add(&h.gate, -1);
        CHECK(dsg_worker_wait(w, kWait) == 0, "the sessions ran");
        dsg_worker_get_stats(w, &st);
        CHECK(st.sessions_replaced == 3 && h.loads == 4 && h.load_rev[3] == 305, "five session jobs: 301 and the newest, 305, loaded (%lld replaced)",
              (long long)st.sessions_replaced);
        CHECK(st.sessions_dropped == 2 && h.close_rev[2] == 301, "301 was dropped");
        CHECK(dsg_worker_take_session(w, &sr) && sr.session && sr.revision == 305, "305 arrived");
        if (sr.session) dsg_worker_close(w, sr.session);
        dsg_worker_wait(w, kWait);
        CHECK(h.n_impl == 0, "every stub session closed (%d open)", (int)h.n_impl);
    }

    /* 6. a load that fails posts its error, and a session never taken is
     *    dropped when a newer one arrives */
    {
        const dsg_check_result* c4 = NULL;
        dsg_worker_check(w, &p2, p2.text, p2.text_len, NULL, 400, NULL);
        dsg_worker_wait(w, kWait);
        dsg_worker_take_check(w, &c4);
        dsg_atomic_add(&h.fail_next, 1);
        dsg_worker_session(w, p2.text, p2.text_len, c4 ? c4->pack : NULL, 401, &set);
        dsg_worker_wait(w, kWait);
        CHECK(dsg_worker_take_session(w, &sr) && !sr.session && sr.revision == 401 && strstr(sr.error, "stub refused"),
              "the failed load's error: '%s'", sr.error);
        dsg_worker_session(w, p2.text, p2.text_len, c4 ? c4->pack : NULL, 402, &set);
        dsg_worker_wait(w, kWait);
        dsg_worker_session(w, p2.text, p2.text_len, c4 ? c4->pack : NULL, 403, &set);
        dsg_worker_wait(w, kWait);
        CHECK(dsg_worker_take_session(w, &sr) && sr.session && sr.revision == 403, "the newest session is the one taken");
        CHECK(h.n_impl == 1, "402, never taken, was closed (%d open)", (int)h.n_impl);
        if (sr.session) dsg_worker_close(w, sr.session);
    }

    /* 7. the dirty tables are a copy taken at the post */
    {
        dsg_doc* d = dsg_doc_new();
        const dsg_check_result* c5 = NULL;
        char csv[DSG_PATH_MAX];
        char* tb = NULL;
        size_t tn = 0;
        int b;
        dsg_join(csv, sizeof csv, p1.folder, "conditions.csv");
        CHECK(d && dsg_doc_reset(d, p1.def_source, p1.text, p1.text_len) == 0 && dsg_read_file(csv, &tb, &tn, err, sizeof err) == 0,
              "a document");
        b = d ? dsg_doc_add_buffer(d, DSG_KIND_TABLE, "conditions.csv", tb, tn) : -1;
        /* a table that the player refuses: a column the definition reads is gone */
        CHECK(b > 0 && dsg_doc_replace(d, b, "x,y\n1,2\n", 8, 0) == 0 && dsg_doc_dirty(d, b), "an edited table");
        CHECK(dsg_worker_check(w, &p1, p1.text, p1.text_len, d, 500, NULL) == 0, "posted with the table");
        /* the document moves on before the thread reads it */
        if (b > 0) dsg_doc_replace(d, b, tb, tn, 0);
        CHECK(dsg_worker_wait(w, kWait) == 0 && dsg_worker_take_check(w, &c5) && c5->revision == 500, "the check with the table ran");
        /* the pack tool refuses the edited table: the build read the copy */
        CHECK(c5 && c5->report.n_errors > 0 && c5->report.issues[0].from == DSG_FROM_PACK && strstr(c5->report.issues[0].message, "key_answer"),
              "it read the copy, not the document (%d errors: '%s')", c5 ? c5->report.n_errors : -1,
              c5 && c5->report.n_issues ? c5->report.issues[0].message : "");
        free(tb);
        dsg_doc_free(d);
    }

    /* 8. a stop with jobs waiting and a session never taken frees them */
    dsg_atomic_add(&h.gate, 1);
    dsg_worker_session(w, p1.text, p1.text_len, NULL, 600, &set);
    wait_in_load(&h, 8);
    dsg_worker_session(w, p1.text, p1.text_len, NULL, 601, &set);
    dsg_worker_check(w, &p1, p1.text, p1.text_len, NULL, 602, NULL);
    dsg_atomic_add(&h.gate, -1);
    dsg_worker_stop(w);
    CHECK(h.n_impl == 0, "the stop closed every session (%d open)", (int)h.n_impl);
    CHECK(dsg_pack_live() == 0, "and freed every pack (%d live)", (int)dsg_pack_live());

    /* 9. no thread: the jobs run in the wait, on this thread */
    {
        dsg_worker* w2;
        const dsg_check_result* c6 = NULL;
        memset(&h, 0, sizeof h);
        wd.no_thread = true;
        w2 = dsg_worker_start(&wd, err, sizeof err);
        CHECK(w2 != NULL, "start without a thread: %s", err);
        if (w2) {
            h.w = w2;
            dsg_worker_check(w2, &p1, p1.text, p1.text_len, NULL, 700, NULL);
            CHECK(!dsg_worker_take_check(w2, &c6), "nothing before the wait");
            dsg_worker_wait(w2, 0);
            CHECK(dsg_worker_take_check(w2, &c6) && c6->revision == 700 && c6->report.status == DSG_CHECK_OK, "the wait ran it");
            dsg_worker_session(w2, p1.text, p1.text_len, c6 ? c6->pack : NULL, 701, &set);
            dsg_worker_wait(w2, 0);
            CHECK(dsg_worker_take_session(w2, &sr) && sr.session && h.close_thread[0] == 0, "a session, loaded inline");
            if (sr.session) dsg_worker_close(w2, sr.session);
            dsg_worker_wait(w2, 0);
            CHECK(h.closes == 1 && h.close_thread[0] == yrt_thread_id(), "closed inline, on this thread");
            dsg_worker_stop(w2);
            CHECK(dsg_pack_live() == 0, "every pack freed (%d live)", (int)dsg_pack_live());
        }
    }

    dsg_project_close(&p1);
    dsg_project_close(&p2);
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails ? 1 : 0;
}
