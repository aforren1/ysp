/* probe_queue.c - the queue-ahead probe (docs/queue_probe.md): a DLL that a
 * Python driver (run_queue.py) calls through ctypes, so the host that
 * sleeps between calls is a real Python interpreter (time.sleep).
 * Built against the probe copy of ysp/screen.h in include/ here, which adds
 * yscr_queue_reserve(), yscr_queue_cancel() and yscr_queue_cancel_on().
 * Windows, MSVC, COMPOSITION only. */
#define COBJMACROS
#define _CRT_SECURE_NO_WARNINGS
#define YSP_RT_IMPLEMENTATION
#define YSP_SCREEN_IMPLEMENTATION
#include <ysp/rt.h>
#include <ysp/screen.h>
#include <SDL3/SDL.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <string.h>

#define PQ_API __declspec(dllexport)

typedef struct pq_rec {        /* one completed flip, as Python reads it */
    int64_t index, target, planned, onset, residual, t_arrive;
    uint32_t dropped;
    uint16_t flags;
    uint8_t path, tier;
} pq_rec;

typedef struct pq_sub {        /* one submitted frame */
    int64_t index, f_onset, target, t_begin, t_begun, t_flipped, cpu_ns;
} pq_sub;

#define PQ_MAX 8192
static yscr_screen g_scr;
static pq_rec g_rec[PQ_MAX];
static volatile LONG g_nrec;
static pq_sub g_sub[PQ_MAX];
static int g_nsub;
static char g_err[512];

typedef void (APIENTRY *pq_clearcolor_fn)(float, float, float, float);
typedef void (APIENTRY *pq_clear_fn)(unsigned);
typedef void (APIENTRY *pq_scissor_fn)(int, int, int, int);
typedef void (APIENTRY *pq_enable_fn)(unsigned);
typedef void (APIENTRY *pq_finish_fn)(void);
static pq_clearcolor_fn gl_clearcolor;
static pq_clear_fn gl_clear;
static pq_scissor_fn gl_scissor;
static pq_enable_fn gl_enable, gl_disable;
static pq_finish_fn gl_finish;
#define PQ_GL_COLOR_BUFFER_BIT 0x00004000u
#define PQ_GL_SCISSOR_TEST     0x0C11u

static void pq_on_flip(void* ctx, const yscr_record* r, const yscr_trigger_result* t, int n) {
    LONG k = g_nrec;
    (void)ctx; (void)t; (void)n;
    if (k >= PQ_MAX) return;
    g_rec[k].index = r->index;
    g_rec[k].target = r->target;
    g_rec[k].planned = r->planned;
    g_rec[k].onset = r->onset;
    g_rec[k].residual = r->residual;
    g_rec[k].t_arrive = yrt_now_ns();
    g_rec[k].dropped = r->dropped;
    g_rec[k].flags = r->flags;
    g_rec[k].path = r->path;
    g_rec[k].tier = r->tier;
    InterlockedExchange(&g_nrec, k + 1);
}

PQ_API const char* pq_error(void) { return g_err; }
PQ_API int64_t pq_now(void) { return yrt_now_ns(); }

/* windowed 0/1; w, h for a window; panic 1 arms the watchdog with
 * grace_ns as its grace (so a host that sleeps looks hung soon). */
PQ_API int pq_open(int windowed, int w, int h, const char* angle_dir, int panic, int64_t grace_ns) {
    yscr_desc d;
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_COMPOSITION;
    d.windowed = windowed != 0;
    d.window_w = w; d.window_h = h;
    d.angle_dir = angle_dir;
    d.foreground = YSCR_FOREGROUND_FORCE;
    d.window_thread = YSCR_WINDOW_THREAD_ON;
    d.panic = panic != 0;
    d.panic_grace_ns = grace_ns;
    if (!yscr_open(&g_scr, &d)) {
        snprintf(g_err, sizeof g_err, "%s", yscr_error(&g_scr));
        return -1;
    }
    yscr_on_flip(&g_scr, pq_on_flip, NULL);
    gl_clearcolor = (pq_clearcolor_fn)yscr_gl_proc(&g_scr, "glClearColor");
    gl_clear = (pq_clear_fn)yscr_gl_proc(&g_scr, "glClear");
    gl_scissor = (pq_scissor_fn)yscr_gl_proc(&g_scr, "glScissor");
    gl_enable = (pq_enable_fn)yscr_gl_proc(&g_scr, "glEnable");
    gl_disable = (pq_enable_fn)yscr_gl_proc(&g_scr, "glDisable");
    gl_finish = (pq_finish_fn)yscr_gl_proc(&g_scr, "glFinish");
    return 0;
}

PQ_API int pq_describe(char* buf, int cap) { return yscr_describe(&g_scr, buf, (size_t)cap); }

PQ_API void pq_size(int* w, int* h) {
    yscr_caps c;
    yscr_get_caps(&g_scr, &c);
    *w = c.mode.w; *h = c.mode.h;
}

/* This process's GPU memory (DXGI's local segment; on an integrated GPU,
 * the shared system memory the driver counts as local). */
static int64_t g_budget;
PQ_API int pq_mem(int64_t* local, int64_t* nonlocal);
/* the OS's budget for this process in DXGI's local segment (after pq_mem) */
PQ_API int64_t pq_budget(void) { int64_t a = 0, b = 0; pq_mem(&a, &b); return g_budget; }

PQ_API int pq_mem(int64_t* local, int64_t* nonlocal) {
    yscr_native_info ni;
    IDXGIDevice* dd = NULL;
    IDXGIAdapter* ad = NULL;
    IDXGIAdapter3* a3 = NULL;
    DXGI_QUERY_VIDEO_MEMORY_INFO mi;
    int ok = 0;
    if (yscr_native(&g_scr, &ni) != YSCR_OK || !ni.d3d11_device) return -1;
    if (SUCCEEDED(ID3D11Device_QueryInterface((ID3D11Device*)ni.d3d11_device, &IID_IDXGIDevice, (void**)&dd)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(dd, &ad)) &&
        SUCCEEDED(IDXGIAdapter_QueryInterface(ad, &IID_IDXGIAdapter3, (void**)&a3))) {
        if (SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(a3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) {
            *local = (int64_t)mi.CurrentUsage;
            g_budget = (int64_t)mi.Budget;
            ok = 1;
        }
        if (SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(a3, 0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &mi)))
            *nonlocal = (int64_t)mi.CurrentUsage;
    }
    if (a3) IDXGIAdapter3_Release(a3);
    if (ad) IDXGIAdapter_Release(ad);
    if (dd) IDXGIDevice_Release(dd);
    return ok ? 0 : -1;
}

/* n displayable textures of the screen's size in fmt (28 RGBA8, 10
 * RGBA16F), each added to a new presentation manager as a buffer; the
 * memory before and after, then all released. What K queued frames would
 * cost in that format. */
#include <psapi.h>
static int64_t pq_priv0, pq_priv1;
static int64_t pq_private(void) {
    PROCESS_MEMORY_COUNTERS_EX m;
    m.cb = sizeof m;
    return GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&m, sizeof m) ? (int64_t)m.PrivateUsage : 0;
}
/* the process's private bytes before and after the last pq_alloc_test() */
PQ_API void pq_alloc_private(int64_t* before, int64_t* after) { *before = pq_priv0; *after = pq_priv1; }
PQ_API int64_t pq_private_now(void) { return pq_private(); }

PQ_API int pq_alloc_test(int fmt, int n, int64_t* before, int64_t* after, int64_t* ns) {
    yscr_native_info ni;
    yscr__comp* c = (yscr__comp*)g_scr.pr_ctx;
    ID3D11Texture2D* tex[31];
    yscr__PBuffer* buf[31];
    yscr__PManager* pm = NULL;
    D3D11_TEXTURE2D_DESC td;
    int i, made = 0;
    int64_t nl, t0;
    /* 31 presentation buffers per manager is the documented maximum; 62
     * here was part of a run that ended in bugcheck 0x133 */
    if (n > 31 || yscr_native(&g_scr, &ni) != YSCR_OK) return -1;
    if (FAILED(YSCR__C(c->pf, CreatePresentationManager, (void**)&pm))) return -2;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)c->w; td.Height = (UINT)c->h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = (DXGI_FORMAT)fmt; td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | 0x100000;
    pq_mem(before, &nl);
    pq_priv0 = pq_private();
    t0 = yrt_now_ns();
    for (i = 0; i < n; i++) {
        tex[i] = NULL; buf[i] = NULL;
        if (FAILED(ID3D11Device_CreateTexture2D((ID3D11Device*)ni.d3d11_device, &td, NULL, &tex[i]))) break;
        if (FAILED(YSCR__C(pm, AddBufferFromResource, (void*)tex[i], (void**)&buf[i]))) { made = i + 1; break; }
        made = i + 1;
        {   /* a clear commits the memory, as a drawn frame would */
            ID3D11RenderTargetView* v = NULL;
            static const float k0[4] = { 0, 0, 0, 1 };
            if (SUCCEEDED(ID3D11Device_CreateRenderTargetView((ID3D11Device*)ni.d3d11_device, (ID3D11Resource*)tex[i], NULL, &v))) {
                ID3D11DeviceContext_ClearRenderTargetView((ID3D11DeviceContext*)ni.d3d11_context, v, k0);
                ID3D11RenderTargetView_Release(v);
            }
        }
    }
    ID3D11DeviceContext_Flush((ID3D11DeviceContext*)ni.d3d11_context);
    *ns = yrt_now_ns() - t0;
    Sleep(200);
    pq_mem(after, &nl);
    for (i = 0; i < made; i++) {
        if (buf[i]) YSCR__CREL(buf[i]);
        if (tex[i]) ID3D11Texture2D_Release(tex[i]);
    }
    pq_priv1 = pq_private();
    YSCR__CREL(pm);
    return made;
}

PQ_API int pq_reserve(int n, int64_t* ns) {
    int64_t t0 = yrt_now_ns();
    int rc = yscr_queue_reserve(&g_scr, n);
    *ns = yrt_now_ns() - t0;
    ((yscr__comp*)g_scr.pr_ctx)->qmax = 1;   /* pq_queue() raises it */
    if (rc != YSCR_OK) snprintf(g_err, sizeof g_err, "reserve %d: rc %d", n, rc);
    return rc;
}

/* The thread's CPU time from its cycle count (GetThreadTimes ticks at
 * 15.6 ms here): cycles per ns from a 50 ms spin against the rt clock. */
static double pq_cyc_per_ns;
static int64_t pq_thread_cpu(void) {
    ULONG64 c = 0;
    if (pq_cyc_per_ns == 0) {
        ULONG64 c0 = 0, c1 = 0;
        int64_t t0 = yrt_now_ns();
        QueryThreadCycleTime(GetCurrentThread(), &c0);
        while (yrt_now_ns() - t0 < 50000000) { }
        QueryThreadCycleTime(GetCurrentThread(), &c1);
        pq_cyc_per_ns = (double)(c1 - c0) / (double)(yrt_now_ns() - t0);
    }
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return (int64_t)((double)c / pq_cyc_per_ns);
}

/* What a frame draws: a gray level and a square that moves with the
 * index, so each frame differs; load extra scissored full-screen clears
 * stand in for GPU work. */
static void pq_draw(int64_t index, int load) {
    int w, h, i;
    float g = (float)((index * 37) % 200 + 30) / 255.0f;
    pq_size(&w, &h);
    gl_disable(PQ_GL_SCISSOR_TEST);
    gl_clearcolor(g, g, g, 1);
    gl_clear(PQ_GL_COLOR_BUFFER_BIT);
    gl_enable(PQ_GL_SCISSOR_TEST);
    for (i = 0; i < load; i++) {
        gl_scissor(0, 0, w, h);
        gl_clearcolor(g, 0, 0, 1);
        gl_clear(PQ_GL_COLOR_BUFFER_BIT);
    }
    gl_scissor((int)((index * 53) % (w - 100)), h / 2 - 50, 100, 100);
    gl_clearcolor(1, 1, 1, 1);
    gl_clear(PQ_GL_COLOR_BUFFER_BIT);
    gl_disable(PQ_GL_SCISSOR_TEST);
}

/* K frames, drawn ahead: frame k targets t0 + k * spacing periods, t0 the
 * first begin()'s onset + lead periods. Returns the first frame's index,
 * or a negative rc (YSCR_QUIT 1 on an abort: then *quit_abort is f.abort). */
PQ_API int64_t pq_queue(int K, int spacing, int lead, int load, int64_t* t0_out, uint32_t* quit_abort) {
    int k;
    int64_t t0 = 0, first = -1;
    yscr__comp* c = (yscr__comp*)g_scr.pr_ctx;
    *quit_abort = 0;
    /* K in flight while it submits (the buffers are reserved already),
     * then one: the frames after wait as 0.10.3's do */
    if (K + 2 > c->nbuf) { snprintf(g_err, sizeof g_err, "reserve %d first", K); return -50; }
    c->qmax = K;
    for (k = 0; k < K; k++) {
        yscr_frame f;
        pq_sub* s = g_nsub < PQ_MAX ? &g_sub[g_nsub] : NULL;
        int64_t c0 = pq_thread_cpu(), tb = yrt_now_ns(), target;
        int rc = yscr_begin(&g_scr, &f);
        if (rc == YSCR_QUIT) {
            /* PQ_RECANCEL=1: cancel again on the frame thread when begin()
             * reports the abort (a present made just before the first
             * cancel escaped it in 3 of 10 runs) */
            if (getenv("PQ_RECANCEL")) yscr_queue_cancel(&g_scr, 0);
            *quit_abort = f.abort; c->qmax = 1; return first >= 0 ? first : -100;
        }
        if (rc != YSCR_OK) { snprintf(g_err, sizeof g_err, "begin rc %d at k %d", rc, k); return rc; }
        if (k == 0) { t0 = f.onset + (int64_t)lead * f.period; first = f.index; }
        target = t0 + (int64_t)k * spacing * f.period;
        if (s) { s->index = f.index; s->f_onset = f.onset; s->target = target; s->t_begin = tb; s->t_begun = yrt_now_ns(); }
        pq_draw(f.index, load);
        rc = yscr_flip_at(&g_scr, target, NULL);
        if (rc != YSCR_OK) {
            snprintf(g_err, sizeof g_err, "flip_at rc %d at k %d hr 0x%08lx", rc, k, (unsigned long)yscr__comp_hr);
            return rc;
        }
        if (s) { s->t_flipped = yrt_now_ns(); s->cpu_ns = pq_thread_cpu() - c0; g_nsub++; }
    }
    c->qmax = 1;
    *t0_out = t0;
    return first;
}

/* n frames, one in flight's way: each for f.onset. */
PQ_API int pq_frames(int n, int load) {
    int k;
    for (k = 0; k < n; k++) {
        yscr_frame f;
        pq_sub* s = g_nsub < PQ_MAX ? &g_sub[g_nsub] : NULL;
        int64_t c0 = pq_thread_cpu(), tb = yrt_now_ns();
        int rc = yscr_begin(&g_scr, &f);
        if (rc != YSCR_OK) { snprintf(g_err, sizeof g_err, "begin rc %d", rc); return rc; }
        if (s) { s->index = f.index; s->f_onset = f.onset; s->target = f.onset; s->t_begin = tb; s->t_begun = yrt_now_ns(); }
        pq_draw(f.index, load);
        rc = yscr_flip_at(&g_scr, f.onset, NULL);
        if (rc != YSCR_OK) { snprintf(g_err, sizeof g_err, "flip_at rc %d", rc); return rc; }
        if (s) { s->t_flipped = yrt_now_ns(); s->cpu_ns = pq_thread_cpu() - c0; g_nsub++; }
    }
    return 0;
}

/* Completes every flip in flight (yscr_wait_flip). */
PQ_API int pq_collect(void) {
    yscr_record r;
    int rc = yscr_wait_flip(&g_scr, &r);
    return rc == YSCR_ERR_ORDER ? 0 : rc;
}

/* One begin(), to read an abort; a frame is not drawn or flipped. */
PQ_API int pq_begin_abort(uint32_t* abort, int32_t* presses, int64_t* abort_ns, int* n_done) {
    yscr_frame f;
    int rc;
    memset(&f, 0, sizeof f);
    rc = yscr_begin(&g_scr, &f);
    *abort = f.abort; *presses = f.abort_presses; *abort_ns = f.abort_ns; *n_done = f.n_done;
    if (rc == YSCR_OK) {
        pq_draw(f.index, 0);
        yscr_flip_at(&g_scr, f.onset, NULL);
    }
    return rc;
}

PQ_API int pq_records(pq_rec* out, int cap, int from) {
    int n = (int)g_nrec - from, i;
    if (n > cap) n = cap;
    for (i = 0; i < n; i++) out[i] = g_rec[from + i];
    return n < 0 ? 0 : n;
}
PQ_API int pq_nrec(void) { return (int)g_nrec; }
PQ_API int pq_subs(pq_sub* out, int cap, int from) {
    int n = g_nsub - from, i;
    if (n > cap) n = cap;
    for (i = 0; i < n; i++) out[i] = g_sub[from + i];
    return n < 0 ? 0 : n;
}
PQ_API int pq_nsub(void) { return g_nsub; }

PQ_API int pq_cancel(int blank) { return yscr_queue_cancel(&g_scr, blank); }
PQ_API void pq_cancel_on(int bits) { yscr_queue_cancel_on(bits); }
/* the last cancel: call and return times, its count, the first id it
 * named, and the blank buffer's shown time (0: none yet) */
PQ_API void pq_cancel_info(int64_t* t0, int64_t* t1, int* n, int64_t* blank_shown) {
    yscr__comp* c = (yscr__comp*)g_scr.pr_ctx;
    *t0 = c->cancel_t0; *t1 = c->cancel_t1; *n = (int)c->cancel_n; *blank_shown = c->blk_shown_ns;
}
/* drains statistics (for the blank buffer's) without a frame */
PQ_API void pq_drain(void) {
    yscr__comp* c = (yscr__comp*)g_scr.pr_ctx;
    yscr__comp_drain(c);
}
/* the first cancel: its time, and who called it (1 window thread, 2 the
 * watchdog's hook thread, 3 the frame thread, 0 another); the last one's
 * caller; frames not presented because a cancel came mid-frame */
PQ_API void pq_cancel_who(int64_t* first_t0, int* first_who, int* last_who, int* dropped) {
    yscr__comp* c = (yscr__comp*)g_scr.pr_ctx;
    DWORD ids[2];
    int i;
    ids[0] = c->cancel_first_tid; ids[1] = c->cancel_tid;
    for (i = 0; i < 2; i++) {
        int w = ids[i] == yscr__wt.tid ? 1 : ids[i] == yscr__wd.tid ? 2 : ids[i] == GetCurrentThreadId() ? 3 : 0;
        if (i == 0) *first_who = w; else *last_who = w;
    }
    *first_t0 = c->cancel_first_t0;
    *dropped = (int)c->dropped_after_cancel;
}
PQ_API int pq_inflight(void) { return ((yscr__comp*)g_scr.pr_ctx)->n_out; }

/* --- keys, the watcher, the window thread -------------------------------- */

static HWND pq_hwnd(void) { return (HWND)g_scr.hwnd; }
static int pq_is_ghost(HWND h) {
    char cls[64];
    return h && GetClassNameA(h, cls, sizeof cls) && strcmp(cls, "Ghost") == 0;
}

static struct { int presses, delay_ms, gap_ms, blind; volatile LONG sent, skipped; int64_t t[16]; HANDLE th; } pq_k;

static void pq_send_shift_esc(void) {
    INPUT in[4];
    memset(in, 0, sizeof in);
    in[0].type = INPUT_KEYBOARD; in[0].ki.wScan = 0x2A; in[0].ki.dwFlags = KEYEVENTF_SCANCODE;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wScan = 0x01; in[1].ki.dwFlags = KEYEVENTF_SCANCODE;
    in[2].type = INPUT_KEYBOARD; in[2].ki.wScan = 0x01; in[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD; in[3].ki.wScan = 0x2A; in[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

static DWORD WINAPI pq_key_main(void* arg) {
    HWND h = (HWND)arg;
    int i;
    Sleep((DWORD)pq_k.delay_ms);
    for (i = 0; i < pq_k.presses && i < 16; i++) {
        HWND fg = GetForegroundWindow();
        if (fg == h || (pq_is_ghost(fg) && IsHungAppWindow(h))) {
            pq_k.t[i] = yrt_now_ns();
            pq_send_shift_esc();
            InterlockedIncrement(&pq_k.sent);
        } else if (pq_k.blind) {   /* no foreground: the request, as an operator console would */
            pq_k.t[i] = yrt_now_ns();
            yscr_request_abort();
            InterlockedIncrement(&pq_k.sent);
        } else {
            InterlockedIncrement(&pq_k.skipped);
        }
        Sleep((DWORD)pq_k.gap_ms);
    }
    return 0;
}

/* presses of Shift+Esc from a helper thread, the first after delay_ms;
 * request 1: yscr_request_abort() when the window is not in front */
PQ_API int pq_keys(int presses, int delay_ms, int gap_ms, int request) {
    pq_k.presses = presses; pq_k.delay_ms = delay_ms; pq_k.gap_ms = gap_ms; pq_k.blind = request;
    pq_k.sent = pq_k.skipped = 0;
    memset(pq_k.t, 0, sizeof pq_k.t);
    pq_k.th = CreateThread(NULL, 0, pq_key_main, pq_hwnd(), 0, NULL);
    return pq_k.th ? 0 : -1;
}
PQ_API int pq_keys_join(int64_t* t, int* sent, int* skipped) {
    if (pq_k.th) { WaitForSingleObject(pq_k.th, INFINITE); CloseHandle(pq_k.th); pq_k.th = NULL; }
    memcpy(t, pq_k.t, sizeof pq_k.t);
    *sent = (int)pq_k.sent; *skipped = (int)pq_k.skipped;
    return 0;
}

static struct { volatile LONG stop, hung_polls, ghost_polls, polls, fg_polls, iconic; int64_t max_beat_gap; HANDLE th; } pq_w;

static DWORD WINAPI pq_watch_main(void* arg) {
    HWND h = (HWND)arg;
    while (!pq_w.stop) {
        HWND fg = GetForegroundWindow();
        int64_t gap = yrt_now_ns() - (int64_t)InterlockedCompareExchange64(&yscr__wt.beat, 0, 0);
        if (gap > pq_w.max_beat_gap) pq_w.max_beat_gap = gap;
        pq_w.polls++;
        if (IsHungAppWindow(h)) pq_w.hung_polls++;
        if (pq_is_ghost(fg)) pq_w.ghost_polls++;
        if (fg == h) pq_w.fg_polls++;
        if (IsIconic(h)) pq_w.iconic = 1;
        Sleep(50);
    }
    return 0;
}
PQ_API int pq_watch_start(void) {
    memset((void*)&pq_w, 0, sizeof pq_w);
    pq_w.th = CreateThread(NULL, 0, pq_watch_main, pq_hwnd(), 0, NULL);
    return pq_w.th ? 0 : -1;
}
/* out: polls, hung polls, ghost polls, foreground polls, iconic, max window-thread beat gap ns */
PQ_API void pq_watch_stop(int64_t* out) {
    pq_w.stop = 1;
    if (pq_w.th) { WaitForSingleObject(pq_w.th, INFINITE); CloseHandle(pq_w.th); pq_w.th = NULL; }
    out[0] = pq_w.polls; out[1] = pq_w.hung_polls; out[2] = pq_w.ghost_polls; out[3] = pq_w.fg_polls;
    out[4] = pq_w.iconic; out[5] = pq_w.max_beat_gap;
}

/* the panic: released, the release time minus the panic's, rt ns */
PQ_API void pq_panic_state(int* released, int64_t* t_panic, int64_t* t_release) {
    *released = yscr__a_load(&yscr__released);
    *t_panic = yscr__wd.t_panic;
    *t_release = (int64_t)yscr__wt.t_release;
}

PQ_API void pq_close(void) { yscr_queue_cancel_on(0); yscr_close(&g_scr); }
