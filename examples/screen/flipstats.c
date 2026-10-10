/* screen_flipstats.c - measure ysp/screen.h's swap path on this machine.
 *
 *     screen_flipstats [--sim | --backend dxgi|composition] [--windowed] [--topmost]
 *                      [--cover N] [--frames N] [--load N]
 *                      [--overrun MS] [--miss N] [--gpu N] [--hold] [--patch]
 *                      [--depth N | --depth-learn] [--long]
 *                      [--codes] [--row clear|update] [--verify N]
 *                      [--trigger] [--fence] [--d3d11-video]
 *                      [--group] [--allocs | --allocs-control] [--csv FILE]
 *
 *   --sim        the simulated display: no window, no GPU (CI runs this)
 *   --backend B  dxgi (DXGI_FLIP) or composition (COMPOSITION); default AUTO
 *   --topmost    keep the window on top and take the foreground (Windows): a
 *                covered window gets no flip statistics on COMPOSITION
 *   --cover N    from frame N on, put a small window over a corner of ours
 *                (Windows), which forces the composed path; takes the
 *                foreground but does not keep the window on top
 *   --windowed   an 800 x 600 window instead of borderless fullscreen
 *   --frames N   frames to run (default 600). Fullscreen runs stop at 7200
 *                (2 minutes at 60 Hz), because the display is somebody's
 *   --load N     N threads spin at normal priority beside the frame loop
 *   --overrun MS in a random 1 of 30 frames, busy-wait MS ms before the flip
 *   --miss N     every N frames, 3 frames in a row with about 25 ms of GPU
 *                work, which miss their vblank with the present on time:
 *                the swap path's own lateness
 *   --gpu N      N full-screen clears in every frame: steady GPU work
 *   --depth N    pin the depth to N vblanks (desc.depth)
 *   --depth-learn  let misses raise the depth (desc.depth_learn)
 *   --long       fullscreen runs longer than 7200 frames (up to 40000)
 *   --hold       ask for a vblank 0 to 4 frames ahead, between grid points
 *   --patch      the photodiode patch, alternating between two dark grays
 *                (bottom left with --codes)
 *   --codes      a VPixx Pixel Mode code (the frame number) and an 8-pixel
 *                pixel-sync row (Psychtoolbox's pattern) on every frame
 *   --row M      draw ROW codes by ClearView per pixel (clear) or one
 *                UpdateSubresource (update); default: the header's choice
 *   --verify N   read the codes back every N flips
 *   --trigger    one at-onset trigger per frame (a no-op callback): the
 *                worker's lateness, moves and mismatches
 *   --fence      with --trigger: move a trigger when the GPU is late
 *   --trigger-offset US  the trigger channel's offset from the onset
 *   --trigger-cpu N      pin the trigger worker to logical CPU N
 *   --trigger-rt-cores   leave the trigger worker where ysp/rt.h puts it
 *                        (the P-cores on a hybrid CPU), not on the E-cores
 *   --trigger-spin US    the trigger worker's spin window
 *   --d3d11-video  the device with video support and multithread
 *                protection (desc.d3d11_video), for its cost per frame
 *   --group      two windows flipped as a group (implies --windowed)
 *   --allocs     count C runtime heap calls in the frame loop (MSVC debug)
 *   --allocs-control  the same, with one malloc per frame the count must see
 *   --csv FILE   every flip record as CSV
 *
 * Esc or closing the window ends the run at once. The screen stays dark gray;
 * only the small patch changes, and only between two dark grays.
 *
 * Prints the describe line, each path and depth change, then one table:
 * prediction error of on-time
 * frames, late targets, drops, early flips, estimated records, paths, the
 * phases and the header's own cost per frame. The header cost comes from
 * ysp/rt.h's trace ring (this file defines YRT_TRACE_RING): the begin zone
 * minus its wait, the flip zone minus the present call and any hold.
 *
 * Exit: 0 when the run completed, 1 when the screen did not open, 2 usage.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
/* fopen() is C4996 under /W4 /WX, and fopen_s() is not portable. */
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YRT_TRACE_RING
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <pthread.h>
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
    #include <crtdbg.h>
#endif

#define MAX_FRAMES 40000

#if defined(_WIN32)
    #define GLAPI_CALL __stdcall
#else
    #define GLAPI_CALL
#endif
typedef struct gl_api {
    void (GLAPI_CALL *ClearColor)(float, float, float, float);
    void (GLAPI_CALL *Clear)(unsigned int);
    void (GLAPI_CALL *Viewport)(int, int, int, int);
} gl_api;

static void load_gl(const yscr_screen* s, gl_api* gl) {
    gl->ClearColor = (void (GLAPI_CALL*)(float, float, float, float))yscr_gl_proc(s, "glClearColor");
    gl->Clear = (void (GLAPI_CALL*)(unsigned int))yscr_gl_proc(s, "glClear");
    gl->Viewport = (void (GLAPI_CALL*)(int, int, int, int))yscr_gl_proc(s, "glViewport");
}

/* --- spinning load threads ---------------------------------------------- */

static volatile int g_stop_load;
#if defined(_WIN32)
static DWORD WINAPI load_main(LPVOID a) { (void)a; while (!g_stop_load) { } return 0; }
#else
static void* load_main(void* a) { (void)a; while (!g_stop_load) { } return NULL; }
#endif

static void start_load(int n) {
    int i;
    for (i = 0; i < n; i++) {
#if defined(_WIN32)
        HANDLE h = CreateThread(NULL, 0, load_main, NULL, 0, NULL);
        if (h) CloseHandle(h);
#else
        pthread_t t;
        if (pthread_create(&t, NULL, load_main, NULL) == 0) pthread_detach(t);
#endif
    }
}

/* --- allocation counter (MSVC debug CRT only) ------------------------------ */

static int g_count_zones;
static volatile long g_allocs;
static volatile int g_count_allocs;
#if defined(_MSC_VER) && defined(_DEBUG)
static int alloc_hook(int type, void* data, size_t size, int block, long req, const unsigned char* file, int line) {
    (void)data; (void)size; (void)block; (void)req; (void)file; (void)line;
    if (g_count_allocs && (type == _HOOK_ALLOC || type == _HOOK_REALLOC)) g_allocs++;
    return 1;
}
#endif

/* --- statistics ------------------------------------------------------------ */

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y);
}

static void row(const char* name, double* v, int n) {
    double sum = 0;
    int i;
    if (n <= 0) { printf("  %-34s n=0\n", name); return; }
    qsort(v, (size_t)n, sizeof *v, cmp_d);
    for (i = 0; i < n; i++) sum += v[i];
    printf("  %-34s n=%6d mean %9.2f p50 %9.2f p99 %9.2f max %9.2f min %9.2f\n", name, n, sum / n,
           v[n / 2], v[(int)((double)n * 0.99)], v[n - 1], v[0]);
}

static yrt_event g_ev[4096];
static unsigned char g_ring_mem[YRT_RING_BYTES(8192)];
static yrt_ring g_ring;

static yscr_record g_rec[MAX_FRAMES];
static int64_t g_pred[MAX_FRAMES];
static int64_t g_vb_prev;
static int g_same_vb, g_done_n;   /* frames planned at or before the last one's vblank; records via f.done */
static int64_t g_expect[MAX_FRAMES];   /* --hold: the vblank time asked for */
static unsigned char g_overran[MAX_FRAMES];
static unsigned char g_heavy[MAX_FRAMES];   /* --miss: GPU-heavy frame */
static int64_t g_tret[MAX_FRAMES];          /* when flip_at() returned       */
static int g_nrec;
static double g_cost_begin[MAX_FRAMES], g_cost_flip[MAX_FRAMES];
static int g_ncb, g_ncf;
static double g_last_wait, g_last_present, g_last_hold;
static FILE* g_csv;
/* --trigger: what the callback saw, and what the after-flip hook reported */
static double g_trig_late_us[MAX_FRAMES], g_trig_wake_us[MAX_FRAMES], g_trig_lock_us[MAX_FRAMES],
              g_trig_disp_us[MAX_FRAMES];
#if defined(_WIN32)
static unsigned char g_trig_cpu[MAX_FRAMES];   /* where the callback ran: only Windows names the CPU */
#endif
static int g_ntl;
static int g_tr_n, g_tr_moved, g_tr_gpu_moved, g_tr_early, g_tr_late, g_tr_pending, g_tr_gpu_caught;
static void trig_fn(void* ctx, const yscr_trigger_info* i) {
    (void)ctx;
    if (g_ntl < MAX_FRAMES && !(i->flags & YSCR_TRIG_FLUSHED)) {
        /* fired - deadline = wake (the worker's own lateness) + lock wait +
         * dispatch (the rest of the header's path) */
        g_trig_late_us[g_ntl] = (double)(i->fired_ns - i->deadline_ns) / 1000.0;
        g_trig_wake_us[g_ntl] = (double)(i->woke_ns - i->deadline_ns) / 1000.0;
        g_trig_lock_us[g_ntl] = (double)i->lock_ns / 1000.0;
        g_trig_disp_us[g_ntl] = (double)(i->fired_ns - i->woke_ns - i->lock_ns) / 1000.0;
#if defined(_WIN32)
        g_trig_cpu[g_ntl] = (unsigned char)GetCurrentProcessorNumber();
#endif
        g_ntl++;
    }
}
static void flip_fn(void* ctx, const yscr_record* r, const yscr_trigger_result* t, int n) {
    int k;
    (void)ctx;
    for (k = 0; k < n; k++) {
        g_tr_n++;
        if (t[k].flags & YSCR_TRIG_MOVED) g_tr_moved++;
        if (t[k].flags & YSCR_TRIG_GPU_MOVED) {
            g_tr_gpu_moved++;
            if (t[k].mismatch == 0 && r->dropped > 0) g_tr_gpu_caught++;   /* moved, and the frame was late */
        }
        if (t[k].flags & YSCR_TRIG_FIRED_EARLY) g_tr_early++;
        if (t[k].flags & YSCR_TRIG_FIRED_LATE) g_tr_late++;   /* a move that was not needed */
        if (t[k].flags & YSCR_TRIG_PENDING) g_tr_pending++;
    }
}
static int g_timeouts;   /* begin() timeouts survived: the swap path stalled */
static int g_depth_changes;   /* YSCR_EV_DEPTH records after open's */

/* Every zone's durations by name, for the cost breakdown. */
#define MAX_ZONES 24
static const char* g_zname[MAX_ZONES];
static double* g_zdur[MAX_ZONES];
static int g_zn[MAX_ZONES];
static int g_nz;

static void zone_add(const char* nm, double us) {
    int k;
    for (k = 0; k < g_nz; k++) if (!strcmp(g_zname[k], nm)) break;
    if (k == g_nz) {
        if (g_nz == MAX_ZONES) return;
        g_zname[k] = nm;
        g_zdur[k] = (double*)malloc(sizeof(double) * MAX_FRAMES);
        g_zn[k] = 0;
        g_nz++;
    }
    if (g_zdur[k] && g_zn[k] < MAX_FRAMES) g_zdur[k][g_zn[k]++] = us;
}

static void drain(void) {
    int n, i;
    while ((n = yrt_ring_drain(&g_ring, g_ev, 4096)) > 0) {
        for (i = 0; i < n; i++) {
            const yrt_event* e = &g_ev[i];
            if (e->source == YRT_SRC_SCREEN && e->kind == YSCR_EV_FLIP) {
                yscr_record r;
                int k;
                memset(&r, 0, sizeof r);
                r.onset = (int64_t)e->t_ns;
                r.target = e->u.i64[0];
                r.dropped = e->u.u16[4];
                r.path = (uint8_t)YSCR_EV_PATH_OF(e->u.u16[5]);
                r.flags = (uint16_t)YSCR_EV_FLAGS_OF(e->u.u16[5]);
                r.tier = (uint8_t)YSCR_EV_TIER_OF(e->u.u16[5]);
                for (k = 0; k < YSCR_N_PHASES; k++) r.phase_ns[k] = e->u.u32[3 + k];
                r.index = e->u.u32[9];
                r.residual = r.onset - r.target;
                if (g_nrec < MAX_FRAMES) g_rec[g_nrec++] = r;
                if (g_csv)
                    fprintf(g_csv, "%lld,%u,%lld,%lld,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", (long long)r.index, e->aux,
                            (long long)r.target, (long long)r.onset, r.dropped, r.path, r.flags, r.tier,
                            r.phase_ns[0], r.phase_ns[1], r.phase_ns[2], r.phase_ns[3], r.phase_ns[4], r.phase_ns[5]);
            } else if (e->source == YRT_SRC_RT && e->kind == YRT_KIND_ZONE && e->u.zone.loc) {
                const char* nm = e->u.zone.loc->name;
                double us = (double)e->u.zone.dur_ns / 1000.0;
                if (g_count_zones) zone_add(nm, us);
                if (!strcmp(nm, "yscr.wait")) g_last_wait = us;
                else if (!strcmp(nm, "yscr.present")) g_last_present = us;
                else if (!strcmp(nm, "yscr.hold")) g_last_hold = us;
                else if (!strcmp(nm, "yscr.begin")) {
                    if (g_ncb < MAX_FRAMES) g_cost_begin[g_ncb++] = us - g_last_wait;
                    g_last_wait = 0;
                } else if (!strcmp(nm, "yscr.flip")) {
                    if (g_ncf < MAX_FRAMES) g_cost_flip[g_ncf++] = us - g_last_present - g_last_hold;
                    g_last_present = 0;
                    g_last_hold = 0;
                }
            } else if (e->source == YRT_SRC_RT && e->kind == YRT_KIND_LOSS) {
                fprintf(stderr, "ring lost %llu records\n", (unsigned long long)e->u.u64[0]);
            } else if (e->source == YRT_SRC_SCREEN && e->kind == YSCR_EV_PATH) {
                printf("  path change at %.3f s: %u -> %u\n", (double)e->t_ns * 1e-9, e->u.u16[0], e->u.u16[1]);
            } else if (e->source == YRT_SRC_SCREEN && e->kind == YSCR_EV_DEPTH) {
                static const char* const why[6] = { "?", "open", "path", "pin", "learner", "early" };
                printf("  depth at %.3f s: %u -> %u (%s, path %u, frame %lld)\n", (double)e->t_ns * 1e-9, e->u.u16[0],
                       e->u.u16[1], why[e->u.u16[2] < 6 ? e->u.u16[2] : 0], e->u.u16[3], (long long)e->u.i64[1]);
                if (e->u.u16[0]) g_depth_changes++;
            }
        }
    }
}

static uint32_t rng_state = 12345u;
static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

static void busy_ms(double ms) {
    int64_t end = yrt_now_ns() + (int64_t)(ms * 1e6);
    while (yrt_now_ns() < end) { }
}

#if defined(_WIN32)
/* Windows gives the foreground to a program started from the background
 * after it sends one input event; a zero-size mouse move is that event. */
static void take_foreground(SDL_Window* w, int topmost) {
    HWND h = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof in);
    if (topmost) SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(h);
}

static LRESULT CALLBACK cover_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcW(h, m, wp, lp); }
static HWND cover_open(SDL_Window* w) {
    HWND h = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    RECT r;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = cover_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"yscr_cover";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNSHADOW + 1);   /* a system color: no gdi32 to link */
    RegisterClassW(&wc);
    GetWindowRect(h, &r);
    return CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"yscr_cover", L"", WS_POPUP | WS_VISIBLE,
                           r.left + 100, r.top + 100, 200, 120, NULL, NULL, wc.hInstance, NULL);
}
#endif

static int quit_requested(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return 1;
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    int sim = 0, windowed = 0, frames = 600, load = 0, hold = 0, patch = 0;
    int group = 0, allocs = 0, i, n_screens = 1, quit = 0, ran = 0, topmost = 0, cover = 0;
    yscr_backend backend = YSCR_BACKEND_AUTO;
#if defined(_WIN32)
    HWND cover_window = NULL;
#endif
    double overrun = 0;
    int miss_every = 0, gpu_clears = 0, depth = 0, depth_learn = 0, long_run = 0;
    int codes = 0, row_method = -1, verify = 0, trigger = 0, fence = 0, video = 0;
    double trig_offset_us = 0;
    int trig_cpu = 0;
    double trig_spin_us = 0;
    uint16_t code_risk = 0;
    yscr_trigger_desc tdesc;
    const char* csv = NULL;
    static yscr_screen scr[2];
    yscr_screen* sp[2] = { &scr[0], &scr[1] };
    yscr_desc d;
    gl_api gl[2];
    char line[512];
    yrt_policy pol;
    yrt_ring_desc rd;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--windowed")) windowed = 1;
        else if (!strcmp(argv[i], "--topmost")) topmost = 1;
        else if (!strcmp(argv[i], "--cover") && i + 1 < argc) cover = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
            i++;
            if (!strcmp(argv[i], "dxgi")) backend = YSCR_BACKEND_DXGI_FLIP;
            else if (!strcmp(argv[i], "composition")) backend = YSCR_BACKEND_COMPOSITION;
            else { fprintf(stderr, "--backend: dxgi or composition\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--load") && i + 1 < argc) load = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--overrun") && i + 1 < argc) overrun = atof(argv[++i]);
        else if (!strcmp(argv[i], "--miss") && i + 1 < argc) miss_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gpu") && i + 1 < argc) gpu_clears = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc) depth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--depth-learn")) depth_learn = 1;
        else if (!strcmp(argv[i], "--long")) long_run = 1;
        else if (!strcmp(argv[i], "--hold")) hold = 1;
        else if (!strcmp(argv[i], "--codes")) codes = 1;
        else if (!strcmp(argv[i], "--row") && i + 1 < argc) { i++; row_method = !strcmp(argv[i], "update") ? 0 : 8; }
        else if (!strcmp(argv[i], "--verify") && i + 1 < argc) verify = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trigger")) trigger = 1;
        else if (!strcmp(argv[i], "--fence")) fence = 1;
        else if (!strcmp(argv[i], "--d3d11-video")) video = 1;
        else if (!strcmp(argv[i], "--trigger-offset") && i + 1 < argc) trig_offset_us = atof(argv[++i]);
        else if (!strcmp(argv[i], "--trigger-cpu") && i + 1 < argc) trig_cpu = atoi(argv[++i]) + 1;
        else if (!strcmp(argv[i], "--trigger-rt-cores")) trig_cpu = -1;
        else if (!strcmp(argv[i], "--trigger-spin") && i + 1 < argc) trig_spin_us = atof(argv[++i]);
        else if (!strcmp(argv[i], "--patch")) patch = 1;
        else if (!strcmp(argv[i], "--group")) group = 1, windowed = 1;
        else if (!strcmp(argv[i], "--allocs")) allocs = 1;
        else if (!strcmp(argv[i], "--allocs-control")) allocs = 2;
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv = argv[++i];
        else if (i == 1 && atoi(argv[i]) > 0) frames = atoi(argv[i]);
        else { fprintf(stderr, "usage: see the comment at the top of screen_flipstats.c\n"); return 2; }
    }
#if !defined(_WIN32)
    if (topmost || cover) fprintf(stderr, "screen_flipstats: --topmost and --cover act on Win32 windows; ignored here\n");
#endif
    if (frames < 1 || frames > MAX_FRAMES) { fprintf(stderr, "--frames must be 1..%d\n", MAX_FRAMES); return 2; }
    if (!sim && !windowed && !long_run && frames > 7200) frames = 7200;
    if (group) n_screens = 2;

    pol = yrt_thread_elevate(NULL);
    rd.memory = g_ring_mem;
    rd.bytes = sizeof g_ring_mem;
    if (!yrt_ring_open(&g_ring, &rd)) { fprintf(stderr, "%s\n", yrt_ring_error(&g_ring)); return 1; }
    yrt_trace_set_ring(&g_ring);
    if (csv) {
        g_csv = fopen(csv, "w");
        if (g_csv) fprintf(g_csv, "index,display,target_ns,onset_ns,dropped,path,flags,tier,eval,script,draw,upload,swap,gpu\n");
    }

#if defined(_WIN32)
    if (topmost || cover) {   /* open() settles (ysp/screen.h v0.5.0): one input event lets this
                     * program take the foreground during open, not after it */
        INPUT in0;
        memset(&in0, 0, sizeof in0);
        in0.type = INPUT_MOUSE;
        in0.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in0, sizeof in0);
    }
#endif
    for (i = 0; i < n_screens; i++) {
        memset(&d, 0, sizeof d);
        d.backend = sim ? YSCR_BACKEND_SIM : backend;
        d.windowed = windowed != 0;
        d.window_w = group ? 480 : 800;
        d.window_h = group ? 360 : 600;
        d.ring = &g_ring;
        d.display_index = (uint32_t)i;
        d.patch.on = patch != 0;
        d.d3d11_video = video != 0;
        d.depth = depth;
        d.depth_learn = depth_learn != 0;
        if (codes) {
            d.patch.corner = YSCR_BOTTOM_LEFT;
            d.codes[0] = yscr_slot_pixel_mode();
            d.codes[1] = yscr_slot_psync();
            d.n_codes = 2;
            d.verify_codes = verify;
        }
        if (trigger) {
            memset(&tdesc, 0, sizeof tdesc);
            tdesc.fn = trig_fn;
            tdesc.name = "noop";
            tdesc.offset_ns = (int64_t)(trig_offset_us * 1000.0);
            d.triggers = &tdesc;
            d.n_triggers = 1;
            d.trigger_fence = fence != 0;
            d.trigger_cpu = trig_cpu;
            d.trigger_spin_ns = (uint32_t)(trig_spin_us * 1000.0);
        }
#if defined(YSCR__DXGI)
        if (row_method >= 0) yscr__row_clear_max = row_method;
#else
        (void)row_method;   /* --row picks a D3D11 code path; nothing to pick elsewhere */
#endif
        if (!yscr_open(&scr[i], &d)) {
            fprintf(stderr, "screen_flipstats: %s\n", yscr_error(&scr[i]));
            if (i) yscr_close(&scr[0]);
            return 1;
        }
        load_gl(&scr[i], &gl[i]);
#if defined(_WIN32)
        if ((topmost || cover) && yscr_window(&scr[i])) take_foreground(yscr_window(&scr[i]), topmost && !cover);
#endif
        if (trigger) yscr_on_flip(&scr[i], flip_fn, NULL);
        yscr_describe(&scr[i], line, sizeof line);
        printf("%s\n", line);
        {
            yscr_native_info ni;
            if (yscr_native(&scr[i], &ni) == YSCR_OK)
                printf("device: video_support=%d multithread_protected=%d\n", ni.video, ni.mt_protected);
        }
    }
#if defined(_WIN32)
    {   /* timing on battery differs: every result names its power source */
        SYSTEM_POWER_STATUS ps;
        if (GetSystemPowerStatus(&ps))
            printf("power: %s, battery %d%%\n", ps.ACLineStatus == 1 ? "AC" : ps.ACLineStatus == 0 ? "battery" : "unknown",
                   ps.BatteryLifePercent == 255 ? -1 : (int)ps.BatteryLifePercent);
    }
#endif
#if defined(YSCR_MAX_DONE)
    {
        yscr_native_info nat;
        int nrc = yscr_native(&scr[0], &nat);
        printf("native: %s, device %s, context %s, egl display %s, adapter LUID %08lx:%08lx\n", yscr_strerror(nrc),
               nat.d3d11_device ? "set" : "null", nat.d3d11_context ? "set" : "null", nat.egl_display ? "set" : "null",
               (unsigned long)(uint32_t)nat.luid_high, (unsigned long)nat.luid_low);
    }
#endif
    printf("frame thread: %s; load threads %d; overrun %.1f ms in 1 of 30; hold %d; patch %d; frames %d; "
           "gpu clears %d; depth %d%s\n", yrt_policy_name(pol), load, overrun, hold, patch, frames, gpu_clears, depth,
           depth_learn ? " (learner)" : "");
    start_load(load);
#if defined(_MSC_VER) && defined(_DEBUG)
    if (allocs) _CrtSetAllocHook(alloc_hook);
#else
    if (allocs) printf("--allocs needs an MSVC debug build; not counted\n");
#endif
    drain();
    g_nrec = 0; g_ncb = 0; g_ncf = 0;
    g_count_zones = 1;

    for (i = 0; i < frames && !quit; i++) {
        yscr_frame f[2];
        int64_t t;
        int k, rc;
        g_count_allocs = allocs && i >= 30;
#if defined(_WIN32)
        if (cover && i == cover && yscr_window(&scr[0])) cover_window = cover_open(yscr_window(&scr[0]));

#endif
        if (allocs == 2 && g_count_allocs) free(malloc(16));   /* --allocs-control: the hook must see this */
        rc = n_screens == 1 ? yscr_begin(&scr[0], &f[0]) : yscr_begin_group(sp, n_screens, f);
        if (rc == YSCR_QUIT) break;
        /* A stall of the swap path is the system's; count it and go on, so
         * one stall does not end a long run. i stays the header's frame
         * index, which only a flip advances. */
        if (rc == YSCR_ERR_TIMEOUT && g_timeouts < 10) { g_timeouts++; i--; continue; }
        if (rc < 0) { fprintf(stderr, "begin: %s\n", yscr_strerror(rc)); break; }
        if (i > 0 && f[0].vblank <= g_vb_prev) g_same_vb++;
        g_vb_prev = f[0].vblank;
#if defined(YSCR_MAX_DONE)
        g_done_n += f[0].n_done;
#endif
        for (k = 0; k < n_screens; k++) {
            int w = scr[k].caps.mode.w, h = scr[k].caps.mode.h;
            if (codes) {
                uint32_t pat[8];
                yscr_psync_pattern(pat, (uint8_t)i);
                yscr_code(&scr[k], 0, yscr_pixel_mode_bits((uint32_t)i));
                yscr_code_row(&scr[k], 1, pat, 8, 1);
            }
            if (trigger && k == 0) yscr_trigger(&scr[k], 0, (uint32_t)i);
            if (!gl[k].Clear) continue;
            yscr_bind(&scr[k]);
            gl[k].Viewport(0, 0, w, h);
            gl[k].ClearColor(0.2f, 0.2f, 0.2f, 1.0f);
            gl[k].Clear(0x4000u);
            if (patch) yscr_set_patch(&scr[k], (i & 1) ? 0.3f : 0.2f);

            {
                int q;
                for (q = 0; q < gpu_clears; q++) gl[k].Clear(0x4000u);
            }
            if (miss_every >= 10 && i >= 60 && i % miss_every >= miss_every - 3) {
                int q;
                for (q = 0; q < 1500; q++) gl[k].Clear(0x4000u);
                if (i < MAX_FRAMES) g_heavy[i] = 1;
            }
        }
        if (overrun > 0 && rnd() % 30 == 0) { busy_ms(overrun); g_overran[i] = 1; }
        t = f[0].onset;
        if (i < MAX_FRAMES) g_pred[i] = f[0].onset;
        if (hold) {
            int ahead = (int)(rnd() % 5);
            double u = ((double)(rnd() % 10001) / 10000.0 - 0.5) * 0.9;   /* within +-0.45 frame */
            g_expect[i] = f[0].onset + (int64_t)ahead * f[0].period;
            t = g_expect[i] + (int64_t)(u * (double)f[0].period);
        }
        rc = n_screens == 1 ? yscr_flip_at(&scr[0], t, NULL) : yscr_flip_group_at(sp, n_screens, t);
        /* the present call returned a few us before this (header cost in flip) */
        if (i < MAX_FRAMES) g_tret[i] = yrt_now_ns();
        if (rc < 0) { fprintf(stderr, "flip: %s\n", yscr_strerror(rc)); break; }
        ran++;
        g_count_allocs = 0;
        drain();
        if (!sim && (i & 7) == 0 && quit_requested()) quit = 1;
    }
    g_stop_load = 1;
#if defined(_WIN32)
    if (cover_window) DestroyWindow(cover_window);
#endif
    code_risk = yscr_code_risk(&scr[0]);
    for (i = 0; i < n_screens; i++) {
        yscr_sync_info si;
        yscr_describe(&scr[i], line, sizeof line);
        printf("%s\n", line);
        /* v0.4.2: a legitimate run must give the guard no evidence */
        yscr_sync_check(&scr[i], &si);
        printf("sync guard: %s; %u flips with an OS time, evidence: same refresh %u, no vblank wait %u, torn %u; "
               "at most %d of 64 at once (32 fire)\n", si.untimed ? "FIRED" : "not fired", (unsigned)si.observed,
               (unsigned)si.same_refresh, (unsigned)si.no_wait, (unsigned)si.torn, si.peak);
        yscr_close(&scr[i]);
    }
    drain();
    yrt_trace_set_ring(NULL);
    if (g_csv) fclose(g_csv);

    {
        static double a[MAX_FRAMES], b[MAX_FRAMES], c[MAX_FRAMES], dd[MAX_FRAMES], e[MAX_FRAMES];
        static double pa[5][MAX_FRAMES];
        static double lat[5][MAX_FRAMES];
        int nlat[5] = { 0, 0, 0, 0, 0 }, first[5] = { 0, 0, 0, 0, 0 };
        int na = 0, nb = 0, nc = 0, nd = 0, ne = 0, npa[5] = { 0, 0, 0, 0, 0 };
        int tiers[5] = { 0, 0, 0, 0, 0 }, skipped = 0, canceled = 0, planned = 0;
        int late = 0, dropped = 0, early = 0, est = 0, unstable = 0, off_period = 0, occl = 0;
        int paths[5] = { 0, 0, 0, 0, 0 };
        int hold_ok = 0, hold_early = 0, hold_late = 0, hold_n = 0;
        int ov_n = 0, ov_late = 0, ov_ontime = 0, ov_unflagged = 0, ov_blamed = 0, false_flags = 0;
        int64_t period = scr[0].caps.period_ns;
        for (i = 0; i < g_nrec; i++) {
            const yscr_record* r = &g_rec[i];
            int64_t idx = r->index;
            if (r->path < 5) paths[r->path]++;
            if (r->tier < 5) tiers[r->tier]++;
            if (r->flags & YSCR_FLIP_SKIPPED) skipped++;
            if (r->flags & YSCR_FLIP_CANCELED) canceled++;
            if (r->flags & YSCR_FLIP_ONSET_PLANNED) planned++;
            if (r->flags & (YSCR_FLIP_SKIPPED | YSCR_FLIP_CANCELED)) continue;
            if (r->flags & YSCR_FLIP_LATE_TARGET) late++;
            if (r->flags & YSCR_FLIP_EARLY) early++;
            if (r->flags & YSCR_FLIP_ESTIMATED) est++;
            if (r->flags & YSCR_FLIP_GRID_UNSTABLE) unstable++;
            if (r->flags & YSCR_FLIP_OCCLUDED) occl++;
            if (r->dropped) dropped++;
            if (idx >= 0 && idx < ran && !hold && !(r->flags & (YSCR_FLIP_LATE_TARGET | YSCR_FLIP_ESTIMATED)) &&
                r->dropped == 0 && n_screens == 1) {
                double err = (double)(r->onset - g_pred[idx]) / 1000.0;
                a[na++] = err < 0 ? -err : err;
                if (r->path < 5) pa[r->path][npa[r->path]++] = err < 0 ? -err : err;
                if (err > (double)period / 2000.0 || err < -(double)period / 2000.0) off_period++;
            }
            /* latency: the present call's return to the reported onset, the
             * start of scanout of the frame (not light) */
            if (!hold && n_screens == 1 && idx >= 0 && idx < ran && idx < MAX_FRAMES && g_tret[idx] &&
                !(r->flags & YSCR_FLIP_ESTIMATED) && r->path < 5) {
                int64_t l = r->onset - g_tret[idx];
                lat[r->path][nlat[r->path]++] = (double)l / 1e6;
                if (l <= period) first[r->path]++;   /* the first vblank after the return */
            }
            if (hold && idx >= 0 && idx < ran && !(r->flags & (YSCR_FLIP_LATE_TARGET | YSCR_FLIP_ESTIMATED))) {
                int64_t dv = r->onset - g_expect[idx];
                hold_n++;
                if (dv < -period / 2) hold_early++;
                else if (dv > period / 2) hold_late++;
                else hold_ok++;
            }
            if (overrun > 0 && idx >= 0 && idx < ran && !hold && n_screens == 1) {
                int missed = r->onset - g_pred[idx] > period / 2;
                int flagged = (r->flags & YSCR_FLIP_LATE_TARGET) != 0;
                if (g_overran[idx]) {
                    ov_n++;
                    if (flagged) ov_late++;
                    else if (!missed) ov_ontime++;
                    if (missed && !flagged) ov_unflagged++;
                    if (flagged && r->phase_ns[YSCR_PHASE_DRAW] >= (uint32_t)(overrun * 1e6)) ov_blamed++;
                } else if (missed || flagged || r->dropped) {
                    false_flags++;
                }
            }
            b[nb++] = (double)r->phase_ns[YSCR_PHASE_DRAW] / 1000.0;
            c[nc++] = (double)r->phase_ns[YSCR_PHASE_SWAP] / 1000.0;
        }
        for (i = 0; i < g_ncb; i++) dd[nd++] = g_cost_begin[i];
        for (i = 0; i < g_ncf; i++) e[ne++] = g_cost_flip[i];
        printf("planned at or before the previous frame's vblank: %d; records seen in f.done %d\n", g_same_vb, g_done_n);
        printf("records %d (frames run %d): late targets %d, dropped %d, early %d, estimated %d, off-grid %d, "
               "occluded %d; depth changes %d\n", g_nrec, ran, late, dropped, early, est, unstable, occl,
               g_depth_changes);
        if (g_timeouts) printf("begin timeouts (the swap path freed no slot in time): %d\n", g_timeouts);
        if (codes) {
            uint32_t chk = 0, bad = 0;
            yscr_code_verify(&scr[0], &chk, &bad);
            printf("codes: risk 0x%04x, read back %u, differed %u\n", (unsigned)code_risk, chk, bad);
        }
        if (trigger) {
            printf("triggers: %d reported, moved %d (GPU %d, of which the frame was late %d), fired a vblank early %d, "
                   "fired a vblank late %d, pending at report %d\n", g_tr_n, g_tr_moved, g_tr_gpu_moved, g_tr_gpu_caught,
                   g_tr_early, g_tr_late, g_tr_pending);
            row("trigger fired - deadline us", g_trig_late_us, g_ntl);
            {   /* "late" is the worker's: fired minus deadline, not a mismatch */
                int over20 = 0, over200 = 0, q;
                for (q = 0; q < g_ntl; q++) {
                    if (g_trig_late_us[q] > 20.0) over20++;
                    if (g_trig_late_us[q] > 200.0) over200++;
                }
                printf("  fired over 20 us after the deadline %d, over 200 us %d\n", over20, over200);
            }
            row("  of which worker wake us", g_trig_wake_us, g_ntl);
            row("  of which trigger lock us", g_trig_lock_us, g_ntl);
            row("  of which dispatch us", g_trig_disp_us, g_ntl);
#if defined(_WIN32)
            {   /* a tail that sits on some CPUs is the OS's work on them */
                int on_cpu[64] = { 0 }, late_on[64] = { 0 }, q;
                for (q = 0; q < g_ntl; q++) {
                    on_cpu[g_trig_cpu[q] & 63]++;
                    if (g_trig_late_us[q] > 20.0) late_on[g_trig_cpu[q] & 63]++;
                }
                printf("  trigger CPU (fired / over 20 us late):");
                for (q = 0; q < 64; q++) if (on_cpu[q]) printf(" %d:%d/%d", q, on_cpu[q], late_on[q]);
                printf("\n");
            }
#endif
        }
        printf("paths: composed %d, overlay %d, independent %d, simulated %d, unknown %d\n",
               paths[1], paths[2], paths[3], paths[4], paths[0]);
        printf("tiers: 1 %d, 2 %d, 3 %d, sim %d, unknown %d; skipped %d, canceled %d, onset planned %d\n",
               tiers[1], tiers[2], tiers[3], tiers[4], tiers[0], skipped, canceled, planned);
        if (!hold && n_screens == 1)
            printf("on-time frames off the predicted onset by over half a period: %d\n", off_period);
        if (overrun > 0)
            printf("overruns: %d injected; late target %d (draw phase blamed on %d), on time %d, missed "
                   "without a flag %d; other frames late, dropped or flagged %d\n",
                   ov_n, ov_late, ov_blamed, ov_ontime, ov_unflagged, false_flags);
        if (miss_every >= 10) {
            /* a drop off the heavy frames, or a frame that waited 2 vblanks
             * for the one before, is the header's cost of the misses */
            static int64_t on[MAX_FRAMES];
            static unsigned char dr[MAX_FRAMES];
            int heavy = 0, heavy_dropped = 0, other_dropped = 0, half = 0;
            for (i = 0; i < g_nrec; i++) {
                int64_t idx = g_rec[i].index;
                if (idx < 0 || idx >= ran || idx >= MAX_FRAMES) continue;
                on[idx] = g_rec[i].onset;
                dr[idx] = g_rec[i].dropped != 0;
            }
            for (i = 0; i < ran && i < MAX_FRAMES; i++) {
                if (g_heavy[i]) { heavy++; heavy_dropped += dr[i]; continue; }
                other_dropped += dr[i];
                if (i > 0 && on[i] && on[i - 1] && on[i] - on[i - 1] > period * 3 / 2 && !g_heavy[i - 1]) half++;
            }
            printf("misses: %d heavy frames, %d of them dropped; other frames dropped %d, "
                   "other frames 2 or more vblanks after the one before %d\n",
                   heavy, heavy_dropped, other_dropped, half);
        }
        if (hold)
            printf("hold: %d checked, on the asked vblank %d, early %d, late %d\n", hold_n, hold_ok, hold_early, hold_late);
        row("|onset - predicted| us (on time)", a, na);
        {
            static const char* const pn[5] = { "unknown", "composed", "overlay", "independent", "simulated" };
            int k;
            for (k = 0; k < 5; k++) if (npa[k] && npa[k] != na) {
                char label[64];
                snprintf(label, sizeof label, "  of which %s", pn[k]);
                row(label, pa[k], npa[k]);
            }
        }
        {
            static const char* const pn2[5] = { "unknown", "composed", "overlay", "independent", "simulated" };
            int k;
            for (k = 0; k < 5; k++) if (nlat[k]) {
                char label[64];
                snprintf(label, sizeof label, "present return to onset ms, %s", pn2[k]);
                printf("  %s: on the first vblank after the return %d of %d (%.1f%%)\n", pn2[k], first[k], nlat[k],
                       100.0 * first[k] / nlat[k]);
                row(label, lat[k], nlat[k]);
            }
        }
        row("draw phase us", b, nb);
        row("swap phase us (wait + present)", c, nc);
        row("header cost in begin us", dd, nd);
        row("header cost in flip us", e, ne);
        for (i = 0; i < g_nz; i++) {
            char label[64];
            snprintf(label, sizeof label, "zone %s us", g_zname[i]);
            row(label, g_zdur[i], g_zn[i]);
        }
        if (allocs) printf("C runtime heap calls in the frame loop after frame 30: %ld\n", (long)g_allocs);
    }
    return 0;
}
