/* gaze_contingent.c - a gaze-contingent window with the mouse standing in for
 * the gaze, and the software part of its latency per frame: from the input
 * event's stamp to the flip's onset.
 *
 * It does what Psychtoolbox-3's GazeContingentDemo.m, BubbleDemo.m and
 * PsychTutorials/GazeContingentTutorial.m show: an image sharp in a
 * Gaussian window at the gaze, blurred outside. PTB is MIT; its files were
 * read for coverage, no code was copied.
 *
 * The image (960 x 640, made here: checks and a fine grating) and a copy
 * blurred on the CPU (Gaussian, SD 4 px) are two textures. Each frame draws
 * the blurred copy over the window, then an IMAGE of the sharp one, 192 x 192
 * px, centered on the newest gaze sample, its source rectangle the same
 * pixels, with a CIRCLE aperture and a GAUSSIAN edge of SD 24 px. Two draws;
 * no shader of our own.
 *
 * Gaze sources: SDL's pointer (default: window pixels, stamped with the
 * window message's time, tier 3), or --raw-mice (Windows: ysp/screen.h's raw
 * reports, stamped when read, in counts at 1 px per count). An eye tracker
 * would push YIN_KIND_EYE samples through yscr_push_input(), and this loop
 * takes them the same way (no eye tracker header exists yet).
 *
 * Per frame with a new sample: the sample's stamp, the time the loop read
 * it, and the flip's onset from its record. Printed: onset minus stamp
 * (input to photon in software: it leaves out the mouse's USB polling, the
 * HID stack and the panel), onset minus read (the frame's own part), and
 * read minus stamp (the wait in the queue). --inject (Windows) moves the
 * pointer with SendInput() about 1000 times a second along a line, and also
 * gives onset minus the SendInput() call of the sample shown.
 * The frame starts right after the previous flip, so a sample waits for the
 * next begin() and then a whole frame: docs/gfx.md Next, item 12 (late frame
 * start: poll input at the planned vblank minus a measured budget) is the
 * improvement, about 10 to 12 ms at 60 Hz, not built here.
 *
 * --sim: the simulated display and synthetic samples, each stamped 3 ms
 * before the frame's begin(): checks that every frame's latency is the
 * period plus 3 ms and the window sits on each sample. A window run checks
 * by read-back that the pixel under the gaze is the sharp image's and a far
 * one the blurred image's. Exits 1 on a mismatch.
 *
 * Usage: gfx_gaze_contingent [--sim] [--fullscreen] [--seconds S] [--raw-mice]
 *                            [--inject] [--out FILE]
 *   --seconds S  run S seconds (default: until Shift+Esc)
 *   --out FILE   one row per frame with a new sample: frame, stamp, read,
 *                onset (s from the first frame's onset), latency (s)
 * Exit code: 0; 1 when something did not open or a check failed; 2 for a bad
 * argument. On Windows set YSCR_ANGLE_DIR.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WW 960
#define WH 640
#define R 96               /* half the window's box, px */
#define EDGE_SD 24.0f
#define BLUR_SD 4.0
#define RING 8192          /* frames kept, by index */
#define MAXS 65536

static yscr_screen scr;
static ygfx_gfx gfx;
static uint8_t sharp[WW * WH * 4], blurred[WW * WH * 4];
static float tmp[WW * WH];
static int64_t ring_in[RING], ring_read[RING], ring_send[RING];
static double lat[MAXS], frame_part[MAXS], queue[MAXS], from_send[MAXS];
static uint8_t px[4], far_px[4];

static void make_images(void) {
    double k[25], ks = 0;
    int x, y, i;
    for (i = 0; i < 25; i++) { k[i] = exp(-(i - 12) * (i - 12) / (2 * BLUR_SD * BLUR_SD)); ks += k[i]; }
    for (y = 0; y < WH; y++)
        for (x = 0; x < WW; x++) {
            int chk = ((x / 24) + (y / 24)) & 1;
            double v = 0.5 + 0.25 * (chk ? 1 : -1) + 0.15 * cos(2 * 3.14159265358979 * x / 6.0);
            uint8_t b = (uint8_t)(v * 255 + 0.5);
            sharp[(y * WW + x) * 4] = sharp[(y * WW + x) * 4 + 1] = sharp[(y * WW + x) * 4 + 2] = b;
            sharp[(y * WW + x) * 4 + 3] = 255;
        }
    for (y = 0; y < WH; y++)   /* separable, edges clamped */
        for (x = 0; x < WW; x++) {
            double s = 0;
            for (i = -12; i <= 12; i++) { int xx = x + i < 0 ? 0 : x + i >= WW ? WW - 1 : x + i; s += k[i + 12] * sharp[(y * WW + xx) * 4]; }
            tmp[y * WW + x] = (float)(s / ks);
        }
    for (y = 0; y < WH; y++)
        for (x = 0; x < WW; x++) {
            double s = 0;
            uint8_t b;
            for (i = -12; i <= 12; i++) { int yy = y + i < 0 ? 0 : y + i >= WH ? WH - 1 : y + i; s += k[i + 12] * tmp[yy * WW + x]; }
            b = (uint8_t)(s / ks + 0.5);
            blurred[(y * WW + x) * 4] = blurred[(y * WW + x) * 4 + 1] = blurred[(y * WW + x) * 4 + 2] = b;
            blurred[(y * WW + x) * 4 + 3] = 255;
        }
}
static void summary(const char* what, double* v, int n) {
    int i, j;
    if (n <= 0) { printf("  %-34s none\n", what); return; }
    for (i = 1; i < n; i++) for (j = i; j > 0 && v[j] < v[j - 1]; j--) { double t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    printf("  %-34s min %6.2f  median %6.2f  p95 %6.2f  max %6.2f ms (n %d)\n", what, v[0], v[n / 2], v[(n * 95) / 100], v[n - 1], n);
}

#if defined(YSCR__DXGI)
/* --inject: a line sweep, one px per SendInput(); the last 256 calls kept
 * (x, time), so a sample's x names the call that put the pointer there. */
static volatile LONG inject_stop, sends;
static volatile int64_t send_t[256];
static volatile int send_x[256];
static int inject_wx, inject_wy;
static DWORD WINAPI inject_main(LPVOID arg) {
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN), k = 0;
    (void)arg;
    while (!inject_stop) {
        int x = 180 + (k / 600 & 1 ? 599 - k % 600 : k % 600);
        INPUT in;
        memset(&in, 0, sizeof in);
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        in.mi.dx = (LONG)((inject_wx + x) * 65535.0 / (sw - 1) + 0.5);
        in.mi.dy = (LONG)((inject_wy + WH / 2) * 65535.0 / (sh - 1) + 0.5);
        send_t[k & 255] = yrt_now_ns();
        send_x[k & 255] = x;
        InterlockedExchange(&sends, k + 1);
        SendInput(1, &in, sizeof in);
        k++;
        Sleep(1);
    }
    return 0;
}
#endif

int main(int argc, char** argv) {
    static ygfx_file_cache pcache;
    static char cache_dir[512];
    yscr_desc sd;
    ygfx_desc gd;
    ygfx_texture_desc td;
    ygfx_image_desc id;
    ygfx_tex tex[2];
    ygfx_stim back, win;
    yscr_frame f;
    SDL_Event ev;
    yin_event in;
    const char* out_path = NULL;
    FILE* out = NULL;
    double seconds = 0;
    float gx = WW / 2.0f, gy = WH / 2.0f;
    int i, unmatched = 0, sim = 0, fullscreen = 0, raw = 0, inject = 0, failed = 0, rc, n_lat = 0, n_send = 0, sim_bad = 0, read_bad = -1;
    int64_t t0 = 0, n = 0;
#if defined(YSCR__DXGI)
    HANDLE th = NULL;
#endif

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--raw-mice")) raw = 1;
        else if (!strcmp(argv[i], "--inject")) inject = 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else { fprintf(stderr, "usage: gfx_gaze_contingent [--sim] [--fullscreen] [--seconds S] [--raw-mice] [--inject] [--out FILE]\n"); return 2; }
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
#if !defined(YSCR__DXGI)
    if (inject || raw) { fprintf(stderr, "gfx_gaze_contingent: --inject and --raw-mice are Windows only\n"); return 2; }
#endif
    if (!(seconds >= 0) || (sim && (inject || raw))) { fprintf(stderr, "gfx_gaze_contingent: --seconds >= 0; --sim takes neither --inject nor --raw-mice\n"); return 2; }
    if (sim && seconds == 0) seconds = 2;
    make_images();
    memset(&sd, 0, sizeof sd);
    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = !fullscreen;
    sd.window_w = WW; sd.window_h = WH;
    sd.raw_mice = raw != 0;
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "gfx_gaze_contingent: %s\n", yscr_error(&scr)); return 1; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    if (ygfx_default_cache_dir(cache_dir, sizeof cache_dir) == YGFX_OK) gd.cache = ygfx_file_cache_init(&pcache, cache_dir);
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx_gaze_contingent: %s\n", ygfx_error(&gfx)); return 1; }
    for (i = 0; i < 2; i++) {
        memset(&td, 0, sizeof td);
        td.w = WW; td.h = WH; td.format = YGFX_RGBA8; td.data = i ? sharp : blurred;
        tex[i] = ygfx_texture(&gfx, &td);
        if (!tex[i].id) { fprintf(stderr, "gfx_gaze_contingent: %s\n", ygfx_error(&gfx)); return 1; }
    }
    memset(&id, 0, sizeof id);
    id.tex = tex[0]; id.place = YGFX_TOP_LEFT; id.x = WW / 2.0f; id.y = WH / 2.0f;   /* window pixels = screen pixels */
    back = ygfx_image(&gfx, &id);
    id.tex = tex[1]; id.place = YGFX_TOP_LEFT; id.w = id.h = 2 * R;
    win = ygfx_image(&gfx, &id);
    win.shape = YGFX_CIRCLE; win.edge = YGFX_EDGE_GAUSSIAN; win.edge_width = EDGE_SD;
    if (out_path && !(out = fopen(out_path, "w"))) { fprintf(stderr, "gfx_gaze_contingent: cannot write %s\n", out_path); return 1; }
    if (out) fprintf(out, "frame,stamp,read,onset,latency\n");
#if defined(YSCR__DXGI)
    if (inject) {
        /* SendInput reaches the foreground window only; a program started
         * from the background gets the foreground after one input event */
        HWND hw = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(yscr_window(&scr)), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        INPUT zero;
        memset(&zero, 0, sizeof zero);
        zero.type = INPUT_MOUSE;
        zero.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &zero, sizeof zero);
        SetForegroundWindow(hw);
        SDL_GetWindowPosition(yscr_window(&scr), &inject_wx, &inject_wy);
        th = CreateThread(NULL, 0, inject_main, NULL, 0, NULL);
    }
#endif
    printf("gaze from %s; Shift+Esc ends\n", sim ? "synthetic samples" : raw ? "raw mouse counts" : "SDL's pointer");

    memset(ring_in, 0, sizeof ring_in);
    while ((rc = yscr_begin(&scr, &f)) == YSCR_OK) {
        int64_t newest = 0, now;
        if (n == 0) t0 = f.onset;
        for (i = 0; i < f.n_done; i++) {   /* completed flips: their sample's latency */
            const yscr_record* r = &f.done[i];
            int64_t s = ring_in[r->index % RING];
            if (s == 0 || r->onset == 0 || f.index - r->index >= RING || n_lat >= MAXS) continue;
            lat[n_lat] = (double)(r->onset - s) / 1e6;
            frame_part[n_lat] = (double)(r->onset - ring_read[r->index % RING]) / 1e6;
            queue[n_lat] = (double)(ring_read[r->index % RING] - s) / 1e6;
            if (ring_send[r->index % RING]) from_send[n_send++] = (double)(r->onset - ring_send[r->index % RING]) / 1e6;
            if (sim && fabs(lat[n_lat] - (double)(f.period + 3000000) / 1e6) > 1e-6) sim_bad++;
            if (out) fprintf(out, "%lld,%.9f,%.9f,%.9f,%.9f\n", (long long)r->index, (double)(s - t0) * 1e-9,
                             (double)(ring_read[r->index % RING] - t0) * 1e-9, (double)(r->onset - t0) * 1e-9, lat[n_lat] / 1e3);
            n_lat++;
        }
        while (yscr_poll(&scr, &ev, NULL)) {
            int doorbell = ev.type == yscr_input_event_type();
            if (!yscr_event_input(&scr, &ev, &in) || in.type != YIN_SAMPLE) continue;
            if (in.kind == YIN_KIND_EYE || (in.kind == YIN_KIND_MOUSE && !raw && !doorbell && in.control == YIN_AXIS_POSITION)) {
                gx = in.x; gy = in.y;
            } else if (in.kind == YIN_KIND_MOUSE && raw && doorbell && in.control == YIN_AXIS_DELTA) {
                gx += in.x; gy += in.y;
            } else continue;
            if (in.t > newest) newest = in.t;
        }
        if (sim) {   /* a sample on a circle, 3 ms before this begin() */
            gx = (float)(WW / 2 + floor(150 * cos(n * 0.05))); gy = (float)(WH / 2 + floor(150 * sin(n * 0.05)));
            newest = f.onset - f.period - 3000000;
        }
        gx = gx < R ? R : gx > WW - R ? WW - R : gx;
        gy = gy < R ? R : gy > WH - R ? WH - R : gy;
        now = yrt_now_ns();
        ring_in[f.index % RING] = newest;
        ring_read[f.index % RING] = sim ? f.onset - f.period : now;
        ring_send[f.index % RING] = 0;
#if defined(YSCR__DXGI)
        if (inject && newest) {   /* the newest call that sent this x, within 256 calls */
            int k, last = (int)sends;
            for (k = last - 1; k >= 0 && k >= last - 256; k--)
                if (send_x[k & 255] == (int)gx) { ring_send[f.index % RING] = send_t[k & 255]; break; }
            unmatched += !ring_send[f.index % RING];
        }
#endif
        win.x = floorf(gx); win.y = floorf(gy);
        win.src[0] = win.x - R; win.src[1] = win.y - R; win.src[2] = win.src[3] = 2 * R;
        if (sim) { float ax, ay; ygfx_resolve(&gfx, &win, &ax, &ay); sim_bad += ax != win.x || ay != win.y; }
        ygfx_begin(&gfx, &f);
        if (ygfx_draw(&gfx, &back) != YGFX_OK || ygfx_draw(&gfx, &win) != YGFX_OK) { fprintf(stderr, "gfx_gaze_contingent: %s\n", ygfx_error(&gfx)); failed = 1; }
        ygfx_end(&gfx);
        if (!sim && n == 5) {   /* under the gaze: sharp; 3 windows away: blurred */
            int x = (int)win.x, y = (int)win.y, fx = x < WW / 2 ? WW - 40 : 40, fy = y < WH / 2 ? WH - 40 : 40;
            read_bad = ygfx_read_output(&gfx, x, y, 1, 1, px) < 0 || ygfx_read_output(&gfx, fx, fy, 1, 1, far_px) < 0;
            read_bad += px[0] != sharp[(y * WW + x) * 4] || far_px[0] != blurred[(fy * WW + fx) * 4];
            if (read_bad) fprintf(stderr, "gfx_gaze_contingent: read back %u at the gaze (sharp %u), %u far (blurred %u)\n",
                                  px[0], sharp[(y * WW + x) * 4], far_px[0], blurred[(fy * WW + fx) * 4]);
        }
        yscr_flip(&scr);
        n++;
        if (seconds > 0 && f.onset + f.period - t0 >= (int64_t)(seconds * 1e9)) break;
    }
    if (rc != YSCR_OK && rc != YSCR_QUIT) { fprintf(stderr, "gfx_gaze_contingent: %s\n", yscr_error(&scr)); failed = 1; }
#if defined(YSCR__DXGI)
    if (th) { inject_stop = 1; WaitForSingleObject(th, INFINITE); CloseHandle(th); }
#endif
    ygfx_close(&gfx);
    yscr_close(&scr);
    if (out) fclose(out);

    printf("%lld frames; %d with a new sample and a flip record\n", (long long)n, n_lat);
    summary("onset - sample stamp (software)", lat, n_lat);
    summary("onset - read (the frame's part)", frame_part, n_lat);
    summary("read - sample stamp (queue)", queue, n_lat);
    if (inject) summary("onset - SendInput() of the sample", from_send, n_send);
    if (inject) printf("  samples whose x matched no recent SendInput(): %d\n", unmatched);
    printf("late frame start (docs/gfx.md Next 12) would move the read toward the onset; not built\n");
    failed |= sim ? sim_bad != 0 || n_lat < (int)n - 3 : read_bad > 0;
    if (failed) { fprintf(stderr, "gfx_gaze_contingent: a check failed\n"); return 1; }
    printf("checks passed%s\n", sim ? "" : read_bad < 0 ? " (no read-back: fewer than 6 frames)" : "");
    return 0;
}
