/* beam_race_probe.c - can this Windows machine race the beam?
 *
 * A standalone probe for an opt-in beam-racing present mode in
 * ysp/screen.h (docs/beam_race_probe.md). It presents with tearing on a
 * flip-model swapchain at times taken from a model of the raster, so the
 * tear line lands at chosen scanlines, and it measures each step in
 * software. The idea is the one of Blur Busters' "Tearline Jedi" and
 * WinUAE's lagless vsync; no code is taken from them.
 *
 *   beam_race_probe [--out DIR] [--only LIST] [--slices LIST] [--seconds S]
 *                   [--drift S] [--static-check] [--guard ROWS]
 *                   [--no-flicker] [--yes] [--buffers N] [--monitor N]
 *                   [--draw-lead US] [--lead US] [--cost-slices N] [--no-flush]
 *                   [--vary-clear] [--patch] [--exclusive] [--seam-lines N]
 *                   [--sweep-lead A:B:STEP] [--ruler]
 *
 *   --out DIR       results folder (default beam_<date>_<time>)
 *   --only LIST     sections, comma-separated: path, recipes, raster, psr,
 *                   tear, cost (default all; latency comes from cost)
 *   --slices LIST   slices per refresh for the tear section (default 2,4,10)
 *   --seconds S     seconds per tear or cost pass (default 5)
 *   --drift S       seconds of raster sampling for the drift (default 60)
 *   --static-check  every slice draws the same picture in its own rows and
 *                   a wrong color elsewhere: a tear line that lands where
 *                   planned is invisible, a miss shows as a stripe
 *   --guard ROWS    rows of the picture drawn beyond each slice edge in
 *                   --static-check (default 8): the misses it hides. The
 *                   guard wraps: the last slice also draws rows 0..ROWS and
 *                   slice 0 the last ROWS rows, for the seam between refreshes
 *   --seam-lines N  slice 0 aims N lines before the first active line, inside
 *                   the vertical blanking (default 0: at line 0)
 *   --sweep-lead A:B:STEP  tear section: one pass per lead from A to B us in
 *                   steps of STEP, --seconds each, the lead shown large on
 *                   screen; no calibration pass
 *   --ruler         ticks at the right edge every 8 rows from 64 rows above to
 *                   128 below each slice's target row (long tick at the target,
 *                   medium every 32 rows), the same in every frame
 *   --no-flicker    low-contrast colors only (photosensitivity)
 *   --yes           skip the 5-second warning countdown
 *   --buffers N     swapchain buffers (default 2)
 *   --monitor N     the Nth DXGI output over all adapters (default: primary)
 *   --draw-lead US  wake this long before a present to read input and draw
 *                   (default 1000)
 *   --lead US       present this long before the raster reaches the target
 *                   line, and skip the calibration pass
 *   --cost-slices N slices per refresh in the cost section (default 4)
 *   --no-flush      leave the draw in the command buffer until Present flushes
 *                   it (default: flush at once, so the GPU starts early)
 *   --vary-clear    clear each frame to its slice color instead of one fixed
 *                   color: shows the vblank-bound flips of the recipes section
 *   --patch         three 48-pixel photodiode patches at the left edge, a tenth,
 *                   half and nine tenths down, drawn by the slice that covers
 *                   them, light on even refreshes (30 Hz flicker); their times
 *                   go to patch_*.csv on the ysp_rt clock
 *   --exclusive     exclusive fullscreen (SetFullscreenState) and Present(0, 0)
 *                   instead of a borderless window and the tearing flag
 *
 * Esc stops a run. Windows 8.1 or later; tearing needs Windows 10 1511 and
 * a driver that supports it.
 *
 * This file is in the public domain (or MIT-0, as the ysp headers).
 */
#if !defined(_WIN32)
#error "beam_race_probe is Windows-only: it reads the raster with D3DKMTGetScanLine"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define COBJMACROS
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcommon.h>
#include <dxgi1_6.h>
#include <wtsapi32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>

#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"

#ifdef _MSC_VER
#pragma warning(disable: 4996)   /* fopen, strncpy: a probe, not a library */
#endif

/* ------------------------------------------------------------------ */
/* IIDs written out so the probe links against no GUID library; the
 * values are the ones ysp/screen.h uses. */
static const IID P_IID_IDXGIFactory1 = {0x770aae78,0xf26f,0x4dba,{0xa8,0x29,0x25,0x3c,0x83,0xd1,0xb3,0x87}};
static const IID P_IID_IDXGIFactory2 = {0x50c83a1c,0xe072,0x4c48,{0x87,0xb0,0x36,0x30,0xfa,0x36,0xa6,0xd0}};
static const IID P_IID_IDXGIFactory5 = {0x7632e1f5,0xee65,0x4dca,{0x87,0xfd,0x84,0xcd,0x75,0xf8,0x83,0x8d}};
static const IID P_IID_IDXGIDevice   = {0x54ec77fa,0x1377,0x44e6,{0x8c,0x32,0x88,0xfd,0x5f,0x44,0xc8,0x4c}};
static const IID P_IID_IDXGISwapChainMedia = {0xdd95b90b,0xf05f,0x4f6a,{0xbd,0x65,0x25,0xbf,0xb2,0x64,0xbd,0x84}};
static const IID P_IID_ID3D11DeviceContext1 = {0xbb2c6faa,0xb5fb,0x4082,{0x8e,0x6b,0x38,0x8b,0x8c,0xfa,0x90,0xe1}};
static const IID P_IID_ID3D11Texture2D = {0x6f15aaf2,0xd208,0x4e89,{0x9a,0xb4,0x48,0x95,0x35,0xd3,0x4f,0x9c}};
static const GUID P_GUID_CONSOLE_DISPLAY_STATE = {0x6fe69556,0x704a,0x47a0,{0x8f,0x24,0xc2,0x8d,0x93,0x6f,0xda,0x47}};

#define RELEASE(o) do { if (o) { (o)->lpVtbl->Release(o); (o) = NULL; } } while (0)

/* ------------------------------------------------------------------ */
/* output: stdout and summary.txt get the same lines */
static FILE* g_sum;
static void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    if (g_sum) { va_start(ap, fmt); vfprintf(g_sum, fmt, ap); va_end(ap); fflush(g_sum); }
    fflush(stdout);
}
static char g_out[MAX_PATH];
static FILE* out_file(const char* name) {
    char p[MAX_PATH + 128];
    snprintf(p, sizeof p, "%s\\%s", g_out, name);
    return fopen(p, "wb");   /* binary: LF line endings */
}

/* ------------------------------------------------------------------ */
/* statistics on a scratch copy; post-processing only, never in a timed loop */
static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}
typedef struct { double p01, p50, p99, max, min, mean; size_t n; } dist;
static dist dist_of(const double* v, size_t n) {
    dist d; double* c; size_t i; double s = 0;
    memset(&d, 0, sizeof d);
    d.n = n;
    if (!n) return d;
    c = (double*)malloc(n * sizeof *c);
    if (!c) return d;
    memcpy(c, v, n * sizeof *c);
    qsort(c, n, sizeof *c, cmp_d);
    for (i = 0; i < n; i++) s += c[i];
    d.mean = s / (double)n;
    d.min = c[0]; d.max = c[n - 1];
    d.p01 = c[(size_t)((double)(n - 1) * 0.01)];
    d.p50 = c[(size_t)((double)(n - 1) * 0.50)];
    d.p99 = c[(size_t)((double)(n - 1) * 0.99)];
    free(c);
    return d;
}
static void abs_inplace(double* v, size_t n) { size_t i; for (i = 0; i < n; i++) v[i] = fabs(v[i]); }

/* ------------------------------------------------------------------ */
/* D3DKMT: declared here because d3dkmthk.h is a WDK header */
typedef UINT kmt_handle;
typedef struct { WCHAR DeviceName[32]; kmt_handle hAdapter; LUID AdapterLuid; UINT VidPnSourceId; } kmt_open_gdi;
typedef struct { kmt_handle hAdapter; UINT VidPnSourceId; BOOLEAN InVerticalBlank; UINT ScanLine; } kmt_scanline;
typedef LONG (APIENTRY *kmt_open_fn)(kmt_open_gdi*);
typedef LONG (APIENTRY *kmt_scan_fn)(kmt_scanline*);
static kmt_scan_fn g_kmt_scan;
static kmt_handle g_kmt_adapter;
static UINT g_kmt_source;

typedef struct { int64_t t; int32_t w; uint16_t line; uint8_t vb; uint8_t ok; } samp;

/* One scanline read, stamped at the middle of the call; w is the call's
 * width, the stamp's uncertainty. */
static int scan_read(samp* s) {
    kmt_scanline k;
    int64_t t0, t1;
    LONG r;
    k.hAdapter = g_kmt_adapter; k.VidPnSourceId = g_kmt_source; k.InVerticalBlank = 0; k.ScanLine = 0;
    t0 = yrt_now_ns();
    r = g_kmt_scan(&k);
    t1 = yrt_now_ns();
    s->t = (t0 + t1) / 2; s->w = (int32_t)(t1 - t0);
    s->line = (uint16_t)(k.ScanLine > 65535u ? 65535u : k.ScanLine);
    s->vb = (uint8_t)(k.InVerticalBlank != 0);
    s->ok = (uint8_t)(r == 0);
    return r == 0;
}

/* ------------------------------------------------------------------ */
/* the sampler thread: polls the raster and keeps every vblank entry */
typedef struct {
    volatile LONG run;
    samp* buf; size_t cap, n;
    int64_t keep_ns;            /* 0 keeps every read */
    int64_t* ent; int32_t* ent_w; size_t ent_cap, n_ent;
    uint64_t calls, fails;
    int max_line_active, max_line_vb, min_line_vb;
    HANDLE th;
} sampler;
static sampler g_smp;

static DWORD WINAPI sampler_main(LPVOID arg) {
    sampler* s = (sampler*)arg;
    samp prev, cur;
    int have = 0;
    int64_t last_keep = 0;
    yrt_thread_elevate(NULL);
    memset(&prev, 0, sizeof prev);
    while (s->run) {
        if (!scan_read(&cur)) { s->fails++; continue; }
        s->calls++;
        if (cur.vb) {
            if (cur.line > s->max_line_vb) s->max_line_vb = cur.line;
            if (cur.line < s->min_line_vb) s->min_line_vb = cur.line;
        } else if (cur.line > s->max_line_active) s->max_line_active = cur.line;
        if (have && !prev.vb && cur.vb && s->n_ent < s->ent_cap) {
            s->ent[s->n_ent] = (prev.t + cur.t) / 2;
            s->ent_w[s->n_ent] = (int32_t)(cur.t - prev.t);
            s->n_ent++;
        }
        if (s->n < s->cap && (s->keep_ns == 0 || cur.t - last_keep >= s->keep_ns)) {
            s->buf[s->n++] = cur;
            last_keep = cur.t;
        }
        prev = cur; have = 1;
    }
    return 0;
}
static void sampler_start(size_t cap, int64_t keep_ns) {
    sampler* s = &g_smp;
    if (s->cap < cap) {
        free(s->buf);
        s->buf = (samp*)malloc(cap * sizeof *s->buf);
        s->cap = s->buf ? cap : 0;
    }
    if (!s->ent) {
        s->ent_cap = 1 << 16;
        s->ent = (int64_t*)malloc(s->ent_cap * sizeof *s->ent);
        s->ent_w = (int32_t*)malloc(s->ent_cap * sizeof *s->ent_w);
    }
    s->n = 0; s->n_ent = 0; s->calls = 0; s->fails = 0; s->keep_ns = keep_ns;
    s->max_line_active = 0; s->max_line_vb = 0; s->min_line_vb = 65535;
    s->run = 1;
    s->th = CreateThread(NULL, 0, sampler_main, s, 0, NULL);
}
static void sampler_stop(void) {
    g_smp.run = 0;
    if (g_smp.th) { WaitForSingleObject(g_smp.th, INFINITE); CloseHandle(g_smp.th); g_smp.th = NULL; }
}

/* ------------------------------------------------------------------ */
/* console display state and session lock, logged with each run */
typedef struct { int64_t t; int kind; int value; } st_rec;   /* kind 0 display (0 off, 1 on, 2 dimmed), 1 lock */
static st_rec g_st[256]; static volatile LONG g_nst; static volatile LONG g_st_stop; static HANDLE g_st_th;
static void st_add(int kind, int value) {
    LONG i = g_nst;
    if (i < 256) { g_st[i].t = yrt_now_ns(); g_st[i].kind = kind; g_st[i].value = value; g_nst = i + 1; }
}
static int st_locked_now(void) {
    WTSINFOEXW* info = NULL; DWORD n = 0; int locked = -1;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx, (LPWSTR*)&info, &n) && info) {
        if (info->Level == 1)
            locked = info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK ? 1 :
                     info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_UNLOCK ? 0 : -1;
        WTSFreeMemory(info);
    }
    return locked;
}
static LRESULT CALLBACK st_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_POWERBROADCAST && wp == PBT_POWERSETTINGCHANGE) {
        POWERBROADCAST_SETTING* ps = (POWERBROADCAST_SETTING*)lp;
        if (IsEqualGUID(&ps->PowerSetting, &P_GUID_CONSOLE_DISPLAY_STATE)) st_add(0, (int)*(DWORD*)ps->Data);
        return TRUE;
    }
    if (m == WM_WTSSESSION_CHANGE) {
        if (wp == WTS_SESSION_LOCK) st_add(1, 1);
        else if (wp == WTS_SESSION_UNLOCK) st_add(1, 0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
static DWORD WINAPI st_main(LPVOID a) {
    WNDCLASSW wc; MSG msg; HWND hw;
    (void)a;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = st_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"beam_state";
    RegisterClassW(&wc);
    hw = CreateWindowExW(WS_EX_TOOLWINDOW, L"beam_state", L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    st_add(1, st_locked_now());
    RegisterPowerSettingNotification(hw, &P_GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);   /* sends the current state at once */
    WTSRegisterSessionNotification(hw, NOTIFY_FOR_THIS_SESSION);
    while (!g_st_stop) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
    }
    st_add(1, st_locked_now());
    WTSUnRegisterSessionNotification(hw);
    DestroyWindow(hw);
    return 0;
}
static void st_report(int64_t t_base) {
    LONG i; int display_on_throughout = 1, unlocked_throughout = 1;
    say("display and session:");
    for (i = 0; i < g_nst; i++) {
        say(" %s=%d@%.1fs", g_st[i].kind ? "locked" : "display", g_st[i].value, (double)(g_st[i].t - t_base) * 1e-9);
        if (g_st[i].kind == 0 && g_st[i].value != 1) display_on_throughout = 0;
        if (g_st[i].kind == 1 && g_st[i].value != 0) unlocked_throughout = 0;
    }
    say("\n  display on throughout: %s; session unlocked throughout: %s\n",
        display_on_throughout ? "yes" : "NO", unlocked_throughout ? "yes" : "NO");
}

/* ------------------------------------------------------------------ */
/* options */
typedef struct {
    int sec_path, sec_recipes, sec_raster, sec_psr, sec_tear, sec_cost;
    int slices[8]; int n_slices;
    double seconds, drift_s;
    int static_check, guard, no_flicker, yes, buffers, monitor, cost_slices, exclusive, flush, vary_clear, patch;
    int seam_lines, ruler, sweep;
    double draw_lead_us, lead_us, sweep_a, sweep_b, sweep_step;
} options;
static options O;

enum { DRAW_BANDS, DRAW_SLICE_SHADER, DRAW_FULL_SHADER };
static int g_no_bars;   /* the recipes section draws static-check frames without their bars */
static int g_refresh;   /* the refresh a pass is drawing, for the photodiode patches */
static void draw_slice(int draw, int k, int n, int y0, int y1, int cur_x, int cur_y, float phase);

/* ------------------------------------------------------------------ */
/* window and D3D11 */
static HWND g_hwnd;
static int g_W, g_H;
static volatile LONG g_abort;
static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static ID3D11DeviceContext1* g_ctx1;
static IDXGISwapChain1* g_sc;
static IDXGISwapChainMedia* g_media;
static ID3D11RenderTargetView* g_rtv;
static int g_tearing;
static UINT g_tear_flag;   /* DXGI_PRESENT_ALLOW_TEARING, or 0 in exclusive fullscreen, where Present(0) tears by itself */
static char g_adapter_name[128];
static WCHAR g_gdi_name[32];
static RECT g_mon_rect;

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_KEYDOWN: if (wp == VK_ESCAPE) g_abort = 1; return 0;
    case WM_CLOSE: g_abort = 1; return 0;
    case WM_SETCURSOR: if (LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; } break;
    default: break;
    }
    return DefWindowProcW(h, m, wp, lp);
}
static void pump(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) g_abort = 1;
}

typedef BOOL (WINAPI *set_dpi_ctx_fn)(HANDLE);

static int pick_output(IDXGIFactory1* f, IDXGIAdapter1** out_ad, DXGI_OUTPUT_DESC* out_od) {
    UINT i, j; int idx = 0;
    HMONITOR prim = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
    say("adapters and outputs:\n");
    for (i = 0; ; i++) {
        IDXGIAdapter1* ad = NULL; DXGI_ADAPTER_DESC1 ds; char name[128];
        if (IDXGIFactory1_EnumAdapters1(f, i, &ad) != S_OK) break;
        IDXGIAdapter1_GetDesc1(ad, &ds);
        WideCharToMultiByte(CP_UTF8, 0, ds.Description, -1, name, (int)sizeof name, NULL, NULL);
        say("  adapter %u: %s (vendor 0x%04x)\n", i, name, ds.VendorId);
        for (j = 0; ; j++) {
            IDXGIOutput* o = NULL; DXGI_OUTPUT_DESC od; char on[64]; int take;
            if (IDXGIAdapter1_EnumOutputs(ad, j, &o) != S_OK) break;
            IDXGIOutput_GetDesc(o, &od);
            RELEASE(o);
            WideCharToMultiByte(CP_UTF8, 0, od.DeviceName, -1, on, (int)sizeof on, NULL, NULL);
            say("    output %d: %s %ldx%ld at (%ld,%ld)\n", idx, on,
                od.DesktopCoordinates.right - od.DesktopCoordinates.left,
                od.DesktopCoordinates.bottom - od.DesktopCoordinates.top,
                od.DesktopCoordinates.left, od.DesktopCoordinates.top);
            take = O.monitor >= 0 ? idx == O.monitor : od.Monitor == prim;
            if (take && !*out_ad) {
                *out_ad = ad; IDXGIAdapter1_AddRef(ad); *out_od = od;
                memcpy(g_adapter_name, name, sizeof g_adapter_name);
            }
            idx++;
        }
        RELEASE(ad);
    }
    return *out_ad != NULL;
}

static int d3d_open(void) {
    IDXGIFactory1* f1 = NULL; IDXGIFactory2* f2 = NULL; IDXGIFactory5* f5 = NULL;
    IDXGIAdapter1* ad = NULL; DXGI_OUTPUT_DESC od;
    IDXGIDevice* dd = NULL; IDXGIAdapter* a = NULL;
    ID3D11Texture2D* tex = NULL;
    D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    DXGI_SWAP_CHAIN_DESC1 sd;
    WNDCLASSW wc;
    HRESULT hr;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    set_dpi_ctx_fn set_dpi = u32 ? (set_dpi_ctx_fn)(void*)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : NULL;
    /* physical pixels: the window must cover the output exactly to flip */
    if (set_dpi) set_dpi((HANDLE)(intptr_t)-4 /* PER_MONITOR_AWARE_V2 */);

    if (FAILED(CreateDXGIFactory1(&P_IID_IDXGIFactory1, (void**)&f1))) { say("CreateDXGIFactory1 failed\n"); return 0; }
    memset(&od, 0, sizeof od);
    if (!pick_output(f1, &ad, &od)) { say("no output found\n"); RELEASE(f1); return 0; }
    memcpy(g_gdi_name, od.DeviceName, sizeof g_gdi_name);
    g_mon_rect = od.DesktopCoordinates;
    g_W = g_mon_rect.right - g_mon_rect.left; g_H = g_mon_rect.bottom - g_mon_rect.top;
    say("using: %s on %ls, %dx%d\n", g_adapter_name, g_gdi_name, g_W, g_H);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"beam_race_probe";
    wc.hCursor = NULL;
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST, L"beam_race_probe", L"beam_race_probe", WS_POPUP | WS_VISIBLE,
                             g_mon_rect.left, g_mon_rect.top, g_W, g_H, NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd) { say("CreateWindow failed\n"); return 0; }
    {   /* a program started from a console does not get the foreground; one
         * zero-size mouse move lets it take it (as screen_flipstats --topmost) */
        INPUT in; memset(&in, 0, sizeof in);
        in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof in);
        SetForegroundWindow(g_hwnd);
        SetWindowPos(g_hwnd, HWND_TOPMOST, g_mon_rect.left, g_mon_rect.top, g_W, g_H, SWP_SHOWWINDOW);
    }
    pump();

    hr = D3D11CreateDevice((IDXGIAdapter*)ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, levels, 2, D3D11_SDK_VERSION,
                           &g_dev, NULL, &g_ctx);
    if (hr == E_INVALIDARG)
        hr = D3D11CreateDevice((IDXGIAdapter*)ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, levels + 1, 1, D3D11_SDK_VERSION,
                               &g_dev, NULL, &g_ctx);
    RELEASE(ad);
    RELEASE(f1);
    if (FAILED(hr)) { say("D3D11CreateDevice 0x%08lx\n", (unsigned long)hr); return 0; }
    ID3D11Device_QueryInterface(g_dev, &P_IID_IDXGIDevice, (void**)&dd);
    if (dd) IDXGIDevice_GetAdapter(dd, &a);
    if (a) IDXGIAdapter_GetParent(a, &P_IID_IDXGIFactory2, (void**)&f2);
    RELEASE(a); RELEASE(dd);
    if (!f2) { say("no DXGI 1.2 factory\n"); return 0; }
    if (SUCCEEDED(IDXGIFactory2_QueryInterface(f2, &P_IID_IDXGIFactory5, (void**)&f5)) && f5) {
        BOOL t = FALSE;
        if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &t, (UINT)sizeof t)))
            g_tearing = t != FALSE;
        RELEASE(f5);
    }
    say("DXGI_FEATURE_PRESENT_ALLOW_TEARING: %s\n", g_tearing ? "supported" : "NOT supported");

    memset(&sd, 0, sizeof sd);
    sd.Width = (UINT)g_W; sd.Height = (UINT)g_H;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = (UINT)O.buffers;
    sd.Scaling = DXGI_SCALING_NONE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(f2, (IUnknown*)g_dev, g_hwnd, &sd, NULL, NULL, &g_sc);
    if (SUCCEEDED(hr)) IDXGIFactory2_MakeWindowAssociation(f2, g_hwnd, DXGI_MWA_NO_ALT_ENTER);
    RELEASE(f2);
    if (FAILED(hr)) { say("CreateSwapChainForHwnd 0x%08lx\n", (unsigned long)hr); return 0; }
    IDXGISwapChain1_QueryInterface(g_sc, &P_IID_IDXGISwapChainMedia, (void**)&g_media);
    IDXGISwapChain1_GetBuffer(g_sc, 0, &P_IID_ID3D11Texture2D, (void**)&tex);
    ID3D11DeviceContext_QueryInterface(g_ctx, &P_IID_ID3D11DeviceContext1, (void**)&g_ctx1);
    if (!tex || !g_ctx1) { say("no back buffer or no ID3D11DeviceContext1\n"); RELEASE(tex); return 0; }
    /* flip model: buffer 0 always names the current back buffer */
    hr = ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource*)tex, NULL, &g_rtv);
    RELEASE(tex);
    if (FAILED(hr)) { say("CreateRenderTargetView 0x%08lx\n", (unsigned long)hr); return 0; }
    g_tear_flag = g_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    if (O.exclusive) {
        /* exclusive fullscreen refuses the tearing flag; Present(0) there
         * flips at once where the hardware can */
        RELEASE(g_rtv);
        hr = IDXGISwapChain1_SetFullscreenState(g_sc, TRUE, NULL);
        if (SUCCEEDED(hr))
            hr = IDXGISwapChain1_ResizeBuffers(g_sc, 0, (UINT)g_W, (UINT)g_H, DXGI_FORMAT_UNKNOWN, sd.Flags);
        if (FAILED(hr)) { say("exclusive fullscreen 0x%08lx\n", (unsigned long)hr); return 0; }
        IDXGISwapChain1_GetBuffer(g_sc, 0, &P_IID_ID3D11Texture2D, (void**)&tex);
        if (!tex || FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource*)tex, NULL, &g_rtv))) {
            RELEASE(tex); say("exclusive fullscreen: no render target\n"); return 0;
        }
        RELEASE(tex);
        g_tear_flag = 0;
    }
    say("swapchain: flip discard, %d buffers, R8G8B8A8, flags %s, %s\n", O.buffers, g_tearing ? "ALLOW_TEARING" : "none",
        O.exclusive ? "exclusive fullscreen, Present(0, 0)" : "borderless window covering the output, Present(0, ALLOW_TEARING)");
    return 1;
}

static int kmt_open(void) {
    HMODULE g = LoadLibraryW(L"gdi32.dll");
    kmt_open_fn op = g ? (kmt_open_fn)(void*)GetProcAddress(g, "D3DKMTOpenAdapterFromGdiDisplayName") : NULL;
    kmt_open_gdi o;
    g_kmt_scan = g ? (kmt_scan_fn)(void*)GetProcAddress(g, "D3DKMTGetScanLine") : NULL;
    if (!op || !g_kmt_scan) { say("D3DKMT entry points missing\n"); return 0; }
    memset(&o, 0, sizeof o);
    memcpy(o.DeviceName, g_gdi_name, sizeof o.DeviceName);
    if (op(&o) != 0) { say("D3DKMTOpenAdapterFromGdiDisplayName failed\n"); return 0; }
    g_kmt_adapter = o.hAdapter; g_kmt_source = o.VidPnSourceId;
    return 1;
}

/* ------------------------------------------------------------------ */
/* the output's mode timing, from the display configuration */
typedef struct {
    int found, active_w, active_h, total_w, total_h;
    double pixel_rate, hsync_hz, vsync_hz;
    UINT32 tech, scan_order;
    char monitor[128];
} mode_info;
static mode_info g_mode;

static const char* tech_name(UINT32 t) {
    switch (t) {
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: return "HDMI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: return "DisplayPort";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED: return "embedded DisplayPort (eDP)";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL: return "internal";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI: return "DVI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15: return "VGA";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS: return "LVDS";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST: return "Miracast";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED: return "indirect wired";
    default: return "other";
    }
}
static void mode_query(void) {
    UINT32 np = 0, nm = 0, i;
    DISPLAYCONFIG_PATH_INFO* paths;
    DISPLAYCONFIG_MODE_INFO* modes;
    memset(&g_mode, 0, sizeof g_mode);
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return;
    paths = (DISPLAYCONFIG_PATH_INFO*)calloc(np, sizeof *paths);
    modes = (DISPLAYCONFIG_MODE_INFO*)calloc(nm, sizeof *modes);
    if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS) {
        for (i = 0; i < np; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sn;
            DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
            UINT32 mi;
            memset(&sn, 0, sizeof sn);
            sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sn.header.size = sizeof sn;
            sn.header.adapterId = paths[i].sourceInfo.adapterId;
            sn.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS) continue;
            if (wcscmp(sn.viewGdiDeviceName, g_gdi_name) != 0) continue;
            mi = paths[i].targetInfo.modeInfoIdx;
            if (mi < nm && modes[mi].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
                DISPLAYCONFIG_VIDEO_SIGNAL_INFO* v = &modes[mi].targetMode.targetVideoSignalInfo;
                g_mode.found = 1;
                g_mode.active_w = (int)v->activeSize.cx; g_mode.active_h = (int)v->activeSize.cy;
                g_mode.total_w = (int)v->totalSize.cx; g_mode.total_h = (int)v->totalSize.cy;
                g_mode.pixel_rate = (double)v->pixelRate;
                g_mode.hsync_hz = v->hSyncFreq.Denominator ? (double)v->hSyncFreq.Numerator / v->hSyncFreq.Denominator : 0;
                g_mode.vsync_hz = v->vSyncFreq.Denominator ? (double)v->vSyncFreq.Numerator / v->vSyncFreq.Denominator : 0;
                g_mode.scan_order = (UINT32)v->scanLineOrdering;
            }
            g_mode.tech = (UINT32)paths[i].targetInfo.outputTechnology;
            memset(&tn, 0, sizeof tn);
            tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tn.header.size = sizeof tn;
            tn.header.adapterId = paths[i].targetInfo.adapterId;
            tn.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&tn.header) == ERROR_SUCCESS)
                WideCharToMultiByte(CP_UTF8, 0, tn.monitorFriendlyDeviceName, -1, g_mode.monitor, (int)sizeof g_mode.monitor, NULL, NULL);
            break;
        }
    }
    free(paths); free(modes);
    if (g_mode.found) {
        say("mode (display configuration): active %dx%d, total %dx%d, pixel rate %.3f MHz, line rate %.4f kHz, refresh %.6f Hz\n",
            g_mode.active_w, g_mode.active_h, g_mode.total_w, g_mode.total_h, g_mode.pixel_rate * 1e-6,
            g_mode.hsync_hz * 1e-3, g_mode.vsync_hz);
        say("  vertical blanking %d lines; connector %s; monitor \"%s\"\n",
            g_mode.total_h - g_mode.active_h, tech_name(g_mode.tech), g_mode.monitor);
    } else {
        say("mode: the display configuration gave no timing; the raster model estimates it\n");
    }
}

/* ------------------------------------------------------------------ */
/* driver settings readable without admin: the display class key of the
 * adapter. Intel keeps its PSR switches there (names vary by driver). */
static void psr_registry(void) {
    static const WCHAR* cls = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
    HKEY root; DWORD i; int shown = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, cls, 0, KEY_READ, &root) != ERROR_SUCCESS) { say("  registry: display class key not readable\n"); return; }
    for (i = 0; ; i++) {
        WCHAR sub[64]; DWORD sl = 64; HKEY k; WCHAR desc[256]; DWORD dl = sizeof desc, ty; char dn[256];
        DWORD j;
        if (RegEnumKeyExW(root, i, sub, &sl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        if (RegQueryValueExW(k, L"DriverDesc", NULL, &ty, (BYTE*)desc, &dl) != ERROR_SUCCESS) { RegCloseKey(k); continue; }
        WideCharToMultiByte(CP_UTF8, 0, desc, -1, dn, (int)sizeof dn, NULL, NULL);
        if (strcmp(dn, g_adapter_name) != 0) { RegCloseKey(k); continue; }
        say("  registry key ...\\%ls (%s):\n", sub, dn);
        for (j = 0; ; j++) {
            WCHAR name[256]; DWORD nl = 256, vt, vl = 64; BYTE val[64]; char nm[256]; char low[256]; size_t c;
            LONG r = RegEnumValueW(k, j, name, &nl, NULL, &vt, val, &vl);
            if (r == ERROR_NO_MORE_ITEMS) break;
            if (r != ERROR_SUCCESS && r != ERROR_MORE_DATA) continue;
            WideCharToMultiByte(CP_UTF8, 0, name, -1, nm, (int)sizeof nm, NULL, NULL);
            for (c = 0; nm[c] && c < sizeof low - 1; c++) low[c] = (char)((nm[c] >= 'A' && nm[c] <= 'Z') ? nm[c] + 32 : nm[c]);
            low[c] = 0;
            if (!strstr(low, "psr") && !strstr(low, "selfrefresh") && !strstr(low, "featuretestcontrol") &&
                !strstr(low, "drrs") && !strstr(low, "lrr")) continue;
            if (vt == REG_DWORD && r == ERROR_SUCCESS) say("    %s = 0x%08lx\n", nm, (unsigned long)*(DWORD*)val);
            else say("    %s (type %lu, %lu bytes)\n", nm, (unsigned long)vt, (unsigned long)vl);
            shown++;
        }
        RegCloseKey(k);
    }
    RegCloseKey(root);
    if (!shown) say("  registry: no value named like PSR, self refresh, DRRS, LRR or FeatureTestControl for this adapter\n");
}

/* ------------------------------------------------------------------ */
/* drawing: clears and one full-screen shader */
typedef HRESULT (WINAPI *d3dcompile_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                        LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
static ID3D11VertexShader* g_vs;
static ID3D11PixelShader* g_ps;
static ID3D11Buffer* g_cb;
static ID3D11RasterizerState* g_rs_scissor;
static ID3D11RasterizerState* g_rs_full;

/* A drifting grating under a Gaussian with 8 sine terms per pixel: about
 * the per-pixel work of a noise or grating stimulus, not a cheap fill. */
static const char g_hlsl[] =
    "cbuffer C : register(b0) { float4 col; float4 p; };\n"
    "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
    "float4 ps(float4 pos : SV_Position) : SV_Target {\n"
    "  float g = 0; [unroll] for (int i = 0; i < 8; i++) {\n"
    "    float f = 0.004 * (i + 1); g += sin(pos.x * f + pos.y * f * 0.37 + p.x * (i + 1)); }\n"
    "  float2 d = pos.xy - p.yz; float e = exp(-dot(d, d) * p.w);\n"
    "  return float4(col.rgb * (0.85 + 0.15 * (g / 8) * e), 1); }\n";

static int shader_open(void) {
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    d3dcompile_fn compile = m ? (d3dcompile_fn)(void*)GetProcAddress(m, "D3DCompile") : NULL;
    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr;
    if (!compile) { say("d3dcompiler_47.dll missing: no shader slices\n"); return 0; }
    hr = compile(g_hlsl, sizeof g_hlsl - 1, "beam", NULL, NULL, "vs", "vs_5_0", 0, 0, &vb, &err);
    if (FAILED(hr)) { say("vs: %s\n", err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "?"); RELEASE(err); return 0; }
    hr = compile(g_hlsl, sizeof g_hlsl - 1, "beam", NULL, NULL, "ps", "ps_5_0", 0, 0, &pb, &err);
    if (FAILED(hr)) { say("ps: %s\n", err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "?"); RELEASE(err); RELEASE(vb); return 0; }
    ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &g_vs);
    ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &g_ps);
    RELEASE(vb); RELEASE(pb);
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_cb);
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_rs_full);
    rd.ScissorEnable = TRUE;
    ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_rs_scissor);
    return g_vs && g_ps && g_cb && g_rs_full && g_rs_scissor;
}

/* y0..y1 rows; full = 1 draws the whole screen */
static void draw_shader(const float col[4], float phase, int y0, int y1, int full) {
    D3D11_MAPPED_SUBRESOURCE ms;
    D3D11_VIEWPORT vp;
    D3D11_RECT r;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource*)g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        float* f = (float*)ms.pData;
        f[0] = col[0]; f[1] = col[1]; f[2] = col[2]; f[3] = 1;
        f[4] = phase; f[5] = (float)g_W * 0.5f; f[6] = (float)g_H * 0.5f; f[7] = 1.0f / (2.0f * 300.0f * 300.0f);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource*)g_cb, 0);
    }
    vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)g_W; vp.Height = (float)g_H; vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_cb);
    if (full) {
        ID3D11DeviceContext_RSSetState(g_ctx, g_rs_full);
    } else {
        r.left = 0; r.right = g_W; r.top = y0 < 0 ? 0 : y0; r.bottom = y1 > g_H ? g_H : y1;
        ID3D11DeviceContext_RSSetState(g_ctx, g_rs_scissor);
        ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &r);
    }
    ID3D11DeviceContext_Draw(g_ctx, 3, 0);
}
static void clear_all(const float c[4]) { ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_rtv, c); }
static void clear_rows(const float c[4], int y0, int y1) {
    D3D11_RECT r;
    r.left = 0; r.right = g_W; r.top = y0 < 0 ? 0 : y0; r.bottom = y1 > g_H ? g_H : y1;
    if (r.bottom > r.top) ID3D11DeviceContext1_ClearView(g_ctx1, (ID3D11View*)g_rtv, c, &r, 1);
}
static void clear_rect(const float c[4], int x0, int y0, int x1, int y1) {
    D3D11_RECT r;
    r.left = x0 < 0 ? 0 : x0; r.right = x1 > g_W ? g_W : x1; r.top = y0 < 0 ? 0 : y0; r.bottom = y1 > g_H ? g_H : y1;
    if (r.bottom > r.top && r.right > r.left) ID3D11DeviceContext1_ClearView(g_ctx1, (ID3D11View*)g_rtv, c, &r, 1);
}

/* ------------------------------------------------------------------ */
/* GPU timestamps, and their map to the QPC clock */
typedef struct { ID3D11Query *dis, *t0, *t1; int64_t idx; } qset;
#define QRING 128
static qset g_q[QRING];
static double g_gpu_freq;

static void queries_open(void) {
    D3D11_QUERY_DESC qd; int i;
    for (i = 0; i < QRING; i++) {
        qd.MiscFlags = 0;
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; ID3D11Device_CreateQuery(g_dev, &qd, &g_q[i].dis);
        qd.Query = D3D11_QUERY_TIMESTAMP; ID3D11Device_CreateQuery(g_dev, &qd, &g_q[i].t0);
        ID3D11Device_CreateQuery(g_dev, &qd, &g_q[i].t1);
        g_q[i].idx = -1;
    }
}
static int get_data(ID3D11Query* q, void* out, UINT n) {
    HRESULT hr;
    while ((hr = ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous*)q, out, n, 0)) == S_FALSE) YieldProcessor();
    return hr == S_OK;
}

/* A timestamp issued on an idle GPU: the CPU bracket of when it ran. The
 * narrowest bracket of n tries gives the offset; its width is the error. */
typedef struct { double gpu_ns, qpc_ns, width_ns; } calib;
static calib gpu_calibrate(int n) {
    calib best; int i;
    D3D11_QUERY_DESC qd; ID3D11Query *dis = NULL, *ts = NULL, *ev = NULL;
    best.gpu_ns = 0; best.qpc_ns = 0; best.width_ns = 1e18;
    qd.MiscFlags = 0;
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; ID3D11Device_CreateQuery(g_dev, &qd, &dis);
    qd.Query = D3D11_QUERY_TIMESTAMP; ID3D11Device_CreateQuery(g_dev, &qd, &ts);
    qd.Query = D3D11_QUERY_EVENT; ID3D11Device_CreateQuery(g_dev, &qd, &ev);
    for (i = 0; i < n; i++) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 t; BOOL done;
        int64_t a, b;
        ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous*)ev);
        get_data(ev, &done, sizeof done);           /* the GPU is idle */
        ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous*)dis);
        a = yrt_now_ns();
        ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous*)ts);
        ID3D11DeviceContext_Flush(g_ctx);
        get_data(ts, &t, sizeof t);
        b = yrt_now_ns();
        ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous*)dis);
        if (!get_data(dis, &dj, sizeof dj) || dj.Disjoint || !dj.Frequency) continue;
        g_gpu_freq = (double)dj.Frequency;
        if ((double)(b - a) < best.width_ns) {
            best.width_ns = (double)(b - a);
            best.qpc_ns = 0.5 * (double)(a + b);
            best.gpu_ns = (double)(t / dj.Frequency) * 1e9 + (double)(t % dj.Frequency) * 1e9 / (double)dj.Frequency;
        }
    }
    RELEASE(dis); RELEASE(ts); RELEASE(ev);
    return best;
}
static calib g_cal0, g_cal1;
static double gpu_to_qpc(double gpu_ns) {
    if (g_cal1.width_ns < 1e17 && g_cal1.gpu_ns != g_cal0.gpu_ns) {
        double s = (g_cal1.qpc_ns - g_cal0.qpc_ns) / (g_cal1.gpu_ns - g_cal0.gpu_ns);
        return g_cal0.qpc_ns + (gpu_ns - g_cal0.gpu_ns) * s;
    }
    return g_cal0.qpc_ns + (gpu_ns - g_cal0.gpu_ns);
}

/* ------------------------------------------------------------------ */
/* the raster model: vblank entries at T0 + n P; in the active area
 * line = a + b (t - entry before t) */
typedef struct { int ok; double T0, P, a, b, vtotal_est, blank_ns; int H; } model;
static model g_m;
static int64_t g_tbase;

static double m_phase(const model* m, double t) {
    double x = fmod(t - m->T0, m->P);
    return x < 0 ? x + m->P : x;
}
static double m_line(const model* m, double t) { return m->a + m->b * m_phase(m, t); }
static double m_entry_before(const model* m, double t) { return t - m_phase(m, t); }
/* the time the raster reaches line L in the frame after the entry Tn */
static double m_time(const model* m, double Tn, double L) { return Tn + (L - m->a) / m->b; }

/* Fit P and T0 from the entries, then a and b from the active reads. */
static int model_fit(model* m, const int64_t* ent, const int32_t* ent_w, size_t ne,
                     const samp* s, size_t ns, double P0, dist* ent_res, dist* call_w) {
    size_t i, k; double* r; int pass;
    double sx, sy, sxx, sxy, n;
    if (ne < 10 || ns < 1000) return 0;
    r = (double*)malloc((ne > ns ? ne : ns) * sizeof *r);
    if (!r) return 0;
    m->P = P0; m->T0 = (double)(ent[0] - g_tbase);
    for (pass = 0; pass < 3; pass++) {
        sx = sy = sxx = sxy = n = 0;
        for (i = 0; i < ne; i++) {
            double t = (double)(ent[i] - g_tbase), x = floor((t - m->T0) / m->P + 0.5);
            double res = t - (m->T0 + x * m->P);
            if (ent_w[i] > 20000 || (pass > 0 && fabs(res) > 30000)) continue;
            sx += x; sy += t; sxx += x * x; sxy += x * t; n += 1;
        }
        if (n < 10) { free(r); return 0; }
        m->P = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        m->T0 = (sy - m->P * sx) / n;
    }
    for (i = k = 0; i < ne; i++) {
        double t = (double)(ent[i] - g_tbase), x = floor((t - m->T0) / m->P + 0.5);
        if (ent_w[i] <= 20000) r[k++] = t - (m->T0 + x * m->P);
    }
    abs_inplace(r, k);
    *ent_res = dist_of(r, k);
    for (i = 0; i < ns; i++) r[i] = (double)s[i].w;
    *call_w = dist_of(r, ns);
    /* line against the time since the entry */
    m->a = 0; m->b = 0;
    for (pass = 0; pass < 2; pass++) {
        double a = m->a, b = m->b;
        sx = sy = sxx = sxy = n = 0;
        for (i = 0; i < ns; i++) {
            double t, ph;
            if (s[i].vb || !s[i].ok || s[i].w > 10000) continue;
            t = (double)(s[i].t - g_tbase); ph = m_phase(m, t);
            if (pass > 0 && fabs((double)s[i].line - (a + b * ph)) > 3) continue;
            sx += ph; sy += s[i].line; sxx += ph * ph; sxy += ph * s[i].line; n += 1;
        }
        if (n < 100) { free(r); return 0; }
        m->b = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        m->a = (sy - m->b * sx) / n;
    }
    m->vtotal_est = m->b * m->P;
    m->blank_ns = -m->a / m->b;
    m->ok = 1;
    free(r);
    return 1;
}
/* |observed - model| over the active reads, in lines */
static size_t model_errors(const model* m, const samp* s, size_t ns, double* out_abs, double* out_signed) {
    size_t i, k = 0;
    for (i = 0; i < ns; i++) {
        double e;
        if (s[i].vb || !s[i].ok || s[i].w > 10000) continue;
        e = (double)s[i].line - m_line(m, (double)(s[i].t - g_tbase));
        if (e > m->vtotal_est / 2) e -= m->vtotal_est;
        if (e < -m->vtotal_est / 2) e += m->vtotal_est;
        if (out_abs) out_abs[k] = fabs(e);
        if (out_signed) out_signed[k] = e;
        k++;
    }
    return k;
}

/* ------------------------------------------------------------------ */
/* present helpers */
static const float k_gray0[4] = { 0.50f, 0.50f, 0.50f, 1 };
static const float k_gray1[4] = { 0.506f, 0.506f, 0.506f, 1 };   /* 1 code apart: invisible, but every frame differs */
static float k_base[4] = { 0.30f, 0.30f, 0.30f, 1 };   /* the one full-clear color of every tearing frame; the wrong color in --static-check */

static int path_mode(UINT* mode) {
    DXGI_FRAME_STATISTICS_MEDIA st;
    if (!g_media) return 0;
    if (FAILED(IDXGISwapChainMedia_GetFrameStatisticsMedia(g_media, &st))) return 0;
    *mode = (UINT)st.CompositionMode;
    return 1;
}
static const char* mode_name(UINT m) {
    switch (m) {
    case DXGI_FRAME_PRESENTATION_MODE_COMPOSED: return "composed";
    case DXGI_FRAME_PRESENTATION_MODE_OVERLAY: return "overlay";
    case DXGI_FRAME_PRESENTATION_MODE_NONE: return "none (independent flip)";
    case DXGI_FRAME_PRESENTATION_MODE_COMPOSITION_FAILURE: return "composition failure";
    default: return "?";
    }
}

/* vsync presents of a changing gray for `sec` seconds (the sampler runs
 * beside it); counts the paths seen */
static void present_vsync(double sec, int* n_path) {
    int64_t end = yrt_now_ns() + (int64_t)(sec * 1e9);
    int i = 0;
    while (yrt_now_ns() < end && !g_abort) {
        UINT md;
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, NULL);
        clear_all(k_base);
        clear_rows((i++ & 1) ? k_gray1 : k_gray0, 0, g_H - 1);   /* not the whole surface: it would set a new clear color */
        IDXGISwapChain1_Present(g_sc, 1, 0);
        if (n_path && path_mode(&md) && md < 4) n_path[md]++;
        pump();
    }
}
static void idle_wait(double sec) {
    int64_t end = yrt_now_ns() + (int64_t)(sec * 1e9);
    while (yrt_now_ns() < end && !g_abort) { pump(); Sleep(10); }
}

/* ------------------------------------------------------------------ */
/* section: path */
static void section_path(void) {
    int n1[4] = {0, 0, 0, 0}, v;
    say("\n== path: does a tearing present go independent flip, and tear in? ==\n");
    present_vsync(1.0, n1);
    say("Present(1, 0), 1 s: paths composed %d, overlay %d, none %d, failure %d\n", n1[0], n1[1], n1[2], n1[3]);
    if (!g_tear_flag && !O.exclusive) return;
    for (v = 0; v < 2 && !g_abort; v++) {
        int n0[4] = {0, 0, 0, 0}, i;
        UINT pc0 = 0, pc1 = 0;
        DXGI_FRAME_STATISTICS a, b;
        double call[240]; dist cd; int64_t t_start;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        present_vsync(0.2, NULL);
        IDXGISwapChain1_GetFrameStatistics(g_sc, &a);
        IDXGISwapChain1_GetLastPresentCount(g_sc, &pc0);
        t_start = yrt_now_ns();
        for (i = 0; i < 240 && !g_abort; i++) {
            UINT md; int64_t c0;
            ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, NULL);
            if (v == 0) clear_all((i & 1) ? k_gray1 : k_gray0);
            else { clear_all(k_base); clear_rows((i & 1) ? k_gray1 : k_gray0, 0, g_H / 2); }   /* not the full screen: see the recipes */
            ID3D11DeviceContext_Flush(g_ctx);
            c0 = yrt_now_ns();
            IDXGISwapChain1_Present(g_sc, 0, g_tear_flag);
            call[i] = (double)(yrt_now_ns() - c0) * 1e-3;
            if (path_mode(&md) && md < 4) n0[md]++;
            yrt_sleep_until(yrt_now_ns() + 4000000, YRT_DEFAULT_SPIN_NS);   /* about 4 presents per refresh */
            pump();
        }
        IDXGISwapChain1_GetFrameStatistics(g_sc, &b);
        IDXGISwapChain1_GetLastPresentCount(g_sc, &pc1);
        cd = dist_of(call, (size_t)i);
        say("Present(0, %s) 4 ms apart, %s: %d presents in %.2f s; paths composed %d, overlay %d, none %d, failure %d\n",
            g_tear_flag ? "ALLOW_TEARING" : "0", v == 0 ? "full clear to a new color each frame" : "one clear color, a half-screen ClearView changes the frame",
            i, (double)(yrt_now_ns() - t_start) * 1e-9, n0[0], n0[1], n0[2], n0[3]);
        say("  Present call p50 %.1f p99 %.1f max %.1f us (a call that waits for a vblank takes up to 16.7 ms)\n", cd.p50, cd.p99, cd.max);
        say("  calls %u; DXGI: presents shown %u over %u refreshes, %.2f per refresh (tearing flips show about 4)\n",
            pc1 - pc0, b.PresentCount - a.PresentCount, b.SyncRefreshCount - a.SyncRefreshCount,
            b.SyncRefreshCount != a.SyncRefreshCount ? (double)(b.PresentCount - a.PresentCount) / (double)(b.SyncRefreshCount - a.SyncRefreshCount) : 0.0);
    }
    {
        UINT md;
        if (path_mode(&md)) say("path of the last present: %s\n", mode_name(md));
    }
}

/* ------------------------------------------------------------------ */
/* section: which frame contents tear in. On the Iris Xe (2026-10-09) a
 * tearing present flipped at once only when its buffer had been cleared
 * last to the same color as the buffer on screen; a full clear to a new
 * color, a ClearView of the whole surface, or no clear at all after one,
 * held it to the next vblank. Each recipe here is one kind of frame,
 * presented with tearing 4 ms apart. */
static void section_recipes(void) {
    static const char* names[] = {
        "full clear", "full clear + ClearView 1 pixel", "full clear + ClearView half screen",
        "shader full screen", "full clear + shader full screen", "shader full screen + ClearView 1 pixel",
        "ClearView full screen, no full clear", "full clear + ClearView 1 pixel + shader full screen",
        "full clear, color", "full clear + ClearView half screen, two grays", "full clear, color + ClearView half screen, gray",
        "shader full screen, color", "static-check slice frames (N=4)", "static-check slice frames, no bars", "band frames (N=4)",
        "color + gray quarter, fixed (frames equal)", "alternating color + gray quarter, moving",
        "alternating gray + gray quarter, moving", "color + alternating gray half, fixed",
        "one color, full clear only (frames equal)", "one color + shader full screen", "one color + shader on a moving quarter",
        "shader full screen, frames equal", "one color + alternating ClearView of all rows but one" };
    int rc, i;
    say("\n== recipes: which frame contents tear in ==\n");
    if (!g_tear_flag && !O.exclusive) { say("  no tearing flag\n"); return; }
    for (rc = 0; rc < 24 && !g_abort; rc++) {
        DXGI_FRAME_STATISTICS a, b; double call[120]; dist cd;
        float c0[4] = { 0.50f, 0.50f, 0.50f, 1 }, c1[4] = { 0.506f, 0.506f, 0.506f, 1 };
        float k0[4] = { 0.40f, 0.32f, 0.40f, 1 }, k1[4] = { 0.42f, 0.32f, 0.40f, 1 }, gr[4] = { 0.35f, 0.35f, 0.35f, 1 };
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        present_vsync(0.3, NULL);
        IDXGISwapChain1_GetFrameStatistics(g_sc, &a);
        for (i = 0; i < 120 && !g_abort; i++) {
            const float* c = (i & 1) ? c1 : c0;
            const float* k = (i & 1) ? k1 : k0;
            int64_t t0;
            ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, NULL);
            switch (rc) {
            case 0: clear_all(c); break;
            case 1: clear_all(c); clear_rect(c, 0, 0, 1, 1); break;
            case 2: clear_all(c); clear_rows(c, 0, g_H / 2); break;
            case 3: draw_shader(c, (float)i * 0.05f, 0, g_H, 1); break;
            case 4: clear_all(c); draw_shader(c, (float)i * 0.05f, 0, g_H, 1); break;
            case 5: draw_shader(c, (float)i * 0.05f, 0, g_H, 1); clear_rect(c, 0, 0, 1, 1); break;
            case 6: clear_rows(c, 0, g_H); break;
            case 7: clear_all(c); clear_rect(c, 0, 0, 1, 1); draw_shader(c, (float)i * 0.05f, 0, g_H, 1); break;
            case 8: clear_all(k); break;
            case 9: clear_all(c); clear_rows(gr, 0, g_H / 2); break;
            case 10: clear_all(k); clear_rows(gr, 0, g_H / 2); break;
            case 11: draw_shader(k, (float)i * 0.05f, 0, g_H, 1); break;
            case 15: clear_all(k0); clear_rows(gr, 0, g_H / 4); break;
            case 16: clear_all(k); clear_rows(gr, (i % 4) * g_H / 4, (i % 4 + 1) * g_H / 4); break;
            case 17: clear_all(c); clear_rows(gr, (i % 4) * g_H / 4, (i % 4 + 1) * g_H / 4); break;
            case 18: clear_all(k0); clear_rows((i & 1) ? c1 : gr, 0, g_H / 2); break;
            case 19: clear_all(k0); break;
            case 20: clear_all(k0); draw_shader(c, (float)i * 0.05f, 0, g_H, 1); break;
            case 21: clear_all(k0); draw_shader(c, (float)i * 0.05f, (i % 4) * g_H / 4, (i % 4 + 1) * g_H / 4, 0); break;
            case 22: draw_shader(c0, 0.0f, 0, g_H, 1); break;
            case 23: clear_all(k0); clear_rows(c, 0, g_H - 1); break;
            default: {
                int sc = O.static_check, s4 = i % 4;
                O.static_check = rc < 14; g_no_bars = rc == 13;
                draw_slice(DRAW_BANDS, s4, 4, s4 * g_H / 4, (s4 + 1) * g_H / 4, -100, -100, 0.0f);
                O.static_check = sc; g_no_bars = 0;
                break;
            }
            }
            ID3D11DeviceContext_Flush(g_ctx);
            t0 = yrt_now_ns();
            IDXGISwapChain1_Present(g_sc, 0, g_tear_flag);
            call[i] = (double)(yrt_now_ns() - t0) * 1e-3;
            yrt_sleep_until(yrt_now_ns() + 4000000, YRT_DEFAULT_SPIN_NS);
            pump();
        }
        IDXGISwapChain1_GetFrameStatistics(g_sc, &b);
        cd = dist_of(call, (size_t)i);
        say("  %-52s shown per refresh %.2f (about 4 if it tears in), Present call p50 %7.1f us\n", names[rc],
            b.SyncRefreshCount != a.SyncRefreshCount ? (double)(b.PresentCount - a.PresentCount) / (double)(b.SyncRefreshCount - a.SyncRefreshCount) : 0.0,
            cd.p50);
    }
}

/* ------------------------------------------------------------------ */
/* section: raster */
static void write_entries(const char* name, const int64_t* e, const int32_t* w, size_t n) {
    FILE* f = out_file(name); size_t i;
    if (!f) return;
    fprintf(f, "t_ns,bracket_ns\n");
    for (i = 0; i < n; i++) fprintf(f, "%lld,%d\n", (long long)(e[i] - g_tbase), (int)w[i]);
    fclose(f);
}

static void report_model(const model* m, const dist* er, const dist* cw, const sampler* s, double sec) {
    say("scanline reads: %.2f M/s, call width p50 %.2f us p99 %.2f us max %.1f us; %llu failed\n",
        (double)s->calls / sec * 1e-6, cw->p50 * 1e-3, cw->p99 * 1e-3, cw->max * 1e-3, (unsigned long long)s->fails);
    say("  ScanLine seen: active 0..%d, in vblank %d..%d\n", s->max_line_active, s->min_line_vb == 65535 ? 0 : s->min_line_vb, s->max_line_vb);
    say("vblank entries: period %.3f us (%.6f Hz), entry residual |p50| %.2f us p99 %.2f us max %.2f us (n %zu)\n",
        m->P * 1e-3, 1e9 / m->P, er->p50 * 1e-3, er->p99 * 1e-3, er->max * 1e-3, er->n);
    say("line model: %.4f lines/us (%.3f us per line), vertical total estimate %.1f lines, vblank %.1f us (%.1f lines)\n",
        m->b * 1e3, 1e-3 / m->b, m->vtotal_est, m->blank_ns * 1e-3, -m->a);
    if (g_mode.found)
        say("  against the mode: vertical total %d, line time %.3f us\n", g_mode.total_h,
            g_mode.hsync_hz > 0 ? 1e6 / g_mode.hsync_hz : 0.0);
}

static void section_raster(void) {
    size_t i, k, n_fit, n_fit_ent;
    double P0, *abs_e, *sg;
    dist er, cw, de, ds;
    sampler* s = &g_smp;
    int64_t t_fit_end;
    say("\n== raster: a model of the scanline against QPC time ==\n");
    P0 = g_mode.vsync_hz > 0 ? 1e9 / g_mode.vsync_hz : 16666667.0;
    /* fit window: every read for 2 s; then one read per 50 us for the drift */
    sampler_start(12u << 20, 0);
    present_vsync(2.0, NULL);
    t_fit_end = yrt_now_ns();
    s->keep_ns = 50000;
    present_vsync(O.drift_s, NULL);
    sampler_stop();
    if (g_abort) return;
    for (n_fit = 0; n_fit < s->n && s->buf[n_fit].t < t_fit_end; n_fit++) {}
    for (n_fit_ent = 0; n_fit_ent < s->n_ent && s->ent[n_fit_ent] < t_fit_end; n_fit_ent++) {}
    g_m.H = g_H;
    if (!model_fit(&g_m, s->ent, s->ent_w, n_fit_ent, s->buf, n_fit, P0, &er, &cw)) {
        say("model fit FAILED: %zu entries, %zu reads, %llu calls, %llu failures\n",
            n_fit_ent, n_fit, (unsigned long long)s->calls, (unsigned long long)s->fails);
        return;
    }
    report_model(&g_m, &er, &cw, s, 2.0 + O.drift_s);
    abs_e = (double*)malloc(s->n * sizeof *abs_e);
    sg = (double*)malloc(s->n * sizeof *sg);
    if (!abs_e || !sg) { free(abs_e); free(sg); return; }
    k = model_errors(&g_m, s->buf, n_fit, abs_e, NULL);
    de = dist_of(abs_e, k);
    say("prediction error, fit window (2 s, %zu reads): |p50| %.3f lines %.2f us, p99 %.3f lines %.2f us, max %.2f lines %.2f us\n",
        k, de.p50, de.p50 / g_m.b * 1e-3, de.p99, de.p99 / g_m.b * 1e-3, de.max, de.max / g_m.b * 1e-3);
    {   /* drift: the 2-s model against the following reads, per 1-s bin */
        FILE* f = out_file("raster_drift.csv");
        int bin;
        double t_end = (double)(s->buf[s->n - 1].t - g_tbase);
        double t0 = (double)(t_fit_end - g_tbase);
        double first = 0, last = 0, worst = 0; int have_first = 0;
        if (f) fprintf(f, "second,n,signed_p50_us,abs_p99_us,abs_max_us\n");
        for (bin = 0; t0 + bin * 1e9 < t_end; bin++) {
            double lo = t0 + bin * 1e9, hi = lo + 1e9; size_t n = 0;
            for (i = n_fit; i < s->n; i++) {
                double t = (double)(s->buf[i].t - g_tbase), e;
                if (t < lo || t >= hi || s->buf[i].vb || !s->buf[i].ok || s->buf[i].w > 10000) continue;
                e = ((double)s->buf[i].line - m_line(&g_m, t)) / g_m.b;
                if (e > g_m.P / 2) e -= g_m.P;
                if (e < -g_m.P / 2) e += g_m.P;
                sg[n] = e; abs_e[n] = fabs(e); n++;
            }
            if (!n) continue;
            ds = dist_of(sg, n); de = dist_of(abs_e, n);
            if (f) fprintf(f, "%d,%zu,%.3f,%.3f,%.3f\n", bin, n, ds.p50 * 1e-3, de.p99 * 1e-3, de.max * 1e-3);
            if (!have_first) { first = ds.p50; have_first = 1; }
            last = ds.p50;
            if (de.max > worst) worst = de.max;
        }
        if (f) fclose(f);
        say("drift of the 2-s model over the next %.0f s: median error %+.2f us in the first second, %+.2f us in the last (%+.2f us/min); worst read %.2f us\n",
            O.drift_s, first * 1e-3, last * 1e-3, (last - first) * 1e-3 / (O.drift_s / 60.0), worst * 1e-3);
    }
    {   /* the period over the whole run */
        model m2 = g_m; dist er2, cw2;
        if (model_fit(&m2, s->ent, s->ent_w, s->n_ent, s->buf, s->n, P0, &er2, &cw2))
            say("period over %.0f s: %.4f us (the 2-s fit is %+.2f ppm off it); vblank entries against this fit: residual p99 %.2f us max %.2f us\n",
                2.0 + O.drift_s, m2.P * 1e-3, (g_m.P - m2.P) / m2.P * 1e6, er2.p99 * 1e-3, er2.max * 1e-3);
    }
    write_entries("raster_vblanks.csv", s->ent, s->ent_w, s->n_ent);
    {   /* the fit window's reads, for a plot */
        FILE* f = out_file("raster_reads_fit.csv");
        if (f) {
            fprintf(f, "t_ns,width_ns,line,vblank,model_line\n");
            for (i = 0; i < n_fit; i += 4)
                fprintf(f, "%lld,%d,%u,%u,%.2f\n", (long long)(s->buf[i].t - g_tbase), (int)s->buf[i].w,
                        s->buf[i].line, s->buf[i].vb, m_line(&g_m, (double)(s->buf[i].t - g_tbase)));
            fclose(f);
        }
    }
    free(abs_e); free(sg);
}

/* Re-anchor the model (T0 and P) on 1 s of reads; a and b stay. Returns
 * the shift of the old model's prediction, in us. */
static double model_reanchor(void) {
    model m = g_m; dist er, cw; double shift;
    sampler_start(4u << 20, 0);
    present_vsync(1.0, NULL);
    sampler_stop();
    if (!model_fit(&m, g_smp.ent, g_smp.ent_w, g_smp.n_ent, g_smp.buf, g_smp.n, g_m.P, &er, &cw)) return 0;
    shift = m_phase(&g_m, m.T0);
    if (shift > g_m.P / 2) shift -= g_m.P;
    g_m.T0 = m.T0; g_m.P = m.P;
    return shift * 1e-3;
}

/* ------------------------------------------------------------------ */
/* section: PSR signs */
static void psr_phase(const char* name, int present) {
    sampler* s = &g_smp; size_t i, k = 0, stalls = 0, active_pairs = 0;
    double *iv, *ae, line_ns, P = g_m.P; dist di, de; size_t out1 = 0;
    sampler_start(4u << 20, 5000);
    if (present) present_vsync(5.0, NULL);
    else idle_wait(5.0);
    sampler_stop();
    if (s->n_ent < 3) { say("  %s: %zu vblank entries in 5 s: the raster stopped or the vblank flag never set\n", name, s->n_ent); }
    iv = (double*)malloc((s->n_ent + s->n + 1) * sizeof *iv);
    ae = (double*)malloc((s->n + 1) * sizeof *ae);
    if (!iv || !ae) { free(iv); free(ae); return; }
    for (i = 1; i < s->n_ent; i++) {
        iv[k] = (double)(s->ent[i] - s->ent[i - 1]);
        if (fabs(iv[k] - P) > 0.01 * P) out1++;
        k++;
    }
    di = dist_of(iv, k);
    line_ns = 1.0 / g_m.b;
    for (i = 1; i < s->n; i++) {
        const samp *p = &s->buf[i - 1], *c = &s->buf[i];
        if (p->vb || c->vb) continue;
        active_pairs++;
        if (p->w > 5000 || c->w > 5000) continue;   /* a preempted read has no time to compare */
        if (c->t - p->t > 2 * line_ns && c->line == p->line) stalls++;
    }
    k = model_errors(&g_m, s->buf, s->n, ae, NULL);
    de = dist_of(ae, k);
    say("  %s: %zu vblanks, interval min %.1f p50 %.1f max %.1f us, %zu off by >1%%; frozen scanline %zu of %zu read pairs; model error p50 %.2f p99 %.2f max %.2f us\n",
        name, s->n_ent, di.min * 1e-3, di.p50 * 1e-3, di.max * 1e-3, out1, stalls, active_pairs,
        de.p50 * line_ns * 1e-3, de.p99 * line_ns * 1e-3, de.max * line_ns * 1e-3);
    free(iv); free(ae);
}
static void section_psr(void) {
    say("\n== psr: signs of Panel Self Refresh in software ==\n");
    say("connector: %s (PSR exists on embedded DisplayPort panels)\n", tech_name(g_mode.tech));
    psr_registry();
    if (!g_m.ok) { say("  no raster model: run the raster section first\n"); return; }
    /* one frame, then nothing: a panel with PSR may enter it after a few
     * static frames */
    present_vsync(0.2, NULL);
    psr_phase("static image, no presents, 5 s", 0);
    psr_phase("changing image, Present(1) every refresh, 5 s", 1);
    present_vsync(0.2, NULL);
    psr_phase("static image again, 5 s", 0);
}

/* ------------------------------------------------------------------ */
/* section: tear placement, and the cost and latency built on it */
typedef struct {
    int r, k, target, s_ret, vb_ret, late, st_mode, st_ok, y_cur;
    double t_line, t_target, t_in, t_call, t_ret, t_s;   /* ns since g_tbase */
    double gpu_b, gpu_e;                                  /* QPC ns since g_tbase, 0 if none */
    double st_qpc; UINT pc, st_pc, st_sync;
    float gpu_us;
} prec;

static const char* draw_name(int d) {
    return d == DRAW_BANDS ? "clears (light)" : d == DRAW_SLICE_SHADER ? "shader on the slice rows" : "shader on the full screen";
}

static void band_color(int k, int n, float c[4]) {
    static const float pal[10][3] = {
        {0.85f, 0.15f, 0.15f}, {0.15f, 0.75f, 0.20f}, {0.20f, 0.30f, 0.90f}, {0.90f, 0.85f, 0.15f}, {0.15f, 0.80f, 0.85f},
        {0.85f, 0.20f, 0.85f}, {0.95f, 0.55f, 0.10f}, {0.60f, 0.60f, 0.60f}, {0.45f, 0.25f, 0.10f}, {0.95f, 0.95f, 0.95f} };
    if (O.no_flicker) {
        float g = 0.30f + 0.12f * (float)k / (float)(n > 1 ? n - 1 : 1);
        c[0] = c[1] = c[2] = g;
    } else {
        c[0] = pal[k % 10][0]; c[1] = pal[k % 10][1]; c[2] = pal[k % 10][2];
    }
    c[3] = 1;
}

/* the photodiode patch rows: a tenth, half and nine tenths down */
static int patch_row(int j) { return j == 0 ? g_H / 10 : j == 1 ? g_H / 2 : g_H * 9 / 10; }

/* The overlay: ruler ticks and the swept lead's digits, one ClearView with
 * a rect list. It is the same in every frame of a pass, so a tear never
 * cuts it, and it is drawn last so it shows over the wrong color too. */
#define OV_CAP 1024
static D3D11_RECT g_ov[OV_CAP];
static UINT g_ov_n;
static void ov_add(int x0, int y0, int x1, int y1) {
    D3D11_RECT r;
    r.left = x0 < 0 ? 0 : x0; r.right = x1 > g_W ? g_W : x1; r.top = y0 < 0 ? 0 : y0; r.bottom = y1 > g_H ? g_H : y1;
    if (r.right > r.left && r.bottom > r.top && g_ov_n < OV_CAP) g_ov[g_ov_n++] = r;
}
/* seven segments a..g in bits 0..6 */
static void ov_digit(int x, int y, int w, int h, int t, unsigned m) {
    if (m & 0x01) ov_add(x, y, x + w, y + t);
    if (m & 0x02) ov_add(x + w - t, y, x + w, y + h / 2);
    if (m & 0x04) ov_add(x + w - t, y + h / 2, x + w, y + h);
    if (m & 0x08) ov_add(x, y + h - t, x + w, y + h);
    if (m & 0x10) ov_add(x, y + h / 2, x + t, y + h);
    if (m & 0x20) ov_add(x, y, x + t, y + h / 2);
    if (m & 0x40) ov_add(x, y + h / 2 - t / 2, x + w, y + h / 2 + t / 2);
}
static int slice_target(int k, int n) { return k == 0 ? -O.seam_lines : k * g_H / n; }
static void overlay_build(int n, double lead_ns) {
    static const unsigned seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    int k, off;
    g_ov_n = 0;
    if (O.ruler) {
        for (k = 0; k < n; k++) {
            int y = k * g_H / n;   /* slice 0: row 0, also with --seam-lines (the first row it must cover) */
            for (off = -64; off <= 128; off += 8) {
                int len = off == 0 ? 96 : off % 32 == 0 ? 48 : 20;
                if (y + off < 0) continue;
                ov_add(g_W - len, y + off, g_W, y + off + 2);
            }
        }
    }
    if (O.sweep) {   /* the lead in us, large: read by eye during a sweep */
        char buf[16]; int i, h = g_H / 8, w = h / 2, t = h / 10, x, y = g_H * 3 / 8 - h / 2;
        snprintf(buf, sizeof buf, "%d", (int)floor(lead_ns * 1e-3 + 0.5));
        x = g_W / 3;
        for (i = 0; buf[i]; i++, x += w + h / 4)
            ov_digit(x, y, w, h, t, buf[i] == '-' ? 0x40u : seg[(buf[i] - '0') % 10]);
    }
}

/* One frame for slice k of n: in --static-check, the picture in the
 * slice's rows (plus the guard) and a wrong color elsewhere. */
static void paint_rows(int draw, const float c[4], const float bar[4], float phase, int ya, int yb) {
    int x;
    if (yb <= ya) return;
    /* the slice's rows only: a ClearView of the whole surface flipped at the
     * vblank like a full clear to a new color (recipes section) */
    if (draw == DRAW_BANDS && !O.vary_clear) clear_rows(c, ya, yb);
    else if (draw == DRAW_SLICE_SHADER) draw_shader(c, phase, ya, yb, 0);
    if (O.static_check && !g_no_bars)   /* vertical bars: structure a stripe would cut */
        for (x = g_W / 16; x < g_W; x += g_W / 8) clear_rect(bar, x, ya, x + g_W / 64, yb);
}
static void draw_slice(int draw, int k, int n, int y0, int y1, int cur_x, int cur_y, float phase) {
    float c[4], bar[4], cur[4], ov[4];   /* the wrong color of --static-check is k_base */
    int g = O.static_check ? O.guard : 0;
    float ph = O.static_check ? 0.0f : phase;
    if (O.no_flicker) {
        bar[0] = bar[1] = bar[2] = 0.42f; cur[0] = cur[1] = cur[2] = 0.50f; ov[0] = ov[1] = ov[2] = 0.55f;
    } else {
        bar[0] = bar[1] = bar[2] = 0.85f; cur[0] = cur[1] = cur[2] = 1; ov[0] = ov[1] = ov[2] = 1;
    }
    bar[3] = cur[3] = ov[3] = 1;
    if (O.static_check) { c[0] = c[1] = c[2] = 0.35f; c[3] = 1; }
    else band_color(k, n, c);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, NULL);
    /* Every frame starts with a full clear to one fixed color. On the Iris
     * Xe a tearing present whose buffer was last cleared to another color
     * than the buffer on screen flipped at the next vblank (recipes
     * section); --vary-clear clears to the slice's color to show it. */
    if (O.vary_clear) clear_all(c);
    else clear_all(k_base);
    if (draw == DRAW_FULL_SHADER) draw_shader(c, ph, 0, g_H, 1);
    paint_rows(draw, c, bar, ph, y0 - g, y1 + g);
    /* The guard wraps between refreshes: a late slice-0 tear shows the last
     * slice's frame in the top rows, an early one shows slice 0's frame in
     * the bottom rows of the refresh before. */
    if (g > 0 && n > 1) {
        if (k == n - 1) paint_rows(draw, c, bar, ph, 0, g);
        if (k == 0) paint_rows(draw, c, bar, ph, g_H - g, g_H);
    }
    if (cur_y >= y0 - g && cur_y < y1 + g) clear_rect(cur, cur_x - 8, cur_y - 8, cur_x + 8, cur_y + 8);
    if (g_ov_n) ID3D11DeviceContext1_ClearView(g_ctx1, (ID3D11View*)g_rtv, ov, g_ov, g_ov_n);
    if (O.patch) {   /* photodiode patches: the slice that covers a patch row draws it, light on even refreshes */
        int j; float pc[4];
        pc[0] = pc[1] = pc[2] = (g_refresh & 1) ? (O.no_flicker ? 0.35f : 0.0f) : (O.no_flicker ? 0.45f : 1.0f); pc[3] = 1;
        for (j = 0; j < 3; j++) {
            int py = patch_row(j);
            if (py >= y0 && py < y1) clear_rect(pc, 0, py - 24, 48, py + 24);
        }
    }
}

typedef struct { prec* rec; size_t n, cap; double lead_ns; int N, draw, late, skipped; UINT mode_n[4]; } pass;

/* N slices per refresh for `sec` seconds, each presented `lead_ns` before
 * the raster reaches its first row. */
static void run_pass(pass* ps, int N, int draw, double lead_ns, double sec) {
    int R = (int)(sec / (g_m.P * 1e-9)) + 1, r, k;
    double dl = O.draw_lead_us * 1e3, Tn, now;
    int64_t qi = 0;
    size_t i;
    ps->N = N; ps->draw = draw; ps->lead_ns = lead_ns; ps->n = 0; ps->late = 0; ps->skipped = 0;
    memset(ps->mode_n, 0, sizeof ps->mode_n);
    ps->cap = (size_t)R * (size_t)N;
    ps->rec = (prec*)calloc(ps->cap, sizeof *ps->rec);
    if (!ps->rec) return;
    for (i = 0; i < QRING; i++) g_q[i].idx = -1;
    overlay_build(N, lead_ns);
    g_cal0 = gpu_calibrate(40);
    now = (double)(yrt_now_ns() - g_tbase);
    Tn = m_entry_before(&g_m, now) + 2 * g_m.P;
    for (r = 0; r < R && !g_abort; r++, Tn += g_m.P) {
        /* behind by a refresh or more (a Present that waited for a vblank):
         * plan the next refresh that can still be met, and count the skip */
        now = (double)(yrt_now_ns() - g_tbase);
        while (m_time(&g_m, Tn, (double)slice_target(0, N)) - lead_ns - dl < now) { Tn += g_m.P; ps->skipped++; }
        for (k = 0; k < N; k++) {
            prec* p = &ps->rec[ps->n];
            int y0 = k * g_H / N, y1 = (k + 1) * g_H / N, slot = (int)(qi % QRING);
            qset* q = &g_q[slot];
            DXGI_FRAME_STATISTICS_MEDIA st;
            POINT cp;
            samp sr;
            p->r = r; p->k = k; p->target = slice_target(k, N);   /* slice 0 may aim inside the vblank: a negative line */
            g_refresh = r;
            p->t_line = m_time(&g_m, Tn, (double)p->target);
            p->t_target = p->t_line - lead_ns;
            if (q->idx >= 0) {   /* the slot's previous timestamps, long since done */
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, b;
                if (get_data(q->dis, &dj, sizeof dj) && !dj.Disjoint && get_data(q->t0, &a, sizeof a) && get_data(q->t1, &b, sizeof b)) {
                    prec* o = &ps->rec[q->idx];
                    double fa = (double)(a / dj.Frequency) * 1e9 + (double)(a % dj.Frequency) * 1e9 / (double)dj.Frequency;
                    double fb = (double)(b / dj.Frequency) * 1e9 + (double)(b % dj.Frequency) * 1e9 / (double)dj.Frequency;
                    o->gpu_us = (float)((fb - fa) * 1e-3);
                    o->gpu_b = fa; o->gpu_e = fb;   /* GPU ns; mapped after the pass */
                }
            }
            yrt_sleep_until((int64_t)(p->t_target - dl + (double)g_tbase), YRT_DEFAULT_SPIN_NS);
            p->t_in = (double)(yrt_now_ns() - g_tbase);
            p->late = p->t_in > p->t_target;   /* no time left to draw */
            if (p->late) ps->late++;
            GetCursorPos(&cp);
            p->y_cur = cp.y - g_mon_rect.top;
            ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous*)q->dis);
            ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous*)q->t0);
            draw_slice(draw, k, N, y0, y1, cp.x - g_mon_rect.left, p->y_cur, (float)(r * 0.05));
            ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous*)q->t1);
            ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous*)q->dis);
            if (O.flush) ID3D11DeviceContext_Flush(g_ctx);   /* start the GPU now, not at Present */
            q->idx = (int64_t)ps->n;
            qi++;
            yrt_sleep_until((int64_t)(p->t_target + (double)g_tbase), YRT_DEFAULT_SPIN_NS);
            p->t_call = (double)(yrt_now_ns() - g_tbase);
            IDXGISwapChain1_Present(g_sc, 0, g_tear_flag);
            p->t_ret = (double)(yrt_now_ns() - g_tbase);
            scan_read(&sr);
            p->t_s = (double)(sr.t - g_tbase); p->s_ret = sr.line; p->vb_ret = sr.vb;
            IDXGISwapChain1_GetLastPresentCount(g_sc, &p->pc);
            if (g_media && SUCCEEDED(IDXGISwapChainMedia_GetFrameStatisticsMedia(g_media, &st))) {
                p->st_ok = 1; p->st_pc = st.PresentCount; p->st_sync = st.SyncRefreshCount;
                p->st_qpc = (double)(yrt_ticks_to_ns(st.SyncQPCTime.QuadPart) - g_tbase);
                p->st_mode = (int)st.CompositionMode;
                if (st.CompositionMode < 4) ps->mode_n[st.CompositionMode]++;
            }
            ps->n++;
        }
        pump();
    }
    for (i = 0; i < QRING; i++) {   /* drain */
        qset* q = &g_q[i];
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, b;
        if (q->idx < 0 || (size_t)q->idx >= ps->n) continue;
        if (get_data(q->dis, &dj, sizeof dj) && !dj.Disjoint && get_data(q->t0, &a, sizeof a) && get_data(q->t1, &b, sizeof b)) {
            prec* o = &ps->rec[q->idx];
            double fa = (double)(a / dj.Frequency) * 1e9 + (double)(a % dj.Frequency) * 1e9 / (double)dj.Frequency;
            double fb = (double)(b / dj.Frequency) * 1e9 + (double)(b % dj.Frequency) * 1e9 / (double)dj.Frequency;
            o->gpu_us = (float)((fb - fa) * 1e-3); o->gpu_b = fa; o->gpu_e = fb;
        }
        q->idx = -1;
    }
    g_cal1 = gpu_calibrate(40);
    for (i = 0; i < ps->n; i++) {
        prec* p = &ps->rec[i];
        if (p->gpu_e > 0) { p->gpu_b = gpu_to_qpc(p->gpu_b) - (double)g_tbase; p->gpu_e = gpu_to_qpc(p->gpu_e) - (double)g_tbase; }
    }
}

/* the observed scanline after Present returned, minus the target, in lines */
static double miss_lines(const prec* p) {
    double d = (double)p->s_ret - (double)p->target, vt = g_m.vtotal_est;
    if (d > vt / 2) d -= vt;
    if (d < -vt / 2) d += vt;
    return d;
}

static void pass_csv(const pass* ps, const char* name) {
    FILE* f = out_file(name); size_t i;
    if (!f) return;
    fprintf(f, "refresh,slice,target_line,t_line_ns,t_target_ns,t_in_ns,t_call_ns,t_ret_ns,t_scan_ns,scan_line,scan_vblank,"
               "model_line_at_scan,late,gpu_begin_ns,gpu_end_ns,gpu_us,present_count,stat_present_count,stat_sync_refresh,"
               "stat_qpc_ns,stat_mode,cursor_y\n");
    for (i = 0; i < ps->n; i++) {
        const prec* p = &ps->rec[i];
        fprintf(f, "%d,%d,%d,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%d,%d,%.2f,%d,%.0f,%.0f,%.2f,%u,%u,%u,%.0f,%d,%d\n",
                p->r, p->k, p->target, p->t_line, p->t_target, p->t_in, p->t_call, p->t_ret, p->t_s, p->s_ret, p->vb_ret,
                m_line(&g_m, p->t_s), p->late, p->gpu_b, p->gpu_e, (double)p->gpu_us, p->pc, p->st_pc, p->st_sync,
                p->st_qpc, p->st_ok ? p->st_mode : -1, p->y_cur);
    }
    fclose(f);
    if (O.patch) {   /* absolute ysp_rt times, to set beside photodiode_check --monitor edges */
        char pn[96]; int j;
        snprintf(pn, sizeof pn, "patch_%s", name);
        f = out_file(pn);
        if (!f) return;
        fprintf(f, "refresh,patch,row,light,t_row_abs_ns,t_call_abs_ns,t_ret_abs_ns,gpu_end_abs_ns\n");
        for (i = 0; i < ps->n; i++) {
            const prec* p = &ps->rec[i];
            int y0 = p->k * g_H / ps->N, y1 = (p->k + 1) * g_H / ps->N;
            for (j = 0; j < 3; j++) {
                int py = patch_row(j);
                double Tn = m_entry_before(&g_m, p->t_line);
                if (py < y0 || py >= y1) continue;
                fprintf(f, "%d,%d,%d,%d,%.0f,%.0f,%.0f,%.0f\n", p->r, j, py, !(p->r & 1),
                        m_time(&g_m, Tn, (double)py) + (double)g_tbase, p->t_call + (double)g_tbase,
                        p->t_ret + (double)g_tbase, p->gpu_e > 0 ? p->gpu_e + (double)g_tbase : 0.0);
            }
        }
        fclose(f);
    }
}

typedef struct {
    dist miss, miss_abs, call, model_err, gpu, margin, stat_line;
    size_t vb_ret, stat_mid, gpu_busy;
    double shown_per_refresh;   /* DXGI's displayed presents per refresh; N when every slice tears in */
} pass_stats;
static pass_stats pass_summary(const pass* ps) {
    pass_stats o; size_t i, n = 0, ng = 0, ns = 0;
    const prec *first = NULL, *last = NULL;
    double *a = (double*)calloc(ps->n + 1, sizeof *a), *b = (double*)calloc(ps->n + 1, sizeof *b),
           *c = (double*)calloc(ps->n + 1, sizeof *c), *d = (double*)calloc(ps->n + 1, sizeof *d),
           *e = (double*)calloc(ps->n + 1, sizeof *e), *f = (double*)calloc(ps->n + 1, sizeof *f),
           *g = (double*)calloc(ps->n + 1, sizeof *g);
    memset(&o, 0, sizeof o);
    if (!a || !b || !c || !d || !e || !f || !g) goto done;
    for (i = 0; i < ps->n; i++) {
        const prec* p = &ps->rec[i];
        if (p->r < 2) continue;   /* the first refreshes settle the path */
        if (p->vb_ret) o.vb_ret++;
        a[n] = miss_lines(p); b[n] = fabs(a[n]);
        c[n] = (p->t_ret - p->t_call) * 1e-3;
        d[n] = (double)p->s_ret - m_line(&g_m, p->t_s);   /* the model counts vblank lines as negative */
        if (d[n] > g_m.vtotal_est / 2) d[n] -= g_m.vtotal_est;
        if (d[n] < -g_m.vtotal_est / 2) d[n] += g_m.vtotal_est;
        d[n] = fabs(d[n]);
        n++;
        if (p->gpu_e > 0) {
            e[ng] = (double)p->gpu_us;
            f[ng] = (p->t_line - p->gpu_e) * 1e-3;   /* > 0: done before the raster reached the slice */
            if (p->gpu_e > p->t_call) o.gpu_busy++;
            ng++;
        }
        if (p->st_ok && p->st_qpc > 0) {
            double ph = m_phase(&g_m, p->st_qpc);
            if (ph > 0.02 * g_m.P && ph < 0.98 * g_m.P) o.stat_mid++;
            g[ns++] = m_line(&g_m, p->st_qpc);
            if (!first) first = p;
            last = p;
        }
    }
    o.miss = dist_of(a, n); o.miss_abs = dist_of(b, n); o.call = dist_of(c, n); o.model_err = dist_of(d, n);
    o.gpu = dist_of(e, ng); o.margin = dist_of(f, ng); o.stat_line = dist_of(g, ns);
    if (first && last && last->st_sync != first->st_sync)
        o.shown_per_refresh = (double)(last->st_pc - first->st_pc) / (double)(last->st_sync - first->st_sync);
done:
    free(a); free(b); free(c); free(d); free(e); free(f); free(g);
    return o;
}

static void pass_report(const pass* ps, const char* label) {
    pass_stats s = pass_summary(ps);
    double lt = 1e-3 / g_m.b;   /* us per line */
    say("  %s: N=%d, lead %.1f us, %zu presents, %d late, %d refreshes skipped, paths composed %u overlay %u none %u\n",
        label, ps->N, ps->lead_ns * 1e-3, ps->n, ps->late, ps->skipped, ps->mode_n[0], ps->mode_n[1], ps->mode_n[2]);
    say("    presents shown per refresh (DXGI): %.2f of %d planned; DXGI SyncQPCTime mid-frame on %zu of %zu stamps; GPU still busy at the Present call on %zu of %zu\n",
        s.shown_per_refresh, ps->N, s.stat_mid, s.stat_line.n, s.gpu_busy, s.gpu.n);
    say("    scanline after Present minus target: p01 %+.1f p50 %+.1f p99 %+.1f lines (p50 %+.1f us); |miss| p50 %.1f p99 %.1f max %.1f lines (max %.1f us); %zu read in vblank\n",
        s.miss.p01, s.miss.p50, s.miss.p99, s.miss.p50 * lt, s.miss_abs.p50, s.miss_abs.p99, s.miss_abs.max, s.miss_abs.max * lt, s.vb_ret);
    say("    Present call p50 %.1f p99 %.1f max %.1f us; model error at those reads p50 %.2f p99 %.2f max %.2f lines\n",
        s.call.p50, s.call.p99, s.call.max, s.model_err.p50, s.model_err.p99, s.model_err.max);
    if (s.gpu.n)
        say("    GPU per slice, on screen, p50 %.1f p99 %.1f max %.1f us; GPU done before the raster reached the slice's first row: margin p01 %.0f p50 %.0f us\n",
            s.gpu.p50, s.gpu.p99, s.gpu.max, s.margin.p01, s.margin.p50);
    if (ps->N > 1) {   /* the seam between refreshes apart from the other slice edges */
        double* v = (double*)malloc((ps->n + 1) * sizeof *v);
        size_t i, n0 = 0, n1 = ps->n;
        if (v) {
            dist d0, d1;
            for (i = 0; i < ps->n; i++) {
                const prec* p = &ps->rec[i];
                if (p->r < 2) continue;
                if (p->k == 0) v[n0++] = miss_lines(p);
                else v[--n1] = miss_lines(p);
            }
            d0 = dist_of(v, n0); d1 = dist_of(v + n1, ps->n - n1);
            say("    slice 0 (target line %d): miss p50 %+.1f p99 %+.1f lines; other slices p50 %+.1f p99 %+.1f lines\n",
                slice_target(0, ps->N), d0.p50, d0.p99, d1.p50, d1.p99);
            free(v);
        }
    }
}

/* latency, software terms: input read to the raster reaching each row, for
 * the frame that row shows. A row shows the newest present whose tear (the
 * later of the Present return and the GPU's end: a lower bound) came
 * before the raster reached it. Valid only where the slices tear in. */
static dist beam_latency(const pass* ps) {
    size_t i, n = 0, cap = ps->n * (size_t)(g_H / 8 + 1); int y;
    double* v;
    dist d; memset(&d, 0, sizeof d);
    if (pass_summary(ps).shown_per_refresh < 1.5) return d;   /* flips wait for vblanks */
    v = (double*)malloc(cap * sizeof *v);
    if (!v) return d;
    for (i = 1; i < ps->n; i++) {
        const prec* p = &ps->rec[i];
        const prec* nx = i + 1 < ps->n ? &ps->rec[i + 1] : NULL;
        double tear = p->t_ret > p->gpu_e ? p->t_ret : p->gpu_e;
        double tear_next = nx ? (nx->t_ret > nx->gpu_e ? nx->t_ret : nx->gpu_e) : 1e300;
        double Tn = m_entry_before(&g_m, tear) - g_m.P;
        if (p->r < 2) continue;
        for (y = 0; y < g_H; y += 8) {
            double ty = m_time(&g_m, Tn, (double)y);
            while (ty < tear) ty += g_m.P;   /* the first scan of this row after the tear */
            if (ty < tear_next && n < cap) v[n++] = (ty - p->t_in) * 1e-3;
        }
    }
    d = dist_of(v, n);
    free(v);
    return d;
}

static double g_lead_cal[16];

/* One pass per lead, for an eye test: the lead is on screen, and each
 * step's start goes to the console with the run time and the wall clock,
 * so a report such as "the stripes went at 800" maps to a pass. */
static void sweep_lead(int N) {
    enum { MAXSTEP = 200 };
    static double lead_us[MAXSTEP], t0_s[MAXSTEP], p50[MAXSTEP], p99[MAXSTEP], shown[MAXSTEP];
    static int late[MAXSTEP];
    int n = (int)floor((O.sweep_b - O.sweep_a) / O.sweep_step + 1e-9) + 1, s;
    if (n > MAXSTEP) n = MAXSTEP;
    say("sweep: N=%d, lead %.0f to %.0f us in steps of %.0f us, %d steps of %.0f s (plus 1 s of model refit between steps, a plain gray screen)\n",
        N, O.sweep_a, O.sweep_b, O.sweep_step, n, O.seconds);
    for (s = 0; s < n && !g_abort; s++) {
        pass p; char nm[96], lb[64]; double shift; SYSTEMTIME lt; pass_stats st;
        memset(&p, 0, sizeof p);
        lead_us[s] = O.sweep_a + s * O.sweep_step;
        shift = model_reanchor();   /* a 1-s fit drifts by about a line in 5 s, not in 30 */
        GetLocalTime(&lt);
        t0_s[s] = (double)(yrt_now_ns() - g_tbase) * 1e-9;
        say("  step %d of %d: lead %.0f us, from %.1f s (local %02u:%02u:%02u), model re-anchored %+.2f us\n", s + 1, n,
            lead_us[s], t0_s[s], (unsigned)lt.wHour, (unsigned)lt.wMinute, (unsigned)lt.wSecond, shift);
        run_pass(&p, N, DRAW_BANDS, lead_us[s] * 1e3, O.seconds);
        snprintf(lb, sizeof lb, "lead %.0f us", lead_us[s]);
        pass_report(&p, lb);
        snprintf(nm, sizeof nm, "tear_N%d_sweep%02d_lead%.0fus.csv", N, s, lead_us[s]);
        pass_csv(&p, nm);
        st = pass_summary(&p);
        p50[s] = st.miss.p50; p99[s] = st.miss.p99; shown[s] = st.shown_per_refresh; late[s] = p.late;
        free(p.rec);
    }
    g_ov_n = 0;
    say("sweep table, N=%d (scanline after Present minus target; the real tear is later, see the doc):\n", N);
    say("  step  lead_us  start_s  scan_p50  scan_p99  shown/refresh  late\n");
    for (n = s, s = 0; s < n; s++)
        say("  %4d  %7.0f  %7.1f  %+8.1f  %+8.1f  %13.2f  %4d\n", s + 1, lead_us[s], t0_s[s], p50[s], p99[s], shown[s], late[s]);
}

static void section_tear(void) {
    int i;
    say("\n== tear: present so the tear line lands on chosen scanlines ==\n");
    say("draw lead %.0f us (input read and draw before the present); %s; guard %d rows (wraps between refreshes); slice 0 target line %d%s; ruler %s\n",
        O.draw_lead_us, O.static_check ? "static check" : "bands", O.guard, -O.seam_lines,
        O.seam_lines > 0 ? " (inside the vblank)" : "", O.ruler ? "on" : "off");
    if (g_m.ok && O.seam_lines > (int)floor(-g_m.a))
        say("  note: --seam-lines %d is more than the %.1f blanking lines; slice 0 aims into the last rows of the refresh before\n",
            O.seam_lines, -g_m.a);
    if (!g_tearing) say("  tearing is not supported: presents flip at vblanks, so the tear placement below cannot work\n");
    if (O.sweep) {
        for (i = 0; i < O.n_slices && !g_abort; i++) sweep_lead(O.slices[i]);
        return;
    }
    for (i = 0; i < O.n_slices && !g_abort; i++) {
        pass p1, p2; char nm[64]; double shift; pass_stats s1;
        int N = O.slices[i];
        memset(&p1, 0, sizeof p1); memset(&p2, 0, sizeof p2);
        shift = model_reanchor();
        say("N=%d (model re-anchored: it had moved %+.2f us)\n", N, shift);
        if (O.lead_us >= 0) {
            g_lead_cal[i] = O.lead_us * 1e3;
        } else {
            run_pass(&p1, N, DRAW_BANDS, 0.0, O.seconds);
            pass_report(&p1, "lead 0");
            snprintf(nm, sizeof nm, "tear_N%d_lead0.csv", N); pass_csv(&p1, nm);
            s1 = pass_summary(&p1);
            g_lead_cal[i] = s1.miss.p50 / g_m.b;   /* the median lateness, in ns */
            free(p1.rec);
        }
        run_pass(&p2, N, DRAW_BANDS, g_lead_cal[i], O.seconds);
        pass_report(&p2, "calibrated");
        snprintf(nm, sizeof nm, "tear_N%d_cal.csv", N); pass_csv(&p2, nm);
        {
            dist L = beam_latency(&p2);
            if (L.n) say("    input to row, beam racing (lower bound): p50 %.2f p99 %.2f max %.2f ms\n", L.p50 * 1e-3, L.p99 * 1e-3, L.max * 1e-3);
            else say("    input to row, beam racing: not computed, the slices did not tear in\n");
        }
        free(p2.rec);
    }
}

/* GPU cost on an offscreen target of the screen's size, one draw at a time
 * on an idle GPU: the slice's own cost, free of any wait for a swapchain
 * buffer. wall = CPU submit to the GPU's end, seen by a polled event. */
typedef struct { dist gpu, wall; } offcost;
static offcost g_off[4];
static const char* off_name(int v) {
    return v == 0 ? "clear, full screen" : v == 1 ? "clear, slice rows" : v == 2 ? "shader, slice rows" : "shader, full screen";
}
static void offscreen_costs(int N) {
    ID3D11Texture2D* tex = NULL; ID3D11RenderTargetView* rtv = NULL; ID3D11RenderTargetView* keep = g_rtv;
    D3D11_TEXTURE2D_DESC td; D3D11_QUERY_DESC qd; ID3D11Query* ev = NULL;
    int v, i, reps = 200, y1 = g_H / N;
    double gpu[200], wall[200];
    float c[4] = { 0.3f, 0.3f, 0.3f, 1 };
    memset(&td, 0, sizeof td);
    td.Width = (UINT)g_W; td.Height = (UINT)g_H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &tex)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource*)tex, NULL, &rtv))) { RELEASE(tex); return; }
    qd.Query = D3D11_QUERY_EVENT; qd.MiscFlags = 0;
    ID3D11Device_CreateQuery(g_dev, &qd, &ev);
    g_rtv = rtv;
    for (v = 0; v < 4; v++) {
        int n = 0;
        for (i = 0; i < reps + 10; i++) {
            qset* q = &g_q[0];
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, b; BOOL done; int64_t t0, t1;
            ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous*)ev);
            get_data(ev, &done, sizeof done);   /* idle */
            t0 = yrt_now_ns();
            ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous*)q->dis);
            ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous*)q->t0);
            ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, NULL);
            switch (v) {
            case 0: clear_all(c); break;
            case 1: clear_rows(c, 0, y1); break;
            case 2: draw_shader(c, (float)i * 0.05f, 0, y1, 0); break;
            default: draw_shader(c, (float)i * 0.05f, 0, g_H, 1); break;
            }
            ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous*)q->t1);
            ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous*)q->dis);
            ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous*)ev);
            ID3D11DeviceContext_Flush(g_ctx);
            get_data(ev, &done, sizeof done);
            t1 = yrt_now_ns();
            if (!get_data(q->dis, &dj, sizeof dj) || dj.Disjoint || !get_data(q->t0, &a, sizeof a) || !get_data(q->t1, &b, sizeof b)) continue;
            if (i < 10) continue;   /* warm-up */
            gpu[n] = (double)(b - a) * 1e6 / (double)dj.Frequency;
            wall[n] = (double)(t1 - t0) * 1e-3;
            n++;
        }
        g_off[v].gpu = dist_of(gpu, (size_t)n);
        g_off[v].wall = dist_of(wall, (size_t)n);
        say("  offscreen %-20s GPU p50 %7.1f p99 %7.1f max %7.1f us; submit to done p50 %7.1f p99 %7.1f us\n", off_name(v),
            g_off[v].gpu.p50, g_off[v].gpu.p99, g_off[v].gpu.max, g_off[v].wall.p50, g_off[v].wall.p99);
    }
    g_rtv = keep;
    RELEASE(ev); RELEASE(rtv); RELEASE(tex);
}

static void section_cost(void) {
    int d, N = O.cost_slices, li; double lead = 0;
    say("\n== cost: GPU time per slice, and the raster ==\n");
    say("slice = %d rows (N=%d); shader = 8 sines and an exp per pixel, %dx%d; %d reps each\n", g_H / N, N, g_W, g_H, 200);
    offscreen_costs(N);
    say("  one slice period at N=%d is %.0f us\n", N, g_m.P / N * 1e-3);
    for (li = 0; li < O.n_slices; li++) if (O.slices[li] == N && g_lead_cal[li] > 0) lead = g_lead_cal[li];
    if (lead <= 0 && O.lead_us >= 0) lead = O.lead_us * 1e3;
    say("on screen, N=%d, lead %.1f us:\n", N, lead * 1e-3);
    for (d = 0; d < 3 && !g_abort; d++) {
        pass p; char nm[64];
        memset(&p, 0, sizeof p);
        model_reanchor();
        run_pass(&p, N, d, lead, O.seconds);
        pass_report(&p, draw_name(d));
        snprintf(nm, sizeof nm, "cost_N%d_%s.csv", N, d == DRAW_BANDS ? "light" : d == DRAW_SLICE_SHADER ? "slice" : "full");
        pass_csv(&p, nm);
        if (d == DRAW_FULL_SHADER) {
            dist L = beam_latency(&p);
            if (L.n) say("    input to row, beam racing, full-screen shader per slice (lower bound): p50 %.2f p99 %.2f max %.2f ms\n",
                         L.p50 * 1e-3, L.p99 * 1e-3, L.max * 1e-3);
            else say("    input to row, beam racing: not computed, the slices did not tear in\n");
        }
        free(p.rec);
    }
    say("GPU clock against QPC, last pass: brackets %.1f and %.1f us; between the two calibrations QPC advanced %.6f s and the GPU %.6f s; timestamp frequency %.0f Hz\n",
        g_cal0.width_ns * 1e-3, g_cal1.width_ns * 1e-3, (g_cal1.qpc_ns - g_cal0.qpc_ns) * 1e-9,
        (g_cal1.gpu_ns - g_cal0.gpu_ns) * 1e-9, g_gpu_freq);
}

/* The late frame start (docs/gfx.md, Next 12) at the same refresh: input
 * at the vblank minus a budget, one full-screen frame, flipped at the
 * vblank; a row shows when the raster reaches it. Beam racing at N slices
 * with the same frame cost, as a model: input read one budget before the
 * slice's first row, the row shown at most a slice later. */
static void section_latency(void) {
    size_t n = 0; dist L; int y, N, li; double B, Bs;
    double* lat;
    say("\n== latency in software terms: input read to the raster reaching each row ==\n");
    if (!g_off[3].wall.n || !g_m.ok) { say("  needs the cost section\n"); return; }
    lat = (double*)malloc((size_t)(g_H / 4 + 1) * sizeof *lat);
    if (!lat) return;
    B = g_off[3].wall.p99 + 500.0;   /* us: p99 of submit-to-done for a full-screen frame, plus a margin */
    say("budget: full-screen frame submit to done p99 %.0f us + 500 us margin = %.0f us\n", g_off[3].wall.p99, B);
    for (y = 0; y < g_H; y += 4) lat[n++] = (g_m.P + m_time(&g_m, 0, (double)y)) * 1e-6;
    L = dist_of(lat, n);
    say("  classic loop, input read right after the previous vblank: p50 %.2f p99 %.2f max %.2f ms\n", L.p50, L.p99, L.max);
    n = 0;
    for (y = 0; y < g_H; y += 4) lat[n++] = B * 1e-3 + m_time(&g_m, 0, (double)y) * 1e-6;
    L = dist_of(lat, n);
    say("  late frame start, flip at the vblank: p50 %.2f p99 %.2f max %.2f ms (row 0 %.2f ms, last row %.2f ms)\n",
        L.p50, L.p99, L.max, lat[0], lat[n - 1]);
    for (li = 0; li < 3; li++) {
        static const int ns[3] = { 2, 4, 10 };
        N = ns[li];
        /* each slice redraws its rows only (offscreen slice cost scales the
         * full frame by 1/N, measured at cost_slices) */
        Bs = (N == O.cost_slices ? g_off[2].wall.p99 : g_off[3].wall.p99 / N) + 500.0;
        n = 0;
        for (y = 0; y < g_H; y += 4) {
            int k = y * N / g_H, y0 = k * g_H / N;
            lat[n++] = Bs * 1e-3 + (m_time(&g_m, 0, (double)y) - m_time(&g_m, 0, (double)y0)) * 1e-6;
        }
        L = dist_of(lat, n);
        say("  beam racing model, N=%d, budget %.0f us per slice: p50 %.2f p99 %.2f max %.2f ms\n", N, Bs, L.p50, L.p99, L.max);
    }
    free(lat);
}

/* ------------------------------------------------------------------ */
static int parse_list(const char* s, int* out, int cap) {
    int n = 0;
    while (*s && n < cap) {
        int v = atoi(s);
        if (v > 0) out[n++] = v;
        while (*s && *s != ',') s++;
        if (*s == ',') s++;
    }
    return n;
}
static int parse_args(int argc, char** argv) {
    int i;
    memset(&O, 0, sizeof O);
    O.sec_path = O.sec_recipes = O.sec_raster = O.sec_psr = O.sec_tear = O.sec_cost = 1;
    O.slices[0] = 2; O.slices[1] = 4; O.slices[2] = 10; O.n_slices = 3;
    O.seconds = 5; O.drift_s = 60; O.guard = 8; O.buffers = 2; O.monitor = -1; O.cost_slices = 4;
    O.draw_lead_us = 1000; O.lead_us = -1; O.flush = 1;
    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--out") && v) { strncpy(g_out, v, sizeof g_out - 1); i++; }
        else if (!strcmp(a, "--only") && v) {
            O.sec_path = strstr(v, "path") != NULL; O.sec_raster = strstr(v, "raster") != NULL;
            O.sec_recipes = strstr(v, "recipes") != NULL;
            O.sec_psr = strstr(v, "psr") != NULL; O.sec_tear = strstr(v, "tear") != NULL;
            O.sec_cost = strstr(v, "cost") != NULL; i++;
        }
        else if (!strcmp(a, "--slices") && v) { O.n_slices = parse_list(v, O.slices, 8); i++; }
        else if (!strcmp(a, "--seconds") && v) { O.seconds = atof(v); i++; }
        else if (!strcmp(a, "--drift") && v) { O.drift_s = atof(v); i++; }
        else if (!strcmp(a, "--guard") && v) { O.guard = atoi(v); i++; }
        else if (!strcmp(a, "--buffers") && v) { O.buffers = atoi(v); i++; }
        else if (!strcmp(a, "--monitor") && v) { O.monitor = atoi(v); i++; }
        else if (!strcmp(a, "--draw-lead") && v) { O.draw_lead_us = atof(v); i++; }
        else if (!strcmp(a, "--lead") && v) { O.lead_us = atof(v); i++; }
        else if (!strcmp(a, "--cost-slices") && v) { O.cost_slices = atoi(v); i++; }
        else if (!strcmp(a, "--static-check")) O.static_check = 1;
        else if (!strcmp(a, "--no-flicker")) O.no_flicker = 1;
        else if (!strcmp(a, "--yes")) O.yes = 1;
        else if (!strcmp(a, "--exclusive")) O.exclusive = 1;
        else if (!strcmp(a, "--no-flush")) O.flush = 0;
        else if (!strcmp(a, "--vary-clear")) O.vary_clear = 1;
        else if (!strcmp(a, "--patch")) O.patch = 1;
        else if (!strcmp(a, "--ruler")) O.ruler = 1;
        else if (!strcmp(a, "--seam-lines") && v) { O.seam_lines = atoi(v); i++; }
        else if (!strcmp(a, "--sweep-lead") && v) {
            if (sscanf(v, "%lf:%lf:%lf", &O.sweep_a, &O.sweep_b, &O.sweep_step) != 3 || O.sweep_step == 0) {
                fprintf(stderr, "--sweep-lead wants A:B:STEP in us, STEP not 0 (for example 0:1500:100)\n"); return 0;
            }
            if ((O.sweep_b - O.sweep_a) * O.sweep_step < 0) O.sweep_step = -O.sweep_step;
            O.sweep = 1; i++;
        }
        else { fprintf(stderr, "unknown or incomplete option %s (see the top of beam_race_probe.c)\n", a); return 0; }
    }
    if (O.buffers < 2) O.buffers = 2;
    if (O.buffers > 8) O.buffers = 8;
    if (O.drift_s < 1) O.drift_s = 1;
    if (O.seconds < 1) O.seconds = 1;
    if (O.guard < 0) O.guard = 0;
    if (O.seam_lines < 0) O.seam_lines = 0;
    return 1;
}

int main(int argc, char** argv) {
    SYSTEM_POWER_STATUS pw;
    yrt_report rep; yrt_policy pol; char line[256];
    time_t now = time(NULL);
    struct tm* tmv = localtime(&now);
    if (!parse_args(argc, argv)) return 2;
    if (O.static_check) {   /* the same values draw_slice() uses for its wrong color */
        k_base[0] = O.no_flicker ? 0.40f : 1.0f; k_base[1] = O.no_flicker ? 0.32f : 0.0f; k_base[2] = O.no_flicker ? 0.40f : 1.0f;
    }
    if (!g_out[0]) strftime(g_out, sizeof g_out, "beam_%Y%m%d_%H%M%S", tmv);
    CreateDirectoryA(g_out, NULL);
    {
        char p[MAX_PATH + 32]; snprintf(p, sizeof p, "%s\\summary.txt", g_out);
        g_sum = fopen(p, "wb");
    }
    printf("WARNING: this program shows full-screen color bands and tear lines that can\n"
           "flicker when a slice misses. Do not run it where a person with photosensitive\n"
           "epilepsy can see the screen. --no-flicker uses low-contrast grays only.\n");
    if (!O.yes) {
        int s;
        for (s = 5; s > 0; s--) { printf("starting in %d s (Ctrl+C to stop)\r", s); fflush(stdout); Sleep(1000); }
        printf("\n");
    }
    g_tbase = yrt_now_ns();
    say("beam_race_probe, %s", asctime(tmv));
    say("results: %s\n", g_out);
    say("time zero of the CSV files: %lld ns on the ysp_rt clock (QPC)\n", (long long)g_tbase);
    GetSystemPowerStatus(&pw);
    say("power: %s, battery %d%%\n", pw.ACLineStatus == 1 ? "AC" : pw.ACLineStatus == 0 ? "battery" : "unknown",
        pw.BatteryLifePercent == 255 ? -1 : (int)pw.BatteryLifePercent);
    say("options: slices");
    { int i; for (i = 0; i < O.n_slices; i++) say(" %d", O.slices[i]); }
    say(", %.0f s per pass, drift %.0f s, %s%s, buffers %d, guard %d, seam lines %d%s\n", O.seconds, O.drift_s,
        O.static_check ? "static check" : "bands", O.no_flicker ? ", no flicker" : "", O.buffers, O.guard, O.seam_lines,
        O.ruler ? ", ruler" : "");
    if (O.sweep) say("lead sweep: %.0f to %.0f us in steps of %.0f us\n", O.sweep_a, O.sweep_b, O.sweep_step);
    else if (O.lead_us >= 0) say("lead: %.1f us (no calibration pass)\n", O.lead_us);
    g_st_th = CreateThread(NULL, 0, st_main, NULL, 0, NULL);
    yrt_timer_resolution_begin();
    pol = yrt_thread_elevate(NULL);
    yrt_report_get(&rep, pol);
    yrt_describe(&rep, line, sizeof line);
    say("%s\n", line);

    if (!d3d_open() || !kmt_open()) goto end;
    mode_query();
    queries_open();
    if ((O.sec_tear || O.sec_cost || O.sec_recipes) && !shader_open()) { say("shader setup failed\n"); goto end; }
    present_vsync(0.5, NULL);   /* let the window take the output */

    if (O.sec_path && !g_abort) section_path();
    if (O.sec_recipes && !g_abort) section_recipes();
    if ((O.sec_raster || O.sec_psr || O.sec_tear || O.sec_cost) && !g_abort) {
        if (!O.sec_raster) O.drift_s = 1;   /* a model is still needed */
        section_raster();
    }
    if (O.sec_psr && !g_abort) section_psr();
    if (O.sec_tear && g_m.ok && !g_abort) section_tear();
    if (O.sec_cost && g_m.ok && !g_abort) { section_cost(); section_latency(); }
    if (g_abort) say("\nstopped by Esc or close\n");
end:
    g_st_stop = 1;
    if (g_st_th) { WaitForSingleObject(g_st_th, 2000); CloseHandle(g_st_th); }
    say("\n");
    st_report(g_tbase);
    yrt_timer_resolution_end();
    if (g_sc && O.exclusive) IDXGISwapChain1_SetFullscreenState(g_sc, FALSE, NULL);
    if (g_hwnd) DestroyWindow(g_hwnd);
    if (g_sum) fclose(g_sum);
    return 0;
}
