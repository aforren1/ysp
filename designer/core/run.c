/* run.c - Run: the player beside the designer, its command line, its
 * progress (dsg.h, step 6; docs/designer.md, "Run", "Stop" and "Part R as
 * built"). OWNED BY PART R.
 *
 * The player runs as a process of its own (dsg_proc_*, platform.c), its
 * standard output and error in DIR.log beside the data set DIR (decision
 * 6). The UI thread reads that log and DIR/trials.csv as they grow: a tick
 * reads only the bytes added since the last read, and only after the watch
 * thread saw a size change (decision 7). The tick allocates nothing.
 *
 * Only the Run section's functions are external: tests/designer/
 * build_run_double.c stands in for this whole file in test_designer_ui. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "dsg.h"
#include "player.h"   /* PL_VERSION_STRING: the linked player library's */
#include "ysp/rt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <share.h>
#endif

/* The player's own budget (player/main.c): a larger --max-seconds changes
 * nothing, and run_part.c reads no larger value. */
#define RUN_MAX_SECONDS_CAP 172800.0
/* `ysp_player --version` ends in 0.3 ms (the plan's "Measured"); a program
 * that has not answered in this time is not the player. */
#define RUN_VERSION_WAIT_NS 5000000000LL
/* The tick reads the files when the watch thread saw a size change, and at
 * least this often, in case a wake-up was lost. */
#define RUN_POLL_NS 1000000000LL
/* The log's lines are cut at this many bytes, so 200 of them bound the
 * view's text; the player's longest line (a refusal) is shorter. */
#define RUN_LINE_MAX 1024
#define RUN_LOG_CAP (DSG_RUN_LOG_LINES * (RUN_LINE_MAX + 1))
/* One read of a file; a tick reads until the end in reads of this size. */
#define RUN_CHUNK 65536
/* A captured trials.csv field (response, outcome, rt_s); longer is cut. */
#define RUN_FIELD 128

/* --- files by their UTF-8 path ------------------------------------------------- */

#if defined(_WIN32)
static bool run_wide(const char* s, wchar_t* w, int cap) { return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, cap) > 0; }
/* fopen() takes the ANSI code page on Windows, and dsg.h has no reader of a
 * growing file. _SH_DENYNO: the player writes the file while it is read. */
static FILE* run_open_read(const char* path) {
    wchar_t w[DSG_PATH_MAX + 16];
    if (!run_wide(path, w, DSG_PATH_MAX + 16)) return NULL;
    return _wfsopen(w, L"rb", _SH_DENYNO);
}
static void run_remove(const char* path) {
    wchar_t w[DSG_PATH_MAX + 16];
    if (run_wide(path, w, DSG_PATH_MAX + 16)) _wremove(w);
}
#else
static FILE* run_open_read(const char* path) { return fopen(path, "rb"); }
static void run_remove(const char* path) { remove(path); }
#endif

static bool is_sep(char c) { return c == '/' || c == '\\'; }

/* A string that ends inside its array: settings come from a caller. */
static bool str_in(const char* s, size_t cap) { return memchr(s, 0, cap) != NULL; }

/* --- the command line ------------------------------------------------------------ */

typedef struct argv_out {
    dsg_run_argv* a;
    size_t at;
    bool full;
} argv_out;

/* a, then sep and b when sep is not 0 ("NAME=VALUE", "ROLE=UNITS"). */
static void put_arg2(argv_out* o, const char* a, char sep, const char* b) {
    const size_t na = strlen(a), nb = sep ? strlen(b) + 1 : 0;
    if (o->full || o->a->argc >= DSG_RUN_ARGV - 1 || o->at + na + nb + 1 > sizeof o->a->buf) {
        o->full = true;
        return;
    }
    memcpy(o->a->buf + o->at, a, na);
    if (sep) {
        o->a->buf[o->at + na] = sep;
        memcpy(o->a->buf + o->at + na + 1, b, nb - 1);
    }
    o->a->buf[o->at + na + nb] = 0;
    o->a->argv[o->a->argc++] = o->a->buf + o->at;
    o->at += na + nb + 1;
}

static void put_arg(argv_out* o, const char* s) { put_arg2(o, s, 0, NULL); }

/* A role's name for --allow-below-tier R,R and --sim-gain ROLE=UNITS: the
 * player splits at ',' and '=', and refuses a value that starts with '-'. */
static bool role_name_ok(const char* s, size_t cap) {
    return str_in(s, cap) && s[0] && s[0] != '-' && !strchr(s, ',') && !strchr(s, '=');
}

/* --seed N: the player reads it with strtoull(), which gives ULLONG_MAX for
 * a larger number, not an error. */
static bool seed_ok(const char* s) {
    size_t n = 0;
    for (; s[n]; n++)
        if (s[n] < '0' || s[n] > '9') return false;
    if (n == 0 || n > 20) return false;
    return n < 20 || strcmp(s, "18446744073709551615") <= 0;
}

/* A double as its shortest text that reads back as the same value, so a
 * gain of 0.1 is "0.1", not "0.10000000000000001". */
static void fmt_double(char* out, size_t cap, double v) {
    int p;
    for (p = 6; p <= 17; p++) {
        snprintf(out, cap, "%.*g", p, v);
        if (strtod(out, NULL) == v) return;
    }
}

int dsg_run_args(const dsg_run_settings* s, const char* player, const char* pack, const char* out_dir, dsg_run_argv* out,
                 char* err, size_t cap) {
    argv_out o;
    char num[64];
    int i;
    out->argc = 0;
    out->argv[0] = NULL;
    if (!s || !player || !player[0] || !pack || !pack[0] || !out_dir || !out_dir[0]) {
        snprintf(err, cap, "no player, pack or output folder");
        return -1;
    }
    /* docs/designer.md, "Tests": no test starts the player without --sim */
    if (!s->sim && getenv("YSP_DESIGNER_TEST")) {
        snprintf(err, cap, "YSP_DESIGNER_TEST is set: a test starts only a simulated run (--sim)");
        return -1;
    }
    if (!str_in(s->rig, sizeof s->rig) || !str_in(s->seed, sizeof s->seed) || !str_in(s->participant, sizeof s->participant)) {
        snprintf(err, cap, "a setting has no end");
        return -1;
    }
    /* a name, never a path: the player reads `--rig X` as a file when X has
     * a slash or ends in ".json" (player/lower.c, pl_rig_load()) */
    if (s->rig[0]) {
        const size_t n = strlen(s->rig);
        if (strchr(s->rig, '/') || strchr(s->rig, '\\') || (n >= 5 && strcmp(s->rig + n - 5, ".json") == 0) || s->rig[0] == '-') {
            snprintf(err, cap, "the rig profile '%s' is not a profile's name", s->rig);
            return -1;
        }
    }
    if (s->seed[0] && !seed_ok(s->seed)) {
        snprintf(err, cap, "the seed '%s' is not a number from 0 to 18446744073709551615", s->seed);
        return -1;
    }
    if (s->n_fields < 0 || s->n_fields > DSG_SESSION_FIELDS || s->n_below < 0 || s->n_below > DSG_RUN_ROLES || s->n_gains < 0 ||
        s->n_gains > DSG_RUN_ROLES) {
        snprintf(err, cap, "a count of the settings is out of range");
        return -1;
    }
    for (i = 0; i < s->n_fields; i++)
        if (!str_in(s->fields[i].name, sizeof s->fields[i].name) || !str_in(s->fields[i].value, sizeof s->fields[i].value) ||
            !s->fields[i].name[0] || strchr(s->fields[i].name, '=')) {
            snprintf(err, cap, "session field %d has no name, an '=' in its name, or no end", i + 1);
            return -1;
        }
    for (i = 0; i < s->n_below; i++)
        if (!role_name_ok(s->allow_below_tier[i], sizeof s->allow_below_tier[i])) {
            snprintf(err, cap, "--allow-below-tier: entry %d is not a role's name", i + 1);
            return -1;
        }
    if (s->sim) {
        if (!(s->max_seconds > 0) || s->max_seconds > RUN_MAX_SECONDS_CAP) {
            snprintf(err, cap, "--max-seconds: give more than 0 and at most %.0f s", RUN_MAX_SECONDS_CAP);
            return -1;
        }
        for (i = 0; i < s->n_gains; i++)
            if (!role_name_ok(s->gains[i].role, sizeof s->gains[i].role) || !(s->gains[i].units > 0) || !isfinite(s->gains[i].units)) {
                snprintf(err, cap, "--sim-gain: entry %d needs a role's name and units per count above 0", i + 1);
                return -1;
            }
    }

    memset(&o, 0, sizeof o);
    o.a = out;
    put_arg(&o, player);
    put_arg(&o, pack);
    put_arg(&o, "--out");
    put_arg(&o, out_dir);
    if (s->rig[0]) {
        put_arg(&o, "--rig");
        put_arg(&o, s->rig);
    }
    if (s->seed[0]) {
        put_arg(&o, "--seed");
        put_arg(&o, s->seed);
    }
    for (i = 0; i < s->n_fields; i++) {
        put_arg(&o, "--field");
        put_arg2(&o, s->fields[i].name, '=', s->fields[i].value);
    }
    if (s->n_below > 0) {
        char list[DSG_RUN_ROLES * 33];
        size_t at = 0;
        for (i = 0; i < s->n_below; i++) {
            const size_t k = strlen(s->allow_below_tier[i]);
            if (i) list[at++] = ',';
            memcpy(list + at, s->allow_below_tier[i], k);
            at += k;
        }
        list[at] = 0;
        put_arg(&o, "--allow-below-tier");
        put_arg(&o, list);
    }
    /* the player refuses --windowed with --sim (exit 2): the simulated
     * display has no window */
    if (s->windowed && !s->sim) put_arg(&o, "--windowed");
    if (s->sim) {
        put_arg(&o, "--sim");
        put_arg(&o, "--max-seconds");
        /* the player reads whole seconds (atoll()): round up, so the
         * session is never cut shorter than asked */
        snprintf(num, sizeof num, "%.0f", ceil(s->max_seconds));
        put_arg(&o, num);
        for (i = 0; i < s->n_gains; i++) {
            fmt_double(num, sizeof num, s->gains[i].units);
            put_arg(&o, "--sim-gain");
            put_arg2(&o, s->gains[i].role, '=', num);
        }
        if (s->participant[0]) {
            put_arg(&o, "--participant");
            put_arg(&o, s->participant);
        }
    }
    if (o.full) {
        out->argc = 0;
        out->argv[0] = NULL;
        snprintf(err, cap, "the command line does not fit (%d arguments and %d bytes at most)", DSG_RUN_ARGV - 1, (int)sizeof out->buf);
        return -1;
    }
    out->argv[out->argc] = NULL;
    return 0;
}

/* --- the output folder ------------------------------------------------------------ */

/* Days since 1970-01-01 to the civil date (H. Hinnant's algorithm): gmtime()
 * is not thread safe, and its safe forms differ by system. */
static void civil_from_days(int64_t z, int64_t* y, int* m, int* d) {
    int64_t era, yoe, doy, mp, doe;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}

int dsg_run_out_dir(const char* folder, const dsg_run_settings* s, int64_t now_unix_s, char* out, size_t cap) {
    char who[65];
    const char* p = "";
    int64_t days, sec, y = 0;
    int m = 0, d = 0, k, i;
    size_t n = 0;
    if (cap) out[0] = 0;
    if (!folder || !folder[0] || !s || !cap) return -1;
    for (i = 0; i < s->n_fields && i < DSG_SESSION_FIELDS; i++)
        if (strcmp(s->fields[i].name, "participant") == 0 && str_in(s->fields[i].value, sizeof s->fields[i].value))
            p = s->fields[i].value;
    /* a folder's name on every system: no separator, no "..", none of
     * Windows' reserved characters; a byte outside ASCII becomes '_' too, so
     * that the name does not depend on a file system's normalization */
    for (; p[n] && n < sizeof who - 1; n++) {
        const char c = p[n];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                        (c == '.' && n > 0);
        who[n] = ok ? c : '_';
    }
    who[n] = 0;
    if (n == 0) snprintf(who, sizeof who, "session");
    days = now_unix_s >= 0 ? now_unix_s / 86400 : -((86399 - now_unix_s) / 86400);
    sec = now_unix_s - days * 86400;
    civil_from_days(days, &y, &m, &d);
    if (y < 0 || y > 9999) return -1;
    {
        const size_t f = strlen(folder);
        const char* sep = is_sep(folder[f - 1]) ? "" : "/";
        k = snprintf(out, cap, "%s%sdata/%s_%04d%02d%02d-%02d%02d%02d", folder, sep, who, (int)y, m, d, (int)(sec / 3600),
                     (int)(sec / 60 % 60), (int)(sec % 60));
    }
    if (k < 0 || (size_t)k >= cap) {
        out[0] = 0;
        return -1;
    }
    return 0;
}

/* --- what an exit means ------------------------------------------------------------ */

#if !defined(_WIN32)
static const char* signal_name(int s) {
    switch (s) {
    case 1: return "SIGHUP";
    case 2: return "SIGINT";
    case 3: return "SIGQUIT";
    case 4: return "SIGILL";
    case 6: return "SIGABRT";
    case 7: return "SIGBUS";
    case 8: return "SIGFPE";
    case 9: return "SIGKILL";
    case 11: return "SIGSEGV";
    case 13: return "SIGPIPE";
    case 15: return "SIGTERM";
    default: return "";
    }
}
#endif

/* dsg_run_exit_text()'s text. display: the run had a display (not --sim),
 * so an end by a signal may leave its mode and gamma ramp changed. */
static const char* exit_text(int status, bool display, char* buf, size_t cap) {
    switch (status) {
    case 0: snprintf(buf, cap, "Done."); return buf;
    case 1: snprintf(buf, cap, "Refused or failed: the log's last line says why."); return buf;
    case 2: snprintf(buf, cap, "A bad argument: the designer's fault. The command line is below, for a report."); return buf;
    case 3: snprintf(buf, cap, "Stopped: the data set is closed, and session.json says \"aborted\"."); return buf;
    case 99: snprintf(buf, cap, "The panic watchdog ended it, and the display was put back. panic.json names the trial."); return buf;
    default: break;
    }
#if defined(_WIN32)
    (void)display;
    if (status == -9) snprintf(buf, cap, "Killed: the data set is not closed.");
    else if (status < 0) snprintf(buf, cap, "Ended with status 0x%08X: the data set is not closed.", (unsigned)status);
    else snprintf(buf, cap, "Ended with exit code %d, which is not one of the player's.", status);
#else
    if (status == -1) snprintf(buf, cap, "Ended; its status is not known.");
    else if (status < 0) {
        const char* n = signal_name(-status);
#if defined(__APPLE__)
        const char* hint = "";
        (void)display;
#else
        /* X11 (docs/screen.md): the player's restore did not run */
        const char* hint = display ? " If the display's mode or gamma ramp is wrong: xrandr --output OUT --mode 1920x1080 --rate 60 "
                                     "--gamma 1:1:1"
                                   : "";
#endif
        snprintf(buf, cap, "Ended by signal %d%s%s%s: the data set is not closed.%s", -status, n[0] ? " (" : "", n, n[0] ? ")" : "",
                 hint);
    } else snprintf(buf, cap, "Ended with exit code %d, which is not one of the player's.", status);
#endif
    return buf;
}

const char* dsg_run_exit_text(int status, char* buf, size_t cap) {
    if (!buf || !cap) return "";
    return exit_text(status, true, buf, cap);
}

/* --- the player's version ------------------------------------------------------------ */

static void wait_end(dsg_proc* p, int* status, bool* ended) {
    const int64_t until = yrt_now_ns() + RUN_VERSION_WAIT_NS;
    *ended = false;
    *status = -1;
    for (;;) {
        if (dsg_proc_poll(p, status)) {
            *ended = true;
            return;
        }
        if (yrt_now_ns() >= until) return;
        /* spin 0: the whole wait is the OS's */
        yrt_sleep_until(yrt_now_ns() + 200000, 0);
    }
}

int dsg_player_version(const char* player, const char* tmp_dir, char* out, size_t cap, char* err, size_t err_cap) {
    char path[DSG_PATH_MAX], line[256];
    const char* argv[3];
    dsg_proc_desc d;
    dsg_proc* p = NULL;
    FILE* f;
    size_t n = 0;
    int status = -1;
    bool ended = false;
    if (cap) out[0] = 0;
    if (!player || !player[0] || !tmp_dir || !tmp_dir[0]) {
        snprintf(err, err_cap, "no player, or no folder for its answer");
        return -1;
    }
    if (dsg_join(path, sizeof path, tmp_dir, "player_version.log")) {
        snprintf(err, err_cap, "the folder's path is too long");
        return -1;
    }
    /* a designer that ended during a check left it, and dsg_proc_start()
     * refuses a log that exists */
    run_remove(path);
    argv[0] = player;
    argv[1] = "--version";
    argv[2] = NULL;
    memset(&d, 0, sizeof d);
    d.argv = argv;
    d.out_path = path;
    if (dsg_proc_start(&p, &d, err, err_cap)) return -1;
    wait_end(p, &status, &ended);
    if (!ended) {
        dsg_proc_kill(p);
        wait_end(p, &status, &ended);
        ended = false;
    }
    dsg_proc_free(p);
    f = run_open_read(path);
    if (f) {
        n = fread(line, 1, sizeof line - 1, f);
        fclose(f);
    }
    line[n] = 0;
    run_remove(path);
    if (!ended) {
        snprintf(err, err_cap, "%s did not answer --version within %d s", player, (int)(RUN_VERSION_WAIT_NS / 1000000000LL));
        return -1;
    }
    if (status != 0) {
        snprintf(err, err_cap, "%s --version ended with status %d: it is not ysp_player %s or later", player, status, PL_VERSION_STRING);
        return -1;
    }
    {
        static const char pre[] = "ysp_player ";
        const char* v = line + sizeof pre - 1;
        size_t k = 0;
        if (strncmp(line, pre, sizeof pre - 1) != 0) {
            snprintf(err, err_cap, "%s --version did not say 'ysp_player VERSION'", player);
            return -1;
        }
        while (v[k] && v[k] != '\n' && v[k] != '\r' && v[k] != ' ') k++;
        if (k == 0 || k >= cap) {
            snprintf(err, err_cap, "%s --version gave no version", player);
            return -1;
        }
        memcpy(out, v, k);
        out[k] = 0;
    }
    return 0;
}

/* --- the run ------------------------------------------------------------------- */

/* trials.csv read as it grows, a byte at a time through a state machine:
 * a row that arrives in two reads needs no buffer, and no byte is read
 * twice. The header names the columns, and the parse reads them by name
 * (the plan's "Risks": the format's columns may move). RFC 4180 quoting, as
 * ysp/trials.h writes it. */
enum { COL_RESPONSE, COL_OUTCOME, COL_RT, NCOL };
static const char* const col_names[NCOL] = { "response", "outcome", "rt_s" };

typedef struct csv_state {
    bool header;           /* the first line is not complete yet            */
    bool quoted, quote_end, started, name_long;
    int col;               /* the field's column                            */
    int idx[NCOL];         /* the columns of response, outcome, rt_s; -1    */
    char name[64];         /* a header field                                */
    int name_n;
    char cur[NCOL][RUN_FIELD];
    int cur_n[NCOL];
    char last[NCOL][RUN_FIELD];
    int rows;
} csv_state;

struct dsg_run {
    dsg_proc* proc;
    volatile int32_t wakes;   /* the watch thread's count                  */
    int32_t seen;
    void (*wake)(void* ctx);
    void* ctx;
    bool sim, had_rig;

    int state;
    int64_t start_ns, now_ns, end_ns, last_poll_ns;
    int exit_status;
    bool stop_sent, stop_failed, kill_offered;
    int64_t stop_ns;

    FILE* log_f;
    FILE* trials_f;
    csv_state csv;
    bool saw_running;         /* the log's "running;" line (a real run)    */

    /* the log's last lines: complete lines, each with its '\n', then the
     * line that is not complete yet; each line cut at RUN_LINE_MAX */
    char log[RUN_LOG_CAP + RUN_CHUNK + RUN_LINE_MAX + 2];
    size_t log_n;
    int log_lines;            /* complete lines in log                    */
    int line_n;               /* bytes of the incomplete line              */
    char chunk[RUN_CHUNK];

    char out_dir[DSG_PATH_MAX];
    char log_path[DSG_PATH_MAX + 8];
    char trials_path[DSG_PATH_MAX + 16];
    char* command;
    char exit_buf[DSG_PATH_MAX + RUN_LINE_MAX + 64];
    char issue[RUN_LINE_MAX + 1];
    bool have_issue;
};

static void csv_init(csv_state* c) {
    int i;
    memset(c, 0, sizeof *c);
    c->header = true;
    for (i = 0; i < NCOL; i++) c->idx[i] = -1;
}

static void csv_end_field(csv_state* c) {
    int i;
    if (c->header) {
        c->name[c->name_n] = 0;
        for (i = 0; i < NCOL; i++)
            if (!c->name_long && c->idx[i] < 0 && strcmp(c->name, col_names[i]) == 0) c->idx[i] = c->col;
        c->name_n = 0;
        c->name_long = false;
    }
    c->col++;
    c->started = c->quoted = c->quote_end = false;
}

static void csv_end_row(csv_state* c) {
    int i;
    csv_end_field(c);
    if (c->header) c->header = false;
    else {
        c->rows++;
        for (i = 0; i < NCOL; i++) {
            memcpy(c->last[i], c->cur[i], (size_t)c->cur_n[i]);
            c->last[i][c->cur_n[i]] = 0;
        }
    }
    for (i = 0; i < NCOL; i++) c->cur_n[i] = 0;
    c->col = 0;
}

static void csv_add(csv_state* c, char ch) {
    int i;
    c->started = true;
    if (c->header) {
        if (c->name_n < (int)sizeof c->name - 1) c->name[c->name_n++] = ch;
        else c->name_long = true;
        return;
    }
    for (i = 0; i < NCOL; i++)
        if (c->idx[i] == c->col && c->cur_n[i] < RUN_FIELD - 1) c->cur[i][c->cur_n[i]++] = ch;
}

static void csv_feed(csv_state* c, const char* s, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        const char ch = s[i];
        if (c->quoted) {
            if (c->quote_end) {
                c->quote_end = false;
                if (ch == '"') {
                    csv_add(c, '"');
                    continue;
                }
                c->quoted = false;   /* that quote closed the field's text */
            } else if (ch == '"') {
                c->quote_end = true;
                continue;
            } else {
                csv_add(c, ch);
                continue;
            }
        }
        if (ch == '"' && !c->started) {
            c->quoted = c->started = true;
            continue;
        }
        if (ch == ',') csv_end_field(c);
        else if (ch == '\n') csv_end_row(c);
        else if (ch != '\r') csv_add(c, ch);
    }
}

/* The log's bytes: lines into the buffer, each cut at RUN_LINE_MAX, then
 * the oldest lines past DSG_RUN_LOG_LINES dropped with one move a read.
 * '\r' is dropped: on Windows the player's stdout and stderr are in text
 * mode, so its lines end in "\r\n". */
static void log_feed(dsg_run* r, const char* s, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (s[i] == '\r') continue;
        if (s[i] == '\n') {
            r->log[r->log_n++] = '\n';
            r->log_lines++;
            r->line_n = 0;
        } else if (r->line_n < RUN_LINE_MAX) {
            r->log[r->log_n++] = s[i];
            r->line_n++;
        }
    }
    if (r->log_lines > DSG_RUN_LOG_LINES) {
        int drop = r->log_lines - DSG_RUN_LOG_LINES;
        size_t at = 0;
        while (drop-- > 0) at = (size_t)((const char*)memchr(r->log + at, '\n', r->log_n - at) - r->log) + 1;
        memmove(r->log, r->log + at, r->log_n - at);
        r->log_n -= at;
        r->log_lines = DSG_RUN_LOG_LINES;
    }
    r->log[r->log_n] = 0;
}

/* Reads what a file gained since the last read; true when it gained. The
 * file stays open at its end: after clearerr() the next fread() sees the
 * bytes written since. */
static bool read_new(dsg_run* r, FILE* f, bool is_log) {
    bool got = false;
    for (;;) {
        size_t n;
        clearerr(f);
        n = fread(r->chunk, 1, sizeof r->chunk, f);
        if (n == 0) break;
        got = true;
        if (is_log) {
            log_feed(r, r->chunk, n);
            if (!r->saw_running && strstr(r->log, "ysp_player: running;")) r->saw_running = true;
        } else csv_feed(&r->csv, r->chunk, n);
        if (n < sizeof r->chunk) break;
    }
    return got;
}

static bool read_files(dsg_run* r) {
    bool got = false;
    if (r->log_f) got |= read_new(r, r->log_f, true);
    /* the player makes the data set after its load */
    if (!r->trials_f) r->trials_f = run_open_read(r->trials_path);
    if (r->trials_f) got |= read_new(r, r->trials_f, false);
    return got;
}

/* The log's last line with text, without the player's "ysp_player: ", into
 * out; "" when there is none. */
static void last_line(const dsg_run* r, char* out, size_t cap) {
    static const char pre[] = "ysp_player: ";
    size_t e = r->log_n, b, k;
    out[0] = 0;
    while (e > 0 && (r->log[e - 1] == '\n' || r->log[e - 1] == '\r' || r->log[e - 1] == ' ')) e--;
    if (e == 0) return;
    b = e;
    while (b > 0 && r->log[b - 1] != '\n') b--;
    if (e - b >= sizeof pre - 1 && memcmp(r->log + b, pre, sizeof pre - 1) == 0) b += sizeof pre - 1;
    k = e - b < cap - 1 ? e - b : cap - 1;
    memcpy(out, r->log + b, k);
    out[k] = 0;
}

static bool is_word_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* "rig" as a word: a refusal that the profile causes says so (player/
 * lower.c: "the rig profile", "the rig binds it"). */
static bool names_rig(const char* s) {
    const char* p = s;
    while ((p = strstr(p, "rig")) != NULL) {
        if ((p == s || !is_word_char(p[-1])) && !is_word_char(p[3])) return true;
        p += 3;
    }
    return false;
}

static void finish(dsg_run* r, int status, int64_t now_ns) {
    char line[RUN_LINE_MAX + 1], base[512];
    r->state = DSG_RUN_ENDED;
    r->exit_status = status;
    r->end_ns = now_ns;
    r->kill_offered = false;
    if (r->log_f) fclose(r->log_f);
    if (r->trials_f) fclose(r->trials_f);
    r->log_f = r->trials_f = NULL;
    /* the watch thread ends after the exit's wake: the join is short */
    dsg_proc_free(r->proc);
    r->proc = NULL;
    last_line(r, line, sizeof line);
    exit_text(status, !r->sim, base, sizeof base);
    switch (status) {
    case 0: snprintf(r->exit_buf, sizeof r->exit_buf, "Done: %d trials in %s.", r->csv.rows, r->out_dir); break;
    case 1:
        snprintf(r->exit_buf, sizeof r->exit_buf, "Refused or failed: %s", line[0] ? line : "the log says nothing.");
        if (line[0]) {
            snprintf(r->issue, sizeof r->issue, "%s", line);
            r->have_issue = true;
        }
        break;
    case 3: {
        /* the player's stop line ends "(trial K has no row)" or "(no trial
         * was running)" (player/main.c) */
        const char* t = strstr(line, "(trial ");
        if (t)
            snprintf(r->exit_buf, sizeof r->exit_buf, "Stopped: %d trials written; trial %d has no row.", r->csv.rows, atoi(t + 7));
        else if (strstr(line, "(no trial was running)"))
            snprintf(r->exit_buf, sizeof r->exit_buf, "Stopped: %d trials written; no trial was running.", r->csv.rows);
        else snprintf(r->exit_buf, sizeof r->exit_buf, "Stopped: %d trials written.", r->csv.rows);
        break;
    }
    default: snprintf(r->exit_buf, sizeof r->exit_buf, "%s", base); break;
    }
}

/* On the watch thread. */
static void run_wake(void* ctx) {
    dsg_run* r = (dsg_run*)ctx;
    dsg_atomic_add(&r->wakes, 1);
    if (r->wake) r->wake(r->ctx);
}

/* An argument as a shell (POSIX) or the C runtime (Windows) splits it back,
 * for the command line that a report quotes. out NULL: only the length. */
static size_t quote_arg(char* out, const char* a) {
    size_t n = 0;
    const char* p;
    bool plain = a[0] != 0;
    for (p = a; *p; p++)
        if (!is_word_char(*p) && !strchr("-./=,:+@%", *p)) plain = false;
    if (plain) {
        n = strlen(a);
        if (out) memcpy(out, a, n);
        return n;
    }
#if defined(_WIN32)
    {
        size_t bs = 0;
        if (out) out[n] = '"';
        n++;
        for (p = a; *p; p++) {
            if (*p == '\\') {
                bs++;
                continue;
            }
            if (*p == '"') bs = bs * 2 + 1;
            for (; bs; bs--, n++)
                if (out) out[n] = '\\';
            if (out) out[n] = *p;
            n++;
        }
        for (bs *= 2; bs; bs--, n++)
            if (out) out[n] = '\\';
        if (out) out[n] = '"';
        n++;
    }
#else
    if (out) out[n] = '\'';
    n++;
    for (p = a; *p; p++) {
        if (*p == '\'') {
            if (out) memcpy(out + n, "'\\''", 4);
            n += 4;
        } else {
            if (out) out[n] = *p;
            n++;
        }
    }
    if (out) out[n] = '\'';
    n++;
#endif
    return n;
}

static char* command_line(const char* const* argv) {
    size_t n = 1, at = 0;
    int i;
    char* s;
    for (i = 0; argv[i]; i++) n += quote_arg(NULL, argv[i]) + 1;
    s = (char*)malloc(n);
    if (!s) return NULL;
    for (i = 0; argv[i]; i++) {
        if (i) s[at++] = ' ';
        at += quote_arg(s + at, argv[i]);
    }
    s[at] = 0;
    return s;
}

/* Makes the missing folders above dir, from the deepest one that exists
 * down, so a drive's root is never made. */
static int make_parents(const char* dir, char* err, size_t cap) {
    char p[DSG_PATH_MAX];
    size_t n = strlen(dir), stops[64];
    int k = 0;
    if (n >= sizeof p) {
        snprintf(err, cap, "the output folder's path is too long");
        return -1;
    }
    memcpy(p, dir, n + 1);
    for (;;) {
        while (n > 0 && !is_sep(p[n - 1])) n--;
        while (n > 0 && is_sep(p[n - 1])) n--;
        if (n == 0) break;
        p[n] = 0;
        if (dsg_is_dir(p)) break;
        if (k == 64) {
            snprintf(err, cap, "the output folder is too deep");
            return -1;
        }
        stops[k++] = n;
    }
    while (k > 0) {
        const size_t e = stops[--k];
        memcpy(p, dir, e);
        p[e] = 0;
        if (dsg_make_dir(p, err, cap)) return -1;
    }
    return 0;
}

int dsg_run_start(dsg_run** out, const dsg_run_desc* d, char* err, size_t cap) {
    dsg_run* r;
    char ver[64];
    const char* watch[2];
    dsg_proc_desc pd;
    size_t n;
    int i;
    if (out) *out = NULL;
    if (!out || !d || !d->argv || !d->argv[0] || !d->out_dir || !d->out_dir[0] || !d->tmp_dir) {
        snprintf(err, cap, "no command line, output folder or folder for the version check");
        return -1;
    }
    n = strlen(d->out_dir);
    while (n > 1 && is_sep(d->out_dir[n - 1])) n--;
    if (n >= DSG_PATH_MAX) {
        snprintf(err, cap, "the output folder's path is too long");
        return -1;
    }
    /* decision 11: the player refuses a folder that exists; say so before a
     * log is made beside it */
    if (dsg_is_dir(d->out_dir) || dsg_is_file(d->out_dir)) {
        snprintf(err, cap, "the output folder %s exists: choose another", d->out_dir);
        return -1;
    }
    /* decision 5: the player must check as the designer's library does */
    if (dsg_player_version(d->argv[0], d->tmp_dir, ver, sizeof ver, err, cap)) return -1;
    if (strcmp(ver, PL_VERSION_STRING) != 0) {
        snprintf(err, cap, "the player %s is version %s, and this designer runs only %s, the version it checks with: build both from one tree",
                 d->argv[0], ver, PL_VERSION_STRING);
        return -1;
    }
    r = (dsg_run*)calloc(1, sizeof *r);
    if (!r) {
        snprintf(err, cap, "out of memory for the run");
        return -1;
    }
    memcpy(r->out_dir, d->out_dir, n);
    r->out_dir[n] = 0;
    snprintf(r->log_path, sizeof r->log_path, "%s.log", r->out_dir);
    snprintf(r->trials_path, sizeof r->trials_path, "%s/trials.csv", r->out_dir);
    csv_init(&r->csv);
    r->wake = d->wake;
    r->ctx = d->ctx;
    r->sim = d->sim;
    r->start_ns = r->now_ns = r->last_poll_ns = d->now_ns;
    r->state = DSG_RUN_STARTING;
    r->exit_status = -1;
    for (i = 0; d->argv[i]; i++)
        if (strcmp(d->argv[i], "--rig") == 0) r->had_rig = true;
    r->command = command_line(d->argv);
    if (!r->command) snprintf(err, cap, "out of memory for the run");
    if (!r->command || make_parents(r->out_dir, err, cap)) {
        free(r->command);
        free(r);
        return -1;
    }
    watch[0] = r->trials_path;
    watch[1] = NULL;
    memset(&pd, 0, sizeof pd);
    pd.argv = d->argv;
    pd.cwd = d->cwd;
    pd.out_path = r->log_path;
    pd.watch = watch;
    pd.wake = run_wake;
    pd.ctx = r;
    if (dsg_proc_start(&r->proc, &pd, err, cap)) {
        free(r->command);
        free(r);
        return -1;
    }
    r->log_f = run_open_read(r->log_path);
    *out = r;
    return 0;
}

int dsg_run_stop(dsg_run* r, int64_t now_ns) {
    int sent, st = 0;
    if (!r || r->state == DSG_RUN_ENDED) return 0;
    sent = dsg_proc_stop(r->proc);
    if (sent) {
        if (!r->stop_sent) r->stop_ns = now_ns;   /* Kill counts from the first Stop */
        r->stop_sent = true;
        r->state = DSG_RUN_STOPPING;
    } else if (!dsg_proc_poll(r->proc, &st)) {
        /* nothing reached it (a --sim run on Windows, part P): Kill now */
        r->stop_failed = true;
        r->kill_offered = true;
    }
    return sent;
}

void dsg_run_kill(dsg_run* r) {
    /* only after a Stop: Kill skips the player's restore of the display */
    if (!r || r->state == DSG_RUN_ENDED || !(r->stop_sent || r->stop_failed)) return;
    dsg_proc_kill(r->proc);
}

bool dsg_run_tick(dsg_run* r, int64_t now_ns) {
    bool changed = false;
    int32_t w;
    if (!r || r->state == DSG_RUN_ENDED) return false;
    r->now_ns = now_ns;
    w = dsg_atomic_load(&r->wakes);
    if (w != r->seen || now_ns - r->last_poll_ns >= RUN_POLL_NS) {
        int st = -1;
        /* the poll first: once the player has ended, the reads below see
         * all that it wrote */
        const int ended = dsg_proc_poll(r->proc, &st);
        r->seen = w;
        r->last_poll_ns = now_ns;
        changed = read_files(r);
        if (r->state == DSG_RUN_STARTING && (r->trials_f || r->saw_running)) {
            r->state = DSG_RUN_RUNNING;
            changed = true;
        }
        if (ended) {
            finish(r, st, now_ns);
            return true;
        }
    }
    if (!r->kill_offered && r->stop_sent && now_ns - r->stop_ns >= DSG_RUN_KILL_AFTER_NS) {
        r->kill_offered = true;
        changed = true;
    }
    return changed;
}

void dsg_run_get_info(const dsg_run* r, dsg_run_info* out) {
    memset(out, 0, sizeof *out);
    out->last_response = out->last_outcome = out->log = out->exit_text = out->out_dir = out->log_path = out->command = "";
    out->last_rt_s = -1;
    if (!r) {
        out->state = DSG_RUN_ENDED;
        out->exit_status = -1;
        return;
    }
    out->state = r->state;
    out->elapsed_ns = (r->state == DSG_RUN_ENDED ? r->end_ns : r->now_ns) - r->start_ns;
    out->trials = r->csv.rows;
    if (r->csv.rows > 0) {
        out->last_response = r->csv.last[COL_RESPONSE];
        out->last_outcome = r->csv.last[COL_OUTCOME];
        if (r->csv.last[COL_RT][0]) {
            char* e = NULL;
            const double v = strtod(r->csv.last[COL_RT], &e);
            if (e && !*e && v >= 0) out->last_rt_s = v;
        }
    }
    out->log = r->log;
    out->exit_status = r->state == DSG_RUN_ENDED ? r->exit_status : 0;
    out->exit_text = r->state == DSG_RUN_ENDED ? r->exit_buf : "";
    out->kill_offered = r->state != DSG_RUN_ENDED && r->kill_offered;
    out->out_dir = r->out_dir;
    out->log_path = r->log_path;
    out->command = r->command;
}

bool dsg_run_issue(const dsg_run* r, dsg_issue* out) {
    memset(out, 0, sizeof *out);
    if (!r || r->state != DSG_RUN_ENDED || !r->have_issue) return false;
    out->severity = DSG_SEV_ERROR;
    out->from = DSG_FROM_PLAYER_RUN;
    out->message = r->issue;
    out->rig = r->had_rig && names_rig(r->issue);
    return true;
}

void dsg_run_free(dsg_run* r) {
    if (!r) return;
    /* a player that runs is left running; its watch thread stays only to
     * reap it, and wakes nothing after this returns */
    dsg_proc_free(r->proc);
    if (r->log_f) fclose(r->log_f);
    if (r->trials_f) fclose(r->trials_f);
    free(r->command);
    free(r);
}
