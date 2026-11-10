/* alloc_test.c - counts heap calls made while pl_frame() runs. A trial's
 * frame must not allocate (rig_spec: no allocation in hot loops). Built with
 * -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc; run as:
 *   player_alloc_test PACK OUT [--rig FILE]          (--sim)
 *   player_alloc_test PACK --out OUT --real [--windowed] [--allow-below-tier] [--rig FILE]
 *                     [--max-trials N] [--seed N]     (a real run: needs a display)
 * A real run has other threads (SDL's, the GL driver's, the data set's
 * writer, the devices' readers) that allocate when they like: only the
 * frame thread's calls count.
 */
#include "player.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static long long n_calls;
static pthread_t g_frame_thread;
void* __real_malloc(size_t n);
void* __real_calloc(size_t a, size_t b);
void* __real_realloc(void* p, size_t n);
#include <execinfo.h>
static void note(void) { if (getenv("PL_ALLOC_TRACE")) { void* b[16]; int k = backtrace(b, 16); backtrace_symbols_fd(b, k, 2); } }
static int counts(void) { return pl_loop_active && pthread_equal(pthread_self(), g_frame_thread); }
void* __wrap_malloc(size_t n) { if (counts()) { n_calls++; note(); } return __real_malloc(n); }
void* __wrap_calloc(size_t a, size_t b) { if (counts()) { n_calls++; note(); } return __real_calloc(a, b); }
void* __wrap_realloc(void* p, size_t n) { if (counts()) n_calls++; return __real_realloc(p, n); }

static pl_player g_p;

int main(int argc, char** argv) {
    pl_args a;
    memset(&a, 0, sizeof a);
    if (argc < 3) return 2;
    int i;
    g_frame_thread = pthread_self();
    a.pack_path = argv[1];
    a.sim = true;
    a.have_seed = true;
    a.seed = 1;
    a.max_seconds = 100000;
    a.n_fields = 1;
    a.fields[0].name = "participant";
    a.fields[0].value = "1";
    for (i = 2; i < argc; i++) {
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(argv[i], "--rig") && v) { a.rig_path = v; i++; }
        else if (!strcmp(argv[i], "--out") && v) { a.out_dir = v; i++; }
        else if (!strcmp(argv[i], "--real")) a.sim = false;
        else if (!strcmp(argv[i], "--windowed")) a.windowed = true;
        else if (!strcmp(argv[i], "--allow-below-tier")) a.allow_below_tier = true;
        else if (!strcmp(argv[i], "--max-trials") && v) { a.max_trials = atoi(v); i++; }
        else if (!strcmp(argv[i], "--seed") && v) { a.seed = strtoull(v, NULL, 10); i++; }
        else if ((!strcmp(argv[i], "--field")) && v) i++;   /* the player's; participant=1 is set */
        else if (!strcmp(argv[i], "--measure")) { }
        else if (!a.out_dir && argv[i][0] != '-') a.out_dir = argv[i];
        else return 2;
    }
    if (!a.out_dir) return 2;
    /* a small RDK buffer, so the mid-trial flush runs inside pl_frame too */
    if (getenv("PL_RDK_BUFFER")) a.rdk_buffer = atoi(getenv("PL_RDK_BUFFER"));
    if (a.sim) yrt_virtual_begin(0);
    if (pl_load(&g_p, &a) < 0 || pl_run(&g_p) < 0) { fprintf(stderr, "%s\n", g_p.error); return 1; }
    pl_close(&g_p);
    printf("heap calls inside pl_frame: %lld\n", n_calls);
    return n_calls ? 1 : 0;
}
