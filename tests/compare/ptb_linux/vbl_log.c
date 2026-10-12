/* vbl_log.c - every vblank of one display, from the kernel, as ground truth
 * for flip stamps (Psychtoolbox's and ysp/screen.h's) on Linux.
 *
 *   vbl_log /dev/dri/card1 DP-5 SECONDS out.csv
 *
 * SIGTERM or SIGINT ends it early with the file complete.
 * Waits on each vblank of the CRTC that drives the connector with
 * drmWaitVBlank (no DRM master needed) and writes one line per vblank:
 *   seq,mono_ns,real_minus_mono_ns
 * mono_ns is the kernel's vblank time (CLOCK_MONOTONIC); the third column
 * is CLOCK_REALTIME minus CLOCK_MONOTONIC read just after, which maps the
 * vblank to Psychtoolbox's GetSecs (CLOCK_REALTIME on Linux).
 *
 * Build: cc -O2 -Wall -I/usr/include/libdrm -o vbl_log vbl_log.c -ldrm
 *
 * Public domain / MIT-0, as the rest of ysp. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static int64_t ns(clockid_t c) {
    struct timespec t;
    clock_gettime(c, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

int main(int argc, char** argv) {
    if (argc != 5) { fprintf(stderr, "usage: vbl_log /dev/dri/cardN CONNECTOR SECONDS out.csv\n"); return 2; }
    int fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror(argv[1]); return 1; }
    drmModeRes* res = drmModeGetResources(fd);
    if (!res) { fprintf(stderr, "no mode resources\n"); return 1; }
    int crtc_index = -1;
    for (int i = 0; i < res->count_connectors && crtc_index < 0; i++) {
        drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        char name[64];
        snprintf(name, sizeof name, "%s-%u", drmModeGetConnectorTypeName(c->connector_type), c->connector_type_id);
        if (strcmp(name, argv[2]) == 0 && c->encoder_id) {
            drmModeEncoder* e = drmModeGetEncoder(fd, c->encoder_id);
            if (e) {
                for (int k = 0; k < res->count_crtcs; k++)
                    if (res->crtcs[k] == e->crtc_id) crtc_index = k;
                drmModeFreeEncoder(e);
            }
        }
        drmModeFreeConnector(c);
    }
    if (crtc_index < 0) { fprintf(stderr, "%s: no active CRTC\n", argv[2]); return 1; }
    FILE* f = fopen(argv[4], "w");
    if (!f) { perror(argv[4]); return 1; }
    fprintf(f, "seq,mono_ns,real_minus_mono_ns\n");
    int64_t end = ns(CLOCK_MONOTONIC) + (int64_t)(atof(argv[3]) * 1e9);
    unsigned type_crtc = crtc_index == 0 ? 0
                       : crtc_index == 1 ? DRM_VBLANK_SECONDARY
                       : ((unsigned)crtc_index << DRM_VBLANK_HIGH_CRTC_SHIFT) & DRM_VBLANK_HIGH_CRTC_MASK;
    long n = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;   /* no SA_RESTART: the wait returns EINTR */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    while (!g_stop && ns(CLOCK_MONOTONIC) < end) {
        drmVBlank v;
        memset(&v, 0, sizeof v);
        v.request.type = (drmVBlankSeqType)(DRM_VBLANK_RELATIVE | type_crtc);
        v.request.sequence = 1;
        if (drmWaitVBlank(fd, &v) != 0) {
            if (errno == EINTR) continue;
            perror("drmWaitVBlank");
            return 1;
        }
        int64_t m0 = ns(CLOCK_MONOTONIC), r = ns(CLOCK_REALTIME), m1 = ns(CLOCK_MONOTONIC);
        fprintf(f, "%u,%lld,%lld\n", v.reply.sequence,
                (long long)((int64_t)v.reply.tval_sec * 1000000000 + (int64_t)v.reply.tval_usec * 1000),
                (long long)(r - (m0 + m1) / 2));
        n++;
    }
    fclose(f);
    fprintf(stderr, "vbl_log: %ld vblanks of CRTC %d\n", n, crtc_index);
    return 0;
}
