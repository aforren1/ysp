/* worker.c - the designer's check thread (docs/designer.md, "Plan of step
 * 5", "The check thread and the session's load"). bimanual's check takes up
 * to 12.9 ms and a session's load costs more, so neither fits beside the
 * UI's own work in a 16 ms frame: both run here, and the UI thread only
 * posts jobs and takes results.
 *
 * The thread is a yrt_pump (ysp/rt.h). Its messages are only wake-ups: the
 * jobs live in slots here, one per kind, so a job posted while one of its
 * kind waits replaces it (newest wins), which a FIFO of jobs would not do.
 * The pump's publish lock guards the slots; no job runs under it.
 *
 * Each slot is a set of buffers that are swapped, not copied, between the
 * poster's stage, the waiting job and the running job, so a post allocates
 * only when a buffer must grow. Results rotate the same way. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "dsg.h"
#include "doc.h"
#include "player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

/* A browser build without threads (no cross-origin isolation) runs the
 * jobs on the UI thread (docs/designer.md: step 7 decides). */
#if defined(YRT_NO_THREADS) || (defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__))
#define DSG_WORKER_NO_THREADS 1
#endif

/* --- buffers that grow and are kept ------------------------------------------ */

static int grow(void** p, size_t* cap, size_t need, size_t elem) {
    size_t nc;
    void* np;
    if (need <= *cap) return 0;
    nc = *cap ? *cap : 256;
    while (nc < need) nc *= 2;
    np = realloc(*p, nc * elem);
    if (!np) return -1;
    *p = np;
    *cap = nc;
    return 0;
}

/* --- the jobs ------------------------------------------------------------------ */

/* A check job: what dsg_check_tables() reads, owned by the slot. */
typedef struct check_job {
    uint64_t        revision;
    dsg_project     proj;        /* folder, desc_path, desc (into desc_buf) */
    char*           desc_buf;
    size_t          desc_cap;
    char*           text;
    size_t          n, text_cap;
    dsg_table_bytes tables[DSG_MAX_BUFFERS];
    int             n_tables;
    size_t          name_off[DSG_MAX_BUFFERS], bytes_off[DSG_MAX_BUFFERS];
    char*           tb;          /* the tables' names and bytes             */
    size_t          tb_n, tb_cap;
} check_job;

typedef struct session_job {
    uint64_t             revision;
    char*                text;
    size_t               n, text_cap;
    dsg_pack*            pack;   /* the job's own reference                */
    dsg_session_settings settings;
} session_job;

/* A check result with the storage its report points into. */
typedef struct check_out {
    dsg_check_result pub;
    dsg_issue*       issues;
    size_t           issues_cap;
    char*            strs;
    size_t           strs_cap;
} check_out;

struct dsg_worker {
    dsg_worker_desc desc;
#if !defined(DSG_WORKER_NO_THREADS)
    yrt_pump        pump;
#endif
    bool            threaded;
    uint32_t        last_seq;    /* the poster's last wake                  */
    dsg_checker*    checker;     /* the check thread's alone                */

    /* --- under the pump's publish lock --- */
    check_job*      check_stage;     /* the poster fills it, then swaps     */
    check_job*      check_wait;
    check_job*      check_run;
    bool            have_check;
    bool            forget;
    session_job*    sess_stage;
    session_job*    sess_wait;
    session_job*    sess_run;
    bool            have_session;
    bool            session_running;
    dsg_session*    close_list;
    check_out*      out_work;        /* the thread fills it                 */
    check_out*      out_ready;
    check_out*      out_ui;          /* the UI's, until its next take       */
    bool            have_out;
    dsg_session_result sres_ready;
    bool            have_sres;
    bool            stopping;
    dsg_worker_stats stats;

    check_job       check_slots[3];
    session_job     sess_slots[3];
    check_out       outs[3];
};

#if !defined(DSG_WORKER_NO_THREADS)
static void lock(dsg_worker* w) {
    if (w->threaded) yrt_pump_lock(&w->pump);
}
static void unlock(dsg_worker* w) {
    if (w->threaded) yrt_pump_unlock(&w->pump);
}

static void wake_thread(dsg_worker* w) {
    int seq;
    if (!w->threaded) return;
    seq = yrt_pump_submit(&w->pump, "w");
    /* Full: a wake that has not started yet is in the ring, and it takes
     * this job too, so last_seq still covers it. */
    if (seq > 0) w->last_seq = (uint32_t)seq;
}
#else
static void lock(dsg_worker* w) { (void)w; }
static void unlock(dsg_worker* w) { (void)w; }
static void wake_thread(dsg_worker* w) { (void)w; }
#endif

/* --- copies into a slot ------------------------------------------------------ */

static int fill_check(check_job* j, const dsg_project* p, const char* text, size_t n, const dsg_doc* d, uint64_t revision) {
    int b;
    size_t at = 0;
    j->revision = revision;
    if (grow((void**)&j->text, &j->text_cap, n + 1, 1)) return -1;
    memcpy(j->text, text, n);
    j->text[n] = 0;
    j->n = n;
    snprintf(j->proj.folder, sizeof j->proj.folder, "%s", p->folder);
    snprintf(j->proj.desc_path, sizeof j->proj.desc_path, "%s", p->desc_path);
    if (grow((void**)&j->desc_buf, &j->desc_cap, p->desc_len + 1, 1)) return -1;
    if (p->desc_len) memcpy(j->desc_buf, p->desc, p->desc_len);
    j->desc_buf[p->desc_len] = 0;
    j->proj.desc = j->desc_buf;
    j->proj.desc_len = p->desc_len;
    /* the dirty tables: their names and bytes into one buffer */
    j->n_tables = 0;
    for (b = 1; d && b < dsg_doc_buffer_count(d) && j->n_tables < DSG_MAX_BUFFERS; b++) {
        const char* nm;
        const char* bytes;
        size_t nn, bn = 0;
        if (dsg_doc_buffer_kind(d, b) != DSG_KIND_TABLE || !dsg_doc_dirty(d, b)) continue;
        nm = dsg_doc_buffer_name(d, b);
        bytes = dsg_doc_bytes(d, b, &bn);
        nn = strlen(nm) + 1;
        if (grow((void**)&j->tb, &j->tb_cap, at + nn + bn, 1)) return -1;
        memcpy(j->tb + at, nm, nn);
        j->name_off[j->n_tables] = at;
        at += nn;
        if (bn) memcpy(j->tb + at, bytes, bn);
        j->bytes_off[j->n_tables] = at;
        at += bn;
        j->tables[j->n_tables].buf = b;
        j->tables[j->n_tables].rev = dsg_doc_rev(d, b);
        j->tables[j->n_tables].n = bn;
        j->n_tables++;
    }
    j->tb_n = at;
    /* the pointers last: the buffer may have moved while it grew */
    for (b = 0; b < j->n_tables; b++) {
        j->tables[b].name = j->tb + j->name_off[b];
        j->tables[b].bytes = j->tb + j->bytes_off[b];
    }
    return 0;
}

/* The report into out's storage; the checker reuses its own at the next
 * check. */
static int copy_report(check_out* o, const dsg_report* r) {
    size_t strs = 0, at = 0;
    int i;
    for (i = 0; i < r->n_issues; i++) {
        strs += strlen(r->issues[i].message) + 1;
        if (r->issues[i].name) strs += strlen(r->issues[i].name) + 1;
    }
    if (grow((void**)&o->issues, &o->issues_cap, (size_t)(unsigned)r->n_issues + 1, sizeof *o->issues) ||
        grow((void**)&o->strs, &o->strs_cap, strs + 1, 1))
        return -1;
    o->pub.report = *r;
    o->pub.report.issues = o->issues;
    for (i = 0; i < r->n_issues; i++) {
        dsg_issue* is = &o->issues[i];
        size_t k;
        *is = r->issues[i];
        k = strlen(r->issues[i].message) + 1;
        memcpy(o->strs + at, r->issues[i].message, k);
        is->message = o->strs + at;
        at += k;
        if (r->issues[i].name) {
            k = strlen(r->issues[i].name) + 1;
            memcpy(o->strs + at, r->issues[i].name, k);
            is->name = o->strs + at;
            at += k;
        }
    }
    return 0;
}

/* --- on the thread ------------------------------------------------------------- */

static void session_free(dsg_worker* w, dsg_session* s) {
    if (w->desc.session.close) w->desc.session.close(w->desc.session.ctx, s);
    dsg_pack_release(s->pack);
    free(s);
}

static void run_check(dsg_worker* w) {
    check_job* j = w->check_run;
    check_out* o = w->out_work;
    dsg_report r;
    bool forget;
    memset(&r, 0, sizeof r);
    lock(w);
    forget = w->forget;
    w->forget = false;
    unlock(w);
    if (forget) dsg_checker_forget(w->checker);
    dsg_check_tables(w->checker, &j->proj, j->text, j->n, j->tables, j->n_tables, &r);
    /* the result's last owner let go of it: its pack goes now */
    dsg_pack_release(o->pub.pack);
    o->pub.pack = NULL;
    o->pub.revision = j->revision;
    if (copy_report(o, &r) != 0) {
        memset(&o->pub.report, 0, sizeof o->pub.report);
        o->pub.report.status = DSG_CHECK_NONE;
    }
    o->pub.pack = dsg_pack_retain(dsg_checker_pack(w->checker));
    lock(w);
    w->out_work = w->out_ready;
    w->out_ready = o;
    w->have_out = true;
    w->stats.checks++;
    unlock(w);
    if (w->desc.wake) w->desc.wake(w->desc.ctx);
}

static void run_session(dsg_worker* w) {
    session_job* j = w->sess_run;
    dsg_session* s = (dsg_session*)calloc(1, sizeof *s);
    dsg_session* stale = NULL;
    dsg_session_result res;
    bool dropped = false;
    memset(&res, 0, sizeof res);
    res.revision = j->revision;
    if (!s) {
        snprintf(res.error, sizeof res.error, "out of memory for the session");
        dsg_pack_release(j->pack);
    } else {
        s->revision = j->revision;
        s->settings = j->settings;
        s->pack = j->pack;   /* the job's reference goes with the session */
        if (w->desc.session.load && w->desc.session.load(w->desc.session.ctx, s, j->text, j->n, res.error, sizeof res.error) != 0) {
            if (!res.error[0]) snprintf(res.error, sizeof res.error, "the session did not load");
            session_free(w, s);
            s = NULL;
        }
    }
    j->pack = NULL;
    lock(w);
    w->session_running = false;
    if (s && w->have_session) {
        /* a newer job waits: this session would be stale on arrival, so it
         * never reaches the UI and never makes a GL object */
        dropped = true;
        w->stats.sessions_dropped++;
    } else {
        if (w->have_sres && w->sres_ready.session) stale = w->sres_ready.session;   /* never taken */
        res.session = s;
        w->sres_ready = res;
        w->have_sres = true;
        if (s) w->stats.sessions_loaded++;
        else w->stats.sessions_failed++;
        if (stale) w->stats.sessions_dropped++;
    }
    unlock(w);
    if (dropped) session_free(w, s);
    if (stale) session_free(w, stale);
    if (!dropped && w->desc.wake) w->desc.wake(w->desc.ctx);
    if (dropped || stale) {
        lock(w);
        w->stats.sessions_closed += (dropped ? 1 : 0) + (stale ? 1 : 0);
        unlock(w);
    }
}

static void run_closes(dsg_worker* w, dsg_session* list) {
    int64_t k = 0;
    while (list) {
        dsg_session* next = list->next;
        session_free(w, list);
        list = next;
        k++;
    }
    lock(w);
    w->stats.sessions_closed += k;
    unlock(w);
}

/* Every job that waits, a check first: the check gives the issues, and a
 * session job's text was checked already. */
static void run_jobs(dsg_worker* w) {
    for (;;) {
        int kind = 0;
        dsg_session* closes = NULL;
        lock(w);
        if (w->stopping) {
            unlock(w);
            return;
        }
        if (w->have_check) {
            check_job* t = w->check_run;
            w->check_run = w->check_wait;
            w->check_wait = t;
            w->have_check = false;
            kind = 1;
        } else if (w->close_list) {
            closes = w->close_list;
            w->close_list = NULL;
            kind = 2;
        } else if (w->have_session) {
            session_job* t = w->sess_run;
            w->sess_run = w->sess_wait;
            w->sess_wait = t;
            w->have_session = false;
            w->session_running = true;
            kind = 3;
        }
        unlock(w);
        if (kind == 1) run_check(w);
        else if (kind == 2) run_closes(w, closes);
        else if (kind == 3) run_session(w);
        else return;
    }
}

#if !defined(DSG_WORKER_NO_THREADS)
static void on_msg(void* ctx, const void* msg, uint32_t seq) {
    (void)msg;
    (void)seq;
    run_jobs((dsg_worker*)ctx);
}
#endif

/* --- the API ------------------------------------------------------------------- */

dsg_worker* dsg_worker_start(const dsg_worker_desc* d, char* err, size_t cap) {
    dsg_worker* w = (dsg_worker*)calloc(1, sizeof *w);
    int i;
    if (!w) {
        snprintf(err, cap, "out of memory for the check thread");
        return NULL;
    }
    if (d) w->desc = *d;
    for (i = 0; i < 3; i++) w->outs[i].pub.report.status = DSG_CHECK_NONE;
    w->check_stage = &w->check_slots[0];
    w->check_wait = &w->check_slots[1];
    w->check_run = &w->check_slots[2];
    w->sess_stage = &w->sess_slots[0];
    w->sess_wait = &w->sess_slots[1];
    w->sess_run = &w->sess_slots[2];
    w->out_work = &w->outs[0];
    w->out_ready = &w->outs[1];
    w->out_ui = &w->outs[2];
    w->checker = dsg_checker_new();
    if (!w->checker) {
        snprintf(err, cap, "out of memory for the checker");
        free(w);
        return NULL;
    }
    /* gfx builds its NOISE GAUSSIAN table at its first use in the process,
     * and that build is not thread safe; pl_check() opens a gfx on this
     * worker. Build it here, before the thread exists. */
    (void)ygfx_noise_value(0, 0, 0u, YGFX_GAUSSIAN);
#if defined(__GLIBC__)
    /* glibc gives the thread an arena of its own and gives its freed top
     * back to the kernel after each check, so the next check faults the
     * pages in again: a "many" keystroke took 6.9 ms on the thread against
     * 5.8 ms in the frame. A fixed 64 MB threshold keeps them: 5.1 ms
     * (docs/designer.md, "Part T as built"). Process wide; set once. */
    (void)mallopt(M_TRIM_THRESHOLD, 64 << 20);
#endif
#if !defined(DSG_WORKER_NO_THREADS)
    if (!w->desc.no_thread) {
        yrt_pump_desc pd;
        memset(&pd, 0, sizeof pd);
        pd.msg_size = 1;
        pd.capacity = 16;
        pd.on_msg = on_msg;
        pd.ctx = w;
        /* a stop drops the waiting wake-ups: their jobs are dropped too */
        pd.drop_on_stop = true;
        if (!yrt_pump_open(&w->pump, &pd)) {
            snprintf(err, cap, "the check thread: %s", yrt_pump_error(&w->pump));
            dsg_checker_free(w->checker);
            free(w);
            return NULL;
        }
        w->threaded = true;
    }
#endif
    return w;
}

void dsg_worker_stop(dsg_worker* w) {
    int i;
    dsg_session* closes;
    if (!w) return;
    lock(w);
    w->stopping = true;
    unlock(w);
#if !defined(DSG_WORKER_NO_THREADS)
    if (w->threaded) yrt_pump_close(&w->pump);
#endif
    w->threaded = false;
    /* the thread is gone: everything left is this thread's */
    closes = w->close_list;
    w->close_list = NULL;
    if (w->have_sres && w->sres_ready.session) {
        w->sres_ready.session->next = closes;
        closes = w->sres_ready.session;
    }
    w->stopping = false;
    run_closes(w, closes);
    for (i = 0; i < 3; i++) {
        check_job* c = &w->check_slots[i];
        session_job* s = &w->sess_slots[i];
        free(c->text);
        free(c->desc_buf);
        free(c->tb);
        free(s->text);
        dsg_pack_release(s->pack);
        dsg_pack_release(w->outs[i].pub.pack);
        free(w->outs[i].issues);
        free(w->outs[i].strs);
    }
    dsg_checker_free(w->checker);
    free(w);
}

int dsg_worker_check(dsg_worker* w, const dsg_project* p, const char* text, size_t n, const dsg_doc* tables, uint64_t revision) {
    check_job* t;
    if (fill_check(w->check_stage, p, text, n, tables, revision) != 0) return -1;
    lock(w);
    t = w->check_wait;
    w->check_wait = w->check_stage;
    w->check_stage = t;
    if (w->have_check) w->stats.checks_replaced++;
    w->have_check = true;
    unlock(w);
    wake_thread(w);
    return 0;
}

void dsg_worker_forget(dsg_worker* w) {
    lock(w);
    w->forget = true;
    unlock(w);
}

int dsg_worker_session(dsg_worker* w, const char* text, size_t n, dsg_pack* pack, uint64_t revision, const dsg_session_settings* s) {
    session_job* j = w->sess_stage;
    session_job* t;
    dsg_pack* replaced = NULL;
    if (grow((void**)&j->text, &j->text_cap, n + 1, 1)) return -1;
    memcpy(j->text, text, n);
    j->text[n] = 0;
    j->n = n;
    j->revision = revision;
    j->pack = dsg_pack_retain(pack);
    if (s) j->settings = *s;
    else memset(&j->settings, 0, sizeof j->settings);
    lock(w);
    t = w->sess_wait;
    w->sess_wait = j;
    w->sess_stage = t;
    if (w->have_session) {
        replaced = t->pack;   /* the job it replaced never runs */
        t->pack = NULL;
        w->stats.sessions_replaced++;
    }
    w->have_session = true;
    unlock(w);
    dsg_pack_release(replaced);
    wake_thread(w);
    return 0;
}

void dsg_worker_close(dsg_worker* w, dsg_session* s) {
    if (!s) return;
    lock(w);
    s->next = w->close_list;
    w->close_list = s;
    unlock(w);
    wake_thread(w);
}

bool dsg_worker_take_check(dsg_worker* w, const dsg_check_result** out) {
    bool got = false;
    lock(w);
    if (w->have_out) {
        check_out* t = w->out_ui;
        w->out_ui = w->out_ready;
        w->out_ready = t;
        w->have_out = false;
        got = true;
    }
    unlock(w);
    if (got && out) *out = &w->out_ui->pub;
    return got;
}

bool dsg_worker_take_session(dsg_worker* w, dsg_session_result* out) {
    bool got = false;
    lock(w);
    if (w->have_sres) {
        *out = w->sres_ready;
        memset(&w->sres_ready, 0, sizeof w->sres_ready);
        w->have_sres = false;
        got = true;
    }
    unlock(w);
    return got;
}

bool dsg_worker_session_queued(dsg_worker* w) {
    bool q;
    lock(w);
    q = w->have_session || w->session_running;
    unlock(w);
    return q;
}

int dsg_worker_wait(dsg_worker* w, int64_t timeout_ns) {
    if (!w->threaded) {
        run_jobs(w);
        return 0;
    }
#if !defined(DSG_WORKER_NO_THREADS)
    if (w->last_seq == 0) return 0;
    return yrt_pump_wait(&w->pump, w->last_seq, timeout_ns) == YRT_OK ? 0 : -1;
#else
    (void)timeout_ns;
    return 0;
#endif
}

bool dsg_worker_threaded(const dsg_worker* w) { return w->threaded; }

void dsg_worker_get_stats(dsg_worker* w, dsg_worker_stats* out) {
    lock(w);
    *out = w->stats;
    unlock(w);
}
