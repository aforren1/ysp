/* run_test.c - Run (designer/core/run.c; docs/designer.md, "Run", "Stop" and
 * "Part R as built"): the command line, the output folder, the exits'
 * texts, and runs of FAKE (tests/designer/fake_player.c, a script for a
 * player) and of PLAYER --sim through the real spawn. No test starts a real
 * run: the test sets YSP_DESIGNER_TEST, so dsg_run_args() refuses one.
 *
 * Usage: test_designer_run WORK FAKE [PLAYER] [--measure] (WORK absolute).
 * Exit 0 on pass. Without PLAYER the ysp_player part is skipped. --measure
 * times dsg_run_tick() on a long trials.csv and prints the numbers.
 * Everything that the runs write is under WORK/runs. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1   /* setenv(), nanosleep() under -std=c11 */
#endif
#include "dsg.h"
#include "player.h"   /* PL_VERSION_STRING */
#include "ysp/pack_tool.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#include <time.h>
#include <unistd.h>
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#if defined(_WIN32)
static long long t_now(void) {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (long long)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
static void t_sleep(long ms) { Sleep((DWORD)ms); }
static void set_env(const char* k, const char* v) { _putenv_s(k, v ? v : ""); }
#else
static long long t_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}
static void t_sleep(long ms) {
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {}
}
static void set_env(const char* k, const char* v) {
    if (v) setenv(k, v, 1);
    else unsetenv(k);
}
#endif

static char work[DSG_PATH_MAX], runs[DSG_PATH_MAX], tmp_dir[DSG_PATH_MAX], fake[DSG_PATH_MAX];

static const char* wpath(const char* name) {
    static char p[8][DSG_PATH_MAX];
    static int k;
    k = (k + 1) & 7;
    if (dsg_join(p[k], sizeof p[k], runs, name)) {
        printf("FAIL: path too long\n");
        exit(1);
    }
    return p[k];
}

static void write_text(const char* path, const char* text) {
    char err[512] = "";
    if (dsg_write_file(path, text, strlen(text), err, sizeof err)) {
        printf("FAIL: %s\n", err);
        exit(1);
    }
}

static bool file_has(const char* path, const char* needle) {
    char* d = NULL;
    size_t n = 0;
    char err[256];
    bool ok;
    if (dsg_read_file(path, &d, &n, err, sizeof err)) return false;
    ok = strstr(d, needle) != NULL;
    free(d);
    return ok;
}

static int count_lines(const char* s) {
    int n = 0;
    for (; *s; s++) n += *s == '\n';
    return n;
}

/* --- the test's wake -------------------------------------------------------- */

static volatile int32_t woken;
static void on_wake(void* ctx) {
    (void)ctx;
    dsg_atomic_add(&woken, 1);
}

/* --- starting the fake --------------------------------------------------------- */

static void sim_settings(dsg_run_settings* s) {
    dsg_run_settings_default(s);
    s->sim = true;
}

/* Starts FAKE with SCRIPT's text, its data set at WORK/runs/NAME; NULL
 * with err filled when dsg_run_start() refuses. sim: dsg_run_desc.sim. */
static dsg_run* start_fake(const char* name, const char* script, const dsg_run_settings* s, bool sim, char* err, size_t cap) {
    static dsg_run_argv a;
    char sp[DSG_PATH_MAX + 128];
    dsg_run_desc d;
    dsg_run* r = NULL;
    size_t i;
    /* the script beside WORK/runs, its name flat: the run's folder and its
     * parents are the run's to make */
    snprintf(sp, sizeof sp, "%s/script_%s.txt", runs, name);
    for (i = strlen(runs) + 1; sp[i]; i++)
        if (sp[i] == '/') sp[i] = '_';
    write_text(sp, script);
    if (dsg_run_args(s, fake, sp, wpath(name), &a, err, cap)) return NULL;
    memset(&d, 0, sizeof d);
    d.argv = a.argv;
    d.cwd = runs;
    d.out_dir = wpath(name);
    d.tmp_dir = tmp_dir;
    d.sim = sim;
    d.wake = on_wake;
    d.now_ns = t_now();
    if (dsg_run_start(&r, &d, err, cap)) return NULL;
    return r;
}

static void go(const char* name) { write_text(wpath(name), "go"); }

/* Ticks with the clock until the info says so, at most ms; the last info
 * in *in. A tick every 5 ms: the UI's at most. */
typedef bool (*run_pred)(const dsg_run_info* in, const void* arg);
static bool tick_until(dsg_run* r, run_pred ok, const void* arg, long ms, dsg_run_info* in) {
    const long long end = t_now() + (long long)ms * 1000000LL;
    for (;;) {
        dsg_run_tick(r, t_now());
        dsg_run_get_info(r, in);
        if (ok(in, arg)) return true;
        if (t_now() > end) return false;
        t_sleep(5);
    }
}
static bool p_trials(const dsg_run_info* in, const void* n) { return in->trials >= *(const int*)n; }
static bool p_log(const dsg_run_info* in, const void* s) { return strstr(in->log, (const char*)s) != NULL; }
static bool p_ended(const dsg_run_info* in, const void* u) {
    (void)u;
    return in->state == DSG_RUN_ENDED;
}
#define UNTIL_TRIALS(r, n, in) do { const int n_ = (n); tick_until(r, p_trials, &n_, 10000, in); } while (0)
#define UNTIL_LOG(r, s, in) tick_until(r, p_log, s, 10000, in)
#define UNTIL_END(r, in) tick_until(r, p_ended, NULL, 30000, in)

/* --- dsg_run_args() --------------------------------------------------------- */

/* The argv, one argument a line, against want ("a|b|c"). */
static bool argv_is(const dsg_run_argv* a, const char* want, char* got, size_t cap) {
    size_t at = 0;
    int i;
    got[0] = 0;
    for (i = 0; i < a->argc && at < cap; i++) at += (size_t)snprintf(got + at, cap - at, "%s%s", i ? "|" : "", a->argv[i]);
    return a->argv[a->argc] == NULL && strcmp(got, want) == 0;
}

static void args_refused(const dsg_run_settings* s, const char* needle, int line) {
    static dsg_run_argv a;
    char err[512] = "";
    const int rc = dsg_run_args(s, "/p/ysp_player", "/p/x.ysppak", "/d/out", &a, err, sizeof err);
    checks++;
    if (rc != -1 || a.argc != 0 || a.argv[0] != NULL || !strstr(err, needle)) {
        fails++;
        printf("FAIL line %d: refused (%d, argc %d) with '%s', want '%s'\n", line, rc, a.argc, err, needle);
    }
}
#define ARGS_REFUSED(s, needle) args_refused(s, needle, __LINE__)

static void test_args(void) {
    static dsg_run_argv a;
    static char got[65536];
    dsg_run_settings s, t;
    char err[512] = "";
    int rc;

    /* a real run's command line, with the variable unset for the pure
     * function only: nothing starts here */
    set_env("YSP_DESIGNER_TEST", NULL);
    dsg_run_settings_default(&s);
    rc = dsg_run_args(&s, "/p/ysp_player", "/p/x.ysppak", "/d/out", &a, err, sizeof err);
    CHECK(rc == 0 && argv_is(&a, "/p/ysp_player|/p/x.ysppak|--out|/d/out", got, sizeof got), "the defaults: %s (%s)", got, err);
    CHECK(a.argc == 4, "argc %d", a.argc);

    snprintf(s.rig, sizeof s.rig, "lab1");
    snprintf(s.seed, sizeof s.seed, "42");
    s.n_fields = 2;
    snprintf(s.fields[0].name, sizeof s.fields[0].name, "participant");
    snprintf(s.fields[0].value, sizeof s.fields[0].value, "P 01");
    snprintf(s.fields[1].name, sizeof s.fields[1].name, "note");
    snprintf(s.fields[1].value, sizeof s.fields[1].value, "a=b, \"c\"");
    s.n_below = 2;
    snprintf(s.allow_below_tier[0], sizeof s.allow_below_tier[0], "resp");
    snprintf(s.allow_below_tier[1], sizeof s.allow_below_tier[1], "trig");
    s.windowed = true;
    snprintf(s.participant, sizeof s.participant, "p=0.8");
    s.n_gains = 1;
    snprintf(s.gains[0].role, sizeof s.gains[0].role, "hand");
    s.gains[0].units = 0.25;
    s.max_seconds = 30;
    rc = dsg_run_args(&s, "/p/ysp_player", "/p/x.ysppak", "/d/out", &a, err, sizeof err);
    CHECK(rc == 0 &&
              argv_is(&a,
                      "/p/ysp_player|/p/x.ysppak|--out|/d/out|--rig|lab1|--seed|42|--field|participant=P 01|--field|note=a=b, \"c\""
                      "|--allow-below-tier|resp,trig|--windowed",
                      got, sizeof got),
          "a real run, every setting (no --sim options): %s (%s)", got, err);

    /* the same with --sim: no --windowed (the player refuses it there), and
     * the --sim options */
    s.sim = true;
    s.n_gains = 2;
    snprintf(s.gains[1].role, sizeof s.gains[1].role, "left");
    s.gains[1].units = 0.1;
    rc = dsg_run_args(&s, "/p/ysp_player", "/p/x.ysppak", "/d/out", &a, err, sizeof err);
    CHECK(rc == 0 &&
              argv_is(&a,
                      "/p/ysp_player|/p/x.ysppak|--out|/d/out|--rig|lab1|--seed|42|--field|participant=P 01|--field|note=a=b, \"c\""
                      "|--allow-below-tier|resp,trig|--sim|--max-seconds|30|--sim-gain|hand=0.25|--sim-gain|left=0.1"
                      "|--participant|p=0.8",
                      got, sizeof got),
          "a simulated run, every setting: %s (%s)", got, err);

    /* --max-seconds: whole seconds, rounded up; the default */
    sim_settings(&t);
    rc = dsg_run_args(&t, "P", "K", "D", &a, err, sizeof err);
    CHECK(rc == 0 && argv_is(&a, "P|K|--out|D|--sim|--max-seconds|600", got, sizeof got), "--sim's defaults: %s", got);
    t.max_seconds = 12.25;
    rc = dsg_run_args(&t, "P", "K", "D", &a, err, sizeof err);
    CHECK(rc == 0 && argv_is(&a, "P|K|--out|D|--sim|--max-seconds|13", got, sizeof got), "12.25 s rounds up: %s", got);
    t.max_seconds = 172800;
    rc = dsg_run_args(&t, "P", "K", "D", &a, err, sizeof err);
    CHECK(rc == 0 && argv_is(&a, "P|K|--out|D|--sim|--max-seconds|172800", got, sizeof got), "the player's budget: %s", got);
    t.max_seconds = 600;
    t.n_gains = 1;
    snprintf(t.gains[0].role, sizeof t.gains[0].role, "hand");
    t.gains[0].units = 2.5e-7;
    rc = dsg_run_args(&t, "P", "K", "D", &a, err, sizeof err);
    CHECK(rc == 0 && argv_is(&a, "P|K|--out|D|--sim|--max-seconds|600|--sim-gain|hand=2.5e-07", got, sizeof got), "a small gain: %s", got);
    t.gains[0].units = 1.0 / 3.0;
    rc = dsg_run_args(&t, "P", "K", "D", &a, err, sizeof err);
    CHECK(rc == 0 && a.argc == 9 && strtod(a.argv[8] + 5, NULL) == 1.0 / 3.0, "a gain reads back as the same double: %s",
          rc ? err : a.argv[8]);
    /* --seed at the largest value; an empty participant gives none */
    sim_settings(&t);
    snprintf(t.seed, sizeof t.seed, "18446744073709551615");
    rc = dsg_run_args(&t, "P", "K", "D", &a, err, sizeof err);
    CHECK(rc == 0 && argv_is(&a, "P|K|--out|D|--seed|18446744073709551615|--sim|--max-seconds|600", got, sizeof got), "the largest seed: %s",
          got);

    /* refusals */
    dsg_run_settings_default(&t);
    snprintf(t.rig, sizeof t.rig, "rigs/lab1");
    ARGS_REFUSED(&t, "not a profile's name");
    snprintf(t.rig, sizeof t.rig, "lab1.json");
    ARGS_REFUSED(&t, "not a profile's name");
    snprintf(t.rig, sizeof t.rig, "C:\\lab1");
    ARGS_REFUSED(&t, "not a profile's name");
    dsg_run_settings_default(&t);
    snprintf(t.seed, sizeof t.seed, "-1");
    ARGS_REFUSED(&t, "is not a number");
    snprintf(t.seed, sizeof t.seed, "18446744073709551616");
    ARGS_REFUSED(&t, "is not a number");
    snprintf(t.seed, sizeof t.seed, "12a");
    ARGS_REFUSED(&t, "is not a number");
    dsg_run_settings_default(&t);
    t.n_fields = 1;
    snprintf(t.fields[0].name, sizeof t.fields[0].name, "a=b");
    ARGS_REFUSED(&t, "session field 1");
    t.fields[0].name[0] = 0;
    ARGS_REFUSED(&t, "session field 1");
    t.n_fields = DSG_SESSION_FIELDS + 1;
    ARGS_REFUSED(&t, "out of range");
    dsg_run_settings_default(&t);
    t.n_below = 1;
    snprintf(t.allow_below_tier[0], sizeof t.allow_below_tier[0], "a,b");
    ARGS_REFUSED(&t, "--allow-below-tier");
    snprintf(t.allow_below_tier[0], sizeof t.allow_below_tier[0], "-x");
    ARGS_REFUSED(&t, "--allow-below-tier");
    sim_settings(&t);
    t.max_seconds = 0;
    ARGS_REFUSED(&t, "--max-seconds");
    t.max_seconds = 172801;
    ARGS_REFUSED(&t, "--max-seconds");
    sim_settings(&t);
    t.n_gains = 1;
    snprintf(t.gains[0].role, sizeof t.gains[0].role, "hand");
    t.gains[0].units = 0;
    ARGS_REFUSED(&t, "--sim-gain");
    t.gains[0].units = 1;
    snprintf(t.gains[0].role, sizeof t.gains[0].role, "ha=nd");
    ARGS_REFUSED(&t, "--sim-gain");
    /* a gain is a --sim setting: a real run ignores a bad one */
    t.sim = false;
    CHECK(dsg_run_args(&t, "P", "K", "D", &a, err, sizeof err) == 0 && a.argc == 4, "a real run passes no gain: %s", err);
    /* a setting with no end */
    dsg_run_settings_default(&t);
    memset(t.rig, 'a', sizeof t.rig);
    ARGS_REFUSED(&t, "no end");
    /* does not fit */
    {
        static char big[40000];
        memset(big, 'k', sizeof big - 1);
        big[sizeof big - 1] = 0;
        dsg_run_settings_default(&t);
        rc = dsg_run_args(&t, "P", big, "D", &a, err, sizeof err);
        CHECK(rc == -1 && a.argc == 0 && strstr(err, "does not fit"), "a pack path that does not fit: %d %s", rc, err);
    }
    rc = dsg_run_args(&t, "P", "", "D", &a, err, sizeof err);
    CHECK(rc == -1 && strstr(err, "no player"), "no pack: %s", err);

    /* YSP_DESIGNER_TEST: a real run is refused, a simulated one is not */
    set_env("YSP_DESIGNER_TEST", "1");
    dsg_run_settings_default(&t);
    ARGS_REFUSED(&t, "YSP_DESIGNER_TEST");
    t.sim = true;
    CHECK(dsg_run_args(&t, "P", "K", "D", &a, err, sizeof err) == 0, "a simulated run under YSP_DESIGNER_TEST: %s", err);
}

/* --- dsg_run_out_dir() ---------------------------------------------------- */

static void out_dir_is(const char* folder, const char* who, int64_t t, const char* want, int line) {
    dsg_run_settings s;
    char out[DSG_PATH_MAX];
    int rc;
    dsg_run_settings_default(&s);
    if (who) {
        s.n_fields = 2;
        snprintf(s.fields[0].name, sizeof s.fields[0].name, "group");
        snprintf(s.fields[0].value, sizeof s.fields[0].value, "g1");
        snprintf(s.fields[1].name, sizeof s.fields[1].name, "participant");
        snprintf(s.fields[1].value, sizeof s.fields[1].value, "%s", who);
    }
    rc = dsg_run_out_dir(folder, &s, t, out, sizeof out);
    checks++;
    if (rc != 0 || strcmp(out, want) != 0) {
        fails++;
        printf("FAIL line %d: out_dir %d '%s', want '%s'\n", line, rc, out, want);
    }
}

static void test_out_dir(void) {
    dsg_run_settings s;
    char out[64];
    out_dir_is("/home/a/proj", "P01", 1795355107, "/home/a/proj/data/P01_20261122-134507", __LINE__);
    out_dir_is("/home/a/proj/", "P01", 1709251199, "/home/a/proj/data/P01_20240229-235959", __LINE__);
    out_dir_is("C:\\proj\\", "s-1.b", 951868801, "C:\\proj\\data/s-1.b_20000301-000001", __LINE__);
    out_dir_is("/p", "../a b:c/\xc3\xa9", 0, "/p/data/_._a_b_c____19700101-000000", __LINE__);
    out_dir_is("/p", NULL, -1, "/p/data/session_19691231-235959", __LINE__);
    out_dir_is("/p", "", 86400, "/p/data/session_19700102-000000", __LINE__);
    dsg_run_settings_default(&s);
    CHECK(dsg_run_out_dir("/a/very/long/project/folder/name", &s, 0, out, 40) == -1 && out[0] == 0, "a name that does not fit");
}

/* --- dsg_run_exit_text() -------------------------------------------------- */

static void test_exit_text(void) {
    char b[1024];
    CHECK(strcmp(dsg_run_exit_text(0, b, sizeof b), "Done.") == 0, "0: %s", b);
    CHECK(strstr(dsg_run_exit_text(1, b, sizeof b), "Refused or failed") == b, "1: %s", b);
    CHECK(strstr(dsg_run_exit_text(2, b, sizeof b), "A bad argument: the designer's fault") == b, "2: %s", b);
    CHECK(strstr(dsg_run_exit_text(3, b, sizeof b), "Stopped") == b && strstr(b, "aborted"), "3: %s", b);
    CHECK(strstr(dsg_run_exit_text(99, b, sizeof b), "The panic watchdog") == b && strstr(b, "panic.json"), "99: %s", b);
    CHECK(strstr(dsg_run_exit_text(7, b, sizeof b), "exit code 7, which is not one of the player's"), "7: %s", b);
#if defined(_WIN32)
    CHECK(strstr(dsg_run_exit_text(-9, b, sizeof b), "Killed") == b, "-9: %s", b);
    CHECK(strstr(dsg_run_exit_text((int)0xC0000005, b, sizeof b), "0xC0000005"), "an NTSTATUS: %s", b);
#else
    CHECK(strstr(dsg_run_exit_text(-9, b, sizeof b), "Ended by signal 9 (SIGKILL): the data set is not closed.") == b, "-9: %s", b);
    CHECK(strstr(dsg_run_exit_text(-15, b, sizeof b), "signal 15 (SIGTERM)"), "-15: %s", b);
    CHECK(strstr(dsg_run_exit_text(-10, b, sizeof b), "Ended by signal 10: "), "-10, no name: %s", b);
    CHECK(strstr(dsg_run_exit_text(-1, b, sizeof b), "not known"), "-1: %s", b);
#if !defined(__APPLE__)
    CHECK(strstr(dsg_run_exit_text(-9, b, sizeof b), "xrandr --output OUT --mode 1920x1080 --rate 60 --gamma 1:1:1") != NULL,
          "a signal: how to put the display back: %s", b);
#endif
#endif
    CHECK(strcmp(dsg_run_exit_text(0, b, 3), "Do") == 0, "a small buffer is cut: %s", b);
}

/* --- runs of the fake ------------------------------------------------------- */

/* Progress: rows and the last row by column name, the log, a half row. */
static void test_progress(void) {
    char script[4096], err[1024] = "";
    dsg_run_settings s;
    dsg_run_info in;
    dsg_run* r;
    const int32_t w0 = dsg_atomic_load(&woken);
    sim_settings(&s);
    /* the columns are not the player's order, a quoted field before them
     * holds a comma, quotes and a line break, and two names start as
     * "response" and "rt_s" do */
    snprintf(script, sizeof script,
             "header trial_index,\"a,b\",response_x,outcome,rt_s_x,rt_s,response\n"
             "out ysp_player: line one\n"
             "row 0,\"x,\"\"y\"\"\\nz\",no,hit,9,0.412,left\n"
             "row 1,plain,no,miss,9,0.5,right\n"
             "wait %s\n"
             "err warning: on stderr\n"
             "err a line in CR LF\\r\n"
             "row 2,\"q\",no,timeout,9,,\n"
             "raw 3,\"half\n"
             "out half written\n"
             "wait %s\n"
             "raw \\n row\",no,hit,9,0.3,\"le\"\"ft\"\\n\n"
             "out ysp_player: 4 trials, DIR, seed 1, experiment e\n"
             "exit 0\n",
             wpath("progress.go1"), wpath("progress.go2"));
    r = start_fake("progress", script, &s, true, err, sizeof err);
    CHECK(r != NULL, "start: %s", err);
    if (!r) return;
    dsg_run_get_info(r, &in);
    CHECK(in.state == DSG_RUN_STARTING && in.trials == 0 && in.last_rt_s < 0 && in.exit_text[0] == 0, "at the start: state %d",
          in.state);
    CHECK(strcmp(in.out_dir, wpath("progress")) == 0, "out_dir %s", in.out_dir);
    {
        char want[DSG_PATH_MAX + 8];
        snprintf(want, sizeof want, "%s.log", wpath("progress"));
        CHECK(strcmp(in.log_path, want) == 0, "the log beside the data set: %s", in.log_path);
    }
    CHECK(strstr(in.command, fake) && strstr(in.command, " --out ") && strstr(in.command, " --sim --max-seconds 600"), "command %s",
          in.command);
    UNTIL_TRIALS(r, 2, &in);
    CHECK(in.trials == 2 && in.state == DSG_RUN_RUNNING, "two rows: %d, state %d", in.trials, in.state);
    CHECK(strcmp(in.last_response, "right") == 0 && strcmp(in.last_outcome, "miss") == 0 && in.last_rt_s == 0.5,
          "the last row by name: '%s' '%s' %g", in.last_response, in.last_outcome, in.last_rt_s);
    UNTIL_LOG(r, "line one", &in);
    CHECK(strstr(in.log, "ysp_player: line one\n") != NULL, "the log's line: %s", in.log);
    go("progress.go1");
    UNTIL_LOG(r, "half written", &in);
    CHECK(in.trials == 3 && strcmp(in.last_response, "") == 0 && strcmp(in.last_outcome, "timeout") == 0 && in.last_rt_s < 0,
          "a row with no response: %d '%s' '%s' %g", in.trials, in.last_response, in.last_outcome, in.last_rt_s);
    CHECK(strstr(in.log, "warning: on stderr\n") != NULL, "standard error in the log");
    /* Windows' text mode ends each line in "\r\n" */
    CHECK(strstr(in.log, "a line in CR LF\n") != NULL && !strchr(in.log, '\r'), "the log drops '\\r'");
    CHECK(in.state == DSG_RUN_RUNNING && in.exit_text[0] == 0 && !in.kill_offered, "running");
    go("progress.go2");
    UNTIL_END(r, &in);
    CHECK(in.state == DSG_RUN_ENDED && in.exit_status == 0, "ended: state %d, exit %d", in.state, in.exit_status);
    CHECK(in.trials == 4 && strcmp(in.last_response, "le\"ft") == 0 && strcmp(in.last_outcome, "hit") == 0 && in.last_rt_s == 0.3,
          "a row in two reads with a line break in a quoted field: %d '%s' '%s' %g", in.trials, in.last_response, in.last_outcome,
          in.last_rt_s);
    {
        char want[DSG_PATH_MAX + 64];
        snprintf(want, sizeof want, "Done: 4 trials in %s.", wpath("progress"));
        CHECK(strcmp(in.exit_text, want) == 0, "exit text: %s", in.exit_text);
    }
    CHECK(strstr(in.log, "ysp_player: 4 trials, DIR") != NULL, "the end line");
    CHECK(in.elapsed_ns > 0, "elapsed %lld", (long long)in.elapsed_ns);
    {
        dsg_issue is;
        CHECK(!dsg_run_issue(r, &is), "exit 0: no issue");
    }
    CHECK(!dsg_run_tick(r, t_now()), "an ended run's tick changes nothing");
    CHECK(dsg_atomic_load(&woken) > w0, "the watch thread woke the caller");
    CHECK(dsg_run_stop(r, t_now()) == 0, "Stop of an ended run: nothing goes out");
    dsg_run_get_info(r, &in);
    CHECK(!in.kill_offered && in.state == DSG_RUN_ENDED, "and no Kill");
    dsg_run_free(r);
}

/* A tick reads the files only after the watch thread saw them grow (or a
 * second after the last read): with no wake-up, a tick costs no system
 * call. The test writes trials.csv itself while the fake waits. */
static void test_lazy(void) {
    char err[512] = "", script[512], tp[DSG_PATH_MAX + 16];
    dsg_run_settings s;
    dsg_run_info in;
    dsg_run* r;
    long long t, end;
    int tries, quiet = 0;
    sim_settings(&s);
    snprintf(script, sizeof script, "header trial_index,response\nout ready\nwait %s\nexit 0\n", wpath("lazy.go"));
    r = start_fake("lazy", script, &s, true, err, sizeof err);
    CHECK(r != NULL, "start: %s", err);
    if (!r) return;
    UNTIL_LOG(r, "ready", &in);
    snprintf(tp, sizeof tp, "%s/trials.csv", wpath("lazy"));
    for (tries = 0; tries < 20 && quiet < 3; tries++) {
        const int n = in.trials;
        int32_t w0, w1;
        FILE* f;
        /* the clock stands still: no second passes since the last read */
        t = t_now();
        dsg_run_tick(r, t);
        w0 = dsg_atomic_load(&woken);
        f = fopen(tp, "ab");
        if (!f) break;
        fprintf(f, "%d,a\n", n);
        fclose(f);
        dsg_run_tick(r, t);
        w1 = dsg_atomic_load(&woken);
        dsg_run_get_info(r, &in);
        if (w1 == w0) {
            quiet++;
            CHECK(in.trials == n, "no wake-up, no read: %d rows, %d before", in.trials, n);
        }
        /* the wake-up comes, and the tick at the same time reads the row */
        for (end = t_now() + 2000000000LL; dsg_atomic_load(&woken) == w1 && t_now() < end;) t_sleep(2);
        dsg_run_tick(r, t);
        dsg_run_get_info(r, &in);
        CHECK(in.trials == n + 1, "after the wake-up: %d rows", in.trials);
    }
    CHECK(quiet == 3, "three writes with no wake-up between them and the tick: %d", quiet);
    go("lazy.go");
    UNTIL_END(r, &in);
    dsg_run_free(r);
}

/* The log keeps its last 200 lines, each cut at 1024 bytes. */
static void test_log_tail(void) {
    char err[512] = "";
    dsg_run_settings s;
    dsg_run_info in;
    dsg_run* r;
    sim_settings(&s);
    r = start_fake("tail", "outs 250 line %d\noutlong 3000\nout last\nexit 0\n", &s, true, err, sizeof err);
    CHECK(r != NULL, "start: %s", err);
    if (!r) return;
    UNTIL_END(r, &in);
    CHECK(in.exit_status == 0 && count_lines(in.log) == DSG_RUN_LOG_LINES, "%d lines", count_lines(in.log));
    CHECK(strncmp(in.log, "line 52\n", 8) == 0, "the oldest kept: %.20s", in.log);
    {
        const char* x = strstr(in.log, "line 249\n");
        size_t n = 0;
        CHECK(x != NULL, "line 249 is there");
        if (x) {
            x += 9;
            while (x[n] == 'x') n++;
            CHECK(n == 1024 && x[n] == '\n', "a long line is cut at 1024: %d", (int)n);
            CHECK(strcmp(x + n, "\nlast\n") == 0, "then the last line");
        }
    }
    dsg_run_free(r);
}

/* Exit 1: the log's last line, as a Run issue; its rig flag. */
static void test_refused(void) {
    static const struct {
        const char* line;
        bool with_rig, rig;
    } cases[] = {
        { "role 'resp': the rig binds it to family 'xid', which the role does not list", true, true },
        { "the rig profile 'lab1': cannot read it", true, true },
        { "role 'resp': the rig binds it to family 'xid', which the role does not list", false, false },
        { "the trigger box did not answer", true, false },
        { "rigid: a word that only starts so", true, false },
    };
    int i;
    for (i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) {
        char script[1024], err[512] = "", name[32], want[1100];
        dsg_run_settings s;
        dsg_run_info in;
        dsg_issue is;
        dsg_run* r;
        sim_settings(&s);
        if (cases[i].with_rig) snprintf(s.rig, sizeof s.rig, "lab1");
        snprintf(name, sizeof name, "refused%d", i);
        snprintf(script, sizeof script, "out first line\nerr ysp_player: %s\nexit 1\n", cases[i].line);
        r = start_fake(name, script, &s, true, err, sizeof err);
        CHECK(r != NULL, "start: %s", err);
        if (!r) continue;
        UNTIL_END(r, &in);
        snprintf(want, sizeof want, "Refused or failed: %s", cases[i].line);
        CHECK(in.exit_status == 1 && strcmp(in.exit_text, want) == 0, "case %d: %d %s", i, in.exit_status, in.exit_text);
        CHECK(dsg_run_issue(r, &is) && is.severity == DSG_SEV_ERROR && is.from == DSG_FROM_PLAYER_RUN && is.place.line == 0 &&
                  is.message && strcmp(is.message, cases[i].line) == 0,
              "case %d: the issue '%s'", i, is.message ? is.message : "(none)");
        CHECK(is.rig == cases[i].rig, "case %d: rig %d", i, (int)is.rig);
        dsg_run_free(r);
    }
}

/* Stop: a player that takes SIGTERM ends with 3 and its stop line. */
static void test_stop(void) {
    char err[512] = "";
    dsg_run_settings s;
    dsg_run_info in;
    dsg_run* r;
    int sent;
    sim_settings(&s);
    r = start_fake("stop", "header trial_index,response\nrow 0,a\nrow 1,b\nterm exit 3\nout ready\nsleep 30000\n", &s, true, err,
                   sizeof err);
    CHECK(r != NULL, "start: %s", err);
    if (!r) return;
    UNTIL_LOG(r, "ready", &in);
    UNTIL_TRIALS(r, 2, &in);
    CHECK(in.state == DSG_RUN_RUNNING && !in.kill_offered, "running, no Kill before a Stop");
    sent = dsg_run_stop(r, t_now());
#if defined(_WIN32)
    /* part P: no SIGTERM, no window: Kill at once */
    dsg_run_get_info(r, &in);
    CHECK(sent == 0 && in.kill_offered && in.state == DSG_RUN_RUNNING, "Windows: Stop reaches nothing (%d), Kill at once", sent);
    dsg_run_kill(r);
    UNTIL_END(r, &in);
    CHECK(in.exit_status == -9 && strstr(in.exit_text, "Killed"), "Windows: killed %d %s", in.exit_status, in.exit_text);
#else
    dsg_run_get_info(r, &in);
    CHECK(sent == 1 && in.state == DSG_RUN_STOPPING && !in.kill_offered, "Stop went out (%d), state %d", sent, in.state);
    UNTIL_END(r, &in);
    CHECK(in.exit_status == 3 && strcmp(in.exit_text, "Stopped: 2 trials written; trial 2 has no row.") == 0, "stopped: %d %s",
          in.exit_status, in.exit_text);
    CHECK(!in.kill_offered, "no Kill after the end");
#endif
    dsg_run_free(r);
}

/* Kill: only after a Stop, DSG_RUN_KILL_AFTER_NS after it. */
static void test_kill(void) {
    char err[512] = "";
    dsg_run_settings s;
    dsg_run_info in;
    dsg_run* r;
    long long t0;
    int sent;
    sim_settings(&s);
    r = start_fake("kill", "term ignore\nout ready\nsleep 30000\n", &s, true, err, sizeof err);
    CHECK(r != NULL, "start: %s", err);
    if (!r) return;
    UNTIL_LOG(r, "ready", &in);
    dsg_run_kill(r);   /* no Stop yet: refused */
    t_sleep(300);
    dsg_run_tick(r, t_now());
    dsg_run_get_info(r, &in);
    CHECK(in.state != DSG_RUN_ENDED, "Kill before a Stop does nothing: state %d", in.state);
    t0 = t_now();
    sent = dsg_run_stop(r, t0);
#if defined(_WIN32)
    dsg_run_get_info(r, &in);
    CHECK(sent == 0 && in.kill_offered, "Windows: Kill at once");
#else
    CHECK(sent == 1, "Stop went out");
    t_sleep(300);
    dsg_run_tick(r, t0 + DSG_RUN_KILL_AFTER_NS - 1);
    dsg_run_get_info(r, &in);
    CHECK(in.state == DSG_RUN_STOPPING && !in.kill_offered, "ignored: still stopping, no Kill before 5 s (state %d)", in.state);
    CHECK(dsg_run_tick(r, t0 + DSG_RUN_KILL_AFTER_NS), "the tick at 5 s changes the info");
    dsg_run_get_info(r, &in);
    CHECK(in.kill_offered && in.state == DSG_RUN_STOPPING, "Kill offered at 5 s");
    CHECK(dsg_run_stop(r, t0 + DSG_RUN_KILL_AFTER_NS) == 1, "a second Stop goes out too");
#endif
    dsg_run_kill(r);
    {
        const long long end = t_now() + 10000000000LL;
        do {
            dsg_run_tick(r, t0 + DSG_RUN_KILL_AFTER_NS + (t_now() - t0));
            dsg_run_get_info(r, &in);
            if (in.state == DSG_RUN_ENDED) break;
            t_sleep(5);
        } while (t_now() < end);
    }
    CHECK(in.state == DSG_RUN_ENDED && in.exit_status == -9 && !in.kill_offered, "killed: %d", in.exit_status);
#if !defined(_WIN32)
    CHECK(strstr(in.exit_text, "signal 9 (SIGKILL)") && !strstr(in.exit_text, "xrandr"), "a simulated run: no display to put back: %s",
          in.exit_text);
#endif
    dsg_run_free(r);
}

#if !defined(_WIN32)
/* An end by a signal of a run with a display (dsg_run_desc.sim false): how
 * to put the display back. SIGINT: no core file. */
static void test_signal(void) {
    char err[512] = "";
    dsg_run_settings s;
    dsg_run_info in;
    dsg_run* r;
    sim_settings(&s);
    r = start_fake("signal", "out ready\nraise 2\nsleep 30000\n", &s, false, err, sizeof err);
    CHECK(r != NULL, "start: %s", err);
    if (!r) return;
    UNTIL_END(r, &in);
    CHECK(in.exit_status == -2 && strstr(in.exit_text, "Ended by signal 2 (SIGINT): the data set is not closed."), "%d %s",
          in.exit_status, in.exit_text);
#if !defined(__APPLE__)
    CHECK(strstr(in.exit_text, "xrandr") != NULL, "with a display: xrandr");
#endif
    dsg_run_free(r);
}
#endif

/* "Leave it running": the free leaves the player running, writing its log. */
static void test_leave(void) {
    char err[512] = "", script[1024], log[DSG_PATH_MAX + 8];
    dsg_run_settings s;
    dsg_run_info in;
    dsg_run* r;
    long long end;
    bool seen = false;
    sim_settings(&s);
    snprintf(script, sizeof script, "out ready\nwait %s\nout after the free\nexit 0\n", wpath("leave.go"));
    r = start_fake("leave", script, &s, true, err, sizeof err);
    CHECK(r != NULL, "start: %s", err);
    if (!r) return;
    UNTIL_LOG(r, "ready", &in);
    snprintf(log, sizeof log, "%s", in.log_path);
    dsg_run_free(r);
    go("leave.go");
    for (end = t_now() + 10000000000LL; t_now() < end && !(seen = file_has(log, "after the free"));) t_sleep(10);
    CHECK(seen, "the player went on after the free");
}

/* The output folder and its log must not exist; the version must be the
 * library's. Each refusal starts nothing. */
static void test_refusals(void) {
    char err[512] = "", ver[64] = "";
    dsg_run_settings s;
    dsg_run* r;
    sim_settings(&s);
    write_text(wpath("exists"), "a file");
    r = start_fake("exists", "exit 0\n", &s, true, err, sizeof err);
    CHECK(r == NULL && strstr(err, "exists: choose another"), "an output folder that exists: %s", err);
    CHECK(!dsg_is_file(wpath("exists.log")), "and no log");
    write_text(wpath("logexists.log"), "old log");
    r = start_fake("logexists", "exit 0\n", &s, true, err, sizeof err);
    CHECK(r == NULL && strstr(err, "it exists"), "a log that exists: %s", err);
    CHECK(file_has(wpath("logexists.log"), "old log"), "and its bytes stay");
    dsg_run_free(r);

    /* decision 5 */
    CHECK(dsg_player_version(fake, tmp_dir, ver, sizeof ver, err, sizeof err) == 0 && strcmp(ver, PL_VERSION_STRING) == 0,
          "the version: '%s' %s", ver, err);
    CHECK(!dsg_is_file(wpath("tmp/player_version.log")), "its answer's file is gone");
    set_env("YSP_FAKE_PLAYER_VERSION", "0.0.1");
    CHECK(dsg_player_version(fake, tmp_dir, ver, sizeof ver, err, sizeof err) == 0 && strcmp(ver, "0.0.1") == 0, "another: '%s'", ver);
    r = start_fake("version", "exit 0\n", &s, true, err, sizeof err);
    CHECK(r == NULL && strstr(err, "is version 0.0.1") && strstr(err, PL_VERSION_STRING), "another version is refused: %s", err);
    CHECK(!dsg_is_file(wpath("version.log")) && !dsg_is_dir(wpath("version")), "and nothing started");
    set_env("YSP_FAKE_PLAYER_OLD", "1");
    CHECK(dsg_player_version(fake, tmp_dir, ver, sizeof ver, err, sizeof err) == -1 && strstr(err, "ended with status 2") && ver[0] == 0,
          "a player before --version: %s", err);
    r = start_fake("version", "exit 0\n", &s, true, err, sizeof err);
    CHECK(r == NULL && strstr(err, "ended with status 2"), "and its run is refused: %s", err);
    set_env("YSP_FAKE_PLAYER_OLD", NULL);
    set_env("YSP_FAKE_PLAYER_VERSION", PL_VERSION_STRING);
    CHECK(dsg_player_version(wpath("no_such_player"), tmp_dir, ver, sizeof ver, err, sizeof err) == -1, "no player: %s", err);
    CHECK(dsg_player_version(fake, wpath("no_such_folder"), ver, sizeof ver, err, sizeof err) == -1, "no folder: %s", err);
    CHECK(!dsg_is_file(wpath("tmp/player_version.log")), "no answer's file is left");
}

/* The output folder's parents are made. */
static void test_parents(void) {
    char err[512] = "";
    dsg_run_settings s;
    dsg_run_info in;
    dsg_run* r;
    sim_settings(&s);
    r = start_fake("deep/er/run", "header trial_index\nrow 0\nexit 0\n", &s, true, err, sizeof err);
    CHECK(r != NULL, "start in a folder that is not there: %s", err);
    if (!r) return;
    UNTIL_END(r, &in);
    CHECK(in.exit_status == 0 && in.trials == 1 && dsg_is_file(wpath("deep/er/run.log")), "it ran: %d %d", in.exit_status, in.trials);
    dsg_run_free(r);
}

/* --- ysp_player --sim ----------------------------------------------------------- */

static void test_player(const char* player) {
    static dsg_run_argv a;
    char err[1024] = "", pack[DSG_PATH_MAX], desc[DSG_PATH_MAX], out[DSG_PATH_MAX], ver[64] = "";
    dsg_run_settings s;
    dsg_run_desc d;
    dsg_run_info in;
    dsg_run* r = NULL;
    ypt_options o;
    CHECK(dsg_player_version(player, tmp_dir, ver, sizeof ver, err, sizeof err) == 0 && strcmp(ver, PL_VERSION_STRING) == 0,
          "ysp_player --version: '%s' %s", ver, err);
    snprintf(desc, sizeof desc, "%s/tests/player/rdk/pack.json", YSP_SOURCE_DIR);
    snprintf(pack, sizeof pack, "%s", wpath("rdk.ysppak"));
    memset(&o, 0, sizeof o);
    if (ypt_build(desc, pack, &o, err, sizeof err) != 0) {
        CHECK(0, "build rdk: %s", err);
        return;
    }
    sim_settings(&s);
    snprintf(s.seed, sizeof s.seed, "7");
    s.n_fields = 1;
    snprintf(s.fields[0].name, sizeof s.fields[0].name, "participant");
    snprintf(s.fields[0].value, sizeof s.fields[0].value, "17");
    CHECK(dsg_run_out_dir(wpath("rdkproj"), &s, 1795355107, out, sizeof out) == 0 && strstr(out, "/data/17_20261122-134507"),
          "out dir %s", out);
    CHECK(dsg_run_args(&s, player, pack, out, &a, err, sizeof err) == 0, "args: %s", err);
    memset(&d, 0, sizeof d);
    d.argv = a.argv;
    d.cwd = runs;
    d.out_dir = out;
    d.tmp_dir = tmp_dir;
    d.sim = true;
    d.wake = on_wake;
    d.now_ns = t_now();
    CHECK(dsg_run_start(&r, &d, err, sizeof err) == 0, "start ysp_player --sim: %s", err);
    if (!r) return;
    UNTIL_END(r, &in);
    CHECK(in.state == DSG_RUN_ENDED && in.exit_status == 0, "rdk: exit %d (%s)\n%s", in.exit_status, in.exit_text, in.log);
    CHECK(in.trials == 40, "40 rows: %d", in.trials);
    CHECK((strcmp(in.last_response, "left") == 0 || strcmp(in.last_response, "right") == 0) && in.last_outcome[0] && in.last_rt_s > 0,
          "the last row: '%s' '%s' %g", in.last_response, in.last_outcome, in.last_rt_s);
    {
        char want[DSG_PATH_MAX + 64];
        snprintf(want, sizeof want, "ysp_player: 40 trials, %s, seed 7, experiment ", out);
        CHECK(strncmp(in.log, want, strlen(want)) == 0 && count_lines(in.log) == 1, "the log's end line: %s", in.log);
        snprintf(want, sizeof want, "Done: 40 trials in %s.", out);
        CHECK(strcmp(in.exit_text, want) == 0, "exit text: %s", in.exit_text);
        snprintf(want, sizeof want, "%s/session.json", out);
        CHECK(dsg_is_file(want), "the data set: %s", want);
    }
    dsg_run_free(r);
}

/* --- --measure ----------------------------------------------------------------- */

static int cmp_ll(const void* x, const void* y) {
    const long long a = *(const long long*)x, b = *(const long long*)y;
    return a < b ? -1 : a > b;
}

static void stats(const char* what, long long* v, int n) {
    qsort(v, (size_t)n, sizeof *v, cmp_ll);
    printf("%-62s n=%4d  median %9.2f us  p95 %9.2f us  max %9.2f us\n", what, n, (double)v[n / 2] / 1e3,
           (double)v[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1] / 1e3, (double)v[n - 1] / 1e3);
}

/* The wake-up after count n0, at most 2 s (the fake waits up to 180 s). */
static bool wait_wake(int32_t n0) {
    const long long end = t_now() + 2000000000LL;
    while (dsg_atomic_load(&woken) == n0)
        if (t_now() > end) {
            printf("FAIL: no wake-up within 2 s\n");
            return false;
        } else t_sleep(2);
    return true;
}

/* Appends rows to a run's trials.csv while the fake waits, and times the
 * tick that reads them, against the file's length. */
static int measure(const char* player) {
    enum { IDLE = 2000, GROW = 40, STARTS = 20 };
    static long long v[IDLE];
    static char rowbuf[1 << 20];
    static const char row[] =
        "rdk,rdk,%d,1.234567,0,0,0,1,0,0,0,0,0,1,1234567890,,,,,0.3,180,1,0,0,0,0,0,left,0.412345,1,left,1,hit\n";
    static const int sizes[] = { 0, 10000, 100000 };
    char err[512] = "", script[1024], tp[DSG_PATH_MAX];
    dsg_run_settings s;
    dsg_run_info in;
    dsg_run* r;
    FILE* f;
    int i, k, total = 0;
    /* the version check, and the whole start, in the caller */
    for (k = 0; k < 2; k++) {
        const char* p = k ? player : fake;
        char ver[64];
        if (!p || !p[0]) continue;
        for (i = 0; i < STARTS; i++) {
            const long long t = t_now();
            if (dsg_player_version(p, tmp_dir, ver, sizeof ver, err, sizeof err)) return 1;
            v[i] = t_now() - t;
        }
        stats(k ? "dsg_player_version(), ysp_player" : "dsg_player_version(), fake_player", v, STARTS);
    }
    sim_settings(&s);
    write_text(wpath("start.txt"), "exit 0\n");
    for (i = 0; i < STARTS; i++) {
        static dsg_run_argv a;
        char name[32];
        dsg_run_desc d;
        long long t;
        snprintf(name, sizeof name, "start%d", i);
        if (dsg_run_args(&s, fake, wpath("start.txt"), wpath(name), &a, err, sizeof err)) return 1;
        memset(&d, 0, sizeof d);
        d.argv = a.argv;
        d.cwd = runs;
        d.out_dir = wpath(name);
        d.tmp_dir = tmp_dir;
        d.sim = true;
        d.wake = on_wake;
        t = t_now();
        d.now_ns = t;
        if (dsg_run_start(&r, &d, err, sizeof err)) return 1;
        v[i] = t_now() - t;
        UNTIL_END(r, &in);
        dsg_run_free(r);
    }
    stats("dsg_run_start() of fake_player, the version check in it", v, STARTS);
    snprintf(script, sizeof script,
             "header node,trial_type,trial_index,time_elapsed_s,index,block,rep,condition,track,practice,warmup,requeued,after_break,"
             "first_in_block,trial_seed,method,level,estimate,a,b,c,d,e,f,g,h,i,response,rt_s,responded,x,correct,outcome\n"
             "out ready\nwait %s\nwait %s\nwait %s\nwait %s\nwait %s\nwait %s\nexit 0\n",
             wpath("measure.go"), wpath("measure.go"), wpath("measure.go"), wpath("measure.go"), wpath("measure.go"),
             wpath("measure.go"));
    r = start_fake("measure", script, &s, true, err, sizeof err);
    if (!r) {
        printf("FAIL: start: %s\n", err);
        return 1;
    }
    UNTIL_LOG(r, "ready", &in);
    snprintf(tp, sizeof tp, "%s/trials.csv", wpath("measure"));
    /* a tick with no wake-up: the steady state at 2 Hz and at every frame */
    for (i = 0; i < IDLE; i++) {
        const long long t = t_now();
        dsg_run_tick(r, t);
        v[i] = t_now() - t;
    }
    stats("tick, no wake-up (no read)", v, IDLE);
    for (k = 0; k < (int)(sizeof sizes / sizeof sizes[0]); k++) {
        /* the file to its length, read by one tick (the first read) */
        size_t at = 0;
        long long t;
        int add = sizes[k] - total;
        /* the count before the write: a wake-up between the write and the
         * read of the count would be missed */
        int n0 = dsg_atomic_load(&woken);
        f = fopen(tp, "ab");
        if (!f) return 1;
        for (i = 0; i < add; i++) {
            at += (size_t)snprintf(rowbuf + at, sizeof rowbuf - at, row, total + i);
            if (at > sizeof rowbuf - 256) {
                fwrite(rowbuf, 1, at, f);
                at = 0;
            }
        }
        fwrite(rowbuf, 1, at, f);
        fclose(f);
        total = sizes[k] > total ? sizes[k] : total;
        if (add > 0) {
            if (!wait_wake(n0)) return 1;
            t = t_now();
            dsg_run_tick(r, t);
            t = t_now() - t;
            dsg_run_get_info(r, &in);
            printf("first read of %6d rows (%7.1f KB): %9.1f us; trials %d\n", add, (double)add * (double)(sizeof row) / 1024.0,
                   (double)t / 1e3, in.trials);
        }
        /* then one row a wake-up: the tick that reads it */
        for (i = 0; i < GROW; i++) {
            n0 = dsg_atomic_load(&woken);
            f = fopen(tp, "ab");
            if (!f) return 1;
            fprintf(f, row, total);
            fclose(f);
            total++;
            if (!wait_wake(n0)) return 1;
            t = t_now();
            dsg_run_tick(r, t);
            v[i] = t_now() - t;
        }
        {
            char what[96];
            snprintf(what, sizeof what, "tick that reads one new row, file of %d rows", total);
            stats(what, v, GROW);
        }
    }
    dsg_run_get_info(r, &in);
    printf("rows read: %d (written %d)\n", in.trials, total);
    go("measure.go");
    UNTIL_END(r, &in);
    dsg_run_free(r);
    return in.trials == total ? 0 : 1;
}

int main(int argc, char** argv) {
    char err[512] = "";
    if (argc < 3) {
        printf("usage: test_designer_run WORK FAKE [PLAYER] [--measure]\n");
        return 2;
    }
#if defined(_WIN32)
    if (!(argv[1][0] && argv[1][1] == ':') && argv[1][0] != '/' && argv[1][0] != '\\') {
#else
    if (argv[1][0] != '/') {
#endif
        printf("FAIL: WORK must be absolute: %s\n", argv[1]);
        return 1;
    }
#if !defined(_WIN32)
    /* a run's watch thread writes nothing to a closed pipe, but the test's
     * own output may go to one */
    signal(SIGPIPE, SIG_IGN);
#endif
    snprintf(work, sizeof work, "%s", argv[1]);
    snprintf(fake, sizeof fake, "%s", argv[2]);
    if (dsg_make_dir(work, err, sizeof err) || dsg_remove_tree(work, "runs", err, sizeof err) ||
        dsg_join(runs, sizeof runs, work, "runs") || dsg_make_dir(runs, err, sizeof err) ||
        dsg_join(tmp_dir, sizeof tmp_dir, runs, "tmp") || dsg_make_dir(tmp_dir, err, sizeof err)) {
        printf("FAIL: the work folder: %s\n", err);
        return 1;
    }
    set_env("YSP_DESIGNER_TEST", "1");
    set_env("YSP_FAKE_PLAYER_VERSION", PL_VERSION_STRING);
    set_env("YSP_FAKE_PLAYER_OLD", NULL);
    if (argc > 3 && strcmp(argv[argc - 1], "--measure") == 0) {
        setvbuf(stdout, NULL, _IONBF, 0);
        return measure(argc > 4 ? argv[3] : NULL);
    }

    test_args();
    test_out_dir();
    test_exit_text();
    test_refusals();
    test_progress();
    test_lazy();
    test_log_tail();
    test_refused();
    test_stop();
    test_kill();
#if !defined(_WIN32)
    test_signal();
#endif
    test_leave();
    test_parents();
    if (argc > 3 && argv[3][0]) test_player(argv[3]);
    else printf("SKIP: ysp_player --sim (no PLAYER)\n");

    printf("test_designer_run: %d of %d checks pass\n", checks - fails, checks);
    return fails ? 1 : 0;
}
