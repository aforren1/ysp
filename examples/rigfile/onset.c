/* onset.c - the display's onset offset from a photodiode run, into the rig
 * profile (docs/devices_spec.md 11.2 and 12, ysp/rigfile.h display.latency).
 *
 *   rigfile_onset RUN.csv [--path N] [--min-n N] [--file PATH]
 *       Reads the per-edge CSV of a photodiode run, as photodiode_check
 *       --csv and tests/loopback/screen_loopback.c write it: the header
 *       "frame,level,onset_ns,photodiode_ns,diff_us,path,flags", then one
 *       row per expected edge that the photodiode saw. The run must have
 *       had desc.onset_offset_ns 0 (both programs leave it 0), so onset_ns
 *       is the flip record's vblank time. Each row gives photodiode_ns -
 *       onset_ns, light at the patch minus flip onset, in exact ns. The
 *       program writes n, min, p5, median, p95, max, mean, SD, the medians
 *       of each polarity, onset_offset_s (the median) and the samples to
 *       display_onset.txt (--file), in seconds, and prints the file's
 *       SHA-256. Rows of more than one presentation path (composed,
 *       independent, ...) are refused unless --path N keeps one: the
 *       offset differs between paths.
 *   rigfile_onset RUN.csv ... --store [--rig NAME] [--rig-dir DIR]
 *       Also keeps the file in the rig profile's loopback/ folder and
 *       writes display.onset_offset_s and display.latency into the profile
 *       (the user's profile.json, or NAME.json, in the user's rig folder or
 *       in DIR). A profile that does not exist is made.
 *
 * Why the median and not the mean: an edge matched to the wrong flip is
 * off by a frame, and one such edge moves a mean of 200 by 80 us at 60 Hz;
 * it does not move the median. The polarities stay pooled: a stimulus can
 * go either way, and the file keeps each polarity's median for a display
 * whose rise and fall differ.
 *
 * Exit code 0 when the file was written (and stored); 1 when the run is
 * refused (fewer than --min-n edges, default 100; mixed paths; an offset
 * of 1 s or more); 2 for a usage error or a file that cannot be read or
 * written.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_RIGFILE_IMPLEMENTATION
#include "ysp/rigfile.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_ROWS 200000
#define HEADER "frame,level,onset_ns,photodiode_ns,diff_us,path,flags"

static const char* const g_path_name[6] = { "unknown", "composed", "overlay", "independent", "simulated", "browser" };

static int64_t g_d[MAX_ROWS], g_rise[MAX_ROWS], g_fall[MAX_ROWS];
static int     g_path[MAX_ROWS], g_level[MAX_ROWS];
static yrig_profile g_prof;

static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : (x > y);
}

/* p in [0, 1] of sorted v, linear between ranks as out_latency.c does,
 * rounded to the ns */
static int64_t pctl(const int64_t* v, int n, double p) {
    double k = p * (double)(n - 1);
    int i = (int)k;
    if (i >= n - 1) return v[n - 1];
    return v[i] + (int64_t)floor((k - (double)i) * (double)(v[i + 1] - v[i]) + 0.5);
}

/* seconds, the exact decimal of the ns */
static const char* secs(char* out, int64_t ns) {
    uint64_t a = ns < 0 ? (uint64_t)0 - (uint64_t)ns : (uint64_t)ns;
    sprintf(out, "%s%llu.%09llu", ns < 0 ? "-" : "", (unsigned long long)(a / 1000000000u), (unsigned long long)(a % 1000000000u));
    return out;
}

/* One integer field ending at ',' or the end of the row; NULL when bad. */
static const char* field_i64(const char* s, const char* end, int64_t* out) {
    char buf[32];
    char* e;
    size_t k = 0;
    while (s + k < end && s[k] != ',') k++;
    if (k == 0 || k >= sizeof buf) return NULL;
    memcpy(buf, s, k);
    buf[k] = '\0';
    *out = (int64_t)strtoll(buf, &e, 10);
    if (*e != '\0') return NULL;
    return s + k;
}

static const char* skip_field(const char* s, const char* end) {
    while (s < end && *s != ',') s++;
    return s < end ? s : NULL;
}

static char* read_all(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    char* buf;
    long len;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || len > (64L << 20) || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = (char*)malloc((size_t)len + 1);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); return NULL; }
    fclose(f);
    buf[len] = '\0';
    *n = (size_t)len;
    return buf;
}

/* The rows, or -1 with a message. */
static int parse_csv(const char* t, size_t n, const char* name) {
    const char* p = t;
    const char* end = t + n;
    int rows = 0, line = 1;
    const char* e = memchr(p, '\n', n);
    size_t hl = e ? (size_t)(e - p) : n;
    if (hl && p[hl - 1] == '\r') hl--;
    if (hl != strlen(HEADER) || memcmp(p, HEADER, hl) != 0) {
        fprintf(stderr, "rigfile_onset: %s: line 1 is not \"%s\" (photodiode_check --csv writes it)\n", name, HEADER);
        return -1;
    }
    p = e ? e + 1 : end;
    while (p < end) {
        const char* le = memchr(p, '\n', (size_t)(end - p));
        const char* re;
        int64_t frame, level, onset, light, path, flags;
        line++;
        if (!le) le = end;
        re = (le > p && le[-1] == '\r') ? le - 1 : le;
        if (re == p) { p = le + 1; continue; }            /* an empty line */
        if (rows >= MAX_ROWS) { fprintf(stderr, "rigfile_onset: %s: more than %d rows\n", name, MAX_ROWS); return -1; }
        if (!(p = field_i64(p, re, &frame)) || *p++ != ',' || !(p = field_i64(p, re, &level)) || *p++ != ',' ||
            !(p = field_i64(p, re, &onset)) || *p++ != ',' || !(p = field_i64(p, re, &light)) || *p++ != ',' ||
            !(p = skip_field(p, re)) || *p++ != ',' || !(p = field_i64(p, re, &path)) || *p++ != ',' ||
            !(p = field_i64(p, re, &flags)) || p != re || frame < 0 || (level != 0 && level != 1) || path < 0 || path > 5 ||
            onset <= 0 || light <= 0) {
            fprintf(stderr, "rigfile_onset: %s: line %d is not a row of %s\n", name, line, HEADER);
            return -1;
        }
        (void)flags;
        g_d[rows] = light - onset;
        g_level[rows] = (int)level;
        g_path[rows] = (int)path;
        rows++;
        p = le + 1;
    }
    return rows;
}

static int store(const char* text, size_t tl, const char* rig_name, const char* rig_dir) {
    char dir[600], path[700], sha[65], err[512], a[40];
    yrig_loopback lb;
    int rc;
    if (yrig_read_loopback(text, tl, &lb, err, sizeof err) != YRIG_FILE_DISPLAY_ONSET) {
        fprintf(stderr, "rigfile_onset: not stored: %s\n", err);
        return 2;
    }
    if (rig_dir) snprintf(dir, sizeof dir, "%s", rig_dir);
    else if (yrig_dir(dir, sizeof dir) != YRIG_OK) {
        fprintf(stderr, "rigfile_onset: not stored: no rig folder (the config folder is missing, or another user can write it)\n");
        return 2;
    }
    if (yrig_path(dir, rig_name, path, sizeof path) != YRIG_OK) {
        fprintf(stderr, "rigfile_onset: not stored: \"%s\" is not a profile name\n", rig_name);
        return 2;
    }
    rc = yrig_load_file(&g_prof, path, err, sizeof err);
    if (rc == YRIG_ERR_MISSING) yrig_init(&g_prof, rig_name ? rig_name : "rig");
    else if (rc != YRIG_OK) {
        fprintf(stderr, "rigfile_onset: not stored: %s\n", err);
        return 2;
    }
    if (yrig_store_loopback(dir, text, tl, sha, err, sizeof err) != YRIG_OK) {
        fprintf(stderr, "rigfile_onset: not stored: %s\n", err);
        return 2;
    }
    g_prof.display.set = true;
    g_prof.display.onset_offset_ns = lb.offset_ns;
    g_prof.display.latency = lb;
    if (yrig_save_file(&g_prof, path, err, sizeof err) != YRIG_OK) {
        fprintf(stderr, "rigfile_onset: not stored: %s\n", err);
        return 2;
    }
    printf("stored: loopback/%s.txt in %s; display.onset_offset_s %s in %s\n", sha, dir, secs(a, lb.offset_ns), path);
    return 0;
}

int main(int argc, char** argv) {
    const char* csv = NULL;
    const char* out = "display_onset.txt";
    const char* rig_name = NULL;
    const char* rig_dir = NULL;
    const char* base;
    int want_path = -1, min_n = 100, do_store = 0, i, rows, n = 0, nr = 0, nf = 0, path = -1;
    int count[6] = { 0, 0, 0, 0, 0, 0 };
    char* data;
    char* text;
    size_t dn, tl = 0, cap;
    char hex[65], date[32], a[40];
    double mean = 0.0, ss = 0.0;
    int64_t med;
    time_t now;
    FILE* f;

    for (i = 1; i < argc; i++) {
        const char* s = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(s, "--path") == 0 && v) { want_path = atoi(v); i++; }
        else if (strcmp(s, "--min-n") == 0 && v) { min_n = atoi(v); i++; }
        else if (strcmp(s, "--file") == 0 && v) { out = v; i++; }
        else if (strcmp(s, "--store") == 0) do_store = 1;
        else if (strcmp(s, "--rig") == 0 && v) { rig_name = v; i++; }
        else if (strcmp(s, "--rig-dir") == 0 && v) { rig_dir = v; i++; }
        else if (s[0] != '-' && !csv) csv = s;
        else { csv = NULL; break; }
    }
    if (!csv || want_path < -1 || want_path > 5 || min_n < 1) {
        fprintf(stderr, "usage: rigfile_onset RUN.csv [--path N] [--min-n N] [--file PATH] [--store [--rig NAME] [--rig-dir DIR]]\n");
        return 2;
    }
    if (!(data = read_all(csv, &dn))) { fprintf(stderr, "rigfile_onset: cannot read %s\n", csv); return 2; }
    rows = parse_csv(data, dn, csv);
    if (rows < 0) { free(data); return 2; }

    for (i = 0; i < rows; i++) count[g_path[i]]++;
    for (i = 0; i < 6; i++)
        if (count[i] && want_path < 0) {
            if (path >= 0) {
                int k;
                fprintf(stderr, "rigfile_onset: %s has rows of more than one presentation path:", csv);
                for (k = 0; k < 6; k++) if (count[k]) fprintf(stderr, " %d %s (%d rows)", k, g_path_name[k], count[k]);
                fprintf(stderr, "; keep one with --path N\n");
                free(data);
                return 1;
            }
            path = i;
        }
    if (want_path >= 0) path = want_path;
    for (i = 0; i < rows; i++) {
        if (g_path[i] != path) continue;
        g_d[n] = g_d[i];
        if (g_level[i]) g_rise[nr++] = g_d[i]; else g_fall[nf++] = g_d[i];
        n++;
    }
    if (n < min_n) {
        fprintf(stderr, "rigfile_onset: %d edges of path %d; at least %d are needed (--min-n)\n", n, path < 0 ? 0 : path, min_n);
        free(data);
        return 1;
    }
    cap = (size_t)n * 24 + 4096;
    if (!(text = (char*)malloc(cap))) { free(data); return 2; }
    for (i = 0; i < n; i++) mean += (double)g_d[i];
    mean /= n;
    for (i = 0; i < n; i++) ss += ((double)g_d[i] - mean) * ((double)g_d[i] - mean);
    /* the samples in the CSV's order, before the sort */
#define PUT(...) do { int w_ = snprintf(text + tl, cap - tl, __VA_ARGS__); if (w_ > 0) tl += (size_t)w_; } while (0)
    {
        static char samples[MAX_ROWS * 24];
        size_t sl = 0;
        for (i = 0; i < n; i++) {
            int w = snprintf(samples + sl, sizeof samples - sl, " %s", secs(a, g_d[i]));
            if (w > 0) sl += (size_t)w;
        }
        qsort(g_d, (size_t)n, sizeof g_d[0], cmp_i64);
        qsort(g_rise, (size_t)nr, sizeof g_rise[0], cmp_i64);
        qsort(g_fall, (size_t)nf, sizeof g_fall[0], cmp_i64);
        med = pctl(g_d, n, 0.5);
        if (med <= -1000000000 || med >= 1000000000) {
            fprintf(stderr, "rigfile_onset: the median is %s s; an onset offset is under 1 s\n", secs(a, med));
            free(text);
            free(data);
            return 1;
        }
        now = time(NULL);
        strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
        {
            uint8_t d[32];
            yrig_sha256(data, dn, d);
            for (i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
        }
        base = csv + strlen(csv);
        while (base > csv && base[-1] != '/' && base[-1] != '\\') base--;
        PUT("# ysp display onset: photodiode edge minus flip onset (docs/devices_spec.md 12)\n");
        PUT("format ysp-display-onset 1\n");
        PUT("date %s\n", date);
        PUT("ysp_rig %s\n", yrig_version());
        PUT("source %s\n", base);
        PUT("source_sha256 %s\n", hex);
        PUT("rows %d\n", rows);
        PUT("path %d %s\n", path, g_path_name[path]);
        PUT("n %d\n", n);
        PUT("min_s %s\n", secs(a, g_d[0]));
        PUT("p5_s %s\n", secs(a, pctl(g_d, n, 0.05)));
        PUT("median_s %s\n", secs(a, med));
        PUT("p95_s %s\n", secs(a, pctl(g_d, n, 0.95)));
        PUT("max_s %s\n", secs(a, g_d[n - 1]));
        PUT("mean_s %s\n", secs(a, (int64_t)floor(mean + 0.5)));
        PUT("sd_s %s\n", secs(a, (int64_t)floor(sqrt(ss / (n > 1 ? n - 1 : 1)) + 0.5)));
        PUT("rising_n %d\n", nr);
        if (nr) PUT("rising_median_s %s\n", secs(a, pctl(g_rise, nr, 0.5)));
        PUT("falling_n %d\n", nf);
        if (nf) PUT("falling_median_s %s\n", secs(a, pctl(g_fall, nf, 0.5)));
        PUT("onset_offset_s %s\n", secs(a, med));
        PUT("samples_s%.*s\n", (int)sl, samples);
    }
#undef PUT
    free(data);
    if (!(f = fopen(out, "wb")) || fwrite(text, 1, tl, f) != tl) {
        fprintf(stderr, "rigfile_onset: cannot write %s\n", out);
        if (f) fclose(f);
        free(text);
        return 2;
    }
    fclose(f);
    {
        uint8_t d[32];
        const char* s = strstr(text, "samples_s");
        yrig_sha256(text, tl, d);
        for (i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
        fwrite(text, 1, s ? (size_t)(s - text) : tl, stdout);
    }
    printf("sha256 %s  %s\n", hex, out);
    if (do_store) {
        int rc = store(text, tl, rig_name, rig_dir);
        free(text);
        return rc;
    }
    free(text);
    return 0;
}
