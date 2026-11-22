/* fake_player.c - a stand-in for ysp_player that test_designer_run (and part
 * V's te_run.cpp) starts through dsg_run_start() (docs/designer.md, "Part R
 * as built"). It takes the player's command line and reads its PACK as a
 * script, so a test decides what the "player" writes and when. It never
 * spins: each wait is a sleep, and each one ends by itself after 30 s.
 *
 *   fake_player --version       "ysp_player V", V from the environment
 *                               variable YSP_FAKE_PLAYER_VERSION ("0.0.0"
 *                               when it is not set); exit 0. With
 *                               YSP_FAKE_PLAYER_OLD set: the usage on
 *                               stderr and exit 2, as players before 0.18.0
 *   fake_player SCRIPT --out DIR [any other argument]
 *
 * SCRIPT, one command a line; DIR/trials.csv is the data set's file:
 *   header TEXT     makes DIR, writes TEXT and a newline to trials.csv
 *   row TEXT        appends TEXT and a newline to trials.csv
 *   rows N MS TEXT  N rows, MS apart; "%d" in TEXT is the row's number
 *   raw TEXT        appends TEXT, no newline (half a row)
 *   out TEXT        TEXT on standard output; err TEXT on standard error
 *   outs N TEXT     N lines on standard output; "%d" in TEXT is the line's
 *                   number
 *   outlong N       one line of N 'x' on standard output
 *   sleep MS        sleeps
 *   wait FILE       sleeps until FILE exists (the test's go-ahead)
 *   term ignore     SIGTERM ignored from here on
 *   term exit CODE  SIGTERM: "ysp_player: stopped by the operator; N trials
 *                   written to DIR (trial N has no row)" and exit CODE
 *   raise SIG       raise(SIG): the end by a signal
 *   exit CODE       exits with CODE (also at the script's end: 0)
 * In TEXT, "\n" is a line break and "\\" a backslash.
 * Windows has no SIGTERM from another process, and this program opens no
 * window, so Stop reaches it no more than it reaches `ysp_player --sim`. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1   /* nanosleep() under -std=c11 */
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

static volatile sig_atomic_t got_term;
static int term_code = -1;   /* -1: SIGTERM at its default action */
static int rows;
static const char* out_dir = "";

static void sleep_ms(long ms) {
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&t, NULL);
#endif
}

static int file_exists(const char* p) {
#if defined(_WIN32)
    return _access(p, 0) == 0;
#else
    return access(p, F_OK) == 0;
#endif
}

#if !defined(_WIN32)
static void on_term(int s) {
    (void)s;
    got_term = 1;
}
#endif

static void check_term(void) {
    if (!got_term) return;
    printf("ysp_player: stopped by the operator; %d trials written to %s (trial %d has no row)\n", rows, out_dir, rows);
    fflush(stdout);
    exit(term_code);
}

/* Sleeps in steps of 10 ms, so a SIGTERM ends it soon. */
static void nap(long ms) {
    for (; ms > 0; ms -= 10) {
        sleep_ms(ms < 10 ? ms : 10);
        check_term();
    }
}

/* Backslash n to a line break, two backslashes to one, in place. */
static void unescape(char* s) {
    char* o = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] == 'n') {
            *o++ = '\n';
            s++;
        } else if (s[0] == '\\' && s[1] == '\\') {
            *o++ = '\\';
            s++;
        } else *o++ = *s;
    }
    *o = 0;
}

/* "%d" in text replaced by k, into out. */
static void number(char* out, size_t cap, const char* text, int k) {
    const char* at = strstr(text, "%d");
    if (at) snprintf(out, cap, "%.*s%d%s", (int)(at - text), text, k, at + 2);
    else snprintf(out, cap, "%s", text);
}

static void append(const char* text, int newline, int make) {
    char p[4200];
    FILE* f;
    snprintf(p, sizeof p, "%s/trials.csv", out_dir);
    if (make) {
#if defined(_WIN32)
        _mkdir(out_dir);
#else
        mkdir(out_dir, 0777);
#endif
    }
    f = fopen(p, make ? "wb" : "ab");
    if (!f) {
        fprintf(stderr, "ysp_player: cannot write %s\n", p);
        exit(1);
    }
    fputs(text, f);
    if (newline) fputc('\n', f);
    fclose(f);
}

int main(int argc, char** argv) {
    char line[4096];
    const char* script = NULL;
    FILE* f;
    int i;
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--version")) {
            const char* v = getenv("YSP_FAKE_PLAYER_VERSION");
            if (getenv("YSP_FAKE_PLAYER_OLD")) {
                fprintf(stderr, "usage: ysp_player PACK --out DIR [--sim]\n");
                return 2;
            }
            printf("ysp_player %s\n", v ? v : "0.0.0");
            return 0;
        }
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
        else if (argv[i][0] != '-' && !script) script = argv[i];
    }
    if (!script || !out_dir[0]) {
        fprintf(stderr, "ysp_player: fake_player SCRIPT --out DIR\n");
        return 2;
    }
    f = fopen(script, "rb");
    if (!f) {
        fprintf(stderr, "ysp_player: cannot read the script %s\n", script);
        return 2;
    }
    while (fgets(line, sizeof line, f)) {
        char* arg;
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        arg = strchr(line, ' ');
        if (arg) *arg++ = 0;
        else arg = line + n;
        check_term();
        unescape(arg);
        if (!strcmp(line, "header")) append(arg, 1, 1);
        else if (!strcmp(line, "row")) {
            append(arg, 1, 0);
            rows++;
        } else if (!strcmp(line, "rows")) {
            int k, cnt = 0;
            long ms = 0;
            char text[2048], row[2100];
            if (sscanf(arg, "%d %ld %2047[^\n]", &cnt, &ms, text) != 3) return 2;
            for (k = 0; k < cnt; k++) {
                nap(ms);
                number(row, sizeof row, text, rows);
                append(row, 1, 0);
                rows++;
            }
        } else if (!strcmp(line, "raw")) append(arg, 0, 0);
        else if (!strcmp(line, "out")) {
            printf("%s\n", arg);
            fflush(stdout);
        } else if (!strcmp(line, "outs")) {
            int k, cnt = 0;
            char text[2048], one[2100];
            if (sscanf(arg, "%d %2047[^\n]", &cnt, text) != 2) return 2;
            for (k = 0; k < cnt; k++) {
                number(one, sizeof one, text, k);
                printf("%s\n", one);
            }
            fflush(stdout);
        } else if (!strcmp(line, "outlong")) {
            int k, cnt = atoi(arg);
            for (k = 0; k < cnt; k++) putchar('x');
            putchar('\n');
            fflush(stdout);
        } else if (!strcmp(line, "err")) fprintf(stderr, "%s\n", arg);
        else if (!strcmp(line, "sleep")) nap(atol(arg));
        else if (!strcmp(line, "wait")) {
            int k;
            for (k = 0; k < 3000 && !file_exists(arg); k++) nap(10);
        } else if (!strcmp(line, "term")) {
#if !defined(_WIN32)
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sigemptyset(&sa.sa_mask);
            if (!strncmp(arg, "exit ", 5)) {
                term_code = atoi(arg + 5);
                sa.sa_handler = on_term;
            } else sa.sa_handler = SIG_IGN;
            sigaction(SIGTERM, &sa, NULL);
#endif
        } else if (!strcmp(line, "raise")) {
            fflush(stdout);
            raise(atoi(arg));
        } else if (!strcmp(line, "exit")) {
            fflush(stdout);
            return atoi(arg);
        } else if (line[0] && line[0] != '#') {
            fprintf(stderr, "ysp_player: fake_player: unknown command '%s'\n", line);
            return 2;
        }
    }
    fclose(f);
    return 0;
}
