/* screen_gamma.c - the OS gamma ramp under ysp/screen.h, checked without
 * changing what the user sees first.
 *
 *   1. Reads the ramp of the display that holds a probe window and says
 *      whether it is the 8-bit identity.
 *   2. Sets that same ramp again (nothing visible can change), reads it
 *      back, compares, and times both calls.
 *   3. Only with --identity, and only when the ramp is not identity already:
 *      opens a fullscreen screen for 2 s (ysp_screen takes the ramp), reads
 *      the ramp during the run and after close, and checks that the user's
 *      ramp is back.
 *
 * Usage: screen_gamma [--identity]
 * Exit: 0 when every check passed, 1 when one failed, 2 usage. Windows only.
 */
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"

#include <stdio.h>
#include <string.h>

#if defined(YSCR__DXGI)

static int read_ramp(const WCHAR* dev, WORD r[3][256], double* us) {
    HDC dc = yscr__win.create_dc(dev, NULL, NULL, NULL);
    int64_t t0;
    BOOL ok;
    if (!dc) return 0;
    t0 = yrt_now_ns();
    ok = yscr__win.get_ramp(dc, r);
    if (us) *us = (double)(yrt_now_ns() - t0) / 1e3;
    yscr__win.delete_dc(dc);
    return ok ? 1 : 0;
}

int main(int argc, char** argv) {
    int identity_run = argc > 1 && strcmp(argv[1], "--identity") == 0;
    WORD before[3][256], back[3][256], during[3][256], after[3][256];
    MONITORINFOEXW mi;
    HMONITOR mon;
    HDC dc;
    double us_get = 0, us_set = 0;
    int64_t t0;
    int ok = 1;
    char name[40];
    if (argc > 1 && !identity_run) { fprintf(stderr, "usage: screen_gamma [--identity]\n"); return 2; }
    yscr__win_load();
    if (!yscr__win.get_ramp || !yscr__win.set_ramp || !yscr__win.create_dc || !yscr__win.monitor_info) {
        fprintf(stderr, "screen_gamma: gdi32 or user32 entry points missing\n");
        return 1;
    }
    mon = MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    yscr__win.monitor_info(mon, (LPMONITORINFO)&mi);
    yscr__copy_w(name, sizeof name, mi.szDevice);
    if (!read_ramp(mi.szDevice, before, &us_get)) { fprintf(stderr, "screen_gamma: GetDeviceGammaRamp failed\n"); return 1; }
    printf("display %s: ramp is %s (red[1]=%u red[128]=%u red[255]=%u); GetDeviceGammaRamp %.1f us\n", name,
           yscr__ramp_identity(&before[0][0]) ? "the 8-bit identity" : "NOT the identity",
           before[0][1], before[0][128], before[0][255], us_get);
    /* 2: the same ramp again: no visible change */
    dc = yscr__win.create_dc(mi.szDevice, NULL, NULL, NULL);
    t0 = yrt_now_ns();
    if (!yscr__win.set_ramp(dc, before)) { printf("SetDeviceGammaRamp(same ramp): refused\n"); ok = 0; }
    us_set = (double)(yrt_now_ns() - t0) / 1e3;
    yscr__win.delete_dc(dc);
    if (!read_ramp(mi.szDevice, back, NULL)) {
        printf("SetDeviceGammaRamp(same ramp) %.1f us; read back FAILED\n", us_set);
        ok = 0;
    } else {
        printf("SetDeviceGammaRamp(same ramp) %.1f us; read back %s\n", us_set,
               memcmp(before, back, sizeof before) == 0 ? "equal" : "DIFFERENT");
        if (memcmp(before, back, sizeof before) != 0) ok = 0;
    }
    for (t0 = 0; t0 < 5; t0++) {   /* the per-second check's cost */
        /* read_ramp() sets neither the ramp nor the time when it fails. */
        double us = 0;
        if (!read_ramp(mi.szDevice, back, &us)) { printf("  GetDeviceGammaRamp again: FAILED\n"); ok = 0; continue; }
        printf("  GetDeviceGammaRamp again: %.1f us\n", us);
    }
    if (identity_run) {
        if (yscr__ramp_identity(&before[0][0])) {
            printf("--identity: the ramp is the identity already; nothing to set\n");
        } else {
            yscr_screen s;
            yscr_desc d;
            yscr_frame f;
            char line[512];
            int i;
            memset(&s, 0, sizeof s);
            memset(&d, 0, sizeof d);
            if (!yscr_open(&s, &d)) { fprintf(stderr, "screen_gamma: %s\n", yscr_error(&s)); return 1; }
            yscr_describe(&s, line, sizeof line);
            printf("%s\n", line);
            if (!read_ramp(mi.szDevice, during, NULL)) { printf("during the run: GetDeviceGammaRamp FAILED\n"); ok = 0; }
            else printf("during the run: ramp is %s\n", yscr__ramp_identity(&during[0][0]) ? "the identity" : "NOT the identity");
            for (i = 0; i < 120 && yscr_begin(&s, &f) == YSCR_OK; i++) yscr_flip(&s);
            yscr_close(&s);
            if (!read_ramp(mi.szDevice, after, NULL)) { printf("after close: GetDeviceGammaRamp FAILED\n"); ok = 0; }
            else {
                printf("after close: ramp %s the one before\n", memcmp(before, after, sizeof before) == 0 ? "equals" : "DIFFERS FROM");
                if (memcmp(before, after, sizeof before) != 0) ok = 0;
            }
        }
    }
    printf("%s\n", ok ? "all gamma checks passed" : "a gamma check FAILED");
    return ok ? 0 : 1;
}

#else
int main(void) { printf("screen_gamma: Windows only\n"); return 0; }
#endif
