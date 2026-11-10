/* alloc_test.c - counts heap calls made while pl_frame() runs. A trial's
 * frame must not allocate (rig_spec: no allocation in hot loops). Built with
 * -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc; run as:
 *   player_alloc_test PACK OUT [--rig FILE]
 */
#include "player.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static long long n_calls;
void* __real_malloc(size_t n);
void* __real_calloc(size_t a, size_t b);
void* __real_realloc(void* p, size_t n);
#include <execinfo.h>
static void note(void) { if (getenv("PL_ALLOC_TRACE")) { void* b[16]; int k = backtrace(b, 16); backtrace_symbols_fd(b, k, 2); } }
void* __wrap_malloc(size_t n) { if (pl_loop_active) { n_calls++; note(); } return __real_malloc(n); }
void* __wrap_calloc(size_t a, size_t b) { if (pl_loop_active) { n_calls++; note(); } return __real_calloc(a, b); }
void* __wrap_realloc(void* p, size_t n) { if (pl_loop_active) n_calls++; return __real_realloc(p, n); }

static pl_player g_p;

int main(int argc, char** argv) {
    pl_args a;
    memset(&a, 0, sizeof a);
    if (argc < 3) return 2;
    a.pack_path = argv[1];
    a.out_dir = argv[2];
    a.sim = true;
    a.have_seed = true;
    a.seed = 1;
    a.max_seconds = 100000;
    a.n_fields = 1;
    a.fields[0].name = "participant";
    a.fields[0].value = "1";
    if (argc > 4 && !strcmp(argv[3], "--rig")) a.rig_path = argv[4];
    /* a small RDK buffer, so the mid-trial flush runs inside pl_frame too */
    if (getenv("PL_RDK_BUFFER")) a.rdk_buffer = atoi(getenv("PL_RDK_BUFFER"));
    yrt_virtual_begin(0);
    if (pl_load(&g_p, &a) < 0 || pl_run(&g_p) < 0) { fprintf(stderr, "%s\n", g_p.error); return 1; }
    pl_close(&g_p);
    printf("heap calls inside pl_frame: %lld\n", n_calls);
    return n_calls ? 1 : 0;
}
