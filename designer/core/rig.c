/* rig.c - rig profiles: list, load with the calibration, the banner, the
 * preview's counts (dsg.h, step 6; docs/designer.md, "Rig profiles" and
 * "Part G as built"). OWNED BY PART G.
 *
 * The profile is ysp/rigfile.h's, read by yrig_load(), so the folder's
 * rules (WHERE IT LIVES: a folder that other users can write is refused)
 * and the checks of the loopback and calibration files are the player's
 * own. A dsg_rig is one thread's; nothing here takes a lock. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1   /* struct stat's st_mtim and S_ISREG under -std=c11 */
#endif
#include "dsg.h"

#include "ysp/color.h"
#include "ysp/rigfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

/* yrig_load()'s own words for a refused folder, so the list says what a
 * load and the player say. */
#define NO_FOLDER "no rig folder: the config folder is missing, or another user can write it (ysp/rt.h yrt_user_dir)"

/* A file's state for dsg_rig_changed(): a stat, not a read. */
typedef struct rig_stamp {
    bool    exists;
    int64_t size;
    int64_t mtime;   /* ns on POSIX, 100 ns ticks on Windows */
} rig_stamp;

struct dsg_rig {
    yrig_profile prof;                 /* 48 KB                               */
    ycol_cal     cal;                  /* 62,528 bytes                        */
    bool         have_cal;             /* read, valid and its check is OK     */
    char         cal_error[320];       /* named and it does not load: why     */
    char         name[DSG_RIG_NAME];
    char         path[DSG_PATH_MAX];
    char         cal_path[DSG_PATH_MAX];
    rig_stamp    st_prof, st_cal;
};

#if defined(_WIN32)
static wchar_t* rig_wide(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t* w;
    if (n <= 0) return NULL;
    w = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) {
        free(w);
        w = NULL;
    }
    return w;
}

static void stamp_of(const char* path, rig_stamp* st) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    wchar_t* w = rig_wide(path);
    memset(st, 0, sizeof *st);
    memset(&a, 0, sizeof a);
    if (w && GetFileAttributesExW(w, GetFileExInfoStandard, &a) && !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        st->exists = true;
        st->size = (int64_t)(((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow);
        st->mtime = (int64_t)(((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime);
    }
    free(w);
}
#else
static void stamp_of(const char* path, rig_stamp* st) {
    struct stat s;
    memset(st, 0, sizeof *st);
    if (!path[0] || stat(path, &s) != 0 || !S_ISREG(s.st_mode)) return;
    st->exists = true;
    st->size = (int64_t)s.st_size;
#if defined(__APPLE__)
    st->mtime = (int64_t)s.st_mtimespec.tv_sec * 1000000000 + (int64_t)s.st_mtimespec.tv_nsec;
#elif defined(__linux__)
    st->mtime = (int64_t)s.st_mtim.tv_sec * 1000000000 + (int64_t)s.st_mtim.tv_nsec;
#else
    st->mtime = (int64_t)s.st_mtime * 1000000000;
#endif
}
#endif

static bool same_stamp(const rig_stamp* a, const rig_stamp* b) {
    return a->exists == b->exists && a->size == b->size && a->mtime == b->mtime;
}

/* ysp/rigfile.h's profile names: 1 to 63 of A-Z a-z 0-9 _ - */
static bool name_ok(const char* s, size_t n) {
    size_t i;
    if (n == 0 || n > 63) return false;
    for (i = 0; i < n; i++) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

typedef struct rig_names {
    char (*v)[DSG_RIG_NAME];
    int n, cap;
    bool have_default;
} rig_names;

/* A folder entry: NAME.json, NAME a profile name, a regular file. Every
 * other file (profile.json.tmp of a save, notes, loopback/) is not one. */
static void take_entry(rig_names* L, const char* dir, const char* file) {
    char path[1200];
    rig_stamp st;
    const size_t n = strlen(file);
    if (n <= 5 || strcmp(file + n - 5, ".json") != 0 || !name_ok(file, n - 5)) return;
    snprintf(path, sizeof path, "%s/%s", dir, file);
    stamp_of(path, &st);
    if (!st.exists) return;
    if (n - 5 == 7 && memcmp(file, "profile", 7) == 0) {
        L->have_default = true;
        return;
    }
    if (L->n == L->cap) {
        const int nc = L->cap ? L->cap * 2 : 16;
        char (*nv)[DSG_RIG_NAME] = (char (*)[DSG_RIG_NAME])realloc(L->v, (size_t)nc * sizeof *nv);
        if (!nv) return;
        L->v = nv;
        L->cap = nc;
    }
    memcpy(L->v[L->n], file, n - 5);
    L->v[L->n][n - 5] = 0;
    L->n++;
}

static int by_name(const void* a, const void* b) { return strcmp((const char*)a, (const char*)b); }

int dsg_rig_list(char (*names)[DSG_RIG_NAME], int cap, char* err, size_t err_cap) {
    char dir[1024];
    rig_names L;
    int k = 0, i;
    if (err && err_cap) err[0] = 0;
    if (yrig_dir(dir, sizeof dir) != YRIG_OK) {
        if (err && err_cap) snprintf(err, err_cap, "%s", NO_FOLDER);
        return -1;
    }
    memset(&L, 0, sizeof L);
#if defined(_WIN32)
    {
        char pat[1100];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        wchar_t* w;
        snprintf(pat, sizeof pat, "%s\\*.json", dir);
        w = rig_wide(pat);
        h = w ? FindFirstFileW(w, &fd) : INVALID_HANDLE_VALUE;
        free(w);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char u[600];
                if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, u, (int)sizeof u, NULL, NULL) > 0) take_entry(&L, dir, u);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
#else
    {
        /* no folder yet: no profile, not an error (yrig_save() makes it) */
        DIR* d = opendir(dir);
        struct dirent* e;
        if (d) {
            while ((e = readdir(d)) != NULL) take_entry(&L, dir, e->d_name);
            closedir(d);
        }
    }
#endif
    if (L.n > 1) qsort(L.v, (size_t)L.n, sizeof *L.v, by_name);
    if (names && L.have_default && k < cap) snprintf(names[k++], DSG_RIG_NAME, "profile");
    for (i = 0; names && i < L.n && k < cap; i++) memcpy(names[k++], L.v[i], DSG_RIG_NAME);
    free(L.v);
    return k;
}

int dsg_rig_load(const char* name, dsg_rig** out, char* err, size_t cap) {
    char dir[1024];
    dsg_rig* r;
    if (err && cap) err[0] = 0;
    if (!out) return -1;
    *out = NULL;
    if (!name || !name[0]) name = "profile";
    r = (dsg_rig*)calloc(1, sizeof *r);
    if (!r) {
        if (err && cap) snprintf(err, cap, "out of memory for the rig profile");
        return -1;
    }
    snprintf(r->name, sizeof r->name, "%s", name);
    /* the stamp before the read: a write between the two shows as a change */
    if (yrig_dir(dir, sizeof dir) == YRIG_OK && yrig_path(dir, name, r->path, sizeof r->path) == YRIG_OK) stamp_of(r->path, &r->st_prof);
    if (yrig_load(&r->prof, name, err, cap) != YRIG_OK) {
        free(r);
        return -1;
    }
    if (r->prof.display.calibration[0]) {
        char* bytes = NULL;
        size_t n = 0;
        char why[256];
        snprintf(r->cal_path, sizeof r->cal_path, "%s/%s", r->prof.dir, r->prof.display.calibration);
        stamp_of(r->cal_path, &r->st_cal);
        /* the player reads it at a real start whatever its check says, and
         * refuses to start when it does not load (player/lower.c) */
        if (dsg_read_file(r->cal_path, &bytes, &n, why, sizeof why) != 0)
            snprintf(r->cal_error, sizeof r->cal_error, "the calibration file '%s' cannot be read", r->prof.display.calibration);
        else if (ycol_cal_load(&r->cal, bytes, n, why, sizeof why) < 0) {
            snprintf(r->cal_error, sizeof r->cal_error, "the calibration file '%s': %.200s", r->prof.display.calibration, why);
            /* its hash is right, but it is not a calibration: the file is
             * another kind, ysp/rigfile.h's DIFFERS, so pl_check() keeps
             * the calibration need (it meets it only with an OK check) */
            if (r->prof.display.check == YRIG_CHECK_OK) r->prof.display.check = YRIG_CHECK_DIFFERS;
        } else r->have_cal = r->prof.display.check == YRIG_CHECK_OK;
        free(bytes);
    }
    *out = r;
    return 0;
}

void dsg_rig_free(dsg_rig* r) { free(r); }

const char* dsg_rig_name(const dsg_rig* r) { return r ? r->name : ""; }

const char* dsg_rig_path(const dsg_rig* r) { return r ? r->path : ""; }

const struct yrig_profile* dsg_rig_profile(const dsg_rig* r) { return r ? &r->prof : NULL; }

const struct ycol_cal* dsg_rig_cal(const dsg_rig* r) { return r && r->have_cal ? &r->cal : NULL; }

bool dsg_rig_changed(const dsg_rig* r) {
    rig_stamp st;
    if (!r) return false;
    stamp_of(r->path, &st);
    if (!same_stamp(&st, &r->st_prof)) return true;
    if (!r->cal_path[0]) return false;
    stamp_of(r->cal_path, &st);
    return !same_stamp(&st, &r->st_cal);
}

int dsg_rig_summary(const dsg_rig* r, char* out, size_t cap) {
    if (!r) return snprintf(out, cap, "Rig: none");
    if (!r->prof.view.set) return snprintf(out, cap, "Rig: %s (no view)", r->name);
    return snprintf(out, cap, "Rig: %s (view %.6g mm, %d px over %.6g mm)", r->name, r->prof.view.distance_mm, (int)r->prof.view.width_px,
                    r->prof.view.width_mm);
}

bool dsg_rig_counts_per_unit(const dsg_rig* r, const char* role, int role_units, double out[2]) {
    const yrig_signal* sg = r && role ? yrig_find_signal(&r->prof, role) : NULL;
    const double f = sg ? yrig_units_factor(&r->prof, sg->units, role_units) : 0.0;
    out[0] = out[1] = 1.0;
    if (!sg || sg->adapter != YRIG_ADAPTER_RAW_MOUSE || !(f > 0.0)) return false;
    /* the adapter's role units a count are per_count x f, each lane with
     * its sign (player/v2.c v2_rig_signals()); the parser refuses 0 */
    out[0] = 1.0 / (sg->per_count[0] * f);
    out[1] = 1.0 / (sg->per_count[1] * f);
    return true;
}

/* --- check.c's, not dsg.h's ------------------------------------------------- */

/* Why the calibration file that the profile names does not load ("": it
 * loads, or none is named). A real start refuses then (player/lower.c
 * pl_cal_load()), whatever the file's check says. */
const char* dsg_rig__cal_error(const dsg_rig* r);
const char* dsg_rig__cal_error(const dsg_rig* r) { return r ? r->cal_error : ""; }

/* What a load of name would find: the folder's refusal, and the profile
 * file's size and time. The checker loads a profile that failed again
 * only when this changes, so a broken profile is not parsed at every
 * check. */
uint64_t dsg_rig__file_key(const char* name);
uint64_t dsg_rig__file_key(const char* name) {
    char dir[1024], path[1100];
    rig_stamp st;
    uint64_t k = 1469598103934665603ull;
    if (yrig_dir(dir, sizeof dir) != YRIG_OK) return 1;
    if (yrig_path(dir, name && name[0] ? name : "profile", path, sizeof path) != YRIG_OK) return 2;
    stamp_of(path, &st);
    if (!st.exists) return 3;
    k = (k ^ (uint64_t)st.size) * 1099511628211ull;
    k = (k ^ (uint64_t)st.mtime) * 1099511628211ull;
    return k < 4 ? k + 4 : k;
}
