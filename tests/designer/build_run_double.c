/* build_run_double.c - the test double of build.c and run.c
 * (build_run_double.h says why). OWNED BY PART S. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "build_run_double.h"
#if DSG_BUILD_RUN_DOUBLE
#include "dsg.h"
#include "ysp/pack_tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BRD_ISSUES 8

static struct {
    int status, n_errors, n_warnings;
    int posts;
    uint64_t revision;
    char out[DSG_PATH_MAX];
    bool pending;
    uint32_t offset;
    /* the result given */
    dsg_build_result res;
    dsg_issue issues[BRD_ISSUES];
    char msgs[BRD_ISSUES][64];
    /* runs */
    int starts, stops, kills;
    char argv[DSG_RUN_ARGV][256];
    int argc;
    bool sim;
    bool issue_rig;
    char cwd[DSG_PATH_MAX];
    bool has_cwd;
} B;

struct dsg_builder {
    dsg_builder_desc desc;
};

struct dsg_run {
    dsg_run_desc desc;
    int state;
    int status;
    bool end_due;
    bool have_issue;
    char issue[256];
    char exit_text[256];
    char out_dir[DSG_PATH_MAX];
    bool stopped;
};

static dsg_run* g_run;   /* the last run started, for brd_run_end()  */

void brd_reset(void) {
    memset(&B, 0, sizeof B);
    B.status = DSG_BUILD_OK;
}

void brd_build_script(int status, int n_errors, int n_warnings) {
    B.status = status;
    B.n_errors = n_errors;
    B.n_warnings = n_warnings;
}

int brd_build_posts(void) { return B.posts; }
uint64_t brd_build_revision(void) { return B.revision; }
const char* brd_build_out(void) { return B.out; }
int brd_run_starts(void) { return B.starts; }
const char* brd_run_arg(int i) { return i >= 0 && i < B.argc ? B.argv[i] : NULL; }
bool brd_run_sim(void) { return B.sim; }
const char* brd_run_cwd(void) { return B.has_cwd ? B.cwd : NULL; }
int brd_run_stops(void) { return B.stops; }
int brd_run_kills(void) { return B.kills; }
void brd_run_issue_rig(bool rig) { B.issue_rig = rig; }

void brd_run_end(int status, const char* issue) {
    if (!g_run) return;
    g_run->end_due = true;
    g_run->status = status;
    g_run->have_issue = issue != NULL;
    snprintf(g_run->issue, sizeof g_run->issue, "%s", issue ? issue : "");
}

void brd_run_wake(void) {
    if (g_run && g_run->desc.wake) g_run->desc.wake(g_run->desc.ctx);
}

/* --- build.c ------------------------------------------------------------------ */

dsg_builder* dsg_builder_start(const dsg_builder_desc* d, char* err, size_t cap) {
    dsg_builder* b = (dsg_builder*)calloc(1, sizeof *b);
    if (!b) {
        snprintf(err, cap, "out of memory");
        return NULL;
    }
    if (d) b->desc = *d;
    return b;
}

void dsg_builder_stop(dsg_builder* b) { free(b); }

int dsg_build_path(const dsg_project* p, char* out, size_t cap) {
    const char* f = p->folder;
    size_t n = strlen(f);
    const char* name;
    int k;
    while (n > 1 && (f[n - 1] == '/' || f[n - 1] == '\\')) n--;
    name = f + n;
    while (name > f && name[-1] != '/' && name[-1] != '\\') name--;
    if (name == f + n) return -1;
    k = snprintf(out, cap, "%.*s/%s/%.*s.ysppak", (int)n, f, DSG_BUILD_DIR, (int)(f + n - name), name);
    return k < 0 || (size_t)k >= cap ? -1 : 0;
}

int dsg_build_post(dsg_builder* b, const dsg_project* p, const char* out_path, uint64_t revision, char* err, size_t cap) {
    (void)b;
    (void)err;
    (void)cap;
    B.posts++;
    B.revision = revision;
    /* the place's offset in the file that Build reads, as B's
     * place_in_message() gives it */
    {
        char* d = NULL;
        size_t n = 0;
        char e[256];
        B.offset = 0;
        if (dsg_read_file(p->def_path, &d, &n, e, sizeof e) == 0) B.offset = dsg_offset_of(d, n, 2, 3);
        free(d);
    }
    snprintf(B.out, sizeof B.out, "%s", out_path);
    B.pending = true;
    return 0;
}

void dsg_build_cancel(dsg_builder* b) {
    (void)b;
    B.pending = false;
}

bool dsg_build_busy(const dsg_builder* b) {
    (void)b;
    return B.pending;
}

bool dsg_build_take(dsg_builder* b, const dsg_build_result** out) {
    int i, k = 0;
    (void)b;
    *out = NULL;
    if (!B.pending) return false;
    B.pending = false;
    memset(&B.res, 0, sizeof B.res);
    B.res.revision = B.revision;
    B.res.status = B.status;
    B.res.path = B.out;
    for (i = 0; i < B.n_errors && k < BRD_ISSUES; i++, k++) {
        snprintf(B.msgs[k], sizeof B.msgs[k], "build error %d", i);
        memset(&B.issues[k], 0, sizeof B.issues[k]);
        B.issues[k].severity = DSG_SEV_ERROR;
        B.issues[k].from = DSG_FROM_BUILD;
        B.issues[k].place.line = 2;
        B.issues[k].place.column = 3;
        B.issues[k].place.offset = B.offset;
        B.issues[k].message = B.msgs[k];
    }
    for (i = 0; i < B.n_warnings && k < BRD_ISSUES; i++, k++) {
        snprintf(B.msgs[k], sizeof B.msgs[k], "warning: build warning %d", i);
        memset(&B.issues[k], 0, sizeof B.issues[k]);
        B.issues[k].severity = DSG_SEV_WARNING;
        B.issues[k].from = DSG_FROM_BUILD;
        B.issues[k].message = B.msgs[k];
    }
    B.res.report.status = DSG_CHECK_NONE;
    B.res.report.issues = B.issues;
    B.res.report.n_issues = k;
    B.res.report.n_errors = B.n_errors < k ? B.n_errors : k;
    if (B.status == DSG_BUILD_OK) {
        B.res.size = 1234;
        snprintf(B.res.pack_id, sizeof B.res.pack_id, "sha256:%064d", 7);
    }
    *out = &B.res;
    return true;
}

int dsg_build_wait(dsg_builder* b, int64_t timeout_ns) {
    (void)b;
    (void)timeout_ns;
    return 0;
}

bool dsg_build_stale(const dsg_project* p, const dsg_build_result* r, uint64_t revision) {
    (void)p;
    return !r || r->status != DSG_BUILD_OK || r->revision != revision;
}

int dsg_build_check_pack(const dsg_project* p, const char* desc, size_t desc_len, const char* out_path, char* err, size_t cap) {
    ypt_options o;
    memset(&o, 0, sizeof o);
    return ypt_build_text(desc, desc_len, p->folder, out_path, &o, err, cap) == 0 ? 0 : -1;
}

/* --- run.c --------------------------------------------------------------------- */

int dsg_run_args(const dsg_run_settings* s, const char* player, const char* pack, const char* out_dir, dsg_run_argv* out,
                 char* err, size_t cap) {
    const char* a[8];
    int n = 0, i;
    size_t at = 0;
    (void)err;
    (void)cap;
    a[n++] = player;
    a[n++] = pack;
    a[n++] = "--out";
    a[n++] = out_dir;
    if (s->rig[0]) {
        a[n++] = "--rig";
        a[n++] = s->rig;
    }
    if (s->sim) a[n++] = "--sim";
    for (i = 0; i < n; i++) {
        const size_t k = strlen(a[i]) + 1;
        memcpy(out->buf + at, a[i], k);
        out->argv[i] = out->buf + at;
        at += k;
    }
    out->argv[n] = NULL;
    out->argc = n;
    return 0;
}

int dsg_run_out_dir(const char* folder, const dsg_run_settings* s, int64_t now_unix_s, char* out, size_t cap) {
    int k;
    (void)s;
    k = snprintf(out, cap, "%s/data/p_%lld", folder, (long long)now_unix_s);
    return k < 0 || (size_t)k >= cap ? -1 : 0;
}

const char* dsg_run_exit_text(int status, char* buf, size_t cap) {
    snprintf(buf, cap, "exit %d", status);
    return buf;
}

int dsg_player_version(const char* player, const char* tmp_dir, char* out, size_t cap, char* err, size_t err_cap) {
    (void)player;
    (void)tmp_dir;
    (void)err;
    (void)err_cap;
    snprintf(out, cap, "test");
    return 0;
}

int dsg_run_start(dsg_run** out, const dsg_run_desc* d, char* err, size_t cap) {
    dsg_run* r = (dsg_run*)calloc(1, sizeof *r);
    int i;
    (void)err;
    (void)cap;
    if (!r) return -1;
    r->desc = *d;
    r->state = DSG_RUN_RUNNING;
    snprintf(r->out_dir, sizeof r->out_dir, "%s", d->out_dir);
    r->desc.out_dir = r->out_dir;
    B.starts++;
    B.sim = d->sim;
    B.has_cwd = d->cwd != NULL;
    snprintf(B.cwd, sizeof B.cwd, "%s", d->cwd ? d->cwd : "");
    for (B.argc = 0, i = 0; i < DSG_RUN_ARGV && d->argv[i]; i++) snprintf(B.argv[B.argc++], sizeof B.argv[0], "%s", d->argv[i]);
    g_run = r;
    *out = r;
    return 0;
}

int dsg_run_stop(dsg_run* r, int64_t now_ns) {
    (void)now_ns;
    B.stops++;
    if (r->state == DSG_RUN_ENDED) return 0;
    r->state = DSG_RUN_STOPPING;
    r->stopped = true;
    return 1;
}

void dsg_run_kill(dsg_run* r) {
    B.kills++;
    r->end_due = true;
    r->status = -9;
}

bool dsg_run_tick(dsg_run* r, int64_t now_ns) {
    (void)now_ns;
    if (!r->end_due || r->state == DSG_RUN_ENDED) return false;
    r->state = DSG_RUN_ENDED;
    dsg_run_exit_text(r->status, r->exit_text, sizeof r->exit_text);
    return true;
}

void dsg_run_get_info(const dsg_run* r, dsg_run_info* out) {
    memset(out, 0, sizeof *out);
    out->state = r->state;
    out->last_response = out->last_outcome = out->log = out->log_path = out->command = "";
    out->last_rt_s = -1;
    out->exit_status = r->status;
    out->exit_text = r->state == DSG_RUN_ENDED ? r->exit_text : "";
    out->kill_offered = r->stopped;
    out->out_dir = r->out_dir;
}

bool dsg_run_issue(const dsg_run* r, dsg_issue* out) {
    memset(out, 0, sizeof *out);
    if (r->state != DSG_RUN_ENDED || !r->have_issue) return false;
    out->severity = DSG_SEV_ERROR;
    out->from = DSG_FROM_PLAYER_RUN;
    out->message = r->issue;
    out->rig = B.issue_rig;
    return true;
}

void dsg_run_free(dsg_run* r) {
    if (!r) return;
    if (g_run == r) g_run = NULL;
    free(r);
}
#endif
