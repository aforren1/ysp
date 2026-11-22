/* rig_test.c - rig profiles in the designer (designer/core/rig.c and the
 * rig's parts of check.c and preview.c; docs/designer.md, "Part G as
 * built" of step 6).
 *
 * The user's config folder is the test's: XDG_CONFIG_HOME (Windows:
 * APPDATA; macOS: HOME) at WORK/cfg, or, when WORK is under a folder that
 * all users can write (/tmp, which yrt_user_dir() rightly refuses), at a
 * folder of its own under HOME/.cache that the test removes at its end.
 * Never the user's real rig folder: the test checks that first.
 *
 * Parts: (1) the list; (2) a load: the profile, its calibration, the
 * banner, the counts; (3) a changed loopback file's note, in the load and
 * in the check; (4) a folder that others can write; (5) the check with a
 * profile: the needs as in the player's case_rig, the calibration, a
 * refusal that the profile causes; (6) loads once per choice or change, not
 * per check; (7) the preview: a stimulus in deg at yrig_ppd()'s scale, a
 * relative role at the profile's counts, a change of the rig restarts, a
 * change of the file reloads at a restart; (8) the same on the check
 * thread (for ThreadSanitizer).
 *
 * Usage: test_designer_rig WORK */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1   /* setenv(), utimensat() under -std=c11 */
#endif
#include "preview.h"
#include "doc.h"
#include "player.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <process.h>
#define getpid _getpid
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char* work;
static char base[1024];          /* the config folder's base: XDG_CONFIG_HOME */
static char rigdir[1200];        /* yrig_dir() under it                       */
static bool base_in_home;        /* made under HOME: removed at the end       */
static const int64_t FR = 16666667;

/* --- files ------------------------------------------------------------------------- */

static int write_bytes(const char* path, const void* data, size_t n) {
    char err[512];
    if (dsg_write_file(path, data, n, err, sizeof err) != 0) {
        printf("  %s\n", err);
        return -1;
    }
    return 0;
}

static int write_in(const char* dir, const char* name, const void* data, size_t n) {
    char path[DSG_PATH_MAX];
    if (dsg_join(path, sizeof path, dir, name)) return -1;
    return write_bytes(path, data, n);
}

static void remove_in(const char* dir, const char* name) {
    char path[DSG_PATH_MAX];
    if (dsg_join(path, sizeof path, dir, name) == 0) remove(path);
}

static int mkdir_private(const char* path) {
#if defined(_WIN32)
    return _mkdir(path) == 0 || dsg_is_dir(path) ? 0 : -1;
#else
    if (mkdir(path, 0700) != 0 && !dsg_is_dir(path)) return -1;
    return chmod(path, 0700);
#endif
}

static void set_config_home(const char* dir) {
#if defined(_WIN32)
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, 1024);
    SetEnvironmentVariableW(L"APPDATA", w);
#elif defined(__APPLE__)
    setenv("HOME", dir, 1);
#else
    setenv("XDG_CONFIG_HOME", dir, 1);
#endif
}

static void sha_hex(const void* data, size_t n, char hex[65]) {
    static const char H[] = "0123456789abcdef";
    uint8_t d[32];
    int i;
    yrig_sha256(data, n, d);
    for (i = 0; i < 32; i++) {
        hex[i * 2] = H[d[i] >> 4];
        hex[i * 2 + 1] = H[d[i] & 15];
    }
    hex[64] = 0;
}

static const char BOUNDS[] = "# a test fixture, not a measurement\nformat ysp-in-bounds 1\ndate 2026-11-22T00:00:00Z\nin_family xid\n"
                             "n 200\nmissed 0\nlo_s -0.000100000\nmedian_s 0.000050000\nhi_s 0.000400000\n";
static char bounds_sha[65], cal_sha[65];
static ycol_cal nominal;
static unsigned char cal_bytes[sizeof(ycol_cal)];
static int cal_n;

#define VIEW "{\"distance_mm\": 570, \"width_mm\": 518.4, \"width_px\": 1920}"
/* far from the built-in view: 83.8 px a degree, not 36.9 */
#define VIEW_FAR "{\"distance_mm\": 1000, \"width_mm\": 400, \"width_px\": 1920}"

/* rigdir/NAME.json from its parts (player/check_test.c's rig_make()) */
static int write_profile(const char* name, const char* roles, const char* display, const char* view, const char* signals) {
    char text[4096], file[96];
    const int n = snprintf(text, sizeof text,
                           "{\"format\": \"ysp-rig 1\", \"rig\": \"test %s\", \"written\": \"2026-11-22T00:00:00Z\", \"roles\": {%s}%s%s%s%s%s%s}\n",
                           name, roles, display ? ", \"display\": " : "", display ? display : "", view ? ", \"view\": " : "", view ? view : "",
                           signals ? ", \"signals\": " : "", signals ? signals : "");
    snprintf(file, sizeof file, "%s.json", name);
    return write_in(rigdir, file, text, (size_t)n);
}

static void display_with(char* out, size_t cap, const char* sha) {
    snprintf(out, cap, "{\"onset_offset_s\": 0, \"calibration\": {\"file\": \"display.yspcal\", \"sha256\": \"%s\"}}", sha);
}

static void roles_full(char* out, size_t cap) {
    snprintf(out, cap,
             "\"resp\": {\"family\": \"xid\", \"key\": \"port:/dev/null\", \"bounds\": {\"n\": 200, \"lo_s\": -0.0001, \"hi_s\": 0.0004, "
             "\"date\": \"2026-11-22T00:00:00Z\", \"sha256\": \"%s\"}}, \"trig\": {\"family\": \"triggerbox\", \"key\": \"port:/dev/null\"}",
             bounds_sha);
}

static const char SIGNALS[] =
    "{\"hand\": {\"adapter\": \"raw_mouse\", \"units\": \"display_px\", \"per_count\": 1, \"device\": \"click\"}, "
    "\"left\": {\"adapter\": \"raw_mouse\", \"units\": \"display_cm\", \"per_count\": 0.01, \"device\": \"click\"}, "
    "\"right\": {\"adapter\": \"raw_mouse\", \"units\": \"display_cm\", \"per_count\": [0.02, -0.01], \"device\": \"click\"}}";

/* The rig folder: the nominal calibration and a bounds loopback for an xid
 * role, as player/check_test.c makes them. */
static int rig_files(void) {
    char lb[1300], name[80];
    cal_n = ycol_cal_save(&nominal, cal_bytes, sizeof cal_bytes);
    if (cal_n < 0) return -1;
    sha_hex(BOUNDS, strlen(BOUNDS), bounds_sha);
    sha_hex(cal_bytes, (size_t)cal_n, cal_sha);
    snprintf(lb, sizeof lb, "%s/loopback", rigdir);
    if (mkdir_private(lb)) return -1;
    snprintf(name, sizeof name, "%s.txt", bounds_sha);
    if (write_in(lb, name, BOUNDS, strlen(BOUNDS)) || write_in(rigdir, "display.yspcal", cal_bytes, (size_t)cal_n)) return -1;
    return 0;
}

/* tests/player/NAME's files into WORK/NAME. */
static int copy_project(const char* name, const char* const* files, char* folder, size_t cap) {
    char src[DSG_PATH_MAX], a[DSG_PATH_MAX], b[DSG_PATH_MAX], err[512];
    int i;
    snprintf(src, sizeof src, "%s/tests/player/%s", YSP_SOURCE_DIR, name);
    if (dsg_join(folder, cap, work, name) || dsg_make_dir(folder, err, sizeof err)) return -1;
    if (dsg_remove_tree(folder, DSG_DIR, err, sizeof err)) return -1;
    for (i = 0; files[i]; i++) {
        char* d = NULL;
        size_t n = 0;
        if (dsg_join(a, sizeof a, src, files[i]) || dsg_join(b, sizeof b, folder, files[i])) return -1;
        if (dsg_read_file(a, &d, &n, err, sizeof err)) {
            printf("  %s\n", err);
            return -1;
        }
        if (write_bytes(b, d, n)) {
            free(d);
            return -1;
        }
        free(d);
    }
    return 0;
}

static int make_project(const char* name, const char* def, char* folder, size_t cap) {
    static const char kPack[] =
        "{\"format\": \"ysp-pack-source\", \"version\": 1,\n"
        " \"experiment\": {\"entry\": \"experiment/main.json\", \"source\": \"experiment.json\", \"media_type\": \"application/vnd.ysp.experiment\"},\n"
        " \"resources\": []}\n";
    char err[512];
    if (dsg_join(folder, cap, work, name) || dsg_make_dir(folder, err, sizeof err)) return -1;
    if (dsg_remove_tree(folder, DSG_DIR, err, sizeof err)) return -1;
    if (write_in(folder, "pack.json", kPack, strlen(kPack)) || write_in(folder, "experiment.json", def, strlen(def))) return -1;
    return 0;
}

/* --- the check --------------------------------------------------------------------- */

static const char* const need_names[] = { "", "field", "calibration", "view", "display_tier", "device", "role_tier", "signal", "sim_gain" };

typedef struct C {
    dsg_checker* c;
    dsg_project  proj;
    dsg_report   r;
} C;

static int c_open(C* k, const char* folder) {
    char err[1024];
    memset(k, 0, sizeof *k);
    k->c = dsg_checker_new();
    if (!k->c || dsg_project_open(&k->proj, folder, err, sizeof err)) {
        printf("  open %s: %s\n", folder, err);
        return -1;
    }
    return 0;
}

static void c_close(C* k) {
    dsg_project_close(&k->proj);
    dsg_checker_free(k->c);
    k->c = NULL;
}

static const dsg_report* c_check(C* k, const char* rig) {
    dsg_checker_set_rig(k->c, rig);
    memset(&k->r, 0, sizeof k->r);
    (void)dsg_check(k->c, &k->proj, k->proj.text, k->proj.text_len, NULL, &k->r);
    return &k->r;
}

/* The needs, "kind:name" sorted, as the player's needs_text() less the value. */
static void needs_of(const dsg_report* r, char* buf, size_t cap) {
    char lines[64][128];
    int i, j, n = 0;
    size_t used = 0;
    for (i = 0; i < r->n_issues && n < 64; i++) {
        const dsg_issue* is = &r->issues[i];
        if (is->severity != DSG_SEV_NEED) continue;
        snprintf(lines[n++], sizeof lines[0], "%s:%s", is->object > 0 && is->object <= 8 ? need_names[is->object] : "?", is->name ? is->name : "");
    }
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && strcmp(lines[j - 1], lines[j]) > 0; j--) {
            char t[128];
            memcpy(t, lines[j], sizeof t);
            memcpy(lines[j], lines[j - 1], sizeof t);
            memcpy(lines[j - 1], t, sizeof t);
        }
    buf[0] = 0;
    for (i = 0; i < n && used < cap; i++) used += (size_t)snprintf(buf + used, cap - used, "%s%s", i ? " " : "", lines[i]);
}

/* The issues of one source and severity (-1: any) whose message has s. */
static int count_issues(const dsg_report* r, int from, int sev, const char* s, bool* rig) {
    int i, n = 0;
    if (rig) *rig = true;
    for (i = 0; i < r->n_issues; i++) {
        const dsg_issue* is = &r->issues[i];
        if ((from >= 0 && is->from != from) || (sev >= 0 && is->severity != sev) || (s && !strstr(is->message, s))) continue;
        n++;
        if (rig) *rig = *rig && is->rig;
    }
    return n;
}

static void dump(const dsg_report* r) {
    int i;
    for (i = 0; i < r->n_issues; i++)
        printf("    [%d from %d rig %d] %s\n", r->issues[i].severity, r->issues[i].from, (int)r->issues[i].rig, r->issues[i].message);
}

/* --- (1) the list -------------------------------------------------------------------- */

static void part_list(void) {
    char names[8][DSG_RIG_NAME], err[512], dirj[1300];
    int n;
    n = dsg_rig_list(names, 8, err, sizeof err);
    CHECK(n == 0, "list: an empty folder gives %d (%s)", n, err);
    if (write_profile("profile", "", NULL, VIEW, NULL) || write_profile("booth-2", "", NULL, NULL, NULL) || write_profile("a", "", NULL, NULL, NULL) ||
        write_profile("bad name", "", NULL, NULL, NULL) || write_profile("x.b", "", NULL, NULL, NULL) || write_in(rigdir, "notes.txt", "x\n", 2) ||
        write_in(rigdir, "profile.json.tmp", "{}", 2)) {
        CHECK(0, "list: the files");
        return;
    }
    snprintf(dirj, sizeof dirj, "%s/dir.json", rigdir);
    CHECK(mkdir_private(dirj) == 0, "list: a folder named dir.json");
    n = dsg_rig_list(names, 8, err, sizeof err);
    CHECK(n == 3 && strcmp(names[0], "profile") == 0 && strcmp(names[1], "a") == 0 && strcmp(names[2], "booth-2") == 0,
          "list: %d: %s %s %s", n, n > 0 ? names[0] : "", n > 1 ? names[1] : "", n > 2 ? names[2] : "");
    n = dsg_rig_list(names, 2, err, sizeof err);
    CHECK(n == 2 && strcmp(names[0], "profile") == 0 && strcmp(names[1], "a") == 0, "list: cap 2 gives %d", n);
    remove_in(rigdir, "profile.json");
    n = dsg_rig_list(names, 8, err, sizeof err);
    CHECK(n == 2 && strcmp(names[0], "a") == 0 && strcmp(names[1], "booth-2") == 0, "list: with no profile.json %d: %s", n, n > 0 ? names[0] : "");
    remove_in(rigdir, "a.json");
    remove_in(rigdir, "bad name.json");
    remove_in(rigdir, "x.b.json");
    remove_in(rigdir, "notes.txt");
    remove_in(rigdir, "profile.json.tmp");
#if defined(_WIN32)
    _rmdir(dirj);
#else
    rmdir(dirj);
#endif
    printf("  list: ok\n");
}

/* --- (2) a load ---------------------------------------------------------------------- */

static void part_load(void) {
    char roles[1024], display[256], err[512], line[256];
    dsg_rig* r = NULL;
    double cpu[2] = { 0, 0 };
    const yrig_profile* p;
    roles_full(roles, sizeof roles);
    display_with(display, sizeof display, cal_sha);
    if (write_profile("booth-2", roles, display, VIEW, SIGNALS)) {
        CHECK(0, "load: the profile");
        return;
    }
    CHECK(dsg_rig_load("booth-2", &r, err, sizeof err) == 0 && r, "load: %s", err);
    if (!r) return;
    p = dsg_rig_profile(r);
    CHECK(strcmp(dsg_rig_name(r), "booth-2") == 0 && strstr(dsg_rig_path(r), "booth-2.json") && strncmp(dsg_rig_path(r), rigdir, strlen(rigdir)) == 0,
          "load: name '%s', path '%s'", dsg_rig_name(r), dsg_rig_path(r));
    CHECK(p && strcmp(p->rig, "test booth-2") == 0 && p->n_roles == 2 && p->n_notes == 0 && p->display.check == YRIG_CHECK_OK,
          "load: the profile's rig '%s', %d roles, %d notes", p ? p->rig : "", p ? p->n_roles : -1, p ? p->n_notes : -1);
    CHECK(dsg_rig_cal(r) && memcmp(dsg_rig_cal(r), &nominal, sizeof nominal) == 0, "load: the calibration is not the file's");
    dsg_rig_summary(r, line, sizeof line);
    CHECK(strcmp(line, "Rig: booth-2 (view 570 mm, 1920 px over 518.4 mm)") == 0, "load: the banner '%s'", line);
    CHECK(!dsg_rig_changed(r), "load: changed at once");
    /* the counts a role unit: 1 / (per_count x the units' factor) */
    CHECK(dsg_rig_counts_per_unit(r, "hand", YRIG_UNITS_DISPLAY_PX, cpu) && cpu[0] == 1.0 && cpu[1] == 1.0, "counts: hand px %g %g", cpu[0], cpu[1]);
    CHECK(dsg_rig_counts_per_unit(r, "left", YRIG_UNITS_DISPLAY_CM, cpu) && fabs(cpu[0] - 100.0) < 1e-9 && fabs(cpu[1] - 100.0) < 1e-9,
          "counts: left cm %g %g", cpu[0], cpu[1]);
    {
        const double f = yrig_units_factor(p, YRIG_UNITS_DISPLAY_CM, YRIG_UNITS_DISPLAY_DEG);
        CHECK(dsg_rig_counts_per_unit(r, "right", YRIG_UNITS_DISPLAY_DEG, cpu) && fabs(cpu[0] - 1.0 / (0.02 * f)) < 1e-9 &&
                  fabs(cpu[1] + 1.0 / (0.01 * f)) < 1e-9 && cpu[1] < 0,
              "counts: right deg %g %g, want %g %g", cpu[0], cpu[1], 1.0 / (0.02 * f), -1.0 / (0.01 * f));
    }
    CHECK(!dsg_rig_counts_per_unit(r, "nosuch", YRIG_UNITS_DISPLAY_PX, cpu) && cpu[0] == 1.0 && cpu[1] == 1.0, "counts: a role with no signal");
    CHECK(!dsg_rig_counts_per_unit(r, "left", YRIG_UNITS_WORKSPACE_CM, cpu) && cpu[0] == 1.0, "counts: units that do not convert");
    dsg_rig_free(r);
    r = NULL;
    /* no view: the banner says so */
    if (write_profile("plain", "", NULL, NULL, NULL) == 0 && dsg_rig_load("plain", &r, err, sizeof err) == 0) {
        dsg_rig_summary(r, line, sizeof line);
        CHECK(strcmp(line, "Rig: plain (no view)") == 0 && !dsg_rig_cal(r), "load: no view '%s'", line);
        dsg_rig_free(r);
    } else CHECK(0, "load: plain: %s", err);
    r = (dsg_rig*)1;
    CHECK(dsg_rig_load("nosuch", &r, err, sizeof err) != 0 && !r && strstr(err, "no rig profile"), "load: a missing profile: '%s'", err);
    CHECK(dsg_rig_load("../x", &r, err, sizeof err) != 0 && !r && strstr(err, "not a profile name"), "load: a bad name: '%s'", err);
    remove_in(rigdir, "plain.json");
    dsg_rig_summary(NULL, line, sizeof line);
    CHECK(!dsg_rig_profile(NULL) && !dsg_rig_cal(NULL) && !dsg_rig_changed(NULL) && strcmp(dsg_rig_name(NULL), "") == 0, "load: NULL");
    dsg_rig_free(NULL);
    printf("  load: ok\n");
}

/* --- (3) a changed loopback file -------------------------------------------------------------- */

static void part_note(const char* oddball) {
    char roles[1024], lb[1300], name[80], err[512], needs[1024];
    char changed[sizeof BOUNDS + 32];
    dsg_rig* r = NULL;
    C k;
    bool rig = false;
    const dsg_report* rep;
    roles_full(roles, sizeof roles);
    if (write_profile("lb", roles, NULL, VIEW, NULL)) {
        CHECK(0, "note: the profile");
        return;
    }
    snprintf(lb, sizeof lb, "%s/loopback", rigdir);
    snprintf(name, sizeof name, "%s.txt", bounds_sha);
    if (c_open(&k, oddball)) {
        CHECK(0, "note: open oddball");
        return;
    }
    rep = c_check(&k, "lb");
    needs_of(rep, needs, sizeof needs);
    CHECK(count_issues(rep, DSG_FROM_RIG, -1, NULL, NULL) == 0 && strcmp(needs, "display_tier: field:participant") == 0,
          "note: checked loopback: needs '%s'", needs);
    /* the file changes after the measurement: its hash no longer matches */
    snprintf(changed, sizeof changed, "%s# edited\n", BOUNDS);
    if (write_in(lb, name, changed, strlen(changed))) {
        CHECK(0, "note: the change");
        c_close(&k);
        return;
    }
    CHECK(dsg_rig_load("lb", &r, err, sizeof err) == 0 && r, "note: load %s", err);
    if (r) {
        const yrig_profile* p = dsg_rig_profile(r);
        CHECK(p->n_notes == 1 && strstr(p->notes[0], "roles.resp.bounds") && strstr(p->notes[0], "UNKNOWN"), "note: %d notes: %s", p->n_notes,
              p->n_notes ? p->notes[0] : "");
        dsg_rig_free(r);
    }
    /* the profile's file did not change: the check loads it again because
     * the caller says so (a new name), as V's look at the files does */
    rep = c_check(&k, "");
    rep = c_check(&k, "lb");
    needs_of(rep, needs, sizeof needs);
    CHECK(count_issues(rep, DSG_FROM_RIG, DSG_SEV_NOTE, "roles.resp.bounds", &rig) == 1 && rig && count_issues(rep, -1, -1, "'lb'", NULL) == 0,
          "note: the check's note (rig %d)", (int)rig);
    CHECK(strcmp(needs, "display_tier: field:participant role_tier:resp") == 0, "note: the tier need is back: '%s'", needs);
    CHECK(strcmp(rep->rig, "lb") == 0 && rep->status == DSG_CHECK_OK && rep->n_errors == 0, "note: report rig '%s' status %d", rep->rig, rep->status);
    if (fails) dump(rep);
    write_in(lb, name, BOUNDS, strlen(BOUNDS));
    c_close(&k);
    remove_in(rigdir, "lb.json");
    printf("  note: ok\n");
}

/* --- (4) a folder that others can write ------------------------------------------------------------ */

static void part_folder(const char* oddball) {
#if defined(_WIN32)
    (void)oddball;
    printf("  folder: POSIX only\n");
#else
    char names[4][DSG_RIG_NAME], err[512];
    dsg_rig* r = NULL;
    C k;
    bool rig = false;
    const dsg_report* rep;
    if (write_profile("profile", "", NULL, VIEW, NULL) || c_open(&k, oddball)) {
        CHECK(0, "folder: setup");
        return;
    }
    rep = c_check(&k, "profile");
    CHECK(count_issues(rep, DSG_FROM_RIG, -1, NULL, NULL) == 0, "folder: a good folder gave a rig issue");
    CHECK(chmod(rigdir, 0777) == 0, "folder: chmod");
    CHECK(dsg_rig_list(names, 4, err, sizeof err) == -1 && strstr(err, "another user can write it"), "folder: the list '%s'", err);
    CHECK(dsg_rig_load("profile", &r, err, sizeof err) == -1 && !r && strstr(err, "another user can write it"), "folder: the load '%s'", err);
    /* the next load: another choice, then this one again (a profile that
     * loaded stays until its file changes) */
    (void)c_check(&k, "");
    rep = c_check(&k, "profile");
    CHECK(count_issues(rep, DSG_FROM_RIG, DSG_SEV_ERROR, "another user can write it", &rig) == 1 && rig && strcmp(rep->rig, "profile") == 0,
          "folder: the check's error");
    {
        /* with no profile: the view need is back */
        char needs[512];
        needs_of(rep, needs, sizeof needs);
        CHECK(strstr(needs, "view:") != NULL, "folder: the check ran with no profile: '%s'", needs);
    }
    CHECK(chmod(rigdir, 0700) == 0, "folder: chmod back");
    rep = c_check(&k, "profile");
    CHECK(count_issues(rep, DSG_FROM_RIG, -1, NULL, NULL) == 0, "folder: the refusal stayed after the fix");
    c_close(&k);
    remove_in(rigdir, "profile.json");
    printf("  folder: ok\n");
#endif
}

/* --- (5) the check with a profile ------------------------------------------------------------------- */

static void part_check(const char* oddball, const char* afc2, const char* visuomotor, const char* bimanual, const char* rdk) {
    char roles[1024], display[256], needs[1024];
    C k;
    bool rig = false;
    const dsg_report* rep;
    size_t i;
    roles_full(roles, sizeof roles);
    display_with(display, sizeof display, cal_sha);
    if (write_profile("full", roles, display, VIEW, SIGNALS) || write_profile("room", "", display, VIEW, SIGNALS) ||
        write_profile("part", "\"resp\": {\"family\": \"xid\", \"key\": \"port:/dev/null\"}", NULL, NULL, NULL) ||
        write_profile("wrong", "\"resp\": {\"family\": \"line\", \"key\": \"port:/dev/null\"}", NULL, NULL, NULL)) {
        CHECK(0, "check: the profiles");
        return;
    }
    {
        /* the player's case_rig, through the designer: each need the
         * profile meets is gone, the others stay */
        static const struct { int proj; const char* rig; const char* needs; } want[] = {
            { 0, "", "device:trig display_tier: field:participant role_tier:resp view:" },
            { 0, "full", "display_tier: field:participant" },
            { 0, "part", "device:trig display_tier: field:participant role_tier:resp view:" },
            { 1, "", "calibration: display_tier: field:participant view:" },
            { 1, "room", "display_tier: field:participant" },
            { 2, "full", "field:participant" },
            { 3, "full", "field:participant" },
            { 3, "part", "field:participant signal:left signal:right sim_gain:left sim_gain:right view:" },
        };
        const char* folders[4];
        folders[0] = oddball;
        folders[1] = afc2;
        folders[2] = visuomotor;
        folders[3] = bimanual;
        for (i = 0; i < sizeof want / sizeof want[0]; i++) {
            if (c_open(&k, folders[want[i].proj])) {
                CHECK(0, "check: open %d", want[i].proj);
                continue;
            }
            rep = c_check(&k, want[i].rig);
            needs_of(rep, needs, sizeof needs);
            CHECK(rep->status == DSG_CHECK_OK && rep->n_errors == 0 && strcmp(needs, want[i].needs) == 0 && strcmp(rep->rig, want[i].rig) == 0,
                  "check %d with '%s': status %d, %d errors, needs '%s', want '%s'", want[i].proj, want[i].rig, rep->status, rep->n_errors, needs,
                  want[i].needs);
            if (rep->n_errors) dump(rep);
            c_close(&k);
        }
    }
    /* the start's refusal, caused by the profile: marked, the name not in
     * the message; with no profile it is not there */
    if (c_open(&k, oddball) == 0) {
        rep = c_check(&k, "wrong");
        CHECK(rep->status != DSG_CHECK_OK && count_issues(rep, -1, DSG_SEV_ERROR, "the rig binds it to family 'line'", &rig) == 1 && rig,
              "check: the profile's refusal (status %d, rig %d)", rep->status, (int)rig);
        CHECK(count_issues(rep, -1, -1, "'wrong'", NULL) == 0, "check: a message repeats the profile's name");
        if (fails) dump(rep);
        rep = c_check(&k, "");
        CHECK(rep->status == DSG_CHECK_OK && rep->n_errors == 0 && rep->rig[0] == 0, "check: with no profile it is gone (status %d)", rep->status);
        c_close(&k);
    }
    /* an error of the definition is not the profile's: not marked */
    if (c_open(&k, rdk) == 0) {
        char* bad = (char*)malloc(k.proj.text_len + 1);
        const char* key = "\"timeline\"";
        char* at;
        memcpy(bad, k.proj.text, k.proj.text_len + 1);
        at = strstr(bad, key);
        if (at) memcpy(at, "\"timeLine\"", strlen(key));
        dsg_checker_set_rig(k.c, "wrong");
        memset(&k.r, 0, sizeof k.r);
        (void)dsg_check(k.c, &k.proj, bad, k.proj.text_len, NULL, &k.r);
        CHECK(k.r.n_errors > 0 && count_issues(&k.r, DSG_FROM_LOADER, DSG_SEV_ERROR, NULL, NULL) > 0 &&
                  count_issues(&k.r, DSG_FROM_LOADER, DSG_SEV_ERROR, "the rig binds", NULL) == 0,
              "check: a definition error with a profile: %d errors", k.r.n_errors);
        {
            int j, marked = 0;
            for (j = 0; j < k.r.n_issues; j++) marked += k.r.issues[j].rig && k.r.issues[j].from != DSG_FROM_RIG && !strstr(k.r.issues[j].message, "the rig binds");
            CHECK(marked == 0, "check: %d definition errors marked as the profile's", marked);
        }
        free(bad);
        c_close(&k);
    }
    /* a definition that needs nothing of the profile: the same issues */
    if (c_open(&k, rdk) == 0) {
        char a[4096] = "", b[4096] = "";
        size_t ua = 0, ub = 0;
        int j;
        rep = c_check(&k, "");
        for (j = 0; j < rep->n_issues; j++) ua += (size_t)snprintf(a + ua, sizeof a - ua, "%d|%s;", rep->issues[j].severity, rep->issues[j].message);
        rep = c_check(&k, "room");
        for (j = 0; j < rep->n_issues; j++) ub += (size_t)snprintf(b + ub, sizeof b - ub, "%d|%s;", rep->issues[j].severity, rep->issues[j].message);
        CHECK(strcmp(a, b) == 0, "check: rdk with no profile '%s', with room '%s'", a, b);
        c_close(&k);
    }
    /* the calibration (Part P): changed, missing, or not a calibration */
    if (c_open(&k, afc2) == 0) {
        static ycol_cal other;
        static unsigned char other_bytes[sizeof(ycol_cal)];
        static const float srgb_xy[4][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.3290f } };
        char junk_sha[65];
        static const char junk[] = "not a calibration";
        int on;
        CHECK(ycol_cal_nominal(&other, srgb_xy, 80.0f, 2.4) == 0, "cal: another calibration");
        on = ycol_cal_save(&other, other_bytes, sizeof other_bytes);
        /* changed: a valid file with another hash: the need stays, a note */
        write_in(rigdir, "display.yspcal", other_bytes, (size_t)(on > 0 ? on : 0));
        rep = c_check(&k, "room");
        needs_of(rep, needs, sizeof needs);
        CHECK(strcmp(needs, "calibration: display_tier: field:participant") == 0 && rep->n_errors == 0 &&
                  count_issues(rep, DSG_FROM_RIG, DSG_SEV_NOTE, "has changed", &rig) == 1 && rig,
              "cal changed: needs '%s', %d errors", needs, rep->n_errors);
        /* missing: a real start cannot read it */
        remove_in(rigdir, "display.yspcal");
        rep = c_check(&k, "room");
        CHECK(count_issues(rep, DSG_FROM_RIG, DSG_SEV_ERROR, "cannot be read", &rig) == 1 && rig && count_issues(rep, DSG_FROM_RIG, DSG_SEV_NOTE, "is missing", NULL) == 1,
              "cal missing: %d errors", rep->n_errors);
        /* its hash is right, but it is not a calibration */
        sha_hex(junk, strlen(junk), junk_sha);
        write_in(rigdir, "display.yspcal", junk, strlen(junk));
        display_with(display, sizeof display, junk_sha);
        write_profile("junk", "", display, VIEW, NULL);
        rep = c_check(&k, "junk");
        needs_of(rep, needs, sizeof needs);
        CHECK(count_issues(rep, DSG_FROM_RIG, DSG_SEV_ERROR, "display.yspcal", &rig) == 1 && rig && strstr(needs, "calibration:"),
              "cal junk: %d errors, needs '%s'", rep->n_errors, needs);
        if (fails) dump(rep);
        write_in(rigdir, "display.yspcal", cal_bytes, (size_t)cal_n);
        remove_in(rigdir, "junk.json");
        c_close(&k);
    }
    printf("  check: ok\n");
}

/* --- (6) loads once per choice or change ---------------------------------------------------------- */

#if !defined(_WIN32)
static int mtime_of(const char* path, struct timespec* ts) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *ts = st.st_mtim;
    return 0;
}
#endif

static void part_reload(const char* oddball) {
    char path[1300], needs[1024], roles[1024];
    C k;
    const dsg_report* rep;
    bool rig = false;
    snprintf(path, sizeof path, "%s/swap.json", rigdir);
    /* two profiles of one size: no view (a need), and the view's place
     * filled with spaces */
    roles_full(roles, sizeof roles);
    if (write_profile("swap", "", NULL, VIEW, NULL) || c_open(&k, oddball)) {
        CHECK(0, "reload: setup");
        return;
    }
    rep = c_check(&k, "swap");
    needs_of(rep, needs, sizeof needs);
    CHECK(!strstr(needs, "view:"), "reload: the view is met: '%s'", needs);
#if !defined(_WIN32)
    {
        /* the same size and time, other bytes: the checker keeps what it
         * loaded, so it did not load at this check */
        struct timespec ts[2];
        char* d = NULL;
        size_t n = 0;
        char err[512];
        char* v;
        if (mtime_of(path, &ts[1]) == 0 && dsg_read_file(path, &d, &n, err, sizeof err) == 0 && (v = strstr(d, ", \"view\"")) != NULL) {
            const size_t span = strlen(", \"view\": " VIEW);
            memset(v, ' ', span);
            write_bytes(path, d, n);
            ts[0] = ts[1];
            CHECK(utimensat(AT_FDCWD, path, ts, 0) == 0, "reload: utimensat");
            rep = c_check(&k, "swap");
            needs_of(rep, needs, sizeof needs);
            CHECK(!strstr(needs, "view:"), "reload: a check with the files unchanged loaded the profile again: '%s'", needs);
            /* now its time changes: the next check loads it */
            ts[1].tv_sec += 2;
            ts[0] = ts[1];
            CHECK(utimensat(AT_FDCWD, path, ts, 0) == 0, "reload: utimensat 2");
            rep = c_check(&k, "swap");
            needs_of(rep, needs, sizeof needs);
            CHECK(strstr(needs, "view:") != NULL, "reload: a changed file was not loaded again: '%s'", needs);
        } else CHECK(0, "reload: read %s", path);
        free(d);
    }
#endif
    /* another size: loaded again */
    write_profile("swap", "", NULL, VIEW, NULL);
    rep = c_check(&k, "swap");
    needs_of(rep, needs, sizeof needs);
    CHECK(!strstr(needs, "view:"), "reload: the view again: '%s'", needs);
    /* a profile that does not load, then one that does */
    write_in(rigdir, "swap.json", "{\"format\": \"ysp-rig 1\"", 22);
    rep = c_check(&k, "swap");
    CHECK(count_issues(rep, DSG_FROM_RIG, DSG_SEV_ERROR, NULL, &rig) == 1 && rig, "reload: a broken profile: %d errors", rep->n_errors);
    rep = c_check(&k, "swap");
    CHECK(count_issues(rep, DSG_FROM_RIG, DSG_SEV_ERROR, NULL, &rig) == 1, "reload: a broken profile, again");
    CHECK(count_issues(rep, -1, -1, "'swap'", NULL) == 0, "reload: a message repeats the profile's name");
#if !defined(_WIN32)
    {
        /* a good profile of the broken one's size and time: the checker does
         * not parse it again, as nothing it looks at changed */
        static const char good[] = "{\"format\": \"ysp-rig 1\", \"rig\": \"s\", \"written\": \"2026-11-22T00:00:00Z\", \"roles\": {}}";
        char broken[sizeof good];
        struct timespec ts[2];
        memset(broken, ' ', sizeof broken - 1);
        broken[sizeof broken - 1] = 0;
        memcpy(broken, "{\"format\": 1", 12);
        write_in(rigdir, "swap.json", broken, strlen(broken));
        rep = c_check(&k, "swap");
        CHECK(count_issues(rep, DSG_FROM_RIG, DSG_SEV_ERROR, NULL, NULL) == 1, "reload: the padded broken profile");
        if (mtime_of(path, &ts[1]) == 0) {
            write_in(rigdir, "swap.json", good, strlen(good));
            ts[0] = ts[1];
            CHECK(utimensat(AT_FDCWD, path, ts, 0) == 0, "reload: utimensat 3");
            rep = c_check(&k, "swap");
            CHECK(count_issues(rep, DSG_FROM_RIG, DSG_SEV_ERROR, NULL, NULL) == 1, "reload: a failed profile was parsed again with its file unchanged");
        }
    }
#endif
    write_profile("swap", roles, NULL, VIEW, NULL);
    rep = c_check(&k, "swap");
    needs_of(rep, needs, sizeof needs);
    CHECK(count_issues(rep, DSG_FROM_RIG, -1, NULL, NULL) == 0 && strcmp(needs, "display_tier: field:participant") == 0, "reload: fixed: '%s'", needs);
    /* the name changes: loaded */
    write_profile("other", "", NULL, NULL, NULL);
    rep = c_check(&k, "other");
    needs_of(rep, needs, sizeof needs);
    CHECK(strstr(needs, "view:") && strcmp(rep->rig, "other") == 0, "reload: another name: '%s'", needs);
    c_close(&k);
    remove_in(rigdir, "swap.json");
    remove_in(rigdir, "other.json");
    printf("  reload: ok\n");
}

/* --- (7) the preview --------------------------------------------------------------------------------- */

typedef struct U {
    dsg_preview_gl gl;
    dsg_worker*    w;
    dsg_doc*       doc;
    dsg_preview*   pv;
    dsg_project    proj;
    bool           open, threaded;
    uint64_t       posted, floor;
    int64_t        now;
    char           rig[DSG_RIG_NAME];     /* the preview's and the check's */
} U;

static int u_start(U* u, bool threaded) {
    dsg_worker_desc wd;
    char err[512];
    memset(u, 0, sizeof *u);
    memset(&wd, 0, sizeof wd);
    dsg_preview_session_ops(&u->gl, &wd.session);
    wd.no_thread = !threaded;
    u->threaded = threaded;
    u->w = dsg_worker_start(&wd, err, sizeof err);
    u->doc = dsg_doc_new();
    if (!u->w || !u->doc || dsg_doc_reset(u->doc, "", "", 0)) return -1;
    u->pv = dsg_preview_new(&u->gl, u->w, u->doc);
    u->now = 5000000000LL;
    return u->pv ? 0 : -1;
}

static void u_close(U* u) {
    if (!u->open) return;
    dsg_preview_close(u->pv);
    dsg_project_close(&u->proj);
    u->open = false;
    dsg_worker_forget(u->w);
    u->floor = dsg_doc_revision(u->doc) + 1;
}

static void u_stop(U* u) {
    u_close(u);
    dsg_preview_free(u->pv);
    dsg_worker_stop(u->w);
    dsg_doc_free(u->doc);
}

static void post_check(U* u) {
    size_t n = 0;
    const char* t = dsg_doc_bytes(u->doc, DSG_BUF_DEF, &n);
    const uint64_t rev = dsg_doc_revision(u->doc);
    if (dsg_worker_check(u->w, &u->proj, t, n, u->doc, rev, u->rig) == 0) u->posted = rev;
    if (!u->threaded) (void)dsg_worker_wait(u->w, 0);
}

static void u_rig(U* u, const char* rig) {
    dsg_preview_settings ps;
    dsg_preview_get(u->pv, &ps);
    snprintf(u->rig, sizeof u->rig, "%s", rig);
    snprintf(ps.session.rig, sizeof ps.session.rig, "%s", rig);
    dsg_preview_set(u->pv, &ps);
}

static int u_open(U* u, const char* folder, const char* rig) {
    char err[1024];
    dsg_preview_settings ps;
    u_close(u);
    if (dsg_project_open(&u->proj, folder, err, sizeof err)) {
        printf("  open %s: %s\n", folder, err);
        return -1;
    }
    u->open = true;
    dsg_doc_reset(u->doc, u->proj.def_source, u->proj.text, u->proj.text_len);
    u->floor = dsg_doc_revision(u->doc);
    dsg_preview_open(u->pv, &u->proj);
    dsg_preview_settings_default(&ps);
    dsg_preview_set(u->pv, &ps);
    u_rig(u, rig);
    post_check(u);
    return 0;
}

static void frame(U* u) {
    const dsg_check_result* res = NULL;
    dsg_session_result sr;
    u->now += FR;
    if (u->threaded) (void)dsg_worker_wait(u->w, 60000000000LL);
    if (dsg_worker_take_check(u->w, &res) && u->open && res->revision >= u->floor) dsg_preview_checked(u->pv, res, u->now);
    memset(&sr, 0, sizeof sr);
    if (dsg_worker_take_session(u->w, &sr)) dsg_preview_adopt(u->pv, &sr, u->now);
    dsg_preview_tick(u->pv, u->now, true);
    if (u->open && dsg_doc_revision(u->doc) != u->posted) post_check(u);
}

static dsg_preview_info info(U* u) {
    dsg_preview_info i;
    memset(&i, 0, sizeof i);
    dsg_preview_get_info(u->pv, &i);
    return i;
}

static dsg_preview_where where(U* u) {
    dsg_preview_where w;
    memset(&w, 0, sizeof w);
    (void)dsg_preview_where_now(u->pv, &w);
    return w;
}

static bool until_running(U* u, int k) {
    int i;
    for (i = 0; i < k; i++) {
        frame(u);
        if (info(u).status == DSG_PV_RUNNING && !info(u).attaching && !info(u).loading) return true;
    }
    return false;
}

static bool until_other(U* u, const char* out0, int k) {
    int i;
    for (i = 0; i < k; i++) {
        frame(u);
        if (info(u).status == DSG_PV_RUNNING && !info(u).attaching && strcmp(info(u).out_dir, out0) != 0) return true;
    }
    return false;
}

static bool in_state(U* u, const char* s) { return where(u).state && strcmp(where(u).state, s) == 0; }

static bool until_state(U* u, const char* s, int k) {
    int i;
    for (i = 0; i < k; i++) {
        if (in_state(u, s)) return true;
        frame(u);
    }
    return in_state(u, s);
}

/* A position2d role (relative: position2d_relative) in deg and a region
 * at CX, CY of radius R: the trial leaves "wait" when the role is in it
 * (tests/designer/preview_test.c's sig_def()). */
static void sig_def(char* out, size_t cap, const char* units, const char* role, bool rel, double cx, double cy, double r) {
    snprintf(out, cap,
             "{\"format\": \"ysp-experiment\", \"version\": 2, \"name\": \"sig\",\n"
             " \"display\": {\"units\": \"%s\"},\n"
             " \"roles\": {\"%s\": {\"type\": \"%s\", \"units\": \"display_deg\"}},\n"
             " \"stimuli\": {\"s\": {\"kind\": \"shape\", \"shape\": \"circle\", \"w\": 1, \"h\": 1}},\n"
             " \"trials\": {\"t\": {\"controls\": {\"in\": {\"control\": \"region\", \"of\": \"%s\", \"center\": [%.6f, %.6f], \"radius\": %.6f}},\n"
             "   \"states\": [{\"name\": \"wait\", \"steps\": [{\"on\": \"s\"}], \"exits\": [{\"on\": \"in\", \"to\": \"done\"}]},\n"
             "              {\"name\": \"done\", \"steps\": [{\"off\": \"s\"}, {\"wait_s\": 0.2}]}]}},\n"
             " \"timeline\": [{\"name\": \"l1\", \"trial\": \"t\", \"rules\": [\"reps 3\"]}]}\n",
             units, role, rel ? "position2d_relative" : "position2d", role, cx, cy, r);
}

static double ppd_of(const char* name) {
    dsg_rig* r = NULL;
    char err[512];
    double ppd = 0;
    if (dsg_rig_load(name, &r, err, sizeof err) == 0) ppd = yrig_ppd(dsg_rig_profile(r));
    dsg_rig_free(r);
    return ppd;
}

/* The pointer at (cx, cy) deg at the profile's scale ends the state; at
 * the built-in rig's it would be 2.3 times as far out. */
static void preview_deg(U* u, const char* folder, const char* tag) {
    const double cx = 5, cy = -3, ppd = ppd_of("far");
    int k;
    CHECK(ppd > 80 && ppd < 90, "%s: ppd %g", tag, ppd);
    /* the last test's pointer is in its region: out of the display */
    dsg_preview_pointer(u->pv, 0.0f, 0.0f, false);
    CHECK(u_open(u, folder, "far") == 0, "%s: open", tag);
    if (!until_running(u, 40)) {
        dsg_issue is;
        memset(&is, 0, sizeof is);
        (void)dsg_preview_issue(u->pv, &is);
        CHECK(0, "%s: no session: status %d, '%s'", tag, info(u).status, is.message ? is.message : "");
    }
    CHECK(until_state(u, "wait", 20), "%s: no wait state", tag);
    dsg_preview_pointer(u->pv, 960.0f, 540.0f, true);
    for (k = 0; k < 3; k++) frame(u);
    CHECK(in_state(u, "wait"), "%s: the center ended the state", tag);
    dsg_preview_pointer(u->pv, (float)(960.0 + cx * ppd), (float)(540.0 + cy * ppd), true);
    CHECK(until_state(u, "done", 10), "%s: at (%g, %g) deg by the profile's %g px a degree the state is %s", tag, cx, cy, ppd,
          where(u).state ? where(u).state : "-");
}

static void part_preview(const char* abs_folder, const char* rel_folder, const char* px_folder, const char* oddball) {
    U u;
    int k;
    char out0[DSG_PATH_MAX];
    const double ppd = ppd_of("far");
    if (u_start(&u, false)) {
        CHECK(0, "preview: start");
        return;
    }
    /* a stimulus in deg at yrig_ppd()'s scale */
    preview_deg(&u, px_folder, "preview px");
    preview_deg(&u, abs_folder, "preview deg");
    /* a change of the rig restarts the session: the built-in rig */
    snprintf(out0, sizeof out0, "%s", info(&u).out_dir);
    u_rig(&u, "");
    CHECK(until_other(&u, out0, 40), "preview: a change of the rig did not restart the session");
    CHECK(until_state(&u, "wait", 20), "preview: built-in: no wait state");
    dsg_preview_pointer(u.pv, 960.0f, 540.0f, true);
    frame(&u);
    dsg_preview_pointer(u.pv, (float)(960.0 + 5 * ppd), (float)(540.0 - 3 * ppd), true);
    for (k = 0; k < 10; k++) frame(&u);
    CHECK(in_state(&u, "wait"), "preview: the built-in rig took the profile's scale");
    /* a relative role: the pointer's motion as the profile's counts */
    CHECK(u_open(&u, rel_folder, "") == 0, "rel: open");
    CHECK(until_running(&u, 40), "rel: no session with the built-in rig");
    CHECK(dsg_preview_undriven(u.pv) == 1, "rel: driven with the built-in rig");
    /* the pointer's last place in this session is (4, 2) deg before the
     * next session's first, and it is out of the display while that
     * session starts: the hand starts at the origin */
    dsg_preview_pointer(u.pv, (float)(900.0 - 4 * ppd), (float)(500.0 - 2 * ppd), true);
    for (k = 0; k < 3; k++) frame(&u);
    dsg_preview_pointer(u.pv, 0.0f, 0.0f, false);
    u_rig(&u, "far");
    snprintf(out0, sizeof out0, "%s", info(&u).out_dir);
    CHECK(until_other(&u, out0, 40), "rel: no session with the profile");
    CHECK(dsg_preview_undriven(u.pv) == 0, "rel: not driven with the profile's signal (%d)", dsg_preview_undriven(u.pv));
    CHECK(until_state(&u, "wait", 20), "rel: no wait state");
    dsg_preview_pointer(u.pv, 900.0f, 500.0f, true);
    for (k = 0; k < 3; k++) frame(&u);
    CHECK(in_state(&u, "wait"), "rel: the first place moved the hand");
    {
        /* out of the display and in again (4, 2) deg away: no motion, or
         * the hand would be in the region */
        const float x1 = (float)(900.0 + 4 * ppd), y1 = (float)(500.0 + 2 * ppd);
        dsg_preview_pointer(u.pv, 0.0f, 0.0f, false);
        frame(&u);
        dsg_preview_pointer(u.pv, x1, y1, true);
        for (k = 0; k < 3; k++) frame(&u);
        CHECK(in_state(&u, "wait"), "rel: the way back in moved the hand");
        /* by (4, 2) deg in two moves */
        dsg_preview_pointer(u.pv, (float)(x1 + 2 * ppd), (float)(y1 + 1 * ppd), true);
        frame(&u);
        CHECK(in_state(&u, "wait"), "rel: half way ended the state");
        dsg_preview_pointer(u.pv, (float)(x1 + 4 * ppd), (float)(y1 + 2 * ppd), true);
        CHECK(until_state(&u, "done", 10), "rel: the hand at (4, 2) deg did not end the state (%s)", where(&u).state ? where(&u).state : "-");
    }
    /* a profile with no signal for it */
    u_rig(&u, "near");
    snprintf(out0, sizeof out0, "%s", info(&u).out_dir);
    CHECK(until_other(&u, out0, 40), "rel: no session with 'near'");
    {
        const char* why = "";
        const char* nm = dsg_preview_undriven_role(u.pv, 0, &why);
        CHECK(dsg_preview_undriven(u.pv) == 1 && nm && strcmp(nm, "hand") == 0 && strstr(why, "no signal"), "rel: 'near' gives '%s'", why);
    }
    /* the file changes, and a restart (V's, at dsg_rig_changed()) reloads it */
    preview_deg(&u, abs_folder, "preview file");
    write_profile("far", "", NULL, VIEW, "{\"hand\": {\"adapter\": \"raw_mouse\", \"units\": \"display_cm\", \"per_count\": [0.01, -0.02]}}");
    snprintf(out0, sizeof out0, "%s", info(&u).out_dir);
    dsg_preview_restart(u.pv);
    CHECK(until_other(&u, out0, 40), "preview file: no restart");
    CHECK(until_state(&u, "wait", 20), "preview file: no wait state");
    dsg_preview_pointer(u.pv, 960.0f, 540.0f, true);
    frame(&u);
    dsg_preview_pointer(u.pv, (float)(960.0 + 5 * ppd), (float)(540.0 - 3 * ppd), true);
    for (k = 0; k < 10; k++) frame(&u);
    CHECK(in_state(&u, "wait"), "preview file: the old view stayed");
    {
        const double ppd2 = ppd_of("far");
        dsg_preview_pointer(u.pv, (float)(960.0 + 5 * ppd2), (float)(540.0 - 3 * ppd2), true);
        CHECK(until_state(&u, "done", 10), "preview file: the new view (%g px a degree) not taken", ppd2);
    }
    write_profile("far", "", NULL, VIEW_FAR, "{\"hand\": {\"adapter\": \"raw_mouse\", \"units\": \"display_cm\", \"per_count\": [0.01, -0.02]}}");
    /* a profile that does not load: the preview's issue names it */
    CHECK(u_open(&u, oddball, "nosuch") == 0, "preview nosuch: open");
    for (k = 0; k < 40; k++) frame(&u);
    {
        dsg_issue is;
        memset(&is, 0, sizeof is);
        CHECK(dsg_preview_issue(u.pv, &is) && strstr(is.message, "'nosuch'") && strstr(is.message, "does not load"), "preview nosuch: '%s'",
              is.message ? is.message : "");
    }
    u_stop(&u);
    printf("  preview: ok\n");
}

/* --- (8) on the check thread ----------------------------------------------------------------------- */

static void part_thread(const char* abs_folder, const char* oddball) {
    U u;
    int k, i;
    if (u_start(&u, true)) {
        CHECK(0, "thread: start");
        return;
    }
    preview_deg(&u, abs_folder, "thread deg");
    /* checks with profiles, on the thread, while a session runs */
    for (i = 0; i < 6; i++) {
        const dsg_check_result* res = NULL;
        snprintf(u.rig, sizeof u.rig, "%s", i % 2 ? "far" : "near");
        post_check(&u);
        (void)dsg_worker_wait(u.w, 60000000000LL);
        if (dsg_worker_take_check(u.w, &res))
            CHECK(strcmp(res->report.rig, u.rig) == 0 && res->report.status == DSG_CHECK_OK, "thread: check %d rig '%s'", i, res->report.rig);
        frame(&u);
    }
    CHECK(u_open(&u, oddball, "near") == 0, "thread: open oddball");
    CHECK(until_running(&u, 60), "thread: oddball with 'near'");
    for (k = 0; k < 30; k++) frame(&u);
    u_stop(&u);
    printf("  thread: ok\n");
}

/* --- main ---------------------------------------------------------------------------------------------- */

static void remove_base(void) {
    char err[512];
    if (base_in_home) {
        char parent[1024];
        const char* slash = strrchr(base, '/');
        if (slash) {
            snprintf(parent, sizeof parent, "%.*s", (int)(slash - base), base);
            (void)dsg_remove_tree(parent, slash + 1, err, sizeof err);
        }
    } else {
        (void)dsg_remove_tree(work, "cfg", err, sizeof err);
    }
}

int main(int argc, char** argv) {
    char err[512], real[1024] = "", odd[DSG_PATH_MAX], afc[DSG_PATH_MAX], vis[DSG_PATH_MAX], bim[DSG_PATH_MAX], rdk[DSG_PATH_MAX];
    char absf[DSG_PATH_MAX], relf[DSG_PATH_MAX], pxf[DSG_PATH_MAX], def[4096], part[1300];
    static const char* const odd_files[] = { "experiment.json", "pack.json", "oddball.csv", "standard.png", "target.png", NULL };
    static const char* const afc_files[] = { "experiment.json", "pack.json", NULL };
    static const char* const vis_files[] = { "experiment.json", "pack.json", "targets.csv", NULL };
    static const char* const rdk_files[] = { "experiment.json", "pack.json", "conditions.csv", NULL };
    if (argc < 2) {
        fprintf(stderr, "usage: test_designer_rig WORK\n");
        return 2;
    }
    work = argv[1];
    if (dsg_make_dir(work, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    /* the user's own folder, never written: the test's must differ */
    (void)yrig_dir(real, sizeof real);
    if (dsg_join(base, sizeof base, work, "cfg") || mkdir_private(base)) {
        fprintf(stderr, "cannot make %s/cfg\n", work);
        return 1;
    }
    set_config_home(base);
    if (yrig_dir(rigdir, sizeof rigdir) != YRIG_OK) {
        /* WORK is under a folder that all users can write (/tmp), which
         * yrt_user_dir() rightly refuses: a folder of the test's own under
         * HOME, removed at the end (tests/adapt/rigfile_test.c does so) */
        const char* home = getenv("HOME");
        char cache[1024];
        (void)dsg_remove_tree(work, "cfg", err, sizeof err);
        if (!home || home[0] != '/') {
            printf("SKIP: the work folder is refused as a config folder and there is no HOME\n");
            return 0;
        }
        snprintf(cache, sizeof cache, "%s/.cache", home);
        (void)mkdir_private(cache);
        snprintf(base, sizeof base, "%s/.cache/ysp_designer_rig_test-%d", home, (int)getpid());
        if (mkdir_private(base)) {
            fprintf(stderr, "cannot make %s\n", base);
            return 1;
        }
        base_in_home = true;
        set_config_home(base);
        printf("  the work folder is under one that all users can write; the config folder is %s\n", base);
    }
    if (yrig_dir(rigdir, sizeof rigdir) != YRIG_OK || (real[0] && strcmp(real, rigdir) == 0)) {
        fprintf(stderr, "the test's rig folder '%s' is not its own (the user's: '%s')\n", rigdir, real);
        remove_base();
        return 1;
    }
    snprintf(part, sizeof part, "%s/ysp", base);
    if (mkdir_private(part) || mkdir_private(rigdir) || pl_check_nominal_cal(&nominal) != 0 || rig_files() != 0) {
        fprintf(stderr, "cannot make the rig folder %s\n", rigdir);
        remove_base();
        return 1;
    }
    printf("  rig folder %s\n", rigdir);
    if (copy_project("oddball", odd_files, odd, sizeof odd) || copy_project("afc2", afc_files, afc, sizeof afc) ||
        copy_project("visuomotor", vis_files, vis, sizeof vis) || copy_project("bimanual", afc_files, bim, sizeof bim) ||
        copy_project("rdk", rdk_files, rdk, sizeof rdk)) {
        fprintf(stderr, "cannot copy the projects\n");
        remove_base();
        return 1;
    }
    /* not "hand": the loader refuses a profile's signal for an absolute role */
    sig_def(def, sizeof def, "deg", "pen", false, 5, -3, 0.3);
    if (make_project("rig_abs", def, absf, sizeof absf)) return 1;
    /* the same in a px definition: the gfx gets no view, the pointer the
     * profile's */
    sig_def(def, sizeof def, "px", "pen", false, 5, -3, 0.3);
    if (make_project("rig_px", def, pxf, sizeof pxf)) return 1;
    sig_def(def, sizeof def, "deg", "hand", true, 4, 2, 0.3);
    if (make_project("rig_rel", def, relf, sizeof relf)) return 1;

    part_list();
    part_load();
    part_note(odd);
    part_folder(odd);
    part_check(odd, afc, vis, bim, rdk);
    part_reload(odd);
    /* the preview's profiles: far (a view far from the built-in one, a
     * signal in cm with y up) and near (a view, no signal) */
    write_profile("far", "", NULL, VIEW_FAR, "{\"hand\": {\"adapter\": \"raw_mouse\", \"units\": \"display_cm\", \"per_count\": [0.01, -0.02]}}");
    write_profile("near", "", NULL, VIEW, NULL);
    part_preview(absf, relf, pxf, odd);
    part_thread(absf, odd);

    remove_base();
    printf("%d of %d checks failed\n", fails, checks);
    return fails ? 1 : 0;
}
