/* mut_test.c - the player's checks for the mutation runner: the player's
 * sources compiled into one program (tests/mutate/player_*.toml), no SDL, and
 * definitions run in process against the RDK pack's table. `YPAK` (the pack
 * tool) builds the pack once in the test's temporary folder.
 *
 * Exit 0 when every check holds; a mutant that survives shows a lowering
 * rule no check can see. */
#include "impl.c"
#include "eval.c"
#include "host.c"
#include "lower.c"
#include "run.c"
#define main pl_cli_main
#include "main.c"
#undef main

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define MAXR 512
#define MAXC 96

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct table { char* cols[MAXC]; int n_cols; char* rows[MAXR][MAXC]; int n_rows; } table;

static char workdir[256], packpath[512];

static void read_csv(const char* path, table* t) {
    static char buf[8 * 1024 * 1024];
    FILE* fp = fopen(path, "r");
    char* line, *sp;
    int r = -1;
    t->n_cols = t->n_rows = 0;
    if (!fp) return;
    size_t n = fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    buf[n] = 0;
    for (line = strtok_r(buf, "\n", &sp); line; line = strtok_r(NULL, "\n", &sp)) {
        char* cell = line, *e;
        int c = 0;
        for (;;) {
            e = strchr(cell, ',');
            if (e) *e = 0;
            if (r < 0) t->cols[t->n_cols++] = strdup(cell);
            else if (c < MAXC) t->rows[r][c] = strdup(cell);
            c++;
            if (!e) break;
            cell = e + 1;
        }
        if (r < MAXR - 1) r++;
    }
    t->n_rows = r < 0 ? 0 : r;
}

static const char* cell(const table* t, int row, const char* col) {
    int i;
    for (i = 0; i < t->n_cols; i++)
        if (strcmp(t->cols[i], col) == 0) return row < t->n_rows && t->rows[row][i] ? t->rows[row][i] : "";
    return "<nocol>";
}

static table T;
static pl_player P;
static void (*g_hook)(void);

/* Run definition text against the RDK pack's table (conditions.pstb). */
static int run_def(const char* name, const char* def, const char* part, double rt) {
    char out[400];
    pl_args a;
    int rc;
    memset(&a, 0, sizeof a);
    snprintf(out, sizeof out, "%s/%s", workdir, name);
    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "rm -r %s 2>/dev/null", out);
        (void)!system(cmd);
    }
    a.pack_path = packpath;
    a.out_dir = out;
    a.sim = true;
    a.have_seed = true;
    a.seed = 99;
    a.participant = part;
    a.rt_s = rt;
    a.def_text = def;
    a.def_len = strlen(def);
    a.n_fields = 1;
    a.fields[0].name = "participant";
    a.fields[0].value = "3";
    a.max_seconds = 600;
    rc = pl_load(&P, &a);
    if (rc == 0) rc = pl_run(&P);
    if (rc != 0) printf("  run %s: %s\n", name, P.error);
    if (rc == 0 && g_hook) g_hook();
    g_hook = NULL;
    pl_close(&P);
    T.n_rows = 0;
    {
        char path[500];
        snprintf(path, sizeof path, "%s/trials.csv", out);
        read_csv(path, &T);
    }
    return rc;
}

#define HEADER "{\"format\":\"ysp-experiment\",\"version\":1,\"name\":\"m\",\"devices\":{\"resp\":{\"direction\":\"in\",\"family\":[\"keyboard\"],\"required\":true},\"out\":{\"direction\":\"out\",\"family\":[\"triggerbox\"],\"required\":true}},\"stimuli\":{\"s\":{\"kind\":\"shape\",\"w\":2,\"h\":2},\"u\":{\"kind\":\"shape\",\"w\":1,\"h\":1},\"r\":{\"kind\":\"rdk\",\"aperture\":\"circle\",\"w\":50,\"count\":10,\"speed\":10,\"dot_size\":2,\"color\":[1,1,1]}},"

static void case_basic(void) {
    const char* d = HEADER "\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"},{\"wait_s\":1},{\"off\":\"s\"}],\"response\":{\"role\":\"resp\",\"choices\":[\"left\",\"right\"],\"from\":\"s\",\"duration_s\":1.5,\"key_answer\":\"$key_answer\"},\"after\":[{\"mark\":\"m1\"}],\"columns\":{\"ct\":\"$key_answer\"}}},"
                    "\"timeline\":[{\"name\":\"a\",\"timeline_variables\":\"conditions.pstb\",\"trial\":\"t\",\"rules\":[\"order sequential\"]},"
                    "{\"name\":\"b\",\"timeline_variables\":\"conditions.pstb\",\"trial\":\"t\",\"rules\":[\"order sequential\"]}]}";
    CHECK(run_def("basic", d, "correct", 0.2) == 0, "basic");
    CHECK(T.n_rows == 16, "16 rows: %d", T.n_rows);
    CHECK(strcmp(cell(&T, 0, "trial_index"), "0") == 0 && strcmp(cell(&T, 8, "trial_index"), "8") == 0, "trial_index joins the session: %s %s", cell(&T, 8, "trial_index"), cell(&T, 8, "index"));
    CHECK(strcmp(cell(&T, 8, "index"), "0") == 0, "index restarts per leaf");
    CHECK(strcmp(cell(&T, 8, "node"), "b") == 0, "node");
    CHECK(strcmp(cell(&T, 0, "rt_s"), "0.200000000") == 0, "rt: %s", cell(&T, 0, "rt_s"));
    CHECK(strcmp(cell(&T, 0, "correct"), "1") == 0 && strcmp(cell(&T, 0, "outcome"), "correct") == 0, "correct: %s %s", cell(&T, 0, "correct"), cell(&T, 0, "outcome"));
    {
        int i, all = 1;
        for (i = 0; i < T.n_rows; i++) if (strcmp(cell(&T, i, "correct"), "1") != 0) all = 0;
        CHECK(all, "the participant that answers correctly is correct on every row");
    }
    CHECK(strcmp(cell(&T, 0, "ct"), cell(&T, 0, "key_answer")) == 0, "template column");
    CHECK(strlen(cell(&T, 0, "trial_seed")) == 17 && cell(&T, 0, "trial_seed")[0] == 'x', "trial_seed: %s", cell(&T, 0, "trial_seed"));
    /* D8 pinned: the first trial's seed of node `a` for session seed 99 */
    CHECK(strcmp(cell(&T, 0, "trial_seed"), PIN_A0) == 0, "trial_seed pin: %s", cell(&T, 0, "trial_seed"));
    /* after starts at the end of the events with an early response (s off at 1 s) */
    {
        char p[500]; table E;
        double on = 0, mark = 0;
        int i;
        memset(&E, 0, sizeof E);
        snprintf(p, sizeof p, "%s/basic/events.csv", workdir);
        read_csv(p, &E);
        for (i = 0; i < E.n_rows; i++) {
            if (strcmp(cell(&E, i, "trial_index"), "0")) continue;
            if (!strcmp(cell(&E, i, "kind"), "onset") && !on) on = atof(cell(&E, i, "t_s"));
            if (!strcmp(cell(&E, i, "text"), "m1")) mark = atof(cell(&E, i, "t_s"));
        }
        CHECK(mark - on > 0.99 && mark - on < 1.02, "after starts at the events' end: %g", mark - on);
    }
}

static void case_correct(void) {
    /* always press "right": conditions rotate left/right, so half are wrong */
    const char* d = HEADER "\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}],\"response\":{\"role\":\"resp\",\"choices\":[\"left\",\"right\"],\"from\":\"s\",\"duration_s\":1,\"key_answer\":\"$key_answer\"}}},"
                    "\"timeline\":[{\"name\":\"a\",\"timeline_variables\":\"conditions.pstb\",\"trial\":\"t\",\"rules\":[\"order sequential\"]}]}";
    FILE* fp;
    char path[300];
    int i, correct = 0;
    snprintf(path, sizeof path, "%s/s.csv", workdir);
    fp = fopen(path, "w");
    fprintf(fp, "trial_index,response,rt_s,hold_s\n");
    for (i = 0; i < 8; i++) fprintf(fp, "%d,right,0.2,0.05\n", i);
    fclose(fp);
    {
        char part[320];
        snprintf(part, sizeof part, "script:%s", path);
        CHECK(run_def("correct", d, part, 0) == 0, "script run");
    }
    for (i = 0; i < T.n_rows; i++) if (strcmp(cell(&T, i, "correct"), "1") == 0) correct++;
    CHECK(correct == 4, "right press matches key_answer half the time: %d", correct);
}

static void case_loop_requeue(void) {
    const char* d = HEADER "\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}],\"response\":{\"role\":\"resp\",\"choices\":[\"left\",\"right\"],\"from\":\"s\",\"duration_s\":1,\"key_answer\":\"$key_answer\"}}},"
                    "\"timeline\":[{\"name\":\"a\",\"timeline_variables\":\"conditions.pstb\",\"trial\":\"t\",\"loop\":{\"until_accuracy\":1,\"max\":3},\"rules\":[\"order sequential\"]}]}";
    CHECK(run_def("loop1", d, "correct", 0.2) == 0 && T.n_rows == 8, "all correct: one iteration, %d rows", T.n_rows);
    CHECK(run_def("loop3", d, "p=0.0", 0.2) == 0 && T.n_rows == 24, "never correct: 3 iterations, %d rows", T.n_rows);
    CHECK(strcmp(cell(&T, 8, "trial_seed"), cell(&T, 0, "trial_seed")) != 0, "each iteration has its own seed");
    {
        const char* r = HEADER "\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}],\"response\":{\"role\":\"resp\",\"choices\":[\"left\",\"right\"],\"from\":\"s\",\"duration_s\":0.5,\"key_answer\":\"$key_answer\"}}},"
                    "\"timeline\":[{\"name\":\"a\",\"timeline_variables\":\"conditions.pstb\",\"trial\":\"t\",\"requeue\":\"no_response\",\"rules\":[\"order sequential\"]}]}";
        FILE* fp;
        char path[300], part[320];
        int i, sum = 0;
        snprintf(path, sizeof path, "%s/rq.csv", workdir);
        fp = fopen(path, "w");
        fprintf(fp, "trial_index,response,rt_s,hold_s\n0,,0,0\n");
        for (i = 1; i < 16; i++) fprintf(fp, "%d,left,0.2,0.05\n", i);
        fclose(fp);
        snprintf(part, sizeof part, "script:%s", path);
        CHECK(run_def("requeue", r, part, 0.2) == 0, "requeue");
        for (i = 0; i < T.n_rows; i++) if (strcmp(cell(&T, i, "requeued"), "1") == 0) sum++;
        CHECK(T.n_rows == 9 && sum == 1, "a timeout re-runs once: %d rows, %d requeued", T.n_rows, sum);
    }
}

static void case_if_node(void) {
    const char* d = "{\"format\":\"ysp-experiment\",\"version\":1,\"name\":\"m\",\"session\":{\"fields\":{\"participant\":{\"type\":\"integer\",\"required\":true},\"flag\":{\"type\":\"integer\",\"required\":false}}},\"devices\":{\"resp\":{\"direction\":\"in\",\"family\":[\"keyboard\"],\"required\":true}},\"stimuli\":{\"s\":{\"kind\":\"shape\",\"w\":2,\"h\":2}},"
                    "\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"},{\"wait_s\":0.1},{\"off\":\"s\"}],\"response\":{\"role\":\"resp\",\"from\":\"s\",\"duration_s\":0.5}}},"
                    "\"timeline\":[{\"name\":\"yes\",\"if\":\"$participant\",\"trial\":\"t\",\"rules\":[\"reps 2\"]},{\"name\":\"no\",\"unless\":\"$participant\",\"trial\":\"t\",\"rules\":[\"reps 3\"]}]}";
    /* participant = 3 is not 0 or 1: refused */
    CHECK(run_def("if3", d, "silent", 0.2) != 0 && strstr(P.error, "0 or 1"), "a field not 0 or 1: %s", P.error);
}

static void case_method(void) {
    const char* d = "{\"format\":\"ysp-experiment\",\"version\":1,\"name\":\"m\",\"devices\":{\"resp\":{\"direction\":\"in\",\"family\":[\"keyboard\"],\"required\":true}},\"stimuli\":{\"s\":{\"kind\":\"shape\",\"w\":2,\"h\":2}},"
                    "\"methods\":{\"st\":{\"type\":\"stair\",\"start\":0.5,\"n_up\":1,\"n_down\":1,\"step_type\":\"lin\",\"steps\":[0.1],\"stop_trials\":3}},"
                    "\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}],\"response\":{\"role\":\"resp\",\"choices\":[\"left\"],\"from\":\"s\",\"duration_s\":0.5,\"key_answer\":\"left\"},\"method_value\":\"$level\"}},"
                    "\"timeline\":[{\"name\":\"a\",\"trial\":\"t\",\"tracks\":[\"st\"],\"interleave\":\"round_robin\"}]}";
    FILE* fp;
    char path[300], part[320];
    int i, timeouts = 0;
    snprintf(path, sizeof path, "%s/m.csv", workdir);
    fp = fopen(path, "w");
    fprintf(fp, "trial_index,response,rt_s,hold_s\n0,left,0.2,0.05\n1,,0,0\n2,left,0.2,0.05\n3,left,0.2,0.05\n4,left,0.2,0.05\n");
    fclose(fp);
    snprintf(part, sizeof part, "script:%s", path);
    CHECK(run_def("method", d, part, 0.2) == 0, "method");
    CHECK(T.n_rows == 4, "a timeout is not told to the track: 4 rows, got %d", T.n_rows);
    for (i = 0; i < T.n_rows; i++) if (strcmp(cell(&T, i, "outcome"), "timeout") == 0) timeouts++;
    CHECK(timeouts == 1, "one timeout row");
    /* log domain: a method value of 0 does not reach log10 */
    {
        const char* q = "{\"format\":\"ysp-experiment\",\"version\":1,\"name\":\"m\",\"devices\":{\"resp\":{\"direction\":\"in\",\"family\":[\"keyboard\"],\"required\":true}},\"stimuli\":{\"s\":{\"kind\":\"shape\",\"w\":2,\"h\":2}},"
                    "\"methods\":{\"q\":{\"type\":\"quest\",\"scale\":\"log10\",\"pf\":\"gumbel\",\"stop_trials\":3,\"estimate\":\"mean\",\"stim\":[{\"linspace\":[-2,0,5]}],\"param\":[{\"linspace\":[-2,0,5]},{\"linspace\":[1,3,3]},0.5,0.02]}},"
                    "\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}],\"response\":{\"role\":\"resp\",\"choices\":[\"left\"],\"from\":\"s\",\"duration_s\":0.5,\"key_answer\":\"left\"},\"method_value\":0}},"
                    "\"timeline\":[{\"name\":\"a\",\"trial\":\"t\",\"tracks\":[\"q\"]}]}";
        CHECK(run_def("logfloor", q, "correct", 0.2) == 0, "a method value of 0 on a log10 method: %s", P.error);
        CHECK(T.n_rows == 3, "3 trials: %d", T.n_rows);
    }
}

static void hook_density(void) {
    pl_stim* s = pl_stim_by_name(&P, "d");
    CHECK(s && s->rdk_count == s->field.n && s->rdk_count == 314, "density x area dots: %d", s ? s->rdk_count : -1);
}

/* the host's role check against a real rig profile (the shapes run with --sim) */
static void case_role(void) {
    static yrig_profile prof;
    static pl_player q;
    yexp_host h;
    char msg[200];
    yexp_role role;
    const char* fam[1] = { "triggerbox" };
    memset(&q, 0, sizeof q);
    yrig_init(&prof, "rig");
    q.rig = &prof;
    q.have_rig = true;
    pl_host_init(&q, &h);
    memset(&role, 0, sizeof role);
    role.name = "trig";
    role.family = fam;
    role.n_family = 1;
    role.required = true;
    CHECK(h.role(h.ctx, &role, msg, sizeof msg) != 0, "unbound required role refused in a real run");
    q.args.sim = true;
    CHECK(h.role(h.ctx, &role, msg, sizeof msg) == 0, "--sim stands in");
    q.args.sim = false;
    {
        yrig_role* r = yrig_add(&prof, "trig");
        snprintf(r->family, sizeof r->family, "parallel");
    }
    CHECK(h.role(h.ctx, &role, msg, sizeof msg) != 0, "wrong family refused");
    snprintf(yrig_find(&prof, "trig")->family, 16, "triggerbox");
    CHECK(h.role(h.ctx, &role, msg, sizeof msg) == 0, "a listed family binds");
}

static void case_density(void) {
    const char* d = "{\"format\":\"ysp-experiment\",\"version\":1,\"name\":\"m\",\"devices\":{},\"stimuli\":{\"d\":{\"kind\":\"rdk\",\"aperture\":\"circle\",\"w\":200,\"density\":0.01,\"speed\":5,\"dot_size\":2,\"color\":[1,1,1]}},\"trials\":{\"t\":{\"events\":[{\"on\":\"d\"},{\"wait_s\":0.05},{\"off\":\"d\"}]}},\"timeline\":[{\"name\":\"a\",\"trial\":\"t\"}]}";
    g_hook = hook_density;
    CHECK(run_def("density", d, "correct", 0.2) == 0, "density: %s", P.error);
}

static void case_rdk(void) {
    const char* d = HEADER "\"trials\":{\"t\":{\"events\":[{\"on\":\"r\"},{\"wait_s\":0.1},{\"off\":\"r\"}]}},"
                    "\"timeline\":[{\"name\":\"a\",\"trial\":\"t\",\"rules\":[\"reps 2\"]}]}";
    CHECK(run_def("rdk", d, "correct", 0.2) == 0, "rdk: %s", P.error);
    CHECK(T.n_rows == 2 && atof(cell(&T, 0, "r_updates")) > 1, "rdk updates: %s", cell(&T, 0, "r_updates"));
    /* D8: the rdk's seed is the trial's */
    {
        char want[32];
        unsigned long long v = strtoull(cell(&T, 0, "trial_seed") + 1, NULL, 16);
        snprintf(want, sizeof want, "%llu", v);
        CHECK(strcmp(cell(&T, 0, "r_seed"), want) == 0, "rdk seed = trial seed: %s vs %s", cell(&T, 0, "r_seed"), want);
    }
    CHECK(strcmp(cell(&T, 0, "r_digest"), cell(&T, 1, "r_digest")) != 0, "two trials, two fields");
}

static void case_code_range(void) {
    const char* d = HEADER "\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"},{\"trigger\":300,\"role\":\"out\"},{\"off\":\"s\"}]}},"
                    "\"timeline\":[{\"name\":\"a\",\"trial\":\"t\"}]}";
    CHECK(run_def("code", d, "correct", 0.2) != 0 && strstr(P.error, "0..255"), "a trigger code past a port's byte: %s", P.error);
}

static void hook_aftervar(void) {
    pl_stim* s = pl_stim_by_name(&P, "t");
    CHECK(s && s->gs.x == 1.0f, "a key known in `after` is applied: x = %g", s ? s->gs.x : -1.0f);
}

static void case_after_var(void) {
    /* a stimulus key whose lookup is by $outcome: known only in `after` */
    const char* d = "{\"format\":\"ysp-experiment\",\"version\":1,\"name\":\"m\",\"devices\":{\"resp\":{\"direction\":\"in\",\"family\":[\"keyboard\"],\"required\":true}},\"stimuli\":{\"s\":{\"kind\":\"shape\",\"w\":2,\"h\":2},\"t\":{\"kind\":\"shape\",\"w\":1,\"h\":1,\"x\":{\"by\":\"$outcome\",\"map\":{\"correct\":1,\"wrong\":2,\"timeout\":3}}}},"
                    "\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}],\"response\":{\"role\":\"resp\",\"choices\":[\"left\"],\"from\":\"s\",\"duration_s\":0.5,\"key_answer\":\"left\"},\"after\":[{\"on\":\"t\"},{\"wait_s\":0.1},{\"off\":\"t\"}]}},"
                    "\"timeline\":[{\"name\":\"a\",\"trial\":\"t\",\"rules\":[\"reps 2\"]}]}";
    g_hook = hook_aftervar;
    CHECK(run_def("aftervar", d, "correct", 0.2) == 0, "a stimulus key by $outcome: %s", P.error);
    CHECK(T.n_rows == 2, "rows");
}

static void case_stagger_trigger(void) {
    const char* d = HEADER "\"trials\":{\"t\":{\"events\":[{\"on\":[\"s\",\"u\"],\"stagger_s\":0.1},{\"trigger\":3,\"role\":\"out\"},{\"wait_s\":0.2},{\"off\":[\"s\",\"u\"]}]}},"
                    "\"timeline\":[{\"name\":\"a\",\"trial\":\"t\",\"rules\":[\"reps 2\"]}]}";
    CHECK(run_def("trig", d, "correct", 0.2) == 0, "trigger: %s", P.error);
    {
        /* the stagger: u lands 0.1 s after s */
        char path[500]; table E;
        double a = 0, b = 0;
        int i;
        memset(&E, 0, sizeof E);
        snprintf(path, sizeof path, "%s/trig/events.csv", workdir);
        read_csv(path, &E);
        for (i = 0; i < E.n_rows; i++) {
            if (strcmp(cell(&E, i, "trial_index"), "0") || strcmp(cell(&E, i, "kind"), "onset")) continue;
            if (!strcmp(cell(&E, i, "stimulus"), "s")) a = atof(cell(&E, i, "t_s"));
            if (!strcmp(cell(&E, i, "stimulus"), "u")) b = atof(cell(&E, i, "t_s"));
        }
        CHECK(b - a > 0.09 && b - a < 0.12, "the stagger: %g", b - a);
    }
    CHECK(T.n_rows == 2 && strcmp(cell(&T, 0, "out_n"), "1") == 0 && strcmp(cell(&T, 0, "out_failed"), "0") == 0, "one code per trial: %s %s", cell(&T, 0, "out_n"), cell(&T, 0, "out_failed"));
    {
        char path[500]; table E;
        int i, outs = 0;
        memset(&E, 0, sizeof E);
        snprintf(path, sizeof path, "%s/trig/events.csv", workdir);
        read_csv(path, &E);
        for (i = 0; i < E.n_rows; i++) if (!strcmp(cell(&E, i, "kind"), "out") && !strcmp(cell(&E, i, "code"), "3")) outs++;
        CHECK(outs == 2, "out rows: %d", outs);
    }
}

/* the system tests too: tests/player/player_test.py against this binary, as
 * YPL_AS_PLAYER makes it the command line */
static int run_shapes(void) {
    char cmd[4096], self[1024];
    const char* repo = getenv("YPL_REPO");
    const char* bin = getenv("YPL_BIN");
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    int rc;
    if (n <= 0 || !repo || !bin) { printf("run_shapes needs YPL_REPO and YPL_BIN\n"); return 2; }
    self[n] = 0;
    snprintf(cmd, sizeof cmd, "YPL_AS_PLAYER=1 python3 '%s/tests/player/player_test.py' --bin '%s' --player '%s' --out '%s/shapes' > '%s/shapes.txt' 2>&1; st=$?; grep -E 'FAIL|cases' '%s/shapes.txt' | head -12; exit $st", repo, bin, self, workdir, workdir, workdir);
    rc = system(cmd);
    return WEXITSTATUS(rc);
}

int main(int argc, char** argv) {
    const char* ypak = getenv("YPAK");
    char cmd[1024];
    if (getenv("YPL_AS_PLAYER")) return pl_cli_main(argc, argv);
    snprintf(workdir, sizeof workdir, "/tmp/pl_mut_%d", (int)getpid());
    mkdir(workdir, 0755);
    snprintf(packpath, sizeof packpath, "%s/rdk.ysppak", workdir);
    if (!ypak) { printf("YPAK not set\n"); return 2; }
    snprintf(cmd, sizeof cmd, "cd %s/tests/player/rdk && '%s' build pack.json -o %s >/dev/null", getenv("YPL_REPO") ? getenv("YPL_REPO") : ".", ypak, packpath);
    if (system(cmd) != 0) { printf("ypak failed\n"); return 2; }
    yrt_virtual_begin(0);
    case_basic();
    case_correct();
    case_loop_requeue();
    case_if_node();
    case_method();
    case_after_var();
    case_density();
    case_role();
    case_rdk();
    case_code_range();
    case_stagger_trigger();
    if (fails == 0 && run_shapes() != 0) { fails++; printf("FAIL line %d: the system tests\n", __LINE__); }
    snprintf(cmd, sizeof cmd, "rm -r %s", workdir);
    (void)!system(cmd);
    printf("mut_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
