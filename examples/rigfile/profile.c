/* profile.c - list, check, create and change rig profiles (ysp/rigfile.h,
 * docs/device.md "The rig profile").
 *
 *   rigfile_profile [--rig NAME | --file PATH] [--dir DIR] [--canonical]
 *       Loads the profile (the user's profile.json, NAME.json, or PATH),
 *       checks every loopback and calibration file it names by SHA-256,
 *       and prints its roles, what the checks found, and its hash (the one
 *       a data file header copies). --canonical prints its canonical bytes
 *       instead.
 *   rigfile_profile --list [--dir DIR]
 *       The profiles (*.json) in the rig folder.
 *   rigfile_profile --new RIGNAME [--rig NAME | --file PATH] [--dir DIR]
 *       Writes an empty profile for the rig RIGNAME where there is none.
 *   rigfile_profile --bind ROLE FAMILY KEY [--rig NAME | --file PATH] [--dir DIR]
 *       Binds ROLE to the device at match key KEY (ysp/device.h, MATCH
 *       KEYS) of a ysp/box.h FAMILY, drops the role's old loopback
 *       numbers, and writes the profile. device_out_latency --list gives
 *       the keys of the serial ports. FAMILY lsl binds ROLE to a Lab
 *       Streaming Layer stream (ysp/net.h); net_stream_view --list gives
 *       the keys of the streams.
 *   --dir DIR is a rig folder other than the user's (yrig_dir()).
 *
 * Exit code 0 when the profile is valid and every file it names checks; 3
 * when it is valid but a file is missing or changed (those roles run with
 * tier UNKNOWN); 1 when it is refused or cannot be read or written; 2 for
 * a usage error.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_RIGFILE_IMPLEMENTATION
#include "ysp/rigfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <dirent.h>
#endif

static yrig_profile g_p;

static void usage(void) {
    fprintf(stderr, "usage: rigfile_profile [--rig NAME | --file PATH] [--dir DIR] [--canonical]\n"
                    "       rigfile_profile --list [--dir DIR]\n"
                    "       rigfile_profile --new RIGNAME [--rig NAME | --file PATH] [--dir DIR]\n"
                    "       rigfile_profile --bind ROLE FAMILY KEY [--rig NAME | --file PATH] [--dir DIR]\n");
}

static int list(const char* dir) {
    int n = 0;
    printf("rig folder: %s\n", dir);
#if defined(_WIN32)
    {
        wchar_t w[700];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        char pat[700];
        snprintf(pat, sizeof pat, "%s\\*.json", dir);
        if (!MultiByteToWideChar(CP_UTF8, 0, pat, -1, w, 700)) return 1;
        h = FindFirstFileW(w, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char name[600];
                if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL) > 0) {
                    printf("  %s\n", name);
                    n++;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
#else
    {
        DIR* d = opendir(dir);
        struct dirent* e;
        if (d) {
            while ((e = readdir(d)) != NULL) {
                size_t k = strlen(e->d_name);
                if (k > 5 && strcmp(e->d_name + k - 5, ".json") == 0) {
                    printf("  %s\n", e->d_name);
                    n++;
                }
            }
            closedir(d);
        }
    }
#endif
    if (!n) printf("  (no profiles)\n");
    return 0;
}

static void secs(char* out, size_t cap, int64_t ns) {
    snprintf(out, cap, "%s%lld.%09lld", ns < 0 ? "-" : "", (long long)((ns < 0 ? -ns : ns) / 1000000000),
             (long long)((ns < 0 ? -ns : ns) % 1000000000));
}

static int show(const yrig_profile* p, const char* path) {
    char hash[65], a[32], b[32];
    int i, k, bad = 0;
    printf("profile: %s\nrig: %s\nwritten: %s\nroles: %d\n", path, p->rig, p->written, p->n_roles);
    for (i = 0; i < p->n_roles; i++) {
        const yrig_role* r = &p->role[i];
        printf("  %-12s %-10s %s", r->name, r->family, r->key);
        if (r->baud) printf("  baud %u", (unsigned)r->baud);
        if (r->latched) printf("  latched");
        if (r->ftdi_latency_ns >= 0) printf("  ftdi %lld ms", (long long)(r->ftdi_latency_ns / 1000000));
        if (r->pulse_ns >= 0) { secs(a, sizeof a, r->pulse_ns); printf("  pulse %s s", a); }
        printf("\n");
        for (k = 0; k < r->n_buttons; k++) printf("               button %s = %s\n", r->buttons[k].code, r->buttons[k].name);
        if (r->latency.set) {
            secs(a, sizeof a, r->latency.median_ns);
            secs(b, sizeof b, r->latency.p95_ns);
            printf("               latency median %s s, p95 %s s, n %d, %s, file %.12s... %s\n", a, b, (int)r->latency.n, r->latency.date,
                   r->latency.sha256, yrig_check_name(r->latency.check));
            if (r->latency.check != YRIG_CHECK_OK) bad = 1;
        }
        if (r->bounds.set) {
            secs(a, sizeof a, r->bounds.lo_ns);
            secs(b, sizeof b, r->bounds.hi_ns);
            printf("               bounds %s to %s s, n %d, %s, file %.12s... %s\n", a, b, (int)r->bounds.n, r->bounds.date, r->bounds.sha256,
                   yrig_check_name(r->bounds.check));
            if (r->bounds.check != YRIG_CHECK_OK) bad = 1;
        }
    }
    if (p->display.set) {
        secs(a, sizeof a, p->display.onset_offset_ns);
        printf("display: onset offset %s s", a);
        if (p->display.calibration[0]) {
            printf(", calibration %s %s", p->display.calibration, yrig_check_name(p->display.check));
            if (p->display.check != YRIG_CHECK_OK) bad = 1;
        }
        printf("\n");
        if (p->display.latency.set) {
            const yrig_loopback* l = &p->display.latency;
            secs(a, sizeof a, l->p5_ns);
            secs(b, sizeof b, l->p95_ns);
            printf("               photodiode run p5 %s s, p95 %s s, n %d, %s, file %.12s... %s\n", a, b, (int)l->n, l->date, l->sha256,
                   yrig_check_name(l->check));
            if (l->check != YRIG_CHECK_OK) bad = 1;
        }
    }
    for (i = 0; i < p->n_notes; i++) printf("note: %s\n", p->notes[i]);
    if (yrig_hash(p, hash) == YRIG_OK) printf("sha256 %s\n", hash);
    return bad ? 3 : 0;
}

int main(int argc, char** argv) {
    const char* name = NULL;
    const char* file = NULL;
    const char* dir_arg = NULL;
    const char* new_rig = NULL;
    const char* bind[3] = { NULL, NULL, NULL };
    int i, do_list = 0, canonical = 0, rc;
    char dir[600], path[700], err[512];
    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "--rig") == 0 && v) { name = v; i++; }
        else if (strcmp(a, "--file") == 0 && v) { file = v; i++; }
        else if (strcmp(a, "--dir") == 0 && v) { dir_arg = v; i++; }
        else if (strcmp(a, "--list") == 0) do_list = 1;
        else if (strcmp(a, "--canonical") == 0) canonical = 1;
        else if (strcmp(a, "--new") == 0 && v) { new_rig = v; i++; }
        else if (strcmp(a, "--bind") == 0 && i + 3 < argc) { bind[0] = argv[i + 1]; bind[1] = argv[i + 2]; bind[2] = argv[i + 3]; i += 3; }
        else { usage(); return 2; }
    }
    if ((name && file) || (new_rig && bind[0]) || (do_list && (file || new_rig || bind[0]))) { usage(); return 2; }
    if (dir_arg) snprintf(dir, sizeof dir, "%s", dir_arg);
    else if (!file && yrig_dir(dir, sizeof dir) != YRIG_OK) {
        fprintf(stderr, "rigfile_profile: no rig folder: the config folder is missing, or another user can write it\n");
        return 1;
    }
    if (do_list) return list(dir);
    if (file) snprintf(path, sizeof path, "%s", file);
    else if (yrig_path(dir, name, path, sizeof path) != YRIG_OK) {
        fprintf(stderr, "rigfile_profile: \"%s\" is not a profile name (1 to 63 of A-Z a-z 0-9 _ -)\n", name);
        return 2;
    }
    rc = yrig_load_file(&g_p, path, err, sizeof err);
    if (new_rig) {
        if (rc != YRIG_ERR_MISSING) {
            fprintf(stderr, "rigfile_profile: %s exists; it is not replaced\n", path);
            return 1;
        }
        yrig_init(&g_p, new_rig);
    } else if (rc != YRIG_OK) {
        fprintf(stderr, "rigfile_profile: %s\n", err);
        return 1;
    }
    if (bind[0]) {
        int fam = 0, k;
        for (k = 1; k <= YBOX_FAMILY_LAST; k++)
            if (strcmp(ybox_family_name(k), bind[1]) == 0) fam = k;
        if (strcmp(bind[1], "lsl") == 0) fam = YRIG_FAMILY_LSL;   /* a stream for ysp/net.h */
        if (!fam) {
            fprintf(stderr, "rigfile_profile: \"%s\" is not a ysp/box.h family or lsl:", bind[1]);
            for (k = 1; k <= YBOX_FAMILY_LAST; k++) fprintf(stderr, " %s", ybox_family_name(k));
            fprintf(stderr, " lsl\n");
            return 2;
        }
        rc = yrig_bind(&g_p, bind[0], fam, bind[2]);
        if (rc != YRIG_OK) {
            fprintf(stderr, "rigfile_profile: cannot bind \"%s\": %s\n", bind[0],
                    rc == YRIG_ERR_FULL ? "the profile has its 32 roles" : "a role is 1 to 31 of A-Z a-z 0-9 _ . -, a key 1 to 159 bytes; "
                    "an lsl key starts with lsl: and a serial key does not");
            return 2;
        }
    }
    if (new_rig || bind[0]) {
        if (yrig_save_file(&g_p, path, err, sizeof err) != YRIG_OK) {
            fprintf(stderr, "rigfile_profile: %s\n", err);
            return 1;
        }
        printf("written: %s\n", path);
        rc = yrig_load_file(&g_p, path, err, sizeof err);
        if (rc != YRIG_OK) {
            fprintf(stderr, "rigfile_profile: %s\n", err);
            return 1;
        }
    }
    if (canonical) {
        static char buf[1 << 20];
        size_t n = yrig_write(&g_p, buf, sizeof buf);
        if (!n || n >= sizeof buf) return 1;
        fwrite(buf, 1, n, stdout);
        return 0;
    }
    return show(&g_p, path);
}
