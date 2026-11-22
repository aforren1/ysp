/* run_view_double.c - the test double of build.c, run.c and rig.c for
 * te_run.cpp (run_view_double.h says why and how to take it out). OWNED BY
 * PART V. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "run_view_double.h"
#if DSG_RUN_VIEW_DOUBLE
#include "dsg.h"
#include "player.h"   /* yrig_profile, yrig_load_file() */
#include "ysp/pack_tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#endif

#define RVD_ISSUES 4
#define RVD_LOG 8192

static struct {
    /* Build */
    int status;
    char error[256], warning[256];
    int line, column;
    int posts;
    uint64_t revision;
    char out[DSG_PATH_MAX];
    uint32_t offset;
    bool pending;
    dsg_build_result res;
    dsg_issue issues[RVD_ISSUES];
    char pack_id[72];
    /* Run */
    int starts, stops, kills;
    char argv[DSG_RUN_ARGV][256];
    int argc;
    bool stop_reaches, stop_ends;
} R;

struct dsg_builder {
    dsg_builder_desc desc;
};

struct dsg_run {
    dsg_run_desc desc;
    int state;
    int64_t start_ns, now_ns, end_ns, stop_ns;
    /* scripted for the next tick */
    bool progress_due, end_due;
    int next_trials, next_status;
    char next_response[64], next_outcome[64], next_line[256];
    double next_rt;
    /* shown */
    int trials;
    char response[64], outcome[64];
    double rt;
    char log[RVD_LOG];
    int status;
    bool stopped, kill_offered;
    char exit_text[512];
    char out_dir[DSG_PATH_MAX], log_path[DSG_PATH_MAX + 8];
    char command[4096];
};

static dsg_run* g_run;   /* the last run started */

void rvd_reset(void) {
    memset(&R, 0, sizeof R);
    R.status = DSG_BUILD_OK;
    R.stop_reaches = true;
}

void rvd_build_script(int status, const char* error, int line, int column, const char* warning) {
    R.status = status;
    snprintf(R.error, sizeof R.error, "%s", error ? error : "");
    snprintf(R.warning, sizeof R.warning, "%s", warning ? warning : "");
    R.line = line;
    R.column = column;
}

int rvd_build_posts(void) { return R.posts; }
int rvd_run_starts(void) { return R.starts; }
const char* rvd_run_arg(int i) { return i >= 0 && i < R.argc ? R.argv[i] : NULL; }
int rvd_run_stops(void) { return R.stops; }
int rvd_run_kills(void) { return R.kills; }
void rvd_run_stop_reaches(bool reaches) { R.stop_reaches = reaches; }
void rvd_run_stop_ends(bool ends) { R.stop_ends = ends; }

static void wake(void) {
    if (g_run && g_run->desc.wake) g_run->desc.wake(g_run->desc.ctx);
}

void rvd_run_progress(int trials, const char* response, const char* outcome, double rt_s, const char* log_line) {
    if (!g_run) return;
    g_run->progress_due = true;
    g_run->next_trials = trials;
    snprintf(g_run->next_response, sizeof g_run->next_response, "%s", response ? response : "");
    snprintf(g_run->next_outcome, sizeof g_run->next_outcome, "%s", outcome ? outcome : "");
    g_run->next_rt = rt_s;
    snprintf(g_run->next_line, sizeof g_run->next_line, "%s", log_line ? log_line : "");
    wake();
}

void rvd_run_end(int status) {
    if (!g_run) return;
    g_run->end_due = true;
    g_run->next_status = status;
    wake();
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
    char* d = NULL;
    size_t n = 0;
    char e[256];
    (void)b;
    (void)err;
    (void)cap;
    R.posts++;
    R.revision = revision;
    R.offset = 0;
    if (R.line > 0 && dsg_read_file(p->def_path, &d, &n, e, sizeof e) == 0) R.offset = dsg_offset_of(d, n, (uint32_t)R.line, (uint32_t)R.column);
    free(d);
    snprintf(R.out, sizeof R.out, "%s", out_path);
    R.pending = true;
    if (b && b->desc.wake) b->desc.wake(b->desc.ctx);
    return 0;
}

void dsg_build_cancel(dsg_builder* b) {
    (void)b;
    R.pending = false;
}

bool dsg_build_busy(const dsg_builder* b) {
    (void)b;
    return R.pending;
}

bool dsg_build_take(dsg_builder* b, const dsg_build_result** out) {
    int k = 0;
    (void)b;
    *out = NULL;
    if (!R.pending) return false;
    R.pending = false;
    memset(&R.res, 0, sizeof R.res);
    memset(R.issues, 0, sizeof R.issues);
    R.res.revision = R.revision;
    R.res.status = R.status;
    R.res.path = R.out;
    R.res.ms = 1.5;
    R.res.done_unix_s = (int64_t)time(NULL);
    if (R.error[0]) {
        R.issues[k].severity = DSG_SEV_ERROR;
        R.issues[k].from = DSG_FROM_BUILD;
        if (R.line > 0) {
            R.issues[k].place.line = (uint32_t)R.line;
            R.issues[k].place.column = (uint32_t)R.column;
            R.issues[k].place.offset = R.offset;
        }
        R.issues[k++].message = R.error;
    }
    if (R.warning[0]) {
        R.issues[k].severity = DSG_SEV_WARNING;
        R.issues[k].from = DSG_FROM_BUILD;
        R.issues[k++].message = R.warning;
    }
    R.res.report.status = DSG_CHECK_NONE;
    R.res.report.issues = R.issues;
    R.res.report.n_issues = k;
    R.res.report.n_errors = R.error[0] ? 1 : 0;
    if (R.status == DSG_BUILD_OK) {
        /* another ID for each Build, as another revision gives */
        snprintf(R.pack_id, sizeof R.pack_id, "sha256:%064d", R.posts);
        memcpy(R.res.pack_id, R.pack_id, sizeof R.res.pack_id);
        R.res.size = 17000 + R.posts;
    }
    *out = &R.res;
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

static int add_arg(dsg_run_argv* out, size_t* at, const char* s) {
    const size_t k = strlen(s) + 1;
    if (out->argc >= DSG_RUN_ARGV - 1 || *at + k > sizeof out->buf) return -1;
    memcpy(out->buf + *at, s, k);
    out->argv[out->argc++] = out->buf + *at;
    *at += k;
    return 0;
}

int dsg_run_args(const dsg_run_settings* s, const char* player, const char* pack, const char* out_dir, dsg_run_argv* out,
                 char* err, size_t cap) {
    size_t at = 0;
    char v[512];
    int i, bad = 0;
    const char* t = getenv("YSP_DESIGNER_TEST");
    out->argc = 0;
    if (!s->sim && t && t[0]) {
        snprintf(err, cap, "a test starts no real run (YSP_DESIGNER_TEST is set)");
        return -1;
    }
    bad |= add_arg(out, &at, player);
    bad |= add_arg(out, &at, pack);
    bad |= add_arg(out, &at, "--out");
    bad |= add_arg(out, &at, out_dir);
    if (s->rig[0]) {
        bad |= add_arg(out, &at, "--rig");
        bad |= add_arg(out, &at, s->rig);
    }
    if (s->seed[0]) {
        bad |= add_arg(out, &at, "--seed");
        bad |= add_arg(out, &at, s->seed);
    }
    for (i = 0; i < s->n_fields; i++) {
        snprintf(v, sizeof v, "%s=%s", s->fields[i].name, s->fields[i].value);
        bad |= add_arg(out, &at, "--field");
        bad |= add_arg(out, &at, v);
    }
    if (s->windowed && !s->sim) bad |= add_arg(out, &at, "--windowed");
    if (s->sim) {
        bad |= add_arg(out, &at, "--sim");
        snprintf(v, sizeof v, "%g", s->max_seconds);
        bad |= add_arg(out, &at, "--max-seconds");
        bad |= add_arg(out, &at, v);
        if (s->participant[0]) {
            bad |= add_arg(out, &at, "--participant");
            bad |= add_arg(out, &at, s->participant);
        }
    }
    out->argv[out->argc] = NULL;
    if (bad) snprintf(err, cap, "the command line does not fit");
    return bad ? -1 : 0;
}

int dsg_run_out_dir(const char* folder, const dsg_run_settings* s, int64_t now_unix_s, char* out, size_t cap) {
    const char* who = "session";
    char when[32] = "";
    const time_t t = (time_t)now_unix_s;
    const struct tm* g = gmtime(&t);
    int i, k;
    for (i = 0; i < s->n_fields; i++)
        if (strcmp(s->fields[i].name, "participant") == 0 && s->fields[i].value[0]) who = s->fields[i].value;
    if (g) strftime(when, sizeof when, "%Y%m%d-%H%M%S", g);
    k = snprintf(out, cap, "%s/data/%s_%s", folder, who, when);
    return k < 0 || (size_t)k >= cap ? -1 : 0;
}

const char* dsg_run_exit_text(int status, char* buf, size_t cap) {
    if (status == 0) snprintf(buf, cap, "Done");
    else if (status == 1) snprintf(buf, cap, "Refused or failed");
    else if (status == 2) snprintf(buf, cap, "A bad argument: the designer's fault");
    else if (status == 3) snprintf(buf, cap, "Stopped");
    else if (status == 99) snprintf(buf, cap, "The panic watchdog ended it; the display was put back");
    else if (status < 0) snprintf(buf, cap, "Ended by signal %d", -status);
    else snprintf(buf, cap, "Exit code %d", status);
    return buf;
}

int dsg_player_version(const char* player, const char* tmp_dir, char* out, size_t cap, char* err, size_t err_cap) {
    (void)player;
    (void)tmp_dir;
    (void)err;
    (void)err_cap;
    snprintf(out, cap, "%s", PL_VERSION_STRING);
    return 0;
}

int dsg_run_start(dsg_run** out, const dsg_run_desc* d, char* err, size_t cap) {
    dsg_run* r = (dsg_run*)calloc(1, sizeof *r);
    size_t at = 0;
    int i;
    (void)err;
    (void)cap;
    *out = NULL;
    if (!r) return -1;
    r->desc = *d;
    r->state = DSG_RUN_RUNNING;
    r->start_ns = r->now_ns = d->now_ns;
    r->rt = -1;
    snprintf(r->out_dir, sizeof r->out_dir, "%s", d->out_dir);
    snprintf(r->log_path, sizeof r->log_path, "%s.log", d->out_dir);
    r->desc.out_dir = r->out_dir;
    R.starts++;
    for (R.argc = 0, i = 0; i < DSG_RUN_ARGV && d->argv[i]; i++) {
        snprintf(R.argv[R.argc++], sizeof R.argv[0], "%s", d->argv[i]);
        at += (size_t)snprintf(r->command + at, at < sizeof r->command ? sizeof r->command - at : 0, "%s%s", i ? " " : "", d->argv[i]);
        if (at >= sizeof r->command) at = sizeof r->command - 1;
    }
    g_run = r;
    *out = r;
    return 0;
}

int dsg_run_stop(dsg_run* r, int64_t now_ns) {
    R.stops++;
    if (r->state == DSG_RUN_ENDED) return 0;
    r->stopped = true;
    if (!R.stop_reaches) {
        r->kill_offered = true;
        return 0;
    }
    r->state = DSG_RUN_STOPPING;
    r->stop_ns = now_ns;
    if (R.stop_ends) {
        r->end_due = true;
        r->next_status = 3;
    }
    return 1;
}

void dsg_run_kill(dsg_run* r) {
    R.kills++;
    if (r->state == DSG_RUN_ENDED) return;
    r->end_due = true;
    r->next_status = -9;
}

static void last_line(const char* log, char* out, size_t cap) {
    size_t n = strlen(log);
    const char* s;
    while (n > 0 && (log[n - 1] == '\n' || log[n - 1] == '\r')) n--;
    s = log + n;
    while (s > log && s[-1] != '\n') s--;
    snprintf(out, cap, "%.*s", (int)(log + n - s), s);
}

bool dsg_run_tick(dsg_run* r, int64_t now_ns) {
    bool changed = false;
    if (r->state == DSG_RUN_ENDED) return false;
    r->now_ns = now_ns;
    if (r->progress_due) {
        r->progress_due = false;
        r->trials = r->next_trials;
        snprintf(r->response, sizeof r->response, "%s", r->next_response);
        snprintf(r->outcome, sizeof r->outcome, "%s", r->next_outcome);
        r->rt = r->next_rt;
        if (r->next_line[0]) {
            const size_t n = strlen(r->log);
            snprintf(r->log + n, sizeof r->log - n, "%s\n", r->next_line);
        }
        changed = true;
    }
    if (r->stopped && !r->kill_offered && now_ns - r->stop_ns >= DSG_RUN_KILL_AFTER_NS) {
        r->kill_offered = true;
        changed = true;
    }
    if (r->end_due) {
        char line[256];
        r->end_due = false;
        r->state = DSG_RUN_ENDED;
        r->status = r->next_status;
        r->end_ns = now_ns;
        dsg_run_exit_text(r->status, r->exit_text, sizeof r->exit_text);
        if (r->status == 0 || r->status == 3) {
            const size_t n = strlen(r->exit_text);
            snprintf(r->exit_text + n, sizeof r->exit_text - n, ": %d trials in %s", r->trials, r->out_dir);
        } else if (r->status == 1) {
            const size_t n = strlen(r->exit_text);
            last_line(r->log, line, sizeof line);
            snprintf(r->exit_text + n, sizeof r->exit_text - n, ": %s", line);
        }
        changed = true;
    }
    return changed;
}

void dsg_run_get_info(const dsg_run* r, dsg_run_info* out) {
    memset(out, 0, sizeof *out);
    out->state = r->state;
    out->elapsed_ns = (r->state == DSG_RUN_ENDED ? r->end_ns : r->now_ns) - r->start_ns;
    out->trials = r->trials;
    out->last_response = r->response;
    out->last_outcome = r->outcome;
    out->last_rt_s = r->trials > 0 ? r->rt : -1;
    out->log = r->log;
    out->exit_status = r->status;
    out->exit_text = r->state == DSG_RUN_ENDED ? r->exit_text : "";
    out->kill_offered = r->kill_offered && r->state != DSG_RUN_ENDED;
    out->out_dir = r->out_dir;
    out->log_path = r->log_path;
    out->command = r->command;
}

bool dsg_run_issue(const dsg_run* r, dsg_issue* out) {
    static char line[256];
    memset(out, 0, sizeof *out);
    if (r->state != DSG_RUN_ENDED || r->status != 1) return false;
    last_line(r->log, line, sizeof line);
    out->severity = DSG_SEV_ERROR;
    out->from = DSG_FROM_PLAYER_RUN;
    out->message = line;
    return true;
}

void dsg_run_free(dsg_run* r) {
    if (!r) return;
    if (g_run == r) g_run = NULL;
    free(r);
}

/* --- rig.c --------------------------------------------------------------------- */

struct dsg_rig {
    char name[DSG_RIG_NAME];
    char path[DSG_PATH_MAX];
    int64_t size, mtime;
    yrig_profile prof;   /* 48 KB: the rig is on the heap */
};

/* $XDG_CONFIG_HOME/ysp/rig (Windows: %APPDATA%\ysp\rig), with no check of
 * the folders' owners and modes (run_view_double.h says why). */
static int rig_folder(char* out, size_t cap) {
#ifdef _WIN32
    const char* base = getenv("APPDATA");
#else
    const char* base = getenv("XDG_CONFIG_HOME");
#endif
    int k;
    if (!base || !base[0]) return -1;
    k = snprintf(out, cap, "%s/ysp/rig", base);
    return k < 0 || (size_t)k >= cap ? -1 : 0;
}

static int cmp_names(const void* a, const void* b) { return strcmp((const char*)a, (const char*)b); }

int dsg_rig_list(char (*names)[DSG_RIG_NAME], int cap, char* err, size_t err_cap) {
    char dir[DSG_PATH_MAX];
    int n = 0, i;
    bool have_profile = false;
    if (rig_folder(dir, sizeof dir)) {
        snprintf(err, err_cap, "no user folder (the double reads $XDG_CONFIG_HOME)");
        return -1;
    }
#ifdef _WIN32
    {
        char pat[DSG_PATH_MAX + 8];
        struct _finddata_t fd;
        intptr_t h;
        snprintf(pat, sizeof pat, "%s/*.json", dir);
        h = _findfirst(pat, &fd);
        if (h != -1) {
            do {
                const size_t k = strlen(fd.name);
                if (k > 5 && k - 5 < DSG_RIG_NAME && n < cap) {
                    snprintf(names[n], DSG_RIG_NAME, "%.*s", (int)(k - 5), fd.name);
                    n++;
                }
            } while (_findnext(h, &fd) == 0);
            _findclose(h);
        }
    }
#else
    {
        DIR* d = opendir(dir);
        struct dirent* e;
        while (d && (e = readdir(d)) != NULL) {
            const size_t k = strlen(e->d_name);
            if (k > 5 && strcmp(e->d_name + k - 5, ".json") == 0 && k - 5 < DSG_RIG_NAME && n < cap) {
                snprintf(names[n], DSG_RIG_NAME, "%.*s", (int)(k - 5), e->d_name);
                n++;
            }
        }
        if (d) closedir(d);
    }
#endif
    /* profile first, then by name */
    for (i = 0; i < n; i++)
        if (strcmp(names[i], "profile") == 0) {
            have_profile = true;
            if (i) {
                char t[DSG_RIG_NAME];
                memcpy(t, names[0], sizeof t);
                memcpy(names[0], names[i], sizeof t);
                memcpy(names[i], t, sizeof t);
            }
        }
    if (n > (have_profile ? 2 : 1)) qsort(names + (have_profile ? 1 : 0), (size_t)(n - (have_profile ? 1 : 0)), DSG_RIG_NAME, cmp_names);
    return n;
}

static void stamp(const char* path, int64_t* size, int64_t* mtime) {
    struct stat st;
    if (stat(path, &st) != 0) {
        *size = -1;
        *mtime = 0;
        return;
    }
    *size = (int64_t)st.st_size;
    *mtime = (int64_t)st.st_mtime;
}

int dsg_rig_load(const char* name, dsg_rig** out, char* err, size_t cap) {
    char dir[DSG_PATH_MAX];
    dsg_rig* r;
    *out = NULL;
    if (!name || !name[0] || strchr(name, '/') || strchr(name, '\\')) {
        snprintf(err, cap, "no such profile name");
        return -1;
    }
    if (rig_folder(dir, sizeof dir)) {
        snprintf(err, cap, "no user folder");
        return -1;
    }
    r = (dsg_rig*)calloc(1, sizeof *r);
    if (!r) {
        snprintf(err, cap, "out of memory");
        return -1;
    }
    snprintf(r->name, sizeof r->name, "%s", name);
    snprintf(r->path, sizeof r->path, "%s/%s.json", dir, name);
    stamp(r->path, &r->size, &r->mtime);
    if (yrig_load_file(&r->prof, r->path, err, cap) != YRIG_OK) {
        free(r);
        return -1;
    }
    *out = r;
    return 0;
}

void dsg_rig_free(dsg_rig* r) { free(r); }
const char* dsg_rig_name(const dsg_rig* r) { return r ? r->name : ""; }
const char* dsg_rig_path(const dsg_rig* r) { return r ? r->path : ""; }
const struct yrig_profile* dsg_rig_profile(const dsg_rig* r) { return r ? &r->prof : NULL; }

const struct ycol_cal* dsg_rig_cal(const dsg_rig* r) {
    (void)r;
    return NULL;
}

bool dsg_rig_changed(const dsg_rig* r) {
    int64_t size, mtime;
    stamp(r->path, &size, &mtime);
    return size != r->size || mtime != r->mtime;
}

int dsg_rig_summary(const dsg_rig* r, char* out, size_t cap) {
    if (!r->prof.view.set) return snprintf(out, cap, "Rig: %s (no view)", r->name);
    return snprintf(out, cap, "Rig: %s (view %g mm, %d px over %g mm)", r->name, r->prof.view.distance_mm, (int)r->prof.view.width_px,
                    r->prof.view.width_mm);
}

bool dsg_rig_counts_per_unit(const dsg_rig* r, const char* role, int role_units, double out[2]) {
    (void)r;
    (void)role;
    (void)role_units;
    out[0] = out[1] = 1.0;
    return false;
}
#endif
